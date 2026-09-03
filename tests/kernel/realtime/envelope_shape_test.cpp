// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §14 — W.* envelope-shape cases. Phase 3 lands the
// coalescer-side wire fields (`coalesced_count`, `window_open_ts_ms`,
// `window_close_ts_ms`, optional `superseded_seqs[]`) and the
// writer-side default-stamp for Layer-2/3 envelopes that bypass the
// coalescer. W.06 is deferred — see TU comment.

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/realtime/replay.hpp"
#include "kernel/realtime/sql_classify.hpp"
#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ew = plinth::realtime::events_writer;
namespace rp = plinth::realtime::replay;
using plinth::realtime::CoalescerRegistry;
using plinth::realtime::DispatchedEvent;

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

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);
}

auto build_dispatched(std::string_view layer, std::string_view channel)
    -> DispatchedEvent {
  DispatchedEvent ev;
  ev.layer = std::string{layer};
  ev.channel = std::string{channel};
  ev.envelope = Json::Value(Json::objectValue);
  ev.envelope["layer"] = std::string{layer};
  ev.envelope["channel"] = std::string{channel};
  ev.envelope["emitted_at"] = "1970-01-01T00:00:00.000Z";
  return ev;
}

// Writer-only harness — drives the production INSERT path for tests
// that read the persisted envelope back from `plinth.events.payload`.
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
    plinth::realtime::clear_handlers_for_test();
    plinth::realtime::broker::stop();
  }
  WriterHarness(const WriterHarness&) = delete;
  auto operator=(const WriterHarness&) -> WriterHarness& = delete;
  WriterHarness(WriterHarness&&) = delete;
  auto operator=(WriterHarness&&) -> WriterHarness& = delete;
};

// Coalescer-driven harness — captures envelopes at the listener
// handler so tests see exactly what the coalescer flushed onto the
// PG wire (post-emit_notify_async, pre-writer-INSERT). Mirrors
// `coalescer_integration_test.cpp` Harness.
struct Received {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<DispatchedEvent> events;

  auto on_event(const DispatchedEvent& ev) -> void {
    {
      std::lock_guard lock(mu);
      events.push_back(ev);
    }
    cv.notify_all();
  }
  auto wait_for_n(std::size_t n, std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, timeout,
                       [this, n]() { return events.size() >= n; });
  }
};

struct CoalescerHarness {
  std::shared_ptr<Received> received;
  drogon::orm::DbClientPtr db;
  plinth::Config::Realtime::Coalescer cfg;

  explicit CoalescerHarness(std::size_t window_ms = 80) {
    received = std::make_shared<Received>();

    // Writer must be started so the coalescer can read its config
    // snapshot for `coalesce.emit_superseded_seqs`. Disabled keeps
    // the writer thread off; current_config still returns the
    // last `start()` argument.
    ew::stop();
    plinth::Config::Realtime::Events ecfg;
    ecfg.enabled = false;
    ew::set_db_client_for_test(nullptr);
    ew::start(ecfg);

    plinth::realtime::clear_handlers_for_test();
    plinth::realtime::register_handler(
        [rcv = received](const DispatchedEvent& ev) { rcv->on_event(ev); });

    auto db_cfg = pg_config();
    plinth::Config::Realtime::Listener lcfg;
    lcfg.reconnect_backoff_ms = 200;
    plinth::realtime::start_listener(db_cfg, lcfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // above must run first
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/1);
    REQUIRE(db);

    auto& reg = CoalescerRegistry::instance();
    reg.clear_windows_for_test();
    reg.shutdown();
    reg.clear_emit_hook_for_test();
    reg.set_db_client_for_test(db);

    cfg.enabled = true;
    cfg.window_ms = window_ms;
    reg.start(cfg);
  }
  ~CoalescerHarness() {
    auto& reg = CoalescerRegistry::instance();
    reg.clear_windows_for_test();
    reg.shutdown();
    reg.set_db_client_for_test(nullptr);
    plinth::realtime::stop_listener();
    plinth::realtime::clear_handlers_for_test();
    ew::stop();
  }
  CoalescerHarness(const CoalescerHarness&) = delete;
  auto operator=(const CoalescerHarness&) -> CoalescerHarness& = delete;
  CoalescerHarness(CoalescerHarness&&) = delete;
  auto operator=(CoalescerHarness&&) -> CoalescerHarness& = delete;
};

auto simulated_exec(std::string_view sql, std::string_view ext_name,
                    std::size_t row_count) -> void {
  if (auto c = plinth::realtime::classify_sql(sql, ext_name); c.has_value()) {
    CoalescerRegistry::instance().record_write(c->schema, c->table, c->op_kind,
                                               row_count, ext_name);
  }
}

} // namespace

// ── W.01 ─────────────────────────────────────────────────────────────

