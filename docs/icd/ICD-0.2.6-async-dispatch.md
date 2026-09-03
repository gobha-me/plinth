# ICD-0.2.6-async-dispatch

**Traces to:** ICD-0.2.2-capability-resolution.md §Dispatch Contract (retires the "Implementation deviation (0.2.2 → 0.2.6)" sub-section)
**Depends on:** ICD-0.2.2-capability-resolution (the synchronous `call_capability` this ICD wraps), ICD-0.2.5 (`batch_call_capability`, for the matching async wrapper)
**Milestone:** 0.2.6 — Async dispatch wrapper (`drogon::Task<>` / coroutine signature)
**Status:** Ready for implementation — lands alongside 0.3.3
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** ICD-0.3.3-async-bridge (the first real coroutine caller — consumes `call_capability_async`), DESIGN-quickjs-bridge.md §8.1 (Capability Dispatch)

---

## Overview

This ICD defines the **async (coroutine) wrapper** around the synchronous `call_capability` / `batch_call_capability` dispatch entry points shipped in 0.2.2 / 0.2.5. It is a strict additive surface — no behaviour change, no new failure modes, no new audit events. The goal is to give coroutine-shaped callers (the QuickJS async bridge, 0.3.3) a `co_await`-compatible entry point while leaving the synchronous kernel callers unchanged.

Per ROADMAP.md:55, 0.2.6 has been "triggered by the first real coroutine caller — expected alongside 0.3.3 (QuickJS async bridge)." This ICD makes that trigger concrete: 0.2.6 ships in the same PR as 0.3.3 implementation (not the 0.3.2.1 ICD-authoring PR).

**Scope:** two coroutine wrappers, two test cases, one retirement note.

**Not in scope:** any change to the dispatch hot path, Tier 1/2/3 resolution logic, RBAC check, audit emission, or call-depth tracking. Those all stay where they live (resolution.cpp / batch.cpp) with their current synchronous shape. This ICD adds a thin outer layer.

---

## Contract

### Single dispatch

```cpp
// src/kernel/capabilities/resolution.hpp
namespace plinth::capabilities {

drogon::Task<ResolveResult> call_capability_async(
    const CapabilityCall& call,
    const UserContext& ctx);

}  // namespace plinth::capabilities
```

Body:

```cpp
drogon::Task<ResolveResult> call_capability_async(
    const CapabilityCall& call,
    const UserContext& ctx) {
    co_return call_capability(call, ctx);
}
```

That is the entire implementation. `call_capability` itself is unchanged. The `co_return` here is legal because `ResolveResult` (an alias for `std::expected<CapabilityResult, CapabilityError>`) is trivially convertible to `drogon::Task<ResolveResult>`'s awaited value.

### Batch dispatch

```cpp
// src/kernel/capabilities/batch.hpp
namespace plinth::capabilities {

drogon::Task<BatchResult> batch_call_capability_async(
    const std::vector<CapabilityCall>& calls,
    const UserContext& ctx);

}  // namespace plinth::capabilities
```

Body:

```cpp
drogon::Task<BatchResult> batch_call_capability_async(
    const std::vector<CapabilityCall>& calls,
    const UserContext& ctx) {
    co_return batch_call_capability(calls, ctx);
}
```

Same pattern, same rules. `BatchResult` semantics (Promise.all-style fan-out, `failed_index` on fail-fast, `values` populated on success) are unchanged from ICD-0.2.5.

---

## Why This Is Just a Wrapper (Not a Real Coroutine)

The synchronous `call_capability` does three things:

