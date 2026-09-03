# ICD-0.3.5-runtime-hardening

**Traces to:** DESIGN-quickjs-bridge.md §4.1 (Memory Limit — "result size limit" clause), §4.2 (CPU Time Limit), §4.3 (Wall-Clock Timeout), §6.2 (Cancellation Procedure), §7.2 (Max Concurrent Async Operations), §9.1 (Implementation Sequence: 0.3.5 adversarial test list)
**Depends on:** ICD-0.3.1-runtime-lifecycle (interrupt handler, `JS_SetMemoryLimit`, `JS_SetMaxStackSize`, `EvalErrorKind::{CPU_TIME_EXCEEDED, WALL_CLOCK_EXCEEDED, STACK_OVERFLOW, CANCELLED}`), ICD-0.3.3-async-bridge (`async_result_size_limit_bytes` field reserved for this milestone; AsyncOp dispatch loop; cancellation cascade), ICD-0.3.4-cap-call-from-js (`cap.*` surface — two of the 11 DESIGN adversarial cases exercise it)
**Milestone:** 0.3.5 — Adversarial hardening test battery + the one enforcement gap ICD-0.3.3 reserved for this milestone.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `project_ws_flaky_segfault.md` (diagnostic handler pattern reusable here if cancellation teardown surfaces any new signal); `0.5.x` ROADMAP TSan CI job (F.23 from ICD-0.3.3 lands there, not here); DEFERRED.md (no active entry folded into this ICD — the one 0.3.3-reserved enforcement lands here rather than deferring further).

---

## Overview

Despite the ROADMAP name "Memory + CPU time limit enforcement,
interrupt handler," 0.3.1 already shipped the **mechanisms** for
memory, CPU, wall-clock, and stack enforcement (see §Inherited
Mechanisms below). 0.3.5 has two jobs:

1. **Adversarial hardening.** DESIGN §9.1 enumerates 9 adversarial
   JS programs designed to break the bridge. This ICD pins each
   case to a test, an expected failure classification, and a target
   test file. Infrastructure to pass these tests already exists
   (0.3.1 + 0.3.3 + 0.3.4) — 0.3.5 writes the test cases that prove
   it.
2. **Close the one enforcement gap ICD-0.3.3 reserved for this
   milestone.** `RuntimeLimits::async_result_size_limit_bytes`
   (default 16 MiB) ships in 0.3.3 and is copied onto every
   BridgeContext, but **never consulted**. ICD-0.3.3 §Resource
   Limits item 1 and §What Must Not Be Decided Yet both name 0.3.5
   as the closure venue. 0.3.5 adds the measurement and rejection
   path inside the per-op outcome helpers.

**Scope:**

- New `EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED` variant in
  `eval.hpp`.
- Result-size measurement + rejection in `run_db_query_outcome`,
  `run_db_exec_outcome`, `run_audit_write_outcome`, and the new 0.3.4
  `CAP_CALL` outcome path. Reject with code
  `async.result_size_exceeded` when a per-op `Json::Value` result
  would exceed `bc.async_result_size_limit_bytes`.
- 11 adversarial test cases split across two new test files — see
  §Adversarial Test Battery.

**Out of scope (deferred):**

- **Per-namespace result-size limits.** The single
  `async_result_size_limit_bytes` field applies uniformly to every
  async op. A per-namespace override (`db.async_result_size_limit_bytes`
  vs `cap.async_result_size_limit_bytes`) adds surface area without
  a concrete requirement. If a real caller demands different caps
  for different namespaces in 0.5.x realtime work, split there.
- **Streaming results.** `db.stream` / `cap.stream` are separate
  APIs per DESIGN; they MUST NOT be built as "result-size-limit-
  bypass" shims to this milestone.
- **Truncation instead of rejection.** Result over limit is an error,
  not a silent truncation. Truncation would hide a correctness
  signal the caller needs to see.
- **F.23 TSan smoke from ICD-0.3.3.** Still deferred to the 0.5.x
  TSan CI job; not re-scheduled here.
- **Extension-level policy knobs.** Tuning `max_stack_depth`,
  `memory_limit_bytes`, `cpu_time_limit`, `wall_clock_limit`,
  `async_result_size_limit_bytes`, `max_concurrent_async_ops`
  per-extension lands with 0.4.x's manifest parser, not here.

