#include "kernel/groups/handlers.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/logging.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <algorithm>
#include <drogon/drogon.h>
#include <drogon/orm/Row.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sstream>

namespace plinth::groups {

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

// ── Validation ──────────────────────────────────────────────────────

constexpr size_t GROUP_NAME_MAX_LEN = 64;
constexpr size_t GROUP_DESC_MAX_LEN = 256;

auto is_group_name_char(char ch) -> bool {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
         ch == '-';
}

auto validate_group_name(const std::string& name)
    -> std::optional<std::string> {
  if (name.empty() || name.size() > GROUP_NAME_MAX_LEN) {
    return "name_invalid";
  }
  if (!std::ranges::all_of(name, is_group_name_char)) {
    return "name_invalid";
  }
  return std::nullopt;
}

auto is_rule_char(char ch) -> bool {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
         ch == '.';
}

auto validate_rule_format(const std::string& rule) -> bool {
  if (rule.empty() || rule.find('.') == std::string::npos) {
    return false;
  }
  return std::ranges::all_of(rule, is_rule_char);
}

// ── Shared response/error helpers ────────────────────────────────────

// Respond with a group JSON object from a query result row.
auto respond_group_json(const drogon::orm::Row& row)
    -> drogon::HttpResponsePtr {
  Json::Value body;
  body["id"] = row["id"].as<std::string>();
  body["name"] = row["name"].as<std::string>();
  if (!row["description"].isNull()) {
    body["description"] = row["description"].as<std::string>();
  }
  body["built_in"] = row["built_in"].as<bool>();
  body["created_at"] = row["created_at"].as<std::string>();
  return drogon::HttpResponse::newHttpJsonResponse(body);
}

// Handle unique-violation vs generic error on group mutations.
auto handle_group_unique_error(const SharedCb& cb,
                               const drogon::orm::DrogonDbException& e,
                               const std::string& operation) -> void {
  auto msg = std::string{e.base().what()};
  if (msg.find("unique") != std::string::npos ||
      msg.find("duplicate") != std::string::npos) {
    (*cb)(json_error(drogon::k409Conflict, "group_exists",
                     "A group with that name already exists"));
  } else {
    spdlog::error("group {} failed: {}", operation, msg);
    (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                     "Failed to " + operation + " group"));
  }
}

// Handle invalid-uuid vs generic error on entity lookups.
auto handle_uuid_lookup_error(const SharedCb& cb,
                              const drogon::orm::DrogonDbException& e,
                              const std::string& not_found_code,
                              const std::string& not_found_msg,
                              const std::string& operation) -> void {
  auto msg = std::string{e.base().what()};
  if (msg.find("invalid input syntax for type uuid") != std::string::npos) {
    (*cb)(json_error(drogon::k404NotFound, not_found_code, not_found_msg));
  } else {
    spdlog::error("{} failed: {}", operation, msg);
    (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                     "Operation failed"));
  }
}

// ── Group CRUD handlers ─────────────────────────────────────────────

