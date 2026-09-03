# ICD-0.2.4-capability-rbac

**Traces to:** architecture/02-capabilities.md §1 (Capability Registry), DESIGN-capability-registry.md, DESIGN-rbac-philosophy.md
**Depends on:** ICD-0.2.0-capability-registry (storage, rbac_rule column), ICD-0.2.2-capability-resolution (dispatch pipeline), ICD-0.1.5-rbac-enforcement (permission check algorithm), ICD-0.1.4-groups-rbac (rule storage)
**Milestone:** 0.2.4 — RBAC integration with capability dispatch
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** DESIGN-logging-subsystem.md

---

## Overview

This ICD defines how RBAC enforcement integrates with the capability dispatch pipeline. It specifies the capability-to-rule mapping, where permission checking occurs in the dispatch flow, what happens on denial, and how call depth interacts with RBAC.

This ICD bridges two existing contracts:
- **ICD-0.1.5** defines how permission checks work (the algorithm, fail-closed behavior, the additive union model).
- **ICD-0.2.2** defines the dispatch pipeline where RBAC checking is invoked (step 3 of the resolution algorithm).

This ICD adds: the mapping mechanism, denial error shapes specific to capability calls, audit event formats, and the per-hop RBAC policy.

---

## Standardized Error Shape

Capability RBAC denials extend the standard error shape with the `rule` field (consistent with ICD-0.1.5):

```json
{
  "error": "permission_denied",
  "capability": "terminal:1:shell",
  "rule": "terminal.shell.execute",
  "message": "You do not have permission to call terminal:1:shell. Required rule: terminal.shell.execute"
}
```

When surfaced to JS via `cap.call()`, this becomes a rejected promise with the same structured error object (per DESIGN-quickjs-bridge.md §5.1).

---

## Capability-to-Rule Mapping

### Mapping Strategy: Explicit, Stored

Each capability has its required RBAC rule stored directly in the `rbac_rule` column of `plinth.capabilities` (defined in ICD-0.2.0). The mapping is:

1. **Not computed** — there is no algorithm that derives a rule name from a capability identifier.
2. **Explicit** — the extension author declares the mapping in `capabilities.json` during package install (0.4.x). The kernel declares it during bootstrap.
3. **Stored** — the rule name is persisted alongside the capability for zero-overhead lookup during dispatch.

### Mapping is Non-Version-Aware

Per DESIGN-capability-registry.md: `terminal:1:shell` and `terminal:2:shell` both map to the same RBAC rule (`terminal.shell.execute`). The version number is an API contract concern, not a security concern.

This means:
- If a user has `terminal.shell.execute`, they can call both `terminal:1:shell` and `terminal:2:shell`.
- Permission grants do not need updating when an extension bumps its capability version.
- Rule names never contain version numbers.

### Rule Naming Convention

Rules follow `<namespace>.<resource>.<action>` or `<namespace>.<action>` (per ICD-0.1.4):

| Capability | RBAC Rule |
|------------|-----------|
| `terminal:1:shell` | `terminal.shell.execute` |
| `fs:1:read` | `fs.read` |
| `fs:1:write` | `fs.write` |
| `kernel:1:db.query` | `kernel.db.query` |
| `kernel:1:db.exec` | `kernel.db.exec` |
| `packages:1:install` | `packages.install` |
| `llm:1:complete` | `llm.complete` |

The rule namespace **must** match the capability namespace (enforced during registration per ICD-0.2.0).

---

## Permission Check in Dispatch Pipeline

### Where It Runs

RBAC checking occurs at **step 3** of the resolution algorithm in ICD-0.2.2, after parsing and call depth checking, before tier resolution:

```
1. Parse capability identifier
2. Check call depth
3. RBAC check  ← THIS ICD
4. Tier 1 resolution
5. Tier 2 resolution
6. Tier 3 stub
```

### Check Algorithm

```cpp
Result<void> checkCapabilityPermission(
    const CapabilityCall& call,
    const UserContext& ctx
) {
    // 1. Look up the RBAC rule for this capability
    //    - Tier 1 capabilities have rules stored in the Tier 1 map
    //    - Tier 2 capabilities have rules in the cache (from rbac_rule column)
    //    - If the capability is not yet resolved, do a preliminary lookup
    //      in the Tier 2 cache to get the rule name
    std::string rule = lookupRuleForCapability(call);

    if (rule.empty()) {
        // Capability not found — this will be caught later in resolution
        // but we fail fast here if we can detect it
        return Error("capability_not_found");
    }

    // 2. Check if the user has this rule (per ICD-0.1.5 algorithm)
    //    Uses the additive union model: permission = union of all group rules
    if (userHasRule(ctx.effective_rules, rule)) {
        return Ok();
    }

    // 3. Denied — audit and return error
    log::audit("capability.rbac.denied", {
        {"user_id", ctx.user_id},
        {"username", ctx.username},
        {"capability", call.signature()},
        {"rule", rule},
        {"call_depth", call.call_depth}
    });

    return Error("permission_denied", {
        {"capability", call.signature()},
        {"rule", rule}
    });
}
```

### Rule Lookup Ordering

The RBAC check needs the rule name before full tier resolution completes. The lookup order:

1. **Tier 1 map:** If the capability is in the Tier 1 map, the rule is stored alongside the handler. Zero-cost lookup.
2. **Tier 2 cache:** If the capability is in the Tier 2 cache, the rule is in the `CachedCapability.rbac_rule` field.
3. **Cache miss = negative result.** A capability absent from both Tier 1 and Tier 2 resolves to `capability_not_found`. There is no database fallback on the hot path.

This ordering means RBAC checking and tier resolution share the same lookup — the resolver finds the capability entry (which contains both the handler reference and the rule name) in a single operation.

