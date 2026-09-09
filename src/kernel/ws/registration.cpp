#include "kernel/ws/registration.hpp"

#include "kernel/ws/events_controller.hpp"

#include <drogon/drogon.h>
#include <memory>
#include <spdlog/spdlog.h>

namespace plinth::ws {

auto register_ws_routes(const Config& cfg) -> void {
  // Authority renewal cannot consume an unbounded kernel-pool operation.
  // Drogon owns and closes this pool with its other database clients. Bound
  // both queue wait/callback completion and execution on the PostgreSQL side.
  drogon::app().addDbClient(drogon::orm::PostgresConfig{
      .host = cfg.db.host,
      .port = cfg.db.port,
      .databaseName = cfg.db.database,
      .username = cfg.db.user,
      .password = cfg.db.password,
      .connectionNumber = 2,
      .name = "ws_authority",
      .isFast = false,
      .characterSet = "",
      .timeout = 1.0,
      .autoBatch = false,
      .connectOptions = {{"options", "-c statement_timeout=1000"}},
  });
  auto ctrl = std::make_shared<EventsController>(
      cfg.ws_auth_timeout_s, cfg.ws_heartbeat_interval_s,
      cfg.ws_heartbeat_timeout_s, cfg.node_id, cfg.ws_browser_origin);
  drogon::app().registerController(ctrl);
  spdlog::info("WebSocket routes registered (/ws/events)");
}

} // namespace plinth::ws