---

## Inherited Mechanisms

Normative reference; not re-specified. Updates to any of these go
through the owning ICD, not this one.

| Mechanism | ICD | File:symbol | Status in 0.3.4 |
|---|---|---|---|
| Memory limit (`JS_SetMemoryLimit`) | ICD-0.3.1 §Resource Limits | `runtime_pool.cpp` — `JS_SetMemoryLimit(rt, limits.memory_limit_bytes)` inside `create_entry` | enforced |
| Stack depth (`JS_SetMaxStackSize`) | ICD-0.3.1 | `runtime_pool.cpp` — `JS_SetMaxStackSize(rt, STACK_BYTES_PER_FRAME * max_stack_depth)` | enforced |
| CPU-time interrupt | ICD-0.3.1 | `runtime_pool.cpp` — `plinth_js_interrupt_cb` + `bc.cpu_limit_exceeded()` | enforced |
| Wall-clock interrupt | ICD-0.3.1 | `plinth_js_interrupt_cb` + `bc.wall_clock_exceeded()` + `run_on_context.cpp` loop check | enforced |
| Cancellation flag | ICD-0.3.1 | `bc.cancelled.load()` checked in interrupt callback + `run_on_context` loop | enforced |
| Concurrent-async-op back-pressure | ICD-0.3.3 §Back-Pressure | `run_on_context.cpp` — `dispatch_ops_batch_fanout` re-queues on saturation | enforced |
| Cancellation cascade (6-step per DESIGN §6.2) | ICD-0.3.3 §Cancellation Cascade + 0.3.3.1 amendment | `run_on_context.cpp` — `run_cancellation_cascade` + `AnyCompletionAwaiter` drain | enforced |
| Top-level promise-rejection classifier | ICD-0.3.3.3 | `conversion.{hpp,cpp}` — `detail::classify_rejection` | enforced |
| `async_result_size_limit_bytes` carry | ICD-0.3.3 | `runtime_pool.hpp` / `bridge_context.hpp` — field present, not consulted | **0.3.5 closes** |
| Call-depth limit | ICD-0.2.2 §Call Depth Tracking | `resolution.cpp` — dispatcher enforces | enforced (bridge forwards `bc.call_depth`) |

---

## New Enforcement: Result-Size Cap

### Semantics

For every completed `AsyncOp` whose outcome is a successful
`Json::Value` result, the bridge measures a serialized size
before conversion to JS and rejects the promise if the size
exceeds `bc.async_result_size_limit_bytes`.

- **Measurement primitive.** `Json::FastWriter` (or equivalent) on
  the `Json::Value` outcome. This is not free (an extra serialization
  pass) but is single-digit microseconds for sub-MiB results and
  the only robust way to reason about "size" for arbitrary JSON
  before it lands in the JS heap.
- **Threshold inclusive-exceed.** `measured_bytes > bc.async_result_size_limit_bytes`
  rejects. Exactly-at-limit resolves. The default limit is 16 MiB;
  tests use smaller values for fast adversarial checks.
- **Rejection shape.**
  ```
  {code: "async.result_size_exceeded",
   message: "result size <N> bytes exceeds limit <L> bytes"}
  ```
  The `PromiseRejection::sqlstate` remains `std::nullopt` (same as
  `cap.*` and `audit.*` rejections).
- **Measurement runs in the detached task**, before the
  `queueInLoop`-landed `bc.resolve` call. This keeps the main loop
  unblocked even for marginal-size results.

### Where It Applies

Every `run_*_outcome` helper in `run_on_context.cpp`:

- `run_db_query_outcome` — measured on the full `{rows, row_count}`
  envelope.
- `run_db_exec_outcome` — practically never exceeds (single
  `{row_count}` object), but kept consistent for uniformity.
- `run_audit_write_outcome` — resolved value is `undefined` (size 0
  after conversion); the check is a no-op, included for uniformity.
- `run_cap_call_outcome` (new in 0.3.4) — measured on
  `CapabilityResult::data`. This is the critical path for adversarial
  cases G.30 and G.31 of the test battery.

