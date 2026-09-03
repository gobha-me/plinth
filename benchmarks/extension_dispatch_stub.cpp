// Benchmark-only stub for plinth::extensions::dispatch.
//
// Tier 1 / Tier 2 / unicode-scanner benchmarks link resolution.cpp for
// its `call_capability` entry (the sync path the < 1µs target measures).
// resolution.cpp's sibling `call_capability_async` path references
// `plinth::extensions::dispatch`, whose real definition lives in
// runtime_registry.cpp alongside the QuickJS coroutine subsystem (pool,
// BridgeContext, handler-file IO). The benchmarks never exercise that
// path — Tier 1 dispatches directly via a registered function pointer,
// Tier 2 is seeded with sidecar providers that resolve to
// tier3_not_available before reaching extension dispatch. Linking a stub
// keeps the benchmark source set small without pulling the runtime pool
// in.

#include "kernel/extensions/runtime_registry.hpp"

namespace plinth::extensions {

// Signature mirrors runtime_registry.hpp exactly so the linker accepts
// this translation unit as the definition. The stub body does not
// retain `args` or `caller`, so the coroutine-lifetime concern behind
// the lint does not apply here.
auto dispatch([[maybe_unused]] std::string_view extension_name,
              [[maybe_unused]] std::string_view function,
              [[maybe_unused]] const Json::Value& args,
              [[maybe_unused]] const plinth::capabilities::UserContext& caller,
              [[maybe_unused]] int caller_call_depth)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>> {
  co_return std::unexpected(plinth::js::PromiseRejection{
      .code = "cap.internal",
      .message = "benchmark stub: extension dispatch not linked",
      .sqlstate = std::nullopt,
  });
}

} // namespace plinth::extensions
