// Deterministic ownership and lifecycle properties for registered RBAC rules.
// The validator decides whether a registration may proceed; the registrar
// performs only registrations that the validator accepted.

#include "kernel/rbac/rule_registrar.hpp"
#include "kernel/rbac/rule_validator.hpp"

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"
#include "kernel/rbac/rbac_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using plinth::packages::CapabilityManifest;
using plinth::packages::ManifestParseError;
using plinth::rbac::RbacManifest;
using plinth::rbac::RbacRule;

// Fixed SplitMix64 arithmetic gives the same cases on every standard library.
struct Generator {
  std::uint64_t state;

  auto next() -> std::uint64_t {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  auto below(std::size_t bound) -> std::size_t {
    return static_cast<std::size_t>(next() % bound);
  }
};

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* value = std::getenv("PLINTH_PG_HOST")) {
    db.host = value;
  }
  if (auto* value = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<std::uint16_t>(std::stoi(value));
  }
  if (auto* value = std::getenv("PLINTH_PG_USER")) {
    db.user = value;
  }
  if (auto* value = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = value;
  }
  if (auto* value = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = value;
  }
  return db;
}

auto conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password + " connect_timeout=3";
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  PGconn* conn = PQconnectdb(conninfo(pg_config()).c_str());
  const bool ready = PQstatus(conn) == CONNECTION_OK;
  PQfinish(conn);
  return ready;
}

auto drop_schema(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo(db).c_str());
  if (PQstatus(conn) == CONNECTION_OK) {
    PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  }
  PQfinish(conn);
}

struct Scratch {
  plinth::Config::Database db{pg_config()};
  PGconn* conn{nullptr};

