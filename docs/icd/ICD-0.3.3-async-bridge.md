# ICD-0.3.3-async-bridge

**Traces to:** DESIGN-quickjs-bridge.md §§3 (Execution Model), 4 (Resource Limits), 5 (Error Propagation), 6 (Cancellation and Cleanup), 7 (Concurrency Within an Extension), 8.1 (Capability Dispatch — DB path), 9.3 (Implementation Sequence: Async Bridge), 10 (Data Structures); architecture/05-extensions.md §3 (QuickJS Runtime and Extension Supervision)
**Depends on:** ICD-0.3.1-runtime-lifecycle (`BridgeContext` field shape, `RuntimePool`, `EvalErrorKind`, interrupt handler), ICD-0.3.2-kernel-stdlib-sync (`inject_kernel_stdlib` seam, `JS_SetContextOpaque` convention, `ConfigProjection`), ICD-0.2.6-async-dispatch (coroutine dispatch wrapper — the first caller lives here), ICD-0.1.7-audit (`plinth::log::audit` writer + `g_audit_ready` gate, canonical audit event catalog), ICD-0.2.4-capability-rbac (`g_audit_ready` gating precedent)
**Milestone:** 0.3.3 — Async bridge: C++20 coroutines ↔ JS promises, plus `db.*` and `audit.*` JS bindings
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DESIGN-logging-subsystem.md (spdlog async audit path this ICD's `audit.log` reuses), DESIGN-quickjs-bridge.md §11 (open questions 2 and 4 — PG pool sizing and thread affinity — deferred to implementation per the 0.2.2 precedent), DISCUSSION-streaming-and-media.md §0 (must preserve `plinth.call()` return-value opacity — `db.query` MUST NOT introduce a narrowed envelope that forecloses later streaming shapes; this ICD does NOT place anything on a JS-visible `plinth` or `cap` global — those are 0.3.4 scope)

---

## Overview

This ICD defines the **async bridge** for Plinth's QuickJS integration: the coroutine dispatch loop that translates JS `await` into Drogon `co_await`, the JS promise↔C++ callback plumbing, and the first two async kernel-API namespaces — `db.*` and `audit.*`.

0.3.3 activates the fields ICD-0.3.1 reserved on `BridgeContext` (`pending_ops`, `callbacks`, `next_callback_id`, `concurrent_async_ops`, `max_concurrent_async_ops`) and lands the `drogon::Task<EvalResult> run_on_context(BridgeContext&, std::string_view)` entry point that pooled execution flows through. The synchronous `plinth::js::eval()` path from ICD-0.3.0 and the pool acquire/release/destroy contract from ICD-0.3.1 are unchanged; this ICD is additive on top.

**Scope:**
- Coroutine dispatch loop (`run_on_context`) with pause/resume CPU-timer bracketing across `co_await` gaps.
- `AsyncOp` struct + `pending_ops` queue wiring, restricted in 0.3.3 to the `DB_QUERY`, `DB_EXEC`, `AUDIT_WRITE` op types. The enum variants `HTTP_REQUEST`, `CAP_CALL`, `STORAGE_GET`, `STORAGE_PUT`, `PUBSUB_PUBLISH` from `DESIGN-quickjs-bridge.md §10` are **reserved on the enum** but their dispatch arms are not implemented — they land in 0.3.4 and later arcs.
- `db.query(sql, params?)` and `db.exec(sql, params?, opts?)` JS bindings via Drogon's async PG client.
- `audit.log(event_type, payload)` JS binding routing through `plinth::log::audit` gated on `g_audit_ready`.
- Full cancellation cascade per DESIGN §6.3 (signal → cancel pending async ops → reject remaining JS promises → `JS_ExecutePendingJob` flush → `RuntimePool::destroy`).
- Per-execution concurrent-async-op limit (back-pressure).
- Four new `EvalErrorKind` variants for async-specific failure modes.

**Out of scope (deferred):**
- `cap.*` JS surface (`cap.call`, `cap.batch`) — 0.3.4.
- `pubsub.*`, `storage.*`, `http.*` — 0.5.x, 0.10.x.
- Adversarial hardening and stress suite — 0.3.5.
- Parallel optimization of `cap.batch` / `Promise.all([cap.call(...)])` below the naive fan-out — deferred to benchmark-driven re-eval.
- Result-size caps per async op (DESIGN §4.1 "result size limit") — introduced as a configurable on `RuntimeLimits` but given a permissive default (16 MiB) and NOT used to reject rows in 0.3.3; enforcement behavior is revisited alongside 0.3.5 hardening.
- Any DESIGN-addendum-level design for `db.*` error taxonomy beyond what this ICD pins. If a real caller in 0.3.4+ needs richer SQLSTATE mapping, a delta ICD patches this contract.

---

## BridgeContext Async Activation

Fields reserved-but-dormant in ICD-0.3.1 (`bridge_context.hpp` lines 50–55) become populated and active in 0.3.3. No new struct field is introduced.

> **0.3.3.1 amendment.** 0.3.3.1 adds four parallel-dispatch signaling fields (`inflight_detached`, `wake_mu`, `wake_count`, `waiter_handle`) and a `signal_completion()` method to `BridgeContext`, and extends `PromiseCallbacks` with a third field `ns_for_cancellation`. See §Implementation deviation (0.3.3.1 parallel dispatch) at the end of this ICD. The 0.3.3 surface below is retained as-written — the amendment enumerates the delta rather than rewriting it inline.

```cpp
// src/kernel/js/bridge_context.hpp
namespace plinth::js {

struct BridgeContext {
    // ... 0.3.1 fields (unchanged) ...

    // --- Active in 0.3.3 ---
    std::vector<AsyncOp> pending_ops;
    std::unordered_map<int, PromiseCallbacks> callbacks;  // see below
    int next_callback_id = 0;
    int concurrent_async_ops = 0;
    int max_concurrent_async_ops = 8;  // default; overridable via RuntimeLimits

    // Methods (new in 0.3.3)
    int register_pending(JSValue resolve, JSValue reject);
    void resolve(int callback_id, Json::Value result);
    void reject(int callback_id, PromiseRejection err);
    std::vector<AsyncOp> take_pending_ops();
    bool has_pending_ops() const noexcept;
    int pending_op_count() const noexcept;
};

struct PromiseCallbacks {
    JSValue resolve;  // owns a QuickJS ref; freed on resolve/reject
    JSValue reject;   // owns a QuickJS ref; freed on resolve/reject
};

struct PromiseRejection {
    std::string code;       // e.g. "db.syntax_error", "audit.reserved_prefix"
    std::string message;    // human-readable
    std::optional<std::string> sqlstate;   // populated for db.* errors only
};

}  // namespace plinth::js
```

Notes:
- `register_pending` records the two callback values in `callbacks[next_callback_id++]` and returns the integer ID. It does NOT `JS_DupValue` — ownership is moved in (see §PromiseCapability).
- `resolve` / `reject` each invoke the recorded callback via `JS_Call`, then `JS_FreeValue` both sides, then erase the map entry, then decrement `concurrent_async_ops`.
- `take_pending_ops()` swaps the vector empty and returns the previous contents. The coroutine loop drains per pass; new ops can re-populate during the pass.
- `pending_op_count()` reports both in-flight (already `co_await`ed but not yet resolved) and queued-by-back-pressure ops. Used for back-pressure decisions and for the cancellation cascade's "drain pending" step.

---

## AsyncOp Contract

```cpp
// src/kernel/js/async_op.hpp
namespace plinth::js {

struct AsyncOp {
    enum class Type {
        DB_QUERY,
        DB_EXEC,
        AUDIT_WRITE,
        // Reserved for future milestones; declared so the enum layout is
        // stable but 0.3.3 does NOT implement a dispatch arm for any of these.
        HTTP_REQUEST,   // 0.10.3
        CAP_CALL,       // 0.3.4
        STORAGE_GET,    // 0.10.x
        STORAGE_PUT,    // 0.10.x
        PUBSUB_PUBLISH, // 0.5.x
    };

    Type type;
    int callback_id;

    // Operation-specific payload — populated per `type`. Unused fields default-initialised.
    std::string sql;                         // DB_QUERY, DB_EXEC
    std::vector<Json::Value> sql_params;     // DB_QUERY, DB_EXEC (optional)
    bool silent = false;                     // DB_EXEC — reserved for 0.5.x realtime hook
    std::string audit_event_type;            // AUDIT_WRITE
    Json::Value audit_payload;               // AUDIT_WRITE
};

}  // namespace plinth::js
```

`AsyncOp` is POD-ish. The coroutine loop's switch over `Type` handles `DB_QUERY` / `DB_EXEC` / `AUDIT_WRITE`; reaching any other variant in 0.3.3 is a `std::unreachable` / logic error and is handled by rejecting the promise with `{code: "internal", message: "unimplemented async op"}` — this path is tested.

---

## Coroutine Dispatch Loop

> **0.3.3.1 amendment.** The `for (auto& op : bc.take_pending_ops()) { co_await dispatch_async_op(bc, op); }` serial body declared below was replaced in 0.3.3.1 by fire-and-forget fan-out via `dispatch_ops_batch_fanout` + `AnyCompletionAwaiter`. See §Implementation deviation (0.3.3.1 parallel dispatch). Critical Invariants are preserved.

```cpp
// src/kernel/js/run_on_context.hpp
namespace plinth::js {

struct EvalResult {
    std::expected<Json::Value, EvalError> value;
    std::chrono::milliseconds duration;  // wall-clock time, for audit/logging
};

// Entry point. `source` is evaluated on `bc.ctx` against its own `globalThis`.
// Takes `bc` by reference; caller owns the BridgeContext's lifecycle and is
// responsible for calling `RuntimePool::release(&bc)` on success or
// `RuntimePool::destroy(&bc)` on failure / cancellation.
drogon::Task<EvalResult> run_on_context(BridgeContext& bc, std::string_view source);

}  // namespace plinth::js
```

### Loop Structure

Pseudocode (canonical; DESIGN §3.3 is the narrative reference):

```cpp
drogon::Task<EvalResult> run_on_context(BridgeContext& bc, std::string_view source) {
    auto start = steady_clock::now();
    bc.execution_start = start;
    bc.cpu_time_accumulated = 0ns;

    bc.resume_cpu_timer();
    JSValue result = JS_Eval(bc.ctx, source.data(), source.size(), "<extension>", JS_EVAL_TYPE_GLOBAL);
    bc.pause_cpu_timer();

    // Drive the JS job queue + async dispatch until JS completes.
    while (bc.has_pending_ops() || JS_IsJobPending(bc.rt)) {
        // 1. Dispatch any newly queued async ops (respecting back-pressure).
        for (auto& op : bc.take_pending_ops()) {
            if (bc.concurrent_async_ops >= bc.max_concurrent_async_ops) {
                // Re-queue and break — a completing op will wake the loop.
                bc.pending_ops.insert(bc.pending_ops.begin(), std::move(op));
                break;
            }
            ++bc.concurrent_async_ops;
            co_await dispatch_async_op(bc, std::move(op));  // resolves/rejects the promise
            // `concurrent_async_ops` is decremented inside bc.resolve()/reject().
        }

        // 2. Resume JS to process resolved promises.
        bc.resume_cpu_timer();
        while (JS_IsJobPending(bc.rt)) {
            JSContext* pctx = bc.ctx;
            int rc = JS_ExecutePendingJob(bc.rt, &pctx);
            if (rc < 0) {
                bc.pause_cpu_timer();
                co_return classify_js_error(bc, pctx);
            }
        }
        bc.pause_cpu_timer();

        // 3. Check for cancellation / timeout triggered by the interrupt handler.
        if (bc.cancelled.load(std::memory_order_acquire) || bc.wall_clock_exceeded()) {
            co_return co_await run_cancellation_cascade(bc);
        }
    }

    co_return finalize(bc, result, start);
}
```

Three critical invariants the implementation MUST uphold:

1. **Serialized access to QuickJS.** `bc.rt` and `bc.ctx` are touched only inside `run_on_context`'s coroutine body. `co_await` may resume on a different thread; QuickJS does not care because no other code path touches the runtime concurrently. This matches DESIGN §3.1 "the coroutine is the serialization mechanism" and resolves §11 Q#4 by construction.
2. **CPU-timer bracket matches JS execution.** Every `JS_Eval` or `JS_ExecutePendingJob` invocation is wrapped in `resume_cpu_timer()` / `pause_cpu_timer()`. `co_await`s fall **outside** the bracket. The accumulated CPU time is JS-only, per DESIGN §4.2.
3. **Back-pressure is FIFO.** When `concurrent_async_ops >= max_concurrent_async_ops`, the loop re-queues the op at the head of `pending_ops` and breaks. As in-flight ops complete (and the resolve/reject path decrements `concurrent_async_ops`), the next loop iteration drains queued ops in the order they were enqueued by JS. No op is starved.

### The Pool–Bridge Boundary

`run_on_context` does NOT call `acquire` or `release`. The expected call pattern (at 0.3.4 / 0.4.x wiring time):

```cpp
drogon::Task<Json::Value> dispatch_js_extension(Extension& ext, ...) {
    auto* bc = ext.pool().acquire();
    auto result = co_await run_on_context(*bc, source);
    if (result.value.has_value() && !bc->cancelled) {
        ext.pool().release(bc);
    } else {
        ext.pool().destroy(bc);
    }
    co_return std::move(result.value);
}
```

0.3.3 lands the coroutine entry point and a test driver; the extension-dispatch caller is a 0.3.4 / 0.4.x concern.

---

## Injected `db.*` Surface

```
db.query(sql: string, params?: Array<null | boolean | number | string | Uint8Array>)
    -> Promise<{ rows: Array<object>, row_count: number }>

db.exec(sql: string,
        params?: Array<null | boolean | number | string | Uint8Array>,
        opts?:   { silent?: boolean })
    -> Promise<{ row_count: number }>
```

Both bindings live in `src/kernel/js/stdlib/db_bindings.cpp` and are registered during `inject_kernel_stdlib(ctx)` so 0.3.3 pools automatically expose them. No separate `inject_async_stdlib` — the split is one source file per namespace per the SESSION-GUIDE "one file per capability handler" convention that ICD-0.3.2 already applied.

### Argument Rules

- `sql` (arg 0): required, non-empty string. Violation → synchronous `TypeError` (thrown, not rejected — matches ICD-0.3.2 §Error Model for sync misuse).
- `params` (arg 1, optional): array of scalars. Accepted JS → SQL mapping:

| JS value | SQL parameter binding |
|---|---|
| `null` / `undefined` | `NULL` |
| `boolean` | `BOOL` |
| `number` (integer in `[-2^53, 2^53]`) | `BIGINT` |
| `number` (non-integer) | `DOUBLE PRECISION` |
| `string` | `TEXT` (UTF-8 bytes) |
| `Uint8Array` | `BYTEA` |

Any other JS type (object, Array, function, Symbol, BigInt, Date) → synchronous `TypeError: unsupported parameter type at index N`. `Date` handling is **explicitly deferred**; extensions must format timestamps as ISO 8601 strings and rely on `TIMESTAMPTZ` implicit cast.

- `opts` (arg 2 of `db.exec`, optional): object; only `silent: boolean` is read. Any unknown key is silently ignored (consistent with ICD-0.3.2's Node-like convention). Non-object `opts` → synchronous `TypeError`.

### Result Shape — `db.query`

```jsonc
{
  "rows": [
    { "<column_1>": <jsValue>, "<column_2>": <jsValue>, ... },
    ...
  ],
  "row_count": <number>
}
```

- Each row is a plain JS object keyed by column name as returned by the PG driver. Ordering is insertion-ordered (QuickJS object property insertion order).
- `row_count` equals `rows.length`; surfaced as a separate field so callers don't have to reach into `rows` to test emptiness.
- No wrapping envelope beyond this — consistent with DISCUSSION-streaming-and-media §0: the shape is the rows themselves, not a generic `{status, data, error}` enclosure. A future streaming variant (`db.cursor`, `db.stream`) is its own API, not a narrowing of this one.

### PG-Value → JS-Value Conversion

| PostgreSQL type | JS value |
|---|---|
| `BOOL` | `boolean` |
| `INT2`, `INT4` | `number` |
| `INT8` (bigint) | `number` if in safe-integer range, else `string` (matches the ICD-0.3.2 posture: 0.3.3 does not introduce BigInt) |
| `FLOAT4`, `FLOAT8`, `NUMERIC` | `number` (NUMERIC via lexical → `parseFloat`; lossy for arbitrary precision — document in the bindings source) |
| `TEXT`, `VARCHAR`, `NAME`, `CHAR`, `UUID` | `string` |
| `BYTEA` | `Uint8Array` (fresh copy; no aliasing to driver buffer — same rule as ICD-0.3.2 §Type Conversion Contract) |
| `JSON`, `JSONB` | parsed to structural JS (`JSON.parse` semantics; driver returns text, binding parses once per cell) |
| `TIMESTAMP`, `TIMESTAMPTZ` | `string` (ISO 8601, UTC for `TIMESTAMPTZ`, naïve for `TIMESTAMP` without zone) |
| `DATE` | `string` (ISO 8601 date-only, `YYYY-MM-DD`) |
| SQL `NULL` | `null` |
| any other type | `string` (driver's textual representation) |

Array types (`TEXT[]`, `INT4[]`, …) fall under "any other type" in 0.3.3. Properly-typed JS array results land when a real caller needs them — documented as a scope boundary in §What Must Not Be Decided Yet.

### Result Shape — `db.exec`

```jsonc
{ "row_count": <number> }
```

`row_count` is the PG driver's `affected_rows`. No `rows` field — `db.exec` is the INSERT/UPDATE/DELETE/DDL path.

### `opts.silent`

Per DESIGN §8.2: when `true`, suppress realtime event emission that the 0.5.x DB layer will eventually fire from `db.exec`. In 0.3.3 the realtime path does not yet exist, so `silent` is plumbed through the `AsyncOp` but has no observable effect. It is **carried forward, not consumed** — the 0.5.x code session wires the effect.

### Promise Rejection Shape (`db.*`)

When the PG driver raises:

```
throw DbError { code: string, message: string, sqlstate: string }
```

Concrete `code` values in 0.3.3:

| `code` | Trigger |
|---|---|
| `db.syntax_error` | PG SQLSTATE `42601` (syntax error) |
| `db.undefined_table` | SQLSTATE `42P01` |
| `db.undefined_column` | SQLSTATE `42703` |
| `db.constraint_violation` | SQLSTATE class `23*` (integrity constraint) |
| `db.permission_denied` | SQLSTATE `42501` |
| `db.serialization_failure` | SQLSTATE `40001` |
| `db.deadlock_detected` | SQLSTATE `40P01` |
| `db.timeout` | query cancelled by wall-clock timeout cascade (see §Cancellation) |
| `db.cancelled` | query cancelled by `bc.cancelled` flip (extension-disable, client disconnect) |
| `db.connection_error` | driver-level connect/read failure |
| `db.internal` | any SQLSTATE not mapped above; `sqlstate` carries the raw code |

The mapping table lives in `src/kernel/js/stdlib/db_error_map.cpp` and is exercised by test fixtures that force each class. Additional mapping entries are a per-PR source change — adding one does not require a new ICD.

`sqlstate` is always populated for driver-origin errors (every row above except `db.timeout` and `db.cancelled`, which originate inside the bridge). Callers may treat `sqlstate` as opaque or match specific values; the JS surface exposes it verbatim.

---

## Injected `audit.*` Surface

```
audit.log(event_type: string, payload: object) -> Promise<void>
```

Lives in `src/kernel/js/stdlib/audit_bindings.cpp`, registered in `inject_kernel_stdlib`.

### Argument Rules

- `event_type`: required, non-empty string. See §Reserved Prefix Policy below. Violation → sync `TypeError` / async `RangeError`-class rejection (different cases below).
- `payload`: required, plain object. `null`, `undefined`, arrays, or non-object → sync `TypeError`. Converted via the same `JS→Json::Value` helper ICD-0.3.2 uses for `log.*` `ctx`.

### Reserved Prefix Policy

Extensions MUST prefix their event types with `ext.<extension_id>.` — the `<extension_id>` comes from the extension's manifest (`architecture/05-extensions.md §1`). Since 0.3.3 does not yet wire the extension installer (0.4.x), the bridge uses the string held on `BridgeContext::extension` (or `"host"` when `extension == nullptr`, the test-driver case).

Kernel-reserved prefixes (canonical list from ICD-0.1.7 §Audit Event Catalog):

- `user.*`
- `session.*`
- `pat.*`
- `group.*`
- `rbac.*`
- `capability.*`

Calling `audit.log` with an event type whose prefix matches any kernel-reserved prefix above **rejects the promise** with:

```
{ code: "audit.reserved_prefix", message: "event_type uses kernel-reserved prefix: <prefix>" }
```

Calling with a malformed prefix (does not start with `ext.`) rejects with:

```
{ code: "audit.invalid_prefix", message: "extension audit events must start with 'ext.<id>.'" }
```

### Non-Forgeable Provenance

These fields on the audit row are filled by the kernel from `BridgeContext`; JS callers **cannot override them**:

- `user_id` — from `bc.user.user_id` (once the BridgeContext is wired to a UserContext via 0.3.4; in 0.3.3 test drivers, populated from the driver-provided `UserContext`)
- `session_id` — from `bc.user.session_id`
- `ip_address` — from `bc.user.ip_address`
- `extension_id` — from `bc.extension` (or `"host"` for direct test-driver execution)
- `node_id` — from `plinth::Config::node_id`
- `call_depth` — from `bc.call_depth`
- `timestamp` — from `NOW()` at PG-insert time (per ICD-0.1.7 table schema)

If the JS `payload` contains any of the keys `user_id`, `session_id`, `ip_address`, `extension_id`, `node_id`, `call_depth`, `timestamp`, **the promise rejects** with:

```
{ code: "audit.reserved_field", message: "payload contains non-forgeable field: <field>" }
```

This is stricter than ICD-0.1.7's existing C++ API (which auto-enriches rather than rejects) because JS code is untrusted by design.

### Writer Path

The async op routes to `plinth::log::audit(...)` (the 0.1.7 primitive). Per the 0.2.4 precedent — the `g_audit_ready` atomic set by `plinth::log::init()` — when audit is not ready:

```
{ code: "audit.not_ready", message: "audit writer not initialized" }
```

The promise rejects cleanly; no insert is attempted. Tests that spin up the bridge without `plinth::log::init()` hit this path and assert the clean rejection.

### Result Shape

Resolved value: `undefined`. `audit.log` is fire-and-forget from the JS caller's perspective; the promise resolves when the audit write hits the async sink (not when PG confirms — the spdlog async sink is the sync boundary, matching ICD-0.1.7 §Security Constraint 5).

### Rejection Shape (`audit.*`)

Same envelope as `db.*` errors minus `sqlstate`:

```
{ code: string, message: string }
```

Canonical codes: `audit.reserved_prefix`, `audit.invalid_prefix`, `audit.reserved_field`, `audit.not_ready`, `audit.internal` (any unexpected writer failure).

---

## `RuntimeLimits` Additions

ICD-0.3.1 defines `RuntimeLimits`. 0.3.3 adds one field:

```cpp
struct RuntimeLimits {
    // ... 0.3.1 fields ...
    int max_concurrent_async_ops = 8;
};
```

The value is copied onto every `BridgeContext` at pool-entry creation. A value of `0` means "no concurrent async ops allowed" — valid for an extension that should not fan out at all; rejected with `{code: "async.concurrency_limit", message: "max_concurrent_async_ops is 0"}` on any attempt. Negative values are rejected by the `RuntimePool` constructor (programming error, same treatment as ICD-0.3.1 §Security Constraint 1).

---

## Error Model — `EvalErrorKind` Additions

```cpp
enum class EvalErrorKind {
    // ... from 0.3.0 / 0.3.1 ...
    ASYNC_CONCURRENCY_LIMIT,     // new in 0.3.3 — bc.max_concurrent_async_ops = 0 path
    PROMISE_REJECTED_UNHANDLED,  // new in 0.3.3 — top-level rejection escaped try/catch
    PROMISE_RESOLVE_AFTER_CANCEL,// new in 0.3.3 — resolve/reject ran after cancellation (defensive — should not happen in correct code; tests assert it doesn't)
    INTERNAL_ASYNC               // new in 0.3.3 — unimplemented AsyncOp::Type hit, or other bridge-internal invariant violation
};
```

- `PROMISE_REJECTED_UNHANDLED`: when the top-level `JS_Eval` returned a promise and that promise rejected without the extension providing a handler. The rejection reason is captured in the `EvalError::message` and (for `db.*`/`audit.*` origins) the `code` field of the rejection is appended.
- `PROMISE_RESOLVE_AFTER_CANCEL`: defensive. The cancellation cascade (§below) is supposed to prevent this. If an in-flight `co_await` completes after step 1 of the cascade, the resolve path detects the `cancelled` flag and drops the result instead of touching freed QuickJS state. This error surfaces only in tests / CI; production hitting it indicates a cascade bug.
- `INTERNAL_ASYNC`: reaching a reserved-but-not-implemented `AsyncOp::Type` in the dispatch switch, or any other invariant violation that would otherwise be undefined behaviour.

---

## Resource Limits During Async

Behavior of each ICD-0.3.1 limit across `co_await` gaps:

1. **Memory limit** — no special handling during async gaps. JS heap is idle; no allocations fire. C++-side async-op results (row buffers, audit payloads) are C++ heap, not counted against `JS_SetMemoryLimit`. DESIGN §4.1's "result size limit" is introduced as a `RuntimeLimits::async_result_size_limit_bytes` field with a 16 MiB default, stored on `BridgeContext`; 0.3.3 carries it through but **does not reject oversized results** — enforcement is 0.3.5 scope, tracked as "adversarial hardening" in §What Must Not Be Decided Yet.
2. **CPU time limit** — paused across every `co_await`. Only JS execution (`JS_Eval` + `JS_ExecutePendingJob`) counts. DESIGN §4.2's "CPU time skips the gap" is exactly what the pause/resume bracket achieves. Tests from ICD-0.3.1 (CPU vs. wall-clock independence via `__host_sleep_ms__`) remain valid; 0.3.3 adds a parallel test using a real async `db.query` as the gap source.
3. **Wall-clock timeout** — keeps running. Triggers the cancellation cascade (§below) when exceeded, consistent with DESIGN §4.3 / §6.2. Enforcement point remains the interrupt handler from ICD-0.3.1 (fires between JS opcodes during `JS_ExecutePendingJob` sweeps) **plus** a `cpp` check in the coroutine loop at step 3 of each iteration (so wall-clock exceeded during a `co_await` gap is caught on the first loop return, not only on the next JS opcode).
4. **Call depth** — enforcement stays with the capability dispatcher per ICD-0.2.2 and ICD-0.2.4; the bridge doesn't touch it. When 0.3.4 wires `cap.call` from JS, that ICD will spell out how depth propagates through the AsyncOp into the dispatcher call.
5. **Stack depth** — unchanged from ICD-0.3.1. `JS_SetMaxStackSize` is a per-runtime QuickJS setting; async gaps don't affect it.

---

## Cancellation Cascade

> **0.3.3.1 amendment.** Step 3's blind 5 s `co_await drogon::sleepCoro` was replaced by a wake-driven bounded drain (`while (inflight_detached > 0 && now < deadline) co_await AnyCompletionAwaiter{bc};`). The 5 s ceiling is retained. See §Implementation deviation (0.3.3.1 parallel dispatch) for the `BridgeContext`-UAF closure rationale.

DESIGN §6.3's six-step sequence is canonical. 0.3.3 implements it as `run_cancellation_cascade(bc)`:

```cpp
drogon::Task<EvalResult> run_cancellation_cascade(BridgeContext& bc) {
    // Step 1: signal — already done by caller (bc.cancelled.store(true) or wall-clock trip).

    // Step 2: cancel all in-flight async ops.
    //   In 0.3.3 this means: every outstanding drogon::Task waiting on pgClient
    //   is requested to cancel. Drogon's PG client in current versions treats
    //   query cancellation as best-effort: the connection is returned to the
    //   pool, the query may still complete server-side. The 5-second bound
    //   below is the enforcement lever.
    for (auto& [id, cbs] : bc.callbacks) {
        request_cancel(id);  // delegates to the per-op cancellation source
    }

    // Step 3: wait up to 5 s for cancelled ops to complete/abort.
    co_await drogon::sleepCoro(drogon::trantor::EventLoop::getEventLoopOfCurrentThread(),
                               std::chrono::seconds(5));
    //   After this deadline, any still-in-flight result is discarded by the
    //   bridge; the callback_id is marked abandoned and future resolve() calls
    //   on it are no-ops (see PROMISE_RESOLVE_AFTER_CANCEL).

    // Step 4: reject every outstanding JS promise so QuickJS can release its
    //         internal promise state cleanly.
    bc.resume_cpu_timer();
    for (auto& [id, cbs] : bc.callbacks) {
        bc.reject(id, PromiseRejection{.code = "db.cancelled",  // or audit.cancelled, etc.
                                       .message = "execution cancelled"});
    }
    // Drive the job queue so QuickJS processes the rejections.
    while (JS_IsJobPending(bc.rt)) {
        JSContext* pctx = bc.ctx;
        JS_ExecutePendingJob(bc.rt, &pctx);
    }
    bc.pause_cpu_timer();

    // Step 5: classification and return — the caller will pass bc to
    //         RuntimePool::destroy(), NOT release(). This is the ICD-0.3.1
    //         §Security Constraint 4 contract.
    co_return EvalResult{
        .value = std::unexpected(EvalError{
            .kind = bc.wall_clock_exceeded() ? EvalErrorKind::WALL_CLOCK_EXCEEDED
                                             : EvalErrorKind::CANCELLED,
            .message = "execution cancelled after 5s abandon window"
        }),
        .duration = steady_clock::now() - bc.execution_start
    };
}
```

Notes:

- **Double cancel is idempotent.** `bc.cancelled.store(true)` called twice is fine — the atomic is set-and-forget. Entering `run_cancellation_cascade` twice is prevented by having the coroutine loop check `bc.cancelled` once per iteration and then dispatch to the cascade; once in the cascade, the loop exits.
- **Recursive cancellation** of downstream `CAP_CALL` contexts is a 0.3.4 concern — 0.3.3 does not implement `CAP_CALL`, so there is nothing to cascade into.
- **Runtime is destroyed, never released.** `ICD-0.3.1 §Security Constraint 4` mandates `release()` on a context with `cancelled` set is a programmer bug; the defensive route-to-`destroy` stays in place.

---

## Back-Pressure

> **0.3.3.1 amendment.** The "insert at head of `pending_ops`, break out of dispatch phase" mechanism declared below is replaced by `dispatch_ops_batch_fanout`'s re-queue-on-saturation + `AnyCompletionAwaiter` suspend. FIFO semantics preserved. See §Implementation deviation (0.3.3.1 parallel dispatch).

Per DESIGN §7.2:

- Default `max_concurrent_async_ops = 8` (DESIGN suggests 16; the ICD lands the lower value to conservatively leave PG connection pool headroom; tunable later based on benchmarks).
- When a new `AsyncOp` is enqueued and `bc.concurrent_async_ops >= bc.max_concurrent_async_ops`, the op is inserted at the head of `pending_ops` and the loop breaks out of the dispatch phase without `co_await`ing it.
- As each in-flight op completes (inside `bc.resolve()` or `bc.reject()`), `concurrent_async_ops` decrements. The next loop iteration drains the queue in FIFO order.
- This is transparent to JS: the promise hasn't been rejected, it's simply not yet scheduled. The JS caller sees a slightly longer `await` wall-clock time — within the wall-clock limit, of course, or cancellation fires.

Open-question carry-over from DESIGN §11 Q#2 (PG connection pool sizing): 0.3.3 does **not** dynamically adjust Drogon's PG pool. The expectation is that operators ensure `pg_pool_size >= runtime_pool_size * max_concurrent_async_ops` (`16 * 8 = 128` by default, which exceeds Drogon's default `1` PG connection; the config template in `config.yml.example` MUST document this). A PR touching `config.yml.example` to raise the default PG pool size and document the formula lands in the same branch as 0.3.3 code.

---

## Type Conversion — Additions Beyond 0.3.2

ICD-0.3.2 §Type Conversion Contract covers the sync bindings. `db.*` / `audit.*` add:

- **JS Array → C++ `std::vector<Json::Value>`** (for `params`): each element converted per the per-slot rules above. Non-array → `TypeError`.
- **JS Object (plain, not Array) → `Json::Value`** (for `audit.log` payload): recursive via the existing helper ICD-0.3.2 uses. Reject the promise (not throw sync) when any reserved-field key is present; the distinction is that "wrong type" is sync-throw-TypeError whereas "policy violation on otherwise-valid argument" is async-reject.
- **PG result row → JS object**: see the PG↔JS mapping table above.

---

## Performance Targets

Measured on the CI builder image, runtime pre-initialized. Informational targets only — no benchmark harness ships in 0.3.3; follow the 0.2.6.2 precedent of a later benchmark PR if regression needs tracking.

- `db.query("SELECT 1")` round trip (single row, empty params): **≤ 2 ms** (dominated by PG round trip, not bridge overhead).
- Bridge overhead alone (measured against a mock async op that resolves immediately): **≤ 50 μs** per op.
- `audit.log("ext.test.foo", {})` round trip: **≤ 100 μs** (async sink-bound, not PG-bound).
- Cancellation cascade to `EvalError`: **≤ 5.1 s** worst case (5 s drain + teardown).

---

## Security Constraints (Non-Negotiable)

1. Every `BridgeContext` injected with `db.*` MUST be pool-constructed with an extension-scoped PG search path. At connection checkout, the bridge issues `SET search_path TO ext_<extension_id>, plinth;` before the user's `sql` runs. For test/host contexts where `extension == nullptr`, the search path is `plinth` only (no ext schema), and `db.query`/`db.exec` are still callable — useful for host-integration tests, not reachable from real extensions.
2. `db.*` arguments ALWAYS bind via PG parameterized queries — the `sql` string is passed as-is (no interpolation) and `params` bind via `$1`, `$2`, ... The bindings MUST NOT offer a "format-in-place" helper. SQL injection surface is a parameterized-query rule, nothing else.
3. `audit.log` MUST fail closed. Any of the following conditions produce a rejection, never a silent drop: missing writer (`g_audit_ready == false`), reserved prefix, invalid prefix, reserved field in payload, PG insert failure. A dropped audit event is a correctness bug.
4. `audit.log` MUST NOT accept non-forgeable fields in the payload, even if the values would match the kernel-computed ones. The rejection is unconditional on key presence; the value is not inspected.
5. The `silent: true` flag on `db.exec` is **not** a generic "suppress audit" flag. It suppresses only the realtime-bus event emission (0.5.x). Every `db.exec` call is still visible to the audit path via the capability dispatcher's hooks (when 0.3.4 wires them); no JS-callable path lets an extension hide DB mutations from audit.
6. The `PROMISE_RESOLVE_AFTER_CANCEL` code path MUST NOT touch QuickJS state (`JS_Call`, `JS_FreeValue` on anything not known-alive, etc.). It logs a warning and drops the result. Production hitting this path is a bug; the defensive handling is a belt-and-braces measure.
7. Running `db.*` or `audit.*` on a `BridgeContext` whose `cancelled` flag is set MUST reject the new op's promise with `{code: "<ns>.cancelled", message: "execution cancelled"}` and NOT enqueue the op. (Belt-and-braces alongside the cascade's step 2; prevents new ops racing the cascade.)
8. Host-binding code MUST NOT call `drogon::app().getDbClient()` synchronously outside a Drogon-managed coroutine — the ICD-0.3.2 §Security Constraint 6 rule carries over to 0.3.3, but 0.3.3 is the milestone that does legitimately reach for `getDbClient()`. The call happens inside `dispatch_async_op`'s `DB_QUERY` / `DB_EXEC` arm, which is itself inside the coroutine driven by Drogon's event loop. Tests run under Catch2 MUST exercise the bridge via the same coroutine path (see §Milestone Criteria §CI Wiring) to match production topology.

---

## What Must Not Be Decided Yet

- **`cap.call` / `cap.batch` JS bindings.** 0.3.4 scope. 0.3.3 does NOT place `cap` on `globalThis`. Downstream recursive cancellation through `CAP_CALL` contexts also waits on 0.3.4.
- **`pubsub.*`, `storage.*`, `http.*`.** Enum variants reserved (§AsyncOp Contract); no implementation, no registration, no tests beyond the "unimplemented type rejects with `internal`" case.
- **Result-size enforcement.** `async_result_size_limit_bytes` ships on `RuntimeLimits` with a 16 MiB default but no rejection path in 0.3.3. 0.3.5 adversarial hardening closes this loop.
- **Typed PG array results.** `TEXT[]` etc. fall into "any other type → string" today. Promoting arrays to JS arrays is a follow-up when a real caller needs it; the mapping-table file (`db_error_map.cpp` and its sibling type-mapping table) is the natural extension point.
- **`Date` / `BigInt` params.** Not supported; extensions format timestamps as ISO 8601 strings and integers within the IEEE 754 safe range. BigInt support tracks with the ICD-0.3.2 posture — a future decision driven by a concrete caller.
- **Parallel optimization of `Promise.all([cap.call(...)])` below naive fan-out.** The DESIGN §7.3 optimization (bundling Tier 3 batch calls into a single HTTP request) is 0.8.x scope. 0.3.3 delivers the naive fan-out via the coroutine loop; benchmarks in a later re-eval may surface a need.
- **Per-extension tuning of `max_concurrent_async_ops`.** The field ships on `RuntimeLimits`; wiring extension manifest → limit values lands in 0.4.x. Until then, only the default applies.
- **`db.batch`.** Explicitly 0.3.4 or later. `Promise.all([db.query(...), db.query(...)])` works in 0.3.3 via the coroutine loop's natural fan-out; a dedicated `db.batch` helper is syntactic sugar that can be added without changing this contract.
- **DESIGN §11 Q#3 module-state heap fragmentation mitigation.** Not decided in 0.3.3; relies on the memory limit + pool rebuild on hot-reload (both already live).
- **Any narrowing of `plinth.call()` / `cap.call()` return shapes.** Per DISCUSSION-streaming-and-media §0, the bridge keeps return values opaque. This ICD's `db.query` shape is a concrete API shape (rows + row_count), NOT a general capability return envelope — it applies to `db.query` specifically.

---

## Milestone Criteria

All test groups below MUST pass under Catch2 before 0.3.3 ships. They mirror `DESIGN-quickjs-bridge.md §9.3` exit criteria, mapped 1-to-1 to test cases.

### Tests

Test file: `tests/kernel/js/async_bridge_test.cpp` (one file per ICD-0.3.2 precedent). PG-backed cases are gated the same way `tests/kernel/audit/*` and `tests/kernel/capabilities/resolution_test.cpp` are — skipped when `PLINTH_PG_TESTS=OFF` or PG env vars unset. Test driver builds a `BridgeContext` via `RuntimePool`, then calls `run_on_context` from within a Drogon coroutine driver (`drogon::app().getLoop()->queueInLoop(...)` + `co_await` in a `drogon::Task<void>` test harness, or equivalent).

**Group A — Correctness (DESIGN §9.3 Correctness tests):**

1. `await db.query("SELECT 1 AS one")` resolves to `{rows: [{one: 1}], row_count: 1}`.
2. `await db.query("INVALID SQL")` rejects with `{code: "db.syntax_error", sqlstate: "42601", ...}`.
3. Sequential awaits: two back-to-back `await db.query(...)` both complete; result values are as expected.
4. Parallel awaits: `await Promise.all([db.query("SELECT pg_sleep(0.1)"), db.query("SELECT pg_sleep(0.1)")])` completes in < 150 ms (both pg_sleep run concurrently; serial would be > 200 ms). Timing margin is deliberately loose.
5. Nested awaits: `.then(async () => await db.query(...))` completes; asserts the inner promise resolves and the outer chain observes the value.
6. Unhandled rejection: `await db.query("BAD")` without `try/catch` surfaces as `EvalError{kind: PROMISE_REJECTED_UNHANDLED, message: contains "db.syntax_error"}`.

**Group B — Resource Limits (DESIGN §9.3 Resource limit tests):**

7. Memory limit enforced during between-await JS (`while(true) { a.push(new Array(1000)); if (cond) await db.query("SELECT 1"); }` → `EvalError{MEMORY_LIMIT}`).
8. CPU time limit enforced between awaits (`for (;;) {}` → `EvalError{CPU_TIME_EXCEEDED}` within `cpu_time_limit + small margin`).
9. CPU time does NOT count async wait: JS awaits a `pg_sleep(1)` (or `__host_sleep_ms__` shim) longer than `cpu_time_limit`; execution completes successfully.
10. Wall-clock timeout: JS awaits `pg_sleep(wall_clock_limit + 2)`; `EvalError{WALL_CLOCK_EXCEEDED}` returned; the abandon window (5 s) is observed; no leaks under ASAN.
11. Concurrent async op limit (`max_concurrent_async_ops = 2`, `Promise.all([q1, q2, q3, q4])`): all four promises resolve; internal counters (instrumented via a debug hook in `BridgeContext`) show at most 2 in-flight at any instant.

**Group C — Cancellation (DESIGN §9.3 Cancellation tests):**

12. Cancel during PG query: external driver flips `bc.cancelled`; cascade produces `EvalError{CANCELLED}`; pool.destroy() runs; ASAN reports zero leaks.
13. Cancel during JS execution: same as above but `bc.cancelled` flipped during a `for(;;){}` loop; interrupt handler fires, cascade runs, clean teardown.
14. Double cancel is idempotent — flip `cancelled` twice, still one cascade, still `EvalError{CANCELLED}`.
15. Cancel with pending promises: JS has three in-flight `db.query` promises; cancellation rejects all three with `{code: "db.cancelled"}`; QuickJS frees internal promise state; ASAN clean.

**Group D — Concurrency (DESIGN §9.3 Concurrency tests):**

16. 10 concurrent test-driver invocations of `run_on_context` (each its own `BridgeContext`) run to completion; pool manages allocation correctly. Reduced from DESIGN's "100 concurrent" to match CI runner budget; the concurrency assertion is the same.
17. 10 concurrent invocations each executing `Promise.all([5 queries])` — total 50 in-flight queries. `max_concurrent_async_ops = 8` per execution; PG pool not exhausted (sized at 80 in the test config); all 10 complete.

**Group E — `audit.*` (this ICD's unique surface):**

18. `audit.log("ext.test.foo", {bar: 1})` resolves; the `plinth.audit_log` row exists with `action = "ext.test.foo"`, `detail->>'bar' = '1'`, `user_id / session_id / node_id` filled by kernel.
19. `audit.log("user.login", {...})` (kernel-reserved prefix) rejects with `{code: "audit.reserved_prefix"}`.
20. `audit.log("malformed", {...})` rejects with `{code: "audit.invalid_prefix"}`.
21. `audit.log("ext.test.foo", {user_id: "00000000-..."})` rejects with `{code: "audit.reserved_field"}`.
22. `audit.log(...)` called before `plinth::log::init()` rejects with `{code: "audit.not_ready"}` (runnable via a test-only `plinth::log::test_reset_ready()` helper — lives under `PLINTH_JS_TEST_SHIMS`).

**Group F — ThreadSanitizer smoke:**

23. Two-thread concurrent invocation of `run_on_context` on distinct `BridgeContext`s (separate pool entries), each executing a `db.query` + `audit.log` sequence. Mirrors the `tests/kernel/capabilities/batch_test.cpp` two-thread TSan case. Must pass clean under `-DPLINTH_SANITIZERS=ON` with ThreadSanitizer enabled.

### CI Wiring

- `src/kernel/js/run_on_context.{hpp,cpp}` — coroutine entry point + dispatch-op switch.
- `src/kernel/js/async_op.hpp` — struct definition.
- `src/kernel/js/stdlib/db_bindings.cpp`, `src/kernel/js/stdlib/db_error_map.cpp`, `src/kernel/js/stdlib/audit_bindings.cpp` — new. Registered from `inject_kernel_stdlib` alongside the 0.3.2 surface. The inject seam does NOT split into sync-vs-async files; one injection point, conditional on whether async is wired. (Rationale: same as ICD-0.3.2's single-file test — a hard split creates a phantom API boundary that doesn't match how extensions consume the stdlib.)
- `src/kernel/js/bridge_context.hpp/cpp` — field-population changes only; no struct-layout churn (fields were reserved in 0.3.1).
- `src/kernel/capabilities/resolution.hpp/cpp` — add `call_capability_async` per ICD-0.2.6 (the 0.2.6 wrapper is a prerequisite; see that ICD).
- `src/kernel/capabilities/batch.hpp/cpp` — add `batch_call_capability_async` per ICD-0.2.6.
- Test file at `tests/kernel/js/async_bridge_test.cpp` registered in the existing Catch2 executable. Twenty-three cases across six groups. PG-backed cases gated the same way `tests/kernel/audit/audit_test.cpp` is.
- No new CI job. Existing `build-and-test` runs under ASAN/UBSan; existing `tsan` runs (when present, per the 0.5.x ROADMAP TSan job) cover Group F.
- `config.yml.example` updated to document the PG pool sizing formula (`pool_size * max_concurrent_async_ops`). If the default PG pool figure needs raising, that change lands in the same PR.
- No new CMake option. `PLINTH_JS_TEST_SHIMS` gains the `plinth::log::test_reset_ready()` helper alongside the existing `__host_sleep_ms__` shim.

---

## Entry / Exit

**Entry:** ICD-0.3.1 and ICD-0.3.2 implementations merged (runtime pool + sync stdlib); ICD-0.2.6-async-dispatch implemented alongside (the coroutine wrapper around `call_capability`, even though no `CAP_CALL` op dispatches through it in 0.3.3 — the wrapper unblocks the `run_on_context` coroutine shape). `DESIGN-quickjs-bridge.md` unchanged — no addendum authored.

**Exit:** all 23 test cases pass under Catch2 default and sanitizer builds; `run-clang-tidy-20` clean on `src/kernel/js/**` and `src/kernel/capabilities/**`; `config.yml.example` updated; CHANGELOG entry describing shipped surface, deviations (if any), and follow-ups. Band flip on ROADMAP.md: `0.3.3 [medium] → shipped`.

---

## Implementation deviation (0.3.3.1 parallel dispatch)

**Added by:** RE-EVAL following 0.3.3 (2026-04-18), folding the 0.3.3.1 parallel-fanout surface into the ICD. 0.3.3.1 shipped without its own ICD by architect choice; `docs/DEFERRED.md` §parallel-fanout (Resolved) + §runtime-binder (Resolved) + this ICD's §Critical Invariants were the running contract. This subsection records the delta so fresh sessions read the specification, not its history.

### New `BridgeContext` fields

Four fields added to `src/kernel/js/bridge_context.hpp`; all are main-loop-touched unless noted.

```cpp
std::atomic<int>        inflight_detached{0};
std::mutex              wake_mu;
int                     wake_count     = 0;
std::coroutine_handle<> waiter_handle;
```

- **`inflight_detached`** — bumped at dispatch, decremented inside the `queueInLoop` callback that ran `resolve`/`reject`. Separate from `concurrent_async_ops` (the back-pressure counter, decremented in `resolve`/`reject` itself). `inflight_detached > 0` keeps the outer loop alive and gates cancellation-cascade teardown. Atomic because the decrement path lives on the main loop but `AnyCompletionAwaiter::await_ready` reads it from the coroutine's resume point.
- **`wake_mu` + `wake_count` + `waiter_handle`** — signaling surface between detached-completion callbacks (via `signal_completion()`) and the outer coroutine's `AnyCompletionAwaiter`. Mutex serializes waiter-vs-signaler state transitions; accumulated signals short-circuit a subsequent `await_suspend` via a `bool` return so completions arriving between awaits are not lost.

### New method

```cpp
void signal_completion();
```

Exchange-and-resume pattern: called from main-loop `queueInLoop` callbacks after `resolve`/`reject` runs and `inflight_detached` is decremented. If a coroutine handle is parked in `waiter_handle`, resume it; otherwise increment `wake_count`.

### Extended `PromiseCallbacks` + `register_pending`

```cpp
struct PromiseCallbacks {
    JSValue     resolve{};
    JSValue     reject{};
    std::string ns_for_cancellation;  // "db" | "audit"
};

auto register_pending(JSValue     resolve_fn,
                      JSValue     reject_fn,
                      std::string ns_for_cancellation) -> int;
```

The `ns_for_cancellation` field lets `run_cancellation_cascade` issue per-namespace reject codes (`db.cancelled` vs `audit.cancelled`, per §Cancellation Cascade Step 4). Bindings populate it at `register_pending()` time; an empty string maps to `internal` in the cascade.

### Revised Coroutine Dispatch Loop

The 0.3.3 serial body:

```cpp
for (auto& op : bc.take_pending_ops()) { co_await dispatch_async_op(bc, op); }
```

is replaced by fan-out:

```cpp
dispatch_ops_batch_fanout(bc, bc.take_pending_ops(), main_loop);  // spawn, return
while (bc.inflight_detached > 0 || bc.has_pending_ops() || JS_IsJobPending(bc.rt)) {
    /* drive job queue, suspend on AnyCompletionAwaiter if nothing drivable */
}
```

- `dispatch_async_op_detached(bc, op, main_loop)` — coroutine. Invokes the per-type outcome helper (`run_db_query_outcome` / `run_db_exec_outcome` / `run_audit_write_outcome`) on whatever Drogon thread the PG client resolves on; `loop->queueInLoop`s back to `main_loop` to run `bc.resolve` / `bc.reject`, decrement `bc.inflight_detached`, and fire `bc.signal_completion()`. **Critical Invariant 1 preserved:** `bc.rt` / `bc.ctx` are only touched from the main loop.
- `dispatch_ops_batch_fanout(bc, ops, main_loop)` — regular function (not a coroutine). Spawns each op up to `max_concurrent_async_ops` as fire-and-forget `drogon::async_run` tasks, increments `inflight_detached` per spawn, returns immediately. Back-pressure saturation re-queues the remainder at the head of `pending_ops`; a completing op's `signal_completion()` wakes the outer loop, which re-runs dispatch.
- `AnyCompletionAwaiter` — suspends the outer coroutine until any detached task fires `signal_completion()`. `await_ready()` checks `wake_count > 0`; `await_suspend(h)` stores `h` under `wake_mu`; completion either resumes `waiter_handle` or bumps `wake_count`.

### Revised Cancellation Cascade Step 3

0.3.3's blind drain:

```cpp
co_await drogon::sleepCoro(loop, std::chrono::seconds(5));
```

is replaced by:

```cpp
auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
while (bc.inflight_detached > 0 && std::chrono::steady_clock::now() < deadline) {
    co_await AnyCompletionAwaiter{bc};
}
```

**Rationale.** Naive parallel fan-out opens a `BridgeContext`-UAF window: a detached task can resume `&bc` *after* the outer coroutine has returned and the caller has moved on to `RuntimePool::destroy()`. The wake-driven drain waits for `inflight_detached == 0` (all detached tasks have landed their `queueInLoop` callback and run to completion) before the cascade returns, bounded by the 5 s ceiling. The ceiling is retained because Drogon's PG client cannot preempt a query libpq has already dispatched (accepted trade-off, same posture as 0.3.3).

### `SqlBinderAwaiter` (runtime-binder path)

0.3.3 bridged runtime-sized SQL params via `std::promise`/`std::future`, which momentarily blocked the loop thread. 0.3.3.1 replaces this with `SqlBinderAwaiter : drogon::CallbackAwaiter<Result>` — `await_suspend` sets up `*db << sql`, binds params via the existing `bind_param` helper, and registers row + exception callbacks that resume the coroutine from libpq's IO thread. Call sites in the `_outcome` helpers swap `exec_binder_path(db, op)` for `co_await SqlBinderAwaiter{db, op.sql, op.sql_params}`. ~30 LoC; no Drogon patch. The `#include <future>` is dropped.

### New and modified TUs

- **New:** `src/kernel/js/conversion.{hpp,cpp}` — consolidates JS↔JSON helpers (`js_to_json`, `json_to_js`, `extract_error`) that 0.3.0–0.3.2 had duplicated as anonymous-namespace helpers. Shared-detail, not part of the public contract.
- **Modified:** `src/kernel/js/run_on_context.{hpp,cpp}` — new `dispatch_async_op_detached`, `dispatch_ops_batch_fanout`, `AnyCompletionAwaiter`, `SqlBinderAwaiter` (all file-scope in the TU). `run_cancellation_cascade` Step 3 rewrite.
- **Modified:** `src/kernel/js/bridge_context.{hpp,cpp}` — four parallel-dispatch fields + `signal_completion()` + 3-arg `register_pending`.
- **Modified:** `src/kernel/js/async_op.hpp` — `PromiseCallbacks::ns_for_cancellation`.

### Accepted trade-off (new to 0.3.3.1, no DEFERRED entry)

Wall-clock cancellation cannot preempt a query libpq has already dispatched. Effective enforcement is `wall_clock_limit + longest in-flight query`, bounded by the 5 s cascade ceiling. Same accepted-risk posture as 0.3.3. This is a Drogon-API constraint, not a design choice.

### Tests added in 0.3.3.1

Three cases in `tests/kernel/js/async_bridge_test.cpp`:
- `parallel awaits with runtime params` — two 50 ms `pg_sleep($1)` via `Promise.all`; exercises `SqlBinderAwaiter`.
- `fan-out stress, 8 parallel db.queries` — saturates default `max_concurrent_async_ops = 8`; asserts ordered return + < 300 ms.
- `wall-clock cancel during fan-out settles cleanly` — short `wall_clock_limit` against 8 × 200 ms `pg_sleep`; primary TSan smoke for the inflight-drain.

Group A.4 timing restored to `< 150 ms` (was loosened in 0.3.3 with a `TODO(0.3.3.1)` pointer).

---

## Test-count amendment (0.3.3.3)

**Added by:** RE-EVAL following 0.3.3 §2.7 + 0.3.3.3 test-backfill milestone. The original §Tests enumeration declared 23 cases; post-0.3.3.1 delivery the codebase had 17. 0.3.3.3 closes the gap with five ICD-numbered cases and folds Group C into B.10's coverage.

### Cases delivered in 0.3.3.3

Five new cases in `tests/kernel/js/async_bridge_test.cpp`:

- **B.7** `memory limit tripped between awaits yields MEMORY_LIMIT` — 1 MiB limit + between-await `Array.push` fan, PG-gated.
- **B.8** `CPU time limit tripped between awaits yields CPU_TIME_EXCEEDED` — 50 ms CPU / 5 s wall split, trivial `await` then `for(;;)`, PG-gated.
- **B.9** `CPU limit excludes async wait time` — 50 ms CPU / 5 s wall, `pg_sleep(0.2)` completes successfully; PG-gated (the `__host_sleep_ms__` shim route from the original ICD wording is not exercised here — real async IO is strictly stronger).
- **D.16** `10 concurrent contexts each run independent query` — 10 `std::thread`s each drive `drogon::sync_wait(run_on_context(...))` on its own BridgeContext; pattern mirrors `tests/kernel/capabilities/batch_test.cpp` concurrent-batch.
- **D.17** `10 contexts x 5-query Promise.all fan-out` — 10 threads × 5 × 50 ms `pg_sleep` = 50 in-flight queries; test DB pool sized at 80 (see fixture change).

### Group C (cancellation) — covered end-to-end

**Architect decision, 2026-04-18.** Cases C.12–C.15 are subsumed by B.10 `wall-clock cancel during fan-out settles cleanly`: that test exercises the full cancellation cascade (external cancel during in-flight PG ops, cascade run, clean teardown, ASAN evidence). Discrete per-trigger cases would duplicate coverage. The ICD count tightens **23 → 19 enumerated cases** (A.1–A.6, B.7–B.11, D.16–D.17, E.18–E.22, F.23), plus the 0.3.3.1 bonus (runtime-binder variant of A.4) and the non-ICD ancillary sync-misuse cases. F.23 remains deferred to the 0.5.x TSan CI job per the original ICD.

### Security-constraint tests (0.3.3.3, new files outside async_bridge_test.cpp)

- **ICD-0.3.1 §Security Constraint 4** — `tests/kernel/js/runtime_pool_test.cpp` → `RuntimePool release on cancelled context routes to destroy`. Captures the warn line from `runtime_pool.cpp` defensive-destroy branch (`"ICD-0.3.1 §Security Constraint 4"`); asserts `active_count()==0 && free_count()==0` as secondary proof.
- **ICD-0.3.2 §Security Constraint 5** — `tests/kernel/js/stdlib_test.cpp` → `stdlib: log.* preserves caller-supplied ctx and does not inject kernel fields`. Asserts caller-supplied `extension_id` / `user_id` / `node_id` pass through verbatim and the `ConfigProjection::node_id` is not spliced in.

### Fixture change

`tests/kernel/js/async_bridge_fixture.cpp → test_config()` sets `cfg.db.pool_size = 80` to match the D.17 §Tests specification. Affects the shared test DbClient; raises a ceiling, does not change any semantic default.

### CI posture unchanged

No new CMake option, no new CI job, no new compile flag. `PLINTH_JS_TEST_SHIMS` remains OFF in CI. All seven new cases compile unconditionally; the PG-gated five skip cleanly when PG env vars are unset.

### Bridge classifier uplift (0.3.3.3 scope expansion)

B.7's first CI run (task #12084) failed with `kind = PROMISE_REJECTED_UNHANDLED` (value 9) instead of `MEMORY_LIMIT` (value 2). Root cause: `convert_top_level` in `src/kernel/js/run_on_context.cpp` was surfacing every rejected top-level promise as `PROMISE_REJECTED_UNHANDLED` without inspecting the underlying trigger. Any code wrapped in an async IIFE (which top-level `await` effectively requires in script mode) would lose its ICD-specified classification because QuickJS converts a synchronous throw inside an `async` function body into a promise rejection.

**Fix:** new `detail::classify_rejection(JSContext*, JSValueConst, const BridgeContext&)` in `src/kernel/js/conversion.{hpp,cpp}`, mirroring `extract_error`'s precedence (bc state → name/msg inspection → fallback) but reading from an in-hand `JSValue` rather than the pending exception. `convert_top_level` now routes rejections through it. The change is symmetric across all async-wrapped trips: OOM → `MEMORY_LIMIT`, stack-overflow JS → `STACK_OVERFLOW`, `cancelled` / `cpu_limit_exceeded()` / `wall_clock_exceeded()` → their respective kinds. Non-matching rejections (rejected with `"some string"`, or a plain `new Error("x")`) still surface as `PROMISE_REJECTED_UNHANDLED` with the stringified reason.

The uplift is a scope expansion from 0.3.3.3's original plan (pure test backfill) — recorded here as an accepted deviation on the same footing as 0.3.3.1's parallel-fan-out expansion. Rationale: writing B.7 surfaced the bug; the only test-only fix would have asserted the buggy behavior, which would silently narrow the ICD's B.7 contract.
