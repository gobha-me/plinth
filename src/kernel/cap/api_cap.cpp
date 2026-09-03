#include "kernel/cap/api_cap.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::cap {

namespace {

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

// Mirrors the SELECT in rbac/enforcement.cpp:261-264. Sync libpq because
// the cap-dispatch handler is already off-loop (Drogon dispatched it via
// the registered handler with the SessionFilter chain) and the resolver
// needs the rules vector before it can co_await call_capability_async.
auto load_effective_rules(const Config::Database& db_cfg,
                          std::string_view user_id)
    -> std::vector<std::string> {
  std::vector<std::string> rules;
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::warn("cap::api_cap: PG connect failed: {}", PQerrorMessage(conn));
    PQfinish(conn);
    return rules;
  }
  auto cleanup = [](PGconn* c) { PQfinish(c); };
  std::unique_ptr<PGconn, decltype(cleanup)> guard(conn, cleanup);

  std::string user_id_str{user_id};
  std::array<const char*, 1> params{user_id_str.c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexecParams(conn,
                   "SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
                   "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
                   "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
                   "WHERE gm.user_id = $1::uuid",
                   1, nullptr, params.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::warn("cap::api_cap: rules SELECT failed: {}",
                 PQresultErrorMessage(res.get()));
    return rules;
  }
  int n = PQntuples(res.get());
  rules.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    rules.emplace_back(PQgetvalue(res.get(), i, 0));
  }
  return rules;
}

// Split `name` (the URL parameter from `/api/cap/{capability}`) into the
// resolver's signature triple `<namespace>:1:<function>`. Default
// version 1 (multi-version caps don't exist yet — see deviation #3).
// Returns std::nullopt when the URL parameter is malformed (no dot, or
// leading/trailing dot, or empty).
auto split_capability(std::string_view name) -> std::optional<std::string> {
  if (name.empty()) {
    return std::nullopt;
  }
  auto dot = name.find('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }
  if (dot == 0 || dot == name.size() - 1) {
    return std::nullopt;
  }
  auto ns = name.substr(0, dot);
  auto fn = name.substr(dot + 1);
  return std::string{ns} + ":1:" + std::string{fn};
}

auto json_response(int status, const Json::Value& body)
    -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  resp->setBody(Json::writeString(w, body));
  return resp;
}

auto make_success(const capabilities::CapabilityResult& r)
    -> drogon::HttpResponsePtr {
  Json::Value body;
  body["ok"] = true;
  body["value"] = r.data;
  body["resolved_tier"] = r.resolved_tier;
  body["provider_type"] = r.provider_type;
  return json_response(200, body);
}

auto make_error(int status, std::string_view code, std::string_view message)
    -> drogon::HttpResponsePtr {
  Json::Value body;
  body["ok"] = false;
  Json::Value error_obj;
  error_obj["code"] = std::string{code};
  error_obj["message"] = std::string{message};
  body["error"] = error_obj;
  return json_response(status, body);
}

// Map the resolver's CapabilityError to the JS-SDK-visible
// (status, code, message) triple per ICD §5.1 + §A.3. Translation:
//   capability_not_found  → 404 not_found
//   permission_denied     → 403 rbac_denied
//   invalid_*             → 400 (the kernel-native snake_case)
//   call_depth_exceeded   → 500 max_depth_exceeded
//   extension_dispatch_failed → 500 (with ext_detail_code if non-empty)
//   tier3_not_available / async_required / db_error / other → 500
struct StatusCode {
  int http;
  std::string_view code;
};

auto map_error(capabilities::CapabilityError e,
               std::string_view ext_detail_code) -> StatusCode {
  using E = capabilities::CapabilityError;
  switch (e) {
    case E::CAPABILITY_NOT_FOUND: return {.http = 404, .code = "not_found"};
    case E::PERMISSION_DENIED: return {.http = 403, .code = "rbac_denied"};
    case E::INVALID_CAPABILITY:
    case E::INVALID_NAMESPACE:
    case E::INVALID_VERSION:
    case E::INVALID_FUNCTION: return {.http = 400, .code = "bad_request"};
    case E::CALL_DEPTH_EXCEEDED:
      return {.http = 500, .code = "max_depth_exceeded"};
    case E::EXTENSION_DISPATCH_FAILED:
      if (ext_detail_code == "invalid_argument") {
        return {.http = 400, .code = "invalid_argument"};
      }
      if (ext_detail_code == "payload_too_large") {
        return {.http = 413, .code = "payload_too_large"};
      }
      return {.http = 500,
              .code = ext_detail_code.empty()
                          ? std::string_view{"dispatch_failed"}
                          : ext_detail_code};
    case E::CAPABILITY_DISABLED:
      return {.http = 503, .code = "capability_disabled"};
    case E::TIER3_NOT_AVAILABLE:
      return {.http = 500, .code = "tier3_not_available"};
    case E::ASYNC_REQUIRED: return {.http = 500, .code = "async_required"};
    case E::DB_ERROR: return {.http = 500, .code = "db_error"};
    default: return {.http = 500, .code = "internal_error"};
  }
}

