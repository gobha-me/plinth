#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"

// ── Integration test helpers ─────────────────────────────────
// These tests exercise the groups and membership SQL logic directly
// via libpq, verifying the data layer that the HTTP handlers depend on.

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
  plinth::groups::bootstrap_groups(db);
}

auto insert_user(TestPg& pg, const std::string& username,
                 const std::string& password) -> std::string {
  auto hash = plinth::auth::hash_password(password);
  auto res =
      pg.exec_params("INSERT INTO plinth.users (username, password_hash) "
                     "VALUES ($1, $2) RETURNING id",
                     {username, hash});
  return PQgetvalue(res.get(), 0, 0);
}

auto insert_group(TestPg& pg, const std::string& name,
                  const std::string& description = "") -> std::string {
  auto res = pg.exec_params("INSERT INTO plinth.groups (name, description) "
                            "VALUES ($1, NULLIF($2, '')) RETURNING id",
                            {name, description});
  return PQgetvalue(res.get(), 0, 0);
}

auto row_count(TestPg& pg, const std::string& table) -> int {
  auto res = pg.exec("SELECT COUNT(*) FROM plinth." + table);
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

} // namespace

// ── Group CRUD ───────────────────────────────────────────────

TEST_CASE("Create group inserts row in database", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "developers", "Dev team");
  REQUIRE_FALSE(group_id.empty());

  auto res = pg.exec_params("SELECT name, description, built_in FROM "
                            "plinth.groups WHERE id = $1::uuid",
                            {group_id});
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "developers");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "Dev team");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "f");
}

TEST_CASE("Create group with duplicate name fails", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  insert_group(pg, "testers");

  auto res = pg.exec_params("INSERT INTO plinth.groups (name) VALUES ($1)",
                            {"testers"});
  REQUIRE(PQresultStatus(res.get()) != PGRES_COMMAND_OK);
}

TEST_CASE("List groups returns all groups including built-in",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  insert_group(pg, "custom-group");

  // Should have admin + everyone (built-in) + custom-group
  auto res = pg.exec("SELECT COUNT(*) FROM plinth.groups");
  REQUIRE(std::stoi(PQgetvalue(res.get(), 0, 0)) == 3);
}

TEST_CASE("Get group by ID returns correct group", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "ops", "Operations team");

  auto res = pg.exec_params(
      "SELECT name, description FROM plinth.groups WHERE id = $1::uuid",
      {group_id});
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "ops");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "Operations team");
}

TEST_CASE("Get nonexistent group returns empty result",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto res = pg.exec_params("SELECT id FROM plinth.groups WHERE id = $1::uuid",
                            {"00000000-0000-0000-0000-000000000000"});
  REQUIRE(PQntuples(res.get()) == 0);
}

TEST_CASE("Update group changes name and description",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "old-name", "old description");

  pg.exec_params("UPDATE plinth.groups SET name = $1, description = $2 WHERE "
                 "id = $3::uuid",
                 {"new-name", "new description", group_id});

  auto res = pg.exec_params(
      "SELECT name, description FROM plinth.groups WHERE id = $1::uuid",
      {group_id});
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "new-name");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "new description");
}

TEST_CASE("Built-in groups have built_in=true", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto res = pg.exec("SELECT name, built_in FROM plinth.groups "
                     "WHERE name IN ('admin', 'everyone') ORDER BY name");
  REQUIRE(PQntuples(res.get()) == 2);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "admin");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "t");
  REQUIRE(std::string{PQgetvalue(res.get(), 1, 0)} == "everyone");
  REQUIRE(std::string{PQgetvalue(res.get(), 1, 1)} == "t");
}

TEST_CASE("Delete group removes row", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "temp-group");
  auto before = row_count(pg, "groups");

  pg.exec_params("DELETE FROM plinth.groups WHERE id = $1::uuid", {group_id});
  REQUIRE(row_count(pg, "groups") == before - 1);
}

