#include "kernel/ws/heartbeat.hpp"

#include "kernel/logging.hpp"
#include "kernel/ws/close_codes.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/messages.hpp"

#include <chrono>
#include <drogon/drogon.h>

namespace plinth::ws {

namespace {

auto now_ms() -> int64_t {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

auto peer_ip(const drogon::WebSocketConnectionPtr& conn) -> std::string {
  return conn->peerAddr().toIp();
}

auto close_heartbeat_timeout(const drogon::WebSocketConnectionPtr& conn,
                             int64_t pending_since) -> void {
  Json::Value detail;
  detail["idle_ms"] = Json::Int64{now_ms() - pending_since};
  const auto* state = conn->getContext<ConnState>().get();
  std::string user_id;
  std::string session_id;
  if (state != nullptr) {
    user_id = state->auth.user_id;
    session_id = state->auth.session_id;
  }
  plinth::log::audit("ws.heartbeat_timeout", detail,
                     {.user_id = user_id,
                      .session_id = session_id,
                      .ip_address = peer_ip(conn)});
  if (conn->connected()) {
    conn->sendJson(msg::make_error("heartbeat_timeout", "Pong timeout"));
    conn->shutdown(to_drogon(WsCloseCode::HEARTBEAT_TIMEOUT),
                   "heartbeat_timeout");
  }
}

// Fired when the pong for a specific ping did not arrive within timeout_s.
// Close only if the same ping is still pending — otherwise the client
// already responded and we issued a new ping in the meantime.
auto check_pong_timeout(const drogon::WebSocketConnectionPtr& conn,
                        int64_t ping_ts) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !conn->connected()) {
    return;
  }
  if (state->pending_ping_ts == ping_ts) {
    close_heartbeat_timeout(conn, ping_ts);
  }
}

// Fired on each heartbeat interval. Sends a ping, remembers the timestamp,
// and schedules a one-shot timeout check.
auto send_ping(const drogon::WebSocketConnectionPtr& conn, double timeout_s)
    -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !state->authenticated || state->loop == nullptr) {
    return;
  }

  // Previous ping still outstanding when the next interval fires = timeout.
  if (state->pending_ping_ts != 0) {
    close_heartbeat_timeout(conn, state->pending_ping_ts);
    return;
  }

  auto ts = now_ms();
  state->pending_ping_ts = ts;
  conn->sendJson(msg::make_ping(ts));

  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  state->loop->runAfter(timeout_s, [weak, ts]() {
    if (auto strong = weak.lock()) {
      check_pong_timeout(strong, ts);
    }
  });
}

} // namespace

auto start_heartbeat(const drogon::WebSocketConnectionPtr& conn,
                     double interval_s, double timeout_s) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || state->loop == nullptr) {
    return;
  }
  if (state->heartbeat_timer_id != trantor::InvalidTimerId) {
    state->loop->invalidateTimer(state->heartbeat_timer_id);
  }

  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  state->heartbeat_timer_id =
      state->loop->runEvery(interval_s, [weak, timeout_s]() {
        auto strong = weak.lock();
        if (!strong || !strong->connected()) {
          return;
        }
        send_ping(strong, timeout_s);
      });
}

auto on_pong_message(const drogon::WebSocketConnectionPtr& conn,
                     const Json::Value& msg) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !state->authenticated) {
    return;
  }
  auto ts = msg["timestamp"].asInt64();
  if (ts == state->pending_ping_ts) {
    state->pending_ping_ts = 0;
  }
  // Mismatched timestamps are ignored — stale pong, or client noise.
}

} // namespace plinth::ws