auto handle_create_group(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
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
                                   "Group name is required"));
    return;
  }
  if (auto err = validate_group_name(name); err.has_value()) {
    std::move(callback)(json_error(drogon::k400BadRequest, err.value(),
                                   "Name must be 1-64 chars, lowercase "
                                   "alphanumeric, hyphens, underscores"));
    return;
  }

  auto description = (*json)["description"].asString();
  if (description.size() > GROUP_DESC_MAX_LEN) {
    std::move(callback)(
        json_error(drogon::k400BadRequest, "description_too_long",
                   "Description must be 256 characters or fewer"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  db->execSqlAsync(
      "INSERT INTO plinth.groups (name, description) "
      "VALUES ($1, NULLIF($2, '')) "
      "RETURNING id, name, description, built_in, created_at",
      [ctx_val, ip, cb](const drogon::orm::Result& result) {
        auto row = result[0];
        Json::Value detail;
        detail["group_id"] = row["id"].as<std::string>();
        detail["group_name"] = row["name"].as<std::string>();
        plinth::log::audit("group.created", detail,
                           {.user_id = ctx_val.user_id,
                            .session_id = ctx_val.session_id,
                            .ip_address = ip});

        auto resp = respond_group_json(row);
        resp->setStatusCode(drogon::k201Created);
        (*cb)(resp);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        handle_group_unique_error(cb, e, "create");
      },
      name, description);
}

auto handle_list_groups(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "SELECT id, name, description, built_in, created_at "
      "FROM plinth.groups ORDER BY name",
      [cb](const drogon::orm::Result& result) {
        Json::Value groups(Json::arrayValue);
        for (const auto& row : result) {
          Json::Value g;
          g["id"] = row["id"].as<std::string>();
          g["name"] = row["name"].as<std::string>();
          if (!row["description"].isNull()) {
            g["description"] = row["description"].as<std::string>();
          }
          g["built_in"] = row["built_in"].as<bool>();
          g["created_at"] = row["created_at"].as<std::string>();
          groups.append(g);
        }

        Json::Value body;
        body["groups"] = groups;
        (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("group list failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to list groups"));
      });
}

auto handle_get_group(const drogon::HttpRequestPtr& req, Callback&& callback,
                      const std::string& group_id) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      "SELECT g.id, g.name, g.description, g.built_in, g.created_at, "
      "       COALESCE(m.member_count, 0) AS member_count "
      "FROM plinth.groups g "
      "LEFT JOIN (SELECT group_id, COUNT(*) AS member_count "
      "           FROM plinth.group_members GROUP BY group_id) m "
      "  ON m.group_id = g.id "
      "WHERE g.id = $1::uuid",
      [cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
          return;
        }

        auto row = result[0];
        Json::Value body;
        body["id"] = row["id"].as<std::string>();
        body["name"] = row["name"].as<std::string>();
        if (!row["description"].isNull()) {
          body["description"] = row["description"].as<std::string>();
        }
        body["built_in"] = row["built_in"].as<bool>();
        body["created_at"] = row["created_at"].as<std::string>();
        body["member_count"] = row["member_count"].as<int64_t>();

        (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        auto msg = std::string{e.base().what()};
        if (msg.find("invalid input syntax for type uuid") !=
            std::string::npos) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
        } else {
          spdlog::error("group get failed: {}", msg);
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Failed to get group"));
        }
      },
      group_id);
}

// Execute the UPDATE after pre-checks pass.
auto execute_group_update(const drogon::orm::DbClientPtr& db,
                          const auth::AuthContext& ctx_val,
                          const std::string& group_id, bool has_name,
                          bool has_desc, const std::string& new_name,
                          const std::string& new_desc,
                          const std::string& old_name, const std::string& ip,
                          const SharedCb& cb) -> void {
  auto final_name = has_name ? new_name : old_name;

  auto on_success = [ctx_val, group_id, ip,
                     cb](const drogon::orm::Result& upd_result) {
    auto row = upd_result[0];
    Json::Value detail;
    detail["group_id"] = group_id;
    detail["group_name"] = row["name"].as<std::string>();
    plinth::log::audit("group.updated", detail,
                       {.user_id = ctx_val.user_id,
                        .session_id = ctx_val.session_id,
                        .ip_address = ip});
    (*cb)(respond_group_json(row));
  };

  auto on_error = [cb](const drogon::orm::DrogonDbException& e) {
    handle_group_unique_error(cb, e, "update");
  };

  if (has_desc) {
    db->execSqlAsync(
        "UPDATE plinth.groups SET name = $1, description = NULLIF($3, '') "
        "WHERE id = $2::uuid "
        "RETURNING id, name, description, built_in, created_at",
        on_success, on_error, final_name, group_id, new_desc);
  } else {
    db->execSqlAsync("UPDATE plinth.groups SET name = $1 "
                     "WHERE id = $2::uuid "
                     "RETURNING id, name, description, built_in, created_at",
                     on_success, on_error, final_name, group_id);
  }
}

