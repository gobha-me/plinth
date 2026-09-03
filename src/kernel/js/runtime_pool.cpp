// SPDX-License-Identifier: MIT
//
// Implementation of plinth::js::RuntimePool + eval_on_context.
// See ICD-0.3.1-runtime-lifecycle.md.

#include "kernel/js/runtime_pool.hpp"

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/eval_guard.hpp"
#include "kernel/js/stdlib_inject.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"

#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace plinth::js {

// ─── Defaults ────────────────────────────────────────────────────────

auto default_runtime_limits() noexcept -> RuntimeLimits {
  return RuntimeLimits{
      .memory_limit_bytes = 16UL * 1024UL * 1024UL, // 16 MiB
      .cpu_time_limit = std::chrono::milliseconds{100},
      .wall_clock_limit = std::chrono::milliseconds{30'000}, // 30 s
      .max_stack_depth = 256,
      .max_call_depth = 8,
      .max_concurrent_async_ops = 8,
      .async_result_size_limit_bytes = 16UL * 1024UL * 1024UL}; // 16 MiB
}

// ─── Config projection ──────────────────────────────────────────────
//
// ICD-0.3.2 §Security Constraints 1–2. Eight scalar fields, copied
// from the loaded Config. Every field here is whitelisted. Never add
// Config::Database fields or `migrations_dir` — adding a key is a
// code-review-gated PR.

auto make_config_projection(const Config& cfg) noexcept -> ConfigProjection {
  return ConfigProjection{
      .dev_mode = cfg.dev_mode,
      .node_id = cfg.node_id,
      .listen_host = cfg.listen_host,
      .listen_port = cfg.listen_port,
      .registration_enabled = cfg.registration_enabled,
      .ws_auth_timeout_s = cfg.ws_auth_timeout_s,
      .ws_heartbeat_interval_s = cfg.ws_heartbeat_interval_s,
      .ws_heartbeat_timeout_s = cfg.ws_heartbeat_timeout_s,
      .security_unicode_scanner_enabled = cfg.security_unicode_scanner_enabled,
      .security_unicode_scanner_threshold =
          cfg.security_unicode_scanner_threshold,
      .security_unicode_scanner_log_findings =
          cfg.security_unicode_scanner_log_findings,
  };
}

// ─── JSON conversion — shared with eval.cpp via a private translation ──
//
// We intentionally duplicate a minimal JS→JSON converter rather than
// exposing eval.cpp's internal helpers. 0.3.0's converter is only used
// by one-shot eval(); exposing it now would couple two code paths that
// are scheduled to diverge when 0.3.3 adds async result shapes. The
// duplication is ≈80 lines; it disappears when the two paths are
// unified behind a common bridge in 0.3.3.

namespace {

constexpr int MAX_CONVERSION_DEPTH = 64;

auto json_null() -> Json::Value {
  return Json::Value{Json::nullValue};
}

auto read_string_prop(JSContext* ctx, JSValueConst obj, const char* name)
    -> std::string {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  std::string out;
  if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsException(v)) {
    const char* s = JS_ToCString(ctx, v);
    if (s != nullptr) {
      out.assign(s);
      JS_FreeCString(ctx, s);
    }
  }
  JS_FreeValue(ctx, v);
  return out;
}

struct ConvOutcome {
  Json::Value value = json_null();
  bool ok = true;
  EvalError error{};
};

auto conv_fail(std::string message) -> ConvOutcome {
  return ConvOutcome{.value = json_null(),
                     .ok = false,
                     .error = EvalError{.kind = EvalErrorKind::INTERNAL,
                                        .message = std::move(message),
                                        .line = 0,
                                        .column = 0}};
}

auto js_to_json(JSContext* ctx, JSValueConst v, int depth) -> ConvOutcome;

auto js_array_to_json(JSContext* ctx, JSValueConst arr, int depth)
    -> ConvOutcome {
  int64_t len = 0;
  if (JS_GetLength(ctx, arr, &len) < 0) {
    return conv_fail("js_to_json: failed to read array length");
  }
  ConvOutcome out{};
  out.value = Json::Value{Json::arrayValue};
  for (int64_t i = 0; i < len; ++i) {
    JSValue elem = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
    if (JS_IsException(elem)) {
      JS_FreeValue(ctx, elem);
      return conv_fail("js_to_json: array element access threw");
    }
    ConvOutcome child = js_to_json(ctx, elem, depth + 1);
    JS_FreeValue(ctx, elem);
    if (!child.ok) {
      return child;
    }
    out.value.append(std::move(child.value));
  }
  return out;
}

auto js_plain_object_to_json(JSContext* ctx, JSValueConst obj, int depth)
    -> ConvOutcome {
  JSPropertyEnum* tab = nullptr;
  uint32_t tab_len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, obj,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    return conv_fail("js_to_json: JS_GetOwnPropertyNames failed");
  }
  ConvOutcome out{};
  out.value = Json::Value{Json::objectValue};
  for (uint32_t i = 0; i < tab_len; ++i) {
    const char* key = JS_AtomToCString(ctx, tab[i].atom);
    JSValue val = JS_GetProperty(ctx, obj, tab[i].atom);
    if (key == nullptr || JS_IsException(val)) {
      if (key != nullptr) {
        JS_FreeCString(ctx, key);
      }
      JS_FreeValue(ctx, val);
      JS_FreePropertyEnum(ctx, tab, tab_len);
      return conv_fail("js_to_json: property read failed");
    }
    ConvOutcome child = js_to_json(ctx, val, depth + 1);
    std::string key_copy{key};
    JS_FreeCString(ctx, key);
    JS_FreeValue(ctx, val);
    if (!child.ok) {
      JS_FreePropertyEnum(ctx, tab, tab_len);
      return child;
    }
    out.value[key_copy] = std::move(child.value);
  }
  JS_FreePropertyEnum(ctx, tab, tab_len);
  return out;
}

