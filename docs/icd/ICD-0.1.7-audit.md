# ICD-0.1.7-audit

**Traces to:** architecture/04-services-ha.md §1 (Audit Logging), DESIGN-logging-subsystem.md, DESIGN-rbac-philosophy.md  
**Depends on:** ICD-0.1.2-auth-sessions, ICD-0.1.3-pats, ICD-0.1.4-groups-rbac, ICD-0.1.5-rbac-enforcement, ICD-0.1.6-websocket (shared auth middleware, user context, RBAC rule checking, realtime event delivery)  
**Milestone:** 0.1.7 — Audit log: plinth.audit_log table, kernel API  
**Status:** Ready for implementation (post-review v1)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Related:** DESIGN-logging-subsystem.md, DESIGN-rbac-philosophy.md

---

## Overview

This ICD defines the canonical audit logging system for the Plinth kernel. Every significant action (authentication events, group/RBAC changes, permission denials, package operations, etc.) must be recorded through a single, consistent mechanism.

The system consists of:
- The `plinth.audit_log` table (append-only)
- The kernel `log::audit(action, detail)` C++ API
- The injected `audit.log(action, detail)` QuickJS API
- A basic query endpoint for the admin UI

All audit calls **must** route through the spdlog-based subsystem defined in DESIGN-logging-subsystem.md. The `admin` group (via the `kernel.admin` rule) has access to view all audit entries. The system follows the least-privilege, additive union philosophy defined in DESIGN-rbac-philosophy.md.

This milestone completes the 0.1 Kernel Bootstrap phase.

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

### `plinth.audit_log` table

| Column       | Type        | Constraints                          | Notes |
|--------------|-------------|--------------------------------------|-------|
| `id`         | UUID        | PK, default `gen_random_uuid()`      | |
| `timestamp`  | TIMESTAMPTZ | NOT NULL, default `NOW()`            | Indexed |
| `action`     | TEXT        | NOT NULL                             | e.g. `user.login`, `rbac.denied`, `group.created`, `pat.revoked` |
| `user_id`    | UUID        | NULL                                 | NULL for system/kernel actions |
| `session_id` | UUID        | NULL                                 | Session or PAT that triggered the action |
| `detail`     | JSONB       | NOT NULL                             | Structured event data (sanitized) |
| `ip_address` | INET        | NULL                                 | Client IP when available |
| `node_id`    | TEXT        | NOT NULL                             | Node that recorded the event (for HA) |

**Indexes:**
- `(timestamp DESC)`
- `(action, timestamp)`
- `(user_id, timestamp)`

**Retention:** Configurable (default 90 days). A scheduled task (0.7) will prune old records. Audit log is append-only — records are never updated or deleted by normal operations.

---

## Kernel APIs

### C++ API (Kernel code)

```cpp
log::audit("user.login", {
    {"username", username},
    {"success", true},
    {"auth_type", "session"}
});

log::audit("rbac.denied", {
    {"rule", "terminal.shell.execute"},
    {"user_id", userId},
    {"endpoint", "/api/terminal/shell"}
});
```

All calls are asynchronous and non-blocking. Detail objects are automatically enriched with timestamp, node_id, and (when available) user_id/session_id.

### QuickJS / Extension API

Injected into every runtime (see DESIGN-quickjs-bridge.md):

```javascript
audit.log("pat.created", {
  "name": "CI token",
  "expires_at": "2027-01-01T00:00:00Z"
});

audit.log("rbac.denied", {
  "rule": "terminal.shell.execute",
  "endpoint": "/api/terminal/shell"
});
```

Objects are JSON-serialized. Sensitive fields (passwords, raw tokens) must never be included.

---

## Audit Event Catalog

This is the canonical list of all audit actions and their required detail fields. Every ICD that emits audit events traces back to this table. Code sessions implementing audit calls must use these exact action names and include all required detail fields.

### Identity & Authentication (ICD-0.1.2, ICD-0.1.3)

