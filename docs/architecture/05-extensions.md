# Architecture 05 — Extension System

**Owner:** this document. Authoritative for the package structure and
split-manifest model, the QuickJS runtime sandbox and supervision, the
kernel's reserved URL prefixes, the cross-cutting composition framework
(surface traits, slots, augmenters), the deferred public HTTP options
(share primitive, site-host), and the **Extension HTTP Surface**
primitive (§6 — manifest-declared prefixes + catch-all + runtime route
table; ratified 2026-04-29).

**Depends on:**
- `architecture/01-identity.md §2` (RBAC rule registration via
  `rbac.json`; rule lifecycle tied to package lifecycle).
- `architecture/02-capabilities.md` (capabilities are registered by
  packages; the registry is the dispatch layer extensions participate
  in).
- `architecture/03-data.md §1` (extension PG schema isolation).
- `architecture/06-frontend.md` (`frontend.mount`, extension asset
  serving; shell is an extension that consumes these).

**Related:**
- `DESIGN-packages-v04x.md` (the 0.4.x arc that implements this
  architecture — manifest parsers, install lifecycle, `plinth.panels`
  table, atomic swap).
- `DESIGN-shell-v06x.md` (the shell is the first package built against
  this architecture; it establishes the panel SDK).
- `DESIGN-quickjs-bridge.md` (the runtime pool, supervision, and
  coroutine lifecycle live there; this document defines the contract
  the bridge honors).

---

## 1. Package Structure

A package is a directory (zipped for distribution) with this layout:

```
my-extension/
  manifest.json              — REQUIRED: identity (name, version, author)
  capabilities.json          — REQUIRED: provides + requires

  rbac.json                  — optional: rules + test contracts
  panels.json                — optional: UI panel registrations
  config.json                — optional: default configuration values
  README.md                  — optional: documentation

  server/                    — backend (QuickJS)
    main.js                  — entry point
    handlers/                — one file per capability handler
      shell.js
      exec.js
    lib/                     — shared server-side utilities
    tests/                   — backend tests

  client/                    — frontend (Preact/htm)
    panels/                  — panel components
    components/              — shared UI components
    css/                     — extension stylesheets
    assets/                  — icons, images

  migrations/                — PG schema migrations (numbered)
    001_create_tables.sql
    002_add_index.sql
```

**`server/handlers/` one-file-per-handler.** The single most impactful
structural decision for LLM-assisted development. When a code session
needs to modify `terminal:1:shell`, it opens `server/handlers/shell.js`.
The file IS the scope boundary.

**Split manifests (k8s-style).** Each file has a focused schema and
purpose. LLM code sessions touch only the file they're working on.

| File | Purpose | Required? |
|------|---------|-----------|
| `manifest.json` | Package identity and metadata | Yes |
| `capabilities.json` | What this package provides and requires | Yes |
| `rbac.json` | Permission rules with test contracts | If package has gated capabilities |
| `panels.json` | UI panel registrations | If package has UI |
| `config.json` | Default configuration values | If package is configurable |

### 1.1 Cross-File Manifest Validation

The kernel performs a **validation pass** at install time that reads
ALL manifest files together and checks:

- Every namespace in `rbac.json` has a matching namespace in
  `capabilities.json`.
- Every capability referenced in `rbac.json` test calls exists in
  `capabilities.json` with matching version.
- Every panel entry in `panels.json` references a file that exists in
  `client/panels/`.
- Every provided capability has a handler file in `server/handlers/`
  (warning, not error — upgraded to an error in the 0.4 design doc,
  see `DESIGN-packages-v04x.md §0.4.2`).
- Every provided capability should have at least one RBAC rule
  (warning, not error).
- `frontend.mount` (if present) does not overlap any reserved prefix
  from §2 below.

Errors block installation. Warnings are logged and shown to admin.

**CLI validation.** `plinth validate ./my-extension/` runs the same
validation locally without a running kernel.

### 1.2 `manifest.json`

```json
{
  "name": "terminal-core",
  "version": "1.0.0",
  "description": "Terminal access extension",
  "author": "jeff",
  "license": "MIT",
  "entry_point": "server/main.js"
}
```

Optional `frontend` block for extensions that claim a frontend mount
— see `architecture/06-frontend.md §2`.

Optional `shareable` array reserved for the deferred public share
primitive — see §Deferred below.

### 1.3 `capabilities.json`

