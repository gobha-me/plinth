# ICD-0.1.5-rbac-enforcement

**Traces to:** architecture/01-identity.md §2 (Groups and RBAC), DESIGN-rbac-philosophy.md  
**Depends on:** ICD-0.1.2-auth-sessions, ICD-0.1.3-pats, ICD-0.1.4-groups-rbac (shared auth middleware, user context, groups & rule storage)  
**Milestone:** 0.1.5 — RBAC: enforcement middleware, permission check on routes  
**Status:** Ready for implementation (post-review v2)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Related:** DESIGN-logging-subsystem.md, DESIGN-rbac-philosophy.md

---

## Overview

This ICD defines the RBAC **enforcement middleware** — the Drogon filter that runs after authentication (ICD-0.1.2/0.1.3) but before route handlers. It checks whether the authenticated user possesses the required rule(s) as defined in ICD-0.1.4.

This ICD does **not** define rule registration, group membership, or the two-phase validation process — those live in ICD-0.1.4 and milestone 0.4. It only defines how declared permissions are checked at runtime and what happens on denial, in full accordance with the least-privilege, additive union philosophy defined in DESIGN-rbac-philosophy.md.

All audit events must be emitted exclusively via the canonical path in DESIGN-logging-subsystem.md (`log::audit()` in C++, `audit.log()` in JS).

---

## Standardized Error Shape

All error responses in this ICD use:

```json
{
  "error": "error_code_snake_case",
  "rule": "optional.rule.name",
  "message": "Human-readable description (optional in production builds)"
}
```

---

## Middleware Contract

### Filter Ordering (Drogon)

```
Request → Authentication Filter (0.1.2/0.1.3) → RBAC Enforcement Filter → Route Handler
                                           │
                                           └─ 403 Forbidden (permission_denied) on failure
```

The RBAC filter **must** run immediately after the shared authentication filter. It receives the request context populated by the auth filter and enriches it with permission data.

### Shared Request Context

The request context is built incrementally by the middleware chain. This is the canonical specification — all ICDs referencing "request context" must use these exact field names and types.

**After Authentication Filter (ICD-0.1.2/0.1.3):**

| Field | Type | Source | Notes |
|-------|------|--------|-------|
| `user_id` | UUID | `plinth.users.id` or `plinth.pats.user_id` | Always set after successful auth |
| `username` | std::string | `plinth.users.username` | Always set |
| `session_id` | UUID | `plinth.sessions.id` or `plinth.pats.id` | Session ID or PAT ID |
| `auth_type` | std::string | `"session"` or `"pat"` | Which auth path succeeded |

**After RBAC Enforcement Filter (this ICD):**

| Field | Type | Source | Notes |
|-------|------|--------|-------|
| `effective_rules` | std::vector\<std::string\> | Union of all rules from all groups the user belongs to | Computed once per request via group membership + group_rules join |
| `permission_granted` | bool | RBAC check result | `true` if any required rule matched |
| `granting_rule` | std::string | The specific rule that granted access | Empty if denied |

**WebSocket context:** The WebSocket auth message (ICD-0.1.6) produces the same context fields as the HTTP auth filter. The token in `{"type": "auth", "token": "..."}` is validated using the same branching logic: if it starts with `plinth_`, use PAT path; otherwise, use session path.

**Capability dispatch context:** ICD-0.2.2's `UserContext` struct is derived from this request context. `UserContext.effective_rules` is the same `effective_rules` computed by the RBAC filter.

### Route Declaration

Routes declare required permission(s) at registration time:

```cpp
// Single required rule
METHOD_ADD(Handler::executeShell,
    "/api/terminal/shell",
    Post,
    "terminal.shell.execute");

// Multiple rules (any one grants access)
METHOD_ADD(Handler::viewItem,
    "/api/items/{id}",
    Get,
    "items.view", "items.admin");

// Kernel admin route
METHOD_ADD(Handler::manageSystem,
    "/api/admin/*",
    Post,
    "kernel.admin");
```

Public endpoints declare no rule and skip the RBAC filter.

### Permission Check Algorithm