TEST_CASE("Delete group removes associated memberships",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "doomed-group");
  auto user_id = insert_user(pg, "member", "password-12345");

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, user_id});
  REQUIRE(row_count(pg, "group_members") >= 1);

  // Delete child rows first (no CASCADE), then parent
  pg.exec_params("DELETE FROM plinth.group_members WHERE group_id = $1::uuid",
                 {group_id});
  pg.exec_params("DELETE FROM plinth.groups WHERE id = $1::uuid", {group_id});

  auto res = pg.exec_params(
      "SELECT 1 FROM plinth.group_members WHERE group_id = $1::uuid",
      {group_id});
  REQUIRE(PQntuples(res.get()) == 0);
}

// ── Membership ───────────────────────────────────────────────

TEST_CASE("Add member creates group_members row", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "team-a");
  auto user_id = insert_user(pg, "alice", "password-12345");

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, user_id});

  auto res = pg.exec_params("SELECT 1 FROM plinth.group_members "
                            "WHERE group_id = $1::uuid AND user_id = $2::uuid",
                            {group_id, user_id});
  REQUIRE(PQntuples(res.get()) == 1);
}

TEST_CASE("Add duplicate member fails with unique violation",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "team-b");
  auto user_id = insert_user(pg, "bob", "password-12345");

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, user_id});

  // Second insert should fail
  auto res =
      pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                     "VALUES ($1::uuid, $2::uuid)",
                     {group_id, user_id});
  REQUIRE(PQresultStatus(res.get()) != PGRES_COMMAND_OK);
}

TEST_CASE("Remove member deletes row", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "team-c");
  auto user_id = insert_user(pg, "charlie", "password-12345");

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, user_id});

  pg.exec_params("DELETE FROM plinth.group_members "
                 "WHERE group_id = $1::uuid AND user_id = $2::uuid",
                 {group_id, user_id});

  auto res = pg.exec_params("SELECT 1 FROM plinth.group_members "
                            "WHERE group_id = $1::uuid AND user_id = $2::uuid",
                            {group_id, user_id});
  REQUIRE(PQntuples(res.get()) == 0);
}

TEST_CASE("Remove non-member returns zero affected rows",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "team-d");
  auto user_id = insert_user(pg, "dave", "password-12345");

  auto res = pg.exec_params("DELETE FROM plinth.group_members "
                            "WHERE group_id = $1::uuid AND user_id = $2::uuid",
                            {group_id, user_id});
  // CmdTuples is "0" for zero affected rows
  REQUIRE(std::string{PQcmdTuples(res.get())} == "0");
}

TEST_CASE("User can belong to multiple groups", "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group1 = insert_group(pg, "team-x");
  auto group2 = insert_group(pg, "team-y");
  auto user_id = insert_user(pg, "eve", "password-12345");

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group1, user_id});
  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group2, user_id});

  auto res = pg.exec_params(
      "SELECT COUNT(*) FROM plinth.group_members WHERE user_id = $1::uuid",
      {user_id});
  REQUIRE(std::stoi(PQgetvalue(res.get(), 0, 0)) == 2);
}

// ── Admin check pattern ──────────────────────────────────────

TEST_CASE("Admin group membership check returns true for admin member",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "admin-user", "password-12345");

  // Get admin group ID
  auto grp = pg.exec("SELECT id FROM plinth.groups WHERE name = 'admin'");
  auto admin_group_id = std::string{PQgetvalue(grp.get(), 0, 0)};

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {admin_group_id, user_id});

  auto res = pg.exec_params("SELECT 1 FROM plinth.group_members gm "
                            "JOIN plinth.groups g ON g.id = gm.group_id "
                            "WHERE gm.user_id = $1::uuid AND g.name = 'admin'",
                            {user_id});
  REQUIRE(PQntuples(res.get()) == 1);
}

TEST_CASE("Admin group membership check returns false for non-admin",
          "[groups][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "regular-user", "password-12345");

  auto res = pg.exec_params("SELECT 1 FROM plinth.group_members gm "
                            "JOIN plinth.groups g ON g.id = gm.group_id "
                            "WHERE gm.user_id = $1::uuid AND g.name = 'admin'",
                            {user_id});
  REQUIRE(PQntuples(res.get()) == 0);
}