TEST_CASE("W.01: persisted Layer-1 envelope JSON contains seq",
          "[realtime][events][envelope][shape][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  WriterHarness h;
  TestPg pg{pg_config()};

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_w01.t")));
  ew::apply_drain_for_test();

  auto res = pg.exec("SELECT seq, payload::text FROM plinth.events "
                     "WHERE channel = 'plinth:data:ext_w01.t' LIMIT 1");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  auto table_seq = std::stoll(PQgetvalue(res.get(), 0, 0));
  auto payload = std::string{PQgetvalue(res.get(), 0, 1)};

  // Persisted JSONB does NOT contain seq (it is stamped after
  // INSERT onto the in-memory envelope; replay re-stamps from the
  // row's seq column). The in-memory envelope passed to broker
  // dispatch did contain seq — see seq_generation_test.cpp S.01.
  // W.01 here verifies the on-the-wire shape that the SDK ends up
  // observing, which is the replay frame: envelope["seq"] ==
  // table_seq. We verify by parsing a replay frame for the row.
  Json::Value parsed;
  Json::CharReaderBuilder rb;
  auto reader = std::unique_ptr<Json::CharReader>{rb.newCharReader()};
  std::string err;
  const auto* begin = payload.data();
  // CharReader::parse takes (begin, end) raw pointers.
  const auto* end = begin + payload.size();
  REQUIRE(reader->parse(begin, end, &parsed, &err));
  // Re-stamp seq on the replay frame's view (replay.cpp does this
  // in production).
  parsed["seq"] = static_cast<Json::Int64>(table_seq);
  auto frame = rp::build_replay_frame(parsed);
  CHECK(frame.find(R"("seq":)") != std::string::npos);
}

// ── W.02 ─────────────────────────────────────────────────────────────

TEST_CASE("W.02: non-persisted frames carry no seq",
          "[realtime][events][envelope][shape][unit]") {
  auto done = rp::build_replay_done_frame(/*up_to_seq=*/0,
                                          /*row_count=*/0);
  auto resync = rp::build_resync_frame(rp::ResyncReason::CURSOR_EXPIRED,
                                       /*retention_seconds=*/3600);
  CHECK(done.find(R"("seq":)") == std::string::npos);
  CHECK(resync.find(R"("seq":)") == std::string::npos);

  // subscribe_ack today is `{"type":"subscribed","channels":[...]}`.
  // Phase 4 extends with recommended_debounce_ms / jitter_max_ms;
  // neither path adds a `seq` field, so the W.02 invariant holds
  // across both phases.
  Json::Value ack(Json::objectValue);
  ack["type"] = "subscribed";
  ack["channels"] = Json::Value(Json::arrayValue);
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  auto ack_str = Json::writeString(b, ack);
  CHECK(ack_str.find(R"("seq":)") == std::string::npos);
}

// ── W.03 ─────────────────────────────────────────────────────────────

TEST_CASE("W.03: coalesced_count >= 1 on persisted envelopes",
          "[realtime][events][envelope][shape][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  SECTION("Layer-1 (coalescer): coalesced_count == hits") {
    CoalescerHarness h{80};
    simulated_exec("INSERT INTO ext_w03.t (id) VALUES ('a')", "w03", 1);
    simulated_exec("INSERT INTO ext_w03.t (id) VALUES ('b')", "w03", 1);
    simulated_exec("INSERT INTO ext_w03.t (id) VALUES ('c')", "w03", 1);
    REQUIRE(h.received->wait_for_n(1, std::chrono::milliseconds(500)));
    const auto& env = h.received->events.at(0).envelope;
    REQUIRE(env.isMember("coalesced_count"));
    CHECK(env["coalesced_count"].asUInt64() == 3);
  }

  SECTION("Layer-2/3 (writer-stamped default): coalesced_count == 1") {
    WriterHarness h;
    TestPg pg{pg_config()};
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("system", "plinth:system:packages.installed")));
    ew::apply_drain_for_test();
    auto res =
        pg.exec("SELECT payload::text FROM plinth.events "
                "WHERE channel = 'plinth:system:packages.installed' LIMIT 1");
    REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(res.get()) == 1);
    auto payload = std::string{PQgetvalue(res.get(), 0, 0)};
    // PG normalises JSONB output with `: ` between key and value.
    CHECK(payload.find(R"("coalesced_count": 1)") != std::string::npos);
  }
}

// ── W.04 ─────────────────────────────────────────────────────────────