auto handle_post_cap(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                     const Config::Database& db, const std::string& capability)
    -> void {
  // 1. Auth context (SessionFilter has run by the time we get here).
  auto auth_opt = auth::get_auth_context(req);
  if (!auth_opt.has_value() || auth_opt->user_id.empty()) {
    cb(make_error(401, "not_authenticated",
                  "Authentication required for /api/cap/*"));
    return;
  }
  auto auth = std::move(*auth_opt);

  // 2. Parse JSON body. Required shape: `{"args": [<arg>, ...]}`.
  auto body_or = req->getJsonObject();
  if (!body_or || body_or->isNull()) {
    cb(make_error(400, "bad_request", "request body must be JSON"));
    return;
  }
  const Json::Value& body = *body_or;
  if (!body.isObject() || !body.isMember("args")) {
    cb(make_error(400, "bad_request", "request body must contain 'args'"));
    return;
  }
  Json::Value args = body["args"];

  // 3. Synthesise the resolver signature from the URL parameter.
  auto sig_opt = split_capability(capability);
  if (!sig_opt.has_value()) {
    cb(make_error(400, "bad_request",
                  "capability name must be '<namespace>.<function>'"));
    return;
  }
  auto signature = std::move(*sig_opt);

  // 4. Build UserContext with full effective_rules (mirror RbacFilter).
  auto rules = load_effective_rules(db, auth.user_id);
  capabilities::UserContext ctx{
      .user_id = auth.user_id,
      .username = auth.username,
      .auth_type = auth.auth_type,
      .effective_rules = std::move(rules),
      .session_id = auth.session_id,
      .ip_address = req->peerAddr().toIp(),
  };
  capabilities::CapabilityCall call{
      .signature = std::move(signature),
      .args = std::move(args),
      .call_depth = 0,
  };
  auto async_task = plinth::lifecycle::async_tasks().try_acquire();
  if (async_task == nullptr) {
    cb(make_error(503, "server_shutting_down", "server is shutting down"));
    return;
  }
  auto response_callback =
      std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
          std::move(cb));

  // 5. Dispatch via the async resolver path.
  // captures own the data for the coroutine's lifetime: `cb`/`call`/`ctx` are
  // move-captured values; drogon::async_run keeps the closure alive across
  // co_awaits. Mirrors call_dispatch.cpp:109's precedent.
  try {
    drogon::async_run([response_callback, call = std::move(call),
                       ctx = std::move(ctx), async_task]() -> drogon::Task<> {
      try {
        std::string ext_code;
        std::string ext_message;
        auto result = co_await capabilities::call_capability_async(
            call, ctx, &ext_code, &ext_message);
        if (result) {
          (*response_callback)(make_success(*result));
          co_return;
        }
        auto enum_e = result.error();
        auto sc = map_error(enum_e, ext_code);
        std::string_view msg = ext_message.empty()
                                   ? capabilities::error_code(enum_e)
                                   : std::string_view{ext_message};
        (*response_callback)(make_error(sc.http, sc.code, msg));
      } catch (...) {
        (*response_callback)(
            make_error(500, "internal_error", "capability dispatch failed"));
      }
    });
  } catch (...) {
    (*response_callback)(
        make_error(500, "internal_error", "capability dispatch failed"));
  }
}

} // namespace

auto register_cap_routes(const Config::Database& db) -> void {
  constexpr auto SF = "plinth::auth::SessionFilter";
  drogon::app().registerHandler(
      "/api/cap/{capability}",
      [db](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb,
           const std::string& capability) {
        handle_post_cap(req, std::move(cb), db, capability);
      },
      {drogon::Post, SF});
  spdlog::info("cap: registered POST /api/cap/{{capability}}");
}

namespace test_seam {

auto dispatch_post_cap(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const Config::Database& db,
                       const std::string& capability) -> void {
  handle_post_cap(req, std::move(cb), db, capability);
}

} // namespace test_seam

} // namespace plinth::cap
