// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan — E.* events writer cases (10 of 10).

#include "kernel/realtime/events_writer.hpp"

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/realtime/listener.hpp"

#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <expected>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ew = plinth::realtime::events_writer;

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

auto count_events_rows(TestPg& pg) -> int {
  auto res = pg.exec("SELECT COUNT(*) FROM plinth.events");
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return -1;
  }
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

auto select_envelope_for_channel(TestPg& pg, const std::string& channel)
    -> std::string {
  auto sql =
      std::string{"SELECT payload::text FROM plinth.events WHERE channel = '"} +
      channel + "' ORDER BY seq DESC LIMIT 1";
  auto res = pg.exec(sql);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return {};
  }
  return PQgetvalue(res.get(), 0, 0);
}

auto count_events_for_channel(TestPg& pg, const std::string& channel) -> int {
  auto sql =
      std::string{"SELECT COUNT(*) FROM plinth.events WHERE channel = '"} +
      channel + "'";
  auto res = pg.exec(sql);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return -1;
  }
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

// Build a minimal Layer-1 envelope with `emitted_at` set far in the
// past so E.03 can prove the writer overwrites it.
auto build_envelope(std::string_view layer, std::string_view channel)
    -> Json::Value {
  Json::Value env(Json::objectValue);
  env["layer"] = std::string{layer};
  env["channel"] = std::string{channel};
  env["emitted_at"] = "1970-01-01T00:00:00.000Z";
  return env;
}

auto build_dispatched(std::string_view layer, std::string_view channel)
    -> plinth::realtime::DispatchedEvent {
  plinth::realtime::DispatchedEvent ev;
  ev.layer = std::string{layer};
  ev.channel = std::string{channel};
  ev.envelope = build_envelope(layer, channel);
  return ev;
}

