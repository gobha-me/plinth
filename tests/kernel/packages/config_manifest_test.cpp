#include "kernel/packages/config_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using plinth::packages::ConfigManifest;
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

auto find_rule_prefix(const std::vector<ManifestParseError>& msgs,
                      std::string_view prefix) -> const ManifestParseError* {
  for (const auto& m : msgs) {
    if (std::string_view{m.rule}.starts_with(prefix)) {
      return &m;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("ConfigManifest parses scalar defaults", "[packages][config]") {
  const auto* json = R"({
      "schema": {
        "max_count": 4096,
        "enabled": true,
        "label": "Notes"
      }
    })";
  auto res = ConfigManifest::parse(json, "config.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->entries.size() == 3);
}

TEST_CASE("ConfigManifest accepts empty schema", "[packages][config]") {
  auto res = ConfigManifest::parse(R"({"schema": {}})", "config.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->entries.empty());
}

TEST_CASE("ConfigManifest rejects non-scalar defaults", "[packages][config]") {
  const auto* json = R"({
      "schema": {
        "nested": {"foo": 1}
      }
    })";
  auto res = ConfigManifest::parse(json, "config.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule_prefix(res.messages, "config.nested.non_scalar_default") !=
          nullptr);
}

TEST_CASE("ConfigManifest rejects array default", "[packages][config]") {
  const auto* json = R"({
      "schema": {
        "tags": [1, 2, 3]
      }
    })";
  auto res = ConfigManifest::parse(json, "config.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule_prefix(res.messages, "config.tags.non_scalar_default") !=
          nullptr);
}

TEST_CASE("ConfigManifest rejects invalid key pattern", "[packages][config]") {
  const auto* json = R"({
      "schema": {
        "Bad-Key": 1
      }
    })";
  auto res = ConfigManifest::parse(json, "config.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule_prefix(res.messages, "config.Bad-Key.invalid_key") !=
          nullptr);
}

TEST_CASE("ConfigManifest rejects schema non-object", "[packages][config]") {
  auto res =
      ConfigManifest::parse(R"({"schema": "not an object"})", "config.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "config.schema.not_object") != nullptr);
}

TEST_CASE("ConfigManifest rejects unparseable JSON", "[packages][config]") {
  auto res = ConfigManifest::parse("not json", "config.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "config.json.parse_error") != nullptr);
}
