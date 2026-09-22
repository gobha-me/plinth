# ICD-auth-sessions

**Traces to:** architecture/01-identity.md §1 (Identity and Authentication), architecture/01-identity.md §2 (Groups and RBAC), architecture/04-services-ha.md §1 (Audit Logging)  
**Milestone:** 0.1.2 — Auth: users table, argon2id, session create/validate/destroy  
**Status:** Ready for implementation (post-review v2)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Current contract amendment (2026-09-16):** Shared authentication middleware
must distinguish invalid, expired, or revoked credentials from an unavailable
authentication database. The former retain their existing `401` bodies; a
database failure returns `503` with exactly
`{"error":"service_unavailable","message":"Authentication service is temporarily unavailable"}`.
This generic result applies to every protected route. Issue #31 implements the
currently missing outcome and regression coverage; it does not change token,
cookie, expiry, or revocation semantics.

**Current contract amendment (2026-09-21):** Issue #37 separates the
secret-authorized first-administrator bootstrap from later local registration.
Registration has `disabled`, `invite`, and `open` modes, generic processed
responses, digest-only one-use invites, and bounded source, submitted-subject-digest,
global, and total-account admission. Plinth intentionally has no persistent
automatic account lock, because it would let an attacker lock a known user;
administrator recovery replaces a password and revokes credentials without
clearing an intentional account disable. Local identity continues to collect
only username and password hash—never email or real name.

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

### POST /api/auth/bootstrap

**Authentication:** The process must have received `PLINTH_BOOTSTRAP_TOKEN`,
containing a generated 32–256-byte secret, and the JSON `bootstrap_token` must
match it. Exact-origin filtering still
applies to browser-shaped requests. This secret is bootstrap authority, not a
session or reusable API credential.

**Request:** `bootstrap_token`, `username`, and `password`, with no additional
identity fields.

**Response:** `201` with the user representation after user creation and admin
membership commit atomically. Missing, wrong, or unconfigured authority returns
`403 bootstrap_denied`. Once any non-test user exists, an attempt with valid
configured bootstrap authority returns `409 bootstrap_closed`; after the secret
is removed, attempts return `403 bootstrap_denied`.

The secret is sourced only from the environment. It is never persisted, logged,
returned, exposed through `config.get`, or accepted by `/api/auth/register`.

### GET /api/auth/registration

**Authentication:** None. Returns only `{ "mode": "disabled|invite|open" }`.

### POST /api/auth/register

**Authentication:** None (public exact-origin contract).

**Request**
```json
{
  "username": "alice",
  "password": "correct-horse-battery-staple",
  "invite_token": "optional-in-invite-mode"
}
```

`invite_token` is required for admission in `invite` mode and ignored in
`open` mode; unknown fields—including `email` and `real_name`—are rejected.
Plinth does not collect them.

**Response (202)**
```json
{"status":"processed"}
```

Every syntactically valid invite/open request returns this response whether an
account was created or rejected because of username state, invite state, or the
total-account ceiling. Disabled mode returns `403 registration_unavailable`;
an exhausted attempt window returns `429 rate_limited`. Syntax and field errors
return `400`, but never disclose stored account or invite state.

**Side effects:**
- A permitted request inserts a non-admin row into `plinth.users`.
- Invite mode consumes exactly one digest-only invite in the same transaction.
- Successful creation emits `user.registered` with the created user id and
  mode, without passwords, raw tokens, or the submitted username. Invite and
  recovery operator mutations are audited against the administrator context.

### Administrator registration controls

- `POST /api/auth/invites` accepts optional `ttl_seconds` and returns
  `201 {id, token, expires_at}`. The raw 43-character base64url token is returned
  once; only its SHA-256 digest is stored.
- `GET /api/auth/invites` returns `id`, `created_at`, `expires_at`, `revoked_at`,
  and `used_at`, never the token or digest.
- `DELETE /api/auth/invites/{id}` returns `200 {"status":"revoked"}`.
- `POST /api/auth/recovery` accepts `username` and `new_password`, returning
  `200 {"status":"recovered"}` after replacing the hash and revoking every
  session and PAT. It does not clear `disabled_at`; an unknown user may return
  `404` because this route is administrator-only.

All four administrator operations require authenticated `kernel.admin`;
mutating cookie calls apply CSRF after session authentication and before RBAC.

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

**Error codes:** `missing_username`, `missing_password`, `invalid_credentials`, `rate_limited`

**Side effects:** Session row created, audit `user.login` (success) or
`user.login_failed`, and bounded pre-Argon2 source/subject attempt admission.

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
2. Login and registration use bounded source controls. Registration additionally
   uses submitted-subject-digest and global attempt windows plus `max_accounts`; all
   admission checks run before Argon2. Limits cannot be configured as unlimited.
3. Tokens: 256-bit CSPRNG entropy. Never log raw tokens or passwords.
4. Cookie flags: `HttpOnly`, `Secure` (except localhost dev_mode), `SameSite=Strict`.
5. Only the secret-authorized bootstrap route creates the first administrator.
   Ordinary registration never grants admin membership, including on an empty
   database.
6. `disabled_at` prevents both login and re-registration with the same username.
7. Unknown, wrong-password, and disabled-account login attempts share
   `invalid_credentials`; the login endpoint never discloses disabled state.
   There is no subject-wide automatic lock.
8. Changing registration mode never invalidates an existing user or credential.

---

## What Must Not Be Decided Yet

- Integration of sessions with the capability registry (0.2.x). These remain permanent kernel HTTP routes.
- RBAC permission checks on auth endpoints (deferred to 0.1.5).
- Any extension-provided authentication (OAuth, etc.) — explicitly forbidden per ARCHITECTURE §3.1.
- Sliding expiry, “remember me” long-lived tokens, or end-user/email-based
  password reset flows. Administrator credential recovery is the only reset.
- Any change to token format, hashing, or cookie name.

---

## Milestone Criteria

**Entry:** 0.1.1 (PG connection, plinth schema, dev_mode bootstrap) complete.

**Exit:**
- All listed endpoints implemented and exercised in Catch2 tests.
- All error codes returned with correct standardized shape.
- Secret-authorized first-user bootstrap verified (auto-added to admin group),
  with the ordinary registration route unable to bootstrap.
- Disabled, invite, and open registration modes, one-time invite races, generic
  enumeration-resistant results, pre-Argon2 bounds, and the account ceiling are
  exercised through production HTTP handlers.
- Disabling registration is verified not to invalidate login, sessions, PATs,
  or WebSocket credentials.
- Session validation middleware passes tests for both cookie and Bearer paths.
- No passwords or raw tokens ever appear in logs or audit entries.
- CI green, tests pass with both dev_mode and migration paths.
- Human review of implementation plan and diff completed before merge.

---

## Open Questions (Deferred)

- Session sliding expiry (consider after 0.7).
- Long-lived “remember me” tokens (new token type, post-1.0).
- End-user password reset and recovery-factor enrollment (post-1.0; would
  require a separately approved privacy and authority contract).