auto js_to_json(JSContext* ctx, JSValueConst v, int depth) -> ConvOutcome {
  if (depth > MAX_CONVERSION_DEPTH) {
    return conv_fail("js_to_json: max conversion depth exceeded");
  }
  if (JS_IsNull(v) || JS_IsUndefined(v)) {
    return ConvOutcome{};
  }
  if (JS_IsBool(v)) {
    int b = JS_ToBool(ctx, v);
    if (b < 0) {
      return conv_fail("js_to_json: bool coercion failed");
    }
    return ConvOutcome{.value = Json::Value{static_cast<bool>(b)}};
  }
  if (JS_IsNumber(v)) {
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
      int32_t i32 = 0;
      if (JS_ToInt32(ctx, &i32, v) == 0) {
        return ConvOutcome{.value = Json::Value{static_cast<Json::Int>(i32)}};
      }
    }
    double d = 0.0;
    if (JS_ToFloat64(ctx, &d, v) == 0) {
      return ConvOutcome{.value = Json::Value{d}};
    }
    return conv_fail("js_to_json: number coercion failed");
  }
  if (JS_IsString(v)) {
    const char* s = JS_ToCString(ctx, v);
    if (s == nullptr) {
      return conv_fail("js_to_json: string coercion failed");
    }
    ConvOutcome out{.value = Json::Value{std::string{s}}};
    JS_FreeCString(ctx, s);
    return out;
  }
  if (JS_IsArray(v)) {
    return js_array_to_json(ctx, v, depth);
  }
  if (JS_IsObject(v)) {
    return js_plain_object_to_json(ctx, v, depth);
  }
  return conv_fail("js_to_json: unsupported JS value type");
}

