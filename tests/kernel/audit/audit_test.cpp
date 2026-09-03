#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <vector>

#include "kernel/audit/handlers.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/logging.hpp"
#include "kernel/rbac/enforcement.hpp"

// ── Integration test helpers ─────────────────────────────────────
// These tests exercise the audit read path + retention purge via
// direct libpq, matching the convention used by
// tests/kernel/groups/rbac_integration_test.cpp.

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

struct TestPg {
  PGconn* conn = nullptr;

  explicit TestPg(const plinth::Config::Database& db) {
    auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                    " dbname=" + db.database + " user=" + db.user +
                    " password=" + db.password;
    conn = PQconnectdb(conninfo.c_str());
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

  [[nodiscard]] auto exec_params(const std::string& sql,
                                 const std::vector<std::string>& params) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
      values.push_back(p.c_str());
    }
    return {PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()),
                         nullptr, values.data(), nullptr, nullptr, 0),
            PQclear};
  }
};

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);
  plinth::log::set_node_id("test-node");
  plinth::groups::bootstrap_groups(db);
}

// Insert a synthetic audit row with an explicit timestamp.
// Empty user_id/session_id/ip_address → SQL NULL (via NULLIF).
auto insert_audit(TestPg& pg, const std::string& timestamp,
                  const std::string& action, const std::string& user_id = "",
                  const std::string& session_id = "",
                  const std::string& detail_json = "{}",
                  const std::string& ip = "",
                  const std::string& node_id = "test-node") -> void {
  auto res = pg.exec_params(
      "INSERT INTO plinth.audit_log "
      "(timestamp, action, user_id, session_id, detail, ip_address, node_id) "
      "VALUES ($1::timestamptz, $2, NULLIF($3, '')::uuid, "
      "NULLIF($4, '')::uuid, $5::jsonb, NULLIF($6, '')::inet, $7)",
      {timestamp, action, user_id, session_id, detail_json, ip, node_id});
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
}

auto clear_audit_log(TestPg& pg) -> void {
  auto res = pg.exec("DELETE FROM plinth.audit_log");
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
}

// Exact SELECT SQL used by handle_list_audit — mirrored here so the test
// validates the same WHERE semantics the handler composes.
constexpr const char* SELECT_SQL =
    "SELECT id, timestamp, action, user_id, session_id, "
    "       detail, ip_address, node_id "
    "FROM plinth.audit_log "
    "WHERE CASE WHEN $1 = '' THEN TRUE ELSE action = $1 END "
    "  AND CASE WHEN $2 = '' THEN TRUE ELSE user_id = $2::uuid END "
    "  AND CASE WHEN $3 = '' THEN TRUE ELSE timestamp >= $3::timestamptz END "
    "  AND CASE WHEN $4 = '' THEN TRUE ELSE timestamp <= $4::timestamptz END "
    "ORDER BY timestamp DESC "
    "LIMIT $5 OFFSET $6";

constexpr const char* COUNT_SQL =
    "SELECT COUNT(*) AS total FROM plinth.audit_log "
    "WHERE CASE WHEN $1 = '' THEN TRUE ELSE action = $1 END "
    "  AND CASE WHEN $2 = '' THEN TRUE ELSE user_id = $2::uuid END "
    "  AND CASE WHEN $3 = '' THEN TRUE ELSE timestamp >= $3::timestamptz END "
    "  AND CASE WHEN $4 = '' THEN TRUE ELSE timestamp <= $4::timestamptz END";

auto query_select(TestPg& pg, const std::string& action,
                  const std::string& user_id, const std::string& start_ts,
                  const std::string& end_ts, int limit, int offset)
    -> std::unique_ptr<PGresult, decltype(&PQclear)> {
  return pg.exec_params(SELECT_SQL,
                        {action, user_id, start_ts, end_ts,
                         std::to_string(limit), std::to_string(offset)});
}

auto query_count(TestPg& pg, const std::string& action,
                 const std::string& user_id, const std::string& start_ts,
                 const std::string& end_ts) -> int64_t {
  auto res = pg.exec_params(COUNT_SQL, {action, user_id, start_ts, end_ts});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  return std::stoll(PQgetvalue(res.get(), 0, 0));
}

constexpr const char* UUID_A = "11111111-1111-1111-1111-111111111111";
constexpr const char* UUID_B = "22222222-2222-2222-2222-222222222222";

} // namespace

// ── Query-layer tests ───────────────────────────────────────────────

