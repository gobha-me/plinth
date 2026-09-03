# ICD-0.1.3-pats

**Traces to:** architecture/01-identity.md §1 (Identity and Authentication), architecture/01-identity.md §2 (Groups and RBAC), architecture/04-services-ha.md §1 (Audit Logging), architecture/02-capabilities.md §2 (Kernel Standard Library)  
**Depends on:** ICD-0.1.2-auth-sessions (shared authentication middleware)  
**Milestone:** 0.1.3 — Auth: PATs (create, revoke, list, authenticate)  
**Status:** Ready for implementation (post-review v2)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Related:** DESIGN-logging-subsystem.md

---

## Overview

Personal Access Tokens (PATs) provide long-lived programmatic access for scripts, CI/CD, API clients, and integrations. They are distinct from sessions:

- PATs do **not** create rows in `plinth.sessions`.
- PATs are bearer-only (`Authorization: Bearer plinth_...`).
- PATs do not expire by default (`expires_at = NULL`).
- PATs are tied 1:1 to a user and inherit that user’s permissions (RBAC enforcement added in 0.1.5).

PATs are accepted by the same authentication middleware defined in ICD-0.1.2. The middleware first checks for a session cookie, then for a Bearer token, then branches on the `plinth_` prefix to select the PAT validation path.

All audit events must be emitted via the canonical logging path defined in DESIGN-logging-subsystem.md (`log::audit()` in C++, `audit.log()` in JS).

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

### `plinth.pats` table

| Column        | Type        | Constraints                          | Notes |
|---------------|-------------|--------------------------------------|-------|
| `id`          | UUID        | PK, default `gen_random_uuid()`      | |
| `user_id`     | UUID        | FK → `plinth.users.id`, NOT NULL     | |
| `name`        | TEXT        | NOT NULL, 1-128 chars                | Human-readable label |
| `token_hash`  | TEXT        | NOT NULL                             | SHA-256 (hex) of raw token *without* prefix |
| `token_prefix`| TEXT        | NOT NULL, exactly 8 chars            | First 8 chars of random portion (after `plinth_`) for safe display |
| `created_at`  | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| `expires_at`  | TIMESTAMPTZ | NULL                                 | NULL = never expires |
| `last_used_at`| TIMESTAMPTZ | NULL                                 | Updated on successful auth (see performance note below) |
| `revoked_at`  | TIMESTAMPTZ | NULL                                 | If set, PAT is invalid |

**Token format:** `plinth_` + 256-bit CSPRNG random bytes base64url-encoded (total 50 chars). The full token is: `plinth_` (7 chars) + base64url(32 bytes) (43 chars) = 50 chars exactly.
**Hashing:** SHA-256 of the portion after the `plinth_` prefix (i.e., `token[7..]`), stored as hex (64 chars).
**Prefix:** First 8 characters of the random portion (i.e., `token[7..15]`) stored in plaintext for safe display.

**Performance note on `last_used_at`:** Updates must not create contention on the hot auth path. The exact implementation (direct UPDATE vs. debounced/background write) will be decided when the realtime coalescer (0.5.1) is available; the chosen approach must remain compatible with that design.

---

## Endpoints

### POST /api/auth/pats

**Authentication:** Required (session token only — PATs may not create other PATs in 0.1.3).

**Request**
```json
{
  "name": "CI deployment token",
  "expires_at": "2027-04-14T12:00:00Z"
}
```

**Response (201)**
```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "name": "CI deployment token",
  "token": "plinth_aBcDeFgH1234567890abcdef...",
  "expires_at": "2027-04-14T12:00:00Z",
  "created_at": "2026-04-14T12:00:00Z"
}
```

The raw `token` is returned **exactly once**. It is never stored server-side and never returned again.

**Error codes:** `missing_name`, `name_too_long`, `invalid_expiry`, `not_authenticated`

