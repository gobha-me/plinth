// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §Test Cases — S.* silent flag semantics pin.
//
// The `db.exec(sql, params, {silent: true})` plumbing has been live
// since 0.3.3 (AsyncOp::silent field + db_bindings parse +
// run_on_context gate). 0.5.3 adds the rate-limited `db.silent.used`
// audit at the gate and locks the contract with these tests.
//
// Audit-row assertions use libpq directly (bypassing Drogon) so the
// tests don't race the async audit writer. A bounded poll loop gives
// the fire-and-forget audit insert time to land before assertion.

#include "async_bridge_fixture.hpp"

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/db_silent_audit.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/coalescer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::reset_schema;
using plinth::async_bridge_test::test_config;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::EvalResult;
using plinth::js::run_on_context;
using plinth::js::RuntimePool;
using plinth::realtime::CoalescerRegistry;

namespace {

auto drive(BridgeContext& bc, std::string_view source) -> EvalResult {
  return drogon::sync_wait(run_on_context(bc, source));
}

auto make_pool(const plinth::Config& cfg) -> RuntimePool {
  return {/*ext=*/nullptr, default_runtime_limits(), cfg, 1};
}

// Acquire-eval-destroy. For 0.5.3 phase 2 S.* tests the BridgeContext
// needs `extension_name` populated so the rate-limiter keys by the
// test-chosen token instead of the default empty string.
auto eval_as(RuntimePool& pool, std::string_view ext_name,
             std::string_view source) -> EvalResult {
  auto* bc = pool.acquire();
  bc->extension_name = ext_name;
  auto r = drive(*bc, source);
  pool.destroy(bc);
  return r;
}

// Raw libpq session for audit-row verification. Mirrors the
// coalescer_integration_test pattern.
struct TestPg {
  PGconn* conn{nullptr};

  explicit TestPg(const plinth::Config::Database& db) {
    auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                    " dbname=" + db.database + " user=" + db.user +
                    " password=" + db.password + " connect_timeout=3";
    conn = PQconnectdb(conninfo.c_str());
    REQUIRE(PQstatus(conn) == CONNECTION_OK);
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

  [[nodiscard]] auto count_audit(const std::string& action,
                                 const std::string& extension) const -> int {
    std::array<const char*, 2> params{action.c_str(), extension.c_str()};
    PGresult* res =
        PQexecParams(conn,
                     "SELECT COUNT(*) FROM plinth.audit_log "
                     "WHERE action = $1 AND detail->>'extension' = $2",
                     2, nullptr, params.data(), nullptr, nullptr, 0);
    int n = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      n = std::stoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return n;
  }

  [[nodiscard]] auto count_audit_any(const std::string& action) const -> int {
    std::array<const char*, 1> params{action.c_str()};
    PGresult* res = PQexecParams(
        conn, "SELECT COUNT(*) FROM plinth.audit_log WHERE action = $1", 1,
        nullptr, params.data(), nullptr, nullptr, 0);
    int n = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      n = std::stoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return n;
  }

  auto exec(std::string_view sql) const -> void {
    PGresult* res = PQexec(conn, std::string{sql}.c_str());
    PQclear(res);
  }
};

// Poll for an audit row to appear. Audit writes are fire-and-forget
// via Drogon's DbClient — the `db.exec` promise resolves before the
// audit insert commits, so tests need a bounded wait.
auto wait_for_audit(const TestPg& pg, const std::string& action,
                    const std::string& extension, int expect) -> bool {
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + std::chrono::seconds{2};
  while (clock::now() < deadline) {
    if (pg.count_audit(action, extension) >= expect) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  return pg.count_audit(action, extension) >= expect;
}

// Common prologue shared across the suite: wipe schema, reset
// coalescer + silent-audit state so each test sees a known baseline.
auto reset_for_silent_test(const plinth::Config& cfg) -> void {
  reset_schema(cfg.db);
  CoalescerRegistry::instance().clear_windows_for_test();
  plinth::js::reset_silent_audit_for_test();
  plinth::js::set_silent_audit_window_ms(60000);
}

} // namespace

// ─── S.01 ─────────────────────────────────────────────────────────
TEST_CASE("S.01: silent=true suppresses coalescer; row written; audit fires",
          "[js][async][db][silent]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_silent_test(cfg);

  TestPg pg(cfg.db);
  // reset_schema only drops plinth.*; ext_* schemas from prior runs
  // persist and would inflate row counts. Explicit drop + recreate.
  pg.exec("DROP SCHEMA IF EXISTS ext_silent_s01 CASCADE");
  pg.exec("CREATE SCHEMA ext_silent_s01");
  pg.exec("CREATE TABLE ext_silent_s01.notes (id serial primary key, "
          "body text)");

  auto pool = make_pool(cfg);
  auto r =
      eval_as(pool, "silent_s01",
              "db.exec(\"INSERT INTO ext_silent_s01.notes(body) VALUES('x')\","
              "        [], {silent: true})");
  REQUIRE(r.value.has_value());

  // Row landed — silent suppresses envelope only, not the write.
  PGresult* res = PQexec(pg.conn, "SELECT COUNT(*) FROM ext_silent_s01.notes");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(res, 0, 0)} == "1");
  PQclear(res);

  // Envelope suppressed — coalescer never saw a record_write.
  REQUIRE(CoalescerRegistry::instance().open_window_count_for_test() == 0);

  // `db.silent.used` audit fired.
  REQUIRE(wait_for_audit(pg, "db.silent.used", "silent_s01", 1));
}

