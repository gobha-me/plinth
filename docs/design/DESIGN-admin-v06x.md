# DESIGN-admin — Administration Package

**Status:** Admin-A package management (#50) and Admin-B groups management
(#51) source for the separately installed `admin` 0.1.1 package. Source
registration is distinct from candidate and exact-merge verification.
**Traces to:** `architecture/05-extensions.md`, `DESIGN-shell-v06x.md`,
`DESIGN-packages-v04x.md`.

## 1. Delivery and authority

`admin` is an ordinary, separately installed extension package. The local
`plinth_admin_zip` target creates `packages/admin-0.1.1.zip`; an authenticated
operator installs that archive through the existing package API. The kernel
does not install it on first boot, the runtime image does not bundle it, and
installation grants no permission to anyone. This is distinct from the
trusted first-boot **shell** bundle in `architecture/05-extensions.md §1.4`.
The development package version is 0.1.1; this does not publish a release or
change the global product version.

The package declares two ordered primary panels: `packages` (order 0, existing
`packages.js` entry) and `groups` (order 1, `groups.js` entry). Their visibility
rules are `plinth.admin.packages` and `plinth.admin.groups` in the `plinth`
namespace, owned by `admin`. It declares no capabilities and does not redeclare
kernel rules. Operators must separately grant panel visibility and the
applicable existing API rules to intended groups. Every groups route requires
`kernel.admin`; visibility of the groups panel confers no groups API authority.
Install and upgrade create no automatic grants.

The panels use the shipped shell panel SDK and the existing authenticated,
CSRF-protected `/api/packages` and `/api/groups` routes. The server entry exists
to satisfy the normal package shape and does not proxy privileged work. There is no private
admin backend, new HTTP endpoint, bypass of package validation, or expanded
shell/SDK contract.

## 2. Package-management workflow

The panel lists current package rows and shows one selected package's detail.
It accepts a local ZIP for dry-run validation and install/upgrade, and allows
enable, disable, and confirmed uninstall. It makes no automatic second
mutation attempt after an uncertain response. Actions are serialized within
one mounted panel owner; deactivation retires that owner and stops its reads.

The existing `GET /api/packages?include_failed=1` is paginated globally.
Name-filtered rows from its observed pages provide a *visible history*, not
an exhaustive audit log or a dedicated `/api/packages/{id}/history` endpoint.
The panel shows request-intent and current session identity for correlation;
it does not claim a historical committed actor. The backend's actual
operation-specific audit/provenance remains authoritative. Route grants may
cause 403 responses even when the panel itself is visible.

The server exposes no progress percentage, intermediate stages, SSE stream,
or mutation idempotency key. The UI therefore distinguishes pending,
acknowledged, known failed, and **UNKNOWN** when a dispatched response is
lost, malformed, or cannot be attributed. A bounded GET-only reconciliation
can display observed package state, but a matching row does not prove that
this request committed or identify its actor. The operator must inspect
authoritative state/audit before deciding whether to submit another action.
Session preflight is a defensive stale-owner check, not an atomic lock on
browser cookie changes between preflight and dispatch.

## 3. Groups workflow and ownership

The groups panel uses the existing list, detail, create, update, delete,
member-add and member-remove routes. List filtering and 25-row paging are
local presentation of the returned group list, not server cursor pagination.
Detail includes the authoritative stored explicit-member count. No member
listing or user-directory endpoint exists: membership changes require the
complete target user UUID, and a count never establishes individual membership.
Changing explicit `admin` membership additionally requires repeating that UUID.

Names are 1–64 lowercase ASCII letters, digits, hyphens or underscores.
Descriptions are limited to 256 UTF-8 bytes. Create and update send an explicit
empty string for a blank description; the API stores SQL NULL and omits the
description from its response. Built-in groups have all metadata read-only
and cannot be deleted. `everyone` membership is virtual and cannot be edited;
the explicit-member count does not count everyone covered by virtual membership.
Other built-in membership, including `admin`, remains modifiable through the
existing authenticated route.

Each activation owns its reads and serialized mutation queue. Deactivation,
unmount or an observed session end retires that owner, aborts its reads and
discards late results and queued work. Session preflight and postflight checks
keep displayed results with the observed account; they cannot atomically lock
the browser's cookie identity across a server request. Denied reads/mutations
clear previously returned group detail and reconciliation data.

Duplicate names, missing/deleted targets and protected groups retain the API's
ordinary error responses. There is no backend revision check or new optimistic
concurrency protocol: another administrator may overwrite metadata. The panel
does not retry a mutation after an uncertain response. A lost, malformed or
unattributable acknowledgement is UNKNOWN. Optional GET-only reconciliation
shows current state without upgrading UNKNOWN to success or proving individual
membership or a historical committed actor. An already dispatched mutation may
complete after owner retirement; retirement cannot undo server work.

The recent-changes view records local request intent, observed account and
acknowledgement, not a server audit ledger. Existing `group.created`,
`group.updated`, `group.deleted`, `group.member_added` and
`group.member_removed` audit events remain authoritative, with their existing
actor/session/IP metadata. RBAC grant editing belongs to the separate #52 scope.

## 4. Test and packaging contract

The source graph under `client/admin/` packs deterministically via
`tools/pack_admin.py`; symlinks, hidden entries, and unexpected root entries
are rejected. The ZIP is an explicit build/test artifact, not part of the
shell bundle or image runtime layer. `plinth validate client/admin` and the
real package API apply the canonical cross-file validators.

Hermetic controller/transport and Preact/PanelManager browser fixtures target
owner retirement, serialization, role responses, unknown outcomes and
read-only reconciliation. The separate production-browser journey uses actual
bootstrap/registration/login sessions, the packaged ZIP, real route grants,
package lifecycle actions, data-preserving upgrade, failed migration, and
bounded shutdown. The groups journey must independently retain that package
proof and cover a genuine 0.1.0-to-0.1.1 installed upgrade, both primary panel
registrations, no automatic grants, denied/concurrent/protected-group cases,
explicit membership and persisted audit attribution. Mocked API or browser
fixtures receive no native persistence or audit credit. Candidate CI and
exact-merge CI are required before #51 closure; all acceptance receipts must
match the final source. This document defines scope and makes no fresh test-pass
claim.

## 5. Deferred work

RBAC administration, sidecar management and an audit-log browser are
separate future scopes. URL installs, complete server-side package history,
live progress stages, and automatic mutation retries are not part of this
panel. Realtime reconnection and sequence policy remain with issue #42.
