# ICD-0.1.4-groups-rbac

**Traces to:** architecture/01-identity.md §2 (Groups and RBAC), DESIGN-rbac-philosophy.md  
**Depends on:** ICD-0.1.2-auth-sessions, ICD-0.1.3-pats (shared authentication middleware and user context)  
**Milestone:** 0.1.4 — Groups: CRUD, membership, RBAC rule registration (storage layer only)  
**Status:** Ready for implementation (post-review v2)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Related:** DESIGN-logging-subsystem.md, DESIGN-rbac-philosophy.md

---

## Overview

This ICD defines the groups system and the RBAC rule *storage* layer. The kernel provides storage and management primitives. Extensions register rules via `rbac.json` during package install (0.4.x); the kernel stores them.

**Core philosophy (per DESIGN-rbac-philosophy.md):**
- Permissions are granted exclusively through explicit rules.
- A user’s effective permissions are the **union** of all rules granted to any group they belong to.
- Users may belong to multiple groups.
- The `admin` group is granted the `kernel.admin` rule (or equivalent comprehensive set) by default. It is **not** an absolute bypass.
- Least privilege is encouraged. Granular rules (`system.backup.run`, `packages.install`, `users.manage`, `rbac.rules.grant`, etc.) should be used wherever possible.

This ICD covers **storage and CRUD only**. Enforcement middleware is defined in ICD-0.1.5. Full two-phase validation and `rbac.json` parsing occur in 0.4.x.

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

### `plinth.groups` table

| Column       | Type        | Constraints                          | Notes |
|--------------|-------------|--------------------------------------|-------|
| `id`         | UUID        | PK, default `gen_random_uuid()`      | |
| `name`       | TEXT        | UNIQUE, NOT NULL, 1-64 chars, `[a-z0-9_-]` | Lowercase enforced |
| `description`| TEXT        | NULL, 0-256 chars                    | |
| `built_in`   | BOOLEAN     | NOT NULL, default `false`            | `true` for `admin` and `everyone` |
| `created_at` | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |

### `plinth.group_members` table

| Column     | Type        | Constraints                          | Notes |
|------------|-------------|--------------------------------------|-------|
| `group_id` | UUID        | FK → `plinth.groups.id`, NOT NULL    | |
| `user_id`  | UUID        | FK → `plinth.users.id`, NOT NULL     | |
| `added_at` | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| **PK**     | `(group_id, user_id)` | Composite key                      | Prevents duplicates |

### `plinth.rbac_rules` table

| Column         | Type        | Constraints                          | Notes |
|----------------|-------------|--------------------------------------|-------|
| `id`           | UUID        | PK, default `gen_random_uuid()`      | |
| `rule`         | TEXT        | NOT NULL, UNIQUE                     | e.g. `kernel.admin`, `terminal.shell.execute` |
| `namespace`    | TEXT        | NOT NULL                             | Must match capability namespace |
| `description`  | TEXT        | NOT NULL, 0-256 chars                | Human-readable |
| `extension_name`| TEXT       | NOT NULL                             | Package/kernel that registered it |
| `created_at`   | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| `orphaned_at`  | TIMESTAMPTZ | NULL                                 | Set when extension disabled |

### `plinth.group_rules` table

| Column      | Type        | Constraints                          | Notes |
|-------------|-------------|--------------------------------------|-------|
| `group_id`  | UUID        | FK → `plinth.groups.id`, NOT NULL    | |
| `rule_id`   | UUID        | FK → `plinth.rbac_rules.id`, NOT NULL| |
| `granted_at`| TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| **PK**      | `(group_id, rule_id)` | Composite key                    | Prevents duplicates |

**Note:** Full rule lifecycle (registration from `rbac.json`, rule-validator + RBAC-test validation, orphaned rule handling) is implemented in milestone 0.4. This milestone only provides the storage tables, built-in group creation, and basic CRUD.

---

## Endpoints

(All endpoints require a valid session or PAT from the shared middleware. See ICD-0.1.5 §Shared Request Context for exact fields attached at each middleware stage.)

### POST /api/groups, GET /api/groups, GET /api/groups/{id}, PUT /api/groups/{id}, DELETE /api/groups/{id}
(CRUD for groups — requires appropriate rules or `kernel.admin`. Built-in groups cannot be modified or deleted.)

**Error codes:** `missing_name`, `name_invalid`, `group_exists`, `group_not_found`, `permission_denied`, `not_authenticated`

**Side effects (all via `log::audit()`):** `group.created`, `group.updated`, `group.deleted`, `group.member_added`, `group.member_removed`

