// SPDX-License-Identifier: MIT
//
// Shared JS↔JSON conversion + JS-exception classification helpers used
// by the 0.3.3 async bridge (run_on_context.cpp) and the db.* JS
// binding (db_bindings.cpp). The 0.3.0 / 0.3.1 sync paths
// (eval.cpp / runtime_pool.cpp) carry their own anonymous-namespace
// copies of these helpers — they predate the shared TU and are left
// alone to keep the 0.3.3 PR's blast radius bounded. A future cleanup
// PR may unify all three.

#pragma once

#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"

#include <expected>
#include <json/value.h>
#include <quickjs.h>

namespace plinth::js::detail {

// Convert a JSValue to Json::Value. Recurses through arrays and plain
// objects; scalar coercions follow the existing 0.3.0 pattern. Returns
// EvalError{INTERNAL} on conversion failure (max-depth, exception
// reading a property, unsupported type). Does NOT consume the input
// JSValue's refcount — caller still owns `v`.
[[nodiscard]] auto js_to_json(JSContext* ctx, JSValueConst v)
    -> std::expected<Json::Value, EvalError>;

// Lift the pending exception off `ctx`, classify it according to ICD
// rules (cancelled / cpu / wall beat QuickJS classification per ICD-
// 0.3.1 §Error Classification), and free the exception. Must be called
// only when an operation just returned JS_EXCEPTION (or rc < 0).
[[nodiscard]] auto extract_error(JSContext* ctx, const BridgeContext& bc)
    -> EvalError;

// Classify an in-hand rejection value from a rejected top-level
// promise. Same classification precedence as extract_error (bc state
// beats name/message inspection), but operates on a JSValue the caller
// already owns rather than the pending exception slot. The rejection
// reason does NOT need to be an Error object — non-error values fall
// through to PROMISE_REJECTED_UNHANDLED with their stringified form.
// Does NOT consume `reason`'s refcount — caller still owns the value.
[[nodiscard]] auto classify_rejection(JSContext* ctx, JSValueConst reason,
                                      const BridgeContext& bc) -> EvalError;

// Sample the JS runtime's malloc_size vs. malloc_limit; if the current
// heap size is within `OOM_MEMORY_SLACK_BYTES` of the limit, latch
// `bc.memory_limit_hit`. Cheap: a single `JS_ComputeMemoryUsage` call
// followed by two field reads. Called from the interrupt handler and
// from `drive_jobs` to capture OOM peaks that would otherwise be
// reclaimed during async-frame unwind before `classify_rejection`
// could read them off the runtime. The latch is a release store; the
// post-hoc check in `extract_error` / `classify_rejection` does an
// acquire load.
auto sample_memory_peak(BridgeContext& bc) noexcept -> void;

} // namespace plinth::js::detail
