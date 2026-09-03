// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §Test Cases — P.* per-op SET LOCAL search_path isolation.
//
// Closes ICD-0.3.3 §Security Constraint 1 (per-op search_path
// wrapper) AND ICD-0.5.0.3 §P.04 test-deferred-on-search_path.
//
// Behavioral assertion style per the 2026-04-24 planning exchange
// (maintainer-approved): "row lands in `ext_<name>.*` not `public.*`"
// is sufficient to prove the SET LOCAL wrapper fired, without
// reaching into PG wire logs (which have no seam on this host).

#include "async_bridge_fixture.hpp"

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>

#include <array>
#include <chrono>
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

namespace {

auto drive(BridgeContext& bc, std::string_view source) -> EvalResult {
  return drogon::sync_wait(run_on_context(bc, source));
}

auto make_pool(const plinth::Config& cfg) -> RuntimePool {
  return {/*ext=*/nullptr, default_runtime_limits(), cfg, 1};
}

auto eval_as(RuntimePool& pool, std::string_view ext_name,
             std::string_view source) -> EvalResult {
  auto* bc = pool.acquire();
  bc->extension_name = ext_name;
  auto r = drive(*bc, source);
  pool.destroy(bc);
  return r;
}

// Raw libpq session for schema-landing verification.
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

  auto exec(const std::string& sql) const -> void {
    PGresult* res = PQexec(conn, sql.c_str());
    PQclear(res);
  }

  [[nodiscard]] auto count_rows(const std::string& qualified_table) const
      -> int {
    PGresult* res =
        PQexec(conn, ("SELECT COUNT(*) FROM " + qualified_table).c_str());
    int n = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      n = std::stoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return n;
  }

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
};

auto reset_for_search_path_test(const plinth::Config& cfg) -> void {
  reset_schema(cfg.db);
  plinth::js::db::reset_search_path_audit_for_test();
  plinth::js::db::set_search_path_enforce(true);
}

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

} // namespace

// ─── P.01 ─────────────────────────────────────────────────────────
TEST_CASE("P.01: single-op wrapper — unqualified INSERT lands in ext_ schema",
          "[js][async][db][search_path]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_search_path_test(cfg);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_notes CASCADE");
  pg.exec("CREATE SCHEMA ext_notes");
  pg.exec("CREATE TABLE ext_notes.notes (id serial primary key, "
          "body text)");

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "notes",
                   "db.exec(\"INSERT INTO notes(body) VALUES('hello')\")");
  REQUIRE(r.value.has_value());

  // Behavioral assertion: the unqualified `notes` resolved to
  // `ext_notes.notes` (the wrapper fired), NOT `public.notes`
  // (would mean the wrapper didn't fire).
  REQUIRE(pg.count_rows("ext_notes.notes") == 1);
}

// ─── P.03 ─────────────────────────────────────────────────────────
TEST_CASE("P.03: kernel-scope bypass — empty extension_name runs unwrapped",
          "[js][async][db][search_path]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_search_path_test(cfg);

  TestPg pg(cfg.db);
  pg.exec("CREATE TABLE IF NOT EXISTS plinth.host_notes "
          "(id serial primary key, body text)");
  pg.exec("TRUNCATE plinth.host_notes");

  // Kernel-scope bc (extension_name = "") — wrapper is bypassed
  // per ICD §Kernel-scope bypass. Verify via fully-qualified DML
  // that still succeeds (raw exec, no SET LOCAL).
  auto pool = make_pool(cfg);
  auto r =
      eval_as(pool, /*ext_name=*/"",
              "db.exec(\"INSERT INTO plinth.host_notes(body) VALUES('h')\")");
  REQUIRE(r.value.has_value());
  REQUIRE(pg.count_rows("plinth.host_notes") == 1);
}

// ─── P.04 ─────────────────────────────────────────────────────────
TEST_CASE("P.04: cross-extension schema isolation — PG permissions prevail",
          "[js][async][db][search_path]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_search_path_test(cfg);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_notes CASCADE");
  pg.exec("DROP SCHEMA IF EXISTS ext_terminal CASCADE");
  pg.exec("CREATE SCHEMA ext_notes");
  pg.exec("CREATE SCHEMA ext_terminal");
  pg.exec("CREATE TABLE ext_terminal.sessions (id serial primary key, "
          "body text)");

  // `notes` ext writes against `ext_terminal.sessions`. Both schemas
  // are created by the superuser test connection, so PG permissions
  // don't block the write in this harness — instead, the assertion
  // is that the search_path wrapper does NOT expand the unqualified
  // table name to ext_terminal (it expands to ext_notes). The
  // qualified write still works; test proves the search_path
  // doesn't silently redirect cross-extension writes.
  auto pool = make_pool(cfg);
  auto r = eval_as(
      pool, "notes",
      "db.exec(\"INSERT INTO ext_terminal.sessions(body) VALUES('x')\")");
  REQUIRE(r.value.has_value());
  REQUIRE(pg.count_rows("ext_terminal.sessions") == 1);
  // The unqualified-`sessions` case: the wrapper points search_path
  // at ext_notes. `sessions` doesn't exist there → PG errors.
  auto r2 = eval_as(pool, "notes",
                    "db.exec(\"INSERT INTO sessions(body) VALUES('x')\")"
                    "  .then(() => 'ok', e => e.code)");
  REQUIRE(r2.value.has_value());
  REQUIRE(r2.value->asString() == "db.undefined_table");
}

