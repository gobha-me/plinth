#include "kernel/ws/auth_flow.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/logging.hpp"
#include "kernel/ws/close_codes.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/heartbeat.hpp"
#include "kernel/ws/messages.hpp"

#include <drogon/drogon.h>
#include <memory>

namespace plinth::ws {

namespace {

// Send a JSON error frame, then shutdown with the given application close
// code. Sending the JSON before the Close frame lets browser clients read
// `event.data` plus `event.code`.
auto send_error_and_close(const drogon::WebSocketConnectionPtr& conn,
                          WsCloseCode code, std::string_view error_code,
                          std::string_view message) -> void {
  if (!conn->connected()) {
    return;
  }
  conn->sendJson(msg::make_error(error_code, message));
  conn->shutdown(to_drogon(code), std::string{error_code});
}

// Build the registry key for an authenticated connection.
auto make_key(const plinth::auth::AuthContext& ctx) -> RegistryKey {
  return RegistryKey{
      .auth_type = ctx.auth_type,
      .id = ctx.auth_type == "session" ? ctx.session_id : ctx.pat_id,
  };
}

auto peer_ip(const drogon::WebSocketConnectionPtr& conn) -> std::string {
  return conn->peerAddr().toIp();
}

// Close a displaced peer on its own event loop (captured in ConnState).
// Fallback: close inline if the displaced state is missing (race at tear-down).
auto close_displaced(const drogon::WebSocketConnectionPtr& displaced) -> void {
  auto close_fn = [displaced]() {
    send_error_and_close(displaced, WsCloseCode::ALREADY_CONNECTED,
                         "already_connected",
                         "Another connection has claimed this session");
  };
  auto* state = displaced->getContext<ConnState>().get();
  if (state != nullptr && state->loop != nullptr) {
    state->loop->queueInLoop(close_fn);
  } else {
    close_fn();
  }
}

// Handle the failure path: audit + error frame + close.
auto on_auth_failure(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& reason) -> void {
  Json::Value detail;
  detail["reason"] = reason;
  plinth::log::audit(
      "ws.auth_failed", detail,
      {.user_id = "", .session_id = "", .ip_address = peer_ip(conn)});
  send_error_and_close(conn, WsCloseCode::AUTH_FAILED, "auth_failed",
                       "Authentication failed");
}

// Finalize the auth flow once effective rules are known: cancel timer,
// populate admin flag, register in the registry (closing any displaced
// peer), send connected, set authenticated=true, start heartbeat, audit.
auto finish_auth(const drogon::WebSocketConnectionPtr& conn, bool is_admin,
                 const std::string& node_id) -> void {
  auto state_ptr = conn->getContext<ConnState>();
  auto* state = state_ptr.get();
  if (state == nullptr || state->authenticated) {
    return;
  }

  if (state->auth_timer_id != trantor::InvalidTimerId) {
    state->loop->invalidateTimer(state->auth_timer_id);
    state->auth_timer_id = trantor::InvalidTimerId;
  }
  state->is_admin = is_admin;

  auto displaced = ConnectionRegistry::instance().register_connection(
      make_key(state->auth), conn, state_ptr);
  if (displaced && displaced.get() != conn.get()) {
    Json::Value detail;
    detail["new_peer"] = peer_ip(conn);
    plinth::log::audit("ws.displaced", detail,
                       {.user_id = state->auth.user_id,
                        .session_id = state->auth.session_id,
                        .ip_address = peer_ip(displaced)});
    close_displaced(displaced);
  }

  conn->sendJson(msg::make_connected(state->auth.user_id, state->auth.username,
                                     state->auth.session_id, state->auth.pat_id,
                                     node_id));

  state->authenticated = true;
  start_heartbeat(conn, state->heartbeat_interval_s,
                  state->heartbeat_timeout_s);

  Json::Value detail;
  detail["auth_type"] = state->auth.auth_type;
  if (!state->auth.pat_id.empty()) {
    detail["pat_id"] = state->auth.pat_id;
  }
  plinth::log::audit("ws.connected", detail,
                     {.user_id = state->auth.user_id,
                      .session_id = state->auth.session_id,
                      .ip_address = peer_ip(conn)});
}

// After token validation, query the full effective rule-set so the
// per-channel subscribe gate (ICD-0.5.2 Phase 3) has a local snapshot
// to check against on its loop. `is_admin` is derived from presence
// of `kernel.admin` in the same result set — no second query needed.
auto resolve_rbac_and_finish(const drogon::WebSocketConnectionPtr& conn,
                             const std::string& node_id) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr) {
    return;
  }
  auto user_id = state->auth.user_id;
  trantor::EventLoop* loop = state->loop;
  std::weak_ptr<drogon::WebSocketConnection> weak{conn};

