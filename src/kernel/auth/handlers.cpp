#include "kernel/auth/handlers.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/auth/rate_limiter.hpp"
#include "kernel/logging.hpp"

#include <drogon/drogon.h>
#include <memory>
#include <spdlog/spdlog.h>

namespace plinth::auth {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
using SharedCb = std::shared_ptr<Callback>;

auto share(Callback&& cb) -> SharedCb {
  return std::make_shared<Callback>(std::move(cb));
}

// Module-level rate limiter (single-node, in-memory)
RateLimiter rate_limiter;

auto json_error(drogon::HttpStatusCode status, const std::string& error_code,
                const std::string& message) -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = error_code;
  json["message"] = message;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
  resp->setStatusCode(status);
  return resp;
}

auto set_session_cookie(drogon::HttpResponsePtr& resp, const std::string& token,
                        bool dev_mode) -> void {
  auto cookie = drogon::Cookie("plinth_session", token);
  cookie.setPath("/");
  cookie.setHttpOnly(true);
  cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
  if (!dev_mode) {
    cookie.setSecure(true);
  }
  cookie.setMaxAge(86400);
  resp->addCookie(cookie);
}

auto clear_session_cookie(drogon::HttpResponsePtr& resp) -> void {
  auto cookie = drogon::Cookie("plinth_session", "");
  cookie.setPath("/");
  cookie.setHttpOnly(true);
  cookie.setMaxAge(0);
  resp->addCookie(cookie);
}

auto get_client_ip(const drogon::HttpRequestPtr& req) -> std::string {
  return req->peerAddr().toIp();
}

// ── Registration helpers ─────────────────────────────────────────────

auto respond_registered(const std::string& user_id, const std::string& username,
                        const std::string& created_at)
    -> drogon::HttpResponsePtr {
  Json::Value body;
  body["id"] = user_id;
  body["username"] = username;
  body["created_at"] = created_at;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(drogon::k201Created);
  return resp;
}

// Adds the first user to the admin group, then responds 201.
// The admin group is guaranteed to exist (created by bootstrap_groups()).
auto ensure_first_user_admin(const drogon::orm::DbClientPtr& db,
                             const std::string& user_id,
                             const std::string& username,
                             const std::string& created_at,
                             const std::string& ip, const SharedCb& cb)
    -> void {
  db->execSqlAsync(
      "SELECT id FROM plinth.groups WHERE name = 'admin'",
      [db, user_id, username, created_at, ip,
       cb](const drogon::orm::Result& grp_result) {
        if (grp_result.empty()) {
          spdlog::error(
              "admin group not found — bootstrap_groups() may not have run");
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Registration failed"));
          return;
        }
        auto group_id = grp_result[0]["id"].as<std::string>();
        db->execSqlAsync(
            "INSERT INTO plinth.group_members (group_id, user_id) "
            "VALUES ($1::uuid, $2::uuid) "
            "ON CONFLICT DO NOTHING",
            [user_id, username, created_at, ip,
             cb](const drogon::orm::Result&) {
              Json::Value detail;
              detail["username"] = username;
              detail["first_user"] = true;
              plinth::log::audit(
                  "user.registered", detail,
                  {.user_id = user_id, .session_id = "", .ip_address = ip});
              (*cb)(respond_registered(user_id, username, created_at));
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("admin membership insert failed: {}",
                            e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Registration failed"));
            },
            group_id, user_id);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("admin group lookup failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Registration failed"));
      });
}

auto handle_insert_error(const SharedCb& cb,
                         const drogon::orm::DrogonDbException& e) -> void {
  auto msg = std::string{e.base().what()};
  if (msg.find("unique") != std::string::npos ||
      msg.find("duplicate") != std::string::npos) {
    (*cb)(json_error(drogon::k409Conflict, "username_taken",
                     "Username already exists"));
  } else {
    spdlog::error("user insert failed: {}", msg);
    (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                     "Registration failed"));
  }
}

// After user INSERT RETURNING: check if first user, set up admin group if so.
auto complete_user_registration(const drogon::orm::DbClientPtr& db,
                                const std::string& user_id,
                                const std::string& username,
                                const std::string& created_at,
                                const std::string& ip, const SharedCb& cb)
    -> void {
  db->execSqlAsync(
      "SELECT COUNT(*) AS cnt FROM plinth.users WHERE is_test_user = false",
      [db, user_id, username, created_at, ip,
       cb](const drogon::orm::Result& cnt_result) {
        if (cnt_result[0]["cnt"].as<int64_t>() == 1) {
          ensure_first_user_admin(db, user_id, username, created_at, ip, cb);
        } else {
          Json::Value detail;
          detail["username"] = username;
          detail["first_user"] = false;
          plinth::log::audit(
              "user.registered", detail,
              {.user_id = user_id, .session_id = "", .ip_address = ip});
          (*cb)(respond_registered(user_id, username, created_at));
        }
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("user count failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Registration failed"));
      });
}

