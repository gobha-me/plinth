#include "kernel/auth/handlers.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/auth/csrf.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/auth/rate_limiter.hpp"
#include "kernel/config.hpp"
#include "kernel/logging.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <drogon/drogon.h>
#include <exception>
#include <memory>
#include <openssl/crypto.h>
#include <set>
#include <spdlog/spdlog.h>

namespace plinth::auth {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
using SharedCb = std::shared_ptr<Callback>;
using TransactionPtr = std::shared_ptr<drogon::orm::Transaction>;

auto share(Callback&& cb) -> SharedCb {
  return std::make_shared<Callback>(std::move(cb));
}

// Login throttles are deliberately process-local and use only the trusted
// socket peer plus a digest of the submitted subject. Both maps are bounded;
// deployments behind a proxy additionally rate-limit at the trusted edge.
std::unique_ptr<RateLimiter> login_source_limiter;
std::unique_ptr<RateLimiter> login_subject_limiter;
std::unique_ptr<RateLimiter> registration_source_limiter;
std::unique_ptr<RateLimiter> registration_subject_limiter;
std::unique_ptr<RateLimiter> registration_global_limiter;
std::unique_ptr<RateLimiter> bootstrap_source_limiter;
std::unique_ptr<RateLimiter> bootstrap_global_limiter;
std::atomic<unsigned int> active_password_hashes{0};

constexpr unsigned int MAX_PASSWORD_HASHES = 2;

auto try_acquire_password_hash_slot() -> bool {
  auto current = active_password_hashes.load(std::memory_order_relaxed);
  while (current < MAX_PASSWORD_HASHES) {
    if (active_password_hashes.compare_exchange_weak(
            current, current + 1, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

auto release_password_hash_slot() -> void {
  active_password_hashes.fetch_sub(1, std::memory_order_release);
}

class HashPermit {
 public:
  HashPermit() : acquired_(try_acquire_password_hash_slot()) {}
  ~HashPermit() {
    if (acquired_) {
      release_password_hash_slot();
    }
  }
  HashPermit(const HashPermit&) = delete;
  auto operator=(const HashPermit&) -> HashPermit& = delete;
  [[nodiscard]] auto acquired() const -> bool { return acquired_; }

 private:
  bool acquired_{false};
};

auto json_error(drogon::HttpStatusCode status, const std::string& error_code,
                const std::string& message) -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = error_code;
  json["message"] = message;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
  resp->setStatusCode(status);
  harden_auth_response(resp, true);
  return resp;
}

auto rate_limited_response(const std::string& message, int retry_after)
    -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = "rate_limited";
  json["message"] = message;
  json["retry_after"] = retry_after;
  auto response = drogon::HttpResponse::newHttpJsonResponse(json);
  response->setStatusCode(drogon::k429TooManyRequests);
  response->addHeader("Retry-After", std::to_string(retry_after));
  harden_auth_response(response, true);
  return response;
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

auto has_exact_fields(const Json::Value& json,
                      std::initializer_list<std::string_view> required,
                      std::initializer_list<std::string_view> optional = {})
    -> bool {
  if (!json.isObject()) {
    return false;
  }
  std::set<std::string> allowed;
  for (const auto field : required) {
    allowed.emplace(field);
    if (!json.isMember(std::string{field}) ||
        !json[std::string{field}].isString()) {
      return false;
    }
  }
  for (const auto field : optional) {
    allowed.emplace(field);
  }
  for (const auto& field : json.getMemberNames()) {
    if (!allowed.contains(field)) {
      return false;
    }
  }
  return true;
}

auto processed_response() -> drogon::HttpResponsePtr {
  Json::Value body;
  body["status"] = "processed";
  auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(drogon::k202Accepted);
  harden_auth_response(resp, true);
  return resp;
}

auto handle_insert_error(const SharedCb& cb,
                         const drogon::orm::DrogonDbException&) -> void {
  spdlog::error("bootstrap user insert failed");
  (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                   "Bootstrap failed"));
}

auto respond_registered(const std::string& user_id, const std::string& username,
                        const std::string& created_at)
    -> drogon::HttpResponsePtr {
  Json::Value body;
  body["id"] = user_id;
  body["username"] = username;
  body["created_at"] = created_at;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(drogon::k201Created);
  harden_auth_response(resp, true);
  return resp;
}

// This callback deliberately does not own tx. Releasing its last query owner
// requests COMMIT; only PostgreSQL's commit acknowledgment may publish 201.
auto finish_registration(const TransactionPtr& tx, const std::string& user_id,
                         const std::string& username,
                         const std::string& created_at, const std::string& ip,
                         bool first_user, const SharedCb& cb) -> void {
  tx->setCommitCallback(
      [user_id, username, created_at, ip, first_user, cb](bool committed) {
        if (!committed) {
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Registration failed"));
          return;
        }
        Json::Value detail;
        detail["mode"] = "bootstrap";
        detail["first_user"] = first_user;
        plinth::log::audit(
            "user.registered", detail,
            {.user_id = user_id, .session_id = "", .ip_address = ip});
        (*cb)(respond_registered(user_id, username, created_at));
      });
}

auto finish_processed(const TransactionPtr& tx, const SharedCb& cb,
                      const std::string& user_id = {},
                      std::string_view mode = {}, const std::string& ip = {})
    -> void {
  tx->setCommitCallback(
      [cb, user_id, mode = std::string{mode}, ip](bool committed) {
        if (!committed) {
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Registration failed"));
          return;
        }
        if (!user_id.empty()) {
          Json::Value detail;
          detail["mode"] = mode;
          plinth::log::audit(
              "user.registered", detail,
              {.user_id = user_id, .session_id = "", .ip_address = ip});
        }
        (*cb)(processed_response());
      });
}

auto insert_registered_user(const TransactionPtr& tx,
                            const std::string& username,
                            const std::string& password_hash,
                            const std::string& ip, bool first_user,
                            const SharedCb& cb) -> void {
  tx->execSqlAsync(
      "INSERT INTO plinth.users (username, password_hash) "
      "VALUES ($1, $2) RETURNING id, username, created_at",
      [tx, ip, first_user, cb](const drogon::orm::Result& result) {
        const auto user_id = result[0]["id"].as<std::string>();
        const auto username = result[0]["username"].as<std::string>();
        const auto created_at = result[0]["created_at"].as<std::string>();
        if (!first_user) {
          finish_registration(tx, user_id, username, created_at, ip, false, cb);
          return;
        }
        tx->execSqlAsync(
            "INSERT INTO plinth.group_members (group_id, user_id) "
            "SELECT id, $1::uuid FROM plinth.groups WHERE name = 'admin' "
            "RETURNING group_id",
            [tx, user_id, username, created_at, ip,
             cb](const drogon::orm::Result& membership) {
              if (membership.size() != 1) {
                tx->rollback();
                spdlog::error("admin group missing during first registration");
                (*cb)(json_error(drogon::k500InternalServerError,
                                 "internal_error", "Registration failed"));
                return;
              }
              finish_registration(tx, user_id, username, created_at, ip, true,
                                  cb);
            },
            [cb](const drogon::orm::DrogonDbException&) {
              // Drogon rolls a failed statement's transaction back before its
              // owners release. The user and membership cannot commit apart.
              spdlog::error("admin membership insert failed");
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Registration failed"));
            },
            user_id);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        handle_insert_error(cb, e);
      },
      username, password_hash);
}

auto bootstrap_user_atomic(const drogon::orm::DbClientPtr& db,
                           const std::string& username,
                           const std::string& password_hash,
                           const std::string& ip, const SharedCb& cb) -> void {
  db->newTransactionAsync(
      [username, password_hash, ip, cb](const TransactionPtr& tx) {
        if (!tx) {
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Registration failed"));
          return;
        }
        tx->setTimeout(5.0);
        auto on_error = [cb](const drogon::orm::DrogonDbException&) {
          spdlog::error("bootstrap transaction failed");
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Registration failed"));
        };
        // Count in a fresh statement after acquiring the lock. A snapshot
        // taken before waiting could miss the preceding winner's committed row.
        tx->execSqlAsync(
            "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
            [tx, username, password_hash, ip, cb,
             on_error](const drogon::orm::Result&) {
              tx->execSqlAsync(
                  "SELECT pg_advisory_xact_lock("
                  "hashtextextended('plinth.auth.registration', 0))",
                  [tx, username, password_hash, ip, cb,
                   on_error](const drogon::orm::Result&) {
                    tx->execSqlAsync(
                        "SELECT COUNT(*) AS cnt FROM plinth.users "
                        "WHERE is_test_user = false",
                        [tx, username, password_hash, ip,
                         cb](const drogon::orm::Result& count) {
                          if (count[0]["cnt"].as<int64_t>() != 0) {
                            tx->rollback();
                            (*cb)(json_error(drogon::k409Conflict,
                                             "bootstrap_closed",
                                             "Bootstrap is already complete"));
                            return;
                          }
                          insert_registered_user(tx, username, password_hash,
                                                 ip, true, cb);
                        },
                        on_error);
                  },
                  on_error);
            },
            on_error);
      });
}

auto finish_invite_use(const TransactionPtr& tx, const std::string& invite_id,
                       const std::string& user_id, const SharedCb& cb,
                       bool created, const std::string& ip) -> void {
  tx->execSqlAsync(
      "UPDATE plinth.registration_invites SET used_at=NOW(), "
      "used_by_user_id=$2::uuid WHERE id=$1::uuid",
      [tx, user_id, cb, created, ip](const drogon::orm::Result&) {
        finish_processed(tx, cb, created ? user_id : "", "invite", ip);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("invite consumption failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Registration failed"));
      },
      invite_id, user_id);
}

auto insert_ordinary_user(const TransactionPtr& tx, const std::string& username,
                          const std::string& password_hash,
                          const std::string& invite_id, const SharedCb& cb,
                          std::string_view mode, const std::string& ip)
    -> void {
  tx->execSqlAsync(
      "INSERT INTO plinth.users (username, password_hash) VALUES ($1,$2) "
      "RETURNING id",
      [tx, invite_id, cb, ip,
       mode = std::string{mode}](const drogon::orm::Result& result) {
        const auto user_id = result[0]["id"].as<std::string>();
        if (!invite_id.empty()) {
          finish_invite_use(tx, invite_id, user_id, cb, true, ip);
          return;
        }
        finish_processed(tx, cb, user_id, mode, ip);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("registration insert failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Registration failed"));
      },
      username, password_hash);
}

auto register_after_invite(const TransactionPtr& tx,
                           const std::string& username,
                           const std::string& password_hash,
                           const std::string& invite_id, const SharedCb& cb,
                           const std::string& ip) -> void {
  tx->execSqlAsync(
      "SELECT id FROM plinth.users WHERE username=$1",
      [tx, username, password_hash, invite_id, ip,
       cb](const drogon::orm::Result& existing) {
        if (!existing.empty()) {
          finish_invite_use(tx, invite_id, existing[0]["id"].as<std::string>(),
                            cb, false, ip);
          return;
        }
        insert_ordinary_user(tx, username, password_hash, invite_id, cb,
                             "invite", ip);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("registration subject lookup failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Registration failed"));
      },
      username);
}

