# ICD-0.2.2-capability-resolution

**Traces to:** architecture/02-capabilities.md §1 (Capability Registry), DESIGN-capability-registry.md
**Depends on:** ICD-0.2.0-capability-registry (storage layer), ICD-0.1.5-rbac-enforcement (permission checking)
**Milestone:** 0.2.2 — Tier 1 + Tier 2 resolution; 0.2.3 — LISTEN/NOTIFY cache invalidation
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DESIGN-quickjs-bridge.md (§4.4 call depth, §7.3 cap.batch, §8.1 capability dispatch), DESIGN-logging-subsystem.md

---

## Overview

This ICD defines the capability **resolution and dispatch** contract — the mechanism that translates a `cap.call("namespace:version:function", args)` into a handler invocation. It covers three-tier resolution, the in-memory cache, cache invalidation via PostgreSQL LISTEN/NOTIFY, scope precedence, and the `cap.batch()` behavioral contract.

This ICD does **not** define RBAC enforcement on capability calls (see ICD-0.2.4) or the QuickJS bridge mechanics (see DESIGN-quickjs-bridge.md). It defines the dispatch layer that both depend on.

---

## Dispatch Contract

### C++ Function Signature

```cpp
struct CapabilityCall {
    std::string signature;         // "namespace:version:function" — pre-composed
    Json::Value args;              // nlohmann::json — forwarded to handler verbatim
    int call_depth = 0;            // incremented per hop (see §Call Depth)
};

struct CapabilityResult {
    Json::Value data;              // return value from handler
    std::string resolved_tier;     // "tier1", "tier2", "tier3"
    std::string provider_type;     // "kernel", "extension", "sidecar"
};

struct UserContext {
    std::string user_id;
    std::string username;
    std::string auth_type;                        // "session" or "pat"
    std::vector<std::string> effective_rules;     // union of all group rules (ICD-0.1.5)
    std::string session_id;                       // empty → SQL NULL in audit (added 0.2.4)
    std::string ip_address;                       // empty → SQL NULL in audit (added 0.2.4)
};

// Primary dispatch function — called by the QuickJS bridge and kernel internals
auto call_capability(const CapabilityCall& call,
                     const UserContext& ctx) -> ResolveResult;
```

Notes:
- `CapabilityCall::signature` is a pre-composed `"namespace:version:function"` string. The bridge and kernel callers already hold the full string; the resolver parses it once at step 1 of the algorithm. Per-field decomposition is an internal detail of the resolver, not of the contract.
- `UserContext::session_id` and `UserContext::ip_address` were added in 0.2.4 to enrich `capability.rbac.denied` audit events. Both may be empty, in which case the audit row writes SQL NULL.

### Async dispatch wrapper

See `ICD-0.2.6-async-dispatch.md` for the coroutine-shaped entry point (`call_capability_async` / `batch_call_capability_async`) consumed by 0.3.3+. The synchronous `call_capability` form defined above remains the primary implementation; the async wrapper composes on top of it.

### Resolution Algorithm

For every `callCapability()` invocation, the resolver executes the following in order:

```
1. Parse and validate the capability identifier
   - Must match namespace:version:function format
   - Validation rules per ICD-0.2.0
   - On failure: return error "invalid_capability"

2. Check call depth
   - If call.call_depth >= MAX_CALL_DEPTH (default 8): return error "call_depth_exceeded"

3. RBAC check (delegated to ICD-0.2.4)
   - Map capability to RBAC rule
   - Check user's effective permissions
   - On denial: return error "permission_denied" (with rule name)

4. Tier 1 — In-process resolution
   - Check the Tier 1 map for a direct function pointer
   - Tier 1 entries are registered at compile time or kernel startup
   - Hit → execute handler directly, return result
   - Miss → proceed to Tier 2

5. Tier 2 — Local cache resolution
   - Look up (namespace, version, function) in the in-memory cache
   - Apply scope precedence (see §Scope Resolution)
   - Hit + enabled → dispatch to the local handler
     - For extension providers: dispatch through QuickJS bridge (DESIGN-quickjs-bridge.md §8.1)
     - call.call_depth is incremented for the nested call
   - Hit + disabled → return error "capability_disabled"
   - Miss → proceed to Tier 3

6. Tier 3 — Remote proxy (stub in 0.2.x)
   - Return error "tier3_not_available"
   - Tier 3 is implemented in milestone 0.8 (Sidecar Tier)
```

