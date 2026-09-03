#pragma once

// WebSocket application-defined close codes (per ICD-0.1.6 §Connection
// Lifecycle).
//
// RFC 6455 reserves the 4000-4999 range for application use. Drogon's
// CloseCode enum tops out at 1015 but its WebSocketConnection::shutdown()
// implementation just htons-encodes whatever value is cast to it, so 4xxx
// codes are emitted correctly. The cast is intentional and verified
// against drogon::WebSocketConnectionImpl::shutdown().

#include <cstdint>
#include <drogon/WebSocketConnection.h>

namespace plinth::ws {

enum class WsCloseCode : uint16_t {
  NORMAL_CLOSURE = 1000,
  AUTH_TIMEOUT = 4001,
  AUTH_FAILED = 4002,
  ALREADY_CONNECTED = 4003,
  HEARTBEAT_TIMEOUT = 4004,
};

inline auto to_drogon(WsCloseCode code) -> drogon::CloseCode {
  return static_cast<drogon::CloseCode>(static_cast<uint16_t>(code));
}

} // namespace plinth::ws
