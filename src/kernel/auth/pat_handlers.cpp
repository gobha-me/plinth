#include "kernel/auth/crypto.hpp"
#include "kernel/auth/handlers.hpp"
#include "kernel/auth/middleware.hpp"
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

auto json_error(drogon::HttpStatusCode status, const std::string& error_code,
                const std::string& message) -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = error_code;
  json["message"] = message;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
  resp->setStatusCode(status);
  return resp;
}

auto get_client_ip(const drogon::HttpRequestPtr& req) -> std::string {
  return req->peerAddr().toIp();
}

// ── PAT handlers ────────────────────────────────────────────────────

constexpr size_t PAT_NAME_MAX_LEN = 128;
constexpr size_t PAT_TOKEN_PREFIX_LEN = 8;

auto handle_create_pat(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  // PATs cannot create other PATs (session-only)
  if (ctx.value().auth_type != "session") {
    std::move(callback)(json_error(drogon::k403Forbidden, "forbidden",
                                   "PATs cannot create other PATs"));
    return;
  }

  auto json = req->getJsonObject();
  if (!json) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto name = (*json)["name"].asString();
  if (name.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_name",
                                   "PAT name is required"));
    return;
  }
  if (name.size() > PAT_NAME_MAX_LEN) {
    std::move(callback)(json_error(drogon::k400BadRequest, "name_too_long",
                                   "PAT name must be 128 characters or fewer"));
    return;
  }

  auto expires_at = (*json)["expires_at"].asString();
  auto raw_random = generate_token();
  auto full_token = "plinth_" + raw_random;
  auto token_hash = sha256_hex(raw_random);
  auto token_prefix = raw_random.substr(0, PAT_TOKEN_PREFIX_LEN);

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  // Helper lambda to perform the INSERT
  auto do_insert = [db, ctx_val, name, token_hash, token_prefix, full_token,
                    expires_at, ip, cb]() {
    auto sql =
        expires_at.empty()
            ? std::string{"INSERT INTO plinth.pats "
                          "(user_id, name, token_hash, token_prefix) "
                          "VALUES ($1::uuid, $2, $3, $4) "
                          "RETURNING id, created_at, expires_at"}
            : std::string{
                  "INSERT INTO plinth.pats "
                  "(user_id, name, token_hash, token_prefix, expires_at) "
                  "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz) "
                  "RETURNING id, created_at, expires_at"};

    auto on_success = [ctx_val, name, full_token, ip,
                       cb](const drogon::orm::Result& result) {
      auto row = result[0];
      auto pat_id = row["id"].as<std::string>();

      Json::Value detail;
      detail["pat_name"] = name;
      detail["pat_id"] = pat_id;
      plinth::log::audit("pat.created", detail,
                         {.user_id = ctx_val.user_id,
                          .session_id = ctx_val.session_id,
                          .ip_address = ip});

      Json::Value body;
      body["id"] = pat_id;
      body["name"] = name;
      body["token"] = full_token;
      body["created_at"] = row["created_at"].as<std::string>();
      if (!row["expires_at"].isNull()) {
        body["expires_at"] = row["expires_at"].as<std::string>();
      }

      auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
      resp->setStatusCode(drogon::k201Created);
      (*cb)(resp);
    };

    auto on_error = [cb](const drogon::orm::DrogonDbException& e) {
      spdlog::error("PAT insert failed: {}", e.base().what());
      (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                       "Failed to create PAT"));
    };

    if (expires_at.empty()) {
      db->execSqlAsync(sql, on_success, on_error, ctx_val.user_id, name,
                       token_hash, token_prefix);
    } else {
      db->execSqlAsync(sql, on_success, on_error, ctx_val.user_id, name,
                       token_hash, token_prefix, expires_at);
    }
  };

  // If expires_at is provided, validate format and ensure it's in the future
  if (!expires_at.empty()) {
    db->execSqlAsync(
        "SELECT $1::timestamptz > NOW() AS is_future",
        [cb, do_insert](const drogon::orm::Result& result) {
          if (!result[0]["is_future"].as<bool>()) {
            (*cb)(json_error(drogon::k400BadRequest, "invalid_expiry",
                             "Expiry must be in the future"));
            return;
          }
          do_insert();
        },
        [cb](const drogon::orm::DrogonDbException&) {
          (*cb)(json_error(drogon::k400BadRequest, "invalid_expiry",
                           "Invalid expiry timestamp format"));
        },
        expires_at);
  } else {
    do_insert();
  }
}