// ─── P.05 ─────────────────────────────────────────────────────────
// ICD amendment 2026-04-24: the ICD's §Failure mode example
// ("extension schema ext_<id> was dropped out-of-band") does NOT
// cause `SET LOCAL search_path` to fail — PostgreSQL's SET LOCAL is
// permissive and accepts non-existent schemas (the subsequent
// unqualified SQL will fail with db.undefined_table instead). The
// `db.search_path.set_failed` rejection + audit fire only on the
// identity-regex defense at pre-flight (see `is_valid_extension_name`
// check). P.05 narrowed accordingly.
TEST_CASE("P.05: pre-flight regex defense rejects malicious extension_name",
          "[js][async][db][search_path]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_search_path_test(cfg);
  TestPg pg(cfg.db);

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "; DROP TABLE users --",
                   "db.exec(\"INSERT INTO notes(body) VALUES('x')\")"
                   "  .then(() => 'ok', e => e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "db.search_path.set_failed");
  REQUIRE(wait_for_audit(pg, "db.search_path.set_failed",
                         "; DROP TABLE users --", 1));
}

// ─── P.06 ─────────────────────────────────────────────────────────
TEST_CASE("P.06: search_path resets between ops — no cross-call leak",
          "[js][async][db][search_path]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_search_path_test(cfg);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_alpha CASCADE");
  pg.exec("DROP SCHEMA IF EXISTS ext_beta CASCADE");
  pg.exec("CREATE SCHEMA ext_alpha");
  pg.exec("CREATE SCHEMA ext_beta");
  pg.exec("CREATE TABLE ext_alpha.marker (id serial primary key, "
          "body text)");
  pg.exec("CREATE TABLE ext_beta.marker  (id serial primary key, "
          "body text)");

  auto pool = make_pool(cfg);
  // Two sequential calls from different ext identities hit different
  // schemas — proves search_path does not leak from the first to
  // the second through any pool-connection-checkout state.
  auto r1 = eval_as(pool, "alpha",
                    "db.exec(\"INSERT INTO marker(body) VALUES('a')\")");
  REQUIRE(r1.value.has_value());
  auto r2 = eval_as(pool, "beta",
                    "db.exec(\"INSERT INTO marker(body) VALUES('b')\")");
  REQUIRE(r2.value.has_value());

  REQUIRE(pg.count_rows("ext_alpha.marker") == 1);
  REQUIRE(pg.count_rows("ext_beta.marker") == 1);
}

// ─── P.07 ─────────────────────────────────────────────────────────
TEST_CASE("P.07: identity-regex defense — malicious extension_name rejected",
          "[js][async][db][search_path]") {
  ensure_drogon_with_db_running();

  // Phase 3 adopts the regex-validation layer over
  // PQescapeIdentifier — the identity is re-validated at dispatch
  // time. `[a-z][a-z0-9_]*` must fail cleanly on inputs including
  // whitespace / punctuation / non-ASCII characters without
  // reaching the SET LOCAL statement.
  REQUIRE(!plinth::js::db::is_valid_extension_name(""));
  REQUIRE(!plinth::js::db::is_valid_extension_name("; DROP TABLE --"));
  REQUIRE(!plinth::js::db::is_valid_extension_name("NOTES"));  // uppercase
  REQUIRE(!plinth::js::db::is_valid_extension_name("1notes")); // leading digit
  REQUIRE(!plinth::js::db::is_valid_extension_name("ext-notes"));
  REQUIRE(!plinth::js::db::is_valid_extension_name(" notes"));
  REQUIRE(!plinth::js::db::is_valid_extension_name("注釈"));

  // Happy-path names that MUST pass — regression for ICD-0.4.1
  // install-time names.
  REQUIRE(plinth::js::db::is_valid_extension_name("notes"));
  REQUIRE(plinth::js::db::is_valid_extension_name("a"));
  REQUIRE(plinth::js::db::is_valid_extension_name("ext_x1_y2"));
  REQUIRE(plinth::js::db::is_valid_extension_name("terminal2"));
}

// ─── P.08 ─────────────────────────────────────────────────────────
TEST_CASE("P.08: enforce=false disables wrapper — raw exec, no BEGIN/COMMIT",
          "[js][async][db][search_path]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_search_path_test(cfg);
  plinth::js::db::set_search_path_enforce(false);

  TestPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_notes CASCADE");
  pg.exec("CREATE SCHEMA ext_notes");
  pg.exec("CREATE TABLE ext_notes.notes (id serial primary key, "
          "body text)");
  // Also create a public.notes to observe that the wrapper-off path
  // hits PG's default search_path (which includes "public" by
  // default on a fresh DB).
  pg.exec("DROP TABLE IF EXISTS public.notes");
  pg.exec("CREATE TABLE public.notes (id serial primary key, "
          "body text)");

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "notes",
                   "db.exec(\"INSERT INTO notes(body) VALUES('hi')\")");
  REQUIRE(r.value.has_value());

  // With enforce=false, no SET LOCAL fires → unqualified `notes`
  // resolves via PG's default search_path. Depending on PG's schema
  // path default (usually `\"$user\", public`), the row lands in
  // public.notes — NOT ext_notes.notes (which would require the
  // wrapper).
  REQUIRE(pg.count_rows("public.notes") == 1);
  REQUIRE(pg.count_rows("ext_notes.notes") == 0);

  // Restore for subsequent tests in the subprocess.
  plinth::js::db::set_search_path_enforce(true);

  // Cleanup: this is the only test in the file that creates a table
  // in `public`. Other tests in the [ws] subprocess (lifecycle X.05's
  // schema-isolation leak assertion in particular) check that no
  // `notes` table leaks into `public`, and would false-positive on
  // this test's intentional fixture state if we didn't drop it.
  pg.exec("DROP TABLE IF EXISTS public.notes");
}

// P.02 defers to phase 4 (B.02 single-SET-LOCAL-per-batch assertion
// belongs with the db.batch TU where the batch-scope wrapper lives).
