// ICD-0.4.6 rule-validator tests — Rules A.1 through A.5.
//
// A.5 (cross-extension collision) touches PG; the unified API
// validate_rules(PGconn&, ...) is tested end-to-end against a
// scratch database using the pattern in rule_registrar_test.cpp.
// A.1–A.4 would be pure-ABI if split, but the ICD deliberately keeps
// the surface unified; this suite is PG-gated under the grouped-
// runner's `[integration]` tag.

#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"
#include "kernel/rbac/rbac_manifest.hpp"
#include "kernel/rbac/rule_validator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

using plinth::packages::CapabilityManifest;
using plinth::packages::ManifestParseError;
using plinth::packages::ProvidedCapability;
using plinth::rbac::RbacManifest;
using plinth::rbac::RbacRule;
using plinth::rbac::validate_rules;

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
};

auto has_rule(const std::vector<ManifestParseError>& msgs,
              std::string_view rule) -> bool {
  return std::ranges::any_of(msgs,
                             [&](const auto& m) { return m.rule == rule; });
}

// Simple `notes` package with one capability `notes:1:edit` — the
// signature used by the A.4 positive-resolution checks.
auto notes_caps() -> CapabilityManifest {
  CapabilityManifest cm;
  cm.provides.push_back(ProvidedCapability{
      .namespace_ = "notes",
      .version = 1,
      .function = "edit",
      .params = {},
      .returns = "object",
      .scope = "instance",
      .description = "edit a note",
      .rbac_rule = std::nullopt,
  });
  return cm;
}

} // namespace

TEST_CASE("P.M1 rule validator rejects rule name that violates the regex",
          "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "Notes.Edit", // uppercase + only two segments OK
      .namespace_ = "notes",
      .description = "Edit notes",
      .test = std::nullopt,
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE(has_rule(findings, "rbac.rule.invalid_name"));
}

TEST_CASE("P.M2 rule validator rejects rule namespace that matches neither "
          "package nor reserved kernel namespaces",
          "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "terminal.edit",
      .namespace_ = "terminal",
      .description = "Edit terminals",
      .test = std::nullopt,
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE(has_rule(findings, "rbac.rule.namespace_mismatch"));
}

TEST_CASE("P.M3 rule validator rejects test.assert_*.call that fails to parse",
          "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "notes.edit",
      .namespace_ = "notes",
      .description = "Edit notes",
      .test = nlohmann::json{{"assert_deny",
                              {
                                  {"call", "not a valid signature"},
                                  {"expect", "permission_denied"},
                              }}},
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE(has_rule(findings, "rbac.test.call.parse_error"));
}

TEST_CASE(
    "P.M4 rule validator rejects test.assert_*.call that does not resolve to "
    "a provided capability",
    "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "notes.edit",
      .namespace_ = "notes",
      .description = "Edit notes",
      .test = nlohmann::json{{"assert_deny",
                              {
                                  {"call", "notes:99:nonexistent"},
                                  {"expect", "permission_denied"},
                              }}},
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE(has_rule(findings, "rbac.test.call.unresolved"));
}

TEST_CASE("P.M5 rule validator reports cross-extension rule-name collision",
          "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  REQUIRE(s.exec_ok("INSERT INTO plinth.rbac_rules (rule, namespace, "
                    "description, extension_name) "
                    "VALUES ('notes.edit', 'notes', 'edit', 'terminal')"));

  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "notes.edit",
      .namespace_ = "notes",
      .description = "Edit notes",
      .test = std::nullopt,
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE(has_rule(findings, "rbac.rule.name_collision"));
  // Message should name the other extension so the admin knows who
  // owns the colliding rule.
  bool names_terminal = std::ranges::any_of(findings, [](const auto& f) {
    return f.rule == "rbac.rule.name_collision" &&
           f.message.find("terminal") != std::string::npos;
  });
  REQUIRE(names_terminal);
}

TEST_CASE("P.N1 rule validator is silent when the same extension re-registers "
          "its own rule (upgrade case)",
          "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  REQUIRE(s.exec_ok("INSERT INTO plinth.rbac_rules (rule, namespace, "
                    "description, extension_name) "
                    "VALUES ('notes.edit', 'notes', 'edit', 'notes')"));

  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "notes.edit",
      .namespace_ = "notes",
      .description = "Edit notes",
      .test = std::nullopt,
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE_FALSE(has_rule(findings, "rbac.rule.name_collision"));
}

TEST_CASE("rule validator accepts a reserved kernel namespace",
          "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "notes.admin",
      .namespace_ = "kernel",
      .description = "Kernel-namespaced override",
      .test = std::nullopt,
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE_FALSE(has_rule(findings, "rbac.rule.namespace_mismatch"));
}

TEST_CASE(
    "rule validator returns empty findings for a valid rule with resolving "
    "test calls",
    "[rule_validator][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  Scratch s;
  RbacManifest rm;
  rm.rules.push_back(RbacRule{
      .rule = "notes.edit",
      .namespace_ = "notes",
      .description = "Edit notes",
      .test =
          nlohmann::json{
              {"assert_deny",
               {
                   {"call", "notes:1:edit"},
                   {"expect", "permission_denied"},
               }},
              {"assert_allow",
               {
                   {"call", "notes:1:edit"},
                   {"expect", "success"},
               }},
          },
  });
  auto findings = validate_rules(rm, notes_caps(), "notes", *s.conn);
  REQUIRE(findings.empty());
}
