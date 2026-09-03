// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §14 — L.* live-vs-replay ordering cases.
//   - L.01 / L.02 (Phase 2): live-path / replay-path monotonicity.
//   - L.03 / L.04 / L.05 (0.6.0.N session 6): mid-replay buffering +
//     overflow-forces-resync; driven via real WS round-trip with the
//     `WsTestClient` drain-pause + frame-inspector hooks, plus the
//     `live_buffer_cap_override` test seam from `subscriptions.hpp`.
//   - L.06 / L.07 / L.08 (Phase 5): multi-channel interleave +
//     RBAC-skip cursor advance + envelope.seq == cursor.seq.

#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/replay.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/rbac/subscribe_rule.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/ws/conn_state.hpp"

#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ew = plinth::realtime::events_writer;
namespace rp = plinth::realtime::replay;

namespace {

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* v = std::getenv("PLINTH_PG_HOST")) {
    db.host = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<uint16_t>(std::stoi(v));
  }
  if (auto* v = std::getenv("PLINTH_PG_USER")) {
    db.user = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = v;
  }
  return db;
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  auto db = pg_config();
  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password + " connect_timeout=3";
  PGconn* conn = PQconnectdb(conninfo.c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);
}

struct TestPg {
  PGconn* conn = nullptr;
  explicit TestPg(const plinth::Config::Database& db) {
    conn = PQconnectdb(build_conninfo(db).c_str());
  }
  ~TestPg() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  TestPg(const TestPg&) = delete;
  auto operator=(const TestPg&) -> TestPg& = delete;
  TestPg(TestPg&&) = delete;
  auto operator=(TestPg&&) -> TestPg& = delete;

  [[nodiscard]] auto exec(const std::string& sql) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    return {PQexec(conn, sql.c_str()), PQclear};
  }
};

auto build_dispatched(std::string_view layer, std::string_view channel)
    -> plinth::realtime::DispatchedEvent {
  plinth::realtime::DispatchedEvent ev;
  ev.layer = std::string{layer};
  ev.channel = std::string{channel};
  ev.envelope = Json::Value(Json::objectValue);
  ev.envelope["layer"] = std::string{layer};
  ev.envelope["channel"] = std::string{channel};
  ev.envelope["emitted_at"] = "1970-01-01T00:00:00.000Z";
  return ev;
}

auto admin_state() -> plinth::ws::ConnState {
  plinth::ws::ConnState s;
  s.authenticated = true;
  s.is_admin = true;
  s.auth.user_id = "00000000-0000-0000-0000-000000000001";
  return s;
}

struct CapturedFrames {
  std::mutex mu;
  std::vector<std::string> frames;
  auto sink() {
    return [this](std::string s) {
      std::lock_guard lock(mu);
      frames.push_back(std::move(s));
    };
  }
  auto snapshot() {
    std::lock_guard lock(mu);
    return frames;
  }
};

// Writer + broker harness. Live-path tests rely on the writer's
// `pre_broker_hook_for_test` to capture the per-envelope seq in
// dispatch order — the hook fires AFTER the writer stamps and BEFORE
// `broker::dispatch` invokes the WS / JS arms, so it gives the test
// a deterministic view of the ordering the broker will see.
struct WriterHarness {
  drogon::orm::DbClientPtr db;
  explicit WriterHarness(plinth::Config::Realtime::Events cfg = {}) {
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::clear_pre_broker_hook_for_test();
    ew::reset_counters_for_test();
    plinth::realtime::clear_handlers_for_test();
    plinth::Config::Realtime::Broker bcfg{};
    plinth::realtime::broker::start(bcfg);
    if (cfg.enabled) {
      // 0.6.3.N — shared process-lifetime client; per-test create+
      // destroy of `newPgClient` reproducibly trips
      // `EventLoopThreadPool::~ + Resource deadlock avoided`.
      // Member-init not viable: assignment is gated on cfg.enabled.
      db = plinth::realtime_test::shared_pg_client(/*connNum=*/1);
      REQUIRE(db);
    }
    ew::set_db_client_for_test(db);
    ew::start(cfg);
  }
  ~WriterHarness() {
    ew::stop();
    ew::set_db_client_for_test(nullptr);
    ew::clear_pre_broker_hook_for_test();
    plinth::realtime::clear_handlers_for_test();
    plinth::realtime::broker::stop();
  }
  WriterHarness(const WriterHarness&) = delete;
  auto operator=(const WriterHarness&) -> WriterHarness& = delete;
  WriterHarness(WriterHarness&&) = delete;
  auto operator=(WriterHarness&&) -> WriterHarness& = delete;
};