auto handle_update_group(const drogon::HttpRequestPtr& req, Callback&& callback,
                         const std::string& group_id) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto json = req->getJsonObject();
  if (!json) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  auto has_name = json->isMember("name") && !(*json)["name"].asString().empty();
  auto has_desc = json->isMember("description");
  auto new_name = has_name ? (*json)["name"].asString() : std::string{};
  auto new_desc = has_desc ? (*json)["description"].asString() : std::string{};

  if (has_name) {
    if (auto err = validate_group_name(new_name); err.has_value()) {
      (*cb)(json_error(drogon::k400BadRequest, err.value(),
                       "Name must be 1-64 chars, lowercase alphanumeric, "
                       "hyphens, underscores"));
      return;
    }
  }
  if (has_desc && new_desc.size() > GROUP_DESC_MAX_LEN) {
    (*cb)(json_error(drogon::k400BadRequest, "description_too_long",
                     "Description must be 256 characters or fewer"));
    return;
  }

  db->execSqlAsync(
      "SELECT built_in, name FROM plinth.groups WHERE id = $1::uuid",
      [db, ctx_val, group_id, has_name, has_desc, new_name, new_desc, ip,
       cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
          return;
        }
        if (result[0]["built_in"].as<bool>()) {
          (*cb)(json_error(drogon::k403Forbidden, "permission_denied",
                           "Built-in groups cannot be modified"));
          return;
        }
        auto old_name = result[0]["name"].as<std::string>();
        execute_group_update(db, ctx_val, group_id, has_name, has_desc,
                             new_name, new_desc, old_name, ip, cb);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        handle_uuid_lookup_error(cb, e, "group_not_found", "Group not found",
                                 "update group");
      },
      group_id);
}

auto handle_delete_group(const drogon::HttpRequestPtr& req, Callback&& callback,
                         const std::string& group_id) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  // Check group exists and is not built-in
  db->execSqlAsync(
      "SELECT name, built_in FROM plinth.groups WHERE id = $1::uuid",
      [db, ctx_val, group_id, ip, cb](const drogon::orm::Result& result) {
        if (result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
          return;
        }
        if (result[0]["built_in"].as<bool>()) {
          (*cb)(json_error(drogon::k403Forbidden, "permission_denied",
                           "Built-in groups cannot be deleted"));
          return;
        }

        auto group_name = result[0]["name"].as<std::string>();

        // Delete child rows, then the group itself
        db->execSqlAsync(
            "DELETE FROM plinth.group_rules WHERE group_id = $1::uuid",
            [db, ctx_val, group_id, group_name, ip,
             cb](const drogon::orm::Result&) {
              db->execSqlAsync(
                  "DELETE FROM plinth.group_members WHERE group_id = $1::uuid",
                  [db, ctx_val, group_id, group_name, ip,
                   cb](const drogon::orm::Result&) {
                    db->execSqlAsync(
                        "DELETE FROM plinth.groups WHERE id = $1::uuid",
                        [ctx_val, group_id, group_name, ip,
                         cb](const drogon::orm::Result&) {
                          Json::Value detail;
                          detail["group_id"] = group_id;
                          detail["group_name"] = group_name;
                          plinth::log::audit("group.deleted", detail,
                                             {.user_id = ctx_val.user_id,
                                              .session_id = ctx_val.session_id,
                                              .ip_address = ip});

                          Json::Value body;
                          body["status"] = "deleted";
                          (*cb)(
                              drogon::HttpResponse::newHttpJsonResponse(body));
                        },
                        [cb](const drogon::orm::DrogonDbException& e) {
                          spdlog::error("group delete failed: {}",
                                        e.base().what());
                          (*cb)(json_error(drogon::k500InternalServerError,
                                           "internal_error",
                                           "Failed to delete group"));
                        },
                        group_id);
                  },
                  [cb](const drogon::orm::DrogonDbException& e) {
                    spdlog::error("group members delete failed: {}",
                                  e.base().what());
                    (*cb)(json_error(drogon::k500InternalServerError,
                                     "internal_error",
                                     "Failed to delete group"));
                  },
                  group_id);
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("group rules delete failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to delete group"));
            },
            group_id);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        auto msg = std::string{e.base().what()};
        if (msg.find("invalid input syntax for type uuid") !=
            std::string::npos) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
        } else {
          spdlog::error("group lookup failed: {}", msg);
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Failed to delete group"));
        }
      },
      group_id);
}

// ── Membership handlers ─────────────────────────────────────────────