// ─── S.02 ─────────────────────────────────────────────────────────
TEST_CASE("S.02: silent=true + zero-row UPDATE; no envelope; audit still fires",
          "[js][async][db][silent]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_silent_test(cfg);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_silent_s02 CASCADE");
  pg.exec("CREATE SCHEMA ext_silent_s02");
  pg.exec("CREATE TABLE ext_silent_s02.notes (id serial primary key, "
          "body text)");

  auto pool = make_pool(cfg);
  auto r = eval_as(
      pool, "silent_s02",
      "db.exec(\"UPDATE ext_silent_s02.notes SET body='x' WHERE false\","
      "        [], {silent: true})");
  REQUIRE(r.value.has_value());
  REQUIRE((*r.value)["row_count"].asInt() == 0);

  REQUIRE(CoalescerRegistry::instance().open_window_count_for_test() == 0);
  REQUIRE(wait_for_audit(pg, "db.silent.used", "silent_s02", 1));
}

// ─── S.03 ─────────────────────────────────────────────────────────
TEST_CASE("S.03: silent=true does not suppress `audit.log` — both rows fire",
          "[js][async][db][silent]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_silent_test(cfg);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_silent_s03 CASCADE");
  pg.exec("CREATE SCHEMA ext_silent_s03");
  pg.exec("CREATE TABLE ext_silent_s03.notes (id serial primary key, "
          "body text)");

  // One JS call does both: audit.log + db.exec(silent). Both audit
  // rows must land (silent suppresses Layer-1 realtime events, NOT
  // the audit side-channel).
  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "silent_s03",
                   "(async () => {"
                   "  await audit.log('ext.test.marker', {k: 1});"
                   "  await db.exec(\"INSERT INTO ext_silent_s03.notes(body) "
                   "                    VALUES('x')\","
                   "                [], {silent: true});"
                   "  return 'ok';"
                   "})()");
  REQUIRE(r.value.has_value());

  REQUIRE(wait_for_audit(pg, "db.silent.used", "silent_s03", 1));
  // The audit.log call emits action="ext.test.marker" unconditionally;
  // any count ≥ 1 verifies the side-channel is independent of silent.
  REQUIRE(pg.count_audit_any("ext.test.marker") >= 1);
}

// ─── S.04 ─────────────────────────────────────────────────────────
TEST_CASE("S.04: silent=true on db.query is a silently-ignored no-op",
          "[js][async][db][silent]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_silent_test(cfg);
  TestPg pg(cfg.db);

  // db.query's 3rd argument is unread per ICD-0.3.3 "unknown opts
  // silently ignored". Call completes normally; no silent audit
  // fires because the gate lives in DB_EXEC's outcome only.
  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "silent_s04",
                   "db.query('SELECT 1 AS one', [], {silent: true})"
                   "  .then(r => r.rows[0].one)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asInt() == 1);

  // Give the audit path time to not fire (harder-to-verify negative —
  // a short poll interval is the cost of confidence).
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  REQUIRE(pg.count_audit("db.silent.used", "silent_s04") == 0);
}

// ─── S.05 (pointer; covered by B.09 in phase 4) ───────────────────
// Silent-within-batch semantics ship in phase 4 with the db.batch
// core (B.09 in `db_batch_test.cpp`).

// ─── S.06 ─────────────────────────────────────────────────────────
TEST_CASE("S.06: short audit_window aggregates bursty silent writes",
          "[js][async][db][silent]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_silent_test(cfg);
  // Shrink the window to the [1000, 3600000] ms min so the test
  // can observe two emissions without a multi-second sleep.
  plinth::js::set_silent_audit_window_ms(1000);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_silent_s06 CASCADE");
  pg.exec("CREATE SCHEMA ext_silent_s06");
  pg.exec("CREATE TABLE ext_silent_s06.notes (id serial primary key, "
          "body text)");

  auto pool = make_pool(cfg);
  auto burst = [&](std::string_view label) {
    auto r =
        eval_as(pool, "silent_s06",
                "db.exec(\"INSERT INTO ext_silent_s06.notes(body) VALUES($1)\","
                "        ['" +
                    std::string{label} + "'], {silent: true})");
    REQUIRE(r.value.has_value());
  };

  // Write 1 — window opens, initial audit fires with count=1.
  burst("a");
  REQUIRE(wait_for_audit(pg, "db.silent.used", "silent_s06", 1));

  // Writes 2 and 3 — window open, both increment suppressed counter.
  burst("b");
  burst("c");
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  REQUIRE(pg.count_audit("db.silent.used", "silent_s06") == 1);

  // Sleep past window end; the NEXT write emits the aggregated audit
  // with count_in_window = 3 (the 2 suppressed + this one).
  std::this_thread::sleep_for(std::chrono::milliseconds{1100});
  burst("d");
  REQUIRE(wait_for_audit(pg, "db.silent.used", "silent_s06", 2));

  // Explicitly verify the second audit row's count_in_window = 3.
  std::array<const char*, 2> params{"db.silent.used", "silent_s06"};
  PGresult* res =
      PQexecParams(pg.conn,
                   "SELECT detail->>'count_in_window' FROM plinth.audit_log "
                   "WHERE action = $1 AND detail->>'extension' = $2 "
                   "ORDER BY timestamp ASC",
                   2, nullptr, params.data(), nullptr, nullptr, 0);
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 2);
  REQUIRE(std::string{PQgetvalue(res, 0, 0)} == "1");
  REQUIRE(std::string{PQgetvalue(res, 1, 0)} == "3");
  PQclear(res);
}