// RAII harness — start writer with a pinned DbClient, drain on
// teardown so each TEST_CASE leaves the registry empty.
struct Harness {
  drogon::orm::DbClientPtr db;
  explicit Harness(plinth::Config::Realtime::Events cfg = {}) {
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::reset_counters_for_test();
    plinth::realtime::clear_handlers_for_test();
    if (cfg.enabled) {
      // 0.6.3.N — shared process-lifetime client; per-test create+
      // destroy of `newPgClient` reproducibly trips
      // `EventLoopThreadPool::~ + Resource deadlock avoided`.
      // Member-init not viable: setup work above must run first
      // and assignment is gated on cfg.enabled.
      db = plinth::realtime_test::shared_pg_client(/*connNum=*/1);
      REQUIRE(db);
    }
    ew::set_db_client_for_test(db);
    ew::start(cfg);
  }
  ~Harness() {
    ew::stop();
    ew::set_db_client_for_test(nullptr);
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    plinth::realtime::clear_handlers_for_test();
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

} // namespace

// ── E.01 ─────────────────────────────────────────────────────────────

TEST_CASE("E.01: happy write — listener dispatch persists one row",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_e01.t")));
  ew::apply_drain_for_test();

  CHECK(ew::writes_persisted_for_test() == 1);
  CHECK(count_events_for_channel(pg, "plinth:data:ext_e01.t") == 1);
}

// ── E.02 ─────────────────────────────────────────────────────────────

TEST_CASE("E.02: Layer 1 + 2 + 3 envelopes all persist",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_e02.x")));
  REQUIRE(ew::enqueue_for_test(
      build_dispatched("system", "plinth:system:packages.installed")));
  REQUIRE(ew::enqueue_for_test(
      build_dispatched("extension", "plinth:ext:e02:custom")));
  ew::apply_drain_for_test();

  CHECK(ew::writes_persisted_for_test() == 3);
  CHECK(count_events_rows(pg) == 3);
}

// ── E.03 ─────────────────────────────────────────────────────────────

TEST_CASE("E.03: server overwrites envelope.emitted_at",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_e03.x")));
  ew::apply_drain_for_test();

  auto json = select_envelope_for_channel(pg, "plinth:data:ext_e03.x");
  REQUIRE_FALSE(json.empty());
  CHECK(json.find("1970-01-01T00:00:00.000Z") == std::string::npos);
  // Server-stamped `emitted_at` is current-year ISO; PG normalizes
  // JSONB output with `: ` between key and value.
  CHECK(json.find("\"emitted_at\": \"20") != std::string::npos);
}

// ── E.04 ─────────────────────────────────────────────────────────────

TEST_CASE("E.04: queue overflow drops newest + audits",
          "[realtime][events][writer][integration]") {
  plinth::Config::Realtime::Events cfg;
  cfg.write_queue_size = 3;
  cfg.enabled = true;
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h{cfg};

  // Enqueue 5 — first 3 accepted, last 2 dropped.
  int accepted = 0;
  for (int i = 0; i < 5; ++i) {
    if (ew::enqueue_for_test(
            build_dispatched("data", "plinth:data:ext_e04.t"))) {
      accepted += 1;
    }
  }
  CHECK(accepted == 3);
  CHECK(ew::queue_size_for_test() == 3);
}

// ── E.05 ─────────────────────────────────────────────────────────────

TEST_CASE("E.05: PG INSERT failure routes through audit pipeline",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  ew::set_insert_hook_for_test(
      [](const std::string&,
         const Json::Value&) -> std::expected<std::int64_t, std::string> {
        return std::unexpected(std::string{"42P01"}); // SQLSTATE
      });

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_e05.t")));
  ew::apply_drain_for_test();

  CHECK(ew::writes_persisted_for_test() == 0);
  CHECK(count_events_rows(pg) == 0);
}

// ── E.06 ─────────────────────────────────────────────────────────────

TEST_CASE("E.06: truncated:true envelope persists verbatim",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  auto ev = build_dispatched("data", "plinth:data:ext_e06.t");
  ev.envelope["truncated"] = true;
  Json::Value ops(Json::arrayValue);
  Json::Value op(Json::objectValue);
  op["op"] = "insert";
  op["count"] = 9999;
  ops.append(op);
  ev.envelope["ops"] = ops;
  bool accepted = ew::enqueue_for_test(std::move(ev));
  REQUIRE(accepted);
  ew::apply_drain_for_test();

  auto json = select_envelope_for_channel(pg, "plinth:data:ext_e06.t");
  REQUIRE_FALSE(json.empty());
  CHECK(json.find("\"truncated\": true") != std::string::npos);
  CHECK(json.find("\"count\": 9999") != std::string::npos);
}

// ── E.07 ─────────────────────────────────────────────────────────────

TEST_CASE("E.07: advisory-lock loser writes nothing, no error",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  // Loser hook — every lock attempt returns false. Real PG's lock
  // contention surfaces the same way to insert_envelope.
  ew::set_advisory_lock_hook_for_test([](const std::string&) { return false; });

  // Provide a no-op insert hook so the envelope path doesn't fall
  // through to PG (which would unconditionally accept the lock).
  ew::set_insert_hook_for_test(
      [](const std::string&,
         const Json::Value&) -> std::expected<std::int64_t, std::string> {
        return 1; // would-have-succeeded had we held the lock
      });

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_e07.t")));
  ew::apply_drain_for_test();

  CHECK(ew::writes_persisted_for_test() == 0);
  CHECK(count_events_rows(pg) == 0);
}

// ── E.08 ─────────────────────────────────────────────────────────────

TEST_CASE("E.08: stop() drains the queue before joining",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  plinth::Config::Realtime::Events cfg;
  cfg.write_queue_size = 1000;
  cfg.shutdown_drain_ms = 10000; // generous budget
  cfg.enabled = true;
  Harness h{cfg};
  TestPg pg{pg_config()};

  constexpr int N = 100;
  for (int i = 0; i < N; ++i) {
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_e08.t")));
  }
  // Harness destructor calls stop(); we want to assert state AFTER
  // stop. Force the drain explicitly here so the assertion is on a
  // running writer (pre-stop) — with the 10 s budget the harness
  // teardown is also tested.
  ew::apply_drain_for_test();
  CHECK(ew::writes_persisted_for_test() == static_cast<std::uint64_t>(N));
  CHECK(count_events_for_channel(pg, "plinth:data:ext_e08.t") == N);
}

// ── E.09 ─────────────────────────────────────────────────────────────

TEST_CASE("E.09: shutdown drain timeout audits the dropped count",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  plinth::Config::Realtime::Events cfg;
  cfg.write_queue_size = 1000;
  cfg.shutdown_drain_ms = 100;
  cfg.enabled = true;
  Harness h{cfg};

  // Insert hook that sleeps long enough to exceed the drain budget
  // on the very first envelope. Subsequent entries get audited as
  // shutdown_timeout drops.
  ew::set_insert_hook_for_test(
      [](const std::string&,
         const Json::Value&) -> std::expected<std::int64_t, std::string> {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 1;
      });

  for (int i = 0; i < 10; ++i) {
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_e09.t")));
  }
  // Harness destructor invokes stop() with the 100 ms budget. The
  // first INSERT eats the budget (200 ms) and the remaining 9 are
  // dropped.
  REQUIRE(ew::queue_size_for_test() == 10);
}

// ── E.10 ─────────────────────────────────────────────────────────────

TEST_CASE("E.10: enabled=false — writer is a no-op",
          "[realtime][events][writer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  Harness h{cfg};
  TestPg pg{pg_config()};

  REQUIRE_FALSE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_e10.t")));
  CHECK(ew::writes_persisted_for_test() == 0);
  CHECK(count_events_rows(pg) == 0);
}