// Classify an exception lifted off `ctx` after JS_Eval returned
// JS_EXCEPTION. Consults the BridgeContext interrupt-state fields so
// terminations driven by the interrupt handler surface as the right
// 0.3.1 code rather than a generic RUNTIME_ERROR.
auto extract_error(JSContext* ctx, const BridgeContext& bc) -> EvalError {
  EvalError err{.kind = EvalErrorKind::RUNTIME_ERROR,
                .message = {},
                .line = 0,
                .column = 0};

  JSValue exc = JS_GetException(ctx);
  bool is_error_obj = JS_IsError(exc);

  std::string name;
  std::string msg;
  if (is_error_obj) {
    name = read_string_prop(ctx, exc, "name");
    msg = read_string_prop(ctx, exc, "message");
    JSValue ln = JS_GetPropertyStr(ctx, exc, "lineNumber");
    int32_t tmp = 0;
    if (!JS_IsUndefined(ln) && JS_ToInt32(ctx, &tmp, ln) == 0) {
      err.line = tmp;
    }
    JS_FreeValue(ctx, ln);
  } else {
    const char* s = JS_ToCString(ctx, exc);
    if (s != nullptr) {
      msg.assign(s);
      JS_FreeCString(ctx, s);
    }
  }
  JS_FreeValue(ctx, exc);

  // Interrupt-induced termination beats QuickJS's own classification:
  // cancelled → CANCELLED; CPU burned → CPU_TIME_EXCEEDED; wall-clock
  // burned → WALL_CLOCK_EXCEEDED. Checked in the order the interrupt
  // handler checks them so a test that trips multiple predicates
  // simultaneously gets a stable code.
  if (bc.cancelled.load(std::memory_order_acquire)) {
    err.kind = EvalErrorKind::CANCELLED;
    err.message = "execution cancelled";
    return err;
  }
  if (bc.cpu_limit_exceeded()) {
    err.kind = EvalErrorKind::CPU_TIME_EXCEEDED;
    err.message = "CPU time limit exceeded";
    return err;
  }
  if (bc.wall_clock_exceeded()) {
    err.kind = EvalErrorKind::WALL_CLOCK_EXCEEDED;
    err.message = "wall-clock time limit exceeded";
    return err;
  }

  // QuickJS exception-class classification.
  //   "out of memory"  → MEMORY_LIMIT (JS_SetMemoryLimit path).
  //   "stack overflow" → STACK_OVERFLOW (JS_SetMaxStackSize path).
  //   SyntaxError      → SYNTAX_ERROR.
  //   everything else  → RUNTIME_ERROR.
  if (name == "InternalError" &&
      msg.find("out of memory") != std::string::npos) {
    err.kind = EvalErrorKind::MEMORY_LIMIT;
    err.message = std::string{"out of memory: "} + msg;
    return err;
  }
  // QuickJS-ng raises stack overflow as
  //   RangeError("Maximum call stack size exceeded")
  // via JS_ThrowStackOverflow (see quickjs.c:8088). Accept either
  // that or a future InternalError("stack overflow") form so the
  // mapping survives an upstream vocabulary change.
  bool is_stack_overflow =
      (name == "RangeError" &&
       msg.find("call stack size exceeded") != std::string::npos) ||
      (name == "InternalError" &&
       msg.find("stack overflow") != std::string::npos);
  if (is_stack_overflow) {
    err.kind = EvalErrorKind::STACK_OVERFLOW;
    err.message = std::string{"stack overflow: "} + msg;
    return err;
  }

  std::string combined;
  if (name.empty() && msg.empty()) {
    combined = "<unknown JS exception>";
  } else if (name.empty()) {
    combined = msg;
  } else {
    combined = name + ": " + msg;
  }
  err.message = std::move(combined);
  err.kind = (name == "SyntaxError") ? EvalErrorKind::SYNTAX_ERROR
                                     : EvalErrorKind::RUNTIME_ERROR;
  return err;
}

// ─── Interrupt handler ───────────────────────────────────────────────
//
// Installed via JS_SetInterruptHandler; the handler's `opaque` pointer
// is the BridgeContext for which the runtime was created. Returning 1
// tells QuickJS to raise an InternalError and abandon execution; 0
// lets execution continue.

