#include "kernel/ws/auth_flow.hpp"
#include "kernel/ws/authority.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/ws/close_codes.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/heartbeat.hpp"
#include "kernel/ws/messages.hpp"

#include <drogon/drogon.h>
#include <memory>
#include <utility>

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

// The registry snapshots the target loop and owns the displaced state under
// its lock. Never read another connection's context from the new peer's loop:
// handleConnectionClosed may concurrently clear that shared_ptr on the target.
auto close_displaced(RegistryEntry displaced) -> void {
  auto* loop = displaced.loop;
  auto close_fn = [displaced = std::move(displaced)]() {
    send_error_and_close(displaced.conn, WsCloseCode::ALREADY_CONNECTED,
                         "already_connected",
                         "Another connection has claimed this session");
  };
  if (loop != nullptr) {
    loop->queueInLoop(std::move(close_fn));
  } else {
    close_fn();
  }
}

// Handle the failure path: audit + error frame + close.
auto on_auth_failure(const drogon::WebSocketConnectionPtr& conn,
                     const std::string& reason) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state != nullptr) {
    state->auth_failed = true;
  }
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
  if (state == nullptr || state->authenticated || state->auth_failed ||
      !conn->connected()) {
    return;
  }

  if (state->auth_timer_id != trantor::InvalidTimerId) {
    state->loop->invalidateTimer(state->auth_timer_id);
    state->auth_timer_id = trantor::InvalidTimerId;
  }
  state->is_admin = is_admin;
  state->authenticated = true;

  auto displaced = ConnectionRegistry::instance().register_connection(
      make_key(state->auth), conn, state_ptr);
  if (displaced.conn && displaced.conn.get() != conn.get()) {
    Json::Value detail;
    detail["new_peer"] = peer_ip(conn);
    plinth::log::audit("ws.displaced", detail,
                       {.user_id = state->auth.user_id,
                        .session_id = state->auth.session_id,
                        .ip_address = peer_ip(displaced.conn)});
    close_displaced(std::move(displaced));
  }

  conn->sendJson(msg::make_connected(state->auth.user_id, state->auth.username,
                                     state->auth.session_id, state->auth.pat_id,
                                     node_id));

  start_authority_monitor(conn);
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
    if (st == nullptr || st->authenticated || st->auth_failed) {
      return;
    }
    st->auth_failed = true;
    plinth::log::audit(
        "ws.auth_timeout", Json::Value{Json::objectValue},
        {.user_id = "", .session_id = "", .ip_address = peer_ip(strong)});
    send_error_and_close(strong, WsCloseCode::AUTH_TIMEOUT, "auth_timeout",
                         "Authentication timeout");
  });
  state->auth_timer_id = timer_id;
}

namespace {

auto authenticate_token(const drogon::WebSocketConnectionPtr& conn,
                        const std::string& token, const std::string& node_id,
                        bool session_only) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || state->auth_started || state->auth_failed ||
      state->authenticated || state->loop == nullptr) {
    return;
  }
  state->auth_started = true;
  if (token.empty()) {
    on_auth_failure(conn, "missing_token");
    return;
  }

  auto task = plinth::lifecycle::async_tasks().try_acquire();
  if (!task) {
    on_auth_failure(conn, "server_shutting_down");
    return;
  }
  auto task_owner =
      std::make_shared<std::shared_ptr<plinth::lifecycle::AsyncTaskLease>>(
          std::move(task));
  auto completed = std::make_shared<std::atomic<bool>>(false);
  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  auto* loop = state->loop;
  auto validated = [weak, loop, node_id, task_owner, completed](
                       const plinth::auth::TokenValidationResult& result) {
    if (completed->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    auto completion_task = std::exchange(*task_owner, {});
    // Database callbacks never read or mutate connection context off-loop.
    loop->queueInLoop([weak, node_id, result, completion_task]() {
      auto strong = weak.lock();
      if (!strong || !strong->connected()) {
        return;
      }
      auto* st = strong->getContext<ConnState>().get();
      if (st == nullptr || st->authenticated || st->auth_failed) {
        return;
      }
      if (!result.ok) {
        on_auth_failure(strong, result.error_code);
        return;
      }
      st->auth = result.context;
      establish_authority(strong, [weak, node_id]() {
        if (auto ready = weak.lock()) {
          auto ready_state = ready->getContext<ConnState>();
          if (ready_state) {
            finish_auth(ready, ready_state->is_admin, node_id);
          }
        }
      });
    });
  };
  if (session_only) {
    plinth::auth::validate_session_token(
        token, std::move(validated), drogon::app().getDbClient("ws_authority"));
  } else {
    plinth::auth::validate_token(token, std::move(validated),
                                 drogon::app().getDbClient("ws_authority"));
  }
}

} // namespace

auto on_session_upgrade(const drogon::HttpRequestPtr& req,
                        const drogon::WebSocketConnectionPtr& conn,
                        const std::string& node_id,
                        const std::string& browser_origin) -> void {
  const auto& cookie = req->getCookie("plinth_session");
  const auto& origin = req->getHeader("origin");
  if (cookie.empty() && origin.empty()) {
    return; // Native clients authenticate explicitly with a token frame.
  }
  const auto& host = req->getHeader("host");
  const std::string scheme =
      req->isOnSecureConnection() ? "https://" : "http://";
  // The configured origin is validated at config loading. A proxy must
  // preserve Host; neither Forwarded nor X-Forwarded-* conveys authority.
  const auto expected = browser_origin.empty() ? scheme + host : browser_origin;
  const auto separator = expected.find("://");
  const bool host_matches =
      separator != std::string::npos && expected.substr(separator + 3) == host;
  if (host.empty() || !host_matches || origin != expected) {
    on_auth_failure(conn, "origin_mismatch");
    return;
  }
  authenticate_token(conn, cookie, node_id, /*session_only=*/true);
}

auto on_auth_message(const drogon::WebSocketConnectionPtr& conn,
                     const Json::Value& msg, const std::string& node_id)
    -> void {
  const auto& token = msg["token"];
  authenticate_token(conn, token.isString() ? token.asString() : "", node_id,
                     /*session_only=*/false);
}

} // namespace plinth::ws
