# DESIGN-capability-registry

**Status:** Approved v1  
**Scale:** 3 — Architecture Arc (foundational for all extension and sidecar work)  
**Traces to:** architecture/02-capabilities.md §1 (Capability Registry), DESIGN-rbac-philosophy.md, ICD-0.1.4-groups-rbac, ICD-0.1.5-rbac-enforcement, DESIGN-logging-subsystem.md, DESIGN-quickjs-bridge.md  
**Milestone:** 0.2.0–0.2.5 (Capability Registry)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Decision Date:** 2026-04-15  
**Author:** the maintainer (Architect) + Grok 4.20 Multi-Agent (Architecture Session with Leo, Enrico, Hans)

---

## Decision

The capability registry is the central dispatch and authorization mechanism of Plinth. It replaces all package-level dependency declarations. Extensions declare what capabilities they **provide** and what they **require**. The kernel resolves every `cap.call()` through a three-tier system and enforces RBAC on every call using the least-privilege, additive union model defined in DESIGN-rbac-philosophy.md.

Capability-to-rule mapping is **explicit and non-version-aware**. A capability such as `terminal:1:shell` or `terminal:2:shell` both map to the single rule `terminal.shell.execute`. Versioning is strictly an API contract concern handled by the QuickJS bridge and handler signatures, never a security concern.

All permission denials must be audited via the canonical `log::audit()` path from DESIGN-logging-subsystem.md.

This design document is the permanent authority for all 0.2.x implementation sessions. No structural decisions about resolution, scoping, caching, or RBAC integration may be made outside this document.

---

## Core Concepts

### Capability Identifier Format

Exact canonical format:

```
namespace:version:function(params) -> return_type
```

Examples:
- `terminal:1:shell(command: string) -> result`
- `fs:1:read(path: string) -> string`
- `kernel:1:db.query(sql: string) -> rows`
- `llm:1:complete(prompt: string, model: string) -> response`

- `namespace`: lowercase, matches extension namespace
- `version`: positive integer, increments only on breaking changes
- `function`: the callable name
- Parameters and return type are for documentation and future type checking (not enforced in 0.2)

### Scopes

