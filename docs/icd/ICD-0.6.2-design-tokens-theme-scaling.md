# ICD-0.6.2-design-tokens-theme-scaling

**Traces to:** ICD-0.6.0 §15 *Design tokens, theme, UI scaling*
(lines 1042–1049 — "**Closes: 0.6.2** per `DESIGN-shell-v06x.md
§9.2`. 0.6.0's CSS is throwaway scaffolding per §6.3 — every hex
value will be replaced with a `var(--token-name)` reference in the
0.6.2 PR." — this ICD authors that replacement); ICD-0.6.0 §6.3
*Inline CSS minimal style block* (lines 463–483 — the throwaway
hardcoded palette + Inter prose + JetBrains Mono identifiers
typography baseline + 13.5 px body baseline this ICD codifies into
named tokens); ICD-0.6.1 §15 *Design tokens and theme persistence
wiring* (the discharge anchor — "ICD-0.6.2 will hook the theme +
scale toggles into `shell.preferences.set('theme.mode', …)` /
`shell.preferences.set('ui.scale', …)` round-trips this ICD pins.
0.6.1 ships zero theme-toggle UI; the round-trip is the substrate
only."); ICD-0.6.1 §6 *`ext_shell.user_preferences` table* (the
storage substrate — `(user_id, key, value JSONB)` PK with 64 KiB
serialised cap, `ON DELETE CASCADE` on `plinth.users`); ICD-0.6.1
§7 *Get/set capability pattern* (the round-trip surface —
`shell.preferences.get` / `shell.preferences.set` /
`shell.preferences.get_all` capabilities ride the existing Tier 2
dispatch path; this ICD adds two specific keys to the namespace,
not new capabilities); architecture/06-frontend.md §4 *Design Token
Serving (`/api/frontend/*`)* (lines 175–247 — the canonical
indirection contract; this ICD pins the `tokens.css` endpoint
producer for the bundled shell + the active-frontend resolver path);
architecture/06-frontend.md §4.1 *Endpoint Table* (lines 187–194
— this ICD lands `/api/frontend/tokens.css` as the *only* endpoint
in the 0.6.2 milestone; `fonts/{name}` / `icons/{name}` /
`manifest.json` deferred to later 0.6.x slices per OQ5);
architecture/06-frontend.md §4.4 *Active-Frontend Requirement*
(503 fallback when the active-frontend singleton is not satisfied
— this ICD pins the diagnostic body shape);
DESIGN-shell-v06x.md §6.2 *Theme: Light / Dark / System* (lines
540–551 — the canonical three-way toggle, `:root` custom-property
swap mechanism, "Extensions that use design tokens get theme support
for free", `ext_shell.user_preferences` persistence); DESIGN-shell-v06x.md
§6.3 *UI Scaling (80%–175%)* (lines 553–572 — the canonical
rem-based mandate that this ICD reaffirms; the **architect-pinned
rem-vs-zoom decision** locked at the 2026-04-29 plan-mode
interaction with this ICD's authoring session — popup-coordinate
drift under `zoom` worsening with gradient is the canonical
`zoom`-vs-Floating-UI failure that disqualifies `zoom`);
DESIGN-shell-v06x.md §3.4 *Avatar popover* (the placement target
for the two 0.6.2 user-facing controls); DESIGN-shell-v06x.md §10
*Constraints for Code Sessions* (item 4 — token-driven theming
mandate; item 8 — accessibility / scaling mandate);
DESIGN-shell-v06x.md §11 OQ3 *Design token serving mechanism*
(recorded as a 0.6.2 ICD obligation in the ICD-0.6.0 cross-cite,
discharged here);
`docs/sketches/shell-design-2026-04-27/project/Plinth Shell.html`
(lines 23–73 — the canonical token palette this ICD codifies; line
85 — the **divergent `zoom` usage** the v0.6.2 *code* PR amends
to rem-based per METHODOLOGY §Phase 2 Constraint #4); the design
bundle's other `*.jsx` files all consume `var(--bg-0)` / `var(--text-0)`
/ etc. — the token names this ICD canonicalises;
`docs/sketches/shell-design-2026-04-27/project/tweaks-panel.jsx`
(visual reference for the post-0.6.2 full tweaks-panel feature —
0.6.2 ships only the **two minimal controls** in the avatar
popover; the full panel is deferred to 0.6.4 / 0.6.6 per OQ6);
ICD-0.4.4-package-install-lifecycle §State Machine (the bundled-
shell upgrade path the new `client/css/tokens.css` artifact rides
at the v0.6.2 ship — same orchestrator, no state-machine change,
shell version bumps `0.6.1 → 0.6.2`); METHODOLOGY-llm-assisted-development.md
§Phase 2 Constraint #4 (lines 838–851 — the reason the v0.6.2 code
PR amends `Plinth Shell.html:85` and any other divergent design-doc
spots in the same PR as the rem-based code; this ICD's Appendix B
inventories those amendments).

**Depends on:** ICD-0.6.1 (the prior milestone whose
`ext_shell.user_preferences` table + `shell.preferences.{get,set,get_all}`
capabilities this ICD's two-key persistence consumes; no new schema,
no new capabilities, no new RBAC rules — the substrate is reused
verbatim); ICD-0.6.0 (the prior-prior milestone whose throwaway hex
palette + 13.5 px baseline + Inter / JetBrains Mono typography this
ICD canonicalises into named tokens; the inline `<style>` block in
`client/index.html` is the rewrite target); architecture/06-frontend.md
§4 (the `/api/frontend/*` indirection contract — the kernel handler
this ICD pins lives at `src/kernel/frontend/api_frontend.{hpp,cpp}`
new); architecture/06-frontend.md §2.2 (active-frontend singleton —
the resolver this ICD reuses verbatim; v0.6.1's `active_frontend.cpp`
already lands the lookup path); ICD-0.5.0.3-extension-dispatch (the
Tier 2 dispatch path the two new preference keys flow through —
no new dispatch surface needed); ICD-0.1.7-audit (the audit writer
the existing `shell.preferences.set` audit family rides — this ICD
adds no new audit events; theme/scale changes ride the v0.6.1
`shell.preferences.set` audit verbatim).

**Milestone:** 0.6.2 — Design Tokens, Theme, UI Scaling. Third
0.6.x code milestone (after v0.6.0 + 0.6.0.1 atexit-shutdown fix +
v0.6.1 shell schema and user preferences). Authored as paper-only
follow-up `0.6.1.N ICD-0.6.2 authoring` per
METHODOLOGY-llm-assisted-development.md §3.1 forward-ICD-presence
rule and `feedback_icd_horizon.md` (ICDs one milestone ahead). The
piece that closes ICD-0.6.0 §15's *Design tokens, theme, UI scaling*
deferral.

**Status:** Paper. Authored 2026-04-29 on
`feat/0.6.1.N-icd-0.6.2-authoring`. Code session pins OQ1–OQ7 then
implements; expected 4-phase commit arc.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
- `docs/architecture/06-frontend.md` (the contract owner; §4 is the
  substrate this ICD's 0.6.2 slice carves out from. Architecture
  promotions land in the eventual 0.6.2 *code* session, not in this
  paper-only PR per the ICD-0.6.1 paper-session precedent).
- `docs/design/DESIGN-shell-v06x.md` (the 0.6.x shell arc; §6.1 the
  canonical visual surface that the bundle realises; §6.2 the
  three-way theme toggle; §6.3 the rem-based scaling mandate this
  ICD reaffirms; §10 constraints #4 + #8 covering tokens + scaling).
- `docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md` (the earliest
  prior milestone; §6.3 inline-CSS scaffolding is the rewrite
  target; §15 *Design tokens, theme, UI scaling* is the discharge
  anchor at lines 1042–1049).
- `docs/icd/ICD-0.6.1-shell-schema-user-preferences.md` (the
  immediately-prior milestone; §6 + §7 deliver the persistence and
  capability surface this ICD's two preference keys consume; §15
  *Design tokens and theme persistence wiring* is the forward-cite
  to this ICD).
- `docs/icd/ICD-0.5.0.3-extension-dispatch.md` (Tier 2 dispatch
  path the `shell.preferences.{get,set}` calls flow through — no
  kernel-side surface change needed; ICD-0.6.2 only adds two new
  *keys* to the extension-owned namespace).
- `docs/icd/ICD-0.4.5-package-upgrade-and-uninstall.md` (the
  upgrade lifecycle the v0.6.2 shell upgrade rides — `client/`
  directory gains `css/tokens.css`, `manifest.json` version bumps
  `0.6.1 → 0.6.2`; no migration changes; the existing 0.4.5 atomic
  swap covers the file replacement).
- `docs/sketches/shell-design-2026-04-27/` (the canonical
  visual reference; the bundle's `:root` custom-property block at
  `Plinth Shell.html:23-73` is the source-of-truth palette; the
  zoom usage at `Plinth Shell.html:85` is the **divergent spot**
  the v0.6.2 code PR amends per METHODOLOGY Constraint #4 —
  inventoried in Appendix B).
- [client/shell/client/index.html](../../client/shell/client/index.html)
  (the rewrite target; ICD-0.6.0 §6.3's throwaway hex values get
  replaced with `var(--token-name)` references; the inline
  `<style>` block shrinks to a minimal pre-paint set + a token
  consumer; full token table moves to `client/css/tokens.css`
  served via `/api/frontend/tokens.css`).
- [client/shell/client/shell.js](../../client/shell/client/shell.js)
  (gains pre-paint theme + scale resolver; the avatar popover gains
  the two controls; `plinthFetch` is reused for capability calls).
- [client/shell/server/handlers/preferences.set.js](../../client/shell/server/handlers/preferences.set.js)
  (the round-trip endpoint; ICD-0.6.2 adds **two well-known keys**
  `shell.theme` + `shell.scale_pct` with **server-side
  validation** in this handler — see §5.5 / §10 / OQ3).
- [src/kernel/shell/active_frontend.cpp](../../src/kernel/shell/active_frontend.cpp)
  (the active-frontend resolver this ICD reuses for the
  `/api/frontend/tokens.css` 302 target builder; the existing
  `name + version` lookup is sufficient — no new query needed).

---

## Overview

ICD-0.6.2 closes the *Design tokens, theme, UI scaling* deferral
queued in ICD-0.6.0 §15. The 0.6.0 milestone shipped with throwaway
hardcoded hex values (e.g. `background: #0b0f14;`) per its §6.3
caller-triggered scaffolding stance; the 0.6.1 milestone shipped
the persistence substrate (`ext_shell.user_preferences` +
`shell.preferences.{get,set,get_all}` capabilities) without any
consumer. This ICD lands the consumer half: a named token system,
two persisted preferences (`shell.theme` + `shell.scale_pct`), the
`/api/frontend/tokens.css` indirection that lets every extension
inherit the active frontend's tokens, and two minimal user-facing
controls in the avatar popover.

Four things land:

1. **Named CSS-custom-property token system.** A canonical palette
   (surface scale `--bg-0..4`, text scale `--text-0..3`, accent +
   `-soft` / `-softer` alphas, semantic tones `--success` /
   `--warn` / `--danger`, border scale, geometry radii `--r` /
   `--r-md` / `--r-lg`, focus-ring token) — names lifted verbatim
   from the 2026-04-27 design bundle's `Plinth Shell.html:23-73`
   `:root` block. Theme switching is a one-shot `:root[data-theme]`
   selector swap; the dark and light palettes both ship in
   `tokens.css`. Inter prose + JetBrains Mono identifiers typography
   baseline (ICD-0.6.0 §6.3 lines 469–483) carries forward
   unchanged; same `body { font-size: 13.5px; }` baseline becomes
   `:root { font-size: 13.5px; }` so `rem` math is intuitive (1 rem
   = 13.5 px at 100% scale).

2. **Theme toggle (`light` / `dark` / `system`).** Persisted to
   `ext_shell.user_preferences` at well-known key `shell.theme`
   (JSONB string literal allow-list of three values). When `system`
   is selected, the shell's pre-paint resolver listens to
   `matchMedia('(prefers-color-scheme: dark)')` and dynamically
   updates `documentElement.dataset.theme` between `light` and
   `dark` for the lifetime of the page. The token table swaps via
   the `:root[data-theme="light"]` selector — extensions get
   theme support for free, by virtue of importing tokens.

3. **UI scaling (80% – 175%, rem-based).** Persisted to
   `ext_shell.user_preferences` at well-known key
   `shell.scale_pct` (JSONB integer 80–175 inclusive). The
   pre-paint resolver applies the chosen scale by setting
   `documentElement.style.fontSize = "${pct * 0.135}px"` (100% →
   13.5 px, the ICD-0.6.0 baseline; 175% → 23.625 px; 80% →
   10.8 px). All shell-owned dimensions consume rem; the bundle's
   `Plinth Shell.html` `style.zoom` usage is amended in the same
   PR per METHODOLOGY §Phase 2 Constraint #4. **The architect-
   pinned rem-vs-zoom decision** is locked: rem-based, because
   `zoom` decouples DOM-coordinate space from rendered space and
   floating-UI elements (avatar popover, tray dropdowns, future
   panel chrome, future float panels) drift from their anchor
   points by progressively-larger pixel counts as the gradient
   from 100% grows. Rem leaves the coordinate system untouched
   and only changes computed sizes; popups stay anchored.

4. **`/api/frontend/tokens.css` indirection (kernel-side).** A new
   kernel HTTP handler at `src/kernel/frontend/api_frontend.{hpp,cpp}`
   resolves the active-frontend singleton and returns 302 →
   `/ext/{name}/{version}/css/tokens.css` per
   `architecture/06-frontend.md §4.1` lines 187–194. Cache headers
   are `Cache-Control: no-cache` on the 302 indirection (so a
   bundled-shell upgrade is picked up on next navigation without a
   hard reload) and `immutable` on the version-pinned target
   (already provided by the 0.6.0 asset-server path). 503 with the
   §4.4 diagnostic body when no active frontend exists or the
   singleton is violated. The shell extension ships a new
   `client/css/tokens.css` artifact, version-bumps `0.6.1 → 0.6.2`,
   and references `<link rel="stylesheet" href="/api/frontend/tokens.css">`
   in its `index.html` so the same path resolves the same way for
   the shell itself as for any extension panel.

The boundary stays narrow on purpose. 0.6.2 does not touch the
panel SDK (0.6.3), tabs / app-switcher / launcher (0.6.4), float
chrome (0.6.5), tray panels (0.6.6), or the full tweaks-panel
feature (0.6.4 / 0.6.6 — the bundle's `tweaks-panel.jsx` is
forward-reference only). Only the **two minimal controls** in the
avatar popover ship: a three-way theme select + a scale select.
Per §15, fonts / icons / manifest.json under `/api/frontend/*` are
deferred to later 0.6.x slices.

**Out of scope (deferred):**

- **Panel SDK + client SDK.** Out of scope; 0.6.3 per
  `DESIGN-shell-v06x.md §9.3`. The token system + indirection are
  the substrate panels consume via `@plinth/frontend/tokens`
  import-map binding (architecture/06-frontend.md §4.3 lines
  208–223); the import-map binding itself ships in 0.6.3 alongside
  `plinth.panel.*` registration.
- **`/api/frontend/fonts/{name}` and `/api/frontend/icons/{name}`.**
  Out of scope; deferred. The architecture-doc table (§4.1) lists
  all four endpoints; ICD-0.6.2 ships only `tokens.css`. Fonts
  ship vendored under `client/vendor/` (already done in 0.6.0)
  and are referenced by relative URL in `tokens.css`, not via
  `/api/frontend/fonts/*`. Icons are not yet a contract — the
  shell ships zero shared icon assets in 0.6.2; the design
  bundle's inline SVGs are panel-local. **OQ5 architect override
  may include `manifest.json`** if downstream extension needs are
  pressing; default recommendation is `tokens.css`-only.
- **`/api/frontend/manifest.json`.** Same — deferred. The shell's
  manifest is already served at `/ext/shell/{version}/manifest.json`
  via the asset path; the indirection `/api/frontend/manifest.json`
  is forward-compat scaffolding for downstream extensions that
  want to introspect the active frontend's name + version + asset
  paths. Add when a consumer milestone needs it.
- **Import-map binding (`@plinth/frontend/tokens`).** Out of
  scope; 0.6.3 per `architecture/06-frontend.md §4.3` and panel-SDK
  delivery. 0.6.2 ships the token-bytes resolver; 0.6.3 makes the
  bare specifier resolve.
- **Per-extension token namespacing.** The shell ships one
  `:root` block (dark + light overrides). Extension panels that
  want their own tokens shadow at the panel-root selector
  (e.g. `[data-panel="ext.foo"] { --accent: ...; }`); kernel-
  enforced per-extension scoping is **not** in scope. **Closes:
  TBD** — likely 0.6.4 with `data-ipoint`-driven panel boundaries.
- **Float panel position recalculation on scale change.**
  `DESIGN-shell-v06x.md §6.3` lines 567–571 flag this as the
  "math-sensitive" concern requiring "explicit test coverage". 0.6.2
  ships zero floats; the scale-change recalc lives in ICD-0.6.5
  alongside float chrome + drag persistence. ICD-0.6.5 inherits
  the rem-based scaling contract from this ICD verbatim — the
  recalc problem is *position-tracking* (drag state stored as px
  is no longer correct after a scale change), not scaling itself.
- **Server-rendered theme attribute on the index-HTML response.**
  An alternative FOUC strategy that templates `<html data-theme="…">`
  server-side from a cookie. Not pursued: the shell is served as a
  static asset by the active-frontend dispatcher (v0.6.1's
  `active_frontend.cpp`) without templating; introducing per-request
  templating just for this is disproportionate. The **inline `<script>`
  pre-paint resolver** OQ3 recommendation is sufficient for the
  observed failure mode (visible flash on a slow first paint).
- **High-contrast / reduced-motion / forced-colors theme variants.**
  Defer; the architecture covers `prefers-color-scheme` only.
  Future ICD slot if accessibility demand surfaces.
- **Theme tokens for shell-owned chrome only vs. extension-shared.**
  This ICD pins one **shared** token set for the active-frontend
  ecosystem. A panel-local override mechanism (e.g. data-panel
  attribute selector cascade) is documented in §3.6 but is
  ecosystem convention, not kernel contract. Per-extension token
  *registration* (where extensions declare tokens that the shell's
  `tokens.css` includes) is **not** in scope and likely never will
  be — extensions ship their own panel CSS.

---

## Glossary

- **Design token.** A named CSS custom property declared at the
  `:root` selector (or one of the `:root[data-theme="…"]`
  overrides). Values are paint-related primitives (color, radius,
  spacing). Code consumers reference tokens via `var(--name)`;
  switching themes swaps the bound value at the root, and the
  reference resolves to the new value on the next paint.
- **Active frontend.** Inherited from ICD-0.6.1 — the row in
  `plinth.packages` whose `provenance ∈ {bundled, admin}`,
  `frontend_mount IS NOT NULL`, `state IN ('ACTIVE',
  'ACTIVE_FLAGGED')` predicate matches. Resolution is single-row
  (architecture/06-frontend.md §2.2 singleton). The
  `/api/frontend/tokens.css` redirector reads the active-frontend
  row's `name + version` to build its `Location:` header.
- **Theme.** One of three string-literal values: `"light"`,
  `"dark"`, `"system"`. `light` and `dark` apply the corresponding
  palette unconditionally. `system` follows the user-agent's
  `prefers-color-scheme` media query and may flip mid-session.
- **`shell.theme`.** Well-known preference key (well-known to the
  shell extension; key namespace remains shell-extension-owned per
  ICD-0.6.1 §6.2 / §15). Value is JSONB string literal — one of
  the three theme literals — with a 64 KiB JSONB cap that the
  literal trivially fits within. Server-side handler validation
  rejects out-of-allow-list values per §11 SC2.
- **`shell.scale_pct`.** Well-known preference key. Value is JSONB
  integer (number with no fractional component) in the inclusive
  range `[80, 175]`. Server-side handler validation rejects
  out-of-range values + non-integer values per §11 SC3. Default
  value (when key absent): `100`.
- **Pre-paint resolver.** A small inline `<script>` block at the
  top of `index.html` that runs synchronously before any other
  module loads (no `defer`, no `async`, no `type="module"`). Reads
  `localStorage.shellPrefs` (a JSON object mirroring the
  user-preferences hydration the shell maintains across reloads),
  applies `documentElement.dataset.theme` and
  `documentElement.style.fontSize` before the first paint. Avoids
  the flash-of-unthemed-content that an after-paint application
  would produce.
- **`/api/frontend/tokens.css`.** The kernel-handled indirection
  endpoint per `architecture/06-frontend.md §4.1`. Returns
  `302 Found` with `Location: /ext/{name}/{version}/css/tokens.css`
  built from the active-frontend lookup; `Cache-Control: no-cache`
  on the redirect; the target is `immutable`-cached per §3.
  Returns `503 Service Unavailable` with a JSON diagnostic body
  if no active frontend exists or the singleton is violated.
- **`tokens.css` artifact.** The shell-extension-owned CSS file at
  `client/css/tokens.css` in the v0.6.2 `shell.zip`. Served at the
  versioned URL `/ext/shell/0.6.2/css/tokens.css` via the standard
  asset path. Body shape pinned in Appendix A. Contains both the
  `:root` block (dark palette) and the `:root[data-theme="light"]`
  override.
- **`data-theme` attribute.** An HTML attribute on the
  `<html>` element (`document.documentElement`). Values:
  `"dark"` (the implicit default — also written explicitly at
  pre-paint-resolver time so the attribute is non-null when JS
  reads `documentElement.dataset.theme`), `"light"` (override).
  Empty / missing also reads as dark per the `:root` selector
  match precedence.
- **Rem.** The CSS length unit equal to the computed font-size of
  the root element. Setting `documentElement.style.fontSize` (px
  value) controls the rem unit's effective size; every rem-based
  dimension scales proportionally. ICD-0.6.0 §6.3's 13.5 px body
  baseline becomes the 100% scale's rem unit. The
  rem-vs-zoom decision pin in this ICD's §5 is normative for the
  0.6.x arc.
- **Token consumer.** Any CSS rule that resolves a `var(--name)`
  reference. The shell's `client/index.html` inline `<style>` and
  `client/shell.css` (when introduced) consume tokens. Future
  extension panels consume tokens via the import-map binding
  shipped in 0.6.3.
- **Floating UI.** Generic term for popups, popovers, dropdowns,
  tooltips, tray menus — UI elements positioned via JS reading the
  anchor's `getBoundingClientRect()` and computing the float's
  absolute position. The architect-pinned rem-over-zoom decision
  is grounded in floating-UI's `getBoundingClientRect()` returning
  zoom-decoupled coordinates under `zoom`, producing visible
  drift between the anchor and the float as the scale gradient
  grows.
- **Pre-paint flash.** The visible state where a page's first
  paint shows the default theme (dark) for a frame or two before
  JS applies the user's preferred (light) theme. Avoided by the
  pre-paint resolver running synchronously above the
  `<script type="module" src="shell.js">` line in the HTML.
- **Avatar popover.** The shell-owned popover anchored to the
  topbar's avatar zone (ICD-0.6.0 §5.5 `Sign Out` popover; reused
  here as the placement target for the two new controls). Per
  `DESIGN-shell-v06x.md §3.4`, the avatar zone graduates to a
  full tray panel in 0.6.6; 0.6.2 keeps the hand-rolled component
  shape from 0.6.0.

---

## §3 — Token taxonomy

This section pins the canonical CSS custom-property names, dark
default values, and light-theme overrides. The set is lifted
verbatim from
`docs/sketches/shell-design-2026-04-27/project/Plinth Shell.html`
lines 23–73 (the design bundle's `:root` blocks). No name renaming;
no value drift; the bundle is the source of truth and this ICD
codifies it.

### 3.1 Surface tokens (background scale)

| Token | Dark | Light | Purpose |
|-------|------|-------|---------|
| `--bg-0` | `#0b0f14` | `#f7f7f4` | Page background; lowest surface |
| `--bg-1` | `#11161d` | `#fcfcfa` | Card / panel surface |
| `--bg-2` | `#171d26` | `#f1f1ed` | Hover / focus surface |
| `--bg-3` | `#1f2630` | `#e8e8e3` | Elevated surface (popover, modal) |
| `--bg-4` | `#262e3a` | `#dedcd5` | Highest surface (tooltip, menu top) |

Five-tier scale; brightest tier is named `--bg-0` (lowest
elevation, page background) and darkest tier `--bg-4` (highest
elevation) **only** in the dark palette — the light palette
inverts the brightness gradient so the *visual* layering is
consistent across themes (the lowest-elevation surface always has
the smallest visual contrast against the page; ascending tiers
add subtle visual elevation by darkening on light, lightening on
dark). Token consumers should not assume a particular hex value
or even a brightness ordering; they assume the tier-based
elevation contract.

### 3.2 Text tokens

| Token | Dark | Light | Purpose |
|-------|------|-------|---------|
| `--text-0` | `#e7edf4` | `#16181c` | Primary headings; high-emphasis |
| `--text-1` | `#c4cbd4` | `#353841` | Body prose default |
| `--text-2` | `#8a94a0` | `#6a6e78` | Secondary / metadata |
| `--text-3` | `#5f6874` | `#93969f` | Disabled / placeholder |

Four-tier scale, high → low emphasis. Default body text color is
`var(--text-1)` per the bundle's `html, body { color: var(--text-1) }`
rule.

### 3.3 Border tokens

| Token | Dark | Light | Purpose |
|-------|------|-------|---------|
| `--border` | `#252c36` | `#dcdad2` | Default 1 px hairline |
| `--border-soft` | `#1a2029` | `#e9e7df` | Internal / nested separators |
| `--border-strong` | `#323a45` | `#c8c5bc` | Emphasised separators |

Three-tier scale; the default `var(--border)` is the hairline
between page sections, surface boundaries, and form-element
outlines.

### 3.4 Accent tokens

| Token | Dark | Light | Purpose |
|-------|------|-------|---------|
| `--accent` | `#5aa9ff` | `#1f6feb` | Primary action color (links, CTAs) |
| `--accent-soft` | `rgba(90,169,255,0.14)` | `rgba(31,111,235,0.10)` | Backgrounds for active / selected states |
| `--accent-softer` | `rgba(90,169,255,0.08)` | `rgba(31,111,235,0.05)` | Hover-state backgrounds |

Three-tier scale (solid + two alpha variants). The two `-soft`
tiers are alpha overlays — they layer on top of any surface and
remain readable.

### 3.5 Semantic-tone tokens

| Token | Dark | Light | Purpose |
|-------|------|-------|---------|
| `--success` | `#3bb77e` | `#1f7a4e` | Confirmations, healthy state |
| `--success-soft` | `rgba(59,183,126,0.14)` | `rgba(31,122,78,0.10)` | Banner backgrounds |
| `--warn` | `#d6a640` | `#9a6a07` | Caution, degraded state |
| `--warn-soft` | `rgba(214,166,64,0.14)` | `rgba(154,106,7,0.10)` | Banner backgrounds |
| `--danger` | `#e26565` | `#b43b3b` | Errors, destructive actions |
| `--danger-soft` | `rgba(226,101,101,0.14)` | `rgba(180,59,59,0.10)` | Banner backgrounds |

Three semantic tones × {solid, soft alpha} = six tokens.

### 3.6 Geometry tokens

| Token | Value | Purpose |
|-------|-------|---------|
| `--r` | `5px` | Default radius (form controls, chips) |
| `--r-md` | `7px` | Medium radius (cards, popovers) |
| `--r-lg` | `10px` | Large radius (modals, full-page surfaces) |

Geometry tokens are theme-invariant — same value across dark and
light palettes. Three-tier scale.

### 3.7 Focus-ring token (added)

| Token | Dark | Light | Purpose |
|-------|------|-------|---------|
| `--focus-ring` | `#5aa9ff` | `#1f6feb` | Outline color for `:focus-visible` |

Same value as `--accent` in both themes — but a separate token so
extensions can override the focus indicator independently from
the accent without drift. ICD-0.6.0 §6.3 hardcoded `outline: 1px
solid #5aa9ff` on form-element focus; ICD-0.6.2 swaps to
`outline: 1px solid var(--focus-ring)`.

### 3.8 Typography (carry-over, not new tokens)

ICD-0.6.0 §6.3 lines 469–483 pinned:

- Inter for prose: `html, body { font-family: 'Inter', system-ui, sans-serif; }`
- JetBrains Mono for identifiers: `.mono { font-family: 'JetBrains Mono',
  ui-monospace, monospace; }`
- Body baseline: `body { font-size: 13.5px; }`

ICD-0.6.2 carries this forward unchanged with **two adjustments**
that don't introduce new tokens:

1. The baseline moves from `body { font-size: 13.5px; }` to
   `:root { font-size: 13.5px; } body { font-size: 1rem; }`. This
   makes `1 rem == 13.5 px` at scale 100%, which is the unit that
   `documentElement.style.fontSize` controls in §5.
2. Token consumers that previously hardcoded font sizes
   (e.g. `.login-card h1 { font-size: 18px; }` in current
   `index.html` line 57) get rewritten to rem (`1.333rem ≈ 18px
   at 100%`). This is the v0.6.2 *code* PR's mechanical
   rewrite; the ICD pins the rule.

Self-hosted fonts are unchanged (no CDN per ICD-0.6.0 §6.3 + the
strict `connect-src 'self'` CSP). Inter and JetBrains Mono
`@font-face` declarations move from inline `<style>` into
`tokens.css` for ergonomic co-location with the rest of the
typography contract.

### 3.9 Token naming summary

Total token count: **5 + 4 + 3 + 3 + 6 + 3 + 1 = 25 tokens**.
Naming convention is `--{group}-{tier-or-variant}`; tiers use
integer suffixes; alpha variants use the `-soft` / `-softer`
suffixes; `--focus-ring` is the lone single-token group (no
variants). No CSS hyphen-camelCase mixing; no double-dashes; no
punctuation other than the leading `--` and intra-name `-`.

The naming is **frozen** in this ICD's §3. The v0.6.2 code PR
ships `client/css/tokens.css` with these names verbatim; future
ICDs may add tokens but must not rename existing ones (token
renames break extension panels that consume them, even if
extension panels don't yet exist — the contract is forward-binding
from this PR onward).

---

## §4 — Theme toggle

### 4.1 Storage shape

The user's chosen theme persists in `ext_shell.user_preferences`
at well-known key `shell.theme`. The value is a JSONB string —
one of three literal allow-list values:

```
"light"
"dark"
"system"
```

Stored as JSONB, the literal `"light"` serialises to the
seven-byte sequence `"light"` (the JSON string), well within the
64 KiB cap (ICD-0.6.1 §6.1 `chk_value_size` DDL constraint;
§6.3 row sizing and limits).

The default value when the key is absent is `"system"` per
`DESIGN-shell-v06x.md §6.2` line 547. New users (no rows in
`ext_shell.user_preferences` for them) get the system theme on
first paint.

### 4.2 Resolution to active theme

The shell's pre-paint resolver computes a *resolved theme*
(`light` or `dark`) from the *stored theme* (`light` / `dark` /
`system`):

```js
// pre-paint resolver — runs synchronously before module imports
(function() {
  const stored = readStoredTheme();           // "light" | "dark" | "system" | null
  const resolved = (stored === "system" || stored === null)
    ? (matchMedia('(prefers-color-scheme: dark)').matches ? "dark" : "light")
    : stored;
  document.documentElement.dataset.theme = resolved;
})();
```

`stored === null` (absent key) takes the same branch as `"system"`
— both fall through to the OS preference. The resolved theme is
**always** `"light"` or `"dark"` (never `"system"`); the
`data-theme` attribute is the *resolved* value, not the *stored*
preference.

### 4.3 System-theme tracking

When `stored === "system"` (or absent), the shell installs a
`matchMedia` change listener that flips
`documentElement.dataset.theme` if the OS preference changes
mid-session. The listener is no-op when stored is `"light"` or
`"dark"` — those are explicit overrides that should not flip on
OS change.

```js
const mq = matchMedia('(prefers-color-scheme: dark)');
function syncFromSystem() {
  if (stored === "system" || stored === null) {
    document.documentElement.dataset.theme = mq.matches ? "dark" : "light";
  }
}
mq.addEventListener('change', syncFromSystem);
```

Consumers of the resolved theme observe the attribute change
through the standard CSS cascade — no JS event broadcast.

### 4.4 Persistence wiring

Setting the theme is a `shell.preferences.set` call:

```js
await cap.call("shell.preferences.set", {
  key: "shell.theme",
  value: "light"   // | "dark" | "system"
});
```

Round-trip is the existing v0.6.1 path with no new wire surface.
The handler validates the value per §11 SC2 (allow-list); a
non-allow-list value returns `400 invalid_argument`.

The shell mirrors the value to `localStorage.shellPrefs` after
each successful set so the pre-paint resolver on the next reload
has a synchronous read source — see §4.5 below.

### 4.5 Pre-paint resolver: hydration source

The pre-paint resolver runs *before* any capability call can be
issued (the call requires `shell.js` and `cap.call`, which load
asynchronously). The hydration source is `localStorage.shellPrefs`
— a JSON object that the shell writes after `get_all` on init and
after every `set`:

```json
{ "shell.theme": "light", "shell.scale_pct": 125 }
```

When `localStorage.shellPrefs` is absent (first ever visit, or
the user cleared site data), the resolver reads no value and
falls through to the system / default-100% branch — which
matches the on-server default for a new user. There is no
inconsistency between "first visit, no localStorage" and "new
user, no DB rows" — both paths yield the same first paint.

When `localStorage.shellPrefs` is *stale* (the user changed their
theme on a different device, then logged in here), the first paint
applies the stale localStorage value; the post-init `get_all`
hydration overwrites localStorage with the current DB values; if
they differ, the second-paint state will reconcile (one frame's
worth of inconsistency, acceptable per the same trade-off
`prefers-color-scheme` makes for system-theme).

### 4.6 Cascade contract

The token-table swap happens at the `:root[data-theme="light"]`
selector in `tokens.css`. The dark palette is the bare `:root`
block (no attribute); a light theme applies as a higher-specificity
override. Switching themes is one attribute write +
one paint — no JS-driven CSS regeneration, no module unload.

Extension panels consuming tokens via `var(--bg-0)` etc. inherit
theme support automatically — the cascade reaches into every
descendant of `<html>` regardless of where the panel root is
mounted (no Shadow DOM in 0.6.x; `architecture/06-frontend.md
§7.3`).

### 4.7 No `prefers-reduced-motion` / `prefers-contrast` integration

Out of scope for 0.6.2. Future accessibility work may add
companion preferences (`shell.reduced_motion`,
`shell.contrast_level`) that follow the same pattern; pinning
them now is premature.

---

## §5 — UI scaling (rem-based)

### 5.1 The architect-pinned rem-vs-zoom decision

`DESIGN-shell-v06x.md §6.3` lines 553–565 mandate rem-based
scaling. The 2026-04-27 design bundle's `Plinth Shell.html` line
85 deviates with `style.zoom`, citing a comment that "[zoom is]
the only property that actually behaves like true UI-scale
without rewriting every px value".

This ICD pins **rem-based scaling**, with the following
architect-confirmed rationale (locked at the 2026-04-29 plan-mode
interaction with this ICD's authoring session):

> "Issue to resolve is as scaling in or out popup items lose
> track to the point of click, the greater the gradient the worse
> it gets; as long is this doesn't happen I am happy."

This is the canonical `zoom`-vs-Floating-UI failure mode. `zoom`
is a non-standard CSS property (only recently re-specified;
historically WebKit / Trident — not Gecko-native, with various
half-implementations). When applied to an ancestor element,
`zoom` scales the rendered geometry of every descendant
**without uniformly scaling the JS coordinate system**:

- Browser-engine-internal layout uses zoomed pixels for paint;
- `Element.getBoundingClientRect()` returns coordinates in the
  *viewport* pixel space, which interacts inconsistently with
  zoom across engines and across CSS-property contexts;
- Mouse-event coordinates (`event.clientX`, `event.pageX`) report
  in viewport pixels, so a click at the *visually* correct
  location may resolve to a different DOM coordinate than the
  one a pre-zoom JS measurement of the anchor predicted;
- Floating-UI libraries (Floating UI, Popper.js, the design
  bundle's hand-rolled positioning logic in `topbar.jsx` /
  `app.jsx`) compute popup positions by reading the anchor's
  bounding rect + offsetting; under `zoom`, the offset is in
  unzoomed pixels but the rendered position needs zoomed pixels,
  so the popup drifts from its anchor by an amount proportional
  to `(zoom - 1) × anchor_offset`. The drift visibly worsens as
  the zoom factor departs from `1.0`, exactly as the architect
  reported.

Rem-based scaling has no such coupling. `documentElement.style.fontSize`
is a font-size assignment; it changes the computed value of the
rem unit; every rem-based dimension scales proportionally; the
*coordinate system* (DOM-tree structure, viewport pixel grid,
event coordinates) is untouched. Floating UI's
`getBoundingClientRect()` reads-and-writes the same coordinate
space at every scale; popups stay anchored.

Cost of the rem decision: every shell-owned px dimension that
should scale gets rewritten as rem. The bundle's
`Plinth Shell.html` had reached for `zoom` precisely to *avoid*
that rewrite cost. The v0.6.2 code PR pays the rewrite cost
inline; the bundle file gets amended in the same PR per
METHODOLOGY §Phase 2 Constraint #4 (Appendix B inventories the
amendment).

### 5.2 Storage shape

The user's chosen scale persists at well-known key
`shell.scale_pct`. Value is JSONB integer in `[80, 175]`
inclusive. The pct unit is a percentage (100 = 100% = no
scaling); the storage is the *integer* literal — `100` not
`"100%"`, not `1.0`, not `100.0`.

The default value when the key is absent is `100`.

### 5.3 Resolution to root font-size

```js
const pct = readStoredScalePct() ?? 100;   // integer 80..175
const remPx = pct * 0.135;                 // 100 → 13.5; 175 → 23.625; 80 → 10.8
document.documentElement.style.fontSize = remPx + "px";
```

The constant `0.135` derives from the ICD-0.6.0 §6.3 baseline
(`13.5 px` at 100% / 100). Modifying this constant changes the
absolute baseline; it is **not** a tunable — every rem-based
dimension downstream is sized assuming this baseline.

### 5.4 Bounds + step granularity

- Inclusive range: `[80, 175]`.
- Step granularity: **integer per-step** (any integer between 80
  and 175 inclusive is valid). The avatar-popover scale select
  in §7.2 offers a small set of preset stops (`80, 90, 100, 110,
  125, 150, 175`) for ergonomic UX, but the storage and
  validation accept any integer in range — extensions or future
  finer-grained controls can write any in-range value.

### 5.5 Server-side bound enforcement

The `shell.preferences.set` handler gains a typed-key validation
block (this is a §15 deferral discharge — generic `value: "any"`
manifest type from ICD-0.6.1 doesn't constrain shape per-key, so
the *handler* enforces the shape):

```js
// preferences.set.js — additions for 0.6.2
const SCHEMA = {
  "shell.theme": {
    validate: (v) => ["light","dark","system"].includes(v)
  },
  "shell.scale_pct": {
    validate: (v) =>
      Number.isInteger(v) && v >= 80 && v <= 175
  },
};
if (key in SCHEMA && !SCHEMA[key].validate(value)) {
  throw { code: "invalid_argument",
          message: "value not valid for key " + key };
}
```

Defense-in-depth: the avatar-popover UI also enforces in client,
but a Tier 2 dispatcher (extension JS) that bypassed the UI to
write `shell.scale_pct = 9999` would otherwise corrupt the
preference and produce a render at `9999 * 0.135 = 1349 px` per
rem — comically broken. Server-side rejection per OQ4
recommendation closes the loophole.

### 5.6 Float panel position recalculation

Out of scope. `DESIGN-shell-v06x.md §6.3` lines 567–571
catalogues this. ICD-0.6.5's float-system milestone inherits
rem-based scaling from this ICD; the position-recalc-on-scale-
change problem is float-specific (drag state stored in px goes
stale on scale change), not scaling-mechanism-specific. ICD-0.6.5
will pin the recalc contract.

### 5.7 Cascade contract

Every shell-owned dimension that should scale uses rem.
Every shell-owned dimension that should NOT scale (1 px hairlines,
icon stroke widths, focus-ring outline widths) uses px. The rule
is mechanical: layout dimensions = rem; renders that should
match the OS-pixel grid = px. ICD-0.6.0 §6.3's throwaway
`index.html` inline `<style>` block gets rewritten in the v0.6.2
code PR (every absolute px dimension that's not a hairline
becomes rem); the rewrite is mechanical enough to be a single
commit.

### 5.8 Extension panel propagation

Extension panels consume rem the same way they consume tokens —
by virtue of mounting under `<html>` and inheriting the cascade.
A panel that hardcodes px breaks the scaling contract; a panel
that hardcodes rem inherits it. The ICD-0.6.3 panel-SDK ICD will
make this a panel-author guideline; this ICD documents the
substrate.

---

## §6 — `/api/frontend/tokens.css` indirection

### 6.1 Endpoint contract

Per `architecture/06-frontend.md §4.1` lines 187–194:

```
GET /api/frontend/tokens.css

Active-frontend present + singleton satisfied:
    HTTP/1.1 302 Found
    Location: /ext/{name}/{version}/css/tokens.css
    Cache-Control: no-cache
    Content-Length: 0

No active frontend OR singleton violated (zero rows OR ≥ 2 rows
matching the active-frontend predicate from architecture/06-frontend.md §2.2):
    HTTP/1.1 503 Service Unavailable
    Content-Type: application/json; charset=utf-8
    Cache-Control: no-cache

    { "error": "no_active_frontend",        // OR "multiple_active_frontends"
      "code": <numeric>,
      "message": "..." }
```

302 specifically (not 301 / 303 / 307 / 308). 302 + `Cache-Control:
no-cache` together are the architecture §4.2 default — the redirect
target may shift on shell upgrade and the browser must revalidate.

### 6.2 Active-frontend lookup

The handler reuses the existing v0.6.1 `active_frontend.cpp`
resolver — same `LIMIT 2` query, same singleton-violation
detection. No new query path. Per
`architecture/06-frontend.md §4.4`, lookup behaviour is identical
to the route-registration-time lookup; only the consumer is new.

The v0.6.1 lookup returns `{ name: string, version: string,
mount: string, entry: string }`. The 0.6.2 indirection ignores
`mount` + `entry` and reads only `name + version` to build
`Location: /ext/{name}/{version}/css/tokens.css`.

### 6.3 Cache semantics

- `/api/frontend/tokens.css` 302 response: `Cache-Control:
  no-cache`. Browser must revalidate the redirect target on every
  navigation. A bundled-shell upgrade from 0.6.2 → 0.6.3 changes
  the target version segment; without `no-cache` the browser
  would serve stale tokens until cache eviction.
- `/ext/{name}/{version}/css/tokens.css` body response:
  `Cache-Control: public, max-age=31536000, immutable` per
  `architecture/06-frontend.md §3` lines 149–151. Versioned URLs
  are forever-immutable; the version bump on shell upgrade
  invalidates the cache key.

### 6.4 503 diagnostic body shape

When the active-frontend lookup returns zero rows or ≥ 2 rows:

```json
{
  "error":   "no_active_frontend",
  "code":    503,
  "message": "no active frontend installed; the bundled-shell first-boot install may not have completed"
}
```

```json
{
  "error":   "multiple_active_frontends",
  "code":    503,
  "message": "multiple active frontends detected; one must be uninstalled — see admin extension"
}
```

(Strings are illustrative; the v0.6.2 code session pins the
exact wording.) Per `architecture/06-frontend.md §4.4`, 503 +
diagnostic is the contract; this ICD pins one body shape that
covers both conditions.

### 6.5 Filter-chain ordering

The `/api/frontend/*` route is **kernel-API-shaped**:

- Auth filter: yes — routes under `/api/*` go through the auth
  filter per ICD-0.1.5. The token-CSS body is not user-specific
  (everyone gets the same bytes), so the filter's job is just to
  surface the user-id for audit; an unauthenticated request gets
  the `no_session` 401 path.
- RBAC filter: pass-through for this route. There's no
  RBAC rule "may view design tokens"; everyone authenticated may
  fetch. The route is added to `architecture/05-extensions.md §2`
  *Reserved URL Prefixes* table at `/api/frontend/*` (already
  reserved as a kernel route prefix; this ICD operationalises it).
- Asset-server filter: no — this is an `/api/*` route, not an
  `/ext/*` route. Asset filter handles only the targets, not the
  indirection.

### 6.6 Shell self-consumption

The shell's own `client/index.html` references
`/api/frontend/tokens.css` rather than the versioned
`/ext/shell/0.6.2/css/tokens.css`. This is the **same path**
extension panels will use (when 0.6.3+ panels exist). Symmetry:

```html
<head>
  <!-- pre-paint resolver -->
  <script>(function(){ /* §4.2 + §5.3 */ })();</script>
  <link rel="stylesheet" href="/api/frontend/tokens.css">
  <!-- minimal pre-paint inline css for layout-before-tokens-load -->
  <style>...minimal block, see §3.8...</style>