auto handle_list_pats(const drogon::HttpRequestPtr& req, Callback&& callback)
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
      "SELECT id, name, token_prefix, created_at, expires_at, "
      "       last_used_at, revoked_at "
      "FROM plinth.pats "
      "WHERE user_id = $1::uuid "
      "ORDER BY created_at DESC",
      [cb](const drogon::orm::Result& result) {
        Json::Value pats(Json::arrayValue);
        for (const auto& row : result) {
          Json::Value pat;
          pat["id"] = row["id"].as<std::string>();
          pat["name"] = row["name"].as<std::string>();
          pat["token_prefix"] = row["token_prefix"].as<std::string>();
          pat["created_at"] = row["created_at"].as<std::string>();
          if (!row["expires_at"].isNull()) {
            pat["expires_at"] = row["expires_at"].as<std::string>();
          } else {
            pat["expires_at"] = Json::nullValue;
          }
          if (!row["last_used_at"].isNull()) {
            pat["last_used_at"] = row["last_used_at"].as<std::string>();
          } else {
            pat["last_used_at"] = Json::nullValue;
          }
          if (!row["revoked_at"].isNull()) {
            pat["revoked_at"] = row["revoked_at"].as<std::string>();
          } else {
            pat["revoked_at"] = Json::nullValue;
          }
          pats.append(pat);
        }

        Json::Value body;
        body["pats"] = pats;
        (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("PAT list failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to list PATs"));
      },
      ctx.value().user_id);
}

auto handle_revoke_pat(const drogon::HttpRequestPtr& req, Callback&& callback,
                       const std::string& target_pat_id) -> void {
  auto ctx = get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  auto ctx_val = ctx.value();

  // Check ownership or admin status
  db->execSqlAsync(
      "SELECT p.user_id, "
      "       EXISTS(SELECT 1 FROM plinth.group_members gm "
      "              JOIN plinth.groups g ON g.id = gm.group_id "
      "              WHERE gm.user_id = $2::uuid AND g.name = 'admin') "
      "       AS is_admin "
      "FROM plinth.pats p "
      "WHERE p.id = $1::uuid AND p.revoked_at IS NULL",
      [db, ctx_val, target_pat_id, ip, cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "pat_not_found",
                           "PAT not found"));
          return;
        }

        auto row = result[0];
        auto pat_owner = row["user_id"].as<std::string>();
        auto is_admin = row["is_admin"].as<bool>();

        if (pat_owner != ctx_val.user_id && !is_admin) {
          (*cb)(json_error(drogon::k403Forbidden, "forbidden",
                           "Cannot revoke another user's PAT"));
          return;
        }

        db->execSqlAsync(
            "UPDATE plinth.pats SET revoked_at = NOW() "
            "WHERE id = $1::uuid AND revoked_at IS NULL",
            [ctx_val, target_pat_id, ip, cb](const drogon::orm::Result&) {
              Json::Value detail;
              detail["target_pat_id"] = target_pat_id;
              plinth::log::audit("pat.revoked", detail,
                                 {.user_id = ctx_val.user_id,
                                  .session_id = ctx_val.session_id,
                                  .ip_address = ip});

              Json::Value body;
              body["status"] = "pat_revoked";
              (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("PAT revoke failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to revoke PAT"));
            },
            target_pat_id);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("PAT lookup failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to revoke PAT"));
      },
      target_pat_id, ctx_val.user_id);
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

auto register_pat_routes() -> void {
  drogon::app().registerHandler(
      "/api/auth/pats",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_create_pat(req, std::move(callback));
      },
      {drogon::Post, "plinth::auth::SessionFilter"});

  drogon::app().registerHandler(
      "/api/auth/pats",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_list_pats(req, std::move(callback));
      },
      {drogon::Get, "plinth::auth::SessionFilter"});

  drogon::app().registerHandler(
      "/api/auth/pats/{id}",
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback,
         const std::string& target_pat_id) {
        handle_revoke_pat(req, std::move(callback), target_pat_id);
      },
      {drogon::Delete, "plinth::auth::SessionFilter"});

  spdlog::info("PAT routes registered");
}

} // namespace plinth::auth