```json
{
  "provides": [
    {
      "namespace": "terminal",
      "version": 1,
      "function": "shell",
      "params": [{ "name": "command", "type": "string" }],
      "returns": "result",
      "scope": "instance",
      "description": "Execute a shell command and return the result"
    }
  ],
  "requires": [
    "kernel:1:db.query(string) -> rows",
    "kernel:1:db.exec(string) -> result"
  ]
}
```

### 1.4 Bundled Packages (shell, admin)

Two extensions ship as bundled blobs inside the kernel binary and
install themselves on first boot through the **standard** package
lifecycle:

- **Shell** (`DESIGN-shell-v06x.md`) — the reference frontend.
- **Admin** (`DESIGN-admin-v06x.md`) — the built-in admin UI extension
  (groups, package management, RBAC).

There is no kernel-privileged code path for either. They consume the
same APIs any extension consumes. See `DESIGN-packages-v04x.md §0.4.4`
for the first-boot install mechanism and `architecture/06-frontend.md §1`
for why this packaging model matters.

---

## 2. Reserved URL Prefixes

The kernel permanently owns a fixed set of URL prefixes. Extensions
must not claim these paths. The kernel enforces this at package install
time (see `DESIGN-packages-v04x.md §0.4.2`).

| Prefix | Owner | Purpose |
|--------|-------|---------|
| `/api/*` | Kernel | Kernel API surface (auth, groups, RBAC, capabilities, packages, storage, frontend-tokens, etc.) |
| `/ext/{name}/{version}/*` | Kernel | Static asset serving for installed extensions (`architecture/06-frontend.md §3`) |
| `/s/*` | Kernel | Public share dispatcher (deferred; see §Deferred below) |
| `/healthz` | Kernel | Liveness probe |
| `/metrics` | Kernel | Prometheus exposition (`architecture/04-services-ha.md §3`) |
| `/docs/*` | Kernel | Markdown help reader (`reserved (planned)` — not yet implemented). In-kernel renderer of human-readable kernel + extension documentation. |
| `/ws` | Kernel | WebSocket upgrade endpoint (`architecture/03-data.md §3`) |
| `/app/*` | Shell extension (kernel-stub in 0.6.0; package-mediated from 0.6.1) | SPA-fallback handler serving the bundled shell's `client/` from disk per ICD-0.6.0 §8. 0.6.1 replaces the kernel-stub with manifest-driven `frontend.mount` dispatch (`architecture/06-frontend.md §2`). |
| `/` | Configurable | Redirects to `shell.root_redirect` (default `/app/`) per ICD-0.6.0 §4.5; the configurable mount-target form documented in `architecture/06-frontend.md §2`. |

Every other path is extension territory, claimed via `frontend.mount`
in an extension's `manifest.json` (see `architecture/06-frontend.md §2`).

**Sub-prefixes of `/api/*` owned by the kernel (non-exhaustive):**

- `/api/auth/*` — authentication (ICD-0.1.2)
- `/api/pats/*` — personal access tokens (ICD-0.1.3)
- `/api/groups/*`, `/api/rbac/*` — groups and RBAC (ICD-0.1.4, ICD-0.1.5)
- `/api/capabilities/*` — capability registry (ICD-0.2.0)
- `/api/packages/*` — package management (`DESIGN-packages-v04x.md`)
- `/api/storage/{extension}/*` — storage HTTP surface
  (`architecture/03-data.md §2.3`)
- `/api/frontend/*` — frontend asset indirection layer
  (`architecture/06-frontend.md §4`)
- `/api/docs/*` — dynamic API discovery surface (OpenAPI / Swagger;
  `reserved (planned)` — not yet implemented).

**Why this list is load-bearing.** Changing any reserved prefix after
1.0 breaks every installed deployment. Every extension manifest, every
bookmark, every documentation link, every third-party integration
depends on these paths being stable. This list is the contract.

**Additions to this list** require an architecture session and a
revision of this document. Removals are effectively impossible post-1.0.

**Extensions outside the reserved set.** Anything not listed in this
table or as a sub-prefix above is available to extensions via the
**§6 Extension HTTP Surface** primitive (ratified 2026-04-29):
extensions declare `http_prefixes` in their `manifest.json`; the
kernel performs install-time conflict checking against this table
(including `reserved (planned)` rows) and against every already-
installed extension. The `reserved (planned)` rows are load-bearing
today: they prevent third-party extensions from claiming prefixes the
kernel has staked but not yet implemented.

### 2.1 Extension HTTP Surface (no arbitrary routes; manifest-declared, conflict-checked prefixes only)

