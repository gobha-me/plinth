# ICD-0.3.4-cap-call-from-js

**Traces to:** DESIGN-quickjs-bridge.md §§7.2 (Max Concurrent Async Ops), 7.3 (cap.batch Implementation), 8.1 (Capability Dispatch), 9.3 (Implementation Sequence: 0.3.4); architecture/02-capabilities.md §3 (Capability Call Flow)
**Depends on:** ICD-0.3.3-async-bridge (`BridgeContext` async activation, `AsyncOp` / `PromiseCallbacks` / `PromiseRejection` envelopes, `run_on_context` loop, cancellation cascade), ICD-0.2.2-capability-resolution (`call_capability` dispatch + Tier 1/2/3 algorithm, `MAX_CALL_DEPTH=8`, `UserContext` shape), ICD-0.2.4-capability-rbac (`PERMISSION_DENIED` enforcement inside dispatch; `effective_rules` + `kernel.admin` universal match), ICD-0.2.6-async-dispatch (`call_capability_async` coroutine wrapper), ICD-0.3.1-runtime-lifecycle (`BridgeContext::call_depth` / `max_call_depth` field shape)
**Milestone:** 0.3.4 — `cap.call()` and `cap.batch()` JS surface; wires the QuickJS async bridge through the capability registry.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DISCUSSION-streaming-and-media.md §0 (keeps `cap.call()` return shape opaque — this ICD places `cap` on a JS binding surface but MUST NOT narrow the return envelope), `project_plinth_state` v0.2.2 note on Tier-2-miss semantics ("extension rolls into sidecar until the 0.3.x JS bridge lands" — the unroll of `provider_type == "extension"` to live JS→JS dispatch is 0.4.x-scoped; 0.3.4 honors the existing `tier3_not_available` result for extension-typed Tier 2 entries), DEFERRED.md §`db.*` PG-type→JS-type mapping (OID-driven mapping — separate concern; do not fold into this ICD)

---

## Overview

This ICD defines the `cap.*` JS binding surface and the `CAP_CALL`
dispatch arm in the async-bridge coroutine loop. 0.3.3 reserved
`AsyncOp::Type::CAP_CALL` on the enum but the dispatch switch in
`run_on_context.cpp` currently rejects it with
`EvalErrorKind::INTERNAL_ASYNC`; this ICD implements that arm and
the pair of JS bindings that enqueue it.