extern "C" auto plinth_js_interrupt_cb(JSRuntime* /*rt*/, void* opaque) -> int {
  auto* bc = static_cast<BridgeContext*>(opaque);
  if (bc == nullptr) {
    return 0;
  }
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return 1;
  }
  if (bc->cpu_limit_exceeded()) {
    return 1;
  }
  if (bc->wall_clock_exceeded()) {
    return 1;
  }
  // Capture OOM peaks during execution so a rejection that unwinds
  // the async frame before we classify still maps to MEMORY_LIMIT.
  // See BridgeContext::memory_limit_hit and detail::sample_memory_peak.
  plinth::js::detail::sample_memory_peak(*bc);
  return 0;
}

// ─── Pool sizing ─────────────────────────────────────────────────────

auto resolve_pool_size(int requested) -> int {
  if (requested > 0) {
    return requested;
  }
  unsigned hw = std::thread::hardware_concurrency();
  int def = std::min(4, static_cast<int>(std::max(1U, hw / 2U)));
  return std::max(1, def);
}

// ─── Globals reset between release()s ───────────────────────────────
//
// Deterministic closeout — drain every pending microtask before freeing
// the context/runtime. run_on_context's loop normally drains jobs as
// part of its async-dispatch fixed point, but a handful of paths let us
// call JS_FreeContext with jobs still queued:
//   * run_on_context takes the drive_jobs error-path co_return (line
//     ~846 in run_on_context.cpp) without re-draining.
//   * finalize's JS_PromiseResult / JS_FreeValue on a still-pending
//     promise can leave reactions on the job queue.
//   * RuntimePool::destroy is called directly by tests that bypass the
//     normal loop.
// A queued reaction job holds dup'd JSValues in its argv (see
// JS_EnqueueJob in quickjs.c:2115); those refs keep gc_obj_list
// non-empty and trip the `list_empty(&rt->gc_obj_list)` assert in
// JS_FreeRuntime. Draining here consumes those refs cleanly.
//
// Bounded at JOB_DRAIN_MAX iterations so a misbehaved microtask chain
// (e.g. a `.then` that enqueues another `.then`) can't wedge teardown.
// The practical ceiling for well-behaved code is a handful of jobs; we
// allow several thousand to cover deep promise chains and leave a
// warning if we blow the cap.
auto drain_pending_jobs(JSContext* ctx, JSRuntime* rt) -> void {
  constexpr int JOB_DRAIN_MAX = 4096;
  if (ctx == nullptr || rt == nullptr) {
    return;
  }
  JS_UpdateStackTop(rt);
  JSContext* pctx = ctx;
  for (int i = 0; i < JOB_DRAIN_MAX; ++i) {
    if (!JS_IsJobPending(rt)) {
      return;
    }
    int rc = JS_ExecutePendingJob(rt, &pctx);
    if (rc < 0) {
      // A job faulted. Consume any exception so we don't leave an
      // error object pinned on the context, then keep draining —
      // the next job may settle other pending state cleanly.
      JSValue exc = JS_GetException(pctx);
      JS_FreeValue(pctx, exc);
    }
  }
  if (JS_IsJobPending(rt)) {
    plinth::log::warn(
        "RuntimePool teardown: JS job queue did not drain after {} "
        "iterations; proceeding to free — leak possible",
        JOB_DRAIN_MAX);
  }
}

// ICD §State Reset — clear globalThis's own enumerable string-keyed
// properties; leave the prototype chain alone so built-ins (Object,
// Array, Math, ...) remain usable. Called after a successful execution
// before the context is returned to the free list.
auto clear_global_own_props(JSContext* ctx) -> bool {
  JSValue global = JS_GetGlobalObject(ctx);
  JSPropertyEnum* tab = nullptr;
  uint32_t tab_len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, global,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    JS_FreeValue(ctx, global);
    return false;
  }
  for (uint32_t i = 0; i < tab_len; ++i) {
    JS_DeleteProperty(ctx, global, tab[i].atom, 0);
  }
  JS_FreePropertyEnum(ctx, tab, tab_len);
  JS_FreeValue(ctx, global);
  return true;
}

} // namespace

// ─── Entry record ────────────────────────────────────────────────────