</head>
```

The shell SHOULD NOT short-circuit through
`/ext/shell/0.6.2/css/tokens.css` even though it knows its own
version — doing so creates an inconsistency where a shell upgrade
delivers new tokens via the indirection but the shell's own UI
serves the old tokens until a hard reload. The 302 path is
fast (μs-scale handler + browser-cached redirect for the rest of
the session) and worth the symmetry.

### 6.7 Implementation slot

Kernel HTTP handler at new
`src/kernel/frontend/api_frontend.{hpp,cpp}` registered after
`register_active_frontend_routes` in `main.cpp`. Lookup wiring is
~30 LoC; the bulk of the file is the 503 diagnostic-body builder.

---

## §7 — Avatar popover controls (light scope)

The full tweaks-panel feature
(`docs/sketches/shell-design-2026-04-27/project/tweaks-panel.jsx`)
is deferred to 0.6.4 / 0.6.6. This ICD ships only the **two
minimal controls** in the existing avatar popover from
ICD-0.6.0 §6.4.

### 7.1 Placement

The avatar popover from ICD-0.6.0 §5.5 currently shows one
button ("Sign Out") — the only avatar-popover content in 0.6.0
per the §5.5 docstring. This ICD adds two controls above the
existing button:

```
┌─────────────────┐
│  Theme:  [v]    │  ← three-way select (light / dark / system)
│  Scale:  [v]    │  ← preset select (80, 90, 100, 110, 125, 150, 175)
├─────────────────┤
│  Sign Out       │
└─────────────────┘
```

Stub layout — the v0.6.2 code session refines spacing /
typography / divider styling. Both controls render rem-based;
both use tokens (`var(--bg-3)` for popover bg, `var(--text-0)`
for label, `var(--accent)` for focus ring).

### 7.2 Theme select

```js
function ThemeSelect() {
  const stored = useStoredPref("shell.theme") ?? "system";
  return html`
    <label>Theme:
      <select value=${stored} onChange=${(e) =>
        setPref("shell.theme", e.target.value)}>
        <option value="system">System</option>
        <option value="dark">Dark</option>
        <option value="light">Light</option>
      </select>
    </label>`;
}
```

`setPref` calls `cap.call("shell.preferences.set", {key, value})`
and on success: (a) updates `localStorage.shellPrefs`; (b)
applies the theme to `documentElement.dataset.theme` immediately
(no reload required); (c) broadcasts to listeners (none in 0.6.2
beyond the pre-paint resolver's matchMedia listener).

### 7.3 Scale select

```js
function ScaleSelect() {
  const stored = useStoredPref("shell.scale_pct") ?? 100;
  const presets = [80, 90, 100, 110, 125, 150, 175];
  return html`
    <label>Scale:
      <select value=${stored} onChange=${(e) =>
        setPref("shell.scale_pct", parseInt(e.target.value, 10))}>
        ${presets.map((p) =>
          html`<option value=${p}>${p}%</option>`)}
      </select>
    </label>`;
}
```

The select offers preset stops; future per-step granularity could
arrive via a slider in the full tweaks-panel, but 0.6.2 keeps the
control minimal. On change, the new font-size applies to
`documentElement.style.fontSize` immediately — the user sees the
re-flow live, no reload.

### 7.4 No layout overhaul

The popover stays rectangular per ICD-0.6.0; no chrome beyond a
thin separator between the controls and Sign Out. The keyboard-
nav contract (Tab cycles through the controls; Esc closes the
popover) is preserved unchanged; new focus stops follow the
existing tab-order conventions.

### 7.5 Accessibility

- Each `<select>` has an associated `<label>`.
- Focus rings use the `--focus-ring` token.
- Both controls are reachable via keyboard from the avatar
  button (which already opens the popover).
- The theme select's "System" option's effective state is
  exposed via `aria-describedby` referencing a hidden span:
  `"System (currently dark)"` / `"System (currently light)"` —
  so screen-reader users know which palette is active.
- Live regions: none. Theme + scale changes don't need
  aria-live announcements (visually obvious).

---

## §8 — Hand-off from ICD-0.6.1

### 8.1 What ICD-0.6.1 pinned that this ICD inherits unchanged

- `ext_shell.user_preferences` table shape — no schema change;
  ICD-0.6.2's two preference keys land as rows under existing
  primary key contract.
- `shell.preferences.{get,set,get_all}` capabilities — no
  manifest change; both new keys ride the existing capability
  surface.
- `shell.preferences.{read,write}` RBAC rules — no rule change;
  default-grant to the `everyone` group covers theme + scale set
  for every authenticated user.
- The `shell.preferences.set` audit family from ICD-0.6.1
  §10.2 — theme and scale set calls audit through the same
  rate-limited dedup as any other key. ICD-0.6.2 adds zero new
  audit events.
- `parse_manifest` reserved-name rule for `name='shell'` — no
  change.
- The 60 s audit dedup TTL from ICD-0.6.1 OQ7 — applies; rapid
  successive scale-slider changes (a future tweaks-panel UX) get
  one audit row per 60 s window.
- The kernel's bundled-shell first-boot detection from
  ICD-0.6.1 §3 — no change. The 0.6.2 ship is a *bundled-shell
  upgrade*, not a first-boot. Per ICD-0.6.1 §15 *Bundled-shell
  upgrade workflow*, the upgrade flow is admin-driven through
  ICD-0.4.5 — drop in new shell.zip on disk, kernel runs the
  upgrade lifecycle on next boot. The 0.6.2 ship pre-stages the
  new `shell.zip` artifact in CMake; the operator's deploy step
  is the only manual action.

### 8.2 What ICD-0.6.1 pinned that this ICD extends

- The shell extension's manifest version: `0.6.1 → 0.6.2`. No
  field shape change; only the version literal.
- The shell extension's `client/` tree gains a new file
  `client/css/tokens.css` (Appendix A pins the body).
- The shell's `client/index.html` inline `<style>` block shrinks
  significantly — only pre-paint scaffold (page-background fill +
  font-size baseline + minimal layout) remains; everything else
  moves into `tokens.css` + companion `client/shell.css` (new).
- The shell's `client/shell.js` gains: (a) the pre-paint
  resolver block (inlined into `<script>` in `index.html`, not
  imported from `shell.js` — see §4.5 / §5.3); (b) two new
  controls in the avatar popover; (c) `localStorage.shellPrefs`
  hydration on init.
- `client/server/handlers/preferences.set.js` gains a per-key
  validation table per §5.5 (typed-key SCHEMA dict).

### 8.3 What this ICD pre-stages for 0.6.3+

- The `/api/frontend/tokens.css` indirection is a *kernel route*
  that 0.6.3's panel-SDK uses verbatim (the panel-SDK import-map
  binding `@plinth/frontend/tokens` resolves through this same
  302 path).
- The `:root` token names are **frozen** for the 0.6.x arc per
  §3.9. Future ICDs may add tokens, never rename.
- The avatar popover gains the two-control pattern that 0.6.6's
  full tray-system feature inherits when the avatar zone
  graduates to a tray panel — the controls' content shape is
  preserved across the avatar-to-tray migration; only the
  enclosing chrome changes.

### 8.4 Phase ordering for the 0.6.2 code session

Recommended four-phase commit arc (analogous to ICD-0.6.1's
five-phase arc):

1. **Phase 1 — Tokens + bundled-shell artifact bump.**
   New `client/shell/client/css/tokens.css` per Appendix A;
   rewrite `client/shell/client/index.html` inline `<style>` to
   consume tokens (mechanical hex-to-`var()` substitution +
   px-to-rem rewrite of layout dimensions); manifest version
   bump `0.6.1 → 0.6.2`; CMake `shell.zip` packing picks up the
   new file automatically (the existing glob includes
   `client/css/*.css`).
2. **Phase 2 — `/api/frontend/tokens.css` kernel handler.**
   New `src/kernel/frontend/api_frontend.{hpp,cpp}` with the
   active-frontend lookup + 302 builder + 503 diagnostic-body
   builder; route registration in `main.cpp` after
   `register_active_frontend_routes`; B.\* test cases (token
   serving) at `tests/kernel/frontend/api_frontend_test.cpp`
   (new).
3. **Phase 3 — Pre-paint resolver + avatar-popover controls.**
   `client/shell/client/index.html` inline `<script>` block per
   §4.2 + §5.3; `client/shell/client/shell.js` gains two new
   controls + `localStorage.shellPrefs` hydration; T.\* and S.\*
   test cases; per-key validation block in
   `client/shell/server/handlers/preferences.set.js`.
4. **Phase 4 — Docs.** CHANGELOG entry; ROADMAP §0.6 line 133
   `[x]` flip + summary; ICD-0.6.0 §15 cross-reference;
   `architecture/06-frontend.md §4.1` status note flip per
   Appendix C; ICD-0.6.2 §17 amendment block (OQ resolutions +
   any implementation deviations per METHODOLOGY Constraint #4);
   `Plinth Shell.html:85` zoom → rem amendment per Appendix B;
   `DESIGN-shell-v06x.md §6.3` reaffirmation note.

---

## §9 — Configuration surface

### 9.1 No new `Config::Shell` fields

This ICD adds zero new fields to `Config::Shell`. Theme and
scale live in `ext_shell.user_preferences`; there is no kernel
or admin-side default override (admins who want to seed defaults
for new users would do so via a future admin tool that writes
the row at user-creation time — not a config concern).

The existing `Config::Shell` block from ICD-0.6.1 §9.1
(`enabled`, `root_redirect`, `bundle_path`) is unchanged.

### 9.2 No `tokens.css` overrides via config

A pattern that lets operators inject custom `--accent` etc. via
`config.yml` was considered and rejected:

- (a) operators who want a branded shell ship a *replacement*
  shell extension with custom `tokens.css`; that's a regular
  ICD-0.4.5 upgrade, not a config patch;
- (b) per-tenant token overrides are not in the 0.6.x scope
  (multi-tenant in general is deferred);
- (c) the `:root` block is a CSS-syntax artifact, not a
  config-file artifact — running it through a kernel templating
  layer just to inject hex values would couple kernel HTTP
  serving to CSS parsing.

### 9.3 No new realtime / extension config

The token serving is a kernel-API route; the persistence is the
existing user-preferences path. No realtime broker plumbing,
no new event channels, no new dispatch surface.

### 9.4 Migration from ICD-0.6.1 config

Zero migration. Existing deployments running v0.6.1 upgrade to
v0.6.2 with no `config.yml` change required.

---

## §10 — Audit events

### 10.1 No new audit events

Theme + scale changes ride the existing v0.6.1
`shell.preferences.set` audit family per ICD-0.6.1 §10.2 — same
schema, same dedup TTL (60 s per `(user_id, key)`). The
audit-row shape:

```json
{
  "kind":   "shell.preferences.set",
  "actor":  { "user_id": "...", "session_id": "..." },
  "data":   { "key":   "shell.theme",
              "value": "light",
              "size":  7 },
  "rate_limited": false
}
```

`value` is the new value (an extension or admin reading the
audit can derive the change from the prior row's value or via
`shell.preferences.get` — the audit doesn't include a `before`
field).

### 10.2 Read/get_all audits

Read calls to `shell.preferences.get` / `get_all` do **not**
audit per ICD-0.6.1 §10.3 (read calls are not audited; the
audit trail tracks writes only). Theme + scale read on every
page-load — auditing them would flood the audit table.

### 10.3 503 from `/api/frontend/tokens.css`

Returning 503 on no-active-frontend is *not* audited as an
operator-facing event. The condition only persists when the
first-boot install hasn't completed (transient) or when
multi-frontend singleton violation is in effect (already
audited at install time per ICD-0.6.1 §3.5). Adding a per-503
audit would noise the table without adding signal.

---

## §11 — Security constraints

### SC1 — Token bytes are not user-specific

The `/api/frontend/tokens.css` body is identical for every
authenticated user — there's no per-user template. SC1 from
ICD-0.6.1 §11 (`user_id` from session, never from arguments)
trivially holds: the handler reads no user-specific data.
However, the route still requires authentication — unauth
requests return 401. Rationale: tokens are not secret, but the
shell's design surface is part of the application; an attacker
wanting to clone the shell's appearance can fetch the public
asset path `/ext/shell/0.6.2/css/tokens.css` directly only if
they're authenticated (i.e., a credentialed user). This matches
the 0.6.0 / 0.6.1 posture: every shell-related endpoint is
auth-gated.

### SC2 — Theme value allow-list

`shell.preferences.set` rejects non-allow-list values for
`shell.theme` with `400 invalid_argument`. The handler-side
validation table (§5.5) is the enforcement point. The capability-
manifest `value: "any"` type is permissive by design (per
ICD-0.6.1 §17 implementation deviation #4); per-key validation
is the discipline that backs the allow-list.

### SC3 — Scale integer + range

`shell.scale_pct` similarly rejects non-integer or out-of-range
values. A non-integer (e.g. `100.5`) is rejected; the smallest
in-range integer is `80`; the largest is `175`. Rejection
returns `400 invalid_argument`.

### SC4 — Inline `<script>` in index.html

The pre-paint resolver is an inline `<script>` block. The shell's
strict CSP from ICD-0.6.0 §11 is `script-src 'self'`, which
**does not** by default allow inline scripts (only same-origin
external scripts via `src=`). This ICD requires either:

- (a) adding a `'sha256-…'` hash for the specific inline block
  to the CSP (recommended — preserves the strict default for
  other inline scripts);
- (b) loosening to `'self' 'unsafe-inline'` (rejected — opens
  the door to XSS);
- (c) moving the resolver to an external file and pre-loading it
  with `<link rel="preload" as="script">` (rejected —
  `defer`-style execution doesn't run before paint reliably).

Path (a) is the v0.6.2 code-session implementation. The hash is
a build-time computation (CMake hashes the inline block content
into a CSP header value). The hash is recomputed on every
`tokens.css` / `index.html` change.

### SC5 — `localStorage.shellPrefs` is not authoritative

The pre-paint resolver reads from `localStorage` to bridge the
synchronous-startup gap. `localStorage` is per-origin and
trivially-mutable by JS; an attacker with a foothold to
manipulate `localStorage` (XSS already required) could cause a
mismatched first paint. The post-init `get_all` hydration
overwrites with DB-authoritative values, so the attack is
limited to one frame's misrender — strictly cosmetic, no
authentication / authorization surface affected.

### SC6 — No XSS via theme name

The theme value flows from
`localStorage.shellPrefs["shell.theme"]` →
`documentElement.dataset.theme = …`. `dataset.theme = …` does
**not** invoke an HTML parser; the value sets the attribute
verbatim regardless of contents. Even a value like
`"x'><script>"` becomes a meaningless `data-theme="x'><script>"`
attribute. The CSS selector `:root[data-theme="light"]` matches
the literal value, so junk values produce no theme swap (default
dark applied). No XSS surface.

### SC7 — Token bytes are not a credential

`tokens.css` does not contain authentication material, session
identifiers, or any user-private value. It's a public CSS
fixture — same posture as `index.html` itself. No additional
caching constraint beyond the architecture-§4.2 default.

---

## §12 — Test cases

Test counts per category. Per ICD-0.6.1's precedent test
files land under `tests/kernel/shell/` (B.\* / T.\* / S.\* / I.\*)
and `tests/kernel/frontend/` (B.\* for the new
`/api/frontend/tokens.css` route).

### 12.1 `B.*` — token serving (4 cases, integration-level via HttpTestFixture)

`tests/kernel/frontend/api_frontend_test.cpp` (new):

- **B.01** `tokens.css` 302 round-trip: GET
  `/api/frontend/tokens.css` while active-frontend present →
  302 with `Location: /ext/shell/0.6.2/css/tokens.css`,
  `Cache-Control: no-cache`, empty body.
- **B.02** Target body sanity: GET the redirect target →
  200 with `Content-Type: text/css`, body contains `:root {`
  and `--bg-0:` and `:root[data-theme="light"]` token strings.
- **B.03** No-active-frontend 503: drop the bundled shell row
  → GET `/api/frontend/tokens.css` → 503 with JSON body
  matching `{"error":"no_active_frontend",…}`.
- **B.04** Multi-active-frontend 503: insert a second
  `provenance='admin'` ACTIVE frontend row → 503 with JSON
  body matching `{"error":"multiple_active_frontends",…}`.

### 12.2 `T.*` — theme toggle (8 cases, integration-level via HttpTestFixture + JS-dispatch fixture)

`tests/kernel/shell/preferences_dispatch_test.cpp`:

- **T.01** Set `light` round-trip:
  `set("shell.theme","light")` → 200 `{ok:true}`;
  `get("shell.theme")` → `{value:"light"}`.
- **T.02** Set `dark` round-trip: same, value `"dark"`.
- **T.03** Set `system` round-trip: same, value `"system"`.
- **T.04** Reject invalid value:
  `set("shell.theme","fuchsia")` → 400
  `{code:"invalid_argument"}`; row not written.
- **T.05** Reject non-string: `set("shell.theme",42)` → 400
  same shape.
- **T.06** Default when absent:
  `get("shell.theme")` for a new user → `{value:undefined}`;
  client-side resolver applies `system` default per §4.1.
- **T.07** Delete reverts: after `set("shell.theme","light")`
  succeeds, `set("shell.theme",undefined)` → 200
  `{ok:true,deleted:true}`; subsequent `get` returns
  `{value:undefined}`.
- **T.08** Audit row written: `set("shell.theme","light")` → one audit row
  of kind `shell.preferences.set` with `key === "shell.theme"`,
  `value_class === "string"`, and serialized size only. The preference value
  itself is deliberately absent from the audit payload.

### 12.3 `S.*` — scaling (8 cases, integration-level)

`tests/kernel/shell/preferences_dispatch_test.cpp`:

- **S.01** Set `100` round-trip:
  `set("shell.scale_pct",100)` → 200; `get` returns
  `{value:100}`.
- **S.02** Set min `80` round-trip: same, value `80`.
- **S.03** Set max `175` round-trip: same, value `175`.
- **S.04** Reject below min:
  `set("shell.scale_pct",79)` → 400
  `{code:"invalid_argument"}`.
- **S.05** Reject above max: same with `176`.
- **S.06** Reject non-integer:
  `set("shell.scale_pct",100.5)` → 400.
- **S.07** Reject string:
  `set("shell.scale_pct","100")` → 400.
- **S.08** Default when absent: `get("shell.scale_pct")` for
  a new user → `{value:undefined}`; client-side resolver
  applies `100` default per §5.2.

### 12.4 `I.*` — full integration (3 cases, full-stack)

`tests/kernel/shell/design_tokens_integration_test.cpp` (new):

- **I.01** Theme persistence across reload:
  HttpTestFixture session → set `shell.theme=light` →
  fixture restart → `get("shell.theme")` → `{value:"light"}`
  (covers the §0.6.2 exit criterion that 0.6.1 §13 exit
  criterion language calls "round-trip preferences").
- **I.02** Scale persistence across reload: same with
  `shell.scale_pct=125`.
- **I.03** Token-serving + persistence interaction: set
  theme → fetch `/api/frontend/tokens.css` → 302 → fetch
  target → body contains both palette blocks (the body is
  user-agnostic per SC1; theme selection happens client-side
  via `:root[data-theme]`).

### 12.5 `U.*` — UI controls (deferred to 0.6.2.N if browser harness absent)

The two avatar-popover controls (§7.2 / §7.3) need a headless
browser harness to test mechanically. ICD-0.6.0 OQ2 deferred the
browser harness to 0.6.0.N (still open). U.\* cases are
enumerated here for completeness; they move to a 0.6.2.N follow-up
if the harness isn't ready at v0.6.2 ship:

- **U.01** Theme select renders three options (system / dark /
  light) with current value preselected.
- **U.02** Theme select on-change invokes `cap.call` with the
  correct args; success path applies `dataset.theme` immediately.
- **U.03** Scale select renders the seven preset stops with
  current value preselected.
- **U.04** Scale select on-change invokes `cap.call` with the
  correct integer arg; success path applies
  `documentElement.style.fontSize` immediately.
- **U.05** Manual smoke (via `feedback_fe_visualize.md`): open
  shell in browser, change theme to light, verify visible
  swap; change scale to 175%, verify avatar popover stays
  anchored to the avatar button (the architect-pinned
  rem-vs-zoom regression check).

### 12.6 `R.*` — popup-coordinate regression (1 case, deferred to browser harness)

The architect-pinned rem-vs-zoom decision rests on the
floating-UI coordinate-stability claim. R.\* cases verify the
claim mechanically:

- **R.01** Avatar popover anchor stability across scale tiers:
  open the popover at scale=80%, scale=100%, scale=175%; for
  each, sample `getBoundingClientRect()` of the avatar button
  and the popover; assert the popover's left edge matches the
  button's left edge within 1 px tolerance (rem math may have
  sub-pixel rounding; 1 px is the typical browser tolerance).

R.01 is the single regression that *must* pass for the architect
acceptance gate (§13 exit criterion #4). Deferred to the browser
harness if not available at ship; manual smoke stand-in per
U.05.

### 12.7 Test counts

Total enumerated: **24 cases** (4 B + 8 T + 8 S + 3 I + 1 R, plus 5 U
deferred to browser harness). Roughly half what ICD-0.6.1 enumerated
(35 → 24) reflecting tighter scope. Phase 1 + Phase 2 + Phase 3 of
the §8.4 commit arc each land their own test slice.

**Status update 2026-09-03:** all 8 T.* and 8 S.* cases are automated
through the bundled QuickJS handler. I.*, R.*, and U.* remain browser/full-
stack cases.

---

## §13 — Entry / Exit Criteria

**Entry:** Bundled shell shipped through 0.6.1 (active-frontend
resolver operational + `ext_shell.user_preferences` table
present + `shell.preferences.{get,set,get_all}` capabilities
green); 0.6.0 frontend frame contracts established (avatar
popover scaffolding + topbar four-zone layout + login flow);
existing `cmake --build build --target tidy` clean.

**Exit:** All four contributions discharged — verifiable by:

1. **Token system.** `client/shell/client/css/tokens.css` ships
   under v0.6.2 shell.zip with the §3 token names verbatim;
   `client/shell/client/index.html` inline `<style>` block
   contains zero raw hex color values (every former hex is
   replaced with `var(--token-name)`); B.02 round-trip body
   inspection passes.
2. **Theme toggle.** `set("shell.theme",…)` round-trip green for
   `light` / `dark` / `system`; rejection green for invalid;
   pre-paint resolver applies `dataset.theme` before first paint
   (verifiable via the manual-smoke walkthrough per
   `feedback_fe_visualize.md`); T.01–T.08 land in
   `plinth_tests_pg` (or `_ws` if HttpTestFixture is the
   carrier).
3. **UI scaling (rem-based).** `set("shell.scale_pct",…)` round-
   trip green for valid; rejection green for invalid /
   out-of-range; `documentElement.style.fontSize` updates live;
   S.01–S.08 land; **R.01 popup-anchor stability test passes at
   80% / 100% / 175%** (the architect's pinned acceptance gate).
4. **`/api/frontend/tokens.css` indirection.** B.01–B.04 green;
   manual `curl /api/frontend/tokens.css` returns 302 with the
   correct version-pinned `Location:` header; the redirect
   target serves `tokens.css` body bytes via the existing asset
   path.

The `DESIGN-shell-v06x.md §0.6.2` exit criterion (token system
ships; theme + scale toggles ride preferences; rem-based scaling
preserved) is satisfied by I.01 + I.02 + I.03 + R.01.

---

## §14 — Open Questions

Each OQ carries an architect-recommendation. Code-session pin
sequence per ICD-0.5.5 §17 / ICD-0.6.0 §17 / ICD-0.6.1 §17
precedent — a §17 amendment block lands in the v0.6.2 code-
session ship PR with the architect-confirmed resolutions.

**OQ1 — Storage shape for theme: literal allow-list vs. JSONB
object.** §4.1 declares the value as a JSONB string literal in
`{"light","dark","system"}`. An alternative is a JSONB object
`{ "mode": "light", "modified_at": <ts>, ... }`, which leaves
room for future fields without schema change. **Recommendation:**
literal. Rationale: (a) the future-fields case is hypothetical
and YAGNI; (b) literal is human-inspectable in psql /
admin-extension; (c) extensions inspecting the value via
`get_all` get a string, not a typed object — simpler. Architect:
confirm or redirect.

**OQ2 — Storage shape for scale: integer vs. float.** §5.2
declares integer 80–175. An alternative is a float (`87.5`, etc.)
to allow finer granularity. **Recommendation:** integer.
Rationale: (a) per-pixel font-size differences below 1% are
imperceptible at the 13.5 px baseline; (b) integer storage is
JSON-canonical (no `100` vs `100.0` ambiguity); (c) preset
stops in §7.3 are integers — matching the storage primitive
avoids round-tripping precision artefacts. Architect: confirm
or redirect.

**OQ3 — First-paint flash strategy: inline script vs. server-
templated attribute vs. accept brief flash.** §4.5 + §5.3
describe the inline-script-from-localStorage approach.
Alternatives:

- (a) accept brief flash: the user's prefs hydrate via
  post-init `get_all`, applied after first paint. Visible
  light-then-dark flash for users with light theme stored.
  Cheapest implementation; worst UX.
- (b) server-rendered attribute: kernel reads a cookie set on
  preference change and inlines `<html data-theme="…"
  style="font-size:…">` into the index-HTML response template
  at request time. Best UX (no flash, no inline script);
  costliest implementation (introduces per-request HTML
  templating into the asset-server path that v0.6.1 ships as
  pure-static byte serving).
- (c) inline script + localStorage: the §4.5 approach. Mid
  complexity; one frame's flash for a stale-localStorage user;
  no asset-server change.

**Recommendation:** (c) — inline script. Rationale: (a) the
flash is invisible in normal use (the happy path is "user
visited last week, localStorage has values, pre-paint resolver
applies before paint"); (b) the only failure mode is "user
cleared localStorage AND has a non-default theme stored
server-side" — that resolves on the second navigation; (c)
preserves the asset-server-as-pure-bytes posture that v0.6.1
established. Path (b) becomes attractive if the design bundle's
`tweaks-panel.jsx` lifecycle in 0.6.4+ generates more divergent
first-paint state. Architect: confirm or redirect.

**OQ4 — Server-side scale-bound enforcement in
`shell.preferences.set`?** §5.5 ships per-key validation in the
handler. An alternative is purely client-side validation
(trusting that no caller bypasses the UI). **Recommendation:**
yes — server-side validation. Rationale: defense-in-depth
against bypass via direct `cap.call` from devtools or
extension JS; well-known-key contract; the validation table
is small (two keys today, growing) and lives in one
file. Architect: confirm or redirect.

**OQ5 — `/api/frontend/*` scope this milestone:
`tokens.css`-only vs. include `manifest.json`.**
`architecture/06-frontend.md §4.1` lists four endpoints
(`tokens.css`, `fonts/{name}`, `icons/{name}`,
`manifest.json`). §1 of this ICD ships `tokens.css` only.
`manifest.json` is an attractive bundle-discovery hint for
downstream extensions. **Recommendation:** `tokens.css`-only.
Rationale: (a) `manifest.json` has no consumer in 0.6.2 (no
extension panels exist yet); (b) shipping unused endpoints
risks a "freeze before consumer" mismatch when 0.6.3+
extensions surface real needs; (c) if 0.6.3 needs it,
ICD-0.6.3 lands the route there. Fonts / icons remain deferred
through 0.6.x — the shell ships zero shared font / icon assets.
Architect: confirm or redirect.

**OQ6 — Avatar-popover placement vs. modal vs. dedicated
panel.** §7.1 places the two controls in the existing avatar
popover. Alternatives:

- (a) standalone preferences modal (anchored to a "Settings"
  button somewhere in the topbar);
- (b) dedicated tweaks panel matching the design bundle's
  `tweaks-panel.jsx` (the full feature, including non-shell
  preferences from extension panels);
- (c) avatar popover (current).

**Recommendation:** (c) — avatar popover. Rationale: (a)
matches the avatar popover's existing role as the "user
chrome" surface; (b) the full tweaks-panel feature is 0.6.4 /
0.6.6 scope — premature here; (c) the controls are minimal
(two selects); a dedicated modal would be over-chrome. The
v0.6.4 + v0.6.6 milestones can migrate the controls into a
larger panel; the migration is straightforward (rename the
component, move the rendering into the new container). The
intermediate state — popover today, panel later — is
acceptable.

**OQ7 — Audit theme/scale changes as their own kinds vs.
inherit `shell.preferences.set` family?** §10.1 inherits the
existing kind. An alternative is to emit
`shell.theme.changed` and `shell.scale.changed` — kind names
that are more findable in audit queries. **Recommendation:**
inherit. Rationale: (a) the audit family already encodes the
key in `data.key`; queries `WHERE data->>'key' = 'shell.theme'`
extract just theme changes; (b) emitting a separate kind
duplicates information; (c) this is a *consumer* of the
family ICD-0.6.1 §10 already pinned — adding parallel kinds
just because would be churn. Architect: confirm or redirect.

---

## §15 — What Must Not Be Decided Yet

These items are explicitly out of scope for ICD-0.6.2. Each
names the milestone (or trigger) that closes the deferral. Per
`feedback_icd_horizon.md`, ICDs author one milestone ahead and
pre-deciding 0.6.3+ contracts based on 0.6.2 state would
violate that discipline.

### Panel SDK + client SDK

`DESIGN-shell-v06x.md §4` specifies `plinth.panel.*` lifecycle
hooks and `plinth.call() / plinth.subscribe() / plinth.useData()`
client wrappers. **Closes: 0.6.3** per
`DESIGN-shell-v06x.md §9.3`. ICD-0.6.3 will wrap the
`shell.preferences.*` capabilities with a
`plinth.preferences.*` client SDK surface. The token system this
ICD pins is the substrate panels consume; the import-map
binding (`@plinth/frontend/tokens` → `/api/frontend/tokens.css`)
ships in 0.6.3 alongside the panel SDK.

### `/api/frontend/fonts/{name}` and `/api/frontend/icons/{name}`

`architecture/06-frontend.md §4.1` lists these. **Trigger:**
extension panels start shipping shared font / icon assets that
need version-stable URLs. 0.6.2 ships only `tokens.css`. Fonts
remain vendored under `client/vendor/` in the shell extension;
icons remain inline SVG.

### `/api/frontend/manifest.json`

Same — deferred. Active-frontend introspection has no consumer
in 0.6.2; ICD-0.6.3 + may add the endpoint when
panel-registration logic surfaces a need.

### Server-rendered theme attribute

OQ3 alternative (b) is the costlier-but-best-UX path. **Trigger:**
the inline-script approach proves insufficient (e.g. the
tweaks-panel feature in 0.6.4 introduces per-extension theme
overrides that compound flash issues). Future ICD slot, not on
the current 0.6.x roadmap.

### Per-extension token namespacing

The shell ships one shared token set. Extension-owned tokens
(`--ext-foo-accent`) are panel-author convention, not kernel
contract. **Closes: TBD** — likely 0.6.4 with the panel-system
landing if a per-panel token-scope mechanism becomes necessary.

### High-contrast / reduced-motion / forced-colors variants

`prefers-contrast`, `prefers-reduced-motion`, Windows
high-contrast mode. **Trigger:** accessibility audit demand or
user feedback. Future ICD slot. The token swap mechanism this
ICD pins is forward-compat: a `:root[data-contrast="high"]`
override block could append without disturbing the existing
selectors.

### Float panel position recalc on scale change

Math-sensitive; `DESIGN-shell-v06x.md §6.3` lines 567–571 flag
it. **Closes: 0.6.5** when float chrome + drag persistence
land. ICD-0.6.5 inherits rem-based scaling from this ICD
verbatim.

### Multi-tenant per-tenant theming

A deployment with two tenant brands wanting different
`tokens.css` per tenant. **Trigger:** a future multi-tenant
ICD (not on 0.6.x roadmap). The `/api/frontend/tokens.css`
indirection is single-tenant by design — one active frontend
serves one tokens.css.

### Bundled-shell pre-paint customisation per role/group

The pre-paint resolver reads `localStorage.shellPrefs` —
which is a per-browser-session key. RBAC-driven theme
overrides (e.g. "admins always see a red accent stripe")
could be added by reading the user's role pre-paint, but
the role lookup is async (DB query) and incompatible with
the pre-paint synchronous-script constraint. **Trigger:**
admin UX demand. The role-based theming would more likely
manifest as an admin-extension panel skin, not a shell-level
override.

### `tokens.css` `@media (prefers-color-scheme)` block

An alternative to the JS-driven `data-theme` attribute swap
is a pure-CSS approach: ship two `:root` blocks, one wrapped
in `@media (prefers-color-scheme: light) { ... }`. Rejected
because (a) the user's stored preference can override the
system preference (the three-way toggle includes explicit
`light` / `dark` overrides), and the CSS-only approach can't
honor that without JS-driven attribute management anyway;
(b) the unified `[data-theme]` selector mechanism is simpler
to reason about than mixing media-query and attribute
selectors. **Trigger:** none planned.

---

## §17 — OQ Resolutions (post-ship amendment, 2026-04-29 v0.6.2 ship)

The seven §14 OQs are pinned per the architect-recommendation
defaults; five implementation deviations are recorded per
METHODOLOGY §Phase 2 Constraint #4. This section is the
authoritative source — `feedback_real_code_paths.md` requires
deviations to be visible in the ICD itself, not only in the
ship CHANGELOG.

### OQ pin table

| # | OQ | Resolution |
|---|----|------------|
| OQ1 | Theme storage shape: literal allow-list vs. JSONB object | **Literal `"light" \| "dark" \| "system"`** (recommendation) |
| OQ2 | Scale storage: integer vs. float | **Integer 80–175** (recommendation) |
| OQ3 | First-paint flash strategy | **Inline-script-from-localStorage pre-paint resolver** (recommendation; see deviation #2 for the external-sync-script implementation) |
| OQ4 | Server-side scale-bound enforcement in `shell.preferences.set` | **Yes** — defense-in-depth + well-known-key contract (recommendation; SCHEMA dict in `preferences.set.js`) |
| OQ5 | `/api/frontend/*` scope: `tokens.css`-only vs. include `manifest.json` | **`tokens.css`-only** (recommendation; fonts/icons/manifest.json deferred per §15) |
| OQ6 | Avatar popover vs. modal vs. dedicated panel | **Avatar popover** (recommendation; full tweaks-panel deferred to 0.6.4 / 0.6.6) |
| OQ7 | Audit theme/scale changes: own kinds vs. inherit `shell.preferences.set` family | **Inherit** (recommendation; existing audit family encodes the key in `data.key`) |

### Rem-vs-zoom decision (re-affirmation)

§5.1's architect-pinned rem-vs-zoom decision is validated
end-to-end in the v0.6.2 browser smoke. **R.01 acceptance gate
PASSES** at 80% / 100% / 175%: popover-vs-button-anchor delta
scales as a 0.30 rem invariant (3.25 / 4.05 / 7.08 px). No
`zoom`-style coordinate-space drift; popups stay anchored
proportionally as scale changes. The locked decision stands;
no future re-litigation absent a re-eval that produces new data.

### Implementation deviations from §3–§7 + §SC4 + §6.5 + §12.1

1. **`/api/frontend/tokens.css` registers WITHOUT auth filter.**
   §6.5 specified the route as auth-required (`SessionFilter`
   producing 401 on unauthenticated requests). The shell self-
   references this URL from its own login page (§6.6) — auth-
   required would 401 the unstyled login UX. Bytes are not user-
   specific (§6.5 acknowledges the same — "everyone gets the same
   bytes") and audit is off (§10.3), so the filter's stated purpose
   ("surface user-id for audit") does not fire. The handler
   registration in
   [`src/kernel/frontend/api_frontend.cpp`](../src/kernel/frontend/api_frontend.cpp)
   omits the filter argument; the route is unauthenticated. Future
   amendment may revisit if a per-tenant theming requirement
   surfaces (deferred per §15 *Multi-tenant per-tenant theming*).

2. **Pre-paint resolver ships as external sync `<script src=…>`,
   not inline `<script>` with CSP `'sha256-…'` hash.** §SC4 path
   (a) prescribed an inline block protected by a build-time SHA-256
   hash in the `script-src` CSP. v0.6.2 ships the resolver at
   [`client/shell/client/prepaint.js`](../client/shell/client/prepaint.js)
   loaded via `<script src="./prepaint.js"></script>` in
   `<head>` (no `defer` / `async` — blocking by default; runs before
   first paint). §SC4 (c) was rejected because of `<link rel=
   "preload">` async semantics; plain sync external scripts are
   blocking-by-default and run before paint reliably (different
   shape from (c)). The external-sync shape preserves the strict
   `script-src 'self'` CSP without per-build hash recompute on
   every shell change.

3. **Kernel-side persistence via `cap.call("shell.preferences.set",
   …)` deferred to 0.6.1.N.** §4.4 + §7.2 + §7.3 specify browser →
   kernel `cap.call` round-trips for theme + scale persistence.
   v0.6.2 ships **localStorage-only persistence** in
   [`shell.js`](../client/shell/client/shell.js) — per-browser,
   per-device. The browser → kernel `cap.call` path needs the
   `async_bridge_fixture` + `ws_test_fixture` scaffold blocked on
   the `init_registry` teardown bug per
   `project_test_fixture_inflight.md` session 9 (the same blocker
   that deferred ICD-0.6.1's P.\* / I.\* JS-dispatch suite per its
   §17 deviation #5). The SCHEMA validator in
   [`preferences.set.js`](../client/shell/server/handlers/preferences.set.js)
   ships in this PR so the wiring is ready when the 0.6.1.N
   follow-up connects browser → kernel.

4. **B.02 verifies URL construction with synthetic name+version
   instead of following the 302.** §12.1 enumerates B.02 as
   "follow the 302; expect 200 + text/css + body contains
   `:root {`, `--bg-0:`, `:root[data-theme="light"]`". Following
   the redirect through the asset server requires per-(name,
   version) route registration set up by the install_lifecycle
   path — expensive setup for these tests. The asset-server
   byte-serving path is covered by
   [`tests/kernel/packages/asset_server_test.cpp`](../tests/kernel/packages/asset_server_test.cpp);
   B.02 here verifies the kernel-side URL-construction
   contribution that
   [`api_frontend.cpp`](../src/kernel/frontend/api_frontend.cpp)
   actually owns — replaces the bundled-shell row with a synthetic
   `name=demo, version=1.2.3, mount=/console` row and asserts
   `Location: /ext/demo/1.2.3/css/tokens.css`.

5. **T.\* / S.\* / I.\* JS-dispatch tests deferred to 0.6.1.N.**
   §12.2 / §12.3 / §12.4 enumerate 19 cases that exercise theme
   + scale validation + integration end-to-end through the JS-
   handler dispatch path (`cap.call("shell.preferences.set", …)`
   + persistence + reload round-trip). Same blocker as deviation
   #3 — the JS-dispatch fixture needs the `init_registry`
   teardown bug resolved first. v0.6.2 ships the 4 B.\* cases
   (api_frontend handler) plus manual FE smoke per
   `feedback_fe_visualize.md` (theme swap, scale change, R.01
   popover-anchor stability gate at 80% / 100% / 175%). Carries
   forward the v0.6.1 posture per its §17 deviation #5.

   **Update 2026-09-03:** The dispatch-backed T.01-T.08 and S.01-S.08
   families now run in `preferences_dispatch_test.cpp` against the installed
   bundled handler and PostgreSQL. Well-known theme and scale validation is
   also enforced at the native shell dispatch boundary so invalid input keeps
   its `invalid_argument` code and maps to HTTP 400. I.* reload/browser wiring
   and U.* UI-control cases remain deferred to a hermetic browser harness.

### Design-bundle amendments (per Appendix B)

Appendix B inventoried the divergent `Plinth Shell.html:85`
`style.zoom` usage that needed amending in this same code PR per
METHODOLOGY §Phase 2 Constraint #4. The amendment landed:

- `docs/sketches/shell-design-2026-04-27/project/Plinth Shell.html`
  lines 85–88 — replaced the `zoom`-as-best-mechanism comment
  with a forward-cite to ICD-0.6.2 §5.1's rem-vs-zoom locked
  decision. The `#scale-root` wrapper retained as visual-reference
  scaffolding for the design bundle (the bundle is paper / sketch,
  not production code; production lives at
  [`client/shell/client/`](../client/shell/client/) with rem-based
  scaling per §5.3).

- `docs/design/DESIGN-shell-v06x.md §6.3` — added a one-line
  citation pointer to ICD-0.6.2 §5.1 + §SC2 + §SC3 so future
  readers of the design doc find the locked decision without
  re-litigating.

### Follow-ups carried forward

- **Browser → kernel cap.call wiring.** The server-side T.\* / S.\* dispatch
  tests landed in the 2026-09-03 backfill. Shell UI wiring plus I.\* / U.\*
  browser persistence and reload coverage remain deferred to the browser
  harness; the former `init_registry` teardown blocker is closed.

- **`Cache-Control` for `/app/*` shell assets.** Pre-existing
  v0.6.0 issue surfaced during v0.6.2 dev iteration: the
  active-frontend handler stamps `Cache-Control: public, max-age=
  31536000, immutable` on every non-index asset (`shell.js`,
  `prepaint.js`, `css/tokens.css` when served at `/app/css/...`).
  But /app/\* is NOT version-pinned — when shell upgrades from
  0.6.2 → 0.6.3, the URLs don't change but the bytes do; immutable
  blocks revalidation. The `/api/frontend/tokens.css` indirection
  this ICD adds is the proper version-pinning path for tokens.css
  consumers; for the shell itself, /app/\* assets need cache
  semantics revisited (likely `no-cache` for the small subset that
  changes per shell version). Trigger: 0.6.3 ship / first
  shell-asset upgrade in the wild. Not on the 0.6.2 critical path.

---

## Appendix A — Authoritative `tokens.css`

```css
/*
 * shell.zip/client/css/tokens.css
 * Plinth bundled-shell design tokens — ICD-0.6.2 §3.
 *
 * Theme switch: set [data-theme="light"] on <html> for the light
 * palette; bare :root is the dark default. The pre-paint resolver
 * in client/index.html sets [data-theme] before first paint.
 *
 * Scale: documentElement.style.fontSize controls rem; 13.5 px at
 * 100%; range 80%–175% per ICD-0.6.2 §5.
 */