auto register_user_atomic(const drogon::orm::DbClientPtr& db,
                          const std::string& username,
                          const std::string& password_hash,
                          const std::string& invite_hash,
                          const Config::Registration& registration,
                          const std::string& ip, const SharedCb& cb) -> void {
  db->newTransactionAsync([username, password_hash, invite_hash, registration,
                           ip, cb](const TransactionPtr& tx) {
    if (!tx) {
      (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                       "Registration failed"));
      return;
    }
    tx->setTimeout(5.0);
    auto on_error = [cb](const drogon::orm::DrogonDbException&) {
      spdlog::error("registration transaction failed");
      (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                       "Registration failed"));
    };
    tx->execSqlAsync(
        "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
        [tx, username, password_hash, invite_hash, registration, ip, cb,
         on_error](const drogon::orm::Result&) {
          tx->execSqlAsync(
              "SELECT pg_advisory_xact_lock("
              "hashtextextended('plinth.auth.registration',0))",
              [tx, username, password_hash, invite_hash, registration, ip, cb,
               on_error](const drogon::orm::Result&) {
                tx->execSqlAsync(
                    "SELECT COUNT(*) AS cnt FROM plinth.users WHERE NOT "
                    "is_test_user",
                    [tx, username, password_hash, invite_hash, registration, ip,
                     cb, on_error](const drogon::orm::Result& count) {
                      const auto accounts = count[0]["cnt"].as<std::size_t>();
                      // Registration never bootstraps authority, even in open
                      // mode. A secret-authorized bootstrap must win first.
                      if (accounts == 0 ||
                          registration.mode ==
                              Config::Registration::Mode::DISABLED) {
                        tx->rollback();
                        (*cb)(processed_response());
                        return;
                      }
                      if (accounts >= registration.max_accounts) {
                        tx->rollback();
                        (*cb)(processed_response());
                        return;
                      }
                      if (registration.mode ==
                          Config::Registration::Mode::OPEN) {
                        tx->execSqlAsync(
                            "SELECT id FROM plinth.users WHERE username=$1",
                            [tx, username, password_hash, ip,
                             cb](const drogon::orm::Result& existing) {
                              if (!existing.empty()) {
                                tx->rollback();
                                (*cb)(processed_response());
                                return;
                              }
                              insert_ordinary_user(tx, username, password_hash,
                                                   "", cb, "open", ip);
                            },
                            on_error, username);
                        return;
                      }
                      tx->execSqlAsync(
                          "SELECT id FROM plinth.registration_invites "
                          "WHERE token_hash=$1 AND revoked_at IS NULL AND "
                          "used_at IS NULL AND expires_at>NOW() FOR UPDATE",
                          [tx, username, password_hash, ip,
                           cb](const drogon::orm::Result& invite) {
                            if (invite.empty()) {
                              tx->rollback();
                              (*cb)(processed_response());
                              return;
                            }
                            register_after_invite(
                                tx, username, password_hash,
                                invite[0]["id"].as<std::string>(), cb, ip);
                          },
                          on_error, invite_hash);
                    },
                    on_error);
              },
              on_error);
        },
        on_error);
  });
}