// Replay-side harness mirrors replay_test.cpp's shape — distinct from
// `WriterHarness` so the replay-driver TUs can run independent of the
// writer's lifecycle.
struct ReplayHarness {
  drogon::orm::DbClientPtr db;
  explicit ReplayHarness() {
    plinth::Config::Realtime::Broker bcfg;
    bcfg.enabled = true;
    bcfg.rbac_enforce = true;
    plinth::realtime::broker::start(bcfg);
    rp::reset_audit_state_for_test();
    // must run first
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    rp::set_db_client_for_test(db);
  }
  ~ReplayHarness() {
    rp::set_db_client_for_test(nullptr);
    plinth::realtime::broker::stop();
  }
  ReplayHarness(const ReplayHarness&) = delete;
  auto operator=(const ReplayHarness&) -> ReplayHarness& = delete;
  ReplayHarness(ReplayHarness&&) = delete;
  auto operator=(ReplayHarness&&) -> ReplayHarness& = delete;
};

// L.03/L.04/L.05 harness — broker + writer + replay all wired against
// a freshly-created connNum=2 pool so we don't fight the WS server's
// pool. Mirrors `delta_sync_test::Harness`. The cap-override is
// cleared in the destructor so cross-test leakage is impossible.
struct LiveReplayWsHarness {
  drogon::orm::DbClientPtr db;
  explicit LiveReplayWsHarness(plinth::Config::Realtime::Events ecfg = {}) {
    namespace br = plinth::realtime::broker;
    namespace cs = plinth::realtime::cursor_store;
    br::stop();
    ew::stop();
    plinth::realtime::clear_handlers_for_test();
    br::reset_metrics_for_test();
    br::reset_audit_windows_for_test();
    br::set_rbac_enforce_for_test(true);
    plinth::Config::Realtime::Broker bcfg;
    bcfg.enabled = true;
    bcfg.rbac_enforce = true;
    br::start(bcfg);

    cs::clear_cache_for_test();
    ew::reset_counters_for_test();
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::clear_pre_broker_hook_for_test();
    // must run first
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    cs::set_db_client_for_test(db);
    rp::set_db_client_for_test(db);
    ew::set_db_client_for_test(db);
    ecfg.enabled = true;
    ew::start(ecfg);
  }
  ~LiveReplayWsHarness() {
    namespace br = plinth::realtime::broker;
    namespace cs = plinth::realtime::cursor_store;
    plinth::ws_test::clear_live_buffer_cap_override();
    ew::stop();
    ew::set_db_client_for_test(nullptr);
    ew::clear_pre_broker_hook_for_test();
    rp::set_db_client_for_test(nullptr);
    cs::set_db_client_for_test(nullptr);
    cs::clear_cache_for_test();
    plinth::realtime::clear_handlers_for_test();
    br::stop();
  }
  LiveReplayWsHarness(const LiveReplayWsHarness&) = delete;
  auto operator=(const LiveReplayWsHarness&) -> LiveReplayWsHarness& = delete;
  LiveReplayWsHarness(LiveReplayWsHarness&&) = delete;
  auto operator=(LiveReplayWsHarness&&) -> LiveReplayWsHarness& = delete;
};

