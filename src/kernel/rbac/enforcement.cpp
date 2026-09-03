#include "kernel/rbac/enforcement.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/logging.hpp"

#include <drogon/HttpTypes.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace plinth::rbac {

namespace {

// ── Route-to-rules registry (populated at startup, read-only at runtime) ─
//
// A flat vector rather than a map: the registry is small (one entry per
// RBAC-gated HTTP route, order-of-ten through 0.2.x), lookups happen
// at request-dispatch time only on the filter's hot path, and the
// linear scan keeps list_registered_rules() trivially cheap. Registration
// order is preserved so the anonymous-identity enforcement test
// (tests/kernel/rbac/anonymous_identity_test.cpp) can iterate routes
// in a deterministic order.

// registry is inherently global state, populated at startup before app().run()
std::vector<RegisteredRule> route_rules;

// ── Helpers ──────────────────────────────────────────────────────────

using Callback = drogon::FilterCallback;
using SharedCb = std::shared_ptr<Callback>;

auto json_error_403(const std::string& error_code, const std::string& rule,
                    const std::string& message) -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = error_code;
  if (!rule.empty()) {
    json["rule"] = rule;
  }
  json["message"] = message;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
  resp->setStatusCode(drogon::k403Forbidden);
  return resp;
}

auto get_client_ip(const drogon::HttpRequestPtr& req) -> std::string {
  return req->peerAddr().toIp();
}

// Set RBAC context attributes on the request.
auto set_rbac_attributes(const drogon::HttpRequestPtr& req,
                         const std::vector<std::string>& effective_rules,
                         bool granted, const std::string& granting_rule)
    -> void {
  auto attrs = req->attributes();
  attrs->insert(ATTR_EFFECTIVE_RULES, effective_rules);
  attrs->insert(ATTR_PERMISSION_GRANTED, granted);
  attrs->insert(ATTR_GRANTING_RULE, granting_rule);
}

// Build comma-separated list of rules for error messages.
auto join_rules(const std::vector<std::string>& rules) -> std::string {
  std::string result;
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i > 0) {
      result += ", ";
    }
    result += "'" + rules[i] + "'";
  }
  return result;
}

// Format a vector of strings as a PostgreSQL text[] literal: {a,b,c}.
auto to_pg_text_array(const std::vector<std::string>& items) -> std::string {
  std::string arr = "{";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      arr += ",";
    }
    arr += items[i];
  }
  arr += "}";
  return arr;
}

// Build the audit-log detail JSON for an RBAC denial.
auto build_denial_detail(const std::vector<std::string>& required_rules,
                         drogon::HttpMethod method,
                         const std::string& path_pattern,
                         const std::string& reason,
                         const std::string& unregistered_rule) -> Json::Value {
  Json::Value detail;
  detail["required_rules"] = Json::arrayValue;
  for (const auto& r : required_rules) {
    detail["required_rules"].append(r);
  }
  detail["reason"] = reason;
  detail["path"] = path_pattern;
  detail["method"] = std::string{drogon::to_string_view(method)};
  if (!unregistered_rule.empty()) {
    detail["unregistered_rule"] = unregistered_rule;
  }
  return detail;
}

// Parameters captured through the filter's async callback chain.
struct DenialContext {
  drogon::HttpRequestPtr req;
  drogon::orm::DbClientPtr db;
  std::vector<std::string> required_rules;
  std::vector<std::string> effective_list;
  std::string user_id;
  std::string session_id;
  std::string ip;
  std::string path_pattern;
};

// Handle the denial path: distinguish "rule not registered" from "user
// lacks rule", audit the denial, and invoke the filter callback with 403.
auto handle_denial(const DenialContext& ctx,
                   const drogon::orm::Result& rule_check,
                   const SharedCb& shared_fcb) -> void {
  std::unordered_set<std::string> registered;
  for (const auto& row : rule_check) {
    registered.insert(row["rule"].as<std::string>());
  }

  // Find first unregistered rule — that's a configuration bug, not
  // a user-permission problem; call it out explicitly.
  for (const auto& rule : ctx.required_rules) {
    if (!registered.contains(rule)) {
      spdlog::warn("RBAC denied: rule '{}' not registered (user={}, path={})",
                   rule, ctx.user_id, ctx.path_pattern);

      auto detail =
          build_denial_detail(ctx.required_rules, ctx.req->method(),
                              ctx.path_pattern, "rule_not_registered", rule);
      plinth::log::audit("rbac.denied", detail,
                         {.user_id = ctx.user_id,
                          .session_id = ctx.session_id,
                          .ip_address = ctx.ip});

      set_rbac_attributes(ctx.req, ctx.effective_list, false, "");
      (*shared_fcb)(json_error_403("permission_denied", rule,
                                   "Required permission '" + rule +
                                       "' is not registered in the system"));
      return;
    }
  }

  // All rules are registered but user lacks them.
  const auto& first_rule = ctx.required_rules.front();
  spdlog::info("RBAC denied: user={} lacks required rule(s) [{}] for {}",
               ctx.user_id, join_rules(ctx.required_rules), ctx.path_pattern);

  auto detail = build_denial_detail(ctx.required_rules, ctx.req->method(),
                                    ctx.path_pattern, "permission_denied", "");
  plinth::log::audit("rbac.denied", detail,
                     {.user_id = ctx.user_id,
                      .session_id = ctx.session_id,
                      .ip_address = ctx.ip});

  set_rbac_attributes(ctx.req, ctx.effective_list, false, "");
  (*shared_fcb)(json_error_403("permission_denied", first_rule,
                               "You do not have permission for this action. "
                               "This requires the " +
                                   join_rules(ctx.required_rules) +
                                   " permission."));
}

} // namespace

