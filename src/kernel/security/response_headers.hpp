#pragma once

#include <drogon/HttpResponse.h>

namespace plinth::security {

// A response-wide MIME boundary: keep each handler's status, content type and
// body, and tell the browser to honor that declared type.
auto apply_response_headers(const drogon::HttpResponsePtr& response) -> void;

// Framework fallbacks contain no active document or dependency information.
[[nodiscard]] auto make_not_found_response() -> drogon::HttpResponsePtr;

// Register once during main's startup, before the Drogon listener runs.
// Pre-sending advice covers handlers, static responses and WS handshakes.
auto register_response_headers() -> void;

} // namespace plinth::security