- **Instance scope**: Available to all users on the node (default)
- **User scope**: Per-user (e.g. personal LLM provider using user's own API key). User-scope overrides instance-scope for that user.

---

## Data Model

### `plinth.capabilities` table

| Column            | Type        | Constraints                          | Notes |
|-------------------|-------------|--------------------------------------|-------|
| `id`              | UUID        | PK, default `gen_random_uuid()`      | |
| `namespace`       | TEXT        | NOT NULL                             | |
| `version`         | INTEGER     | NOT NULL                             | |
| `function`        | TEXT        | NOT NULL                             | |
| `signature`       | TEXT        | NOT NULL                             | Full identifier string |
| `provider_type`   | TEXT        | NOT NULL                             | 'kernel', 'extension', 'sidecar' |
| `extension_name`  | TEXT        | NULL                                 | For extension/sidecar providers |
| `scope`           | TEXT        | NOT NULL                             | 'instance' or 'user' |
| `description`     | TEXT        | NOT NULL                             | Human readable |
| `registered_at`   | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| `enabled`         | BOOLEAN     | NOT NULL, default `true`             | For future soft-disable |

**Unique constraint:** `(namespace, version, function, scope)`

---

## Three-Tier Resolution

Every `cap.call()` follows this exact order:

**Tier 1 — In-process (zero overhead)**  
Capabilities provided by the kernel itself or by the same QuickJS runtime resolve to a direct function pointer. No registry lookup. This is the hot path for `kernel:1:*` calls.

**Tier 2 — Local-node (in-memory cache)**  
Capabilities provided by other extensions on the same node use a per-node in-memory cache invalidated via PostgreSQL LISTEN/NOTIFY. Cache is rebuilt on package install/uninstall/enable/disable (rare events).

**Tier 3 — Remote-proxy (cross-node / sidecar)**  
For capabilities on a different node or provided by a sidecar, the registry performs a PG lookup for location and proxies the call via HTTP. Circuit breaker and timeout (default 500ms) apply.

**Batching (`cap.batch()`)**  
Multiple calls are dispatched as a single unit. Calls to the same tier are parallelized. Calls to the same remote node are multiplexed over one connection.

---

## RBAC Integration

Before any capability is executed, the enforcement middleware from ICD-0.1.5 is invoked using the rule derived from the capability:

- `terminal:1:shell(...)` → rule `terminal.shell.execute`
- `packages:1:install(...)` → rule `packages.install`

Mapping is **non-version-aware** and **explicit**. The rule must exist in `plinth.rbac_rules`. If the rule does not exist, the call is denied and audited.

Permission checking uses the additive union model from DESIGN-rbac-philosophy.md. The `kernel.admin` rule grants all capabilities.

---

## Public APIs

### From QuickJS (injected by bridge)

```javascript
const result = await cap.call("terminal:1:shell", "ls -la");

const results = await cap.batch([
  ["fs:1:read", "/etc/hostname"],
  ["terminal:1:shell", "whoami"]
]);
```

Both throw on permission denial or capability not found. Errors are structured and contain the required rule when applicable.

### C++ Internal Dispatch

Used by kernel and bridge:

```cpp
Result<Json> callCapability(const CapabilityCall& call, const UserContext& ctx);
```

---

## What Must Not Be Decided Yet

- Any form of version range matching or semantic versioning for capabilities.
- Dynamic rule generation or custom capability-to-rule mapping logic by extensions.
- Per-call resource budgets or execution timeouts beyond what the QuickJS bridge already enforces.
- Client-side capability discovery API (e.g. `registry.list()` for LLM tool generation) — this is explicitly deferred (see architecture "Tool Discovery" thought experiment).
- Any change to the three-tier resolution order, non-version-aware rule mapping, or the requirement that every capability call goes through RBAC.
- Any deviation from the least-privilege additive union philosophy in DESIGN-rbac-philosophy.md.
- **Registry-schema fields for the cross-cutting composition framework** (`surface_traits`, `slots`, `augments_traits` — see architecture/05-extensions.md §4). Those fields are reserved in the 0.4 manifest schema but are not yet consumed by the capability registry. The dispatch model for augmentation (slot injection vs. event stream vs. capability-registered handlers) is explicitly an open question; the registry must not grow composition-aware dispatch logic until that architecture session concludes.

All future changes to capability resolution, scoping, or authorization must go through a new architecture session and update this document.

---

## Milestone Criteria (0.2.0–0.2.5)

**Entry:** All 0.1 ICDs implemented and merged; audit logging, RBAC enforcement, and QuickJS bridge (0.3.0–0.3.2) available.

**Exit:**
- `plinth.capabilities` table and registration API complete (0.2.0).
- Full parser, three-tier resolver, cache with LISTEN/NOTIFY invalidation, and batch support implemented (0.2.1–0.2.5).
- Every capability call correctly maps to its RBAC rule and is enforced per ICD-0.1.5.
- All permission denials are audited via DESIGN-logging-subsystem.md.
- Comprehensive tests cover all three tiers, scopes, batching, RBAC bypass with `kernel.admin`, and error cases.
- Implementation strictly follows this design document and DESIGN-rbac-philosophy.md.
- Human approval of implementation plan for each 0.2.x task obtained before work begins.
- CI green and all tests pass in HA configuration.

---

## Open Questions

1. Exact default set of kernel capabilities and their corresponding rules (to be finalized during implementation).
2. Whether to add a queryable registry API for extensions/LLMs (deferred until after 1.0).
3. Performance target for Tier 2 cache hit (< 1ms) — confirm via benchmarking in 0.2.5.

---

**This document is the permanent authority on the Plinth capability registry.** Any code session working on 0.2.x or any future capability, extension, or sidecar code **must** read this document, DESIGN-rbac-philosophy.md, and all relevant 0.1 ICDs before beginning work. Changes to resolution strategy, mapping rules, or contracts require a new architecture session and revision of this document.