// Insert user row, then complete registration (admin check, audit, respond).
auto insert_user_then_complete(const drogon::orm::DbClientPtr& db,
                               const std::string& username,
                               const std::string& password_hash,
                               const std::string& ip, const SharedCb& cb)
    -> void {
  db->execSqlAsync(
      "INSERT INTO plinth.users (username, password_hash) "
      "VALUES ($1, $2) RETURNING id, username, created_at",
      [db, ip, cb](const drogon::orm::Result& result) {
        auto row = result[0];
        complete_user_registration(db, row["id"].as<std::string>(),
                                   row["username"].as<std::string>(),
                                   row["created_at"].as<std::string>(), ip, cb);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        handle_insert_error(cb, e);
      },
      username, password_hash);
}

// ── Route handlers ───────────────────────────────────────────────────

auto handle_register(const drogon::HttpRequestPtr& req, Callback&& callback,
                     bool registration_enabled) -> void {
  auto json = req->getJsonObject();
  if (!json) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto username = (*json)["username"].asString();
  auto password = (*json)["password"].asString();

  if (username.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_username",
                                   "Username is required"));
    return;
  }
  if (password.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_password",
                                   "Password is required"));
    return;
  }
  if (auto err = validate_username(username); err.has_value()) {
    std::move(callback)(
        json_error(drogon::k400BadRequest, err.value(), "Invalid username"));
    return;
  }
  if (auto err = validate_password(password); err.has_value()) {
    std::move(callback)(
        json_error(drogon::k400BadRequest, err.value(), "Invalid password"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));

  if (!registration_enabled) {
    // Allow registration if no users exist yet (first-user bootstrap)
    db->execSqlAsync(
        "SELECT COUNT(*) AS cnt FROM plinth.users WHERE is_test_user = false",
        [db, username, password, ip, cb](const drogon::orm::Result& result) {
          if (result[0]["cnt"].as<int64_t>() > 0) {
            (*cb)(json_error(drogon::k403Forbidden, "registration_disabled",
                             "Registration is disabled"));
            return;
          }
          // First user — insert and go straight to admin setup
          auto password_hash = hash_password(password);
          db->execSqlAsync(
              "INSERT INTO plinth.users (username, password_hash) "
              "VALUES ($1, $2) RETURNING id, username, created_at",
              [db, ip, cb](const drogon::orm::Result& res) {
                auto row = res[0];
                ensure_first_user_admin(db, row["id"].as<std::string>(),
                                        row["username"].as<std::string>(),
                                        row["created_at"].as<std::string>(), ip,
                                        cb);
              },
              [cb](const drogon::orm::DrogonDbException& e) {
                handle_insert_error(cb, e);
              },
              username, password_hash);
        },
        [cb](const drogon::orm::DrogonDbException& e) {
          spdlog::error("user count check failed: {}", e.base().what());
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Registration failed"));
        });
    return;
  }

  // Registration enabled — standard flow
  auto password_hash = hash_password(password);

  // Check for disabled_at (username reuse prevention)
  db->execSqlAsync(
      "SELECT disabled_at FROM plinth.users WHERE username = $1",
      [db, username, password_hash, ip, cb](const drogon::orm::Result& result) {
        if (!result.empty() && !result[0]["disabled_at"].isNull()) {
          (*cb)(json_error(drogon::k409Conflict, "username_taken",
                           "Username is unavailable"));
          return;
        }
        insert_user_then_complete(db, username, password_hash, ip, cb);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("disabled check failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Registration failed"));
      },
      username);
}

