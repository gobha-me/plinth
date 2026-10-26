#include "kernel/packages/http_declarations.hpp"
#include "kernel/packages/manifest.hpp"
#include "kernel/packages/validator.hpp"
#include "static_validation_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using plinth::packages::http_claim_matches;
using plinth::packages::http_claims_intersect;
using plinth::packages::HttpDeclaration;
using plinth::packages::HttpHandlerMode;
using plinth::packages::is_valid_http_claim;
using plinth::packages::ManifestParseError;
using plinth::packages::PackageManifest;

auto declaration(std::vector<std::string> claims,
                 std::string_view mode = "quickjs") -> Json {
  return {{"http_prefixes", std::move(claims)}, {"handler_mode", mode}};
}

auto has_rule(const std::vector<ManifestParseError>& messages,
              std::string_view rule) -> bool {
  return std::ranges::any_of(
      messages, [&](const auto& message) { return message.rule == rule; });
}

auto reject(const Json& document, std::string_view rule) -> void {
  const auto result = HttpDeclaration::parse(document.dump());
  REQUIRE_FALSE(result.value.has_value());
  REQUIRE(has_rule(result.messages, rule));
  for (const auto& message : result.messages) {
    REQUIRE(message.severity == plinth::packages::Severity::ERROR);
  }
}

auto numbered_claims(std::size_t count) -> std::vector<std::string> {
  std::vector<std::string> claims;
  for (std::size_t index = 0; index < count; ++index) {
    claims.push_back("/p" + std::to_string(index));
  }
  return claims;
}

auto with_extra_members(std::string manifest, std::string_view extra)
    -> std::string {
  manifest.pop_back();
  manifest += ',';
  manifest += extra;
  manifest += '}';
  return manifest;
}

} // namespace

TEST_CASE("HTTP declarations: absence and empty arrays carry no authority",
          "[packages][http-declarations]") {
  for (const auto& raw : {R"({})", R"({"http_prefixes":[]})",
                          R"({"unauthenticated_prefixes":[]})",
                          R"({"http_prefixes":[],"unauthenticated_prefixes":[],
                              "future":{"x":1},"other":{"x":2}})"}) {
    const auto result = HttpDeclaration::parse(raw);
    REQUIRE(result.value.has_value());
    REQUIRE(result.messages.empty());
    REQUIRE(result.value->http_prefixes.empty());
    REQUIRE(result.value->unauthenticated_prefixes.empty());
    REQUIRE_FALSE(result.value->handler_mode.has_value());
  }
}

TEST_CASE("HTTP declarations: all three modes are data, canonically ordered",
          "[packages][http-declarations]") {
  const std::array modes{
      std::pair{"quickjs", HttpHandlerMode::QUICKJS},
      std::pair{"sidecar", HttpHandlerMode::SIDECAR},
      std::pair{"bundled_native", HttpHandlerMode::BUNDLED_NATIVE}};
  for (const auto& [name, mode] : modes) {
    auto document = declaration({"/z/*", "/A", "/a"}, name);
    document["unauthenticated_prefixes"] = {"/z/*", "/A"};
    const auto result = HttpDeclaration::parse(document.dump());
    REQUIRE(result.value.has_value());
    REQUIRE(result.messages.empty());
    REQUIRE(result.value->handler_mode == mode);
    REQUIRE(result.value->http_prefixes ==
            std::vector<std::string>{"/A", "/a", "/z/*"});
    REQUIRE(result.value->unauthenticated_prefixes ==
            std::vector<std::string>{"/A", "/z/*"});
  }
}

TEST_CASE("HTTP claims: exact ASCII grammar accepts literal and subtree edges",
          "[packages][http-declarations]") {
  for (std::string_view claim :
       {"/a", "/A09._~-", "/a/b", "/a/*", "/a/b/*", "/.well-known", "/a/..."}) {
    INFO(claim);
    REQUIRE(is_valid_http_claim(claim));
  }
  REQUIRE(is_valid_http_claim("/" + std::string(255, 'a')));
  REQUIRE(is_valid_http_claim("/" + std::string(253, 'a') + "/*"));
}

