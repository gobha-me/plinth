#include "kernel/frontend/admin_defaults_contract.hpp"

#include "kernel/frontend/surface_protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
namespace admin = plinth::frontend::admin_defaults;
namespace protocol = plinth::frontend::protocol;
using AdminJson = nlohmann::json;
constexpr std::string_view USER = "10000000-0000-4000-8000-000000000046";
// Independently computed with Python stdlib json(sort_keys,separators),
// hmac.sha256 and base64.urlsafe_b64encode, not the production codec/signer.
constexpr std::string_view PAYLOAD =
    R"({"after":"text/plain","credential":"aU8t7ANB_fIOdAVVM2G5uF5eF9LqZryfCh0aw265nnc","expires":1060,"issued":1000,"kind":"admin_defaults","limit":2,"platform_profile":"v1:10000000-0000-4000-8000-000000000046:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","user":"10000000-0000-4000-8000-000000000046","v":1})";
constexpr std::string_view GOLDEN = "eyJhZnRlciI6InRleHQvcGxhaW4iLCJjcmVkZW50aW"
                                    "FsIjoiYVU4dDdBTkJfZklPZEFWVk0yRzV1"
                                    "RjVlRjlMcVpyeWZDaDBhdzI2NW5uYyIsImV4cGlyZX"
                                    "MiOjEwNjAsImlzc3VlZCI6MTAwMCwia2lu"
                                    "ZCI6ImFkbWluX2RlZmF1bHRzIiwibGltaXQiOjIsIn"
                                    "BsYXRmb3JtX3Byb2ZpbGUiOiJ2MToxMDAw"
                                    "MDAwMC0wMDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwND"
                                    "Y6YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJi"
                                    "YmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYm"
                                    "JiYmJiYmJiYiIsInVzZXIiOiIxMDAwMDAwMC0w"
                                    "MDAwLTQwMDAtODAwMC0wMDAwMDAwMDAwNDYiLCJ2Ij"
                                    "oxfQ.G5MhofeSt0BliydB_G-ZFU_3klOx-"
                                    "HUbP-ZBda7Xs1M";

auto handler(std::string_view app = "uninstalled-app",
             std::string_view panel = "old_panel") -> AdminJson {
  return {{"application_id", app}, {"panel_id", panel}};
}
auto put() -> AdminJson {
  return {{"schema_version", 1},
          {"content_type", "text/plain"},
          {"handler", handler()}};
}
auto deletion() -> AdminJson {
  return {{"schema_version", 1}, {"content_type", "text/plain"}};
}
auto row(std::string_view mime = "text/plain") -> AdminJson {
  return {{"content_type", mime},
          {"application_id", "uninstalled-app"},
          {"panel_id", "old_panel"}};
}
auto page(AdminJson entries, AdminJson next = nullptr) -> AdminJson {
  return {{"entries", std::move(entries)}, {"next_after", std::move(next)}};
}
template <typename Result>
auto refuse(const Result& result, admin::Error error = admin::Error::INVALID)
    -> void {
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == error);
}
auto owner() -> plinth::auth::AuthContext {
  return {.user_id = std::string{USER},
          .username = "fixture",
          .auth_type = "session",
          .session_id = std::string{USER},
          .pat_id = {},
          .token_hash = std::string(64, 'a'),
          .credential_source = plinth::auth::CredentialSource::COOKIE};
}
auto binding() -> admin::CursorBinding {
  return {.limit = 2,
          .platform_profile =
              "v1:" + std::string{USER} + ":" + std::string(64, 'b')};
}
auto secret() -> std::array<unsigned char, 32> {
  std::array<unsigned char, 32> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<unsigned char>(index);
  return bytes;
}
auto b64(std::string_view bytes) -> std::string {
  REQUIRE(bytes.size() <= 4096);
  std::string result(4 * ((bytes.size() + 2) / 3), '\0');
  const auto size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                      reinterpret_cast<const unsigned char*>(bytes.data()),
                      static_cast<int>(bytes.size()));
  REQUIRE(size == static_cast<int>(result.size()));
  std::ranges::replace(result, '+', '-');
  std::ranges::replace(result, '/', '_');
  while (!result.empty() && result.back() == '=')
    result.pop_back();
  return result;
}
// Independent one-shot OpenSSL framing permits authentic malformed payload
// controls; it does not share production canonicalization/base64 code.
auto sign(std::string_view raw,
          std::string_view domain = "plinth.admin-defaults.cursor.v1")
    -> std::string {
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
  return b64(raw) + "." +
         b64({reinterpret_cast<const char*>(mac.data()), mac.size()});
}
} // namespace

