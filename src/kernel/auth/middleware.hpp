#pragma once

#include <drogon/HttpFilter.h>
#include <functional>
#include <optional>
#include <string>

namespace plinth::auth {

// Request attribute keys set by the authentication middleware
inline constexpr auto ATTR_USER_ID = "plinth.user_id";
inline constexpr auto ATTR_USERNAME = "plinth.username";
inline constexpr auto ATTR_AUTH_TYPE = "plinth.auth_type";
inline constexpr auto ATTR_SESSION_ID = "plinth.session_id";
inline constexpr auto ATTR_PAT_ID = "plinth.pat_id";
inline constexpr auto ATTR_TOKEN_HASH = "plinth.token_hash";

struct AuthContext {
  std::string user_id;
  std::string username;
  std::string auth_type;  // "session" or "pat"
  std::string session_id; // empty for PAT auth
  std::string pat_id;     // empty for session auth
  std::string token_hash;
};

// Backward-compat alias
using SessionContext = AuthContext;

// Extract the AuthContext from a request that has passed through SessionFilter.
// Returns nullopt if the middleware has not run or authentication failed.
auto get_auth_context(const drogon::HttpRequestPtr& req)
    -> std::optional<AuthContext>;

// Backward-compat alias
inline auto get_session_context(const drogon::HttpRequestPtr& req)
    -> std::optional<AuthContext> {
  return get_auth_context(req);
}

// Drogon HTTP filter that validates session tokens and PATs.
// Extracts token from Cookie: plinth_session=... or Authorization: Bearer ...
// Bearer tokens starting with "plinth_" are routed to the PAT validation path.
// On success: attaches AuthContext attributes to the request.
// On failure: returns 401 JSON response.
class SessionFilter : public drogon::HttpFilter<SessionFilter, false> {
 public:
  auto doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                drogon::FilterChainCallback&& fccb) -> void override;
};

// Extract raw token from request (cookie or bearer header).
// Exported for testing.
auto extract_token(const drogon::HttpRequestPtr& req)
    -> std::optional<std::string>;

// ── Token validation primitives ──────────────────────────────────────
//
// Used by both the HTTP SessionFilter and the WebSocket auth flow. The
// validators do the DB lookup, build an AuthContext on success, or return
// an error_code string on failure. Errors map to the existing 401 codes:
//   "not_authenticated", "session_revoked", "session_expired"

struct TokenValidationResult {
  bool ok{false};
  AuthContext context;    // valid only if ok
  std::string error_code; // valid only if !ok
};

using TokenValidationCallback = std::function<void(TokenValidationResult)>;

// Validate a session token (raw, not yet hashed). Async DB call.
auto validate_session_token(const std::string& raw_token,
                            TokenValidationCallback cb) -> void;

// Validate a PAT (raw, must be prefixed with "plinth_"). Async DB call.
// Also fires a non-blocking last_used_at update on success.
auto validate_pat_token(const std::string& raw_token,
                        TokenValidationCallback cb) -> void;

// Dispatch by prefix: tokens starting with "plinth_" go to validate_pat_token,
// all others to validate_session_token.
auto validate_token(const std::string& raw_token, TokenValidationCallback cb)
    -> void;

} // namespace plinth::auth