### POST /api/groups/{id}/members, DELETE /api/groups/{id}/members/{user_id}
(Membership management — requires appropriate rules or `kernel.admin`.)

**Error codes:** `missing_user_id`, `already_member`, `not_a_member`, `group_not_found`, `user_not_found`, `permission_denied`, `not_authenticated`

### POST /api/rbac/rules
**Internal only.** Called by the kernel/package system (0.4.x) when processing `rbac.json` or during kernel bootstrap. Must not be exposed as a public API.

**Request & Response** as defined in previous drafts (rule, namespace, description, extension_name).

**Error codes:** `invalid_rule_format`, `rule_exists`, `namespace_mismatch`

**Side effects:** Rule inserted; audit `rbac.rule_registered` via `log::audit()`.

### POST /api/groups/{id}/rules, DELETE /api/groups/{id}/rules/{rule}
(Grant/revoke rules to groups — requires appropriate rules or `kernel.admin`.)

**Error codes:** `missing_rule`, `rule_not_found`, `rule_already_granted`, `permission_denied`, `not_authenticated`

**Side effects:** Audit `rbac.rule_granted` / `rbac.rule_revoked` via `log::audit()`.

---

## Rule Naming Convention

Rules follow `<namespace>.<action>.<resource>` (e.g. `terminal.shell.execute`, `kernel.admin`, `system.backup.run`, `packages.install`). The namespace **must** match a capability declared in the extension’s `capabilities.json` (or `kernel` for built-in rules). This is enforced during rule-validator checks in 0.4.6.

---

## Security Constraints (Non-Negotiable)

1. Built-in groups (`admin`, `everyone`) have `built_in = true` and cannot be modified or deleted.
2. The `admin` group is granted the `kernel.admin` rule (or equivalent) by default. It is **not** an absolute bypass — it flows through normal rule checking (see ICD-0.1.5).
3. Rule registration is internal/kernel-only. External clients must not call `POST /api/rbac/rules`.
4. Orphaned rules (`orphaned_at` set) remain in the system for admin visibility when their extension is disabled.
5. All group/rule changes are audited via DESIGN-logging-subsystem.md. No raw sensitive data in logs.
6. Group membership lookups must remain efficient for the hot auth path.

---

## What Must Not Be Decided Yet

- Specific list or granularity of kernel-level rules beyond `kernel.admin` (to be refined during 0.1.7–0.7 while respecting DESIGN-rbac-philosophy.md).
- Whether `admin` receives a single `kernel.admin` rule or is automatically granted every registered rule.
- Rule wildcards (`*.manage`, `terminal.*`), group hierarchy/inheritance, dynamic rule evaluation, or attribute-based access control.
- Session-level permission caching strategy.
- Any deviation from the additive union model or explicit rule declaration requirement.
- Any change that would make membership in `admin` an absolute bypass outside the normal rule-checking path defined in ICD-0.1.5.

All future extensions to the RBAC model must conform to DESIGN-rbac-philosophy.md. Local optimization is forbidden.

---

## Milestone Criteria

**Entry:** ICD-0.1.2 and ICD-0.1.3 implemented and merged; `plinth.groups`, `plinth.group_members`, `plinth.rbac_rules`, and `plinth.group_rules` tables present in `migrations/schema.sql`; built-in groups and `kernel.admin` rule created on bootstrap.

**Exit:**
- All listed endpoints (except internal rule registration) implemented and covered by Catch2 tests.
- Built-in group protection, orphaned flag handling, and additive membership correctly tested.
- Rule registration endpoint works when called internally and rejects invalid namespaces.
- All audit events emitted exclusively via DESIGN-logging-subsystem.md path.
- Shared auth middleware correctly passes context to this layer.
- No structural decisions made that would conflict with DESIGN-rbac-philosophy.md, 0.4.x package system, or ICD-0.1.5 enforcement.
- Human approval of implementation plan and diff obtained before merge.
- CI green; tests pass in both dev_mode and migration modes.

---

## Open Questions (Deferred)

- Exact set of kernel-level rules to register by default.
- Convenience built-in groups beyond `admin` and `everyone` (re-evaluate after 1.0).
- UI behavior for namespace grouping and orphaned rules (0.6.x).

---

**This document is the permanent authority on groups and RBAC storage.** Any code session implementing 0.1.4 or touching groups/RBAC afterward **must** read this ICD, DESIGN-rbac-philosophy.md, the prior auth ICDs, and architecture/01-identity.md §2 before beginning work. Changes to these contracts require a new architecture session.