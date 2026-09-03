# ICD-0.2.0-capability-registry

**Traces to:** architecture/02-capabilities.md §1 (Capability Registry), DESIGN-capability-registry.md
**Depends on:** ICD-0.1.4-groups-rbac (RBAC rule storage), ICD-0.1.7-audit (audit logging)
**Milestone:** 0.2.0 — Registry table (plinth.capabilities), registration API
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DESIGN-rbac-philosophy.md, DESIGN-logging-subsystem.md

---

## Overview

This ICD defines the capability registry storage layer and registration API. The registry is the central catalog of all capabilities available in a Plinth instance — kernel-provided, extension-provided, and sidecar-provided.

This ICD covers **storage, registration, deregistration, and lifecycle management only**. Resolution (how `cap.call()` finds a handler) is defined in ICD-0.2.2. RBAC integration (how capabilities map to permission rules) is defined in ICD-0.2.4.

The capability registry replaces package-level dependencies entirely (per DESIGN-capability-registry.md). Extensions declare what capabilities they **provide** and what they **require**. The kernel resolves calls at runtime.

All audit events must be emitted exclusively via the canonical path in DESIGN-logging-subsystem.md (`log::audit()` in C++, `audit.log()` in JS).

---

## Standardized Error Shape

All error responses in this ICD use:

```json
{
  "error": "error_code_snake_case",
  "message": "Human-readable description (optional in production builds)"
}
```

---

## Data Model

### `plinth.capabilities` table

| Column           | Type        | Constraints                                | Notes |
|------------------|-------------|--------------------------------------------|-------|
| `id`             | UUID        | PK, default `gen_random_uuid()`            | |
| `namespace`      | TEXT        | NOT NULL                                   | Lowercase, `[a-z][a-z0-9_]{0,63}` |
| `version`        | INTEGER     | NOT NULL, CHECK (`version` > 0)            | Positive integer, increments on breaking changes |
| `function`       | TEXT        | NOT NULL                                   | `[a-z][a-z0-9_.]{0,127}` — dots allowed for grouping (e.g. `db.query`) |
| `signature`      | TEXT        | NOT NULL                                   | Full canonical string: `namespace:version:function` |
| `provider_type`  | TEXT        | NOT NULL, CHECK IN ('kernel', 'extension', 'sidecar') | |
| `extension_name` | TEXT        | NULL                                       | Required when `provider_type` is 'extension' or 'sidecar'; NULL for 'kernel' |
| `scope`          | TEXT        | NOT NULL, CHECK IN ('instance', 'user'), default 'instance' | See Scopes below |
| `description`    | TEXT        | NOT NULL, 0–256 chars                      | Human-readable |
| `rbac_rule`      | TEXT        | NOT NULL                                   | The RBAC rule required to invoke this capability (see ICD-0.2.4) |
| `registered_at`  | TIMESTAMPTZ | NOT NULL, default `NOW()`                  | |
| `enabled`        | BOOLEAN     | NOT NULL, default `true`                   | Set to `false` when extension is disabled |

**Unique constraint:** `UNIQUE (namespace, version, function, scope)`

**Index:** `CREATE INDEX idx_capabilities_lookup ON plinth.capabilities (namespace, version, function) WHERE enabled = true;`

### Scopes

