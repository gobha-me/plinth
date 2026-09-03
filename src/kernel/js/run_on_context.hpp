// SPDX-License-Identifier: MIT
//
// run_on_context — the coroutine entry point for the async bridge.
// See ICD-0.3.3-async-bridge.md §Coroutine Dispatch Loop.
//
// Translates JS `await` into Drogon `co_await` by driving the JS job
// queue, dispatching enqueued AsyncOps, and pumping promise resolutions
// until JS execution completes (or the wall-clock / cancel cascade
// fires).
//
// The caller (a future ext-dispatch entry point at 0.3.4 / 0.4.x) owns
// the BridgeContext lifecycle — pool acquire/release/destroy is NOT
// done here. See §The Pool–Bridge Boundary in the ICD.

#pragma once

#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"

#include <chrono>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <json/value.h>
#include <string_view>

namespace plinth::js {

struct EvalResult {
  std::expected<Json::Value, EvalError> value;
  std::chrono::milliseconds duration{};
};

// Evaluate `source` on `bc.ctx` against its own globalThis, driving
// the JS job queue + AsyncOp dispatch loop until execution completes.
// `bc` is taken by reference; lifecycle is owned by the caller.
//
// On success: result.value carries the converted JS completion value
// (or the resolved value of a top-level promise).
// On failure: result.value carries an EvalError with the appropriate
// EvalErrorKind. Cancellation / wall-clock / unhandled-rejection paths
// all surface here.
//
// BridgeContext outlives the coroutine; copying is not an option.
auto run_on_context(BridgeContext& bc, std::string_view source)
    -> drogon::Task<EvalResult>;

} // namespace plinth::js