// Insert the membership row after group and user existence are confirmed.
auto insert_membership(const drogon::orm::DbClientPtr& db,
                       const auth::AuthContext& ctx_val,
                       const std::string& group_id,
                       const std::string& user_id_to_add, const std::string& ip,
                       const SharedCb& cb) -> void {
  db->execSqlAsync(
      "INSERT INTO plinth.group_members (group_id, user_id) "
      "VALUES ($1::uuid, $2::uuid) RETURNING added_at",
      [ctx_val, group_id, user_id_to_add, ip,
       cb](const drogon::orm::Result& mem_result) {
        Json::Value detail;
        detail["group_id"] = group_id;
        detail["target_user_id"] = user_id_to_add;
        plinth::log::audit("group.member_added", detail,
                           {.user_id = ctx_val.user_id,
                            .session_id = ctx_val.session_id,
                            .ip_address = ip});

        Json::Value body;
        body["group_id"] = group_id;
        body["user_id"] = user_id_to_add;
        body["added_at"] = mem_result[0]["added_at"].as<std::string>();

        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k201Created);
        (*cb)(resp);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        auto msg = std::string{e.base().what()};
        if (msg.find("unique") != std::string::npos ||
            msg.find("duplicate") != std::string::npos) {
          (*cb)(json_error(drogon::k409Conflict, "already_member",
                           "User is already a member of this group"));
        } else {
          spdlog::error("member add failed: {}", msg);
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Failed to add member"));
        }
      },
      group_id, user_id_to_add);
}

auto handle_add_member(const drogon::HttpRequestPtr& req, Callback&& callback,
                       const std::string& group_id) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto json = req->getJsonObject();
  if (!json) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto user_id_to_add = (*json)["user_id"].asString();
  if (user_id_to_add.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_user_id",
                                   "user_id is required"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  // Verify group exists, then user exists, then insert
  db->execSqlAsync(
      "SELECT id FROM plinth.groups WHERE id = $1::uuid",
      [db, ctx_val, group_id, user_id_to_add, ip,
       cb](const drogon::orm::Result& grp_result) {
        if (grp_result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
          return;
        }
        db->execSqlAsync(
            "SELECT id FROM plinth.users WHERE id = $1::uuid",
            [db, ctx_val, group_id, user_id_to_add, ip,
             cb](const drogon::orm::Result& usr_result) {
              if (usr_result.empty()) {
                (*cb)(json_error(drogon::k404NotFound, "user_not_found",
                                 "User not found"));
                return;
              }
              insert_membership(db, ctx_val, group_id, user_id_to_add, ip, cb);
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              handle_uuid_lookup_error(cb, e, "user_not_found",
                                       "User not found", "add member");
            },
            user_id_to_add);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        handle_uuid_lookup_error(cb, e, "group_not_found", "Group not found",
                                 "add member");
      },
      group_id);
}

auto handle_remove_member(const drogon::HttpRequestPtr& req,
                          Callback&& callback, const std::string& group_id,
                          const std::string& target_user_id) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  db->execSqlAsync(
      "DELETE FROM plinth.group_members "
      "WHERE group_id = $1::uuid AND user_id = $2::uuid",
      [ctx_val, group_id, target_user_id, ip,
       cb](const drogon::orm::Result& result) {
        if (result.affectedRows() == 0) {
          (*cb)(json_error(drogon::k404NotFound, "not_a_member",
                           "User is not a member of this group"));
          return;
        }

        Json::Value detail;
        detail["group_id"] = group_id;
        detail["target_user_id"] = target_user_id;
        plinth::log::audit("group.member_removed", detail,
                           {.user_id = ctx_val.user_id,
                            .session_id = ctx_val.session_id,
                            .ip_address = ip});

        Json::Value body;
        body["status"] = "removed";
        (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("member remove failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to remove member"));
      },
      group_id, target_user_id);
}

// ── Rule grant/revoke handlers ──────────────────────────────────────