- **Instance scope** (default): Available to all authenticated users on the node.
- **User scope**: Per-user capability (e.g. personal LLM provider using user's own API key). User-scope overrides instance-scope for that user during resolution (see ICD-0.2.2).

**User-scope deferral (0.2.x):** The `scope` column and unique constraint support the user-scope concept, but the table does not yet include a `user_id` column. In 0.2.x, only instance-scope capabilities may be registered. Per-user registration (requiring a `user_id` column and updated unique constraint) will be added in 0.4.x when the package install workflow and real usage patterns inform the schema design. Code sessions implementing 0.2.x must reject `scope = 'user'` registrations with error `user_scope_not_supported`.

### Capability Identifier Format

Exact canonical format (per DESIGN-capability-registry.md):

```
namespace:version:function
```

Examples:
- `terminal:1:shell`
- `fs:1:read`
- `kernel:1:db.query`
- `llm:1:complete`

The `signature` column stores this canonical form. Parameters and return types are documentation-only and not stored in the registry (deferred — see "What Must Not Be Decided Yet").

---

## Validation Rules

### Namespace
- Pattern: `[a-z][a-z0-9_]{0,63}` (1–64 chars, lowercase, starts with letter)
- `kernel` is reserved for kernel-provided capabilities
- Must match the extension's declared namespace when `provider_type` is 'extension'

### Version
- Positive integer (≥ 1)
- Increments only on breaking signature changes (non-breaking additions do not require a bump)

### Function
- Pattern: `[a-z][a-z0-9_.]{0,127}` (1–128 chars, lowercase, starts with letter)
- Dots are permitted for logical grouping: `db.query`, `db.exec`, `shell.execute`
- Must not start or end with a dot; no consecutive dots

### Signature
- Must exactly equal `namespace:version:function` constructed from the individual fields
- Computed and validated on registration; not user-provided

### RBAC Rule
- Must conform to the rule naming convention in ICD-0.1.4: `<namespace>.<action>` or `<namespace>.<resource>.<action>`
- The rule must already exist in `plinth.rbac_rules` at the time of capability registration, or be registered in the same transaction (during package install)
- **Validation is application-level, not a database foreign key.** During package install (0.4.x), rules and capabilities are registered in the same transaction — an FK would require strict ordering within the transaction. The kernel validates rule existence in application code before committing.

---

## Registration API

### Internal Registration

Registration is **internal only** — called by the kernel during bootstrap and by the package system during extension install (0.4.x). It is not exposed as a public HTTP endpoint.

```cpp
struct CapabilityRegistration {
    std::string namespace_;
    int version;
    std::string function;
    std::string provider_type;    // "kernel", "extension", "sidecar"
    std::optional<std::string> extension_name;  // required for extension/sidecar
    std::string scope;            // "instance" or "user"
    std::string description;
    std::string rbac_rule;
};

Result<UUID> registerCapability(const CapabilityRegistration& reg);
Result<void> deregisterCapability(const std::string& signature, const std::string& scope);
Result<void> disableByExtension(const std::string& extension_name);
Result<void> enableByExtension(const std::string& extension_name);
```

### `registerCapability(reg)`

1. Validate all fields per rules above.
2. Compute `signature` from `namespace:version:function`.
3. Verify that `rbac_rule` exists in `plinth.rbac_rules` (or is being registered in the same transaction).
4. Insert into `plinth.capabilities`.
5. Emit audit event: `capability.registered`.
6. Notify cache invalidation (LISTEN/NOTIFY — see ICD-0.2.2).

**On duplicate (unique constraint violation):** Return error `capability_exists`. The caller must explicitly deregister the old capability before re-registering a new version. This prevents accidental overwrites during package updates.

### `deregisterCapability(signature, scope)`

1. Delete the row matching `(signature, scope)`.
2. Emit audit event: `capability.deregistered`.
3. Notify cache invalidation.

**If not found:** Return error `capability_not_found`.

### `disableByExtension(extension_name)`

1. Set `enabled = false` for all capabilities where `extension_name` matches.
2. Emit audit event: `capability.extension_disabled` with `extension_name`.
3. Notify cache invalidation.

Used during package disable (0.4.x). Capabilities remain in the registry for admin visibility but are excluded from resolution.

### `enableByExtension(extension_name)`

1. Set `enabled = true` for all capabilities where `extension_name` matches.
2. Emit audit event: `capability.extension_enabled` with `extension_name`.
3. Notify cache invalidation.

Used during package re-enable (0.4.x).

---

## Error Codes

| Code | HTTP Status | Condition |
|------|-------------|-----------|
| `invalid_namespace` | — | Namespace fails validation pattern |
| `invalid_version` | — | Version ≤ 0 |
| `invalid_function` | — | Function name fails validation pattern |
| `invalid_scope` | — | Scope is not 'instance' or 'user' |
| `invalid_provider_type` | — | Provider type not in allowed set |
| `missing_extension_name` | — | Extension/sidecar provider without extension_name |
| `capability_exists` | — | Unique constraint violation on (namespace, version, function, scope) |
| `capability_not_found` | — | Deregistration target does not exist |
| `rbac_rule_not_found` | — | Referenced RBAC rule does not exist in plinth.rbac_rules |
| `user_scope_not_supported` | — | User-scope registration attempted (deferred to 0.4.x) |

These are internal error codes (returned as `Result<>` error variants), not HTTP status codes — registration is not an HTTP endpoint.

---

## Bootstrap: Kernel Capabilities

On startup, the kernel registers its own built-in capabilities. The exact set will be finalized during implementation of 0.2.0 and subsequent milestones, but must include at minimum:

| Capability | Rule | Description |
|------------|------|-------------|
| `kernel:1:db.query` | `kernel.db.query` | Execute a read query against the database |
| `kernel:1:db.exec` | `kernel.db.exec` | Execute a write statement against the database |
| `kernel:1:log` | `kernel.log` | Write to the application log |
| `kernel:1:audit` | `kernel.audit` | Write to the audit log |
| `kernel:1:config.get` | `kernel.config.get` | Read kernel configuration values |

All kernel capabilities have `provider_type = 'kernel'`, `extension_name = NULL`, `scope = 'instance'`.

Additional kernel capabilities will be registered as kernel subsystems are implemented (realtime, storage, scheduler, etc.). Each capability must have a corresponding RBAC rule registered in `plinth.rbac_rules` before or during the same bootstrap transaction.

---

## Audit Events

All events emitted via `log::audit()`:

| Action | Detail | Trigger |
|--------|--------|---------|
| `capability.registered` | `{signature, provider_type, extension_name, scope, rbac_rule}` | After successful registration |
| `capability.deregistered` | `{signature, scope}` | After successful deregistration |
| `capability.extension_disabled` | `{extension_name, count}` | After disabling all capabilities for an extension |
| `capability.extension_enabled` | `{extension_name, count}` | After enabling all capabilities for an extension |

---

## Security Constraints (Non-Negotiable)

1. Registration is internal-only. External clients must never directly call registration APIs.
2. The `kernel` namespace is reserved. Only `provider_type = 'kernel'` may register capabilities in the `kernel` namespace.
3. An extension may only register capabilities in its own namespace. This is enforced during package install (0.4.x).
4. Every capability must have a corresponding RBAC rule. Capabilities without rules cannot be registered.
5. Disabled capabilities must not appear in resolution results (ICD-0.2.2).
6. All registration/deregistration operations are audited.

---

## What Must Not Be Decided Yet

- Capability discovery or listing API for extensions/LLMs (deferred post-1.0 per DESIGN-capability-registry.md).
- Version range matching or semantic versioning for capabilities.
- Parameter type storage, validation, or enforcement in the registry (currently documentation-only).
- Dynamic capability registration from JS runtime (extensions register via `capabilities.json` during install only).
- User-scope capability registration UX or API (the storage supports it; the registration workflow is deferred to 0.4.x).
- Any change to the unique constraint, identifier format, or RBAC rule requirement.

---

## Milestone Criteria

**Entry:** ICD-0.1.4 (groups/RBAC storage) and ICD-0.1.7 (audit) implemented and merged. `plinth.capabilities` table present in `migrations/schema.sql`.

**Exit:**
- `plinth.capabilities` table created with all columns, constraints, and indexes as specified.
- All four registration functions (`register`, `deregister`, `disableByExtension`, `enableByExtension`) implemented and passing Catch2 tests.
- Validation rules enforced for all fields; rejected registrations return correct error codes.
- Duplicate registration correctly returns `capability_exists`.
- Kernel bootstrap registers the minimum capability set on startup.
- `kernel` namespace reservation enforced.
- All audit events emitted via `log::audit()`.
- LISTEN/NOTIFY notification sent on every mutation (channel defined in ICD-0.2.2).
- No structural decisions made that conflict with DESIGN-capability-registry.md or DESIGN-rbac-philosophy.md.
- Human approval of implementation plan obtained before work begins.
- CI green; all tests pass.

---

**This document is the permanent authority on capability registration and storage.** Any code session implementing 0.2.0 or touching the capability registry afterward **must** read this ICD, DESIGN-capability-registry.md, DESIGN-rbac-philosophy.md, and the relevant 0.1 ICDs before beginning work. Changes to this contract require a new architecture session.
