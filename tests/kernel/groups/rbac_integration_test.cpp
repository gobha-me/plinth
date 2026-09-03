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
// These tests exercise the RBAC rule storage and grant/revoke SQL
// logic directly via libpq.

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

auto insert_group(TestPg& pg, const std::string& name) -> std::string {
  auto res = pg.exec_params(
      "INSERT INTO plinth.groups (name) VALUES ($1) RETURNING id", {name});
  return PQgetvalue(res.get(), 0, 0);
}

auto insert_rule(TestPg& pg, const std::string& rule, const std::string& ns,
                 const std::string& description) -> std::string {
  auto res = pg.exec_params("INSERT INTO plinth.rbac_rules (rule, namespace, "
                            "description, extension_name) "
                            "VALUES ($1, $2, $3, $2) RETURNING id",
                            {rule, ns, description});
  return PQgetvalue(res.get(), 0, 0);
}

auto get_group_id(TestPg& pg, const std::string& name) -> std::string {
  auto res =
      pg.exec_params("SELECT id FROM plinth.groups WHERE name = $1", {name});
  return PQgetvalue(res.get(), 0, 0);
}

auto get_rule_id(TestPg& pg, const std::string& rule) -> std::string {
  auto res = pg.exec_params("SELECT id FROM plinth.rbac_rules WHERE rule = $1",
                            {rule});
  return PQgetvalue(res.get(), 0, 0);
}

} // namespace

// ── Bootstrap: built-in groups and rules ─────────────────────

TEST_CASE("Bootstrap creates admin group with built_in=true",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto res = pg.exec(
      "SELECT built_in, description FROM plinth.groups WHERE name = 'admin'");
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "t");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "Administrators");
}

TEST_CASE("Bootstrap creates everyone group with built_in=true",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto res = pg.exec("SELECT built_in, description FROM plinth.groups WHERE "
                     "name = 'everyone'");
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "t");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} ==
          "All authenticated users");
}

TEST_CASE("Bootstrap registers kernel.admin rule", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto res = pg.exec("SELECT namespace, description, extension_name "
                     "FROM plinth.rbac_rules WHERE rule = 'kernel.admin'");
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "kernel");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} ==
          "Full administrative access");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "kernel");
}

TEST_CASE("Bootstrap grants kernel.admin to admin group",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto res = pg.exec("SELECT 1 FROM plinth.group_rules gr "
                     "JOIN plinth.groups g ON g.id = gr.group_id "
                     "JOIN plinth.rbac_rules r ON r.id = gr.rule_id "
                     "WHERE g.name = 'admin' AND r.rule = 'kernel.admin'");
  REQUIRE(PQntuples(res.get()) == 1);
}

TEST_CASE("Bootstrap is idempotent", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  // Run bootstrap again
  plinth::groups::bootstrap_groups(db);

  TestPg pg(db);

  // Still exactly 2 built-in groups
  auto grp =
      pg.exec("SELECT COUNT(*) FROM plinth.groups WHERE built_in = true");
  REQUIRE(std::stoi(PQgetvalue(grp.get(), 0, 0)) == 2);

  // Still exactly 1 kernel.admin rule
  auto rule = pg.exec(
      "SELECT COUNT(*) FROM plinth.rbac_rules WHERE rule = 'kernel.admin'");
  REQUIRE(std::stoi(PQgetvalue(rule.get(), 0, 0)) == 1);

  // Still exactly 1 admin→kernel.admin grant
  auto grant = pg.exec("SELECT COUNT(*) FROM plinth.group_rules gr "
                       "JOIN plinth.groups g ON g.id = gr.group_id "
                       "JOIN plinth.rbac_rules r ON r.id = gr.rule_id "
                       "WHERE g.name = 'admin' AND r.rule = 'kernel.admin'");
  REQUIRE(std::stoi(PQgetvalue(grant.get(), 0, 0)) == 1);
}

// ── Rule registration ────────────────────────────────────────

TEST_CASE("Register rule inserts into rbac_rules", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto rule_id = insert_rule(pg, "terminal.shell.execute", "terminal",
                             "Execute shell commands");
  REQUIRE_FALSE(rule_id.empty());

  auto res =
      pg.exec_params("SELECT rule, namespace, description, extension_name "
                     "FROM plinth.rbac_rules WHERE id = $1::uuid",
                     {rule_id});
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "terminal.shell.execute");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "terminal");
}

TEST_CASE("Register duplicate rule fails", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  // Use a rule NOT bootstrap-seeded; `packages.install` is seeded
  // by bootstrap_groups since 0.4.4 Slice A, so it would collide on
  // the initial insert here, not on the duplicate attempt.
  insert_rule(pg, "feature.custom", "feature", "Custom feature");

  auto res = pg.exec_params("INSERT INTO plinth.rbac_rules (rule, namespace, "
                            "description, extension_name) "
                            "VALUES ($1, $2, $3, $2)",
                            {"feature.custom", "feature", "Duplicate"});
  REQUIRE(PQresultStatus(res.get()) != PGRES_COMMAND_OK);
}

TEST_CASE("Rule orphaned_at defaults to NULL", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto rule_id = insert_rule(pg, "system.backup.run", "system", "Run backups");

  auto res = pg.exec_params(
      "SELECT orphaned_at FROM plinth.rbac_rules WHERE id = $1::uuid",
      {rule_id});
  REQUIRE(PQgetisnull(res.get(), 0, 0) == 1);
}

// ── Rule grant/revoke ────────────────────────────────────────

TEST_CASE("Grant rule to group creates group_rules row",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "operators");
  auto rule_id =
      insert_rule(pg, "system.metrics.view", "system", "View metrics");

  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, rule_id});

  auto res = pg.exec_params("SELECT 1 FROM plinth.group_rules "
                            "WHERE group_id = $1::uuid AND rule_id = $2::uuid",
                            {group_id, rule_id});
  REQUIRE(PQntuples(res.get()) == 1);
}

