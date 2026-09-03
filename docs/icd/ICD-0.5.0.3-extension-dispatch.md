# ICD-0.5.0.3-extension-dispatch

**Traces to:** architecture/02-capabilities.md §3 (Capability Call Flow
— Tier 1/2/3 resolution; this ICD pins the long-absent Tier 2
extension dispatch arm); architecture/05-extensions.md §1.2 (Package
structure — `server/main.js` entry point, `server/handlers/<fn>.js`
convention this ICD promotes from fixture observation to normative
contract); architecture/05-extensions.md §3 (Runtime Limits +
Supervision — per-extension RuntimePool scope); DESIGN-quickjs-bridge.md
§3.2 (Runtime Pool — per-extension pools); DESIGN-quickjs-bridge.md
§8.1 (Capability Dispatch — the "JS→JS recursion" lane 0.3.4 deferred
and this ICD unblocks).
**Depends on:** ICD-0.2.2-capability-resolution (Tier 2 cache shape,
`dispatch_tier2` dispatch point, `CapabilityError` taxonomy,
`MAX_CALL_DEPTH=8` enforcement, `call_capability` /
`call_capability_async` entry points); ICD-0.2.4-capability-rbac
(permission check runs at the resolver before dispatch — contract
unchanged by this ICD); ICD-0.2.6-async-dispatch
(`call_capability_async` coroutine wrapper — the sole entry point
the extension lane consumes); ICD-0.3.1-runtime-lifecycle
(`BridgeContext` field layout, `RuntimePool` acquire/release/destroy
contract, per-extension pool scope); ICD-0.3.3-async-bridge
(`run_on_context` coroutine loop, `PromiseCallbacks`,
`PromiseRejection` shape, cancellation cascade, detached-dispatch
fan-in); ICD-0.3.4-cap-call-from-js (`AsyncOp::CAP_CALL` payload
shape — `cap_signature`, `cap_args`, `cap_user`, `cap_call_depth`;
`BridgeContext::user` value-copy; §49–60 explicit deferral this ICD
resolves); ICD-0.4.4-package-install-lifecycle (capability
registration sites, `{data_dir}/extensions/{name}/{version}/` asset
layout, DISABLED / ACTIVE / UPGRADING state transitions — the hook
points this ICD consumes).
**Milestone:** 0.5.0.3 — Tier 2 extension capability dispatch. Paper
session authoring this contract; code session follows as 0.5.0.4.
Both are four-part follow-ups, untagged per `feedback_tagging_rule.md`
— prior-arc tech debt, not a new product milestone.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:**
[src/kernel/capabilities/resolution.cpp:313–344](../../src/kernel/capabilities/resolution.cpp)
(the `dispatch_tier2` function and its comment block pinning this
ICD as the 0.3.x replacement for the `TIER3_NOT_AVAILABLE` stub);
[src/kernel/capabilities/resolution.hpp:193–201](../../src/kernel/capabilities/resolution.hpp)
(`CachedCapability` — already carries `extension_name` + `scope` +
`rbac_rule` — zero schema churn here);
[src/kernel/js/bridge_context.hpp:69–138](../../src/kernel/js/bridge_context.hpp)
(`BridgeContext` — `extension_name` at :83, `call_depth` at :100,
`user` value-copy at :136; all three fields already shaped for this
use);
[src/kernel/js/runtime_pool.hpp:68–137](../../src/kernel/js/runtime_pool.hpp)
(`RuntimePool` per-extension pool — ctor takes `const Extension* ext`
+ optional `UserContext* user`, matches the per-dispatch identity
propagation this ICD pins);
[src/kernel/js/run_on_context.cpp:447–469](../../src/kernel/js/run_on_context.cpp)
(`run_cap_call_outcome` — current JS-side CAP_CALL arm; becomes a
correct dispatch path automatically once the resolver's extension
branch stops returning `TIER3_NOT_AVAILABLE`);
[src/kernel/js/stdlib/cap_bindings.cpp:92–159](../../src/kernel/js/stdlib/cap_bindings.cpp)
(`cap.call` — already snapshots caller `user` + `call_depth` onto the
op; no binding-side change for this ICD);
[src/kernel/ws/call_dispatch.cpp:65–111](../../src/kernel/ws/call_dispatch.cpp)
(WS `on_call` — currently sync `call_capability`; this ICD requires
migration to `co_await call_capability_async` for the extension lane);
[src/kernel/packages/install_lifecycle.cpp:842](../../src/kernel/packages/install_lifecycle.cpp),
[:930](../../src/kernel/packages/install_lifecycle.cpp),
[:1891](../../src/kernel/packages/install_lifecycle.cpp)
(three existing `provider_type = "extension"` registration sites;
runtime-registry hooks attach here);
[src/kernel/packages/install_lifecycle.cpp:1869](../../src/kernel/packages/install_lifecycle.cpp)
(`{data_dir}/extensions/{name}/{version}/` layout — the
`server/handlers/<fn>.js` path resolves under this root);
[src/kernel/js/stdlib/pubsub_bindings.cpp:154–167](../../src/kernel/js/stdlib/pubsub_bindings.cpp)
(the extension-identity-gate pattern this ICD's handler invocation
relies on — the acquired BridgeContext must carry the target
extension's name so the callee's `pubsub.publish` succeeds);
[tests/fixtures/packages/valid-full/server/handlers/read.js](../../tests/fixtures/packages/valid-full/server/handlers/read.js)
(fixture demonstrating the `export default function <fn>()` handler
convention — promoted here to a contract);
[docs/icd/ICD-0.3.4-cap-call-from-js.md §49–60](ICD-0.3.4-cap-call-from-js.md)
(explicit deferral this ICD picks up);
[docs/icd/ICD-LH-1-listen-notify-storm.md §4.5](ICD-LH-1-listen-notify-storm.md)
(the blocked caller that gates on this ICD's implementation landing).

---

## Overview

Tier 2 capability resolution has a stub arm that rejects every
`provider_type == "extension"` entry with `TIER3_NOT_AVAILABLE`. The
arm was carved in 0.2.2 with a note — *"the 0.3.x JS bridge will
replace this branch with the real bridge call"*
([resolution.cpp:313–318](../../src/kernel/capabilities/resolution.cpp))
— but 0.3.0–0.3.6 shipped without that replacement, and ICD-0.3.4
explicitly deferred "live same-node JS→JS recursion" to a later ICD
pending the extension installer (§49–60). The installer landed in
0.4.4; the replacement did not. This ICD is the replacement.

0.5.0.3 contributes **one design contract**, not one code landing:
the paper authoring slot per `feedback_icd_horizon.md`. The 0.5.0.4
implementation session codes against this contract.

**What this ICD unblocks:**

- **LH-1** — the ICD-LH-1 §4.5 driver extension + `pubsub.publish`
  handler can be reached via WS `call lh1storm:1:burst`. The 2026-04-22
  smoke run (1.1M calls / 20s, zero handler invocations, zero emits)
  that paused the LH-1 implementation becomes runnable.
- **0.5.2 WS broker's client-side dispatch** — when clients invoke a
  capability via the broker, the same resolver path runs; extension
  capabilities can be reached.
- **0.6a admin-extension → admin-extension calls** — future
  admin-panel features that `cap.call` into another extension's
  handler work.
- **The general `cap.call` from JS path** — already shipped in 0.3.4
  for the Tier 1 kernel-stub lane, becomes usable for the
  extension-to-extension lane it has always advertised.

**Scope:**

- Replace `dispatch_tier2`'s extension branch with a dispatch into a
  per-extension `RuntimePool` that invokes the installed
  `server/handlers/<fn>.js` handler.
- Introduce a process-lifetime `plinth::extensions::RuntimeRegistry`
  owning `RuntimePool` instances keyed by extension name. Install
  lifecycle creates/destroys/rebuilds entries at ACTIVE / DISABLED /
  UPGRADING transitions.
- Migrate WS `on_call` from the sync `call_capability` path to
  `co_await call_capability_async` so extension dispatch has a path
  to the coroutine fan-in. Sync `call_capability` keeps working for
  Tier 1 kernel stubs; the extension arm of the sync path rejects
  with a new `cap.async_required` code.
- Pin the handler-file convention (`server/handlers/<fn>.js`, ES
  module with default export whose name matches `<fn>`) as normative.
- Extend the `cap.*` error taxonomy with five extension-specific
  rejection codes: `cap.extension_not_loaded`,
  `cap.handler_not_found`, `cap.handler_load_failed`,
  `cap.handler_threw`, `cap.async_required`.
- Test plan (R.*/E.*/P.*) verifiable without LH-1 — a fixture driver
  extension installed via the standard `POST /api/packages` path
  proves the end-to-end contract.

**Out of scope (deferred):**

- **Cross-extension `pubsub.subscribe`.** 0.5.2 WS broker scope. The
  callee extension may call `pubsub.publish`; subscribing to another
  extension's channel from JS is still unimplemented.
- **Tier 3 sidecar proxy.** 0.8.x. `cap.tier3_not_available` remains
  the stable rejection for sidecar entries after this ICD lands.
- **Per-extension handler-module caching beyond a single execution.**
  0.5.0.4 ships per-call handler module load (cached for the duration
  of one acquired `BridgeContext` execution). Persistent per-pool
  module caching is a future optimization gated on measured handler-
  load hotpath cost; if needed, ships as `0.5.0.5` or folds into a
  later milestone.
- **Hot reload semantics beyond minimal correctness.** When a package
  upgrades (ACTIVE → UPGRADING → ACTIVE), the RuntimeRegistry
  destroys the old pool + rebuilds. In-flight calls against the old
  pool rejected with `cap.extension_not_loaded`. No graceful drain;
  no versioned coexistence. The `0.6a admin hot reload` milestone may
  revisit.
- **Per-extension resource limit tuning.** 0.5.0.4 uses the current
  default `RuntimeLimits` from `Config` for every extension pool
  created. Per-extension manifest-declared limit overrides are
  DESIGN-quickjs-bridge.md §4 scope and stay deferred.
- **Concurrent pool contention backoff.** Pool `acquire()` blocks
  via the existing `RuntimePool` semantics; if contention becomes a
  measured hotpath (LH-2 or later), introduce a concurrency ceiling
  + fast-reject with `cap.pool_exhausted`. Not implemented in 0.5.0.4
  because the LH-1 storm tier is the first real pressure-test for it.

---

## Architecture

### Dispatch flow (end-state after 0.5.0.4 ships)

```
   ┌────────────────────────────┐         ┌────────────────────────────┐
   │  WS `call ext:1:fn`        │         │  JS `cap.call("ext:1:fn")` │
   │  (drogon WS message)       │         │  (cap_bindings enqueue)    │
   └─────────────┬──────────────┘         └──────────────┬─────────────┘
                 │ co_await                              │ AsyncOp::CAP_CALL
                 ▼                                       ▼ (run_cap_call_outcome)
          ┌────────────────────────────────────────────────┐
          │  call_capability_async(call, caller_ctx)       │   ── Entry point
          │  (ICD-0.2.6)                                   │
          └─────────────────────────┬──────────────────────┘
                                    │ Tier 1 miss
                                    ▼
                          ┌─────────────────────┐
                          │  dispatch_tier2     │   ── §Resolver integration
                          │  (this ICD)         │
                          └──────────┬──────────┘
           provider_type="extension" │
                                    ▼
                  ┌─────────────────────────────────────┐
                  │  RuntimeRegistry::dispatch(         │   ── §RuntimeRegistry
                  │    extension_name, function,        │
                  │    args, caller_ctx, call_depth)    │
                  └─────────────────┬───────────────────┘
                                    ▼
                  ┌─────────────────────────────────────┐
                  │  pool.acquire() → BridgeContext*    │   (ICD-0.3.1)
                  │  set bc.user = caller_ctx           │
                  │  set bc.call_depth = caller+1       │
                  │  (extension_name already from pool) │
                  └─────────────────┬───────────────────┘
                                    ▼
                  ┌─────────────────────────────────────┐
                  │  invoke_handler(bc, function, args) │   ── §Handler invocation
                  │  (loads `server/handlers/<fn>.js`,  │
                  │   runs via run_on_context)          │
                  └─────────────────┬───────────────────┘
                                    ▼
                         Result or PromiseRejection
                                    │
                                    ▼
                         pool.release(bc) — or destroy on failure
                                    │
                                    ▼
                  Mapped into `CapabilityError` / `ResolveResult`
                  per §Error Mapping
```

### Sync vs async

`dispatch_tier2` for extension entries requires JS evaluation. The
sync `call_capability` entry point cannot drive JS without either
blocking the caller thread on a coroutine (deadlock-prone,
event-loop-starving) or duplicating the async path's mechanics in
sync form (code duplication + divergence risk).

**Decision: extension dispatch is async-only.** The sync
`call_capability` path rejects extension entries with a new
`CapabilityError::ASYNC_REQUIRED` code (`cap.async_required`), and
every caller that needs extension dispatch migrates to
`call_capability_async`. The already-migrated call sites are:

- `src/kernel/js/run_on_context.cpp:447–469` — `run_cap_call_outcome`
  (uses `call_capability_async`, already correct).
- `src/kernel/js/stdlib/cap_bindings.cpp:92–159` — `cap.call`
  (enqueues the async op; correct by composition).

The one call site that needs migration is:

- `src/kernel/ws/call_dispatch.cpp:65–111` — `on_call`. Currently
  calls `call_capability(call, ctx)` synchronously at line 103.
  **Migration:** change the handler signature to
  `drogon::Task<> on_call(...)` and `co_await
  call_capability_async(call, ctx)`. Drogon supports coroutine WS
  handlers; the migration is local to this one function. After this
  ICD's implementation, the sync `call_capability` path handles only
  Tier 1 kernel stubs and sidecar stubs; extension entries on that
  path reject with `cap.async_required`.

### RuntimeRegistry

New subsystem at `src/kernel/extensions/runtime_registry.{hpp,cpp}`
(new namespace `plinth::extensions`). Process-lifetime, owns one
`RuntimePool` per installed-and-ACTIVE extension.

```cpp
// src/kernel/extensions/runtime_registry.hpp
namespace plinth::extensions {

// Process-lifetime registry of per-extension RuntimePools. Created
// at kernel bootstrap; populated by the install lifecycle on
// ACTIVE transitions; torn down on DISABLED / UPGRADING / uninstall.
// All public methods are thread-safe under the internal shared_mutex.

auto init_registry(const Config& cfg) -> void;
auto shutdown_registry() -> void;

// Install-lifecycle hooks. Idempotent under repeated calls.
//   create_pool  — ACTIVE / ENABLE transition. If a pool for this
//                  name already exists (ENABLE from DISABLED), the
//                  existing pool is destroyed first and rebuilt
//                  from the on-disk `{name}/active` state. Returns
//                  true if created; false if the extension has
//                  no `server/` tree (client-only package — no JS
//                  capabilities to dispatch).
//   destroy_pool — DISABLED / UPGRADING / UNINSTALL transition.
//                  Destroys the pool. In-flight acquirers on
//                  another thread race-lose harmlessly: their
//                  acquired bc is still valid until release, but
//                  post-release the pool is gone. See §Hot reload.
auto create_pool(std::string_view extension_name) -> bool;
auto destroy_pool(std::string_view extension_name) -> void;

// Dispatch entry. Resolves the pool, acquires a bc, sets caller
// identity + call_depth, loads + invokes the handler, returns the
// handler's Promise resolution or a PromiseRejection. Intended to
// be called only from `dispatch_tier2`'s extension arm.
auto dispatch(std::string_view extension_name,
              std::string_view function,
              const Json::Value& args,
              const plinth::capabilities::UserContext& caller,
              int caller_call_depth)
    -> drogon::Task<std::expected<Json::Value, PromiseRejection>>;

}  // namespace plinth::extensions
```

**Why a new namespace + file:** the registry is not a capability-
resolution concern (belongs outside `capabilities/`) and not a JS-
runtime concern (sits above `RuntimePool`). `extensions/` names the
new subsystem after what it manages. Sits alongside `packages/`
(install-lifecycle) + `realtime/` (0.5.0 pubsub) + `capabilities/`
(resolver).

**Lifecycle:**

- `init_registry(cfg)` — called from `src/kernel/main.cpp` at
  bootstrap, **after** `capabilities::init_resolver` and **before**
  route registration. Iterates `SELECT name FROM plinth.packages
  WHERE state = 'ACTIVE'` and, for each row, calls `create_pool`
  which internally checks
  `fs::exists({data_dir}/extensions/{name}/active/server)` before
  spinning the pool up (client-only packages skip cleanly). No
  schema change.
- `shutdown_registry()` — called from `main.cpp` `atexit` chain,
  **after** `realtime::stop_listener()` and **before**
  `drogon::app().quit()`. Follows the ordering pattern pinned in
  `feedback_deterministic_teardown.md` (cancel framework callbacks
  before the app quits).
- `create_pool(name)` — hooked from the three registration sites in
  `install_lifecycle.cpp` (lines 842 INSTALL-from-empty, 930
  UPGRADE-to-ACTIVE, 1891 ACTIVE-from-DISABLED). Each site, after
  the capability INSERT commits, calls `create_pool(name)` — a
  non-tx side effect. If the call fails (handler load error,
  runtime alloc failure), the install transaction commits anyway;
  the extension is marked ACTIVE but calls to it fail with
  `cap.extension_not_loaded` until a retry. The registry's startup
  scan picks it up on next kernel boot.
- `destroy_pool(name)` — hooked from DISABLED / UPGRADING /
  UNINSTALL transition commits. Calls
  `RuntimePool::rebuild()`-equivalent behavior (destroys all
  pooled contexts; tolerates checked-out contexts that race-lose
  after release).

**Thread-safety:** internal `std::shared_mutex` on the
`unordered_map<std::string, std::unique_ptr<RuntimePool>>`. Reads
(dispatch lookups) take shared locks; mutations
(`create_pool`/`destroy_pool`) take exclusive locks. Dispatch takes
a shared lock only to look up the pool pointer, then releases it
before calling `acquire()` — the pool's own mutex serializes pool
state. Hot-reload race window (pool destroyed mid-dispatch)
returns `cap.extension_not_loaded` from the dispatcher.

### Handler invocation

`invoke_handler(bc, function, args)` is an internal helper in
`runtime_registry.cpp`. Steps:

1. **Resolve handler path.** `{data_dir}/extensions/{name}/active/server/handlers/{function}.js`.
   Uses the `active/` symlink-or-directory pattern pinned in 0.4.5
   (matches install_lifecycle.cpp:3124–3125's "active" convention).
   The `function` string is already validated by the capability
   signature parser (ICD-0.2.1); no re-validation here.
2. **Existence check.** If the file does not exist, return
   `PromiseRejection{code: "cap.handler_not_found", ...}`. Do not
   leak the full filesystem path in the rejection message; cite only
   `<extension>:<function>`.
3. **Build execution source.** The handler file is an ES module with
   `export default function <function>(args) { ... }`. The
   executing source is a thin wrapper that:
   - Dynamic-imports the handler module from an in-memory source
     string (the handler file's contents read fresh per call in
     0.5.0.4 — caching is a §Out of scope deferral).
   - Invokes the default export with `args` as a single argument.
   - Returns whatever the handler returns (a Promise or a value).
   The wrapper source shape is pinned here to fix the contract:
   ```js
   // Generated per-call. `__args` and `__handler_src` are closed
   // over via a C++-side JSValue injection; not visible to the
   // handler.
   const __mod = await import_from_src(__handler_src, "<extension>/<function>");
   return await __mod.default(__args);
   ```
   The `import_from_src` helper is a QuickJS intrinsic the 0.5.0.4
   impl provides; it accepts a source string + a module identifier
   and returns the module namespace. Fully isolated from `fetch` /
   network / filesystem — the string is supplied by the C++
   caller after the filesystem read.
4. **Execute via run_on_context.** `co_await run_on_context(bc, wrapper_src)`.
   The existing coroutine loop drives the async frame (the handler
   may `cap.call`, `db.query`, `pubsub.publish`, etc.). All existing
   resource limits (memory, CPU, wall-clock, call_depth) apply.
5. **Classify outcome.** `run_on_context` returns
   `std::expected<Json::Value, EvalError>` (struct `EvalError` with
   `EvalErrorKind kind`). Map:
   - Success → `Json::Value` (handler return).
   - `EvalErrorKind::SYNTAX_ERROR` reached during the wrapper-source
     import of the handler module → `cap.handler_load_failed`
     (module didn't parse). Also any `RUNTIME_ERROR` raised before
     the default export is invoked (e.g. import-time throw).
   - `EvalErrorKind::RUNTIME_ERROR` raised by the default export
     (JS throw or rejected Promise) → `cap.handler_threw` with the
     JS error's `message` (capped to 1024 bytes to prevent
     log/audit blowup).
   - `EvalErrorKind::MEMORY_LIMIT` / `CPU_TIME_EXCEEDED` /
     `WALL_CLOCK_EXCEEDED` / `STACK_OVERFLOW` /
     `ASYNC_CONCURRENCY_LIMIT` / `ASYNC_RESULT_SIZE_EXCEEDED` /
     `PROMISE_REJECTED_UNHANDLED` / `UNICODE_SMUGGLE_DETECTED` /
     `INTERNAL` / `INTERNAL_ASYNC` / `PROMISE_RESOLVE_AFTER_CANCEL`
     → `cap.handler_threw` with a kind-prefixed message (e.g.
     `"memory_limit: ..."`). The per-kind rejection codes are
     intentionally folded into one `cap.handler_threw` variant —
     from the caller's perspective, any per-op resource trip is
     "your target handler's execution failed"; the kind is
     informational in the message, not a separate taxonomy.
   - `EvalErrorKind::CANCELLED` → `cap.cancelled` (matches the
     existing cancellation taxonomy; NOT folded into
     `cap.handler_threw` because cancellation is caller-initiated).
   - Distinguishing "during import" vs "after default-export
     invocation" is an implementation detail of `invoke_handler`
     — the wrapper source structure (import first, then call) makes
     the distinction mechanical: any EvalError raised before
     `__mod.default` is reachable maps to `cap.handler_load_failed`
     or the kind-specific code; any after maps to `cap.handler_threw`.
6. **Release or destroy.** On success, `pool.release(bc)`. On any
   failure, `pool.destroy(bc)` (matches ICD-0.3.1's defensive-
   destroy on *_EXCEEDED).

---

## Resolver integration

New case in `dispatch_tier2` for `provider_type == "extension"`. The
function today returns `ResolveResult` synchronously; extension
dispatch needs a coroutine. **Change:** promote `dispatch_tier2` to
a coroutine (`drogon::Task<ResolveResult>`) consumed by
`call_capability_async`. The sync `call_capability` path detects the
extension branch and rejects inline before entering the coroutine
— preserving its sync contract for the Tier 1 + sidecar lanes.

```cpp
// src/kernel/capabilities/resolution.cpp (after this ICD's impl)

// Sync entry — used by WS until call_dispatch.cpp migrates, plus any
// future in-process caller that doesn't need extension dispatch.
auto call_capability(const CapabilityCall& call, const UserContext& ctx)
    -> ResolveResult {
    // ... existing Tier 1 dispatch unchanged ...
    // ... existing signature parse + RBAC check unchanged ...
    if (entry.provider_type == "extension") {
        return failure(CapabilityError::ASYNC_REQUIRED);
    }
    // ... sidecar branch unchanged ...
}

// Async entry — full dispatch including extension lane.
auto call_capability_async(const CapabilityCall& call,
                           const UserContext& ctx)
    -> drogon::Task<ResolveResult> {
    // ... Tier 1 + signature parse + RBAC unchanged ...
    if (entry.provider_type == "extension") {
        auto res = co_await plinth::extensions::dispatch(
            entry.extension_name, parsed.function, call.args,
            ctx, call.call_depth);
        if (!res.has_value()) {
            // PromiseRejection carries a cap.* code; map back into
            // CapabilityError for the resolver's uniform return shape.
            co_return failure(rejection_to_capability_error(res.error()));
        }
        co_return CapabilityResult{.data = std::move(*res),
                                   .resolved_tier = 2};
    }
    // ... sidecar branch unchanged ...
}
```

The `rejection_to_capability_error` helper maps the `cap.*` strings
back onto `CapabilityError` variants. Most collapse to a single new
variant `EXTENSION_DISPATCH_FAILED` carrying the `cap.*` code as a
detail string; the resolver's public API widens its error-detail
channel to carry it. `call_capability_async`'s existing callers
(`run_cap_call_outcome`, `cap.batch` expansion) already map
`CapabilityError` to `PromiseRejection` through
`capability_error_to_rejection` — extending that function to preserve
the detail string is a small addition.

**Alternative considered + rejected:** skip the
`CapabilityError::EXTENSION_DISPATCH_FAILED` round trip and have
`dispatch_tier2` return the `PromiseRejection` directly.
Rejected because the resolver's `ResolveResult` type
(`std::expected<CapabilityResult, CapabilityError>`) is consumed by
sync + async callers alike; widening it to a variant type is a
larger refactor. The round trip is lossless because the `cap.*`
code survives as detail.

### CachedCapability: no changes

The cache schema already stores everything dispatch needs
(`signature`, `provider_type`, `extension_name`, `scope`,
`rbac_rule`). No migration, no new columns.

### RBAC enforcement: unchanged location

The RBAC check runs in `call_capability` / `call_capability_async`
**before** `dispatch_tier2` is reached (ICD-0.2.4 §Permission Check
in Dispatch Pipeline). This ICD inherits that enforcement verbatim —
the caller's `UserContext::effective_rules` is checked against the
entry's `rbac_rule` at the resolver boundary, and the `RuntimeRegistry`
is never called for a permission-denied capability. No RBAC re-check
inside the callee's handler (the callee runs under the caller's
identity for the purpose of its own `cap.call` / `db.query` /
`pubsub.publish` checks).

### Call depth enforcement: unchanged location, extended propagation

`MAX_CALL_DEPTH = 8` is enforced in `call_capability_async` against
the `call.call_depth` value the caller supplies (ICD-0.2.2 §Call
Depth Tracking). This ICD threads the incremented depth across the
extension boundary:

- Caller's `bc.call_depth` → `call.call_depth` (already done at
  cap.call enqueue time — `cap_bindings.cpp:157` captures
  `bc.call_depth`).
