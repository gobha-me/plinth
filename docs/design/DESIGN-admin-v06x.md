# DESIGN-admin — Administration Package

**Status:** Admin-A package-management panel implementation for issue #50.
**Traces to:** `architecture/05-extensions.md`, `DESIGN-shell-v06x.md`,
`DESIGN-packages-v04x.md`.

## 1. Delivery and authority

`admin` is an ordinary, separately installed extension package. The local
`plinth_admin_zip` target creates `packages/admin-0.1.0.zip`; an authenticated
operator installs that archive through the existing package API. The kernel
does not install it on first boot, the runtime image does not bundle it, and
installation grants no permission to anyone. This is distinct from the
trusted first-boot **shell** bundle in `architecture/05-extensions.md §1.4`.
There is no release or global product-version change in Admin-A.

The package has one primary `packages` panel. Its rule is
`plinth.admin.packages` in the `plinth` namespace, owned by `admin`. It declares
no capabilities and does not redeclare the kernel's package rules. Operators
must separately grant both panel visibility and the existing route-specific
package rules to the intended groups. An installed package does not grant its
own access.

The panel uses the shipped shell panel SDK and the existing authenticated,
CSRF-protected `/api/packages` routes. Its server entry exists to satisfy the
normal package shape and does not proxy privileged work. There is no private
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

## 3. Test and packaging contract

The source graph under `client/admin/` packs deterministically via
`tools/pack_admin.py`; symlinks, hidden entries, and unexpected root entries
are rejected. The ZIP is an explicit build/test artifact, not part of the
shell bundle or image runtime layer. `plinth validate client/admin` and the
real package API apply the canonical cross-file validators.

Hermetic controller/transport and real Preact/PanelManager browser tests
cover owner retirement, serialization, role responses, unknown outcomes and
read-only reconciliation. A separate production-browser journey uses actual
bootstrap/registration/login sessions, the packaged ZIP, real route grants,
package lifecycle actions, data-preserving upgrade, failed migration, and
bounded shutdown. Candidate CI and exact-merge CI are required before issue
#50 is closed; the source and test receipts must match the final SHA.

## 4. Deferred work

Groups, RBAC administration, sidecar management and an audit-log browser are
separate future scopes. URL installs, complete server-side package history,
live progress stages, and automatic mutation retries are not part of this
panel. Realtime reconnection and sequence policy remain with issue #42.
