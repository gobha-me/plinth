// SPDX-License-Identifier: MIT
//
// RuntimePool — per-extension pool of QuickJS JSRuntime/JSContext pairs.
// See ICD-0.3.1-runtime-lifecycle.md §RuntimePool Contract.
//
// Scope in 0.3.1: synchronous acquire / release / destroy / rebuild with
// interrupt-handler-backed limit enforcement. No coroutine loop, no
// kernel API injection — those are 0.3.2 / 0.3.3 respectively.

#pragma once

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"

#include <json/value.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::js {

struct RuntimeLimits {
  // Memory: passed verbatim to JS_SetMemoryLimit. Must be > 0.
  std::size_t memory_limit_bytes = 0;
  // CPU and wall-clock: enforced by the interrupt handler via the
  // pause/resume bracket on BridgeContext.
  std::chrono::milliseconds cpu_time_limit{0};
  std::chrono::milliseconds wall_clock_limit{0};
  // Stack: ICD §5 — the effective C-stack byte cap is
  // max_stack_depth × STACK_BYTES_PER_FRAME. Must be > 0.
  int max_stack_depth = 0;
  // Carried through to BridgeContext::max_call_depth. Enforcement
  // lives in the capability dispatcher (ICD-0.2.2).
  int max_call_depth = 8;
  // 0.3.3 — Per-execution back-pressure cap on in-flight async ops.
  // 0 disables async fan-out (bindings reject with
  // async.concurrency_limit). Defaults to 8 per ICD-0.3.3
  // §RuntimeLimits Additions.
  int max_concurrent_async_ops = 8;
  // 0.3.3 — Per-AsyncOp result-size cap. Carried onto BridgeContext
  // for 0.3.5 hardening; 0.3.3 does NOT enforce. Default 16 MiB.
  std::size_t async_result_size_limit_bytes = 16ULL * 1024ULL * 1024ULL;
};

// Factory for the kernel-default limit profile. Mirrors the ICD
// "Resource Limits" defaults — 16 MiB memory, 100 ms CPU, 30 s wall,
// 256 stack frames, depth 8.
[[nodiscard]] auto default_runtime_limits() noexcept -> RuntimeLimits;

// Conservative per-frame byte estimate folded into the JS_SetMaxStackSize
// call. 4 KiB matches QuickJS-ng's own default when paired with 256
// frames (1 MiB total C-stack cap). Maintainer-approved on 2026-04-17.
inline constexpr std::size_t STACK_BYTES_PER_FRAME = 4UL * 1024UL;

// Projects the 0.3.2-allowed subset of `plinth::Config` into a
// ConfigProjection value. Defined here (not in config.hpp) because the
// projection contract is JS-bridge-scoped — see ICD-0.3.2 §Security
// Constraints 1–2.
[[nodiscard]] auto make_config_projection(const Config& cfg) noexcept
    -> ConfigProjection;

class RuntimePool {
 public:
  // Created with a per-extension limit profile and the process
  // Config (sliced into a ConfigProjection for `config.get()` JS
  // reads). Pass `nullptr` for `ext` in host-side (test) usage until
  // the 0.4.x installer lands. `pool_size = -1` means "use the
  // default formula" (min(4, hardware_concurrency() / 2), floored
  // at 1). `user` is the value-copy identity every acquired
  // BridgeContext will carry (ICD-0.3.4 §BridgeContext Additions);
  // nullptr yields UserContext::anonymous(), which is the right
  // choice for host-side test drivers that exercise non-RBAC paths.
  //
  // ICD-0.5.0.3 §RuntimeRegistry — the extension-dispatch pools
  // created by `plinth::extensions::RuntimeRegistry` populate
  // `extension_name` with the owning extension's name. Every
  // BridgeContext produced by `create_entry` copies this value
  // onto `bc.extension_name` so the `pubsub.publish` identity gate
  // (`pubsub_bindings.cpp`) and the realtime listener scope checks
  // resolve to the callee's own identity, even as the callee runs
  // under the caller's UserContext. Empty string denotes a kernel-
  // scope pool (host-eval, LH-0.1 js_stress, tests) and preserves
  // the existing "no extension identity" semantics.
  RuntimePool(const Extension* ext, RuntimeLimits runtime_limits,
              const Config& cfg, int pool_size = -1,
              const plinth::capabilities::UserContext* user = nullptr,
              std::string extension_name = {});
  ~RuntimePool();

  RuntimePool(const RuntimePool&) = delete;
  auto operator=(const RuntimePool&) -> RuntimePool& = delete;
  RuntimePool(RuntimePool&&) = delete;
  auto operator=(RuntimePool&&) -> RuntimePool& = delete;

  // Acquire a ready-to-use context. Returns a context from the free
  // list if one is available; otherwise creates a fresh context on
  // demand. When active_count() ≥ pool_size the on-demand context is
  // marked transient and is destroyed on release() instead of being
  // pooled — the pool does not grow unboundedly.
  [[nodiscard]] auto acquire() -> BridgeContext*;

  // Return a context after successful use. State is reset per ICD
  // §State Reset (globalThis own enumerable string-keyed properties
  // cleared; heap and module state preserved). A transient context is
  // destroyed instead. A context whose cancelled flag is set, or whose
  // last execution produced a *_EXCEEDED error, is also destroyed —
  // this is the defensive branch from ICD §Security Constraint 4.
  auto release(BridgeContext* bc) -> void;

  // Return a context after failure / cancellation. Always destroys.
  auto destroy(BridgeContext* bc) -> void;

  // Destroy all pooled contexts and rebuild a fresh free list of the
  // same pool_size. No-op for any context currently checked out — the
  // caller must first release() or destroy() those. Called on
  // extension hot-reload (architecture/05-extensions.md §3.3).
  auto rebuild() -> void;

  // Diagnostics.
  [[nodiscard]] auto pool_size() const noexcept -> int;
  [[nodiscard]] auto active_count() const -> int;
  [[nodiscard]] auto free_count() const -> int;

 private:
  struct Entry; // internal PIMPL-ish record per context
  using EntryPtr = std::unique_ptr<Entry>;

  auto create_entry(bool transient) -> EntryPtr;

  // `capacity` is the configured pool size; the public `pool_size()`
  // accessor returns it. Naming them differently avoids trailing-
  // underscore members (project-wide convention — plain lower_case,
  // see ConnectionRegistry for the precedent).
  const Extension* extension;
  RuntimeLimits limits;
  ConfigProjection projection;
  plinth::capabilities::UserContext user_ctx;
  std::string ext_name; // see ctor comment
  int capacity;

  mutable std::mutex mu;
  std::vector<EntryPtr> free_list;
  std::vector<EntryPtr> checked_out;
};

// Test / host-eval helper. Runs `src` on the supplied context using the
// matched pause/resume bracket required by the interrupt handler. Sets
// `ctx.execution_start` on entry; resets `cpu_time_accumulated` and
// `cpu_timer_start` so consecutive calls are independent. Classifies
// interrupt-induced exceptions into the 0.3.1-added EvalErrorKind
// variants (CPU_TIME_EXCEEDED, WALL_CLOCK_EXCEEDED, CANCELLED) when
// the interrupt handler was the termination source.
//
// This is the minimum surface needed to drive the ICD §Tests exit
// criteria in 0.3.1. A richer async-aware surface lands in 0.3.3.
[[nodiscard]] auto eval_on_context(BridgeContext& bc, std::string_view src)
    -> std::expected<Json::Value, EvalError>;

} // namespace plinth::js
