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
#include "kernel/lifecycle/async_task_registry.hpp"

#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <functional>
#include <json/value.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::frontend {
class FloatLayoutService;
}
namespace plinth::packages::generation {
class CapturedArtifact;
class Budget;
} // namespace plinth::packages::generation

namespace plinth::extensions {

// Metadata only: never exposes an uncounted pool owner. Serial changes on each
// replacement, even when the database UUID/version are unchanged.
struct PoolIdentity {
  std::string package_id;
  std::string name;
  std::string version;
  std::uint64_t serial = 0;
  auto operator==(const PoolIdentity&) const -> bool = default;
};

struct FixedShellStorageDigests {
  std::string module;
  std::string contract;
  std::string migration;
  std::string applied_migration;
};

struct FixedStorageSource;
class PreparedShellStorage {
 public:
  PreparedShellStorage() = default;

 private:
  friend auto prepare_fixed_shell_storage(
      const packages::generation::CapturedArtifact&, const PoolIdentity&,
      const FixedShellStorageDigests&, packages::generation::Budget&)
      -> std::expected<PreparedShellStorage, js::PromiseRejection>;
  friend auto publish_fixed_shell_storage(PreparedShellStorage) -> bool;
  std::shared_ptr<const FixedStorageSource> source;
};

// Kernel-generated expected pins plus the acknowledged kernel-owned migration
// receipt are required. Preparation does no IO and holds no registry mutex.
// Publication attaches only to the exact current stamped pool. Neither step
// establishes the service's fresh RBAC/policy/readiness preconditions.
[[nodiscard]] auto prepare_fixed_shell_storage(
    const packages::generation::CapturedArtifact& artifact,
    const PoolIdentity& expected, const FixedShellStorageDigests& digests,
    packages::generation::Budget& budget)
    -> std::expected<PreparedShellStorage, js::PromiseRejection>;
[[nodiscard]] auto publish_fixed_shell_storage(PreparedShellStorage prepared)
    -> bool;
[[nodiscard]] auto pool_identity(std::string_view name)
    -> std::optional<PoolIdentity>;
[[nodiscard]] auto fixed_shell_storage_identity()
    -> std::optional<PoolIdentity>;
struct FixedShellStorageBinding {
  PoolIdentity identity;
  std::string artifact_digest;
};
// One immutable metadata snapshot, not a runtime owner or authority lease.
[[nodiscard]] auto fixed_shell_storage_binding()
    -> std::optional<FixedShellStorageBinding>;

enum class FixedStorageOperation {
  FLOATS_READ,
  OWN_DEFAULTS_READ,
  ADMIN_DEFAULT_READ,
  ADMIN_DEFAULT_PUT,
  ADMIN_DEFAULT_DELETE,
  RESOLVE_ADMIN_CANDIDATE,
};

// Opaque service-created authority. No public JSON decoder/factory or bypass
// flag. In particular the non-layout operations cannot select FLOATS_COMMIT.
class FixedStorageCall {
 public:
  [[nodiscard]] auto operation() const -> FixedStorageOperation { return op; }
  [[nodiscard]] auto identity() const -> const PoolIdentity& {
    return expected;
  }
  [[nodiscard]] auto arguments() const -> const Json::Value& { return args; }
  auto take_guard() -> std::unique_ptr<capabilities::drain::DispatchGuard> {
    return std::move(guard);
  }

 private:
  friend class plinth::frontend::FloatLayoutService;
  FixedStorageCall(FixedStorageOperation operation, PoolIdentity identity,
                   Json::Value arguments,
                   std::unique_ptr<capabilities::drain::DispatchGuard> guard);
  FixedStorageOperation op;
  PoolIdentity expected;
  Json::Value args;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
};

class ValidatedFloatCommit {
 public:
  [[nodiscard]] auto identity() const -> const PoolIdentity& {
    return expected;
  }
  [[nodiscard]] auto snapshot() const -> const Json::Value& { return layout; }
  auto take_guard() -> std::unique_ptr<capabilities::drain::DispatchGuard> {
    return std::move(guard);
  }

