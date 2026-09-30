#include "kernel/security/response_headers.hpp"
#include "kernel/browser_origin.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

#include <utility>

namespace plinth::security {

auto apply_response_headers(const drogon::HttpResponsePtr& response) -> void {
  if (response && response->getHeader("X-Content-Type-Options") != "nosniff") {
    response->addHeader("X-Content-Type-Options", "nosniff");
  }
}

auto make_not_found_response() -> drogon::HttpResponsePtr {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(drogon::k404NotFound);
  response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
  response->setBody("not found");
  apply_response_headers(response);
  return response;
}

auto apply_websocket_transport_headers(const drogon::HttpRequestPtr& request,
                                       const drogon::HttpResponsePtr& response,
                                       std::string_view configured_origin)
    -> void {
  if (!request || !response ||
      response->statusCode() != drogon::k101SwitchingProtocols ||
      request->getMethod() != drogon::Get || request->path() != "/ws/events" ||
      !configured_origin.starts_with("https://") ||
      !plinth::valid_browser_origin(configured_origin)) {
    return;
  }
  const auto& host = request->getHeader("host");
  const auto& origin = request->getHeader("origin");
  const bool has_origin = request->headers().contains("origin");
  if (configured_origin.substr(8) != host ||
      (has_origin && origin != configured_origin)) {
    return;
  }
  response->addHeader("Strict-Transport-Security", "max-age=31536000");
}

auto register_response_headers(std::string configured_origin) -> void {
  drogon::app()
      .enableServerHeader(false)
      .setCustom404Page(make_not_found_response())
      .registerPreSendingAdvice([configured_origin =
                                     std::move(configured_origin)](
                                    const drogon::HttpRequestPtr& request,
                                    const drogon::HttpResponsePtr& response) {
        apply_response_headers(response);
        apply_websocket_transport_headers(request, response, configured_origin);
      });
}

} // namespace plinth::security
