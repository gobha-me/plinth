#include "kernel/frontend/surface_protocol.hpp"

#include "kernel/packages/restore_digest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
namespace protocol = plinth::frontend::protocol;
namespace generation = plinth::packages::generation;
using ProtocolJson = nlohmann::json;
constexpr std::string_view GENERATION = "10000000-0000-4000-8000-000000000046";
constexpr std::string_view ORIGIN = "http://127.0.0.1:8765";
constexpr std::string_view PAYLOAD =
    R"({"content_type":"text/plain","credential":"sanADvnIuudZjcwLQI8bj--LzqaiKRooLYrCyYa5WgU","expires":1060,"include_admin_default":true,"issued":1000,"kind":"float","limit":2,"platform_profile":"v1:10000000-0000-4000-8000-000000000046:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","position":{"application_id":"notes","logical_install_order":"18446744073709551615","panel_id":"preview"},"user":"10000000-0000-4000-8000-000000000046","v":1})";
constexpr std::string_view GOLDEN =
    "eyJjb250ZW50X3R5cGUiOiJ0ZXh0L3BsYWluIiwiY3JlZGVudGlhbCI6InNhbkFEdm5JdXVkWm"
    "pjd0xRSThiai0tTHpxYWlLUm9vTFlyQ3lZYTVXZ1UiLCJleHBpcmVzIjoxMDYwLCJpbmNsdWRl"
    "X2FkbWluX2RlZmF1bHQiOnRydWUsImlzc3VlZCI6MTAwMCwia2luZCI6ImZsb2F0IiwibGltaX"
    "QiOjIsInBsYXRmb3JtX3Byb2ZpbGUiOiJ2MToxMDAwMDAwMC0wMDAwLTQwMDAtODAwMC0wMDAw"
    "MDAwMDAwNDY6YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYm"
    "JiYmJiYmJiYmJiYmJiYmJiYiIsInBvc2l0aW9uIjp7ImFwcGxpY2F0aW9uX2lkIjoibm90ZXMi"
    "LCJsb2dpY2FsX2luc3RhbGxfb3JkZXIiOiIxODQ0Njc0NDA3MzcwOTU1MTYxNSIsInBhbmVsX2"
    "lkIjoicHJldmlldyJ9LCJ1c2VyIjoiMTAwMDAwMDAtMDAwMC00MDAwLTgwMDAtMDAwMDAwMDAw"
    "MDQ2IiwidiI6MX0.HOfXrIjVNmrLYvmzqDBPo1CiAfdIX2n5noJOOSVfkpk";

auto secret() -> std::array<unsigned char, 32> {
  std::array<unsigned char, 32> key{};
  for (std::size_t index = 0; index < key.size(); ++index)
    key[index] = static_cast<unsigned char>(index);
  return key;
}
auto owner() -> plinth::auth::AuthContext {
  return {.user_id = std::string{GENERATION},
          .username = "fixture",
          .auth_type = "session",
          .session_id = std::string{GENERATION},
          .pat_id = {},
          .token_hash = std::string(64, 'a'),
          .credential_source = plinth::auth::CredentialSource::COOKIE};
}
auto query() -> protocol::CursorQuery {
  return {.content_type = "text/plain",
          .limit = 2,
          .include_admin_default = true,
          .platform_profile =
              "v1:" + std::string{GENERATION} + ":" + std::string(64, 'b')};
}
auto position() -> plinth::frontend::authority::Position {
  return {"18446744073709551615", "notes", "preview"};
}
auto base64url(std::string_view raw) -> std::string {
  REQUIRE(raw.size() <= 4096);
  std::string result(4 * ((raw.size() + 2) / 3), '\0');
  const auto size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                      reinterpret_cast<const unsigned char*>(raw.data()),
                      static_cast<int>(raw.size()));
  REQUIRE(size == static_cast<int>(result.size()));
  std::ranges::replace(result, '+', '-');
  std::ranges::replace(result, '/', '_');
  while (!result.empty() && result.back() == '=')
    result.pop_back();
  return result;
}
// Independent EVP one-shot framing for malformed but authentic payload
// controls. GOLDEN above was computed separately with Python stdlib
// json/hmac/base64, not by production encoding or this test signer.
auto sign(std::string_view raw,
          std::string_view domain = "plinth.surface.cursor.v1") -> std::string {
  const auto bytes = secret();
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
      EVP_PKEY_new_raw_private_key(EVP_PKEY_HMAC, nullptr, bytes.data(),
                                   bytes.size()),
      EVP_PKEY_free};
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
      EVP_MD_CTX_new(), EVP_MD_CTX_free};
  REQUIRE(key);
  REQUIRE(context);
  REQUIRE(EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr,
                             key.get()) == 1);
  std::string framed{domain};
  framed.push_back('\0');
  framed.append(raw);
  std::array<unsigned char, 32> mac{};
  std::size_t size = mac.size();
  REQUIRE(EVP_DigestSign(context.get(), mac.data(), &size,
                         reinterpret_cast<const unsigned char*>(framed.data()),
                         framed.size()) == 1);
  REQUIRE(size == mac.size());
  return base64url(raw) + "." +
         base64url({reinterpret_cast<const char*>(mac.data()), mac.size()});
}

