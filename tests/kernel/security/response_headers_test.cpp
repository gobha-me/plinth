#include "kernel/security/response_headers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpTypes.h>

#include <string>

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

TEST_CASE("TLS proxy upgrade policy matches only its declared HTTPS authority",
          "[security][response-headers]") {
  struct UpgradeCase {
    std::string configured_origin;
    std::string host;
    std::string origin;
    std::string path;
    drogon::HttpStatusCode status;
    bool expected;
    bool has_origin = false;
    drogon::HttpMethod method = drogon::Get;
  };
  for (const auto& example : {
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443",
                       "https://plinth.example:8443", "/ws/events",
                       drogon::k101SwitchingProtocols, true},
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443", "",
                       "/ws/events", drogon::k101SwitchingProtocols, true},
           UpgradeCase{"", "plinth.example:8443", "https://plinth.example:8443",
                       "/ws/events", drogon::k101SwitchingProtocols, false},
           UpgradeCase{"http://plinth.example:8443", "plinth.example:8443",
                       "http://plinth.example:8443", "/ws/events",
                       drogon::k101SwitchingProtocols, false},
           UpgradeCase{"https://plinth.example:8443", "other.example:8443",
                       "https://plinth.example:8443", "/ws/events",
                       drogon::k101SwitchingProtocols, false},
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443",
                       "https://other.example:8443", "/ws/events",
                       drogon::k101SwitchingProtocols, false},
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443",
                       "https://plinth.example:8443", "/other",
                       drogon::k101SwitchingProtocols, false},
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443",
                       "https://plinth.example:8443", "/ws/events",
                       drogon::k200OK, false},
           UpgradeCase{"https://plinth.example:8443/", "plinth.example:8443",
                       "https://plinth.example:8443/", "/ws/events",
                       drogon::k101SwitchingProtocols, false},
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443", "",
                       "/ws/events", drogon::k101SwitchingProtocols, false,
                       true},
           UpgradeCase{"https://plinth.example:8443", "plinth.example:8443",
                       "https://plinth.example:8443", "/ws/events",
                       drogon::k101SwitchingProtocols, false, false,
                       drogon::Post},
       }) {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(example.method);
    request->setPath(example.path);
    request->addHeader("Host", example.host);
    if (!example.origin.empty() || example.has_origin) {
      request->addHeader("Origin", example.origin);
    }
    request->addHeader("Forwarded", "proto=https;host=plinth.example:8443");
    request->addHeader("X-Forwarded-Proto", "https");
    request->addHeader("X-Forwarded-Host", "plinth.example:8443");
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(example.status);
    response->setContentTypeCode(drogon::CT_NONE);
    response->addHeader("Upgrade", "websocket");
    response->addHeader("Sec-WebSocket-Accept", "preserved-handshake");
    if (example.expected) {
      response->addHeader("Strict-Transport-Security",
                          "max-age=7; includeSubDomains; preload");
    }
    plinth::security::apply_websocket_transport_headers(
        request, response, example.configured_origin);
    plinth::security::apply_websocket_transport_headers(
        request, response, example.configured_origin);
    REQUIRE(response->getHeader("Strict-Transport-Security") ==
            (example.expected ? "max-age=31536000" : ""));
    REQUIRE(response->statusCode() == example.status);
    REQUIRE(response->getContentType() == drogon::CT_NONE);
    REQUIRE(response->body().empty());
    REQUIRE(response->getHeader("Upgrade") == "websocket");
    REQUIRE(response->getHeader("Sec-WebSocket-Accept") ==
            "preserved-handshake");
    REQUIRE(response->getHeader("Content-Security-Policy").empty());
  }
}
