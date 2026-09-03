#pragma once

// Wire-format constants and JSON builders for /ws/events.
// Centralized so message strings don't leak into controller code.

#include <json/value.h>
#include <string>
#include <string_view>

namespace plinth::ws::msg {

// Inbound message types (client → server)
inline constexpr auto AUTH = "auth";
inline constexpr auto PONG = "pong";
inline constexpr auto SUBSCRIBE = "subscribe";
inline constexpr auto UNSUBSCRIBE = "unsubscribe";
inline constexpr auto CALL = "call";
// ICD-0.5.5 §7 — client may renegotiate the debounce advisory.
// Kernel ignores the renegotiation (advisory + audit, not enforced
// per OQ5) and emits `realtime.debounce.advisory_overridden`.
inline constexpr auto DEBOUNCE_RENEGOTIATE = "debounce_renegotiate";

// Outbound message types (server → client)
inline constexpr auto CONNECTED = "connected";
inline constexpr auto PING = "ping";
inline constexpr auto SUBSCRIBED = "subscribed";
inline constexpr auto UNSUBSCRIBED = "unsubscribed";
inline constexpr auto EVENT = "event";
inline constexpr auto ERROR_TYPE = "error";
inline constexpr auto CALL_RESULT = "call_result";
inline constexpr auto CALL_ERROR = "call_error";
// ICD-0.5.4 §Server frame additions — delta-sync handshake.
inline constexpr auto REPLAY = "replay";
inline constexpr auto REPLAY_DONE = "replay_done";
inline constexpr auto RESYNC = "resync";

// Build an error frame: {"type":"error","error":<code>,"message":<msg>}
inline auto make_error(std::string_view code, std::string_view message)
    -> Json::Value {
  Json::Value v;
  v["type"] = ERROR_TYPE;
  v["error"] = std::string{code};
  v["message"] = std::string{message};
  return v;
}

// Build the connected frame for a freshly authenticated connection.
// session_id may be empty for PAT-authed connections; if so we emit an
// empty string (consistent with AuthContext.session_id semantics) and
// surface pat_id alongside.
inline auto make_connected(const std::string& user_id,
                           const std::string& username,
                           const std::string& session_id,
                           const std::string& pat_id,
                           const std::string& node_id) -> Json::Value {
  Json::Value v;
  v["type"] = CONNECTED;
  v["user"]["id"] = user_id;
  v["user"]["username"] = username;
  v["session_id"] = session_id;
  if (!pat_id.empty()) {
    v["pat_id"] = pat_id;
  }
  v["node_id"] = node_id;
  return v;
}

// Build a ping frame with a millisecond-precision timestamp.
inline auto make_ping(int64_t timestamp_ms) -> Json::Value {
  Json::Value v;
  v["type"] = PING;
  v["timestamp"] = static_cast<Json::Int64>(timestamp_ms);
  return v;
}

} // namespace plinth::ws::msg