**Implementation deviation (0.3.5 placement).** The shipped code
places the `check_result_size` call **once at
`dispatch_async_op_detached`'s success-outcome fan-in** rather than
in each of the four `run_*_outcome` helpers. Rationale: the
measurement runs in the detached task before `queueInLoop`-landed
`bc.resolve` fires (ICD §Semantics point 4); placing it at the single
fan-in covers all four arms uniformly with identical observable
behavior and no duplication. The ICD's per-helper enumeration remains
the authoritative list of *where the check applies*, but the *call
site* is a single helper. Ratified via the 0.3.5 CHANGELOG "Accepted
deviations" item 2 on the same footing as 0.2.0 / 0.2.2 / 0.3.1 /
0.3.2 precedents. Absorbed into this ICD by
`RE-EVAL-0.3.x-arc-closeout.md §2.3`.

### New EvalErrorKind Variant

```cpp
// src/kernel/js/eval.hpp
enum class EvalErrorKind : std::uint8_t {
    // ... 0.3.0 / 0.3.1 / 0.3.3 variants ...
    ASYNC_RESULT_SIZE_EXCEEDED,   // new in 0.3.5
};
```

Surfaces only when an adversarial-wrapped top-level promise carries
a rejected inner with `code = "async.result_size_exceeded"` and no
handler catches it — the 0.3.3.3 `classify_rejection` path adds the
new variant alongside `MEMORY_LIMIT` et al. The helper's switch gets
one new case mapping the rejection code to the new kind.

---

## Adversarial Test Battery

Each case from DESIGN §9.1 is mapped to a test ID, setup, expected
classification, and target file. Tests live in two new files to
keep the existing `async_bridge_test.cpp` focused on the ICD-0.3.3
Groups A–F and the ICD-0.3.4 Groups G–M:

- `tests/kernel/js/limits_test.cpp` — memory / CPU / stack / wall-clock
  pressure cases.
- `tests/kernel/js/async_hardening_test.cpp` — async-specific pressure
  cases (result-size, `Promise.all` fan-out burst, in-flight
  cancellation, promise-in-catch, non-serializable return, nested
  `cap.call` depth chain).

