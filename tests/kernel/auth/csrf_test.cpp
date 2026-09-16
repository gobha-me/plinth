#include "kernel/auth/csrf.hpp"

#include "kernel/auth/middleware.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <nlohmann/json.hpp>

namespace {

struct FilterResult {
  drogon::HttpResponsePtr response;
  bool continued{false};
};

template <typename Filter>
auto dispatch(Filter& filter, const drogon::HttpRequestPtr& request)
    -> FilterResult {
  FilterResult result;
  filter.doFilter(
      request,
      [&result](const drogon::HttpResponsePtr& response) {
        result.response = response;
      },
      [&result] { result.continued = true; });
  return result;
}

auto cookie_request(std::string_view token) -> drogon::HttpRequestPtr {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Post);
  request->addHeader("Host", "plinth.example");
  request->addHeader("Origin", "http://plinth.example");
  request->addCookie("plinth_csrf", std::string{token});
  request->addHeader("X-Plinth-CSRF", std::string{token});
  request->attributes()->insert(plinth::auth::ATTR_USER_ID,
                                std::string{"user-id"});
  request->attributes()->insert(plinth::auth::ATTR_CREDENTIAL_SOURCE,
                                std::string{"cookie"});
  request->attributes()->insert(plinth::auth::ATTR_CSRF_TOKEN,
                                std::string{token});
  return request;
}

auto require_csrf_failure(const FilterResult& result) -> void {
  REQUIRE_FALSE(result.continued);
  REQUIRE(result.response);
  REQUIRE(result.response->statusCode() == drogon::k403Forbidden);
  REQUIRE(nlohmann::json::parse(result.response->body()) ==
          nlohmann::json{{"error", "csrf_failed"},
                         {"message", "Request validation failed"}});
  REQUIRE(result.response->getHeader("Cache-Control") == "no-store");
  REQUIRE(result.response->getHeader("Vary") ==
          "Origin, Cookie, Authorization");
}

} // namespace

TEST_CASE("CSRF token derivation is stable, session-bound, and base64url",
          "[auth][csrf][unit]") {
  const auto token =
      plinth::auth::csrf_token_for_session("session-token-for-test");
  constexpr auto expected = "4QiLXgHg5DvZWCP"
                            "XHWwVyPE0YuvaK"
                            "X67CSLnZh72wFs";
  REQUIRE(token == expected);
  REQUIRE(token.size() == 43);
  REQUIRE(
      token.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
      std::string::npos);
  REQUIRE(token != plinth::auth::csrf_token_for_session("another-session"));
}

TEST_CASE("CSRF cookie attributes match the session lifetime",
          "[auth][csrf][unit]") {
  auto response = drogon::HttpResponse::newHttpResponse();
  plinth::auth::add_csrf_cookie(response, "csrf-value", false);
  const auto cookie = response->getCookie("plinth_csrf").cookieString();
  REQUIRE(cookie.find("plinth_csrf=csrf-value") != std::string::npos);
  REQUIRE(cookie.find("Path=/") != std::string::npos);
  REQUIRE(cookie.find("Max-Age=86400") != std::string::npos);
  REQUIRE(cookie.find("SameSite=Strict") != std::string::npos);
  REQUIRE(cookie.find("Secure") != std::string::npos);
  REQUIRE(cookie.find("HttpOnly") == std::string::npos);

  auto cleared = drogon::HttpResponse::newHttpResponse();
  plinth::auth::clear_csrf_cookie(cleared);
  REQUIRE(cleared->getCookie("plinth_csrf").cookieString().find("Max-Age=0") !=
          std::string::npos);
}