Extensions cannot register *arbitrary* URL paths at runtime. They can
claim **manifest-declared, install-time-conflict-checked** prefixes
via the §6 Extension HTTP Surface primitive.

The distinction matters:

- **Arbitrary registration (rejected).** Any extension at any time
  registering any path with the kernel's HTTP router, replacing
  previous registrations. Conflict resolution moves to runtime, the
  trust boundary diffuses, and the kernel cannot validate before
  dispatch. Re-introducing it defeats the capability-model guarantees.
- **Manifest-declared, conflict-checked prefixes (supported via §6).**
  An extension declares `http_prefixes` in its `manifest.json`; the
  kernel validates at install time against the §2 reserved set and
  against every already-installed extension's claims. Conflicts fail
  the install. The runtime route table is read-only between
  install/uninstall transitions.

The §6 primitive supports the latter and the latter only. See §6 for
the full contract: shape, schema, claim semantics, drain on uninstall,
privilege model, audit, performance, milestone slot.

---

## 3. QuickJS Runtime and Extension Supervision

Each script execution gets an isolated `JSRuntime`:

- Separate heap, separate GC.
- No shared mutable state between extensions.
- Runs within a Drogon coroutine (see `architecture/02-capabilities.md §3`
  and `DESIGN-quickjs-bridge.md`).

### 3.1 Runtime Limits

**Configurable per extension:**

```json
{
  "runtime": {
    "memory_limit_mb": 64,
    "cpu_time_limit_ms": 5000,
    "max_stack_depth": 1000
  }
}
```

Kernel provides defaults. Admin can override per-extension. The kernel
enforces the **lowest** of: extension request, admin override, kernel
maximum.

**Maximum package size:** configurable, default 50MB. Enforced at
install time before extraction.

**GlassWorm Unicode-smuggle defense (0.4.1):** Layer 2 of the
[GlassWorm scanner](../design/DESIGN-glassworm-defense-v0x.md) hooks
every `JS_Eval` call site (one-shot, pooled, async) via
[`pre_eval_scan`](../icd/ICD-0.4.1-glassworm-defense.md). Above-threshold
findings reject as `EvalErrorKind::UNICODE_SMUGGLE_DETECTED` before
the runtime sees the source bytes. Composes upstream of every other
runtime limit on this list.

### 3.2 Extension Supervision and Failure Recovery

Extensions fail. The kernel must handle this without operator
intervention for transient failures and with clear escalation for
persistent ones.

**On failure:**

1. The specific `BridgeContext` + its owning `JSRuntime` instance are
   torn down (`JS_FreeRuntime` on the context's runtime per
   ICD-0.3.1). The extension's `RuntimePool` itself persists.
2. Pending C++ coroutines associated with that context are cancelled.
3. An error is returned to the caller.
4. The failure is logged to `plinth.audit_log`.
5. The extension's failure counter increments.

**Restart with backoff:**

- After a failure, the extension remains available. The next
  capability call acquires a fresh `BridgeContext` from the persistent
  per-extension `RuntimePool` (see `architecture/02-capabilities.md
  §3.1` for the pool-ownership model and ICD-0.5.0.3 for the full
  lifecycle). The pool is torn down only on
  DISABLED / UPGRADING / UNINSTALL transitions or kernel shutdown —
  not on per-call failures.
- If an extension fails **N times in M minutes** (configurable,
  default: 5 in 5), the kernel **auto-disables** the extension.
- Admin is notified. Auto-disabled extensions can be re-enabled.
- The failure counter resets on re-enable.

**What is NOT affected by an extension failure:**

- Other extensions (separate runtimes, separate schemas).
- The kernel itself (failures are contained in the bridge layer).
- Other in-flight requests.
- Sidecar connections.

### 3.3 Extension Hot-Reload

QuickJS runtimes are cheap to create/destroy. The kernel supports
live-reload of extension code: when extension files change, the kernel
creates a new runtime, migrates active subscriptions, and tears down
the old runtime. No kernel restart needed.

**What is NOT available in the runtime:**

- Direct filesystem access (use `storage.*` API).
- Direct network access (use `http.*` API, RBAC-gated).
- Process spawning.
- `eval()` of arbitrary code strings (disabled by default).
- Access to other extensions' data or runtimes.
- Raw PG connection strings.

---

## 4. Cross-Cutting Composition Framework

Some extensions augment other extensions' surfaces rather than standing
alone. A grammar checker augments any text editor. An AI assistant
augments any content panel. An STT service augments any input surface.
The host extension doesn't know about the augmenter; the augmenter
doesn't know about specific hosts. They rendezvous through the registry
on a common vocabulary: **traits**.

