# Architecture 02 — Capability Registry and Kernel APIs

**Owner:** this document. Authoritative for capability identification,
registration, three-tier resolution, scope, conflict rules, version
lifecycle, batching semantics, and the kernel standard library surface
(the QuickJS API contract).

**Depends on:**
- `architecture/01-identity.md §2` (every capability call passes through
  RBAC enforcement using the additive union model).
- `DESIGN-rbac-philosophy.md`.
- `DESIGN-capability-registry.md` (historical 0.2.x rationale; this document
  owns the current resolution, scoping, caching, and RBAC contract).
- `DESIGN-quickjs-bridge.md` (the transport between JS `cap.call()` and
  kernel C++ dispatch).
- `DESIGN-logging-subsystem.md` (audit path for denials).

**Related:**
- `architecture/04-services-ha.md §4` (sidecar contract — Tier 3 dispatch
  terminates at sidecars).
- `architecture/05-extensions.md §2` (QuickJS runtime, supervision, limits).

---

## 1. Capability Registry

**This replaces package-level dependencies entirely.**

Extensions do not declare "I depend on package X." They declare "I
require capability `namespace:version:function(params) -> return_type`."
The kernel maintains a registry of all registered capabilities and
resolves calls at runtime. Every capability call passes through RBAC
enforcement (see `ICD-0.1.5-rbac-enforcement.md`).

See `DESIGN-capability-registry.md` for the historical 0.2.x rationale and its
current-state correction.

### 1.1 Contract Format

The canonical capability identifier is:

```
namespace:version:function
```

Examples:

```
terminal:1:shell(command: string) -> result
terminal:2:shell(command: string, options: object) -> result
fs:1:cp(source: string, dest: string) -> boolean
fs:1:read(path: string) -> string
llm:1:complete(prompt: string, model: string) -> response
kernel:1:db.query(sql: string) -> rows
kernel:1:db.exec(sql: string) -> result
```

These are identifier-shape examples, not availability promises. Planned
storage and user-list surfaces are classified in §2.

The version is an integer. It increments when the signature changes in
a breaking way (different params, different return type, different
semantics). Non-breaking additions (new optional params with defaults)
do not require a version bump.

Rule mapping for capabilities is **not version-aware**:
`terminal:1:shell` and `terminal:2:shell` both map to the rule
`terminal.shell.execute`.

### 1.2 Registration

A package registers capabilities via `capabilities.json`:

```json
{
  "provides": [
    {
      "namespace": "terminal",
      "version": 1,
      "function": "shell",
      "params": [{ "name": "command", "type": "string" }],
      "returns": "result",
      "scope": "instance",
      "description": "Execute a shell command and return the result"
    }
  ],
  "requires": [
    "kernel:1:db.query(string) -> rows",
    "kernel:1:db.exec(string) -> result"
  ]
}
```

A package MAY register multiple versions simultaneously for backward
compatibility.

### 1.3 Three-Tier Resolution

Every capability call resolves through three tiers, tried in order.

**Tier 1 — In-process.** Kernel handlers resolve to an in-process function.
Extension-provided handlers, including same-extension calls, are registered in
Tier 2 and dispatch asynchronously through `RuntimeRegistry`; the synchronous
path rejects them with `cap.async_required`.

**Tier 2 — Local-node (in-memory registry).**
Capabilities provided by other extensions running on this node resolve
via a local in-memory cache of the registry. No PG hit. A PG
`LISTEN/NOTIFY` hint triggers a full reload from the authoritative registry when
it changes (package install/uninstall/enable/disable — rare events, not
per-request). Notification payload fields never mutate cache state because
PostgreSQL channels do not have producer ACLs.

- **Stale cache window.** Registry changes propagate via `LISTEN/NOTIFY`
  which is asynchronous. There is a window (tens of milliseconds) where
  a node may have a stale cache. This is acceptable because registry
  changes are rare (admin actions, not user traffic).
