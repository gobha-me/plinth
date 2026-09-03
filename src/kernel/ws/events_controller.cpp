#include "kernel/ws/events_controller.hpp"

#include "kernel/logging.hpp"
#include "kernel/ws/auth_flow.hpp"
#include "kernel/ws/call_dispatch.hpp"
#include "kernel/ws/close_codes.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/heartbeat.hpp"
#include "kernel/ws/messages.hpp"
#include "kernel/ws/publish.hpp"
#include "kernel/ws/subscriptions.hpp"

#include <drogon/drogon.h>
#include <json/reader.h>
#include <memory>
#include <mutex>
#include <string>

namespace plinth::ws {

namespace {

auto peer_ip(const drogon::WebSocketConnectionPtr& conn) -> std::string {
  return conn->peerAddr().toIp();
}

// Parse a text frame as JSON. On parse failure: log a debug line, return
// an empty Value (controller treats this as an unknown message type and
// ignores it; per ICD we don't error on malformed frames).
auto parse_json(std::string_view text) -> Json::Value {
  Json::Value root;
  Json::CharReaderBuilder builder;
  auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
  std::string errs;
  if (!reader->parse(text.data(), text.data() + text.size(), &root, &errs)) {
    spdlog::debug("ws: malformed JSON frame: {}", errs);
    return {};
  }
  return root;
}

} // namespace

EventsController::EventsController(double auth_timeout_s,
                                   double heartbeat_interval_s,
                                   double heartbeat_timeout_s,
                                   std::string node_id)
    : auth_timeout_s(auth_timeout_s),
      heartbeat_interval_s(heartbeat_interval_s),
      heartbeat_timeout_s(heartbeat_timeout_s), node_id(std::move(node_id)) {
}

auto EventsController::handleNewConnection(
    const drogon::HttpRequestPtr& /*req*/,
    const drogon::WebSocketConnectionPtr& conn) -> void {
  // Suppress Drogon's protocol-level ping; ICD-0.1.6 specifies
  // application-level JSON ping/pong so the JS SDK can observe it.
  conn->disablePing();

  auto state = std::make_shared<ConnState>();
  state->loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  state->heartbeat_interval_s = heartbeat_interval_s;
  state->heartbeat_timeout_s = heartbeat_timeout_s;
  conn->setContext(state);
  start_auth_timer(conn, auth_timeout_s);

  spdlog::debug("ws: new connection from {}", peer_ip(conn));
}

auto EventsController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    // Drogon's override signature takes std::string&&; we only read it.
    std::string&& message, const drogon::WebSocketMessageType& type) -> void {
  // Per ICD §Security Constraints, only text frames are accepted.
  // Binary frames are silently dropped (deferred per "What Must Not Be
  // Decided Yet" §1).
  if (type != drogon::WebSocketMessageType::Text) {
    return;
  }

  auto root = parse_json(message);
  if (!root.isObject()) {
    return;
  }

  auto msg_type = root["type"].asString();
  if (msg_type == msg::AUTH) {
    on_auth_message(conn, root, node_id);
    return;
  }
  if (msg_type == msg::PONG) {
    on_pong_message(conn, root);
    return;
  }
  if (msg_type == msg::SUBSCRIBE) {
    on_subscribe(conn, root);
    return;
  }
  if (msg_type == msg::UNSUBSCRIBE) {
    on_unsubscribe(conn, root);
    return;
  }
  if (msg_type == msg::CALL) {
    on_call(conn, root);
    return;
  }
  if (msg_type == msg::DEBOUNCE_RENEGOTIATE) {
    on_debounce_renegotiate(conn, root);
    return;
  }

  spdlog::debug("ws: ignoring unhandled message type '{}'", msg_type);
}

auto EventsController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr) {
    return;
  }

  if (state->loop != nullptr) {
    if (state->auth_timer_id != trantor::InvalidTimerId) {
      state->loop->invalidateTimer(state->auth_timer_id);
      state->auth_timer_id = trantor::InvalidTimerId;
    }
    if (state->heartbeat_timer_id != trantor::InvalidTimerId) {
      state->loop->invalidateTimer(state->heartbeat_timer_id);
      state->heartbeat_timer_id = trantor::InvalidTimerId;
    }
  }

  {
    std::lock_guard lk(*state->channels_mu);
    if (!state->channels.empty()) {
      note_subscriptions_removed(state->channels.size());
      // Intentionally leave state->channels populated — conn->clearContext()
      // below drops the ConnState and the set goes with it.
    }
  }

  if (state->authenticated) {
    RegistryKey key{
        .auth_type = state->auth.auth_type,
        .id = state->auth.auth_type == "session" ? state->auth.session_id
                                                 : state->auth.pat_id,
    };
    ConnectionRegistry::instance().unregister_connection(key, conn);

    plinth::log::audit("ws.closed", Json::Value{Json::objectValue},
                       {.user_id = state->auth.user_id,
                        .session_id = state->auth.session_id,
                        .ip_address = peer_ip(conn)});
  }

  conn->clearContext();
  spdlog::debug("ws: connection closed for {}", peer_ip(conn));
}

} // namespace plinth::ws