auto handle_grant_rule(const drogon::HttpRequestPtr& req, Callback&& callback,
                       const std::string& group_id) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto json = req->getJsonObject();
  if (!json) {
    std::move(callback)(json_error(drogon::k400BadRequest, "invalid_request",
                                   "Invalid JSON body"));
    return;
  }

  auto rule_name = (*json)["rule"].asString();
  if (rule_name.empty()) {
    std::move(callback)(json_error(drogon::k400BadRequest, "missing_rule",
                                   "Rule name is required"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  // Verify group exists
  db->execSqlAsync(
      "SELECT id FROM plinth.groups WHERE id = $1::uuid",
      [db, ctx_val, group_id, rule_name, ip,
       cb](const drogon::orm::Result& grp_result) {
        if (grp_result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
          return;
        }

        // Look up rule by name
        db->execSqlAsync(
            "SELECT id FROM plinth.rbac_rules WHERE rule = $1",
            [db, ctx_val, group_id, rule_name, ip,
             cb](const drogon::orm::Result& rule_result) {
              if (rule_result.empty()) {
                (*cb)(json_error(drogon::k404NotFound, "rule_not_found",
                                 "Rule not found"));
                return;
              }

              auto rule_id = rule_result[0]["id"].as<std::string>();

              db->execSqlAsync(
                  "INSERT INTO plinth.group_rules (group_id, rule_id) "
                  "VALUES ($1::uuid, $2::uuid) RETURNING granted_at",
                  [ctx_val, group_id, rule_name, ip,
                   cb](const drogon::orm::Result& grant_result) {
                    Json::Value detail;
                    detail["group_id"] = group_id;
                    detail["rule"] = rule_name;
                    plinth::log::audit("rbac.rule_granted", detail,
                                       {.user_id = ctx_val.user_id,
                                        .session_id = ctx_val.session_id,
                                        .ip_address = ip});

                    Json::Value body;
                    body["group_id"] = group_id;
                    body["rule"] = rule_name;
                    body["granted_at"] =
                        grant_result[0]["granted_at"].as<std::string>();

                    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
                    resp->setStatusCode(drogon::k201Created);
                    (*cb)(resp);
                  },
                  [cb](const drogon::orm::DrogonDbException& e) {
                    auto msg = std::string{e.base().what()};
                    if (msg.find("unique") != std::string::npos ||
                        msg.find("duplicate") != std::string::npos) {
                      (*cb)(json_error(
                          drogon::k409Conflict, "rule_already_granted",
                          "Rule is already granted to this group"));
                    } else {
                      spdlog::error("rule grant failed: {}", msg);
                      (*cb)(json_error(drogon::k500InternalServerError,
                                       "internal_error",
                                       "Failed to grant rule"));
                    }
                  },
                  group_id, rule_id);
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("rule lookup failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to grant rule"));
            },
            rule_name);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        auto msg = std::string{e.base().what()};
        if (msg.find("invalid input syntax for type uuid") !=
            std::string::npos) {
          (*cb)(json_error(drogon::k404NotFound, "group_not_found",
                           "Group not found"));
        } else {
          spdlog::error("group lookup failed: {}", msg);
          (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                           "Failed to grant rule"));
        }
      },
      group_id);
}

auto handle_revoke_rule(const drogon::HttpRequestPtr& req, Callback&& callback,
                        const std::string& group_id,
                        const std::string& rule_name) -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto ip = get_client_ip(req);
  auto cb = share(std::move(callback));
  const auto& ctx_val = ctx.value();

  // Look up rule by name to get its UUID
  db->execSqlAsync(
      "SELECT id FROM plinth.rbac_rules WHERE rule = $1",
      [db, ctx_val, group_id, rule_name, ip,
       cb](const drogon::orm::Result& rule_result) {
        if (rule_result.empty()) {
          (*cb)(json_error(drogon::k404NotFound, "rule_not_found",
                           "Rule not found"));
          return;
        }

        auto rule_id = rule_result[0]["id"].as<std::string>();

        db->execSqlAsync(
            "DELETE FROM plinth.group_rules "
            "WHERE group_id = $1::uuid AND rule_id = $2::uuid",
            [ctx_val, group_id, rule_name, ip,
             cb](const drogon::orm::Result& del_result) {
              if (del_result.affectedRows() == 0) {
                (*cb)(json_error(drogon::k404NotFound, "rule_not_found",
                                 "Rule is not granted to this group"));
                return;
              }

              Json::Value detail;
              detail["group_id"] = group_id;
              detail["rule"] = rule_name;
              plinth::log::audit("rbac.rule_revoked", detail,
                                 {.user_id = ctx_val.user_id,
                                  .session_id = ctx_val.session_id,
                                  .ip_address = ip});

              Json::Value body;
              body["status"] = "revoked";
              (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("rule revoke failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to revoke rule"));
            },
            group_id, rule_id);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("rule lookup failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to revoke rule"));
      },
      rule_name);
}

// ── Bootstrap helpers (libpq, synchronous) ──────────────────────────