// ── Route handlers ───────────────────────────────────────────────────

auto handle_register(const drogon::HttpRequestPtr& req, Callback&& callback,
                     const Config::Registration& registration) -> void {
  if (registration.mode == Config::Registration::Mode::DISABLED) {
    std::move(callback)(json_error(drogon::k403Forbidden,
                                   "registration_unavailable",
                                   "Registration is unavailable"));
    return;
  }
  auto json = req->getJsonObject();
  if (!json ||
      !has_exact_fields(*json, {"username", "password"}, {"invite_token"}) ||
      (json->isMember("invite_token") && !(*json)["invite_token"].isString())) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto username = (*json)["username"].asString();
  auto password = (*json)["password"].asString();
  auto invite_token = (*json)["invite_token"].asString();

  if (invite_token.size() > 256) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid registration request"));
    return;
  }

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

  auto ip = get_client_ip(req);
  auto subject = sha256_hex(username);
  if (!registration_source_limiter->consume(ip) ||
      !registration_subject_limiter->consume(subject) ||
      !registration_global_limiter->consume("global")) {
    auto wait = std::max({registration_source_limiter->retry_after(ip),
                          registration_subject_limiter->retry_after(subject),
                          registration_global_limiter->retry_after("global")});
    auto response =
        rate_limited_response("Too many registration attempts", wait);
    std::move(callback)(response);
    return;
  }
  HashPermit permit;
  if (!permit.acquired()) {
    auto response = rate_limited_response("Registration is busy", 1);
    std::move(callback)(response);
    return;
  }
  auto password_hash = hash_password(password);
  auto invite_hash = invite_token.empty() ? "" : sha256_hex(invite_token);
  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));
  register_user_atomic(db, username, password_hash, invite_hash, registration,
                       ip, cb);
}

