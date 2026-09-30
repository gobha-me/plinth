#include "kernel/security/response_headers.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

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

auto register_response_headers() -> void {
  drogon::app()
      .enableServerHeader(false)
      .setCustom404Page(make_not_found_response())
      .registerPreSendingAdvice([](const drogon::HttpRequestPtr&,
                                   const drogon::HttpResponsePtr& response) {
        apply_response_headers(response);
      });
}

} // namespace plinth::security