auto build_conninfo(const Config::Database& db_cfg) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db_cfg.host << " port=" << db_cfg.port
     << " dbname=" << db_cfg.database << " user=" << db_cfg.user
     << " password=" << db_cfg.password;
  return ss.str();
}

auto pg_exec(PGconn* conn, const std::string& sql) -> void {
  std::unique_ptr<PGresult, decltype(&PQclear)> res(PQexec(conn, sql.c_str()),
                                                    PQclear);
  auto status = PQresultStatus(res.get());
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    throw std::runtime_error(std::string("bootstrap_groups SQL failed: ") +
                             PQresultErrorMessage(res.get()));
  }
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

auto bootstrap_groups(const Config::Database& db_cfg) -> void {
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    throw std::runtime_error("bootstrap_groups: PG connect failed: " + err);
  }

  // RAII cleanup
  auto cleanup = [](PGconn* c) { PQfinish(c); };
  std::unique_ptr<PGconn, decltype(cleanup)> guard(conn, cleanup);

  spdlog::info("seeding built-in groups and kernel.admin rule");

  // 1. Built-in groups
  pg_exec(conn, "INSERT INTO plinth.groups (name, description, built_in) "
                "VALUES ('admin', 'Administrators', true) "
                "ON CONFLICT (name) DO NOTHING");
  pg_exec(conn, "INSERT INTO plinth.groups (name, description, built_in) "
                "VALUES ('everyone', 'All authenticated users', true) "
                "ON CONFLICT (name) DO NOTHING");

  // 2. kernel.admin rule
  pg_exec(conn, "INSERT INTO plinth.rbac_rules (rule, namespace, description, "
                "extension_name) "
                "VALUES ('kernel.admin', 'kernel', 'Full administrative "
                "access', 'kernel') "
                "ON CONFLICT (rule) DO NOTHING");

  // 3. Grant kernel.admin → admin group
  pg_exec(conn, "INSERT INTO plinth.group_rules (group_id, rule_id) "
                "SELECT g.id, r.id "
                "FROM plinth.groups g, plinth.rbac_rules r "
                "WHERE g.name = 'admin' AND r.rule = 'kernel.admin' "
                "ON CONFLICT DO NOTHING");

  // 4. Emit rbac.rule_registered audit event for the seeded kernel.admin
  //    rule, guarded to stay idempotent across restarts. Per ICD-0.1.7
  //    §Audit Event Catalog, every RBAC rule registration must be audited.
  std::unique_ptr<PGresult, decltype(&PQclear)> audit_check(
      PQexec(conn, "SELECT 1 FROM plinth.audit_log "
                   "WHERE action = 'rbac.rule_registered' "
                   "  AND detail->>'rule' = 'kernel.admin' LIMIT 1"),
      PQclear);
  if (PQresultStatus(audit_check.get()) == PGRES_TUPLES_OK &&
      PQntuples(audit_check.get()) == 0) {
    Json::Value detail;
    detail["rule"] = "kernel.admin";
    detail["namespace"] = "kernel";
    detail["extension_name"] = "kernel";
    plinth::log::audit_sync(db_cfg, "rbac.rule_registered", detail);
  }

  // 5. packages.install + packages.read rules (ICD-0.4.4). Both granted to
  //    admin group; audited once per rule with the same idempotency guard.
  pg_exec(conn, "INSERT INTO plinth.rbac_rules (rule, namespace, description, "
                "extension_name) "
                "VALUES ('packages.install', 'kernel', 'Upload and install "
                "extension packages', 'kernel') "
                "ON CONFLICT (rule) DO NOTHING");
  pg_exec(conn, "INSERT INTO plinth.rbac_rules (rule, namespace, description, "
                "extension_name) "
                "VALUES ('packages.read', 'kernel', 'Read installed-package "
                "metadata', 'kernel') "
                "ON CONFLICT (rule) DO NOTHING");

  pg_exec(conn, "INSERT INTO plinth.group_rules (group_id, rule_id) "
                "SELECT g.id, r.id "
                "FROM plinth.groups g, plinth.rbac_rules r "
                "WHERE g.name = 'admin' "
                "  AND r.rule IN ('packages.install', 'packages.read') "
                "ON CONFLICT DO NOTHING");

  for (const auto* rule : {"packages.install", "packages.read"}) {
    std::string sql = std::string("SELECT 1 FROM plinth.audit_log "
                                  "WHERE action = 'rbac.rule_registered' "
                                  "  AND detail->>'rule' = '") +
                      rule + "' LIMIT 1";
    std::unique_ptr<PGresult, decltype(&PQclear)> check(
        PQexec(conn, sql.c_str()), PQclear);
    if (PQresultStatus(check.get()) == PGRES_TUPLES_OK &&
        PQntuples(check.get()) == 0) {
      Json::Value detail;
      detail["rule"] = rule;
      detail["namespace"] = "kernel";
      detail["extension_name"] = "kernel";
      plinth::log::audit_sync(db_cfg, "rbac.rule_registered", detail);
    }
  }

  spdlog::info("built-in groups and rules seeded");
}

