// SPDX-License-Identifier: MIT
//
// `cap.*` host bindings — ICD-0.3.4 §Injected `cap.*` Surface.
//
// Two bindings: `cap.call(signature, args?)` and `cap.batch(calls)`.
//   1. `cap.call` validates its args synchronously, honors
//      Security Constraint 6 (cancelled BC → reject inline), creates a
//      Promise capability, registers resolve/reject on the BridgeContext
//      under namespace "cap" (cancellation cascade produces
//      "cap.cancelled"), and enqueues one `AsyncOp::Type::CAP_CALL`.
//   2. `cap.batch` is the JS-`Promise.all` form (per ICD §Binding
//      Implementation Rules "default expectation"): it synthesizes
//      `Promise.all([cap.call(...), ...])` by invoking `js_cap_call`
//      once per tuple, collecting the promises into a JS Array, then
//      invoking `globalThis.Promise.all` on it. Fail-fast semantics
//      are inherited from `Promise.all`.
//
// Real dispatch happens in run_on_context.cpp's `CAP_CALL` arm, which
// reads `bc.user` and `bc.call_depth` off the BridgeContext and awaits
// `plinth::capabilities::call_capability_async(cc, bc.user)`. See
// `capability_error_to_rejection` below for the `CapabilityError` → JS
// `cap.*` rejection code mapping.
//
// **Pseudocode-vs-signature correction from ICD §Binding Implementation
// Rules:** the ICD pseudocode shows `detail::js_to_json` as a throwing
// helper. The real `js_to_json` returns `std::expected<Json::Value,
// EvalError>`; bindings unpack the expected and call JS_ThrowTypeError
// inline on the failure arm. The runtime shape (TypeError) is what the
// ICD actually specifies — the divergence is cosmetic.
//
// **Explicit error-code switch from ICD §Error Mapping:** the ICD's
// table calls for `cap.not_found` / `cap.invalid_signature`, but
// `validation.cpp`'s `error_string()` produces `capability_not_found` /
// `invalid_capability` for those same variants. We use an explicit
// switch here — NOT a `"cap." + error_string(e)` concatenation.

#include "kernel/js/stdlib/cap_bindings.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/stdlib_inject.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <json/value.h>
#include <quickjs.h>
#include <span>
#include <string>
#include <utility>

namespace plinth::js {

namespace {

auto make_arg_span(int argc, JSValue* argv) -> std::span<const JSValue> {
  return {argv, static_cast<std::size_t>(argc)};
}

auto get_bc(JSContext* ctx) -> BridgeContext* {
  return static_cast<BridgeContext*>(JS_GetContextOpaque(ctx));
}

// Reject the new op's promise inline, used by Security Constraint 6
// (cancelled-context: never enqueue). Local copy of the db_bindings.cpp
// helper by the same name — not hoisted to a shared header (per 0.3.4
// scope; a future refactor may unify).
auto reject_inline(JSContext* ctx, std::string code, std::string msg)
    -> JSValue {
  std::array<JSValue, 2> resolving{};
  JSValue prom = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(prom)) {
    return prom;
  }
  JSValue err = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, err, "code",
                    JS_NewStringLen(ctx, code.data(), code.size()));
  JS_SetPropertyStr(ctx, err, "message",
                    JS_NewStringLen(ctx, msg.data(), msg.size()));
  JSValue ret = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &err);
  JS_FreeValue(ctx, err);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, resolving[0]);
  JS_FreeValue(ctx, resolving[1]);
  return prom;
}

