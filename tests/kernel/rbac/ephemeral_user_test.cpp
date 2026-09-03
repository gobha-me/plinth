// PG-gated unit tests for the RBAC-test ephemeral-user factory
// (ICD-0.4.7 cases B.04–B.07). Each case provisions a fresh plinth
// schema via bootstrap, drives `create_run_users` /
// `destroy_run_users` / `grant_rule_to_run_group` /
// `cleanup_orphaned_test_users` directly against libpq, and asserts
// table state with targeted SELECTs.

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/rbac/ephemeral_user.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  // single-threaded.
  if (auto* v = std::getenv("PLINTH_PG_HOST")) {
    db.host = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<std::uint16_t>(std::stoi(v));
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
  PQfinish(conn);
  return ok;
}

auto drop_schema(const plinth::Config::Database& db) -> void {
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
    drop_schema(db);
    plinth::db::bootstrap_schema(
        db, std::string{CMAKE_SOURCE_DIR} + "/migrations", true);
    plinth::groups::bootstrap_groups(db);
    conn = PQconnectdb(conninfo_of(db).c_str());
  }
  ~Scratch() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    drop_schema(db);
  }
  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  Scratch(Scratch&&) = delete;
  auto operator=(Scratch&&) -> Scratch& = delete;
};

auto count(PGconn* conn, const char* sql) -> long {
  PgResultPtr res(PQexec(conn, sql), PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return -1;
  }
  return std::stol(PQgetvalue(res.get(), 0, 0));
}

auto seed_rule(PGconn* conn, std::string_view rule, std::string_view namespace_)
    -> void {
  std::string r{rule};
  std::string n{namespace_};
  std::array<const char*, 2> values = {r.c_str(), n.c_str()};
  PgResultPtr res(PQexecParams(conn,
                               "INSERT INTO plinth.rbac_rules "
                               "(rule, namespace, description, extension_name) "
                               "VALUES ($1, $2, 'test rule', 'notes')",
                               2, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
}

} // namespace

TEST_CASE("B.04 create_run_users / destroy_run_users idempotency",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set / PG unreachable");
  }
  Scratch s;

  auto pair = plinth::rbac::create_run_users("abc123", *s.conn);
  REQUIRE(pair.has_value());
  REQUIRE(pair->run_id == "abc123");
  REQUIRE(pair->denied_username == "__test_denied_abc123");
  REQUIRE(pair->allowed_username == "__test_allowed_abc123");
  REQUIRE(pair->allowed_group_name == "__rbac_test_abc123");
  REQUIRE(!pair->denied_user_id.empty());
  REQUIRE(!pair->allowed_user_id.empty());
  REQUIRE(!pair->allowed_group_id.empty());

  // Both users flagged as test users.
  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      2);
  // One synthetic group; both users in `everyone`; one user in the
  // synthetic group.
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE name = '__rbac_test_abc123'") == 1);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_members m "
                        "JOIN plinth.groups g ON g.id = m.group_id "
                        "WHERE g.name = 'everyone'") == 2);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_members m "
                        "JOIN plinth.groups g ON g.id = m.group_id "
                        "WHERE g.name = '__rbac_test_abc123'") == 1);

  auto d1 = plinth::rbac::destroy_run_users("abc123", *s.conn);
  REQUIRE(d1.has_value());
  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      0);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE name = '__rbac_test_abc123'") == 0);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_members m "
                        "JOIN plinth.groups g ON g.id = m.group_id "
                        "WHERE g.name = 'everyone'") == 0);

  // Second destroy is a no-op.
  auto d2 = plinth::rbac::destroy_run_users("abc123", *s.conn);
  REQUIRE(d2.has_value());
}

TEST_CASE("B.05 grant/revoke rule cycle is idempotent",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set / PG unreachable");
  }
  Scratch s;

  seed_rule(s.conn, "notes.edit", "notes");
  auto pair = plinth::rbac::create_run_users("xy", *s.conn);
  REQUIRE(pair.has_value());

  auto g1 = plinth::rbac::grant_rule_to_run_group("xy", "notes.edit", *s.conn);
  REQUIRE(g1.has_value());
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_rules gr "
                        "JOIN plinth.groups g ON g.id = gr.group_id "
                        "WHERE g.name = '__rbac_test_xy'") == 1);

  // Re-grant is idempotent (ON CONFLICT DO NOTHING).
  auto g2 = plinth::rbac::grant_rule_to_run_group("xy", "notes.edit", *s.conn);
  REQUIRE(g2.has_value());
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_rules gr "
                        "JOIN plinth.groups g ON g.id = gr.group_id "
                        "WHERE g.name = '__rbac_test_xy'") == 1);

  auto r1 =
      plinth::rbac::revoke_rule_from_run_group("xy", "notes.edit", *s.conn);
  REQUIRE(r1.has_value());
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_rules gr "
                        "JOIN plinth.groups g ON g.id = gr.group_id "
                        "WHERE g.name = '__rbac_test_xy'") == 0);

  // Re-revoke is idempotent.
  auto r2 =
      plinth::rbac::revoke_rule_from_run_group("xy", "notes.edit", *s.conn);
  REQUIRE(r2.has_value());

  // Re-grant after revoke works (tests the rule-level isolation cycle).
  auto g3 = plinth::rbac::grant_rule_to_run_group("xy", "notes.edit", *s.conn);
  REQUIRE(g3.has_value());
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.group_rules gr "
                        "JOIN plinth.groups g ON g.id = gr.group_id "
                        "WHERE g.name = '__rbac_test_xy'") == 1);
}

TEST_CASE("B.06 cleanup_orphaned_test_users sweeps users past cutoff",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set / PG unreachable");
  }
  Scratch s;

  REQUIRE(plinth::rbac::create_run_users("r1", *s.conn).has_value());
  REQUIRE(plinth::rbac::create_run_users("r2", *s.conn).has_value());
  REQUIRE(plinth::rbac::create_run_users("r3", *s.conn).has_value());

  // Back-date all test users + groups so they count as orphaned.
  PgResultPtr res(
      PQexec(s.conn,
             "UPDATE plinth.users SET created_at = NOW() - interval '2 hour' "
             "WHERE is_test_user = true; "
             "UPDATE plinth.groups SET created_at = NOW() - interval '2 hour' "
             "WHERE starts_with(name, '__rbac_test_')"),
      PQclear);
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);

  auto out = plinth::rbac::cleanup_orphaned_test_users(
      std::chrono::system_clock::now(), *s.conn);
  REQUIRE(out.has_value());
  REQUIRE(*out == 3);

  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      0);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE starts_with(name, '__rbac_test_')") == 0);
}

TEST_CASE("B.07 cleanup_orphaned_test_users skips fresh users",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set / PG unreachable");
  }
  Scratch s;

  REQUIRE(plinth::rbac::create_run_users("fresh1", *s.conn).has_value());
  REQUIRE(plinth::rbac::create_run_users("fresh2", *s.conn).has_value());

  // Cutoff one hour in the past — all fresh users are NEWER, should be kept.
  auto out = plinth::rbac::cleanup_orphaned_test_users(
      std::chrono::system_clock::now() - std::chrono::hours{1}, *s.conn);
  REQUIRE(out.has_value());
  REQUIRE(*out == 0);

  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      4);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE starts_with(name, '__rbac_test_')") == 2);
}
