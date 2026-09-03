#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using plinth::packages::CapabilityManifest;
using plinth::packages::ManifestParseError;

namespace {

auto find_rule(const std::vector<ManifestParseError>& msgs,
               std::string_view rule) -> const ManifestParseError* {
  for (const auto& m : msgs) {
    if (m.rule == rule) {
      return &m;
    }
  }
  return nullptr;
}

auto valid_caps_json() -> std::string {
  return R"({
      "provides": [
        {
          "namespace": "notes",
          "version": 1,
          "function": "create",
          "params": [
            {"name": "title", "type": "string"}
          ],
          "returns": "object",
          "scope": "instance",
          "description": "Create a note",
          "rbac_rule": "notes.create"
        }
      ],
      "requires": ["audit:1:record"]
    })";
}

} // namespace

TEST_CASE("CapabilityManifest parses a full valid manifest",
          "[packages][capabilities]") {
  auto res = CapabilityManifest::parse(valid_caps_json(), "capabilities.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->provides.size() == 1);
  REQUIRE(res.value->provides[0].namespace_ == "notes");
  REQUIRE(res.value->provides[0].function == "create");
  REQUIRE(res.value->requires_.size() == 1);
  REQUIRE(res.value->requires_[0] == "audit:1:record");
  REQUIRE(res.messages.empty());
}

TEST_CASE("CapabilityManifest accepts empty provides[]",
          "[packages][capabilities]") {
  auto res =
      CapabilityManifest::parse(R"({"provides": []})", "capabilities.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->provides.empty());
}

TEST_CASE("CapabilityManifest rejects invalid namespace",
          "[packages][capabilities]") {
  const auto* json = R"({
      "provides": [
        {
          "namespace": "CamelCase",
          "version": 1,
          "function": "create",
          "returns": "object",
          "scope": "instance",
          "description": "d"
        }
      ]
    })";
  auto res = CapabilityManifest::parse(json, "capabilities.json");
  REQUIRE(find_rule(res.messages,
                    "capabilities.provides[0].namespace.invalid") != nullptr);
}

TEST_CASE("CapabilityManifest rejects invalid scope",
          "[packages][capabilities]") {
  const auto* json = R"({
      "provides": [
        {
          "namespace": "notes",
          "version": 1,
          "function": "create",
          "returns": "object",
          "scope": "global",
          "description": "d"
        }
      ]
    })";
  auto res = CapabilityManifest::parse(json, "capabilities.json");
  REQUIRE(find_rule(res.messages, "capabilities.provides[0].scope.invalid") !=
          nullptr);
}

TEST_CASE("CapabilityManifest rejects invalid requires[] signature",
          "[packages][capabilities]") {
  const auto* json = R"({
      "requires": ["not a signature"]
    })";
  auto res = CapabilityManifest::parse(json, "capabilities.json");
  REQUIRE(find_rule(res.messages,
                    "capabilities.requires[0].invalid_signature") != nullptr);
}

TEST_CASE("CapabilityManifest rejects invalid param type",
          "[packages][capabilities]") {
  const auto* json = R"({
      "provides": [
        {
          "namespace": "notes",
          "version": 1,
          "function": "create",
          "params": [{"name": "title", "type": "float"}],
          "returns": "object",
          "scope": "instance",
          "description": "d"
        }
      ]
    })";
  auto res = CapabilityManifest::parse(json, "capabilities.json");
  // Rule-name uses the rule_for helper which renders params.[idx].type.invalid
  bool found = false;
  for (const auto& m : res.messages) {
    if (m.rule.find("params") != std::string::npos &&
        m.rule.find("type") != std::string::npos) {
      found = true;
      break;
    }
  }
  REQUIRE(found);
}

TEST_CASE("CapabilityManifest round-trips through serialize/parse",
          "[packages][capabilities]") {
  auto res = CapabilityManifest::parse(valid_caps_json(), "capabilities.json");
  REQUIRE(res.value.has_value());
  auto round_trip =
      CapabilityManifest::parse(res.value->serialize(), "capabilities.json");
  REQUIRE(round_trip.value.has_value());
  REQUIRE(*round_trip.value == *res.value);
}
