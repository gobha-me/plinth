# Architecture 02 — Capability Registry and Kernel APIs

**Owner:** this document. Authoritative for capability identification,
registration, three-tier resolution, scope, conflict rules, version
lifecycle, batching semantics, and the kernel standard library surface
(the QuickJS API contract).

**Depends on:**
- `architecture/01-identity.md §2` (every capability call passes through
  RBAC enforcement using the additive union model).
- `DESIGN-rbac-philosophy.md`.
- `DESIGN-capability-registry.md` (permanent authority on resolution,
  scoping, caching, and RBAC integration — this doc is the kernel-level
  architectural summary; the design doc is the mechanism).
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

See `DESIGN-capability-registry.md` for the permanent authority on
resolution, scoping, caching, and RBAC integration.

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
kernel:1:storage.put(key: string, data: buffer) -> boolean
kernel:1:storage.get(key: string) -> buffer
kernel:1:users.list() -> { user_ids: UUID[] }
```

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

**Tier 1 — In-process (zero overhead).**
Capabilities provided by the kernel itself or by the same extension
resolve to a direct function pointer. No registry lookup. No PG hit.
Most `kernel:1:*` calls land here.

**Tier 2 — Local-node (in-memory registry).**
Capabilities provided by other extensions running on this node resolve
via a local in-memory cache of the registry. No PG hit. Cache is
invalidated by PG `LISTEN/NOTIFY` when the registry changes (package
install/uninstall/enable/disable — rare events, not per-request).

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

**Tier 3 — Remote-proxy (cross-node).**
Capabilities only reachable via another node (sidecars on a different
node, or extensions hot-moved to another replica). PG lookup for
sidecar location + HTTP proxy to the correct node. This path has:

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

Batches are dispatched as a single unit. Calls to the same tier are
parallelized. Calls to the same remote node are multiplexed over a
single connection.

### 1.5 Failure Modes

| Scenario | Behavior |
|----------|----------|
| Capability not registered | Error: "capability `X` not found" with suggestions if similar names exist |
| Target node down | Circuit breaker trips after N failures. Error: "capability unavailable, provider node unreachable" |
| Sidecar disconnected | Registry updated via `LISTEN/NOTIFY`. Subsequent calls get "capability unavailable" |
| RBAC denied | Error: "permission denied for `X`" with the rule name that would grant access |
| Handler throws exception | Error propagated to caller with exception message. Audit logged. |
| Timeout exceeded | Error: "capability call timed out after Nms" |

### 1.6 Resolution Path

When extension code calls `cap.call("terminal:1:shell", "ls")`:

1. Kernel parses the capability identifier: `namespace=terminal`,
   `version=1`, `function=shell`.
2. Checks Tier 1 (in-process) → Tier 2 (local cache) → Tier 3 (remote).
3. Checks RBAC: does the calling user's group set include a rule
   that grants `terminal.shell.execute`? (NOT version-aware.)
4. If authorized, dispatches to the resolved handler.
5. Returns result to caller.

**Resolution is exact-match only.** No version ranges, no ">=1" matching.
Simple, predictable, debuggable.

### 1.7 Version Lifecycle

- Provider registers v1.
- Provider adds v2 (both v1 and v2 are active).
- Kernel can report: "package X requires `terminal:1:shell` —
  `terminal:2:shell` is also available" (upgrade guidance).
- Provider drops v1 → all v1 callers get a clear error:
  "capability `terminal:1:shell` no longer available,
  `terminal:2:shell` exists".
- The removal of a version is an admin-visible event (audit log +
  notification if any installed packages still require it).

### 1.8 Tool Discovery (Thought Experiment — NOT v1)

The capability registry is queryable. An LLM package could call
`kernel:1:registry.list()`, receive every registered capability with
its typed signature, and generate tool definitions. The architecture
supports this naturally; it's not a v1 feature but the design doesn't
block it.

### 1.9 Scope

- **Instance scope.** Capability is available to all users on this
  instance. Registered once. Example: `fs:1:read`, `terminal:1:shell`.
- **User scope.** Capability is per-user. Each user may have their own
  provider. Example: `llm:1:complete` (user's own API key).

### 1.10 Conflict Resolution

- **Same namespace + version + function, same scope:** rejected. Second
  registration fails with a conflict error. Admin must resolve.
- **Same namespace + version + function, different scope:** allowed.
  User-scope overrides instance-scope for that user (personal
  provider).
- **Same namespace, different version, same function:** allowed. This
  is how backward compatibility works.

### 1.11 HA Considerations

- Capability registry stored in PG (`plinth.capabilities` table,
  defined in `DESIGN-capability-registry.md §Data Model`).
- All nodes see the same registry via Tier 2 cache +
  `LISTEN/NOTIFY` invalidation.
- Cross-node dispatch via Tier 3 proxy with circuit breaker.

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

The drain primitive is the per-extension-name analog of the kernel-wide
`g_shutdown_pending` gate (see `architecture/04-services-ha.md §1`):
both bracket dispatch with a decrement-after-completion contract,
both are designed to be free on the hot path. Future Tier 3 sidecar
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

The kernel injects these APIs into every QuickJS extension runtime.
This is the complete surface area available to extension code. All
`audit.log()` and `log.*` calls route through the spdlog subsystem
defined in `DESIGN-logging-subsystem.md`.

### 2.1 Always Available (no permission required)

| API | Purpose |
|-----|---------|
| `db.query(sql)` | Query extension's own tables (search_path enforced) |
| `db.exec(sql, opts?)` | Write to extension's own tables. `opts.silent` suppresses events. |
| `db.batch(fn)` | Execute multiple writes, emit single coalesced event |
| `log.info(msg)`, `.warn()`, `.error()`, `.debug()` | Write to kernel log, tagged with extension + timestamp |
| `audit.log(action, detail)` | Append to `plinth.audit_log` |
| `cap.call(capability, ...args)` | Call a registered capability (RBAC-gated per-capability) |
| `cap.batch(calls[])` | Batched capability calls, parallelized per-tier |
| `pubsub.publish(channel, payload)` | Publish to extension's own channels |
| `pubsub.subscribe(channel, callback)` | Subscribe to own channels |
| `notify.send(userId, notification)` | In-app notification to a user |
| `storage.get(key)`, `.put()`, `.delete()`, `.list()` | File/blob storage within extension's prefix |
| `metrics.gauge(name, value)`, `.counter(name)` | Register and update metrics |
| `config.get(key)` | Read extension's own config values |
| `crypto.hash(algo, data)`, `.hmac(algo, key, data)` | Basic crypto primitives |

### 2.2 Permission-Gated (requires RBAC rule)

| API | Required rule | Purpose |
|-----|---------------|---------|
| `http.get(url)`, `.post()`, etc. | `kernel.http.outbound` | External HTTP. Admin URL allowlist. |
| `pubsub.subscribe(channel, handler) → Promise<() => void>` | per-channel rule (cross-extension only) | Subscribe a handler to a channel; resolves to an unsubscribe function. Own-extension channels skip the RBAC gate. Cross-extension subscription requires the per-channel rule. Contract pinned in [ICD-0.5.2 §`pubsub.subscribe`](../icd/ICD-0.5.2-ws-broker.md). |
| `storage.get(other_prefix)` | per-extension rule | Cross-extension storage access |

**Pattern:** read-your-own-data is free. Cross-extension access is
RBAC-gated. External I/O is RBAC-gated. Logging and metrics are always
available.

### 2.3 Kernel-Provided Capabilities (summary)

The kernel provides a set of capabilities callable via `cap.call()`.
These are `namespace=kernel` and available Tier 1. Non-exhaustive:

- `kernel:1:db.query`, `kernel:1:db.exec` — same as the injected
  `db.*` API, exposed also as capabilities for uniformity.
- `kernel:1:storage.put`, `kernel:1:storage.get`, `kernel:1:storage.delete`,
  `kernel:1:storage.list` — same relationship with `storage.*`.
- `kernel:1:users.list` — see `architecture/01-identity.md §4.2`.
- `kernel:1:pubsub.publish`, `kernel:1:pubsub.subscribe` — pub/sub
  wrappers for cases where extensions need capability-style dispatch
  rather than the injected API.

The authoritative list is defined in the kernel's `capabilities.json`
seed at bootstrap.

---

## 3. QuickJS Async Bridge

**This is the hardest engineering problem in the platform.** It has its
own design document: `DESIGN-quickjs-bridge.md`. This is a Scale 3
(Architecture Arc) item per the methodology.

The bridge connects QuickJS (single-threaded JS interpreter) to Drogon
(multi-threaded async C++ framework) using C++20 coroutines. Each
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

**Per-extension `RuntimePool` ownership.** A process-lifetime
`plinth::extensions::RuntimeRegistry` owns one `plinth::js::RuntimePool`
per installed-and-ACTIVE extension. Install-lifecycle transitions
create/destroy pools at `create_pool` (INSTALL, ENABLE, UPGRADE-T4)
and `destroy_pool` (DISABLE, UPGRADE-cutover-prep, UNINSTALL) call
sites. A dispatch acquires a fresh `BridgeContext` from the target
extension's pool; the callee's `bc.extension_name` is pool-populated
(the `pubsub.publish` identity gate matches on this), the callee's
`bc.user` is a value-copy of the caller's (audit + nested `cap.call`
/ `db.query` attribution runs under the caller). Per-call failures
tear down the `BridgeContext` but the pool persists.

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

See `docs/icd/ICD-0.5.0.3-extension-dispatch.md` for the full contract
(sync-vs-async framing, RuntimeRegistry lifecycle, handler-invocation
wrapper, error-taxonomy mapping, eight security constraints).

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
Admin-only via synthesised RBAC at the dispatch site. Pool init in
`src/kernel/main.cpp` after `init_resolver`; pool teardown from the
existing atexit lambda before `drogon::app().quit()` (pool teardown
pumps pending JS jobs and needs the Drogon loop alive).

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
narrative. Future LH-* milestones (LH-1 LISTEN/NOTIFY storm, LH-2
WS fan-out, LH-3 reconnect-storm, LH-4 metrics cross-validation)
extend this stream with similarly-purpose-built diagnostic
surfaces; each carries its own ICD-LH-* contract under the same
"not a blueprint" framing.

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
Three-Tier Resolution
    │
    ├── Tier 1: In-process? → direct function call (zero overhead)
    │
    ├── Tier 2: Local-node cache? → dispatch locally
    │
    └── Tier 3: Remote? → PG sidecar lookup → proxy to node
         │                 (circuit breaker, 500ms timeout)
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
         ├── Local handler → execute, return result
         │
         └── Sidecar → POST /execute → return result
              │
              ▼
    Result returned to JS runtime via Promise resolution
    JS_ExecutePendingJob continues execution
```