This section reserves the architectural framework — the manifest
fields, the registry extensibility, and the three composition modes —
without designing the full system. The full composition mechanism is a
Scale-2 arc (targeted at ~0.9.x) that depends on real extensions
existing to test against; designing it in the abstract now produces
abstractions no real extension can adopt.

### 4.1 Reserved Manifest Fields

Extensions declare capability **shape** via three manifest fields. All
three are parsed and stored in 0.4 (when the manifest schema lands)
but **not interpreted** until the composition arc lands. Code and
design sessions in 0.4.x–0.8.x validate these fields structurally and
store them verbatim; no semantic validation until the arc.

**In `panels.json` entries:**

- **`surface_traits`** — array of strings. Traits this panel exposes
  to augmenters. Examples: `"text-editor"`, `"rich-text"`,
  `"markdown"`, `"code-surface"`, `"image-preview"`. A panel may
  declare multiple traits. The vocabulary is open; extensions compete
  on ecosystem adoption of their names.
- **`slots`** — object mapping slot names to slot definitions. Slots
  are named injection points where augmenters insert components.
  Canonical slot names (reserved, should be preferred): `toolbar`,
  `status-bar`, `context-menu`, `overlay`. Additional slot names
  permitted.

**At manifest level** (exact location — `capabilities.json` vs. a new
`augmenters.json` — deferred to the composition arc; reserve in both
parsers):

- **`augments_traits`** — array of strings. Traits this extension
  augments. An extension with `"augments_traits": ["text-editor"]`
  declares it wants to operate on any panel exposing the `text-editor`
  trait.

This pre-reservation is the entire purpose of the 0.4 manifest schema
being permanent. Extensions built in 0.4.x–0.8.x may declare traits
even though no augmenter machinery exists yet. When the composition
arc lands, those declarations retroactively become meaningful.

### 4.2 Three Composition Modes (framework, not design)

The composition arc will cover three mechanisms. Their relative scope
is sketched here to keep the eventual design doc grounded.

**Mode 1 — Slot Injection (primary, ~60% of cases).**
Host panels declare named slots. Augmenters inject components.
The shell/frontend mediates injection based on trait matching and
RBAC. The host doesn't know who injects; it provides the slot
vocabulary. Example: a grammar checker injects into the `toolbar`
slot of every `text-editor`-trait panel the user has enabled
augmentation for.

**Mode 2 — Event Stream / Pipeline (secondary).**
For non-UI augmentation — content transformation, LLM processing,
grammar checking as a service. Host panels emit surface events on a
well-known channel; augmenters subscribe and push results back through
a standardized channel. Uses the kernel's existing pub/sub
(`architecture/03-data.md §3`) with trait-scoped channels. Full design
deferred to the composition arc; this mode must be compatible with the
existing realtime infrastructure and must not require a new transport.

**Mode 3 — System-Level Services (deferred, distinct mechanism).**
STT, TTS, translation, accessibility services operate at the
shell/platform boundary, not within individual panels. They transform
the input/output stream between the user and any surface. This is a
**separate mechanism** from Modes 1 and 2 — a distinct architectural
concept, to be designed alongside or after the composition arc.
Reserving acknowledgment here so that the composition arc doesn't
accidentally absorb this scope.

### 4.3 Capability Registry Extensibility

When the composition arc lands, the capability registry
(`architecture/02-capabilities.md §1`) gains a query dimension: "who
augments surfaces of trait T?" This is a new query kind alongside "who
provides namespace X?"

The 0.2.x `plinth.capabilities` schema is sufficient to support this
without modification, provided the composition arc joins through a
separate augmenter index table (added in the arc). No 0.2.x schema
change is required now; equally, no 0.2.x schema change may be made
that would later conflict with this addition. See
`architecture/02-capabilities.md §1.12`.

### 4.4 RBAC for Augmentation (open question for the arc)

**Does the user control which augmenters attach to which surfaces?**

Expected answer: yes. Same "default applications" pattern used for
content-type resolution (`DESIGN-shell-v06x.md §5`), extended to
augmentation. A user opts in to a grammar checker for their text
editors; they do not get grammar-checking in their notes by default
just because both extensions happen to be installed.

Flagging this now so that:

- The composition arc has the question on its input list.
- The default-apps pattern in `DESIGN-shell-v06x.md §5` remains
  compatible with extension.
- No 0.6.x work forecloses this by, for example, storing content-type
  resolution in a shape incompatible with trait-based resolution.