// L.03/L.04/L.05 frame inspector — captures (type, seq) tuples in
// inbox arrival order. Tests use this to assert strict-monotonic
// ordering across replay → replay_done → buffered-live frames.
struct OrderInspector {
  std::mutex mu;
  std::vector<std::pair<std::string, std::int64_t>> entries;
  auto callback() {
    return [this](const Json::Value& v) {
      std::lock_guard lock(mu);
      std::string type = v["type"].asString();
      std::int64_t seq = -1;
      if (type == "replay" && v.isMember("envelope") &&
          v["envelope"].isMember("seq")) {
        seq = v["envelope"]["seq"].asInt64();
      } else if (type == "replay_done" && v.isMember("up_to_seq")) {
        seq = v["up_to_seq"].asInt64();
      } else if (type == "event" && v.isMember("payload") &&
                 v["payload"].isMember("seq")) {
        seq = v["payload"]["seq"].asInt64();
      }
      entries.emplace_back(std::move(type), seq);
    };
  }
  auto snapshot() {
    std::lock_guard lock(mu);
    return entries;
  }
};

} // namespace

// ── L.01 ─────────────────────────────────────────────────────────────

TEST_CASE("L.01: live path is strictly monotonic across 100 envelopes",
          "[realtime][events][writer][seq][ordering][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  WriterHarness h;

  // Capture the writer-stamped seq in dispatch order.
  std::vector<std::int64_t> seqs;
  std::mutex seqs_mu;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent& ev) {
        std::lock_guard lock(seqs_mu);
        REQUIRE(ev.envelope.isMember("seq"));
        seqs.push_back(ev.envelope["seq"].asInt64());
      });

  constexpr std::size_t N = 100;
  for (std::size_t i = 0; i < N; ++i) {
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_l01.t")));
  }
  ew::apply_drain_for_test();

  REQUIRE(seqs.size() == N);
  for (std::size_t i = 1; i < seqs.size(); ++i) {
    CHECK(seqs[i] > seqs[i - 1]);
  }
}

// ── L.02 ─────────────────────────────────────────────────────────────

TEST_CASE("L.02: replay path is strictly monotonic across 50 envelopes",
          "[realtime][events][replay][seq][ordering][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  // Seed via the writer so `plinth.events.seq` is the canonical
  // BIGSERIAL the writer-first topology will hand off to replay.
  {
    WriterHarness h;
    for (std::size_t i = 0; i < 50; ++i) {
      REQUIRE(ew::enqueue_for_test(
          build_dispatched("data", "plinth:data:ext_l02.t")));
    }
    ew::apply_drain_for_test();
  }

  ReplayHarness rh;
  auto state = admin_state();
  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(state, cf.sink(), /*since_seq=*/0,
                                            {"plinth:data:ext_l02.t"}, ecfg));

  CHECK(r.emitted == 50);
  CHECK_FALSE(r.resync.has_value());

  // Walk the captured frames and assert the per-frame envelope.seq
  // is strictly increasing. Skip the trailing replay_done frame —
  // it carries `up_to_seq`, not an envelope.
  auto frames = cf.snapshot();
  REQUIRE(frames.size() >= 51); // 50 replay + 1 replay_done
  std::int64_t last_seq = 0;
  for (const auto& f : frames) {
    if (f.find(R"("type":"replay_done")") != std::string::npos) {
      continue;
    }
    // Cheap inline parse of the seq field — replay frames inline
    // the envelope as `"envelope":{ ..., "seq":<n>, ... }`.
    auto pos = f.find(R"("seq":)");
    REQUIRE(pos != std::string::npos);
    auto start = pos + std::string{R"("seq":)"}.size();
    auto end = f.find_first_of(",}", start);
    REQUIRE(end != std::string::npos);
    auto seq = std::stoll(f.substr(start, end - start));
    CHECK(seq > last_seq);
    last_seq = seq;
  }
}

// ── L.03 ─────────────────────────────────────────────────────────────

