// SPDX-License-Identifier: MIT
//
// Implementation of BridgeContext timer methods (ICD-0.3.1
// §Timer Semantics) and async-state methods (ICD-0.3.3
// §BridgeContext Async Activation).

#include "kernel/js/bridge_context.hpp"

#include <json/value.h>
#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace plinth::js {

namespace {

// Convert a Json::Value into a JSValue on `ctx`. Mirrors the
// json-to-js helper that lives privately in eval.cpp; duplicated
// here for the resolve() fast path so we don't drag eval.cpp's
// internals into a public-method dependency. Recursive on objects
// and arrays. Allocates fresh JS objects/arrays/strings — no aliasing
// to the source `Json::Value` after this returns.
// Decode a PG hex-escape payload ("deadbeef") into raw bytes. Returns
// nullopt if the input contains any non-hex char or has an odd length.
auto decode_hex_payload(const std::string& hex)
    -> std::optional<std::vector<std::uint8_t>> {
  if ((hex.size() % 2) != 0) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return (c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return (c - 'A') + 10;
    }
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

auto json_to_js(JSContext* ctx, const Json::Value& v) -> JSValue {
  switch (v.type()) {
    case Json::nullValue: return JS_NULL;
    case Json::booleanValue: return JS_NewBool(ctx, v.asBool());
    case Json::intValue: return JS_NewInt64(ctx, v.asInt64());
    case Json::uintValue:
      return JS_NewInt64(ctx, static_cast<int64_t>(v.asUInt64()));
    case Json::realValue: return JS_NewFloat64(ctx, v.asDouble());
    case Json::stringValue: {
      auto s = v.asString();
      return JS_NewStringLen(ctx, s.data(), s.size());
    }
    case Json::arrayValue: {
      JSValue arr = JS_NewArray(ctx);
      for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, json_to_js(ctx, v[i]));
      }
      return arr;
    }
    case Json::objectValue: {
      // ICD-0.5.3 §OID switch table BYTEA — kernel-side tag
      // `{__bytea_hex__: "deadbeef"}` unwraps into a JS
      // Uint8Array. Matches the write-direction `__bytea__` tag
      // symmetrically; the hex-vs-raw distinction is an impl
      // detail (PG text repr for BYTEA is already hex-prefixed).
      if (v.size() == 1 && v.isMember("__bytea_hex__") &&
          v["__bytea_hex__"].isString()) {
        auto bytes = decode_hex_payload(v["__bytea_hex__"].asString());
        if (bytes.has_value()) {
          return JS_NewUint8ArrayCopy(ctx, bytes->data(), bytes->size());
        }
        // Malformed payload → fall through to generic object.
      }
      JSValue obj = JS_NewObject(ctx);
      for (const auto& key : v.getMemberNames()) {
        JS_SetPropertyStr(ctx, obj, key.c_str(), json_to_js(ctx, v[key]));
      }
      return obj;
    }
  }
  return JS_UNDEFINED;
}

// Build the JS-visible rejection object `{code, message[, sqlstate]}`.
auto rejection_to_js(JSContext* ctx, const PromiseRejection& err) -> JSValue {
  JSValue obj = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, obj, "code",
                    JS_NewStringLen(ctx, err.code.data(), err.code.size()));
  JS_SetPropertyStr(
      ctx, obj, "message",
      JS_NewStringLen(ctx, err.message.data(), err.message.size()));
  if (err.sqlstate.has_value()) {
    JS_SetPropertyStr(
        ctx, obj, "sqlstate",
        JS_NewStringLen(ctx, err.sqlstate->data(), err.sqlstate->size()));
  }
  return obj;
}

} // namespace

auto BridgeContext::resume_cpu_timer() noexcept -> void {
  cpu_timer_start = std::chrono::steady_clock::now();
}

auto BridgeContext::pause_cpu_timer() noexcept -> void {
  // An unpaired pause (timer never resumed, or already paused) folds
  // a zero-length interval — cheap and safe.
  if (cpu_timer_start == std::chrono::steady_clock::time_point{}) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  cpu_time_accumulated += (now - cpu_timer_start);
  cpu_timer_start = std::chrono::steady_clock::time_point{};
}

auto BridgeContext::cpu_limit_exceeded() const noexcept -> bool {
  // The live phase's elapsed time is added when a timer is currently
  // running so the interrupt handler can trip mid-execution without
  // waiting for a pause().
  auto live = std::chrono::nanoseconds{0};
  if (cpu_timer_start != std::chrono::steady_clock::time_point{}) {
    live = std::chrono::steady_clock::now() - cpu_timer_start;
  }
  auto total_ns = cpu_time_accumulated + live;
  auto limit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(cpu_time_limit);
  return total_ns > limit_ns;
}

auto BridgeContext::wall_clock_exceeded() const noexcept -> bool {
  if (execution_start == std::chrono::steady_clock::time_point{}) {
    return false;
  }
  auto elapsed = std::chrono::steady_clock::now() - execution_start;
  auto limit_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_clock_limit);
  return elapsed > limit_ns;
}

// ── Async state methods (ICD-0.3.3) ──────────────────────────────────

auto BridgeContext::register_pending(JSValue resolve_fn, JSValue reject_fn,
                                     std::string ns_for_cancellation) -> int {
  int id = next_callback_id++;
  callbacks.emplace(id,
                    PromiseCallbacks{
                        .resolve = resolve_fn,
                        .reject = reject_fn,
                        .ns_for_cancellation = std::move(ns_for_cancellation),
                    });
  return id;
}

