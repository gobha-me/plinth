# ICD-0.6.3-panel-sdk-client-sdk

**Traces to:** ICD-0.6.0 §15 *Panel SDK and client SDK* (lines
1055–1063 — "`DESIGN-shell-v06x.md §4` specifies the `plinth.panel.*`
API surface (lifecycle hooks, navigation intents, focus, shortcuts,
tray state) and the client wrappers `plinth.call()`,
`plinth.subscribe()`, `plinth.useData()`. **Closes: 0.6.3** per
`DESIGN-shell-v06x.md §9.3`. 0.6.0 ships zero panel surface…" —
this ICD authors that surface); ICD-0.6.0 §10 *Audit events*
(lines 708–711 — "Boundary-caught errors (§7.4) log to the browser
console only. Promotion to a kernel-side `frontend.boundary.caught`
audit family is deferred to 0.6.3 when the panel SDK exposes
`audit.log` to client code per `DESIGN-logging-subsystem.md`." —
this ICD promotes that family); ICD-0.6.0 §7 *Top-level error
boundary* (the boundary scaffold this ICD wires to the audit family
without changing the boundary's React semantics); DESIGN-shell-v06x.md
§4 *Panel SDK (`plinth.panel.*`)* (lines 405–457 — the canonical 11-
method API surface this ICD codifies into a contract; §4.1 line 410
the source-of-truth method list; §4.2 lines 447–456 the pub/sub
philosophy "the shell is not a message broker — it's a panel
container"); DESIGN-shell-v06x.md §3.2 *Panel Lifecycle* (lines
173–185 — the lifecycle states `mount → activate → deactivate →
unmount` this ICD pins as the shell-managed sequence around panel
component trees); DESIGN-shell-v06x.md §3.6 *Error Boundaries*
(lines 343–358 — every panel wrapped in a Preact boundary; throws
caught + fallback rendered + audit emitted; ICD-0.6.0 §7's top-level
boundary is the 0.6.3 emitter; per-panel boundaries arrive 0.6.5
with float chrome); DESIGN-shell-v06x.md §0.6.3 *Panel SDK and
Client SDK* (lines 742–753 — the milestone scope this ICD is the
paper for; entry "capability registry (0.2.x) operational; 0.6.2
complete"; exit "test extension loads, receives lifecycle events,
makes capability calls, subscribes to realtime events");
DESIGN-shell-v06x.md §10 *Constraints for Code Sessions* (lines
805–856 — esp. constraint #2 line 814 "the panel SDK is the only
contract"; #3 line 819 "RBAC gates everything visible"; #10 line
848 "do not foreclose cross-cutting composition" — this ICD's
event-model design accommodates future surface_traits / slots
augmentation without API churn); DESIGN-shell-v06x.md §11 OQ4
*Panels query API* (lines 884–887 — flagged as "ICD-level,
resolved during 0.6.x implementation"; ICD-0.6.3 explicitly does
**not** resolve OQ4 — deferred to ICD-0.6.4 paper authoring per
`feedback_icd_horizon.md` one-milestone-ahead discipline);
DESIGN-logging-subsystem.md *Logging API (QuickJS / Extensions)*
(lines 55–70 — the existing `audit.log("action_name", { detail })`
QuickJS binding shape this ICD extends to browser-side panel code
via the `plinth.call("audit.emit_boundary", …)` capability path —
not by re-exposing `audit.log` directly to the browser, per §10
SC2 below); architecture/06-frontend.md §4.3 *Import-Map Binding*
(lines 212–228 — the canonical bare-specifier resolution
`@plinth/frontend/sdk → /api/frontend/sdk.js`; this ICD pins the
`/api/frontend/sdk.js` endpoint as a 5th `/api/frontend/*` row, in
the same indirection family as `/api/frontend/tokens.css` v0.6.2
shipped); architecture/06-frontend.md §5 *Panel System (Summary)*
(lines 261–279 — "the panel SDK is a frontend-ecosystem contract,
not a kernel contract"; `panels.json` parse-and-store path already
lands in 0.4.x but is interpreted starting in 0.6.3 by the shell's
panel loader); architecture/06-frontend.md §1 *The Shell Is an
Extension* (the constraint that the panel SDK is shell-side
JavaScript injected into panel module scope, **not** kernel-side
QuickJS bindings — clear separation from the `cap.*` / `audit.*`
QuickJS surface);
`docs/sketches/shell-design-2026-04-27/project/notes.jsx`
+ `homecare.jsx` + `kb.jsx` + `automations.jsx` (the design-bundle
JSX prototypes whose component-factory + `data-ipoint` + props-
shape patterns this ICD canonicalises into the panel-loading
contract; Plinth Shell.html lines 23–73 already supply the token
substrate v0.6.2 shipped); ICD-0.5.0.3-extension-dispatch (the
Tier 2 dispatch path the new `audit.emit_boundary` capability
flows through — no new dispatch surface needed); ICD-0.5.2 *Pubsub
subscribe widening* (the SC6 cross-extension subscribe predicate
this ICD's `plinth.subscribe` consumes verbatim — no new RBAC
classification work; the broker validated at LH-2 2026-04-24 stress
of 4 producers × 4 subscribers × 120 s × burst=8 = 262k envelopes
clean carries forward); ICD-0.1.7-audit (the audit writer the new
`frontend.boundary.caught` audit family rides — kernel-prefixed
reservation rule per `audit_bindings.cpp` lines 44–56 means the
`frontend.*` prefix is **not** kernel-reserved and an extension-
owned audit family is permitted; the v0.6.3 code session's
shell-extension `audit.json` adds the family); METHODOLOGY-llm-
assisted-development.md §Phase 2 Constraint #4 (lines 838–851 —
the reason architecture/06-frontend.md gains a §4.1 row for
`/api/frontend/sdk.js` in the v0.6.3 *code* PR same-PR-as-the-
implementation; this ICD's Appendix B inventories that amendment).

**Depends on:** ICD-0.6.2 (tokens + theme + scaling shipped; the
panel substrate this ICD's plumbing consumes — extensions get
themed UI for free by importing tokens; no token contract change
this milestone); ICD-0.6.1 (the prior milestone whose
`active_frontend.cpp` resolver this ICD reuses verbatim for the
`/api/frontend/sdk.js` 302 target builder; no new query needed —
same `name + version` lookup pattern as `/api/frontend/tokens.css`
gets); ICD-0.6.0 (the bootstrap milestone; §7 top-level error
boundary is the 0.6.3 boundary emitter; CSP `connect-src 'self'`
constrains where `plinth.call` can dispatch — same-origin only;
strict CSP carries forward unchanged); ICD-0.5.0.3-extension-
dispatch (Tier 2 dispatch path the new `audit.emit_boundary`
capability + the existing `pubsub.subscribe` capability flow
through — no new dispatch surface); ICD-0.5.2-ws-broker (the
WS broker + SC2 dispatch-arm + SC6 cross-extension subscribe
predicate `plinth.subscribe` consumes; LH-2 2026-04-24 stress
validation carries forward); ICD-0.4.5-package-lifecycle-transitions
(the upgrade lifecycle the v0.6.3 shell upgrade rides —
`client/sdk.js` artefact + version bump `0.6.2 → 0.6.3`; no
migration changes; existing 0.4.5 atomic swap covers the file
replacement); ICD-0.1.7-audit (audit writer + non-forgeable payload
keys `audit_bindings.cpp:44-56` — `frontend.boundary.caught` rides
that contract for `user_id` / `session_id` / `extension_id`).

**Milestone:** 0.6.3 — Panel SDK + Client SDK. Eighth 0.6.x code
milestone (after v0.6.0 + 0.6.0.1 atexit-shutdown fix + v0.6.1
shell schema + v0.6.2 design tokens). Authored as paper-only
follow-up `0.6.2.N ICD-0.6.3 authoring` per
METHODOLOGY-llm-assisted-development.md §3.1 forward-ICD-presence
rule and `feedback_icd_horizon.md` (ICDs one milestone ahead).
The piece that closes ICD-0.6.0 §15's *Panel SDK and client SDK*
deferral and promotes ICD-0.6.0 §10's *boundary-caught audit*
deferral.

**Status:** Paper. Authored 2026-04-29 on
`feat/0.6.2.N-icd-0.6.3-authoring`. Code session pins OQ1–OQ7 then
implements; expected 4-phase commit arc.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
- `docs/architecture/06-frontend.md` (the contract owner; §4.3
  import-map binding this ICD ratifies for `@plinth/frontend/sdk`;
  §5 panel system summary this ICD's loader interprets;
  architecture promotions land in the eventual 0.6.3 *code* session,
  not in this paper-only PR per the ICD-0.6.1 / 0.6.2 paper-session
  precedent).
- `docs/design/DESIGN-shell-v06x.md` (the 0.6.x shell arc; §3.2
  panel lifecycle; §3.6 error boundaries; §4 the canonical SDK
  surface; §0.6.3 milestone scope; §10 constraints; §11 OQ4
  panels-query *deferred to ICD-0.6.4 paper, not resolved here*).
- `docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md` (the earliest
  prior milestone; §7 top-level error boundary is the 0.6.3
  boundary emitter; §15 panel-SDK + boundary-audit deferrals are
  the discharge anchors at lines 708–711 and 1055–1063).
- `docs/icd/ICD-0.6.1-shell-schema-user-preferences.md` (the
  immediately-prior code milestone; §3 first-boot bundled-shell
  install lifecycle the v0.6.3 shell upgrade rides; `frontend.mount`
  + `active_frontend.cpp` resolver path verbatim reused).
- `docs/icd/ICD-0.6.2-design-tokens-theme-scaling.md` (the most-
  recent code milestone; the `/api/frontend/tokens.css` indirection
  pattern this ICD mirrors for `/api/frontend/sdk.js`; the kernel
  handler at `src/kernel/frontend/api_frontend.{hpp,cpp}` gains a
  second route in the v0.6.3 code PR).
- `docs/icd/ICD-0.5.0.3-extension-dispatch.md` (Tier 2 dispatch
  path for the new `audit.emit_boundary` capability — no new
  dispatch surface).
- `docs/icd/ICD-0.5.2-ws-broker.md` (the WS broker
  `plinth.subscribe` consumes; SC6 cross-extension subscribe
  predicate; LH-2 2026-04-24 broker stress validation).
- `docs/icd/ICD-0.1.7-audit.md` (the audit writer the new
  `frontend.boundary.caught` family rides; kernel-prefixed
  reservation rule means `frontend.*` is permissible as an
  extension-owned family).
- `docs/icd/ICD-0.4.5-package-lifecycle-transitions.md` (the
  upgrade lifecycle the v0.6.3 shell upgrade rides — `client/`
  directory gains `sdk.js` artefact, manifest version bumps
  `0.6.2 → 0.6.3`; no migration changes; existing 0.4.5 atomic
  swap covers the file replacement).
- `docs/sketches/shell-design-2026-04-27/` (the canonical
  visual reference; the bundle's component-factory + `data-ipoint`
  + props-shape patterns are the rhetorical model for §4 panel
  loader semantics; the `*.jsx` files all consume tokens via
  `var(--bg-0)` / `var(--text-0)` etc. — already canonicalised by
  v0.6.2 + ICD-0.6.2 §3 token taxonomy).
- [client/shell/client/index.html](../../client/shell/client/index.html)
  (gains `<script type="importmap">…</script>` block for the
  `@plinth/frontend/{sdk,tokens}` bare-specifier resolution; gains
  `<script type="module" src="/api/frontend/sdk.js"></script>` for
  the early-load SDK).
- [client/shell/client/shell.js](../../client/shell/client/shell.js)
  (gains `plinth.panel.*` injection scaffold + `plinth.call` /
  `plinth.subscribe` / `plinth.useData` wrappers + boundary →
  audit emission; the v0.6.3 code session may split this into
  multiple files for ergonomics — `client/sdk.js` ships the SDK
  surface; `shell.js` retains shell-specific orchestration).
- [client/shell/client/sdk.js](../../client/shell/client/sdk.js)
  (NEW; the panel + client SDK module; default export shape pinned
  in §3 + §5; consumed by panels via the import-map specifier
  `@plinth/frontend/sdk`).
- [client/shell/client/panels/loader.js](../../client/shell/client/panels/loader.js)
  (NEW; panel-module loader; reads `panels.json` from the active
  frontend's manifest, dynamic-imports the panel ES modules,
  injects the `plinth.panel` scope, mounts the Preact component
  tree under the appropriate container).
- [client/shell/server/handlers/audit.emit_boundary.js](../../client/shell/server/handlers/audit.emit_boundary.js)
  (NEW; the QuickJS handler for the new `audit.emit_boundary`
  capability — calls `audit.log("frontend.boundary.caught", …)`
  with non-forgeable identity payload sourced from the kernel-
  bound session context per `audit_bindings.cpp:44-56`).
- [src/kernel/frontend/api_frontend.cpp](../../src/kernel/frontend/api_frontend.cpp)
  (gains a second route `/api/frontend/sdk.js` mirroring
  `/api/frontend/tokens.css` from v0.6.2; same active-frontend
  lookup, same 302 + Cache-Control: no-cache + 503 diagnostic; the
  active-frontend's `client/sdk.js` byte target replaces
  `client/css/tokens.css`).
- [src/kernel/js/stdlib/cap_bindings.hpp](../../src/kernel/js/stdlib/cap_bindings.hpp)
  (the existing `cap.call(capability, ...args) → Promise<result>`
  + error-mapping shape `capability_error_to_rejection(code,
  message, sqlstate?)` — the contract `plinth.call` mirrors over
  HTTP; no kernel-side change to this binding for v0.6.3).
- [src/kernel/js/stdlib/audit_bindings.cpp](../../src/kernel/js/stdlib/audit_bindings.cpp)
  (lines 44–56 — kernel-reserved-prefix table + non-forgeable
  payload-key table; `frontend.boundary.caught` rides these rules
  verbatim; no kernel-side change to these bindings for v0.6.3).

---

## Overview

ICD-0.6.3 closes the *Panel SDK and Client SDK* deferral queued in
ICD-0.6.0 §15 and promotes the *boundary-caught audit* family
queued in ICD-0.6.0 §10. The 0.6.0 milestone shipped a static
shell with no panel mechanism; 0.6.1 shipped the user-preferences
substrate and bundled-shell install lifecycle; 0.6.2 shipped the
token + theme + scale substrate. 0.6.3 lands the **first
extension-loadable panel surface** — a panel module exports a
Preact component, the shell's loader mounts it under the topbar,
the panel calls kernel capabilities through `plinth.call` and
subscribes to realtime events through `plinth.subscribe`, and the
top-level error boundary emits a kernel-side audit when the panel
throws.

Three things land:

1. **Panel SDK (`plinth.panel.*`).** The narrow contract between
   extensions and the shell, codified from `DESIGN-shell-v06x.md
   §4.1`. Eleven methods — five **live** in 0.6.3 (`onActivate`,
   `onDeactivate`, `setDirty`, `registerShortcut`, `getContext`)
   driving real shell-side state, six **stub** (`navigate`,
   `openFloat`, `onNavigationIntent`, `requestFocus`,
   `setTrayState`, `setTrayBadge`) registering signatures + reserve
   semantics for 0.6.4–0.6.6 to hook up. The SDK is in-process
   JavaScript, injected into panel module scope by the shell's
   panel loader; it is **not** a network API. Per
   `DESIGN-shell-v06x.md §10` constraint #2 (line 814): *the panel
   SDK is the only contract* — extensions do not import shell
   internals.

2. **Client SDK (`plinth.call` / `plinth.subscribe` /
   `plinth.useData`).** Browser-side wrappers that turn kernel
   capability dispatch + realtime subscription into ergonomic
   panel-side calls.
   - `plinth.call(capability, args) → Promise<result>` — POSTs to
     a new `POST /api/cap/{capability}` kernel HTTP route (pinned
     in §5.2; 200 on success with capability return value, 4xx on
     RBAC denial / capability not found, 5xx on dispatch failure;
     CapabilityError JSON body parsed to typed rejection
     `{code, message, sqlstate?}` per the `cap_bindings.hpp`
     precedent).
   - `plinth.subscribe(channel, handler) → unsubscribe` — opens or
     reuses a single shell-managed WebSocket connection
     multiplexed across all panel subscribers; channel-name
     classification per ICD-0.5.2 SC6; handler-error isolation
     (one panel's thrown handler does not break other subscribers).
   - `plinth.useData(channel, opts) → { data, error, loading }` —
     a Preact hook composing `subscribe` plus an initial snapshot
     fetch via `call`; cleanup on unmount; React-Query-style
     stale-on-error semantics.
   The SDK ships at `client/sdk.js` in the bundled-shell artefact;
   served via the kernel indirection `/api/frontend/sdk.js`
   (302 → `/ext/{active-frontend}/{version}/client/sdk.js`),
   mirroring the `/api/frontend/tokens.css` pattern v0.6.2 shipped.
   Consumed by panel ES modules via the import-map specifier
   `@plinth/frontend/sdk` per `architecture/06-frontend.md §4.3`.

3. **`frontend.boundary.caught` audit family.** Promoted from the
   ICD-0.6.0 §10 deferral. The top-level Preact boundary's
   `componentDidCatch` calls `plinth.call("audit.emit_boundary",
   …)` with a sanitised detail payload `{ panel_id, error_message,
   error_stack?, component_path? }`; the kernel writes
   `frontend.boundary.caught` to `plinth.audit_log` with non-
   forgeable identity payload (`user_id` / `session_id` /
   `extension_id` per `audit_bindings.cpp:44-56`) filled by the
   handler. **Audit emission via a dedicated capability
   (`audit.emit_boundary`), not via re-exposing `audit.log` to the
   browser** — security shape pinned in §10 SC2 to prevent panels
   forging arbitrary audit-event kinds.

The boundary stays narrow on purpose. 0.6.3 does not touch tabs /
sub-tabs / app-switcher / launcher (0.6.4), float chrome (0.6.5),
tray runtime (0.6.6), content-type resolution (0.6.6), or per-panel
error boundaries (0.6.5 with float chrome). Per
`DESIGN-shell-v06x.md §11 OQ4`, the kernel-side panels-query API
(`plinth.panels.list` or equivalent) is **not** resolved here —
deferred to ICD-0.6.4 paper authoring per
`feedback_icd_horizon.md` one-milestone-ahead discipline. ICD-0.6.3
ships the SDK; ICD-0.6.4 ships the topbar that consumes a panels-
list.

The v0.6.3 code session proves the contract end-to-end with a
**test-only fixture extension** (per architect-pin §14 OQ4) at
`tests/extensions/sdk_demo/` — a minimal `panels.json` declaring
one primary panel + an `index.js` that imports `@plinth/frontend/
sdk`, registers a callback, makes one `plinth.call` to a kernel
capability, subscribes to one realtime channel, and exercises
`useData`. The test extension is **not** shipped to production
(no `client/sdk_demo/` in the bundled-shell artefact, no entry in
the first-boot install path). Production users see no demo panel
in 0.6.3 — the topbar remains the empty four-zone ICD-0.6.0 frame
until 0.6.4 lands the dynamic panel rendering.

**Out of scope (deferred):**

- **Kernel-side `plinth.panels` table + query API
  (`plinth.panels.list`).** Out of scope; 0.6.4 ICD authoring
  resolves DESIGN-shell-v06x.md §11 OQ4 per
  `feedback_icd_horizon.md`. ICD-0.6.3 explicitly declines to pin
  the OQ4 shape; the 0.6.4 paper session pins it. **Closes:
  0.6.4.**

- **Topbar dynamic rendering (tabs, sub-tabs, app-switcher, Home
  launcher).** Out of scope; 0.6.4 code per
  `DESIGN-shell-v06x.md §0.6.4`. The four-zone topbar from
  ICD-0.6.0 §6 stays empty in 0.6.3; the SDK is wired but no
  shell UI consumes the panels-list yet. **Closes: 0.6.4.**

- **Float lifecycle + chrome.** Out of scope; 0.6.5 per
  `DESIGN-shell-v06x.md §0.6.5`. `plinth.panel.openFloat` ships
  as a stub method that throws `not_implemented`. Per-panel error
  boundaries also arrive 0.6.5 alongside float chrome — 0.6.3
  uses ICD-0.6.0 §7's top-level boundary as the single emission
  point. **Closes: 0.6.5.**

- **Tray runtime (icon rendering, popover lifecycle, badge/state
  API).** Out of scope; 0.6.6 per `DESIGN-shell-v06x.md §0.6.6`.
  `plinth.panel.setTrayState` + `setTrayBadge` ship as stub
  methods. **Closes: 0.6.6.**

- **Content-type handler resolution + navigation intents.** Out of
  scope; 0.6.6 per `DESIGN-shell-v06x.md §0.6.6` + §5.
  `plinth.panel.navigate` + `onNavigationIntent` ship as stub /
  stub-receiver methods. **Closes: 0.6.6.**

- **Cross-cutting composition (`surface_traits` / `slots`).** Out
  of scope; 0.9.x per `architecture/05-extensions.md §3.4`. ICD-
  0.6.3 accepts these fields in `panels.json` (parse + store; do
  not interpret) per ICD-0.6.0 §10 constraint #10 ("do not
  foreclose cross-cutting composition"). The panel-loader's
  injection model is designed so that surface-event delivery and
  slot injection can be added in 0.9.x without API churn.
  **Closes: 0.9.x.**

- **`audit.log` direct exposure to browser.** Out of scope; 1.0.x
  if ever. ICD-0.6.3 ships `audit.emit_boundary` as a single-
  purpose capability with `frontend.boundary.caught` as the only
  permitted action. Allowing browser code to emit arbitrary audit
  kinds would let panels forge `user.login.success` etc. — the
  kernel-reserved-prefix table (`audit_bindings.cpp:44`) already
  blocks panel-side `user.*` / `session.*` / `pat.*` / `group.*`
  / `rbac.*` / `capability.*`, but adding a single-purpose
  capability is cleaner than relying on prefix exclusion alone.

- **Per-panel error boundaries.** Out of scope; 0.6.5 per
  `DESIGN-shell-v06x.md §3.6` line 345 ("every panel … is wrapped
  in a Preact error boundary"). 0.6.3's top-level boundary
  catches everything; per-panel boundaries arrive when float
  chrome lands the per-float container. **Closes: 0.6.5.**

- **`/api/frontend/manifest.json`.** Out of scope; same as ICD-
  0.6.2 §15 — deferred. The shell's manifest is already served at
  `/ext/shell/{version}/manifest.json` via the asset path; the
  indirection is forward-compat scaffolding for downstream
  introspection. Add when a consumer milestone needs it.

- **`/api/frontend/fonts/{name}` and `/api/frontend/icons/{name}`.**
  Out of scope; same as ICD-0.6.2 §15 — deferred. The shell ships
  zero shared font / icon assets across extensions in 0.6.3; the
  `tokens.css` `@font-face` declarations cover Inter + JetBrains
  Mono via relative URL within the active-frontend's asset tree.

- **`superseded_seqs[]` in subscribe envelopes.** Out of scope;
  ICD-0.6.x source-seq tracking authoring (ROADMAP line 141 —
  `[fuzzy]`) closes ICD-0.5.5 W.06's `superseded_seqs[]` design
  defer. Three options on the table per ROADMAP: (a) plumb source-
  seq through coalescer→writer boundary, (b) re-introduce a peer-
  listener path, (c) discard the field entirely. ICD-0.6.3's
  `plinth.subscribe` does not pin `superseded_seqs[]`; the
  envelope shape is whatever the realtime broker emits today, and
  the future ICD's resolution applies transparently.

- **Sidecar / native panel modules.** Out of scope. 0.6.3 panels
  are QuickJS-runtime extensions producing browser-side ES
  modules; sidecar (0.8.x) and native (post-1.0) panel modules
  use the same SDK contract but the loader doesn't yet handle
  non-QuickJS handler_mode resolution — the kernel-side-extension-
  HTTP-surface arc (0.6.7 ICD authoring at ROADMAP line 150) will
  pin `handler_mode` resolution for capability dispatch, and the
  panel loader inherits whatever that resolves to. ICD-0.6.3
  does not pre-empt the 0.6.7 resolution.

---

## Glossary

- **Panel.** A self-contained UI surface registered by an
  extension via `panels.json` and rendered by the shell. Three
  types per `DESIGN-shell-v06x.md §3.1`: **primary** (the topbar's
  active-extension content area), **float** (draggable / modal
  windows; runtime arrives 0.6.5), **tray** (icon-anchored
  popovers; runtime arrives 0.6.6). 0.6.3 ships only primary-panel
  loading; the SDK `panel.type === 'primary'` branch is the live
  path.
- **Panel module.** The ES module file (typically `client/panels/
  {id}.js`) that exports a default Preact component. Loaded by
  the shell's panel loader (`client/shell/client/panels/loader.js`)
  via dynamic `import()`. The module receives `plinth.panel` as
  an in-process injection (not an import); the `plinth.call` /
  `plinth.subscribe` / `plinth.useData` surface is imported via
  the bare specifier `@plinth/frontend/sdk` resolved by the
  shell's import-map.
- **Panel ID.** The `id` field in `panels.json`. Globally
  unique-per-extension (the shell namespaces by extension; an
  extension's `id="editor"` and another extension's `id="editor"`
  do not collide). Used as the `panel_id` in
  `frontend.boundary.caught` audit payloads.
- **Active panel.** The primary panel currently displayed in the
  topbar's content area. Singleton — at most one primary panel
  active at a time. Set by the shell's panel loader at navigation
  time. The active panel's `onActivate` callback fires when it
  becomes active; `onDeactivate` when another panel takes over.
- **Panel SDK.** The `plinth.panel.*` surface injected into panel
  module scope by the shell loader. In-process JavaScript; not a
  network API. Eleven methods (5 live + 6 stub in 0.6.3).
- **Client SDK.** The `plinth.call` / `plinth.subscribe` /
  `plinth.useData` surface, served as a JavaScript module at
  `client/sdk.js` and consumed by panels via the import-map
  specifier `@plinth/frontend/sdk`. Browser-side wrapper around
  the kernel HTTP cap-dispatch endpoint and the realtime
  WebSocket broker.
- **`plinth.call`.** Browser→kernel capability dispatch wrapper.
  Translates a `plinth.call(capability, args)` JS call to a
  `POST /api/cap/{capability}` HTTP request with `args` as the
  JSON body. Returns a Promise resolving to the capability's
  return value or rejecting with a typed CapabilityError shape
  `{ code, message, sqlstate? }`.
- **`plinth.subscribe`.** Browser-side wrapper for the kernel
  realtime broker (ICD-0.5.2). Opens or reuses a shell-managed
  WebSocket connection; subscribes to the named channel; routes
  envelopes to the panel-supplied handler; handler-error isolation
  ensures one panel's thrown handler does not break other
  subscribers. Returns an `unsubscribe()` function.
- **`plinth.useData`.** Preact hook composing `plinth.subscribe`
  with an initial snapshot fetch via `plinth.call`. Returns
  `{ data, error, loading }`. Cleanup on component unmount via
  Preact's `useEffect` cleanup function.
- **Top-level boundary.** The Preact error boundary scaffolded in
  ICD-0.6.0 §7 wrapping the entire shell render tree at
  `client/shell/client/shell.js` (the `Boundary` component). Per-
  panel boundaries arrive 0.6.5 with float chrome; 0.6.3 uses the
  top-level boundary as the single emission point for
  `frontend.boundary.caught`.
- **`frontend.boundary.caught`.** The audit family promoted from
  ICD-0.6.0 §10 deferral. Emitted by the top-level boundary's
  `componentDidCatch` via `plinth.call("audit.emit_boundary", …)`.
  Detail payload: `{ panel_id, error_message, error_stack?,
  component_path? }`. Non-forgeable identity payload (`user_id`
  / `session_id` / `extension_id`) filled by the kernel handler.
- **`audit.emit_boundary`.** The new shell-extension-owned
  capability that wraps `audit.log("frontend.boundary.caught",
  …)` for browser-side panel emission. Single-purpose; rejects
  any action other than the literal `frontend.boundary.caught`
  per §10 SC2. Gated by RBAC rule `frontend.boundary.audit_emit`
  (granted to `everyone` group by default).
- **Active frontend.** Inherited from ICD-0.6.1 / 0.6.2 — the row
  in `plinth.packages` whose `provenance ∈ {bundled, admin}`,
  `frontend_mount IS NOT NULL`, `state IN ('ACTIVE',
  'ACTIVE_FLAGGED')` predicate matches. The
  `/api/frontend/sdk.js` redirector reads the active-frontend
  row's `name + version` to build its `Location:` header,
  identical to ICD-0.6.2's `tokens.css` pattern.
- **Import-map specifier.** A bare-module specifier like
  `@plinth/frontend/sdk` resolved by the browser's import-map
  declaration in `index.html`. Per
  `architecture/06-frontend.md §4.3`, `@plinth/frontend/sdk`
  resolves to `/api/frontend/sdk.js` and `@plinth/frontend/tokens`
  resolves to `/api/frontend/tokens.css`. Panel modules import
  `{ panel, call, subscribe, useData } from
  '@plinth/frontend/sdk'`.
- **Live method.** A `plinth.panel.*` method that ships a real
  implementation in 0.6.3. Five: `onActivate`, `onDeactivate`,
  `setDirty`, `registerShortcut`, `getContext`. Each drives
  shell-side state (callback registry, dirty bit, shortcut
  registry, context map).
- **Stub method.** A `plinth.panel.*` method that ships in 0.6.3
  with the correct signature but throws `not_implemented` (sync
  methods) or returns a rejected Promise (async methods) when
  called. Six: `navigate`, `openFloat`, `onNavigationIntent`
  (registers callback but never fires), `requestFocus`,
  `setTrayState`, `setTrayBadge`. Forward-compat regression
  category R.\* asserts the stub failure shape so 0.6.4–0.6.6
  cannot silently no-op the methods.
- **CapabilityError.** The kernel's typed error shape for
  capability-dispatch failures. JSON: `{ code, message, sqlstate? }`
  where `code` is one of the kernel-defined CapabilityError codes
  (`not_found`, `rbac_denied`, `quickjs_throw`, `dispatch_timeout`,
  etc.). The browser-side `plinth.call` rejection shape mirrors
  this (`Error` with `.code` / `.message` / `.sqlstate`
  properties). Inherited from `cap_bindings.hpp`'s
  `capability_error_to_rejection` precedent.

---

## §1 — Purpose

ICD-0.6.3 pins three contracts:

1. **The in-process panel SDK** (`plinth.panel.*`) — what
   extensions use to participate in shell lifecycle, register
   shortcuts, declare unsaved changes, retrieve navigation
   context, and (forward-compat) drive navigation, focus, tray
   state.

2. **The network-side client SDK** (`plinth.call` /
   `plinth.subscribe` / `plinth.useData`) — what extensions use
   to reach kernel capabilities and realtime events from
   browser-side panel code without raw `fetch` / `WebSocket`
   plumbing. Includes pinning the new browser→kernel HTTP cap-
   dispatch endpoint `POST /api/cap/{capability}`.

3. **The `frontend.boundary.caught` audit family** — promoted
   from the ICD-0.6.0 §10 deferral. The top-level Preact error
   boundary now emits a kernel-side audit when a panel throws,
   carrying enough payload (panel_id + error message + optional
   stack + optional component path) to triage the failure
   without exposing PII.

The boundary stays narrow: panel rendering in the topbar arrives
0.6.4; float chrome arrives 0.6.5; tray runtime + content-type
resolution + navigation intents arrive 0.6.6. ICD-0.6.3 ships the
SDK *signatures* and lifecycle contract for those forward
milestones (so they can hook up the existing methods without API
churn) but does not implement the runtime.

The discharge surface for this ICD's predecessors:

- **ICD-0.6.0 §15 *Panel SDK and client SDK* (lines 1055–1063)** —
  this ICD authors the surface. Discharged.
- **ICD-0.6.0 §10 *Boundary-caught audit promotion* (lines
  708–711)** — this ICD promotes the family to a kernel-side
  audit. Discharged.
- **DESIGN-shell-v06x.md §11 OQ4 *Panels query API*** — **NOT
  resolved here.** Deferred to ICD-0.6.4 paper authoring per
  `feedback_icd_horizon.md` (one milestone ahead). ICD-0.6.3
  cites OQ4 as live and reserves; ICD-0.6.4 pins.

---

## §2 — Architecture

The 0.6.3 surfaces split cleanly across two axes: in-process vs
network, and panel-scoped vs shell-managed.

### 2.1 The two surfaces

**Panel SDK (in-process).** `plinth.panel` is a JavaScript object
injected into a panel module's runtime scope by the shell's panel
loader. The injection is a closure-over-bound-state shape:

```js
// inside the loader, per-panel
const panelApi = makePanelApi({
  shell: shellState,
  panelId: 'editor',
  context: { file: 'meeting-notes.md' },
});
panelModule(panelApi);   // panelModule is the dynamic-imported default export
```

The panel module receives `plinth.panel` (renamed `panelApi` in
the loader for clarity but exposed to panel code as `plinth.panel`)
as the call argument; it does **not** import `plinth.panel` from
anywhere. The injection ensures: (a) the API is always bound to
the correct panel context (no per-call `panel_id` argument); (b)
the API is per-instance, so two instances of the same panel
(e.g. two open Notes documents) get separate `setDirty` / shortcut
registries; (c) the shell controls the API lifecycle — when the
panel unmounts, the shell unbinds the API and any further calls
become no-ops (or throw, per §3.x per-method).

**Client SDK (network).** `plinth.call` / `plinth.subscribe` /
`plinth.useData` are exported from a singleton ES module at
`client/sdk.js`, served by the active frontend at
`/ext/{name}/{version}/client/sdk.js`, accessed by panels via the
import-map specifier `@plinth/frontend/sdk`. The SDK module is
import-cached browser-wide; one module instance per page lifetime.
The SDK manages a single WebSocket connection multiplexed across
all subscribers. `plinth.call` is stateless per-call; each
invocation is a new `POST /api/cap/{capability}` request.

The two surfaces compose: panel modules import the network SDK at
the top of the module file (`import { call, subscribe, useData }
from '@plinth/frontend/sdk'`) and receive the in-process panel
SDK as an injection at the default-export call site.

### 2.2 Where the SDK module lives

The `client/sdk.js` artefact ships in the bundled-shell `shell.zip`
per the ICD-0.6.1 first-boot install lifecycle. The
`/api/frontend/sdk.js` kernel handler (new this milestone) reads
the active-frontend row from `plinth.packages` and returns 302 to
`/ext/{name}/{version}/client/sdk.js`. The asset-server byte path
serves the file with the standard immutable cache headers (per
ICD-0.6.0 / 0.6.1 / 0.6.2 asset-server posture) and the indirection
itself uses `Cache-Control: no-cache` so a shell upgrade is picked
up without a hard reload — identical posture to
`/api/frontend/tokens.css` v0.6.2 shipped.

The reason this is an indirection rather than a direct asset
reference: extensions cannot hardcode `/ext/shell/0.6.3/client/
sdk.js` because the version segment changes on every shell
upgrade and panels would 404 after upgrade. The kernel-mediated
stable URL `/api/frontend/sdk.js` lets every panel reference the
SDK with one canonical URL that always resolves to the active
frontend.

### 2.3 Per-extension panel module loading

The panel loader (`client/shell/client/panels/loader.js`, NEW)
performs the following sequence on first navigation to an
extension's primary panel:

```
1. Read the extension's panels.json from the kernel
   (placeholder API in 0.6.3 — no live consumer until 0.6.4 wires
    a topbar / app-switcher; the loader is testable via the
    test-fixture extension's static panels.json read directly)
2. For the panel matching the requested ID, resolve the panel
   module URL: /ext/{ext_name}/{ext_version}/client/panels/{id}.js
3. Dynamic import: const mod = await import(panelModuleUrl)
4. Build the panel API: panelApi = makePanelApi({...})
5. Call mod.default(panelApi) → returns a Preact component
6. Mount the component under the topbar content-area container
7. Fire panelApi.onActivate callback (if registered)
```

The dynamic-import path requires the active-frontend asset path
to serve panel-module bytes; this works because `frontend.mount`
extensions get the standard asset-server treatment per ICD-0.6.0
§8 and ICD-0.6.1 §4 — the shell's `register_active_frontend_routes`
already lands `/ext/{name}/{version}/(.*)` for every active-
frontend row.

For the test-fixture extension at `tests/extensions/sdk_demo/`,
the panels.json read step is mocked — the test loads the JSON
directly from disk via the test harness rather than through a
kernel API. The 0.6.4 milestone replaces the mock with the real
`plinth.panels.list` call (whatever shape ICD-0.6.4 pins).

### 2.4 Boundary emission path

ICD-0.6.0 §7's top-level Preact `Boundary` component wraps the
entire shell render tree. On `componentDidCatch`:

```js
componentDidCatch(error, errorInfo) {
  const detail = sanitizeBoundaryPayload({
    panel_id: this.context.activePanelId ?? null,
    error_message: error?.message ?? String(error),
    error_stack: process.env.NODE_ENV === 'production' ? null : error?.stack,
    component_path: errorInfo?.componentStack ?? null,
  });
  plinth.call("audit.emit_boundary", detail).catch(() => {
    // emission failure does not propagate (the boundary already
    // caught one error; we don't want a second one tearing down
    // the fallback UI).
  });
  this.setState({ caught: true, error });
}
```

The `audit.emit_boundary` capability is implemented as a shell-
extension QuickJS handler (`client/shell/server/handlers/
audit.emit_boundary.js`, NEW) that calls the kernel
`audit.log("frontend.boundary.caught", detail)` per
`DESIGN-logging-subsystem.md:67`. The non-forgeable identity
payload (`user_id` / `session_id` / `extension_id`) is filled by
the kernel side per `audit_bindings.cpp:44-56` — the panel cannot
forge those.

### 2.5 What's reused from prior milestones

Nothing kernel-side gets new infrastructure. The 0.6.3 contract
rides on:

- **`active_frontend.cpp`** (ICD-0.6.1) — verbatim. Same lookup
  for `/api/frontend/sdk.js`'s 302 target as for tokens.css.
- **`api_frontend.cpp`** (ICD-0.6.2) — gains a second registered
  route (`/api/frontend/sdk.js`) parallel to the first
  (`/api/frontend/tokens.css`). Same handler shape, same 503
  diagnostic, same Cache-Control semantics.
- **Tier 2 capability dispatch** (ICD-0.5.0.3) — the new
  `audit.emit_boundary` capability dispatches through the existing
  Tier 2 path; no new dispatch surface.
- **Realtime WS broker** (ICD-0.5.2) — `plinth.subscribe` consumes
  the existing channel-classification + cross-extension subscribe
  predicates verbatim. LH-2 (2026-04-24) broker stress validation
  carries forward; no new broker work.
- **Audit writer** (ICD-0.1.7) — `frontend.boundary.caught` rides
  the existing writer. Kernel-reserved-prefix table
  (`audit_bindings.cpp:44`) does not list `frontend.*`, so the
  family is permissible as an extension-owned audit.
- **Top-level boundary scaffolding** (ICD-0.6.0 §7) — the
  Preact `Boundary` component already wraps the shell. ICD-0.6.3
  adds the `componentDidCatch` audit emission; the React
  semantics (fallback rendering, panel isolation) carry forward
  unchanged.

The new kernel-side work this ICD authors:

- **`POST /api/cap/{capability}` HTTP handler.** New route on the
  Drogon HTTP listener that dispatches a capability call via the
  Tier 2 path with the session's PAT-or-cookie identity. JSON
  body parsed as the args array; capability return value JSON-
  encoded as 200 body; CapabilityError mapped to 4xx / 5xx with
  JSON body `{ code, message, sqlstate? }`. Filter chain:
  session-cookie → CSRF check → cap-dispatch.

- **`POST /api/frontend/sdk.js` 302 redirector.** New route on the
  kernel HTTP listener that resolves the active-frontend row and
  returns 302 to the versioned asset path. Mirrors
  `/api/frontend/tokens.css` from v0.6.2.

- **Panel-loader scaffold + `plinth.panel.*` injection.** New
  shell-extension JavaScript at `client/shell/client/panels/
  loader.js` and the SDK module at `client/sdk.js`. No kernel-side
  C++ change.

- **`audit.emit_boundary` capability + RBAC rule.** New shell-
  extension capability + `frontend.boundary.audit_emit` RBAC rule.
  Both ride the existing shell-extension manifest path (per ICD-
  0.6.1 — `client/shell/capabilities.json` + `client/shell/
  rbac.json`). No kernel-side change.

---

## §3 — Panel SDK API surface

This section pins the canonical method signatures, semantics,
lifecycle context (when each method is safe to call), error modes,
and 0.6.3 status (live vs stub). The set is lifted verbatim from
`DESIGN-shell-v06x.md §4.1` lines 410–441 — no method renaming, no
signature drift; the design doc is the source of truth and this
ICD codifies it.

The eleven methods split 5 live + 6 stub for 0.6.3. The split was
**architect-pinned** at the 2026-04-29 plan-mode interaction (per
the AskUserQuestion answer: "Plan's 5-live / 5-stub split
(Recommended)" — corrected to 5 live / 6 stub when the method
count was reconciled to the design doc's 11). Per §14 OQ1 the
**stub failure mode** is `throw not_implemented` for sync methods
and `Promise.reject(NotImplementedError)` for async methods —
never silently no-op, so 0.6.4–0.6.6 cannot accidentally let stub
behaviour leak into production.

### 3.1 Method status table

| Method | Status | Failure on call (if stub) |
|--------|--------|---------------------------|
| `onActivate(callback)` | Live | — |
| `onDeactivate(callback)` | Live | — |
| `setDirty(isDirty)` | Live | — |
| `registerShortcut(combo, callback)` | Live | — |
| `getContext()` | Live | — |
| `navigate(target, context)` | Stub | `throw NotImplementedError("plinth.panel.navigate")` |
| `openFloat(contentType, context)` | Stub | `Promise.reject(NotImplementedError("plinth.panel.openFloat"))` |
| `onNavigationIntent(callback)` | Stub-receiver | Registers callback (does not throw); callback never fires until 0.6.6 |
| `requestFocus()` | Stub | `throw NotImplementedError("plinth.panel.requestFocus")` |
| `setTrayState(stateName)` | Stub | `throw NotImplementedError("plinth.panel.setTrayState")` |
| `setTrayBadge(value)` | Stub | `throw NotImplementedError("plinth.panel.setTrayBadge")` |

The "stub-receiver" status for `onNavigationIntent` means the
shell accepts the callback registration without throwing (so 0.6.3
panels can register intent handlers eagerly), but the callback
never fires until 0.6.6 lands the navigation-intent runtime. The
distinction matters: extensions that wire up intent handlers in
their panel `onActivate` won't break in 0.6.3, and migration to
0.6.6 is zero-code-change.

### 3.2 `onActivate(callback)` — Live

**Signature:** `onActivate(callback: () => void): void`

**Semantics:** Registers a function to be called when this panel
becomes the active primary panel. Fires on:

1. Initial mount, after the Preact component tree is rendered
   for the first time.
2. Re-activation, when the user navigates away to another panel
   then back to this one (the panel module remains mounted; only
   the activation state changes — see §4 for the lifecycle
   contract).

**Multiple callbacks:** Multiple `onActivate` calls accumulate; all
registered callbacks fire in registration order on each activation.
There is no `offActivate`; callbacks are released on panel unmount.

**Synchronous fire:** Callbacks fire synchronously during the
shell's activation transition. If a callback throws, the
top-level boundary catches it; subsequent callbacks in the chain
do not fire (the chain aborts on first throw). This is the same
contract as DOM event listeners — chain abort on throw, not
continue-and-suppress.

**Lifecycle context:** Safe to call inside the panel module's
default-export factory function (i.e. at the top of the panel's
component setup). Calling after the panel has unmounted is a
silent no-op (the bound API is dead; the registry is gone).

**Error modes:** None for the registration itself. Callback
throws are caught by the top-level boundary.

**0.6.3 implementation site:** Shell-side per-panel registry
inside the loader (`client/shell/client/panels/loader.js`).

### 3.3 `onDeactivate(callback)` — Live

**Signature:** `onDeactivate(callback: () => void): void`

**Semantics:** Registers a function to be called when this panel
ceases to be the active primary panel. Fires on:

1. User-initiated navigation to another panel.
2. Panel unmount (the deactivate fires before the unmount
   sequence — so callbacks see a still-rendered panel they can
   read state from).

**Multiple callbacks + chain abort on throw:** Same contract as
`onActivate`. Callbacks fire in registration order; first throw
aborts the chain; top-level boundary catches.

**Lifecycle context:** Same as `onActivate` — safe to call inside
the panel module's setup.

**Error modes:** None for registration. Callback throws caught
by boundary.

**0.6.3 implementation site:** Same per-panel registry as
`onActivate`.

### 3.4 `setDirty(isDirty)` — Live

**Signature:** `setDirty(isDirty: boolean): void`

**Semantics:** Records the panel's dirty bit on the shell side.
Used by the shell to show a confirmation dialog ("You have unsaved
changes — discard?") on panel close, switch, or tab-close. The
dirty bit is per-panel-instance (two open Notes documents have
independent dirty bits).

**Idempotent:** Calling `setDirty(true)` repeatedly is a no-op
after the first call until `setDirty(false)` clears the bit.
Calling `setDirty(false)` repeatedly is idempotent.

**0.6.3 surfaces:** The dirty bit is stored shell-side; the
confirmation UI **does not yet ship** in 0.6.3 because 0.6.3 has
no panel-switching surface (the topbar is empty until 0.6.4). The
test-fixture extension's I.\* test exercises `setDirty(true) →
read-back via test seam` to confirm the bit lands. The
confirmation UI itself arrives 0.6.4 with the topbar's panel-
switch interaction.

**Lifecycle context:** Safe to call any time after panel mount.
Calling after unmount is a silent no-op.

**Error modes:** Throws `TypeError` if `isDirty` is not a
boolean. (Catch-and-rethrow contract: if the panel passes
`undefined`, the caller sees the throw immediately; the
top-level boundary catches if uncaught.)

**0.6.3 implementation site:** Shell-side per-panel state map
inside the loader.

### 3.5 `registerShortcut(combo, callback)` — Live

**Signature:** `registerShortcut(combo: string, callback:
(event: KeyboardEvent) => void): UnregisterFn`

Where `combo` is a key-combination string in the standard MDN
form: `"Ctrl+S"`, `"Ctrl+Shift+P"`, `"Meta+K"`, etc. (Modifier
order: Ctrl, Alt, Shift, Meta — alphabetical-by-name. Per §14 OQ7
the modifier ordering is normalised by the shell on registration
so `"Shift+Ctrl+S"` and `"Ctrl+Shift+S"` register the same combo;
duplicate registrations from the same panel are first-wins per
§14 OQ7.)

`UnregisterFn` is `() => void`: calling it deregisters the
shortcut. The function is idempotent — calling unregister twice
is a silent no-op.

**Semantics:** Installs a DOM-level `keydown` listener on
`document` (the shell's listener, not per-panel) that dispatches
to the registered callback when the combo matches. Only the
**active** panel's shortcuts fire; deactivated panels' shortcuts
are dormant (the shell holds the registration but skips dispatch
until reactivation).

**Conflict resolution:** First-wins per §14 OQ7. If panel A
registers `"Ctrl+S"` and panel B (later, while A is active and B
is some background panel that someone is trying to register
shortcuts for prior to activation) also registers `"Ctrl+S"`, the
second registration **fails** — `registerShortcut` throws
`ShortcutConflictError("combo Ctrl+S already registered by panel
{a_panel_id}")`. The error is synchronous; it surfaces at the
panel module's setup rather than silently overriding A's
binding.

**Cross-panel shortcuts:** Each panel sees only its own
shortcuts. The shell deduplicates per-panel; panel A's `"Ctrl+S"`
registration is independent of panel B's `"Ctrl+S"` (they are
different `(panel_id, combo)` keys). Conflict-detection is
**per-panel** at registration time. Cross-panel conflicts
(panel A and panel B both want Ctrl+S, both active simultaneously
post-0.6.5 floats) are resolved at activation time: only the
active panel's bindings fire. The shell does not pop a
disambiguation UI for cross-panel conflicts; first-mounted-wins
is the runtime semantic, but the design intent is that
extensions will rarely have non-overlapping shortcut sets and
the conflict cases are edge-cases.

**Browser default behaviour:** The callback's
`event.preventDefault()` is the panel's responsibility. The
shell's listener does **not** preventDefault by default —
panels that want to suppress browser shortcuts (e.g. `Ctrl+S`'s
"Save Page" prompt) call `event.preventDefault()` inside the
callback.

**Lifecycle context:** Safe to call inside the panel module's
setup. Calling after unmount throws `PanelUnboundError` (the
panel API is dead).

**Error modes:**
- `TypeError` if `combo` is not a string or `callback` is not a
  function.
- `ShortcutConflictError` if a same-panel shortcut for the same
  normalised combo is already registered.
- `PanelUnboundError` if the panel has unmounted.

**0.6.3 implementation site:** Shell-side global shortcut
registry keyed on `(panel_id, normalised_combo) → callback`.
DOM listener installed once at shell mount.

### 3.6 `getContext()` — Live

**Signature:** `getContext(): unknown`

**Semantics:** Returns the context object passed to the panel
when it was spawned or navigated to. The context is whatever the
caller of `navigate(target, context)` (forward-stub in 0.6.3) or
`openFloat(contentType, context)` (forward-stub in 0.6.3) supplied.
0.6.3's only live caller is the panel-loader's initial mount,
which passes `{}` (empty object) by default for the test-fixture
extension; production panels won't have a non-default context
until 0.6.4 wires the topbar's panel-switch interaction.

**Return shape:** Whatever the spawn-time caller passed —
typically a plain JSON-serialisable object. The shell does **not**
deep-clone the context; the returned object reference is the
same one the caller passed. Panels should treat the context as
read-only (mutating would surprise the caller); the shell does
not enforce this.

**Default:** `{}` if no context was passed (initial mount, test-
fixture).

**Lifecycle context:** Safe to call any time after the panel
factory is invoked. Calling after unmount returns the last-known
context (the bound API holds the reference); not particularly
useful but not a failure mode.

**Error modes:** None.

**0.6.3 implementation site:** Closure-captured value inside the
panel API factory.

### 3.7 `navigate(target, context)` — Stub

**Signature:** `navigate(target: string, context?: unknown): void`

**0.6.3 status:** Stub. Throws synchronously:

```js
function navigate(target, context) {
  throw new NotImplementedError(
    "plinth.panel.navigate is not implemented in 0.6.3 — closes 0.6.6"
  );
}
```

**0.6.6 contract:** Per `DESIGN-shell-v06x.md §3.5` — sends a
**navigation intent** to another panel. `target` is the panel
identifier in the form `"{ext_name}:{panel_id}"`; `context` is a
JSON-serialisable object passed to the target panel's
`onNavigationIntent` callback (and accessible via `getContext`
after the navigation completes).

**0.6.3 forward-compat regression test (R.\*):** R.01 asserts that
calling `navigate(...)` throws `NotImplementedError` with the
exact message string above — so 0.6.4–0.6.6 cannot let the
method silently no-op or change its error shape without breaking
the test.

### 3.8 `openFloat(contentType, context)` — Stub

**Signature:** `openFloat(contentType: string, context?:
unknown): Promise<{ floatId: string }>`

**0.6.3 status:** Stub. Returns a rejected Promise:

```js
function openFloat(contentType, context) {
  return Promise.reject(new NotImplementedError(
    "plinth.panel.openFloat is not implemented in 0.6.3 — closes 0.6.5"
  ));
}
```

**0.6.5 contract:** Per `DESIGN-shell-v06x.md §3.3` — requests
the shell to open a float for the given content type. The shell
performs content-type resolution (per `DESIGN §5`, arriving 0.6.6
in concert with `default_apps`), spawns the resolver-selected
panel as a float, and resolves the Promise with the assigned
`floatId`. The float lifecycle (drag, minimise, maximise, close,
state persistence) is managed by the shell; the calling panel
gets only the `floatId` for forward references.

**0.6.3 forward-compat regression test (R.\*):** R.02 asserts the
rejected-Promise shape (`Promise.reject(NotImplementedError)` with
the exact `.message`).

### 3.9 `onNavigationIntent(callback)` — Stub-receiver

**Signature:** `onNavigationIntent(callback: (target: string,
context: unknown) => void): void`

**0.6.3 status:** Stub-receiver. Registers the callback into the
panel's intent-handler chain (no throw on registration), but the
callback never fires in 0.6.3 because no live call site invokes
the dispatch path (no `navigate(...)` runtime; no
`openFloat(...)` content-type resolution). The chain is dormant
until 0.6.6 wires the dispatch.

**0.6.6 contract:** The panel's intent-handler chain fires when
another panel's `navigate(target, context)` arrives at this
panel's `target`. The callback receives the target's local part
(the substring after `"{ext_name}:"`) and the context object.
First-throwing callback aborts the chain (same contract as
`onActivate` / `onDeactivate`).

**Multiple callbacks:** Same accumulating registry as
`onActivate` — multiple `onNavigationIntent` calls register
multiple callbacks fired in order. There is no
`offNavigationIntent`; callbacks are released on panel unmount.

**Lifecycle context:** Safe to call inside panel module setup.
Post-unmount registration is a silent no-op.

**Error modes (0.6.3):** None — registration succeeds; callback
never fires until 0.6.6.

**0.6.3 forward-compat regression test (R.\*):** R.03 asserts
that registration succeeds (no throw) and the callback does not
fire even if the panel sees its own `navigate(...)` call (which
itself throws per R.01 — the callback never runs).

**0.6.3 implementation site:** Same per-panel callback registry
as `onActivate` / `onDeactivate`.

### 3.10 `requestFocus()` — Stub

**Signature:** `requestFocus(): void`

**0.6.3 status:** Stub. Throws synchronously:

```js
function requestFocus() {
  throw new NotImplementedError(
    "plinth.panel.requestFocus is not implemented in 0.6.3 — closes 0.6.5"
  );
}
```

**0.6.5 contract:** Per `DESIGN-shell-v06x.md §4.1` line 427 —
asks the shell to bring the panel to front. Only meaningful for
floats; primary panels are always in front; tray panels are
popover-anchored. Sister method to `openFloat`; both arrive in
the float-chrome milestone.

**0.6.3 forward-compat regression test (R.\*):** R.04 asserts the
sync throw with exact message.

### 3.11 `setTrayState(stateName)` — Stub

**Signature:** `setTrayState(stateName: string): void`

**0.6.3 status:** Stub. Throws synchronously:

```js
function setTrayState(stateName) {
  throw new NotImplementedError(
    "plinth.panel.setTrayState is not implemented in 0.6.3 — closes 0.6.6"
  );
}
```

**0.6.6 contract:** Per `DESIGN-shell-v06x.md §3.4` — sets the
active icon state for a tray panel. Allowed values per the
panel's `panels.json` `tray_states[]` list. The shell renders
the corresponding icon variant; out-of-list values throw
`InvalidStateError`.

**0.6.3 forward-compat regression test (R.\*):** R.05 asserts the
sync throw with exact message.

### 3.12 `setTrayBadge(value)` — Stub

**Signature:** `setTrayBadge(value: number | "dot" | null):
void`

**0.6.3 status:** Stub. Throws synchronously:

```js
function setTrayBadge(value) {
  throw new NotImplementedError(
    "plinth.panel.setTrayBadge is not implemented in 0.6.3 — closes 0.6.6"
  );
}
```

**0.6.6 contract:** Per `DESIGN-shell-v06x.md §4.1` line 439 —
sets the badge on the panel's tray icon. Number values render
as a numeric badge (e.g. unread count); `"dot"` renders an
indicator dot (no number); `null` clears the badge.

**0.6.3 forward-compat regression test (R.\*):** R.06 asserts the
sync throw with exact message.

### 3.13 Method ordering and naming summary

The 11 methods organise into four groups:

| Group | Methods | Status |
|-------|---------|--------|
| Lifecycle (callback registration) | `onActivate`, `onDeactivate`, `onNavigationIntent` | 2 live + 1 stub-receiver |
| State (per-panel data) | `setDirty`, `getContext` | 2 live |
| Input (keyboard) | `registerShortcut` | 1 live |
| Cross-panel (intents, focus, tray) | `navigate`, `openFloat`, `requestFocus`, `setTrayState`, `setTrayBadge` | 5 stub |

Naming convention is `verbNoun()` for action methods
(`setDirty`, `requestFocus`, `setTrayState`, `setTrayBadge`,
`registerShortcut`, `navigate`, `openFloat`) and `onEvent(callback)`
for callback-registration methods (`onActivate`, `onDeactivate`,
`onNavigationIntent`). `getContext()` is the lone `getNoun()`
read accessor. The naming is **frozen** in this ICD's §3 — future
ICDs may add methods but must not rename existing ones (panel
extensions consume the names, even if no production panels exist
in 0.6.3 — the contract is forward-binding from this PR onward).

---

## §4 — Panel lifecycle and injection model

This section pins how the shell loads a panel module, mounts it
under the appropriate container, and injects the `plinth.panel`
API. The 0.6.3 implementation lands the **primary** panel-type
path; **float** and **tray** types stub at the loader level and
defer to 0.6.5 / 0.6.6 for runtime.

### 4.1 Panel registration: `panels.json`

Extensions declare their panels in `panels.json` per
`DESIGN-shell-v06x.md §12`. The `parse_manifest` infrastructure
already lands in 0.4.x — the shell reads the parsed JSON from
`plinth.packages.manifest` rather than re-parsing the on-disk
file. A primary-panel entry has the shape:

```json
{
  "id": "editor",
  "type": "primary",
  "label": "Editor",
  "icon": "edit-3",
  "component": "client/panels/editor.js"
}
```

`id`, `type`, `label`, and `component` are required. `icon`
optional (used by 0.6.4's topbar render). `surface_traits` and
`slots` are accepted-and-stored per ICD-0.6.0 §10 constraint #10
but not interpreted in 0.6.3. `chrome_essential` (per
`DESIGN-shell-v06x.md §12` line 955) is accepted on tray panels
only and not interpreted in 0.6.3.

### 4.2 Loading sequence

When a panel needs to render (test-fixture path in 0.6.3; topbar
navigation path in 0.6.4+), the loader executes:

```
LOAD(extension_name, panel_id, context):
  1. row    ← SELECT name, version, manifest
                FROM plinth.packages
                WHERE name=$1 AND state IN ('ACTIVE', 'ACTIVE_FLAGGED')
  2. panel  ← row.manifest.panels.find(p => p.id == panel_id)
            (404 if not found; the test fixture treats this as a
             test failure)
  3. url    ← `/ext/${row.name}/${row.version}/${panel.component}`
  4. mod    ← await import(url)         // dynamic-import; CSP 'self'
  5. panelApi ← makePanelApi({
                  shell:      shellSingleton,
                  panel:      panel,
                  context:    context ?? {},
                  packageRow: row,
                })
  6. const Component = mod.default(panelApi)
            // panelApi is the call argument; not an import
            // Component is a Preact function or class component
  7. shell.mount(Component, container)
            // shell maintains the container element; 0.6.3 uses the
            // topbar's content-area container; 0.6.5 will dispatch
            // to per-float containers
  8. (panelApi.onActivate callbacks fire after the first paint)
```

The dynamic-import path requires `script-src 'self'` CSP — same-
origin only — which is satisfied because the asset bytes come
from `/ext/{name}/{version}/...` served by the kernel asset-
server (same origin as the shell). Cross-origin panel loading is
**not** supported; the kernel does not amend CSP at runtime.

### 4.3 The `makePanelApi` factory

```js
// pseudocode; v0.6.3 code session pins the exact signature
function makePanelApi({ shell, panel, context, packageRow }) {
  // per-panel state
  const activateCallbacks = [];
  const deactivateCallbacks = [];
  const navIntentCallbacks = [];
  const shortcutRegistry = new Map();  // normalised_combo -> callback
  let dirty = false;
  let unbound = false;

  // returns the plinth.panel object passed to the panel module
  return {
    onActivate(cb) {
      if (unbound) return;  // silent no-op post-unmount
      activateCallbacks.push(cb);
    },
    onDeactivate(cb) {
      if (unbound) return;
      deactivateCallbacks.push(cb);
    },
    onNavigationIntent(cb) {
      if (unbound) return;
      navIntentCallbacks.push(cb);
    },
    setDirty(isDirty) {
      if (unbound) return;
      if (typeof isDirty !== 'boolean') {
        throw new TypeError(`setDirty: expected boolean, got ${typeof isDirty}`);
      }
      dirty = isDirty;
      shell.notifyDirtyChange(panel.id, isDirty);
    },
    registerShortcut(combo, callback) {
      if (unbound) {
        throw new PanelUnboundError(`registerShortcut: panel '${panel.id}' is unbound`);
      }
      const normalised = normaliseCombo(combo);
      if (shortcutRegistry.has(normalised)) {
        throw new ShortcutConflictError(
          `combo ${normalised} already registered by panel '${panel.id}'`
        );
      }
      shortcutRegistry.set(normalised, callback);
      return () => shortcutRegistry.delete(normalised);
    },
    getContext() {
      return context;
    },
    navigate(target, _ctx) {
      throw new NotImplementedError(
        "plinth.panel.navigate is not implemented in 0.6.3 — closes 0.6.6"
      );
    },
    openFloat(contentType, _ctx) {
      return Promise.reject(new NotImplementedError(
        "plinth.panel.openFloat is not implemented in 0.6.3 — closes 0.6.5"
      ));
    },
    requestFocus() {
      throw new NotImplementedError(
        "plinth.panel.requestFocus is not implemented in 0.6.3 — closes 0.6.5"
      );
    },
    setTrayState(_stateName) {
      throw new NotImplementedError(
        "plinth.panel.setTrayState is not implemented in 0.6.3 — closes 0.6.6"
      );
    },
    setTrayBadge(_value) {
      throw new NotImplementedError(
        "plinth.panel.setTrayBadge is not implemented in 0.6.3 — closes 0.6.6"
      );
    },

    // shell-side internal accessor — not part of the public SDK contract
    __shell_internal: {
      fireActivate() { for (const cb of activateCallbacks) cb(); },
      fireDeactivate() { for (const cb of deactivateCallbacks) cb(); },
      fireNavigationIntent(target, ctx) { for (const cb of navIntentCallbacks) cb(target, ctx); },
      isDirty() { return dirty; },
      getShortcuts() { return shortcutRegistry; },
      unbind() { unbound = true; activateCallbacks.length = 0; deactivateCallbacks.length = 0; navIntentCallbacks.length = 0; shortcutRegistry.clear(); },
    },
  };
}
```

The `__shell_internal` namespace is not exposed to the panel
module — only the named methods + `getContext` form the contract.
The shell uses `__shell_internal` to drive lifecycle transitions
without the panel needing to be aware. Per
`DESIGN-shell-v06x.md §10` constraint #2 — *the panel SDK is the
only contract* — extensions never reach into `__shell_internal`;
the shell keeps it private by convention and discipline (no
runtime enforcement; v0.6.3 trusts panel modules not to
introspect).

### 4.4 Lifecycle states

A panel transitions through four states:

```
   (load)         (mount)        (activate)
unloaded → loaded → mounted → active
                       ↑          ↓
                       └──(deactivate)──
                       (still mounted; another panel takes over)

   (unmount)
mounted → unmounted (panel API unbound; further calls no-op or throw)
```

- **unloaded:** Panel module not yet dynamic-imported. Initial
  state.
- **loaded:** Panel module imported; default export available.
  Shell has not yet called `mod.default(panelApi)`.
- **mounted:** Panel API constructed; default export called; the
  returned Preact component is rendered in the container. The
  panel is now in the React tree but may not be active (e.g. a
  panel that's been swapped out for another but kept in memory).
  In 0.6.3, "mounted but not active" is theoretical only — the
  topbar lacks panel-switching, so once a panel mounts it remains
  active until unmount.
- **active:** Panel is the topbar's current primary content. Its
  shortcuts dispatch; its `onActivate` callbacks have fired.
- **unmounted:** Panel API unbound (`__shell_internal.unbind()`
  called); the React tree no longer contains the component;
  callback registries are cleared.

In 0.6.3, the only legal transitions are `unloaded → loaded →
mounted → active` (forward) and `mounted → unmounted` (cleanup).
Backward transitions and panel-swap-without-unmount are 0.6.4+
scope.

### 4.5 Boundary scope per panel

Per `DESIGN-shell-v06x.md §3.6` line 345, "every panel … is
wrapped in a Preact error boundary." 0.6.3 ships **only the
top-level boundary** (ICD-0.6.0 §7); per-panel boundaries arrive
0.6.5 with float chrome (the natural locus for per-float
containers). The implication: a panel's uncaught throw bubbles
to the top-level boundary, which renders the fallback UI for the
**entire shell**, not just the offending panel. This is the same
posture as 0.6.0 + 0.6.1 + 0.6.2 — single boundary, whole-shell
fallback.

The 0.6.3-specific addition: the top-level boundary's
`componentDidCatch` now emits a kernel audit (§6).

### 4.6 Multiple instances of the same panel

In 0.6.5 with float chrome, two open Notes documents will be
two instances of the Notes editor panel. The factory shape in
§4.3 supports this — each instance gets its own `panelApi` object
with its own callback registry, dirty bit, and shortcut
registry. The 0.6.3 loader does not exercise multi-instance
behaviour (no float runtime); the design is forward-compat for
0.6.5 to consume without API churn.

### 4.7 Panel-context object identity

The `context` argument to `getContext()` is the same object
reference passed to the loader at mount time. The shell does **not**
deep-clone or freeze the context. Panel modules should treat it
as read-only by convention; mutations are observable to the
caller (e.g. the calling panel that issued `navigate(target,
context)` would see its own object mutated). In 0.6.3 this is a
non-issue — there's no cross-panel `navigate` runtime — but it
matters for 0.6.6's intent dispatch.

The deep-clone alternative was considered and rejected: panels
that pass large context objects (e.g. a fully-loaded document
state) should not pay the clone cost on every navigation. Read-
only-by-convention matches React props posture.

---

## §5 — Client SDK API surface

This section pins the contract for `plinth.call` /
`plinth.subscribe` / `plinth.useData` and the new
`POST /api/cap/{capability}` kernel HTTP endpoint that
`plinth.call` consumes. The architecture for this layer derives
from `architecture/06-frontend.md §4.3` (import-map binding for
`@plinth/frontend/sdk`) and §5 (client-SDK-as-frontend-ecosystem-
contract).

### 5.1 The new kernel HTTP cap-dispatch endpoint

**Route:** `POST /api/cap/{capability}`

**Request:**
```http
POST /api/cap/shell.preferences.set HTTP/1.1
Cookie: plinth_session=<session_id>
Content-Type: application/json

{ "args": ["shell.theme", "dark"] }
```

The `args` field is a JSON array, matching the QuickJS
`cap.call(capability, ...args)` rest-args convention. Empty array
for capabilities that take no arguments. The kernel deserialises
each array element into the corresponding handler argument.

**Authentication:** The route is gated behind the existing
session-cookie filter (the same one ICD-0.1.x ships for
`/api/auth/*`). Unauthenticated requests get 401 with an empty
body; the browser's `plinth.call` wrapper does not redirect on
401 — that's the shell's `plinthFetch`/router responsibility. The
client SDK surfaces 401 as a typed rejection so panels can react
(e.g. by triggering a re-login prompt; out of scope for 0.6.3).

**Response (200 OK):**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{ "ok": true, "value": <capability return value JSON> }
```

The `value` field carries whatever the capability returned. The
`ok: true` envelope is forward-compat for streaming /
out-of-band responses; in 0.6.3 only `ok: true` shapes ship.

**Response (4xx — RBAC denied / not found / bad request):**
```http
HTTP/1.1 403 Forbidden
Content-Type: application/json

{ "ok": false, "error": { "code": "rbac_denied", "message": "User lacks rule shell.preferences.write" } }
```

Status code mapping:
- `400 Bad Request` — malformed JSON body, missing `args` field,
  invalid args type.
- `401 Unauthorized` — no session cookie / expired session.
- `403 Forbidden` — RBAC denial. Body's `error.code` is
  `rbac_denied` and `error.message` names the missing rule.
- `404 Not Found` — capability does not exist (no row in
  `plinth.capabilities`). Body's `error.code` is `not_found`.
- `429 Too Many Requests` — kernel-side rate limit exceeded
  (per ICD-0.5.0.3 dispatch rate limiting). Body's `error.code`
  is `rate_limited`.

**Response (5xx — dispatch failure):**
```http
HTTP/1.1 500 Internal Server Error
Content-Type: application/json

{ "ok": false, "error": { "code": "quickjs_throw", "message": "TypeError: ...", "sqlstate": "P0001" } }
```

Status code mapping:
- `500` — capability handler threw. Body's `error.code` is
  `quickjs_throw` (or whatever sub-code the kernel surfaces;
  inherits the existing `cap_bindings.hpp`
  `capability_error_to_rejection` table verbatim).
- `502` — sidecar / Tier 3 dispatch unreachable. Forward-compat
  for 0.8.x; 0.6.3 capabilities are all Tier 2 so this code
  shouldn't fire in practice.
- `503` — kernel transient unavailable (e.g. circuit breaker
  open per ICD-0.8.x).
- `504` — dispatch timeout.

**SQLSTATE optionality:** The `sqlstate` field appears only when
the capability handler's failure was a database error (the
existing `cap_bindings.hpp` Tier 2 code already populates
sqlstate when relevant). Browser code that wants to react to
specific PG errors (e.g. unique-violation `23505`) reads
`err.sqlstate`.

**CSRF:** The route requires a CSRF token for state-mutating
capabilities. Per existing `architecture/01-identity.md §3` the
double-submit cookie pattern applies: the SDK reads
`document.cookie['plinth_csrf']` and includes it as
`X-Plinth-CSRF` header. Read-only capabilities (e.g.
`shell.preferences.get`) skip the CSRF check; mutating ones
(e.g. `shell.preferences.set`) require it. The kernel filter
classifies via the `plinth.capabilities.read_only` column (added
0.5.0.3? — confirm at code session; if absent, all capabilities
require CSRF and read-only optimisation defers).

**Filter chain order:**
```
session-cookie → CSRF (if mutating) → cap-dispatch
```

The dispatch step writes the standard ICD-0.5.0.3 audit row for
the capability call.

### 5.2 `plinth.call(capability, ...args) → Promise<value>`

**Signature:**
```ts
function call<T = unknown>(
  capability: string,
  ...args: unknown[]
): Promise<T>;
```

**Behaviour:**
1. POST `/api/cap/{capability}` with body
   `{ "args": [<args[0]>, <args[1]>, ...] }`.
2. Include the CSRF header for mutating capabilities (the SDK
   sends it always — the kernel filter strips it for read-only
   routes, costing nothing).
3. Parse the response:
   - 200 `{ ok: true, value }` → resolve with `value`.
   - non-200 `{ ok: false, error }` → reject with a
     `CapabilityError` instance:
     ```js
     class CapabilityError extends Error {
       constructor({ code, message, sqlstate }) {
         super(message);
         this.name = 'CapabilityError';
         this.code = code;
         this.sqlstate = sqlstate;
       }
     }
     ```
4. Network failure (connection refused, DNS, etc.) →
   reject with a `NetworkError` instance:
     ```js
     class NetworkError extends Error {
       constructor(cause) {
         super(cause?.message ?? 'Network error');
         this.name = 'NetworkError';
         this.cause = cause;
       }
     }
     ```

**No retry:** `plinth.call` does not retry on any status code.
Panel code that wants retry-on-503 implements it explicitly
(usually via `useData` which has an opt-in retry hook).

**No timeout:** `plinth.call` inherits the browser's fetch
timeout (none by default). The kernel's dispatch timeout is the
authoritative one (per ICD-0.5.0.3); the browser sees 504
`dispatch_timeout` rejections when it fires.

**Cancellation:** `plinth.call` accepts an optional `AbortSignal`
as a final argument-prefix-flag. Per OQ4 the recommendation is to
**defer** AbortSignal support to a follow-up (0.6.3 ships
without; panels that need cancellation manage it via their own
fetch-replacement until the SDK adds the option).

**Example:**
```js
import { call } from '@plinth/frontend/sdk';

async function saveTheme(theme) {
  try {
    await call('shell.preferences.set', 'shell.theme', theme);
  } catch (err) {
    if (err.code === 'rbac_denied') {
      console.warn('user lacks shell.preferences.write');
    }
    throw err;  // surfaces in the boundary
  }
}
```

### 5.3 `plinth.subscribe(channel, handler) → unsubscribe`

**Signature:**
```ts
function subscribe<T = unknown>(
  channel: string,
  handler: (envelope: T) => void
): () => void;
```

**Behaviour:**
1. The SDK maintains a single shell-managed WebSocket connection
   to the kernel realtime broker (`/api/ws` per ICD-0.5.x). The
   connection opens lazily on first subscribe; subsequent
   subscribes reuse it.
2. Send a `{ "kind": "subscribe", "channel": "<channel>" }`
   message on the WS.
3. The kernel broker validates the channel name per ICD-0.5.2
   §SC6 cross-extension subscribe predicate (the same RBAC-gated
   classification that pubsub_subscribe applies).
4. Incoming envelopes for the channel are routed to the handler
   via the SDK's per-channel handler registry.
5. The returned `unsubscribe()` function:
   - removes the handler from the per-channel registry;
   - if the channel has zero remaining subscribers, sends
     `{ "kind": "unsubscribe", "channel": "<channel>" }` on the WS;
   - closes the WS if all channels are unsubscribed.

**Handler-error isolation:** A handler that throws does not break
other subscribers. The SDK wraps each handler call in a try/catch
and emits an `frontend.boundary.caught` audit (with `panel_id =
null` since the throw is in subscribe-routing context, not in a
panel render). Other handlers for the same channel still fire;
other channels are unaffected.

**Multiplex:** N panels can subscribe to the same channel; the
WS sends one subscribe + each envelope arrives once on the WS;
the SDK routes to all N handlers locally. This is more efficient
than per-panel WS connections (the LH-2 broker stress validation
2026-04-24 already established the broker's fan-out efficiency at
4×4 = 16 producer×subscriber × 262k envelopes clean — single-WS-
multiplex inherits the same posture).

**Reconnection:** The SDK reconnects the WS automatically on
unexpected disconnect (e.g. network blip, kernel restart). On
reconnect, it re-subscribes to all active channels. Per OQ5 the
reconnection backoff is **exponential with cap** (1s → 2s → 4s
→ 8s → ... cap 30s); panel code does not need to manage
reconnection. The handler does not receive a "connection lost"
notification in 0.6.3 (per OQ5 default — defer to future ICD
when use-cases surface).

**Example:**
```js
import { subscribe } from '@plinth/frontend/sdk';

const unsub = subscribe('notifications:user', (envelope) => {
  console.log('new notification:', envelope);
});

// later, on panel unmount:
unsub();
```

### 5.4 `plinth.useData(channel, opts) → { data, error, loading }`

**Signature:**
```ts
function useData<T = unknown>(
  channel: string,
  opts?: {
    snapshot?: { capability: string, args: unknown[] };
    initialData?: T;
  }
): { data: T | undefined; error: Error | null; loading: boolean };
```

**Behaviour:**
This is a Preact hook that composes `plinth.subscribe` with an
optional initial `plinth.call` snapshot. State machine:

```
mount:
  if opts.snapshot:
    state ← { data: opts.initialData, error: null, loading: true }
    call(opts.snapshot.capability, ...opts.snapshot.args)
      .then(value => state ← { data: value, error: null, loading: false })
      .catch(err  => state ← { data: state.data, error: err, loading: false })
                     # leave-stale-on-error per OQ5 (already settled in plan)
  else:
    state ← { data: opts.initialData, error: null, loading: false }

  unsub ← subscribe(channel, envelope => state ← { ...state, data: envelope })

unmount:
  unsub()
```

The hook returns the current state on every render. Preact's
`useState` + `useEffect` machinery handles the re-render
triggers; the SDK's job is to translate envelope arrivals into
state updates.

**Stale-on-error:** Per OQ5 (already settled in the plan), if a
subscribe envelope arrives but the handler call somehow fails
(typically only via a malformed envelope JSON), the previous
`data` is preserved and `error` is set. Panels render error
banners on top of stale data rather than blanking.

**Snapshot optionality:** If `opts.snapshot` is omitted, the
hook starts in `{ data: initialData, error: null, loading: false }`
and only updates when subscribe envelopes arrive. This is the
"realtime-only" mode for use cases like a notification-stream
panel that doesn't need an initial fetch.

**Example:**
```js
import { useData } from '@plinth/frontend/sdk';

function NotesList() {
  const { data, error, loading } = useData('notes:list', {
    snapshot: { capability: 'notes.list', args: [] },
    initialData: [],
  });

  if (loading) return html`<p>Loading…</p>`;
  if (error) return html`<p>Error: ${error.message}</p>`;
  return html`
    <ul>
      ${data.map(note => html`<li>${note.title}</li>`)}
    </ul>
  `;
}
```

### 5.5 The `client/sdk.js` module

The exports of the singleton ES module at `client/shell/client/
sdk.js` (NEW) include:

```js
// public API
export { call, subscribe, useData };

// re-exports for ergonomic import
export const plinth = {
  call,
  subscribe,
  useData,
  // panel namespace is injected per-panel by the loader, not via this export.
};

// types
export class CapabilityError extends Error { /* see §5.2 */ }
export class NetworkError extends Error { /* see §5.2 */ }
export class NotImplementedError extends Error { /* see §3 */ }
export class ShortcutConflictError extends Error { /* see §3.5 */ }
export class PanelUnboundError extends Error { /* see §3.5 */ }
```

The `plinth` named export is a convenience for panels that prefer
namespaced access:
```js
import { plinth } from '@plinth/frontend/sdk';
plinth.call('foo.bar', 1, 2);
```

vs the named-imports pattern:
```js
import { call } from '@plinth/frontend/sdk';
call('foo.bar', 1, 2);
```

Both are supported.

### 5.6 The `/api/frontend/sdk.js` indirection

A new kernel HTTP route at the same handler as
`/api/frontend/tokens.css` from v0.6.2 (`src/kernel/frontend/
api_frontend.cpp`):

**Route:** `GET /api/frontend/sdk.js`

**Behaviour:** Identical to `/api/frontend/tokens.css`:
1. Resolve the active-frontend row via the existing
   `active_frontend.cpp` lookup.
2. If found, return:
   ```http
   HTTP/1.1 302 Found
   Location: /ext/{name}/{version}/client/sdk.js
   Cache-Control: no-cache
   ```
3. If not found / multiple found, return 503 with the same
   diagnostic body shape as `/api/frontend/tokens.css` (per
   ICD-0.6.2 §6.4):
   ```json
   { "error": "no_active_frontend", "detail": "..." }
   ```

The asset-server byte path serves `/ext/{name}/{version}/client/
sdk.js` with the standard immutable cache headers — same posture
as v0.6.2's tokens.css. A shell upgrade picks up the new SDK on
the next page load (the indirection's `no-cache` ensures
revalidation; the versioned target is immutable but the version
segment changes with each upgrade).

**No auth filter** — same as `/api/frontend/tokens.css` per
ICD-0.6.2 §17 deviation #1. The shell self-references this URL
from the login page (panels need the SDK to render the login form
itself, eventually); auth-gating would 401 the unstyled login UX.

### 5.7 Import-map declaration

The shell's `index.html` gains an import-map block:

```html
<script type="importmap">
{
  "imports": {
    "@plinth/frontend/sdk": "/api/frontend/sdk.js",
    "@plinth/frontend/tokens": "/api/frontend/tokens.css"
  }
}
</script>
```

The import-map must appear in `<head>` before any
`<script type="module">` that uses the bare specifiers. Per the
HTML spec, only one import-map is allowed per page; the shell
owns it.

The CSS specifier `@plinth/frontend/tokens` is for completeness
(a panel could `import` the URL string and use it as a stylesheet
href programmatically); the canonical consumption pattern is
still `<link rel="stylesheet" href="/api/frontend/tokens.css">`
from panel HTML, not via import.

### 5.8 What `plinth.call` does NOT do

Out of scope for `plinth.call`:

- **Streaming / chunked responses.** 0.6.3's `plinth.call` only
  handles single-JSON-object responses. Streaming (e.g.
  Server-Sent Events for long-running tasks) defers to a future
  capability shape — likely `subscribe`-based.
- **File uploads / multipart.** 0.6.3's `plinth.call` only sends
  JSON bodies. File uploads use a different surface (e.g. Files
  extension's `POST /api/storage/...` per ICD-0.10.0 placeholder)
  that bypasses the cap-dispatch route.
- **Capability batching.** The QuickJS-side `cap.batch([...])`
  has no `plinth.call`-side analogue in 0.6.3. Panels that need
  batched calls issue parallel `plinth.call(...)` invocations
  (the kernel handles concurrency); explicit batching defers
  until usage pressure surfaces.

### 5.9 What `plinth.subscribe` does NOT do

Out of scope for `plinth.subscribe`:

- **Custom routing key patterns.** The channel name is opaque to
  the SDK; the realtime broker (ICD-0.5.x) handles patterns. The
  SDK does not synthesise channel names from object selectors.
- **Replay / rewind.** Subscribers receive only envelopes
  published *after* their subscribe lands; `superseded_seqs[]`
  tracking + replay defer per §15.
- **Multi-channel batched subscribe.** One `subscribe` call =
  one channel. Panels that subscribe to N channels issue N calls
  (the SDK batches the WS messages internally).

---

## §6 — `frontend.boundary.caught` audit family

This section pins the audit family promoted from ICD-0.6.0 §10
(lines 708–711). The browser-side top-level boundary now emits a
kernel audit when a panel throws, replacing the 0.6.0–0.6.2
console-only behaviour.

### 6.1 Audit action name

Literal string: `frontend.boundary.caught`.

The `frontend.*` prefix is **not** kernel-reserved (per
`audit_bindings.cpp:44` — the reserved prefixes are
`user.`, `session.`, `pat.`, `group.`, `rbac.`, `capability.`).
This means an extension-owned audit family at `frontend.*` is
permissible — the shell extension owns it. The shell's
`audit.json` (added 0.6.3) declares:

```json
{
  "kinds": [
    { "kind": "frontend.boundary.caught" }
  ]
}
```

(Manifest format inherits the existing extension-audit-declaration
shape from ICD-0.1.7. The exact JSON schema is whatever
`parse_manifest` accepts today — confirm at v0.6.3 code session.)

### 6.2 Detail JSON shape

```json
{
  "panel_id": "editor",         // string | null
  "error_message": "TypeError: cannot read property 'x' of undefined",
  "error_stack": "Error\n  at NotesPanel (.../editor.js:42:15)\n  ...",
  "component_path": "in NotesPanel\n  in App\n  in Boundary"
}
```

- **`panel_id`** — the active panel's id at the time of the
  catch, or `null` if the throw fired in a non-panel context (e.g.
  inside a `subscribe` handler where the panel is no longer
  active). Sourced from the boundary component's React context.
- **`error_message`** — the `error.message` string, length-capped
  at 1024 bytes (longer messages truncated with a `...` suffix).
- **`error_stack`** — optional. The `error.stack` string,
  length-capped at 8192 bytes. Per OQ6 (§14) the recommendation
  is to **omit in production builds** (NODE_ENV ≠ 'development'),
  shipping only in dev/debug; this prevents stack traces from
  including potentially sensitive variable names from minified
  bundles. Architect may pin to "always include" if minification
  is judged sufficient.
- **`component_path`** — optional. Preact's `componentStack`
  string from `errorInfo` — the component-tree path to the
  failing component. Same length cap as stack (8192 bytes).
  Always included if available.

### 6.3 Non-forgeable identity payload

Per `audit_bindings.cpp:44-56` the kernel-side audit writer fills
non-forgeable identity fields automatically:

```cpp
constexpr std::array<std::string_view, 7> RESERVED_PAYLOAD_KEYS = {
    "user_id", "session_id", "ip_address", "extension_id",
    "node_id", "call_depth", "timestamp"};
```

These fields are stripped from any panel-supplied detail and
filled by the kernel from the dispatch context. The
`audit.emit_boundary` capability handler reads them from
`session.user_id`, `session.session_id`, etc. The panel cannot
forge a different `user_id` or claim to be running under a
different `extension_id`. This is the same posture as every
other extension-emitted audit per ICD-0.1.7.

### 6.4 The `audit.emit_boundary` capability

To prevent panels from forging arbitrary audit kinds (e.g.
`user.login.success`), the boundary does **not** call `audit.log`
directly — there is no `audit.log` exposed to browser-side panel
code in 0.6.3. Instead, the boundary calls a **single-purpose
capability** that wraps `audit.log` server-side with the action
name pinned:

**Capability:** `audit.emit_boundary`

**Owned by:** shell extension (declared in `client/shell/
capabilities.json` per ICD-0.6.1's pattern).

**Signature (QuickJS handler):**
```js
// client/shell/server/handlers/audit.emit_boundary.js
export default async function(detail) {
  // detail is { panel_id, error_message, error_stack?, component_path? }
  // The kernel-side audit.log fills user_id / session_id / extension_id.
  // This handler enforces the action name; it cannot be coerced to
  // emit a different kind.
  audit.log("frontend.boundary.caught", {
    panel_id:       String(detail?.panel_id ?? null),
    error_message:  truncate(String(detail?.error_message ?? ''), 1024),
    error_stack:    detail?.error_stack ? truncate(String(detail.error_stack), 8192) : undefined,
    component_path: detail?.component_path ? truncate(String(detail.component_path), 8192) : undefined,
  });
}

function truncate(s, max) {
  return s.length > max ? s.slice(0, max - 3) + '...' : s;
}
```

The handler:
1. Sanitises and length-caps each detail field.
2. Calls the kernel-side `audit.log` with the **fixed** action
   name. Panels cannot vary the action name.
3. Returns nothing (capability return value is ignored).

### 6.5 Boundary integration

The top-level Preact `Boundary` component's `componentDidCatch`:

```js
componentDidCatch(error, errorInfo) {
  const detail = {
    panel_id:       this.context.activePanelId ?? null,
    error_message:  String(error?.message ?? error ?? 'unknown error'),
    error_stack:    process.env.NODE_ENV === 'production'
                       ? undefined
                       : (error?.stack ? String(error.stack) : undefined),
    component_path: errorInfo?.componentStack
                       ? String(errorInfo.componentStack)
                       : undefined,
  };

  // Fire-and-forget; audit emission failure must not bubble.
  call('audit.emit_boundary', detail).catch((auditErr) => {
    // Last-resort: log to browser console. We do not re-throw
    // because we are already inside componentDidCatch — re-throwing
    // would leave the React tree in an inconsistent state.
    console.error('frontend.boundary.caught: audit emission failed', auditErr);
  });

  this.setState({ caught: true, error });
}
```

The `.catch` ensures audit-emission failure does not propagate.
The boundary's primary job is rendering the fallback UI; audit is
secondary and best-effort.

### 6.6 RBAC rule

A new RBAC rule:

**Rule name:** `frontend.boundary.audit_emit`

**Default grant:** `everyone` group (per ICD-0.6.1's
`default_grants[]` infrastructure). Granted by default to every
authenticated user.

**Why a rule exists at all:** The rule lets administrators
**revoke** boundary-audit emission for diagnostic / forensic
postures (e.g. when investigating an audit-noise pattern, an
admin can temporarily revoke the rule and observe the resulting
kernel quiet). It is not a security gate — boundary-caught errors
are not adversarial signals — but a mute switch for audit-volume
control.

**Capability gating:** The `audit.emit_boundary` capability
checks the `frontend.boundary.audit_emit` rule on dispatch. RBAC
denial returns 403 with the standard CapabilityError shape; the
boundary's `.catch` swallows the rejection (audit silently
suppressed; console-fallback fires).

### 6.7 Rate limiting / dedup

Per OQ7, **no built-in rate limit** for `frontend.boundary.caught`
in 0.6.3. Boundary-caught errors should be rare; the volume is
not expected to require dedup. A future ICD adds rate-limit /
TTL infrastructure if dedup pressure surfaces (e.g. a panel that
re-throws on every render would emit one audit per render —
admin-side de-dup in audit queries is the workaround).

### 6.8 Read access

The `frontend.boundary.caught` audit rows are readable via the
standard ICD-0.1.7 audit-log read APIs (RBAC-gated by
`audit.read.shell` or whatever the per-extension audit-read rule
is — confirm at code session). Admins can query:

```sql
SELECT timestamp, user_id, data->>'panel_id' as panel, data->>'error_message' as msg
FROM plinth.audit_log
WHERE action = 'frontend.boundary.caught'
ORDER BY timestamp DESC
LIMIT 100;
```

The 0.6a-E admin-extension audit-log browser (ROADMAP line 162)
displays these rows with the same UX as any other audit family.

---

## §7 — Hand-off from ICD-0.6.2

What ICD-0.6.2 pinned that this ICD inherits unchanged:

- **Token system + theme + scaling.** The `tokens.css` artefact +
  pre-paint resolver + `documentElement.style.fontSize` mechanism
  carry forward verbatim. Panel modules consume tokens via
  `var(--name)` references in their own panel CSS; theme + scale
  apply uniformly to all panels (the cascade contract is
  unchanged).
- **`/api/frontend/tokens.css` indirection.** ICD-0.6.3 lands a
  parallel `/api/frontend/sdk.js` indirection, identical handler
  shape (302 + Cache-Control: no-cache + 503 diagnostic). The
  v0.6.3 code session adds the second route to
  `src/kernel/frontend/api_frontend.cpp`.
- **`ext_shell.user_preferences` substrate.** ICD-0.6.3 adds **no
  new preferences keys** — the shell does not persist any new
  per-user state for the panel SDK in 0.6.3. (Tab ordering, open
  float state, tray collection ordering all defer to 0.6.4 /
  0.6.5 / 0.6.6 alongside their respective runtimes.)
- **`shell.preferences.{get,set,get_all}` capabilities.** Panels
  consume these via `plinth.call('shell.preferences.get', 'foo.bar')`
  for any extension's preference-read needs. The capabilities are
  unchanged by ICD-0.6.3.
- **Top-level Preact `Boundary` component.** ICD-0.6.0 §7's
  scaffolded boundary at `client/shell/client/shell.js` — ICD-
  0.6.3 augments its `componentDidCatch` with the audit emission
  per §6 but does not change the React semantics (fallback UI,
  setState, error recovery).

What ICD-0.6.2 pinned that this ICD extends:

- **`api_frontend.cpp` handler.** Was a single-route handler in
  v0.6.2 (only `/api/frontend/tokens.css`). ICD-0.6.3 extends it
  to host the parallel `/api/frontend/sdk.js` route. Same
  active-frontend lookup; same 302/503 contract; the file gets a
  second register-route call in `register_api_frontend_routes`.
- **Asset-server byte path.** Already serves `/ext/{name}/{version}/
  client/(.*)` for the active frontend per ICD-0.6.1
  `register_active_frontend_routes`. ICD-0.6.3 adds new files to
  `client/sdk.js` + `client/panels/loader.js` to the bundled-
  shell artefact; the asset path serves them with no kernel
  change.

What this ICD pre-stages for 0.6.4+:

- **Panel-loader infrastructure.** The `panels/loader.js` module
  is the foundation 0.6.4 builds on for topbar-driven panel
  switching. 0.6.4's topbar reads `plinth.panels.list` (per
  ICD-0.6.4 OQ4 resolution) and calls `loader.LOAD(...)` for the
  user-selected panel. 0.6.3's loader handles the LOAD primitive;
  0.6.4 adds the topbar UI that drives it.
- **Panel-API factory.** The `makePanelApi` factory in §4.3 is
  forward-compat for 0.6.5's per-float instances. Each float gets
  its own `panelApi` object; the factory is reused.
- **Stub method signatures.** All 11 methods ship with their
  final 1.0 signatures. 0.6.4–0.6.6 swap the stub bodies for live
  implementations without changing signatures, return shapes, or
  error modes. This is the "API stable from 0.6.3 onward"
  contract per `DESIGN-shell-v06x.md §0.6.3` exit gate.
- **`audit.emit_boundary` capability + RBAC rule.** The single-
  purpose capability shape is forward-compat for 0.6.5's per-
  panel boundaries — they will all call the same capability with
  per-panel `panel_id` payloads.

### Phase ordering for the 0.6.3 code session

Per METHODOLOGY §Phase 2, the v0.6.3 code session lands the
contract in the following order. Each phase is a logical commit
boundary:

**Phase 1** — Kernel-side `POST /api/cap/{capability}` route +
filter chain (session, CSRF, dispatch, audit). This is the
substrate everything else needs to test against. C.* test cases
land here.

**Phase 2** — Kernel-side `GET /api/frontend/sdk.js` route +
the shell-side `client/sdk.js` artefact (containing
`call`/`subscribe`/`useData`/error classes/import-map registration).
B.* test cases land here.

**Phase 3** — Shell-side panel loader (`client/shell/client/
panels/loader.js`) + `makePanelApi` factory + import-map
declaration in `index.html`. L.* + R.* test cases land here.

**Phase 4** — `audit.emit_boundary` capability + RBAC rule +
boundary integration in `Boundary.componentDidCatch`. A.* test
cases land here.

**Phase 5** — Test-fixture extension at `tests/extensions/
sdk_demo/` exercising the contract end-to-end. I.* test cases
land here.

**Phase 6** — Docs + ROADMAP flip + CHANGELOG entry +
architecture/06-frontend.md §4.1 row addition for `/api/frontend/
sdk.js` (per Constraint #4 — same PR as the implementation).

---

## §8 — Configuration surface

### 8.1 No new `Config::Shell` fields

ICD-0.6.3 introduces no new kernel config fields. The
`Config::Shell` block stays as ICD-0.6.1 / 0.6.2 left it
(`enabled`, `root_redirect`, `bundle_path`). The panel SDK is
shell-internal JavaScript; the client SDK is shell-internal
JavaScript; the kernel HTTP routes are kernel-internal
registrations driven off the config that already exists.

### 8.2 No new realtime / extension config

The `plinth.subscribe` path uses the existing realtime broker
config (per ICD-0.5.x). No new keys.

### 8.3 No `cap-dispatch` rate-limit config

The `POST /api/cap/{capability}` route inherits the existing
ICD-0.5.0.3 dispatch rate limit. No new per-route rate limit in
0.6.3.

### 8.4 Migration from ICD-0.6.2 config

Existing `config.json` files require **no changes**. Operators
can upgrade from v0.6.2 to v0.6.3 with byte-identical config.

---

## §9 — Audit events

### 9.1 New audit family this milestone

| Action | Owner | Trigger | Detail keys |
|--------|-------|---------|-------------|
| `frontend.boundary.caught` | shell extension | Top-level Preact boundary's `componentDidCatch` | `panel_id`, `error_message`, `error_stack?`, `component_path?` |

The kernel-side write fills `user_id`, `session_id`,
`extension_id`, `node_id`, `timestamp` per
`audit_bindings.cpp:44-56`.

### 9.2 No new audit for capability dispatch

`POST /api/cap/{capability}` calls write the existing ICD-0.5.0.3
`capability.dispatched` audit — no 0.6.3-specific addition. The
HTTP-side dispatch is indistinguishable in the audit log from a
QuickJS-side `cap.call`.

### 9.3 No audit for panel lifecycle

`onActivate` / `onDeactivate` / panel mount / unmount fire **no**
audit events. Per the `feedback_no_audit_chatter.md` precedent
(implicit; matches ICD-0.6.1/0.6.2 posture) — lifecycle
transitions are too frequent to audit, and the existing
`shell.preferences.set` audit family captures the user-visible
state changes (panel-tab ordering, etc., via 0.6.4+ persistence).

### 9.4 No audit for SDK errors at boundary

If `plinth.call` itself fails (e.g. 401 / network error inside the
boundary's audit emission attempt), the kernel does not emit a
secondary audit. The console-fallback log is the only signal.
Adding a "we tried to audit but failed" audit would create
audit-storms in the offline case.

### 9.5 No audit for stub-method calls

When a panel calls a stub method (e.g. `plinth.panel.navigate(...)`)
and the stub throws `NotImplementedError`, that throw propagates
to the boundary, which **does** emit `frontend.boundary.caught`.
The audit captures the panel's misuse without needing a separate
"stub called" audit family.

---

## §10 — Security constraints

### SC1 — Panel sandbox boundary

`plinth.panel` is injected into the panel module's runtime scope
by the loader (per §4.3). The shell does **not** expose its
internals (the `shell` singleton, the panels-list, the active-
panel state machine) to the panel module. The panel sees only
the methods declared in §3.

Per `DESIGN-shell-v06x.md §10` constraint #2: "The panel SDK is
the only contract. Extensions never import shell internals. The
shell never reaches into panel component trees." This is convention-
plus-discipline (no runtime enforcement); v0.6.3 trusts panel
modules not to introspect via globals or DOM walking. A panel
that walks the DOM and reads shell-state divs is breaking the
contract; future kernel work (e.g. shadow DOM panel containers)
could harden this.

### SC2 — `audit.log` not exposed to browser; single-purpose `audit.emit_boundary`

Per §6.4, browser-side panel code does **not** have access to a
generic `audit.log("...", { ... })` API. The `audit.emit_boundary`
capability is single-purpose: it ignores the panel-supplied action
name and pins the literal `frontend.boundary.caught`. Panels
cannot forge:

- `user.login.success` audits (kernel-reserved-prefix table at
  `audit_bindings.cpp:44` already blocks `user.*` from any
  extension; redundantly, the single-purpose capability prevents
  even attempting it).
- `frontend.click.tracked` (or any other `frontend.*` family) —
  the capability's hard-coded action name allows only the one
  family.
- Arbitrary data-payload keys for `frontend.boundary.caught` —
  the handler only forwards the four documented keys; extra keys
  are ignored.

### SC3 — `plinth.call` capability gate is server-side

Browser-side `plinth.call(capability, args)` does **not** check
RBAC client-side. The kernel filter (`POST /api/cap/{capability}`)
runs the server-side RBAC check with the session's authenticated
user. Panel-side RBAC checks (e.g. "if user-can-write-prefs, show
the save button") are advisory only — the kernel is the
authoritative gate.

This means panels can attempt any capability; denied calls
return 403 + `rbac_denied`; panels handle the rejection
gracefully. There is no "client-side RBAC table" to keep in sync
with the server.

### SC4 — Subscribe channel-name validation

`plinth.subscribe(channel, handler)` does no client-side channel-
name validation. The kernel broker (per ICD-0.5.2 §SC6
classification) rejects invalid / unauthorised channels server-
side; the WS sends back a `{ "kind": "subscribe_error", "channel":
"...", "code": "rbac_denied" }` envelope which the SDK surfaces
as a `CapabilityError` rejection on the subscribe call.

### SC5 — Boundary-caught error sanitization

The `audit.emit_boundary` handler length-caps each detail field
(per §6.4 — `error_message` 1024 bytes, `error_stack` 8192 bytes,
`component_path` 8192 bytes). Panel code cannot exfiltrate large
volumes of data via boundary throws.

The handler does **not** redact PII — error messages may legitimately
include user-supplied strings (e.g. "Invalid email: jeff@..."). PII
in error messages is a panel-author concern; the shell does not
attempt to scrub. Per OQ6, production builds omit `error_stack`
to reduce minified-symbol exposure; dev builds include the full
stack.

### SC6 — CSP `connect-src 'self'`

The strict CSP `connect-src 'self'` (per ICD-0.6.0 §11) constrains
both `plinth.call` and `plinth.subscribe` to the same-origin
kernel HTTP/WS endpoints. Panels cannot reach external services
via the SDK; they must go through a kernel-side capability that
itself reaches out (per ICD-0.10.3 `http.*` API arc — out-of-
scope here).

### SC7 — Import-map cannot remap to attacker-controlled URLs

The import-map is rendered into `index.html` server-side (the
shell ships it as part of its static asset). Panel code cannot
mutate the import-map at runtime (per HTML spec). The bare
specifier `@plinth/frontend/sdk` always resolves to
`/api/frontend/sdk.js` — same-origin, kernel-controlled.

### SC8 — Test-fixture extension is not shipped to production

Per §1 + §14 OQ4 architect-pin: the test-fixture extension at
`tests/extensions/sdk_demo/` is NOT installed via the first-boot
bundled-shell lifecycle. Production data_dir contents are
identical between v0.6.2 and v0.6.3 except for the upgraded
shell.zip (gains `client/sdk.js` + `client/panels/loader.js`).

---

## §11 — RBAC rules

### 11.1 New rule introduced this milestone

| Rule | Owner | Default grant | Purpose |
|------|-------|---------------|---------|
| `frontend.boundary.audit_emit` | shell extension | `everyone` (default-grant) | Permits the shell-extension `audit.emit_boundary` capability dispatch |

### 11.2 Capability-rule binding

The `audit.emit_boundary` capability declares its rule binding in
`client/shell/capabilities.json`:

```json
{
  "provides": [
    {
      "name": "audit.emit_boundary",
      "version": 1,
      "handler": "server/handlers/audit.emit_boundary.js",
      "requires_rule": "frontend.boundary.audit_emit"
    }
  ]
}
```

### 11.3 No new rules for `plinth.subscribe` / `plinth.call`

`plinth.subscribe` channel access is gated by the per-channel
classification rules already in ICD-0.5.2 (e.g.
`pubsub.subscribe.shell.notifications`, etc.). 0.6.3 adds no new
subscribe-rules; whatever channels panels subscribe to inherit
the existing per-channel RBAC classification.

`plinth.call` capability access is gated by the per-capability
rule on the called capability. 0.6.3 adds no new call-rules
beyond `frontend.boundary.audit_emit`.

### 11.4 Default grants

Per ICD-0.6.1's `default_grants[]` infrastructure:

```json
// client/shell/rbac.json
{
  "rules": [
    { "rule": "frontend.boundary.audit_emit" }
  ],
  "default_grants": [
    { "rule": "frontend.boundary.audit_emit", "group": "everyone" }
  ]
}
```

The grant is idempotent on shell upgrade — the
`install_lifecycle::register_extension_rbac_rules` INSERT...SELECT
path (per ICD-0.6.1 §17 deviation #1) handles the no-op upgrade
case.

---

## §12 — Test cases

Test taxonomy follows the established ICD-0.6.0 / 0.6.1 / 0.6.2
prefix conventions. Total ~38 cases across nine categories.

The JS-dispatch suite (per ICD-0.6.1's deferred P.* / I.* and
ICD-0.6.2's deferred T.* / S.* / I.*) **continues to be blocked**
on the `init_registry` teardown bug from test-fixture-buildout
session 9 (`project_test_fixture_inflight.md`). The 0.6.3 code
session inherits the same posture: ship B.\*, run manual FE
smoke for the L.\* / U.\* / I.\* cases that exercise the full
browser-to-kernel path; defer real-bridge dispatch tests to the
0.6.1.N JS-dispatch follow-up.

### 12.1 `B.*` — bridge / boundary tests (4 cases, library-level via Catch2 + HttpTestFixture)

- **B.01** — `POST /api/cap/{capability}` happy path: dispatch
  `shell.preferences.get` with valid args; expect 200 +
  `{ ok: true, value: <expected> }`.
- **B.02** — `POST /api/cap/{capability}` RBAC denial: dispatch a
  capability the test-user lacks the rule for; expect 403 +
  `{ ok: false, error: { code: "rbac_denied", message: "..." } }`.
- **B.03** — `POST /api/cap/{capability}` not found: dispatch a
  capability that doesn't exist; expect 404 +
  `{ ok: false, error: { code: "not_found", message: "..." } }`.
- **B.04** — `GET /api/frontend/sdk.js` 302: with active-frontend
  installed, expect 302 + `Location: /ext/shell/{version}/client/
  sdk.js` + `Cache-Control: no-cache`. Replaces the
  bundled-shell row with synthetic name+version per ICD-0.6.2 §17
  deviation #4 to avoid full asset-server setup; asserts the
  URL-construction contribution.

### 12.2 `L.*` — lifecycle tests (6 cases, integration-level via JS-dispatch fixture)

- **L.01** — `onActivate` fires once on initial mount.
- **L.02** — `onActivate` fires again on reactivation after
  navigation-away (deferred — relies on 0.6.4 panel switching;
  scaffolded only in 0.6.3).
- **L.03** — `onDeactivate` fires before `onActivate` on the
  next panel during a navigation transition (similar deferral).
- **L.04** — `setDirty(true)` followed by `setDirty(false)`:
  dirty bit transitions are observed by the test seam.
- **L.05** — `setDirty('not a boolean')` throws `TypeError`.
- **L.06** — `getContext()` returns the context object passed to
  the loader at mount time; default `{}` if none passed.

L.02 / L.03 depend on the 0.6.4 panel-switching runtime; in
0.6.3 they assert "registers without throwing" only. The full
multi-panel reactivation tests defer to ICD-0.6.4.

### 12.3 `C.*` — `plinth.call` round-trip (6 cases, integration-level)

- **C.01** — Successful call resolves with the capability return
  value.
- **C.02** — RBAC-denied call rejects with `CapabilityError` +
  `code: "rbac_denied"`.
- **C.03** — Capability-not-found call rejects with
  `CapabilityError` + `code: "not_found"`.
- **C.04** — Network failure rejects with `NetworkError`.
- **C.05** — Capability-handler throw rejects with
  `CapabilityError` + `code: "quickjs_throw"` + propagated
  message.
- **C.06** — Bad request body (missing `args` field) → 400 →
  `CapabilityError` + `code: "bad_request"`.

### 12.4 `S.*` — `plinth.subscribe` round-trip (5 cases, integration-level)

- **S.01** — Subscribe + receive: panel subscribes to a channel;
  kernel publishes; envelope arrives at handler.
- **S.02** — Unsubscribe: returned function deregisters; further
  publishes do not fire the handler.
- **S.03** — Multiplex: N panels subscribe to the same channel;
  one publish → all N handlers fire.
- **S.04** — Handler-error isolation: handler A throws on
  envelope; handlers B, C still fire for the same envelope.
- **S.05** — RBAC-denied channel: subscribe rejects with
  `CapabilityError` + `code: "rbac_denied"`.

### 12.5 `U.*` — `plinth.useData` Preact hook (4 cases, browser-harness gate; deferred if harness absent)

- **U.01** — Initial loading state: `loading: true`, `data:
  initialData`, `error: null`. Snapshot resolves → state updates
  to `loading: false, data: <snapshot value>, error: null`.
- **U.02** — Snapshot rejection: `loading: false, data:
  initialData, error: <CapabilityError>`. (Stale-on-error: the
  initialData persists.)
- **U.03** — Live update: subscribe envelope arrives → state
  updates `data: <envelope>`.
- **U.04** — Unmount cleanup: panel unmounts → SDK-side
  unsubscribe fires; further publishes do not trigger re-renders.

### 12.6 `A.*` — audit family tests (2 cases, library-level)

- **A.01** — `frontend.boundary.caught` row written: simulate a
  panel throw → boundary calls `audit.emit_boundary` → audit row
  written with the four detail keys + non-forgeable identity
  payload.
- **A.02** — Action-name forgery rejected: a hand-crafted call
  to `audit.emit_boundary` with an action-name override (per
  malicious panel attempt) results in the literal
  `frontend.boundary.caught` action — the handler ignores the
  override.

### 12.7 `K.*` — `registerShortcut` keyboard handling (3 cases, manual FE smoke)

- **K.01** — Shortcut registration + dispatch: `Ctrl+S` registers;
  pressing the combo invokes the callback.
- **K.02** — Same-panel duplicate registration throws
  `ShortcutConflictError`.
- **K.03** — Shortcut combo normalisation: `"Shift+Ctrl+S"` and
  `"Ctrl+Shift+S"` register the same key → second registration
  throws.

### 12.8 `R.*` — forward-compat regression tests for stub methods (6 cases, library-level)

- **R.01** — `navigate(...)` throws `NotImplementedError` with
  exact message string.
- **R.02** — `openFloat(...)` returns rejected Promise with
  exact message.
- **R.03** — `onNavigationIntent(callback)` registers without
  throwing; callback never fires.
- **R.04** — `requestFocus()` throws with exact message.
- **R.05** — `setTrayState(...)` throws with exact message.
- **R.06** — `setTrayBadge(...)` throws with exact message.

### 12.9 `I.*` — full integration via test-fixture extension (3 cases, full-stack)

- **I.01** — Test-fixture extension loads, `onActivate` fires,
  panel renders something visible. Per `DESIGN-shell-v06x.md
  §0.6.3` exit gate ("test extension loads, receives lifecycle
  events").
- **I.02** — Test-fixture panel makes a `plinth.call` to a
  shell-provided capability and receives the result. Per exit
  gate ("makes capability calls").
- **I.03** — Test-fixture panel `subscribe`s to a kernel-emitted
  channel and receives an envelope. Per exit gate ("subscribes
  to realtime events").

### 12.10 Test counts

Total cases by category:

| Category | Cases | Fixture | Status |
|----------|-------|---------|--------|
| B.* | 4 | Catch2 + HttpTestFixture | Library-level — ships in 0.6.3 |
| L.* | 6 | Client/panel fixture | Deferred to owning client/panel harness |
| C.* | 6 | Client HTTP fixture | Deferred to client harness |
| S.* | 5 | Client WebSocket fixture | Deferred to client harness |
| U.* | 4 | Browser harness | Deferred to harness ship; manual FE smoke covers |
| A.* | 2 | Catch2 + bundled QuickJS dispatch | Automated 2026-09-03 |
| K.* | 3 | Manual FE smoke | Manual gate per `feedback_fe_visualize.md` |
| R.* | 6 | Client SDK fixture | Existing source assertions only; runtime cases deferred |
| I.* | 3 | Test-fixture extension | Manual FE smoke this milestone; full automation deferred |
| **Total** | **39** | — | **B.* and A.* automated; client/browser families remain deferred** |

Per the v0.6.1 / v0.6.2 precedent, the deferred cases land at
the 0.6.1.N JS-dispatch follow-up (whose blocker is the
`init_registry` teardown bug at `project_test_fixture_inflight.md`
session 9).

**Update 2026-04-30 (0.6.3.N):** The `init_registry` teardown
blocker is closed. `tests/kernel/ws/ws_test_fixture.cpp` now
mirrors production `main.cpp`'s init pair + atexit teardown
(`init_resolver` + `init_registry` + symmetric `shutdown_registry`
+ `stop_notify_listener`); the [ws] suite passes 81 cases / 953
assertions cleanly post-fix. The 33 deferred test cases (L.\*/C.\*/
S.\*/U.\*/I.\*/A.\* here + v0.6.1 P.\*/I.\* + v0.6.2 T.\*/S.\*/I.\*)
are unblocked for the renamed `0.6.3.N JS-dispatch test suite
backfill` milestone (per ROADMAP §0.6).

**Update 2026-09-03:** A.01-A.02 now execute through the real bundled
`shell.audit.emit` QuickJS handler and assert the literal action, sanitized
detail, caller-attributed audit row, and action-override resistance. The
underlying `AUDIT_WRITE` operation now snapshots user/session/IP identity at
enqueue time before detached execution. L.\*/C.\*/S.\*/U.\*/I.\* remain
client/browser/full-stack work; they are listed by family in
`docs/DEFERRED.md`. The earlier "33" aggregate mixed distinct fixtures and
was not a valid executable-suite count.

---

## §13 — Entry / Exit Criteria

### Entry

- v0.6.2 shipped (tag `v0.6.2`; commit `6b1643e`).
- Capability registry operational (ICD-0.5.0.3 + 0.5.0.x arc;
  shipped through v0.5.5.2).
- Realtime broker operational (ICD-0.5.2; LH-2 stress validated
  2026-04-24).
- Shell first-boot install lifecycle operational (ICD-0.6.1).
- Active-frontend resolver operational (ICD-0.6.1).
- `/api/frontend/tokens.css` indirection operational (ICD-0.6.2).

### Exit

Per `DESIGN-shell-v06x.md §0.6.3`:

- A test extension loads.
- The extension receives lifecycle events (`onActivate`,
  `onDeactivate`).
- The extension makes capability calls (`plinth.call`).
- The extension subscribes to realtime events
  (`plinth.subscribe`).

Plus the ICD-0.6.3-specific gates:

- `POST /api/cap/{capability}` route operational (B.* tests
  green).
- `GET /api/frontend/sdk.js` indirection operational (B.04
  green).
- `frontend.boundary.caught` audit family writes correctly
  (A.* tests green).
- All 6 stub methods throw the documented `NotImplementedError`
  with exact message strings (R.* tests green).
- `audit.emit_boundary` capability + RBAC rule installed via
  shell-extension upgrade lifecycle (no manual SQL; the
  install_lifecycle path handles it).
- Architecture promotion: `architecture/06-frontend.md §4.1`
  gains a `/api/frontend/sdk.js` row marked ✓ implemented v0.6.3.
- ICD-0.6.0 §15 panel-SDK deferral marked discharged.
- ICD-0.6.0 §10 boundary-audit deferral marked promoted.

### Smoke acceptance gate (manual FE)

The architect's manual FE smoke must verify:

1. Test-fixture extension visually appears with the expected
   panel contents.
2. Lifecycle callbacks fire (a console.log inside `onActivate`
   shows up in browser devtools).
3. A `plinth.call` round-trip works (the panel's onActivate
   invokes call and renders the result).
4. A `plinth.subscribe` round-trip works (the panel's onActivate
   subscribes; a kernel-side `pubsub.publish` (manually
   triggered) routes to the panel's handler).
5. A deliberately-throwing panel triggers the boundary fallback
   UI + an audit row visible via `SELECT FROM plinth.audit_log
   WHERE action='frontend.boundary.caught'`.

If any of the above fails, the milestone is not shipped.

---

## §14 — Open Questions

Each OQ carries an architect-recommendation. Code-session pin
sequence per ICD-0.5.5 §17 / ICD-0.6.0 §17 / ICD-0.6.1 §17 /
ICD-0.6.2 §17 precedent — a §17 amendment block lands in the
v0.6.3 code-session ship PR with the architect-confirmed
resolutions.

**OQ1 — Stub-method failure mode: throw `NotImplementedError`
vs. silent no-op vs. log-and-continue.** §3 declares all 6 stub
methods throw synchronously (or reject for async). An
alternative is silent no-op (the call returns `undefined` and
the panel keeps running). Another is log-warning (console.warn
+ continue). **Recommendation:** throw / reject. Rationale:
(a) silent no-op makes 0.6.4–0.6.6 fragile — a bug where 0.6.6
forgets to wire `setTrayState` would silently fail in production
without a regression test catching it; (b) throw + boundary +
audit gives 0.6.4–0.6.6 a clear "wire me up" signal in the
audit log; (c) panel authors get an immediate failure they can
react to (try/catch around the call) rather than a silently-
broken UI. Architect: confirm or redirect.

**OQ2 — Panel sandbox enforcement: convention-only vs. shadow
DOM container vs. iframe.** §10 SC1 declares the panel sandbox
boundary is "convention-plus-discipline" — no runtime
enforcement. Alternatives: (a) wrap each panel in a shadow DOM
root (CSS isolation; no DOM-walk to shell internals); (b) load
each panel in an iframe (full process isolation; cross-origin
posture). **Recommendation:** convention-only. Rationale:
(a) shadow DOM costs design-token continuity (cascade does not
cross shadow boundaries; tokens.css would need explicit
injection per shadow root, doubling the asset path); (b) iframe
costs same-page communication (postMessage replaces direct
calls, bloating SDK shape); (c) panel authors are first-party
extension developers, not adversarial — convention is the
right level. The hardening could land later if the trust model
changes (e.g. third-party-marketplace extension delivery in
1.0+). Architect: confirm or redirect.

**OQ3 — `plinth.call` cancellation: defer vs. ship with
AbortSignal.** §5.2 ships without cancellation. An alternative
is to add `AbortSignal` support now via an opts-bag overload:
`call(capability, args, { signal: ... })`. **Recommendation:**
defer. Rationale: (a) no current capability is long-running
enough to need cancellation (typical dispatch < 10ms); (b)
adding the opt-bag overload is API churn; the bareword
positional-args shape is ergonomic for the common case;
(c) when streaming / long-running capabilities arrive (e.g.
LLM-completions, file-uploads) the API extension is naturally
the streaming-response shape, not abort. Architect: confirm
or redirect.

**OQ4 — Test-fixture extension shape: bundled vs. test-only vs.
inline.** Architect-pinned at the 2026-04-29 plan-mode
interaction (per AskUserQuestion answer): **test-only fixture
at `tests/extensions/sdk_demo/`**. Production users see no demo
panel; v0.6.3 ship gates manual FE smoke with the test-fixture
load via test seam. The locked decision is recorded here for
reference; no further re-litigation. (Architect-pinned; not a
live OQ.)

**OQ5 — `plinth.subscribe` reconnection feedback: silent
exponential backoff vs. surface to handler.** §5.3 ships silent
exponential backoff with cap (1s → 30s). An alternative is to
surface connection-state changes to the handler via a sidecar
event (e.g. `handler({ kind: 'connection_lost' })` between
disconnect and reconnect). **Recommendation:** silent. Rationale:
(a) the realtime broker's own `superseded_seqs[]` field (deferred
to 0.6.x source-seq tracking) will be the canonical mechanism
for "you missed events while disconnected"; (b) connection-state
notifications duplicate useEffect-cleanup-and-resubscribe in
panel code; (c) the typical reconnect is sub-second and
transparent. Architect: confirm or redirect.

**OQ6 — `frontend.boundary.caught` `error_stack` field:
production-omit vs. always-include vs. dev-only-include.** §6.2
recommends production-omit (NODE_ENV check). Alternatives:
(a) always include (full stack everywhere; minified-symbol
exposure if production bundles are minified); (b) dev-only
include (stack is omitted in production; boolean flag in the
detail body says whether stack was redacted). **Recommendation:**
production-omit (current §6.2 default). Rationale:
(a) production minified bundles obfuscate symbols already, but
including stack invites accidental info leak (e.g. parameter
names from non-minified vendor chunks); (b) admin-side debugging
of production issues would normally trigger a dev-build
deployment with stacks enabled; (c) the audit row's
`component_path` is sufficient for triage in most cases.
Architect: confirm or redirect. If pinned to (a), v0.6.3 code
session removes the NODE_ENV check.

**OQ7 — `registerShortcut` conflict resolution: first-wins vs.
last-wins vs. priority-explicit.** §3.5 ships first-wins with
explicit throw. Alternatives: (a) last-wins silently overwrites
(matches DOM listener semantics); (b) panel declares priority
in `registerShortcut(combo, callback, { priority: 10 })`.
**Recommendation:** first-wins + throw. Rationale: (a) silent
last-wins makes shortcut conflicts hard to debug; (b) priority
adds API complexity for an edge case; (c) the typical
panel-author writes ~3-5 shortcuts; conflicts within a panel
are author errors (the panel ought not register the same combo
twice). Architect: confirm or redirect.

---

## §15 — What Must Not Be Decided Yet

These items are explicitly out of scope for ICD-0.6.3. Each
names the milestone (or trigger) that closes the deferral. Per
`feedback_icd_horizon.md`, ICDs author one milestone ahead and
pre-deciding 0.6.4+ contracts based on 0.6.3 state would
violate that discipline.

### Kernel-side `plinth.panels` table + query API (`plinth.panels.list`)

`DESIGN-shell-v06x.md §11 OQ4` flags this as "ICD-level,
resolved during 0.6.x implementation." The kernel needs an
endpoint for "what panels should I render for this user" —
shape (REST endpoint vs. capability call), RBAC filtering,
response format. **Closes: ICD-0.6.4 paper authoring (ROADMAP
line 144)** per `feedback_icd_horizon.md` one-milestone-ahead
discipline. ICD-0.6.3 explicitly declines to pre-resolve OQ4.

### Topbar dynamic panel rendering + tabs / sub-tabs / app-switcher / Home launcher

`DESIGN-shell-v06x.md §0.6.4` lays out the topbar runtime: the
topbar reads `plinth.panels.list` and renders tabs / app-switcher
chevron / sub-tabs / Home launcher icon grid. **Closes: 0.6.4**
code milestone. 0.6.3's topbar remains the empty four-zone
ICD-0.6.0 frame.

### Float lifecycle + chrome + per-float boundaries

Float chrome (title bar, minimise/maximise/close, jump-to-app),
lifecycle (spawn, minimise, maximise, close), responsive
transforms (desktop window vs. tablet slide-over vs. mobile
modal), state persistence across reload, max-float limit with
oldest-minimised behaviour, per-panel error boundaries. **Closes:
0.6.5** code milestone. 0.6.3's `plinth.panel.openFloat` ships as
a stub method.

### Tray runtime: icon rendering, popover lifecycle, badge/state API, tray collection ordering

Tray-panel runtime (icon → popover → state-driven icon variants
→ badge). Shell-owned bell + avatar converted to dogfooded tray
panels. `chrome_essential` fallback rendering. Tray collection
ordering and position persistence to `ext_shell.user_preferences`.
**Closes: 0.6.6** code milestone. 0.6.3's `setTrayState` /
`setTrayBadge` ship as stub methods.

### Content-type handler resolution + navigation intents

Per-extension content-type registration (e.g. Notes registers as
`.md` handler), three-tier priority resolution
(user-preference → admin-default → first-installed),
`ext_shell.default_apps` storage, one-time chooser UI for
ambiguous resolution. Navigation intent dispatch system:
`navigate()` routes via content-type resolution; `openFloat()`
spawns a float per content-type; `onNavigationIntent()`
delivers intents to the target panel. **Closes: 0.6.6** code
milestone. 0.6.3's `navigate` / `onNavigationIntent` ship as
stub / stub-receiver.

### Per-panel error boundaries (vs. top-level boundary only)

Per `DESIGN-shell-v06x.md §3.6` line 345 — "every panel … is
wrapped in a Preact error boundary." 0.6.3 ships only the
top-level boundary (ICD-0.6.0 §7); a panel throw triggers the
whole-shell fallback UI. Per-panel boundaries (a panel throw
shows a panel-local fallback while other panels remain alive)
arrive **0.6.5** with float chrome (the natural locus for per-
container boundaries).

### Cross-cutting composition (`surface_traits` / `slots`)

Per `architecture/05-extensions.md §3.4` — extension panels
declare `surface_traits[]` (e.g. "text-editor", "rich-text") and
`slots: { toolbar: {}, status-bar: {} }` (extension-injectable
named regions). The shell loader's panel-mount path is designed
to accommodate these (per ICD-0.6.0 §10 constraint #10) but the
**runtime semantics** of trait-based surface events and slot
injection defer to the cross-cutting-composition arc at Scale 2.
**Closes: 0.9.x** ICD authoring slot (TBD). 0.6.3 stores
the JSON fields verbatim; the loader does not interpret them.

### `plinth.call` AbortSignal / cancellation

Per OQ3 default — defer until use-cases pressure. **Closes: TBD**
(likely a streaming-capability ICD post-0.7).

### `plinth.subscribe` connection-state handler notifications

Per OQ5 default — deferred. **Closes: TBD** (likely paired with
the realtime source-seq tracking arc — ROADMAP line 141).

### `superseded_seqs[]` in subscribe envelopes

Per ICD-0.5.5 W.06 — three options on the table (plumb-through /
peer-listener / discard-field). 0.6.3's `plinth.subscribe`
envelope shape inherits whatever the realtime broker emits today;
the future ICD's resolution applies transparently. **Closes:
ICD-0.6.x source-seq tracking authoring (ROADMAP line 141)**.

### `audit.log` direct exposure to browser

Per §10 SC2 — single-purpose `audit.emit_boundary` capability
prevents arbitrary action-name forgery. Direct browser-side
`audit.log` access (allowing panels to emit any audit family)
remains out-of-scope; the kernel-reserved-prefix table at
`audit_bindings.cpp:44` already blocks the dangerous families,
but the discipline is to expose audit emission only via single-
purpose capabilities. **Closes: TBD** (probably never).

### Sidecar / native panel modules

`handler_mode` resolution for capability dispatch — quickjs vs.
sidecar vs. bundled_native. The ratified `architecture/05-
extensions.md §6` extension HTTP surface introduces the
`handler_mode` manifest field; its dispatch wiring lands in the
0.6.7 milestone (ICD authoring at ROADMAP line 150). 0.6.3's
panel-loader and `plinth.call` assume QuickJS handlers
exclusively. **Closes: 0.6.7** code milestone for capability-
dispatch handler-mode; panel-modules-as-non-JS likely remain
forever-deferred (panels are inherently JS).

### `/api/frontend/manifest.json` and `/api/frontend/fonts/{name}` and `/api/frontend/icons/{name}`

Per ICD-0.6.2 §15 — same deferrals carry forward. The shell
ships zero shared font / icon assets across extensions in 0.6.3;
the four `/api/frontend/*` endpoints other than `tokens.css` and
the new `sdk.js` remain `(deferred)` per
`architecture/06-frontend.md §4.1` table. **Closes: TBD**
(consumer-driven — when an extension panel needs them).

### Multi-tenant per-tenant panel scoping

Per ICD-0.6.2 §15 — same deferral. The 0.6.3 panel SDK assumes
single-tenant; per-tenant panel scoping (e.g. tenant A sees
panels {X, Y}; tenant B sees panels {Y, Z}) defers to the
multi-tenant arc. **Closes: TBD** (no roadmap slot yet).

---

## §17 — OQ Resolutions (post-ship amendment, populated at v0.6.3 ship)

This section is populated in the v0.6.3 code-session ship PR
with the architect-confirmed resolutions of the §14 OQs. At
paper time it is empty; the v0.6.3 code session pins each OQ
at session start, implements, then amends this section with
the resolution table + any implementation deviations per
METHODOLOGY §Phase 2 Constraint #4 + design-bundle / arch-doc
amendments per Constraint #4.

### OQ pin table

Architect confirmed all six live OQs at session start per the
§14 architect-recommendation defaults; OQ4 was already pinned at
paper time.

| # | OQ | Resolution |
|---|----|------------|
| OQ1 | Stub-method failure mode | throw `NotImplementedError` (sync) / reject Promise (async) — exact-string messages frozen in `panel_api.js` |
| OQ2 | Panel sandbox enforcement | convention-only (no shadow DOM, no iframe) |
| OQ3 | `plinth.call` cancellation | defer (no AbortSignal in 0.6.3) |
| OQ4 | Test-fixture shape | architect-pinned at paper session — test-only fixture at `tests/extensions/sdk-demo/`. Production data_dir unchanged from v0.6.2 except `shell.zip` upgrade. (Naming note: dashes per validator regex; see deviation #5.) |
| OQ5 | `plinth.subscribe` reconnect feedback | silent exponential backoff with 1s → 30s cap; not surfaced to handler |
| OQ6 | `error_stack` redaction | production-omit; gated on `window.__PLINTH_PRODUCTION__` (see deviation #8) |
| OQ7 | `registerShortcut` conflict resolution | first-wins + throw `ShortcutConflictError` after combo normalisation |

### Implementation deviations from §3–§13

1. **`RbacFilter` not in `/api/cap/{capability}` filter chain.**
   §5.1 / Up-Front Ask #1. The route attaches only `SessionFilter`;
   the resolver enforces RBAC step 3 internally per
   ICD-0.2.4 (`resolution.cpp:282-291`). Wiring `RbacFilter` would
   require per-capability rule registration — the static per-route
   rule table at `register_rule_requirement` doesn't fit the
   per-capability lookup pattern. Resolver-internal RBAC has been
   the contract since 0.2.4; the HTTP route just rides it.

2. **CSRF deferred.** §5.1 specifies a double-submit cookie
   pattern; Plinth has no CSRF infrastructure today (no
   `plinth_csrf` cookie issuance, no `X-Plinth-CSRF` validation in
   `src/kernel/auth/`). Deferred to a follow-up that lands CSRF
   cohesively across all `/api/*` mutating routes.

3. **URL → resolver signature synthesis.** §5.1 examples show
   `/api/cap/shell.preferences.set` (bare dotted name); the
   resolver expects the full triple `<namespace>:1:<function>`.
   The handler at `src/kernel/cap/api_cap.cpp:split_capability`
   parses the URL parameter into `(namespace, function)` by
   splitting on the first dot, then synthesises
   `<namespace>:1:<function>` (default version 1). Multi-version
   capabilities don't exist yet; future expansion is a query
   parameter or path-versioning.

4. **`UserContext::effective_rules` full-DB expansion.** Up-Front
   Ask #2 / §5.1. `src/kernel/cap/api_cap.cpp:load_effective_rules`
   runs the same `plinth.group_rules` JOIN
   `RbacFilter::doFilter` runs (`enforcement.cpp:261-264`). The
   WS path's admin-only shortcut at `call_dispatch.cpp:47-61`
   carries forward unchanged — adopting the full expansion there
   is a separate follow-up.

5. **Capability name `shell.audit.emit` (not `audit.emit_boundary`
   per §A.4).** §6 / §11. Three validator constraints jointly
   force the redesign:
   - **CF7** (`cross_file_validator.cpp:307-330`) requires every
     capability's namespace to equal `manifest.name`. The shell
     extension's manifest is `name=shell`, so all caps must be
     `shell.*` — the ICD-suggested `frontend` (or `audit`)
     namespace is unusable.
   - **`is_valid_rule_name`** (`rule_validator.cpp:39-65`) enforces
     `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$` — no underscores in
     segments. The ICD-§11.1 rule name
     `frontend.boundary.audit_emit` fails this regex.
   - **`validate_capability` namespace-rule match**
     (`validation.cpp:155-161`) requires `rbac_rule` to start with
     `<namespace>.`. Combined with CF7, this means the rule must
     begin with `shell.`.
   The combined resolution: cap = `shell.audit.emit`, function =
   `audit.emit`. Handler file at
   `client/shell/server/handlers/audit.emit.js`. Browser SDK
   callsite in `shell.js`'s `Boundary.componentDidCatch` updated
   accordingly.

6. **RBAC rule `shell.audit.emit` (not `frontend.boundary.audit_emit`
   per §11.1).** Same set of constraints as deviation #5.
   Default-grant to `everyone` group carries forward unchanged via
   the v0.6.1 `default_grants[]` infrastructure (idempotent on
   shell upgrade per ICD-0.6.1 §17 deviation #1).

7. **Audit action `ext.shell.frontend.boundary.caught` (not
   `frontend.boundary.caught` per §9.1).** §6.1 asserts that
   `frontend.*` is permissible per `audit_bindings.cpp:44-56`.
   That's only half-correct — the kernel-reserved-prefix table at
   `audit_bindings.cpp:46-47` does NOT list `frontend.*`, but the
   binding additionally rejects any extension audit whose
   `event_type` doesn't start with `ext.` (lines 168-173: `audit.invalid_prefix`).
   Resolution: prefix the action with `ext.shell.` to satisfy the
   binding while preserving the original taxonomy semantics.
   The audit action name is the only consumer-visible difference;
   downstream filters (admin dashboards, log aggregation) already
   key by the `ext.<name>.` shape.

8. **`process.env.NODE_ENV` → `window.__PLINTH_PRODUCTION__`.**
   §6.5 / Up-Front Ask #3 / OQ6. Browsers have no `process.env`
   global (that's a Node.js convention). The shell injects
   `<script>window.__PLINTH_PRODUCTION__ = false;</script>` after
   the importmap and before any module script in `index.html`.
   v0.6.3 ships `false` (full stacks in dev); a future build-step
   or kernel-injected variable flips this to `true` for production
   deploys. Boundary's `componentDidCatch` reads the global to
   decide whether to include `error_stack` in the audit detail.

9. **Test-fixture name `sdk-demo` (not `sdk_demo` per §D.1).**
   Manifest `name` regex `^[a-z][a-z0-9-]{1,63}$` rejects
   underscores; switched to dash. The fixture's loader callsite
   uses `loadPanel('sdk-demo', '0.1.0', 'demo', ...)` accordingly.

10. **Test-fixture `frontend` block omitted entirely.** §D.2
    specified `frontend: { mount: null, entry: null }` for the
    test-only fixture. v0.6.1's `parse_manifest` only accepts a
    `frontend` block with non-null mount + entry strings; nullable
    shape isn't supported. Omitting `frontend` validates cleanly
    and matches the "primary panel only" use case — the fixture's
    `panels.json` is its only shell-visible surface.

11. **panels.json field `client_path` (not `component` per §A.5).**
    The v0.4.4 `panels_manifest_test.cpp` parser uses
    `client_path` (`panels_manifest.cpp:23`); ICD-0.6.3 §A.5
    showed `component`. Both name the same path — the panel
    module file relative to the extension's `client/panels/`
    directory. The shell loader (`loader.js`) reads
    `panel.client_path` and synthesises the URL
    `/ext/{name}/{version}/client/panels/{client_path}` per the
    parser convention.

12. **Importmap entries beyond `@plinth/frontend/{sdk,tokens}`.**
    §A.6 showed only the two `@plinth/frontend/*` entries. The
    shipping importmap also declares `preact`, `preact/hooks`,
    `htm` so panel modules can use bare specifiers without
    hardcoding the shell version segment in their import URLs.
    `preact-hooks` was vendored same-origin alongside `preact` for
    the v0.6.3 `useData` hook; CSP-clean (no `eval(`/`new Function(`).

13. **A.* tests deferred (was scheduled to ship per §12.10).**
    A.01 + A.02 require extension QuickJS dispatch to invoke the
    `shell.audit.emit` JS handler. That path is gated on the
    `init_registry` teardown bug from test-fixture-buildout
    session 9 (`project_test_fixture_inflight.md`) — same blocker
    as the L.*/C.*/S.* cases. Manual FE smoke via the `sdk-demo`
    fixture's deliberate-throw button is the ship-acceptance gate
    for the audit family in v0.6.3; A.* full-stack automation
    folds into the 0.6.1.N JS-dispatch follow-up.

    **Update 2026-09-03:** Resolved. A.01 and A.02 are automated in
    `tests/kernel/shell/preferences_dispatch_test.cpp` through resolver/RBAC,
    the installed handler, and the audit table. Browser-trigger simulation is
    still represented by the explicit capability payload; general browser
    harness work remains separate.

14. **`plinth.call(capability, args)` is single-arg, not rest.**
    §A.2 specified `call<T>(capability: string, ...args: unknown[])
    : Promise<T>` — rest spread. The kernel binding
    `cap.call(signature, args?)` (`cap_bindings.cpp:92-159`) takes
    only one args value and forwards it verbatim to the handler's
    first positional argument. The SDK matches the kernel: pass an
    object for handlers that destructure `({key, value})`, a
    primitive for handlers that take a single positional, or
    `undefined` for parameterless caps. POST body shape:
    `{"args": <single value>}` (not `{"args": [<arg>, <arg>...]}`).

15. **`/api/frontend/sdk.js` redirect target is
    `/ext/{name}/{version}/sdk.js` (not `/ext/{name}/{version}/client/
    sdk.js` per §5.2).** The asset server at
    `src/kernel/packages/asset_server.cpp:287-296` registers
    `client_root = <data_dir>/extensions/<name>/<version>/client/`
    and serves files RELATIVE to that root — the `client/` segment is
    absorbed by the route registration. Equivalent for the
    `/api/frontend/tokens.css` v0.6.2 redirect path which was
    correct because tokens.css lives under `client/css/` in the
    bundle but is reached via `/ext/{name}/{version}/css/tokens.css`
    (no `client/` in the URL).

16. **`panels.json` fetch deferred from `loadPanel` happy path.**
    §4.2 step 1 specified the loader fetches
    `panels.json` from the kernel. The asset server's `/ext/{name}/
    {version}/(.*)` regex only serves files under `client/`; the
    `panels.json` file lives at the package root. The kernel-side
    `/api/frontend/manifest.json` endpoint that would expose
    package-root metadata is `(deferred)` per
    `architecture/06-frontend.md §4.1`. Resolution: the v0.6.3
    `loadPanel` accepts `opts.panel` as a fallback so callers can
    pass the panel struct directly (manual FE smoke uses this);
    the panels.json fetch path remains in the loader as forward-
    compat scaffolding for 0.6.4 once the panels-query API ships.

17. **Boundary-emit immutable cache caveat (FE-smoke note).** The
    asset server sets `Cache-Control: public, max-age=31536000,
    immutable` on `shell.js`. After a shell upgrade, browsers must
    reload to pick up new boundary code — within a single browser
    session, the cached pre-upgrade `shell.js` is what runs. v0.6.3
    manual FE smoke verified the boundary→audit chain via direct
    `POST /api/cap/shell.audit.emit` (curl + browser fetch); the
    in-page boundary trigger after a live edit re-uses cached
    bytes. Production behavior is correct because shell upgrades
    bump the version segment in `/ext/shell/{version}/sdk.js`,
    invalidating the cache. No remedial action.

### Design-bundle / architecture amendments (per Constraint #4)

Per METHODOLOGY §Phase 2 Constraint #4, the v0.6.3 code PR
amends:

- **`docs/architecture/06-frontend.md §4.1` Endpoint Table** —
  `/api/frontend/sdk.js` row added with status `✓ implemented v0.6.3`
  alongside the v0.6.2 `tokens.css` row.
- **`docs/architecture/06-frontend.md §4.3` Import-Map Binding** —
  status flipped from forward-cite to "published as of v0.6.3";
  importmap entries for `preact` / `preact/hooks` / `htm` noted
  per deviation #12.
- **`docs/architecture/06-frontend.md §5` Panel System (Summary)** —
  status-flipped from forward-cite to operational reference.
- **`docs/design/DESIGN-shell-v06x.md §0.6.3`** — status footer
  with v0.6.3 ship date and squash-merge SHA (filled at architect
  merge).
- **`docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md §15`** —
  *Discharged by ICD-0.6.3 (panel SDK + client SDK)* line added.
- **`docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md §10`** —
  *Promoted by ICD-0.6.3 to kernel-side audit family* line added.

No design-bundle (`docs/sketches/shell-design-2026-04-27/`)
amendments — the bundle's JSX prototypes already match the SDK's
event-injection shape.

### Follow-ups carried forward

1. **CSRF infrastructure** — defer #2 above. Lands cohesively
   across all `/api/*` mutating routes. Likely scheduled as
   `0.6.3.N` or absorbed into a future 0.6.x security-pass
   milestone.

2. **WS path effective-rules unification** — adopt the
   `api_cap.cpp:load_effective_rules` pattern in
   `ws/call_dispatch.cpp:47-61`. Simple change; carry to the
   next 0.5.x.N or 0.6.x.N follow-up.

3. **Production `__PLINTH_PRODUCTION__` flip** — v0.6.3 ships
   `false`. A future build-step or kernel-injection (e.g. via the
   bundled-shell first-boot path reading a config flag) flips
   this to `true` for production deploys. Defer to whichever
   milestone introduces the first build-step convention or the
   kernel-side production-flag plumbing.

4. **Multi-version capability dispatch** — current handler
   defaults `version=1`. Multi-version capabilities arrive when a
   real use case pressures the contract (likely 0.7+).

5. **`audit.log()` `ext.` prefix relaxation** — deviation #7's
   action-name redesign was forced by the binding's `ext.` rule.
   If a future ICD wants to lift the `ext.` rule for shell- or
   trusted-extension-owned audits, the rule lives at
   `audit_bindings.cpp:168-173`.

6. **L.\* + C.\* + S.\* + U.\* + I.\* client/browser tests** — A.\* landed
   in the 2026-09-03 server/kernel backfill and the `init_registry` blocker is
   closed. The remaining families need client, WebSocket, Preact, panel
   switching, or full-stack browser fixtures and remain tracked in
   `docs/DEFERRED.md`.

7. **Per-panel boundaries** — 0.6.3 ships only the top-level
   boundary; per-panel boundaries arrive 0.6.5 alongside float
   chrome per §4.5.

8. **Topbar dynamic rendering** — 0.6.3's loader is callable
   from devtools but no UI surface drives it. 0.6.4 wires the
   topbar.

---

## Appendix A — Authoritative `plinth.panel.*` + client SDK signatures

This appendix is the single source of truth for the v0.6.3 code
session's Phase 1 read. The signatures are pseudocode (not
literal TypeScript — Plinth's JS code is plain ES modules,
no TypeScript build pipeline today); for clarity the appendix
uses TS-style annotations but the v0.6.3 code session ships
plain JavaScript that implements these contracts.

### A.1 Panel SDK (`plinth.panel.*`)

```ts
type PanelApi = {
  // === Live in 0.6.3 ===
  onActivate(callback: () => void): void;
  onDeactivate(callback: () => void): void;
  setDirty(isDirty: boolean): void;
  registerShortcut(
    combo: string,
    callback: (event: KeyboardEvent) => void
  ): UnregisterFn;
  getContext(): unknown;

  // === Stub in 0.6.3 ===
  navigate(target: string, context?: unknown): void;
    // throws NotImplementedError
  openFloat(contentType: string, context?: unknown): Promise<{ floatId: string }>;
    // returns rejected Promise
  onNavigationIntent(callback: (target: string, context: unknown) => void): void;
    // registers (does not throw); callback never fires until 0.6.6
  requestFocus(): void;
    // throws NotImplementedError
  setTrayState(stateName: string): void;
    // throws NotImplementedError
  setTrayBadge(value: number | "dot" | null): void;
    // throws NotImplementedError
};

type UnregisterFn = () => void;
```

### A.2 Client SDK exports (`@plinth/frontend/sdk`)

```ts
// === Functions ===
export function call<T = unknown>(
  capability: string,
  ...args: unknown[]
): Promise<T>;

export function subscribe<T = unknown>(
  channel: string,
  handler: (envelope: T) => void
): () => void;

export function useData<T = unknown>(
  channel: string,
  opts?: {
    snapshot?: { capability: string, args: unknown[] };
    initialData?: T;
  }
): { data: T | undefined; error: Error | null; loading: boolean };

// === Convenience namespace ===
export const plinth: {
  call: typeof call;
  subscribe: typeof subscribe;
  useData: typeof useData;
};

// === Error classes ===
export class CapabilityError extends Error {
  code: string;
  sqlstate?: string;
}
export class NetworkError extends Error {
  cause?: unknown;
}
export class NotImplementedError extends Error {
  // message: "plinth.panel.{method} is not implemented in 0.6.3 — closes 0.6.{N}"
}
export class ShortcutConflictError extends Error {
  // message: "combo {normalised_combo} already registered by panel '{panel_id}'"
}
export class PanelUnboundError extends Error {
  // message: "{method}: panel '{panel_id}' is unbound"
}
```

### A.3 Kernel HTTP cap-dispatch contract

```http
POST /api/cap/{capability} HTTP/1.1
Cookie: plinth_session=<session_id>
Content-Type: application/json
X-Plinth-CSRF: <csrf_token>          # required for mutating capabilities

{ "args": [<arg0>, <arg1>, ...] }
```

200 OK:
```json
{ "ok": true, "value": <capability return value> }
```

4xx / 5xx:
```json
{
  "ok": false,
  "error": {
    "code": "rbac_denied" | "not_found" | "bad_request" | "rate_limited" |
            "quickjs_throw" | "dispatch_timeout" | "circuit_open" |
            "tier3_unreachable",
    "message": "<human-readable message>",
    "sqlstate": "<5-char PG SQLSTATE>"   // optional; populated if cause is a PG error
  }
}
```

### A.4 `audit.emit_boundary` capability contract

```ts
audit.emit_boundary(detail: {
  panel_id:       string | null;
  error_message:  string;
  error_stack?:   string;     // omitted in production builds per OQ6
  component_path?: string;
}): Promise<void>;
```

The capability ignores the action name; it always emits
`frontend.boundary.caught`. The detail fields are length-capped
at the handler:
- `error_message`: 1024 bytes
- `error_stack`: 8192 bytes
- `component_path`: 8192 bytes

### A.5 `panels.json` schema (subset interpreted by 0.6.3)

```ts
type PanelsJson = {
  panels: PanelEntry[];
};

type PanelEntry = {
  id: string;                // unique within the extension
  type: 'primary' | 'float' | 'tray';
  label: string;             // human-readable display name
  icon?: string;             // icon identifier (consumed by 0.6.4 topbar)
  component: string;         // path within extension's `client/` dir, e.g. "client/panels/editor.js"
  chrome_essential?: boolean; // tray-only; not interpreted in 0.6.3
  tray_states?: string[];    // tray-only; not interpreted in 0.6.3
  surface_traits?: string[]; // accepted-and-stored; not interpreted in 0.6.3
  slots?: Record<string, unknown>; // accepted-and-stored; not interpreted in 0.6.3
};
```

### A.6 Import-map declaration (in `index.html`)

```html
<script type="importmap">
{
  "imports": {
    "@plinth/frontend/sdk": "/api/frontend/sdk.js",
    "@plinth/frontend/tokens": "/api/frontend/tokens.css"
  }
}
</script>
```

The import-map must precede any `<script type="module">` that
uses the bare specifiers.

---

## Appendix B — Design-doc amendment plan

Per METHODOLOGY §Phase 2 Constraint #4, the v0.6.3 *code* PR
amends architecture and design docs in the same PR as the
implementation. This appendix inventories the amendments.

### B.1 `architecture/06-frontend.md §4.1` — Endpoint Table

The table at lines 192–199 currently has:

| `/api/frontend/sdk.js` | (deferred) … |

(The row exists implicitly via the §4.3 `@plinth/frontend/sdk`
import-map binding but is not enumerated in the §4.1 table.)

The v0.6.3 code PR adds the row explicitly:

```
| `/api/frontend/sdk.js` | ✓ implemented v0.6.3 — 302 → `/ext/{active-frontend}/{version}/client/sdk.js` |
```

Plus a **status header** above the table, mirroring the §4
v0.6.2 header:

```
**Status (2026-04-XX, v0.6.3): `sdk.js` endpoint implemented.**
Per ICD-0.6.3 §5.6 and §5.7. `tokens.css` row inherited from
v0.6.2; other rows below remain `(deferred)`.
```

### B.2 `architecture/06-frontend.md §5` — Panel System (Summary)

The lines 261–279 currently describe the panel system as a
forward-cite to `DESIGN-shell-v06x.md`. The v0.6.3 code PR
status-flips:

- Add a header note: **Status (2026-04-XX, v0.6.3): SDK + loader
  operational; topbar runtime arrives 0.6.4.**
- Change "Authoritative panel-system design lives in
  `DESIGN-shell-v06x.md`." to: "Authoritative panel-SDK
  contract lives in `ICD-0.6.3-panel-sdk-client-sdk.md`;
  authoritative panel-system *visual* design lives in
  `DESIGN-shell-v06x.md`."

### B.3 `DESIGN-shell-v06x.md §0.6.3` — Status footer

Add a footer paragraph noting v0.6.3 ship date + git SHA + ICD
cross-reference. Matches the existing §0.6.0 / §0.6.1 / §0.6.2
footers.

### B.4 No design-bundle amendments

Unlike ICD-0.6.2 (which amended `Plinth Shell.html:85` from zoom
to rem), ICD-0.6.3 has **no design-bundle amendments**. The
bundle's panel JSX prototypes already use the React/Preact
component-factory pattern that the SDK ratifies; no divergence.

---

## Appendix C — Architecture promotion checklist

This appendix lists the cross-references that need to be added
when the v0.6.3 code milestone ships. Matches ICD-0.6.2 Appendix
C shape.

### C.1 `architecture/06-frontend.md §4.1`

Endpoint table — add `/api/frontend/sdk.js ✓ implemented v0.6.3`
row per Appendix B.1.

### C.2 `architecture/06-frontend.md §4.3`

Import-Map Binding — flip status from "expected to publish" to
"published as of v0.6.3"; cross-reference ICD-0.6.3 §5.7.

### C.3 `architecture/06-frontend.md §5`

Panel System (Summary) — status-flip per Appendix B.2.

### C.4 `ICD-0.6.0 §15` — Panel SDK and client SDK

Add `_Discharged by ICD-0.6.3 (paper authored 2026-04-29 on
feat/0.6.2.N-icd-0.6.3-authoring)._` line under the deferral
entry (lines 1055–1063).

### C.5 `ICD-0.6.0 §10` — Boundary-caught audit promotion

Add `_Promoted by ICD-0.6.3 (paper authored 2026-04-29 on
feat/0.6.2.N-icd-0.6.3-authoring)._` line under the deferral
entry (lines 708–711).

### C.6 `ROADMAP.md` line 135

Flip `[ ] 0.6.2.N ICD-0.6.3 authoring` → `[x]`; add summary
line per the ICD-0.6.2-authoring precedent (line 131).

### C.7 `DESIGN-shell-v06x.md §0.6.3`

Add status footer per Appendix B.3.

---

## Appendix D — Test-fixture extension sketch (informative)

The v0.6.3 ship gates manual FE smoke against a test-only fixture
extension at `tests/extensions/sdk_demo/`. This appendix sketches
the smallest possible such extension that exercises the contract
end-to-end. Per architect-pin §14 OQ4, the fixture is **not**
shipped to production.

### D.1 Directory layout

```
tests/extensions/sdk_demo/
├── manifest.json
├── panels.json
├── capabilities.json     # empty (no provided capabilities)
├── rbac.json             # empty (no rules)
└── client/
    └── panels/
        └── demo.js
```

### D.2 `manifest.json`

```json
{
  "name": "sdk_demo",
  "version": "0.0.1",
  "description": "Test fixture for ICD-0.6.3 panel SDK + client SDK",
  "author": "plinth-tests",
  "license": "MIT",
  "entry_point": null,
  "frontend": {
    "mount": null,
    "entry": null
  },
  "runtime": {
    "memory_limit_mb": 16,
    "cpu_time_limit_ms": 1000,
    "max_stack_depth": 250
  }
}
```

The `frontend.mount` is null because `sdk_demo` is not the
active frontend; only the shell is. The fixture's panel is
loadable via direct asset path during tests but does not
register as a frontend root.

### D.3 `panels.json`

```json
{
  "panels": [
    {
      "id": "demo",
      "type": "primary",
      "label": "SDK Demo",
      "icon": "code",
      "component": "client/panels/demo.js"
    }
  ]
}
```

### D.4 `client/panels/demo.js`

```js
import { call, subscribe, useData } from '@plinth/frontend/sdk';
import { html } from 'htm/preact';
import { h, render } from 'preact';

// Default export receives the plinth.panel API as injection.
export default function(panel) {
  let mounted = true;
  panel.onActivate(() => {
    console.log('[sdk_demo] onActivate');
  });
  panel.onDeactivate(() => {
    console.log('[sdk_demo] onDeactivate');
    mounted = false;
  });

  // Test 1: plinth.call to a shell-provided capability.
  let theme;
  call('shell.preferences.get', 'shell.theme')
    .then(value => { theme = value; })
    .catch(err => { console.error('[sdk_demo] call failed:', err); });

  // Test 2: plinth.subscribe to an arbitrary channel (kernel
  // pubsub.publish triggers the handler in the smoke gate).
  const unsub = subscribe('sdk_demo:test', (envelope) => {
    console.log('[sdk_demo] subscribe envelope:', envelope);
  });

  return () => html`
    <div data-ipoint="ext.sdk_demo.primaryPane">
      <h1>SDK Demo</h1>
      <p>Theme: ${theme ?? 'loading…'}</p>
      <p>Subscribed to: <code>sdk_demo:test</code></p>
    </div>
  `;
}
```

### D.5 What the fixture exercises

| Method | Exercise |
|--------|----------|
| `onActivate` | Logged on initial mount |
| `onDeactivate` | Logged on unmount (test seam triggers) |
| `plinth.call` | `shell.preferences.get('shell.theme')` round-trip |
| `plinth.subscribe` | `sdk_demo:test` channel subscribe + receive |
| Panel render | Visible HTML with theme value rendered |

The fixture does **not** exercise:
- `plinth.useData` — covered by U.* test cases (manual FE smoke
  if browser harness absent).
- Stub methods — covered by R.* regression tests (separate from
  the fixture).
- Boundary audit — covered by A.* test cases via a
  deliberately-throwing fixture variant; not in the smoke fixture.

### D.6 Loading the fixture in tests

The Catch2 / FE-smoke harness loads the fixture via:

```cpp
// pseudocode
auto fixture = sdk_demo_load(test_data_dir / "extensions" / "sdk_demo");
auto panel = fixture.load_panel("demo", /* context */ {});
panel.activate();
// assertions on panel state, call response, subscribe envelope
```

The harness is implementation-defined at v0.6.3 code session; the
ICD does not pin its shape. The key constraint is that the
fixture is **not** installed via the production install_lifecycle
path (it does not appear in `plinth.packages` of a production
data_dir).

---

## Appendix E — Pre-existing infrastructure inventory

This appendix lists the infrastructure that **already exists** as
of v0.6.2, which the v0.6.3 code session reuses without
modification. The point is to bound the v0.6.3 code session's
scope by enumerating what is NOT new this milestone.

### E.1 Kernel-side infrastructure

| Infrastructure | Location | Status |
|----------------|----------|--------|
| Capability-dispatch Tier 2 path | `src/kernel/extensions/dispatch.cpp` (or wherever ICD-0.5.0.3 landed) | Operational since v0.5.0 |
| `cap.call` QuickJS binding | `src/kernel/js/stdlib/cap_bindings.hpp` | Operational since v0.3.x |
| `audit.log` QuickJS binding | `src/kernel/js/stdlib/audit_bindings.cpp` | Operational since v0.1.7 |
| Realtime WS broker | `src/kernel/realtime/broker.cpp` | Operational since v0.5.2; LH-2 stress validated 2026-04-24 |
| Active-frontend resolver | `src/kernel/shell/active_frontend.cpp` | Operational since v0.6.1 |
| `/api/frontend/tokens.css` indirection | `src/kernel/frontend/api_frontend.{hpp,cpp}` | Operational since v0.6.2 |
| Asset-server `/ext/{name}/{version}/(.*)` route | `src/kernel/packages/asset_server.cpp` | Operational since v0.4.x |
| RBAC dispatch-time check | `src/kernel/rbac/enforcement.cpp` | Operational since v0.1.5 |
| Audit non-forgeable payload key stripping | `src/kernel/js/stdlib/audit_bindings.cpp:44-56` | Operational since v0.1.7 |
| Session-cookie filter | `src/kernel/auth/session_filter.cpp` (or equivalent) | Operational since v0.1.x |
| CSRF double-submit cookie | `src/kernel/auth/csrf.cpp` (or wherever) | Operational since v0.1.x |
| `parse_manifest` `panels.json` parsing + storage | `src/kernel/packages/manifest_parser.cpp` | Operational since v0.4.x |
| `register_extension_rbac_rules` (default-grant infra) | `src/kernel/packages/install_lifecycle.cpp` | Operational since v0.6.1 |

### E.2 Shell-extension infrastructure

| Infrastructure | Location | Status |
|----------------|----------|--------|
| Top-level Preact `Boundary` component | `client/shell/client/shell.js` | Operational since v0.6.0 §7 |
| `plinthFetch` cookie-auth wrapper | `client/shell/client/shell.js:113` | Operational since v0.6.0 |
| Pre-paint resolver | `client/shell/client/prepaint.js` | Operational since v0.6.2 |
| `preferences.set` / `get` / `get_all` JS handlers | `client/shell/server/handlers/preferences.{set,get,get_all}.js` | Operational since v0.6.1 |
| `tokens.css` artefact | `client/shell/client/css/tokens.css` | Operational since v0.6.2 |
| Avatar popover with theme + scale controls | `client/shell/client/shell.js` (the AvatarPopover component) | Operational since v0.6.2 |
| Bundled-shell first-boot install | `src/kernel/shell/firstboot.cpp` | Operational since v0.6.1 |
| Manifest version bump on shell upgrade | inherits ICD-0.4.5 atomic-swap path | Operational since v0.4.5 |

### E.3 What's NEW this milestone

- `POST /api/cap/{capability}` kernel HTTP route +
  filter chain (session, CSRF, dispatch, audit). Lands at
  `src/kernel/cap/api_cap.{hpp,cpp}` (NEW; or the existing
  `api_frontend.cpp` is renamed to `api_*.cpp` and grows; v0.6.3
  code session decides ergonomics).
- `GET /api/frontend/sdk.js` kernel HTTP route (second route on
  the existing `api_frontend.cpp` handler; minimal addition).
- `client/shell/client/sdk.js` artefact (NEW).
- `client/shell/client/panels/loader.js` artefact (NEW).
- `client/shell/server/handlers/audit.emit_boundary.js` handler
  (NEW).
- `client/shell/capabilities.json` declares
  `audit.emit_boundary`.
- `client/shell/rbac.json` declares
  `frontend.boundary.audit_emit` rule + default grant.
- `client/shell/audit.json` declares `frontend.boundary.caught`
  audit-kind (if the manifest format requires it; v0.6.3 code
  session confirms — may be implicit per ICD-0.1.7 patterns).
- `client/shell/client/index.html` gains the `<script
  type="importmap">` block.
- `client/shell/client/shell.js` gains the `componentDidCatch`
  audit emission.
- `client/shell/manifest.json` version bumps `0.6.2 → 0.6.3`.
- Test-fixture extension at `tests/extensions/sdk_demo/` (NEW
  directory, test-only).

The *new* file count is small (~7 files); the kernel-side C++
delta is bounded (one new HTTP route + one extension to an
existing handler). Most of the work is in the shell-extension
JavaScript artefact + test-fixture authoring.
