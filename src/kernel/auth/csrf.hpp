#pragma once

#include <drogon/HttpFilter.h>

#include <optional>
#include <string>
#include <string_view>

namespace plinth::auth {

inline constexpr std::string_view CSRF_COOKIE = "plinth_csrf";
inline constexpr std::string_view CSRF_HEADER = "X-Plinth-CSRF";

// Deterministically derive the public double-submit value from the secret raw
// session token. The result is 32-byte HMAC-SHA256 encoded as 43 base64url
// characters without padding.
auto csrf_token_for_session(std::string_view raw_session_token) -> std::string;

// Read the server-derived expectation attached by SessionFilter. It exists
// only after successful cookie-backed session authentication.
auto request_expected_csrf_token(const drogon::HttpRequestPtr& request)
    -> std::optional<std::string>;

auto add_csrf_cookie(drogon::HttpResponsePtr& response,
                     const std::string& token, bool dev_mode) -> void;
auto clear_csrf_cookie(drogon::HttpResponsePtr& response) -> void;

// Configure the canonical browser origin once during route registration.
// Empty means the connection scheme plus Host. Forwarded headers are ignored.
auto configure_browser_origin(std::string origin) -> void;

class CsrfFilter : public drogon::HttpFilter<CsrfFilter, false> {
 public:
  auto doFilter(const drogon::HttpRequestPtr& request,
                drogon::FilterCallback&& filter_callback,
                drogon::FilterChainCallback&& chain_callback) -> void override;
};

class PublicOriginFilter
    : public drogon::HttpFilter<PublicOriginFilter, false> {
 public:
  auto doFilter(const drogon::HttpRequestPtr& request,
                drogon::FilterCallback&& filter_callback,
                drogon::FilterChainCallback&& chain_callback) -> void override;
};

} // namespace plinth::auth