TEST_CASE("L.03: first live > last replay (D.08 redux)",
          "[realtime][events][replay][seq][ordering][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  // Tiny chunk size makes the replay coro do many small paginated
  // SELECTs (~2 ms each); this widens the replay_in_flight=true
  // window so the test thread's `dispatch_for_test` deliver lambda
  // lands inside it. With chunk=1 and 5 seed events: 5 + 1 SELECTs
  // ≈ 12 ms, deterministically wider than the test-thread window.
  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_rows_per_chunk = 1;
  LiveReplayWsHarness h(ecfg);
  plinth::ws_test::TestPg pg{cfg.db};

  // Seed 5 events via the writer so plinth.events.seq is the canonical
  // BIGSERIAL the replay coro reads back.
  constexpr auto CHANNEL = "plinth:data:ext_l03.t";
  for (int i = 0; i < 5; ++i) {
    REQUIRE(ew::enqueue_for_test(build_dispatched("data", CHANNEL)));
  }
  ew::apply_drain_for_test();

  auto user_id = plinth::ws_test::insert_user(pg, "l03-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  OrderInspector inspector;
  plinth::ws_test::WsTestClient client;
  client.set_frame_inspector(inspector.callback());

  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append(CHANNEL);
  sub["channels"] = arr;
  sub["since_seq"] = 0;
  client.send_json(sub);
  auto sub_ack = client.receive_json(3s);
  REQUIRE(sub_ack.has_value());
  REQUIRE((*sub_ack)["type"].asString() == "subscribed");

  // Brief sleep so post_replay_setup is guaranteed to have run on
  // the conn loop before we queue the deliver lambda. Without this
  // the deliver could race ahead in FIFO order, observe an empty
  // replay_in_flight map, and broadcast directly (no buffering).
  std::this_thread::sleep_for(2ms);

  // Pause AFTER ack so the on_subscribe handler + post_replay_setup
  // queue have run; subsequent replay frames + live event flush all
  // queue to paused_raw, giving us a clean total-order read window
  // post-resume.
  client.pause_drain();

  // Fire one live event via `broker::dispatch_for_test` (skip the
  // writer's PG INSERT). With a stamped seq=6 the live frame sits
  // strictly after the 5 replay frames; conn-loop FIFO ordering
  // (post_replay_setup queued by fire_replay → deliver lambda
  // queued here → post_replay_cleanup queued at replay end) puts
  // the deliver inside the replay window. Whether it buffers or
  // broadcasts directly, ordering is preserved.
  {
    auto ev = build_dispatched("data", CHANNEL);
    ev.envelope["seq"] = static_cast<Json::Int64>(6);
    (void)plinth::realtime::broker::dispatch_for_test(ev);
  }

  // Allow replay + cleanup + buffer flush to settle.
  std::this_thread::sleep_for(500ms);
  client.resume_drain();

  // Drain inbox: 5 replay + replay_done + 1 live event (subscribed
  // ack was consumed before pause).
  int replay_seen = 0;
  bool done_seen = false;
  int event_seen = 0;
  std::int64_t up_to_seq = -1;
  std::int64_t live_seq = -1;
  for (int i = 0; i < 20; ++i) {
    auto f = client.receive_json(2s);
    if (!f.has_value()) {
      break;
    }
    const auto& t = (*f)["type"].asString();
    if (t == "replay") {
      replay_seen += 1;
    } else if (t == "replay_done") {
      done_seen = true;
      up_to_seq = (*f)["up_to_seq"].asInt64();
    } else if (t == "event") {
      event_seen += 1;
      live_seq = (*f)["payload"]["seq"].asInt64();
      break;
    }
    // ignore ping
  }
  CHECK(replay_seen == 5);
  CHECK(done_seen);
  CHECK(event_seen == 1);
  CHECK(up_to_seq == 5);
  CHECK(live_seq > up_to_seq);

  // Inspector should record strictly increasing seqs across replay
  // and event frames; replay_done's `up_to_seq` is a sentinel (==
  // last replay seq), not a new envelope seq, so skip it from the
  // strict-monotonic walk.
  auto entries = inspector.snapshot();
  std::int64_t last_seq = 0;
  for (const auto& [type, seq] : entries) {
    if (type == "replay" || type == "event") {
      CHECK(seq > last_seq);
      last_seq = seq;
    }
  }
}

// ── L.04 ─────────────────────────────────────────────────────────────

TEST_CASE("L.04: mid-replay buffering preserves order",
          "[realtime][events][replay][seq][ordering][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  // chunk_size=1 keeps replay slow enough for our 3 deliver lambdas
  // to all land inside the replay_in_flight=true window (see L.03
  // commentary above).
  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_rows_per_chunk = 1;
  LiveReplayWsHarness h(ecfg);
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_l04.t";
  for (int i = 0; i < 5; ++i) {
    REQUIRE(ew::enqueue_for_test(build_dispatched("data", CHANNEL)));
  }
  ew::apply_drain_for_test();

  auto user_id = plinth::ws_test::insert_user(pg, "l04-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  OrderInspector inspector;
  plinth::ws_test::WsTestClient client;
  client.set_frame_inspector(inspector.callback());

  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append(CHANNEL);
  sub["channels"] = arr;
  sub["since_seq"] = 0;
  client.send_json(sub);
  auto sub_ack = client.receive_json(3s);
  REQUIRE(sub_ack.has_value());
  REQUIRE((*sub_ack)["type"].asString() == "subscribed");

  std::this_thread::sleep_for(2ms);

  client.pause_drain();

  // Three live events (seq=6, 7, 8) via `dispatch_for_test` —
  // skips the writer's PG INSERT so they cannot be picked up by
  // replay's next paginated SELECT (would race with the original
  // 5-row replay). Default live_buffer_cap=256 — all three fit;
  // flushed in seq-ascending order after replay_done by
  // `post_replay_cleanup`.
  for (int i = 0; i < 3; ++i) {
    auto ev = build_dispatched("data", CHANNEL);
    ev.envelope["seq"] = static_cast<Json::Int64>(6 + i);
    (void)plinth::realtime::broker::dispatch_for_test(ev);
  }

  std::this_thread::sleep_for(500ms);
  client.resume_drain();

  // Drain: subscribed + 5 replay + replay_done + 3 events in order.
  int replay_seen = 0;
  bool done_seen = false;
  std::int64_t up_to_seq = -1;
  std::vector<std::int64_t> live_seqs;
  for (int i = 0; i < 25; ++i) {
    auto f = client.receive_json(2s);
    if (!f.has_value()) {
      break;
    }
    const auto& t = (*f)["type"].asString();
    if (t == "replay") {
      replay_seen += 1;
    } else if (t == "replay_done") {
      done_seen = true;
      up_to_seq = (*f)["up_to_seq"].asInt64();
    } else if (t == "event") {
      live_seqs.push_back((*f)["payload"]["seq"].asInt64());
      if (live_seqs.size() >= 3) {
        break;
      }
    }
  }
  CHECK(replay_seen == 5);
  CHECK(done_seen);
  CHECK(up_to_seq == 5);
  REQUIRE(live_seqs.size() == 3);
  for (std::size_t i = 0; i < live_seqs.size(); ++i) {
    CHECK(live_seqs[i] == 6 + static_cast<std::int64_t>(i));
  }
  for (std::size_t i = 1; i < live_seqs.size(); ++i) {
    CHECK(live_seqs[i] > live_seqs[i - 1]);
  }
}

// ── L.05 ─────────────────────────────────────────────────────────────

TEST_CASE("L.05: live_buffer overflow forces resync",
          "[realtime][events][replay][seq][ordering][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  // Tiny chunk size makes the replay coro do many small paginated
  // SELECTs back-to-back, each taking ~2 ms. With 200 events and
  // chunk=10 → 20 chunks × ~2 ms = ~40 ms replay window. That's
  // well wider than the test thread's <1 ms dispatch_for_test
  // calls, so all 5 deliver_to_conn lambdas reliably land while
  // replay_in_flight=true.
  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_rows_per_chunk = 10;
  LiveReplayWsHarness h(ecfg);
  plinth::ws_test::TestPg pg{cfg.db};

  // Cap override BEFORE connect — `fire_replay` reads the override at
  // replay-start, post_replay_setup snapshots the effective cap into
  // ConnState::live_buffer_cap.
  plinth::ws_test::set_live_buffer_cap_override(4);

  constexpr auto CHANNEL = "plinth:data:ext_l05.t";
  for (int i = 0; i < 200; ++i) {
    REQUIRE(ew::enqueue_for_test(build_dispatched("data", CHANNEL)));
  }
  ew::apply_drain_for_test();

  auto user_id = plinth::ws_test::insert_user(pg, "l05-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;

  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append(CHANNEL);
  sub["channels"] = arr;
  sub["since_seq"] = 0;
  client.send_json(sub);
  auto sub_ack = client.receive_json(3s);
  REQUIRE(sub_ack.has_value());
  REQUIRE((*sub_ack)["type"].asString() == "subscribed");

  // Brief sleep to win the post_replay_setup race: the ack lands
  // before the queueInLoop'd setup lambda runs in some scheduler
  // orderings, and we need replay_in_flight=true at deliver time.
  // 5 ms is well below the chunk_size=10 + 200-event replay window
  // (~40 ms) so we stay firmly inside the buffering region.
  std::this_thread::sleep_for(5ms);

  client.pause_drain();

  // Fire 5 live events via `broker::dispatch_for_test` — this
  // queues 5 deliver_to_conn lambdas directly to the conn loop
  // from the test thread (~µs each), MUCH faster than going
  // through the writer's PG INSERT path (~1 ms each). With
  // chunk_size=10 the replay coro is still mid-paginated
  // SELECT loop; deliver_1..4 buffer (cap=4), deliver_5 →
  // `buf.size() >= cap` at publish.cpp:132 →
  // `handle_buffer_overflow` flips the abort flag, clears the
  // buffer, and emits resync(reason=live_buffer_overflow).
  for (int i = 0; i < 5; ++i) {
    auto ev = build_dispatched("data", CHANNEL);
    ev.envelope["seq"] = static_cast<Json::Int64>(10000 + i);
    (void)plinth::realtime::broker::dispatch_for_test(ev);
  }

  // Allow conn loop to process deliver lambdas + emit resync.
  std::this_thread::sleep_for(500ms);
  client.resume_drain();

  // Drain: some replay frames + resync(live_buffer_overflow). Replay
  // aborts mid-flight so replay_done may or may not be sent; the
  // load-bearing assertion is on the resync presence + reason.
  bool resync_seen = false;
  std::string resync_reason;
  for (int i = 0; i < 600; ++i) {
    auto f = client.receive_json(2s);
    if (!f.has_value()) {
      break;
    }
    const auto& t = (*f)["type"].asString();
    if (t == "resync") {
      resync_seen = true;
      resync_reason = (*f)["reason"].asString();
      break;
    }
  }
  CHECK(resync_seen);
  CHECK(resync_reason == "live_buffer_overflow");

  plinth::ws_test::clear_live_buffer_cap_override();
}

// ── L.06 ─────────────────────────────────────────────────────────────

TEST_CASE("L.06: multi-channel reconnect interleaves replay frames by seq",
          "[realtime][events][replay][seq][ordering][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  // Seed alternating writes on two channels so the BIGSERIAL seqs
  // interleave (A=1, B=2, A=3, B=4, ...). Replay must return the
  // global ORDER BY seq ASC sequence regardless of channel grouping.
  {
    WriterHarness h;
    for (std::size_t i = 0; i < 10; ++i) {
      const auto* channel_name =
          (i % 2 == 0) ? "plinth:data:ext_l06.a" : "plinth:data:ext_l06.b";
      REQUIRE(ew::enqueue_for_test(build_dispatched("data", channel_name)));
    }
    ew::apply_drain_for_test();
  }

  ReplayHarness rh;
  auto state = admin_state();
  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;
  auto r = drogon::sync_wait(
      rp::run_replay(state, cf.sink(), /*since_seq=*/0,
                     {"plinth:data:ext_l06.a", "plinth:data:ext_l06.b"}, ecfg));

  CHECK(r.emitted == 10);
  CHECK_FALSE(r.resync.has_value());

  auto frames = cf.snapshot();
  std::int64_t last_seq = 0;
  bool seen_a = false;
  bool seen_b = false;
  for (const auto& f : frames) {
    if (f.find(R"("type":"replay_done")") != std::string::npos) {
      continue;
    }
    auto pos = f.find(R"("seq":)");
    REQUIRE(pos != std::string::npos);
    auto start = pos + std::string{R"("seq":)"}.size();
    auto end = f.find_first_of(",}", start);
    REQUIRE(end != std::string::npos);
    auto seq = std::stoll(f.substr(start, end - start));
    CHECK(seq > last_seq);
    last_seq = seq;
    if (f.find("ext_l06.a") != std::string::npos) {
      seen_a = true;
    }
    if (f.find("ext_l06.b") != std::string::npos) {
      seen_b = true;
    }
  }
  CHECK(seen_a);
  CHECK(seen_b);
}

// ── L.07 ─────────────────────────────────────────────────────────────

TEST_CASE("L.07: RBAC-denied row in replay does not bump up_to_seq",
          "[realtime][events][replay][seq][ordering][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  // Seed events on two DIFFERENT extensions so each maps to a
  // distinct subscribe rule (the rule key is per-extension, not
  // per-table — see derive_subscribe_rule in rbac/subscribe_rule).
  // Ext_l07a.t = allowed; ext_l07b.t = denied. Order: a, b, a so
  // result.up_to_seq tracks the last DELIVERED (third) row.
  {
    WriterHarness h;
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_l07a.t")));
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_l07b.t")));
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_l07a.t")));
    ew::apply_drain_for_test();
  }

  ReplayHarness rh;
  plinth::ws::ConnState state;
  state.authenticated = true;
  state.is_admin = false;
  state.auth.user_id = "00000000-0000-0000-0000-000000000007";
  auto rule_a = plinth::rbac::derive_subscribe_rule("plinth:data:ext_l07a.t");
  REQUIRE(!rule_a.empty());
  state.effective_rules.insert(rule_a);

  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(
      state, cf.sink(), /*since_seq=*/0,
      {"plinth:data:ext_l07a.t", "plinth:data:ext_l07b.t"}, ecfg));

  CHECK(r.emitted == 2);
  CHECK_FALSE(r.resync.has_value());
  auto frames = cf.snapshot();
  for (const auto& f : frames) {
    CHECK(f.find("ext_l07b") == std::string::npos);
  }
}

// ── L.08 ─────────────────────────────────────────────────────────────

TEST_CASE("L.08: envelope.seq == cursor.seq invariant after dispatch",
          "[realtime][events][writer][seq][ordering][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  // Force cursor_store to flush every record_delivered immediately
  // (cursor_cache_ttl_ms=0) so the on-disk row matches the
  // in-memory cursor before we query it. Pin a DbClient for
  // cursor_store too — it falls back to drogon::app() otherwise,
  // which the unit harness doesn't bring up.
  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_cache_ttl_ms = 0;
  WriterHarness h{ecfg};
  TestPg pg{pg_config()};
  // The cursor store's UPSERT has a FK on plinth.users; seed a row.
  auto user_res = pg.exec("INSERT INTO plinth.users (username, password_hash) "
                          "VALUES ('l08-user', 'x') RETURNING id::text");
  REQUIRE(PQresultStatus(user_res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(user_res.get()) == 1);
  const std::string USER_ID{PQgetvalue(user_res.get(), 0, 0)};

  auto cursor_db = plinth::realtime_test::shared_pg_client(/*connNum=*/1);
  REQUIRE(cursor_db);
  plinth::realtime::cursor_store::set_db_client_for_test(cursor_db);

  std::int64_t last_envelope_seq = -1;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent& ev) {
        REQUIRE(ev.envelope.isMember("seq"));
        last_envelope_seq = ev.envelope["seq"].asInt64();
        ev.delivered_to_users.push_back(USER_ID);
      });

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_l08.t")));
  ew::apply_drain_for_test();

  REQUIRE(last_envelope_seq > 0);
  auto res = pg.exec("SELECT last_seq FROM plinth.user_event_cursors "
                     "WHERE user_id = '" +
                     USER_ID + "'::uuid");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  auto cursor_seq = std::stoll(PQgetvalue(res.get(), 0, 0));
  CHECK(cursor_seq == last_envelope_seq);

  plinth::realtime::cursor_store::set_db_client_for_test(nullptr);
}