auto handle_bootstrap(const drogon::HttpRequestPtr& req, Callback&& callback,
                      const std::string& bootstrap_digest) -> void {
  const auto ip = get_client_ip(req);
  if (!bootstrap_source_limiter->consume(ip) ||
      !bootstrap_global_limiter->consume("global")) {
    const auto wait = std::max(bootstrap_source_limiter->retry_after(ip),
                               bootstrap_global_limiter->retry_after("global"));
    auto response = rate_limited_response("Too many bootstrap attempts", wait);
    std::move(callback)(response);
    return;
  }
  auto json = req->getJsonObject();
  if (!json ||
      !has_exact_fields(*json, {"username", "password"}, {"bootstrap_token"})) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }
  if (!json->isMember("bootstrap_token") ||
      !(*json)["bootstrap_token"].isString()) {
    std::move(callback)(json_error(drogon::k403Forbidden, "bootstrap_denied",
                                   "Bootstrap authorization failed"));
    return;
  }
  const auto supplied_token = (*json)["bootstrap_token"].asString();
  if (supplied_token.size() > 256) {
    std::move(callback)(json_error(drogon::k403Forbidden, "bootstrap_denied",
                                   "Bootstrap authorization failed"));
    return;
  }
  const auto supplied_digest = sha256_hex(supplied_token);
  if (bootstrap_digest.empty() ||
      CRYPTO_memcmp(supplied_digest.data(), bootstrap_digest.data(),
                    supplied_digest.size()) != 0) {
    std::move(callback)(json_error(drogon::k403Forbidden, "bootstrap_denied",
                                   "Bootstrap authorization failed"));
    return;
  }
  const auto username = (*json)["username"].asString();
  const auto password = (*json)["password"].asString();
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
  HashPermit permit;
  if (!permit.acquired()) {
    std::move(callback)(rate_limited_response("Bootstrap is busy", 1));
    return;
  }
  auto cb = share(std::move(callback));
  bootstrap_user_atomic(drogon::app().getDbClient(), username,
                        hash_password(password), ip, cb);
}

auto handle_registration_status(Callback&& callback,
                                Config::Registration::Mode mode) -> void {
  Json::Value body;
  body["mode"] = std::string{registration_mode_name(mode)};
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  harden_auth_response(response, true);
  std::move(callback)(response);
}

