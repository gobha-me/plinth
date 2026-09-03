# ICD-0.6.0-frontend-shell-bootstrap

**Traces to:** architecture/06-frontend.md §1 (The Shell is an Extension
— first boot bundled-package install lifecycle is *deferred* to 0.6.1
per `DESIGN-shell-v06x.md §9` 0.6.1 entry; this ICD pins only the
in-browser frame the eventual installed shell will deliver);
architecture/06-frontend.md
§2 (`frontend.mount` URL ownership — this ICD reserves `/app/*` as the
shell's mount prefix per `architecture/05-extensions.md §2` and pins
the SPA-fallback semantics; the manifest-driven mount **declaration**
contract belongs to the 0.6.1 package-system landing, not here);
architecture/06-frontend.md §7 (Stack — Preact/htm, no build step,
single-page application loaded from the active frontend's `entry`
file; this ICD pins those choices to concrete file shapes for 0.6.0);
architecture/05-extensions.md §2 (Reserved URL Prefixes — the `/`
configurable redirect row and the `/app/*` shell mount; this ICD wires
both for 0.6.0 with a shell-baked static handler standing in for the
package-system asset path that lands in 0.6.1); architecture/01-identity.md
§1 (Identity and Authentication — the shell's login form is a thin
client over the auth surface defined in ICD-0.1.2; this ICD adds no
new auth concepts); architecture/01-identity.md §2 (Groups and RBAC —
the shell's "render only what the user can see" rule from
`DESIGN-shell-v06x.md §10` constraint #3 trivially holds for 0.6.0
because the only RBAC-gated surface is the post-login frame, gated by
session presence; per-element RBAC arrives in 0.6.4 with the
`plinth.panels` query API per `DESIGN-shell-v06x.md §11` OQ4);
ICD-0.1.2-auth-sessions §POST /api/auth/login (the shell submits
username + password, receives a session cookie via Set-Cookie, and
treats subsequent requests as authenticated; this ICD pins the
client-side cookie semantics + redirect-on-401 behaviour),
ICD-0.1.2 §POST /api/auth/logout (Sign Out wires through this
endpoint), ICD-0.1.2 §GET /api/auth/session (the "Hello, {user}"
placeholder reads the username from this endpoint after login);
ICD-0.1.5-rbac-enforcement §Filter Ordering (the shell's session
cookie is consumed by the same Drogon authentication filter as the
HTTP API, then the RBAC filter; the shell takes no special path —
its requests are just HTTP requests); `DESIGN-shell-v06x.md §9.0`
(0.6.0 milestone scope — Preact/htm scaffold, login screen, empty
topbar, "Hello, {user}", error boundary scaffolding); `DESIGN-shell-v06x.md
§2.1` (Topbar Structure — four zones: Home / app-name / tray /
avatar; this ICD reduces it to placeholders for 0.6.0); `DESIGN-shell-v06x.md
§3.6` (Error Boundaries — Preact error boundary at every panel; this
ICD pins the *top-level* boundary only, panel-scoped boundaries
arrive in 0.6.5 with the float system); `DESIGN-shell-v06x.md §10`
(Constraints for Code Sessions — the ten cross-arc rules; this ICD
honours items 1, 2, 4, 8, 9 verbatim; item 3 (RBAC-gates-everything-
visible) holds vacuously at 0.6.0 since there are no RBAC-gated UI
elements yet); `DESIGN-shell-v06x.md §11` OQ3 (Design token serving
mechanism — recorded as a 0.6.2 ICD obligation, NOT pinned here),
OQ4 (Panels query API — recorded as a 0.6.4 ICD obligation, NOT
pinned here); `docs/sketches/shell-topbar-reference.html` (visual
reference; State 1 is the canonical four-zone layout — 0.6.0 ships
the empty-frame variant of that structure).

