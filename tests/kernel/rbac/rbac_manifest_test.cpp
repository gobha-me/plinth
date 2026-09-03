#include "kernel/packages/manifest_error.hpp"
#include "kernel/rbac/rbac_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

using plinth::packages::ManifestParseError;
using plinth::packages::Severity;
using plinth::rbac::parse_rbac_manifest;
using plinth::rbac::RbacManifest;

namespace {

auto has_rule(const std::vector<ManifestParseError>& msgs,
              std::string_view rule) -> bool {
  return std::ranges::any_of(msgs,
                             [&](const auto& m) { return m.rule == rule; });
}

auto any_error(const std::vector<ManifestParseError>& msgs) -> bool {
  return std::ranges::any_of(
      msgs, [](const auto& m) { return m.severity == Severity::ERROR; });
}

} // namespace

TEST_CASE("P.01 RbacManifest parses happy full rbac.json", "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        {
          "rule": "notes.edit",
          "namespace": "notes",
          "description": "Edit notes",
          "test": {
            "assert_deny": {
              "call": "notes:1:edit('hello')",
              "expect": "permission_denied"
            },
            "assert_allow": {
              "call": "notes:1:edit('hello')",
              "expect": "success"
            }
          }
        }
      ]
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.size() == 1);
  const auto& r = res.value->rules[0];
  REQUIRE(r.rule == "notes.edit");
  REQUIRE(r.namespace_ == "notes");
  REQUIRE(r.description == "Edit notes");
  REQUIRE(r.test.has_value());
  REQUIRE(r.test->contains("assert_deny"));
  REQUIRE(r.test->contains("assert_allow"));
  REQUIRE(res.messages.empty());
}

TEST_CASE("P.E1 RbacManifest accepts empty rules[]", "[rbac][manifest]") {
  auto res = parse_rbac_manifest(R"({"rules": []})", "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.empty());
  REQUIRE(res.messages.empty());
}

TEST_CASE("P.E2 RbacManifest accepts assert_deny-only test",
          "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        {
          "rule": "notes.edit",
          "namespace": "notes",
          "description": "Edit notes",
          "test": {
            "assert_deny": {
              "call": "notes:1:edit",
              "expect": "permission_denied"
            }
          }
        }
      ]
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.size() == 1);
  const auto& r = res.value->rules[0];
  REQUIRE(r.test.has_value());
  REQUIRE(r.test->contains("assert_deny"));
  REQUIRE(!r.test->contains("assert_allow"));
  REQUIRE(res.messages.empty());
}

TEST_CASE("P.E3 RbacManifest accepts assert_allow-only test",
          "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        {
          "rule": "notes.edit",
          "namespace": "notes",
          "description": "Edit notes",
          "test": {
            "assert_allow": {
              "call": "notes:1:edit",
              "expect": "success"
            }
          }
        }
      ]
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.size() == 1);
  const auto& r = res.value->rules[0];
  REQUIRE(r.test.has_value());
  REQUIRE(r.test->contains("assert_allow"));
  REQUIRE(!r.test->contains("assert_deny"));
  REQUIRE(res.messages.empty());
}

TEST_CASE("P.E4 RbacManifest accepts rule without test object",
          "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        {
          "rule": "notes.read",
          "namespace": "notes",
          "description": "Read notes"
        }
      ]
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.size() == 1);
  const auto& r = res.value->rules[0];
  REQUIRE(r.rule == "notes.read");
  REQUIRE(!r.test.has_value());
  REQUIRE(res.messages.empty());
}

TEST_CASE("P.R1 RbacManifest serialize → re-parse round-trip is stable",
          "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        {
          "rule": "notes.edit",
          "namespace": "notes",
          "description": "Edit notes",
          "test": {
            "assert_deny": {
              "call": "notes:1:edit('hello')",
              "expect": "permission_denied"
            },
            "assert_allow": {
              "call": "notes:1:edit('hello')",
              "expect": "success"
            }
          }
        }
      ]
    })json";
  auto first = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(first.value.has_value());
  auto dumped = first.value->serialize();
  auto second = parse_rbac_manifest(dumped, "rbac.json");
  REQUIRE(second.value.has_value());
  REQUIRE(*first.value == *second.value);
}

TEST_CASE("RbacManifest rejects non-object root", "[rbac][manifest]") {
  auto res = parse_rbac_manifest(R"([])", "rbac.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(has_rule(res.messages, "rbac.root.not_object"));
}

TEST_CASE("RbacManifest rejects rules as non-array", "[rbac][manifest]") {
  auto res = parse_rbac_manifest(R"({"rules": {}})", "rbac.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(has_rule(res.messages, "rbac.rules.not_array"));
}

TEST_CASE("RbacManifest drops rule with missing required fields",
          "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        { "rule": "notes.edit" }
      ]
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.empty());
  REQUIRE(any_error(res.messages));
  REQUIRE(has_rule(res.messages, "rbac.namespace.missing"));
  REQUIRE(has_rule(res.messages, "rbac.description.missing"));
}

TEST_CASE("RbacManifest rejects expect literal mismatch", "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [
        {
          "rule": "notes.edit",
          "namespace": "notes",
          "description": "Edit notes",
          "test": {
            "assert_deny": {
              "call": "notes:1:edit",
              "expect": "denied"
            }
          }
        }
      ]
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->rules.empty());
  REQUIRE(has_rule(res.messages, "rbac.test.expect.invalid"));
}

TEST_CASE("RbacManifest preserves unknown top-level fields",
          "[rbac][manifest]") {
  const auto* json = R"json({
      "rules": [],
      "future_top_level": "ignored"
    })json";
  auto res = parse_rbac_manifest(json, "rbac.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->unknown_fields.contains("future_top_level"));
  auto serialized = res.value->serialize();
  auto reparsed = parse_rbac_manifest(serialized, "rbac.json");
  REQUIRE(reparsed.value.has_value());
  REQUIRE(reparsed.value->unknown_fields.contains("future_top_level"));
}