1. Parse + validate the signature (in-memory).
2. RBAC check (in-memory — the user's effective rules live on `UserContext`).
3. Tier 1 (in-memory map) or Tier 2 (in-memory cache) dispatch.

None of those steps suspend. There is no I/O boundary that benefits from `co_await` today. Tier 3 (remote proxy) is the only step that would need real async, and it's not yet implemented (0.8.x). Until then, the async wrapper is a shape-change only: callers can `co_await` it, but the body completes without yielding the event loop.

When Tier 3 lands (0.8.x) — or when an RBAC-check DB fallback is introduced (currently explicitly rejected per ICD-0.2.4 §Accepted deviation (a)) — the wrapper's body will grow real `co_await`s. That growth is a new milestone, not a deviation from this ICD. This ICD locks the **signature**; the body is an implementation detail that may deepen over time.

---

## Caller Expectations

- JS async bridge (ICD-0.3.3, `run_on_context`): calls `call_capability_async` when the AsyncOp::CAP_CALL variant is dispatched. (In 0.3.3, `CAP_CALL` is reserved-but-not-implemented, so this call doesn't actually happen yet — but the wrapper is present so the dispatch switch in the coroutine loop compiles without `#ifdef` cruft.)
- Kernel sync callers (anything currently calling `call_capability` directly in `src/kernel/**`): stay on the synchronous form. No migration, no deprecation. Both entry points coexist permanently.
- Future coroutine-shaped callers (ICD-0.3.4 `cap.call` wiring, ICD-0.5.x pub/sub handlers, ICD-0.8.x sidecar proxy): all use `call_capability_async`.

This dual-entry pattern is expected to be the long-term shape. There is no plan to retire the synchronous `call_capability`.

---

## Error Semantics

Identical to ICD-0.2.2 / ICD-0.2.4 / ICD-0.2.5:

- Capability-not-found → `CapabilityError::CAPABILITY_NOT_FOUND`
- Invalid signature → `CapabilityError::INVALID_CAPABILITY`
- RBAC denial → `CapabilityError::PERMISSION_DENIED` (with audit event emitted by the synchronous path)
- Call-depth exceeded → `CapabilityError::CALL_DEPTH_EXCEEDED`
- Tier 3 path (sidecar / extension provider) → `CapabilityError::TIER3_NOT_AVAILABLE` until the relevant milestone lands
- Handler-raised errors → `CapabilityError::HANDLER_FAILED` with the handler's message
- Tier 1 not-implemented stub → `CapabilityError::NOT_IMPLEMENTED`

Batch errors: `BatchResult::error` + `BatchResult::failed_index` semantics per ICD-0.2.5 §BatchResult.

---

## Tests

Added to existing files, no new test file:

### In `tests/kernel/capabilities/resolution_test.cpp`

1. **Sync-async parity.** Run a Tier 1 dispatch through `call_capability` and the same call through `call_capability_async` (driven via a minimal coroutine harness — either `drogon::sync_wait`-equivalent or a Catch2 fixture that spins up a tiny event loop); assert byte-identical `ResolveResult`. Exercised for: Tier 1 hit, Tier 2 hit, RBAC deny, capability-not-found, call-depth-exceeded. Five sub-assertions.
2. **Coroutine-driver smoke.** Call `call_capability_async` three times in a row within a single `drogon::Task<void>` harness (three sequential `co_await`s); assert all three complete and return the expected values. Proves the wrapper composes in a coroutine context.

### In `tests/kernel/capabilities/batch_test.cpp`

3. **Batch sync-async parity.** Same pattern for `batch_call_capability_async`: one success case (three-call batch, all resolve), one fail-fast case (middle call fails → `failed_index == 1`, `values` empty), one empty-input case (`calls == {}` → `BatchResult{.values = {}}`). Three sub-assertions.

Total: three new Catch2 cases, covering the two-wrapper contract.

---

## What Must Not Be Decided Yet

- **Real async dispatch inside the wrapper body.** The body is `co_return sync_impl(...)` in 0.2.6 and stays that way until a real suspension point is added. Adding one is a new milestone with its own ICD. Inlining Tier 3 remote dispatch (when 0.8.x lands) is the first expected expansion.
- **Coroutine allocator / promise-type customization.** `drogon::Task<>` is the chosen coroutine type — no custom `promise_type` machinery in 0.2.6. DESIGN-quickjs-bridge.md §2 picks Drogon's coroutine shape; this ICD follows.
- **Retirement of synchronous `call_capability`.** Not planned. Both entry points coexist.
- **Async variant of any `capabilities/` helper beyond the two dispatch entry points.** `register_tier1_handler`, `upsert_tier2_entry`, etc. stay synchronous — they're mutation helpers on in-memory maps, not dispatch.
- **Thread-affinity decisions for the coroutine.** Drogon resumes coroutines on event-loop threads; the sync body touches no thread-local state; no affinity constraint. This matches the DESIGN §11 Q#4 resolution-by-construction in ICD-0.3.3.
- **Fallback sync dispatch when the async caller is on a non-Drogon thread.** Not supported. Async callers MUST run inside a Drogon-managed coroutine context.

---

## Retirement of the 0.2.2 Deviation

ICD-0.2.2 §Implementation deviation (currently at lines 55–69) is deleted and replaced with a single-line pointer to this ICD:

> **Async dispatch wrapper:** see `ICD-0.2.6-async-dispatch.md` for the coroutine-shaped entry point consumed by 0.3.3+. The synchronous `call_capability` form defined above remains the primary implementation; the async wrapper composes on top of it.

The re-statement of the deviation at the top of `src/kernel/capabilities/resolution.hpp` (the comment block at lines 14–21 per memory) is deleted in the 0.2.6 implementation commit.

---

## Entry / Exit

**Entry:** 0.2.2 + 0.2.4 + 0.2.5 implementations live on main (already the case as of v0.3.2); Drogon's `drogon::Task<>` surface is available (it is — Drogon has been linked since 0.1.0).

**Exit:** both wrappers compile, three new Catch2 cases pass, ICD-0.2.2 deviation section retired, `src/kernel/capabilities/resolution.hpp` header deviation note removed, CHANGELOG entry for 0.3.3 includes the 0.2.6 landing (shipped alongside, not separately tagged).
