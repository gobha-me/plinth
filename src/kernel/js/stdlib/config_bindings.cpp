// SPDX-License-Identifier: MIT
//
// `config.get(key)` host binding — ICD-0.3.2 §Injected Surface /
// config.get + §Security Constraint 1–2.
//
// The projection table is compile-time. Every key returned here is
// explicitly whitelisted; unknown or excluded keys (including every
// Config::Database field and `migrations_dir`) return `null`. Adding a
// key means editing this file.

#include "kernel/js/bridge_context.hpp"
#include "kernel/js/stdlib_inject.hpp"

#include <quickjs.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace plinth::js {

namespace {

// A single projection row. `extract` receives the populated
// ConfigProjection for the current context and returns a fresh
// owned JSValue. The handler is responsible for the JSValue
// lifetime contract (caller frees the result when it falls out of
// scope / is handed to QuickJS as a return value).
struct ProjectionRow {
  std::string_view key;
  JSValue (*extract)(JSContext* ctx, const ConfigProjection& cp);
};

auto extract_dev_mode(JSContext* ctx, const ConfigProjection& cp) -> JSValue {
  return JS_NewBool(ctx, cp.dev_mode);
}
auto extract_node_id(JSContext* ctx, const ConfigProjection& cp) -> JSValue {
  return JS_NewStringLen(ctx, cp.node_id.data(), cp.node_id.size());
}
auto extract_listen_host(JSContext* ctx, const ConfigProjection& cp)
    -> JSValue {
  return JS_NewStringLen(ctx, cp.listen_host.data(), cp.listen_host.size());
}
auto extract_listen_port(JSContext* ctx, const ConfigProjection& cp)
    -> JSValue {
  return JS_NewInt32(ctx, static_cast<int32_t>(cp.listen_port));
}
auto extract_reg_enabled(JSContext* ctx, const ConfigProjection& cp)
    -> JSValue {
  return JS_NewBool(ctx, cp.registration_enabled);
}
auto extract_ws_auth_timeout(JSContext* ctx, const ConfigProjection& cp)
    -> JSValue {
  return JS_NewFloat64(ctx, cp.ws_auth_timeout_s);
}
auto extract_ws_hb_interval(JSContext* ctx, const ConfigProjection& cp)
    -> JSValue {
  return JS_NewFloat64(ctx, cp.ws_heartbeat_interval_s);
}
auto extract_ws_hb_timeout(JSContext* ctx, const ConfigProjection& cp)
    -> JSValue {
  return JS_NewFloat64(ctx, cp.ws_heartbeat_timeout_s);
}

// The compile-time projection. Adding a row is a PR (see ICD-0.3.2
// §Security Constraint 1). Order is display order; lookup is linear
// — 8 rows, negligible cost relative to a QuickJS call.
constexpr std::array<ProjectionRow, 8> PROJECTION{{
    {.key = "dev_mode", .extract = &extract_dev_mode},
    {.key = "node_id", .extract = &extract_node_id},
    {.key = "listen_host", .extract = &extract_listen_host},
    {.key = "listen_port", .extract = &extract_listen_port},
    {.key = "registration_enabled", .extract = &extract_reg_enabled},
    {.key = "ws.auth_timeout_s", .extract = &extract_ws_auth_timeout},
    {.key = "ws.heartbeat_interval_s", .extract = &extract_ws_hb_interval},
    {.key = "ws.heartbeat_timeout_s", .extract = &extract_ws_hb_timeout},
}};

auto config_get(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  // Pull the argv pointer into a span once so per-slot access uses
  // std::span::operator[] (permitted by cppcoreguidelines) instead
  // of raw pointer arithmetic on every reference.
  std::span<const JSValue> args(argv, static_cast<std::size_t>(argc));
  if (args.empty() || !JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "config.get: expected string at arg 0");
  }
  std::size_t len = 0;
  const char* key = JS_ToCStringLen(ctx, &len, args[0]);
  if (key == nullptr) {
    return JS_EXCEPTION;
  }
  std::string_view key_sv{key, len};

  const auto* bc = static_cast<const BridgeContext*>(JS_GetContextOpaque(ctx));
  // Missing BridgeContext should never happen under RuntimePool (the
  // pool calls JS_SetContextOpaque before inject_kernel_stdlib); if
  // it does, treat every key as absent rather than dereferencing a
  // nullptr. Safe fail.
  if (bc == nullptr) {
    JS_FreeCString(ctx, key);
    return JS_NULL;
  }

  for (const auto& row : PROJECTION) {
    if (row.key == key_sv) {
      JS_FreeCString(ctx, key);
      return row.extract(ctx, bc->config_proj);
    }
  }
  JS_FreeCString(ctx, key);
  return JS_NULL;
}

} // namespace

auto register_config(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "config", "get", &config_get, 1);
}

} // namespace plinth::js