auto register_group_routes() -> void {
  // All group routes require kernel.admin via RBAC enforcement filter.
  // RBAC rule registration is idempotent and needs to be visible to any
  // test that calls this from within a grouped Catch2 subprocess
  // (0.4.5.1) AFTER drogon has started via
  // ensure_drogon_with_db_running(). Register all rule requirements
  // unconditionally; the handler-bindings below are guarded separately.
  rbac::register_rule_requirement(drogon::Post, "/api/groups",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Get, "/api/groups", {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Get, "/api/groups/{id}",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Put, "/api/groups/{id}",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Delete, "/api/groups/{id}",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Post, "/api/groups/{id}/members",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(
      drogon::Delete, "/api/groups/{id}/members/{user_id}", {"kernel.admin"});
  rbac::register_rule_requirement(drogon::Post, "/api/groups/{id}/rules",
                                  {"kernel.admin"});
  rbac::register_rule_requirement(
      drogon::Delete, "/api/groups/{id}/rules/{rule}", {"kernel.admin"});

  // Drogon's `registerHandler` fires a `!routersInit_` assertion once
  // `app().run()` has been invoked. In production main() calls this
  // helper before run(). In tests the grouped pg subprocess may have
  // started drogon in an earlier TEST_CASE; skip the handler step
  // there — tests look at list_registered_rules() for RBAC coverage,
  // not the live HTTP handlers. The call_once guards against duplicate
  // registrations before drogon starts (production path).
  if (drogon::app().isRunning()) {
    return;
  }
  static std::once_flag once;
  std::call_once(once, [&] {
    // ── Group CRUD ────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/groups",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
          handle_create_group(req, std::move(callback));
        },
        {drogon::Post, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    drogon::app().registerHandler(
        "/api/groups",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
          handle_list_groups(req, std::move(callback));
        },
        {drogon::Get, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    drogon::app().registerHandler(
        "/api/groups/{id}",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id) {
          handle_get_group(req, std::move(callback), group_id);
        },
        {drogon::Get, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    drogon::app().registerHandler(
        "/api/groups/{id}",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id) {
          handle_update_group(req, std::move(callback), group_id);
        },
        {drogon::Put, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    drogon::app().registerHandler(
        "/api/groups/{id}",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id) {
          handle_delete_group(req, std::move(callback), group_id);
        },
        {drogon::Delete, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    // ── Membership ────────────────────────────────────────────
    drogon::app().registerHandler(
        "/api/groups/{id}/members",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id) {
          handle_add_member(req, std::move(callback), group_id);
        },
        {drogon::Post, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    drogon::app().registerHandler(
        "/api/groups/{id}/members/{user_id}",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id, const std::string& target_user_id) {
          handle_remove_member(req, std::move(callback), group_id,
                               target_user_id);
        },
        {drogon::Delete, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    // ── Rule grant/revoke ─────────────────────────────────────
    drogon::app().registerHandler(
        "/api/groups/{id}/rules",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id) {
          handle_grant_rule(req, std::move(callback), group_id);
        },
        {drogon::Post, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    drogon::app().registerHandler(
        "/api/groups/{id}/rules/{rule}",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
           const std::string& group_id, const std::string& rule_name) {
          handle_revoke_rule(req, std::move(callback), group_id, rule_name);
        },
        {drogon::Delete, "plinth::auth::SessionFilter",
         "plinth::rbac::RbacFilter"});

    spdlog::info("group and RBAC routes registered");
  });
}

} // namespace plinth::groups
