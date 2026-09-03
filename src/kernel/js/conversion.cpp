// SPDX-License-Identifier: MIT
//
// See conversion.hpp for scope notes. Implementation mirrors the
// existing anonymous-namespace copies in eval.cpp / runtime_pool.cpp.

#include "kernel/js/conversion.hpp"

#include <cstdint>
#include <expected>
#include <json/value.h>
#include <quickjs.h>
#include <string>

namespace plinth::js::detail {

namespace {

constexpr int MAX_CONVERSION_DEPTH = 64;

// When the JS_SetMemoryLimit ceiling is hit, quickjs-ng throws an
// `InternalError: out of memory`. That rejection's name/message
// properties normally introspect cleanly through
// `read_string_prop` → MEMORY_LIMIT. But when the OOM fired deep
// inside an async function body, the error's property-read path
// itself can fail to allocate the temporary strings — name+message
// come back empty, and the rejection falls through to
// PROMISE_REJECTED_UNHANDLED. This predicate gives classify_rejection
// and extract_error a second-chance check: if the runtime's
// `malloc_size` is within a small slack of `malloc_limit`, the
// rejection is almost certainly OOM, regardless of what we could
// read off the reason value. The slack is tuned so short-lived
// post-OOM GC reclaims don't produce false negatives while normal
// runtime memory use (tens of KiB below the limit) doesn't produce
// false positives.
// 0.5.5.2: bumped from 256 KiB → 1 MiB. Post-OOM the runtime's
// `malloc_size` can drop substantially below the `malloc_limit`
// before the kernel samples (the failed allocation isn't accounted,
// QuickJS may force a GC sweep on the next eval frame, exception-
// shape construction reclaims live state). 256 KiB was tight enough
// that the synchronous-OOM exemplar (`limits: promise-allocation
// loop trips MEMORY_LIMIT`) flaked at ~30-60% under the grouped-
// subprocess shape. 1 MiB is comfortably below the 4 MiB test cap
// and the 16 MiB production cap, so a long-lived bc using ~1 MiB
// of legitimate state will not be misclassified as OOM.
constexpr int64_t OOM_MEMORY_SLACK_BYTES = 1LL * 1024LL * 1024LL;

auto is_runtime_near_memory_limit(JSRuntime* rt) noexcept -> bool {
  if (rt == nullptr) {
    return false;
  }
  JSMemoryUsage stats{};
  JS_ComputeMemoryUsage(rt, &stats);
  return stats.malloc_limit > 0 &&
         stats.malloc_size >= stats.malloc_limit - OOM_MEMORY_SLACK_BYTES;
}

// bc-aware memory predicate: the bc's latched `memory_limit_hit`
// signal beats a live sample, because an OOM peak observed during
// execution may have been reclaimed before we classify.
auto was_memory_limit_hit(const BridgeContext& bc) noexcept -> bool {
  if (bc.memory_limit_hit.load(std::memory_order_acquire)) {
    return true;
  }
  return is_runtime_near_memory_limit(bc.rt);
}

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

auto fail(std::string msg) -> std::expected<Json::Value, EvalError> {
  return std::unexpected(EvalError{.kind = EvalErrorKind::INTERNAL,
                                   .message = std::move(msg),
                                   .line = 0,
                                   .column = 0});
}

auto js_to_json_rec(JSContext* ctx, JSValueConst v, int depth)
    -> std::expected<Json::Value, EvalError>;

auto js_array_to_json(JSContext* ctx, JSValueConst arr, int depth)
    -> std::expected<Json::Value, EvalError> {
  int64_t len = 0;
  if (JS_GetLength(ctx, arr, &len) < 0) {
    return fail("js_to_json: failed to read array length");
  }
  Json::Value out{Json::arrayValue};
  for (int64_t i = 0; i < len; ++i) {
    JSValue elem = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
    if (JS_IsException(elem)) {
      JS_FreeValue(ctx, elem);
      return fail("js_to_json: array element access threw");
    }
    auto child = js_to_json_rec(ctx, elem, depth + 1);
    JS_FreeValue(ctx, elem);
    if (!child.has_value()) {
      return child;
    }
    out.append(std::move(*child));
  }
  return out;
}

auto js_plain_object_to_json(JSContext* ctx, JSValueConst obj, int depth)
    -> std::expected<Json::Value, EvalError> {
  JSPropertyEnum* tab = nullptr;
  uint32_t tab_len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, obj,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    return fail("js_to_json: JS_GetOwnPropertyNames failed");
  }
  Json::Value out{Json::objectValue};
  for (uint32_t i = 0; i < tab_len; ++i) {
    const char* key = JS_AtomToCString(ctx, tab[i].atom);
    JSValue val = JS_GetProperty(ctx, obj, tab[i].atom);
    if (key == nullptr || JS_IsException(val)) {
      if (key != nullptr) {
        JS_FreeCString(ctx, key);
      }
      JS_FreeValue(ctx, val);
      JS_FreePropertyEnum(ctx, tab, tab_len);
      return fail("js_to_json: property read failed");
    }
    auto child = js_to_json_rec(ctx, val, depth + 1);
    std::string key_copy{key};
    JS_FreeCString(ctx, key);
    JS_FreeValue(ctx, val);
    if (!child.has_value()) {
      JS_FreePropertyEnum(ctx, tab, tab_len);
      return child;
    }
    out[key_copy] = std::move(*child);
  }
  JS_FreePropertyEnum(ctx, tab, tab_len);
  return out;
}

auto js_to_json_rec(JSContext* ctx, JSValueConst v, int depth)
    -> std::expected<Json::Value, EvalError> {
  if (depth > MAX_CONVERSION_DEPTH) {
    return fail("js_to_json: max conversion depth exceeded");
  }
  if (JS_IsNull(v) || JS_IsUndefined(v)) {
    return json_null();
  }
  if (JS_IsBool(v)) {
    int b = JS_ToBool(ctx, v);
    if (b < 0) {
      return fail("js_to_json: bool coercion failed");
    }
    return Json::Value{static_cast<bool>(b)};
  }
  if (JS_IsNumber(v)) {
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
      int32_t i32 = 0;
      if (JS_ToInt32(ctx, &i32, v) == 0) {
        return Json::Value{static_cast<Json::Int>(i32)};
      }
    }
    double d = 0.0;
    if (JS_ToFloat64(ctx, &d, v) == 0) {
      return Json::Value{d};
    }
    return fail("js_to_json: number coercion failed");
  }
  if (JS_IsString(v)) {
    const char* s = JS_ToCString(ctx, v);
    if (s == nullptr) {
      return fail("js_to_json: string coercion failed");
    }
    Json::Value out{std::string{s}};
    JS_FreeCString(ctx, s);
    return out;
  }
  if (JS_IsArray(v)) {
    return js_array_to_json(ctx, v, depth);
  }
  if (JS_IsObject(v)) {
    return js_plain_object_to_json(ctx, v, depth);
  }
  return fail("js_to_json: unsupported JS value type");
}

} // namespace