// cap.call(signature, args?) -> Promise<any>
auto cap_call(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);

  if (args.empty() || !JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "cap.call: signature must be a string");
  }
  const char* sig_cs = JS_ToCString(ctx, args[0]);
  if (sig_cs == nullptr) {
    return JS_EXCEPTION;
  }
  std::string signature(sig_cs);
  JS_FreeCString(ctx, sig_cs);
  if (signature.empty()) {
    return JS_ThrowTypeError(ctx, "cap.call: signature must be non-empty");
  }

  Json::Value args_json;
  if (args.size() >= 2 && !JS_IsUndefined(args[1])) {
    auto converted = detail::js_to_json(ctx, args[1]);
    if (!converted.has_value()) {
      return JS_ThrowTypeError(ctx,
                               "cap.call: args contains unsupported value");
    }
    args_json = std::move(*converted);
  }

  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "cap.call: bridge context unavailable");
  }
  // Security Constraint 6: cancelled context → reject inline, do not
  // enqueue.
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "cap.cancelled", "execution cancelled");
  }

  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "cap");
  bc->pending_ops.push_back(
      AsyncOp{.type = AsyncOp::Type::CAP_CALL,
              .callback_id = id,
              .sql = {},
              .sql_params = {},
              .silent = false,
              .audit_event_type = {},
              .audit_payload = {},
              .cap_signature = std::move(signature),
              .cap_args = std::move(args_json),
              // Snapshot identity + call_depth on the enqueuing (main-loop)
              // thread. The detached dispatch arm reads these off the op
              // rather than bc — removes a cross-thread read dependency on
              // bc.user / bc.call_depth (see AsyncOp comment).
              .cap_user = bc->user,
              .cap_call_depth = bc->call_depth});
  return promise;
}

// cap.batch([[sig, args?], ...]) -> Promise<any[]>
//
// Synthesizes `Promise.all([cap.call(sig, args), ...])` by invoking
// cap_call N times in-process and collecting the returned Promises.
// All JS temporaries are JS_FreeValue'd on every error path — this is
// the ASAN gate for Group L.34 / L-style cancellation tests.
auto cap_batch(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);

  if (args.empty() || !JS_IsArray(args[0])) {
    return JS_ThrowTypeError(ctx, "cap.batch: calls must be an array");
  }

  int64_t outer_len = 0;
  if (JS_GetLength(ctx, args[0], &outer_len) < 0) {
    return JS_EXCEPTION;
  }

  // Build a JS Array of promises in-place.
  JSValue arr = JS_NewArray(ctx);
  if (JS_IsException(arr)) {
    return arr;
  }
  for (int64_t i = 0; i < outer_len; ++i) {
    JSValue tuple =
        JS_GetPropertyUint32(ctx, args[0], static_cast<uint32_t>(i));
    if (JS_IsException(tuple) || !JS_IsArray(tuple)) {
      JS_FreeValue(ctx, tuple);
      JS_FreeValue(ctx, arr);
      return JS_ThrowTypeError(
          ctx, "cap.batch: each call must be a [signature, args?] array");
    }
    int64_t inner_len = 0;
    if (JS_GetLength(ctx, tuple, &inner_len) < 0) {
      JS_FreeValue(ctx, tuple);
      JS_FreeValue(ctx, arr);
      return JS_EXCEPTION;
    }
    if (inner_len < 1 || inner_len > 2) {
      JS_FreeValue(ctx, tuple);
      JS_FreeValue(ctx, arr);
      return JS_ThrowTypeError(ctx,
                               "cap.batch: each call must have 1 or 2 elements "
                               "[signature, args?]");
    }
    JSValue sig_val = JS_GetPropertyUint32(ctx, tuple, 0);
    JSValue args_val =
        (inner_len == 2) ? JS_GetPropertyUint32(ctx, tuple, 1) : JS_UNDEFINED;
    std::array<JSValue, 2> call_argv{sig_val, args_val};
    JSValue promise = cap_call(ctx, JS_UNDEFINED, static_cast<int>(inner_len),
                               call_argv.data());
    JS_FreeValue(ctx, sig_val);
    if (inner_len == 2) {
      JS_FreeValue(ctx, args_val);
    }
    JS_FreeValue(ctx, tuple);
    if (JS_IsException(promise)) {
      JS_FreeValue(ctx, arr);
      return promise;
    }
    // JS_SetPropertyUint32 takes ownership of `promise`.
    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), promise);
  }

  // Call globalThis.Promise.all(arr).
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
  JS_FreeValue(ctx, global);
  if (JS_IsException(promise_ctor)) {
    JS_FreeValue(ctx, arr);
    return promise_ctor;
  }
  JSValue all_fn = JS_GetPropertyStr(ctx, promise_ctor, "all");
  if (JS_IsException(all_fn)) {
    JS_FreeValue(ctx, promise_ctor);
    JS_FreeValue(ctx, arr);
    return all_fn;
  }
  JSValue result = JS_Call(ctx, all_fn, promise_ctor, 1, &arr);
  JS_FreeValue(ctx, all_fn);
  JS_FreeValue(ctx, promise_ctor);
  JS_FreeValue(ctx, arr);
  return result;
}

} // namespace