**Side effects:** Row inserted into `plinth.pats`; audit event `pat.created` via `log::audit()`.

### GET /api/auth/pats

**Authentication:** Required (session or PAT).

**Response (200)**
```json
{
  "pats": [
    {
      "id": "...",
      "name": "CI deployment token",
      "token_prefix": "aBcDeFgH",
      "created_at": "...",
      "expires_at": "...",
      "last_used_at": "...",
      "revoked_at": null
    }
  ]
}
```

Full token never returned. Only `token_prefix` is shown.

**Error codes:** `not_authenticated`

### DELETE /api/auth/pats/{id}

**Authentication:** Required (session or PAT). Users may revoke their own PATs; admins may revoke any.

**Response (200):** `{ "status": "pat_revoked" }`

**Error codes:** `not_authenticated`, `forbidden`, `pat_not_found`

**Side effects:** `revoked_at` set; audit event `pat.revoked` via `log::audit()`.

---

## Authentication Mechanism (Middleware Contract)

The shared authentication middleware (ICD-0.1.2) implements the following ordered logic:

1. Check for `Cookie: plinth_session=…` → session validation path.
2. Check for `Authorization: Bearer …`:
   - If token starts with `plinth_` → PAT validation path (skip session table).
   - Otherwise → session validation path.
3. For PATs: verify prefix, hash remainder, query `plinth.pats` for valid non-revoked/non-expired row.
4. On success: update `last_used_at`, attach `user_id`, `username`, and `auth_type: "pat"` (or `"session"`) to request context.
5. On failure: return 401 with appropriate error code.

This middleware runs **before** any RBAC middleware (0.1.5). It only establishes identity.

---

## Security Constraints (Non-Negotiable)

1. 256-bit CSPRNG entropy for all tokens.
2. Only hash (never raw token) is stored.
3. Raw token returned exactly once at creation; never logged.
4. PATs are bearer-only; never delivered via `Set-Cookie`.
5. Revocation and expiry checks are performed on every request.
6. PATs inherit the full permissions of their owning user. No per-PAT scopes in this milestone.
7. `last_used_at` updates must not block the auth hot path.

---

## What Must Not Be Decided Yet

- Any form of per-PAT scoping or RBAC rules (deferred to 0.1.5 and the RBAC rule registration system).
- Integration of PATs with the capability registry (0.2.x) or extension-provided authentication methods.
- Token rotation (create-new-and-revoke-old in one atomic operation).
- Different rate limits or audit behavior for PATs vs sessions.
- Any change to the `plinth_` prefix, token format, or middleware branching logic.
- Storage of PAT metadata beyond what is defined in the table above.

All future extensions to PAT behavior must go through an architecture session and produce an updated ICD.

---

## Milestone Criteria

**Entry:** ICD-0.1.2-auth-sessions implemented, tested, and merged; `plinth.pats` table exists in `migrations/schema.sql`.

**Exit:**
- All three endpoints implemented and covered by Catch2 tests (including error paths and the “token returned once” rule).
- Shared middleware correctly branches between session and PAT paths (tested with both cookie and Bearer).
- Audit events emitted exclusively via the DESIGN-logging-subsystem.md path.
- First-user/admin PAT creation tested.
- No raw tokens or passwords appear in logs.
- Human approval of implementation plan and code diff obtained before merge.
- CI green; tests pass in both dev_mode and migration modes.

---

## Open Questions (Deferred to Later Architecture Sessions)

- Granular PAT scopes (read-only, specific capabilities, etc.).
- Automatic PAT rotation support.
- Distinct rate limiting or throttling for PAT-authenticated traffic.
- UI for PAT management in the admin panel (0.6.x).

---

**This document is the permanent authority on PAT behavior.** Any code session implementing 0.1.3 or touching authentication after this milestone **must** read both this ICD and ICD-0.1.2-auth-sessions before beginning work. Changes to these contracts require a new architecture session.

**Approved for implementation upon human review.**