TEST_CASE("HTTP claims: malformed grammar never normalizes into acceptance",
          "[packages][http-declarations]") {
  const std::vector<std::string> invalid{"",
                                         "/",
                                         "/*",
                                         "a",
                                         "//a",
                                         "/a/",
                                         "/a//b",
                                         "/.",
                                         "/..",
                                         "/a/./b",
                                         "/a/../b",
                                         "/a/*/b",
                                         "/a/**",
                                         "/a*",
                                         "/a//*",
                                         "/a?b",
                                         "/a#b",
                                         "/a%b",
                                         "/a%2Fb",
                                         "/a\\b",
                                         "/a b",
                                         " /a",
                                         "/a\t",
                                         "/a\n",
                                         "/a:b",
                                         "https://host/a",
                                         "/café",
                                         std::string{"/a\0b", 4},
                                         "/" + std::string(256, 'a')};
  for (const auto& claim : invalid) {
    INFO("claim byte count: " << claim.size());
    REQUIRE_FALSE(is_valid_http_claim(claim));
    REQUIRE_FALSE(http_claim_matches(claim, "/a"));
    REQUIRE_FALSE(http_claims_intersect(claim, "/a/*"));
    REQUIRE_FALSE(http_claims_intersect("/a/*", claim));
    reject(declaration({claim}), "http-prefix-invalid");
  }
}

TEST_CASE("HTTP claims: matching uses exact case and component boundaries",
          "[packages][http-declarations]") {
  struct Control {
    std::string_view claim;
    std::string_view path;
    bool matches;
  };
  const std::array controls{Control{"/status.php", "/status.php", true},
                            Control{"/status.php", "/status.php/", false},
                            Control{"/status.php", "/status.php?x=1", false},
                            Control{"/dav/*", "/dav", true},
                            Control{"/dav/*", "/dav/", true},
                            Control{"/dav/*", "/dav/files/a", true},
                            Control{"/dav/*", "/davx", false},
                            Control{"/dav/*", "/davx/a", false},
                            Control{"/dav/*", "/DAV/a", false},
                            Control{"/dav", "/dav/a", false},
                            Control{"/dav/*", "/dav/café", true}};
  for (const auto& control : controls) {
    INFO(control.claim << " -> " << control.path);
    REQUIRE(http_claim_matches(control.claim, control.path) == control.matches);
  }
}

TEST_CASE("HTTP claims: intersection is symmetric and distinguishes ownership",
          "[packages][http-declarations]") {
  struct Control {
    std::string_view left;
    std::string_view right;
    bool intersects;
  };
  const std::array controls{Control{"/dav", "/dav", true},
                            Control{"/dav", "/dav/a", false},
                            Control{"/dav/*", "/dav", true},
                            Control{"/dav/*", "/dav/*", true},
                            Control{"/dav/*", "/dav/files/*", true},
                            Control{"/dav/*", "/dav/files/a", true},
                            Control{"/dav/*", "/davx/*", false},
                            Control{"/dav/*", "/DAV/*", false},
                            Control{"/dav/a", "/dav/b", false}};
  for (const auto& control : controls) {
    REQUIRE(http_claims_intersect(control.left, control.right) ==
            control.intersects);
    REQUIRE(http_claims_intersect(control.right, control.left) ==
            control.intersects);
  }
}

TEST_CASE("HTTP declarations: both claim arrays enforce the 64 entry limit",
          "[packages][http-declarations]") {
  auto document = declaration(numbered_claims(64));
  document["unauthenticated_prefixes"] = document["http_prefixes"];
  const auto accepted = HttpDeclaration::parse(document.dump());
  REQUIRE(accepted.value.has_value());
  REQUIRE(accepted.value->http_prefixes.size() == 64);
  REQUIRE(accepted.value->unauthenticated_prefixes.size() == 64);
  document["http_prefixes"].push_back("/p64");
  reject(document, "http-prefix-invalid");
  document = declaration(numbered_claims(64));
  document["unauthenticated_prefixes"] = numbered_claims(65);
  reject(document, "http-prefix-invalid");
}

TEST_CASE("HTTP declarations: 256 bytes is inclusive for both claim forms",
          "[packages][http-declarations]") {
  const std::array controls{
      std::pair{"/" + std::string(255, 'a'), "/" + std::string(256, 'a')},
      std::pair{"/" + std::string(253, 'a') + "/*",
                "/" + std::string(254, 'a') + "/*"}};
  for (const auto& [claim, overlong] : controls) {
    REQUIRE(claim.size() == 256);
    REQUIRE(overlong.size() == 257);
    auto document = declaration({claim});
    document["unauthenticated_prefixes"] = {claim};
    const auto result = HttpDeclaration::parse(document.dump());
    REQUIRE(result.value.has_value());
    REQUIRE(result.value->http_prefixes == std::vector<std::string>{claim});
    document["http_prefixes"] = {overlong};
    reject(document, "http-prefix-invalid");
    document["http_prefixes"] = {claim};
    document["unauthenticated_prefixes"] = {overlong};
    reject(document, "http-prefix-invalid");
  }
}

