#include "kernel/auth/middleware.hpp"
#include "kernel/auth/crypto.hpp"

#include <drogon/drogon.h>
#include <memory>
#include <spdlog/spdlog.h>

namespace plinth::auth {

namespace {

constexpr std::string_view PAT_PREFIX = "plinth_";

auto make_401(const std::string& error_code, const std::string& message)
    -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = error_code;
  json["message"] = message;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
  resp->setStatusCode(drogon::k401Unauthorized);
  return resp;
}

auto set_auth_attributes(const drogon::HttpRequestPtr& req,
                         const AuthContext& ctx) -> void {
  const auto& attrs = req->attributes();
  attrs->insert(ATTR_USER_ID, ctx.user_id);
  attrs->insert(ATTR_USERNAME, ctx.username);
  attrs->insert(ATTR_AUTH_TYPE, ctx.auth_type);
  attrs->insert(ATTR_SESSION_ID, ctx.session_id);
  attrs->insert(ATTR_PAT_ID, ctx.pat_id);
  attrs->insert(ATTR_TOKEN_HASH, ctx.token_hash);
}

// Map HTTP-friendly messages to error codes for the 401 path.
auto error_message(const std::string& code) -> std::string {
  if (code == "session_revoked") {
    return "Session has been revoked";
  }
  if (code == "session_expired") {
    return "Session has expired";
  }
  return "Invalid or missing authentication token";
}

} // namespace

auto get_auth_context(const drogon::HttpRequestPtr& req)
    -> std::optional<AuthContext> {
  const auto& attrs = req->attributes();
  auto user_id = attrs->get<std::string>(ATTR_USER_ID);
  if (user_id.empty()) {
    return std::nullopt;
  }
  return AuthContext{
      .user_id = user_id,
      .username = attrs->get<std::string>(ATTR_USERNAME),
      .auth_type = attrs->get<std::string>(ATTR_AUTH_TYPE),
      .session_id = attrs->get<std::string>(ATTR_SESSION_ID),
      .pat_id = attrs->get<std::string>(ATTR_PAT_ID),
      .token_hash = attrs->get<std::string>(ATTR_TOKEN_HASH),
  };
}

auto extract_token(const drogon::HttpRequestPtr& req)
    -> std::optional<std::string> {
  // Cookie takes precedence per ICD
  auto cookie = req->getCookie("plinth_session");
  if (!cookie.empty()) {
    return cookie;
  }

  // Fall back to Authorization: Bearer ...
  auto auth_header = req->getHeader("Authorization");
  if (auth_header.starts_with("Bearer ") && auth_header.size() > 7) {
    auto token = auth_header.substr(7);
    if (!token.empty()) {
      return token;
    }
  }

  return std::nullopt;
}

auto validate_session_token(const std::string& raw_token,
                            TokenValidationCallback cb) -> void {
  auto token_hash = sha256_hex(raw_token);
  auto shared_cb = std::make_shared<TokenValidationCallback>(std::move(cb));

  auto db = drogon::app().getDbClient();
  db->execSqlAsync(
      "SELECT s.id, s.user_id, u.username, s.revoked_at, "
      "       (s.expires_at <= NOW()) AS is_expired "
      "FROM plinth.sessions s "
      "JOIN plinth.users u ON u.id = s.user_id "
      "WHERE s.token_hash = $1",
      [shared_cb, token_hash](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*shared_cb)(
              {.ok = false, .context = {}, .error_code = "not_authenticated"});
          return;
        }

        auto row = result[0];

        if (!row["revoked_at"].isNull()) {
          (*shared_cb)(
              {.ok = false, .context = {}, .error_code = "session_revoked"});
          return;
        }

        if (row["is_expired"].as<bool>()) {
          (*shared_cb)(
              {.ok = false, .context = {}, .error_code = "session_expired"});
          return;
        }

        (*shared_cb)({
            .ok = true,
            .context =
                AuthContext{
                    .user_id = row["user_id"].as<std::string>(),
                    .username = row["username"].as<std::string>(),
                    .auth_type = "session",
                    .session_id = row["id"].as<std::string>(),
                    .pat_id = "",
                    .token_hash = token_hash,
                },
            .error_code = "",
        });
      },
      [shared_cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("session validation DB error: {}", e.base().what());
        (*shared_cb)(
            {.ok = false, .context = {}, .error_code = "not_authenticated"});
      },
      token_hash);
}