auto handle_create_invite(const drogon::HttpRequestPtr& req,
                          Callback&& callback,
                          const Config::Registration& registration) -> void {
  auto json = req->getJsonObject();
  std::size_t ttl = registration.invite_ttl_seconds;
  if (json) {
    if (!json->isObject()) {
      std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                     "Invalid JSON body"));
      return;
    }
    const auto fields = json->getMemberNames();
    if (fields.size() > 1 ||
        (!fields.empty() && fields.front() != "ttl_seconds") ||
        (json->isMember("ttl_seconds") && !(*json)["ttl_seconds"].isUInt64())) {
      std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                     "Invalid JSON body"));
      return;
    }
    if (json->isMember("ttl_seconds")) {
      ttl = (*json)["ttl_seconds"].asUInt64();
    }
  }
  if (ttl < 60 || ttl > registration.invite_ttl_seconds) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_ttl",
                                   "Invite TTL is out of range"));
    return;
  }
  const auto ctx = get_auth_context(req);
  const auto ip = get_client_ip(req);
  if (!ctx) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }
  const auto token = generate_token();
  const auto token_hash = sha256_hex(token);
  auto cb = share(std::move(callback));
  drogon::app().getDbClient()->execSqlAsync(
      "INSERT INTO plinth.registration_invites "
      "(token_hash,created_by_user_id,expires_at) "
      "VALUES ($1,$2::uuid,NOW()+($3::text||' seconds')::interval) "
      "RETURNING id,expires_at",
      [cb, token, ctx = *ctx, ip](const drogon::orm::Result& result) {
        const auto id = result[0]["id"].as<std::string>();
        Json::Value detail;
        detail["invite_id"] = id;
        plinth::log::audit("auth.invite.created", detail,
                           {.user_id = ctx.user_id,
                            .session_id = ctx.session_id,
                            .ip_address = ip});
        Json::Value body;
        body["id"] = id;
        body["token"] = token;
        body["expires_at"] = result[0]["expires_at"].as<std::string>();
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(drogon::k201Created);
        harden_auth_response(response, true);
        (*cb)(response);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("invite creation failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Invite creation failed"));
      },
      token_hash, ctx->user_id, std::to_string(ttl));
}

auto nullable_string(const drogon::orm::Row& row, const char* field)
    -> Json::Value {
  return row[field].isNull() ? Json::Value{}
                             : Json::Value{row[field].as<std::string>()};
}

auto handle_list_invites(Callback&& callback) -> void {
  auto cb = share(std::move(callback));
  drogon::app().getDbClient()->execSqlAsync(
      "SELECT id,created_at,expires_at,revoked_at,used_at FROM "
      "plinth.registration_invites ORDER BY created_at DESC LIMIT 500",
      [cb](const drogon::orm::Result& result) {
        Json::Value invites(Json::arrayValue);
        for (const auto& row : result) {
          Json::Value invite;
          invite["id"] = row["id"].as<std::string>();
          invite["created_at"] = row["created_at"].as<std::string>();
          invite["expires_at"] = row["expires_at"].as<std::string>();
          invite["revoked_at"] = nullable_string(row, "revoked_at");
          invite["used_at"] = nullable_string(row, "used_at");
          invites.append(invite);
        }
        Json::Value body;
        body["invites"] = invites;
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        harden_auth_response(response, true);
        (*cb)(response);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("invite listing failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Invite listing failed"));
      });
}

auto handle_revoke_invite(const drogon::HttpRequestPtr& req,
                          Callback&& callback, const std::string& invite_id)
    -> void {
  const auto ctx = get_auth_context(req);
  const auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  drogon::app().getDbClient()->execSqlAsync(
      "UPDATE plinth.registration_invites SET revoked_at=NOW() "
      "WHERE id=$1::uuid AND revoked_at IS NULL AND used_at IS NULL RETURNING "
      "id",
      [cb, ctx, invite_id, ip](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "invite_not_found",
                           "Invite not found"));
          return;
        }
        Json::Value detail;
        detail["invite_id"] = invite_id;
        plinth::log::audit("auth.invite.revoked", detail,
                           {.user_id = ctx ? ctx->user_id : "",
                            .session_id = ctx ? ctx->session_id : "",
                            .ip_address = ip});
        Json::Value body;
        body["status"] = "revoked";
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        harden_auth_response(response, true);
        (*cb)(response);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("invite revocation failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Invite revocation failed"));
      },
      invite_id);
}

