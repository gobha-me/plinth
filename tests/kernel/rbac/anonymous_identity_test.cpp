#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "kernel/audit/handlers.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/rbac/enforcement.hpp"

// ── Anonymous-identity enforcement test (roadmap 0.2.6.1) ───────────
//
// Architecture/01-identity.md §3 requires a permanent safeguard test
// asserting that `UserContext::anonymous()` is rejected by every
// RBAC-gated route until a rule is explicitly granted to `everyone`.
// This file is that safeguard. Its job is to fail loudly whenever
// a future change breaks the "no anonymous access by default" invariant.
//
// Coverage is in two axes:
//   Axis A — registry coverage. Walk every route populated by the
//            production registration helpers and assert that the
//            baseline anonymous context (everyone has zero rules)
//            cannot satisfy any of them. An empty required-rules
//            vector is a test failure: it would silently permit
//            anonymous access via vacuous match.
//   Axis B — grant unlocks. Granting a rule to `everyone` and
//            rebuilding the anonymous context with that rule must
//            unlock the routes that require it, confirming the
//            denial is not an unconditional fail-closed at the
//            capability-check layer.

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

// Return the rules granted to the `everyone` group — the production
// "effective rules" for an anonymous identity per §3. Mirrors the SQL
// the RbacFilter runs for authenticated users, but keyed on the group
// instead of a specific user.
auto everyone_effective_rules(TestPg& pg) -> std::vector<std::string> {
  auto res = pg.exec_params("SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
                            "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
                            "JOIN plinth.groups g ON g.id = gr.group_id "
                            "WHERE g.name = 'everyone'",
                            {});
  int n = PQntuples(res.get());
  std::vector<std::string> rules;
  rules.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    rules.emplace_back(PQgetvalue(res.get(), i, 0));
  }
  return rules;
}

auto grant_rule_to_group_by_name(TestPg& pg, const std::string& group_name,
                                 const std::string& rule) -> void {
  pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                 "SELECT g.id, r.id FROM plinth.groups g, plinth.rbac_rules r "
                 "WHERE g.name = $1 AND r.rule = $2",
                 {group_name, rule});
}

auto revoke_rule_from_group_by_name(TestPg& pg, const std::string& group_name,
                                    const std::string& rule) -> void {
  pg.exec_params(
      "DELETE FROM plinth.group_rules "
      "WHERE group_id = (SELECT id FROM plinth.groups WHERE name = $1) "
      "  AND rule_id  = (SELECT id FROM plinth.rbac_rules WHERE rule = $2)",
      {group_name, rule});
}

// Mirrors RbacFilter's grant check: ANY required rule present in the
// caller's effective set grants access.
auto grants_access(const std::vector<std::string>& effective,
                   const std::vector<std::string>& required) -> bool {
  std::unordered_set<std::string> set(effective.begin(), effective.end());
  return std::ranges::any_of(required,
                             [&](const auto& r) { return set.contains(r); });
}

// Populate the registry with the production RBAC-gated routes. Calling
// the real registration helpers means new routes added in future
// milestones are picked up automatically — the defining property of
// the registry-coverage safeguard. Idempotent (register_rule_requirement
// replaces on re-register).
auto register_production_routes() -> void {
  plinth::audit::register_audit_routes();
  plinth::groups::register_group_routes();
}

} // namespace

// ── Axis 0 — factory shape ──────────────────────────────────────────

TEST_CASE("UserContext::anonymous() produces the baseline sentinel",
          "[rbac][anonymous]") {
  auto ctx = plinth::capabilities::UserContext::anonymous();
  REQUIRE(ctx.user_id.empty());
  REQUIRE(ctx.username.empty());
  REQUIRE(ctx.auth_type == "anonymous");
  REQUIRE(ctx.effective_rules.empty());
  REQUIRE(ctx.session_id.empty());
  REQUIRE(ctx.ip_address.empty());
}