  auto db = drogon::app().getDbClient();
  db->execSqlAsync(
      "SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
      "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
      "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
      "WHERE gm.user_id = $1::uuid",
      [weak, loop, node_id](const drogon::orm::Result& result) {
        std::unordered_set<std::string> rules;
        rules.reserve(result.size());
        for (const auto& row : result) {
          rules.insert(row["rule"].as<std::string>());
        }
        bool is_admin = rules.contains("kernel.admin");
        loop->queueInLoop(
            [weak, rules = std::move(rules), is_admin, node_id]() mutable {
              auto strong = weak.lock();
              if (!strong || !strong->connected()) {
                return;
              }
              auto* st = strong->getContext<ConnState>().get();
              if (st == nullptr) {
                return;
              }
              st->effective_rules = std::move(rules);
              finish_auth(strong, is_admin, node_id);
            });
      },
      [weak, loop, node_id](const drogon::orm::DrogonDbException& e) {
        spdlog::error("ws: effective-rules query failed: {}", e.base().what());
        // Fail safe: treat as non-admin with an empty rule set so
        // auth still completes — subscribe will silent-omit every
        // non-admin request.
        loop->queueInLoop([weak, node_id]() {
          if (auto strong = weak.lock()) {
            if (strong->connected()) {
              finish_auth(strong, /*is_admin=*/false, node_id);
            }
          }
        });
      },
      user_id);
}

} // namespace

auto start_auth_timer(const drogon::WebSocketConnectionPtr& conn,
                      double timeout_s) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || state->loop == nullptr) {
    return;
  }

  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  auto timer_id = state->loop->runAfter(timeout_s, [weak]() {
    auto strong = weak.lock();
    if (!strong || !strong->connected()) {
      return;
    }
    auto* st = strong->getContext<ConnState>().get();
    if (st == nullptr || st->authenticated) {
      return;
    }
    plinth::log::audit(
        "ws.auth_timeout", Json::Value{Json::objectValue},
        {.user_id = "", .session_id = "", .ip_address = peer_ip(strong)});
    send_error_and_close(strong, WsCloseCode::AUTH_TIMEOUT, "auth_timeout",
                         "Authentication timeout");
  });
  state->auth_timer_id = timer_id;
}

auto on_auth_message(const drogon::WebSocketConnectionPtr& conn,
                     const Json::Value& msg, const std::string& node_id)
    -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || state->authenticated) {
    return; // No state (impossible) or auth replay — ignore.
  }

  auto token = msg["token"].asString();
  if (token.empty()) {
    on_auth_failure(conn, "missing_token");
    return;
  }

  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  plinth::auth::validate_token(
      token,
      [weak, node_id](const plinth::auth::TokenValidationResult& result) {
        auto strong = weak.lock();
        if (!strong || !strong->connected()) {
          return;
        }
        auto* st = strong->getContext<ConnState>().get();
        if (st == nullptr || st->loop == nullptr) {
          return;
        }
        // Hop back to the connection's owning loop so per-conn state
        // mutations stay loop-local.
        st->loop->queueInLoop([weak, node_id, result]() {
          auto conn_strong = weak.lock();
          if (!conn_strong || !conn_strong->connected()) {
            return;
          }
          auto* ctx_st = conn_strong->getContext<ConnState>().get();
          if (ctx_st == nullptr || ctx_st->authenticated) {
            return;
          }
          if (!result.ok) {
            on_auth_failure(conn_strong, result.error_code);
            return;
          }
          // Record the auth context on the connection, then load
          // the full effective rule set. Finalization happens in
          // finish_auth().
          ctx_st->auth = result.context;
          resolve_rbac_and_finish(conn_strong, node_id);
        });
      });
}

} // namespace plinth::ws
