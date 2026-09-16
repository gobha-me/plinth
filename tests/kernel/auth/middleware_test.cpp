#include "kernel/auth/csrf.hpp"
#include "kernel/auth/middleware.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace {

struct FilterResult {
  drogon::HttpResponsePtr response;
  bool continued{false};
};

auto dispatch(
    const drogon::HttpRequestPtr& request,
    plinth::auth::test_seam::TokenValidator validator =
        [](const std::string&, plinth::auth::CredentialSource,
           plinth::auth::TokenValidationCallback) {
          FAIL("token validator must not run without a token");
        }) -> FilterResult {
  FilterResult result;
  plinth::auth::test_seam::dispatch_session_filter(
      request,
      [&result](const drogon::HttpResponsePtr& response) {
        result.response = response;
      },
      [&result] { result.continued = true; }, std::move(validator));
  return result;
}

auto require_error(const FilterResult& result,
                   drogon::HttpStatusCode expected_status,
                   const nlohmann::json& expected_body) -> void {
  REQUIRE_FALSE(result.continued);
  REQUIRE(result.response);
  REQUIRE(result.response->statusCode() == expected_status);
  REQUIRE(nlohmann::json::parse(result.response->body()) == expected_body);
  REQUIRE(result.response->getHeader("Cache-Control") == "no-store");
  REQUIRE(result.response->getHeader("Vary") == "Cookie, Authorization");
}

} // namespace

TEST_CASE("SessionFilter rejects a missing token with a non-cacheable 401",
          "[auth][middleware]") {
  auto request = drogon::HttpRequest::newHttpRequest();

  require_error(
      dispatch(request), drogon::k401Unauthorized,
      nlohmann::json{{"error", "not_authenticated"},
                     {"message", "No authentication token provided"}});
}

TEST_CASE("SessionFilter sanitizes an invalid-token response",
          "[auth][middleware]") {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->addCookie("plinth_session", "invalid-session-token");

  auto result =
      dispatch(request, [](const std::string& raw_token,
                           plinth::auth::CredentialSource source,
                           plinth::auth::TokenValidationCallback callback) {
        REQUIRE(raw_token == "invalid-session-token");
        REQUIRE(source == plinth::auth::CredentialSource::COOKIE);
        callback(
            {.ok = false, .context = {}, .error_code = "not_authenticated"});
      });

  require_error(
      result, drogon::k401Unauthorized,
      nlohmann::json{{"error", "not_authenticated"},
                     {"message", "Invalid or missing authentication token"}});
}

TEST_CASE("SessionFilter sanitizes authentication database failures",
          "[auth][middleware]") {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->addHeader("Authorization", "Bearer opaque-session-token");

  auto result =
      dispatch(request, [](const std::string& raw_token,
                           plinth::auth::CredentialSource source,
                           plinth::auth::TokenValidationCallback callback) {
        REQUIRE(raw_token == "opaque-session-token");
        REQUIRE(source == plinth::auth::CredentialSource::BEARER);
        callback(
            {.ok = false, .context = {}, .error_code = "service_unavailable"});
      });

  require_error(
      result, drogon::k503ServiceUnavailable,
      nlohmann::json{
          {"error", "service_unavailable"},
          {"message", "Authentication service is temporarily unavailable"}});
}

TEST_CASE("SessionFilter records cookie precedence and its bound CSRF token",
          "[auth][middleware][csrf]") {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->addCookie("plinth_session", "cookie-session");
  request->addHeader("Authorization", "Bearer plinth_pat-must-not-win");

  auto result =
      dispatch(request, [](const std::string& raw_token,
                           plinth::auth::CredentialSource source,
                           plinth::auth::TokenValidationCallback callback) {
        REQUIRE(raw_token == "cookie-session");
        REQUIRE(source == plinth::auth::CredentialSource::COOKIE);
        callback({.ok = true,
                  .context = {.user_id = "user-id",
                              .username = "user",
                              .auth_type = "session",
                              .session_id = "session-id",
                              .pat_id = "",
                              .token_hash = "hash"},
                  .error_code = ""});
      });

  REQUIRE(result.continued);
  REQUIRE_FALSE(result.response);
  const auto context = plinth::auth::get_auth_context(request);
  REQUIRE(context.has_value());
  REQUIRE(context->credential_source == plinth::auth::CredentialSource::COOKIE);
  REQUIRE(
      request->attributes()->get<std::string>(plinth::auth::ATTR_CSRF_TOKEN) ==
      plinth::auth::csrf_token_for_session("cookie-session"));
}

TEST_CASE("SessionFilter bearer auth has no CSRF expectation",
          "[auth][middleware][csrf]") {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->addHeader("Authorization", "Bearer bearer-session");

  auto result =
      dispatch(request, [](const std::string& raw_token,
                           plinth::auth::CredentialSource source,
                           plinth::auth::TokenValidationCallback callback) {
        REQUIRE(raw_token == "bearer-session");
        REQUIRE(source == plinth::auth::CredentialSource::BEARER);
        callback({.ok = true,
                  .context = {.user_id = "user-id",
                              .username = "user",
                              .auth_type = "session",
                              .session_id = "session-id",
                              .pat_id = "",
                              .token_hash = "hash"},
                  .error_code = ""});
      });

  REQUIRE(result.continued);
  const auto context = plinth::auth::get_auth_context(request);
  REQUIRE(context.has_value());
  REQUIRE(context->credential_source == plinth::auth::CredentialSource::BEARER);
  REQUIRE_FALSE(plinth::auth::request_expected_csrf_token(request).has_value());
}

TEST_CASE("session validation maps database errors to service unavailable",
          "[auth][middleware][db]") {
#if USE_SQLITE3
  using namespace std::chrono_literals;

  auto database =
      drogon::orm::DbClient::newSqlite3Client("filename=:memory:", 1);
  auto promise =
      std::make_shared<std::promise<plinth::auth::TokenValidationResult>>();
  auto future = promise->get_future();

  plinth::auth::validate_session_token(
      "opaque-session-token",
      [promise](plinth::auth::TokenValidationResult result) {
        promise->set_value(std::move(result));
      },
      database);

  REQUIRE(future.wait_for(5s) == std::future_status::ready);
  const auto result = future.get();
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error_code == "service_unavailable");
  database->closeAll();
#else
  SKIP("Drogon was built without SQLite support");
#endif
}

TEST_CASE("PAT validation maps database errors to service unavailable",
          "[auth][middleware][db]") {
#if USE_SQLITE3
  using namespace std::chrono_literals;

  auto database =
      drogon::orm::DbClient::newSqlite3Client("filename=:memory:", 1);
  auto promise =
      std::make_shared<std::promise<plinth::auth::TokenValidationResult>>();
  auto future = promise->get_future();

  plinth::auth::validate_pat_token(
      "plinth_opaque-pat-token",
      [promise](plinth::auth::TokenValidationResult result) {
        promise->set_value(std::move(result));
      },
      database);

  REQUIRE(future.wait_for(5s) == std::future_status::ready);
  const auto result = future.get();
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error_code == "service_unavailable");
  database->closeAll();
#else
  SKIP("Drogon was built without SQLite support");
#endif
}
