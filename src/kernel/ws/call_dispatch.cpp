#include "kernel/ws/call_dispatch.hpp"

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/js_stress.hpp"
#include "kernel/ws/messages.hpp"

#include <drogon/drogon.h>
#include <string>
#include <vector>

namespace plinth::ws {

namespace {

auto peer_ip(const drogon::WebSocketConnectionPtr& conn) -> std::string {
  return conn->peerAddr().toIp();
}

auto make_call_result(const std::string& id,
                      const capabilities::CapabilityResult& r) -> Json::Value {
  Json::Value v;
  v["type"] = msg::CALL_RESULT;
  v["id"] = id;
  v["value"] = r.data;
  v["resolved_tier"] = r.resolved_tier;
  v["provider_type"] = r.provider_type;
  return v;
}

auto make_call_error(const std::string& id, std::string_view code,
                     std::string_view message) -> Json::Value {
  Json::Value v;
  v["type"] = msg::CALL_ERROR;
  v["id"] = id;
  v["code"] = std::string{code};
  v["message"] = std::string{message};
  return v;
}

// Build a UserContext for the resolver from ConnState. ConnState carries
// is_admin (per ws/auth_flow.cpp) but not the full rules vector; see the
// RBAC comment in call_dispatch.hpp for why this is the LH-0-scope choice.
auto to_user_context(const ConnState& state, const std::string& ip)
    -> capabilities::UserContext {
  std::vector<std::string> rules;
  if (state.is_admin) {
    rules.emplace_back("kernel.admin");
  }
  return capabilities::UserContext{
      .user_id = state.auth.user_id,
      .username = state.auth.username,
      .auth_type = state.auth.auth_type,
      .effective_rules = std::move(rules),
      .session_id = state.auth.session_id,
      .ip_address = ip,
  };
}

} // namespace

auto on_call(const drogon::WebSocketConnectionPtr& conn,
             const Json::Value& msg_in) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !state->authenticated) {
    return;
  }

  auto call_id =
      msg_in["id"].isString() ? msg_in["id"].asString() : std::string{};
  auto signature = msg_in["signature"].isString()
                       ? msg_in["signature"].asString()
                       : std::string{};

  if (signature.empty()) {
    conn->sendJson(make_call_error(call_id, "invalid_call",
                                   "missing or non-string 'signature'"));
    return;
  }

  auto args = msg_in["args"];
  if (args.isNull()) {
    args = Json::Value{Json::arrayValue};
  }

  // LH-0.1 fork — see ICD-LH-0.1 §4. Signatures other than
  // lh0:1:js_stress fall through to the standard resolver.
  if (try_dispatch_js_stress(conn, call_id, signature, args, state->is_admin)) {
    return;
  }

  auto ctx = to_user_context(*state, peer_ip(conn));
  capabilities::CapabilityCall call{
      .signature = std::move(signature),
      .args = std::move(args),
      .call_depth = 0,
  };
  auto async_task = plinth::lifecycle::async_tasks().try_acquire();
  if (async_task == nullptr) {
    conn->sendJson(make_call_error(call_id, "server_shutting_down",
                                   "server is shutting down"));
    return;
  }

  // ICD-0.5.0.3 §Sync vs async — WS migrated to `call_capability_async`
  // so extension capability entries can dispatch into the
  // RuntimeRegistry without blocking an event-loop thread on a JS
  // evaluation. Kernel-Tier-1 caps still resolve in-memory; the
  // coroutine adds only a `co_await` scheduling hop for those.
  //
  // captures are owned by the lambda closure for the coroutine's lifetime:
  // `conn` is a shared_ptr, `call`/`ctx`/`call_id` are move-captured values.
  // drogon::async_run keeps the closure alive across co_awaits. The project's
  // other async_run + coroutine uses follow the same pattern.
  try {
    drogon::async_run([conn = conn, call = std::move(call),
                       ctx = std::move(ctx), async_task,
                       call_id]() -> drogon::Task<> {
      try {
        std::string ext_detail_code;
        std::string ext_detail_message;
        auto result = co_await capabilities::call_capability_async(
            call, ctx, &ext_detail_code, &ext_detail_message);
        if (result) {
          conn->sendJson(make_call_result(call_id, *result));
          co_return;
        }

        auto enum_code = result.error();
        if (enum_code ==
                capabilities::CapabilityError::EXTENSION_DISPATCH_FAILED &&
            !ext_detail_code.empty()) {
          conn->sendJson(
              make_call_error(call_id, ext_detail_code, ext_detail_message));
          co_return;
        }
        auto code_sv = capabilities::error_code(enum_code);
        conn->sendJson(make_call_error(call_id, code_sv, code_sv));
      } catch (...) {
        if (conn->connected()) {
          conn->sendJson(make_call_error(call_id, "internal_error",
                                         "capability dispatch failed"));
        }
      }
    });
  } catch (...) {
    if (conn->connected()) {
      conn->sendJson(make_call_error(call_id, "internal_error",
                                     "capability dispatch failed"));
    }
  }
}

} // namespace plinth::ws
