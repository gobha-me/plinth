#pragma once

#include "kernel/config.hpp"

namespace plinth::ws {

// Construct and register the EventsController with Drogon. Call from
// main() after config is loaded but before app().run().
auto register_ws_routes(const Config& cfg) -> void;

} // namespace plinth::ws