TEST_CASE("admin defaults: closed raw and native requests",
          "[frontend][admin_defaults]") {
  SECTION("single and page are disjoint with one decoded query pass") {
    const auto empty = admin::parse_read_query("");
    REQUIRE(empty);
    REQUIRE(std::get<admin::ReadPage>(*empty) == admin::ReadPage{});
    const auto single = admin::parse_read_query(
        "content_%74ype=application%2Fvnd.notes%2Bjson");
    REQUIRE(single);
    REQUIRE(std::get<admin::ReadSingle>(*single).content_type ==
            "application/vnd.notes+json");
    for (const auto limit : {"1", "64"}) {
      const auto parsed = admin::parse_read_query(
          "limit=" + std::string{limit} + "&cursor=opaque.token");
      REQUIRE(parsed);
      const auto& value = std::get<admin::ReadPage>(*parsed);
      REQUIRE(value.limit == (limit == std::string_view{"1"} ? 1 : 64));
      REQUIRE(value.cursor == "opaque.token");
      REQUIRE(admin::validate_read(*parsed));
    }
    const auto cursor_only = admin::parse_read_query("cursor=opaque.token");
    REQUIRE(cursor_only);
    REQUIRE(std::get<admin::ReadPage>(*cursor_only).limit == 64);
    for (const auto raw : {"content_type=text/plain&limit=1",
                           "content_type=text/plain&cursor=a.b",
                           "limit=1&limit=2",
                           "l%69mit=1&limit=2",
                           "cursor=a.b&curs%6fr=c.d",
                           "content_type=text/plain&content_%74ype=text/plain",
                           "operation=admin_default_read",
                           "user_id=someone",
                           "after=text/plain",
                           "kind=float",
                           "depth=0",
                           "schema_version=1",
                           "limit=1&",
                           "&limit=1",
                           "=x",
                           "limit",
                           "limit=1=2",
                           "?limit=1",
                           "limit=%",
                           "limit=%0G",
                           "content_type=text%2500/plain",
                           "content_type=text/plain%00",
                           "content_type=text/plain%0A",
                           "content_type=text%2Fplain%7f",
                           "cursor=a%0Ab",
                           "cursor=%FF",
                           "content_type=TEXT/plain",
                           "content_type=text/*",
                           "content_type=",
                           "cursor=",
                           "cursor=a/b",
                           "cursor=a+b",
                           "limit=0",
                           "limit=65",
                           "limit=01",
                           "limit=-1",
                           "limit=+1",
                           "limit=1.0",
                           "limit=1e0",
                           "limit=true",
                           "limit=%221%22",
                           "limit=18446744073709551616"})
      refuse(admin::parse_read_query(raw));
    const auto maximum_cursor =
        admin::parse_read_query("cursor=" + std::string(4096, 'A'));
    REQUIRE(maximum_cursor);
    refuse(admin::parse_read_query("cursor=" + std::string(4097, 'A')));
    refuse(admin::parse_read_query(std::string(16384, 'A')));
    refuse(admin::parse_read_query(std::string(16385, 'A')),
           admin::Error::TOO_LARGE);
    refuse(admin::parse_read_query(std::string{"cursor=a\0b", 10}));
    refuse(admin::validate_read(admin::ReadSingle{"text/plain\n"}));
    refuse(admin::validate_read(admin::ReadPage{0, std::nullopt}));
    refuse(admin::validate_read(admin::ReadPage{65, std::nullopt}));
    refuse(admin::validate_read(admin::ReadPage{64, std::string(4097, 'A')}));
  }
  SECTION("exact mutations keep retained logical uninstalled choices") {
    const auto parsed = admin::parse_put(put().dump());
    REQUIRE(parsed);
    REQUIRE(*parsed ==
            admin::PutRequest{"text/plain", {"uninstalled-app", "old_panel"}});
    REQUIRE(admin::validate_put(*parsed));
    const auto removed = admin::parse_delete(deletion().dump());
    REQUIRE(removed);
    REQUIRE(removed->content_type == "text/plain");
    REQUIRE(admin::validate_delete(*removed));
    for (const auto& value : {AdminJson(nullptr), AdminJson::array(),
                              AdminJson(true), AdminJson(1), AdminJson("1")}) {
      refuse(admin::parse_put(value.dump()));
      refuse(admin::parse_delete(value.dump()));
    }
    for (const auto key : {"schema_version", "content_type", "handler"}) {
      auto value = put();
      value.erase(key);
      refuse(admin::parse_put(value.dump()));
    }
    for (const auto key : {"schema_version", "content_type"}) {
      auto value = deletion();
      value.erase(key);
      refuse(admin::parse_delete(value.dump()));
    }
    for (const auto& version :
         {AdminJson(1.0), AdminJson(1.1), AdminJson(0), AdminJson(2),
          AdminJson(true), AdminJson("1"), AdminJson(nullptr)}) {
      auto value = put();
      value["schema_version"] = version;
      refuse(admin::parse_put(value.dump()));
      value.erase("handler");
      refuse(admin::parse_delete(value.dump()));
    }
    for (const auto key : {"operation", "op", "user_id", "depth", "module",
                           "sql", "version", "generation", "capability",
                           "context", "authorized", "platform_profile"}) {
      auto value = put();
      value[key] = "forged";
      refuse(admin::parse_put(value.dump()));
      value.erase("handler");
      refuse(admin::parse_delete(value.dump()));
    }
    auto value = deletion();
    value["handler"] = handler();
    refuse(admin::parse_delete(value.dump()));
    value = put();
    value["handler"]["permission"] = true;
    refuse(admin::parse_put(value.dump()));
    for (const auto& bad_handler :
         {AdminJson(nullptr), AdminJson::array(), AdminJson("notes"),
          AdminJson{{"application_id", "notes"}},
          AdminJson{{"panel_id", "preview"}}}) {
      value = put();
      value["handler"] = bad_handler;
      refuse(admin::parse_put(value.dump()));
    }
  }
  SECTION("exact native full-string identifier and MIME boundaries") {
    for (const auto& app :
         {std::string{"aa"}, std::string{"a0-"}, std::string(64, 'a')}) {
      for (const auto& panel :
           {std::string{"a"}, std::string{"a_0-"}, std::string(64, 'a')}) {
        auto value = put();
        value["handler"] = handler(app, panel);
        REQUIRE(admin::parse_put(value.dump()));
        REQUIRE(admin::validate_put({"text/plain", {app, panel}}));
      }
    }
    for (const auto& app :
         {std::string{}, std::string{"a"}, std::string{"Notes"},
          std::string{"0notes"}, std::string{"notes_thing"},
          std::string{"notes\n"}, std::string{"notes\0", 6},
          std::string{"nötes"}, std::string(65, 'a')}) {
      auto value = put();
      value["handler"]["application_id"] = app;
      refuse(admin::parse_put(value.dump()));
      refuse(admin::validate_put({"text/plain", {app, "preview"}}));
    }
    for (const auto& panel :
         {std::string{}, std::string{"Preview"}, std::string{"_preview"},
          std::string{"1preview"}, std::string{"p/"}, std::string{"p\n"},
          std::string{"p\0", 2}, std::string(65, 'a')}) {
      auto value = put();
      value["handler"]["panel_id"] = panel;
      refuse(admin::parse_put(value.dump()));
      refuse(admin::validate_put({"text/plain", {"notes", panel}}));
    }
    for (const auto& type :
         {std::string{"a/0"}, std::string{"application/vnd.a+json"},
          "a/" + std::string(126, 'b')}) {
      auto value = put();
      value["content_type"] = type;
      REQUIRE(admin::parse_put(value.dump()));
      value.erase("handler");
      REQUIRE(admin::parse_delete(value.dump()));
      REQUIRE(plinth::frontend::is_canonical_content_type(type));
    }
    for (const auto& type :
         {std::string{}, std::string{"text/plain\n"},
          std::string{"text/plain\0", 11}, std::string{"TEXT/plain"},
          std::string{"text/*"}, std::string{"a/.b"}, std::string{"a/b;c=d"},
          "a/" + std::string(127, 'b')}) {
      auto value = put();
      value["content_type"] = type;
      refuse(admin::parse_put(value.dump()));
      value.erase("handler");
      refuse(admin::parse_delete(value.dump()));
      refuse(admin::validate_delete({type}));
      REQUIRE_FALSE(plinth::frontend::is_canonical_content_type(type));
    }
  }
  SECTION(
      "bounded SAX refuses decoded aliases structure UTF8 and trailing data") {
    for (
        const auto raw :
        {R"({"schema_version":1,"schema_versio\u006e":1,"content_type":"text/plain"})",
         R"({"schema_version":1,"content_type":"text/plain","content_\u0074ype":"text/plain"})",
         R"({"schema_version":1,"content_type":"text/plain","handler":{"application_id":"notes","application_\u0069d":"notes","panel_id":"p"}})",
         R"({"schema_version":1,"content_type":"text/plain","handler":{"application_id":"notes","panel_id":"p","panel_\u0069d":"p"}})",
         R"({"schema_version":1,"content_type":"text/plain","handler":{"application_id":"notes","panel_id":"p","extra":{"nested":{"deep":1}}}})",
         R"({"schema_version":1,"content_type":"text/plain"}false)",
         R"({"schema_version":1e0,"content_type":"text/plain"})",
         R"({"schema_version":1,"content_type":"text/plain\u0000"})"}) {
      refuse(admin::parse_put(raw));
      refuse(admin::parse_delete(raw));
    }
    std::string too_many = R"({"schema_version":1,"content_type":"text/plain")";
    for (int index = 0; index < 40; ++index)
      too_many += ",\"x" + std::to_string(index) + "\":0";
    too_many += '}';
    REQUIRE(too_many.size() < 2048);
    refuse(admin::parse_delete(too_many));
    std::string bad_utf8 = deletion().dump();
    bad_utf8.insert(2, 1, '\xff');
    refuse(admin::parse_delete(bad_utf8));
    for (const bool writing : {false, true}) {
      std::string raw = writing ? put().dump() : deletion().dump();
      raw.append(2048 - raw.size(), ' ');
      if (writing)
        REQUIRE(admin::parse_put(raw));
      else
        REQUIRE(admin::parse_delete(raw));
      raw.push_back(' ');
      if (writing)
        refuse(admin::parse_put(raw), admin::Error::TOO_LARGE);
      else
        refuse(admin::parse_delete(raw), admin::Error::TOO_LARGE);
      raw.back() = '\0';
      if (writing)
        refuse(admin::parse_put(raw), admin::Error::TOO_LARGE);
      else
        refuse(admin::parse_delete(raw), admin::Error::TOO_LARGE);
    }
    auto nul = put().dump();
    nul.push_back('\0');
    refuse(admin::parse_put(nul));
  }
  SECTION("actual shared operational boundary classifies allocation without "
          "retries") {
    using Result = std::expected<admin::PutRequest, admin::Error>;
    unsigned int calls = 0;
    const auto allocation =
        plinth::frontend::typed_request_detail::operational_boundary(
            [&]() -> Result {
              ++calls;
              throw std::bad_alloc{};
            });
    refuse(allocation, admin::Error::UNAVAILABLE);
    const auto length =
        plinth::frontend::typed_request_detail::operational_boundary(
            [&]() -> Result {
              ++calls;
              throw std::length_error{"controlled parser length pressure"};
            });
    refuse(length, admin::Error::UNAVAILABLE);
    REQUIRE(calls == 2);
    REQUIRE_THROWS_AS(
        plinth::frontend::typed_request_detail::operational_boundary(
            []() -> Result { throw std::runtime_error{"unrelated failure"}; }),
        std::runtime_error);
    // Deterministic production-boundary classification, not physical OOM or
    // evidence of an executed service/fixed-dispatch path.
  }
}

