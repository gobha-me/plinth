// SPDX-License-Identifier: MIT
//
// `pubsub.*` host bindings — ICD-0.5.0 §`pubsub.*` JS Stdlib.
//
// pubsub.publish(channel, payload) → Promise<undefined>
//
// Validation order (matches ICD §Binding implementation):
//   1. Sync arg-type checks → JS TypeError. Promise not created.
//   2. Cancelled-context check (Security Constraint 7) → reject
//      inline with pubsub.cancelled.
//   3. Channel regex check → reject inline with pubsub.channel_invalid.
//      Enforced here AND in emit helper (defense in depth per
//      ICD §Security Constraints 1).
//   4. Extension-identity gate — the channel's `<extension>` segment
//      MUST equal bc.extension_name. Rejects pubsub.extension_mismatch.
//      Empty bc.extension_name → same rejection with "no extension
//      context" semantic (audit reason below).
//   5. Layer-3 regex gate — binding only accepts plinth:ext:* channels
//      (Layer 1/2 go through C++ emit_notify directly; JS cannot emit
//      Layer 1 or Layer 2 events). Rejects pubsub.channel_invalid.
//   6. Post-serialization size check — pre-enqueue so the rejection
//      fires even when no DbClient is reachable. Uses the module-level
//      max_payload_bytes from kernel/realtime/emit.hpp.
//   7. Enqueue AsyncOp{type=PUBSUB_PUBLISH} — dispatch arm builds the
//      envelope and calls emit_notify_async.

#include "kernel/js/async_op.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/stdlib_inject.hpp"
#include "kernel/rbac/subscribe_rule.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/channel.hpp"
#include "kernel/realtime/emit.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <json/value.h>
#include <json/writer.h>
#include <quickjs.h>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::js {

namespace {

auto make_arg_span(int argc, JSValue* argv) -> std::span<const JSValue> {
  return {argv, static_cast<std::size_t>(argc)};
}

auto get_bc(JSContext* ctx) -> BridgeContext* {
  return static_cast<BridgeContext*>(JS_GetContextOpaque(ctx));
}

// Reject inline with a {code, message} error object. Identical
// pattern to audit_bindings.cpp.
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

// Serialize to compact JSON — matches the emit helper's wire format
// for pre-enqueue size checking. The dispatch arm re-serializes when
// actually calling pg_notify; the size budget here is therefore the
// same as validate_envelope enforces.
auto compact_size_estimate(const std::string& channel,
                           const Json::Value& payload) -> std::size_t {
  Json::Value env(Json::objectValue);
  env["layer"] = "extension";
  env["channel"] = channel;
  env["payload"] = payload;
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, env).size();
}

auto pubsub_publish(JSContext* ctx, JSValue /*this_val*/, int argc,
                    JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);

  if (args.size() < 2) {
    return JS_ThrowTypeError(
        ctx, "pubsub.publish: expected (channel: string, payload)");
  }
  if (!JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "pubsub.publish: channel must be a string");
  }

  const char* ch_cs = JS_ToCString(ctx, args[0]);
  if (ch_cs == nullptr) {
    return JS_EXCEPTION;
  }
  std::string channel(ch_cs);
  JS_FreeCString(ctx, ch_cs);

  // Convert payload via the shared js_to_json helper.
  auto payload_or = detail::js_to_json(ctx, args[1]);
  if (!payload_or.has_value()) {
    return JS_ThrowTypeError(ctx,
                             "pubsub.publish: failed to convert payload (%s)",
                             payload_or.error().message.c_str());
  }
  Json::Value payload = std::move(*payload_or);

  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "pubsub.publish: bridge context unavailable");
  }
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "pubsub.cancelled", "execution cancelled");
  }

  // Step 3 + 5 — channel regex + Layer-3 gate. A JS binding may
  // only emit on Layer-3 (plinth:ext:<extension>:...); Layer 1 & 2
  // are kernel-side only.
  if (!plinth::realtime::validate_channel(channel)) {
    return reject_inline(ctx, "pubsub.channel_invalid",
                         "channel failed regex: " + channel);
  }
  auto ext = plinth::realtime::channel_extension(channel);
  if (ext.empty()) {
    // Layer 1 or Layer 2 channel — not permitted via JS binding.
    return reject_inline(
        ctx, "pubsub.channel_invalid",
        "pubsub.publish only emits Layer-3 (plinth:ext:*) channels");
  }

  // Step 4 — extension-identity gate. Empty bc.extension_name =
  // kernel-scope BridgeContext, rejected with an explicit "no
  // extension context" semantic (ICD §BridgeContext extension identity).
  if (bc->extension_name.empty()) {
    return reject_inline(ctx, "pubsub.extension_mismatch",
                         "pubsub.publish requires an extension BridgeContext");
  }
  if (ext != bc->extension_name) {
    return reject_inline(ctx, "pubsub.extension_mismatch",
                         "channel's extension segment '" + std::string{ext} +
                             "' does not match caller '" + bc->extension_name +
                             "'");
  }

  // Step 6 — pre-enqueue size check. Compose the same envelope the
  // dispatch arm will serialize and re-check against the emit
  // helper's module-level ceiling.
  auto est = compact_size_estimate(channel, payload);
  if (est > plinth::realtime::get_max_payload_bytes()) {
    return reject_inline(
        ctx, "pubsub.payload_too_large",
        "serialized envelope " + std::to_string(est) + " bytes exceeds limit " +
            std::to_string(plinth::realtime::get_max_payload_bytes()) +
            " bytes");
  }

  // Step 7 — enqueue.
  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "pubsub");
  bc->pending_ops.push_back(AsyncOp{.type = AsyncOp::Type::PUBSUB_PUBLISH,
                                    .callback_id = id,
                                    .pubsub_channel = std::move(channel),
                                    .pubsub_payload = std::move(payload)});
  return promise;
}