:root {
  /* Surface scale (§3.1) */
  --bg-0: #0b0f14;
  --bg-1: #11161d;
  --bg-2: #171d26;
  --bg-3: #1f2630;
  --bg-4: #262e3a;

  /* Text scale (§3.2) */
  --text-0: #e7edf4;
  --text-1: #c4cbd4;
  --text-2: #8a94a0;
  --text-3: #5f6874;

  /* Border scale (§3.3) */
  --border:        #252c36;
  --border-soft:   #1a2029;
  --border-strong: #323a45;

  /* Accent (§3.4) */
  --accent:        #5aa9ff;
  --accent-soft:   rgba(90, 169, 255, 0.14);
  --accent-softer: rgba(90, 169, 255, 0.08);

  /* Semantic tones (§3.5) */
  --success:      #3bb77e;
  --success-soft: rgba(59, 183, 126, 0.14);
  --warn:         #d6a640;
  --warn-soft:    rgba(214, 166, 64, 0.14);
  --danger:       #e26565;
  --danger-soft:  rgba(226, 101, 101, 0.14);

  /* Geometry (§3.6) */
  --r:    5px;
  --r-md: 7px;
  --r-lg: 10px;

  /* Focus ring (§3.7) */
  --focus-ring: #5aa9ff;
}

