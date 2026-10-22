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
#include "kernel/lifecycle/async_task_registry.hpp"

#include <chrono>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <functional>
#include <json/value.h>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace plinth::js {

struct EvalResult {
  std::expected<Json::Value, EvalError> value;
  std::chrono::milliseconds duration{};
};

enum class ExecutionObservationStage { ENTER, FINISH };

struct ExecutionObservation {
  ExecutionObservationStage stage;
  std::weak_ptr<ExecutionLifetime> execution;
  std::string extension_name;
  int call_depth = 0;
  bool cancelled = false;
  // Only FINISH classifies a result: nullopt then means actual success.
  std::optional<EvalErrorKind> error_kind;
};

// Trusted C++ observation only. ENTER runs after the actual owner-loop hop;
// FINISH projects every owned driver's actual result before retirement. Failed
// admission/ownership publication has no owned generation and emits neither.
// No BC/JS handles are exposed. Observers must not throw, block, mutate the
// execution or re-enter the driver. They run outside the observer mutex on the
// owner loop and must independently own captured state. Weak generation
// ownership, not a context address, identifies an execution. Empty removes the
// observer, but already copied callbacks may still run; retain their captures
// through owner-loop quiescence and release temporary strong leases before
// asserting native drain. There is no JS registration or cancellation policy.
using ExecutionObserver = std::function<void(const ExecutionObservation&)>;
auto set_execution_observer_for_test(ExecutionObserver observer) -> void;

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
// Internal ancestor ownership, never exposed through the JS/SDK data plane.
auto run_on_context(
    BridgeContext& bc, std::string_view source,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr,
    std::stop_token inherited = {}) -> drogon::Task<EvalResult>;

} // namespace plinth::js
