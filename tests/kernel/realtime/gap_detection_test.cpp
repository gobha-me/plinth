// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §11 / §14 row S.06 — broker-side `realtime.seq.gap_detected`
// audit pipeline. Three TEST_CASEs:
//
//   S.06a — happy gap fires audit. Subscribe live (no since_seq → no
//           replay), dispatch seq=1 then seq=3; assert audit row with
//           prev_seq=1, next_seq=3, gap_size=1, count_in_window=1.
//   S.06b — same-window dedup suppresses. Dispatch seq=1, seq=3 (audit
//           fires), seq=5 (gap of 1 again, same window); assert exactly
//           ONE audit row exists.
//   S.06c — first live frame after subscribe doesn't false-fire.
//           Dispatch seq=42 with no prior baseline → no audit; then
//           seq=44 → audit fires with prev_seq=42, next_seq=44.
//
// Pure live-path tests (replay unused) — `subscribe` is sent WITHOUT
// `since_seq` so `fire_replay` does not run and `deliver_to_conn`
// always takes the immediate-send arm where gap detection lives.
//
// Reuses the session 6 `LiveReplayWsHarness` + `WsTestClient` pattern
// from `live_replay_ordering_test.cpp` per the
// `project_test_fixture_inflight.md` Session 8 plan.

#include "kernel/realtime/events_writer.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/realtime/replay.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/publish.hpp"

#include "shared_pg_client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ew = plinth::realtime::events_writer;
namespace rp = plinth::realtime::replay;

namespace {

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

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

// Mirrors LiveReplayWsHarness from live_replay_ordering_test.cpp —
// broker + writer + replay all wired against a connNum=2 pool. Writer
// is started but no events are seeded; tests inject live envelopes
// directly via `broker::dispatch_for_test` (bypasses writer's PG
// INSERT and the BIGSERIAL — seq stamps come from each test's
// `ev.envelope["seq"] = ...`). Cap override is cleared in the
// destructor so cross-test leakage is impossible.
struct GapDetectHarness {
  drogon::orm::DbClientPtr db;
  explicit GapDetectHarness() {
    namespace br = plinth::realtime::broker;
    namespace cs = plinth::realtime::cursor_store;
    br::stop();
    ew::stop();
    plinth::realtime::clear_handlers_for_test();
    br::reset_metrics_for_test();
    br::reset_audit_windows_for_test();
    plinth::ws::reset_gap_audit_windows_for_test();
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
    // 0.6.3.N — shared process-lifetime client; per-test create+
    // destroy of `newPgClient` reproducibly trips
    // `EventLoopThreadPool::~ + Resource deadlock avoided`. Member-
    // init not viable: setup work above must run first.
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    cs::set_db_client_for_test(db);
    rp::set_db_client_for_test(db);
    ew::set_db_client_for_test(db);
    plinth::Config::Realtime::Events ecfg;
    ecfg.enabled = true;
    ew::start(ecfg);
  }
  ~GapDetectHarness() {
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
  GapDetectHarness(const GapDetectHarness&) = delete;
  auto operator=(const GapDetectHarness&) -> GapDetectHarness& = delete;
  GapDetectHarness(GapDetectHarness&&) = delete;
  auto operator=(GapDetectHarness&&) -> GapDetectHarness& = delete;
};

// Subscribe with no `since_seq` so replay does NOT fire (per
// subscriptions.cpp:277-279 → fire_replay branch at :399 only runs
// when `since_seq` is present). Returns once the `subscribed` ack
// arrives.
auto auth_and_subscribe(plinth::ws_test::WsTestClient& client,
                        const std::string& token, const std::string& channel)
    -> void {
  using namespace std::chrono_literals;
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
  arr.append(channel);
  sub["channels"] = arr;
  client.send_json(sub);
  auto sub_ack = client.receive_json(3s);
  REQUIRE(sub_ack.has_value());
  REQUIRE((*sub_ack)["type"].asString() == "subscribed");
}

// Dispatch one envelope at the given seq. Sleeps briefly for the
// queueInLoop deliver lambda + WS frame send + (if applicable) the
// audit's async PG INSERT to land.
auto dispatch_seq(const std::string& channel, std::int64_t seq) -> void {
  auto ev = build_dispatched("data", channel);
  ev.envelope["seq"] = static_cast<Json::Int64>(seq);
  (void)plinth::realtime::broker::dispatch_for_test(ev);
}

// Drain the next `event` frame off the inbox. Returns the parsed seq
// or -1 if no event arrives within the timeout.
auto next_event_seq(plinth::ws_test::WsTestClient& client) -> std::int64_t {
  using namespace std::chrono_literals;
  for (int i = 0; i < 20; ++i) {
    auto f = client.receive_json(2s);
    if (!f.has_value()) {
      return -1;
    }
    if ((*f)["type"].asString() == "event" && (*f).isMember("payload") &&
        (*f)["payload"].isMember("seq")) {
      return (*f)["payload"]["seq"].asInt64();
    }
  }
  return -1;
}

// Count `realtime.seq.gap_detected` audit rows for the given user.
auto gap_audit_count(plinth::ws_test::TestPg& pg, const std::string& user_id)
    -> int {
  auto r = pg.exec_params("SELECT count(*) FROM plinth.audit_log "
                          "WHERE action = 'realtime.seq.gap_detected' "
                          "AND user_id = $1::uuid",
                          {user_id});
  REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(r.get()) == 1);
  return std::stoi(PQgetvalue(r.get(), 0, 0));
}

// Fetch the most recent `realtime.seq.gap_detected` detail JSON for
// the user. Returns an empty string if no row exists.
auto latest_gap_detail(plinth::ws_test::TestPg& pg, const std::string& user_id)
    -> std::string {
  auto r = pg.exec_params("SELECT detail FROM plinth.audit_log "
                          "WHERE action = 'realtime.seq.gap_detected' "
                          "AND user_id = $1::uuid "
                          "ORDER BY timestamp DESC LIMIT 1",
                          {user_id});
  REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  if (PQntuples(r.get()) == 0) {
    return {};
  }
  return PQgetvalue(r.get(), 0, 0);
}

} // namespace