| Action | Detail Fields | Trigger |
|--------|---------------|---------|
| `user.registered` | `{username}` | New user account created |
| `user.login` | `{username, success: bool, auth_type: "session"}` | Login attempt (success or failure) |
| `user.login_failed` | `{username, reason: "invalid_credentials"\|"account_disabled"\|"rate_limited"}` | Failed login attempt |
| `user.logout` | `{username}` | User logged out |
| `session.revoked` | `{session_id, revoked_by: user_id}` | Session manually revoked |
| `pat.created` | `{pat_id, name, expires_at}` | PAT created (never include raw token) |
| `pat.revoked` | `{pat_id, name, revoked_by: user_id}` | PAT revoked |

### Groups & RBAC (ICD-0.1.4)

| Action | Detail Fields | Trigger |
|--------|---------------|---------|
| `group.created` | `{group_id, name}` | Group created |
| `group.updated` | `{group_id, name, changes: {}}` | Group metadata updated |
| `group.deleted` | `{group_id, name}` | Group deleted |
| `group.member_added` | `{group_id, group_name, user_id, username}` | User added to group |
| `group.member_removed` | `{group_id, group_name, user_id, username}` | User removed from group |
| `rbac.rule_registered` | `{rule, namespace, extension_name}` | RBAC rule registered |
| `rbac.rule_granted` | `{rule, group_id, group_name}` | Rule granted to group |
| `rbac.rule_revoked` | `{rule, group_id, group_name}` | Rule revoked from group |

### RBAC Enforcement (ICD-0.1.5)

| Action | Detail Fields | Trigger |
|--------|---------------|---------|
| `rbac.denied` | `{user_id, username, rule, endpoint, method}` | HTTP route RBAC check failed |

### Capability Registry (ICD-0.2.0)

| Action | Detail Fields | Trigger |
|--------|---------------|---------|
| `capability.registered` | `{signature, provider_type, extension_name, scope, rbac_rule}` | Capability registered |
| `capability.deregistered` | `{signature, scope}` | Capability deregistered |
| `capability.extension_disabled` | `{extension_name, count}` | All capabilities for an extension disabled |
| `capability.extension_enabled` | `{extension_name, count}` | All capabilities for an extension re-enabled |

### Capability RBAC (ICD-0.2.4)

| Action | Detail Fields | Trigger |
|--------|---------------|---------|
| `capability.rbac.denied` | `{user_id, username, capability, rule, call_depth}` | Capability call RBAC check failed |

**Note:** `rbac.denied` and `capability.rbac.denied` are distinct actions. The former is for HTTP route denials (ICD-0.1.5 middleware), the latter for capability dispatch denials (ICD-0.2.4). Both are always logged.

---

## Endpoints

### GET /api/audit

**Authentication:** Required. Requires `kernel.admin` rule or equivalent audit viewing permission.

**Query Parameters:**
- `limit` (default 100, max 500)
- `offset`
- `action` (filter by action)
- `user_id` (filter by user)
- `start` / `end` (ISO 8601 timestamp range)

**Response (200 OK)**
```json
{
  "events": [
    {
      "id": "...",
      "timestamp": "2026-04-15T12:00:00Z",
      "action": "rbac.denied",
      "user_id": "...",
      "detail": { ... },
      "ip_address": "192.168.1.1"
    }
  ],
  "total": 1247
}
```

**Error codes:** `not_authenticated`, `permission_denied`, `invalid_parameter`

---

## Security Constraints (Non-Negotiable)

1. **No sensitive data.** Passwords, raw tokens, and secrets must never be stored in `detail`.
2. **Append-only.** Normal code paths may only INSERT. Updates or deletes are forbidden outside of retention cleanup.
3. **Admin visibility.** Only users with the `kernel.admin` rule (or future granular audit rules) may query the audit log.
4. **Always on.** All permission denials, group/RBAC changes, authentication events, and package lifecycle events must generate audit records.
5. **Performance.** Audit writes must be asynchronous and must not block the hot path (handled by spdlog async sink + background writer).

---

## What Must Not Be Decided Yet

