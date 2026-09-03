#pragma once

// LH-0.1 — async-bridge stress dispatch fork.
// See docs/icd/ICD-LH-0.1-async-bridge-stress.md.
//
// The WS `call` frame dispatch in `on_call` routes the single signature
// `lh0:1:js_stress` through this helper instead of the sync resolver.
// The helper acquires a BridgeContext from a process-lifetime
// RuntimePool, co_awaits `run_on_context(bc, script)`, and sends the
// result back on the caller's WS connection. Everything outside that
// one signature falls through to the standard `call_capability` path.
//
// This is an explicit diagnostic-scope deviation from the "every call
// goes through the resolver" shape — the goal is to saturate the
// `signal_completion → JS_ExecutePendingJob` path under load to
// reproduce the `free_zero_refcount` family. See ICD §4 for the
// rationale; ICD §5 for the pool lifetime rules.

#include "kernel/config.hpp"

#include <chrono>
#include <cstddef>
#include <drogon/WebSocketConnection.h>
#include <json/value.h>
#include <string>

namespace plinth::ws {

// Initialize the process-lifetime RuntimePool used by js_stress
// dispatch. Called from main.cpp after init_resolver + createDbClient,
// and from the WS test fixture bootstrap. Idempotent.
auto init_js_stress_pool(const plinth::Config& cfg) -> void;

// Stop admission, wait up to `timeout` for every accepted dispatch to settle,
// and tear down the RuntimePool while Drogon's event loop is still alive.
// Returns false on timeout. Idempotent; safe if init was never called.
[[nodiscard]] auto shutdown_js_stress_pool(
    std::chrono::milliseconds timeout = std::chrono::seconds{35}) -> bool;

// Test-visible ownership diagnostic. Returns the number of accepted dispatches
// which shutdown must drain.
[[nodiscard]] auto js_stress_inflight_count_for_test() -> std::size_t;

// Recognise `lh0:1:js_stress` frames and dispatch them asynchronously.
// Returns true when the signature matched (caller must NOT fall
// through to the sync resolver) and false for every other signature.
// On match, sends `call_result` or `call_error` back on `conn` when
// the coroutine settles; if the conn dropped, the frame is discarded.
auto try_dispatch_js_stress(const drogon::WebSocketConnectionPtr& conn,
                            const std::string& call_id,
                            const std::string& signature,
                            const Json::Value& args, bool caller_is_admin)
    -> bool;

} // namespace plinth::ws
