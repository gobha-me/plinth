#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <string>
#include <string_view>

namespace plinth::security {

// A response-wide MIME boundary: keep each handler's status, content type and
// body, and tell the browser to honor that declared type.
auto apply_response_headers(const drogon::HttpResponsePtr& response) -> void;

// TLS-terminating proxies can bypass response modifiers while handing off an
// upgrade. Use only the operator's existing exact public HTTPS origin contract,
// matching Host and any supplied Origin; never infer TLS from forwarded
// headers.
auto apply_websocket_transport_headers(const drogon::HttpRequestPtr& request,
                                       const drogon::HttpResponsePtr& response,
                                       std::string_view configured_origin)
    -> void;

// Framework fallbacks contain no active document or dependency information.
[[nodiscard]] auto make_not_found_response() -> drogon::HttpResponsePtr;

// Register once during main's startup, before the Drogon listener runs.
// Pre-sending advice covers handlers, static responses and WS handshakes.
auto register_response_headers(std::string configured_origin) -> void;

} // namespace plinth::security