TEST_CASE("admin defaults: closed fixed module results and schema envelopes",
          "[frontend][admin_defaults]") {
  SECTION("single null means actual storage absence and logical rows survive") {
    const auto absent = admin::parse_single_result(R"({"candidate":null})");
    REQUIRE(absent);
    REQUIRE_FALSE(absent->handler);
    REQUIRE(admin::encode_single_result("text/plain", *absent) ==
            AdminJson{{"schema_version", 1},
                      {"content_type", "text/plain"},
                      {"handler", nullptr}});
    const auto present =
        admin::parse_single_result(AdminJson{{"candidate", handler()}}.dump());
    REQUIRE(present);
    REQUIRE(present->handler == admin::Handler{"uninstalled-app", "old_panel"});
    REQUIRE(admin::encode_single_result("text/plain", *present) ==
            AdminJson{{"schema_version", 1},
                      {"content_type", "text/plain"},
                      {"handler", handler()}});
    for (const auto& value :
         {AdminJson::object(), AdminJson::array(),
          AdminJson{{"candidate", false}},
          AdminJson{{"candidate", handler()}, {"extra", 0}},
          AdminJson{{"candidate", row()}}, AdminJson{{"handler", handler()}}})
      refuse(admin::parse_single_result(value.dump()),
             admin::Error::UNAVAILABLE);
    refuse(admin::parse_single_result(
               R"({"candidate":null,"candid\u0061te":null})"),
           admin::Error::UNAVAILABLE);
    refuse(admin::encode_single_result("text/plain\n", *present),
           admin::Error::UNAVAILABLE);
    refuse(
        admin::encode_single_result(
            "text/plain", admin::SingleResult{admin::Handler{"bad_app", "p"}}),
        admin::Error::UNAVAILABLE);
  }
  SECTION("put requires literal true while false delete is honest successful "
          "absence") {
    const auto saved = admin::parse_put_result(R"({"saved":true})");
    REQUIRE(saved);
    REQUIRE(admin::encode_put_result(*saved) ==
            AdminJson{{"schema_version", 1}, {"saved", true}});
    refuse(admin::encode_put_result({false}), admin::Error::UNAVAILABLE);
    for (const bool flag : {false, true}) {
      const auto deleted =
          admin::parse_delete_result(AdminJson{{"deleted", flag}}.dump());
      REQUIRE(deleted);
      REQUIRE(deleted->deleted == flag);
      REQUIRE(admin::encode_delete_result(*deleted) ==
              AdminJson{{"schema_version", 1}, {"deleted", flag}});
    }
    for (const auto& value :
         {AdminJson::object(), AdminJson::array(), AdminJson{{"saved", false}},
          AdminJson{{"saved", 1}}, AdminJson{{"saved", "true"}},
          AdminJson{{"saved", true}, {"deleted", true}}})
      refuse(admin::parse_put_result(value.dump()), admin::Error::UNAVAILABLE);
    for (const auto& value :
         {AdminJson::object(), AdminJson{{"deleted", 0}},
          AdminJson{{"deleted", "false"}}, AdminJson{{"deleted", nullptr}},
          AdminJson{{"deleted", false}, {"schema_version", 1}}})
      refuse(admin::parse_delete_result(value.dump()),
             admin::Error::UNAVAILABLE);
    refuse(admin::parse_put_result(R"({"saved":true,"sav\u0065d":true})"),
           admin::Error::UNAVAILABLE);
    refuse(
        admin::parse_delete_result(R"({"deleted":false,"delet\u0065d":true})"),
        admin::Error::UNAVAILABLE);
  }
  SECTION("page validates every flat row and maps to nested handlers") {
    const auto empty =
        admin::parse_page_result(page(AdminJson::array()).dump(), 64);
    REQUIRE(empty);
    REQUIRE(admin::encode_page_result(*empty) ==
            AdminJson{{"schema_version", 1},
                      {"entries", AdminJson::array()},
                      {"next_cursor", nullptr}});
    const auto rows =
        AdminJson::array({row("application/a"), row("text/plain")});
    const auto parsed =
        admin::parse_page_result(page(rows, "text/plain").dump(), 2);
    REQUIRE(parsed);
    REQUIRE(parsed->entries.size() == 2);
    REQUIRE(parsed->next_after == "text/plain");
    const auto encoded = admin::encode_page_result(*parsed, GOLDEN);
    REQUIRE(encoded);
    REQUIRE(*encoded ==
            AdminJson{
                {"schema_version", 1},
                {"entries",
                 AdminJson::array({AdminJson{{"content_type", "application/a"},
                                             {"handler", handler()}},
                                   AdminJson{{"content_type", "text/plain"},
                                             {"handler", handler()}}})},
                {"next_cursor", GOLDEN}});
    REQUIRE_FALSE(encoded->contains("next_after"));
    refuse(admin::encode_page_result(*parsed), admin::Error::UNAVAILABLE);
    refuse(admin::encode_page_result(*parsed, "text/plain"),
           admin::Error::UNAVAILABLE);
    refuse(admin::encode_page_result(*empty, GOLDEN),
           admin::Error::UNAVAILABLE);
    REQUIRE(admin::parse_page_result(page(rows).dump(), 2));
    refuse(admin::parse_page_result(page(rows, "text/plain").dump(), 3),
           admin::Error::UNAVAILABLE);
    refuse(admin::parse_page_result(page(rows).dump(), 1),
           admin::Error::UNAVAILABLE);
    refuse(admin::parse_page_result(page(rows).dump(), 0),
           admin::Error::UNAVAILABLE);
    refuse(admin::parse_page_result(page(rows).dump(), 65),
           admin::Error::UNAVAILABLE);
    REQUIRE(admin::parse_page_result(page(rows).dump(), 2, "application/0"));
    refuse(admin::parse_page_result(page(rows).dump(), 2, "application/a"),
           admin::Error::UNAVAILABLE);
    refuse(admin::parse_page_result(page(rows).dump(), 2, "text/plain"),
           admin::Error::UNAVAILABLE);
    refuse(admin::parse_page_result(page(rows).dump(), 2, "TEXT/plain"),
           admin::Error::UNAVAILABLE);
    refuse(admin::parse_page_result(page(rows).dump(), 2, ""),
           admin::Error::UNAVAILABLE);
    for (const auto& bad :
         {page(AdminJson::array({row("text/plain"), row("application/a")})),
          page(AdminJson::array({row(), row()})), page(rows, "application/a"),
          page(rows, "text/zz"), page(rows, false),
          page(AdminJson::array(), "text/plain"), AdminJson{{"entries", rows}},
          AdminJson{{"entries", rows}, {"next_cursor", nullptr}},
          AdminJson{
              {"entries", rows}, {"next_after", nullptr}, {"extra", true}}})
      refuse(admin::parse_page_result(bad.dump(), 2),
             admin::Error::UNAVAILABLE);
    for (unsigned int variant = 0; variant < 9; ++variant) {
      auto malformed = row("text/plain");
      if (variant == 0) malformed["application_id"] = "Notes";
      if (variant == 1) malformed["panel_id"] = "preview\n";
      if (variant == 2) malformed["content_type"] = "text/plain\n";
      if (variant == 3) malformed["content_type"] = "TEXT/plain";
      if (variant == 4) malformed["application_id"] = nullptr;
      if (variant == 5) malformed["handler"] = handler();
      if (variant == 6) malformed.erase("panel_id");
      if (variant == 7) malformed = nullptr;
      if (variant == 8) malformed = AdminJson::array();
      // A valid first row cannot hide a later malformed row.
      refuse(
          admin::parse_page_result(
              page(AdminJson::array({row("application/a"), malformed})).dump(),
              2),
          admin::Error::UNAVAILABLE);
    }
    refuse(
        admin::parse_page_result(
            R"({"entries":[{"content_type":"text/plain","application_id":"notes","panel_id":"p","panel_\u0069d":"p"}],"next_after":null})",
            2),
        admin::Error::UNAVAILABLE);
  }
  SECTION("ASCII byte order maximum page and total response caps") {
    const auto punctuation =
        AdminJson::array({row("a/a!"), row("a/a+"), row("a/a-"), row("a/a."),
                          row("a/a0"), row("a/a_")});
    REQUIRE(admin::parse_page_result(page(punctuation).dump(), 64));
    auto rows = AdminJson::array();
    for (int index = 0; index < 64; ++index)
      rows.push_back(row("application/x-" + std::to_string(index + 100)));
    const auto maximum =
        admin::parse_page_result(page(rows, "application/x-163").dump(), 64);
    REQUIRE(maximum);
    REQUIRE(maximum->entries.size() == 64);
    const auto encoded = admin::encode_page_result(*maximum, GOLDEN);
    REQUIRE(encoded);
    REQUIRE(encoded->dump().size() <= admin::MAX_RESPONSE_BYTES);
    rows.push_back(row("application/x-164"));
    refuse(admin::parse_page_result(page(rows).dump(), 64),
           admin::Error::UNAVAILABLE);
    auto padded = std::string{R"({"candidate":null})"};
    padded.append(admin::MAX_RESPONSE_BYTES - padded.size(), ' ');
    REQUIRE(admin::parse_single_result(padded));
    padded.push_back(' ');
    refuse(admin::parse_single_result(padded), admin::Error::UNAVAILABLE);
    for (const auto& raw :
         {std::string{""}, std::string{"{}garbage"}, std::string{"{\xff}"},
          std::string{"{}\0", 3}, std::string(131073, ' ')}) {
      refuse(admin::parse_single_result(raw), admin::Error::UNAVAILABLE);
      refuse(admin::parse_page_result(raw, 64), admin::Error::UNAVAILABLE);
      refuse(admin::parse_put_result(raw), admin::Error::UNAVAILABLE);
      refuse(admin::parse_delete_result(raw), admin::Error::UNAVAILABLE);
    }
  }
}