TEST_CASE("UserContext::anonymous_with_rules() preserves the sentinel identity",
          "[rbac][anonymous]") {
  auto ctx = plinth::capabilities::UserContext::anonymous_with_rules(
      {"kernel.admin", "ext.read"});
  REQUIRE(ctx.user_id.empty());
  REQUIRE(ctx.auth_type == "anonymous");
  REQUIRE(ctx.effective_rules.size() == 2);
  REQUIRE(ctx.effective_rules[0] == "kernel.admin");
  REQUIRE(ctx.effective_rules[1] == "ext.read");
}

// ── Axis A — registry coverage ──────────────────────────────────────

TEST_CASE("Anonymous rejected by every registered RBAC-gated route",
          "[rbac][anonymous]") {
  register_production_routes();
  auto routes = plinth::rbac::list_registered_rules();

  // The registry must be non-empty — otherwise the coverage
  // assertion below is vacuous and the safeguard silent.
  REQUIRE_FALSE(routes.empty());

  auto anonymous = plinth::capabilities::UserContext::anonymous();
  for (const auto& route : routes) {
    // Empty required-rules would let ANY caller through (including
    // anonymous) via vacuous match. Treat the registration itself
    // as the bug.
    INFO("route: method=" << drogon::to_string_view(route.method)
                          << " path=" << route.path_pattern);
    REQUIRE_FALSE(route.rules.empty());
    CHECK_FALSE(grants_access(anonymous.effective_rules, route.rules));
  }
}

// ── Axis B — grant unlocks ──────────────────────────────────────────

TEST_CASE("Granting kernel.admin to everyone unlocks anonymous on gated routes",
          "[rbac][anonymous][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);

  register_production_routes();
  auto routes = plinth::rbac::list_registered_rules();
  REQUIRE_FALSE(routes.empty());

  // Baseline: `everyone` has no rules, anonymous locked out everywhere.
  {
    auto baseline = everyone_effective_rules(pg);
    REQUIRE(baseline.empty());
    auto anon =
        plinth::capabilities::UserContext::anonymous_with_rules(baseline);
    for (const auto& route : routes) {
      INFO("baseline route: method=" << drogon::to_string_view(route.method)
                                     << " path=" << route.path_pattern);
      CHECK_FALSE(grants_access(anon.effective_rules, route.rules));
    }
  }

  // Grant kernel.admin to everyone. Now every kernel.admin-gated
  // route in the registry should admit the anonymous context.
  grant_rule_to_group_by_name(pg, "everyone", "kernel.admin");
  {
    auto unlocked = everyone_effective_rules(pg);
    REQUIRE(unlocked.size() == 1);
    REQUIRE(unlocked[0] == "kernel.admin");
    auto anon =
        plinth::capabilities::UserContext::anonymous_with_rules(unlocked);

    int kernel_admin_routes = 0;
    for (const auto& route : routes) {
      auto requires_kernel_admin =
          std::ranges::find(route.rules, std::string{"kernel.admin"}) !=
          route.rules.end();
      if (requires_kernel_admin) {
        ++kernel_admin_routes;
        INFO("granted route: method=" << drogon::to_string_view(route.method)
                                      << " path=" << route.path_pattern);
        CHECK(grants_access(anon.effective_rules, route.rules));
      }
    }
    // Every route registered today requires kernel.admin (audit +
    // groups). If a future route adds a different gate, this
    // assertion relaxes automatically — the grant-unlocks axis
    // still holds for the subset that matches.
    REQUIRE(kernel_admin_routes > 0);
  }

  // Revoke and confirm the lockout returns.
  revoke_rule_from_group_by_name(pg, "everyone", "kernel.admin");
  {
    auto post_revoke = everyone_effective_rules(pg);
    REQUIRE(post_revoke.empty());
    auto anon =
        plinth::capabilities::UserContext::anonymous_with_rules(post_revoke);
    for (const auto& route : routes) {
      CHECK_FALSE(grants_access(anon.effective_rules, route.rules));
    }
  }
}
