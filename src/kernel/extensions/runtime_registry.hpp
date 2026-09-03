// SPDX-License-Identifier: MIT
//
// plinth::extensions::RuntimeRegistry — process-lifetime registry of
// per-extension RuntimePools with the Tier 2 capability dispatch entry
// point. See ICD-0.5.0.3-extension-dispatch §RuntimeRegistry.
//
// Scope in 0.5.0.4:
//   * One RuntimePool per installed-ACTIVE extension with a `server/`
//     tree on disk. Client-only packages (no `server/`) skip cleanly.
//   * Install-lifecycle hooks (`create_pool` / `destroy_pool`) are
//     idempotent and tolerate being called under racing state
//     transitions — a pool that cannot be built is a spdlog::warn, not
//     a fatal.
//   * `dispatch(...)` snapshots a shared pool lease under the registry lock;
//     no mutex is held across coroutine suspension, and shutdown drains every
//     accepted lease before reporting success.
//   * Handler file layout:
//       {data_dir}/extensions/{name}/active/server/handlers/{fn}.js
//     ES module with `export default function <fn>(args) { ... }`.
//     ICD §Handler contract fixes the module shape.
//
// Out of scope (deferred):
//   * Persistent per-pool handler-module cache (ICD §Out of scope).
//   * `cap.pool_exhausted` back-pressure (ICD §Out of scope).
//   * Versioned pool coexistence during UPGRADE (hard cut for 0.5.0.4).
//   * Per-extension manifest-declared RuntimeLimits overrides.

#pragma once

#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"

#include <chrono>
#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <json/value.h>
#include <string>
#include <string_view>

namespace plinth::extensions {

// Process-lifetime bootstrap. Caches a pointer to `cfg` for use at
// dispatch time (handler-path resolution consumes `cfg.packages_data_dir`)
// and iterates `SELECT name FROM plinth.packages WHERE state = 'ACTIVE'`
// to spin up a pool per installed extension. Per-row failures log a
// warn; the kernel boots regardless — a broken extension's handlers
// reject with `cap.extension_not_loaded`, not a startup abort.
//
// Call from main.cpp after `capabilities::init_resolver` (so the Tier 2
// cache is populated when a freshly-created pool is first dispatched
// into) and before route registration.
auto init_registry(const Config& cfg) -> void;

// Deterministic teardown. Stops admission, removes every registered pool, and
// waits up to `timeout` for accepted dispatches to release their owned pool
// references. Returns false on timeout.
// The process coordinator calls this after realtime listener shutdown and
// before the database/audit/logging teardown:
// pool destruction may emit final spdlog lines; realtime must already
// be stopped so any in-flight `pubsub.publish` from a torn-down handler
// doesn't race the listener.
auto shutdown_registry(std::chrono::milliseconds timeout = std::chrono::seconds{
                           35}) -> bool;

// Install-lifecycle hook — called from the three
// `provider_type = "extension"` capability-INSERT sites in
// `install_lifecycle.cpp` (INSTALL-from-empty, UPGRADE-to-ACTIVE,
// ENABLE-from-DISABLED) AFTER the owning SQL transaction commits.
// Idempotent: a second call for the same name destroys the existing
// pool first.
//
// Returns false (without logging) when `{data_dir}/extensions/{name}/
// active/server/` does not exist (client-only package; nothing to
// dispatch to). Returns true on a created pool, or on an error where
// the kernel chooses to continue (the error is logged).
auto create_pool(std::string_view extension_name) -> bool;

// Install-lifecycle hook — called from DISABLED / UPGRADING / UNINSTALL
// commit sites. Removes the pool under the registry's exclusive lock and
// destroys it after releasing that lock. Idempotent: no-op when the name is
// unknown. In-flight dispatches retain a shared lease to the removed pool and
// finish safely.
auto destroy_pool(std::string_view extension_name) -> void;

// Test-visible ownership diagnostic. Returns the number of dispatch leases
// which shutdown must drain.
[[nodiscard]] auto inflight_dispatch_count_for_test() -> std::size_t;

// The Tier 2 extension dispatch entry — invoked from
// `capabilities::call_capability_async` after resolve + RBAC for an
// entry whose `provider_type == "extension"`. Looks up the pool,
// acquires a BridgeContext, threads caller identity + (depth + 1),
// reads the handler file from disk fresh each call, invokes the
// ES-module's default export via the wrapper source + `import_from_src`
// intrinsic, and maps the outcome onto the PromiseRejection shape the
// resolver promotes to `CapabilityError::EXTENSION_DISPATCH_FAILED`.
//
// `args` is moved in by the caller. `caller` is the caller's UserContext
// (authoritative identity per ICD-0.5.0.3 §Security Constraint 1).
// `caller_call_depth` is the caller's pre-dispatch depth; this function
// sets `bc.call_depth = caller_call_depth + 1` on the callee.
//
// and caller outlive the coroutine by resolver contract; copying would double
// every JSON-arg hop.
auto dispatch(std::string_view extension_name, std::string_view function,
              const Json::Value& args,
              const plinth::capabilities::UserContext& caller,
              int caller_call_depth)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>>;

} // namespace plinth::extensions