```cpp
bool checkPermission(const RequestContext& ctx, const std::vector<std::string>& requiredRules) {
    auto userGroups = getUserGroups(ctx.user_id);

    for (const auto& rule : requiredRules) {
        auto ruleId = getRuleId(rule);
        if (!ruleId) {
            log::audit("rbac.denied", {{"rule", rule}, {"reason", "rule_not_registered"}});
            return false;  // fail closed
        }

        if (userHasRule(userGroups, ruleId)) {
            return true;
        }
    }

    log::audit("rbac.denied", {
        {"user_id", ctx.user_id},
        {"rule", requiredRules[0]},
        {"endpoint", ctx.path},
        {"method", ctx.method}
    });
    return false;
}
```

- Permissions are the **union** of all rules granted to any group the user belongs to (per DESIGN-rbac-philosophy.md).
- The `kernel.admin` rule (granted to the `admin` group by default) functions as the highest privilege rule.
- There is no absolute bypass outside the rule system. Even members of the `admin` group must possess the required rule(s).
- On any internal error the request is denied (fail-closed).

The result and the granting rule are attached to the request context for the route handler.

---

## Responses

### Allowed
The middleware attaches to the request context:
- `permission_granted: true`
- `granting_rule` (the specific rule that granted access)

The route handler executes normally.

### Denied (403 Forbidden)
```json
{
  "error": "permission_denied",
  "rule": "terminal.shell.execute",
  "message": "You do not have permission to execute shell commands. This action requires the 'terminal.shell.execute' permission."
}
```

The route handler is **never called**. The denial is always audited.

---

## Integration with Capability Registry (0.2.x)

When `cap.call()` is implemented, the capability resolver will map capabilities (e.g. `terminal:1:shell`) to their corresponding rule (`terminal.shell.execute`) using the same enforcement logic defined in this ICD. The mapping is non-version-aware. Exact mapping behavior and error formats for capability denials will be defined in a future ICD for milestone 0.2.

---

## Audit Logging

Every permission denial **must** be logged using the canonical path from DESIGN-logging-subsystem.md. Permission grants are not logged.

---

## Security Constraints (Non-Negotiable)

1. **Fail closed.** Any error during evaluation results in denial.
2. **Explicit rules only.** No inference from paths, methods, or ownership.
3. **Additive union model.** Permissions are the strict union of rules across all groups the user belongs to.
4. **kernel.admin rule.** The `admin` group is granted this rule by default. It is not an absolute bypass outside the rule system.
5. **Middleware ordering invariant.** RBAC filter must run after authentication and before business logic.
6. All rule checks must respect the philosophy in DESIGN-rbac-philosophy.md.

---

## What Must Not Be Decided Yet

- Session-level or per-user rule-set caching strategy.
- Dynamic or custom rule evaluation logic (e.g. ownership checks or extension-provided evaluators).
- Rule wildcards (`terminal.*`), group hierarchy, or attribute-based access control.
- Exact error format or behavior for capability-call denials (to be defined in 0.2.x).
- Any deviation from the additive union model, explicit rule declaration, or least-privilege philosophy defined in DESIGN-rbac-philosophy.md.
- Any change that would make membership in `admin` an absolute bypass outside normal rule checking.

This enforcement contract must remain stable. Any extension of the permission model requires a new architecture session, revision of DESIGN-rbac-philosophy.md, and updated ICD(s).

---

## Milestone Criteria

**Entry:** ICD-0.1.4-groups-rbac implemented and merged; storage tables exist; built-in groups and `kernel.admin` rule created on bootstrap; shared authentication middleware passes tests.

**Exit:**
- RBAC filter correctly implements the algorithm above and integrates into the Drogon filter chain.
- All route declaration patterns (single rule, multiple rules, no rule, kernel.admin) are exercised in tests.
- 403 responses match the standardized shape; all denials are audited via DESIGN-logging-subsystem.md.
- Least-privilege behavior, additive union, and `kernel.admin` rule handling verified.
- No route handler is ever executed on denial.
- Implementation fully conforms to DESIGN-rbac-philosophy.md.
- Human approval of implementation plan, filter ordering, and code diff obtained before merge.
- CI green; full test suite passes in both dev_mode and migration modes.

---

## Open Questions (Deferred)

- Performance implications of per-request group/rule lookups (evaluate before 0.2).
- Whether to introduce a small set of true kernel.root rules that can only be granted to admin.
- Support for wildcard rules or group inheritance (post-1.0).

---

**This document is the permanent authority on RBAC enforcement.** Any code session implementing 0.1.5 or touching authorization afterward **must** read this ICD, DESIGN-rbac-philosophy.md, the three prior auth/RBAC ICDs, and architecture/01-identity.md §2 before beginning work. Changes to this contract require a new architecture session.