- **Reconnect-triggered full resync.** LISTEN/NOTIFY delivery is
  connection-scoped: a notification fired while the listener is
  reconnecting is lost forever. The listener calls a full
  `reload_tier2_cache` helper after every successful LISTEN open
  (initial *and* reconnect). This bounds missed-NOTIFY divergence to
  one reconnect-backoff window (≤ 1 s) plus one `SELECT`, rather than
  process lifetime. Mechanism landed in 0.2.4; see ICD-0.2.2
  §Reconnect-triggered full resync.
- **Cache invalidation debounce.** If multiple packages are installed
  simultaneously, each triggers a `LISTEN/NOTIFY`. The cache
  invalidation handler debounces: on first NOTIFY, start a 100ms
  window. Coalesce all invalidation events in the window. Rebuild
  cache once.

**Tier 3 — Remote-proxy (planned).** The current resolver returns
`TIER3_NOT_AVAILABLE`. [Issue #66](https://github.com/gobha-me/plinth/issues/66)
owns remote capability dispatch; [#67](https://github.com/gobha-me/plinth/issues/67)
and [#73](https://github.com/gobha-me/plinth/issues/73) own containment and
cross-node routing. The target path has:

- **Latency budget.** Configurable timeout, default 500ms.
- **Circuit breaker.** If a remote node fails N times in M seconds, the
  circuit opens and calls fail fast until the node recovers.
- **Tracked metric.** Capability resolution latency is tracked per-tier
  (see `architecture/04-services-ha.md §3`).

### 1.4 Batched Capability Calls

Extensions that need multiple capability calls can batch them:

```javascript
const results = await cap.batch([
  ["terminal:1:shell", "ls -la"],
  ["fs:1:read", "/etc/hostname"],
  ["terminal:1:shell", "whoami"]
]);
```

The QuickJS binding validates the batch and composes ordinary `cap.call`
promises with `Promise.all`; it is not an atomic dispatch unit. The C++ batch
helper is sequential. Remote grouping and multiplexing are future Tier 3
design inputs, not current behavior.

### 1.5 Failure Modes

| Scenario | Behavior |
|----------|----------|
| Capability not registered | `cap.not_found` with the requested signature |
| Target node down | Planned Tier 3 behavior; current builds return `TIER3_NOT_AVAILABLE`. |
| Sidecar disconnected | Planned sidecar behavior; no current sidecar transport exists. |
| RBAC denied | `cap.permission_denied` with the requested signature; the server-side audit records the required rule |
| Handler load/evaluation fails | `cap.handler_load_failed` or `cap.handler_threw`; the bounded message carries an error-kind tag |
| Wall-clock limit exceeded | `cap.handler_threw` with a `wall_clock_exceeded` kind tag |

### 1.6 Resolution Path

When extension code calls `cap.call("terminal:1:shell", "ls")`:

1. Kernel parses the capability identifier: `namespace=terminal`,
   `version=1`, `function=shell`.
2. Checks Tier 1 (kernel) → Tier 2 (local provider cache). A Tier 3
   candidate currently fails closed as unavailable.
3. Checks RBAC: does the calling user's group set include a rule
   that grants `terminal.shell.execute`? (NOT version-aware.)
4. If authorized, dispatches to the resolved handler.
5. Returns result to caller.

**Resolution is exact-match only.** No version ranges, no ">=1" matching.
Simple, predictable, debuggable.

### 1.7 Version Lifecycle

- Provider registers v1.
- Provider adds v2 (both v1 and v2 are active).
- Provider drops v1 → exact v1 calls return `cap.not_found` with the requested
  signature. The current resolver does not suggest alternative versions.
- Package lifecycle audit records the provider transition. Dependency-aware
  upgrade guidance and admin notifications are not current behavior.

### 1.8 Tool Discovery (Thought Experiment — NOT v1)

The capability registry is queryable. An LLM package could call
`kernel:1:registry.list()`, receive every registered capability with
its typed signature, and generate tool definitions. The architecture
supports this naturally; it's not a v1 feature but the design doesn't
block it.

### 1.9 Scope

- **Instance scope.** Capability is available to all users on this
  instance. Registered once. Example: `fs:1:read`, `terminal:1:shell`.
- **User scope (conditional, not implemented).** Registration currently rejects
  this scope with `USER_SCOPE_NOT_SUPPORTED`. No delivery is scheduled; a
  concrete consumer must first produce a bounded issue and authority design.

### 1.10 Conflict Resolution

- **Same namespace + version + function, same scope:** rejected. Second
  registration fails with a conflict error. Admin must resolve.
- **Same namespace + version + function, different scope:** reserved for the
  conditional user-scope design; not accepted by current registration.
- **Same namespace, different version, same function:** allowed. This
  is how backward compatibility works.

### 1.11 HA Considerations (planned)

- Capability registry stored in PG (`plinth.capabilities` table,
  defined in `DESIGN-capability-registry.md §Data Model`).
- All nodes see the same registry via Tier 2 cache +
  `LISTEN/NOTIFY` invalidation.
- Cross-node dispatch via Tier 3 proxy with circuit breaker is owned by #66,
  #67, #73, and #74.

### 1.12 Composition Hooks (reserved)

The composition framework in `architecture/05-extensions.md §3` adds a
future query dimension to the registry: "who augments surfaces of trait
T?" This is a new query kind alongside "who provides namespace X?" The
existing `plinth.capabilities` schema is sufficient if the composition
arc joins through a separate augmenter index table (added when the arc
lands). No 0.2.x schema change is required now; no 0.2.x schema change
must be made that would later conflict with this addition.
`DESIGN-capability-registry.md §"What Must Not Be Decided Yet"` is
amended accordingly.

### 1.13 Drain Primitive (per-extension in-flight counter)

Live since 0.4.5. `src/kernel/capabilities/drain.{hpp,cpp}` provides
a per-extension-name in-flight-call counter consumed by the upgrade
choreography in `install_lifecycle.cpp::upgrade_package` (atomic-swap
T1/T2 — `begin_drain` then `wait_for_zero` with a configurable
timeout). Every capability dispatch passes through a `DispatchGuard`
constructor that performs a single relaxed atomic load on the
counter when no drain is active (the hot path) and increments when
one is, bracketing the dispatch with a matching decrement on
guard destruction.

**Invariants the drain primitive enforces.** During an upgrade's T1→T3
window for extension `X`, every new capability call against `X` is
counted; the upgrade waits up to `upgrade_drain_timeout_ms` (default
5000 ms, configurable per `Config::packages_upgrade_drain_timeout_ms`)
for outstanding calls to complete before performing the atomic-swap
T3 (PG state flip + symlink rename). Pre-drain in-flight calls are
not counted by the guard but are naturally serialized via the
`state_mutex` shared lock that the resolution path holds, so REGISTERING's
unique lock acquisition still waits on them.

The drain primitive is specific to extension-name upgrade admission. Process
shutdown uses the separate coordinator and async-task registry described in
`architecture/shutdown.md`; there is no kernel-wide `g_shutdown_pending`
dispatch counter. Future Tier 3 sidecar
dispatch and any future asynchronous wrapper that lands as the
caller for `call_capability_async` must compose with the drain by
incrementing through the same `DispatchGuard`. Uninstall
(0.4.5 `uninstall_package`) and any future cancel-extension-mid-call
work also reuse the primitive without modification.

`DESIGN-packages-v04x.md §0.4.5` and `ICD-0.4.5-package-lifecycle-transitions.md
§State Machine` carry the upgrade-side detail; this section captures
the architectural commitment that the dispatch path will always call
through the guard for any extension-scoped capability invocation.

---

## 2. Kernel Standard Library (QuickJS APIs)

The status column distinguishes the shipped QuickJS surface from planned
contracts. All `audit.log()` and `log.*` calls route through the spdlog
subsystem defined in `DESIGN-logging-subsystem.md`.

### 2.1 Always Available (no permission required)

| API | Status | Purpose / owner |
|-----|--------|-----------------|
| `db.query(sql)`, `db.exec(sql, opts?)`, `db.batch(fn)` | Shipped | Restricted-login extension database access; see `extension-database-isolation.md`. |
| `log.info(msg)`, `.warn()`, `.error()`, `.debug()` | Shipped | Write to the kernel log with extension attribution. |
| `audit.log(action, detail)` | Shipped | Append through the bounded audit path. |
| `cap.call(capability, ...args)`, `cap.batch(calls[])` | Shipped | RBAC-gated dispatch; batch is `Promise.all` composition, not atomic. |
| `pubsub.publish(channel, payload)`, `pubsub.subscribe(channel, callback)` | Shipped | Protected realtime publication and subscription. |
| `config.get(key)` | Shipped | Read extension-scoped configuration. |
| `crypto.hash`, `crypto.randomBytes`, `crypto.timingSafeEqual` | Shipped | Current bounded crypto primitives. |
| `notify.send(userId, notification)` | Planned | [#81](https://github.com/gobha-me/plinth/issues/81). |
| `storage.get`, `.put`, `.delete`, `.list` | Planned | [#78](https://github.com/gobha-me/plinth/issues/78), [#79](https://github.com/gobha-me/plinth/issues/79). |
| `metrics.*` | Planned | [#61](https://github.com/gobha-me/plinth/issues/61). |

`crypto.hmac` is not shipped or scheduled. It is not part of the supported
surface unless a concrete consumer produces a reviewed issue.

### 2.2 Permission-Gated (requires RBAC rule)

| API | Required rule | Purpose |
|-----|---------------|---------|
| `http.get(url)`, `.post()`, etc. | `kernel.http.outbound` | Planned by [#82](https://github.com/gobha-me/plinth/issues/82); external HTTP with an admin allowlist. |
| `pubsub.subscribe(channel, handler) → Promise<() => void>` | per-channel rule (cross-extension only) | Subscribe a handler to a channel; resolves to an unsubscribe function. Own-extension channels skip the RBAC gate. Cross-extension subscription requires the per-channel rule. Contract pinned in [ICD-0.5.2 §`pubsub.subscribe`](../icd/ICD-0.5.2-ws-broker.md). |
| `storage.get(other_prefix)` | per-extension rule | Planned with the storage contract; cross-extension access is not current behavior. |

**Pattern:** read-your-own-data is free. Cross-extension access and external
I/O are RBAC-gated when their owning issue lands. Logging is always available.

### 2.3 Kernel-Provided Capabilities (summary)

The bootstrap currently reserves DB, log, audit, and config capability rows,
but their Tier 1 handlers fail with `not_implemented`; extensions use the
shipped injected APIs above. There is no authoritative kernel
`capabilities.json` seed. A capability becomes public only when its owning issue
defines implementation, RBAC, lifecycle, and tests. In particular,
`kernel:1:users.list` is planned by
[#97](https://github.com/gobha-me/plinth/issues/97), while storage and
capability-style pub/sub wrappers are not current surfaces.

---

## 3. QuickJS Async Bridge

**This is the hardest engineering problem in the platform.** It has its
own design document: `DESIGN-quickjs-bridge.md`. This is a Scale 3
(Architecture Arc) item per the methodology.

The bridge connects QuickJS (single-threaded JS interpreter) to Drogon
(multi-threaded async C++ framework) using C++23 coroutines. Each
extension execution is a Drogon coroutine. The bridge translates JS
`await` into Drogon `co_await`, yielding the thread back to the event
loop so other requests proceed while JS awaits. Extension-provider
capability dispatch runs exclusively on the async path — see §3.1
below for the sync-vs-async contract and the per-extension runtime
pool ownership model.

See `DESIGN-quickjs-bridge.md` for: coroutine lifecycle, runtime pool,
the JS↔C++ promise bridge, resource limits (memory, CPU time,
wall-clock timeout, call depth), error propagation, cancellation and
cleanup, and the full implementation sequence (0.3.0–0.3.5).

The runtime limits and extension supervision model are documented in
`architecture/05-extensions.md §2`.

### 3.1 Async Dispatch Arm + Extension Runtimes

Live since 0.5.0.4. Extension-provider capability entries dispatch
**async-only**; the sync `call_capability` path rejects them with
`CapabilityError::ASYNC_REQUIRED` / `cap.async_required`. The async
entry `call_capability_async` consumes the `drogon::Task<>` coroutine
from the bridge and threads the caller's `UserContext` + incremented
`call_depth` across the extension boundary (MAX_CALL_DEPTH=8 uniform
per §1.13-adjacent call-depth enforcement). Tier 1 kernel stubs and
sidecar stubs continue to resolve through both sync and async paths;
only extension-provider entries are async-required.

**Per-extension `RuntimePool` ownership.** `RuntimeRegistry` owns one shared
pool per installed-and-ACTIVE extension. Dispatches hold shared leases; pool
replacement/removal and last-lease retirement are claimed under the registry
mutex, then bounded shutdown runs outside it. Failed closes retain durable
owners for coordinator retry, and shutdown waits for both dispatches and
retirements. A dispatch acquires a fresh `BridgeContext`; the callee extension
identity comes from the pool and user authority is copied from the caller.
Database clients follow the explicit private-loop ownership graph in
`architecture/shutdown.md`. Per-call failure releases the context lease but
does not bypass pool retirement.

**Extension-specific `cap.*` rejection codes** introduced alongside
this surface:

| Code | When |
|------|------|
| `cap.async_required` | Sync `call_capability` reached an extension-provider entry. |
| `cap.extension_not_loaded` | Dispatch reached the extension arm but no pool exists (race with DISABLE/UPGRADE, or create-time pool allocation failed). |
| `cap.handler_not_found` | `server/handlers/<fn>.js` absent on disk (install-manifest drift). |
| `cap.handler_load_failed` | Handler-module parse error or import-time throw before the default export runs. |
| `cap.handler_threw` | Handler raised or rejected after the default export invocation (including resource-limit trips folded into one caller-visible code). |

**Handler-file convention.** Extension capability handlers live at
`server/handlers/<fn>.js`, ES modules with a default export whose
invocation receives one `args` argument and returns a
JSON-serializable value or a Promise of one. The convention was
observable in 0.4.x fixtures; ICD-0.5.0.3 promotes it to normative
contract.

`docs/icd/ICD-0.5.0.3-extension-dispatch.md` records the original dispatch
contract. Its raw lookup, unique-owner, and `atexit` lifecycle text is
superseded by the shared-lease coordinator contract above and
`architecture/shutdown.md`.

---

## 4. Diagnostic Kernel Surfaces (Load Harness stream)

Live since 2026-04-21 with the LH-0 + LH-0.1 ship. The kernel exposes
two purpose-built surfaces for the parallel Load Harness stream
(`load-harness/` top-level binary, separate from `plinth_tests`). Both
are explicitly **diagnostic-only deviations from the standard
capability-dispatch path** and are NOT blueprints for extension
dispatch or for any future production capability surface.

**`lh0:1:chain` Tier 1 capability.** Registered by
`register_lh0_harness_handlers_locked` (called from `init_resolver`)
as a kernel-namespaced Tier 1 entry, RBAC-gated by `kernel.admin`.
Recurses through the standard `call_capability` pipeline by issuing
nested `cap.call("lh0:1:chain", depth-1)` invocations until depth
reaches zero. Exercises the Tier 1 lookup, RBAC enforcement, and
`MAX_CALL_DEPTH=8` guard end-to-end at saturation. Reachable only via
the WS `call` message type (`src/kernel/ws/call_dispatch.{hpp,cpp}`,
also new in LH-0); not exposed to extension JS through `cap.call`.
Contract pinned in `ICD-LH-0-load-harness-scaffold.md §4`.

**`lh0:1:js_stress(script)` dispatch fork.** Recognised by
`try_dispatch_js_stress` in `src/kernel/ws/js_stress.{hpp,cpp}`,
called from `src/kernel/ws/call_dispatch.cpp::on_call` **before**
`call_capability` — a deliberate dispatch fork that bypasses the
Tier 1 / Tier 2 / Tier 3 resolution chain. Drives `run_on_context` on
a process-lifetime `RuntimePool` with `default_runtime_limits()`
(16 MiB mem, 100 ms CPU, 30 s wall-clock, 8 concurrent async ops).
Admin-only via synthesised RBAC at the dispatch site. Pool init occurs after
`init_resolver`. The shared shutdown coordinator closes diagnostic admission,
drains its owned leases, and retires the pool before Drogon event loops stop;
`atexit` is not a lifecycle owner. See `architecture/shutdown.md`.

**Why these are not blueprints.** Both surfaces exist to exercise
specific paths under load that production extensions reach via
different routes. `lh0:1:chain` recurses through `call_capability`;
production extension capability calls reach the same path via
`cap.call` from JS, with the same RBAC + call-depth guards.
`lh0:1:js_stress` bypasses `call_capability` entirely so the
load harness can drive arbitrary JS scripts through the async
bridge without going through the capability registry — this is a
diagnostic shortcut, not a generalizable pattern. Future
extension dispatch (Tier 3 sidecar, JS-driven extension cap calls)
must compose with the `DispatchGuard` drain primitive (§1.13),
the standard `call_capability` resolver, and the standard RBAC
enforcement; the `js_stress` fork explicitly does not, and its
existence is not precedent for any future bypass.

LH-0.1's empirical mandate (3-trial diagnostic against the
production kernel, 133,755 calls / ~535,020 `db.query` ops, zero
reproductions of `free_zero_refcount` / `list_empty(&rt->gc_obj_list)` /
`bad_weak_ptr`) closed the hypothesis-vs-empirical-finding question
on the WS-teardown bandaid family — see `DEFERRED.md` WS-teardown
entry and `RE-EVAL-0.4.x-arc-closeout.md §2.4 + §5.1` for the
narrative. Future load work is issue-owned: reconnect-under-storm is
[#88](https://github.com/gobha-me/plinth/issues/88), while hard/crushing
metrics cross-validation is [#62](https://github.com/gobha-me/plinth/issues/62).
New diagnostic surfaces remain "not a blueprint" and require their own bounded
contract.

---

## Appendix: Capability Call Flow

```
Extension JS code
    │
    │  cap.call("terminal:1:shell", "ls -la")
    │
    ▼
QuickJS Runtime (sandboxed, within Drogon coroutine)
    │
    │  Kernel C++ bridge function invoked
    │
    ▼
Capability String Parsed
    │
    │  namespace=terminal, version=1, function=shell
    │
    ▼
Resolution
    │
    ├── Tier 1: Kernel handler? → direct function call
    │
    ├── Tier 2: Local extension? → async RuntimeRegistry dispatch
    │
    └── Sidecar/remote candidate? → fail closed: TIER3_NOT_AVAILABLE
         │
         └── Planned #66/#67/#73/#74: bounded remote dispatch
         │
         ▼
RBAC Check
    │
    │  User's groups → union of permissions
    │  Has terminal.shell.execute? (NOT version-aware)
    │
    ├── NO → Return permission_denied to JS runtime
    │
    └── YES
         │
         ▼
    Dispatch to handler
         │
         ├── Kernel handler → execute, return result
         │
         └── Extension handler → await bounded QuickJS dispatch

    Result returned to JS runtime via Promise resolution
    JS_ExecutePendingJob continues execution
```