### 4.5 Sequencing and Prerequisites

The composition arc depends on real extensions existing to pressure-test
the abstraction:

- **Files extension** (Scale 2, targets ~0.10.x–0.11.x).
- **Notes extension** (Scale 2, targets ~0.11.x).
- **At least one augmenter extension** (even a toy grammar checker)
  for design pressure-testing.

Real extensions provide the abstraction pressure. Designing composition
against hypothetical panels produces abstractions no real extension can
adopt.

**Infrastructure prerequisite.** By the time the composition arc
starts, extension design docs should live in a separate Gitea project
(docs-only, isolated from kernel code). The composition arc needs a
place for extension authors to declare and document their surface
traits. See `ARCHITECTURE.md §8` open question #9.

### 4.6 What This Framework Does NOT Design

All of the following are Scale-2 composition-arc design-doc work. The
architecture framework here exists so the arc starts from a sketch, not
from zero. Explicitly out of scope now:

- Exact slot component API (props passed, slot signature, unmount
  signaling).
- Event-stream channel naming conventions, ordering guarantees,
  backpressure.
- System-level service registration mechanism (the Mode 3 extension
  model).
- Augmentation RBAC rule shape.
- Default-apps extension to trait-based augmentation.
- Interaction with panel lifecycle (does an augmenter unmount when the
  host deactivates?).