auto handle_login(const drogon::HttpRequestPtr& req, Callback&& callback,
                  bool dev_mode) -> void {
  auto json = req->getJsonObject();
  if (!json) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto username = (*json)["username"].asString();
  auto password = (*json)["password"].asString();

  if (username.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_username",
                                   "Username is required"));
    return;
  }
  if (password.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_password",
                                   "Password is required"));
    return;
  }

  auto ip = get_client_ip(req);

  // Rate limit check
  if (!rate_limiter.check(ip)) {
    auto wait = rate_limiter.retry_after(ip);
    auto resp = json_error(drogon::k429TooManyRequests, "rate_limited",
                           "Too many failed login attempts");
    resp->addHeader("Retry-After", std::to_string(wait));
    std::move(callback)(resp);
    return;
  }

  auto user_agent = req->getHeader("User-Agent");
  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "SELECT id, password_hash, disabled_at FROM plinth.users "
      "WHERE username = $1",
      [db, username, password, ip, user_agent, dev_mode,
       cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          // Run dummy hash to prevent timing side-channel
          dummy_hash();
          rate_limiter.record_failure(ip);

          Json::Value detail;
          detail["username"] = username;
          detail["reason"] = "user_not_found";
          plinth::log::audit(
              "user.login_failed", detail,
              {.user_id = "", .session_id = "", .ip_address = ip});

          (*cb)(json_error(drogon::k401Unauthorized, "invalid_credentials",
                           "Invalid username or password"));
          return;
        }

        auto row = result[0];
        auto user_id = row["id"].as<std::string>();
        auto password_hash = row["password_hash"].as<std::string>();

        // Check disabled
        if (!row["disabled_at"].isNull()) {
          dummy_hash();
          (*cb)(json_error(drogon::k403Forbidden, "account_disabled",
                           "Account is disabled"));
          return;
        }

        if (!verify_password(password, password_hash)) {
          rate_limiter.record_failure(ip);

          Json::Value detail;
          detail["username"] = username;
          detail["reason"] = "wrong_password";
          plinth::log::audit(
              "user.login_failed", detail,
              {.user_id = user_id, .session_id = "", .ip_address = ip});

          (*cb)(json_error(drogon::k401Unauthorized, "invalid_credentials",
                           "Invalid username or password"));
          return;
        }

        // Generate session token
        auto raw_token = generate_token();
        auto token_hash = sha256_hex(raw_token);

        db->execSqlAsync(
            "INSERT INTO plinth.sessions "
            "(user_id, token_hash, user_agent, ip_address) "
            "VALUES ($1::uuid, $2, $3, $4::inet) "
            "RETURNING id, expires_at",
            [user_id, username, raw_token, ip, dev_mode,
             cb](const drogon::orm::Result& sess_result) {
              auto sess_row = sess_result[0];
              auto session_id = sess_row["id"].as<std::string>();
              auto expires_at = sess_row["expires_at"].as<std::string>();

              Json::Value detail;
              detail["username"] = username;
              plinth::log::audit("user.login", detail,
                                 {.user_id = user_id,
                                  .session_id = session_id,
                                  .ip_address = ip});

              Json::Value body;
              body["user"]["id"] = user_id;
              body["user"]["username"] = username;
              body["session"]["id"] = session_id;
              body["session"]["expires_at"] = expires_at;

              auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
              resp->setStatusCode(drogon::k200OK);
              set_session_cookie(resp, raw_token, dev_mode);
              (*cb)(resp);
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("session insert failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Login failed"));
            },
            user_id, token_hash, user_agent, ip);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("user lookup failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Login failed"));
      },
      username);
}

auto handle_logout(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "UPDATE plinth.sessions SET revoked_at = NOW() "
      "WHERE id = $1::uuid AND revoked_at IS NULL",
      [ctx_val = ctx.value(), ip, cb](const drogon::orm::Result&) {
        Json::Value detail;
        detail["username"] = ctx_val.username;
        plinth::log::audit("user.logout", detail,
                           {.user_id = ctx_val.user_id,
                            .session_id = ctx_val.session_id,
                            .ip_address = ip});

        Json::Value body;
        body["status"] = "logged_out";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        clear_session_cookie(resp);
        (*cb)(resp);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("logout failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Logout failed"));
      },
      ctx.value().session_id);
}

auto handle_get_session(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "SELECT u.id AS user_id, u.username, u.created_at AS user_created, "
      "       s.id AS session_id, s.created_at AS session_created, "
      "       s.expires_at, s.user_agent, s.ip_address "
      "FROM plinth.users u "
      "JOIN plinth.sessions s ON s.user_id = u.id "
      "WHERE s.id = $1::uuid",
      [cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k401Unauthorized, "not_authenticated",
                           "Session not found"));
          return;
        }

        auto row = result[0];
        Json::Value body;
        body["user"]["id"] = row["user_id"].as<std::string>();
        body["user"]["username"] = row["username"].as<std::string>();
        body["user"]["created_at"] = row["user_created"].as<std::string>();
        body["session"]["id"] = row["session_id"].as<std::string>();
        body["session"]["created_at"] =
            row["session_created"].as<std::string>();
        body["session"]["expires_at"] = row["expires_at"].as<std::string>();
        if (!row["user_agent"].isNull()) {
          body["session"]["user_agent"] = row["user_agent"].as<std::string>();
        }
        if (!row["ip_address"].isNull()) {
          body["session"]["ip_address"] = row["ip_address"].as<std::string>();
        }

        (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("session query failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to retrieve session"));
      },
      ctx.value().session_id);
}