TEST_CASE("admin defaults: independent bounded authenticated cursor domain",
          "[frontend][admin_defaults]") {
  const auto codec = admin::test_seam::cursor_codec(secret());
  SECTION("independent exact byte vector and sixty-second lifetime") {
    const auto encoded = codec.encode(owner(), binding(), "text/plain", 1000);
    REQUIRE(encoded);
    REQUIRE(*encoded == GOLDEN);
    REQUIRE(sign(PAYLOAD) == GOLDEN);
    REQUIRE(encoded->size() <= 4096);
    REQUIRE(std::string{PAYLOAD}.find(owner().token_hash) == std::string::npos);
    REQUIRE(codec.decode(GOLDEN, owner(), binding(), 1000) == "text/plain");
    REQUIRE(codec.decode(GOLDEN, owner(), binding(), 1059) == "text/plain");
    refuse(codec.decode(GOLDEN, owner(), binding(), 999));
    refuse(codec.decode(GOLDEN, owner(), binding(), 1060));
    refuse(codec.decode(GOLDEN, owner(), binding(), -1));
    for (const auto now :
         {std::int64_t{-1}, std::numeric_limits<std::int64_t>::max() - 59})
      refuse(codec.encode(owner(), binding(), "text/plain", now));
    REQUIRE(codec.encode(owner(), binding(), "text/plain",
                         std::numeric_limits<std::int64_t>::max() - 60));
  }
  SECTION(
      "owner original auth kind source profile limit and process all bind") {
    for (unsigned int variant = 0; variant < 7; ++variant) {
      auto auth = owner();
      if (variant == 0) auth.user_id.back() = '7';
      if (variant == 1) auth.token_hash.back() = 'b';
      if (variant == 2)
        auth.credential_source = plinth::auth::CredentialSource::BEARER;
      if (variant == 3) {
        auth.auth_type = "pat";
        auth.pat_id = auth.session_id;
        auth.session_id.clear();
        auth.credential_source = plinth::auth::CredentialSource::BEARER;
      }
      if (variant == 4)
        auth.credential_source = plinth::auth::CredentialSource::UNKNOWN;
      if (variant == 5) auth.auth_type = "administrator";
      if (variant == 6) auth.session_id = "invalid";
      refuse(codec.decode(GOLDEN, auth, binding(), 1000));
      if (variant >= 4)
        refuse(codec.encode(auth, binding(), "text/plain", 1000));
    }
    auto source = owner();
    source.credential_source = plinth::auth::CredentialSource::BEARER;
    const auto bearer = codec.encode(source, binding(), "text/plain", 1000);
    REQUIRE(bearer);
    REQUIRE(codec.decode(*bearer, source, binding(), 1000) == "text/plain");
    refuse(codec.decode(*bearer, owner(), binding(), 1000));
    auto pat = source;
    pat.auth_type = "pat";
    pat.pat_id = pat.session_id;
    pat.session_id.clear();
    const auto personal = codec.encode(pat, binding(), "text/plain", 1000);
    REQUIRE(personal);
    REQUIRE(codec.decode(*personal, pat, binding(), 1000) == "text/plain");
    refuse(codec.decode(*bearer, pat, binding(), 1000));
    pat.credential_source = plinth::auth::CredentialSource::COOKIE;
    refuse(codec.encode(pat, binding(), "text/plain", 1000));
    for (unsigned int variant = 0; variant < 6; ++variant) {
      auto query = binding();
      if (variant == 0) query.limit = 1;
      if (variant == 1) query.limit = 0;
      if (variant == 2) query.limit = 65;
      if (variant == 3) query.platform_profile.back() = 'c';
      if (variant == 4) query.platform_profile[5] = '1';
      if (variant == 5) query.platform_profile = "caller-supplied";
      refuse(codec.decode(GOLDEN, owner(), query, 1000));
    }
    auto other = secret();
    other[0] = 255;
    const auto restarted = admin::test_seam::cursor_codec(other);
    refuse(restarted.decode(GOLDEN, owner(), binding(), 1000));
    const admin::CursorCodec process_one;
    const admin::CursorCodec process_two;
    const auto random =
        process_one.encode(owner(), binding(), "text/plain", 1000);
    REQUIRE(random);
    refuse(process_two.decode(*random, owner(), binding(), 1000));
  }
  SECTION("float and admin domains cannot be confused even with the same test "
          "key") {
    const auto floats = protocol::test_seam::cursor_codec(secret());
    protocol::CursorQuery query{.content_type = "text/plain",
                                .limit = 2,
                                .include_admin_default = true,
                                .platform_profile = binding().platform_profile};
    plinth::frontend::authority::Position position{"1", "notes", "preview"};
    const auto cursor = floats.encode(owner(), query, position, 1000);
    REQUIRE(cursor);
    refuse(codec.decode(*cursor, owner(), binding(), 1000));
    REQUIRE_FALSE(floats.decode(GOLDEN, owner(), query, 1000));
    for (const auto domain :
         {"plinth.surface.cursor.v1", "plinth.surface.credential.v1",
          "plinth.admin-defaults.credential.v1"})
      refuse(codec.decode(sign(PAYLOAD, domain), owner(), binding(), 1000));
    auto kind = AdminJson::parse(PAYLOAD);
    for (const auto name : {"float", "primary", "tray", "admin_default_read"}) {
      kind["kind"] = name;
      refuse(codec.decode(sign(kind.dump()), owner(), binding(), 1000));
    }
  }
  SECTION(
      "MAC canonical base64url and canonical closed payload refuse aliases") {
    auto wrong_mac = std::string{GOLDEN};
    wrong_mac[wrong_mac.find('.') + 1] = 'A';
    auto nonzero_bits = std::string{GOLDEN};
    nonzero_bits.back() = 'N'; // Last M has two zero low bits; N does not.
    auto payload_bits = std::string{GOLDEN};
    payload_bits[payload_bits.find('.') - 1] =
        'R'; // Final Q -> nonzero low bits.
    for (const auto& raw :
         {std::string{}, std::string{"text/plain"}, std::string{GOLDEN} + "=",
          std::string{GOLDEN} + ".x", wrong_mac, nonzero_bits, payload_bits,
          std::string(4096, 'A'), std::string(4097, 'A'), sign("{}"),
          sign(std::string{PAYLOAD} + " "), sign(" " + std::string{PAYLOAD}),
          sign(std::string{PAYLOAD} + "false"),
          sign(std::string{PAYLOAD}.substr(0, PAYLOAD.size() - 1) +
               ",\"v\":1}"),
          sign(std::string{PAYLOAD}.substr(0, PAYLOAD.size() - 1) +
               ",\"\\u0076\":1}"),
          sign(std::string{PAYLOAD}.substr(0, PAYLOAD.size() - 1) +
               ",\"after\":\"text/plain\"}"),
          sign(R"({"v":1,"after":{"nested":{"too_deep":1}}})")})
      refuse(codec.decode(raw, owner(), binding(), 1000));
    for (const auto key : {"v", "user", "credential", "kind", "limit",
                           "platform_profile", "after", "issued", "expires"}) {
      auto payload = AdminJson::parse(PAYLOAD);
      payload.erase(key);
      refuse(codec.decode(sign(payload.dump()), owner(), binding(), 1000));
    }
    for (unsigned int variant = 0; variant < 20; ++variant) {
      auto payload = AdminJson::parse(PAYLOAD);
      if (variant == 0) payload["v"] = 1.0;
      if (variant == 1) payload["limit"] = 2.0;
      if (variant == 2) payload["issued"] = 1000.0;
      if (variant == 3) payload["issued"] = -1;
      if (variant == 4) payload["expires"] = 1061;
      if (variant == 5)
        payload["expires"] = std::numeric_limits<std::uint64_t>::max();
      if (variant == 6) payload["after"] = "text/plain\n";
      if (variant == 7) payload["after"] = "TEXT/plain";
      if (variant == 8) payload["after"] = "";
      if (variant == 9) payload["after"] = AdminJson::object();
      if (variant == 10) payload["credential"] = owner().token_hash;
      if (variant == 11) payload["credential"] = AdminJson::array();
      if (variant == 12) payload["credential"] = std::string(43, 'A');
      if (variant == 13) payload["user"] = nullptr;
      if (variant == 14) payload["operation"] = "admin_default_read";
      if (variant == 15) payload["position"] = {"1", "notes", "preview"};
      if (variant == 16)
        payload["issued"] = std::numeric_limits<std::int64_t>::max();
      if (variant == 17) payload["expires"] = false;
      if (variant == 18) payload["v"] = true;
      if (variant == 19) payload["limit"] = "2";
      refuse(codec.decode(sign(payload.dump()), owner(), binding(), 1000));
    }
    for (const auto after : {"", "text/plain\n", "text/*", "text/plain;v=1"})
      refuse(codec.encode(owner(), binding(), after, 1000));
  }
}
