#include "kernel/packages/manifest_error.hpp"
#include "kernel/packages/panels_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using plinth::packages::ManifestParseError;
using plinth::packages::PanelsManifest;

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

} // namespace

TEST_CASE("PanelsManifest parses a valid object-form manifest",
          "[packages][panels]") {
  const auto* json = R"({
      "panels": [
        {"id": "list", "client_path": "list.js", "title": "Notes"}
      ]
    })";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->panels.size() == 1);
  REQUIRE(res.value->panels[0].id == "list");
  REQUIRE(res.value->panels[0].client_path == "list.js");
  REQUIRE(res.value->panels[0].title == "Notes");
  REQUIRE(res.messages.empty());
}

TEST_CASE("PanelsManifest warns on top-level array form",
          "[packages][panels]") {
  const auto* json = R"([{"id": "list", "client_path": "list.js"}])";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.shape.array_at_root") != nullptr);
}

TEST_CASE("PanelsManifest rejects missing id", "[packages][panels]") {
  const auto* json = R"({"panels": [{"client_path": "list.js"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].id.missing") != nullptr);
}

TEST_CASE("PanelsManifest rejects invalid id pattern", "[packages][panels]") {
  const auto* json = R"({"panels": [{"id": "Bad-ID", "client_path": "x.js"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].id.invalid") != nullptr);
}

TEST_CASE("PanelsManifest rejects missing client_path", "[packages][panels]") {
  const auto* json = R"({"panels": [{"id": "list"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].client_path.missing") !=
          nullptr);
}

TEST_CASE("PanelsManifest rejects absolute client_path", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"id": "list", "client_path": "/etc/hostname"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages,
                    "panels.panels[0].client_path.invalid_path") != nullptr);
}

TEST_CASE("PanelsManifest rejects dotdot client_path", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"id": "list", "client_path": "../escape.js"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages,
                    "panels.panels[0].client_path.invalid_path") != nullptr);
}

TEST_CASE("PanelsManifest rejects unparseable JSON", "[packages][panels]") {
  auto res = PanelsManifest::parse("{not json", "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.json.parse_error") != nullptr);
}