auto BridgeContext::resolve(int callback_id, const Json::Value& result)
    -> void {
  auto it = callbacks.find(callback_id);
  if (it == callbacks.end()) {
    // Abandoned by run_cancellation_cascade. The coroutine loop
    // surfaces PROMISE_RESOLVE_AFTER_CANCEL if it really matters;
    // here we just drop the result without touching freed JS state
    // (Security Constraint 6).
    spdlog::debug("BridgeContext::resolve: abandoned callback_id={}",
                  callback_id);
    return;
  }
  PromiseCallbacks cbs = it->second;
  callbacks.erase(it);

  JSValue js_result = json_to_js(ctx, result);
  JSValue ret = JS_Call(ctx, cbs.resolve, JS_UNDEFINED, 1, &js_result);
  JS_FreeValue(ctx, js_result);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, cbs.resolve);
  JS_FreeValue(ctx, cbs.reject);

  if (concurrent_async_ops > 0) {
    --concurrent_async_ops;
  }
}

auto BridgeContext::reject(int callback_id, const PromiseRejection& err)
    -> void {
  auto it = callbacks.find(callback_id);
  if (it == callbacks.end()) {
    spdlog::debug("BridgeContext::reject: abandoned callback_id={}",
                  callback_id);
    return;
  }
  PromiseCallbacks cbs = it->second;
  callbacks.erase(it);

  JSValue js_err = rejection_to_js(ctx, err);
  JSValue ret = JS_Call(ctx, cbs.reject, JS_UNDEFINED, 1, &js_err);
  JS_FreeValue(ctx, js_err);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, cbs.resolve);
  JS_FreeValue(ctx, cbs.reject);

  if (concurrent_async_ops > 0) {
    --concurrent_async_ops;
  }
}

auto BridgeContext::resolve_with_js_value(int callback_id, JSValue value)
    -> void {
  auto it = callbacks.find(callback_id);
  if (it == callbacks.end()) {
    spdlog::debug(
        "BridgeContext::resolve_with_js_value: abandoned callback_id={}",
        callback_id);
    JS_FreeValue(ctx, value);
    return;
  }
  PromiseCallbacks cbs = it->second;
  callbacks.erase(it);

  JSValue ret = JS_Call(ctx, cbs.resolve, JS_UNDEFINED, 1, &value);
  JS_FreeValue(ctx, value);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, cbs.resolve);
  JS_FreeValue(ctx, cbs.reject);

  if (concurrent_async_ops > 0) {
    --concurrent_async_ops;
  }
}

auto BridgeContext::invoke_callback(const std::string& channel,
                                    const Json::Value& arg) -> void {
  auto it = persistent_callbacks.find(channel);
  if (it == persistent_callbacks.end()) {
    // Subscriber torn down between the broker's snapshot and our
    // queueInLoop hop — silent drop per ICD §pubsub.subscribe JS
    // Binding → Handler invocation on dispatch.
    return;
  }
  JSValue handler = it->second;
  JSValue js_arg = json_to_js(ctx, arg);
  JSValue ret = JS_Call(ctx, handler, JS_UNDEFINED, 1, &js_arg);
  if (JS_IsException(ret)) {
    // Swallow + log: a handler throw should not propagate into
    // the broker's dispatch loop. A real logging path would lift
    // the exception here; 0.5.2 ships the minimal drop.
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    spdlog::warn("pubsub.subscribe handler threw: {}",
                 (msg != nullptr ? msg : "<no message>"));
    if (msg != nullptr) {
      JS_FreeCString(ctx, msg);
    }
    JS_FreeValue(ctx, exc);
  }
  JS_FreeValue(ctx, js_arg);
  JS_FreeValue(ctx, ret);
}

auto BridgeContext::drop_persistent_callbacks() -> void {
  for (auto& [_channel, handler] : persistent_callbacks) {
    JS_FreeValue(ctx, handler);
  }
  persistent_callbacks.clear();
}

auto BridgeContext::take_pending_ops() -> std::vector<AsyncOp> {
  std::vector<AsyncOp> out;
  out.swap(pending_ops);
  return out;
}

auto BridgeContext::has_pending_ops() const noexcept -> bool {
  return !pending_ops.empty();
}

auto BridgeContext::pending_op_count() const noexcept -> int {
  // In-flight (callbacks size) + queued (pending_ops size). The
  // queued ones haven't yet bumped concurrent_async_ops; the
  // in-flight ones have.
  return static_cast<int>(callbacks.size()) +
         static_cast<int>(pending_ops.size());
}

auto BridgeContext::signal_completion() -> void {
  // Invariants:
  //   - Called from main loop (queueInLoop callback site).
  //   - Mutex serializes the waiter-vs-signaler transition so that
  //     an await_suspend storing `h` cannot race a signal that
  //     arrives between our earlier await_ready check and the
  //     store. See AnyCompletionAwaiter below.
  std::coroutine_handle<> to_resume;
  {
    std::lock_guard<std::mutex> g{wake_mu};
    if (waiter_handle) {
      to_resume = std::exchange(waiter_handle, std::coroutine_handle<>{});
      wake_count = 0; // consumed by the resumption below
    } else {
      ++wake_count;
    }
  }
  if (to_resume) {
    to_resume.resume();
  }
}

} // namespace plinth::js