// Rule-set membership check matching `resolution.cpp::check_permission`
// semantics — the required rule OR the universal `kernel.admin` rule
// grants permission. Vector iteration is fine at the scale of a
// per-user effective-rules list (~ tens of rules).
auto has_rule_or_admin(const std::vector<std::string>& rules,
                       std::string_view required) -> bool {
  return std::ranges::any_of(rules, [required](const std::string& r) {
    return r == required || r == "kernel.admin";
  });
}

auto pubsub_unsubscribe_trampoline(JSContext* ctx, JSValueConst /*this_val*/,
                                   int /*argc*/, JSValueConst* /*argv*/,
                                   int /*magic*/, JSValueConst* func_data)
    -> JSValue {
  // func_data[0] holds the channel string captured at subscribe time.
  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_UNDEFINED;
  }
  // func_data is a QuickJS-managed array; wrap as span to keep the
  // `[0]` read in bounds-safe API.
  auto data_span = std::span<const JSValueConst>{func_data, 1};
  const char* ch_cs = JS_ToCString(ctx, data_span.front());
  if (ch_cs == nullptr) {
    return JS_UNDEFINED;
  }
  std::string channel(ch_cs);
  JS_FreeCString(ctx, ch_cs);

  // Enqueue a sync AsyncOp so the actual deregistration happens on
  // the drain-loop iteration (same ordering contract as other
  // bindings — no reentrant broker mutation inside a running JS
  // call). The dispatcher's inline PUBSUB_UNSUBSCRIBE arm resolves
  // the returned (unused) promise with undefined.
  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return JS_UNDEFINED;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "pubsub");
  bc->pending_ops.push_back(AsyncOp{.type = AsyncOp::Type::PUBSUB_UNSUBSCRIBE,
                                    .callback_id = id,
                                    .pubsub_channel = std::move(channel)});
  // The caller of `unsub()` does not typically await; return the
  // promise anyway so `await unsub()` works for hygienic callers.
  return promise;
}

// Map a `pubsub.*` rejection code to the `(layer, reason)` pair the
// `realtime.broker.subscribe_denied` audit carries. `channel` is
// inspected only for `rbac_denied` / `quota_exceeded` where the
// layer string is not implied by the rejection itself.
auto rejection_audit_fields(std::string_view rejection,
                            std::string_view channel)
    -> std::pair<std::string_view, std::string_view> {
  if (rejection == "pubsub.channel_invalid") {
    return {"invalid", "channel_invalid"};
  }
  if (rejection == "pubsub.layer_unsupported") {
    return {"system", "layer_unsupported"};
  }
  if (rejection == "pubsub.extension_mismatch") {
    return {"extension", "extension_mismatch"};
  }
  // `rbac_denied` or `quota_exceeded` — derive the layer from the
  // channel shape when possible (the quota hit still carries a
  // well-formed channel).
  std::string_view layer = "invalid";
  if (plinth::realtime::validate_channel(channel)) {
    using plinth::realtime::ChannelLayer;
    switch (plinth::realtime::channel_layer(channel)) {
      case ChannelLayer::DATA: layer = "data"; break;
      case ChannelLayer::SYSTEM: layer = "system"; break;
      case ChannelLayer::EXTENSION: layer = "extension"; break;
    }
  }
  std::string_view reason =
      (rejection == "pubsub.quota_exceeded") ? "quota_exceeded" : "rbac_denied";
  return {layer, reason};
}