auto handle_delete_session(const drogon::HttpRequestPtr& req,
                           Callback&& callback,
                           const std::string& target_session_id) -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));

  // Check ownership or admin status
  db->execSqlAsync(
      "SELECT s.user_id, "
      "       EXISTS(SELECT 1 FROM plinth.group_members gm "
      "              JOIN plinth.groups g ON g.id = gm.group_id "
      "              WHERE gm.user_id = $2::uuid AND g.name = 'admin') "
      "       AS is_admin "
      "FROM plinth.sessions s "
      "WHERE s.id = $1::uuid AND s.revoked_at IS NULL",
      [db, ctx_val = ctx.value(), target_session_id, ip,
       cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "session_not_found",
                           "Session not found"));
          return;
        }

        auto row = result[0];
        auto session_owner = row["user_id"].as<std::string>();
        auto is_admin = row["is_admin"].as<bool>();

        if (session_owner != ctx_val.user_id && !is_admin) {
          (*cb)(json_error(drogon::k403Forbidden, "forbidden",
                           "Cannot revoke another user's session"));
          return;
        }

        db->execSqlAsync(
            "UPDATE plinth.sessions SET revoked_at = NOW() "
            "WHERE id = $1::uuid AND revoked_at IS NULL",
            [ctx_val, target_session_id, ip, cb](const drogon::orm::Result&) {
              Json::Value detail;
              detail["target_session_id"] = target_session_id;
              plinth::log::audit("session.revoked", detail,
                                 {.user_id = ctx_val.user_id,
                                  .session_id = ctx_val.session_id,
                                  .ip_address = ip});

              Json::Value body;
              body["status"] = "revoked";
              (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("session revoke failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to revoke session"));
            },
            target_session_id);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("session lookup failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to revoke session"));
      },
      target_session_id, ctx.value().user_id);
}

auto handle_list_sessions(const drogon::HttpRequestPtr& req,
                          Callback&& callback) -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto current_token_hash = ctx.value().token_hash;
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "SELECT id, user_agent, ip_address, created_at, expires_at, token_hash "
      "FROM plinth.sessions "
      "WHERE user_id = $1::uuid AND revoked_at IS NULL AND expires_at > NOW() "
      "ORDER BY created_at DESC",
      [current_token_hash, cb](const drogon::orm::Result& result) {
        Json::Value sessions(Json::arrayValue);
        for (const auto& row : result) {
          Json::Value s;
          s["id"] = row["id"].as<std::string>();
          if (!row["user_agent"].isNull()) {
            s["user_agent"] = row["user_agent"].as<std::string>();
          }
          if (!row["ip_address"].isNull()) {
            s["ip_address"] = row["ip_address"].as<std::string>();
          }
          s["created_at"] = row["created_at"].as<std::string>();
          s["expires_at"] = row["expires_at"].as<std::string>();
          s["is_current"] =
              (row["token_hash"].as<std::string>() == current_token_hash);
          sessions.append(s);
        }

        Json::Value body;
        body["sessions"] = sessions;
        (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("sessions list failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to list sessions"));
      },
      ctx.value().user_id);
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

auto register_auth_routes(bool dev_mode, bool registration_enabled) -> void {
  drogon::app().registerHandler(
      "/api/auth/register",
      [registration_enabled](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_register(req, std::move(callback), registration_enabled);
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/auth/login",
      [dev_mode](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_login(req, std::move(callback), dev_mode);
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/auth/logout",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_logout(req, std::move(callback));
      },
      {drogon::Post, "plinth::auth::SessionFilter"});

  drogon::app().registerHandler(
      "/api/auth/session",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_get_session(req, std::move(callback));
      },
      {drogon::Get, "plinth::auth::SessionFilter"});

  drogon::app().registerHandler(
      "/api/auth/session/{id}",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback,
         const std::string& target_session_id) {
        handle_delete_session(req, std::move(callback), target_session_id);
      },
      {drogon::Delete, "plinth::auth::SessionFilter"});

  drogon::app().registerHandler(
      "/api/auth/sessions",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_list_sessions(req, std::move(callback));
      },
      {drogon::Get, "plinth::auth::SessionFilter"});

  spdlog::info("auth routes registered (registration_enabled={})",
               registration_enabled);
}

} // namespace plinth::auth
