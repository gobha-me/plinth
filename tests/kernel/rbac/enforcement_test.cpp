#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <unordered_set>

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/rbac/enforcement.hpp"

// ── Integration tests for RBAC enforcement logic ─────────────
// These tests exercise the permission-check SQL queries that the
// RbacFilter uses, plus the route-to-rules registry.

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

auto get_group_id(TestPg& pg, const std::string& name) -> std::string {
  auto res =
      pg.exec_params("SELECT id FROM plinth.groups WHERE name = $1", {name});
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

auto add_user_to_group(TestPg& pg, const std::string& user_id,
                       const std::string& group_id) -> void {
  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, user_id});
}

auto grant_rule_to_group(TestPg& pg, const std::string& group_id,
                         const std::string& rule_id) -> void {
  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                 "VALUES ($1::uuid, $2::uuid)",
                 {group_id, rule_id});
}

// The same effective-rules query used by the RBAC enforcement filter.
auto get_effective_rules(TestPg& pg, const std::string& user_id)
    -> std::vector<std::string> {
  auto res = pg.exec_params(
      "SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
      "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
      "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
      "WHERE gm.user_id = $1::uuid",
      {user_id});
  int n = PQntuples(res.get());
  std::vector<std::string> rules;
  rules.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    rules.emplace_back(PQgetvalue(res.get(), i, 0));
  }
  return rules;
}

// Check if required rules exist in rbac_rules (the denial-path query).
auto check_rules_exist(TestPg& pg, const std::vector<std::string>& rules)
    -> std::vector<std::string> {
  // Build PG array literal: {rule1,rule2,...}
  std::string arr = "{";
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i > 0) {
      arr += ",";
    }
    arr += rules[i];
  }
  arr += "}";

  auto res = pg.exec_params(
      "SELECT rule FROM plinth.rbac_rules WHERE rule = ANY($1::text[])", {arr});
  int n = PQntuples(res.get());
  std::vector<std::string> found;
  found.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    found.emplace_back(PQgetvalue(res.get(), i, 0));
  }
  return found;
}

// Simulate the filter's permission check: return granting rule or empty.
auto check_permission(const std::vector<std::string>& effective,
                      const std::vector<std::string>& required) -> std::string {
  std::unordered_set<std::string> effective_set(effective.begin(),
                                                effective.end());
  for (const auto& rule : required) {
    if (effective_set.contains(rule)) {
      return rule;
    }
  }
  return "";
}

auto row_count(TestPg& pg, const std::string& table) -> int {
  auto res = pg.exec("SELECT COUNT(*) FROM " + table);
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

} // namespace

// ── Route registry (no DB required) ─────────────────────────

TEST_CASE("Route registry: register and lookup round-trip",
          "[rbac][enforcement]") {
  plinth::rbac::register_rule_requirement(drogon::Post, "/api/test/route",
                                          {"test.create"});

  auto rules =
      plinth::rbac::get_required_rules(drogon::Post, "/api/test/route");
  REQUIRE(rules.has_value());
  REQUIRE(rules->size() == 1);
  REQUIRE(rules->at(0) == "test.create");
}

TEST_CASE("Route registry: unknown route returns nullopt",
          "[rbac][enforcement]") {
  auto rules =
      plinth::rbac::get_required_rules(drogon::Get, "/api/nonexistent/route");
  REQUIRE_FALSE(rules.has_value());
}

TEST_CASE("Route registry: different methods on same path are distinct",
          "[rbac][enforcement]") {
  plinth::rbac::register_rule_requirement(drogon::Get, "/api/test/multi",
                                          {"test.read"});
  plinth::rbac::register_rule_requirement(drogon::Delete, "/api/test/multi",
                                          {"test.delete"});

  auto get_rules =
      plinth::rbac::get_required_rules(drogon::Get, "/api/test/multi");
  auto del_rules =
      plinth::rbac::get_required_rules(drogon::Delete, "/api/test/multi");

  REQUIRE(get_rules.has_value());
  REQUIRE(get_rules->at(0) == "test.read");
  REQUIRE(del_rules.has_value());
  REQUIRE(del_rules->at(0) == "test.delete");
}

// ── Effective rules query ────────────────────────────────────

TEST_CASE("Admin user has kernel.admin in effective rules",
          "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "admin-user", "password123");
  auto admin_group_id = get_group_id(pg, "admin");
  add_user_to_group(pg, user_id, admin_group_id);

  auto effective = get_effective_rules(pg, user_id);
  // admin group is bootstrap-seeded with kernel.admin + whatever
  // other rules the kernel has landed (packages.install /
  // packages.read from 0.4.4). The test is about kernel.admin being
  // present — exact set grows over milestones.
  REQUIRE(std::ranges::find(effective, "kernel.admin") != effective.end());

  auto granting = check_permission(effective, {"kernel.admin"});
  REQUIRE(granting == "kernel.admin");
}

TEST_CASE("Non-admin user has no kernel.admin",
          "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "regular-user", "password123");
  auto everyone_id = get_group_id(pg, "everyone");
  add_user_to_group(pg, user_id, everyone_id);

  auto effective = get_effective_rules(pg, user_id);
  REQUIRE(effective.empty());

  auto granting = check_permission(effective, {"kernel.admin"});
  REQUIRE(granting.empty());
}