auto pubsub_subscribe(JSContext* ctx, JSValue /*this_val*/, int argc,
                      JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);

  if (args.size() < 2) {
    return JS_ThrowTypeError(
        ctx, "pubsub.subscribe: expected (channel: string, handler)");
  }
  if (!JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "pubsub.subscribe: channel must be a string");
  }
  if (!JS_IsFunction(ctx, args[1])) {
    return JS_ThrowTypeError(ctx,
                             "pubsub.subscribe: handler must be a function");
  }

  const char* ch_cs = JS_ToCString(ctx, args[0]);
  if (ch_cs == nullptr) {
    return JS_EXCEPTION;
  }
  std::string channel(ch_cs);
  JS_FreeCString(ctx, ch_cs);

  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx,
                             "pubsub.subscribe: bridge context unavailable");
  }
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "pubsub.cancelled", "execution cancelled");
  }

  // Synchronous gauntlet — validation + layer/identity gate + quota.
  // RBAC-derived rejections (`pubsub.rbac_denied`, `pubsub.layer_unsupported`,
  // `pubsub.extension_mismatch`, `pubsub.channel_invalid`) fire inline
  // per ICD §Rejection codes; only the broker-registry mutation is
  // deferred to the dispatch arm. Each denial also fires a
  // rate-limited `realtime.broker.subscribe_denied` audit
  // (§Audit Events — 1/min per (user_id, channel)).
  plinth::log::AuditCtx audit_ctx{
      .user_id = bc->user.user_id, .session_id = {}, .ip_address = {}};
  if (auto rejection = classify_pubsub_subscribe(channel, *bc);
      !rejection.empty()) {
    auto [layer, reason] = rejection_audit_fields(rejection, channel);
    plinth::realtime::broker::note_subscribe_denied(audit_ctx, channel, layer,
                                                    reason, "js");
    return reject_inline(ctx, rejection,
                         "pubsub.subscribe gate denied: " + channel);
  }
  if (bc->persistent_callbacks.size() >=
          plinth::realtime::broker::max_subscriptions_per_conn() &&
      !bc->persistent_callbacks.contains(channel)) {
    auto [layer, reason] =
        rejection_audit_fields("pubsub.quota_exceeded", channel);
    plinth::realtime::broker::note_subscribe_denied(audit_ctx, channel, layer,
                                                    reason, "js");
    return reject_inline(
        ctx, "pubsub.quota_exceeded",
        "subscription quota (" +
            std::to_string(
                plinth::realtime::broker::max_subscriptions_per_conn()) +
            ") exceeded");
  }

  // Store the handler on the bc's persistent-callback map. If a
  // prior subscription exists on this channel, its JSValue is freed
  // before overwrite (§OQ3 resolution — second subscribe overwrites
  // first; U.12 pins the last-writer-wins semantic).
  //
  // The dup is captured in a local so the overwrite path doesn't
  // leak the dup that emplace() rejected when the key already
  // exists — emplace's r-value argument is destroyed without
  // invoking JS_FreeValue, so a JS_DupValue passed inline would
  // bump the handler refcount without a matching decrement.
  JSValue dup_handler = JS_DupValue(ctx, args[1]);
  auto [it, inserted] = bc->persistent_callbacks.emplace(channel, dup_handler);
  if (!inserted) {
    JS_FreeValue(ctx, it->second);
    it->second = dup_handler;
  }

  // Enqueue the broker-registration op.
  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "pubsub");
  bc->pending_ops.push_back(AsyncOp{.type = AsyncOp::Type::PUBSUB_SUBSCRIBE,
                                    .callback_id = id,
                                    .pubsub_channel = std::move(channel)});
  return promise;
}

} // namespace