struct CaptureFixture {
  std::filesystem::path root;
  generation::Budget budget;
  std::shared_ptr<const generation::CapturedArtifact> artifact;
  plinth::frontend::authority::SurfaceTarget target{
      .generation = std::string{GENERATION},
      .application_id = "notes",
      .version = "1.0.0",
      .kind = plinth::frontend::SurfaceKind::FLOAT,
      .declaration = {},
      .logical_install_order = "18446744073709551615",
      .capability_rule = "notes.call",
      .provider_type = "extension",
      .provider_name = "notes",
      .capability_scope = "instance"};
  CaptureFixture() {
    std::array<char, 80> pattern{};
    std::ranges::copy(std::string_view{"/tmp/plinth-surface-protocol.XXXXXX"},
                      pattern.begin());
    const auto* temporary = ::mkdtemp(pattern.data());
    if (!temporary)
      throw std::runtime_error("surface protocol fixture directory");
    root = temporary;
    std::filesystem::create_directories(root / "client/panels");
    std::ofstream file{root / "client/panels/é%2e?#.js"};
    file << "export default function Fixture() {}\n";
    file.close();
    recapture();
    target.declaration.id = "preview";
    target.declaration.client_path = "é%2e?#.js";
    target.declaration.rbac_rule = "notes.panel";
    target.declaration.typed_declaration = true;
    target.declaration.type = plinth::packages::PanelType::FLOAT;
    target.declaration.capability = "notes:1:preview";
    target.declaration.content_types = {"text/plain"};
    target.declaration.context_schema = plinth::packages::PanelContextSchema{
        .version = 1,
        .fields = {{"record_id",
                    {.required = true,
                     .max_bytes = 64,
                     .meaning = "Stable fake ID"}}},
        .persist_keys = {"record_id"}};
  }
  auto recapture() -> void {
    auto captured = generation::capture_artifact(
        {.key = {.package_id = std::string{GENERATION},
                 .name = "notes",
                 .version = "1.0.0",
                 .profile = 1},
         .root = root,
         .deadline =
             std::chrono::steady_clock::now() + std::chrono::seconds{10},
         .cancellation = {}},
        budget);
    REQUIRE(captured);
    artifact = std::move(*captured);
  }
  ~CaptureFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  CaptureFixture(const CaptureFixture&) = delete;
  auto operator=(const CaptureFixture&) -> CaptureFixture& = delete;
};
} // namespace