### Scope Resolution

When both instance-scope and user-scope capabilities exist for the same `(namespace, version, function)`:

1. If the calling user has a **user-scope** capability registered for them: use it.
2. Otherwise: use the **instance-scope** capability.

User-scope overrides instance-scope for that specific user only. Other users still use instance-scope.

User-scope resolution requires `call.user_id` to be set (which it always is for authenticated calls).

---

## Tier 1 Map

The Tier 1 map is a compile-time or startup-time `std::unordered_map` mapping capability signatures to direct C++ function pointers:

```cpp
using CapabilityHandler = std::function<Task<Result<Json::Value>>(
    const Json::Value& args,
    const UserContext& ctx
)>;

std::unordered_map<std::string, CapabilityHandler> tier1_map;
```

- Key: capability signature (`"kernel:1:db.query"`)
- Value: direct function pointer (zero-overhead dispatch)
- Populated during kernel startup from hardcoded registrations
- **Never changes at runtime** — kernel capabilities do not hot-reload

Tier 1 is the hot path for all `kernel:*` calls. No database or cache lookup occurs.

---

## Tier 2 Cache

### Structure

```cpp
struct CachedCapability {
    UUID id;
    std::string signature;
    std::string provider_type;     // "extension" or "sidecar"
    std::string extension_name;
    std::string scope;             // "instance" or "user"
    std::string rbac_rule;
    bool enabled;
};

// Key: "namespace:version:function" (instance scope)
// or   "namespace:version:function:user:<user_id>" (user scope)
std::unordered_map<std::string, CachedCapability> tier2_cache;
```

### Cache Population

The cache is populated:

1. **On startup:** Query all enabled capabilities from `plinth.capabilities` and load into memory.
2. **On LISTEN/NOTIFY event:** Reload the affected entries (see §Cache Invalidation).

The cache is a **read-optimized, eventually consistent** view of `plinth.capabilities`. The authoritative data is always in PostgreSQL.

### Cache Key Format

- Instance scope: `"terminal:1:shell"`
- User scope: `"terminal:1:shell:user:550e8400-e29b-41d4-a716-446655440000"`

A lookup first checks for a user-scope key (if `user_id` is available), then falls back to the instance-scope key.

---

## Cache Invalidation (0.2.3)

### LISTEN/NOTIFY Channel

Channel name: `plinth_capability_changed`

### Notification Payload

```json
{
  "action": "register" | "deregister" | "disable" | "enable",
  "signature": "namespace:version:function",
  "scope": "instance" | "user",
  "extension_name": "ext_name_or_null"
}
```

### Invalidation Behavior

On receiving a notification:

1. **`register`:** Fetch the new capability from `plinth.capabilities` by signature+scope. Insert into cache.
2. **`deregister`:** Remove the matching entry from cache.
3. **`disable`:** If `extension_name` is provided, mark all matching entries as `enabled = false` in cache. Otherwise, mark the specific entry.
4. **`enable`:** Same as disable but set `enabled = true`.

### Who Sends Notifications

The registration functions in ICD-0.2.0 (`registerCapability`, `deregisterCapability`, `disableByExtension`, `enableByExtension`) send `NOTIFY plinth_capability_changed` with the appropriate payload after each successful mutation.

### Multi-Node Behavior