struct RuntimePool::Entry {
  BridgeContext bc{};
  bool transient = false;
};

// ─── Lifecycle ───────────────────────────────────────────────────────

RuntimePool::RuntimePool(const Extension* ext, RuntimeLimits runtime_limits,
                         const Config& cfg, int pool_size,
                         const plinth::capabilities::UserContext* user,
                         std::string extension_name)
    : extension(ext), limits(runtime_limits),
      projection(make_config_projection(cfg)),
      user_ctx(user != nullptr
                   ? *user
                   : plinth::capabilities::UserContext::anonymous()),
      ext_name(std::move(extension_name)),
      capacity(resolve_pool_size(pool_size)) {
  // ICD §Security Constraints #2: memory / stack / cpu / wall-clock
  // must all be set before any JS code runs. There is no fallback to
  // an unlimited default — a missing limit is a programming error,
  // not a user input, so we fail loud.
  if (limits.memory_limit_bytes == 0 || limits.max_stack_depth <= 0 ||
      limits.cpu_time_limit.count() <= 0 ||
      limits.wall_clock_limit.count() <= 0) {
    plinth::log::critical(
        "RuntimePool: unset limit (memory={}, stack_depth={}, "
        "cpu_ms={}, wall_ms={}); aborting",
        limits.memory_limit_bytes, limits.max_stack_depth,
        limits.cpu_time_limit.count(), limits.wall_clock_limit.count());
    std::abort();
  }

  free_list.reserve(static_cast<std::size_t>(capacity));
  for (int i = 0; i < capacity; ++i) {
    free_list.push_back(create_entry(/*transient=*/false));
  }
}

RuntimePool::~RuntimePool() {
  // Destroy whatever's still in our custody. Callers that leak
  // checked-out contexts get a warning and cleanup — not a leak.
  std::lock_guard<std::mutex> lock(mu);
  if (!checked_out.empty()) {
    plinth::log::warn(
        "RuntimePool destroyed with {} context(s) still checked out; "
        "destroying them",
        checked_out.size());
  }
  for (auto& e : free_list) {
    drain_pending_jobs(e->bc.ctx, e->bc.rt);
    if (e->bc.ctx != nullptr) {
      JS_FreeContext(e->bc.ctx);
    }
    if (e->bc.rt != nullptr) {
      JS_FreeRuntime(e->bc.rt);
    }
  }
  for (auto& e : checked_out) {
    for (auto& [id, cbs] : e->bc.callbacks) {
      JS_FreeValue(e->bc.ctx, cbs.resolve);
      JS_FreeValue(e->bc.ctx, cbs.reject);
    }
    e->bc.callbacks.clear();
    drain_pending_jobs(e->bc.ctx, e->bc.rt);
    if (e->bc.ctx != nullptr) {
      JS_FreeContext(e->bc.ctx);
    }
    if (e->bc.rt != nullptr) {
      JS_FreeRuntime(e->bc.rt);
    }
  }
  free_list.clear();
  checked_out.clear();
}

// ─── Entry factory ───────────────────────────────────────────────────

