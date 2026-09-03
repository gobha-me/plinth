# ICD-auth-sessions

**Traces to:** architecture/01-identity.md §1 (Identity and Authentication), architecture/01-identity.md §2 (Groups and RBAC), architecture/04-services-ha.md §1 (Audit Logging)  
**Milestone:** 0.1.2 — Auth: users table, argon2id, session create/validate/destroy  
**Status:** Ready for implementation (post-review v2)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

---

## Overview

This ICD defines the complete authentication surface for local user accounts in the Plinth kernel. It covers user registration, login, session creation, validation, revocation, and listing.

All subsequent endpoints and capabilities in the system depend on this contract. Later milestones (RBAC, capability registry, extensions) **must not** alter the core session token format, cookie semantics, or database schema defined here without an architecture session and updated ICD.

**Transport:** 
- HTTP-only cookies for browser/web clients (`plinth_session`)
- Bearer tokens for API/programmatic access

Both mechanisms use the identical session token semantics. Cookie takes precedence if both are present.

**Password hashing:** argon2id (memory-hard, GPU-resistant) with fixed parameters below.

---

## Data Model

### `plinth.users` table

| Column       | Type        | Constraints                          | Notes |
|--------------|-------------|--------------------------------------|-------|
| `id`         | UUID        | PK, default `gen_random_uuid()`      | |
| `username`   | TEXT        | UNIQUE, NOT NULL, 3-64 chars, `[a-z0-9_-]` | Lowercase enforced at insert |
| `password_hash` | TEXT     | NOT NULL                             | argon2id encoded string |
| `created_at` | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| `disabled_at`| TIMESTAMPTZ | NULL                                 | If set, user cannot authenticate or register with same name |

**Argon2id parameters (fixed for 0.1.x):** memory=64 MiB, iterations=4, parallelism=4. These may become configurable after 0.7 only via architecture session.

### `plinth.sessions` table

| Column       | Type        | Constraints                          | Notes |
|--------------|-------------|--------------------------------------|-------|
| `id`         | UUID        | PK, default `gen_random_uuid()`      | |
| `user_id`    | UUID        | FK → `plinth.users.id`, NOT NULL     | |
| `token_hash` | TEXT        | NOT NULL                             | SHA-256 of raw token (hex encoded) |
| `user_agent` | TEXT        | NULL                                 | From `User-Agent` header |
| `ip_address` | INET        | NULL                                 | From request |
| `created_at` | TIMESTAMPTZ | NOT NULL, default `NOW()`            | |
| `expires_at` | TIMESTAMPTZ | NOT NULL, default `NOW() + 24 hours` | Fixed lifetime (no sliding in 0.1.x) |
| `revoked_at` | TIMESTAMPTZ | NULL                                 | If set, session is invalid |

**Token format:** 256-bit random bytes from CSPRNG, base64url-encoded (43 characters). Raw token is returned **exactly once** at creation. Server stores only the SHA-256 hash (hex).

**Session lifetime:** 24 hours fixed. Re-authentication always creates a new session.

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

## Endpoints

### POST /api/auth/register

**Authentication:** None (public). Can be disabled via config after first user.

**Request**
```json
{
  "username": "alice",
  "password": "correct-horse-battery-staple"
}
```

**Response (201)**
```json
{
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "username": "alice",
  "created_at": "2026-04-14T12:00:00Z"
}
```

**Error codes:** `username_too_short`, `username_invalid_chars`, `password_too_short`, `username_taken`, `registration_disabled`

**Side effects:**
- Row inserted into `plinth.users`
- First user created is added to the `admin` group (via direct table insert compatible with 0.1.4 RBAC design — no other group logic permitted)
- Audit entry: `user.registered`

### POST /api/auth/login

**Authentication:** None.

**Request:** Same as register.

**Response (200)**
```json
{
  "user": { "id": "...", "username": "alice" },
  "session": { "id": "...", "expires_at": "..." }
}
```

**Set-Cookie:** `plinth_session=<raw_token>; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=86400`

**Error codes:** `missing_username`, `missing_password`, `invalid_credentials`, `account_disabled`, `rate_limited`

**Side effects:** Session row created, audit `user.login` (success or failure), rate limit (5 failed attempts/IP/minute).

### POST /api/auth/logout

**Authentication:** Required.

**Response (200):** `{ "status": "logged_out" }`

**Set-Cookie:** clear cookie (`Max-Age=0`)

**Error codes:** `not_authenticated`

**Side effects:** `revoked_at` set, audit `user.logout`

### GET /api/auth/session

**Authentication:** Required.

**Response (200):** Current user + session details.

**Error codes:** `not_authenticated`, `session_expired`, `session_revoked`

### DELETE /api/auth/session/{id}

**Authentication:** Required. User can revoke own sessions; admin can revoke any.

**Error codes:** `not_authenticated`, `forbidden`, `session_not_found`

**Side effects:** `revoked_at` set, audit `session.revoked`

### GET /api/auth/sessions

**Authentication:** Required.

**Response (200):**
```json
{
  "sessions": [
    {
      "id": "...",
      "user_agent": "...",
      "ip_address": "...",
      "created_at": "...",
      "expires_at": "...",
      "is_current": true
    }
  ]
}
```

`is_current` is true when the session’s `token_hash` matches the incoming request token.

---

## Authentication Mechanism & Middleware Contract

1. Extract raw token from `Cookie: plinth_session=...` or `Authorization: Bearer ...`
2. Compute SHA-256 (hex) of raw token
3. Query `plinth.sessions` for valid, non-revoked, non-expired match
4. On success: attach `user_id`, `session_id`, `username` to Drogon request context
5. On failure: return appropriate 401 with error code above

**Middleware ordering:** This session middleware **must run before** the RBAC middleware (0.1.5). It only establishes identity; it does not perform permission checks.

---

## Security Constraints (Non-Negotiable)

1. Argon2id verification **must** be constant-time. Always run a dummy hash when username is not found.
2. Login rate limiting: 5 failed attempts per IP per minute → 429 `rate_limited` with `retry_after`.
3. Tokens: 256-bit CSPRNG entropy. Never log raw tokens or passwords.
4. Cookie flags: `HttpOnly`, `Secure` (except localhost dev_mode), `SameSite=Strict`.
5. First-user bootstrap becomes `admin` group member. Registration is disabled after first user unless config re-enables it.
6. `disabled_at` prevents both login and re-registration with the same username.

---

## What Must Not Be Decided Yet

- Integration of sessions with the capability registry (0.2.x). These remain permanent kernel HTTP routes.
- RBAC permission checks on auth endpoints (deferred to 0.1.5).
- Any extension-provided authentication (OAuth, etc.) — explicitly forbidden per ARCHITECTURE §3.1.
- Sliding expiry, “remember me” long-lived tokens, or password reset flows.
- Any change to token format, hashing, or cookie name.

---

## Milestone Criteria

**Entry:** 0.1.1 (PG connection, plinth schema, dev_mode bootstrap) complete.

**Exit:**
- All listed endpoints implemented and exercised in Catch2 tests.
- All error codes returned with correct standardized shape.
- First-user bootstrap verified (auto-added to admin group).
- Session validation middleware passes tests for both cookie and Bearer paths.
- No passwords or raw tokens ever appear in logs or audit entries.
- CI green, tests pass with both dev_mode and migration paths.
- Human review of implementation plan and diff completed before merge.

---

## Open Questions (Deferred)

- Session sliding expiry (consider after 0.7).
- Long-lived “remember me” tokens (new token type, post-1.0).
- Password reset (admin-only or extension-owned).