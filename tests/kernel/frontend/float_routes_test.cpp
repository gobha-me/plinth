#include "kernel/frontend/float_layout_service.hpp"
#include "kernel/frontend/surface_protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <nlohmann/json.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// Low-level actual facade helpers, not a replacement service/filter/authority.
// No handler registration, PG, native operation or proof token is fabricated.
namespace plinth::frontend::test_seam {
auto float_admission_error_code(TypedRequestError) -> std::string_view;
auto float_commit_error_code(layout_contract::Error) -> std::string_view;
auto parse_float_catalog_query(std::string_view)
    -> std::optional<FloatLayoutService::CatalogQuery>;
auto float_request_origin(const drogon::HttpRequestPtr&, std::string_view)
    -> std::optional<std::string>;
auto float_asset_selector(const drogon::HttpRequestPtr&, bool)
    -> std::expected<protocol::AssetSelector, protocol::Error>;
auto float_route_contract() -> Json::Value;
auto float_json_completion(std::function<void(const drogon::HttpResponsePtr&)>,
                           bool) -> FloatLayoutService::Completion;
auto float_asset_completion(std::function<void(const drogon::HttpResponsePtr&)>)
    -> FloatLayoutService::AssetCompletion;
auto float_admin_read_request(std::string_view, std::string_view)
    -> std::expected<admin_defaults::ReadRequest, TypedRequestError>;
auto float_admin_put_request(std::string_view, std::string_view)
    -> std::expected<admin_defaults::PutRequest, TypedRequestError>;
auto float_admin_delete_request(std::string_view, std::string_view)
    -> std::expected<admin_defaults::DeleteRequest, TypedRequestError>;
auto float_admin_completion(std::function<void(const drogon::HttpResponsePtr&)>,
                            bool) -> FloatLayoutService::AdminCompletion;
} // namespace plinth::frontend::test_seam

namespace {
namespace seam = plinth::frontend::test_seam;
using RouteJson = nlohmann::json;
constexpr std::string_view GENERATION = "10000000-0000-4000-8000-000000000046";
auto rejection(std::string code)
    -> plinth::frontend::FloatLayoutService::Result {
  return std::unexpected(plinth::js::PromiseRejection{
      .code = std::move(code),
      .message = "private fake fixture message must not render",
      .sqlstate = "private-fake-sql-state"});
}
auto hardening(const drogon::HttpResponsePtr& response, bool mutation) -> void {
  REQUIRE(response);
  REQUIRE(response->contentType() == drogon::CT_APPLICATION_JSON);
  REQUIRE(response->getHeader("Cache-Control") == "no-store");
  REQUIRE(
      response->getHeader("Vary") ==
      (mutation ? "Origin, Cookie, Authorization" : "Cookie, Authorization"));
  REQUIRE(response->getHeader("X-Content-Type-Options") == "nosniff");
  REQUIRE(response->getHeader("X-Plinth-Platform-Profile").empty());
  REQUIRE(response->body().find("private") == std::string_view::npos);
  REQUIRE(RouteJson::parse(response->body()).is_object());
}
} // namespace