| # | DESIGN §9.1 bullet | Test ID | File | Setup | Expected |
|---|---|---|---|---|---|
| 1 | "Allocate until memory limit, then `await`, then allocate more → limit still enforced" | N.37 | `limits_test.cpp` | 4 MiB memory_limit; pre-allocate ~3 MiB of arrays; `await db.query('SELECT 1')`; continue allocating | `EvalError{kind: MEMORY_LIMIT}` — proves 0.3.3.3 classifier path (between-await) |
| 2 | "Infinite loop inside a `.then()` callback → CPU limit fires" | N.38 | `limits_test.cpp` | 50 ms cpu_time_limit / 5 s wall; `db.query(...).then(() => { while(1); })` | `EvalError{kind: CPU_TIME_EXCEEDED}` — proves CPU interrupt during `.then` |
| 3 | "`Promise.all` with 10,000 entries → concurrent op limit prevents PG pool exhaustion" | N.39 | `async_hardening_test.cpp` | 100 synthetic `cap.call` entries against a fast Tier 1 stub; `max_concurrent_async_ops=8` | All 100 resolve; in-bridge back-pressure counter peaks ≤ 8 (instrumented via same hook as ICD-0.3.3 B.11). Ten-thousand is CI-prohibitive; 100 is the same correctness property with realistic overhead. |
| 4 | "Create promises in a loop without awaiting → memory limit fires" | N.40 | `limits_test.cpp` | 4 MiB memory_limit; `for(let i=0;i<1e6;i++) new Promise(() => {})` — no awaits | `EvalError{kind: MEMORY_LIMIT}` — proves promise-capability-allocation path counts toward JS heap |
| 5 | "Throw exception inside promise `.catch()` → doesn't crash bridge" | N.41 | `async_hardening_test.cpp` | `db.query('BAD').catch(e => { throw new Error("nope"); })` | `EvalError{kind: PROMISE_REJECTED_UNHANDLED}` with message containing `"nope"`; no ASAN / crash. Proves cascade / teardown robustness under nested throws. |
| 6 | "Return non-serializable value from handler → bridge returns error, doesn't crash" | N.42 | `async_hardening_test.cpp` | JS handler returns a value containing a cycle (`let x={}; x.self=x; return x;`); outer driver `co_await`s the result | `EvalError{kind: INTERNAL}` with message indicating serialization failure. The `js_to_json` helper rejects cycles today (verify behavior; if not, add the check); test proves the rejection is clean (no use-after-free, no hang). |
| 7 | "`eval()` attempt (disabled by default) → error, not execution" | N.43 | `limits_test.cpp` | `eval("1+1")` in script source | QuickJS may expose `eval` by default; 0.3.5 verifies the current posture and decides: either (a) delete the global if disabled-by-default is the intent, or (b) document the current posture in this test as the baseline and add disabling as a separate `[strong]` ROADMAP item. **Preferred:** delete `globalThis.eval` / `globalThis.Function` in `inject_kernel_stdlib` to match DESIGN. The test asserts `typeof globalThis.eval === 'undefined'` + `eval("1+1")` throws `ReferenceError`. |
| 8 | "Deeply nested `cap.call` chains → depth limit enforced" | N.44 | `async_hardening_test.cpp` | Tier 1 stub that calls `cap.call(...)` recursively; driver invokes with `bc.call_depth=0`; chain depth grows one per hop | Rejected with `cap.call_depth_exceeded` once `call_depth >= MAX_CALL_DEPTH`. **Note:** requires a Tier 1 stub that itself recurses — this is a test-only stub registered via `register_tier1_handler`, NOT a production kernel capability. 0.4.x's installer enables the same test with real JS-extension recursion. |
| 9 | "Wall-clock timeout with 100 pending async ops → all cancelled cleanly" | N.45 | `async_hardening_test.cpp` | 100 × `pg_sleep(5)` via `Promise.all`; `wall_clock_limit = 200 ms` | `EvalError{kind: WALL_CLOCK_EXCEEDED}`; cascade runs in ≤ 5 s; ASAN clean; no `PROMISE_RESOLVE_AFTER_CANCEL`. |
| 10 | (N.46) **Result size exceeded — explicit — no DESIGN bullet but the enforcement close-out needs direct coverage** | N.46 | `async_hardening_test.cpp` | `bc.async_result_size_limit_bytes = 4096`; `await db.query("SELECT string_agg(...) WHERE …")` returning > 4 KiB | Promise rejects with `{code: "async.result_size_exceeded"}`; top-level classifier surfaces `EvalError{kind: ASYNC_RESULT_SIZE_EXCEEDED}`. Result-size enforcement proof. |
| 11 | (N.47) **Result size under limit — boundary** | N.47 | `async_hardening_test.cpp` | Same as N.46 but returning exactly-at-limit bytes | Resolves cleanly. Proves the `>` vs `≥` semantic and that measurement overhead doesn't falsely reject. |

Bullet 10 and 11 in the table are not enumerated in DESIGN §9.1 but
are inseparable from the result-size enforcement that is this ICD's
other deliverable; folding them into the same test file keeps the
adversarial suite contiguous.

**Case count:** 11 test cases — N.37 through N.47. DESIGN §9.1's
9 bullets map 1:1 to N.37–N.45; N.46 and N.47 are added for the
result-size enforcement close-out (rejection + boundary) that is
this ICD's other deliverable. Numbering is contiguous with the
0.3.3 / 0.3.4 case IDs (A.1–F.23, G.24–M.36, now N.37–N.47).

---

## Security Constraints (Non-Negotiable)

1. **Result-size check runs before JS receives the value.**
   Oversized results never cross into the JS heap. This is defense
   in depth against OOM from a runaway query or capability return;
   the JS memory limit alone is insufficient because `Json::Value`
   → JS conversion allocates transiently inside QuickJS.
2. **Interrupt-handler precedence is unchanged.** The order
   `cancelled → cpu → wall-clock` from ICD-0.3.1's
   `plinth_js_interrupt_cb` is NOT modified. The adversarial tests
   simultaneously trip two conditions (cancel + cpu in N.38;
   wall-clock + cancel in N.45) and assert the documented
   precedence.