PostgreSQL LISTEN/NOTIFY is connection-scoped. Every Plinth node maintains a dedicated listener connection that subscribes to `plinth_capability_changed`. When any node mutates the registry, all nodes receive the notification and update their local caches.

This is sufficient for eventual consistency. There is no cache coherence guarantee within a single transaction boundary — a capability registered on node A may not be resolvable on node B until the NOTIFY propagates (typically < 10ms on the same PG instance).

### Reconnect-triggered full resync

LISTEN/NOTIFY delivery is connection-scoped: a notification fired while the listener is reconnecting is lost forever. To bound the divergence, the listener calls:

```cpp
auto reload_tier2_cache(const Config::Database& db_cfg) -> std::size_t;
```

after every successful LISTEN open (initial connect *and* reconnect). `reload_tier2_cache` takes the resolver write lock, clears the Tier 2 cache, and re-runs the `WHERE enabled = true` load from `plinth.capabilities`. Missed-NOTIFY divergence is therefore bounded by one reconnect-backoff window (≤ 1 s) plus one `SELECT`, rather than by process lifetime. Tier 1 handlers are untouched.

This reconnect resync was added in 0.2.4 as an amendment after an architect question about missed-NOTIFY TTL; it is now the canonical recovery mechanism for listener churn.

---

## cap.batch() Behavioral Contract

`cap.batch()` is semantically equivalent to `Promise.all()` over individual `cap.call()` invocations (per DESIGN-quickjs-bridge.md §7.3):

```javascript
const results = await cap.batch([
    ["terminal:1:shell", "ls"],
    ["fs:1:read", "/etc/hostname"]
]);
```

C++ surface (as implemented in 0.2.5):

```cpp
struct BatchResult {
    std::optional<std::vector<CapabilityResult>> values = std::nullopt;  // success
    std::optional<CapabilityError>               error = std::nullopt;   // first failure
    std::size_t                                  failed_index = 0;       // index of the failing call on error; unspecified on success
};

auto batch_call_capability(std::vector<CapabilityCall> calls,
                           const UserContext& ctx) -> BatchResult;
```

Behavioral guarantees:
- Each call in the batch goes through the full resolution pipeline independently (RBAC check, tier resolution, dispatch).
- Calls **may** be executed concurrently where possible; sequential dispatch is a conforming initial implementation (see below).
- If any call fails, the entire batch rejects with the first error. Individual call results before the failure are discarded. `failed_index` points at the element of the input vector that aborted the batch — useful for downstream error reporting and audit debugging.
- Call depth is shared across the batch — all calls in a batch inherit the same `call_depth` from the caller (normalised to `calls[0].call_depth`; per-element values are ignored).
- Empty input resolves to `BatchResult{.values = {}}`, consistent with `Promise.all([])` resolving to `[]`.

**Initial implementation is permitted to be sequential.** DESIGN-quickjs-bridge.md §7.3 classifies concurrency as "an optimization opportunity, not a correctness requirement", and this ICD explicitly allows the initial form `Promise.all(calls.map(c => cap.call(...c)))`. The 0.2.5 kernel ships sequential dispatch because no thread pool is wired and no caller demands parallelism; this is expected to be revisited alongside 0.2.6 (async wrapper) or 0.3.3 (first JS `Promise.all` user). Optimization of Tier 3 multiplexing is deferred to 0.8.

---

## Call Depth Tracking

Per DESIGN-quickjs-bridge.md §4.4:

- Every `callCapability()` invocation carries a `call_depth` counter.
- The counter starts at 0 for the originating request (HTTP or WebSocket).
- Each hop through the capability registry increments the counter by 1.
- Default maximum depth: **8**.
- Exceeding the limit returns error `call_depth_exceeded`.
- The counter is per originating request, not per extension.

---

## Error Codes