// ── S.06a ────────────────────────────────────────────────────────────

TEST_CASE("S.06a: live-path gap fires realtime.seq.gap_detected audit",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws::reset_gap_audit_windows_for_test();
  GapDetectHarness h;
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_s06a.t";

  auto user_id = plinth::ws_test::insert_user(pg, "s06a-admin", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
  auth_and_subscribe(client, token, CHANNEL);

  // First live frame establishes the baseline (no gap fires).
  dispatch_seq(CHANNEL, 1);
  REQUIRE(next_event_seq(client) == 1);

  // Second live frame skips seq=2 → forward gap of K=2.
  dispatch_seq(CHANNEL, 3);
  REQUIRE(next_event_seq(client) == 3);

  // Audit INSERT is async fire-and-forget; give it time to land.
  std::this_thread::sleep_for(300ms);

  CHECK(gap_audit_count(pg, user_id) == 1);
  auto detail = latest_gap_detail(pg, user_id);
  REQUIRE(!detail.empty());
  CHECK(detail.find("\"prev_seq\": 1") != std::string::npos);
  CHECK(detail.find("\"next_seq\": 3") != std::string::npos);
  CHECK(detail.find("\"gap_size\": 1") != std::string::npos);
  CHECK(detail.find("\"count_in_window\": 1") != std::string::npos);
  CHECK(detail.find("\"channel\": \"plinth:data:ext_s06a.t\"") !=
        std::string::npos);
  CHECK(detail.find("\"window_ms\": 60000") != std::string::npos);
}

// ── S.06b ────────────────────────────────────────────────────────────

TEST_CASE("S.06b: same-window dedup suppresses second gap audit",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws::reset_gap_audit_windows_for_test();
  GapDetectHarness h;
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_s06b.t";

  auto user_id = plinth::ws_test::insert_user(pg, "s06b-admin", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
  auth_and_subscribe(client, token, CHANNEL);

  // Establish baseline at seq=1.
  dispatch_seq(CHANNEL, 1);
  REQUIRE(next_event_seq(client) == 1);

  // First gap (1 → 3) fires audit (count_in_window=1).
  dispatch_seq(CHANNEL, 3);
  REQUIRE(next_event_seq(client) == 3);

  // Second gap (3 → 5) within the same 60s window → suppressed.
  dispatch_seq(CHANNEL, 5);
  REQUIRE(next_event_seq(client) == 5);

  std::this_thread::sleep_for(300ms);

  CHECK(gap_audit_count(pg, user_id) == 1);
  // The single audit row reflects the FIRST gap (1 → 3); the second
  // gap was suppressed by the sliding-window dedup.
  auto detail = latest_gap_detail(pg, user_id);
  CHECK(detail.find("\"prev_seq\": 1") != std::string::npos);
  CHECK(detail.find("\"next_seq\": 3") != std::string::npos);

  // The emit-count test seam confirms exactly one audit was fired
  // from publish.cpp's deliver_to_conn path (not just one INSERT
  // landed in PG — proves the suppression really happened upstream).
  CHECK(plinth::ws::gap_audit_emit_count_for_test() == 1);
}

// ── S.06c ────────────────────────────────────────────────────────────

TEST_CASE("S.06c: first live frame after subscribe does not false-fire",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws::reset_gap_audit_windows_for_test();
  GapDetectHarness h;
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_s06c.t";

  auto user_id = plinth::ws_test::insert_user(pg, "s06c-admin", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
  auth_and_subscribe(client, token, CHANNEL);

  // First live frame on this channel since subscribe — establishes
  // baseline at 42, no gap audit even though seq jumped from "no
  // baseline" to 42.
  dispatch_seq(CHANNEL, 42);
  REQUIRE(next_event_seq(client) == 42);
  std::this_thread::sleep_for(200ms);
  CHECK(gap_audit_count(pg, user_id) == 0);
  CHECK(plinth::ws::gap_audit_emit_count_for_test() == 0);

  // Second live frame at 44 → gap of K=2 against the baseline.
  dispatch_seq(CHANNEL, 44);
  REQUIRE(next_event_seq(client) == 44);
  std::this_thread::sleep_for(300ms);

  CHECK(gap_audit_count(pg, user_id) == 1);
  auto detail = latest_gap_detail(pg, user_id);
  CHECK(detail.find("\"prev_seq\": 42") != std::string::npos);
  CHECK(detail.find("\"next_seq\": 44") != std::string::npos);
  CHECK(detail.find("\"gap_size\": 1") != std::string::npos);
}
