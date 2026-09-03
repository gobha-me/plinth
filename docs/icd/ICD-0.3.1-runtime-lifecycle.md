# ICD-0.3.1-runtime-lifecycle

**Traces to:** architecture/05-extensions.md §3.1 (Runtime Limits), architecture/05-extensions.md §3.2 (Extension Supervision and Failure Recovery), DESIGN-quickjs-bridge.md §3.2 (Runtime Pool), DESIGN-quickjs-bridge.md §4 (Resource Limits), DESIGN-quickjs-bridge.md §9.1 (Implementation Sequence: Runtime Lifecycle + Limits), DESIGN-quickjs-bridge.md §10 (Data Structures)
**Depends on:** ICD-0.3.0-quickjs-vendoring (static library, one-shot eval, `EvalError`)
**Milestone:** 0.3.1 — Runtime lifecycle: create, configure limits, destroy
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** ICD-0.2.2-capability-resolution (§Call Depth Tracking — the counter this ICD's `BridgeContext` carries is the same one the dispatcher already enforces), DESIGN-quickjs-bridge.md §6 (Cancellation — full cascade is 0.3.3 scope; this ICD defines only the detection & termination trigger)

---

## Overview

This ICD defines the **synchronous** runtime-lifecycle surface for Plinth's QuickJS integration: the `BridgeContext` struct (fields only, no async methods), the `RuntimePool` class, and the enforcement mechanism for four resource limits — memory, CPU time, wall-clock time, and call depth.

The surface is synchronous because the async bridge (`drogon::Task<>`, promise↔coroutine plumbing, `JS_ExecutePendingJob` loop) is the 0.3.3 milestone. 0.3.1 lays the lifecycle foundation and the limit-enforcement plumbing so that 0.3.3 only has to add the coroutine loop on top. All struct fields required by `DESIGN-quickjs-bridge.md §10` appear here in their final shape; async-only fields are present but fenced as **reserved**.

---

## BridgeContext Contract

### Field Layout (final shape — reserved fields are dormant until 0.3.3)

```cpp
// src/kernel/js/bridge_context.hpp
namespace plinth::js {

struct BridgeContext {
    // --- Populated and active in 0.3.1 ---
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    const Extension* extension = nullptr;   // may be nullptr for host-side eval

    // Resource tracking
    std::chrono::steady_clock::time_point execution_start{};
    std::chrono::steady_clock::time_point cpu_timer_start{};
    std::chrono::nanoseconds cpu_time_accumulated{0};
    std::chrono::milliseconds cpu_time_limit{0};
    std::chrono::milliseconds wall_clock_limit{0};

    // Call depth (populated here; enforced by the capability dispatcher
    // per ICD-0.2.2 §Call Depth Tracking)
    int call_depth = 0;
    int max_call_depth = 8;

    // Cancellation flag — read by the interrupt handler in 0.3.1;
    // also consumed by the async cancellation cascade in 0.3.3
    std::atomic<bool> cancelled{false};

    // --- Reserved for 0.3.3 (async bridge); NOT populated in 0.3.1 ---
    // DESIGN-quickjs-bridge.md §10: pending_ops, callbacks, next_callback_id,
    // concurrent_async_ops, max_concurrent_async_ops.
    // These fields are declared on the struct from 0.3.1 so that the struct
    // layout does not churn when 0.3.3 lands, but code sessions MUST NOT read
    // from or write to them before the async bridge exists.

    // Methods active in 0.3.1
    void resume_cpu_timer() noexcept;
    void pause_cpu_timer() noexcept;
    [[nodiscard]] bool cpu_limit_exceeded() const noexcept;
    [[nodiscard]] bool wall_clock_exceeded() const noexcept;
};

}  // namespace plinth::js
```

The reserved fields (`pending_ops`, `callbacks`, `next_callback_id`, `concurrent_async_ops`, `max_concurrent_async_ops`) MAY be declared on the struct now — a code-session decision — or added in 0.3.3. The ICD does not mandate their declaration in 0.3.1; it mandates only that no 0.3.1 code reads or writes them.

### Timer Semantics

`resume_cpu_timer()` and `pause_cpu_timer()` form a matched bracket around **JS execution phases**. In 0.3.1 — with no async bridge — every `eval()` call has exactly one resume/pause bracket wrapped around the synchronous `JS_Eval` invocation. In 0.3.3, the pair will also bracket each `JS_ExecutePendingJob` sweep, leaving async gaps **outside** the accumulated CPU time.

Declaring the pause/resume API in 0.3.1 — even though there is only one bracket per execution — is deliberate: it locks the interface 0.3.3 will consume, so the async-bridge change is additive, not refactor-driven.

---

## RuntimePool Contract

### Class Surface

```cpp
// src/kernel/js/runtime_pool.hpp
namespace plinth::js {

struct RuntimeLimits {
    std::size_t memory_limit_bytes;
    std::chrono::milliseconds cpu_time_limit;
    std::chrono::milliseconds wall_clock_limit;
    int max_stack_depth;
    int max_call_depth = 8;
};

class RuntimePool {
public:
    // Created with a per-extension limit profile. `pool_size` defaults to
    // min(4, hardware_concurrency() / 2), floored at 1.
    RuntimePool(const Extension& ext, RuntimeLimits limits, int pool_size = -1);
    ~RuntimePool();

    // Non-copyable, non-movable (owns raw JSRuntime*).
    RuntimePool(const RuntimePool&) = delete;
    RuntimePool& operator=(const RuntimePool&) = delete;

    // Acquire a ready-to-use context. Returns a context from the free list
    // if available; otherwise creates a new one on-demand (does NOT grow
    // the pool — the on-demand context is destroyed on release, not pooled).
    [[nodiscard]] BridgeContext* acquire();

    // Return a context after successful use. State is reset (see §State
    // Reset). If the context was created on-demand (above pool size), it
    // is destroyed instead of returned to the free list.
    void release(BridgeContext* ctx);

    // Return a context after failure/cancellation. Context is always
    // destroyed — a failed context is never reused.
    void destroy(BridgeContext* ctx);

    // Destroy all pooled contexts and rebuild them from fresh. Called on
    // extension hot-reload (architecture/05-extensions.md §3.3).
    void rebuild();

    // Diagnostics
    [[nodiscard]] int pool_size() const noexcept;
    [[nodiscard]] int active_count() const noexcept;
};

}  // namespace plinth::js
```

### State Reset Between Uses

Per `DESIGN-quickjs-bridge.md §3.2`:

- **Cleared** between uses: global variables set during execution (the pool clears the global object's own enumerable string-keyed properties — not the prototype chain).
- **Preserved** between uses: the extension's module-level state. This is intentional — extensions may cache expensive computations across calls.
- **Preserved** between uses: the JS heap itself. `JS_FreeRuntime` is only called by `destroy()`, never by `release()`.

Because 0.3.1 has no module loader (ICD-0.3.0 note: `import`/`export` are `SYNTAX_ERROR`), "module-level state" in 0.3.1 reduces to any named `globalThis.*` property set by the host before execution. Real module state appears in 0.3.2/0.3.3 when kernel APIs are injected.

### Pool Sizing

- **Default:** `min(4, hardware_concurrency() / 2)`, floored at 1.
- **Override:** `pool_size` constructor argument; a value of `-1` means "use the default formula".
- **On-demand growth:** when `acquire()` is called and the free list is empty and `active_count() < pool_size`, a fresh context is created and returned. When `active_count() >= pool_size`, a context is still created on-demand but marked **transient** — `release()` on a transient context calls `destroy()` instead of returning it to the free list. There is no unbounded pool growth.

### Pool Lifecycle

Per `DESIGN-quickjs-bridge.md §3.2`:

- Pool **created** when the owning extension is installed + enabled (driven by 0.4.x — 0.3.1 only provides the pool class; wiring into extension install/enable lands later).
- Pool **destroyed** when the extension is disabled / uninstalled.
- Pool **rebuilt** (via `rebuild()`) on extension hot-reload.

In 0.3.1, with no extension installer yet, the only caller of `RuntimePool` is Catch2 tests that construct/destruct the pool directly.

---

## Resource Limits

### 1. Memory Limit

- **Mechanism:** `JS_SetMemoryLimit(rt, limits.memory_limit_bytes)` called immediately after `JS_NewRuntime` and before any `JS_Eval`.
- **Default:** **16 MiB** per runtime (matches the ICD-0.3.0 hard-coded ceiling; configurable per-extension in later milestones via `manifest.json` → `runtime.memory_limit_mb`, architecture/05-extensions.md §3.1).
- **Surfacing:** QuickJS raises `InternalError: out of memory` as a JS exception. The runtime error path converts this to `EvalError{ kind = MEMORY_LIMIT }` (carried forward from ICD-0.3.0). The 0.3.1-specific addition: when thrown during a pooled execution, the context is passed to `destroy()`, not `release()`.

### 2. CPU Time Limit

- **Mechanism:** `JS_SetInterruptHandler(rt, &interrupt_cb, ctx)`. The handler is installed at runtime creation. It fires between JS opcodes (not during C++ work) and:
  1. Checks `ctx->cancelled` — returns `1` (terminate) if set.
  2. Checks `ctx->cpu_limit_exceeded()` — returns `1` if the accumulated `cpu_time_accumulated + (now - cpu_timer_start)` exceeds `cpu_time_limit`.
  3. Returns `0` otherwise.
- **Accumulation:** the pause/resume bracket on `BridgeContext` is the accounting mechanism. In 0.3.1, there is one bracket per execution; in 0.3.3, each async gap adds a pause → co_await → resume cycle and the accumulated time skips the gap.
- **Default:** **100 ms** per execution (configurable per-extension in later milestones via `manifest.json` → `runtime.cpu_time_limit_ms`; `architecture/05-extensions.md §3.1` calls out a 5000 ms upper example — the kernel's *default* is the 100 ms figure above, and the enforcement order is "min of extension-request, admin-override, kernel-maximum" per §3.1).
- **Surfacing:** on handler-induced termination, `JS_Eval` returns an exception. The 0.3.1 layer converts it to a new error code `cpu_time_exceeded` (see Error Codes below). The context is passed to `destroy()`.

### 3. Wall-Clock Timeout

- **Mechanism:** separate from CPU time. The same interrupt handler checks `ctx->wall_clock_exceeded()` — `(steady_clock::now() - execution_start) > wall_clock_limit`.
- **Default:** **30 s** per execution (per `DESIGN-quickjs-bridge.md §4.3`).
- **Termination path:** interrupt handler returns `1` → `JS_Eval` returns an exception → `EvalError{ kind = WALL_CLOCK_EXCEEDED }` → `pool.destroy(ctx)`. **The full cancellation cascade of async-op cancellation, pending-promise rejection, and teardown ordering (`DESIGN-quickjs-bridge.md §6`) is 0.3.3 scope.** In 0.3.1, "cancellation" is the single synchronous path: the interrupt handler cuts JS, the pool destroys the context, control returns to the caller.
- **Surfacing:** returns as `wall_clock_exceeded`.

### 4. Call Depth

- **Field:** `BridgeContext::call_depth` and `BridgeContext::max_call_depth`.
- **Default max:** 8 (matches `DESIGN-quickjs-bridge.md §4.4`).
- **Enforcement:** the capability dispatcher (`src/kernel/capabilities/resolution.cpp`, landed in 0.2.2 and extended in 0.2.4) already enforces depth — see ICD-0.2.2 §Call Depth Tracking. **0.3.1 only defines the field shape on `BridgeContext` and the default**; the dispatcher keeps owning the enforcement check.
- **Why locked now:** a future 0.3.4 ICD (`cap.call()` from JS) will wire the bridge to the dispatcher. Locking the field shape in 0.3.1 prevents shape drift.

### 5. Stack Depth (QuickJS setting)

- **Mechanism:** `JS_SetMaxStackSize(rt, stack_bytes)` — a QuickJS feature that prevents C-stack overflow from deeply recursive JS.
- **Default stack ceiling:** derive from `RuntimeLimits::max_stack_depth` × a conservative per-frame estimate (implementation-defined — document the exact number in the commit that lands 0.3.1). The effective C-stack byte cap is the product.
- **Surfacing:** QuickJS raises `InternalError: stack overflow` → `EvalError{ kind = STACK_OVERFLOW }`.

---

## Error Codes

This ICD extends `plinth::js::EvalErrorKind` from ICD-0.3.0 with four new variants. The full enum after 0.3.1 is:

```cpp
enum class EvalErrorKind {
    SYNTAX_ERROR,           // from 0.3.0
    RUNTIME_ERROR,          // from 0.3.0
    MEMORY_LIMIT,           // from 0.3.0 (hard-coded ceiling); now configurable
    INTERNAL,               // from 0.3.0
    CPU_TIME_EXCEEDED,      // new in 0.3.1
    WALL_CLOCK_EXCEEDED,    // new in 0.3.1
    STACK_OVERFLOW,         // new in 0.3.1
    CANCELLED               // new in 0.3.1 (ctx->cancelled set by host)
};
```

`CANCELLED` in 0.3.1 is reachable only via direct test-driven toggling of `ctx->cancelled` — the full cancellation trigger matrix (wall-clock, node shutdown, extension disable, client disconnect per `DESIGN-quickjs-bridge.md §6.1`) is 0.3.3 scope. The 0.3.1 definition is enough for the interrupt handler and the async-bridge implementation to share one enum.

---

## Performance Targets

Measured on the CI builder image (gcc 13 / clang 18 / Linux 6.x). All targets are **hot-path after initial pool warm-up** — i.e., the pool is already populated.

- **`acquire()` from a non-empty free list:** < 1 μs (lock + vector pop + return).
- **`acquire()` with on-demand creation (pool exhausted):** ≤ 5 ms.
- **`release()` (including state reset):** < 50 μs.
- **Runtime initial creation (from `JS_NewRuntime` through all limit-setters to `JS_NewContext`):** ≤ 10 ms.

These numbers are targets for green CI; tighter budgets may land via Google Benchmark harnesses in a later re-eval, consistent with the 0.2.6.2 capability-dispatch benchmark precedent.

---

## Security Constraints (Non-Negotiable)

1. Every `JSRuntime` created by `RuntimePool` MUST have, before any JS code runs:
   - `JS_SetMemoryLimit` called with a non-zero byte count.
   - `JS_SetMaxStackSize` called with a non-zero byte count.
   - `JS_SetInterruptHandler` installed pointing at the Plinth interrupt callback.
2. Constructing a runtime with any of those three unset is a **programming error, not a fallback to an unlimited default**. The constructor MUST abort (via `CHECK_*` / `assert` / equivalent — a code-session decision) rather than silently proceed.
3. The limit-enforcement order when extensions gain configurable limits (post-0.3.1) MUST follow `architecture/05-extensions.md §3.1`: "the kernel enforces the **lowest** of: extension request, admin override, kernel maximum." Until extensions are wired in (0.4.x), only the kernel default applies.
4. `release(ctx)` on a context whose `cancelled` flag is set, or whose last execution produced any `*_EXCEEDED` error, MUST be a programmer-visible bug — the caller should have used `destroy(ctx)`. Defensive behavior: `release()` detects this and routes to `destroy()` internally. Code-session decision: log a warning or trigger a `DCHECK`, whichever the project's logging/assertion conventions prefer. **Test (0.3.3.3):** `tests/kernel/js/runtime_pool_test.cpp → "RuntimePool release on cancelled context routes to destroy"` captures the defensive-destroy warn line and asserts slot teardown.

---

## What Must Not Be Decided Yet

- **Async bridge — `drogon::Task<>` interop, promise↔coroutine plumbing, `JS_ExecutePendingJob` loop, async op queue, concurrent-op limits, result-size caps.** All 0.3.3 scope. 0.3.1 MUST NOT introduce a coroutine loop, an `AsyncOp` struct, or a promise-resolution helper.
- **Full cancellation cascade per `DESIGN-quickjs-bridge.md §6`** — the teardown-order steps 1–6, the 5-second pending-op abandon window, the recursive cancellation of downstream contexts. 0.3.1 defines only the `cancelled` flag + interrupt-handler read path.
- **Kernel API injection.** `log.*`, `config.*`, `crypto.*` land in ICD-0.3.2. 0.3.1 does not register any host functions.
- **Extension supervision** (`architecture/05-extensions.md §3.2` — N-failures-in-M-minutes auto-disable). Depends on extension install/enable (0.4.x) + audit (already live in 0.1.7). 0.3.1 provides the destroy-on-failure primitive; the counter + auto-disable are wired later.
- **Module-state heap fragmentation mitigation** (`DESIGN-quickjs-bridge.md §11` open question 3). Not decided in 0.3.1; rely on the memory limit.
- **Runtime pool sizing re-tune** (`DESIGN-quickjs-bridge.md §11` open question 1). The 0.3.1 default matches the design doc; benchmark-driven re-tuning happens at the re-eval following 0.3.3.

---

## Milestone Criteria

All five test groups below MUST pass under Catch2 before 0.3.1 ships. They mirror the bullets in `DESIGN-quickjs-bridge.md §9.1`.

### Tests

1. **Pool acquire / release / reuse.** Acquire `N = pool_size` contexts, release them all, acquire again: the second round returns contexts from the free list (pointer identity or a test-only instrumentation counter proves reuse).
2. **Pool on-demand creation + transient destroy.** With `pool_size = 2`, acquire 3 contexts; confirm the third is created on-demand. Release all three; confirm the first two return to the free list and the third is destroyed (free-list size is `2` after the round, not `3`).
3. **Memory limit enforced.** Eval JS that allocates past `memory_limit_bytes`; result is `EvalError{ kind = MEMORY_LIMIT }`; pool's `active_count()` is unchanged after `destroy()`; ASAN reports no leak.
4. **CPU time enforced — and independent of wall-clock.** Eval `while(true) {}` with `cpu_time_limit = 50 ms`; terminates within 50 ms ± a small margin (target: < 100 ms observed wall time). Separately, eval code that sleeps via a synchronous host-exposed `__host_sleep_ms__` test-only function (NOT part of the 0.3.2 kernel API surface — lives under a test-only compile flag) to prove that wall-clock-but-not-CPU workloads do not trip the CPU limit. (This second sub-test is what the 0.3.3 async bridge will replace with a real async gap.)
5. **Stack depth enforced.** Eval deeply recursive JS (`function f(){f()} f()`); result is `EvalError{ kind = STACK_OVERFLOW }`; no process crash; pool state consistent after `destroy()`.

### CI Wiring

- `runtime_pool.{hpp,cpp}` and `bridge_context.{hpp,cpp}` added under `src/kernel/js/`.
- Test file at `tests/kernel/js/runtime_pool_test.cpp` registered in the existing Catch2 executable.
- No new CI job.
- The test-only synchronous sleep shim (test group 4, second sub-test) lives at `tests/kernel/js/test_host_sleep.cpp` behind a `PLINTH_JS_TEST_SHIMS` compile define — it MUST NOT leak into production builds.
