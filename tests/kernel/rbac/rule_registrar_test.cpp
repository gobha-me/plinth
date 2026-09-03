// PG-gated unit tests for the 0.4.5 rbac::rule_registrar helpers
// (mark/clear orphaned + delete). Integration-style — connect to the
// configured Postgres, drop+recreate the plinth schema, exercise the
// helpers directly. Mirrors tests/kernel/packages/packages_schema_test.cpp
// for env var handling and PG bringup.

#include "kernel/rbac/rule_registrar.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"

namespace {

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  // single-threaded.
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

auto conninfo_of(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password + " connect_timeout=3";
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  PGconn* conn = PQconnectdb(conninfo_of(pg_config()).c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  // rename-to-uppercase less readable here.
  PQfinish(conn);
  return ok;
}

auto drop_plinth_schema(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) == CONNECTION_OK) {
    PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  }
  PQfinish(conn);
}

struct Scratch {
  plinth::Config::Database db;
  PGconn* conn = nullptr;

  Scratch() : db(pg_config()) {
    drop_plinth_schema(db);
    plinth::db::bootstrap_schema(
        db, std::string{CMAKE_SOURCE_DIR} + "/migrations", true);
    conn = PQconnectdb(conninfo_of(db).c_str());
  }

  ~Scratch() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    drop_plinth_schema(db);
  }

  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  Scratch(Scratch&&) = delete;
  auto operator=(Scratch&&) -> Scratch& = delete;

  [[nodiscard]] auto exec_ok(const std::string& sql) const -> bool {
    PGresult* res = PQexec(conn, sql.c_str());
    auto status = PQresultStatus(res);
    PQclear(res);
    return status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
  }

  [[nodiscard]] auto count(const std::string& sql) const -> std::string {
    std::unique_ptr<PGresult, decltype(&PQclear)> res(PQexec(conn, sql.c_str()),
                                                      PQclear);
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
        PQntuples(res.get()) == 0) {
      return {};
    }
    return {PQgetvalue(res.get(), 0, 0)};
  }
};

auto seed_rules(const Scratch& s, std::string_view ext, int n) -> void {
  for (int i = 1; i <= n; ++i) {
    auto rule_name = std::string{ext} + ".rule" + std::to_string(i);
    REQUIRE(s.exec_ok("INSERT INTO plinth.rbac_rules (rule, namespace, "
                      "description, extension_name) "
                      "VALUES ('" +
                      rule_name + "', '" + std::string{ext} + "', 'desc', '" +
                      std::string{ext} + "')"));
  }
}

} // namespace

TEST_CASE("mark_extension_rules_orphaned sets orphaned_at on matching rows",
          "[rbac_rule_registrar][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  seed_rules(s, "notes", 3);
  seed_rules(s, "calendar", 2); // unrelated — must not be touched

  auto marked = plinth::rbac::mark_extension_rules_orphaned("notes", *s.conn);
  REQUIRE(marked.has_value());
  REQUIRE(*marked == 3);

  REQUIRE(s.count("SELECT COUNT(*) FROM plinth.rbac_rules "
                  "WHERE extension_name='notes' AND orphaned_at IS NOT NULL") ==
          "3");
  REQUIRE(s.count("SELECT COUNT(*) FROM plinth.rbac_rules "
                  "WHERE extension_name='calendar' AND orphaned_at IS NULL") ==
          "2");

  // Idempotent: re-marking already-orphaned rules is a no-op.
  auto remarked = plinth::rbac::mark_extension_rules_orphaned("notes", *s.conn);
  REQUIRE(remarked.has_value());
  REQUIRE(*remarked == 0);
}

TEST_CASE("clear_extension_rules_orphaned clears every row for the extension",
          "[rbac_rule_registrar][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  seed_rules(s, "notes", 2);
  auto marked = plinth::rbac::mark_extension_rules_orphaned("notes", *s.conn);
  REQUIRE(marked.has_value());

  auto cleared = plinth::rbac::clear_extension_rules_orphaned("notes", *s.conn);
  REQUIRE(cleared.has_value());
  REQUIRE(*cleared == 2);
  REQUIRE(s.count("SELECT COUNT(*) FROM plinth.rbac_rules "
                  "WHERE extension_name='notes' AND orphaned_at IS NULL") ==
          "2");

  // Idempotent: re-clearing already-null rows touches them again
  // (no WHERE orphaned_at IS NOT NULL predicate) but the row count
  // is still the affected set.
  auto recleared =
      plinth::rbac::clear_extension_rules_orphaned("notes", *s.conn);
  REQUIRE(recleared.has_value());
  REQUIRE(*recleared == 2);
}

TEST_CASE("delete_extension_rules removes every row for the extension",
          "[rbac_rule_registrar][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  seed_rules(s, "notes", 3);
  seed_rules(s, "calendar", 2);

  auto deleted = plinth::rbac::delete_extension_rules("notes", *s.conn);
  REQUIRE(deleted.has_value());
  REQUIRE(*deleted == 3);

  REQUIRE(s.count("SELECT COUNT(*) FROM plinth.rbac_rules "
                  "WHERE extension_name='notes'") == "0");
  REQUIRE(s.count("SELECT COUNT(*) FROM plinth.rbac_rules "
                  "WHERE extension_name='calendar'") == "2");
}

TEST_CASE(
    "delete_extension_rules fails when group_rules still reference the rows",
    "[rbac_rule_registrar][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  seed_rules(s, "notes", 1);
  // Create a group + grant the rule to surface the FK constraint.
  REQUIRE(
      s.exec_ok("INSERT INTO plinth.groups (id, name) "
                "VALUES ('77777777-7777-7777-7777-777777777777', 'testers')"));
  REQUIRE(s.exec_ok("INSERT INTO plinth.group_rules (group_id, rule_id) "
                    "SELECT '77777777-7777-7777-7777-777777777777', id "
                    "FROM plinth.rbac_rules WHERE rule = 'notes.rule1'"));

  // Raw delete fails (FK constraint); callers must strip group_rules first.
  auto result = plinth::rbac::delete_extension_rules("notes", *s.conn);
  REQUIRE_FALSE(result.has_value());
}