:root[data-theme="light"] {
  --bg-0: #f7f7f4;
  --bg-1: #fcfcfa;
  --bg-2: #f1f1ed;
  --bg-3: #e8e8e3;
  --bg-4: #dedcd5;

  --text-0: #16181c;
  --text-1: #353841;
  --text-2: #6a6e78;
  --text-3: #93969f;

  --border:        #dcdad2;
  --border-soft:   #e9e7df;
  --border-strong: #c8c5bc;

  --accent:        #1f6feb;
  --accent-soft:   rgba(31, 111, 235, 0.10);
  --accent-softer: rgba(31, 111, 235, 0.05);

  --success:      #1f7a4e;
  --success-soft: rgba(31, 122, 78, 0.10);
  --warn:         #9a6a07;
  --warn-soft:    rgba(154, 106, 7, 0.10);
  --danger:       #b43b3b;
  --danger-soft:  rgba(180, 59, 59, 0.10);

  --focus-ring: #1f6feb;
  /* Geometry tokens are theme-invariant; not redeclared here. */
}

/* Self-hosted typography per ICD-0.6.0 §6.3 + ICD-0.6.2 §3.8. The
   shell vendors Inter and JetBrains Mono under client/vendor/ in
   the bundle; absolute URLs work because the shell extension
   serves them at /ext/shell/0.6.2/vendor/<file>. The relative
   `url('../vendor/<file>')` resolves against the tokens.css URL
   at /ext/shell/0.6.2/css/tokens.css. */