auto RuntimePool::create_entry(bool transient) -> EntryPtr {
  auto entry = std::make_unique<Entry>();
  JSRuntime* rt = JS_NewRuntime();
  if (rt == nullptr) {
    plinth::log::error("RuntimePool: JS_NewRuntime failed");
    return entry; // rt/ctx stay nullptr; acquire() surfaces the failure
  }
  JS_SetMemoryLimit(rt, limits.memory_limit_bytes);
  JS_SetMaxStackSize(rt, static_cast<std::size_t>(limits.max_stack_depth) *
                             STACK_BYTES_PER_FRAME);
  JS_SetInterruptHandler(rt, &plinth_js_interrupt_cb, &entry->bc);

  JSContext* ctx = JS_NewContext(rt);
  if (ctx == nullptr) {
    plinth::log::error("RuntimePool: JS_NewContext failed");
    JS_FreeRuntime(rt);
    return entry;
  }

  entry->bc.rt = rt;
  entry->bc.ctx = ctx;
  entry->bc.extension = extension;
  // ICD-0.5.0.3 §Security Constraint 3 — pool populates the callee's
  // identity so `pubsub.publish`'s extension-identity gate matches on
  // handler invocation. Host-eval pools pass an empty string; kernel-
  // scope behavior is preserved.
  entry->bc.extension_name = ext_name;
  entry->bc.cpu_time_limit = limits.cpu_time_limit;
  entry->bc.wall_clock_limit = limits.wall_clock_limit;
  entry->bc.max_call_depth = limits.max_call_depth;
  entry->bc.max_concurrent_async_ops = limits.max_concurrent_async_ops;
  entry->bc.async_result_size_limit_bytes =
      limits.async_result_size_limit_bytes;
  entry->bc.config_proj = projection;
  entry->bc.user = user_ctx;
  entry->transient = transient;

  // Install the BridgeContext pointer as the context opaque so every
  // 0.3.2 stdlib binding can retrieve it via JS_GetContextOpaque, then
  // inject the kernel stdlib (log.*, config.get, crypto.*) before any
  // JS code can run on this context. Order matters: config_bindings
  // reads `bc.config_proj` on first call, which requires both the
  // opaque slot populated AND the projection copied above.
  JS_SetContextOpaque(ctx, &entry->bc);
  inject_kernel_stdlib(ctx);

  return entry;
}

// ─── acquire / release / destroy / rebuild ──────────────────────────

auto RuntimePool::acquire() -> BridgeContext* {
  std::lock_guard<std::mutex> lock(mu);
  EntryPtr entry;
  if (!free_list.empty()) {
    entry = std::move(free_list.back());
    free_list.pop_back();
  } else {
    bool transient = std::cmp_greater_equal(checked_out.size(), capacity);
    entry = create_entry(transient);
  }
  if (entry->bc.rt == nullptr || entry->bc.ctx == nullptr) {
    // Partial construction — don't hand it to the caller; let the
    // unique_ptr destroy what little there is.
    return nullptr;
  }
  BridgeContext* raw = &entry->bc;
  checked_out.push_back(std::move(entry));
  return raw;
}