auto handle_recovery(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto json = req->getJsonObject();
  if (!json || !has_exact_fields(*json, {"username", "new_password"})) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }
  const auto username = (*json)["username"].asString();
  const auto password = (*json)["new_password"].asString();
  if (validate_username(username) || validate_password(password)) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid recovery request"));
    return;
  }
  const auto ctx = get_auth_context(req);
  const auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  HashPermit permit;
  if (!permit.acquired()) {
    (*cb)(rate_limited_response("Recovery is busy", 1));
    return;
  }
  const auto password_hash = hash_password(password);
  drogon::app().getDbClient()->newTransactionAsync(
      [username, password_hash, cb, ctx, ip](const TransactionPtr& tx) {
        if (!tx) {
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Recovery failed"));
          return;
        }
        tx->setTimeout(5.0);
        auto on_error = [cb](const drogon::orm::DrogonDbException&) {
          spdlog::error("account recovery transaction failed");
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Recovery failed"));
        };
        tx->execSqlAsync(
            "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
            [tx, username, password_hash, cb, ctx, ip,
             on_error](const drogon::orm::Result&) {
              tx->execSqlAsync(
                  "SELECT id FROM plinth.users WHERE username=$1 FOR UPDATE",
                  [tx, password_hash, cb, ctx, ip,
                   on_error](const drogon::orm::Result& target) {
                    if (target.empty()) {
                      tx->rollback();
                      (*cb)(json_error(drogon::k404NotFound, "user_not_found",
                                       "User not found"));
                      return;
                    }
                    const auto target_user_id =
                        target[0]["id"].as<std::string>();
                    tx->execSqlAsync(
                        "WITH target AS (UPDATE plinth.users SET "
                        "password_hash=$2 WHERE id=$1::uuid RETURNING id), "
                        "revoked_sessions AS (UPDATE plinth.sessions SET "
                        "revoked_at=NOW() WHERE user_id=$1::uuid AND "
                        "revoked_at IS NULL), revoked_pats AS (UPDATE "
                        "plinth.pats SET revoked_at=NOW() WHERE "
                        "user_id=$1::uuid AND revoked_at IS NULL) "
                        "SELECT id FROM target",
                        [tx, cb, ctx, target_user_id,
                         ip](const drogon::orm::Result&) {
                          tx->setCommitCallback([cb, ctx, target_user_id,
                                                 ip](bool committed) {
                            if (!committed) {
                              (*cb)(json_error(drogon::k500InternalServerError,
                                               "internal_error",
                                               "Recovery failed"));
                              return;
                            }
                            Json::Value detail;
                            detail["credentials_revoked"] = true;
                            detail["target_user_id"] = target_user_id;
                            plinth::log::audit(
                                "auth.account.password_reset", detail,
                                {.user_id = ctx ? ctx->user_id : "",
                                 .session_id = ctx ? ctx->session_id : "",
                                 .ip_address = ip});
                            Json::Value body;
                            body["status"] = "recovered";
                            auto response =
                                drogon::HttpResponse::newHttpJsonResponse(body);
                            harden_auth_response(response, true);
                            (*cb)(response);
                          });
                        },
                        on_error, target_user_id, password_hash);
                  },
                  on_error, username);
            },
            on_error);
      });
}