auto capability_error_to_rejection(plinth::capabilities::CapabilityError e,
                                   std::string_view signature,
                                   std::string_view ext_detail_code,
                                   std::string_view ext_detail_message)
    -> PromiseRejection {
  using plinth::capabilities::CapabilityError;
  std::string code;
  std::string msg;
  switch (e) {
    case CapabilityError::CAPABILITY_NOT_FOUND: code = "cap.not_found"; break;
    case CapabilityError::PERMISSION_DENIED:
      code = "cap.permission_denied";
      break;
    case CapabilityError::CALL_DEPTH_EXCEEDED:
      code = "cap.call_depth_exceeded";
      break;
    case CapabilityError::TIER3_NOT_AVAILABLE:
      code = "cap.tier3_not_available";
      break;
    case CapabilityError::CAPABILITY_DISABLED:
      code = "cap.capability_disabled";
      break;
    case CapabilityError::INVALID_CAPABILITY:
      code = "cap.invalid_signature";
      break;
    case CapabilityError::ASYNC_REQUIRED:
      // ICD-0.5.0.3 §Error taxonomy: caller used the sync
      // `call_capability` on an extension entry. JS paths always
      // go through `call_capability_async`, so this surfaces only
      // from non-JS callers that pre-date the migration.
      code = "cap.async_required";
      break;
    case CapabilityError::EXTENSION_DISPATCH_FAILED:
      // ICD-0.5.0.3 §Error taxonomy: the concrete cap.* code was
      // surfaced through the out-parameter channel on
      // `call_capability_async`. Use it verbatim (with its capped
      // message); fall back to cap.internal if the caller neglected
      // to forward the detail.
      if (!ext_detail_code.empty()) {
        code = std::string{ext_detail_code};
        msg = std::string{ext_detail_message};
      } else {
        code = "cap.internal";
      }
      break;
    // Registration-time validation variants should never surface at
    // dispatch time. If they do, we reject as cap.internal per
    // ICD §Error Mapping last row.
    case CapabilityError::INVALID_NAMESPACE:
    case CapabilityError::INVALID_VERSION:
    case CapabilityError::INVALID_FUNCTION:
    case CapabilityError::INVALID_SCOPE:
    case CapabilityError::INVALID_PROVIDER_TYPE:
    case CapabilityError::INVALID_DESCRIPTION:
    case CapabilityError::MISSING_EXTENSION_NAME:
    case CapabilityError::RESERVED_NAMESPACE:
    case CapabilityError::NAMESPACE_MISMATCH:
    case CapabilityError::CAPABILITY_EXISTS:
    case CapabilityError::RBAC_RULE_NOT_FOUND:
    case CapabilityError::USER_SCOPE_NOT_SUPPORTED:
    case CapabilityError::DB_ERROR: code = "cap.internal"; break;
  }
  if (msg.empty()) {
    msg = code + ": " + std::string{signature};
  }
  return PromiseRejection{.code = std::move(code),
                          .message = std::move(msg),
                          .sqlstate = std::nullopt};
}

auto register_cap(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "cap", "call", &cap_call, 2);
  inject_sync_fn(ctx, "cap", "batch", &cap_batch, 1);
}

} // namespace plinth::js