auto RuntimePool::release(BridgeContext* bc) -> void {
  if (bc == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu);
  auto it = std::ranges::find_if(
      checked_out, [bc](const EntryPtr& e) { return &e->bc == bc; });
  if (it == checked_out.end()) {
    plinth::log::warn("RuntimePool::release called on context that is not "
                      "checked out");
    return;
  }

  EntryPtr entry = std::move(*it);
  checked_out.erase(it);

  // Defensive destroy (ICD §Security Constraint 4): if the caller
  // release()s a context whose last execution tripped the interrupt
  // handler or was cancelled, route to destroy and warn. Predicates
  // are evaluated BEFORE the reset below so the post-execution state
  // is still visible.
  bool was_cancelled = entry->bc.cancelled.load(std::memory_order_acquire);
  bool cpu_tripped = entry->bc.cpu_limit_exceeded();
  bool wall_tripped = entry->bc.wall_clock_exceeded();
  // A non-empty callbacks map at release time is a coroutine-loop bug
  // (the loop should have drained or rejected every promise before
  // returning). Treat as defensive-destroy per ICD-0.3.3
  // §State Reset on release() so we don't pool a context with leaked
  // JSValue refs.
  bool async_dirty =
      !entry->bc.callbacks.empty() || !entry->bc.pending_ops.empty();
  bool must_destroy = entry->transient || was_cancelled || cpu_tripped ||
                      wall_tripped || async_dirty;

  if (must_destroy) {
    if (async_dirty && !entry->transient) {
      plinth::log::warn("RuntimePool::release on a context with pending async "
                        "state (callbacks={}, pending_ops={}); routing to "
                        "destroy (ICD-0.3.3 §State Reset)",
                        entry->bc.callbacks.size(),
                        entry->bc.pending_ops.size());
    }
    if (!entry->transient && !async_dirty) {
      plinth::log::warn(
          "RuntimePool::release on a context whose last execution "
          "failed (cancelled={}, cpu={}, wall={}); routing to "
          "destroy (ICD-0.3.1 §Security Constraint 4)",
          was_cancelled, cpu_tripped, wall_tripped);
    }
    // Free any leftover promise-capability refs before destroying
    // the context, so ASAN / leak detectors stay clean. The
    // resolve/reject closures themselves are managed by QuickJS via
    // JS_FreeContext, but the callback-pair refs we hold are not.
    for (auto& [id, cbs] : entry->bc.callbacks) {
      JS_FreeValue(entry->bc.ctx, cbs.resolve);
      JS_FreeValue(entry->bc.ctx, cbs.reject);
    }
    entry->bc.callbacks.clear();
    entry->bc.pending_ops.clear();
    // ICD-0.5.2 §Per-bc lifetime + eviction — bc teardown must
    // notify the broker and release the persistent subscriber
    // JSValues before the runtime goes away.
    plinth::realtime::broker::drop_bc_subscriptions(&entry->bc);
    entry->bc.drop_persistent_callbacks();
    drain_pending_jobs(entry->bc.ctx, entry->bc.rt);
    JS_FreeContext(entry->bc.ctx);
    JS_FreeRuntime(entry->bc.rt);
    return;
  }

  // State reset: clear globalThis own props; reset timer/interrupt
  // fields. The heap and built-ins are intentionally preserved. The
  // 0.3.2 kernel stdlib (log/config/crypto) is cleared alongside
  // user globals and re-injected below so every acquire() sees a
  // fresh surface.
  if (!clear_global_own_props(entry->bc.ctx)) {
    // A reset failure means the context is in an unknown state;
    // fall back to destroy to avoid returning a poisoned context.
    plinth::log::warn("RuntimePool::release: global-reset failed; destroying "
                      "context");
    drain_pending_jobs(entry->bc.ctx, entry->bc.rt);
    JS_FreeContext(entry->bc.ctx);
    JS_FreeRuntime(entry->bc.rt);
    return;
  }
  inject_kernel_stdlib(entry->bc.ctx);
  entry->bc.execution_start = {};
  entry->bc.cpu_timer_start = {};
  entry->bc.cpu_time_accumulated = std::chrono::nanoseconds{0};
  entry->bc.call_depth = 0;
  // Async-state counters reset to defaults; callbacks / pending_ops
  // were already required-empty above (otherwise we destroyed).
  entry->bc.next_callback_id = 0;
  entry->bc.concurrent_async_ops = 0;
  entry->bc.cancelled.store(false, std::memory_order_release);
  entry->bc.memory_limit_hit.store(false, std::memory_order_release);
  // ICD-0.5.2 §Per-bc lifetime + eviction — persistent subscriber
  // callbacks don't survive a release/acquire cycle (their JSValues
  // reference globalThis state that just got reset). Notify the
  // broker alongside the JSValue frees so the registry stays
  // consistent.
  plinth::realtime::broker::drop_bc_subscriptions(&entry->bc);
  entry->bc.drop_persistent_callbacks();
  free_list.push_back(std::move(entry));
}

auto RuntimePool::destroy(BridgeContext* bc) -> void {
  if (bc == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu);
  auto it = std::ranges::find_if(
      checked_out, [bc](const EntryPtr& e) { return &e->bc == bc; });
  if (it == checked_out.end()) {
    plinth::log::warn("RuntimePool::destroy called on context that is not "
                      "checked out");
    return;
  }
  EntryPtr entry = std::move(*it);
  checked_out.erase(it);
  // Free any leftover promise-capability refs before destroying the
  // context, matching the release()-defensive-destroy path. The
  // cancellation cascade should have drained these already; this is
  // belt-and-braces so an early-exit destroy() in tests stays clean
  // under ASAN.
  for (auto& [id, cbs] : entry->bc.callbacks) {
    JS_FreeValue(entry->bc.ctx, cbs.resolve);
    JS_FreeValue(entry->bc.ctx, cbs.reject);
  }
  entry->bc.callbacks.clear();
  entry->bc.pending_ops.clear();
  // ICD-0.5.2 §Per-bc lifetime + eviction — release any persistent
  // `pubsub.subscribe` handler JSValues and notify the broker before
  // the runtime goes away. Matches the release()-defensive-destroy
  // path; without this a bc that called `pubsub.subscribe` and is
  // then `destroy()`d (early-exit tests, runtime-pool shutdown)
  // leaks handler JSValues into `rt->gc_obj_list`, which trips
  // `list_empty` in `JS_FreeRuntime`.
  plinth::realtime::broker::drop_bc_subscriptions(&entry->bc);
  entry->bc.drop_persistent_callbacks();
  drain_pending_jobs(entry->bc.ctx, entry->bc.rt);
  JS_FreeContext(entry->bc.ctx);
  JS_FreeRuntime(entry->bc.rt);
}