auto handle_login(const drogon::HttpRequestPtr& req, Callback&& callback,
                  bool dev_mode) -> void {
  auto json = req->getJsonObject();
  if (!json || !has_exact_fields(*json, {"username", "password"})) {
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
  // Login remains compatible with accounts created before the password-size
  // ceiling existed. New registration and recovery enforce that ceiling.
  if (validate_username(username)) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid credentials shape"));
    return;
  }

  auto ip = get_client_ip(req);
  auto subject = sha256_hex(username);

  // Check both dimensions before allocating Argon2's 64 MiB working set.
  if (!login_source_limiter->consume(ip) ||
      !login_subject_limiter->consume(subject)) {
    auto wait = std::max(login_source_limiter->retry_after(ip),
                         login_subject_limiter->retry_after(subject));
    auto resp = rate_limited_response("Too many failed login attempts", wait);
    Json::Value detail;
    detail["subject_hash"] = subject;
    detail["reason"] = "rate_limited";
    plinth::log::audit("user.login_failed", detail,
                       {.user_id = "", .session_id = "", .ip_address = ip});
    std::move(callback)(resp);
    return;
  }

  auto user_agent = req->getHeader("User-Agent");
  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "SELECT id, password_hash, disabled_at FROM plinth.users "
      "WHERE username = $1",
      [db, username, password, ip, subject, user_agent, dev_mode,
       cb](const drogon::orm::Result& result) {
        HashPermit permit;
        if (!permit.acquired()) {
          auto response = rate_limited_response("Login is busy", 1);
          (*cb)(response);
          return;
        }
        if (result.empty()) {
          // Run dummy hash to prevent timing side-channel
          dummy_hash();

          Json::Value detail;
          detail["subject_hash"] = subject;
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

        const bool password_valid = verify_password(password, password_hash);
        const bool disabled = !row["disabled_at"].isNull();
        if (!password_valid || disabled) {
          Json::Value detail;
          detail["subject_hash"] = subject;
          detail["reason"] = disabled ? "account_disabled" : "wrong_password";
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
        std::string csrf_token;
        try {
          csrf_token = csrf_token_for_session(raw_token);
        } catch (const std::exception& error) {
          spdlog::error("login CSRF token derivation failed: {}", error.what());
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Login failed"));
          return;
        }

        db->newTransactionAsync([user_id, username,
                                 verified_hash = std::move(password_hash),
                                 raw_token, token_hash,
                                 csrf_token = std::move(csrf_token), user_agent,
                                 ip, subject, dev_mode,
                                 cb](const TransactionPtr& tx) mutable {
          if (!tx) {
            (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                             "Login failed"));
            return;
          }
          tx->setTimeout(5.0);
          auto on_error = [cb](const drogon::orm::DrogonDbException&) {
            spdlog::error("session issuance transaction failed");
            (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                             "Login failed"));
          };
          tx->execSqlAsync(
              "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
              [tx, user_id, username, verified_hash = std::move(verified_hash),
               raw_token, token_hash, csrf_token = std::move(csrf_token),
               user_agent, ip, subject, dev_mode, cb,
               on_error](const drogon::orm::Result&) mutable {
                tx->execSqlAsync(
                    "SELECT id FROM plinth.users WHERE id=$1::uuid "
                    "FOR UPDATE",
                    [tx, user_id, username,
                     verified_hash = std::move(verified_hash), raw_token,
                     token_hash, csrf_token = std::move(csrf_token), user_agent,
                     ip, subject, dev_mode, cb,
                     on_error](const drogon::orm::Result&) mutable {
                      tx->execSqlAsync(
                          "INSERT INTO plinth.sessions "
                          "(user_id, token_hash, user_agent, ip_address) "
                          "SELECT id, $2, $3, $4::inet FROM plinth.users "
                          "WHERE id=$1::uuid AND password_hash=$5 "
                          "AND disabled_at IS NULL "
                          "RETURNING id, expires_at",
                          [tx, user_id, username, raw_token,
                           csrf_token = std::move(csrf_token), ip, subject,
                           dev_mode,
                           cb](const drogon::orm::Result& session) mutable {
                            if (session.empty()) {
                              tx->rollback();
                              Json::Value detail;
                              detail["subject_hash"] = subject;
                              detail["reason"] = "credentials_changed";
                              plinth::log::audit("user.login_failed", detail,
                                                 {.user_id = user_id,
                                                  .session_id = "",
                                                  .ip_address = ip});
                              (*cb)(json_error(drogon::k401Unauthorized,
                                               "invalid_credentials",
                                               "Invalid username or password"));
                              return;
                            }
                            const auto session_id =
                                session[0]["id"].as<std::string>();
                            const auto expires_at =
                                session[0]["expires_at"].as<std::string>();
                            tx->setCommitCallback([user_id, username, raw_token,
                                                   csrf_token =
                                                       std::move(csrf_token),
                                                   ip, subject, session_id,
                                                   expires_at, dev_mode,
                                                   cb](bool committed) mutable {
                              if (!committed) {
                                (*cb)(json_error(
                                    drogon::k500InternalServerError,
                                    "internal_error", "Login failed"));
                                return;
                              }
                              login_subject_limiter->reset(subject);
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
                              auto response =
                                  drogon::HttpResponse::newHttpJsonResponse(
                                      body);
                              response->setStatusCode(drogon::k200OK);
                              set_session_cookie(response, raw_token, dev_mode);
                              add_csrf_cookie(response, csrf_token, dev_mode);
                              harden_auth_response(response, true);
                              (*cb)(response);
                            });
                          },
                          on_error, user_id, token_hash, user_agent, ip,
                          verified_hash);
                    },
                    on_error, user_id);
              },
              on_error);
        });
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("user lookup failed");
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
  if (ctx->auth_type != "session") {
    std::move(callback)(json_error(drogon::k403Forbidden, "session_required",
                                   "A session is required"));
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
        clear_csrf_cookie(resp);
        harden_auth_response(resp, true);
        (*cb)(resp);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("logout failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Logout failed"));
      },
      ctx.value().session_id);
}

auto handle_get_session(const drogon::HttpRequestPtr& req, Callback&& callback,
                        bool dev_mode) -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }
  if (ctx->auth_type != "session") {
    std::move(callback)(json_error(drogon::k403Forbidden, "session_required",
                                   "A session is required"));
    return;
  }

  auto expected_csrf = request_expected_csrf_token(req);
  if (ctx->credential_source == CredentialSource::COOKIE &&
      !expected_csrf.has_value()) {
    std::move(callback)(json_error(drogon::k500InternalServerError,
                                   "internal_error",
                                   "Failed to retrieve session"));
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
      [cb, expected_csrf = std::move(expected_csrf),
       dev_mode](const drogon::orm::Result& result) {
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

        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        if (expected_csrf.has_value()) {
          add_csrf_cookie(resp, expected_csrf.value(), dev_mode);
        }
        harden_auth_response(resp);
        (*cb)(resp);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("session query failed");
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
              auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
              if (ctx_val.credential_source == CredentialSource::COOKIE &&
                  target_session_id == ctx_val.session_id) {
                clear_session_cookie(resp);
                clear_csrf_cookie(resp);
              }
              harden_auth_response(resp, true);
              (*cb)(resp);
            },
            [cb](const drogon::orm::DrogonDbException&) {
              spdlog::error("session revoke failed");
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to revoke session"));
            },
            target_session_id);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("session lookup failed");
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
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        harden_auth_response(resp);
        (*cb)(resp);
      },
      [cb](const drogon::orm::DrogonDbException&) {
        spdlog::error("sessions list failed");
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to list sessions"));
      },
      ctx.value().user_id);
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

