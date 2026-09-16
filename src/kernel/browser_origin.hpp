#pragma once

#include <drogon/HttpRequest.h>

#include <string_view>

namespace plinth {

// Browser origins are exact, serialized http(s) origins. They never contain
// credentials, paths, query strings, fragments, or a trailing slash.
auto valid_browser_origin(std::string_view origin) -> bool;

// Verify the request Origin against either the configured public origin or,
// when unset, the connection scheme plus Host. A configured origin still
// requires its authority to equal Host; forwarded headers are never trusted.
auto browser_origin_matches(const drogon::HttpRequestPtr& request,
                            std::string_view configured_origin) -> bool;

// Signals emitted automatically by browser fetch/navigation requests. This is
// used only to distinguish an Origin-less native API call from a browser-shaped
// public login/registration request.
auto browser_shaped_request(const drogon::HttpRequestPtr& request) -> bool;

} // namespace plinth