TEST_CASE("HTTP declarations: arrays and entries reject coercion and objects",
          "[packages][http-declarations]") {
  const std::array<Json, 6> invalid{
      nullptr, false, 7, "/dav/*", Json::object(), Json::array({"/dav", 1})};
  for (std::string_view field : {"http_prefixes", "unauthenticated_prefixes"}) {
    for (const auto& value : invalid) {
      auto document = declaration({"/dav/*"});
      document[field] = value;
      reject(document, "http-prefix-invalid");
    }
  }
  auto document = declaration({"/dav/*"});
  document["http_prefixes"] =
      Json::array({{{"prefix", "/dav/*"}, {"handler_mode", "quickjs"}}});
  reject(document, "http-prefix-invalid");
}

TEST_CASE("HTTP declarations: duplicate and overlapping array claims reject",
          "[packages][http-declarations]") {
  reject(declaration({"/dav", "/dav"}), "http-prefix-duplicate");
  reject(declaration({"/dav/*", "/dav"}), "http-prefix-overlap");
  reject(declaration({"/dav/*", "/dav/files/*"}), "http-prefix-overlap");
  auto public_duplicate = declaration({"/dav/*"});
  public_duplicate["unauthenticated_prefixes"] = {"/dav/*", "/dav/*"};
  reject(public_duplicate, "http-prefix-duplicate");
  const auto escaped = HttpDeclaration::parse(
      R"({"http_prefixes":["/dav","/d\u0061v"],"handler_mode":"quickjs"})");
  REQUIRE_FALSE(escaped.value.has_value());
  REQUIRE(has_rule(escaped.messages, "http-prefix-duplicate"));
}

TEST_CASE("HTTP declarations: public subset requires exact declared spelling",
          "[packages][http-declarations]") {
  for (const auto& public_claim : {"/dav", "/dav/files/*", "/DAV/*"}) {
    auto document = declaration({"/dav/*"});
    document["unauthenticated_prefixes"] = {public_claim};
    reject(document, "http-public-prefix-not-declared");
  }
  reject(Json{{"unauthenticated_prefixes", {"/dav"}}},
         "http-public-prefix-not-declared");
}

TEST_CASE("HTTP declarations: mode is explicit, exact and requires claims",
          "[packages][http-declarations]") {
  reject(Json{{"http_prefixes", {"/dav"}}}, "http-handler-mode-unavailable");
  for (const auto& mode : {Json(nullptr), Json(true), Json(1), Json("QuickJS"),
                           Json("remote"), Json::object(), Json::array()}) {
    auto document = declaration({"/dav"});
    document["handler_mode"] = mode;
    reject(document, "http-handler-mode-unavailable");
  }
  for (const auto& mode : {"quickjs", "sidecar", "bundled_native"}) {
    reject(Json{{"handler_mode", mode}}, "http-mode-without-claims");
    reject(declaration({}, mode), "http-mode-without-claims");
  }
}

TEST_CASE("HTTP declarations: original raw JSON keys reject before overwrite",
          "[packages][http-declarations]") {
  for (
      std::string_view raw :
      {R"({"http_prefixes":[],"http_prefixes":["/dav"],"handler_mode":"quickjs"})",
       R"({"http_prefixes":[],"http_\u0070refixes":[]})",
       R"({"future":{"x":1,"\u0078":2}})", R"({"future":[{"x":1,"x":2}]})",
       R"({"name":"notes","name":"notes"})"}) {
    const auto result = HttpDeclaration::parse(raw, "raw-manifest.json");
    REQUIRE_FALSE(result.value.has_value());
    REQUIRE(result.messages.size() == 1);
    REQUIRE(result.messages[0].rule == "http-prefix-invalid");
    REQUIRE(result.messages[0].file == "raw-manifest.json");
    REQUIRE(result.messages[0].message ==
            "manifest contains a duplicate decoded JSON key");
  }
  REQUIRE(
      HttpDeclaration::parse(R"({"future":[{"x":1},{"x":2}],"other":{"x":3}})")
          .value.has_value());
}

TEST_CASE("HTTP declarations: invalid JSON and root kinds do not become empty",
          "[packages][http-declarations]") {
  for (std::string_view raw :
       {"", "{", "[]", "null", "true", "0", "\"x\"", "{} {}", R"({"x":NaN})",
        R"({"future":"\ud800"})", R"({"future":1e400})"}) {
    const auto result = HttpDeclaration::parse(raw);
    REQUIRE_FALSE(result.value.has_value());
    REQUIRE(has_rule(result.messages, "http-prefix-invalid"));
  }
}