- Resolver enforces `call.call_depth >= MAX_CALL_DEPTH` → rejects
  (existing behavior).
- Dispatch into target extension: target bc's `call_depth =
  caller.call_depth + 1`. The target's subsequent `cap.call`
  carries this incremented depth onto its own ops. A 9-deep chain
  fails at the ninth resolver entry.

---

## Handler contract

The installed extension's `server/handlers/<function>.js` must be
an ES module with a default export:

```js
// server/handlers/<function>.js
export default async function <function>(args) {
    // args is whatever the caller passed as cap.call's second arg
    // (already converted JS value → JSON value → JS value).
    // Return a value or a Promise of a value.
    return { ok: true };
}
```

**Contract details:**

- **Module type:** ES module (`export default`). The 0.3.0 QuickJS
  build supports ES modules via `JS_EVAL_TYPE_MODULE`; no CommonJS
  fallback.
- **Function name:** default export. The named function inside is
  conventionally named after `<function>` for readability but the
  dispatcher does not enforce name matching — it calls
  `module.default(args)`.
- **Arg shape:** a single argument, whatever the caller passed. `null`
  / `undefined` / primitives / objects / arrays all supported per
  the existing `cap.call` arg shape (ICD-0.3.4 §`cap.call()` Argument
  Rules). No multi-arg variadic dispatch.
- **Return shape:** any JSON-serializable value (or a Promise of
  one). Returned via the existing `Json::Value` round-trip. A
  non-JSON-serializable return (function, Symbol, etc.) rejects
  with `cap.handler_threw` — the JSON conversion fails inside the
  run_on_context loop and surfaces as a throw.
- **Throwing:** any throw (sync or Promise rejection) rejects the
  caller's promise with `cap.handler_threw`.
- **Available bindings:** the full QuickJS stdlib available to
  extension code — `cap.call`, `cap.batch`, `db.query`, `db.exec`,
  `audit.log`, `log.*`, `config.get`, `pubsub.publish`, `crypto.*`.
  The callee's `bc.extension_name` is the callee's own name (pool
  ctor); `bc.user` is the caller's UserContext.

**Fixture for test coverage:** 0.5.0.4 extends
`tests/fixtures/packages/valid-full/` (already has
`server/handlers/read.js`) with:

- A handler that returns `args.n * 2` — verifies arg pass-through
  and return plumbing (R.* tests).
- A handler that throws `new Error("boom")` — verifies
  `cap.handler_threw` (E.* test).
- A handler that calls `await cap.call("kernel:1:log", {msg: args.msg})`
  — verifies the recursion lane works (P.* test).
- A handler that returns after awaiting `db.query("SELECT 1 AS one")`
  — verifies the callee has full binding access (P.* test).
- Capability declarations in `capabilities.json` for each, plus RBAC
  rules in `rbac.json` — proves the full install → register →
  dispatch cycle.

---

## Error taxonomy

Extends the `cap.*` rejection code set introduced in ICD-0.3.4
§Error Mapping. Five new codes:

| Code | When | Error source |
|---|---|---|
| `cap.async_required` | Sync `call_capability` reached a `provider_type == "extension"` entry. Indicates the caller should use `call_capability_async`. | Sync resolver path (WS pre-migration; never surfaced in JS cap.call). |
| `cap.extension_not_loaded` | `dispatch_tier2`'s extension branch reached, but `RuntimeRegistry::dispatch` found no pool (race with DISABLED / UPGRADING; or install-time pool creation failed silently). | RuntimeRegistry. |
| `cap.handler_not_found` | The `server/handlers/<function>.js` file does not exist on disk. Indicates a manifest → on-disk drift (installer bug or post-install tampering). | `invoke_handler` step 2. |
| `cap.handler_load_failed` | Module parse error, import failure, or any error before the default export is invoked. | `invoke_handler` step 4 (run_on_context returns `EvalError::MODULE_ERROR` or similar before the handler runs). |
| `cap.handler_threw` | Any error during or after the default export invocation — sync throw, rejected Promise, resource-limit trip, JSON-conversion failure on return. | `invoke_handler` step 5. |

**Existing codes that continue to apply unchanged:** `cap.not_found`
(resolver couldn't find a Tier 1 handler and no Tier 2 cache entry),
`cap.permission_denied` (RBAC check failed at the resolver boundary,
never reaches dispatch), `cap.call_depth_exceeded` (caller's depth
at MAX_CALL_DEPTH), `cap.invalid_signature`, `cap.capability_disabled`
(cache entry with `enabled = false`), `cap.cancelled` (cancellation
cascade during in-flight dispatch).

**No `cap.pool_exhausted` in 0.5.0.4.** Deferred — see §Out of
scope. Pool contention today blocks on `acquire()`; if the blocking
time becomes a measured problem, add the fast-reject path then.

**Error message sanitization:** all five new codes cap the `message`
field at 1024 bytes (matches `cap.handler_threw` message-length
treatment from ICD-0.3.4). No filesystem paths leak into messages;
no stack traces leak. The extension-internal error is available in
the audit log if the handler-threw path emits one (see below); it
does NOT leak to the caller.

### Audit

When `cap.handler_threw` or `cap.handler_load_failed` fires, the
dispatcher emits an audit event:

- `type`: `capability.extension.error`
- `detail`: `{ extension: <name>, function: <fn>, code: <cap.*>,
  message: <capped>, caller_user_id: <caller.user_id> }`
- Audit context: caller's `UserContext` (the callee runs under
  caller identity, so this is the correct attribution).

`cap.handler_not_found` and `cap.extension_not_loaded` do not audit
— they represent platform state issues, not caller actions;
operator-visible via the spdlog warn path.

---

## Security constraints (non-negotiable)

1. **Caller identity is authoritative.** The callee's BridgeContext
   carries the caller's `UserContext` value-copy. No shim, no
   handler-declarable "service identity" override, no impersonation.
   The callee's `cap.call` / `db.query` / `pubsub.publish` / audit
   events all attribute to the caller. This mirrors ICD-0.3.4
   Security Constraint 3 and applies at the extension boundary.
2. **RBAC runs at the caller's boundary, not the callee's.** The
   capability's `rbac_rule` is checked against the caller's
   `effective_rules` at the resolver, before dispatch. The callee
   does NOT re-check. Declaring an `rbac_rule` on a capability means
   "the caller must have this rule to invoke the handler"; it does
   not mean "the handler re-checks anything."
3. **`bc.extension_name` is set by the pool, not by args.** The
   callee's `BridgeContext` has `extension_name == target extension`.
   No binding-visible API exposes a way to change it mid-execution.
   This is what makes the callee's `pubsub.publish("plinth:ext:<target>:*")`
   succeed (matches `bc.extension_name`) while preventing the callee
   from spoofing another extension's publish (the extension-identity
   gate rejects).
4. **Call depth propagates across the boundary.** The target's
   `bc.call_depth = caller.call_depth + 1`. The existing
   MAX_CALL_DEPTH=8 enforcement applies uniformly; a handler that
   recursively `cap.call`s its own extension still counts each hop.
5. **Handler source is read from the active install, not from
   user-supplied input.** The wrapper source embeds
   `__handler_src` from a filesystem read of
   `{data_dir}/extensions/{name}/active/server/handlers/<fn>.js`.
   No path traversal: the `<fn>` string is validated upstream by the
   signature parser (ICD-0.2.0 §Capability Schema — function regex
   `[a-z][a-z0-9_.]{0,127}` with dot-position rules).
   The installer verified the file at install time; the dispatcher
   trusts the on-disk state.
6. **Resource limits apply to the callee's execution.** The callee's
   BridgeContext uses the RuntimePool's configured `RuntimeLimits`
   (memory, CPU time, wall-clock, stack depth, call depth,
   max_concurrent_async_ops). The caller's limits do not compose
   additively; each hop gets its own ceiling. Wall-clock resets at
   each hop — the nested deadline question raised in ICD-0.3.4
   §Deferred is resolved here as "each hop starts fresh."
7. **Cancellation cascades across the boundary.** If the caller's
   `bc.cancelled` flips mid-dispatch, the target bc must also see
   `cancelled = true` and surface `cap.cancelled`. The 0.5.0.4
   implementation must thread the caller's cancellation flag into
   the target bc — either by pointing the target's atomic at the
   caller's, or by a watcher task. Pinned as a contract; the
   mechanism choice (pointer-sharing vs polling) is implementation
   latitude, but the behavior is non-negotiable.
8. **No mutable bridge between caller and callee.** The JSON-round-
   trip on args and return ensures the callee can't obtain a
   reference to any caller-side JS object, runtime, or context.

---

## Test plan

Test cases join `tests/kernel/capabilities/dispatch_extension_test.cpp`
(new file; follows the per-concern-file pattern from
`resolution_test.cpp`). Uses a real installed fixture extension
driven via the library-level install path (not the HTTP
`POST /api/packages` — that's the deferred HTTP test harness's
concern). Each test installs the fixture, dispatches, asserts,
uninstalls via the standard lifecycle transitions.

The fixture is `tests/fixtures/packages/extdispatch/` — new
minimal extension with `server/handlers/echo.js`,
`server/handlers/throw.js`, `server/handlers/nested.js`,
`server/handlers/dbread.js`, plus the matching `capabilities.json` +
`rbac.json`.

### Group R — Resolve (happy paths)

- **R.01** `cap.call("extdispatch:1:echo", {x: 42})` resolves to
  `{x: 42, doubled: 84}` — proves arg pass-through + return
  plumbing + JSON round-trip both directions.
- **R.02** WS `call extdispatch:1:echo` returns the same value —
  proves the WS → `call_capability_async` migration.
- **R.03** A handler that calls `audit.log(...)` — verifies
  binding availability + caller-identity attribution on the audit
  side.

### Group E — Errors

- **E.01** `cap.call("extdispatch:1:throw")` — handler throws;
  rejects with `{code: "cap.handler_threw", message: <capped>}`.
  Audit event `capability.extension.error` emitted with
  caller's user_id.
- **E.02** `cap.call("extdispatch:1:ghost")` — capability not in
  manifest; rejects with `{code: "cap.not_found"}` at the resolver
  (never reaches dispatch).
- **E.03** A fixture with a capability declared but the handler
  file missing on disk (post-install tampering simulated by
  deleting the file before dispatch); rejects with
  `{code: "cap.handler_not_found"}`. No audit.
- **E.04** A fixture with a handler that contains a syntax error
  — rejects with `{code: "cap.handler_load_failed"}`. Audit
  event `capability.extension.error` emitted.
- **E.05** Dispatch against an extension immediately after
  `destroy_pool` (race window simulated by calling destroy between
  the RBAC check and the dispatch step) — rejects with
  `{code: "cap.extension_not_loaded"}`. No audit.
- **E.06** Sync `call_capability` against an extension entry —
  returns `CapabilityError::ASYNC_REQUIRED`; caller sees
  `cap.async_required` rejection via the sync-to-JS mapping. Not
  exercised from JS (JS always goes async); tested via a direct
  C++ call in the test.
- **E.07** Nested `cap.call` exceeds MAX_CALL_DEPTH: handler A
  calls handler B calls ... calls handler H (depth 8). The 9th
  call rejects with `cap.call_depth_exceeded` at the resolver;
  pre-9-depth calls all resolved. Asserts the depth propagation
  across extension boundaries.

### Group P — Propagation + identity

- **P.01** Handler calls `pubsub.publish("plinth:ext:extdispatch:test", {...})` —
  succeeds. Proves the callee's `bc.extension_name` gate matches
  (no `pubsub.extension_mismatch`).
- **P.02** Handler attempts `pubsub.publish("plinth:ext:other_ext:test", {...})`
  — rejects with `pubsub.extension_mismatch`. Proves the callee
  cannot spoof another extension's identity.
- **P.03** Caller has `effective_rules = ["extdispatch.read"]`;
  `extdispatch:1:echo` has `rbac_rule = "extdispatch.read"`.
  Dispatch succeeds. Separately, caller with empty
  `effective_rules` rejects with `cap.permission_denied` at the
  resolver — never reaches dispatch. Proves RBAC enforcement
  location.
- **P.04** Handler calls `db.query("SELECT 1 AS one")` — returns
  `[{one: 1}]`. Proves callee has `db.*` binding access + uses its
  own `ext_extdispatch` search_path (per DEFERRED 2026-04-18 entry
  — resolved at 0.5.0.4 if search_path is wired, or documented as
  a remaining follow-up if not).
- **P.05** Cross-extension call: install fixtures A + B; handler
  A calls `cap.call("B:1:fn")`; returns B's value. Proves the
  recursion lane works with two separate pools.

### Group H — Hot reload / lifecycle

- **H.01** Install extension; dispatch succeeds. DISABLE extension;
  subsequent dispatch returns `cap.capability_disabled` at the
  resolver (existing behavior, but asserted against the new path).
  ENABLE extension; dispatch succeeds again. Proves the
  create_pool / destroy_pool cycle.
- **H.02** Install extension; dispatch succeeds. UPGRADE to new
  version; dispatch against the new version succeeds with the new
  handler's return value. Proves the UPGRADING → ACTIVE rebuild
  destroys and recreates the pool.
- **H.03** In-flight dispatch (slow handler) during DISABLE
  transition — the in-flight call completes with its result
  (acquired bc still valid); a subsequent call rejects with
  `cap.capability_disabled` at the resolver. Proves the race
  semantics pinned in §RuntimeRegistry are honored.

### Group C — Cancellation

- **C.01** Caller's `bc.cancelled = true` during in-flight
  extension call — rejects with `cap.cancelled`; the callee's
  `bc.cancelled` flipped; callee sees cancellation inside its
  handler (if it polls via awaiting anything). Proves Security
  Constraint 7.
- **C.02** Caller's wall-clock expires during an extension call
  whose handler is slower than the caller's wall-clock — rejects
  with `cap.cancelled` (caller-side) and the callee's execution
  is torn down via the existing `RuntimePool::destroy` defensive
  path.

### Negative

- No ws_test_fixture dependency — the library-level install path
  is sufficient (matches the 0.4.5 Slice B test-strategy decision).
- No HTTP-surface dependency — `POST /api/packages` is the
  deferred HTTP harness's concern (DEFERRED 2026-04-20 entry).
- No LH-1 dependency — this fixture + dispatch test prove the
  path end-to-end without the external harness.

---

## CI wiring

- `src/kernel/capabilities/resolution.{hpp,cpp}` — promote
  `dispatch_tier2` to a coroutine consumed by
  `call_capability_async`; sync `call_capability` rejects extension
  entries with `ASYNC_REQUIRED`. Add helper
  `rejection_to_capability_error`.
- `src/kernel/capabilities/error.{hpp,cpp}` — add
  `CapabilityError::ASYNC_REQUIRED` and
  `EXTENSION_DISPATCH_FAILED` variants; extend
  `capability_error_to_rejection` to preserve the detail `cap.*`
  code for `EXTENSION_DISPATCH_FAILED`.
- `src/kernel/extensions/runtime_registry.{hpp,cpp}` — **new**.
  Process-lifetime registry + dispatch entry point. New namespace.
- `src/kernel/packages/install_lifecycle.cpp:842, 930, 1891` —
  add `plinth::extensions::create_pool(name)` call after each
  capability INSERT commits. Add
  `plinth::extensions::destroy_pool(name)` on DISABLED / UPGRADING
  / UNINSTALL commits (sites TBD by 0.5.0.4 implementation — the
  DISABLED transition site).
- `src/kernel/main.cpp` — `plinth::extensions::init_registry(cfg)`
  at bootstrap after `capabilities::init_resolver`;
  `plinth::extensions::shutdown_registry()` in the atexit chain
  after `realtime::stop_listener()`.
- `src/kernel/ws/call_dispatch.cpp:65–111` — migrate `on_call` to
  `drogon::Task<>` coroutine; `co_await call_capability_async`.
- `src/kernel/js/run_on_context.cpp` — add `import_from_src`
  intrinsic (the wrapper-source primitive). Maps onto a thin
  QuickJS C API call; no new dependency.
- `tests/fixtures/packages/extdispatch/` — **new** fixture.
  Manifest + capabilities + rbac + `server/handlers/*.js`.
  Optional extension of `valid-full/` instead of a new fixture;
  0.5.0.4 implementation chooses.
- `tests/kernel/capabilities/dispatch_extension_test.cpp` —
  **new**. Groups R, E, P, H, C per §Test plan.
- `CMakeLists.txt` — add the new subsystem to the `plinth` target
  sources.
- No schema change. No new `CMake` option.
- New runtime dependency on `plinth::extensions::` from
  `src/kernel/capabilities/resolution.cpp` — introduces a link
  edge, but the dependency graph stays acyclic because
  `extensions/` depends on `capabilities/`, not the reverse (the
  call from resolution.cpp into runtime_registry is through a
  forward-declared extern `auto dispatch(...)` function, not a
  class member).

---

## Entry / Exit

**Entry:**

- v0.5.0 merged (tagged `v0.5.0`, commit `f3552b3`) — the pubsub
  emit path this ICD's Group P.01 exercises needs the 0.5.0
  bridge.
- 0.5.0.1 + 0.5.0.2 follow-ups merged (rename + ICD-LH-1).
- This ICD (`ICD-0.5.0.3-extension-dispatch.md`) merged as a
  paper-only four-part follow-up.
- No blocker on ROADMAP 0.5.x milestones — they do not touch the
  resolver extension arm.

**Exit (for the 0.5.0.4 implementation session):**

- All Groups R, E, P, H, C test cases pass under Catch2 default
  and sanitizer builds.
- `run-clang-tidy-20` clean on
  `src/kernel/capabilities/**`, `src/kernel/extensions/**`,
  `src/kernel/ws/call_dispatch.cpp`, `src/kernel/packages/install_lifecycle.cpp`
  (touch points only — not a repo-wide run).
- CHANGELOG entry for 0.5.0.4 describes shipped surface, any ICD
  deviations, and any new DEFERRED.md entries opened during
  implementation.
- `DEFERRED.md` §2026-04-22 entry for the tier3-extension-dispatch
  gap (if one is added by this session's §DEFERRED.md touch —
  TBD during write) moves from Active to Resolved.
- LH-1 is unblocked: the resumption session picks up
  `feat/lh-1-listen-notify-storm` at `339afb0`, rebuilds the
  fixture zip, runs the 3-trial diagnostic discipline, ships.
- ROADMAP untouched — LH-1 stays as `[medium]` until its ship
  entry; 0.5.1 stays as `[medium]` pending its own ICD authoring
  slot (which slides to 0.5.0.5 after this ICD + 0.5.0.4 land,
  per the horizon rule).

---

## What must not be decided yet

- **Per-extension manifest-declared RuntimeLimits.** Every pool
  uses the `Config`-default limits in 0.5.0.4. DESIGN-quickjs-bridge.md
  §4 envisions per-extension overrides; this ICD does not enable
  them. Adding them is a future milestone; the manifest already
  has the `resources` field reserved.
- **Persistent handler-module cache.** 0.5.0.4 reads the handler
  file per call. A per-pool cache keyed on `(name, version, fn)`
  with an mtime invalidator is an obvious optimization but is
  premature — the LH-1 storm tier will tell us if handler load is
  a hotpath.
- **`cap.pool_exhausted` back-pressure.** If `acquire()` contention
  becomes measurably painful, add the concurrency ceiling + fast-
  reject. LH-1 or LH-2 is the first measurement opportunity.
- **Versioned pool coexistence during upgrade.** Today: hard cut.
  In-flight calls against the old version fail on next dispatch.
  A graceful drain would require the registry to keep the old pool
  alive while the new one spins up, draining active checkouts
  under a deadline. Deferred until an extension genuinely needs it.
- **Cross-extension sync dispatch.** Sync `call_capability` rejects
  extension entries with `cap.async_required`. If a future
  in-kernel caller needs sync extension dispatch, it must migrate
  to async. No sync bridge will be added.
- **`cap.whoami()` / identity introspection for handlers.** The
  callee sees caller identity via `bc.user` in C++ but not via any
  JS binding. Same constraint as ICD-0.3.4 §Security Constraint 3.
  Extensions that need to see caller identity should take it as an
  explicit handler arg.
- **Per-call timeout override.** Not added. Each hop uses its own
  pool's `RuntimeLimits::wall_clock_limit`. Nested deadlines
  remain a per-hop ceiling.

---

## Implementation latitude

Explicit call-outs for the 0.5.0.4 session — decisions the ICD does
not pin because they're code-level choices:

- Whether `import_from_src` is a new QuickJS intrinsic in
  `stdlib_inject.cpp` or a helper in `runtime_registry.cpp`. Both
  work; the intrinsic is cleaner if any other subsystem reuses it.
- How the caller's cancellation flag threads into the target bc —
  shared atomic pointer vs watcher task vs per-hop propagation.
  Behavior pinned (§Security Constraint 7); mechanism open.
- Whether `RuntimeRegistry::dispatch` takes a `RuntimePool*` or
  resolves the pool itself. The latter is safer against race with
  `destroy_pool`; both are acceptable.
- Whether to extend the existing `valid-full/` fixture or add a new
  `extdispatch/` fixture. Trade-off: extending keeps the fixture
  count down but entangles unrelated test paths' expectations.
- Whether to land the WS migration in the same 0.5.0.4 PR or split
  it out as 0.5.0.4.1. The ICD's scope includes both; the impl
  session decides on PR granularity.
- Whether to update `DEFERRED.md §2026-04-18` (search_path for
  `db.*`) in 0.5.0.4 since the P.04 test depends on it — if
  0.5.0.4 implements search_path, fold the DEFERRED entry;
  otherwise preserve it and relax P.04's assertion.

---

## References

- [docs/icd/ICD-0.2.2-capability-resolution.md](ICD-0.2.2-capability-resolution.md)
- [docs/icd/ICD-0.2.4-capability-rbac.md](ICD-0.2.4-capability-rbac.md)
- [docs/icd/ICD-0.2.6-async-dispatch.md](ICD-0.2.6-async-dispatch.md)
- [docs/icd/ICD-0.3.1-runtime-lifecycle.md](ICD-0.3.1-runtime-lifecycle.md)
- [docs/icd/ICD-0.3.3-async-bridge.md](ICD-0.3.3-async-bridge.md)
- [docs/icd/ICD-0.3.4-cap-call-from-js.md](ICD-0.3.4-cap-call-from-js.md)
- [docs/icd/ICD-0.4.4-package-install-lifecycle.md](ICD-0.4.4-package-install-lifecycle.md)
- [docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md](ICD-0.5.0-pg-listen-notify-bridge.md)
- [docs/icd/ICD-LH-1-listen-notify-storm.md](ICD-LH-1-listen-notify-storm.md)
- [docs/architecture/02-capabilities.md](../architecture/02-capabilities.md) §3
- [docs/architecture/05-extensions.md](../architecture/05-extensions.md) §§1.2, 3
- [docs/design/DESIGN-quickjs-bridge.md](../design/DESIGN-quickjs-bridge.md) §§3.2, 4, 8.1
- [docs/METHODOLOGY-llm-assisted-development.md](../METHODOLOGY-llm-assisted-development.md) §3.1
- [docs/DEFERRED.md](../DEFERRED.md) 2026-04-18 (db.* search_path); 2026-04-22 (this ICD closes the tier3-extension-dispatch gap when 0.5.0.4 ships)