namespace test_seam {

auto try_acquire_password_hash_slot() -> bool {
  return plinth::auth::try_acquire_password_hash_slot();
}

auto release_password_hash_slot() -> void {
  plinth::auth::release_password_hash_slot();
}

auto active_password_hash_slots() -> unsigned int {
  return active_password_hashes.load(std::memory_order_acquire);
}

} // namespace test_seam

auto register_auth_routes(bool dev_mode,
                          const Config::Registration& registration,
                          const std::string& bootstrap_token) -> void {
  const RateLimiter::Options source_options{
      .max_attempts = registration.source_attempts,
      .window = std::chrono::seconds{registration.window_seconds},
      .max_keys = 4096};
  const RateLimiter::Options subject_options{
      .max_attempts = registration.subject_attempts,
      .window = std::chrono::seconds{registration.window_seconds},
      .max_keys = 4096};
  const RateLimiter::Options global_options{
      .max_attempts = registration.global_attempts,
      .window = std::chrono::seconds{registration.window_seconds},
      .max_keys = 1};
  registration_source_limiter = std::make_unique<RateLimiter>(source_options);
  registration_subject_limiter = std::make_unique<RateLimiter>(subject_options);
  registration_global_limiter = std::make_unique<RateLimiter>(global_options);
  login_source_limiter = std::make_unique<RateLimiter>(source_options);
  login_subject_limiter = std::make_unique<RateLimiter>(subject_options);
  bootstrap_source_limiter = std::make_unique<RateLimiter>(source_options);
  bootstrap_global_limiter = std::make_unique<RateLimiter>(global_options);
  const auto bootstrap_digest =
      bootstrap_token.empty() ? std::string{} : sha256_hex(bootstrap_token);

  drogon::app().registerHandler(
      "/api/auth/registration",
      [mode = registration.mode](
          const drogon::HttpRequestPtr&,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_registration_status(std::move(callback), mode);
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/api/auth/bootstrap",
      [bootstrap_digest](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_bootstrap(req, std::move(callback), bootstrap_digest);
      },
      {drogon::Post, "plinth::auth::PublicOriginFilter"});

  drogon::app().registerHandler(
      "/api/auth/register",
      [registration](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_register(req, std::move(callback), registration);
      },
      {drogon::Post, "plinth::auth::PublicOriginFilter"});

  rbac::register_rule_requirement(drogon::Post, "/api/auth/invites",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Get, "/api/auth/invites",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Delete, "/api/auth/invites/{id}",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Post, "/api/auth/recovery",
                                  {"kernel.admin"});

  drogon::app().registerHandler(
      "/api/auth/invites",
      [registration](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_create_invite(req, std::move(callback), registration);
      },
      {drogon::Post, "plinth::auth::SessionFilter", "plinth::auth::CsrfFilter",
       "plinth::rbac::RbacFilter"});

  drogon::app().registerHandler(
      "/api/auth/invites",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_list_invites(std::move(callback));
      },
      {drogon::Get, "plinth::auth::SessionFilter", "plinth::rbac::RbacFilter"});

  drogon::app().registerHandler(
      "/api/auth/invites/{id}",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback,
         const std::string& invite_id) {
        handle_revoke_invite(req, std::move(callback), invite_id);
      },
      {drogon::Delete, "plinth::auth::SessionFilter",
       "plinth::auth::CsrfFilter", "plinth::rbac::RbacFilter"});

  drogon::app().registerHandler(
      "/api/auth/recovery",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_recovery(req, std::move(callback));
      },
      {drogon::Post, "plinth::auth::SessionFilter", "plinth::auth::CsrfFilter",
       "plinth::rbac::RbacFilter"});

  drogon::app().registerHandler(
      "/api/auth/login",
      [dev_mode](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_login(req, std::move(callback), dev_mode);
      },
      {drogon::Post, "plinth::auth::PublicOriginFilter"});

  drogon::app().registerHandler(
      "/api/auth/logout",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_logout(req, std::move(callback));
      },
      {drogon::Post, "plinth::auth::SessionFilter",
       "plinth::auth::CsrfFilter"});

  drogon::app().registerHandler(
      "/api/auth/session",
      [dev_mode](
          const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_get_session(req, std::move(callback), dev_mode);
      },
      {drogon::Get, "plinth::auth::SessionFilter"});

  drogon::app().registerHandler(
      "/api/auth/session/{id}",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback,
         const std::string& target_session_id) {
        handle_delete_session(req, std::move(callback), target_session_id);
      },
      {drogon::Delete, "plinth::auth::SessionFilter",
       "plinth::auth::CsrfFilter"});

  drogon::app().registerHandler(
      "/api/auth/sessions",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_list_sessions(req, std::move(callback));
      },
      {drogon::Get, "plinth::auth::SessionFilter"});

  spdlog::info("auth routes registered (registration_mode={})",
               registration_mode_name(registration.mode));
}

} // namespace plinth::auth