namespace {

// Cross-extension subscribe gate — honors the per-channel rule
// (ICD-0.5.2 §SC6) or falls back to `kernel.admin`-only when
// `rbac_enforce` is disabled (WS-side parity with
// `subscriptions.cpp::subscribe_allowed`). `derived_rule` is the
// empty string when the caller has already proven the channel is
// well-formed but no rule applies (shouldn't happen in practice —
// validate_channel guards upstream).
auto check_cross_ext(const std::vector<std::string>& rules,
                     std::string_view derived_rule) -> std::string {
  if (!plinth::realtime::broker::is_rbac_enforced()) {
    return has_rule_or_admin(rules, "kernel.admin")
               ? std::string{}
               : std::string{"pubsub.rbac_denied"};
  }
  if (derived_rule.empty()) {
    return "pubsub.channel_invalid";
  }
  return has_rule_or_admin(rules, derived_rule)
             ? std::string{}
             : std::string{"pubsub.rbac_denied"};
}

auto classify_layer_extension(std::string_view channel, const BridgeContext& bc)
    -> std::string {
  auto ext = plinth::realtime::channel_extension(channel);
  if (bc.extension_name.empty()) {
    return "pubsub.extension_mismatch";
  }
  if (ext == bc.extension_name) {
    return {};
  }
  return check_cross_ext(bc.user.effective_rules,
                         plinth::rbac::derive_subscribe_rule(channel));
}

auto classify_layer_data(std::string_view channel, const BridgeContext& bc)
    -> std::string {
  auto rule = plinth::rbac::derive_subscribe_rule(channel);
  if (rule.empty()) {
    return "pubsub.channel_invalid";
  }
  auto dot = rule.find('.');
  std::string_view owner =
      (dot == std::string::npos) ? rule : std::string_view{rule}.substr(0, dot);
  if (bc.extension_name.empty()) {
    return "pubsub.rbac_denied";
  }
  if (owner == bc.extension_name) {
    return {};
  }
  return check_cross_ext(bc.user.effective_rules, rule);
}

} // namespace

// ICD-0.5.2 §pubsub.subscribe JS Binding — validate the channel/layer
// combination against the calling extension's identity and the
// caller's effective RBAC rules. Returns empty string on allow; a
// `pubsub.*` rejection code on deny. Exported via `stdlib_inject.hpp`
// so `run_on_context.cpp`'s PUBSUB_SUBSCRIBE dispatch arm can
// re-validate per §Security Constraint 2 (defense-in-depth).
//
// Ownership model:
//   - Own-extension channels (Layer 3 `plinth:ext:<self>:*`, Layer 1
//     `plinth:data:ext_<self>.*`) are identity-allowed without an RBAC
//     check — consistent with `pubsub.publish`'s identity gate.
//   - Cross-extension channels honor the per-channel subscribe rule
//     (ICD-0.5.2 §Security Constraint 6). The caller's
//     `bc.user.effective_rules` must contain the derived rule
//     `<other>.realtime.subscribe[.<event_class>]` (or the universal
//     `kernel.admin` rule). Mirrors WS-side `subscribe_allowed` in
//     `subscriptions.cpp:68`.
//   - `broker::is_rbac_enforced() == false` degrades to the 0.1.6
//     admin-only fallback for cross-ext (matches the WS-side
//     `rbac_enforce=false` posture).
auto classify_pubsub_subscribe(std::string_view channel,
                               const BridgeContext& bc) -> std::string {
  using plinth::realtime::ChannelLayer;
  if (!plinth::realtime::validate_channel(channel)) {
    return "pubsub.channel_invalid";
  }
  switch (plinth::realtime::channel_layer(channel)) {
    case ChannelLayer::SYSTEM: return "pubsub.layer_unsupported";
    case ChannelLayer::EXTENSION: return classify_layer_extension(channel, bc);
    case ChannelLayer::DATA: return classify_layer_data(channel, bc);
  }
  return "pubsub.channel_invalid";
}

// ICD-0.5.2 §pubsub.subscribe — construct the JS-side unsubscribe
// function that `pubsub.subscribe`'s promise resolves with. Lives in
// the public namespace so `run_on_context.cpp`'s PUBSUB_SUBSCRIBE
// dispatch arm can build it inline on the bc's loop alongside the
// broker registration.
auto make_unsubscribe_function(JSContext* ctx, const std::string& channel)
    -> JSValue {
  JSValue channel_val = JS_NewStringLen(ctx, channel.data(), channel.size());
  std::array<JSValue, 1> data{channel_val};
  JSValue fn = JS_NewCFunctionData(ctx, pubsub_unsubscribe_trampoline, 0, 0, 1,
                                   data.data());
  // JS_NewCFunctionData duplicates the data array; free our copy.
  JS_FreeValue(ctx, channel_val);
  return fn;
}

auto register_pubsub(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "pubsub", "publish", &pubsub_publish, 2);
  inject_sync_fn(ctx, "pubsub", "subscribe", &pubsub_subscribe, 2);
}

} // namespace plinth::js
