#pragma once

#include <memory>
#include <string>

namespace plinth::frontend {
class FloatLayoutService;
// Register before the active-frontend catch-all. Only the exact captured
// bundled platform graph is public; typed providers and JSON require sessions.
auto register_float_routes(std::shared_ptr<FloatLayoutService> service,
                           std::string configured_origin = {}) -> void;
} // namespace plinth::frontend
