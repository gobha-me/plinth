#include "kernel/packages/manifest.hpp"
#include "kernel/packages/manifest_error.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>

using plinth::packages::ManifestParseError;
using plinth::packages::PackageManifest;
using plinth::packages::Severity;

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

auto valid_manifest_json() -> std::string {
  return R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "Markdown notes",
      "author": "jeff",
      "license": "MIT",
      "entry_point": "server/main.js"
    })";
}

} // namespace

TEST_CASE("PackageManifest parses a minimal valid manifest",
          "[packages][manifest]") {
  auto res = PackageManifest::parse(valid_manifest_json(), "manifest.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->name == "notes");
  REQUIRE(res.value->version == "1.2.3");
  REQUIRE(res.value->license == "MIT");
  REQUIRE(res.messages.empty());
}

TEST_CASE("PackageManifest rejects invalid name", "[packages][manifest]") {
  const auto* json = R"({
      "name": "NotLowercase",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js"
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "manifest.name.invalid") != nullptr);
}

TEST_CASE("PackageManifest rejects non-SemVer version",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js"
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(find_rule(res.messages, "manifest.version.invalid_semver") !=
          nullptr);
}

TEST_CASE("PackageManifest rejects missing description",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js"
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(find_rule(res.messages, "manifest.description.missing") != nullptr);
}

TEST_CASE("PackageManifest rejects over-long description",
          "[packages][manifest]") {
  std::string huge(2000, 'x');
  auto json = nlohmann::json::object();
  json["name"] = "notes";
  json["version"] = "1.2.3";
  json["description"] = huge;
  json["author"] = "a";
  json["license"] = "MIT";
  json["entry_point"] = "server/main.js";
  auto res = PackageManifest::parse(json.dump(), "manifest.json");
  REQUIRE(find_rule(res.messages, "manifest.description.too_long") != nullptr);
}

TEST_CASE("PackageManifest rejects leading-slash entry_point",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "/server/main.js"
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(find_rule(res.messages, "manifest.entry_point.invalid_path") !=
          nullptr);
}

TEST_CASE("PackageManifest warns on unknown SPDX license",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "WTFPL",
      "entry_point": "server/main.js"
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(res.value.has_value()); // warning doesn't block value
  const auto* w = find_rule(res.messages, "manifest.license.unknown_spdx");
  REQUIRE(w != nullptr);
  REQUIRE(w->severity == Severity::WARNING);
}

TEST_CASE("PackageManifest rejects /ext-prefixed frontend.mount",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js",
      "frontend": {"mount": "/ext/foo", "entry": "index.html"}
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(find_rule(res.messages,
                    "manifest.frontend.mount.reserved_ext_prefix") != nullptr);
}

TEST_CASE("PackageManifest rejects zero runtime.memory_limit_mb",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js",
      "runtime": {"memory_limit_mb": 0}
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(find_rule(res.messages, "manifest.runtime.memory_limit_mb.zero") !=
          nullptr);
}

TEST_CASE("PackageManifest warns on non-empty shareable",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js",
      "shareable": [{"placeholder": true}]
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(res.value.has_value());
  const auto* w =
      find_rule(res.messages, "manifest.shareable.non_empty_reserved");
  REQUIRE(w != nullptr);
  REQUIRE(w->severity == Severity::WARNING);
}

TEST_CASE("PackageManifest preserves unknown top-level fields",
          "[packages][manifest]") {
  const auto* json = R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js",
      "future_shell_sdk_field": {"arbitrary": 42}
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->unknown_fields.contains("future_shell_sdk_field"));
  REQUIRE(res.value->unknown_fields["future_shell_sdk_field"]["arbitrary"] ==
          42);
}

TEST_CASE("PackageManifest round-trips through serialize/parse",
          "[packages][manifest]") {
  auto res = PackageManifest::parse(valid_manifest_json(), "manifest.json");
  REQUIRE(res.value.has_value());
  auto round_trip =
      PackageManifest::parse(res.value->serialize(), "manifest.json");
  REQUIRE(round_trip.value.has_value());
  REQUIRE(*round_trip.value == *res.value);
}

TEST_CASE("PackageManifest reports JSON parse errors with line/column",
          "[packages][manifest]") {
  // Trailing comma after "MIT" on line 5.
  auto json = std::string{R"({
      "name": "notes",
      "version": "1.2.3",
      "description": "d",
      "license": "MIT",,
      "author": "a"
    })"};
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(!res.value.has_value());
  const auto* e = find_rule(res.messages, "manifest.json.parse_error");
  REQUIRE(e != nullptr);
  REQUIRE(e->line.has_value());
}

// ICD-0.6.1 §5.5 / OQ4 — `name='shell'` is reserved for the bundled shell
// extension. User-uploaded manifests are rejected at parse time; bundled
// callers (first-boot install) bypass via is_bundled=true.
TEST_CASE("PackageManifest rejects name='shell' for non-bundled callers",
          "[packages][manifest][shell-reserved]") {
  const auto* json = R"({
      "name": "shell",
      "version": "1.2.3",
      "description": "d",
      "author": "a",
      "license": "MIT",
      "entry_point": "server/main.js"
    })";
  auto res = PackageManifest::parse(json, "manifest.json");
  REQUIRE(!res.value.has_value());
  REQUIRE(find_rule(res.messages, "manifest.name.reserved") != nullptr);
}

TEST_CASE("PackageManifest accepts name='shell' for bundled callers",
          "[packages][manifest][shell-reserved]") {
  const auto* json = R"({
      "name": "shell",
      "version": "0.6.1",
      "description": "Plinth bundled shell",
      "author": "plinth",
      "license": "Apache-2.0",
      "entry_point": "server/main.js",
      "frontend": { "mount": "/app", "entry": "index.html" }
    })";
  auto res = PackageManifest::parse(json, "manifest.json", /*is_bundled=*/true);
  REQUIRE(res.value.has_value());
  REQUIRE(res.value->name == "shell");
  REQUIRE(find_rule(res.messages, "manifest.name.reserved") == nullptr);
}