auto validate_pat_token(const std::string& raw_token,
                        TokenValidationCallback cb) -> void {
  if (!std::string_view{raw_token}.starts_with(PAT_PREFIX) ||
      raw_token.size() <= PAT_PREFIX.size()) {
    cb({.ok = false, .context = {}, .error_code = "not_authenticated"});
    return;
  }

  // Strip "plinth_" prefix, hash the random portion
  auto random_part = raw_token.substr(PAT_PREFIX.size());
  auto token_hash = sha256_hex(random_part);
  auto shared_cb = std::make_shared<TokenValidationCallback>(std::move(cb));

  auto db = drogon::app().getDbClient();
  db->execSqlAsync(
      "SELECT p.id, p.user_id, u.username "
      "FROM plinth.pats p "
      "JOIN plinth.users u ON u.id = p.user_id "
      "WHERE p.token_hash = $1 "
      "  AND p.revoked_at IS NULL "
      "  AND (p.expires_at IS NULL OR p.expires_at > NOW()) "
      "  AND u.disabled_at IS NULL",
      [shared_cb, token_hash, db](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*shared_cb)(
              {.ok = false, .context = {}, .error_code = "not_authenticated"});
          return;
        }

        auto row = result[0];
        auto pat_id = row["id"].as<std::string>();

        (*shared_cb)({
            .ok = true,
            .context =
                AuthContext{
                    .user_id = row["user_id"].as<std::string>(),
                    .username = row["username"].as<std::string>(),
                    .auth_type = "pat",
                    .session_id = "",
                    .pat_id = pat_id,
                    .token_hash = token_hash,
                },
            .error_code = "",
        });

        // Fire-and-forget last_used_at update
        db->execSqlAsync(
            "UPDATE plinth.pats SET last_used_at = NOW() "
            "WHERE id = $1::uuid",
            [](const drogon::orm::Result&) {},
            [pat_id](const drogon::orm::DrogonDbException& e) {
              spdlog::warn("PAT last_used_at update failed for {}: {}", pat_id,
                           e.base().what());
            },
            pat_id);
      },
      [shared_cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("PAT validation DB error: {}", e.base().what());
        (*shared_cb)(
            {.ok = false, .context = {}, .error_code = "not_authenticated"});
      },
      token_hash);
}

auto validate_token(const std::string& raw_token, TokenValidationCallback cb)
    -> void {
  if (std::string_view{raw_token}.starts_with(PAT_PREFIX) &&
      raw_token.size() > PAT_PREFIX.size()) {
    validate_pat_token(raw_token, std::move(cb));
  } else {
    validate_session_token(raw_token, std::move(cb));
  }
}

auto SessionFilter::doFilter(const drogon::HttpRequestPtr& req,
                             drogon::FilterCallback&& fcb,
                             drogon::FilterChainCallback&& fccb) -> void {
  auto raw_token = extract_token(req);
  if (!raw_token.has_value()) {
    fcb(make_401("not_authenticated", "No authentication token provided"));
    return;
  }

  auto shared_fcb = std::make_shared<drogon::FilterCallback>(std::move(fcb));
  auto shared_fccb =
      std::make_shared<drogon::FilterChainCallback>(std::move(fccb));

  validate_token(raw_token.value(), [req, shared_fcb, shared_fccb](
                                        const TokenValidationResult& result) {
    if (!result.ok) {
      (*shared_fcb)(
          make_401(result.error_code, error_message(result.error_code)));
      return;
    }
    set_auth_attributes(req, result.context);
    (*shared_fccb)();
  });
}

} // namespace plinth::auth