@font-face {
  font-family: 'Inter';
  font-style: normal;
  font-weight: 400 700;
  font-display: swap;
  src: url('../vendor/inter-variable.woff2') format('woff2-variations');
}
@font-face {
  font-family: 'JetBrains Mono';
  font-style: normal;
  font-weight: 400 600;
  font-display: swap;
  src: url('../vendor/jetbrains-mono-variable.woff2') format('woff2-variations');
}

/* Base typography per ICD-0.6.0 §6.3 + ICD-0.6.2 §3.8. */
:root { font-size: 13.5px; }
html, body {
  margin: 0; padding: 0; height: 100%;
  background: var(--bg-0);
  color: var(--text-1);
  font-family: 'Inter', system-ui, sans-serif;
  font-size: 1rem;
  line-height: 1.5;
  -webkit-font-smoothing: antialiased;
  -moz-osx-font-smoothing: grayscale;
}
.mono {
  font-family: 'JetBrains Mono', ui-monospace, SFMono-Regular,
               Menlo, Consolas, monospace;
  font-feature-settings: 'zero' 1, 'ss01' 1;
}
* { box-sizing: border-box; }
:focus-visible { outline: 1px solid var(--focus-ring); }
```

The `vendor/inter-variable.woff2` + `vendor/jetbrains-mono-variable.woff2`
filenames are illustrative — the v0.6.2 code session pins the
exact filenames that ship in `client/vendor/`. Path is
relative-to-tokens.css (the `..` resolves to `client/`, then
`vendor/` from there).

Total `tokens.css` body: ~3.5 KiB uncompressed; ~1 KiB
gzipped. Light footprint.

---

## Appendix B — Design-doc + design-bundle amendment plan

Per METHODOLOGY-llm-assisted-development.md §Phase 2 Constraint
#4 (lines 838–851), code-session deviations from the ICD that
ship require an "Implementation deviation" subsection in the
ICD itself. The inverse case — *paper-session* changes that
require co-amending design-side documents — is governed by the
same constraint: amend the divergent doc(s) in the same PR as
the code that depends on the resolution. ICD-0.6.2 ships as
*paper*; the v0.6.2 *code* PR carries the amendments.

The v0.6.2 code PR will execute the following amendments
**in the same PR as the code that depends on them**:

### B.1 `Plinth Shell.html:85` — zoom → rem-based scaling

File:
`docs/sketches/shell-design-2026-04-27/project/Plinth Shell.html`

Before (line 85, comment block):

```css
  /* Scale wrapper: `zoom` scales every px dimension + font uniformly.
     Chromium/WebKit/modern FF all support it; it's the only property that
     actually behaves like true UI-scale without rewriting every px value. */
  #scale-root { width: 100%; height: 100vh; }
