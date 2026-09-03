// SPDX-License-Identifier: MIT
//
// `audit.*` host bindings — ICD-0.3.3 §Injected `audit.*` Surface.
//
// audit.log(event_type, payload) → Promise<undefined>
//
// Validation order (ICD §Argument Rules + Reserved Prefix Policy +
// Non-Forgeable Provenance):
//   1. Sync arg type checks → JS TypeError. The promise is NOT
//      created.
//   2. Cancelled-context check (Security Constraint 7) → reject inline
//      with audit.cancelled.
//   3. Reserved-prefix check (event_type starts with kernel-reserved
//      prefix) → reject with audit.reserved_prefix.
//   4. Invalid-prefix check (event_type doesn't start with "ext.")
//      → reject with audit.invalid_prefix.
//   5. Reserved-field check (payload contains user_id/session_id/
//      ip_address/extension_id/node_id/call_depth/timestamp) → reject
//      with audit.reserved_field.
//   6. g_audit_ready gate is checked at dispatch time (Step 8) — not
//      here — so the audit.not_ready rejection path is exercised by a
//      separate code path. The binding always enqueues if the
//      validation gauntlet passes.

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
#include <string_view>
#include <utility>

namespace plinth::js {

namespace {

// Kernel-reserved event-type prefixes (ICD §Reserved Prefix Policy,
// canonical list from ICD-0.1.7 §Audit Event Catalog).
constexpr std::array<std::string_view, 6> KERNEL_RESERVED_PREFIXES = {
    "user.", "session.", "pat.", "group.", "rbac.", "capability."};

// Non-forgeable payload keys — the kernel fills these from the
// BridgeContext at dispatch time. Presence of any in the JS payload
// rejects with audit.reserved_field per ICD §Non-Forgeable Provenance.
constexpr std::array<std::string_view, 7> RESERVED_PAYLOAD_KEYS = {
    "user_id", "session_id", "ip_address", "extension_id",
    "node_id", "call_depth", "timestamp"};

auto make_arg_span(int argc, JSValue* argv) -> std::span<const JSValue> {
  return {argv, static_cast<std::size_t>(argc)};
}

auto get_bc(JSContext* ctx) -> BridgeContext* {
  return static_cast<BridgeContext*>(JS_GetContextOpaque(ctx));
}

// Reject inline (no AsyncOp enqueue) with the given code/message.
// Returns the freshly-created rejected promise.
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

auto starts_with(std::string_view s, std::string_view prefix) -> bool {
  return s.starts_with(prefix);
}

auto find_reserved_payload_key(const Json::Value& payload) -> std::string_view {
  if (!payload.isObject()) {
    return {};
  }
  for (auto key : RESERVED_PAYLOAD_KEYS) {
    if (payload.isMember(std::string{key})) {
      return key;
    }
  }
  return {};
}

auto audit_log(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);

  // Sync arg validation.
  if (args.size() < 2) {
    return JS_ThrowTypeError(
        ctx, "audit.log: expected (event_type: string, payload: object)");
  }
  if (!JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "audit.log: event_type must be a string");
  }
  if (!JS_IsObject(args[1]) || JS_IsArray(args[1]) || JS_IsNull(args[1])) {
    return JS_ThrowTypeError(ctx, "audit.log: payload must be a plain object");
  }
  const char* et_cs = JS_ToCString(ctx, args[0]);
  if (et_cs == nullptr) {
    return JS_EXCEPTION;
  }
  std::string event_type(et_cs);
  JS_FreeCString(ctx, et_cs);
  if (event_type.empty()) {
    return JS_ThrowTypeError(ctx, "audit.log: event_type must be non-empty");
  }

  // Convert payload to Json::Value via the shared helper.
  auto payload_or = detail::js_to_json(ctx, args[1]);
  if (!payload_or.has_value()) {
    return JS_ThrowTypeError(ctx, "audit.log: failed to convert payload (%s)",
                             payload_or.error().message.c_str());
  }
  Json::Value payload = std::move(*payload_or);

  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "audit.log: bridge context unavailable");
  }
  // Security Constraint 7: cancelled context → reject inline.
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "audit.cancelled", "execution cancelled");
  }

  // Reserved-prefix check (kernel-namespace prefixes).
  for (auto pfx : KERNEL_RESERVED_PREFIXES) {
    if (starts_with(event_type, pfx)) {
      return reject_inline(
          ctx, "audit.reserved_prefix",
          std::string{"event_type uses kernel-reserved prefix: "} +
              std::string{pfx});
    }
  }
  // Invalid-prefix check (must start with "ext.").
  if (!starts_with(event_type, "ext.")) {
    return reject_inline(ctx, "audit.invalid_prefix",
                         "extension audit events must start with 'ext.<id>.'");
  }
  // Reserved-field check.
  auto bad_key = find_reserved_payload_key(payload);
  if (!bad_key.empty()) {
    return reject_inline(ctx, "audit.reserved_field",
                         std::string{"payload contains non-forgeable field: "} +
                             std::string{bad_key});
  }

  // All validation passed — enqueue the AsyncOp.
  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "audit");
  bc->pending_ops.push_back(AsyncOp{.type = AsyncOp::Type::AUDIT_WRITE,
                                    .callback_id = id,
                                    .sql = {},
                                    .sql_params = {},
                                    .silent = false,
                                    .audit_event_type = std::move(event_type),
                                    .audit_payload = std::move(payload),
                                    .audit_user_id = bc->user.user_id,
                                    .audit_session_id = bc->user.session_id,
                                    .audit_ip_address = bc->user.ip_address});
  return promise;
}

} // namespace

auto register_audit(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "audit", "log", &audit_log, 2);
}

} // namespace plinth::js