3. **Truncation is not an option.** A result over limit rejects;
   the caller sees the failure. Silent truncation would mask a
   correctness signal and is explicitly forbidden.
4. **No new enforcement beyond result-size.** 0.3.5 does NOT add
   any `allocation rate limit`, `per-tick quantum`, `cooperative
   yield`, or similar — those are out-of-scope and would require
   their own ICD.
5. **`eval` / `Function` disabling (N.43).** If the architect
   ratifies disabling these at pool-entry creation, the deletion
   MUST run BEFORE `inject_kernel_stdlib` — the stdlib may rely on
   internal `eval` access during its own setup. If the architect
   ratifies keeping `eval` enabled, this constraint is void and
   N.43 becomes a baseline-documenting test instead of an enforcement
   test. Ratification decision is in-scope for this ICD's PR; see
   §Open Questions below.

---

## Open Questions (resolve during implementation)

1. **`eval` / `Function` posture (N.43).** Delete from `globalThis`
   in `inject_kernel_stdlib`, or document the current quickjs-ng
   default? DESIGN §9.1 says "disabled by default" — reading this as
   prescriptive, the default is to delete. Flag for architect
   ratification in the PR description.
   **Resolved (0.3.5 PR merge 2026-04-19):** deletion posture
   shipped. `disable_dynamic_code_entrypoints` deletes
   `globalThis.eval` and `globalThis.Function` at the top of
   `inject_kernel_stdlib` (both initial `create_entry` and
   post-`clear_global_own_props` `release()` paths). Architect
   ratification was tacit via PR merge without amendment.
   Cross-referenced in RE-EVAL-0.3.x-arc-closeout §2.7.
2. **`Json::FastWriter` overhead.** The result-size measurement
   adds a serialization pass per async op. For large results this
   doubles the conversion cost. If benchmarks in the PR show a
   regression on the 0.2.6.2 reference line, switch to a
   size-estimating visitor (visits without allocating the string).
   Implementation latitude; test suite asserts observable behavior,
   not the measurement primitive.

---

## Milestone Criteria

All 11 cases above MUST pass under Catch2 default and sanitizer
builds. Existing Groups A–M from ICDs 0.3.3 + 0.3.4 remain green
(no regression).

### CI Wiring

- `src/kernel/js/eval.hpp` — add `ASYNC_RESULT_SIZE_EXCEEDED` enum
  variant.
- `src/kernel/js/run_on_context.cpp` — add the result-size check in
  each `run_*_outcome` helper (3 existing, 1 added in 0.3.4);
  extend `detail::classify_rejection` to map
  `"async.result_size_exceeded"` → the new kind.
- `src/kernel/js/stdlib_inject.cpp` — **if** the architect ratifies
  N.43's eval-deletion, add a 2-line step to delete
  `globalThis.eval` and `globalThis.Function` before per-namespace
  registrars run.
- `tests/kernel/js/limits_test.cpp` — **new**. 4 cases (N.37, N.38,
  N.40, N.43).
- `tests/kernel/js/async_hardening_test.cpp` — **new**. 7 cases
  (N.39, N.41, N.42, N.44, N.45, N.46, N.47).
- No CMake option change. No new CI job. `KERNEL_SOURCES` glob
  (widened in 0.3.3.2 to cover `tests/kernel/**`) captures the new
  test files without edits.
- PG-gated cases (N.37, N.38, N.39 if using real PG, N.45, N.46,
  N.47) skip cleanly when PG env vars are unset, following the
  existing `async_bridge_test.cpp` gating pattern.

---

## Entry / Exit

**Entry:** ICD-0.3.4 implementation merged (`cap.*` surface + CAP_CALL
dispatch arm + `bc.user`). Without 0.3.4, N.44 (`cap.call` depth
chain) and result-size coverage of the CAP_CALL outcome path are
infeasible.

**Exit:** N.37–N.47 all pass under Catch2 default and sanitizer
builds; `run-clang-tidy-20` clean on the touched files; CHANGELOG
entry describes the 11 cases + the result-size enforcement close-
out + the `eval`/`Function` ratification outcome; DEFERRED.md
updated with any deviation that warrants tracking; ROADMAP `0.3.5`
line removed.