TEST_CASE("HTTP declarations: canonical claim bytes determine finding order",
          "[packages][http-declarations]") {
  for (const auto& claims :
       {std::vector<std::string>{"/a/x", "/z", "/a/*", "/z"},
        std::vector<std::string>{"/z", "/a/*", "/z", "/a/x"}}) {
    const auto result = HttpDeclaration::parse(declaration(claims).dump());
    REQUIRE_FALSE(result.value.has_value());
    REQUIRE(result.messages.size() == 2);
    REQUIRE(result.messages[0].rule == "http-prefix-overlap");
    REQUIRE(result.messages[1].rule == "http-prefix-duplicate");
  }
}

TEST_CASE("HTTP declarations: six fixed kernel mounts reject component overlap",
          "[packages][http-declarations]") {
  for (std::string_view root :
       {"/api", "/ext", "/s", "/healthz", "/metrics", "/ws"}) {
    for (const auto& claim : {std::string{root}, std::string{root} + "/*",
                              std::string{root} + "/child"}) {
      reject(declaration({claim}), "http-prefix-reserved");
    }
    REQUIRE(
        HttpDeclaration::parse(declaration({std::string{root} + "x/*"}).dump())
            .value.has_value());
  }
  // /docs is not a reservation. /app is configured-frontend policy, not a
  // fixed kernel root; callers must check actual frontend/installed claims.
  REQUIRE(HttpDeclaration::parse(declaration({"/docs/*", "/app/*"}).dump())
              .value.has_value());
}

TEST_CASE(
    "HTTP submission: neutral legacy reads stay inert while submissions fail",
    "[packages][http-declarations][validator]") {
  plinth::packages::static_test::PackageCopy package;
  auto manifest = package.json("manifest.json");
  manifest["http_prefixes"] = {"/dav/*"};
  manifest["handler_mode"] = "not-a-mode";
  const auto raw = manifest.dump();
  const auto neutral = PackageManifest::parse(raw, "manifest.json");
  REQUIRE(neutral.value.has_value());
  REQUIRE(neutral.value->unknown_fields["handler_mode"] == "not-a-mode");
  REQUIRE(neutral.value->unknown_fields["http_prefixes"] ==
          Json::array({"/dav/*"}));
  package.write("manifest.json", raw);
  for (bool cross_file : {false, true}) {
    plinth::packages::ValidationConfig config;
    config.cross_file = cross_file;
    config.unicode_scanner_enabled = false;
    const auto report = plinth::packages::validate(package.root(), config);
    REQUIRE(report.error_count() > 0);
    REQUIRE(std::ranges::any_of(report.messages, [](const auto& finding) {
      return finding.rule == "http-handler-mode-unavailable" &&
             finding.path == "manifest.json/handler_mode";
    }));
  }
}

TEST_CASE(
    "HTTP submission: original raw duplicate keys survive the validator seam",
    "[packages][http-declarations][validator]") {
  plinth::packages::static_test::PackageCopy package;
  const auto raw =
      with_extra_members(package.json("manifest.json").dump(),
                         R"("http_prefixes":[],"http_\u0070refixes":[])");
  package.write("manifest.json", raw);
  for (bool cross_file : {false, true}) {
    plinth::packages::ValidationConfig config;
    config.cross_file = cross_file;
    config.unicode_scanner_enabled = false;
    const auto report = plinth::packages::validate(package.root(), config);
    REQUIRE(std::ranges::any_of(report.messages, [](const auto& finding) {
      return finding.rule == "http-prefix-invalid" &&
             finding.message ==
                 "manifest contains a duplicate decoded JSON key";
    }));
  }
}

TEST_CASE("HTTP submission: rejected numeric overflow does not escape neutral "
          "parsing",
          "[packages][http-declarations][validator]") {
  plinth::packages::static_test::PackageCopy package;
  const auto manifest = package.json("manifest.json").dump();
  for (std::string_view extra :
       {R"("future":1e400)", R"("http_prefixes":1e400)"}) {
    package.write("manifest.json", with_extra_members(manifest, extra));
    for (bool cross_file : {false, true}) {
      plinth::packages::ValidationConfig config;
      config.cross_file = cross_file;
      config.unicode_scanner_enabled = false;
      plinth::packages::ValidationReport report;
      REQUIRE_NOTHROW(report =
                          plinth::packages::validate(package.root(), config));
      REQUIRE(report.error_count() > 0);
      REQUIRE(std::ranges::any_of(report.messages, [](const auto& finding) {
        return finding.rule == "http-prefix-invalid" &&
               finding.message == "manifest is not valid JSON" &&
               finding.path == "manifest.json";
      }));
    }
  }
}
