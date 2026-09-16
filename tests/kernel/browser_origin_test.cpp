#include "kernel/browser_origin.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>

TEST_CASE("browser origins have one canonical absolute form",
          "[browser-origin][unit]") {
  for (const auto* origin :
       {"", "http://localhost:8080", "https://plinth.example",
        "https://plinth.example:8443", "http://127.0.0.1:8080",
        "http://[::1]:8080"}) {
    INFO(origin);
    REQUIRE(plinth::valid_browser_origin(origin));
  }
  for (const auto* origin : {"null",
                             "//plinth.example",
                             "ftp://plinth.example",
                             "https://user@plinth.example",
                             "https://plinth.example/",
                             "http://plinth.example:80",
                             "https://plinth.example:443",
                             "https://plinth.example/path",
                             "https://plinth.example?x=1",
                             "https://plinth.example#fragment",
                             "https://plinth.example:0",
                             "https://plinth.example:65536",
                             "http://[:::1]",
                             "http://[0:0:0:0:0:0:0:1]:8080",
                             "http://127.000.000.001:8080",
                             "http://127.1:8080",
                             "http://999.999.999.999:8080",
                             "http://[::ffff:192.0.2.1]:8080",
                             "http://[::ffff:c000:201]:8080",
                             "http://[::192.0.2.1]:8080",
                             "http://[::c000:201]:8080",
                             "https://a..example",
                             "https://plinth.example,https://evil.example"}) {
    INFO(origin);
    REQUIRE_FALSE(plinth::valid_browser_origin(origin));
  }
}

TEST_CASE("browser origin matching uses exact Origin and Host authority",
          "[browser-origin][unit]") {
  auto request = drogon::HttpRequest::newHttpRequest();
  request->addHeader("Host", "plinth.example:8080");
  request->addHeader("Origin", "http://plinth.example:8080");
  request->addHeader("X-Forwarded-Proto", "https");

  REQUIRE(plinth::browser_origin_matches(request, ""));
  REQUIRE_FALSE(
      plinth::browser_origin_matches(request, "https://plinth.example:8080"));

  request->addHeader("Origin", "https://plinth.example:8080");
  REQUIRE(
      plinth::browser_origin_matches(request, "https://plinth.example:8080"));
  REQUIRE_FALSE(plinth::browser_origin_matches(
      request, "https://different.example:8080"));
}

TEST_CASE("browser-shaped request detection uses ambient browser signals",
          "[browser-origin][unit]") {
  auto native = drogon::HttpRequest::newHttpRequest();
  native->addHeader("Content-Type", "application/json");
  REQUIRE_FALSE(plinth::browser_shaped_request(native));

  auto cookie = drogon::HttpRequest::newHttpRequest();
  cookie->addCookie("plinth_session", "session");
  REQUIRE(plinth::browser_shaped_request(cookie));

  auto fetch = drogon::HttpRequest::newHttpRequest();
  fetch->addHeader("Sec-Fetch-Site", "same-origin");
  REQUIRE(plinth::browser_shaped_request(fetch));
}
