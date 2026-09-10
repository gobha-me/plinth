// SPDX-License-Identifier: MIT
#pragma once

#include <drogon/WebSocketConnection.h>
#include <functional>

namespace plinth::ws {

// Runs on the owning connection loop. Initial success installs the immutable
// rule snapshot before invoking ready; later refreshes close on any change.
auto establish_authority(const drogon::WebSocketConnectionPtr& conn,
                         std::function<void()> ready) -> void;
auto start_authority_monitor(const drogon::WebSocketConnectionPtr& conn)
    -> void;
auto stop_authority_monitor(const drogon::WebSocketConnectionPtr& conn) -> void;

} // namespace plinth::ws