TEST_CASE("Audit query: no filters returns all rows ordered DESC",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "a.earlier");
  insert_audit(pg, "2026-01-02T00:00:00Z", "b.middle");
  insert_audit(pg, "2026-01-03T00:00:00Z", "c.latest");

  auto res = query_select(pg, "", "", "", "", 100, 0);
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 3);
  // Newest first
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "c.latest");
  REQUIRE(std::string{PQgetvalue(res.get(), 1, 2)} == "b.middle");
  REQUIRE(std::string{PQgetvalue(res.get(), 2, 2)} == "a.earlier");
}

TEST_CASE("Audit query: action filter matches exact action",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "user.login");
  insert_audit(pg, "2026-01-02T00:00:00Z", "user.logout");
  insert_audit(pg, "2026-01-03T00:00:00Z", "user.login");

  auto res = query_select(pg, "user.login", "", "", "", 100, 0);
  REQUIRE(PQntuples(res.get()) == 2);
  REQUIRE(query_count(pg, "user.login", "", "", "") == 2);
}

TEST_CASE("Audit query: user_id UUID filter", "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "user.login", UUID_A);
  insert_audit(pg, "2026-01-02T00:00:00Z", "user.login", UUID_B);
  insert_audit(pg, "2026-01-03T00:00:00Z", "user.login", UUID_A);

  auto res = query_select(pg, "", UUID_A, "", "", 100, 0);
  REQUIRE(PQntuples(res.get()) == 2);
  REQUIRE(query_count(pg, "", UUID_A, "", "") == 2);
}

TEST_CASE("Audit query: start timestamp filter is inclusive",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "old");
  insert_audit(pg, "2026-01-02T00:00:00Z", "boundary");
  insert_audit(pg, "2026-01-03T00:00:00Z", "new");

  auto res = query_select(pg, "", "", "2026-01-02T00:00:00Z", "", 100, 0);
  REQUIRE(PQntuples(res.get()) == 2);
  // The boundary row should be included
  REQUIRE(std::string{PQgetvalue(res.get(), 1, 2)} == "boundary");
}

TEST_CASE("Audit query: end timestamp filter is inclusive",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "old");
  insert_audit(pg, "2026-01-02T00:00:00Z", "boundary");
  insert_audit(pg, "2026-01-03T00:00:00Z", "new");

  auto res = query_select(pg, "", "", "", "2026-01-02T00:00:00Z", 100, 0);
  REQUIRE(PQntuples(res.get()) == 2);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "boundary");
}

TEST_CASE("Audit query: combined action + user_id filters AND together",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "user.login", UUID_A);
  insert_audit(pg, "2026-01-02T00:00:00Z", "user.logout", UUID_A);
  insert_audit(pg, "2026-01-03T00:00:00Z", "user.login", UUID_B);

  auto res = query_select(pg, "user.login", UUID_A, "", "", 100, 0);
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 3)} == UUID_A);
}

TEST_CASE("Audit query: limit caps result count", "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  for (int i = 0; i < 10; ++i) {
    insert_audit(pg, "2026-01-01T00:00:0" + std::to_string(i) + "Z",
                 "evt" + std::to_string(i));
  }

  auto res = query_select(pg, "", "", "", "", 3, 0);
  REQUIRE(PQntuples(res.get()) == 3);
}

TEST_CASE("Audit query: offset skips rows", "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "a");
  insert_audit(pg, "2026-01-02T00:00:00Z", "b");
  insert_audit(pg, "2026-01-03T00:00:00Z", "c");

  // ORDER BY timestamp DESC → c, b, a. Offset 1 → b, a.
  auto res = query_select(pg, "", "", "", "", 100, 1);
  REQUIRE(PQntuples(res.get()) == 2);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "b");
  REQUIRE(std::string{PQgetvalue(res.get(), 1, 2)} == "a");
}

TEST_CASE("Audit query: COUNT matches unbounded SELECT size",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  insert_audit(pg, "2026-01-01T00:00:00Z", "user.login", UUID_A);
  insert_audit(pg, "2026-01-02T00:00:00Z", "user.login", UUID_A);
  insert_audit(pg, "2026-01-03T00:00:00Z", "user.logout", UUID_A);

  // Both SELECT (no limit) and COUNT with same filters should agree.
  auto res = query_select(pg, "user.login", "", "", "", 100, 0);
  REQUIRE(PQntuples(res.get()) == 2);
  REQUIRE(query_count(pg, "user.login", "", "", "") == 2);
}

TEST_CASE("Audit query: NULL user_id returned as NULL for system events",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  // Insert a system event (no user_id).
  insert_audit(pg, "2026-01-01T00:00:00Z", "system.boot");

  auto res = query_select(pg, "system.boot", "", "", "", 100, 0);
  REQUIRE(PQntuples(res.get()) == 1);
  // Column 3 is user_id; PQgetisnull returns 1 when NULL.
  REQUIRE(PQgetisnull(res.get(), 0, 3) == 1);
}

// ── Retention tests ─────────────────────────────────────────────────