- Advanced search features, full-text indexing, or analytics on the audit log (belongs in a future metrics/observability extension).
- Granular per-rule audit viewing permissions (beyond `kernel.admin`).
- Real-time streaming of audit events over WebSocket (deferred to 0.5+ realtime enhancements).
- Retention policy details beyond the basic scheduled cleanup task (to be finalized in 0.7).
- Any change to the core audit API surface (`log::audit` / `audit.log`), table structure, or the requirement that all significant actions must be audited.
- Any deviation from the least-privilege additive model defined in DESIGN-rbac-philosophy.md.

All future extensions to auditing must go through an architecture session and produce an updated ICD or design document.

---

## Milestone Criteria

**Entry:** ICD-0.1.6-websocket implemented and merged; `plinth.audit_log` table added to `migrations/schema.sql`; DESIGN-logging-subsystem.md and DESIGN-rbac-philosophy.md are locked.

**Exit:**
- `plinth.audit_log` table created with correct schema and indexes.
- `log::audit()` (C++) and `audit.log()` (QuickJS) fully functional and routed through the spdlog async path.
- All previous ICDs updated to call the audit API for their side effects.
- Basic `/api/audit` query endpoint implemented with RBAC protection.
- Retention cleanup task stubbed and tested.
- Comprehensive Catch2 tests covering audit write paths, query filtering, permission checks, and sanitization.
- Human approval of implementation plan and code diff obtained before merge.
- CI green; tests pass in both dev_mode and migration modes.

---

## Open Questions (Deferred)

- Exact default retention period and cleanup schedule (finalize in 0.7).
- Whether to add a dedicated audit viewer extension with charts and filters (0.10+).
- Integration with external log aggregators (post-1.0).

---

**This document is the permanent authority on audit logging in Plinth.** Any code session implementing 0.1.7 or emitting audit events in any future milestone **must** read this ICD, DESIGN-logging-subsystem.md, DESIGN-rbac-philosophy.md, and all prior 0.1.x ICDs before beginning work. Changes to this contract require a new architecture session.

---

## Implementation Notes (0.3.4.1)

*Added by RE-EVAL-0.3.x-arc-closeout §§2.2, 7-item-5. The pattern
originated in 0.3.3.1 for the WS `ConnectionRegistry` (ICD-0.1.6's
corresponding footer); 0.3.4.1 applies the same shape to the audit
writer.*

The 0.3.4.1 four-part follow-up added a public `plinth::log::shutdown()`
to `src/kernel/logging.{hpp,cpp}` as an audit-path shutdown gate.
The test fixtures (`tests/kernel/js/async_bridge_fixture.cpp` and
`tests/kernel/ws/ws_test_fixture.cpp`) call it in their `atexit`
handler **before** `drogon::app().quit()`. Any late
`plinth::log::audit()` call during the Drogon drain — most commonly
from `EventsController::handleConnectionClosed` — short-circuits on
the `g_audit_ready` flag and does not touch Drogon's DbClient pool,
closing the `bad_weak_ptr` → `trantor::EventLoop::loop` → `abort`
sub-path that CI surfaced on post-0.3.3.2 merge runs.

**Contract:** `g_audit_ready` has three states in the writer's
lifecycle:

1. `false` before `plinth::log::init()` returns — `audit()` warns and
   skips (original 0.2.4 behavior).
2. `true` after `init()` returns — `audit()` routes through
   `drogon::app().getDbClient()` and the async spdlog pipeline.
3. `false` after `plinth::log::shutdown()` returns — `audit()` warns
   and skips again.

The `false → true → false` lifecycle is bracket-shaped. Test
fixtures flip state 3 manually in their `atexit` handler; in
production, state 3 is entered implicitly on process exit via the
signal handlers or the graceful-shutdown path in `main.cpp`'s
`drogon::app().run()` return (no explicit shutdown call is
required of production callers, but adding one is safe).

Pattern reusable for any future kernel singleton whose lifecycle
overlaps with Drogon's `EventLoopThreadPool` drain: file-scope
atomic + public `shutdown()` method + fixture-level `atexit` gate
before `drogon::app().quit()`.

Reference implementation: `src/kernel/logging.cpp` anonymous
namespace (`g_audit_ready`, `plinth::log::shutdown()` entry point).
See also ICD-0.1.6 §Implementation Notes (0.3.3.1) for the WS
registry precedent.