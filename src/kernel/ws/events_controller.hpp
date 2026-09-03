#pragma once

// /ws/events — the kernel realtime endpoint per ICD-0.1.6.
//
// Construction is explicit (no auto-creation) so we can pass timing config
// from the loaded plinth::Config. Register via register_ws_routes() in
// kernel/ws/registration.hpp; main.cpp does not touch this directly.

#include <drogon/WebSocketController.h>
#include <string>

namespace plinth::ws {

class EventsController
    : public drogon::WebSocketController<EventsController,
                                         /*AutoCreation=*/false> {
 public:
  EventsController(double auth_timeout_s, double heartbeat_interval_s,
                   double heartbeat_timeout_s, std::string node_id);

  auto handleNewConnection(const drogon::HttpRequestPtr& req,
                           const drogon::WebSocketConnectionPtr& conn)
      -> void override;

  auto handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                        std::string&& message,
                        const drogon::WebSocketMessageType& type)
      -> void override;

  auto handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn)
      -> void override;

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/ws/events");
  WS_PATH_LIST_END

 private:
  double auth_timeout_s;
  double heartbeat_interval_s;
  double heartbeat_timeout_s;
  std::string node_id;
};

} // namespace plinth::ws