 private:
  friend class plinth::frontend::FloatLayoutService;
  ValidatedFloatCommit(
      PoolIdentity identity, Json::Value snapshot,
      std::unique_ptr<capabilities::drain::DispatchGuard> guard);
  PoolIdentity expected;
  Json::Value layout;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
};

// Frame-owned values: lazy tasks do not borrow a temporary caller or args.
// Both paths acquire a real shell lifecycle guard and counted runtime lease.
auto dispatch_fixed_shell_storage(
    FixedStorageCall call, capabilities::UserContext caller,
    int caller_call_depth,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr)
    -> drogon::Task<std::expected<Json::Value, js::PromiseRejection>>;
auto dispatch_fixed_shell_commit(
    ValidatedFloatCommit commit, capabilities::UserContext caller,
    int caller_call_depth,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted = nullptr)
    -> drogon::Task<std::expected<Json::Value, js::PromiseRejection>>;

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
// references and for each pool's private database loops to drain. Returns
// false on timeout and retains failed owners for retry.
// The process coordinator calls this after ingress/workers stop and before
// database state is flushed and realtime/database/logging are torn down.
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
auto create_pool(std::string_view extension_name, std::string_view package_id,
                 std::string_view version) -> bool;

// Install-lifecycle hook — called from DISABLED / UPGRADING / UNINSTALL
// commit sites. Removes the pool under the registry's exclusive lock and
// destroys it after releasing that lock. Idempotent: no-op when the name is
// unknown. In-flight dispatches retain a shared lease to the removed pool and
// finish safely.
auto destroy_pool(std::string_view extension_name) -> void;

// Read-only admission diagnostic used by startup readiness reconciliation.
// A server-backed application is not ready until its runtime pool exists.
[[nodiscard]] auto has_pool(std::string_view extension_name) -> bool;

// Test-visible ownership diagnostic. Returns the number of dispatch leases
// which shutdown must drain.
[[nodiscard]] auto inflight_dispatch_count_for_test() -> std::size_t;
// Monotonic count of final dispatch releases handed to Drogon's owner loop,
// plus the number of pools retained after a bounded close failure.
[[nodiscard]] auto owner_loop_handoff_count_for_test() -> std::size_t;
[[nodiscard]] auto failed_pool_count_for_test() -> std::size_t;

enum class PoolCreationStage { RESERVED, CONSTRUCTED };
// Deterministic interleaving/fault injection only; invoked OUTSIDE registry_mu.
// A throwing hook uses the real exception/unpublished-retirement path.
using PoolCreationHook =
    std::function<void(std::string_view, PoolCreationStage)>;
auto set_pool_creation_hook_for_test(PoolCreationHook hook) -> void;
[[nodiscard]] auto pending_pool_constructions_for_test() -> std::size_t;
[[nodiscard]] auto fixed_storage_retained_bytes_for_test() -> std::size_t;

enum class ModuleEngineStage { COMPILE, EVALUATE };
// Trusted C++ observation only, immediately before the actual engine calls.
// Observers must not throw, block, re-enter the registry, or retain borrowed
// module-id bytes. Invoked outside the observer mutex; no JS registration or
// authority surface is added. Captured state must have independent ownership.
using ModuleEngineObserver =
    std::function<void(std::string_view, ModuleEngineStage)>;
auto set_module_engine_observer_for_test(ModuleEngineObserver observer) -> void;

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
auto dispatch(
    std::string_view extension_name, std::string_view function,
    const Json::Value& args, const plinth::capabilities::UserContext& caller,
    int caller_call_depth,
    std::shared_ptr<plinth::lifecycle::AsyncTaskLease> accepted = nullptr)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>>;

} // namespace plinth::extensions
