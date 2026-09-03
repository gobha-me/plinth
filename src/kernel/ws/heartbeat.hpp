#pragma once

// Application-level ping/pong heartbeat per ICD-0.1.6 §Heartbeat.
// Runs on the connection's own event loop.

#include <drogon/WebSocketConnection.h>
#include <json/value.h>

namespace plinth::ws {

// Schedule the heartbeat ping timer on conn's loop. Every `interval_s`
// seconds, sends a JSON ping; if the previous ping's pong did not arrive
// within `timeout_s`, closes the connection with
// WsCloseCode::HEARTBEAT_TIMEOUT. Idempotent — calling twice replaces the prior
// timer.
auto start_heartbeat(const drogon::WebSocketConnectionPtr& conn,
                     double interval_s, double timeout_s) -> void;

// Handle a parsed `{type:"pong", timestamp:N}` frame — clears the pending
// ping flag if the timestamp matches what we sent.
auto on_pong_message(const drogon::WebSocketConnectionPtr& conn,
                     const Json::Value& msg) -> void;

} // namespace plinth::ws
