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
        {"id": "list", "client_path": "list.js", "title": "Notes",
         "icon": "edit-3", "rbac_rule": "notes.read", "order": 10,
         "future_field": {"retained": true}}
      ]
    })";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->panels.size() == 1);
  REQUIRE(res.value->panels[0].id == "list");
  REQUIRE(res.value->panels[0].client_path == "list.js");
  REQUIRE(res.value->panels[0].title == "Notes");
  REQUIRE(res.value->panels[0].icon == "edit-3");
  REQUIRE(res.value->panels[0].rbac_rule == "notes.read");
  REQUIRE(res.value->panels[0].order == 10);
  REQUIRE(res.value->panels[0].unknown_fields["future_field"]["retained"] ==
          true);
  REQUIRE(res.messages.empty());

  auto declaration = res.value->panels[0].declaration();
  REQUIRE(declaration["id"] == "list");
  REQUIRE(declaration["rbac_rule"] == "notes.read");
  REQUIRE(declaration["order"] == 10);
  REQUIRE(declaration["future_field"]["retained"] == true);

  auto round_trip =
      PanelsManifest::parse(res.value->serialize(), "panels.json");
  REQUIRE(round_trip.value.has_value());
  REQUIRE(*round_trip.value == *res.value);
}

TEST_CASE("PanelsManifest warns on top-level array form",
          "[packages][panels]") {
  const auto* json =
      R"([{"id": "list", "client_path": "list.js", "rbac_rule": "notes.read"}])";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.shape.array_at_root") != nullptr);
}

TEST_CASE("PanelsManifest rejects missing id", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"client_path": "list.js", "rbac_rule": "notes.read"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].id.missing") != nullptr);
}

TEST_CASE("PanelsManifest rejects invalid id pattern", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"id": "Bad-ID", "client_path": "x.js", "rbac_rule": "notes.read"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].id.invalid") != nullptr);
}

TEST_CASE("PanelsManifest rejects missing client_path", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"id": "list", "rbac_rule": "notes.read"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].client_path.missing") !=
          nullptr);
}

TEST_CASE("PanelsManifest rejects absolute client_path", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"id": "list", "client_path": "/etc/hostname", "rbac_rule": "notes.read"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages,
                    "panels.panels[0].client_path.invalid_path") != nullptr);
}

TEST_CASE("PanelsManifest rejects dotdot client_path", "[packages][panels]") {
  const auto* json =
      R"({"panels": [{"id": "list", "client_path": "../escape.js", "rbac_rule": "notes.read"}]})";
  auto res = PanelsManifest::parse(json, "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages,
                    "panels.panels[0].client_path.invalid_path") != nullptr);
}

TEST_CASE("PanelsManifest requires a valid rbac_rule",
          "[packages][panels][launcher]") {
  auto missing = PanelsManifest::parse(
      R"({"panels":[{"id":"list","client_path":"list.js"}]})", "panels.json");
  REQUIRE_FALSE(missing.value.has_value());
  REQUIRE(find_rule(missing.messages, "panels.panels[0].rbac_rule.missing") !=
          nullptr);

  auto invalid = PanelsManifest::parse(
      R"({"panels":[{"id":"list","client_path":"list.js","rbac_rule":"Notes..Read"}]})",
      "panels.json");
  REQUIRE_FALSE(invalid.value.has_value());
  REQUIRE(find_rule(invalid.messages, "panels.panels[0].rbac_rule.invalid") !=
          nullptr);
}

TEST_CASE("PanelsManifest validates order and defaults it to zero",
          "[packages][panels][launcher]") {
  auto defaulted = PanelsManifest::parse(
      R"({"panels":[{"id":"list","client_path":"list.js","rbac_rule":"notes.read"}]})",
      "panels.json");
  REQUIRE(defaulted.value.has_value());
  REQUIRE(defaulted.value->panels[0].order == 0);
  REQUIRE(defaulted.value->panels[0].declaration()["order"] == 0);

  for (const auto* order : {"-1", "10001", "1.5", "\"1\""}) {
    auto json =
        std::string{
            R"({"panels":[{"id":"list","client_path":"list.js","rbac_rule":"notes.read","order":)"} +
        order + "}]}";
    auto invalid = PanelsManifest::parse(json, "panels.json");
    REQUIRE_FALSE(invalid.value.has_value());
    REQUIRE(find_rule(invalid.messages, "panels.panels[0].order.invalid") !=
            nullptr);
  }
}

TEST_CASE("PanelsManifest rejects duplicate panel ids",
          "[packages][panels][launcher]") {
  auto res = PanelsManifest::parse(
      R"({"panels":[
        {"id":"list","client_path":"list.js","rbac_rule":"notes.read"},
        {"id":"list","client_path":"other.js","rbac_rule":"notes.read"}
      ]})",
      "panels.json");
  REQUIRE_FALSE(res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[1].id.duplicate") != nullptr);
}

TEST_CASE("PanelsManifest enforces strict client path components",
          "[packages][panels][launcher]") {
  for (const auto* path :
       {".", "./list.js", "dir//list.js", "list.js/", "dir\\list.js"}) {
    auto json = nlohmann::json{{"panels",
                                {{{"id", "list"},
                                  {"client_path", path},
                                  {"rbac_rule", "notes.read"}}}}};
    auto res = PanelsManifest::parse(json.dump(), "panels.json");
    REQUIRE_FALSE(res.value.has_value());
    REQUIRE(find_rule(res.messages,
                      "panels.panels[0].client_path.invalid_path") != nullptr);
  }

  auto nul_path =
      nlohmann::json{{"panels",
                      {{{"id", "list"},
                        {"client_path", std::string{"dir\0list.js", 11}},
                        {"rbac_rule", "notes.read"}}}}};
  auto nul = PanelsManifest::parse(nul_path.dump(), "panels.json");
  REQUIRE_FALSE(nul.value.has_value());
  REQUIRE(find_rule(nul.messages,
                    "panels.panels[0].client_path.invalid_path") != nullptr);
}

TEST_CASE("PanelsManifest validates title scalars and icon grammar",
          "[packages][panels][launcher]") {
  auto json = nlohmann::json{{"panels",
                              {{{"id", "list"},
                                {"client_path", "list.js"},
                                {"rbac_rule", "notes.read"},
                                {"title", ""},
                                {"icon", "Edit_3"}}}}};
  auto res = PanelsManifest::parse(json.dump(), "panels.json");
  REQUIRE_FALSE(res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.panels[0].title.invalid") != nullptr);
  REQUIRE(find_rule(res.messages, "panels.panels[0].icon.invalid") != nullptr);

  json["panels"][0]["title"] = std::string(255, 'a') + "📝";
  json["panels"][0]["icon"] = "edit-3";
  auto exact = PanelsManifest::parse(json.dump(), "panels.json");
  REQUIRE(exact.value.has_value());

  json["panels"][0]["title"] = std::string(256, 'a') + "📝";
  auto too_long = PanelsManifest::parse(json.dump(), "panels.json");
  REQUIRE_FALSE(too_long.value.has_value());
  REQUIRE(find_rule(too_long.messages, "panels.panels[0].title.too_long") !=
          nullptr);
}

TEST_CASE("PanelsManifest rejects unparseable JSON", "[packages][panels]") {
  auto res = PanelsManifest::parse("{not json", "panels.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "panels.json.parse_error") != nullptr);
}
