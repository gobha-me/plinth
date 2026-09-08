#pragma once

// WebSocket auth flow: 5s timer, validate auth message, register in
// the connection registry (handling displacement), send connected frame.
// Heartbeat / subscribe / publish are wired up in their own modules.

#include <drogon/HttpRequest.h>
#include <drogon/WebSocketConnection.h>
#include <json/value.h>
#include <string>

namespace plinth::ws {

// Schedule the auth-timeout timer on `conn`'s loop. Captures a weak ptr
// so the timer never extends connection lifetime. On expiry: send an
// error frame and shutdown with WsCloseCode::AUTH_TIMEOUT.
auto start_auth_timer(const drogon::WebSocketConnectionPtr& conn,
                      double timeout_s) -> void;

// Authenticate browser upgrades from their HttpOnly session cookie. Cookie
// authentication requires an exact Origin match to the actual scheme and Host;
// forwarded headers are not trusted. No cookie and no Origin retains native
// token-frame authentication.
auto on_session_upgrade(const drogon::HttpRequestPtr& req,
                        const drogon::WebSocketConnectionPtr& conn,
                        const std::string& node_id,
                        const std::string& browser_origin) -> void;

// Handle a parsed `{type:"auth", token:"..."}` message. Validates the
// token via the shared middleware path; on success populates ConnState,
// registers in the registry (closing any displaced peer with code 4003),
// and emits the `connected` frame. On failure: error + close 4002.
auto on_auth_message(const drogon::WebSocketConnectionPtr& conn,
                     const Json::Value& msg, const std::string& node_id)
    -> void;

} // namespace plinth::ws