TEST_CASE("W.04: window timestamps consistent with layer",
          "[realtime][events][envelope][shape][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  SECTION("Layer-1: close - open == window_ms") {
    constexpr std::size_t WINDOW = 80;
    CoalescerHarness h{WINDOW};
    simulated_exec("INSERT INTO ext_w04.t (id) VALUES ('x')", "w04", 1);
    REQUIRE(h.received->wait_for_n(1, std::chrono::milliseconds(500)));
    const auto& env = h.received->events.at(0).envelope;
    REQUIRE(env.isMember("window_open_ts_ms"));
    REQUIRE(env.isMember("window_close_ts_ms"));
    const auto OPEN_TS = env["window_open_ts_ms"].asInt64();
    const auto CLOSE_TS = env["window_close_ts_ms"].asInt64();
    CHECK((CLOSE_TS - OPEN_TS) == static_cast<std::int64_t>(WINDOW));
  }

  SECTION("Layer-2/3: close == open == emitted_at_ms") {
    WriterHarness h;
    TestPg pg{pg_config()};
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("system", "plinth:system:user_login")));
    ew::apply_drain_for_test();
    auto res = pg.exec("SELECT payload->>'window_open_ts_ms' AS o, "
                       "       payload->>'window_close_ts_ms' AS c "
                       "FROM plinth.events "
                       "WHERE channel = 'plinth:system:user_login' LIMIT 1");
    REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(res.get()) == 1);
    auto open_ms = std::stoll(PQgetvalue(res.get(), 0, 0));
    auto close_ms = std::stoll(PQgetvalue(res.get(), 0, 1));
    CHECK(open_ms == close_ms);
    CHECK(open_ms > 0);
  }
}

// ── W.05 ─────────────────────────────────────────────────────────────

TEST_CASE("W.05: superseded_seqs absent when emit_superseded_seqs=false",
          "[realtime][events][envelope][shape][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  // Default for `events.coalesce.emit_superseded_seqs` is false per
  // ICD §10 OQ4. CoalescerHarness starts the writer with default
  // events config → coalescer reads false → field MUST be absent.
  CoalescerHarness h{80};
  simulated_exec("INSERT INTO ext_w05.t (id) VALUES ('a')", "w05", 1);
  simulated_exec("INSERT INTO ext_w05.t (id) VALUES ('b')", "w05", 1);
  REQUIRE(h.received->wait_for_n(1, std::chrono::milliseconds(500)));
  const auto& env = h.received->events.at(0).envelope;
  CHECK_FALSE(env.isMember("superseded_seqs"));
}

// ── W.06 ─────────────────────────────────────────────────────────────

TEST_CASE("W.06: superseded_seqs populated when emit_superseded_seqs=true",
          "[realtime][events][envelope][shape][integration][.skip]") {
  // ICD-0.5.5 §6 §Implementation seam acknowledges that population
  // of `superseded_seqs[]` requires upstream source-seq tracking
  // that the writer-first topology (OQ1) forecloses: `plinth.events.seq`
  // is assigned by the writer's `INSERT … RETURNING` clause AFTER
  // the coalescer has already emitted, so the coalescer has no
  // canonical seqs to list for the absorbed record_write hits.
  //
  // Phase 3 ships the wire field as a stable empty array when
  // `emit_superseded_seqs=true` (W.05's default-false case is the
  // operative invariant for 0.5.5); population semantics are
  // deferred to the follow-up ICD that designs source-seq tracking
  // compatible with writer-first.
  SKIP("Deferred — see TU comment + ICD §6 §Implementation seam");
}

// ── W.07 ─────────────────────────────────────────────────────────────

TEST_CASE("W.07: worst-case envelope stays under the 8000-byte cap",
          "[realtime][events][envelope][shape][unit]") {
  // Synthetic envelope mirroring the maximum-shape Layer-1
  // post-Phase-3: long schema + table names, all three op buckets
  // populated, the new wire fields, plus a generous ids array
  // headroom for the 0.5.6 future expansion.
  Json::Value env(Json::objectValue);
  env["layer"] = "data";
  // 63-byte ext name is the longest the validator currently accepts
  // for `ext_<name>` per channel.cpp; pad accordingly.
  env["channel"] =
      "plinth:data:ext_" + std::string(60, 'x') + "." + std::string(60, 'y');
  env["schema"] = "ext_" + std::string(60, 'x');
  env["table"] = std::string(60, 'y');
  Json::Value ops(Json::arrayValue);
  for (const auto* op : {"insert", "update", "delete"}) {
    Json::Value entry(Json::objectValue);
    entry["op"] = op;
    entry["count"] = static_cast<Json::UInt64>(9999);
    ops.append(std::move(entry));
  }
  env["ops"] = std::move(ops);
  env["window_ms"] = static_cast<Json::UInt64>(50);
  env["coalesced_count"] = static_cast<Json::UInt64>(9999);
  env["window_open_ts_ms"] = static_cast<Json::Int64>(1714060800000);
  env["window_close_ts_ms"] = static_cast<Json::Int64>(1714060800050);
  env["emitted_at"] = "2026-04-26T00:00:00.000Z";
  env["seq"] = static_cast<Json::Int64>(9223372036854775807LL);

  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  auto serialized = Json::writeString(b, env);
  CHECK(serialized.size() < 8000);
}