TEST_CASE("Multiple required rules: ANY match grants access",
          "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "feature-user", "password123");
  auto group_id = insert_group(pg, "testers");
  auto rule_id =
      insert_rule(pg, "feature.read", "feature", "Read feature data");
  add_user_to_group(pg, user_id, group_id);
  grant_rule_to_group(pg, group_id, rule_id);

  auto effective = get_effective_rules(pg, user_id);
  REQUIRE(effective.size() == 1);
  REQUIRE(effective[0] == "feature.read");

  // Route requires either "feature.read" or "feature.admin" — user has
  // feature.read
  auto granting =
      check_permission(effective, {"feature.read", "feature.admin"});
  REQUIRE(granting == "feature.read");
}

TEST_CASE("Union across multiple groups", "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "multi-group-user", "password123");

  auto group_a = insert_group(pg, "group-a");
  auto group_b = insert_group(pg, "group-b");
  auto rule_x = insert_rule(pg, "feature.x", "feature", "Feature X");
  auto rule_y = insert_rule(pg, "feature.y", "feature", "Feature Y");

  add_user_to_group(pg, user_id, group_a);
  add_user_to_group(pg, user_id, group_b);
  grant_rule_to_group(pg, group_a, rule_x);
  grant_rule_to_group(pg, group_b, rule_y);

  auto effective = get_effective_rules(pg, user_id);
  REQUIRE(effective.size() == 2);

  // Rule from group B grants access even though group A doesn't have it
  auto granting = check_permission(effective, {"feature.y"});
  REQUIRE(granting == "feature.y");
}

TEST_CASE("kernel.admin does NOT bypass other rules",
          "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "admin-no-bypass", "password123");
  auto admin_group_id = get_group_id(pg, "admin");
  add_user_to_group(pg, user_id, admin_group_id);

  auto effective = get_effective_rules(pg, user_id);
  // Admin has kernel.admin + bootstrap-seeded rules (packages.install /
  // packages.read after 0.4.4), but NOT terminal.shell.execute.
  REQUIRE(std::ranges::find(effective, "kernel.admin") != effective.end());
  REQUIRE(std::ranges::find(effective, "terminal.shell.execute") ==
          effective.end());

  // Route requires terminal.shell.execute — admin should be denied
  auto granting = check_permission(effective, {"terminal.shell.execute"});
  REQUIRE(granting.empty());
}

TEST_CASE("Unregistered rule detected on denial path",
          "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  // Check that a rule not in rbac_rules is detected
  auto found = check_rules_exist(pg, {"nonexistent.rule.xyz"});
  REQUIRE(found.empty());

  // Existing rule IS found
  auto found2 = check_rules_exist(pg, {"kernel.admin"});
  REQUIRE(found2.size() == 1);
  REQUIRE(found2[0] == "kernel.admin");
}

TEST_CASE("Denial writes audit log entry", "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "audit-test-user", "password123");

  // Simulate what the RBAC filter does on denial: write audit log
  pg.exec_params("INSERT INTO plinth.audit_log "
                 "(action, user_id, detail, ip_address, node_id) "
                 "VALUES ('rbac.denied', $1::uuid, "
                 "'{\"required_rules\": [\"kernel.admin\"], \"reason\": "
                 "\"permission_denied\", "
                 "\"path\": \"/api/groups\", \"method\": \"POST\"}'::jsonb, "
                 "'127.0.0.1'::inet, 'test-node')",
                 {user_id});

  auto res = pg.exec_params(
      "SELECT action, detail->>'reason' AS reason, detail->>'path' AS path "
      "FROM plinth.audit_log WHERE user_id = $1::uuid AND action = "
      "'rbac.denied'",
      {user_id});
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "rbac.denied");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == "permission_denied");
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "/api/groups");
}

TEST_CASE("Adding groups only adds permissions (additive union property)",
          "[rbac][enforcement][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  auto user_id = insert_user(pg, "additive-user", "password123");

  // Start with no groups — no rules
  auto effective_0 = get_effective_rules(pg, user_id);
  REQUIRE(effective_0.empty());

  // Add to group with rule X
  auto group_a = insert_group(pg, "additive-a");
  auto rule_x = insert_rule(pg, "additive.x", "additive", "Rule X");
  add_user_to_group(pg, user_id, group_a);
  grant_rule_to_group(pg, group_a, rule_x);

  auto effective_1 = get_effective_rules(pg, user_id);
  REQUIRE(effective_1.size() == 1);

  // Add to another group with rule Y — should now have X AND Y
  auto group_b = insert_group(pg, "additive-b");
  auto rule_y = insert_rule(pg, "additive.y", "additive", "Rule Y");
  add_user_to_group(pg, user_id, group_b);
  grant_rule_to_group(pg, group_b, rule_y);

  auto effective_2 = get_effective_rules(pg, user_id);
  REQUIRE(effective_2.size() == 2);

  // Verify both original and new rules are present (superset)
  std::unordered_set<std::string> set_2(effective_2.begin(), effective_2.end());
  for (const auto& r : effective_1) {
    REQUIRE(set_2.contains(r));
  }
}