**Rationale for no DB fallback (deviation from the initial ICD draft, accepted in 0.2.4):** querying PG inside the dispatch path would tie the hot path to database availability and latency, and the cache already handles eventual consistency via LISTEN/NOTIFY + reconnect-triggered full resync (see ICD-0.2.2 §Reconnect-triggered full resync). The bounded-divergence guarantee there makes a DB fallback redundant — any real capability will show up in the cache within one reconnect-backoff window. Restoring the fallback would require a new architecture session.

---

## RBAC at Each Hop (Call Chains)

When extension A calls `cap.call("B:1:func")`, which calls `cap.call("C:1:func")`:

**RBAC is checked at every hop using the originating user's context.**

- Hop 1: A calls B. RBAC checks whether the originating user has the rule for `B:1:func`.
- Hop 2: B calls C. RBAC checks whether the **same originating user** has the rule for `C:1:func`.

The `UserContext` is propagated through the entire call chain. Extensions cannot escalate privileges — they execute with the permissions of the user who initiated the request.

This means:
- An extension cannot call a capability the user doesn't have permission for, even if the extension itself "needs" it.
- There is no service-account or extension-level permission model. All calls are on behalf of a user.
- If a user has rules for A and B but not C, the chain A → B → C will fail at the C hop with `permission_denied`.

### Rationale

This follows the least-privilege, additive union model from DESIGN-rbac-philosophy.md. Allowing extensions to escalate beyond user permissions would create a privilege escalation vector — a malicious extension could use an innocent intermediary to access capabilities the user never granted.

---

## kernel.admin Behavior

Per DESIGN-rbac-philosophy.md and ICD-0.1.5:

- The `kernel.admin` rule is the highest-privilege rule, granted to the `admin` group by default.
- A user with `kernel.admin` in their effective rules passes **all** capability RBAC checks.
- This is implemented in the same `userHasRule()` check from ICD-0.1.5 — `kernel.admin` is treated as a universal match.
- `kernel.admin` is **not** an absolute bypass outside the rule system. It flows through the same check algorithm as every other rule.

---

## Audit Events

All capability RBAC denials are audited via `log::audit()`:

| Action | Detail | Trigger |
|--------|--------|---------|
| `capability.rbac.denied` | `{user_id, username, capability, rule, call_depth}` | User lacks the required rule for a capability call |

Permission grants are **not** audited (consistent with ICD-0.1.5 — only denials are logged).

The `call_depth` field in the audit event helps administrators trace which hop in a call chain was denied.

---

## JS Error Contract

When `cap.call()` is denied in JavaScript, the rejected promise contains:

```javascript
try {
    await cap.call("terminal:1:shell", "ls");
} catch (e) {
    // e.error === "permission_denied"
    // e.capability === "terminal:1:shell"
    // e.rule === "terminal.shell.execute"
    // e.message === "You do not have permission to call terminal:1:shell. Required rule: terminal.shell.execute"
}
```

The same shape applies for `cap.batch()` — the batch rejects with the first denial encountered.

---

## Security Constraints (Non-Negotiable)

1. **Every capability call checks RBAC.** There is no bypass path, not even for kernel capabilities.
2. **Fail closed.** If the rule lookup fails (capability not in cache, database error), the call is denied.
3. **Per-hop checking.** Every hop in a capability chain checks RBAC against the originating user's permissions.
4. **No privilege escalation.** Extensions execute with user permissions only. There is no service-account or extension-level permission override.
5. **Non-version-aware mapping.** Rule mapping never considers the capability version number.
6. **All denials audited.** Every RBAC denial produces an audit event with full context.
7. Conforms to the least-privilege, additive union model in DESIGN-rbac-philosophy.md.

---

## What Must Not Be Decided Yet

- Dynamic or extension-provided rule evaluation logic (e.g. ownership checks: "user can edit their own items").
- Per-call resource budgets or execution cost tracking.
- Extension-level permissions (allowing extensions to call capabilities the user doesn't have access to).
- Rule wildcards or hierarchical matching (`terminal.*` granting all terminal rules).
- Capability-level scoping of PATs (limiting a PAT to specific capabilities rather than inheriting all user permissions).
- Any deviation from the per-hop, user-context-only checking model.

---

## Milestone Criteria

**Entry:** ICD-0.2.2 (Tier 1 + Tier 2 resolution) implemented and merged; RBAC enforcement middleware (ICD-0.1.5) working; capabilities registered with `rbac_rule` column populated.

**Exit:**
- `checkCapabilityPermission()` integrated into the dispatch pipeline at step 3.
- Rule lookup works from Tier 1 map, Tier 2 cache, and database fallback.
- Permission check uses the existing ICD-0.1.5 algorithm (additive union, `kernel.admin` as universal match).
- Denial produces correct error shape with capability and rule fields.
- Per-hop RBAC checking verified: A → B → C chain with insufficient permissions at C returns `permission_denied`.
- `kernel.admin` user passes all capability RBAC checks.
- All denials audited with `capability.rbac.denied` event including call depth.
- JS error contract matches the specified shape.
- Catch2 tests cover: allowed call, denied call, kernel.admin bypass, multi-hop chain, denial at depth > 0, disabled capability, missing rule (fail closed).
- No structural decisions conflict with DESIGN-rbac-philosophy.md.
- Human approval of implementation plan obtained before work begins.
- CI green; all tests pass.

---

**This document is the permanent authority on capability RBAC integration.** Any code session implementing 0.2.4 or touching capability authorization afterward **must** read this ICD, ICD-0.2.0, ICD-0.2.2, ICD-0.1.5, DESIGN-capability-registry.md, and DESIGN-rbac-philosophy.md before beginning work. Changes to this contract require a new architecture session.