| Code | Condition |
|------|-----------|
| `invalid_capability` | Capability identifier fails parsing or validation |
| `capability_not_found` | No matching capability in Tier 1 map or Tier 2 cache |
| `capability_disabled` | Capability exists but `enabled = false` |
| `tier3_not_available` | Capability requires remote dispatch (not implemented until 0.8) |
| `call_depth_exceeded` | Call depth ≥ MAX_CALL_DEPTH (default 8) |
| `permission_denied` | RBAC check failed (see ICD-0.2.4 for full error shape) |
| `dispatch_error` | Handler execution failed (wraps the underlying error) |
| `dispatch_timeout` | Handler did not complete within the allowed time |

All errors are returned as structured `Result<>` error variants. When surfaced to JS via `cap.call()`, they become rejected promises with structured error objects (per DESIGN-quickjs-bridge.md §5.1).

---

## Performance Targets

- **Tier 1 resolution:** < 1μs (direct map lookup + function pointer call)
- **Tier 2 cache hit:** < 1ms (including scope precedence check)
- **Cache invalidation propagation:** < 100ms (PG LISTEN/NOTIFY latency + cache update)

These targets should be validated via benchmarks during 0.2.5 or a dedicated performance pass.

---

## Security Constraints (Non-Negotiable)

1. Every capability call passes through RBAC enforcement (ICD-0.2.4). There is no bypass path.
2. Disabled capabilities must never resolve successfully.
3. Call depth limits must be enforced to prevent infinite recursion between extensions.
4. Tier 2 cache must not serve stale enabled/disabled state for longer than the LISTEN/NOTIFY propagation window.
5. The `kernel` namespace in Tier 1 is immutable at runtime.

---

## What Must Not Be Decided Yet

- Tier 3 implementation details: remote proxy protocol, circuit breaker, timeout values, connection pooling (milestone 0.8).
- Cross-node capability routing or node-affinity (milestone 0.9).
- `cap.batch()` optimization: multiplexing same-node Tier 3 calls (milestone 0.8).
- Cache eviction or size limits (evaluate if needed based on real-world capability counts).
- Any capability discovery or introspection API.
- Dynamic Tier 1 registration (kernel capabilities are always registered at startup).

---

## Milestone Criteria

### 0.2.2 — Tier 1 + Tier 2 Resolution

**Entry:** ICD-0.2.0 implemented and merged; `plinth.capabilities` table populated with kernel capabilities on bootstrap; capability string parser (0.2.1) implemented.

**Exit:**
- `callCapability()` implemented with the exact signature and resolution algorithm above.
- Tier 1 map populated with kernel capabilities; direct dispatch working.
- Tier 2 cache loaded from database on startup; lookup and scope precedence working.
- Tier 3 stub returns `tier3_not_available`.
- Call depth tracking enforced at the configured limit.
- All error codes returned correctly for each failure case.
- Catch2 tests cover: Tier 1 hit, Tier 2 hit, Tier 2 miss, disabled capability, scope override, call depth exceeded, invalid identifier.
- Human approval of implementation plan obtained before work begins.
- CI green; all tests pass.

### 0.2.3 — Cache Invalidation

**Entry:** 0.2.2 implemented and merged; Tier 2 cache operational.

**Exit:**
- LISTEN/NOTIFY channel `plinth_capability_changed` operational.
- Registration functions in 0.2.0 send notifications on every mutation.
- Cache updates correctly on register, deregister, disable, and enable events.
- Catch2 tests cover: register → cache updated, deregister → cache cleared, disable → capability no longer resolves, enable → capability resolves again.
- Multi-node invalidation verified (two listener connections both update).
- Human approval obtained; CI green.

---

**This document is the permanent authority on capability resolution and dispatch.** Any code session implementing 0.2.2, 0.2.3, or touching the dispatch pipeline afterward **must** read this ICD, ICD-0.2.0, DESIGN-capability-registry.md, and DESIGN-quickjs-bridge.md before beginning work. Changes to resolution strategy, caching, or dispatch order require a new architecture session.
