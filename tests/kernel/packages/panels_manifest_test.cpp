#include "kernel/packages/manifest_error.hpp"
#include "kernel/packages/panels_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using plinth::packages::ManifestParseError;
using plinth::packages::PanelsManifest;
using plinth::packages::PanelType;

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

auto surface(std::string_view type) -> nlohmann::json {
  auto panel = nlohmann::json{{"id", "preview"},
                              {"type", type},
                              {"client_path", "list.js"},
                              {"rbac_rule", "notes.read"}};
  if (type == "float") {
    panel["capability"] = "notes:1:read";
    panel["content_types"] = {"text/plain"};
    panel["context_schema"] = {{"version", 1},
                               {"fields",
                                {{"record_id",
                                  {{"required", true},
                                   {"max_bytes", 64},
                                   {"meaning", "Stable record identifier"}}}}},
                               {"persist_keys", {"record_id"}}};
  } else if (type == "tray") {
    panel["tray_states"] = {{"quiet", "bell"}};
    panel["tray_initial_state"] = "quiet";
    panel["preferred_width"] = 320;
  }
  return panel;
}

auto surface_document(const nlohmann::json& panel) -> nlohmann::json {
  return {{"format_version", 2}, {"panels", nlohmann::json::array({panel})}};
}

auto parse_surface(const nlohmann::json& panel)
    -> plinth::packages::PanelsManifestParseResult {
  return PanelsManifest::parse(surface_document(panel).dump(), "panels.json");
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

TEST_CASE("typed surfaces: strict v2 manifest contract",
          "[packages][panels][typed-surfaces][typed-surface-contract]") {
  auto document = surface_document(surface("primary"));
  document["panels"].push_back(surface("float"));
  document["panels"][1]["id"] = "float-preview";
  document["panels"][1]["capability"] = "notes:001:read";
  document["panels"][1]["content_types"] = {"Text/Plain",
                                            "application/ld+json"};
  document["panels"].push_back(surface("tray"));
  document["panels"][2]["id"] = "status";
  document["panels"][2]["chrome_essential"] = true;
  document["future_metadata"] = {{"inert", true}};
  document["panels"][0]["future_metadata"] = {{"inert", true}};
  auto parsed = PanelsManifest::parse(document.dump(), "panels.json");
  REQUIRE(parsed.value.has_value());
  CHECK(parsed.messages.empty());
  CHECK(parsed.value->format_version == 2);
  REQUIRE(parsed.value->panels.size() == 3);
  const auto& primary = parsed.value->panels[0];
  const auto& floating = parsed.value->panels[1];
  const auto& tray = parsed.value->panels[2];
  CHECK(primary.typed_declaration);
  CHECK(primary.type == PanelType::PRIMARY);
  CHECK_FALSE(primary.accepts_navigation_intent);
  CHECK_FALSE(primary.context_schema.has_value());
  CHECK(primary.unknown_fields["future_metadata"]["inert"] == true);
  CHECK(floating.type == PanelType::FLOAT);
  CHECK(floating.capability == "notes:1:read");
  CHECK(floating.content_types ==
        std::vector<std::string>{"text/plain", "application/ld+json"});
  REQUIRE(floating.context_schema.has_value());
  CHECK(floating.context_schema->version == 1);
  REQUIRE(floating.context_schema->fields.contains("record_id"));
  CHECK(floating.context_schema->fields.at("record_id").required);
  CHECK(floating.context_schema->fields.at("record_id").max_bytes == 64);
  CHECK(floating.context_schema->fields.at("record_id").meaning ==
        "Stable record identifier");
  CHECK(floating.context_schema->persist_keys ==
        std::vector<std::string>{"record_id"});
  CHECK(tray.type == PanelType::TRAY);
  CHECK(tray.tray_states.at("quiet") == "bell");
  CHECK(tray.tray_initial_state == "quiet");
  CHECK(tray.preferred_width == 320);
  CHECK(tray.chrome_essential); // Declaration only: this confers no essential
                                // admission.
  CHECK(parsed.value->unknown_fields["future_metadata"]["inert"] == true);
  auto round_trip =
      PanelsManifest::parse(parsed.value->serialize(), "panels.json");
  REQUIRE(round_trip.value.has_value());
  CHECK(*round_trip.value == *parsed.value);
  CHECK(floating.declaration()["type"] == "float");
  CHECK(floating.declaration()["content_types"][0] == "text/plain");
  CHECK(tray.declaration()["chrome_essential"] == true);
  auto missing = surface("float");
  missing.erase("context_schema");
  CHECK_FALSE(parse_surface(missing).value.has_value());
  auto mixed = surface("primary");
  mixed["capability"] = nullptr;
  CHECK_FALSE(parse_surface(mixed).value.has_value());
  auto bad_mime = surface("float");
  bad_mime["content_types"] = {"text/*"};
  CHECK_FALSE(parse_surface(bad_mime).value.has_value());
  auto bad_tray = surface("tray");
  bad_tray["tray_initial_state"] = "missing";
  CHECK_FALSE(parse_surface(bad_tray).value.has_value());
}

TEST_CASE("typed surfaces: absent marker preserves all inert legacy metadata",
          "[packages][panels][typed-surfaces]") {
  auto panel = surface("float");
  panel["type"] = "unrecognized-future-type";
  panel["content_types"] = nullptr;
  panel["chrome_essential"] = true;
  panel["context_schema"] = "legacy opaque value";
  auto parsed = PanelsManifest::parse(
      nlohmann::json{{"panels", {panel}}}.dump(), "panels.json");
  REQUIRE(parsed.value.has_value());
  CHECK_FALSE(parsed.value->format_version.has_value());
  const auto& entry = parsed.value->panels.front();
  CHECK(entry.type == PanelType::PRIMARY);
  CHECK_FALSE(entry.typed_declaration);
  CHECK_FALSE(entry.capability.has_value());
  CHECK(entry.content_types.empty());
  CHECK_FALSE(entry.context_schema.has_value());
  CHECK_FALSE(entry.chrome_essential);
  CHECK(entry.unknown_fields["type"] == "unrecognized-future-type");
  CHECK(entry.unknown_fields["chrome_essential"] == true);
  CHECK(entry.unknown_fields["context_schema"] == "legacy opaque value");
  CHECK(entry.declaration()["content_types"].is_null());
  CHECK_FALSE(entry.declaration().contains("accepts_navigation_intent"));
  auto round_trip =
      PanelsManifest::parse(parsed.value->serialize(), "panels.json");
  REQUIRE(round_trip.value.has_value());
  CHECK(*round_trip.value == *parsed.value);
  // The historical parser keeps the final DOM value for repeated raw keys.
  auto legacy_duplicate = PanelsManifest::parse(
      R"({"panels":[],"opaque":1,"opaque":2})", "panels.json");
  REQUIRE(legacy_duplicate.value.has_value());
  CHECK(legacy_duplicate.value->unknown_fields["opaque"] == 2);
}

TEST_CASE("typed surfaces: explicit versions and every observed raw duplicate "
          "fail closed",
          "[packages][panels][typed-surfaces]") {
  for (const auto& version : std::vector<nlohmann::json>{
           nullptr, false, 0, 1, 3, 2.0, "2", nlohmann::json::object()}) {
    auto document = surface_document(surface("primary"));
    document["format_version"] = version;
    auto parsed = PanelsManifest::parse(document.dump(), "panels.json");
    CHECK_FALSE(parsed.value.has_value());
    CHECK(find_rule(parsed.messages, "panels.format_version.unsupported") !=
          nullptr);
  }
  for (
      const auto* raw :
      {R"({"format_version":2,"format_version":0,"panels":[]})",
       R"({"format_version":0,"format_version":2,"panels":[]})",
       R"({"format_version":2,"panels":[],"opaque":{"a":1,"a":2}})",
       R"({"format_version":2,"panels":[{"type":"primary","type":"tray"}]})",
       R"({"format_version":2,"panels":[{"context_schema":{"fields":{"id":1,"id":2}}}]})",
       R"({"format_version":2,"panels":[],"opaque":{"x":1,"\u0078":2}})"}) {
    auto parsed = PanelsManifest::parse(raw, "panels.json");
    CHECK_FALSE(parsed.value.has_value());
    CHECK(find_rule(parsed.messages, "panels.json.duplicate_key") != nullptr);
  }
  for (const auto* raw :
       {R"({"format_version":2,"panels":[],"bad":"\ud800"})",
        R"({"format_version":2,"panels":[],"bad":"\udc00"})"}) {
    CHECK_FALSE(PanelsManifest::parse(raw, "panels.json").value.has_value());
  }
  auto invalid_utf8 = std::string{R"({"format_version":2,"panels":[],"bad":")"};
  invalid_utf8.push_back(static_cast<char>(0xc0));
  invalid_utf8 += "\"}";
  CHECK_FALSE(
      PanelsManifest::parse(invalid_utf8, "panels.json").value.has_value());
}

TEST_CASE("typed surfaces: type-specific field presence and navigation "
          "combinations are strict",
          "[packages][panels][typed-surfaces]") {
  for (const auto* type : {"settings", "PRIMARY", "", "future"}) {
    auto panel = surface("primary");
    panel["type"] = type;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  auto absent_type = surface("primary");
  absent_type.erase("type");
  CHECK_FALSE(parse_surface(absent_type).value.has_value());
  for (const auto* type : {"primary", "tray"}) {
    for (const auto* field : {"capability", "content_types"}) {
      auto panel = surface(type);
      panel[field] = nullptr;
      CHECK_FALSE(parse_surface(panel).value.has_value());
    }
  }
  for (const auto* type : {"primary", "float"}) {
    for (const auto* field : {"tray_states", "tray_initial_state",
                              "preferred_width", "chrome_essential"}) {
      auto panel = surface(type);
      panel[field] = nullptr;
      CHECK_FALSE(parse_surface(panel).value.has_value());
    }
  }
  auto navigation = surface("primary");
  navigation["accepts_navigation_intent"] = true;
  CHECK_FALSE(parse_surface(navigation).value.has_value());
  navigation["context_schema"] = surface("float")["context_schema"];
  auto receiver = parse_surface(navigation);
  REQUIRE(receiver.value.has_value());
  CHECK(receiver.value->panels[0].accepts_navigation_intent);
  navigation["accepts_navigation_intent"] = false;
  CHECK(parse_surface(navigation).value.has_value());
  for (const auto* type : {"float", "tray"}) {
    auto panel = surface(type);
    panel["accepts_navigation_intent"] = true;
    CHECK_FALSE(parse_surface(panel).value.has_value());
    panel["accepts_navigation_intent"] = false;
    panel["context_schema"] = surface("float")["context_schema"];
    CHECK(parse_surface(panel).value.has_value());
  }
  auto untyped_bool = surface("primary");
  untyped_bool["accepts_navigation_intent"] = "false";
  CHECK_FALSE(parse_surface(untyped_bool).value.has_value());
}

TEST_CASE("typed surfaces: bounded context schemas preserve projection "
          "requests without approval",
          "[packages][panels][typed-surfaces]") {
  auto panel = surface("float");
  const auto original = panel["context_schema"];
  for (const auto* key : {"extra", "version", "fields", "persist_keys"}) {
    panel["context_schema"] = original;
    if (std::string_view{key} == "extra")
      panel["context_schema"][key] = true;
    else
      panel["context_schema"].erase(key);
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto& value :
       std::vector<nlohmann::json>{nullptr, false, -1, 513, 1.5, "1"}) {
    panel["context_schema"] = original;
    panel["context_schema"]["fields"]["record_id"]["max_bytes"] = value;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto* key : {"__proto__", "prototype", "constructor", ""}) {
    panel["context_schema"] = original;
    panel["context_schema"]["fields"][key] = original["fields"]["record_id"];
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto* field : {"required", "max_bytes", "meaning"}) {
    panel["context_schema"] = original;
    panel["context_schema"]["fields"]["record_id"].erase(field);
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto& version :
       std::vector<nlohmann::json>{nullptr, false, 0, 2, 1.0, "1"}) {
    panel["context_schema"] = original;
    panel["context_schema"]["version"] = version;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto& required : std::vector<nlohmann::json>{nullptr, 0, "true"}) {
    panel["context_schema"] = original;
    panel["context_schema"]["fields"]["record_id"]["required"] = required;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto& meaning : std::vector<nlohmann::json>{nullptr, ""}) {
    panel["context_schema"] = original;
    panel["context_schema"]["fields"]["record_id"]["meaning"] = meaning;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  panel["context_schema"] = original;
  panel["context_schema"]["fields"]["record_id"]["meaning"] =
      std::string{"a\0b", 3};
  panel["title"] = std::string{"a\0b", 3};
  auto nul_text = parse_surface(panel);
  REQUIRE(nul_text.value.has_value());
  CHECK(nul_text.value->panels[0].title == std::string{"a\0b", 3});
  CHECK(nul_text.value->panels[0]
            .context_schema->fields.at("record_id")
            .meaning == std::string{"a\0b", 3});
  // Structural UTF-8 validity is not a promise that PostgreSQL JSONB stores
  // NUL. The existing native installation boundary may reject it honestly.
  auto key_field = original["fields"]["record_id"];
  panel["context_schema"] = {{"version", 1},
                             {"fields", {{std::string(64, 'x'), key_field}}},
                             {"persist_keys", nlohmann::json::array()}};
  CHECK(parse_surface(panel).value.has_value());
  panel["context_schema"]["fields"] = {{std::string(65, 'x'), key_field}};
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["context_schema"]["fields"] = {{std::string{"a\0b", 3}, key_field}};
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["context_schema"] = original;
  panel["context_schema"]["fields"]["record_id"]["extra"] = true;
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["context_schema"] = original;
  panel["context_schema"]["persist_keys"] = {"record_id", "record_id"};
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["context_schema"]["persist_keys"] = {"undeclared"};
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["context_schema"] = original;
  panel["context_schema"]["fields"]["record_id"]["max_bytes"] = 0;
  CHECK(parse_surface(panel).value.has_value());
  panel["context_schema"]["fields"]["record_id"]["max_bytes"] = 512;
  panel["context_schema"]["fields"]["record_id"]["meaning"] =
      std::string(256, 'x');
  CHECK(parse_surface(panel).value.has_value());
  panel["context_schema"]["fields"]["record_id"]["meaning"] =
      std::string(257, 'x');
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["context_schema"] = {{"version", 1},
                             {"fields", nlohmann::json::object()},
                             {"persist_keys", nlohmann::json::array()}};
  for (int i = 0; i < 16; ++i)
    panel["context_schema"]["fields"]["field" + std::to_string(i)] =
        original["fields"]["record_id"];
  CHECK(parse_surface(panel).value.has_value());
  panel["context_schema"]["fields"]["seventeenth"] =
      original["fields"]["record_id"];
  CHECK_FALSE(parse_surface(panel).value.has_value());
}

TEST_CASE(
    "typed surfaces: canonical literal content types and capability bounds",
    "[packages][panels][typed-surfaces]") {
  for (const auto* type :
       {"text/*", "*/plain", " text/plain", "text/plain ", "text/plain\n",
        "text/plain; charset=utf-8", "text", "text/", "/plain", "text//plain",
        "tést/plain", "text/?plain", "!text/plain"}) {
    auto panel = surface("float");
    panel["content_types"] = {type};
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  auto panel = surface("float");
  panel["content_types"] = {"text/plain", "TEXT/PLAIN"};
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["content_types"] = {"a/" + std::string(126, 'b')};
  CHECK(parse_surface(panel).value.has_value());
  panel["content_types"] = {"a/" + std::string(127, 'b')};
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel["content_types"] = nlohmann::json::array();
  CHECK(parse_surface(panel)
            .value.has_value()); // At most32, no invented minimum.
  for (int i = 0; i < 32; ++i)
    panel["content_types"].push_back("application/x-" + std::to_string(i));
  CHECK(parse_surface(panel).value.has_value());
  panel["content_types"].push_back("application/x-extra");
  CHECK_FALSE(parse_surface(panel).value.has_value());
  for (const auto* capability :
       {"notes:0:read", "notes:*:read", "notes:1:read\n", "notes:1:read:extra",
        "notes:1:..read"}) {
    panel = surface("float");
    panel["capability"] = capability;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  panel = surface("float");
  panel["capability"] = std::string(257, 'a');
  CHECK_FALSE(parse_surface(panel).value.has_value());
}

TEST_CASE(
    "typed surfaces: tray states width and metadata retain strict common rules",
    "[packages][panels][typed-surfaces]") {
  auto panel = surface("tray");
  for (int i = 0; i < 15; ++i)
    panel["tray_states"]["state-" + std::to_string(i)] = "bell";
  CHECK(parse_surface(panel).value.has_value());
  panel["tray_states"]["seventeenth"] = "bell";
  CHECK_FALSE(parse_surface(panel).value.has_value());
  for (const auto* state : {"bad_state", "UPPER", "", "quiet\n"}) {
    panel = surface("tray");
    panel["tray_states"][state] = "bell";
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (const auto& width :
       std::vector<nlohmann::json>{nullptr, false, 279, 481, 320.0, "320"}) {
    panel = surface("tray");
    panel["preferred_width"] = width;
    CHECK_FALSE(parse_surface(panel).value.has_value());
  }
  for (int width : {280, 480}) {
    panel = surface("tray");
    panel["preferred_width"] = width;
    CHECK(parse_surface(panel).value.has_value());
  }
  panel = surface("tray");
  panel["chrome_essential"] = 1;
  CHECK_FALSE(parse_surface(panel).value.has_value());
  panel = surface("primary");
  panel["title"] = std::string(256, 'x');
  CHECK(parse_surface(panel).value.has_value());
  panel["title"] = std::string(257, 'x');
  CHECK_FALSE(parse_surface(panel).value.has_value());
  std::string emoji_title;
  for (int i = 0; i < 256; ++i)
    emoji_title += "😀";
  panel["title"] = emoji_title;
  CHECK(parse_surface(panel)
            .value.has_value()); // 256scalars,1024UTF8bytes, no widening.
  panel["client_path"] = std::string(2046, 'a') + ".j";
  CHECK(parse_surface(panel).value.has_value());
  panel["client_path"] = std::string(2047, 'a') + ".j";
  CHECK_FALSE(parse_surface(panel).value.has_value());
}

TEST_CASE(
    "typed surfaces: compact normalized byte and entry limits include defaults",
    "[packages][panels][typed-surfaces]") {
  constexpr std::size_t declaration_limit = 16 * 1024;
  constexpr std::size_t manifest_limit = 256 * 1024;
  auto document = surface_document(surface("primary"));
  document["panels"][0]["padding"] = "";
  auto initial = PanelsManifest::parse(document.dump(), "panels.json");
  REQUIRE(initial.value.has_value());
  auto base = initial.value->panels[0].declaration().dump().size();
  REQUIRE(base < declaration_limit);
  document["panels"][0]["padding"] = std::string(declaration_limit - base, 'x');
  auto exact = PanelsManifest::parse(document.dump(), "panels.json");
  REQUIRE(exact.value.has_value());
  CHECK(exact.value->panels[0].declaration().dump().size() ==
        declaration_limit);
  document["panels"][0]["padding"].get_ref<std::string&>().push_back('x');
  CHECK(document["panels"][0].dump().size() <= declaration_limit);
  auto too_large = PanelsManifest::parse(document.dump(), "panels.json");
  CHECK_FALSE(too_large.value.has_value());
  CHECK(find_rule(too_large.messages,
                  "panels.panels[0].declaration.too_large") != nullptr);
  document = surface_document(surface("primary"));
  document["padding"] = "";
  initial = PanelsManifest::parse(document.dump(), "panels.json");
  REQUIRE(initial.value.has_value());
  base = nlohmann::json::parse(initial.value->serialize()).dump().size();
  document["padding"] = std::string(manifest_limit - base, 'x');
  exact = PanelsManifest::parse(document.dump(), "panels.json");
  REQUIRE(exact.value.has_value());
  CHECK(nlohmann::json::parse(exact.value->serialize()).dump().size() ==
        manifest_limit);
  document["padding"].get_ref<std::string&>().push_back('x');
  CHECK(document.dump().size() <= manifest_limit);
  too_large = PanelsManifest::parse(document.dump(), "panels.json");
  CHECK_FALSE(too_large.value.has_value());
  CHECK(find_rule(too_large.messages, "panels.manifest.too_large") != nullptr);
  document = {{"format_version", 2}, {"panels", nlohmann::json::array()}};
  for (int i = 0; i < 256; ++i) {
    auto panel = surface("primary");
    panel["id"] = "panel" + std::to_string(i);
    document["panels"].push_back(std::move(panel));
  }
  CHECK(
      PanelsManifest::parse(document.dump(), "panels.json").value.has_value());
  auto extra = surface("primary");
  extra["id"] = "extra";
  document["panels"].push_back(extra);
  CHECK_FALSE(
      PanelsManifest::parse(document.dump(), "panels.json").value.has_value());
  auto spaced = std::string(manifest_limit + 1, ' ') +
                surface_document(surface("primary")).dump();
  CHECK(PanelsManifest::parse(spaced, "panels.json").value.has_value());
}
