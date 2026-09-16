# Architecture 01 — Identity, Authentication, and RBAC

**Owner:** this document. Authoritative for identity, session management,
group membership, RBAC rule storage and lifecycle, anonymous identity,
and the user-deletion cleanup contract.

**Depends on:** `DESIGN-rbac-philosophy.md` (the philosophical authority
on permission semantics; this document provides the kernel contract on
top of that philosophy).

**Related:**
- `architecture/02-capabilities.md` (every capability call is RBAC-gated;
  the mapping from capability to rule is defined there.)
- `architecture/03-data.md` (realtime event bus used for the `users.deleted`
  event.)
- `architecture/04-services-ha.md §1` (audit logging; all permission
  denials audit.)
- `architecture/websocket-authority.md` (database-backed WebSocket
  credential and effective-rule lease).
- ICD-0.1.2, ICD-0.1.3 (sessions, PATs)
- ICD-0.1.4, ICD-0.1.5 (groups, RBAC storage and enforcement)

---

## 1. Identity and Authentication

- Local user accounts (username + argon2id password hash)
- Session tokens (HTTP-only cookies for web, bearer tokens for API)
- Personal Access Tokens (PATs) for programmatic access
- Bootstrap tokens for sidecar registration (planned by
  [#63](https://github.com/gobha-me/plinth/issues/63))

**No OAuth provider built into kernel.** OAuth can be an extension.

**K8s JWT validation (conditional, not implemented).** A future design may
validate service account JWTs for sidecar auto-registration. Sidecars
in the same namespace present their JWT instead of a bootstrap token.
Trust-by-proximity: "you're in my namespace, I trust you to register."
This is a kernel capability (trust boundary), not a package.

See `ICD-0.1.2-auth-sessions.md` and `ICD-0.1.3-pats.md` for full
contracts.

First-user creation and admin membership commit in one advisory-lock-
serialized transaction. Concurrent bootstrap attempts cannot create multiple
first administrators, and success is not published before commit
acknowledgment. Session and PAT validation both fail closed for disabled users.
WebSocket credentials and rule snapshots use the bounded renewal contract in
`architecture/websocket-authority.md` rather than connection-lifetime caching.
The broader local-registration policy remains owned by
[#37](https://github.com/gobha-me/plinth/issues/37).

### 1.1 User Record (summary)

The kernel owns one identity table, `plinth.users`, storing the base
identity record: user id (UUID), username, password hash (argon2id),
timestamps. Extensions that attach per-user data do so in their own schemas
using the user id as a reference. The restricted extension role may declare an
extension-owned foreign key to `plinth.users(id)`; this is optional, and
extensions without such a constraint need the planned cleanup contract in §4.

---

## 2. Groups and RBAC

**See `DESIGN-rbac-philosophy.md` for the authoritative philosophy.**

**No roles. No teams. Groups only.**

Two default groups:

- `admin` — granted the `kernel.admin` rule by default (full control)
- `everyone` — no permissions by default

Every identity is a virtual member of `everyone`; this implicit membership is
not represented by a `plinth.group_members` row. Authenticated users may also
belong to one or more explicit groups. Permissions are **additive**: effective
rules are the union of non-orphaned rules granted to `everyone` and rules from
all explicit memberships. A user may belong to both `backup-operators` and
`package-managers` and receive the combined permissions of both.

All effective-rule loaders must apply that same union, including HTTP route
and capability authorization, WebSocket authority snapshots, and application
discovery. The current loaders still derive authenticated authority only from
stored `group_members`; [#31](https://github.com/gobha-me/plinth/issues/31)
owns correction and regression coverage before the launcher ships. Code must
not compensate by materializing an `everyone` row per user.

### 2.1 RBAC Rules

Rules are **registered by extensions** (via `rbac.json` during package
install) or by the kernel itself. The kernel provides the rule storage,
enforcement mechanism, and audit trail.

A rule registration (in `rbac.json` or kernel bootstrap):

```json
{
  "rules": [
    {
      "rule": "terminal.shell.execute",
      "namespace": "terminal",
      "description": "Execute shell commands via the terminal extension",
      "test": {
        "assert_deny": {
          "call": "terminal:1:shell('echo test')",
          "expect": "permission_denied"
        },
        "assert_allow": {
          "call": "terminal:1:shell('echo test')",
          "expect": "success"
        }
      }
    }
  ]
}
```

Kernel-level rules (e.g. `kernel.admin`, `users.manage`, `packages.install`,
`system.backup.run`, `rbac.rules.grant`) are registered by the kernel
during bootstrap.

**Rules are NOT version-aware.** `terminal.shell.execute` grants access
to `terminal:1:shell`, `terminal:2:shell`, etc. Versioning is an API
contract concern handled by the capability registry
(see `architecture/02-capabilities.md`).

### 2.2 RBAC Test Validation — Two Phase

RBAC tests are NOT executed at install time. Running arbitrary
capability calls during installation is unsafe and circular (the
handler may not be registered yet, and `assert_allow` would actually
execute the command).

**Rule validator — install time: schema validation only.**

- Kernel validates every rule in `rbac.json` has a matching namespace
  in `capabilities.json`.
- Kernel validates test contract call signatures match declared
  capabilities (namespace, version, function name).
- Kernel validates rule names follow naming conventions.
- No actual execution. No handler invocation. Pure structural checks.

**RBAC test — post-install: integration test suite.**

- After the package is installed and its handlers are running, the
  kernel runs the actual RBAC tests.
- Tests use kernel-created ephemeral test users (scoped to the test
  run, cleaned up after).
- `assert_deny` and `assert_allow` execute against the live handlers.
- If the RBAC test fails, the package is **flagged** (not uninstalled)
  and admin is notified: "RBAC tests failed for terminal-core. Package
  is installed but permissions may not be enforced correctly."
- The RBAC test can be re-run on demand:
  `plinth test rbac terminal-core`.

See `ICD-0.1.4-groups-rbac.md` for storage tables and CRUD contracts.
See `ICD-0.1.5-rbac-enforcement.md` for the middleware enforcement
contract.

### 2.3 Admin Group

The `admin` group is bootstrapped with the explicit kernel-owned rules
`kernel.admin`, `packages.install`, and `packages.read`; package manifests may
separately opt extension rules into the admin group's defaults. It is **not**
an enforcement bypass outside the RBAC system. Capability resolution treats
`kernel.admin` as its universal match, while routes declare their required
rules through normal registration. All checks flow through the RBAC paths
defined in `ICD-0.1.5-rbac-enforcement.md` and remain auditable.

### 2.4 Rule Lifecycle

- Extension installs → kernel reads `rbac.json`, performs rule-validator
  validation, registers rules in `plinth.rbac_rules`.
- Extension starts → RBAC integration tests run.
- Admin assigns rules to groups via `plinth.group_rules`.
- Extension disabled → rules marked **orphaned** (`orphaned_at` set).
- Extension uninstalled → rules removed, stripped from all groups.

**Annotation requirement.** Every rule records its `namespace`,
human-readable `description`, and owning `extension_name`.

### 2.5 Admin UX

Group-by-namespace UI with collapse-by-default. Namespace-level toggles,
orphaned rule warnings, and clear visibility into which packages provide
which rules. `DESIGN-admin-v06x.md` is design input for planned administration
extensions #50-#55; no admin package is currently bundled.

---

## 3. Anonymous Identity

`SessionFilter` accepts either the HttpOnly session cookie or a PAT and attaches
an `AuthContext`; `RbacFilter` attaches the route's `RbacContext`. Capability
handlers combine authenticated identity and effective rules into the
`UserContext` passed to resolution.
Login, registration, health, and frontend assets are intentionally public and
do not synthesize authority. `UserContext::anonymous()` is the reserved context
for a future explicitly public RBAC-gated surface:

- User ID: `null` (sentinel, not a real user)
- Groups: `{"everyone"}` only
- Rules: the union of rules granted to `everyone`

By the philosophy in `DESIGN-rbac-philosophy.md`, the `everyone` group
starts with zero rules. Therefore `UserContext::anonymous()` is denied
by every RBAC-gated route and every RBAC-gated capability call.

No current production route injects this anonymous context. Public routes have
their own narrow handlers; authenticated routes fail before RBAC if credentials
are missing or invalid.

**What this reserves.** A future capability such as the deferred share
primitive (see `architecture/05-extensions.md §Deferred`) requires a
well-defined anonymous identity. Defining it before a public surface lands
avoids a retrofit later.

**Enforcement invariant.** Tests assert that
`UserContext::anonymous()` is rejected by every RBAC-gated route until
a rule is explicitly granted to `everyone`. This test is the permanent
safeguard against accidental public exposure of authenticated
endpoints.

---

## 4. User Deletion Cleanup Contract

**Status: planned, not implemented.**
[Issue #97](https://github.com/gobha-me/plinth/issues/97) owns the final
authority, privacy, delivery, reconciliation, and test contract. The shape
below is the original architecture input to that issue, not a current product
claim.

When a user is deleted from `plinth.users`, every extension with
per-user data has orphaned rows (shell preferences, notes authored by
the user, chat history, etc.). Two mechanisms together ensure
extensions learn about and respond to user deletion without kernel
coupling.

### 4.1 Asynchronous Event Notification (happy path)

The kernel emits a `users.deleted` event on the realtime event bus
(`architecture/03-data.md §3`) when a user is deleted.

- **Channel:** `plinth:kernel:users.deleted`
- **Payload:** `{ user_id, deleted_at }`. No username, no PII — the user
  is gone, the kernel does not republish their identity.
- **Emission point:** after `DELETE FROM plinth.users WHERE id = ?`
  succeeds, within the same PG transaction. If the transaction aborts,
  no event is emitted.
- **HA:** the final design must use the protected realtime authority and
  multi-node semantics current when #97 is implemented; raw notification
  payloads are never authoritative.

Extensions subscribe during their runtime initialization. Their
handlers run cleanup asynchronously (`DELETE FROM ext_*.table WHERE
user_id = ?`). The kernel is fire-and-forget — it does not wait for
extensions to acknowledge, and an extension that fails to clean up does
not block the deletion.

**No pre-deletion hooks.** Deliberately not added. The kernel is the
authority on identity; extensions clean up after the fact, not before.
Allowing extensions to veto user deletion would couple identity to
extension availability.

### 4.2 Synchronous Reconciliation Capability (disaster recovery)

Fire-and-forget handles the happy path. For the cases where extensions
miss the event — disabled at the time, sidecar disconnected, handler
had a bug, extension was installed only after the user was already
gone — the kernel provides a kernel capability:

```
kernel:1:users.list() -> { user_ids: UUID[] }
```

Returns the complete set of currently-existing user IDs. Extensions
with per-user data query this at boot time (or on an admin-triggered
basis) and garbage-collect any `ext_*.table` rows whose `user_id` is
not in the returned set.

**RBAC.**

- Subscribing to `plinth:kernel:users.deleted` is unrestricted. It is
  a kernel-emitted channel, and the payload is minimal (user id +
  timestamp).
- Calling `kernel:1:users.list()` requires the rule
  `kernel.users.list`, granted by default to every extension at install
  time. It is a system-integrity capability, not a PII capability — it
  returns opaque UUIDs, not names or emails.

### 4.3 Out of Scope for This Contract

- **Cascading foreign keys.** Extensions control their own schema;
  they may choose to declare `FOREIGN KEY (user_id) REFERENCES
  plinth.users(id) ON DELETE CASCADE` if the PG GRANT policy permits
  it. That is an extension-level choice, not a kernel contract.
- **Pre-deletion hooks.** Not added. See §4.1.
- **Soft-delete.** The kernel deletes hard. Extensions that want
  soft-delete implement it themselves (tombstone rows, `deleted_at`
  columns, etc.).

### 4.4 Delivery owner

The `users.deleted` event and `kernel:1:users.list()` capability remain a
fuzzy long-horizon commitment. #97, rather than a historical release number,
is the executable owner.

---

## Appendix: RBAC Rule Lifecycle

```
Package Install
    │
    ▼
Rule validator: Schema Validation
    │
    ├── Read ALL manifest files
    ├── Cross-file validation pass
    ├── Errors → block install
    └── Warnings → log, show to admin, continue
         │
         ▼
    Register rules in plinth.rbac_rules
    Create extension PG schema
    Run migrations
         │
         ▼
RBAC test: Integration Tests (post-install)
    │
    ├── Create ephemeral test users
    ├── Run assert_deny / assert_allow
    ├── Clean up test users
    │
    ├── PASS → Package fully operational
    └── FAIL → Package flagged, admin notified

Package Disable → Rules marked ORPHANED
Package Uninstall → Rules DELETED, stripped from all groups
```
