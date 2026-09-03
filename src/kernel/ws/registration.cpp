#include "kernel/ws/registration.hpp"

#include "kernel/ws/events_controller.hpp"

#include <drogon/drogon.h>
#include <memory>
#include <spdlog/spdlog.h>

namespace plinth::ws {

auto register_ws_routes(const Config& cfg) -> void {
  auto ctrl = std::make_shared<EventsController>(
      cfg.ws_auth_timeout_s, cfg.ws_heartbeat_interval_s,
      cfg.ws_heartbeat_timeout_s, cfg.node_id);
  drogon::app().registerController(ctrl);
  spdlog::info("WebSocket routes registered (/ws/events)");
}

} // namespace plinth::ws