TEST_CASE("float routes: strict raw facade and one-shot redacted responses",
          "[frontend][float_routes]") {
  SECTION(
      "admission parser resource errors do not masquerade as oversized input") {
    using Error = plinth::frontend::TypedRequestError;
    for (const auto& [error, status] :
         {std::pair{Error::INVALID, drogon::k400BadRequest},
          std::pair{Error::TOO_LARGE, drogon::k413RequestEntityTooLarge},
          std::pair{Error::UNAVAILABLE, drogon::k503ServiceUnavailable}}) {
      unsigned calls = 0;
      drogon::HttpResponsePtr response;
      auto completion = seam::float_json_completion(
          [&](const auto& actual) {
            ++calls;
            response = actual;
          },
          true);
      completion(
          rejection(std::string{seam::float_admission_error_code(error)}));
      REQUIRE(calls == 1);
      REQUIRE(response->statusCode() == status);
      hardening(response, true);
    }
  }
  SECTION("commit parse pressure is unavailable not semantic invalidity") {
    namespace contract = plinth::frontend::layout_contract;
    for (const auto& [error, status] :
         {std::pair{contract::Error::INVALID, drogon::k400BadRequest},
          std::pair{contract::Error::STRUCTURAL_LIMIT, drogon::k400BadRequest},
          std::pair{contract::Error::TOO_LARGE,
                    drogon::k413RequestEntityTooLarge},
          std::pair{contract::Error::LIMIT_EXCEEDED,
                    drogon::k503ServiceUnavailable}}) {
      unsigned calls = 0;
      drogon::HttpResponsePtr response;
      auto completion = seam::float_json_completion(
          [&](const auto& actual) {
            ++calls;
            response = actual;
          },
          true);
      completion(rejection(std::string{seam::float_commit_error_code(error)}));
      REQUIRE(calls == 1);
      REQUIRE(response->statusCode() == status);
      hardening(response, true);
    }
  }
  SECTION("actual registration definition separates public graph and ordered "
          "auth CSRF filters") {
    const auto routes = seam::float_route_contract();
    REQUIRE(routes.isArray());
    REQUIRE(routes.size() == 9);
    for (Json::ArrayIndex index = 0; index < routes.size(); ++index) {
      const auto& row = routes[index];
      const auto& filters = row["filters"];
      if (index == 5)
        REQUIRE(filters.empty());
      else {
        REQUIRE(filters[0].asString() == "plinth::auth::SessionFilter");
        const bool mutation =
            index == 1 || index == 3 || index == 7 || index == 8;
        REQUIRE(filters.size() == (mutation ? 2U : 1U));
        if (mutation)
          REQUIRE(filters[1].asString() == "plinth::auth::CsrfFilter");
      }
      REQUIRE(row["regex"].asBool() == (index == 4 || index == 5));
    }
    REQUIRE(routes[0]["path"].asString() == "/api/frontend/surfaces");
    REQUIRE(routes[1]["path"].asString() == "/api/frontend/surfaces/admission");
    REQUIRE(routes[2]["path"].asString() == "/api/frontend/float-layout");
    REQUIRE(routes[3]["path"].asString() == "/api/frontend/float-layout");
    REQUIRE(routes[4]["path"].asString() == "^/_plinth/typed/.*$");
    REQUIRE(routes[5]["path"].asString() == "^/_plinth/platform/.*$");
    for (const Json::ArrayIndex index : {6U, 7U, 8U})
      REQUIRE(routes[index]["path"].asString() ==
              "/api/frontend/handler-defaults/admin");
    REQUIRE(routes[0]["methods"][0].asInt() == static_cast<int>(drogon::Get));
    REQUIRE(routes[1]["methods"][0].asInt() == static_cast<int>(drogon::Post));
    REQUIRE(routes[3]["methods"][0].asInt() == static_cast<int>(drogon::Put));
    REQUIRE(routes[6]["methods"][0].asInt() == static_cast<int>(drogon::Get));
    REQUIRE(routes[7]["methods"][0].asInt() == static_cast<int>(drogon::Put));
    REQUIRE(routes[8]["methods"][0].asInt() ==
            static_cast<int>(drogon::Delete));
    for (const Json::ArrayIndex index : {4U, 5U}) {
      REQUIRE(routes[index]["methods"].size() == 2);
      REQUIRE(routes[index]["methods"][0].asInt() ==
              static_cast<int>(drogon::Get));
      REQUIRE(routes[index]["methods"][1].asInt() ==
              static_cast<int>(drogon::Head));
    }
  }
  SECTION("actual admin raw adapters reject GET bodies and mutation queries") {
    namespace admin = plinth::frontend::admin_defaults;
    // HttpRequest::setPath does not populate query() in this pinned framework.
    // Exercise the SAME raw query/body adapter used by handle(), rather than
    // fabricate parser state or erase duplicates through a parameter map.
    // Real HTTP request parsing/filtering is a separate native-browser gate.
    const auto page = seam::float_admin_read_request("", "");
    REQUIRE(page);
    REQUIRE(std::holds_alternative<admin::ReadPage>(*page));
    REQUIRE(std::get<admin::ReadPage>(*page).limit == 64);
    const auto single =
        seam::float_admin_read_request("content_type=text%2Fplain", "");
    REQUIRE(single);
    REQUIRE(std::holds_alternative<admin::ReadSingle>(*single));
    REQUIRE(std::get<admin::ReadSingle>(*single).content_type == "text/plain");
    REQUIRE_FALSE(seam::float_admin_read_request("", " "));
    REQUIRE_FALSE(seam::float_admin_read_request("limit=1&l%69mit=2", ""));
    constexpr std::string_view put =
        R"({"schema_version":1,"content_type":"text/plain","handler":{"application_id":"uninstalled-app","panel_id":"old_panel"}})";
    REQUIRE(seam::float_admin_put_request("", put));
    REQUIRE_FALSE(seam::float_admin_delete_request("", put));
    REQUIRE_FALSE(seam::float_admin_put_request("limit=1", put));
    constexpr std::string_view deletion =
        R"({"schema_version":1,"content_type":"text/plain"})";
    REQUIRE(seam::float_admin_delete_request("", deletion));
    REQUIRE_FALSE(seam::float_admin_delete_request("content_type=text%2Fplain",
                                                   deletion));
    const auto large =
        seam::float_admin_put_request("", std::string(2049, ' '));
    REQUIRE_FALSE(large);
    REQUIRE(large.error() == plinth::frontend::TypedRequestError::TOO_LARGE);
  }
  SECTION(
      "actual admin error adapter is fixed redacted hardened and one-shot") {
    for (const bool changes : {false, true}) {
      for (const auto& [code, status] :
           {std::pair{"invalid_argument", drogon::k400BadRequest},
            std::pair{"not_authenticated", drogon::k401Unauthorized},
            std::pair{"permission_denied", drogon::k403Forbidden},
            std::pair{"stale_binding", drogon::k409Conflict},
            std::pair{"body_too_large", drogon::k413RequestEntityTooLarge},
            std::pair{"storage_unavailable", drogon::k503ServiceUnavailable},
            std::pair{"private-unrecognized",
                      drogon::k503ServiceUnavailable}}) {
        unsigned calls = 0;
        drogon::HttpResponsePtr response;
        auto completion = seam::float_admin_completion(
            [&](const auto& actual) {
              ++calls;
              response = actual;
            },
            changes);
        const auto reject =
            [&]() -> plinth::frontend::FloatLayoutService::AdminResult {
          return std::unexpected(plinth::js::PromiseRejection{
              .code = code,
              .message = "private fake fixture message",
              .sqlstate = "private-fake-sqlstate"});
        };
        completion(reject());
        completion(reject());
        REQUIRE(calls == 1);
        REQUIRE(response->statusCode() == status);
        hardening(response, changes);
      }
    }
    unsigned calls = 0;
    auto completion = seam::float_admin_completion(
        [&](const auto&) {
          ++calls;
          throw std::runtime_error{"controlled callback"};
        },
        true);
    const auto reject =
        []() -> plinth::frontend::FloatLayoutService::AdminResult {
      return std::unexpected(plinth::js::PromiseRejection{
          .code = "storage_unavailable", .message = "private", .sqlstate = ""});
    };
    REQUIRE_THROWS_AS(completion(reject()), std::runtime_error);
    REQUIRE_NOTHROW(completion(reject()));
    REQUIRE(calls == 1);
    // No success-frame construction seam: actual native service and HTTP
    // journeys, not this low-level helper, must prove ACK/profile ownership.
  }
  SECTION("single raw query decode validates exact closed fields without "
          "map-erased duplicates") {
    auto parsed = seam::parse_float_catalog_query("kind=float");
    REQUIRE(parsed);
    REQUIRE(parsed->filters.limit == 64);
    REQUIRE_FALSE(parsed->filters.content_type);
    REQUIRE_FALSE(parsed->filters.include_admin_default);
    REQUIRE(parsed->filters.platform_profile.empty());
    REQUIRE(parsed->filters.kind == plinth::frontend::SurfaceKind::FLOAT);
    for (const auto& [name, kind] :
         {std::pair{"primary", plinth::frontend::SurfaceKind::PRIMARY},
          std::pair{"tray", plinth::frontend::SurfaceKind::TRAY}}) {
      const auto typed = seam::parse_float_catalog_query(
          std::string{"kind="} + name + "&limit=1&include_admin_default=0");
      REQUIRE(typed);
      REQUIRE(typed->filters.kind == kind);
      REQUIRE_FALSE(typed->filters.content_type);
      REQUIRE_FALSE(typed->filters.include_admin_default);
    }
    parsed = seam::parse_float_catalog_query(
        "kind=float&limit=1&content_type=text%2Fx-c%2B%2B&include_admin_"
        "default=1&cursor=opaque-token.signature");
    REQUIRE(parsed);
    REQUIRE(parsed->filters.limit == 1);
    REQUIRE(parsed->filters.content_type == "text/x-c++");
    REQUIRE(parsed->filters.include_admin_default);
    REQUIRE(parsed->cursor == "opaque-token.signature");
    parsed = seam::parse_float_catalog_query(
        "kind=float&include_admin_default=0&cursor=%252e");
    REQUIRE(parsed);
    REQUIRE(parsed->cursor ==
            "%2e"); // Later real MAC rejects; never a second decode.
    for (const auto raw : {"",
                           "limit=2",
                           "kind=settings",
                           "kind=primary&content_type=text%2Fplain",
                           "kind=tray&content_type=text%2Fplain",
                           "kind=primary&include_admin_default=1",
                           "kind=tray&include_admin_default=1",
                           "kind=tray&include_admin_default=true",
                           "kind=float&kind=float",
                           "kind=float&k%69nd=float",
                           "kind=float&extra=x",
                           "kind=float&limit=0",
                           "kind=float&limit=65",
                           "kind=float&limit=01",
                           "kind=float&limit=+1",
                           "kind=float&limit=1.0",
                           "kind=float&limit=1&limit=2",
                           "kind=float&content_type=Text%2FPlain",
                           "kind=float&content_type=text%252Fplain",
                           "kind=float&content_type=text/x-c++",
                           "kind=float&include_admin_default=1",
                           "kind=float&include_admin_default=true",
                           "kind=float&cursor=",
                           "kind=float&cursor=%00",
                           "kind=float&cursor=%FF",
                           "kind=float&cursor=%3",
                           "kind=float&cursor=%GG",
                           "kind=float&",
                           "kind=float&&limit=2",
                           "kind=float&limit",
                           "kind=float&limit=1=2",
                           "kind=float&X-Plinth-Platform-Profile=x"}) {
      INFO(raw);
      REQUIRE_FALSE(seam::parse_float_catalog_query(raw));
    }
    REQUIRE(seam::parse_float_catalog_query("kind=float&cursor=" +
                                            std::string(4096, 'a')));
    REQUIRE_FALSE(seam::parse_float_catalog_query("kind=float&cursor=" +
                                                  std::string(4097, 'a')));
    REQUIRE_FALSE(seam::parse_float_catalog_query("kind=float&cursor=" +
                                                  std::string(16384, 'a')));
  }
  SECTION("trusted origin ignores forwarded and request profile claims and "
          "validates connection host") {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->addHeader("Host", "127.0.0.1:8765");
    request->addHeader("Forwarded", "proto=https;host=attacker.test");
    request->addHeader("X-Forwarded-Proto", "https");
    request->addHeader("X-Forwarded-Host", "attacker.test");
    request->addHeader("X-Plinth-Platform-Profile", "attacker-controlled");
    REQUIRE(seam::float_request_origin(request, "") == "http://127.0.0.1:8765");
    REQUIRE(seam::float_request_origin(request, "https://configured.test") ==
            "https://configured.test");
    for (const auto host :
         {"", "user@attacker.test", "example.test/path", "example.test?x",
          "example.test#x", "example.test:00080", "example.test\n"}) {
      auto bad = drogon::HttpRequest::newHttpRequest();
      bad->addHeader("Host", host);
      REQUIRE_FALSE(seam::float_request_origin(bad, ""));
    }
    REQUIRE_FALSE(
        seam::float_request_origin(request, "https://configured.test/"));
  }
  SECTION("asset facade consumes original raw path with exact route kind and "
          "no query") {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setPathEncode(false);
    const auto typed = "/_plinth/typed/" + std::string{GENERATION} + "/" +
                       std::string(64, 'a') + "/preview/client/panels/%252e.js";
    request->setPath(typed);
    auto selected = seam::float_asset_selector(request, false);
    REQUIRE(selected);
    REQUIRE(selected->member_path == "client/panels/%2e.js");
    REQUIRE_FALSE(seam::float_asset_selector(request, true));
    request->setPath(typed + "?x=1");
    REQUIRE_FALSE(seam::float_asset_selector(request, false));
    request->setPath(typed + "/../x.js");
    REQUIRE_FALSE(seam::float_asset_selector(request, false));
    request->setPath("/_plinth/platform/" + std::string{GENERATION} + "/" +
                     std::string(64, 'a') + "/client/sdk.js");
    REQUIRE(seam::float_asset_selector(request, true));
    REQUIRE_FALSE(seam::float_asset_selector(request, false));
  }
  SECTION("actual error completions harden redact and deliver once across "
          "service codes") {
    for (const auto mutation : {false, true}) {
      for (const auto& pair :
           {std::pair{"invalid_argument", drogon::k400BadRequest},
            std::pair{"not_authenticated", drogon::k401Unauthorized},
            std::pair{"permission_denied", drogon::k403Forbidden},
            std::pair{"stale_binding", drogon::k409Conflict},
            std::pair{"body_too_large", drogon::k413RequestEntityTooLarge},
            std::pair{"payload_too_large", drogon::k413RequestEntityTooLarge},
            std::pair{"request_busy", drogon::k503ServiceUnavailable},
            std::pair{"private-fake-unknown",
                      drogon::k503ServiceUnavailable}}) {
        unsigned int calls = 0;
        drogon::HttpResponsePtr response;
        auto completion = seam::float_json_completion(
            [&](const auto& actual) {
              ++calls;
              response = actual;
            },
            mutation);
        completion(rejection(pair.first));
        completion(rejection("not_authenticated"));
        REQUIRE(calls == 1);
        REQUIRE(response->statusCode() == pair.second);
        hardening(response, mutation);
      }
    }
    unsigned int calls = 0;
    auto throwing = seam::float_json_completion(
        [&](const auto&) {
          ++calls;
          throw std::runtime_error("controlled callback failure");
        },
        false);
    REQUIRE_THROWS(throwing(rejection("request_busy")));
    REQUIRE_NOTHROW(throwing(rejection("not_authenticated")));
    REQUIRE(calls == 1);
    drogon::HttpResponsePtr invalid_response;
    auto invalid = seam::float_json_completion(
        [&](const auto& actual) { invalid_response = actual; }, false);
    invalid(Json::Value{Json::arrayValue});
    REQUIRE(invalid_response->statusCode() == drogon::k503ServiceUnavailable);
    hardening(invalid_response, false);
  }
  SECTION("asset error mapping does not turn denial into metadata and "
          "preserves actual serializer response") {
    for (const auto& pair :
         {std::pair{"asset_not_found", drogon::k404NotFound},
          std::pair{"permission_denied", drogon::k404NotFound},
          std::pair{"invalid_argument", drogon::k404NotFound},
          std::pair{"range_not_satisfiable",
                    drogon::k416RequestedRangeNotSatisfiable},
          std::pair{"request_busy", drogon::k503ServiceUnavailable},
          std::pair{"not_authenticated", drogon::k401Unauthorized}}) {
      unsigned int calls = 0;
      drogon::HttpResponsePtr response;
      auto completion = seam::float_asset_completion([&](const auto& actual) {
        ++calls;
        response = actual;
      });
      completion(std::unexpected(
          plinth::js::PromiseRejection{.code = pair.first,
                                       .message = "private fake message",
                                       .sqlstate = {}}));
      completion(std::unexpected(plinth::js::PromiseRejection{
          .code = "request_busy", .message = "", .sqlstate = {}}));
      REQUIRE(calls == 1);
      REQUIRE(response->statusCode() == pair.second);
      hardening(response, false);
    }
    auto serializer_response = drogon::HttpResponse::newHttpResponse();
    serializer_response->setStatusCode(drogon::k304NotModified);
    drogon::HttpResponsePtr delivered;
    auto completion = seam::float_asset_completion(
        [&](const auto& actual) { delivered = actual; });
    completion(serializer_response);
    REQUIRE(delivered == serializer_response);
    // Pointer pass-through only: no AdmittedAsset/source/authority is
    // fabricated.
  }
}
