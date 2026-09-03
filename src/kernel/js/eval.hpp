// SPDX-License-Identifier: MIT
//
// QuickJS host-side eval surface. See docs/icd/ICD-0.3.0-quickjs-vendoring.md.
//
// Scope: one-shot eval that creates a fresh JSRuntime + JSContext with a
// hard-coded 16 MiB memory ceiling, evaluates a source string, converts the
// result to Json::Value, and tears the runtime down. No runtime reuse, no
// module loader, no kernel API injection. Callable only from Plinth C++ and
// Catch2 test binaries — this function is deliberately NOT reachable from
// HTTP, WebSocket, PAT, or any capability dispatch path in 0.3.0.
//
// Uses std::expected per ICD-0.3.0's preferred signature. The project was
// bumped to C++23 alongside this milestone (see CMakeLists.txt:32).

#pragma once

#include <json/value.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace plinth::js {

enum class EvalErrorKind : std::uint8_t {
  // 0.3.0 variants
  SYNTAX_ERROR,  // QuickJS parse failure
  RUNTIME_ERROR, // Uncaught JS exception during execution
  MEMORY_LIMIT,  // Allocation refused by JS_SetMemoryLimit
  INTERNAL,      // Host-side failure (runtime creation, conversion, ...)
  // 0.3.1 variants — interrupt-handler and stack surfacing.
  CPU_TIME_EXCEEDED,   // CPU-time budget burned
  WALL_CLOCK_EXCEEDED, // Wall-clock budget burned
  STACK_OVERFLOW,      // JS_SetMaxStackSize threshold hit
  CANCELLED,           // BridgeContext::cancelled toggled by host / test
  // 0.3.3 variants — async coroutine bridge.
  ASYNC_CONCURRENCY_LIMIT,      // bc.max_concurrent_async_ops == 0 path
  PROMISE_REJECTED_UNHANDLED,   // top-level promise rejected without handler
  PROMISE_RESOLVE_AFTER_CANCEL, // defensive: resolve/reject after cascade
  INTERNAL_ASYNC,               // unimplemented AsyncOp::Type or invariant
  // 0.3.5 variant — result-size enforcement close-out.
  ASYNC_RESULT_SIZE_EXCEEDED, // async op resolved with >
                              // async_result_size_limit_bytes
  // 0.4.1 variant — GlassWorm Layer 2 pre-eval gate.
  UNICODE_SMUGGLE_DETECTED // pre_eval_scan rejected the source bytes
};

struct EvalError {
  EvalErrorKind kind;
  std::string message; // QuickJS diagnostic + stack when available
  int line = 0;        // 1-based; 0 if unavailable
  int column = 0;      // 1-based; 0 if unavailable
};

// One-shot eval. Creates a fresh JSRuntime + JSContext, invokes
// JS_SetMemoryLimit(16 MiB), evaluates `src` as a script (NOT a module),
// converts the completion value to Json::Value, and destroys the runtime.
//
// The 16 MiB ceiling is intentionally rigid in 0.3.0 so the MEMORY_LIMIT
// code path is exercised from day one; configurable per-extension budgets
// land in ICD-0.3.1 alongside the runtime pool.
[[nodiscard]] auto eval(std::string_view src)
    -> std::expected<Json::Value, EvalError>;

} // namespace plinth::js