auto sample_memory_peak(BridgeContext& bc) noexcept -> void {
  if (bc.rt == nullptr) {
    return;
  }
  if (bc.memory_limit_hit.load(std::memory_order_acquire)) {
    return; // already latched
  }
  JSMemoryUsage stats{};
  JS_ComputeMemoryUsage(bc.rt, &stats);
  if (stats.malloc_limit > 0 &&
      stats.malloc_size >= stats.malloc_limit - OOM_MEMORY_SLACK_BYTES) {
    bc.memory_limit_hit.store(true, std::memory_order_release);
  }
}

auto js_to_json(JSContext* ctx, JSValueConst v)
    -> std::expected<Json::Value, EvalError> {
  return js_to_json_rec(ctx, v, 0);
}

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

  // Interrupt-induced termination beats QuickJS's own classification.
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

  if (name == "InternalError" &&
      msg.find("out of memory") != std::string::npos) {
    err.kind = EvalErrorKind::MEMORY_LIMIT;
    err.message = std::string{"out of memory: "} + msg;
    return err;
  }
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
  // Peak-memory latch fallback: if the runtime ever came within
  // OOM_MEMORY_SLACK_BYTES of the malloc_limit during execution
  // (latched by the interrupt handler / drive_jobs), or we still
  // see it at the limit now, upgrade to MEMORY_LIMIT. Catches the
  // async-frame-unwind case where the OOM that fired deep in an
  // async body reclaimed its peak before we could introspect the
  // exception.
  if (was_memory_limit_hit(bc)) {
    err.kind = EvalErrorKind::MEMORY_LIMIT;
    err.message = "out of memory: " + combined;
    return err;
  }
  err.message = std::move(combined);
  err.kind = (name == "SyntaxError") ? EvalErrorKind::SYNTAX_ERROR
                                     : EvalErrorKind::RUNTIME_ERROR;
  return err;
}