**Depends on:** ICD-0.1.2-auth-sessions (the only kernel API the
0.6.0 shell exercises beyond the static-asset surface);
ICD-0.1.5-rbac-enforcement (the auth filter's request-context shape
the shell's redirect-on-401 logic relies on); architecture/05-extensions.md
§2 (the `/app/*` reserved prefix; this ICD's static handler claims
that prefix in advance of the 0.6.1 manifest-driven claim);
architecture/06-frontend.md (the contract owner — this ICD is the
first 0.6.x slice of that contract).

**Milestone:** 0.6.0 — Bootstrap and Frame. First code milestone of
the 0.6.x Frontend-Shell arc. Authored as paper-only follow-up
`0.5.5.N ICD-0.6.0 authoring` per METHODOLOGY-llm-assisted-development.md
§3.1 forward-ICD-presence rule and `feedback_icd_horizon.md`
(ICDs one milestone ahead).

**Status:** Paper. Authored 2026-04-27 on
`feat/0.5.5.N-icd-0.6.0-authoring`. Code session pins OQ1–OQ3 then
implements; expected 4–5 phase commit arc.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
- `docs/architecture/06-frontend.md` (the contract owner; §1–§4 are
  the substrate this ICD's 0.6.0 slice carves out from).
- `docs/design/DESIGN-shell-v06x.md` (the 0.6.x shell arc; §9.0 is
  the 0.6.0 milestone definition; §9.1–§9.6 catalogue what 0.6.1+
  adds — every item there is OUT OF SCOPE for this ICD per §16).
- `docs/design/DESIGN-packages-v04x.md §0.4.4` (first-boot bundled-
  package install — the eventual 0.6.1 substrate for replacing the
  baked-static handler this ICD pins).
- `docs/sketches/shell-design-2026-04-27/` (**canonical
  visual reference for the 0.6.x arc** — Claude Design handoff bundle
  shipped 2026-04-27; HTML/CSS/JSX prototypes plus the design-session
  chat transcript at `chats/chat1.md`. The 0.6.0 frame is a structural
  subset of `project/Plinth Shell.html` with empty zones — the
  authenticated DOM at first paint matches the topbar zone structure
  verbatim, only with extension-owned content placeholders empty
  pending 0.6.3+. Also defines: app-identity treatment (= "mark");
  Inter prose + JetBrains Mono IDs typography baseline; dark/light
  palette aimed-at values; integration-point overlay `data-ipoint`
  scheme for 0.6.4+ package-coder ergonomics).
- `docs/sketches/shell-topbar-reference.html` (older visual reference,
  superseded by the 2026-04-27 bundle; structurally compatible — the
  newer bundle refines the palette and adds extension-tray exemplars).
- `docs/icd/ICD-0.1.2-auth-sessions.md` (the auth surface the login
  form consumes; cookie semantics, error codes).
- `docs/icd/ICD-0.1.5-rbac-enforcement.md` (the filter chain the
  shell's HTTP requests pass through).
- `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md` (the
  most-recent ICD; format reference for §-numbering, Glossary,
  test-case prefix convention).

---

## Overview

ICD-0.6.0 pins the in-browser frame the Plinth shell delivers at the
first UI milestone. It is deliberately the smallest possible surface
that satisfies the four `DESIGN-shell-v06x.md §9.0` exit criteria:
"Shell renders, user can log in, empty frame displays, avatar shows
username and sign-out works."

Three things land:

1. **A static-asset handler at `/app/*`** that serves a baked-in
   Preact/htm bundle. The handler stands in for the 0.6.1 package-
   system asset lifecycle described in `architecture/06-frontend.md
   §3`; its URL contract is forward-compatible with that lifecycle so
   the 0.6.1 transition is a substitution at the handler layer, not a
   protocol change. The kernel root redirect `/` → `/app/` per
   `architecture/05-extensions.md §2` activates in this milestone.

2. **A login flow.** The shell renders a login form when an
   unauthenticated user reaches `/app/`. The form POSTs to
   `/api/auth/login` per ICD-0.1.2; on 200 the browser gets the
   `plinth_session` cookie and the shell re-renders the authenticated
   frame; on 401 the shell shows the inline error code from the auth
   ICD; redirect-on-401 from any subsequent fetch returns the user to
   the login form without losing the cookie path. Sign Out wires to
   `/api/auth/logout` and clears the cookie.

3. **An empty topbar plus a top-level error boundary.** Four zones —
   Home / app-name / tray / avatar — render as structural placeholders
   per `DESIGN-shell-v06x.md §2.1`. The avatar zone shows the
   authenticated username read from `/api/auth/session` and exposes a
   Sign Out action. The content area shows "Hello, {user}". A
   Preact error boundary wraps the entire root and catches any
   unhandled exception below the auth layer.

What this ICD does **not** do:

- No `frontend.mount` manifest contract (lands in 0.6.1).
- No bundled-package install lifecycle on first boot (lands in 0.6.1).
- No `ext_shell` PG schema (lands in 0.6.1).
- No design tokens, theme, or UI scaling (lands in 0.6.2).
- No panel SDK (lands in 0.6.3).
- No tab strip, sub-tabs, app-switcher dropdown, or Home launcher
  (lands in 0.6.4).
- No floats, tray popovers, content-type resolution, navigation
  intents (lands in 0.6.5/0.6.6).
- No realtime / WebSocket subscriptions (the shell uses no realtime
  surface in 0.6.0; that arrives with the panel SDK in 0.6.3).
- No admin extension preview (the entire `0.6a-*` parallel track is
  out of scope; admin extension is a second bundled package whose
  earliest possible slot is 0.6a-A, gated on 0.4.x + 0.6.3).

§16 *What Must Not Be Decided Yet* fences each of the above with the
named milestone that closes it.

The 0.6.0 surface is small enough that the test-case taxonomy is
proportionally lighter than ICD-0.5.4 (41 cases) or ICD-0.5.5
(36 cases). ~22 test cases land across §13's five groups, plus an
end-to-end browser-driven integration walkthrough.

---

## Glossary

This ICD uses several terms from `DESIGN-shell-v06x.md` and
`architecture/06-frontend.md` with narrowed semantics for the 0.6.0
slice. The terms below are pinned for the rest of the document and
the implementing code.

- **shell** — the bundled reference frontend extension defined in
  `architecture/06-frontend.md §1`. In 0.6.0 the shell exists only as
  baked-in static assets behind the kernel's `/app/*` handler; it is
  *not* yet a row in `plinth.packages` (that lands in 0.6.1). Code
  that special-cases the shell at the kernel layer beyond the
  static-asset handler is a bug per `DESIGN-shell-v06x.md §10`
  constraint #1.

- **bundle** — the shell's `index.html` + a single ES-module bundle
  file (`shell.js` per §4.2) + an optional CSS file. No build step;
  source files ship as-is per `architecture/06-frontend.md §3`. The
  bundle is consumed verbatim by the browser; `htm` provides
  template-literal JSX without a transpiler step.

- **frame** — the empty topbar + content area + error boundary that
  renders for an authenticated user in 0.6.0. Distinct from "panel"
  (a panel is an extension-provided component tree rendered into a
  shell-managed container; panels arrive with the SDK in 0.6.3).

- **four zones** — the topbar layout from `DESIGN-shell-v06x.md
  §2.1`: Home (leftmost), app-name (chevron + text), tray (extension-
  provided icons + shell-owned bell + avatar), avatar (rightmost,
  pinned). In 0.6.0 each zone exists as a structural placeholder; only
  the avatar zone has interactive content (the Sign Out menu).

- **redirect-on-401** — the client-side response to any kernel HTTP
  request returning 401 with `not_authenticated` / `session_expired`
  / `session_revoked` (per ICD-0.1.2 §Authentication Mechanism). The
  shell hops back to the login form, preserving any in-flight
  navigation context as URL state. Distinct from a server-issued
  HTTP 302 redirect.

- **error boundary** — the top-level Preact error boundary that wraps
  the entire root component tree. Catches any synchronous exception
  thrown during render or any unhandled promise rejection during
  lifecycle. Per `DESIGN-shell-v06x.md §3.6` the boundary renders a
  fallback, logs via the kernel audit/logging API once that surface
  exists, and does not auto-retry. In 0.6.0 the audit-log call is a
  console.error (kernel log surface for the shell consumer arrives
  with the panel SDK in 0.6.3).

- **`/app/`** — the shell's mount prefix per
  `architecture/05-extensions.md §2`. In 0.6.0 the kernel
  static-handler claims this prefix directly (no manifest); in 0.6.1+
  the claim is mediated by the shell's `frontend.mount` declaration.

---

## §4 — Bootstrap entry HTML and Preact/htm scaffold

### 4.1 Static-asset handler at `/app/*`

The kernel registers a static-asset handler bound to the `/app/*`
prefix. The handler:

- Returns `index.html` for any path under `/app/` that does not
  match a known asset filename (the standard SPA-fallback pattern).
- Returns the requested file for paths matching baked-in asset names
  — at minimum `shell.js`, optionally `shell.css`. Asset names are
  hardcoded; no dynamic discovery.
- Returns 404 for paths matching reserved prefixes (`/api/`, `/ext/`,
  `/s/`, `/healthz`, `/metrics`, `/ws`) — those are kernel territory
  per `architecture/05-extensions.md §2` and the static handler
  must not shadow them. The `/app/*` registration ordering in Drogon
  must run **after** the kernel API filters; the SPA-fallback never
  sees an `/api/*` path.
- Sets `Cache-Control: no-cache` on `index.html` (so a shell upgrade
  takes effect on next page load without a hard reload) and
  `Cache-Control: public, max-age=31536000, immutable` on the bundle
  files (consistent with `architecture/06-frontend.md §3`'s
  immutable-asset semantics; in 0.6.0 the immutability holds because
  the bundle ships baked into the kernel binary).
- Sets the strict CSP `script-src 'self'; style-src 'self'
  'unsafe-inline'; connect-src 'self'` on every response, per
  `architecture/06-frontend.md §3`. No external sources.

### 4.2 Bundle layout

The 0.6.0 bundle is **three files** at most:

| File | Purpose | Size budget |
|------|---------|------------:|
| `index.html` | SPA entry; loads `shell.js` as `<script type="module">`; declares root `<div id="root">`; minimal inline CSS (~50 lines) for layout-before-JS-loads only | < 4 KB |
| `shell.js` | Single ES module; imports Preact + htm via specifier, renders the root component tree | < 80 KB pre-gzip |
| `shell.css` | Optional. Empty placeholder file or absent in 0.6.0; design tokens land in 0.6.2 | n/a |

Preact and htm ship inside `shell.js` — vendored as source. **No
import map**. The 0.6.2 design-token serving (with `/api/frontend/*`
indirection per `architecture/06-frontend.md §4`) introduces the
import-map binding; until then, every `import` in `shell.js` resolves
to a relative path inside the file.

### 4.3 Bake-time storage of bundle files

The bundle files (`index.html`, `shell.js`) ship inside the kernel
binary as embedded resources. **OQ1** records the choice between
embedded-resource (preferred) versus shipped-alongside-the-binary
directory; the recommendation is embedded so deployment ships a
single binary.

### 4.4 Boot sequence (browser-side)

When the browser navigates to `/app/`:

1. Kernel returns `index.html` (200, `text/html`, `Cache-Control:
   no-cache`, CSP header).
2. Browser parses HTML, requests `shell.js` (200, `application/
   javascript`, immutable cache).
3. `shell.js` evaluates. The module's top-level code:
   1. Imports Preact `h` + `render` + `Component`, htm `html`.
   2. Defines `App` component (root).
   3. Calls `render(html\`<App />\`, document.getElementById('root'))`.
4. `App` mounts. Its `componentDidMount` (or the equivalent
   functional `useEffect` if Preact hooks are used) issues `GET
   /api/auth/session`.
5. The fetch's response determines initial route:
   - **200**: render the authenticated frame (§6, §7 placeholder
     content).
   - **401** (`not_authenticated`): render the login form (§5).
   - **5xx / network error**: render an error state with retry button
     (boundary fallback per §7; the boundary catches network errors
     wrapped as a thrown exception inside the App's effect).

`shell.js` exposes no globals beyond what Preact + htm define. The
shell's panel SDK injection mechanism (`DESIGN-shell-v06x.md §4`)
does not exist yet in 0.6.0.

### 4.5 Root redirect `/` → `/app/`

Per `architecture/05-extensions.md §2` reserved-prefix table, `/` is
configurable and redirects to the active frontend's mount. In 0.6.0:

- `GET /` returns HTTP 302 to `/app/`.
- The redirect is unconditional; no logged-in vs. logged-out branching
  at the kernel layer (the shell handles both cases client-side).

In 0.6.1, the same redirect is mediated by the kernel's "active
frontend" lookup against `plinth.packages` (per
`architecture/06-frontend.md §2.2`); 0.6.0's hardcoded redirect is
the simplest conforming form per METHODOLOGY *Caller-Triggered
Implementation*.

---

## §5 — Login flow

### 5.1 Form contract

The login form renders when `GET /api/auth/session` returns 401.
It contains:

- One `<input type="text" name="username" required>` field.
- One `<input type="password" name="password" required>` field.
- One submit `<button>` ("Sign In").
- One inline error region that displays the most recent submit's
  error code (translated to a user-readable string per §5.4).

The form is a controlled Preact component; on submit it preventDefault
and dispatches the request body via `fetch`.

### 5.2 Submit flow

1. `fetch('/api/auth/login', { method: 'POST', credentials:
   'include', headers: { 'Content-Type': 'application/json' },
   body: JSON.stringify({ username, password }) })`.
2. **200**: the browser stores the `plinth_session` cookie (set by
   the server's `Set-Cookie` header). The shell:
   - Re-issues `GET /api/auth/session` to populate user-context state.
   - On 200, transitions to the authenticated frame.
3. **400 / 401**: the shell extracts `body.error` (one of
   `missing_username`, `missing_password`, `invalid_credentials`,
   `account_disabled`, `username_too_short`, `username_invalid_chars`,
   `password_too_short`, `username_taken`, `registration_disabled`
   per ICD-0.1.2) and renders the corresponding string into the
   inline error region. The form remains; the password field is
   cleared.
4. **429** (`rate_limited`): the shell shows the rate-limit message
   and disables the submit button for the `retry_after` window
   reported in the response body.
5. **5xx / network error**: the shell renders a generic
   "Cannot reach server" state with a Retry button.

### 5.3 Cookie semantics (client-side)

The shell never reads the `plinth_session` cookie value (it cannot —
the cookie is `HttpOnly`). It relies on the browser to attach the
cookie automatically when `fetch` runs with `credentials: 'include'`
on same-origin requests.

The shell **must** issue every kernel-API fetch with
`credentials: 'include'`. A future-proof helper at the top of
`shell.js` wraps `fetch` with this default.

### 5.4 Error-code → user-string mapping

| Error code | User-facing string |
|------------|--------------------|
| `missing_username` | "Username is required." |
| `missing_password` | "Password is required." |
| `invalid_credentials` | "Username or password is incorrect." |
| `account_disabled` | "This account is disabled." |
| `rate_limited` | "Too many attempts. Try again in {retry_after} seconds." |
| `username_too_short` | "Username must be at least 3 characters." |
| `username_invalid_chars` | "Username may only contain letters, numbers, underscores, and hyphens." |
| `password_too_short` | "Password is too short." |
| `username_taken` | "That username is already taken." |
| `registration_disabled` | "Registration is disabled on this server." |
| any other code | "Sign-in failed. ({code})" — the code is shown verbatim so kernel-side audit can correlate. |

Localization is out of scope for 0.6.0; strings ship hard-coded in
English. A localization arc is unscheduled.

### 5.5 Sign Out flow

The avatar zone exposes a "Sign Out" action (the only avatar-popover
content in 0.6.0; the full 5-item popover from
`DESIGN-shell-v06x.md §3.4` arrives in 0.6.6 with the tray system).
On click:

1. `fetch('/api/auth/logout', { method: 'POST', credentials:
   'include' })` (per ICD-0.1.2 §POST /api/auth/logout).
2. The browser receives `Set-Cookie: plinth_session=; Max-Age=0` and
   discards the session cookie.
3. The shell redirects to the login form (re-renders the same root
   component tree with the unauthenticated branch).

Sign Out failures (network errors, 5xx) surface in the boundary
fallback; no silent-swallow per `DESIGN-shell-v06x.md §3.6`.

### 5.6 Redirect-on-401 (post-login fetches)

Any subsequent kernel-API fetch beyond `/api/auth/login` that returns
401 (e.g., session expired mid-flow) triggers the shell's
redirect-on-401 path:

- The fetch wrapper (§5.3) intercepts 401 responses globally.
- It clears in-memory user context and re-renders the login form.
- The form shows the inline message "Your session has expired.
  Please sign in again." (mapped from the `session_expired`,
  `session_revoked`, or `not_authenticated` error code).

The shell does **not** auto-retry the failed request after re-login.
That contract is harder than it looks (which queued requests are
safe to replay? what if they had side effects?) and is out of
scope for 0.6.0. The user re-issues the action manually.

---

## §6 — Empty topbar four-zone layout

### 6.1 Structural HTML

The topbar is a single `<header>` element holding four child zones in
this order:

```html
<header class="topbar">
  <div class="zone zone-home"><!-- Home icon placeholder --></div>
  <div class="zone zone-app-name"><!-- App name placeholder --></div>
  <div class="zone zone-tray"><!-- Tray placeholder (empty) --></div>
  <div class="zone zone-avatar"><!-- Avatar + Sign Out --></div>
</header>
```

This matches the `docs/sketches/shell-topbar-reference.html` State 1
structure with all dynamic content replaced by placeholders.

### 6.2 Per-zone content (0.6.0)

| Zone | Content | Notes |
|------|---------|-------|
| `zone-home` | A static SVG home icon (no click handler). | Click handler arrives in 0.6.4 with the Home view + launcher. |
| `zone-app-name` | The "mark" identity treatment per the 2026-04-27 design bundle: a small colored glyph (the bundle's `IdentityMark` shape — square `--r-md` swatch with the active extension's accent color) + the static text "Plinth" + a decorative chevron. **No click handler in 0.6.0.** | The live app-switcher dropdown (chevron-driven) arrives in 0.6.4 with the active-extension lookup; the 0.6.0 mark is a visual placeholder for that surface. |
| `zone-tray` | Empty `<div>`. | Tray panel rendering arrives in 0.6.6. |
| `zone-avatar` | Avatar circle (gradient background, first character of username) + chevron. Click toggles a popover holding only the Sign Out action. | Full avatar popover (theme toggle, scale slider, User Settings, identity block) arrives in 0.6.6 — `DESIGN-shell-v06x.md §3.4`. |

### 6.3 Layout semantics (CSS)

The 0.6.0 CSS is intentionally minimal — design tokens, theme, and
scaling all arrive in 0.6.2. The 0.6.0 stylesheet (inline in
`index.html`) covers:

- `.topbar` flex row, fixed 48px height, left-to-right zone order.
- `.zone-tray` `flex: 1` so the avatar zone pins right.
- A neutral dark palette (hardcoded hex values aimed at the
  2026-04-27 design bundle's dark-default values — `--bg-0:
  #0b0f14`, `--bg-1: #11161d`, `--text-0: #e7edf4`, `--text-1:
  #c4cbd4`, `--accent: #5aa9ff`. Hardcoded as throwaway scaffolding;
  CSS custom properties land in 0.6.2). Light-mode palette (warm
  off-white `#fcfcfa` / ink `#16181c` / accent `#1f6feb`) is **NOT**
  shipped in 0.6.0 — there is no theme switcher yet.
- Two `@font-face`-equivalent declarations citing the design
  bundle's typography baseline:
  - **Inter** for prose (`html, body { font-family: 'Inter',
    system-ui, sans-serif; }`).
  - **JetBrains Mono** for structural IDs — a `.mono` utility class
    used wherever a string is a kernel-namespace identifier
    (capability strings, schema names, version numbers, package IDs).
    0.6.0 has effectively no IDs to render in the empty frame; the
    class ships for forward-compat with the 0.6.3+ panel SDK.
  - Web fonts ship same-origin from the bundled assets, NOT from
    `fonts.googleapis.com` — the design bundle's CDN reference is
    the prototype; the 0.6.0 production bundle inlines or
    self-hosts both fonts to honour the strict CSP from §11
    (`script-src 'self'`, no external sources).
- `body { font-size: 13.5px; }` baseline per the design bundle.

The CSS is **NOT** a design-token contract. Code sessions in 0.6.2
WILL replace every hex value here with a `var(--token-name)`
reference; 0.6.0 hex values are throwaway scaffolding per the
*Caller-Triggered Implementation* pattern. The palette + typography
baseline above is a **direction-of-travel commitment**, not a
contract — code sessions are free to land equivalent values that
match the design bundle's visual.

### 6.4 "Hello, {user}" content area

Below the topbar, the content area renders a single `<main>` element
with one heading: `Hello, {username}`, where `{username}` is the
value from `GET /api/auth/session`. No other content. No styling
beyond centred text.

This is the simplest possible "shell renders, user can log in"
exit-criterion-satisfier per `DESIGN-shell-v06x.md §9.0`. The content
area's eventual home — primary panel container, dispatching by active
extension — arrives in 0.6.3/0.6.4.

### 6.5 No mobile / responsive treatment

The 0.6.0 frame ships **desktop-only** layout. The mobile / responsive
behaviour catalogued in `DESIGN-shell-v06x.md §3.8` (sub-tab
horizontal scroll, popover → bottom sheet, float → full-screen modal)
arrives milestone-by-milestone with the surfaces that need it. The
0.6.0 frame is unaffected by viewport — it has nothing that needs to
collapse. A 320px viewport renders the same four zones.

---

## §7 — Top-level error boundary

### 7.1 Boundary placement

A single Preact error boundary wraps the root `App` component. There
are no nested boundaries in 0.6.0 — panel-scoped boundaries arrive
with the float system in 0.6.5 per `DESIGN-shell-v06x.md §3.6`.

### 7.2 What the boundary catches

- Synchronous exceptions thrown during `render()`.
- Synchronous exceptions thrown from lifecycle methods.
- Errors propagated from descendants via Preact's
  `componentDidCatch` mechanism.

It does **not** catch:

- Unhandled promise rejections from `fetch` calls inside effects
  (those are handled via try/catch in the originating effect; the
  fetch wrapper from §5.3 is the canonical catch site).
- Errors during `App` itself's first render before the boundary
  mounts (Preact's documented limitation; the static `index.html`
  must render *something* even if `shell.js` fails to evaluate at
  all — the inline CSS handles that).

### 7.3 Fallback render

When the boundary catches, it renders:

```
+--------------------------------------------------+
|  Something went wrong.                           |
|  [Reload]                                        |
+--------------------------------------------------+
```

Styling matches the topbar's neutral dark palette. Reload triggers
`window.location.reload()` (full page reload — re-fetches `index.html`,
re-evaluates `shell.js`).

### 7.4 Logging

The boundary's `componentDidCatch(error, info)` calls `console.error`
with the error and a structured payload:

```js
console.error('[shell] boundary caught', { error, info, route });
```

Where `route` is `'login'` or `'authenticated'`. **No kernel-side
audit-log call.** The shell-to-kernel audit channel
(`audit.log` per `DESIGN-logging-subsystem.md`) ships with the panel
SDK in 0.6.3; 0.6.0 logs to the browser console only. This is an
intentional deferral, not a deviation — see §16.

---

## §8 — Static asset serving for the 0.6.0 frame

This section pins the kernel-side handler that backs §4. The
0.6.1 milestone replaces this handler with the package-system asset
lifecycle (`/ext/{name}/{version}/*` per `architecture/06-frontend.md
§3`); the 0.6.0 contract is forward-compatible at the URL layer so
the substitution requires no client-side change.

### 8.1 Drogon registration

The kernel registers two route handlers at boot:

1. `GET /` → 302 to `/app/` (no auth, no RBAC).
2. `GET /app/{path:.*}` → SPA-fallback handler (no auth, no RBAC; the
   handler returns static assets that themselves carry CSP +
   credentialed-fetch semantics).

The `/app/{path:.*}` handler must register **after** all `/api/*`
handlers per Drogon's first-match dispatch — the catch-all glob would
otherwise shadow API paths. CI verification: a smoke test asserts
`GET /api/auth/session` returns 401 (not 200 with `index.html`).

### 8.2 Handler logic

```
handler(request, callback):
  path = request.path.substr(len("/app/"))  // strip prefix
  if path == "" or no extension or path matches no baked asset:
    callback(serve(index_html, "text/html",
                   cache: "no-cache",
                   csp: STRICT_CSP))
    return
  match path:
    case "shell.js":
      callback(serve(shell_js, "application/javascript",
                     cache: "public, max-age=31536000, immutable",
                     csp: STRICT_CSP))
    case "shell.css":
      callback(serve(shell_css, "text/css",
                     cache: "public, max-age=31536000, immutable",
                     csp: STRICT_CSP))
    default:
      callback(404)
```

The "no extension" branch is the SPA-fallback heuristic — paths like
`/app/login` (no `.` in the last segment) return `index.html` so
client-side routing works on reload.

### 8.3 Assets storage

Per **OQ1**, the recommended storage form is embedded resources baked
into the kernel binary (e.g., via CMake `configure_file` + a `const
char*` constant, or an equivalent mechanism). Alternatives:
(a) `extern "C"` symbol linked from object file converted from the
asset bytes (`xxd -i` style); (b) shipped-alongside directory read at
boot. Both are mechanically valid; embedded is preferred so a single
binary deploys.

The 0.6.0 bundle is small enough (~80 KB total) that any reasonable
embedding strategy is sub-second to read at startup.

### 8.4 What this handler does NOT do

- No content negotiation (no `Accept` parsing, no gzip on-the-fly —
  the kernel serves the raw file body; CDN/reverse-proxy compresses).
- No ETag / `If-None-Match` / `If-Modified-Since` logic — the
  immutable cache headers make conditional GET unnecessary, and the
  no-cache `index.html` is small enough that revalidation is cheap.
- No range requests (assets are tiny; HTTP `Range` is for media).
- No serving of unknown asset names — every served path is in the
  hardcoded set. No directory listing. No path traversal surface
  (the strip-prefix + named-asset match avoids `..`-style attacks
  by construction).

---

## §9 — Configuration surface

### 9.1 New `Config::Shell` block (kernel-side)

The 0.6.0 milestone adds a single config block:

```cpp
struct Config {
  // ... existing blocks ...
  struct Shell {
    bool enabled = true;        // serve /app/* and / redirect
    std::string root_redirect = "/app/";
                                 // overridable for future BYO frontend
                                 // (architecture/06-frontend.md §6);
                                 // value MUST start with '/' and end with '/'
  } shell;
};
```

The block is trivially default-constructible (no `std::string` non-
default state); `Config::Shell` instances live alongside the existing
`Config::Realtime` blocks at static-storage scope without tripping
`cert-err58-cpp`.

### 9.2 Operator settings

| Key | Default | Purpose |
|-----|--------:|---------|
| `shell.enabled` | `true` | Disable to deploy kernel as headless API only (BYO frontend territory per `architecture/06-frontend.md §6`). When `false`, neither `/` redirect nor `/app/*` handler registers. |
| `shell.root_redirect` | `/app/` | Override the root redirect target. When `shell.enabled=false`, this still controls `/`. Validation: must match `^/[^/]+/$` (single-segment trailing slash). Invalid values fall back to `/app/`. |

### 9.3 Build-time constants

Two build-time constants:

| Constant | Definition | Notes |
|----------|------------|-------|
| `SHELL_VERSION` | `"0.6.0"` (literal) | Forward-compat — the 0.6.1 install lifecycle reads this to seed the `plinth.packages` row. Today, the shell does not consume this string. |
| `SHELL_BUNDLE_PATH` | Build-system-resolved path to the bundle source directory | Used at compile time only (resource embedding); not read at runtime. |

### 9.4 No environment-variable overrides

The 0.6.0 shell needs no environment-variable surface. Operator
overrides flow through the kernel's existing TOML config plumbing.

---

## §10 — Audit events

The 0.6.0 milestone introduces **no new audit families**. Every
audit-loggable event in the 0.6.0 surface is already emitted by the
auth subsystem:

- `user.login` (success / failure) — emitted by ICD-0.1.2's login
  handler.
- `user.logout` — emitted by ICD-0.1.2's logout handler.
- `session.revoked` — emitted by ICD-0.1.2 / 0.1.4.

Boundary-caught errors (§7.4) log to the browser console only.
Promotion to a kernel-side `frontend.boundary.caught` audit family
is deferred to 0.6.3 when the panel SDK exposes `audit.log` to client
code per `DESIGN-logging-subsystem.md`.

_Promoted by ICD-0.6.3 (paper authored 2026-04-29 on
`feat/0.6.2.N-icd-0.6.3-authoring`); shipped in v0.6.3 (2026-04-30
on `feat/0.6.3-panel-sdk-client-sdk`). Boundary emission goes via a
single-purpose capability `shell.audit.emit` rather than direct
`audit.log` exposure — see ICD-0.6.3 §6 + §10 SC2 for the
forgery-prevention rationale. Audit action shipped as
`ext.shell.frontend.boundary.caught` (the `ext.` prefix is required
by `audit.log()` per ICD-0.6.3 §17 deviation #7); capability name
shipped as `shell.audit.emit` (CF7 + rule-regex constraints; see
ICD-0.6.3 §17 deviations #5 + #6)._

The static-asset handler (§8) emits no audits. Static-asset 404s are
ordinary HTTP 404s; they belong on the access log, not the audit log.

---

## §11 — Security constraints

### 11.1 Non-negotiable constraints

1. **Strict CSP on every static-handler response.** `script-src
   'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'`. No
   exception for development. CSP violations indicate either a bug
   in the bundle (an inline `<script>` tag, an external URL) or an
   attempt to inject; both must visibly fail rather than silently
   succeed.
2. **No inline `<script>` tags in `index.html`.** All JS lives in
   `shell.js`, loaded via `<script type="module" src="shell.js">`.
   `style-src 'unsafe-inline'` exists only for the minimal layout-
   before-JS-loads CSS in `<style>` blocks; that surface is bounded
   and reviewed per release.
3. **HttpOnly + Secure + SameSite=Strict cookies.** The shell never
   reads `plinth_session` directly; relies on browser-managed cookie
   attachment. Per ICD-0.1.2, the kernel sets these flags on
   `Set-Cookie`; the shell adds nothing.
4. **`credentials: 'include'` on every fetch.** The fetch wrapper
   from §5.3 enforces this; raw `fetch` calls bypassing the wrapper
   are a code-review red flag.
5. **No `eval`, no `new Function`, no `dangerouslySetInnerHTML`-
   style escape hatches.** Preact + htm renders are tagged-template
   based; user-supplied strings go through Preact's standard text-
   content path, which escapes. The username displayed in
   "Hello, {user}" is a text node, not innerHTML.
6. **No third-party origins in `<link>`, `<script>`, `<img>`, or
   `fetch` URLs.** All assets are same-origin via the static
   handler. Web fonts arrive in 0.6.2 and ship same-origin under
   `/api/frontend/fonts/` per `architecture/06-frontend.md §4`.
7. **Path-traversal closed by construction.** §8.2's strip-prefix +
   named-asset match means no user-supplied substring reaches the
   filesystem. Even with embedded resources, this is the right
   habit for the 0.6.1 transition.

### 11.2 Threat model deltas vs. ICD-0.1.2

The 0.6.0 shell adds no new attack surface beyond what ICD-0.1.2
already defends. Specifically:

- The login form is a thin client over the auth API; same rate
  limiting (5 failed attempts / IP / minute), same constant-time
  Argon2id verification, same standardized error shape.
- The static handler is read-only and serves a fixed set of files;
  no upload, no parameter injection, no SQL / OS-command path.
- The Sign Out action revokes via the existing `/api/auth/logout`
  handler; no new revocation primitive.

### 11.3 What the security review at the 0.6.0 PR boundary checks

- CSP header present on all `/app/*` responses.
- No `<script src="https://...">` or `<link rel="stylesheet"
  href="https://...">` anywhere in `index.html`.
- No `eval` / `Function` / `setTimeout(string, ...)` /
  `setInterval(string, ...)` in `shell.js`.
- The fetch wrapper (§5.3) is used by every kernel-API request.
- The username rendered in the topbar / content area passes
  through Preact's text-node path, not raw HTML insertion.

---

## §12 — Test cases

Test cases follow the ICD-0.5.4 / ICD-0.5.5 prefix convention. Five
prefix groups land in 0.6.0:

- **B.\*** — bootstrap (the static handler + boot sequence)
- **L.\*** — login flow
- **T.\*** — topbar four-zone layout
- **E.\*** — error boundary
- **I.\*** — integration (browser-driven end-to-end)

Total: ~22 cases. Lighter than ICD-0.5.4 (41) / ICD-0.5.5 (36) per
the narrower 0.6.0 surface.

### Bootstrap — `tests/kernel/shell/static_handler_test.cpp` (new TU)

| # | Type | Scenario | Expected outcome |
|---|------|----------|------------------|
| B.01 | Happy | `GET /` | 302; `Location: /app/` |
| B.02 | Happy | `GET /app/` | 200; `Content-Type: text/html`; body matches embedded `index.html`; `Cache-Control: no-cache`; CSP header present |
| B.03 | Happy | `GET /app/shell.js` | 200; `Content-Type: application/javascript`; body matches embedded `shell.js`; `Cache-Control: public, max-age=31536000, immutable`; CSP header present |
| B.04 | Happy | `GET /app/some/deep/spa/path` (no extension) | 200; body matches `index.html` (SPA fallback) |
| B.05 | Sad | `GET /app/missing.png` | 404 |
| B.06 | Sad | `GET /api/auth/session` (handler-ordering check) | NOT shadowed by `/app/*` glob; reaches the auth handler (returns 401 if unauth, 200 if auth) |
| B.07 | Config | `shell.enabled=false`, `GET /app/` | 404 (handler not registered) |
| B.08 | Config | `shell.root_redirect=/console/`, `GET /` | 302 to `/console/` |
| B.09 | Config | `shell.root_redirect=invalid`, `GET /` | 302 to `/app/` (fallback) |

### Login — `tests/kernel/shell/login_e2e_test.cpp` (new TU; uses HTTP test fixture from `0.5.x.N HTTP test harness` if available, else minimal Drogon HTTP harness)

| # | Type | Scenario | Expected outcome |
|---|------|----------|------------------|
| L.01 | Happy | Submit valid credentials | 200; `Set-Cookie: plinth_session=...; HttpOnly; Secure; SameSite=Strict`; subsequent `GET /api/auth/session` returns user object |
| L.02 | Sad | Submit wrong password | 401; `body.error == "invalid_credentials"`; cookie not set |
| L.03 | Sad | Submit empty username | 400; `body.error == "missing_username"` |
| L.04 | Sad | Submit empty password | 400; `body.error == "missing_password"` |
| L.05 | Sad | 6th rapid failed attempt from same IP | 429; `body.error == "rate_limited"`; `body.retry_after` present |
| L.06 | Happy | Sign Out after successful login | 200; `Set-Cookie: plinth_session=; Max-Age=0`; subsequent `GET /api/auth/session` returns 401 |

The login tests are kernel-side HTTP exercises — they validate the
wire contract between the shell and the auth API, not browser
behaviour. Browser-driven coverage lives under I.\*.

### Topbar — `tests/shell/topbar_render_test.html` (new browser test, runs under headless Chromium / Playwright if available; else manual smoke)

| # | Type | Scenario | Expected outcome |
|---|------|----------|------------------|
| T.01 | Happy | Render after `GET /api/auth/session` returns 200 | Four `.zone` elements present in DOM in order: home, app-name, tray, avatar |
| T.02 | Happy | Avatar zone shows username's first character | `.zone-avatar .avatar-circle` text content matches `username[0]` (uppercased) |
| T.03 | Happy | Click avatar → popover renders Sign Out | `.zone-avatar` click toggles a popover with one `[role="menuitem"]` whose text is "Sign Out" |
| T.04 | Happy | "Hello, {user}" renders in content area | `<main>` text content matches `Hello, {username}` |
| T.05 | Happy | Tray zone is empty | `.zone-tray` has zero child element nodes |

T.\* are best-effort under whatever browser harness is already
available in-tree at 0.6.0 ship; if no harness exists, the cases
ship as documented manual smoke scripts and a 0.6.0.N follow-up
backfills the headless-browser harness. Document the state in the
ship CHANGELOG entry per METHODOLOGY §Phase 2 Constraint #4.

### Error boundary — `tests/shell/boundary_render_test.html` (new browser test, same harness as T.\*)

| # | Type | Scenario | Expected outcome |
|---|------|----------|------------------|
| E.01 | Sad | A test-only "make-me-throw" component renders inside `App` | The boundary fallback renders ("Something went wrong" + Reload button); `console.error` is called with `[shell] boundary caught` payload |
| E.02 | Sad | Boundary fallback's Reload button click | `window.location.reload()` invoked (mockable via Playwright) |

### Integration — `tests/shell/login_walkthrough_test.html` (new end-to-end browser test)

| # | Type | Scenario | Expected outcome |
|---|------|----------|------------------|
| I.01 | Happy | Navigate to `/`, get redirected to `/app/`, see login form, submit valid credentials, see authenticated frame with username and topbar | All transitions occur in expected order; final DOM contains the four zones + "Hello, {user}" |
| I.02 | Happy | Sign Out from authenticated frame, return to login form | Cookie cleared (`document.cookie` does not contain `plinth_session`); login form re-renders |

### Test seam notes

- B.\* uses the existing Drogon HTTP test fixture pattern; no new
  C++ harness required.
- L.\* depends on the auth surface being available in test (existing
  fixtures from `tests/kernel/auth_*_test.cpp` cover seeding).
- T.\* / E.\* / I.\* are browser-driven. The 0.6.0 milestone may
  ship without a headless-browser harness in CI if the existing
  Catch2 + tests/util harness does not include one; in that case the
  cases ship as documented manual smoke scripts and the harness
  build is a 0.6.0.N follow-up. **OQ2** records this choice.

### Test count totals

- B.\* — 9 cases
- L.\* — 6 cases
- T.\* — 5 cases
- E.\* — 2 cases
- I.\* — 2 cases
- **Total: 24 cases**

### CI wiring

- `src/kernel/shell/static_handler.{hpp,cpp}` — new TU for §8.
- `src/kernel/shell/embedded_assets.{hpp,cpp}` — new TU embedding
  the bundle (per OQ1 — embedded resource is the recommended form).
- `src/kernel/config.hpp` — add `Config::Shell` block per §9.1.
- `src/kernel/main.cpp` — register the static handler and root
  redirect at boot (handler-ordering pinned per §8.1).
- `client/shell/index.html` — new bundle file.
- `client/shell/shell.js` — new bundle file (Preact + htm vendored).
- `tests/kernel/shell/static_handler_test.cpp` — new TU.
- `tests/kernel/shell/login_e2e_test.cpp` — new TU.
- `tests/shell/{topbar_render,boundary_render,login_walkthrough}_test.html`
  — new browser-driven test files (manual smoke or harness-driven
  per OQ2).

The `client/` directory at the repo root is the home for shell
source files. The build system embeds them at compile time per §8.3.

---

## §13 — Entry / Exit Criteria

### Entry criteria

- v0.5.5.2 shipped on `main`. Confirmed 2026-04-27.
- ICD-0.6.0 (this document) merged to `main` via paper-only PR.
- Architect has resolved OQ1–OQ3 in writing (PR review or follow-up
  commit on the implementation branch).

### Exit criteria

The 0.6.0 code milestone ships when all of the following are true:

1. **All 24 test cases pass.** B.\*, L.\*, T.\*, E.\*, I.\*.
   Deferrals to a follow-up `0.6.0.N` are permitted only with
   architect sign-off (mirrors the 0.5.4 D.08/I.02/I.03 deferral
   pattern); the most likely deferral is the headless-browser
   harness — T.\* / E.\* / I.\* may ship as documented manual
   smoke scripts pending OQ2 resolution.

2. **Static handler registered with correct ordering.** B.06
   passes — `/api/*` is not shadowed by `/app/{path:.*}`.

3. **Boot sequence demonstrably completes in a browser.** A manual
   walkthrough (or I.01 if the harness is in place) with a freshly
   seeded admin user shows: `/` → `/app/` → login form →
   credentials → authenticated frame with "Hello, admin" and
   four zones.

4. **Sign Out works.** L.06 + I.02 pass.

5. **CSP enforced and observed.** Browser DevTools / response
   headers show CSP header on every `/app/*` response; no CSP
   violations in the console during the walkthrough.

6. **Architecture promotion.** `architecture/06-frontend.md §1`
   gains an "Implemented 2026-04-XX (v0.6.0). Initial frame pinned
   in ICD-0.6.0; bundled-package install lifecycle still pending
   in 0.6.1" footnote (mirrors the 0.5.4 §3.5 promotion pattern).

7. **CHANGELOG entry.** `docs/CHANGELOG.md` gets a new dated entry
   above the v0.5.5.2 entry summarizing what shipped, the static-
   handler approach, the discharged DESIGN-shell-v06x §9.0 exit
   criteria, and any documented pseudocode deviations.

8. **Memory updates.** `project_plinth_state.md` gains a v0.6.0
   entry; `project_next_session_post_055N.md` (or equivalent) is
   replaced with `project_next_session_post_060.md` pointing at
   the next work — likely **0.6.1 Shell schema and user
   preferences** (which finally lands the bundled-package install
   lifecycle deferred from 0.6.0).

### Non-exit criteria (NOT required for ship)

- Bundled-package install lifecycle. Out of scope; 0.6.1.
- `frontend.mount` manifest contract. Out of scope; 0.6.1.
- `ext_shell` PG schema. Out of scope; 0.6.1.
- Design tokens, theme, UI scaling. Out of scope; 0.6.2.
- Panel SDK + client SDK. Out of scope; 0.6.3.
- Anything from `0.6a-*` admin track. Out of scope; gated on 0.6.3
  minimum and tracked separately.
- Realtime / WebSocket subscriptions from the shell. Out of scope;
  0.6.3.
- Headless-browser harness in CI. **Permitted to defer to 0.6.0.N
  per OQ2** — manual smoke is acceptable for ship.

---

## §14 — Open Questions

**OQ1 — Bundle storage form.** §4.3 / §8.3 records the choice between
embedded-resources (preferred), `xxd -i`-style linked symbol, and
shipped-alongside directory. **Recommendation:** embedded resources
via the build system's most idiomatic mechanism (CMake `configure_file`
with a generated header containing the asset bytes as a `constexpr
std::string_view`, or equivalent). Rationale: single-binary deploy,
trivial CSP / cache-header handling, no runtime FS dependency.
Architect: confirm or redirect.

**OQ2 — Headless-browser harness in CI.** T.\* / E.\* / I.\* need
browser-driven verification. The 0.6.0 milestone could ship in two
shapes: (a) manual smoke scripts for the browser cases, harness build
deferred to a 0.6.0.N follow-up; (b) headless-browser harness
(Playwright + Chromium, or equivalent) lands as part of 0.6.0 itself.
**Recommendation:** (a). Rationale: browser test infra is a non-
trivial CI dependency (Chromium binary, browser-runtime container
layer) and 0.6.0 has only 9 browser cases — the harness build is
better amortized across 0.6.x as more browser cases land. The
follow-up `0.6.0.N browser-test harness` slot would complete the
deferred T.\* / E.\* / I.\* coverage at 0.6.0.N OR fold into the
first later milestone that needs the harness (likely 0.6.3 with the
panel SDK). Architect: confirm or redirect.

**OQ3 — Login form rate-limit retry-after UX.** ICD-0.1.2 §POST
/api/auth/login response shape includes `retry_after` on 429. §5.4
specifies the user-facing string interpolates that value. The exact
behaviour during the retry-after window is undefined: should the
submit button stay disabled with a countdown, or unlock immediately
and let the next submit re-trigger 429? **Recommendation:** disable
the submit button and render a countdown (`Try again in {N}
seconds`); decrement client-side; re-enable when `N == 0`.
Rationale: the kernel rate-limit is per-IP per-minute; spamming the
server post-429 worsens the rate-limit window. Architect: confirm or
redirect.

---

## §15 — What Must Not Be Decided Yet

These items are explicitly out of scope for ICD-0.6.0. Each names the
milestone (or trigger) that closes the deferral. The fence is
deliberate: per `feedback_icd_horizon.md`, ICDs author one milestone
ahead, and pre-deciding 0.6.1+ contracts based on 0.6.0 state would
violate that discipline.

### Bundled-package first-boot install lifecycle

`architecture/06-frontend.md §1` describes the kernel detecting "no
frontend installed" on first boot, extracting the bundled shell
package, and installing it through the standard 0.4 lifecycle, ending
with a `provenance='bundled'` row in `plinth.packages`. **Closes:
0.6.1** per `DESIGN-shell-v06x.md §9.1`. The 0.6.0 static-handler
approach (§8) is the simplest conforming form per METHODOLOGY
*Caller-Triggered Implementation*; the 0.6.1 lifecycle replaces the
handler bytes-source from "embedded resource" to "package-system
extraction" while keeping the URL contract identical.

### `frontend.mount` manifest contract

`architecture/06-frontend.md §2` specifies the manifest-driven mount
prefix declaration: `{"frontend": {"mount": "/app", "entry":
"index.html"}}`. **Closes: 0.6.1** when the shell's manifest lands
through the install lifecycle. 0.6.0 hardcodes the equivalent of
`mount="/app"` at the kernel layer; this works because there is
exactly one frontend in 0.6.0 and the active-frontend singleton from
`architecture/06-frontend.md §2.2` reduces to a no-op.

### `ext_shell` PG schema and user preferences

`DESIGN-shell-v06x.md §3.7` describes the shell's own PG schema:
`ext_shell.user_preferences` (per-user JSONB key/value),
`ext_shell.default_apps` (per-user content-type handler preferences).
**Closes: 0.6.1** per the §9.1 exit criterion ("preferences round-
trip"). 0.6.0 ships zero persisted user state — every reload returns
to the login form; theme is system-default, scale is 100%, no per-
user customizations exist yet.

### Design tokens, theme, UI scaling

`architecture/06-frontend.md §4` and `DESIGN-shell-v06x.md §6`
specify the design-token serving contract (`/api/frontend/tokens.css`,
import-map binding, theme toggle, 80–175% scale). **Closes: 0.6.2**
per `DESIGN-shell-v06x.md §9.2`. 0.6.0's CSS is throwaway scaffolding
per §6.3 — every hex value will be replaced with a `var(--token-name)`
reference in the 0.6.2 PR.

_Discharged by ICD-0.6.2 (paper authored 2026-04-29 on
`feat/0.6.1.N-icd-0.6.2-authoring`); shipped in v0.6.2 (2026-04-29
on `feat/0.6.2-design-tokens-theme-scaling`)._

### Panel SDK and client SDK

`DESIGN-shell-v06x.md §4` specifies the `plinth.panel.*` API
surface (lifecycle hooks, navigation intents, focus, shortcuts, tray
state) and the client wrappers `plinth.call()`, `plinth.subscribe()`,
`plinth.useData()`. **Closes: 0.6.3** per `DESIGN-shell-v06x.md §9.3`.
0.6.0 ships zero panel surface — extensions cannot register UI in
0.6.0 (the kernel-side `plinth.panels` table doesn't exist yet
either).

_Discharged by ICD-0.6.3 (paper authored 2026-04-29 on
`feat/0.6.2.N-icd-0.6.3-authoring`); shipped in v0.6.3 (2026-04-30
on `feat/0.6.3-panel-sdk-client-sdk`). Kernel-side `plinth.panels`
table + query API explicitly remain deferred — ICD-0.6.3 §15
forward-cites their resolution to ICD-0.6.4 paper authoring per
`feedback_icd_horizon.md` one-milestone-ahead discipline._

### Tab strip, sub-tabs, app-switcher, Home launcher

`DESIGN-shell-v06x.md §2.2/§2.3/§2.4` specify the topbar's dynamic
content — extensions read from `plinth.panels` via the 0.6.4 panels
query API (`DESIGN-shell-v06x.md §11` OQ4); the Home view is a
RBAC-gated icon grid; the app-switcher dropdown opens from the
app-name chevron. **Closes: 0.6.4**. 0.6.0's app-name zone is the
static text "Plinth" (§6.2); the chevron is purely decorative; the
Home icon is a no-op.

### Float panels

`DESIGN-shell-v06x.md §3.3` specifies float chrome, lifecycle, max-
float limit, persistence. **Closes: 0.6.5**. 0.6.0 ships no floats.

### Tray panels

`DESIGN-shell-v06x.md §3.4` specifies tray icons as live SVG
surfaces, anchored popovers, badge / state APIs, the dogfooded
shell-owned bell + avatar. **Closes: 0.6.6**. 0.6.0's tray zone is
an empty `<div>`; the avatar zone is a hand-rolled component, NOT a
tray panel — it gains tray-panel semantics in 0.6.6 when the bell +
avatar both convert to dogfooded tray panels with `chrome_essential:
true` fallback.

### Content-type handler resolution and navigation intents

`DESIGN-shell-v06x.md §5` (three-tier priority, `ext_shell.default_apps`
storage, one-time chooser UI) and §3.5 (jump-to-app, navigation
intent system). **Closes: 0.6.6**. 0.6.0 ships no content-type
resolution; no `openFloat` API exists.

### Admin extension preview

The entire `0.6a-*` parallel track is a separate bundled extension
(`docs/design/DESIGN-admin-v06x.md`). Earliest possible slot is
`0.6a-A` package management panel, gated on 0.4.x + 0.6.3.
**Closes: each `0.6a-*` milestone individually.** 0.6.0 ships
nothing admin-extension-shaped.

### Localization

User-facing strings ship hard-coded English in 0.6.0 (§5.4).
**Closes: unscheduled.** No localization arc exists on the ROADMAP
through 1.0; if added, it would be a separate cross-cutting design
doc.

### Mobile / responsive treatment

`DESIGN-shell-v06x.md §3.8` catalogues responsive transforms (sub-
tab horizontal scroll, popover → bottom sheet, float → full-screen
modal). **Closes: per surface, milestone-by-milestone** as each
surface lands (sub-tabs 0.6.4, popover 0.6.6, float 0.6.5). 0.6.0's
frame has nothing that needs collapsing.

### `frontend.boundary.caught` audit family

§7.4 / §10 describes the boundary's `console.error` log. **Closes:
0.6.3** when the panel SDK exposes `audit.log` to client code per
`DESIGN-logging-subsystem.md`.

### Anything from `DESIGN-shell-v06x.md §10` constraints 5/6/7/10

Constraint 5 (no Files extension), 6 (no content-type resolution
without 0.2.x), 7 (no home screen beyond app launcher), 10 (no
foreclosing cross-cutting composition) are all 0.6.x cross-arc
rules that 0.6.0 honours vacuously — the surfaces they constrain
don't exist yet.

### Forward-looking design-bundle commitments

The 2026-04-27 design bundle (`docs/sketches/shell-design-2026-04-27/`)
includes several commitments beyond the 0.6.0 surface
that future ICDs will need to ratify:

- **Integration-point overlay (`data-ipoint` attributes).** Every
  shell-owned and extension-owned render seam carries a
  `data-ipoint` attribute naming the contract (e.g.,
  `shell.topbar`, `shell.appIdentity`, `ext.notes.primaryPane`).
  Toggleable overlay highlights blue=shell / amber=ext + legend.
  **Closes: 0.6.4** when the panels query API + RBAC-gated tab
  rendering land — the overlay becomes meaningful once extensions
  declare panels and slots.
- **Automations extension (user scripting + deferred actions).**
  The bundle ships an `Automations` extension as a third top-level
  bundled extension with three subtabs (My schedules / Deferred
  actions / Scripts), gated on `user.scripting` capability.
  **NOT yet on the ROADMAP.** Architect ratification required
  before this lands as a real milestone — likely a `0.6a-*` or
  `0.7.x` slot following the scheduled-tasks subsystem in 0.7.2.
- **Privacy-safe admin schedules.** The bundle's
  `admin-schedules.jsx` makes the user-policy view aggregate-only
  (no per-user content); admin sees ceilings, capabilities,
  per-user counts only. **Architecturally interesting** — pins a
  privacy contract that should land in
  `architecture/01-identity.md §2` or a new RBAC sub-section
  before Admin schedules ship in `0.6a-D` or wherever the
  scheduled-tasks admin UI lands.
- **UI-scaling via `zoom`, NOT rem-based.** The bundle's primary
  HTML uses `document.documentElement.style.zoom = '...'` to scale
  the entire UI; the comment explains "it's the only property that
  actually behaves like true UI-scale without rewriting every px
  value." **This is a deviation from `DESIGN-shell-v06x.md §6.3`**
  which mandates rem-based scaling. Architect must ratify either
  approach before 0.6.2 design-token work begins. Recommendation
  for the 0.6.2 ICD: pin one approach (whichever the architect
  prefers); update the design doc footnote in the same PR per
  METHODOLOGY §Phase 2 Constraint #4.
- **Explicit "shell holes" inventory.** The bundle's
  `shell-holes.jsx` enumerates visual elements that "if done wrong
  destroy the look and feel" — a useful reference for every 0.6.x
  code session reviewing visual diffs. NOT a contract; reference
  material.

These commitments are **explicitly out of scope for ICD-0.6.0** and
do not change the 0.6.0 ship criteria. They are recorded here so
the eventual 0.6.1+ ICDs and the next architecture session inherit
the design-bundle state as input.

---

## Appendix A — Four-zone HTML skeleton

The empty topbar shipped in 0.6.0 (per §6.1 / §6.2). This is
informative; the actual implementation may differ in attribute
ordering or class names but must preserve the structural shape.

```html
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Plinth</title>
    <style>
      /* Minimal layout-before-JS-loads CSS only.
         Full styling moves to shell.css in 0.6.0;
         design tokens land in 0.6.2. */
      :root { color-scheme: dark; }
      html, body { margin: 0; padding: 0; height: 100%;
                   background: #0d1117; color: #c9d1d9;
                   font-family: -apple-system, BlinkMacSystemFont,
                                'Segoe UI', system-ui, sans-serif; }
      #root { height: 100%; }
      .topbar { display: flex; align-items: center; height: 48px;
                background: #161b22; border-bottom: 1px solid #30363d; }
      .zone { display: flex; align-items: center; padding: 0 12px;
              height: 100%; }
      .zone-tray { flex: 1; }
      .avatar-circle { width: 24px; height: 24px; border-radius: 50%;
                        background: linear-gradient(135deg, #58a6ff, #bc8cff);
                        color: #fff; display: flex; align-items: center;
                        justify-content: center; font-size: 11px;
                        font-weight: 700; }
      main { display: flex; align-items: center; justify-content: center;
             height: calc(100% - 48px); font-size: 18px; }
    </style>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="shell.js"></script>
  </body>
</html>
```

After `shell.js` mounts, the authenticated DOM looks like:

```html
<div id="root">
  <header class="topbar">
    <div class="zone zone-home">
      <svg><!-- home icon --></svg>
    </div>
    <div class="zone zone-app-name">
      <span>Plinth</span>
      <svg class="chev"><!-- chevron --></svg>
    </div>
    <div class="zone zone-tray"><!-- empty --></div>
    <div class="zone zone-avatar">
      <button>
        <span class="avatar-circle">A</span>
        <svg class="chev"><!-- chevron --></svg>
      </button>
    </div>
  </header>
  <main>Hello, alice</main>
</div>
```

When the avatar is clicked, a popover opens with one menu item
("Sign Out"). When unauthenticated, the same `#root` renders the
login form instead of the topbar + main pair.

---

## Appendix B — Forward-compat manifest sketch (informative)

The shell's manifest does NOT land in 0.6.0. This appendix documents
the shape for reference only — the contract is owned by the 0.6.1
ICD that will land alongside the bundled-package install lifecycle.

```json
{
  "name": "shell",
  "version": "0.6.0",
  "description": "Plinth reference frontend",
  "author": "plinth",
  "license": "MIT",
  "entry_point": "server/main.js",
  "frontend": {
    "mount": "/app",
    "entry": "index.html"
  }
}
```

In 0.6.0 the kernel does NOT read this manifest — the static handler
hardcodes the equivalent of `mount: "/app"` and `entry:
"index.html"`. The 0.6.1 ICD will pin (a) where this manifest lives
in the build tree, (b) how it's included in the bundled package
blob, (c) how the kernel detects "no frontend installed" on first
boot, (d) the install-lifecycle path the bundled shell flows through.

`runtime` and `shareable` blocks from `DESIGN-shell-v06x.md §12` are
deliberately omitted from this sketch — the shell's QuickJS runtime
limits and shareable resources are a 0.6.1+ concern (the shell is
a frontend; it has no `server/main.js` handlers in 0.6.0).

---

## Appendix C — Architecture promotion checklist

When 0.6.0 ships, the following architecture-doc promotions land in
the same PR (per `feedback_changelog_scope.md` and METHODOLOGY
§Phase 2 Constraint #4):

- `architecture/06-frontend.md §1` — append "Implemented {date}
  (v0.6.0). Initial frame pinned in ICD-0.6.0; bundled-package install
  lifecycle still pending in 0.6.1" footnote.
- `architecture/05-extensions.md §2` reserved-prefix table — flip
  the `/app/*` row's owner to "Shell extension (kernel-stub in
  0.6.0; package-mediated from 0.6.1)" or equivalent prose.
- `architecture/05-extensions.md §2` reserved-prefix table — flip
  the `/` row's behaviour to "Redirects to `shell.root_redirect`
  (default `/app/`) per ICD-0.6.0".

The ROADMAP entry for `0.6.0` flips `[ ]` → `[x]` with a one-paragraph
summary in the established prose style (mirror the v0.5.5 entry's
shape).

---