TEST_CASE("cookie mutations require exact origin and double-submit values",
          "[auth][csrf][unit]") {
  plinth::auth::configure_browser_origin("");
  plinth::auth::CsrfFilter filter;
  const auto token = plinth::auth::csrf_token_for_session("session");

  SECTION("valid request") {
    const auto result = dispatch(filter, cookie_request(token));
    REQUIRE(result.continued);
    REQUIRE_FALSE(result.response);
  }

  SECTION("missing header") {
    auto request = cookie_request(token);
    request->removeHeader("X-Plinth-CSRF");
    require_csrf_failure(dispatch(filter, request));
  }

  SECTION("wrong cookie") {
    auto request = cookie_request(token);
    request->addCookie("plinth_csrf", "wrong");
    require_csrf_failure(dispatch(filter, request));
  }

  SECTION("cross-origin") {
    auto request = cookie_request(token);
    request->addHeader("Origin", "http://evil.example");
    require_csrf_failure(dispatch(filter, request));
  }

  SECTION("missing server expectation") {
    auto request = cookie_request(token);
    request->attributes()->insert(plinth::auth::ATTR_CSRF_TOKEN, std::string{});
    require_csrf_failure(dispatch(filter, request));
  }
}

TEST_CASE("bearer mutations and safe methods do not require CSRF",
          "[auth][csrf][unit]") {
  plinth::auth::configure_browser_origin("");
  plinth::auth::CsrfFilter filter;

  auto bearer = drogon::HttpRequest::newHttpRequest();
  bearer->setMethod(drogon::Post);
  bearer->attributes()->insert(plinth::auth::ATTR_USER_ID,
                               std::string{"user-id"});
  bearer->attributes()->insert(plinth::auth::ATTR_CREDENTIAL_SOURCE,
                               std::string{"bearer"});
  REQUIRE(dispatch(filter, bearer).continued);

  auto safe = cookie_request("not-a-valid-token");
  safe->setMethod(drogon::Get);
  REQUIRE(dispatch(filter, safe).continued);
}

TEST_CASE("CSRF filter fails closed when authentication context is absent",
          "[auth][csrf][unit]") {
  plinth::auth::configure_browser_origin("");
  plinth::auth::CsrfFilter filter;

  auto missing = drogon::HttpRequest::newHttpRequest();
  missing->setMethod(drogon::Post);
  require_csrf_failure(dispatch(filter, missing));

  auto unknown = drogon::HttpRequest::newHttpRequest();
  unknown->setMethod(drogon::Post);
  unknown->attributes()->insert(plinth::auth::ATTR_USER_ID,
                                std::string{"user-id"});
  unknown->attributes()->insert(plinth::auth::ATTR_CREDENTIAL_SOURCE,
                                std::string{"unknown"});
  require_csrf_failure(dispatch(filter, unknown));
}

TEST_CASE("configured proxy origin preserves Host authority",
          "[auth][csrf][unit]") {
  plinth::auth::configure_browser_origin("https://plinth.example");
  plinth::auth::CsrfFilter filter;
  const auto token = plinth::auth::csrf_token_for_session("session");
  auto request = cookie_request(token);
  request->addHeader("Origin", "https://plinth.example");
  REQUIRE(dispatch(filter, request).continued);

  request->addHeader("Host", "internal.example");
  require_csrf_failure(dispatch(filter, request));
}

TEST_CASE("public mutations allow native clients but constrain browsers",
          "[auth][csrf][unit]") {
  plinth::auth::configure_browser_origin("");
  plinth::auth::PublicOriginFilter filter;

  auto native = drogon::HttpRequest::newHttpRequest();
  native->setMethod(drogon::Post);
  REQUIRE(dispatch(filter, native).continued);

  auto browser = drogon::HttpRequest::newHttpRequest();
  browser->setMethod(drogon::Post);
  browser->addHeader("Sec-Fetch-Site", "same-origin");
  require_csrf_failure(dispatch(filter, browser));

  auto same_origin = drogon::HttpRequest::newHttpRequest();
  same_origin->setMethod(drogon::Post);
  same_origin->addHeader("Host", "plinth.example");
  same_origin->addHeader("Origin", "http://plinth.example");
  REQUIRE(dispatch(filter, same_origin).continued);

  same_origin->addHeader("Origin", "http://evil.example");
  require_csrf_failure(dispatch(filter, same_origin));
}