  Scratch() {
    drop_schema(db);
    plinth::db::bootstrap_schema(
        db, std::string{CMAKE_SOURCE_DIR} + "/migrations", true);
    conn = PQconnectdb(conninfo(db).c_str());
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

struct RuleState {
  std::string owner;
  std::string namespace_;
  std::string description;
  bool orphaned;
};

auto lookup_rule(PGconn& conn, std::string_view rule)
    -> std::optional<RuleState> {
  std::string name{rule};
  const char* values[] = {name.c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> result(
      PQexecParams(&conn,
                   "SELECT extension_name, namespace, description, "
                   "orphaned_at IS NOT NULL FROM plinth.rbac_rules "
                   "WHERE rule = $1",
                   1, nullptr, values, nullptr, nullptr, 0),
      PQclear);
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(result.get()) <= 1);
  if (PQntuples(result.get()) == 0) {
    return std::nullopt;
  }
  return RuleState{
      .owner = PQgetvalue(result.get(), 0, 0),
      .namespace_ = PQgetvalue(result.get(), 0, 1),
      .description = PQgetvalue(result.get(), 0, 2),
      .orphaned = std::string_view{PQgetvalue(result.get(), 0, 3)} == "t",
  };
}

auto manifest_for(std::string rule, std::string namespace_) -> RbacManifest {
  RbacManifest manifest;
  manifest.rules.push_back(RbacRule{
      .rule = std::move(rule),
      .namespace_ = std::move(namespace_),
      .description = "Generated rule",
      .test = std::nullopt,
  });
  return manifest;
}

auto validate(PGconn& conn, const RbacManifest& manifest,
              std::string_view owner) -> std::vector<ManifestParseError> {
  return plinth::rbac::validate_rules(manifest, CapabilityManifest{}, owner,
                                      conn);
}

auto has_finding(const std::vector<ManifestParseError>& findings,
                 std::string_view code) -> bool {
  return std::ranges::any_of(
      findings, [&](const auto& finding) { return finding.rule == code; });
}

auto register_rule(PGconn& conn, const RbacManifest& manifest,
                   std::string_view owner, std::string_view description)
    -> void {
  const auto& rule = manifest.rules.front();
  auto result = plinth::rbac::upsert_extension_rule(
      conn, rule.rule, rule.namespace_, description, owner, std::nullopt);
  REQUIRE(result.has_value());
}

auto require_rule(PGconn& conn, std::string_view name, std::string_view owner,
                  std::string_view namespace_, bool orphaned) -> void {
  const auto row = lookup_rule(conn, name);
  REQUIRE(row.has_value());
  CHECK(row->owner == owner);
  CHECK(row->namespace_ == namespace_);
  CHECK(row->orphaned == orphaned);
}

} // namespace

TEST_CASE("Generated RBAC rule ownership and lifecycle remain isolated",
          "[rbac][property][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch scratch;
  REQUIRE(PQstatus(scratch.conn) == CONNECTION_OK);

  constexpr std::array<std::uint64_t, 16> SEEDS{
      0x89a01ULL, 0x89a02ULL, 0x89a03ULL, 0x89a04ULL, 0x89a05ULL, 0x89a06ULL,
      0x89a07ULL, 0x89a08ULL, 0x89a09ULL, 0x89a0aULL, 0x89a0bULL, 0x89a0cULL,
      0x89a0dULL, 0x89a0eULL, 0x89a0fULL, 0x89a10ULL,
  };

  for (std::size_t case_index = 0; case_index < SEEDS.size(); ++case_index) {
    INFO("case=" << case_index << " seed=" << SEEDS[case_index]);
    Generator gen{SEEDS[case_index]};
    const auto suffix = std::to_string(case_index);
    const auto owner_a = "ownera" + suffix;
    const auto owner_b = "ownerb" + suffix;
    const auto first_owner = gen.below(2) == 0 ? owner_a : owner_b;
    const auto other_owner = first_owner == owner_a ? owner_b : owner_a;
    const auto rule_count = 1U + gen.below(3);
    std::vector<RbacManifest> owned;
    owned.reserve(rule_count);

    for (std::size_t rule_index = 0; rule_index < rule_count; ++rule_index) {
      const auto name = "property" + suffix + ".rule" +
                        std::to_string(gen.below(1000)) + "x" +
                        std::to_string(rule_index);
      owned.push_back(manifest_for(name, first_owner));
      REQUIRE(validate(*scratch.conn, owned.back(), first_owner).empty());
      register_rule(*scratch.conn, owned.back(), first_owner, "initial");
      require_rule(*scratch.conn, name, first_owner, first_owner, false);
    }

    const auto other_name =
        "property" + suffix + ".other" + std::to_string(gen.below(1000));
    const auto other_rule = manifest_for(other_name, other_owner);
    REQUIRE(validate(*scratch.conn, other_rule, other_owner).empty());
    register_rule(*scratch.conn, other_rule, other_owner, "other");

    const auto selected = gen.below(owned.size());
    const auto& selected_rule = owned[selected];
    const auto& selected_name = selected_rule.rules.front().rule;

    // An owner can refresh its rule, but a different owner may not claim it.
    REQUIRE(validate(*scratch.conn, selected_rule, first_owner).empty());
    register_rule(*scratch.conn, selected_rule, first_owner, "refreshed");
    const auto refreshed = lookup_rule(*scratch.conn, selected_name);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->description == "refreshed");

    const auto collision = manifest_for(selected_name, other_owner);
    CHECK(has_finding(validate(*scratch.conn, collision, other_owner),
                      "rbac.rule.name_collision"));
    require_rule(*scratch.conn, selected_name, first_owner, first_owner, false);

    // A different non-reserved namespace is rejected for a new rule.
    const auto wrong_namespace = manifest_for(
        "property" + suffix + ".wrong" + std::to_string(gen.below(1000)),
        other_owner);
    CHECK(has_finding(validate(*scratch.conn, wrong_namespace, first_owner),
                      "rbac.rule.namespace_mismatch"));
    const auto reserved_namespace =
        manifest_for("property" + suffix + ".reserved", "kernel");
    CHECK(validate(*scratch.conn, reserved_namespace, first_owner).empty());

    auto marked =
        plinth::rbac::mark_extension_rules_orphaned(first_owner, *scratch.conn);
    REQUIRE(marked.has_value());
    CHECK(*marked == owned.size());
    for (const auto& manifest : owned) {
      require_rule(*scratch.conn, manifest.rules.front().rule, first_owner,
                   first_owner, true);
    }
    require_rule(*scratch.conn, other_name, other_owner, other_owner, false);
    CHECK(has_finding(validate(*scratch.conn, collision, other_owner),
                      "rbac.rule.name_collision"));
    CHECK(validate(*scratch.conn, selected_rule, first_owner).empty());

    auto cleared = plinth::rbac::clear_extension_rules_orphaned(first_owner,
                                                                *scratch.conn);
    REQUIRE(cleared.has_value());
    CHECK(*cleared == owned.size());
    for (const auto& manifest : owned) {
      require_rule(*scratch.conn, manifest.rules.front().rule, first_owner,
                   first_owner, false);
    }
    require_rule(*scratch.conn, other_name, other_owner, other_owner, false);

    // No grants reference these generated rules, satisfying the registrar's
    // group_rules foreign-key precondition for physical deletion.
    auto deleted =
        plinth::rbac::delete_extension_rules(first_owner, *scratch.conn);
    REQUIRE(deleted.has_value());
    CHECK(*deleted == owned.size());
    for (const auto& manifest : owned) {
      CHECK_FALSE(
          lookup_rule(*scratch.conn, manifest.rules.front().rule).has_value());
    }
    require_rule(*scratch.conn, other_name, other_owner, other_owner, false);
  }
}