// ── Public API: route registry ───────────────────────────────────────

auto register_rule_requirement(drogon::HttpMethod method,
                               const std::string& path_pattern,
                               std::vector<std::string> rules) -> void {
  for (auto& entry : route_rules) {
    if (entry.method == method && entry.path_pattern == path_pattern) {
      entry.rules = std::move(rules);
      return;
    }
  }
  route_rules.push_back({method, path_pattern, std::move(rules)});
}

auto get_required_rules(drogon::HttpMethod method,
                        const std::string& path_pattern)
    -> std::optional<std::vector<std::string>> {
  for (const auto& entry : route_rules) {
    if (entry.method == method && entry.path_pattern == path_pattern) {
      return entry.rules;
    }
  }
  return std::nullopt;
}

auto list_registered_rules() -> std::vector<RegisteredRule> {
  return route_rules;
}

// ── Public API: RBAC context extraction ──────────────────────────────

auto get_rbac_context(const drogon::HttpRequestPtr& req)
    -> std::optional<RbacContext> {
  auto attrs = req->attributes();
  auto granted = attrs->find(ATTR_PERMISSION_GRANTED);
  if (!granted) {
    return std::nullopt;
  }

  RbacContext ctx;
  ctx.effective_rules =
      attrs->get<std::vector<std::string>>(ATTR_EFFECTIVE_RULES);
  ctx.permission_granted = attrs->get<bool>(ATTR_PERMISSION_GRANTED);
  ctx.granting_rule = attrs->get<std::string>(ATTR_GRANTING_RULE);
  return ctx;
}

// ── RbacFilter implementation ────────────────────────────────────────

auto RbacFilter::doFilter(const drogon::HttpRequestPtr& req,
                          drogon::FilterCallback&& fcb,
                          drogon::FilterChainCallback&& fccb) -> void {
  // 1. Read user_id from auth context (set by SessionFilter).
  auto user_id = req->attributes()->get<std::string>(auth::ATTR_USER_ID);
  if (user_id.empty()) {
    // No authentication context — fail closed.
    fcb(json_error_403("permission_denied", "",
                       "Authentication required for this resource"));
    return;
  }

  // 2. Look up required rules for this route.
  auto path_pattern = std::string{req->getMatchedPathPattern()};
  auto required = get_required_rules(req->method(), path_pattern);

  if (!required.has_value() || required->empty()) {
    // No RBAC rules declared — pass through (authenticated-only route).
    fccb();
    return;
  }

  auto required_rules = std::move(required.value());
  auto session_id = req->attributes()->get<std::string>(auth::ATTR_SESSION_ID);
  auto ip = get_client_ip(req);

  // 3. Query effective rules: union of all rules from all groups the user
  // belongs to.
  auto db = drogon::app().getDbClient();
  auto shared_fcb = std::make_shared<Callback>(std::move(fcb));

  db->execSqlAsync(
      "SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
      "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
      "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
      "WHERE gm.user_id = $1::uuid",
      [req, shared_fcb, fccb = std::move(fccb), required_rules, db, user_id,
       session_id, ip,
       path_pattern](const drogon::orm::Result& result) mutable {
        // Build effective rules set.
        std::vector<std::string> effective_list;
        std::unordered_set<std::string> effective_set;
        effective_list.reserve(result.size());
        for (const auto& row : result) {
          auto rule = row["rule"].as<std::string>();
          effective_set.insert(rule);
          effective_list.push_back(std::move(rule));
        }

        // 4. ANY required rule in effective set grants access.
        for (const auto& rule : required_rules) {
          if (effective_set.contains(rule)) {
            set_rbac_attributes(req, effective_list, true, rule);
            fccb();
            return;
          }
        }

        // 5. Denied — distinguish "unregistered rule" from "user lacks rule"
        //    via a second query (runs only on the denial path).
        DenialContext ctx{.req = req,
                          .db = db,
                          .required_rules = required_rules,
                          .effective_list = effective_list,
                          .user_id = user_id,
                          .session_id = session_id,
                          .ip = ip,
                          .path_pattern = path_pattern};
        db->execSqlAsync(
            "SELECT rule FROM plinth.rbac_rules WHERE rule = ANY($1::text[])",
            [ctx, shared_fcb](const drogon::orm::Result& rule_check) {
              handle_denial(ctx, rule_check, shared_fcb);
            },
            [shared_fcb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("RBAC rule existence check failed: {}",
                            e.base().what());
              (*shared_fcb)(json_error_403("permission_denied", "",
                                           "Permission check failed"));
            },
            to_pg_text_array(required_rules));
      },
      [shared_fcb](const drogon::orm::DrogonDbException& e) {
        // Fail closed on DB error.
        spdlog::error("RBAC enforcement DB error: {}", e.base().what());
        (*shared_fcb)(
            json_error_403("permission_denied", "", "Permission check failed"));
      },
      user_id);
}

} // namespace plinth::rbac