0.3.4 activates the `call_depth` field ICD-0.3.1 reserved on
`BridgeContext` and introduces a `UserContext` value-copy on
`BridgeContext` so the resolver's existing RBAC enforcement (ICD-0.2.4)
and call-depth enforcement (ICD-0.2.2) run against the calling
identity without threading an extra argument through
`run_on_context`. `call_capability_async` (ICD-0.2.6, already shipped
in 0.3.3's branch) is the entry point consumed here — no new
resolver API lands in 0.3.4.

**Scope:**

- `cap.call(signature, args?) → Promise<any>` JS binding.
- `cap.batch(calls) → Promise<any[]>` JS binding (naive
  `Promise.all([cap.call(...), ...])` expansion).
- `CAP_CALL` dispatch arm in `run_on_context.cpp` — constructs a
  `CapabilityCall` from the op payload, awaits
  `call_capability_async`, resolves/rejects with mapped `CapabilityError`.
- `BridgeContext::user` field (value-copy of `UserContext`; populated
  at pool-entry creation, consulted by the dispatch arm).
- Per-hop `call_depth` propagation through the resolver (the
  dispatcher already enforces `MAX_CALL_DEPTH=8`; 0.3.4 just forwards
  the caller's `bc.call_depth` into the `CapabilityCall` it dispatches).
- Error taxonomy: `cap.not_found`, `cap.permission_denied`,
  `cap.call_depth_exceeded`, `cap.tier3_not_available`,
  `cap.invalid_signature`, `cap.capability_disabled`,
  `cap.cancelled`, `cap.internal`.

**Out of scope (deferred):**

- **Live same-node JS→JS recursion.** When a Tier 2 entry has
  `provider_type == "extension"`, 0.3.4 honors the current resolver
  semantics (`tier3_not_available` — per the 0.2.2 "extension rolls
  into sidecar until 0.3.x JS bridge lands" precedent) and rejects
  with `cap.tier3_not_available`. The unroll — acquiring a
  `BridgeContext` from the target extension's `RuntimePool`,
  copying `UserContext`, incrementing `call_depth`, dispatching via
  `run_on_context`, returning the result — requires the extension
  installer + per-extension `RuntimePool` registry that doesn't
  exist until 0.4.x. A delta ICD or an amendment to this one can
  specify it when the installer lands.
- **Tier 3 sidecar proxy.** 0.8.x. `cap.tier3_not_available` is the
  stable rejection code.
- **`cap.batch()` Tier 3 bundling optimization.** DESIGN §7.3
  explicitly permits the naive `Promise.all`-expansion implementation;
  bundling bundles multiple Tier 3 calls against the same remote
  node into a single HTTP request. 0.8.x scope.
- **Timeout cascade through nested `cap.call`.** Wall-clock and CPU
  continue across a `cap.call`; no per-hop timeout is introduced.
  When 0.4.x wires live JS→JS recursion, the question of whether
  the callee inherits the caller's wall-clock deadline or gets a
  fresh one becomes real; 0.3.4 defers by not recursing.
- **`cap.call` from sync JS context.** Every `cap.call` returns a
  Promise; synchronous execution paths that cannot `await` still
  must. QuickJS doesn't offer a no-`await` bridge, so there is no
  sync variant — and one isn't needed.
- **PG-type → JS-type OID mapping.** Tracked in DEFERRED.md; the
  current heuristic stays.

---

## BridgeContext Additions

One new field, mirroring the `ConfigProjection` value-copy pattern
from ICD-0.3.2:

```cpp
// src/kernel/js/bridge_context.hpp
namespace plinth::js {

struct BridgeContext {
    // ... 0.3.1 / 0.3.2 / 0.3.3 fields (unchanged) ...

    // Value-copy of the caller's UserContext. Populated by
    // RuntimePool::create_entry (from the dispatcher-supplied context)
    // or by a test driver before run_on_context. `auth_type ==
    // "anonymous"` is the baseline (UserContext::anonymous()) — 0.3.4
    // tests that do not synthesize identity hit this.
    //
    // Read only from the CAP_CALL dispatch arm and the 0.3.3 audit
    // arm (already consumes `bc.user.user_id / session_id / ip_address
    // / extension_id / node_id` fields — in 0.3.3 those read from
    // the Extension handle; 0.3.4 unifies them here).
    plinth::capabilities::UserContext user{
        plinth::capabilities::UserContext::anonymous()};
};

}  // namespace plinth::js
```

Notes:

- Value-copy, not pointer. Decouples pool lifetime from whatever
  the caller's context object was. Matches the `ConfigProjection`
  decision in ICD-0.3.2.
- Default-initialized to `UserContext::anonymous()` so a driver or
  test that forgets to set it still produces well-defined RBAC
  behavior (every RBAC-gated capability rejects — see
  `tests/kernel/rbac/anonymous_identity_test.cpp` for the
  safeguard test).
- Set by `RuntimePool::create_entry` after the Extension handle has
  been resolved. The pool ctor gains an optional `UserContext*`
  argument (nullable — tests that acquire contexts without identity
  get the anonymous baseline; production dispatch paths pass the
  caller's context).
- **Not mutated across a `cap.call`.** Identity is the caller's
  throughout. If 0.4.x adds live JS→JS recursion, the callee's
  BridgeContext receives the *same* `UserContext` value-copy so the
  callee's audit events + RBAC checks attribute to the caller.

`call_depth` and `max_call_depth` already exist on `BridgeContext`
(ICD-0.3.1 §BridgeContext Contract) with `max_call_depth = 8` default.
0.3.4 reads `bc.call_depth` at dispatch time and forwards it to the
`CapabilityCall`. The dispatcher then enforces
`call_depth >= MAX_CALL_DEPTH` — a single source of truth per
ICD-0.2.2 §Call Depth Tracking.

---

## AsyncOp CAP_CALL Payload

The `AsyncOp::Type::CAP_CALL` variant is already reserved on the
enum (`src/kernel/js/async_op.hpp:59`). 0.3.4 adds two string-valued
fields to the payload:

```cpp
// src/kernel/js/async_op.hpp
struct AsyncOp {
    // ... 0.3.3 fields (unchanged) ...

    // CAP_CALL payload.
    std::string cap_signature;   // "ns:v:fn" — forwarded verbatim to the resolver
    Json::Value cap_args;        // forwarded verbatim; any JSON shape permitted
};
```

`cap_args` is a `Json::Value` (not `std::vector<Json::Value>` like
`sql_params`). `cap.call()` takes a single optional `args` parameter
per DESIGN §3.3 / §8.1 — it is NOT a variadic spread. Callers that
need multiple conceptual inputs pass an object or array.

`call_depth` lives on `BridgeContext`, not on `AsyncOp`. The dispatch
arm reads `bc.call_depth` when it constructs the `CapabilityCall`;
the op payload does not need to carry it. This mirrors the 0.3.3
decision to thread CPU-time / wall-clock state through `BridgeContext`
rather than each op.

---

## Injected `cap.*` Surface

```
cap.call(signature: string, args?: any) -> Promise<any>
cap.batch(calls: Array<[signature: string, args?: any]>)
    -> Promise<Array<any>>
```

Both bindings live in `src/kernel/js/stdlib/cap_bindings.cpp`
(new file), registered from `inject_kernel_stdlib` alongside the
0.3.2 / 0.3.3 surface. Registration mirrors the
`register_db(ctx) / register_audit(ctx)` pattern already in
`src/kernel/js/stdlib_inject.cpp`.

### `cap.call()`

Argument rules:

- `signature` (arg 0): required, non-empty string. Violation → sync
  `TypeError` (thrown, not rejected — matches ICD-0.3.2 §Error Model
  for sync misuse).
- `args` (arg 1, optional): any JSON-serializable JS value (null,
  boolean, number, string, array, plain object, Uint8Array).
  Converted via the `js_to_json` helper ICD-0.3.3.1 consolidated in
  `src/kernel/js/conversion.{hpp,cpp}`. Non-serializable values
  (function, Symbol, BigInt outside safe-integer range, Date) →
  sync `TypeError`. If `args` is omitted, the resolver receives
  `Json::Value()` (null).

Return value: a Promise that resolves to whatever
`CapabilityResult::data` contains for the target capability, or
rejects per §Error Mapping below. **The return is deliberately opaque**
— per DISCUSSION-streaming-and-media §0, any future streaming
variant (`cap.stream`?) is its own API; this one does not narrow the
shape.

### `cap.batch()`

Argument rules:

- `calls` (arg 0): required, non-empty Array. Each element MUST be a
  2-element Array `[signature, args?]` where `signature` is a
  non-empty string and `args` is optional JSON-serializable. A
  shorter or longer inner array, or a non-array outer/inner shape,
  throws sync `TypeError`. Empty outer array resolves with `[]` —
  consistent with `Promise.all([])`.

Semantics:

- Initial implementation is a **naive expansion** to
  `Promise.all([cap.call(sig, args), ...])`. The C++-side expansion
  lives in `cap_bindings.cpp` and enqueues one `AsyncOp::CAP_CALL`
  per entry; the order of the resolved array matches the input order
  (FIFO delivery through the async bridge, same as `db.query`
  ordering in 0.3.3's Group A.4 test).
- **Fail-fast.** If any element rejects, the whole batch's outer
  promise rejects with the first rejection's shape — same Promise.all
  semantics. Other elements may still complete; their resolutions
  are discarded. This matches the 0.2.5 `batch_call_capability`
  precedent (`failed_index` + `calls[i..n]` values discarded).
- **`failed_index` on the JS surface:** 0.2.5 shipped a
  `BatchResult::failed_index` field in the C++ dispatcher; 0.3.4
  does NOT surface this field on the JS rejection shape. JS Promise
  rejection only carries `{code, message}`. The index is
  discoverable by hand on the JS side (the caller knows the input
  array order). Tracked as an accepted deviation on the same footing
  as 0.2.5's `BatchResult::failed_index` deviation — a surface
  enhancement, not a security constraint.
- **Tier 3 bundling optimization is out of scope** (DESIGN §7.3;
  0.8.x).

---

## Binding Implementation Rules

Each of the two bindings is a `JSCFunction` that runs synchronously on
the QuickJS thread. The pattern follows `db_bindings.cpp` (the
canonical 0.3.3 example):

```cpp
JSValue js_cap_call(JSContext* ctx, JSValueConst /*this_val*/,
                    int argc, JSValueConst* argv) {
    // 1. Sync arg validation — throws JS exception on TypeError.
    //    Uses the same pattern as db.query's convert_param helpers,
    //    adapted for (string, optional any).
    std::string signature = extract_required_string(ctx, argc, argv, 0, "signature");
    Json::Value args_json =
        (argc >= 2 && !JS_IsUndefined(argv[1]))
            ? detail::js_to_json(ctx, argv[1])
            : Json::Value();  // null

    // 2. Create promise + callbacks.
    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);

    // 3. Register callbacks + enqueue op.
    auto* bc = static_cast<BridgeContext*>(JS_GetContextOpaque(ctx));
    int cb_id = bc->register_pending(resolve_funcs[0], resolve_funcs[1], "cap");
    bc->pending_ops.push_back(AsyncOp{
        .type = AsyncOp::Type::CAP_CALL,
        .callback_id = cb_id,
        .cap_signature = std::move(signature),
        .cap_args = std::move(args_json),
    });

    return promise;  // refcount-1; caller owns
}
```

`cap.batch()` expansion:

```cpp
JSValue js_cap_batch(JSContext* ctx, JSValueConst /*this_val*/,
                     int argc, JSValueConst* argv) {
    // 1. Validate `calls` is an Array; iterate and unpack each
    //    [signature, args?] tuple into its own CAP_CALL AsyncOp.
    //    Empty array → resolve with [].
    // 2. Create ONE outer Promise; the naive implementation lets JS
    //    do the Promise.all wiring via a small host helper — the
    //    simplest path is to build `Promise.all([cap.call(...), ...])`
    //    as a JS array and invoke it through QuickJS's C API.
    // ... (details in src/kernel/js/stdlib/cap_bindings.cpp)
}
```

Implementation latitude on the `cap.batch` expansion point: either
do the fan-out in C++ (N `cap.call` enqueues, plus an aggregator
coroutine) or in JS via `Promise.all`. Either is acceptable for
0.3.4; the CI test suite asserts the externally observable behavior,
not the internal arm. The default expectation is the
JS-`Promise.all` form because it reuses the `cap.call` path directly.

`"cap"` is the new `ns_for_cancellation` value passed to
`register_pending`. The cancellation cascade (ICD-0.3.3 §Cancellation
Cascade) already rejects callbacks with the per-namespace prefix;
`cap.*` slots in alongside `db.*` and `audit.*`.

---

## CAP_CALL Dispatch Arm

Goes in `run_on_context.cpp`'s `dispatch_async_op_detached`
switch — the arm currently returning `INTERNAL_ASYNC` at the
"reserved variants" fall-through. Pseudocode:

```cpp
case AsyncOp::Type::CAP_CALL: {
    plinth::capabilities::CapabilityCall cc{
        .signature  = op.cap_signature,
        .args       = op.cap_args,
        .call_depth = bc.call_depth,  // caller's depth — resolver enforces
    };

    auto resolve_result =
        co_await plinth::capabilities::call_capability_async(cc, bc.user);

    if (resolve_result.has_value()) {
        outcome = resolve_result->data;   // CapabilityResult::data is
                                          // a Json::Value
    } else {
        outcome = std::unexpected(
            detail::capability_error_to_rejection(resolve_result.error()));
    }
    break;
}
```

Runs inside the detached-task path that 0.3.3.1 introduced. The
outcome — `std::expected<Json::Value, PromiseRejection>` — flows
through the same `queueInLoop` landing that `run_db_query_outcome`
and friends use; `bc.resolve` / `bc.reject` fire on the main loop.
**Critical Invariant 1** (`bc.rt` / `bc.ctx` touched only from the
main loop) is preserved by construction — the dispatch arm reads
`bc.user` and `bc.call_depth` inside the detached task, but both are
read-only snapshots at that point; the main-loop invariant applies to
QuickJS state, not to the `UserContext` value-copy.

`call_capability_async` is the coroutine wrapper ICD-0.2.6 introduced.
Its signature (`const UserContext&`) and `drogon::Task<ResolveResult>`
return shape are stable per that ICD.

The dispatcher internally handles call-depth enforcement, Tier 1
handler invocation, Tier 2 cache lookup, RBAC check against
`bc.user.effective_rules`, and the audit-on-denial path. 0.3.4
contributes zero new resolver logic — everything is consumed
through the existing entry point.

---

## Error Mapping

`CapabilityError` → `PromiseRejection{code, message, sqlstate=nullopt}`.
The `code` mirrors the variant's `error_code()` string (see
`src/kernel/capabilities/error.cpp`), prefixed with `cap.`. The
`message` is a short human-readable string including the signature
the resolver was asked for.

| `CapabilityError` | `cap.*` rejection code | When |
|---|---|---|
| `CAPABILITY_NOT_FOUND` | `cap.not_found` | Signature not registered; Tier 2 cache miss with no Tier 1 match. |
| `PERMISSION_DENIED` | `cap.permission_denied` | 0.2.4 RBAC check — caller's `effective_rules` do not include the capability's `rbac_rule`. Audit event `capability.rbac.denied` is emitted by the dispatcher (not the bridge) per ICD-0.2.4. |
| `CALL_DEPTH_EXCEEDED` | `cap.call_depth_exceeded` | `call_depth >= MAX_CALL_DEPTH` on entry. |
| `TIER3_NOT_AVAILABLE` | `cap.tier3_not_available` | Tier 2 entry with `provider_type ∈ {sidecar, extension}`. Extension case unrolls in 0.4.x; sidecar case unrolls in 0.8.x. |
| `CAPABILITY_DISABLED` | `cap.capability_disabled` | `enabled = false` on the cached entry. |
| `INVALID_CAPABILITY` | `cap.invalid_signature` | Signature failed `parse_signature` (ICD-0.2.1). |
| Any other variant | `cap.internal` | Registration-time validation variants (INVALID_NAMESPACE, INVALID_VERSION, …) should never surface at dispatch time; if they do, the bridge rejects as `cap.internal`. |

`sqlstate` is not populated — `cap.*` rejections have no PG origin.
The optional field on `PromiseRejection` remains `std::nullopt`.

A helper `detail::capability_error_to_rejection(CapabilityError)`
lives in `src/kernel/js/stdlib/cap_bindings.cpp` alongside the
bindings. It is NOT moved to `conversion.{hpp,cpp}` — the mapping
is capability-specific, not a shared JS↔JSON primitive.

### Cancellation during in-flight cap.call

The 0.3.3 cancellation cascade (§Cancellation Cascade Step 4)
rejects every outstanding callback with a per-namespace code.
`cap.*` callbacks were registered with `ns_for_cancellation = "cap"`,
so they reject with `{code: "cap.cancelled", message: "execution
cancelled"}`. No change to the cascade's structure is needed —
adding `"cap"` as a known namespace is a one-line extension to the
existing ns → code map in `run_on_context.cpp`.

---

## Type Conversion — Additions Beyond 0.3.3

- **JS → `Json::Value` for `cap.call` args (recursive).** Reuses the
  `detail::js_to_json` consolidated helper from 0.3.3.1's
  `conversion.{hpp,cpp}`. Null, boolean, number, string, Array,
  plain object, Uint8Array all supported; Uint8Array produces a
  `Json::Value` of base64-encoded string (matches the
  `audit.log` payload convention introduced in 0.3.3 — see
  `audit_bindings.cpp`). Date, function, Symbol, and BigInt outside
  safe-integer range → sync `TypeError`.
- **`Json::Value` → JS for `cap.call` result.** Reuses
  `detail::json_to_js`. Returns whatever shape the handler returned;
  the opacity invariant is preserved.
- **Array unpacking for `cap.batch` inputs.** One 2-element JS Array
  per call. The outer array's length is bounded only by
  `max_concurrent_async_ops` (back-pressure, not rejection — the
  bridge queues beyond the limit per ICD-0.3.3 §Back-Pressure).

---

## Security Constraints (Non-Negotiable)

1. **`BridgeContext::call_depth` is the single source of truth.**
   The `AsyncOp::CAP_CALL` payload does NOT carry a `call_depth`
   field. The dispatch arm reads `bc.call_depth` when it builds the
   `CapabilityCall`. A binding that tried to let JS override the
   depth (via args or a side channel) would be a depth-enforcement
   bypass; there is no such path.
2. **RBAC enforcement is the resolver's responsibility.** 0.3.4
   contributes no RBAC check of its own. Every `cap.call` flows
   through `call_capability_async`, which runs the ICD-0.2.4 check
   inside its own shared-lock window. A binding that pre-checks
   `effective_rules` and then dispatches is forbidden — the
   cache / DB view inside the resolver is authoritative.
3. **`UserContext` is immutable across dispatch.** The callee sees
   the caller's `user_id`, `username`, `auth_type`,
   `effective_rules`, `session_id`, `ip_address`. No JS-visible
   shim may modify these values before dispatch. The
   `BridgeContext::user` field is private to the C++ surface; it is
   never exposed to JS (no `cap.whoami()`, no `cap.impersonate()`).
4. **Per-hop cancellation is honored.** When the cancellation cascade
   fires during an in-flight `cap.call`, the callback rejects with
   `cap.cancelled` and no further JS execution observes the result.
   If 0.4.x adds live JS→JS recursion, the callee's own
   `BridgeContext::cancelled` flag must be flipped by the caller's
   cascade; that propagation is out of scope for 0.3.4 but this
   constraint is the contract that the 0.4.x amendment inherits.
5. **`cap.batch` preserves per-call failure shape.** A single
   rejection rejects the whole outer promise with that rejection's
   `{code, message}`; other elements' resolutions are discarded.
   No silent-drop of individual failures.
6. **Running `cap.*` on a cancelled `BridgeContext` rejects
   synchronously.** Mirrors ICD-0.3.3 §Security Constraint 7: if
   `bc.cancelled` is set when a binding is invoked, the binding MUST
   reject the new op's promise with
   `{code: "cap.cancelled", message: "execution cancelled"}` and
   NOT enqueue the op.

---

## Performance Targets

Informational, not gated. Measured on the CI builder image with the
runtime pre-initialized.

- `cap.call("<kernel Tier 1 stub>")` round trip: **≤ 200 μs** —
  dominated by the JS↔C++ boundary; resolver dispatch is in-memory.
- `cap.call("<Tier 2 cache hit>")` round trip: **≤ 250 μs** — adds
  one `shared_mutex` read and one cache lookup.
- `cap.batch([n calls])` for `n ≤ max_concurrent_async_ops`:
  **≤ n × 250 μs + 100 μs** (parallel-ish fan-out).
- Bridge CAP_CALL dispatch-arm overhead alone: **≤ 50 μs**
  (matches the 0.3.3 mock async-op target).

If benchmarks matter, follow the 0.2.6.2 precedent: a later PR
adds a `benchmarks/cap_call_benchmark.cpp` harness gated behind
`-DPLINTH_BENCHMARKS=ON`.

---

## What Must Not Be Decided Yet

- **Live same-node JS→JS recursion.** The 0.2.2 memory note
  "extension rolls into sidecar until 0.3.x JS bridge lands" is
  refined: the bridge lands in 0.3.3; JS→JS dispatch lands in 0.4.x
  alongside the extension installer. 0.3.4 rejects extension-typed
  Tier 2 entries with `cap.tier3_not_available`. The unroll
  amendment belongs to the 0.4.x ICD that introduces the installer.
- **`cap.batch` Tier 3 bundling.** DESIGN §7.3 optimization; 0.8.x.
- **Per-call timeout override.** `cap.call("ns:v:fn", args, {timeout_ms})`
  is not added. Wall-clock and CPU continue from the caller's
  `BridgeContext`. Adding a per-call override would require
  reasoning about nested deadlines and is deferred until a concrete
  caller demands it.
- **`cap.stream()`.** DISCUSSION-streaming-and-media §0 explicitly
  keeps `cap.call` opaque so a future streaming variant is a
  separate API. 0.3.4 does NOT introduce any streaming surface.
- **JS-visible `failed_index` on `cap.batch` rejection.** 0.2.5's
  C++ dispatcher carries it; 0.3.4 does not surface it via JS. A
  caller recovers the index by correlating with the input array.
- **`cap.whoami()` / identity introspection.** The caller's
  `UserContext` is kernel-internal. If an extension genuinely needs
  its own identity at runtime, a separate sync binding (e.g.
  `log.getContext()` or a `cap.whoami()` capability registered via
  the normal kernel Tier 1 path) is the right venue. No shortcut
  through `BridgeContext::user` exposure.
- **OID-driven PG type mapping for handlers invoked via cap.call.**
  Tracked in DEFERRED.md (§`db.*` PG-type→JS-type mapping). 0.3.4
  passes `Json::Value` through verbatim; any PG-side conversion
  questions belong to the underlying `db.*` bindings.

---

## Milestone Criteria

All test groups below MUST pass under Catch2 before 0.3.4 ships.
Mirrors DESIGN §9.3 *0.3.4 Tests* and layers in the security-constraint
coverage this ICD adds.

### Tests

Test cases join `tests/kernel/js/async_bridge_test.cpp` alongside the
0.3.3 A–F groups. Test driver builds a `BridgeContext`, sets `bc.user`
(using `UserContext{...}` literals or `anonymous_with_rules`), then
calls `run_on_context` inside a Drogon coroutine. Uses the same
fixture as 0.3.3 (`async_bridge_fixture.cpp`).

**Group G — Correctness (DESIGN §9.3 0.3.4 bullet 1–2):**

G.24. `cap.call("kernel:1:log", {msg: "hi"})` with
`bc.user.effective_rules = ["kernel.log"]` resolves to whatever
the Tier 1 stub returns (`{not_implemented: "kernel:1:log"}` per
ICD-0.2.2 §Deviation 2) — asserts the resolver dispatch path, not
the handler's content.

G.25. `cap.call("does.not:1:exist")` rejects with
`{code: "cap.not_found"}`.

G.26. `cap.call("bad_signature")` (no version / function) rejects
with `{code: "cap.invalid_signature"}`.

**Group H — RBAC (DESIGN §9.3 0.3.4 bullet 3):**

H.27. `cap.call("kernel:1:log", {})` with empty `effective_rules`
rejects with `{code: "cap.permission_denied"}`. The dispatcher
emits `capability.rbac.denied` to the audit log (asserted via the
existing `capture_audit` test hook from ICD-0.2.4).

H.28. Same call with `effective_rules = ["kernel.admin"]` resolves
(universal-match path).

**Group I — Depth (DESIGN §9.3 0.3.4 bullet 4):**

I.29. A `bc.call_depth = MAX_CALL_DEPTH` context calling
`cap.call("kernel:1:log", {})` rejects with
`{code: "cap.call_depth_exceeded"}`. The test sets `call_depth` on
the driver-owned BridgeContext; the resolver enforces the cap.

**Group J — Batch (DESIGN §9.3 0.3.4 bullet 5):**

J.30. `cap.batch([])` resolves to `[]`.

J.31. `cap.batch([["kernel:1:log", {msg: "a"}], ["kernel:1:log",
{msg: "b"}]])` with the appropriate rules resolves to a 2-element
array preserving input order.

J.32. `cap.batch([["kernel:1:log", {}], ["does.not:1:exist"]])`
rejects with `{code: "cap.not_found"}` (the second element's
failure); the first element's resolution is discarded.

**Group K — Concurrency + back-pressure:**

K.33. 10 parallel `cap.call`s via `Promise.all([cap.call(...), ...])`
all resolve; runs within
`max_concurrent_async_ops = 8` back-pressure (measured by the same
in-bridge counter used in ICD-0.3.3 Test B.11).

**Group L — Cancellation:**

L.34. In-flight `cap.call` during wall-clock timeout rejects with
`{code: "cap.cancelled"}` and the full cascade runs clean (no leak
under ASAN, no `PROMISE_RESOLVE_AFTER_CANCEL`). Mirrors the 0.3.3
B.10 pattern, extended to cap.* namespace.

**Group M — Security constraint assertions (ICD-cited):**

M.35. `bc.user.user_id` is NOT exposed to JS. A test attempts
`globalThis.cap.whoami?.()` and asserts `undefined`; separately
verifies no `cap.*` binding returns identity data. Proves
Security Constraint 3's non-leak.

M.36. Running `cap.call` on a `bc` with `bc.cancelled = true` set
before invocation rejects synchronously without enqueuing the op
(pending_ops size unchanged). Proves Security Constraint 6.

### CI Wiring

- `src/kernel/js/stdlib/cap_bindings.cpp` — **new**. Two bindings +
  the `capability_error_to_rejection` helper.
- `src/kernel/js/stdlib_inject.{hpp,cpp}` — add `register_cap(ctx)`
  to the `inject_kernel_stdlib` list alongside `register_db` /
  `register_audit`.
- `src/kernel/js/async_op.hpp` — add `cap_signature` + `cap_args`
  fields to `AsyncOp`. No enum-layout change; `CAP_CALL` variant
  already exists.
- `src/kernel/js/bridge_context.{hpp,cpp}` — add `user` field
  (value-copy `UserContext`). Update
  `RuntimePool::create_entry` to populate from the ctor argument
  (nullable pointer → `anonymous()` default). `RuntimePool` ctor
  gains one optional argument.
- `src/kernel/js/run_on_context.cpp` — implement the CAP_CALL arm
  in the detached-dispatch switch. Add `"cap"` to the
  `ns_for_cancellation` → code map in the cancellation cascade.
- Test cases added to `tests/kernel/js/async_bridge_test.cpp` (Groups
  G–M). No new test file; no new CI job.
- No schema change. No CMake option. `PLINTH_JS_TEST_SHIMS` stays
  OFF; the new tests use real PG (PG-gated where relevant).

---

## Entry / Exit

**Entry:** ICD-0.3.3-async-bridge implementation merged (including
the 0.3.3.1 parallel-dispatch amendment and 0.3.3.3 test backfill);
`call_capability_async` is live (0.2.6 shipped alongside 0.3.3).
ROADMAP `0.3.4` entry is `[strong]` — no further blocker.

**Exit:** all Group G–M cases pass under Catch2 default and
sanitizer builds; `run-clang-tidy-20` clean on
`src/kernel/js/**` and `src/kernel/capabilities/**`; CHANGELOG
entry describes shipped surface, any deviations, and this ICD's
§What Must Not Be Decided Yet items folded into DEFERRED.md or
marked for 0.4.x; ROADMAP `0.3.4` line removed per the completed-
milestones-are-removed rule.

---

## Implementation deviation (0.3.4.1 memory-limit peak tracking)

*Folded back into this ICD by RE-EVAL-0.3.x-arc-closeout §§2.1, 2.2.
The 0.3.4.1 four-part follow-up shipped three fixes for a teardown-
race / classifier-accuracy cascade documented in
`project_ws_flaky_segfault.md` §§Fourth–Fifth occurrence. The surface
below is load-bearing for the `async_bridge` test suite (closes test
264 `MEMORY_LIMIT` reclassification) and for the test fixture
lifecycle; re-listing in this ICD so a reader of 0.3.4's contract
does not have to consult the CHANGELOG for the complete shape.*

### New `BridgeContext` field

In addition to the `user` value-copy field introduced in 0.3.4, the
0.3.4.1 fix adds:

```cpp
struct BridgeContext {
    // ... 0.3.1 / 0.3.2 / 0.3.3 / 0.3.4 fields (unchanged) ...

    // OOM peak-tracking latch. Set by `sample_memory_peak` when
    // `JS_ComputeMemoryUsage` reports `malloc_size` is within
    // 256 KiB of the runtime's `malloc_limit`. Read by
    // `was_memory_limit_hit` from the async classifier and the
    // sync `extract_error` path as the primary OOM-upgrade signal
    // (replaces the string-scan heuristic as primary; that scan
    // stays as a secondary safety net). Cleared by
    // `RuntimePool::release()` on pool recycle so state does not
    // bleed across executions.
    std::atomic<bool> memory_limit_hit{false};
};
```

### New helpers

`src/kernel/js/conversion.{hpp,cpp}` gains three helpers:

```cpp
auto sample_memory_peak(BridgeContext& bc) -> void;
auto was_memory_limit_hit(BridgeContext& bc) -> bool;
auto is_runtime_near_memory_limit(BridgeContext& bc) -> bool;
```

Sampling sites:

- `plinth_js_interrupt_cb` in `runtime_pool.cpp` calls
  `sample_memory_peak` on every tick (10,000-bytecode cadence). This
  catches allocation bursts that fit inside one interrupt window.
- `drive_jobs` in `run_on_context.cpp` calls `sample_memory_peak`
  after every `JS_ExecutePendingJob`. This catches allocation bursts
  in async bodies that a pre-interrupt sample misses.

`classify_rejection` and `extract_error` both consult
`was_memory_limit_hit` as the primary post-hoc upgrade path before
falling back to the "out of memory" / "stack overflow" substring
scan on the `JS_ToCString` fallback.

### Fixture split — `async_bridge_fixture.{hpp,cpp}`

`tests/kernel/js/async_bridge_fixture.cpp` is split into two entry
points sharing a single `std::call_once` (exclusivity holds per
Catch2 per-`TEST_CASE`-subprocess execution model):

- `ensure_drogon_running()` — starts Drogon **WITHOUT** a DbClient.
  Used by `cap.*`, `bc.*`, audit-validation-only, and
  concurrency-edge tests. Removing the DbClient eliminates the
  Drogon-internal heartbeat's `queueInLoop` work that raced with
  `atexit` drain, closing the `bad_weak_ptr` cascade for those
  subprocesses.
- `ensure_drogon_with_db_running()` — starts Drogon **AND** calls
  `createDbClient()` against the test PG instance. Used by tests
  that actually drive PG (`reset_schema` + `db.query` / `audit.log`
  writes).

13 PG-backed tests migrated to the `_with_db_` variant; 13 no-DB
tests kept the lean path. Both routes share one `std::call_once` so
only one wins per subprocess.

### Audit path shutdown gate — `plinth::log::shutdown()`

New public `plinth::log::shutdown()` in
`src/kernel/logging.{hpp,cpp}` (release-store `false` on the existing
`g_audit_ready` atomic). The `async_bridge_fixture` + `ws_test_fixture`
`atexit` handlers call it **before** `drogon::app().quit()`. Late
`plinth::log::audit()` calls during the Drogon drain short-circuit
before touching any Drogon state, closing the audit sub-path that
0.3.3.1's `ConnectionRegistry::initiate_shutdown()` didn't cover.
Documented as a pattern in ICD-0.1.7's Implementation Notes footer.

### Status

Net effect on CI: the post-0.3.4 cascade of 6 tests dropped to 2 after
the first commit (audit gate only), then to zero after the full
three-fix bundle. Local `ctest --repeat until-fail:20 -R "cap|bc.user"`
is 100% on the fix branch.

**No architectural change to the cap.* contract in 0.3.4.** The
deviation is in the plumbing around the contract (identity path,
fixture lifecycle, OOM classification). The 0.3.4-shipped surface
(`cap.call`, `cap.batch`, `CAP_CALL` dispatch arm, `BridgeContext::user`,
error taxonomy) is unchanged by 0.3.4.1.
