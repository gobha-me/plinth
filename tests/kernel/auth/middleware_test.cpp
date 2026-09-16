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
        [](const std::string&, plinth::auth::TokenValidationCallback) {
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
                           plinth::auth::TokenValidationCallback callback) {
        REQUIRE(raw_token == "invalid-session-token");
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
                           plinth::auth::TokenValidationCallback callback) {
        REQUIRE(raw_token == "opaque-session-token");
        callback(
            {.ok = false, .context = {}, .error_code = "service_unavailable"});
      });

  require_error(
      result, drogon::k503ServiceUnavailable,
      nlohmann::json{
          {"error", "service_unavailable"},
          {"message", "Authentication service is temporarily unavailable"}});
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
