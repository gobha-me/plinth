#include "kernel/security/response_headers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpTypes.h>

TEST_CASE("response headers preserve handler semantics across response classes",
          "[security][response-headers]") {
  for (const auto status :
       {drogon::k200OK, drogon::k302Found, drogon::k401Unauthorized,
        drogon::k404NotFound, drogon::k413RequestEntityTooLarge,
        drogon::k429TooManyRequests}) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody("{\"example\":true}");
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Content-Security-Policy", "script-src 'self'");
    response->addHeader("X-Content-Type-Options", "invalid");
    plinth::security::apply_response_headers(response);
    plinth::security::apply_response_headers(response);
    REQUIRE(response->getHeader("X-Content-Type-Options") == "nosniff");
    REQUIRE(response->statusCode() == status);
    REQUIRE(response->getContentType() == drogon::CT_APPLICATION_JSON);
    REQUIRE(response->body() == "{\"example\":true}");
    REQUIRE(response->getHeader("Cache-Control") == "no-cache");
    REQUIRE(response->getHeader("Content-Security-Policy") ==
            "script-src 'self'");
  }
}

TEST_CASE("response headers keep an empty WebSocket upgrade empty",
          "[security][response-headers]") {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(drogon::k101SwitchingProtocols);
  response->setContentTypeCode(drogon::CT_NONE);
  response->addHeader("Upgrade", "websocket");
  plinth::security::apply_response_headers(response);
  REQUIRE(response->statusCode() == drogon::k101SwitchingProtocols);
  REQUIRE(response->body().empty());
  REQUIRE(response->getContentType() == drogon::CT_NONE);
  REQUIRE(response->getHeader("Upgrade") == "websocket");
  REQUIRE(response->getHeader("Content-Security-Policy").empty());
  REQUIRE(response->getHeader("X-Content-Type-Options") == "nosniff");
}

TEST_CASE("framework fallback is an inert constant plain-text 404",
          "[security][response-headers]") {
  auto response = plinth::security::make_not_found_response();
  REQUIRE(response->statusCode() == drogon::k404NotFound);
  REQUIRE(response->getContentType() == drogon::CT_TEXT_PLAIN);
  REQUIRE(response->body() == "not found");
  REQUIRE(response->getHeader("X-Content-Type-Options") == "nosniff");
  REQUIRE(response->getHeader("Server").empty());
}