- Performance / isolation boundaries (does augmenter code run in the
  host panel's JS context or its own?).
- Augmenter discovery UX in the shell.

---

## 5. Deferred: Public HTTP Surface Options

Two specific shapes for a public HTTP surface have been considered and
deferred. This section records them so that if either is picked up in
the future, the shape is already agreed and a design session starts
from a sketch rather than from zero.

Neither is on any current roadmap. Both are deliberately additive to
the existing architecture — nothing in the kernel forecloses either.

### 5.1 Share Primitive (most likely to land)

A kernel-mediated primitive for exposing individual resources (files,
notes, chat transcripts, etc.) at stable public URLs under
`/s/{token}`.

**Kernel owns:**

- `plinth.shares` table:
  `(token, resource_owner, owning_extension, resource_type,
    resource_id, expires_at, revoked_at, created_at, access_count)`.
- `/s/*` URL prefix (reserved in §2).
- Share creation, revocation, expiration, rate limiting.
- `UserContext::anonymous()` (`architecture/01-identity.md §3`) as the
  caller identity.
- HTML response composition: base template with OG tag slots,
  hydration script injection, `Cache-Control`, `ETag`, CSP,
  `X-Robots-Tag` per share options.

**Extension provides (per shareable resource type):** a `render_share`
QuickJS handler with a constrained signature:

```
render_share(token, resource_id, opts) -> {
  og: { title, description, image, type },
  body: <serialized content string>,
  hydration_data: <json for client-side interactivity>,
  cache_max_age: <seconds; kernel clamps>
}
```

The extension returns structured data. The kernel composes the final
HTML response using a fixed template. The extension never emits raw
HTML. This keeps the XSS surface in kernel C++ code that can be audited
once rather than in every extension.

**Request flow for `GET /s/{token}`:**

1. Kernel looks up token in `plinth.shares`. Miss / revoked / expired
   → 404.
2. Kernel checks rate limit for caller IP against token. Exceeded
   → 429.
3. Kernel dispatches to `{owning_extension}.render_share(...)` via the
   QuickJS bridge, with `UserContext::anonymous()`.
4. Extension reads its own data, returns the structured response.
5. Kernel composes HTML: doctype, OG tags, server-rendered body (for
   crawlers and no-JS fallback), hydration script.
6. Kernel sets response headers per share options.

**Manifest declaration (reserved in the 0.4 schema):**

```json
{
  "shareable": [
    { "resource_type": "note", "handler": "server/handlers/share_note.js" },
    { "resource_type": "file", "handler": "server/handlers/share_file.js" }
  ]
}
```

**Roadmap position if picked up:** new milestone ~0.11, Scale-2 design
doc required (`DESIGN-sharing-v011x.md` — outline exists).

**Dependencies:** 0.2.x (capability registry), 0.3.x (QuickJS bridge),
0.10.0–0.10.1 (storage, since Files is the first consumer).

**What this does not cover:**

- Human-readable URLs (slugs). `/s/{token}` is opaque.
- Writable anonymous flows (public comments, anonymous voting). Read
  paths only.
- Static marketing content. Use a reverse proxy / static host in front
  of Plinth for that.

### 5.2 Site-Host Extension (only if public slugged content matters)

**Status (2026-04-29):** §6 Extension HTTP Surface ratification reframes
this. Public-page surfaces (the primary §5.2 motivation — public
landing pages at known URLs) can now be served by extensions claiming
explicit `http_prefixes` + (where appropriate) `unauthenticated_prefixes`
via the §6 primitive — no fall-through machinery required. The
*single-site-host fall-through role* described below remains deferred
as a distinct shape: it would let one extension own "everything not
otherwise claimed," which the §6 primitive does not provide. If picked
up, the shape below applies; if a public-page extension is enough, §6
covers it without a new primitive.

If the project ever needs human-readable public URLs outside the share
primitive — `/packages/foo-ext`, `/forum/t/why-not-rust`, a public docs
site served from Plinth itself — the following shape applies.

**Single-site-host invariant.** At most one extension holds the
site-host role at a time. The role is claimed via a dedicated RBAC
rule (`sitehost.claim`), transferred by admin action, and revoked by
admin action. The kernel enforces singleton.

**Fall-through routing.** After the reserved prefixes in §2 and any
`frontend.mount` claims are resolved, any remaining request paths
route to the site-host extension's SSR handler. The handler receives
`(request, UserContext)` — `UserContext::anonymous()` for
unauthenticated requests — and returns a structured response (headers,
status, body) that the kernel emits.

**Why this is deferred.** The share primitive covers 80% of the
realistic public-content use cases (shared files, shared notes, shared
chat transcripts). The remaining 20% (package registry with nice URLs,
forum with SEO-friendly threads) is speculative — those products might
never ship. Adding site-host now pays for that speculation. Not adding
it now costs nothing, because nothing in the architecture precludes
adding it later.

**Interaction with §5.1.** Share primitive and site-host are
compatible. Share primitive handles opaque-token resource shares.
Site-host handles slugged public content. They occupy different URL
spaces (`/s/*` vs. fall-through).

### 5.3 Rejected (Not Deferred)

The following patterns are deliberately not in the architecture and are
not deferred for later consideration. Adding them requires a
fundamental revision of the architecture, not an additive patch:

- **Extensions registering arbitrary HTTP routes at runtime.**
  Runtime-mutable, unconstrained-by-manifest registration was
  rejected; manifest-declared, install-time-conflict-checked prefixes
  ARE supported via §6 Extension HTTP Surface (ratified 2026-04-29).
  The "arbitrary" framing of this bullet refers strictly to runtime-
  mutable / unconstrained-by-manifest registration. See §2.1 + §6.
- **Authenticated HTML-returning endpoints outside the frontend.**
  HTML is the frontend's job. Extensions expose capabilities. The
  `render_share` handler in §5.1 is the sole exception and is scoped
  to unauthenticated share rendering.
- **Public write endpoints without explicit abuse-surface design.** No
  anonymous POST, no anonymous capability invocation, no "quick
  feedback form" path. Any future public write requires its own
  architecture session covering captcha / rate limiting / moderation.

---

## 6. Extension HTTP Surface

The kernel exposes a primitive that lets extensions own HTTP route
prefixes outside the §2 reserved set — for protocol-compatibility
extensions (Files-Nextcloud-compat as the immediate case;
CalDAV / CardDAV / S3-compat / ActivityPub federation later), public
extension pages at predictable URLs, OAuth/embed flows, and the
share-primitive (§5.1) shape if it ever moves out of the kernel.

This section is **normative**. Design history lives in
[`DISCUSSION-extension-http-surface.md`](../discussion/DISCUSSION-extension-http-surface.md);
the architecture session of 2026-04-29 ratified the proposal with the
nine commitments below.

**Status:** ratified, not yet implemented. Implementation milestone:
**0.6.7 Extension HTTP surface — catch-all primitive + manifest
prefixes + runtime route table** (see `ROADMAP.md §0.6`). ICD-authoring
slot is `0.6.6.N ICD-0.6.7-extension-http-surface authoring`. No
schema, code, or test changes land from this section alone — only the
contract.

### 6.1 Principle

> **Kernel owns primitives; extensions own application surfaces.**

The test, applied to any HTTP-handling code: *can the route be
described without naming an application or a protocol?* If yes →
kernel; if no → extension. Each entry in the §2 reserved-prefix table
passes this test (`/api/*` is "kernel API surface" without naming a
consumer; `/healthz` is "liveness" without naming a probe).
`/remote.php/dav/files/...`, `/ocs/v1.php/cloud/...`, and
`/status.php` cannot be described without saying "Nextcloud" — the
signal that they belong in an extension, not the kernel.

This principle is the discipline that prevents the kernel from
accreting one application-specific endpoint at a time and turning
into a list of every protocol Plinth ever needed to speak. Future
architecture sessions start from it; departures must be argued
explicitly.

### 6.2 Shape

The primitive: one catch-all kernel route + manifest-declared
prefixes + install-time conflict check + runtime prefix → handler
lookup table + kernel-side PAT auth pre-dispatch + execution-mode-
agnostic handler reference.

Mechanically:

1. The kernel registers a catch-all path-pattern in Drogon at startup,
   sitting *after* the §2 fixed primitive routes (which match first
   by specificity).
2. Each extension's manifest declares `http_prefixes` it claims (e.g.
   `["/dav/*", "/ocs/*", "/remote.php/*", "/status.php"]`).