TEST_CASE("purge_older_than: deletes rows older than retention window",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  // Direct INSERT with SQL-expression timestamp (can't pass NOW()-based
  // expressions as a libpq parameter).
  pg.exec("INSERT INTO plinth.audit_log (timestamp, action, detail, node_id) "
          "VALUES (NOW() - INTERVAL '100 days', 'very.old', '{}'::jsonb, "
          "'test-node')");
  pg.exec("INSERT INTO plinth.audit_log (timestamp, action, detail, node_id) "
          "VALUES (NOW() - INTERVAL '30 days', 'recent', '{}'::jsonb, "
          "'test-node')");

  auto deleted = plinth::audit::purge_older_than(db, 90);
  REQUIRE(deleted == 1);

  auto remaining = pg.exec("SELECT action FROM plinth.audit_log");
  REQUIRE(PQntuples(remaining.get()) >= 1);
  // The recent row must survive; no row with 'very.old' may remain.
  auto old_check =
      pg.exec("SELECT 1 FROM plinth.audit_log WHERE action = 'very.old'");
  REQUIRE(PQntuples(old_check.get()) == 0);
  auto recent_check =
      pg.exec("SELECT 1 FROM plinth.audit_log WHERE action = 'recent'");
  REQUIRE(PQntuples(recent_check.get()) == 1);
}

TEST_CASE("purge_older_than: returns correct rows-deleted count",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  for (int i = 0; i < 3; ++i) {
    pg.exec("INSERT INTO plinth.audit_log (timestamp, action, detail, node_id) "
            "VALUES (NOW() - INTERVAL '200 days', 'old', '{}'::jsonb, "
            "'test-node')");
  }
  for (int i = 0; i < 2; ++i) {
    pg.exec(
        "INSERT INTO plinth.audit_log (timestamp, action, detail, node_id) "
        "VALUES (NOW() - INTERVAL '10 days', 'new', '{}'::jsonb, 'test-node')");
  }

  auto deleted = plinth::audit::purge_older_than(db, 90);
  REQUIRE(deleted == 3);
}

TEST_CASE("purge_older_than: retention_days <= 0 is a no-op",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);
  pg.exec("INSERT INTO plinth.audit_log (timestamp, action, detail, node_id) "
          "VALUES (NOW() - INTERVAL '9999 days', 'ancient', '{}'::jsonb, "
          "'test-node')");

  REQUIRE(plinth::audit::purge_older_than(db, 0) == 0);
  REQUIRE(plinth::audit::purge_older_than(db, -5) == 0);

  // Row should still be there.
  auto res = pg.exec("SELECT 1 FROM plinth.audit_log WHERE action = 'ancient'");
  REQUIRE(PQntuples(res.get()) == 1);
}

TEST_CASE("purge_older_than: empty table returns 0", "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  clear_audit_log(pg);

  REQUIRE(plinth::audit::purge_older_than(db, 90) == 0);
}

// ── Route-registry test ─────────────────────────────────────────────
//
// Verifies that register_audit_routes() binds `kernel.admin` as the
// required rule for GET /api/audit. This catches regressions where
// a future edit might accidentally drop the RBAC binding. The
// RbacFilter's runtime behavior is already covered in
// tests/kernel/rbac/enforcement_test.cpp.

TEST_CASE("Audit endpoint: kernel.admin rule requirement registered",
          "[audit]") {
  plinth::audit::register_audit_routes();
  auto rules = plinth::rbac::get_required_rules(drogon::Get, "/api/audit");
  REQUIRE(rules.has_value());
  REQUIRE(rules->size() == 1);
  REQUIRE(rules->at(0) == "kernel.admin");
}

// ── Bootstrap-audit tests ───────────────────────────────────────────

TEST_CASE("Bootstrap emits rbac.rule_registered for kernel.admin",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db); // calls bootstrap_groups()

  TestPg pg(db);
  auto res = pg.exec("SELECT detail->>'rule' AS rule, "
                     "       detail->>'namespace' AS ns, "
                     "       detail->>'extension_name' AS ext "
                     "FROM plinth.audit_log "
                     "WHERE action = 'rbac.rule_registered' "
                     "  AND detail->>'rule' = 'kernel.admin'");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "kernel.admin");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "kernel");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "kernel");
}

TEST_CASE("Bootstrap rbac.rule_registered is idempotent",
          "[audit][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  // Run bootstrap a second time (simulates kernel restart).
  plinth::groups::bootstrap_groups(db);

  TestPg pg(db);
  auto res = pg.exec("SELECT COUNT(*) FROM plinth.audit_log "
                     "WHERE action = 'rbac.rule_registered' "
                     "  AND detail->>'rule' = 'kernel.admin'");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "1");
}