TEST_CASE(
    "surface protocol: canonical asset paths float wire and bound cursors",
    "[frontend][surface_protocol]") {
  SECTION("canonical namespace decoder preserves literal data and decodes "
          "exactly once") {
    const auto typed = "/_plinth/typed/" + std::string{GENERATION} + "/" +
                       std::string(64, 'a') + "/preview/client/";
    const auto platform = "/_plinth/platform/" + std::string{GENERATION} + "/" +
                          std::string(64, 'a') + "/client/";
    auto parsed = protocol::parse_asset_selector(
        typed + "panels/sp%20ace/%C3%A9%252e%3F%23.js");
    REQUIRE(parsed);
    REQUIRE(parsed->kind == protocol::AssetKind::TYPED);
    REQUIRE(parsed->panel_id == "preview");
    REQUIRE(parsed->member_path == "client/panels/sp ace/é%2e?#.js");
    parsed = protocol::parse_asset_selector(platform + "sdk.js");
    REQUIRE(parsed);
    REQUIRE(parsed->kind == protocol::AssetKind::PLATFORM);
    REQUIRE_FALSE(parsed->panel_id);
    REQUIRE(parsed->member_path == "client/sdk.js");
    for (const auto& suffix :
         {"", ".", "..", "%2E", "%2e", "%2F", "%5C", "%00", "%FF", "%ED%A0%80",
          "%41", "a//b", "a/../b", "a?b", "a#b", "é.js", "x%", "x%3", "x%3g"}) {
      INFO(suffix);
      REQUIRE_FALSE(protocol::parse_asset_selector(typed + suffix));
    }
    REQUIRE(protocol::parse_asset_selector(typed + "%25/%252e/.hidden/a..b"));
    REQUIRE_FALSE(protocol::parse_asset_selector("https://example.test" +
                                                 typed + "x.js"));
    REQUIRE_FALSE(
        protocol::parse_asset_selector(platform + std::string(2048, 'x')));
    REQUIRE_FALSE(
        protocol::parse_asset_selector("/_plinth/typed/A" + typed.substr(16)));
  }
  SECTION("independent byte golden and exact sixty-second cursor boundary") {
    const auto codec = protocol::test_seam::cursor_codec(secret());
    const auto encoded = codec.encode(owner(), query(), position(), 1000);
    REQUIRE(encoded);
    REQUIRE(*encoded == GOLDEN);
    REQUIRE(sign(PAYLOAD) == GOLDEN);
    REQUIRE(encoded->find(owner().token_hash) == std::string::npos);
    REQUIRE(PAYLOAD.find(owner().token_hash) == std::string_view::npos);
    REQUIRE(codec.decode(*encoded, owner(), query(), 1000) == position());
    REQUIRE(codec.decode(*encoded, owner(), query(), 1059) == position());
    REQUIRE_FALSE(codec.decode(*encoded, owner(), query(), 999));
    REQUIRE_FALSE(codec.decode(*encoded, owner(), query(), 1060));
    REQUIRE_FALSE(codec.encode(owner(), query(), position(), -1));
    REQUIRE_FALSE(codec.encode(owner(), query(), position(),
                               std::numeric_limits<std::int64_t>::max() - 59));
    REQUIRE(codec.encode(owner(), query(), position(),
                         std::numeric_limits<std::int64_t>::max() - 60));
    for (const auto limit : {std::size_t{1}, std::size_t{64}}) {
      auto unfiltered = query();
      unfiltered.content_type.reset();
      unfiltered.include_admin_default = false;
      unfiltered.limit = limit;
      const auto cursor =
          codec.encode(owner(), unfiltered, {"1", "notes", "preview"}, 0);
      REQUIRE(cursor);
      const plinth::frontend::authority::Position start{"1", "notes",
                                                        "preview"};
      REQUIRE(codec.decode(*cursor, owner(), unfiltered, 59) == start);
    }
  }
  SECTION("wrong user credential source authkind filters profile and process "
          "reject without fallback") {
    const auto codec = protocol::test_seam::cursor_codec(secret());
    for (unsigned int variant = 0; variant < 4; ++variant) {
      auto auth = owner();
      if (variant == 0) auth.user_id[0] = '2';
      if (variant == 1) auth.token_hash[0] = 'b';
      if (variant == 2)
        auth.credential_source = plinth::auth::CredentialSource::BEARER;
      if (variant == 3) {
        auth.auth_type = "pat";
        auth.credential_source = plinth::auth::CredentialSource::BEARER;
      }
      REQUIRE_FALSE(codec.decode(GOLDEN, auth, query(), 1000));
    }
    for (unsigned int variant = 0; variant < 5; ++variant) {
      auto changed = query();
      if (variant == 0) changed.limit = 3;
      if (variant == 1) changed.content_type = "application/json";
      if (variant == 2) changed.include_admin_default = false;
      if (variant == 3) changed.platform_profile.back() = 'c';
      if (variant == 4) {
        changed.content_type.reset();
        changed.include_admin_default = false;
      }
      REQUIRE_FALSE(codec.decode(GOLDEN, owner(), changed, 1000));
    }
    auto other = secret();
    other[0] = 255;
    const auto restarted = protocol::test_seam::cursor_codec(other);
    REQUIRE_FALSE(restarted.decode(GOLDEN, owner(), query(), 1000));
    const protocol::CursorCodec random_first, random_second;
    const auto random_cursor =
        random_first.encode(owner(), query(), position(), 1000);
    REQUIRE(random_cursor);
    REQUIRE_FALSE(random_second.decode(*random_cursor, owner(), query(), 1000));
  }
  SECTION("primary and tray cursors bind kind and exact rank-free positions") {
    using enum plinth::frontend::SurfaceKind;
    const auto codec = protocol::test_seam::cursor_codec(secret());
    const plinth::frontend::authority::Position start{std::nullopt, "notes",
                                                      "preview"};
    for (const auto kind : {PRIMARY, TRAY}) {
      auto selected = query();
      selected.kind = kind;
      selected.content_type.reset();
      selected.include_admin_default = false;
      auto payload = ProtocolJson::parse(PAYLOAD);
      payload["kind"] = kind == PRIMARY ? "primary" : "tray";
      payload["content_type"] = nullptr;
      payload["include_admin_default"] = false;
      payload["position"].erase("logical_install_order");
      const auto encoded = codec.encode(owner(), selected, start, 1000);
      REQUIRE(encoded);
      // Expected bytes are independently framed/signed, not production encoded.
      REQUIRE(*encoded == sign(payload.dump()));
      REQUIRE(codec.decode(*encoded, owner(), selected, 1059) == start);
      REQUIRE_FALSE(codec.decode(*encoded, owner(), selected, 1060));
      auto changed = selected;
      changed.kind = kind == PRIMARY ? TRAY : PRIMARY;
      REQUIRE_FALSE(codec.decode(*encoded, owner(), changed, 1000));
      REQUIRE_FALSE(codec.decode(*encoded, owner(), query(), 1000));
      REQUIRE_FALSE(codec.encode(owner(), selected, position(), 1000));
      for (const auto& rank :
           {ProtocolJson(nullptr), ProtocolJson(""), ProtocolJson("1")}) {
        auto forged = payload;
        forged["position"]["logical_install_order"] = rank;
        REQUIRE_FALSE(
            codec.decode(sign(forged.dump()), owner(), selected, 1000));
      }
      for (const auto key : {"application_id", "panel_id"}) {
        auto forged = payload;
        forged["position"].erase(key);
        REQUIRE_FALSE(
            codec.decode(sign(forged.dump()), owner(), selected, 1000));
      }
      changed = selected;
      changed.content_type = "text/plain";
      REQUIRE_FALSE(codec.encode(owner(), changed, start, 1000));
      REQUIRE_FALSE(codec.decode(*encoded, owner(), changed, 1000));
      changed = selected;
      changed.include_admin_default = true;
      REQUIRE_FALSE(codec.encode(owner(), changed, start, 1000));
      changed = selected;
      changed.platform_profile.back() = 'c';
      REQUIRE_FALSE(codec.decode(*encoded, owner(), changed, 1000));
      changed = selected;
      changed.limit = 1;
      REQUIRE_FALSE(codec.decode(*encoded, owner(), changed, 1000));
      auto wrong_owner = owner();
      wrong_owner.token_hash = std::string(64, 'd');
      REQUIRE_FALSE(codec.decode(*encoded, wrong_owner, selected, 1000));
    }
    REQUIRE_FALSE(codec.encode(owner(), query(), start, 1000));
    auto invalid = query();
    invalid.kind = static_cast<plinth::frontend::SurfaceKind>(99);
    REQUIRE_FALSE(codec.encode(owner(), invalid, position(), 1000));
  }
  SECTION("primary and tray target shapes are closed and bound to captured "
          "members") {
    using enum plinth::frontend::SurfaceKind;
    CaptureFixture fixture;
    auto primary = fixture.target;
    primary.kind = PRIMARY;
    primary.logical_install_order.reset();
    primary.declaration.type = plinth::packages::PanelType::PRIMARY;
    primary.declaration.capability.reset();
    primary.declaration.content_types.clear();
    primary.declaration.accepts_navigation_intent = true;
    const auto encoded =
        protocol::encode_primary_target(primary, *fixture.artifact, ORIGIN);
    REQUIRE(encoded);
    const std::string expected_url =
        "/_plinth/typed/" + std::string{GENERATION} + "/" +
        fixture.artifact->digest() +
        "/preview/client/panels/%C3%A9%252e%3F%23.js";
    const ProtocolJson expected_primary{
        {"application_id", "notes"},
        {"panel_id", "preview"},
        {"type", "primary"},
        {"generation", GENERATION},
        {"version", "1.0.0"},
        {"title", "preview"},
        {"icon", "square"},
        {"module_url", expected_url},
        {"accepts_navigation_intent", true},
        {"context_schema",
         {{"version", 1},
          {"fields",
           {{"record_id",
             {{"required", true},
              {"max_bytes", 64},
              {"meaning", "Stable fake ID"}}}}},
          {"persist_keys", ProtocolJson::array({"record_id"})}}}};
    REQUIRE(*encoded == expected_primary);
    REQUIRE(encoded->size() == 10);
    auto tray = primary;
    tray.kind = TRAY;
    tray.declaration.type = plinth::packages::PanelType::TRAY;
    tray.declaration.accepts_navigation_intent = false;
    tray.declaration.context_schema.reset();
    tray.declaration.tray_states = {{"idle", "circle"}, {"busy", "clock"}};
    tray.declaration.tray_initial_state = "idle";
    tray.declaration.preferred_width = 320;
    tray.declaration.chrome_essential =
        true; // Foreign claim cannot set output.
    auto expected_tray = expected_primary;
    expected_tray.erase("accepts_navigation_intent");
    expected_tray.erase("context_schema");
    expected_tray["type"] = "tray";
    expected_tray["tray_states"] = {{"idle", "circle"}, {"busy", "clock"}};
    expected_tray["tray_initial_state"] = "idle";
    expected_tray["preferred_width"] = 320;
    expected_tray["chrome_essential"] = false;
    auto tray_wire =
        protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN);
    REQUIRE(tray_wire);
    REQUIRE(*tray_wire == expected_tray);
    REQUIRE(tray_wire->size() == 12);
    expected_tray["chrome_essential"] = true;
    tray_wire =
        protocol::encode_tray_target(tray, *fixture.artifact, true, ORIGIN);
    REQUIRE(tray_wire);
    REQUIRE(*tray_wire == expected_tray);
    tray.declaration.chrome_essential = false;
    tray_wire =
        protocol::encode_tray_target(tray, *fixture.artifact, true, ORIGIN);
    REQUIRE(tray_wire);
    REQUIRE(*tray_wire == expected_tray);
    // This tests pure serialization of a service-derived boolean, NOT authority
    // to become essential. Real active-shell proof belongs to the service.
    tray.declaration.context_schema = primary.declaration.context_schema;
    REQUIRE(
        protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN));
    auto invalid_schema = tray;
    invalid_schema.declaration.context_schema->fields["record_id"].max_bytes =
        513;
    REQUIRE_FALSE(protocol::encode_tray_target(
        invalid_schema, *fixture.artifact, false, ORIGIN));
    for (unsigned int variant = 0; variant < 12; ++variant) {
      auto forged = primary;
      if (variant == 0) forged.declaration.accepts_navigation_intent = false;
      if (variant == 1) forged.declaration.context_schema.reset();
      if (variant == 2) forged.logical_install_order = "1";
      if (variant == 3) forged.declaration.capability = "notes:1:preview";
      if (variant == 4) forged.declaration.content_types = {"text/plain"};
      if (variant == 5) forged.declaration.tray_states = {{"idle", "circle"}};
      if (variant == 6) forged.kind = TRAY;
      if (variant == 7)
        forged.declaration.type = plinth::packages::PanelType::TRAY;
      if (variant == 8) forged.generation[0] = '2';
      if (variant == 9) forged.declaration.client_path = "missing.js";
      if (variant == 10) forged.declaration.client_path = "../server/main.js";
      if (variant == 11) forged.version = "2.0.0";
      REQUIRE_FALSE(
          protocol::encode_primary_target(forged, *fixture.artifact, ORIGIN));
    }
    for (unsigned int variant = 0; variant < 12; ++variant) {
      auto forged = tray;
      if (variant == 0) forged.declaration.accepts_navigation_intent = true;
      if (variant == 1) forged.logical_install_order = "1";
      if (variant == 2) forged.declaration.capability = "notes:1:preview";
      if (variant == 3) forged.declaration.content_types = {"text/plain"};
      if (variant == 4) forged.declaration.tray_states.clear();
      if (variant == 5) forged.declaration.tray_initial_state.reset();
      if (variant == 6) forged.declaration.tray_initial_state = "missing";
      if (variant == 7) forged.declaration.preferred_width.reset();
      if (variant == 8) forged.declaration.preferred_width = 279;
      if (variant == 9) forged.declaration.preferred_width = 481;
      if (variant == 10) forged.declaration.tray_states["Bad State"] = "circle";
      if (variant == 11) forged.declaration.tray_states["idle"] = "Bad Icon";
      REQUIRE_FALSE(protocol::encode_tray_target(forged, *fixture.artifact,
                                                 false, ORIGIN));
    }
    for (const auto width : {280U, 480U}) {
      tray.declaration.preferred_width = width;
      REQUIRE(
          protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN));
    }
    tray.declaration.tray_states.clear();
    for (unsigned int index = 0; index < 16; ++index)
      tray.declaration.tray_states["state" + std::to_string(index)] = "circle";
    tray.declaration.tray_initial_state = "state0";
    REQUIRE(
        protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN));
    tray.declaration.tray_states["excess"] = "circle";
    REQUIRE_FALSE(
        protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN));
    std::string emoji;
    for (unsigned int index = 0; index < 256; ++index)
      emoji += "😀";
    for (const auto kind : {PRIMARY, TRAY}) {
      auto target = kind == PRIMARY ? primary : tray;
      if (kind == TRAY) target.declaration.tray_states.erase("excess");
      auto encode = [&] {
        return kind == PRIMARY
                   ? protocol::encode_primary_target(target, *fixture.artifact,
                                                     ORIGIN)
                   : protocol::encode_tray_target(target, *fixture.artifact,
                                                  false, ORIGIN);
      };
      target.declaration.title = emoji;
      REQUIRE(encode());
      target.declaration.title = emoji + "😀";
      REQUIRE_FALSE(encode());
      target.declaration.title = std::string(1, static_cast<char>(0xff));
      REQUIRE_FALSE(encode());
      target.declaration.title = "Valid title";
      target.declaration.icon = "Bad Icon";
      REQUIRE_FALSE(encode());
    }
    REQUIRE_FALSE(protocol::encode_primary_target(primary, *fixture.artifact,
                                                  "http://example.test/"));
    REQUIRE_FALSE(protocol::encode_tray_target(tray, *fixture.artifact, false,
                                               "https://user@example.test"));
  }
  SECTION("authentic malformed or noncanonical cursor bytes never reach a "
          "position") {
    const auto codec = protocol::test_seam::cursor_codec(secret());
    auto wrong_mac = std::string{GOLDEN};
    wrong_mac[wrong_mac.find('.') + 2] = 'A';
    auto padding_bits = std::string{GOLDEN};
    padding_bits[padding_bits.find('.') - 1] = '1';
    auto nested_alias = std::string{PAYLOAD};
    const std::string nested_key = "\"panel_id\":\"preview\"";
    const auto nested_position = nested_alias.find(nested_key);
    REQUIRE(nested_position != std::string::npos);
    nested_alias.replace(nested_position, nested_key.size(),
                         nested_key + ",\"panel_\\u0069d\":\"preview\"");
    for (const auto& raw :
         {std::string{}, std::string{GOLDEN} + "=", std::string{GOLDEN} + ".x",
          wrong_mac, padding_bits, sign(nested_alias),
          sign(std::string{PAYLOAD} + " "), sign(" " + std::string{PAYLOAD}),
          sign("{}"),
          sign(std::string{PAYLOAD}.substr(0, PAYLOAD.size() - 1) +
               ",\"v\":1}"),
          sign(std::string{PAYLOAD}.substr(0, PAYLOAD.size() - 1) +
               ",\"\\u0076\":1}"),
          sign(PAYLOAD, "plinth.surface.credential.v1"),
          std::string(4097, 'A')}) {
      REQUIRE_FALSE(codec.decode(raw, owner(), query(), 1000));
    }
    for (const auto key :
         {"v", "kind", "position", "issued", "expires", "credential", "user"}) {
      auto payload = ProtocolJson::parse(PAYLOAD);
      payload.erase(key);
      REQUIRE_FALSE(codec.decode(sign(payload.dump()), owner(), query(), 1000));
    }
    for (unsigned int variant = 0; variant < 12; ++variant) {
      auto payload = ProtocolJson::parse(PAYLOAD);
      if (variant == 0) payload["issued"] = -1;
      if (variant == 1) payload["issued"] = 1000.0;
      if (variant == 2) payload["expires"] = 1061;
      if (variant == 3) payload["position"]["logical_install_order"] = "01";
      if (variant == 4)
        payload["position"]["logical_install_order"] = "18446744073709551616";
      if (variant == 5) payload["position"]["application_id"] = "../notes";
      if (variant == 6) payload["position"]["panel_id"] = "../preview";
      if (variant == 7) payload["extra"] = true;
      if (variant == 8)
        payload["expires"] = std::numeric_limits<std::uint64_t>::max();
      if (variant == 9) payload["kind"] = "tray";
      if (variant == 10) payload["credential"] = ProtocolJson::object();
      if (variant == 11) payload["position"] = ProtocolJson::array();
      REQUIRE_FALSE(codec.decode(sign(payload.dump()), owner(), query(), 1000));
    }
  }
  SECTION("float target exact browser keys URL schema projection and byte "
          "profile") {
    CaptureFixture fixture;
    auto result = protocol::encode_float_target(
        fixture.target, *fixture.artifact, std::nullopt, ORIGIN);
    REQUIRE(result);
    REQUIRE(result->size() == 13);
    REQUIRE((*result)["title"] == "preview");
    REQUIRE((*result)["icon"] == "square");
    REQUIRE((*result)["persistence_contract"].is_null());
    const auto url = (*result)["module_url"].get<std::string>();
    REQUIRE(url == "/_plinth/typed/" + std::string{GENERATION} + "/" +
                       fixture.artifact->digest() +
                       "/preview/client/panels/%C3%A9%252e%3F%23.js");
    const auto selector = protocol::parse_asset_selector(url);
    REQUIRE(selector);
    REQUIRE(selector->member_path == "client/panels/é%2e?#.js");
    REQUIRE(fixture.artifact->find(selector->member_path));
    const ProtocolJson expected{
        {"application_id", "notes"},
        {"panel_id", "preview"},
        {"type", "float"},
        {"generation", GENERATION},
        {"version", "1.0.0"},
        {"title", "preview"},
        {"icon", "square"},
        {"module_url", url},
        {"capability", "notes:1:preview"},
        {"content_types", ProtocolJson::array({"text/plain"})},
        {"context_schema",
         {{"version", 1},
          {"fields",
           {{"record_id",
             {{"required", true},
              {"max_bytes", 64},
              {"meaning", "Stable fake ID"}}}}},
          {"persist_keys", ProtocolJson::array({"record_id"})}}},
        {"logical_install_order", "18446744073709551615"},
        {"persistence_contract", nullptr}};
    REQUIRE(*result == expected);
    const auto schema = plinth::packages::restore::schema_digest(
        *fixture.target.declaration.context_schema);
    REQUIRE(schema);
    const plinth::frontend::PersistenceProjection projection{
        .projection_contract = {.id = "record-v1", .version = 1},
        .artifact_digest = fixture.artifact->digest(),
        .schema_digest = *schema,
        .policy_revision = "18446744073709551615",
        .approved_keys = {"record_id"}};
    result = protocol::encode_float_target(fixture.target, *fixture.artifact,
                                           projection, ORIGIN);
    REQUIRE(result);
    REQUIRE((*result)["persistence_contract"]["approved_keys"] ==
            ProtocolJson::array({"record_id"}));
    REQUIRE_FALSE(result->contains("applicationTitle"));
    for (unsigned int variant = 0; variant < 9; ++variant) {
      auto forged = projection;
      if (variant == 0)
        forged.artifact_digest[0] =
            forged.artifact_digest[0] == 'a' ? 'b' : 'a';
      if (variant == 1)
        forged.schema_digest[0] = forged.schema_digest[0] == 'a' ? 'b' : 'a';
      if (variant == 2) forged.approved_keys.clear();
      if (variant == 3) forged.approved_keys.push_back("record_id");
      if (variant == 4) forged.approved_keys = {"unknown"};
      if (variant == 5) forged.policy_revision = "0";
      if (variant == 6)
        forged.projection_contract.version = 9007199254740992ULL;
      if (variant == 7) forged.projection_contract.id = "../record";
      if (variant == 8) forged.projection_contract.version = 0;
      REQUIRE_FALSE(protocol::encode_float_target(
          fixture.target, *fixture.artifact, forged, ORIGIN));
    }
    for (unsigned int variant = 0; variant < 12; ++variant) {
      auto forged = fixture.target;
      if (variant == 0) forged.generation[0] = '2';
      if (variant == 1) forged.kind = plinth::frontend::SurfaceKind::TRAY;
      if (variant == 2) forged.declaration.typed_declaration = false;
      if (variant == 3) forged.version = "2.0.0";
      if (variant == 4) forged.declaration.capability = "notes:01:preview";
      if (variant == 5) forged.declaration.content_types = {"Text/Plain"};
      if (variant == 6)
        forged.declaration.content_types = {"text/plain", "text/plain"};
      if (variant == 7) forged.declaration.client_path = "missing.js";
      if (variant == 8) forged.declaration.client_path = "../server/main.js";
      if (variant == 9) forged.logical_install_order = "0";
      if (variant == 10)
        forged.declaration.context_schema->fields["record_id"].max_bytes = 513;
      if (variant == 11) forged.declaration.icon = "Bad Icon";
      REQUIRE_FALSE(protocol::encode_float_target(forged, *fixture.artifact,
                                                  std::nullopt, ORIGIN));
    }
    fixture.target.declaration.title = std::string(256, 'x');
    REQUIRE(protocol::encode_float_target(fixture.target, *fixture.artifact,
                                          std::nullopt, ORIGIN));
    fixture.target.declaration.title = std::string(257, 'x');
    REQUIRE_FALSE(protocol::encode_float_target(
        fixture.target, *fixture.artifact, std::nullopt, ORIGIN));
    fixture.target.declaration.title = "Valid title";
    REQUIRE_FALSE(protocol::encode_float_target(fixture.target,
                                                *fixture.artifact, std::nullopt,
                                                "http://example.test/"));
    std::string emoji;
    for (unsigned int index = 0; index < 256; ++index)
      emoji += "😀";
    fixture.target.declaration.title = emoji;
    REQUIRE(protocol::encode_float_target(fixture.target, *fixture.artifact,
                                          std::nullopt, ORIGIN));
    fixture.target.declaration.title = emoji + "😀";
    REQUIRE_FALSE(protocol::encode_float_target(
        fixture.target, *fixture.artifact, std::nullopt, ORIGIN));
    fixture.target.declaration.title = std::string(1, static_cast<char>(0xff));
    REQUIRE_FALSE(protocol::encode_float_target(
        fixture.target, *fixture.artifact, std::nullopt, ORIGIN));
  }
  SECTION("full normalized href byte boundary includes trusted origin not just "
          "relative path") {
    CaptureFixture fixture;
    const auto prefix = "/_plinth/typed/" + std::string{GENERATION} + "/" +
                        fixture.artifact->digest() + "/preview/client/panels/";
    std::string folders;
    for (unsigned int index = 0; index < 4; ++index)
      folders += std::string(150, ' ') + "/";
    const auto fixed_bytes = prefix.size() + std::size_t{4} * 451 + 3;
    REQUIRE(fixed_bytes < 2048 - ORIGIN.size());
    const auto leaf =
        std::string(2048 - ORIGIN.size() - fixed_bytes, 'x') + ".js";
    REQUIRE(leaf.size() + 1 <= 255);
    const auto path = folders + leaf;
    std::filesystem::create_directories(fixture.root / "client/panels" /
                                        folders);
    for (const auto& member : {path, path + "x"}) {
      std::ofstream file{fixture.root / "client/panels" / member};
      file << "export default {};\n";
      REQUIRE(file.good());
    }
    fixture.recapture();
    fixture.target.declaration.client_path = path;
    const auto exact = protocol::encode_float_target(
        fixture.target, *fixture.artifact, std::nullopt, ORIGIN);
    REQUIRE(exact);
    REQUIRE(ORIGIN.size() +
                (*exact)["module_url"].get_ref<const std::string&>().size() ==
            2048);
    auto nonfloat = fixture.target;
    nonfloat.logical_install_order.reset();
    nonfloat.declaration.capability.reset();
    nonfloat.declaration.content_types.clear();
    nonfloat.kind = plinth::frontend::SurfaceKind::PRIMARY;
    nonfloat.declaration.type = plinth::packages::PanelType::PRIMARY;
    nonfloat.declaration.accepts_navigation_intent = true;
    REQUIRE(
        protocol::encode_primary_target(nonfloat, *fixture.artifact, ORIGIN));
    auto tray = nonfloat;
    tray.kind = plinth::frontend::SurfaceKind::TRAY;
    tray.declaration.type = plinth::packages::PanelType::TRAY;
    tray.declaration.accepts_navigation_intent = false;
    tray.declaration.tray_states = {{"idle", "circle"}};
    tray.declaration.tray_initial_state = "idle";
    tray.declaration.preferred_width = 320;
    REQUIRE(
        protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN));
    nonfloat.declaration.client_path = path + "x";
    tray.declaration.client_path = path + "x";
    REQUIRE_FALSE(
        protocol::encode_primary_target(nonfloat, *fixture.artifact, ORIGIN));
    REQUIRE_FALSE(
        protocol::encode_tray_target(tray, *fixture.artifact, false, ORIGIN));
    fixture.target.declaration.client_path = path + "x";
    REQUIRE_FALSE(protocol::encode_float_target(
        fixture.target, *fixture.artifact, std::nullopt, ORIGIN));
  }
}