```

The HTML uses `document.documentElement.style.zoom` for the
scale toggle (search the file for `zoom` to confirm the JS
binding).

After (rem-based — the bundle prototype gets the same treatment
the production shell gets):

```css
  /* Scale: documentElement.style.fontSize controls rem; every
     scaled dimension uses rem. Per ICD-0.6.2 §5, zoom-based
     scaling is rejected because of floating-UI coordinate-space
     drift at non-100% scales (the avatar popover loses its anchor
     by progressively-larger pixel counts as zoom departs from
     100%). The rem path leaves DOM coordinate space unchanged. */
  /* #scale-root removed; rem-based scaling needs no wrapper element. */
```

The JS line that sets `style.zoom` is rewritten to set
`style.fontSize` on `documentElement`; every absolute `px`
dimension in the bundle's CSS that was layout-scaling
(`width: 240px` for a card, `padding: 8px` for a button) gets
mechanically rewritten to `rem` (`width: 17.778rem`,
`padding: 0.593rem`); hairline borders (`border: 1px solid …`)
stay px.

This is a substantial diff to the bundle file. The v0.6.2 code
PR's commit message names this amendment explicitly so the
diff is easy to review.

### B.2 `DESIGN-shell-v06x.md §6.3` — reaffirm rem mandate

File: `docs/design/DESIGN-shell-v06x.md`

§6.3's existing rem mandate (lines 553–565) is unchanged in body
text but gains a one-line citation pointer:

```
+ Pinned by ICD-0.6.2 §5 (rem-vs-zoom decision; architect-
+ confirmed at 2026-04-29 via the popup-coordinate drift
+ failure mode under zoom).
```

The pointer makes the cross-doc relationship discoverable
without re-litigating the decision.

### B.3 No other amendments required

The other design-bundle `.jsx` / `.html` files
(`shell/topbar.jsx`, `shell/admin.jsx`, etc.) consume
`var(--bg-0)` / `var(--text-0)` etc. — names this ICD
canonicalises verbatim. No amendment needed; they pre-comply.

`architecture/06-frontend.md §4` already specifies the
`/api/frontend/tokens.css` indirection as the canonical
contract; no amendment, only a status flip per Appendix C.

`architecture/05-extensions.md §2` *Reserved URL Prefixes*
already includes `/api/frontend/*` as kernel-reserved; no
amendment.

---

## Appendix C — Architecture promotion checklist

When v0.6.2 ships, the following architecture-doc status notes
flip from "specified" to "implemented" / from "deferred" to
"shipped":

### C.1 `architecture/06-frontend.md §4`

Add a `Status (2026-04-30, v0.6.2):` line in the §4 header
indicating the `tokens.css` endpoint is implemented. Body
unchanged; only the status note adds.

### C.2 `architecture/06-frontend.md §4.1` Endpoint Table

The `/api/frontend/tokens.css` row gets a "✓ implemented in v0.6.2"
marker; the other three rows (`fonts`, `icons`, `manifest.json`)
remain status-unmarked (deferred per ICD-0.6.2 §15).

### C.3 ICD-0.6.0 §15 *Design tokens, theme, UI scaling* (line 1042)

A one-line cross-reference appended:

```
+ Discharged by ICD-0.6.2 (paper authored 2026-04-29);
+ shipped in v0.6.2 (date pending).
```

This mirrors the §15-cross-reference pattern ICD-0.6.1 used.

### C.4 ICD-0.6.1 §15 *Design tokens and theme persistence wiring*

A symmetric one-line:

```
+ Discharged by ICD-0.6.2 (paper authored 2026-04-29);
+ shipped in v0.6.2 (date pending).
```

### C.5 ROADMAP.md line 133

Flip the `0.6.2` checkbox `[ ]` → `[x]` and append the
`_Shipped …_` summary line per the v0.6.1 entry pattern at
line 130.

### C.6 DESIGN-shell-v06x.md §0.6.2

Add a "Status: shipped 2026-04-30 (paper authored 2026-04-29 in
ICD-0.6.2)" line at the top of the §0.6.2 milestone block.

---

## Appendix D — `shell.zip` contents diff (v0.6.1 → v0.6.2)

```
shell.zip/
├── manifest.json                 (modified — version 0.6.1 → 0.6.2)
├── capabilities.json             (unchanged)
├── rbac.json                     (unchanged)
├── server/
│   ├── main.js                   (unchanged)
│   └── handlers/
│       ├── preferences.get.js     (unchanged)
│       ├── preferences.set.js     (modified — per-key validation
│       │                          per ICD-0.6.2 §5.5)
│       └── preferences.get_all.js (unchanged)
├── client/
│   ├── index.html                 (modified — pre-paint resolver
│   │                              + tokens.css link + inline-style
│   │                              shrunk to scaffolding only)
│   ├── shell.js                   (modified — avatar popover gains
│   │                              two controls + localStorage
│   │                              hydration)
│   ├── css/                       (NEW directory)
│   │   └── tokens.css             (NEW — Appendix A body)
│   └── vendor/
│       ├── preact.module.js       (unchanged)
│       ├── htm.module.js          (unchanged)
│       ├── inter-variable.woff2   (NEW — self-hosted font)
│       └── jetbrains-mono-variable.woff2  (NEW — self-hosted font)
└── migrations/
    └── 001_init.sql               (unchanged — no schema change)
```

Total uncompressed size delta: roughly +500 KiB for the two
font files (Inter Variable ~330 KiB, JetBrains Mono Variable
~180 KiB compressed-WOFF2 figures); +3.5 KiB for tokens.css; net
~500 KiB increase. Compressed shell.zip moves ~24 KiB → ~530 KiB.
Acceptable — fonts dominate; the alternatives (CDN or system-
font fallback) are ruled out by `connect-src 'self'` CSP and
the design-bundle's typography baseline.

---

## Appendix E — Pre-paint resolver script (informative)

The block that lands in `client/shell/client/index.html`'s
`<head>` above the `<link rel="stylesheet">`:

```html
<script>
(function () {
  var prefs;
  try {
    prefs = JSON.parse(localStorage.getItem('shellPrefs')) || {};
  } catch (_) {
    prefs = {};
  }

  /* Theme resolver — ICD-0.6.2 §4.2 */
  var stored = prefs['shell.theme'];
  var resolved;
  if (stored === 'light' || stored === 'dark') {
    resolved = stored;
  } else {
    /* "system" or absent → follow OS preference */
    resolved = (matchMedia('(prefers-color-scheme: dark)').matches)
      ? 'dark' : 'light';
  }
  document.documentElement.dataset.theme = resolved;

  /* Scale resolver — ICD-0.6.2 §5.3 */
  var pct = prefs['shell.scale_pct'];
  if (typeof pct !== 'number' || !Number.isInteger(pct)
      || pct < 80 || pct > 175) {
    pct = 100;
  }
  document.documentElement.style.fontSize = (pct * 0.135) + 'px';

  /* System-theme listener — only active when stored = system|absent */
  if (stored !== 'light' && stored !== 'dark') {
    var mq = matchMedia('(prefers-color-scheme: dark)');
    mq.addEventListener('change', function (e) {
      document.documentElement.dataset.theme = e.matches ? 'dark' : 'light';
    });
  }
})();
</script>
```

Sized at ~700 bytes. The CSP hash for this block is
build-time-computed and added to the kernel's `script-src`
header per §11 SC4.

---