auto RuntimePool::rebuild() -> void {
  std::lock_guard<std::mutex> lock(mu);
  if (!checked_out.empty()) {
    plinth::log::warn("RuntimePool::rebuild called while {} context(s) are "
                      "checked out; pooled contexts rebuilt, checked-out ones "
                      "untouched",
                      checked_out.size());
  }
  for (auto& e : free_list) {
    // Pooled entries should never carry pending callbacks (they were
    // drained at release-time); free defensively just in case.
    for (auto& [id, cbs] : e->bc.callbacks) {
      JS_FreeValue(e->bc.ctx, cbs.resolve);
      JS_FreeValue(e->bc.ctx, cbs.reject);
    }
    e->bc.callbacks.clear();
    e->bc.pending_ops.clear();
    drain_pending_jobs(e->bc.ctx, e->bc.rt);
    JS_FreeContext(e->bc.ctx);
    JS_FreeRuntime(e->bc.rt);
  }
  free_list.clear();
  free_list.reserve(static_cast<std::size_t>(capacity));
  for (int i = 0; i < capacity; ++i) {
    free_list.push_back(create_entry(/*transient=*/false));
  }
}

auto RuntimePool::pool_size() const noexcept -> int {
  return capacity;
}

auto RuntimePool::active_count() const -> int {
  std::lock_guard<std::mutex> lock(mu);
  return static_cast<int>(checked_out.size());
}

auto RuntimePool::free_count() const -> int {
  std::lock_guard<std::mutex> lock(mu);
  return static_cast<int>(free_list.size());
}

// ─── eval_on_context helper ─────────────────────────────────────────

auto eval_on_context(BridgeContext& bc, std::string_view src)
    -> std::expected<Json::Value, EvalError> {
  // Reset execution-scoped timing state. Limit fields and cancelled
  // are left as the caller set them.
  bc.execution_start = std::chrono::steady_clock::now();
  bc.cpu_timer_start = std::chrono::steady_clock::time_point{};
  bc.cpu_time_accumulated = std::chrono::nanoseconds{0};

  // ICD-0.4.1 Layer 2 — pre-`JS_Eval` GlassWorm gate. Runs before
  // the CPU-timer bracket per ICD §Layer 2 / Pass ordering.
  if (auto err = pre_eval_scan(src, "<pool>"); err.has_value()) {
    return std::unexpected(std::move(*err));
  }

  bc.resume_cpu_timer();
  JSValue ev =
      JS_Eval(bc.ctx, src.data(), src.size(), "<pool>", JS_EVAL_TYPE_GLOBAL);
  bc.pause_cpu_timer();

  std::expected<Json::Value, EvalError> result = Json::Value{Json::nullValue};
  if (JS_IsException(ev)) {
    result = std::unexpected(extract_error(bc.ctx, bc));
  } else {
    ConvOutcome conv = js_to_json(bc.ctx, ev, 0);
    if (conv.ok) {
      result = std::move(conv.value);
    } else {
      result = std::unexpected(std::move(conv.error));
    }
  }
  JS_FreeValue(bc.ctx, ev);
  return result;
}

} // namespace plinth::js