3. At install time, the kernel validates `http_prefixes` against (a)
   every reserved prefix in §2 (including `reserved (planned)` rows
   like `/docs/*` and `/api/docs/*`), and (b) every already-installed
   extension's `http_prefixes`. Conflicts fail the install with a
   clear error code (final code-shape pinned in ICD).
4. The kernel maintains a runtime route table keyed by prefix →
   `{extension_id, handler_reference, handler_mode}`. Thread-safety
   uses the same `shared_mutex` pattern as the
   [`enforcement.cpp`](../../src/kernel/rbac/enforcement.cpp)
   rule-requirement registry — dominated by reads (every catch-all
   dispatch), writes only on install / uninstall.
5. The catch-all handler does longest-prefix lookup in the runtime
   table, validates auth (PAT in the kernel, before dispatch), and
   forwards to the extension's declared handler.
6. The handler reference is execution-mode-agnostic: `quickjs` (in
   the extension's runtime pool), `sidecar` (over UDS), or
   `bundled_native` (Shell, Admin, etc.). The kernel doesn't pick;
   the manifest declares the mode.

### 6.3 Manifest schema additions

Three new fields land in the package manifest (exact location +
validation pinned in ICD-0.6.7; reserved in the parsers when the ICD
authoring slot opens):

- **`http_prefixes: string[]`** — the prefixes this extension claims.
- **`unauthenticated_prefixes: string[]`** — subset of `http_prefixes`
  that the catch-all dispatches *without* PAT validation (e.g.
  `/status.php` for Nextcloud-compat clients that probe before
  authenticating). Privileged: see §6.6.
- **`handler_mode: "quickjs" | "sidecar" | "bundled_native"`** —
  execution-mode tag. Per-prefix overrides reserved for ICD-0.6.7.

Validation against the §2 reserved-prefix table runs at install time;
overlapping any kernel-owned prefix (including `reserved (planned)`
rows) fails installation. Validation against already-installed
extensions runs in the same install-time pass.

### 6.4 Prefix-claim semantics (simplest-rule first)

- **Full-prefix exclusive ownership.** No two extensions may claim
  overlapping subtrees. `/dav/*` and `/dav/files/*` are conflicting
  claims — install rejects one or the other; both cannot coexist.
- **No method scoping.** An extension owning `/dav/*` owns *all* HTTP
  verbs on it. Two extensions cannot split GET vs POST on the same
  prefix.
- **No host scoping in single-tenant.** Multi-tenant per-host scoping
  is a separate question deferred to a future architecture session
  (see `DEFERRED.md`).
- **Wildcard depth.** A prefix matches any path depth below it
  (`/dav/*` matches `/dav/files/foo/bar`); the literal grammar pinned
  in ICD-0.6.7 (likely Drogon-compatible globbing).

Simplest-rule first is reversible: a future session can introduce
method or host scoping if a real use case demands it. Starting
permissive and narrowing later breaks installed extensions.

### 6.5 Uninstall while requests in flight

On extension uninstall:

1. The runtime route table entry is marked `draining`.
2. In-flight requests dispatched against the entry complete to
   natural termination.
3. New requests for the prefix → `503 Service Unavailable`.
4. The entry is removed from the table once the in-flight count
   reaches zero OR a configurable timeout fires (default and bounds
   pinned in ICD-0.6.7).

This matches the upgrade-drain semantics already implemented for
extension dispatch in
[`install_lifecycle.cpp`](../../src/kernel/packages/install_lifecycle.cpp)
(see ICD-0.4.5 §X.07–X.09); the HTTP-surface uninstall path reuses
the same `DrainState` machinery where the shapes align.

### 6.6 Privilege model

Declaring `http_prefixes` is gated by an RBAC rule at install time.
A new rule (working name `packages.install.with_http_prefixes`; final
name pinned in ICD-0.6.7) gates whether an install request carrying a
non-empty `http_prefixes` array succeeds.

- **Default grant:** `admins` group only.
- **Bundled-extension exception:** the bundled-package first-boot
  install lifecycle (ICD-0.6.1 §3) bypasses the install-request RBAC
  check by design — the kernel installs bundled packages directly. A
  bundled extension declaring `http_prefixes` is therefore implicitly
  privileged. The bundled shell does not currently declare
  `http_prefixes`; it consumes `frontend.mount` (ICD-0.6.1 §4) which
  is a separate primitive.
- **`unauthenticated_prefixes`:** declaring a non-empty
  `unauthenticated_prefixes` is gated by a separate, more privileged
  rule (working name `packages.install.with_unauthenticated_prefixes`;
  pinned in ICD-0.6.7). Default grant: `admins` only, with explicit
  install-time confirmation. Arbitrary third-party extensions cannot
  drop authentication without an admin grant + explicit confirmation.

The privilege model is RBAC-on-install — *not* RBAC-on-runtime-route.
Once an extension is installed, its prefix dispatches without further
runtime RBAC check (auth still happens; the privileged check is on
the act of *claiming* the prefix in the first place).

### 6.7 Audit log entry per dispatch

The kernel emits one non-skippable audit entry per catch-all-routed
request:

- `event`: `extension.http.dispatched` (final name pinned in
  ICD-0.6.7).
- `pat_identity`: PAT identity if authenticated; `unauthenticated`
  marker for `unauthenticated_prefixes` dispatches.
- `prefix_matched`: the manifest prefix that resolved.
- `extension`: extension name + version.
- `http_method`, `http_status`.
- `latency_ms`: optional, reserved for ICD-0.6.7.
- Rate-limited / dedup'd via the existing `audit::claim_*` pattern
  (reference ICD-0.5.5 OQ7's 60-second TTL precedent).

Extensions may emit additional audit entries via the
`kernel:1:audit.write` capability; the kernel-issued
`extension.http.dispatched` entry is non-skippable and authoritative
for "request reached an extension surface."

### 6.8 Performance contract

The catch-all + lookup + auth path must add **≤ 100 μs of overhead**
per request versus an equivalent fixed-prefix kernel-owned route
(starting figure; final threshold pinned in ICD-0.6.7).

Validation: a one-off micro-bench OR an LH-tier exercise of the
catch-all under sustained load (LH slot pinned at implementation
time). Performance below threshold gates the implementation
milestone.

Rationale: high-throughput surfaces (WebDAV under sync clients,
S3-compat object stores) have native-perf expectations; the catch-
all primitive cannot pay a per-request overhead that meaningfully
degrades those workloads. ≤ 100 μs is well below the millisecond-
scale latency of any realistic upstream operation, so the primitive
does not become the bottleneck.

### 6.9 Implementation milestone

**0.6.7 Extension HTTP surface — catch-all primitive + manifest
prefixes + runtime route table.** Slot lands after 0.6.6 closes the
shell SDK arc (tray + content-type + navigation); before any 0.7
schema-freeze work. ICD-authoring slot at `0.6.6.N`.

The Files-Nextcloud-compat arc (post-1.0 candidate) depends on this
primitive having landed; CalDAV / CardDAV / S3-compat / ActivityPub
extensions inherit it without further architecture work. The share
primitive (§5.1) and any future site-host shape (§5.2) become
extension-design questions instead of kernel-architecture questions.

### 6.10 What §6 does not decide

The session of 2026-04-29 deliberately did not pin:

- **Multi-tenant per-host scoping** for prefix claims. Single-tenant
  exclusive ownership only; multi-tenant is a future arch-session
  question.
- **Per-prefix `handler_mode` overrides.** The 2026-04-29 session
  pins one mode per extension via the manifest field; per-prefix
  overrides reserved for ICD-0.6.7 if a real use case surfaces.
- **Final performance threshold.** Starting figure ≤ 100 μs; ICD
  pins the final number after a measurement pass.
- **Drain timeout bounds and default.** ICD-0.6.7 pins the default
  and the configurable bounds.

These appear in `DEFERRED.md` to keep the active-question list
visible; each is a single-question follow-up that does not block
the implementation milestone or the ICD authoring.
