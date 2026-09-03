# DESIGN-admin — The Admin Extension

**Status:** Stub. Expanded when the 0.6.x arc reaches admin milestones.
**Scale:** 2 (second built-in extension; bundled; reference consumer of shell SDK).
**Traces to:** `architecture/05-extensions.md §1.4` (bundled packages, first-boot install), `DESIGN-shell-v06x.md §9` (scope extraction), `DESIGN-packages-v04x.md` (install lifecycle).
**Depends on:** `DESIGN-shell-v06x.md §4` (panel SDK), `architecture/01-identity.md §2` (RBAC rules), `architecture/02-capabilities.md` (kernel admin capabilities it consumes).

---

## 1. Scope

The admin extension is a **second built-in extension**, bundled with
the kernel binary and installed on first boot alongside the shell
through the standard 0.4 package lifecycle. It is the reference
consumer of the shell's panel SDK and the reference test case for the
"everything is an extension" architecture — if admin can't be built
from shell-SDK-only contracts, the shell SDK is wrong.

### 1.1 Package Identity

| Field | Value |
|-------|-------|
| Package name | `admin` |
| PG schema | `ext_admin` |
| Provenance | `bundled` (installed by kernel bootstrap when missing) |
| Shell SDK requirement | `shell >= 0.6.3` (panel SDK lands in 0.6.3) |
| Primary RBAC gate | `kernel.admin` (most panels) |
| Frontend mount | none (consumes shell's `/app`; registers panels only) |

### 1.2 What Admin Owns

- Groups management panel (create, edit, delete groups; assign users).
- Package management panel (install, upgrade, disable, uninstall).
- RBAC administration panels (rule grants per group, effective-rule
  inspection, policy explanation).
- Sidecar management panel (list registered sidecars, health,
  circuit-breaker state).
- Audit log browser (query, filter, export).

### 1.3 What Admin Does Not Own

- Authentication flows (login, logout, PAT issuance). The shell owns
  these as part of the frame.
- User profile / preferences. Owned by shell (`ext_shell`).
- System health / metrics dashboards beyond the sidecar panel.
  Deferred — a separate observability extension may cover this.
- Extension-specific admin UI. Each extension provides its own admin
  panels if needed; admin doesn't reach into extension schemas.

---

## 2. Structure

Admin is a normal package. Authoritative structure lives in
`architecture/05-extensions.md §1`:

- `manifest.json` — identity; declares no `frontend.mount`.
- `capabilities.json` — `provides` (admin-specific capabilities, if
  any) and `requires` (kernel admin capabilities, shell SDK
  capabilities).
- `rbac.json` — declares rules admin consumes (`kernel.admin`,
  `packages.manage`, `groups.manage`, `rbac.manage`, `audit.read`).
- `panels.json` — primary panels (one per top-level admin area), with
  sub-tab declarations.
- `migrations/` — `ext_admin` schema: admin-specific state (saved
  filters, UI preferences for admin views).

---

## 3. Milestone Plan (Placeholders)

Version numbers to be assigned by the architect when the admin arc is
scheduled on the roadmap. The progression tracks the shell arc (each
admin milestone depends on the shell SDK feature it uses).

### Admin-A — Package Management Panel

- Install from URL / upload, list installed packages, disable/enable,
  uninstall.
- Version history, upgrade path UI.
- **Entry:** Package system (0.4.x) feature-complete; shell 0.6.3
  (panel SDK).

### Admin-B — Groups Management Panel

- CRUD on groups, membership management, search.
- Visual indication of `everyone`, `authenticated`, `admin`
  system groups.
- **Entry:** Groups (0.1.4) complete; Admin-A landed (panel SDK
  conventions established).

### Admin-C — RBAC Administration

- Rule browser: every rule provided by every installed extension.
- Grant matrix: groups × rules.
- Effective-rule inspector: given a user, show the full set of
  granted rules and their provenance.
- Policy explanation: given a capability call, explain allow/deny.
- **Entry:** RBAC enforcement (0.1.5) complete; capability registry
  (0.2.x) complete.

### Admin-D — Sidecar Management

- List registered sidecars, health, circuit-breaker state.
- Manual re-register / deregister.
- **Entry:** Sidecar contract (`architecture/04-services-ha.md §5`)
  implemented.

### Admin-E — Audit Log Browser

- Query, filter (by user, capability, time range, outcome), export.
- Realtime tail mode.
- **Entry:** Audit (0.1.7) complete.

---

## 4. Open Questions

- Which admin capabilities should be kernel-provided vs. admin-extension
  provided? (Kernel already exposes group/package/RBAC admin
  capabilities; the admin extension is mostly a UI consumer. Confirm
  no admin-specific kernel capabilities are needed.)
- Should audit-log export be a capability the admin extension calls,
  or a kernel endpoint with an RBAC gate? (Leaning: kernel endpoint,
  gated on `audit.read`.)
- Does admin need a "dangerous operations" confirmation pattern (type
  package name to confirm uninstall, etc.), or is that a shell SDK
  primitive every extension should get?

---

## 5. What This Stub Does Not Decide

- Panel-by-panel UX (beyond what's implied by scope).
- Bulk-operation patterns (bulk-grant, bulk-uninstall).
- Audit retention / pruning policy — owned by
  `architecture/04-services-ha.md §1`.
- Delegated admin (sub-admin roles). Out of scope until post-1.0.
- i18n of admin UI. Tracked globally, not here.

---

## Appendix A: Why Admin is a Separate Bundled Package

Two equally valid options were considered:

1. Admin lives inside the shell package (shell and admin ship
   together, one bundled blob).
2. Admin is a separate bundled package, installed alongside the
   shell on first boot.

Option 2 was chosen because:

- **Dogfooding.** A second bundled package exercises the "bundled
  package" install path beyond a single case. If the kernel's
  first-boot logic only ever installs one package, the logic is
  under-tested.
- **Upgrade independence.** Admin UI iterates faster than the shell
  frame. A new admin version can ship without shipping a new shell.
- **Permission gate.** Admin panels are `kernel.admin`-gated; shell
  panels are not. Keeping them in the same package would complicate
  the RBAC story (shell's panels can't be in a package where the
  default grant includes `kernel.admin`).
- **Removability.** An operator who wants to replace admin with a
  custom admin extension uninstalls the bundled admin and installs
  their own. This is harder if admin is welded to the shell.

The tradeoff is a second manifest / migration set / CI target. That
cost is small and bounded.
