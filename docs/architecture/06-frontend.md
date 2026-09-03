# Architecture 06 — Frontend

**Owner:** this document. Authoritative for the "shell is an extension"
claim, the `frontend.mount` mechanism, extension asset serving at
`/ext/{name}/{version}/*`, the design-token / shared-asset serving
contract under `/api/frontend/*`, and the project's stance on
alternative frontends.

**Depends on:**
- `architecture/01-identity.md` (the frontend consumes the same auth,
  session, and RBAC model as any other extension).
- `architecture/02-capabilities.md` (the frontend calls kernel
  capabilities through the standard dispatch path).
- `architecture/03-data.md §3` (realtime pub/sub is the frontend's
  live-data substrate, including inter-panel communication).
- `architecture/05-extensions.md §1.3` (package structure — the shell
  is a package), `§1.4` (bundled packages — the shell is bundled and
  installed on first boot), `§2` (reserved URL prefixes — `/ext/` and
  the root redirect).

**Related:**
- `DESIGN-shell-v06x.md` (the 0.6.x shell arc: bootstrap, schema, the
  panel SDK, the tab/launcher model, floating panels, the system
  tray, content-type negotiation, and intents).
- `DESIGN-admin-v06x.md` (the admin extension, the second bundled
  package, consumes the shell's panel SDK).
- `DESIGN-packages-v04x.md` (install lifecycle, atomic version swap,
  `frontend.mount` validation).

---

## 1. The Shell is an Extension

Plinth's reference frontend — the shell — is implemented as a built-in
extension, not as kernel-privileged code. It consumes the same kernel
APIs (auth, RBAC, capability registry, realtime events, storage) that
every other extension uses. It has no special bypass into kernel
internals.

The shell is bundled with the kernel binary as a package blob
(`architecture/05-extensions.md §1.4`). On first boot, the kernel
bootstrap step checks whether a frontend extension is installed; if
none exists, the kernel extracts the bundled shell package and
installs it through the standard 0.4 package install lifecycle. From
that point on, the shell is a normal row in `plinth.packages`,
indistinguishable from any other extension except for a
`provenance = 'bundled'` flag.

**Three properties fall out of this:**

- **Upgradability without kernel rebuild.** A new shell version ships
  as a package. An admin installs it; the standard install lifecycle
  runs. No C++ recompile.
- **Architectural cleanliness.** The kernel has no shell-privileged
  code path. Every feature the shell uses is available to any other
  extension that requests it through the normal API. Code that
  special-cases the shell by name is a bug.
- **Dogfooding of the package system.** The shell's manifest is the
  first test case for package validation. If the shell can't install
  cleanly through the 0.4 lifecycle, the lifecycle is wrong.

Full shell design: `DESIGN-shell-v06x.md`. Full packaging contract:
`DESIGN-packages-v04x.md`.

**Implemented 2026-04-27 (v0.6.0).** Initial in-browser frame pinned in
ICD-0.6.0 (login flow consuming ICD-0.1.2, empty four-zone topbar, top-
level Preact error boundary). The 0.4.4 first-boot install lifecycle
already extracts the bundled shell to disk per `install_shell_if_needed`;
0.6.0 adds the kernel-stub static handler at `/app/*` that serves from
that on-disk location with strict CSP. The `frontend.mount` manifest
contract — i.e., the kernel reading the active frontend's mount prefix
from `plinth.packages` rather than hardcoding `/app/*` — is still
pending in 0.6.1 per `DESIGN-shell-v06x.md §9.1`.

---

## 2. `frontend.mount` — URL Ownership via Manifest

A frontend extension declares its URL prefix in `manifest.json`:

```json
{
  "name": "shell",
  "version": "1.0.0",
  "frontend": {
    "mount": "/app",
    "entry": "index.html"
  }
}
```

When this manifest is installed, the kernel reserves `/app/*` for this
extension's SPA serving. Concretely:

- `/app/` and every path under it that doesn't match a more specific
  rule returns the extension's `entry` file (`index.html`). The SPA
  handles routing client-side.
- `/ext/shell/{version}/*` serves the extension's static assets
  (JS, CSS, images) — same path any other extension uses (§3).
- The kernel's `/` root redirect points at the active frontend's
  mount (default: `/app/` for the shell).

### 2.1 Validation Rules

Enforced by the package system at install time
(`architecture/05-extensions.md §1.5`, `DESIGN-packages-v04x.md
§manifest-validation`):

- A `frontend.mount` value must not overlap any reserved prefix from
  `architecture/05-extensions.md §2`.
- At most one installed extension may declare `"mount": "/"`.
- At most one extension may claim any specific mount prefix (no two
  extensions mounted at `/app`).
- Installing an extension that would conflict requires explicit admin
  confirmation and disables the conflicting extension atomically (the
  atomic-swap contract in `DESIGN-packages-v04x.md
  §install-lifecycle`).

### 2.2 Active-Frontend Singleton

At most one installed frontend extension is the **active frontend** at
any moment. The active frontend is:

- The single extension that holds `"mount": "/"` (root redirect
  target), OR
- If no extension claims root, the single extension whose mount
  matches the kernel's configured default redirect target.

The active-frontend concept is load-bearing for the design-token
serving rules in §4. Most of those rules would be undefined with zero
or multiple active frontends; the install-time validation prevents
that.

---

## 3. Extension Asset Serving

Every installed extension has its client assets served at
`/ext/{name}/{version}/*`. The version segment is taken verbatim from
the extension's manifest. Example layout for Notes at 1.2.3:

```
/ext/notes/1.2.3/main.js
/ext/notes/1.2.3/panels/editor.js
/ext/notes/1.2.3/css/styles.css
/ext/notes/1.2.3/assets/icon.svg
```

**Cache semantics.** URLs are immutable within a version. The kernel
serves assets with `Cache-Control: public, max-age=31536000, immutable`.
When an extension upgrades from 1.2.3 to 1.2.4, all URLs change, the
browser fetches fresh assets, and the 1.2.3 assets become
garbage-collectable after a drain window (`DESIGN-packages-v04x.md
§install-lifecycle` for the atomic-swap contract).

**No build step.** Extensions ship source files directly. The shell
and any other frontend loads them via native ES modules. Shared
dependencies (Preact, htm, the kernel SDK, design tokens) live in the
active frontend's bundle and are imported by specifier through import
maps — extensions never ship their own copy of Preact.

**CSP.** The kernel serves a strict content-security policy:
`script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src
'self'`. Extensions run within this CSP. Any extension requiring
external script or connect sources is an architecture-session
conversation, not a code-session workaround.

**Unversioned URLs are not permitted.** `/ext/notes/main.js` without a
version segment is rejected by the serving layer. Versioned URLs are
load-bearing for cache invalidation and atomic upgrade; the kernel
does not support an "unversioned alias" mode.

---

## 4. Design Token Serving (`/api/frontend/*`)

**Status (2026-04-30, v0.6.3): `tokens.css` + `sdk.js` endpoints
implemented.** Per ICD-0.6.2 §6.1 / §6.7 and ICD-0.6.3 §5.2. Other
rows below remain `(deferred)` until extension panels surface a real
consumer (per ICD-0.6.2 OQ5 + ICD-0.6.3 §15 carry-forward).

Every extension that renders UI needs the active frontend's design
tokens — CSS custom properties, fonts, icons, any shared visual
primitives. Extensions cannot hardcode `/ext/shell/1.0.0/css/tokens.css`
because the version segment in that path changes on every shell
upgrade, and the extension has no reliable way to discover the
current version.

The kernel exposes a stable indirection layer under `/api/frontend/*`
that resolves to the active frontend's currently-installed version.

### 4.1 Endpoint Table

| Endpoint | Behavior |
|----------|----------|
| `/api/frontend/tokens.css` | ✓ implemented v0.6.2 — 302 → `/ext/{active-frontend}/{version}/css/tokens.css` |
| `/api/frontend/sdk.js` | ✓ implemented v0.6.3 — 302 → `/ext/{active-frontend}/{version}/client/sdk.js` |
| `/api/frontend/fonts/{name}` | (deferred) 302 → `/ext/{active-frontend}/{version}/fonts/{name}` |
| `/api/frontend/icons/{name}` | (deferred) 302 → `/ext/{active-frontend}/{version}/icons/{name}` |
| `/api/frontend/manifest.json` | (deferred) 200, JSON describing the active frontend's name, version, and exported asset paths |

### 4.2 Cache Semantics

The redirect responses are served with `Cache-Control: no-cache` — the
browser must revalidate the redirect target on every navigation so
that a shell upgrade is picked up without requiring a hard reload.

The targets (`/ext/{active-frontend}/{version}/...`) are served with
the standard immutable caching from §3. The indirection is cheap (302
with a small header payload); the heavy asset bodies are cached by
the browser forever and invalidated by the version bump.

### 4.3 Import-Map Binding

**Status (2026-04-30, v0.6.3): published.** The shell's `index.html`
declares the import map per ICD-0.6.3 §A.6.

The active frontend publishes an import map, consumed by
extension panels, that wires bare specifiers to the `/api/frontend/*`
indirections plus the same-origin Preact runtime. The stable specifiers
extensions write are:

```js
import { call, subscribe, useData } from '@plinth/frontend/sdk';
import { h } from 'preact';
import { useState, useEffect } from 'preact/hooks';
```

The import map resolves `@plinth/frontend/tokens` to
`/api/frontend/tokens.css`, `@plinth/frontend/sdk` to
`/api/frontend/sdk.js`, and `preact` / `preact/hooks` / `htm` to the
shell's same-origin vendored modules under `./vendor/`. The versioned
assets behind the `/api/frontend/*` redirector resolve once per shell
upgrade (specifier → `/api/frontend/*` → `/ext/{active}/{version}/*`);
every other import is cached.

### 4.4 Active-Frontend Requirement

The `/api/frontend/*` resolver depends on the active-frontend
singleton (§2.2). If no active frontend is installed — an invalid
state the kernel's first-boot bootstrap prevents — the resolver
returns 503. If two frontends are installed and both claim root — a
state the install-time validator prevents — the resolver returns 503
with a diagnostic response.

### 4.5 BYO-Frontend Constraint

An alternative frontend (§6) that wants to replace the shell **must
expose the same asset paths** under `/ext/{self}/{version}/...`:

- `css/tokens.css`
- `fonts/{name}` for whatever fonts it ships
- `icons/{name}` for whatever icons it ships
- `sdk.js` with the same panel-SDK exports

If the replacement frontend doesn't expose these, existing extension
panels break because their `@plinth/frontend/*` imports 404. This is
the design-token contract that a BYO frontend inherits by virtue of
claiming the active-frontend role.

This constraint is why BYO is permitted but not supported (§6): the
project publishes the shell's panel SDK as the ecosystem target, but
doesn't freeze its shape across shell revisions, so BYO frontends
chase a moving target.

---

## 5. Panel System (Summary)

**Status (2026-04-30, v0.6.3): operational.** Per ICD-0.6.3 §3
(Panel SDK API surface) and §4 (Panel module loading), extensions
register UI panels via `panels.json`; the shell's panel loader
(`client/shell/client/panels/loader.js`) dynamic-imports panel
modules and injects the `plinth.panel` API object.

Extensions register UI panels via `panels.json`. The active frontend
(the shell, in the default deployment) provides:

- **Panel container.** Topbar navigation, content area, tab strip,
  launcher. Authoritatively specified in `DESIGN-shell-v06x.md`.
- **Panel lifecycle.** Activate, deactivate, destroy.
- **Inter-panel communication.** Through the kernel realtime event
  system (`architecture/03-data.md §3`), not a frontend-specific bus.
  Panels talk to each other the same way services talk to each other.
- **Shared component library.** Primitives (buttons, forms, modals,
  tables) and design tokens (§4).

The panel SDK is a **frontend-ecosystem contract**, not a kernel
contract. A `client/` directory in an extension assumes the shell's
SDK and design tokens. This distinction matters for §6.

Authoritative panel-system design lives in `DESIGN-shell-v06x.md`.
The kernel's side of the contract — asset serving, token
indirection, realtime substrate — is defined here and in the
referenced cross-doc sections.

---

## 6. Alternative Frontends

The architecture does not prevent an alternative frontend from being
installed as a package and claiming `/`. A different SPA, a static
site, a custom dashboard — any of these can be packaged and installed
through the standard lifecycle.

**This configuration is not supported by the project.** Concretely:

- No compatibility testing. The kernel test suite validates the shell;
  it does not validate third-party frontends.
- No second-frontend feature parity commitment. Shell-specific APIs
  may evolve in ways that break alternative frontends.
- No guidance in `EXTENSION-GUIDE.md` for authoring panels against
  multiple frontends. Extension authors write against the shell's
  SDK and design tokens.
- No guarantee that the panel SDK shape is stable across shell
  versions.

The kernel's contract ends at the API surface
(`architecture/02-capabilities.md`, `architecture/03-data.md`); the
shell's contract ends at the panel SDK (`DESIGN-shell-v06x.md`). A
BYO frontend inherits the §4.5 asset-path contract and whatever
panel-SDK contract the current shell version publishes.

This is a deliberate asymmetry. The cleanliness of treating the shell
as an extension is architecturally valuable. Marketing BYO-frontend
as a product feature is not — it leads to ecosystem fragmentation
(the KDE-vs-GNOME failure mode) in exchange for a flexibility nobody
asked for.

---

## 7. Stack (Summary)

Authoritatively listed in `ARCHITECTURE.md §2` (core stack). For this
document's purposes the relevant facts are:

- **Preact/htm**, no build step.
- **Single-page application** loaded from the active frontend's
  `entry` file.
- **WebSocket for realtime** (first-class —
  `architecture/03-data.md §3`).
- **Client SDK** (`plinth.subscribe()`, `plinth.call()`,
  `plinth.useData()`, etc.) served by the active frontend under
  `/api/frontend/sdk.js` (§4).