TEST_CASE("Grant same rule twice fails with unique violation",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "testers");
  auto rule_id = insert_rule(pg, "tests.run", "tests", "Run tests");

  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, rule_id});

  auto res =
      pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                     "VALUES ($1::uuid, $2::uuid)",
                     {group_id, rule_id});
  REQUIRE(PQresultStatus(res.get()) != PGRES_COMMAND_OK);
}

TEST_CASE("Revoke rule removes group_rules row", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "devs");
  auto rule_id = insert_rule(pg, "code.deploy", "code", "Deploy code");

  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, rule_id});

  pg.exec_params("DELETE FROM plinth.group_rules "
                 "WHERE group_id = $1::uuid AND rule_id = $2::uuid",
                 {group_id, rule_id});

  auto res = pg.exec_params("SELECT 1 FROM plinth.group_rules "
                            "WHERE group_id = $1::uuid AND rule_id = $2::uuid",
                            {group_id, rule_id});
  REQUIRE(PQntuples(res.get()) == 0);
}

TEST_CASE("Revoke non-granted rule returns zero affected rows",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "empty-group");
  auto rule_id = insert_rule(pg, "nothing.here", "nothing", "No-op");

  auto res = pg.exec_params("DELETE FROM plinth.group_rules "
                            "WHERE group_id = $1::uuid AND rule_id = $2::uuid",
                            {group_id, rule_id});
  REQUIRE(std::string{PQcmdTuples(res.get())} == "0");
}

// ── Permission union (effective rules for a user) ────────────

TEST_CASE("User in multiple groups gets union of all rules",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "multi-role", "password-12345");

  auto group_a = insert_group(pg, "group-a");
  auto group_b = insert_group(pg, "group-b");

  auto rule1 = insert_rule(pg, "feature.read", "feature", "Read access");
  auto rule2 = insert_rule(pg, "feature.write", "feature", "Write access");
  auto rule3 = insert_rule(pg, "feature.delete", "feature", "Delete access");

  // group-a gets rule1 + rule2
  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) VALUES "
                 "($1::uuid, $2::uuid)",
                 {group_a, rule1});
  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) VALUES "
                 "($1::uuid, $2::uuid)",
                 {group_a, rule2});

  // group-b gets rule2 + rule3
  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) VALUES "
                 "($1::uuid, $2::uuid)",
                 {group_b, rule2});
  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) VALUES "
                 "($1::uuid, $2::uuid)",
                 {group_b, rule3});

  // User joins both groups
  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) VALUES "
                 "($1::uuid, $2::uuid)",
                 {group_a, user_id});
  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) VALUES "
                 "($1::uuid, $2::uuid)",
                 {group_b, user_id});

  // Query: effective rules = DISTINCT union of all rules from all groups
  auto res = pg.exec_params(
      "SELECT DISTINCT r.rule "
      "FROM plinth.rbac_rules r "
      "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
      "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
      "WHERE gm.user_id = $1::uuid "
      "ORDER BY r.rule",
      {user_id});

  REQUIRE(PQntuples(res.get()) == 3);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "feature.delete");
  REQUIRE(std::string{PQgetvalue(res.get(), 1, 0)} == "feature.read");
  REQUIRE(std::string{PQgetvalue(res.get(), 2, 0)} == "feature.write");
}

TEST_CASE("User with kernel.admin via admin group", "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "admin-user", "password-12345");
  auto admin_group_id = get_group_id(pg, "admin");

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {admin_group_id, user_id});

  // User's effective rules should include kernel.admin
  auto res = pg.exec_params(
      "SELECT r.rule "
      "FROM plinth.rbac_rules r "
      "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
      "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
      "WHERE gm.user_id = $1::uuid AND r.rule = 'kernel.admin'",
      {user_id});
  REQUIRE(PQntuples(res.get()) == 1);
}

// ── Orphaned rules ───────────────────────────────────────────

TEST_CASE("Orphaned rule can be flagged and still queried",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto rule_id =
      insert_rule(pg, "old.extension.action", "old", "Legacy action");

  // Mark as orphaned (extension disabled)
  pg.exec_params(
      "UPDATE plinth.rbac_rules SET orphaned_at = NOW() WHERE id = $1::uuid",
      {rule_id});

  auto res = pg.exec_params(
      "SELECT orphaned_at FROM plinth.rbac_rules WHERE id = $1::uuid",
      {rule_id});
  REQUIRE(PQgetisnull(res.get(), 0, 0) == 0); // NOT NULL

  // Rule still exists (visible for admin)
  auto exists = pg.exec_params(
      "SELECT rule FROM plinth.rbac_rules WHERE id = $1::uuid", {rule_id});
  REQUIRE(PQntuples(exists.get()) == 1);
}

// ── Delete group cascades rule grants ────────────────────────

TEST_CASE("Deleting group's rule grants does not delete the rule itself",
          "[rbac][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto group_id = insert_group(pg, "temp-group");
  auto rule_id = insert_rule(pg, "temp.action", "temp", "Temporary action");

  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, rule_id});

  // Delete the grant (simulating group deletion cascade)
  pg.exec_params("DELETE FROM plinth.group_rules WHERE group_id = $1::uuid",
                 {group_id});
  pg.exec_params("DELETE FROM plinth.groups WHERE id = $1::uuid", {group_id});

  // Rule still exists in the registry
  auto res = pg.exec_params(
      "SELECT 1 FROM plinth.rbac_rules WHERE id = $1::uuid", {rule_id});
  REQUIRE(PQntuples(res.get()) == 1);
}