auto classify_rejection(JSContext* ctx, JSValueConst reason,
                        const BridgeContext& bc) -> EvalError {
  // Same precedence as extract_error: bc-state triggers beat anything
  // QuickJS stamped on the rejection value. The async bridge may
  // wrap an OOM / CPU / wall-clock / cancel trip inside an async
  // function body's promise rejection; this routing keeps the
  // error kind consistent with the sync-throw path.
  if (bc.cancelled.load(std::memory_order_acquire)) {
    return EvalError{.kind = EvalErrorKind::CANCELLED,
                     .message = "execution cancelled",
                     .line = 0,
                     .column = 0};
  }
  if (bc.cpu_limit_exceeded()) {
    return EvalError{.kind = EvalErrorKind::CPU_TIME_EXCEEDED,
                     .message = "CPU time limit exceeded",
                     .line = 0,
                     .column = 0};
  }
  if (bc.wall_clock_exceeded()) {
    return EvalError{.kind = EvalErrorKind::WALL_CLOCK_EXCEEDED,
                     .message = "wall-clock time limit exceeded",
                     .line = 0,
                     .column = 0};
  }

  std::string name;
  std::string msg;
  if (JS_IsError(reason)) {
    name = read_string_prop(ctx, reason, "name");
    msg = read_string_prop(ctx, reason, "message");
  }

  // 0.3.5 — bridge-stamped rejection envelopes carry a `.code`
  // property (see bridge_context.cpp::rejection_to_js). Read it
  // independently of JS_IsError since the envelope is a plain
  // object, not an Error instance. Map the result-size code to the
  // new kind so an unhandled oversize rejection surfaces with a
  // dedicated EvalErrorKind instead of falling through to
  // PROMISE_REJECTED_UNHANDLED.
  std::string code_prop = read_string_prop(ctx, reason, "code");
  if (code_prop == "async.result_size_exceeded") {
    std::string code_msg = read_string_prop(ctx, reason, "message");
    return EvalError{.kind = EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED,
                     .message = code_msg.empty()
                                    ? std::string{"result size exceeded"}
                                    : code_msg,
                     .line = 0,
                     .column = 0};
  }

  if (name == "InternalError" &&
      msg.find("out of memory") != std::string::npos) {
    return EvalError{.kind = EvalErrorKind::MEMORY_LIMIT,
                     .message = std::string{"out of memory: "} + msg,
                     .line = 0,
                     .column = 0};
  }
  bool is_stack_overflow =
      (name == "RangeError" &&
       msg.find("call stack size exceeded") != std::string::npos) ||
      (name == "InternalError" &&
       msg.find("stack overflow") != std::string::npos);
  if (is_stack_overflow) {
    return EvalError{.kind = EvalErrorKind::STACK_OVERFLOW,
                     .message = std::string{"stack overflow: "} + msg,
                     .line = 0,
                     .column = 0};
  }

  // Fall-through: coerce the whole rejection to a string for the
  // diagnostic. Three second-chance upgrades:
  //   1. Peak-memory latch: if the interrupt handler / drive_jobs
  //      saw the runtime within OOM_MEMORY_SLACK_BYTES of the
  //      malloc_limit at any point during execution, the rejection
  //      is OOM even if the async frame has since unwound and
  //      reclaimed memory — see BridgeContext::memory_limit_hit.
  //   2. JS_ToCString scan: `InternalError: out of memory` may
  //      surface via JS_ToCString even when JS_GetPropertyStr for
  //      "name" / "message" individually OOM'd.
  //   3. Stack overflow mirror — same scan pattern for call stack
  //      messages.
  std::string reason_str;
  const char* cs = JS_ToCString(ctx, reason);
  if (cs != nullptr) {
    reason_str.assign(cs);
    JS_FreeCString(ctx, cs);
  }
  // Null / undefined rejection reason with a configured memory limit
  // is a strong signal that QuickJS hit OOM during InternalError
  // construction — the exception path couldn't allocate the Error
  // object and fell back to a bare JS_NULL / JS_UNDEFINED reason.
  // JavaScript code that legitimately `throw null`s is extremely
  // rare; treating this as MEMORY_LIMIT when memory_limit > 0 is
  // safer than leaking it as PROMISE_REJECTED_UNHANDLED. (Found via
  // 0.5.0's added stdlib namespace shifting the N.37 test baseline.)
  bool reason_is_nullish = JS_IsNull(reason) || JS_IsUndefined(reason);
  JSMemoryUsage cur_stats{};
  JS_ComputeMemoryUsage(bc.rt, &cur_stats);
  if (reason_is_nullish && cur_stats.malloc_limit > 0) {
    return EvalError{.kind = EvalErrorKind::MEMORY_LIMIT,
                     .message = "out of memory: rejection reason unrecoverable "
                                "(likely OOM during exception construction)",
                     .line = 0,
                     .column = 0};
  }
  if (was_memory_limit_hit(bc) ||
      reason_str.find("out of memory") != std::string::npos) {
    return EvalError{.kind = EvalErrorKind::MEMORY_LIMIT,
                     .message = std::string{"out of memory: "} + reason_str,
                     .line = 0,
                     .column = 0};
  }
  if (reason_str.find("call stack size exceeded") != std::string::npos ||
      reason_str.find("stack overflow") != std::string::npos) {
    return EvalError{.kind = EvalErrorKind::STACK_OVERFLOW,
                     .message = std::string{"stack overflow: "} + reason_str,
                     .line = 0,
                     .column = 0};
  }
  return EvalError{.kind = EvalErrorKind::PROMISE_REJECTED_UNHANDLED,
                   .message = std::string{"unhandled promise rejection: "} +
                              reason_str,
                   .line = 0,
                   .column = 0};
}

} // namespace plinth::js::detail
