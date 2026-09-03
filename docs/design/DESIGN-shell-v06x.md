# DESIGN: Frontend Shell (0.6.x)

**Status:** Draft — revised 2026-04-16 against architecture patch and packages design
**Depends on:**
- architecture/01-identity.md §2 (Groups/RBAC)
- architecture/02-capabilities.md §1 (Capability Registry)
- architecture/03-data.md §2 (Storage), §3 (Realtime pub/sub)
- architecture/05-extensions.md §1 (Package Structure), §2 (Reserved URL Prefixes)
- architecture/06-frontend.md (Frontend Architecture — shell-as-extension, `frontend.mount`, asset serving, design tokens, BYO stance)
- DESIGN-packages-v04x.md (manifest schema, `panels.json` registration via `plinth.panels`, install lifecycle, first-boot shell install)
- ICD-0.1.5 (RBAC enforcement), ICD-0.2.x (capability registry), ICD-0.1.6 (WebSocket/realtime)
**Informs:** Files extension, Notes extension, Admin extension, all panel-bearing extensions
**Visual reference:** `docs/sketches/shell-topbar-reference.html`

---

## 1. What the Shell Is

The shell is a **built-in extension**, not the kernel itself. It provides
the application frame — navigation, panel lifecycle, float management,
tray system, design tokens — and it consumes the same kernel APIs (RBAC,
capability registry, realtime events) that every other extension uses.

The shell is **one frontend**. The kernel's contract is the API; the
shell's contract is the panel SDK. A deployment that wants a different
frontend — a headless API-only mode, a CLI, a full-chrome replacement —
can package it and install it through the standard lifecycle. The
architecture permits this. The project does not market it, test against
it, or write extension guidance for it. Extensions target the shell's
panel SDK and design tokens; an alternative frontend accepting that
ecosystem cost is the replacer's responsibility.

But for the ecosystem — public registry, consistent UX, one-click
install — the shell is the target. Extensions that use the shell's
panel system and design tokens get discoverability, composability, and
visual consistency for free. Extensions that don't are on their own.

---

## 2. Layout: Topbar + Content

The shell uses a **topbar-only** navigation model. No sidebar. The
topbar is the active application's chrome, not an extension switcher.

### 2.1 Topbar Structure

```
[Home] [Active App Name ▼] | [sub-tab] [sub-tab] ...   [tray icons] | [bell] [avatar ▼]
```

**Left zone — Home icon.** Always present, always leftmost. Navigates
to the Home view (app launcher — see §2.4). When the Home view is
active, the Home icon shows as selected.

**App name zone.** Icon + name of the active extension. Clickable —
the chevron opens an app-switcher dropdown (compact list of installed
extensions, RBAC-gated) for fast switching without navigating to Home.
When Home is active, this zone shows "Home" in muted styling.

**Sub-tab zone.** Separated from the app name by a vertical divider.
Horizontal tabs for the active extension's sub-views, declared in
the extension's `panels.json`. Only appears when the active extension
declares sub-tabs. The extension does not draw its own tab bar — the
shell owns this chrome.

**Tray zone.** Extension-provided and shell-owned tray panels (see
§3.4). Separated from shell-owned items (bell, avatar) by a thin
vertical divider. The tray collection is user-orderable and its
position is configurable. Extension-provided tray icons appear left
of the divider; shell-owned items (bell, avatar) appear right of
it, pinned.

### 2.2 Why Not Extension Tabs in the Topbar

Prior design had extension tabs occupying the topbar center zone.
This fails at scale: 6+ extensions compress the tabs and push
half into an overflow menu, while the active extension's sub-tabs
require a second navigation row. Two rows of chrome before content
starts. The topbar becomes a cramped extension switcher competing
with the active app's own navigation.

The revised model dedicates the topbar to the active app. Extension
count is a Home-view scaling problem (the Home launcher has the
full content area), not a topbar scaling problem. Knowledge Base
with 7 sub-tabs fits comfortably in a single topbar row.

### 2.3 Active Extension Sub-Navigation

Sub-tabs are declared in the extension's `panels.json` and rendered
by the shell. This is the pattern from Armature's Knowledge Base
(Search | Chat | Sources | Sync Log | Health | Analytics | Settings)
and Memory (Active | Proposed | Promotions | Insights).

Sub-tabs are:

- **RBAC-gated:** a user only sees sub-tabs they have permission
  to access.
- **Shell-rendered:** the extension declares them; the shell draws
  them. Visual consistency across all extensions.

### 2.4 Home View (App Launcher)

The Home view is the extension discovery and switching surface.
It renders as the content area when no extension is active (or
when the user clicks Home).

**MVP (0.6.x):** RBAC-gated icon grid of installed extensions.
Each item shows the extension's icon, name, and visual identity
color. The last-active extension is highlighted. Clicking an item
switches to that extension.

**Additive slots for later (explicitly deferred):** Search bar,
recent activity feed, pinned favorites, extension grouping. None
required at launch; the icon grid is sufficient.

The Home view is load-bearing — it replaces the always-visible
extension tabs as the primary extension discovery mechanism. It
must be good enough to serve that purpose from 0.6.x onward.

### 2.5 Content Area

Everything below the topbar belongs to the active extension's primary
panel. The extension has **full rendering control** within this space.
It manages its own internal layout (sidebar, split panes, grids — the
Files extension's left folder tree is the extension's business, not
the shell's).

The shell provides the container. The extension fills it. The shell
does not reach in; the extension does not reach out.

---

## 3. Panel System

### 3.1 Panel Types

An extension can register multiple panel types in `panels.json`:

| Type | Where it renders | Example |
|------|-----------------|---------|
| `primary` | Content area (one active at a time) | `notes:main`, `files:main` |
| `float` | Float layer (multiple can coexist) | `notes:render`, `photos:preview` |
| `settings` | Settings context (admin/user prefs) | `notes:settings` |
| `tray` | Topbar tray zone (icon + popover) | `media:now-playing`, `health:status` |

A panel declaration includes:
- `id`: Unique within the extension (e.g., `"main"`, `"render"`, `"now-playing"`)
- `type`: `primary`, `float`, `settings`, or `tray`
- `label`: Display name
- `icon`: Icon reference (from a shared icon set or extension-provided)
- `sub_tabs`: Array of sub-tab definitions (primary panels only)
- `handles_content_types`: Array of MIME types this panel can render
  (float panels only — used by content type handler resolution)
- `capability`: The capability string this panel maps to (float
  panels — used for RBAC checks before spawning)
- `accepts_navigation_intent`: Boolean — can this panel receive a
  `navigate` intent with context
- `tray_states`: Array of named icon states (tray panels only —
  see §3.4)
- `chrome_essential`: Boolean — whether the shell provides a
  fallback renderer if this panel's component fails (tray panels
  only, shell-owned panels only — see §3.4)
- `surface_traits`: Array of trait strings this panel exposes
  (reserved — see §8). E.g., `["text-editor", "rich-text"]`.
  Must parse as a string array; validated but ignored in 0.6.x.
  Present so early extensions can declare traits before the
  cross-cutting composition arc lands.
- `slots`: Object mapping slot names to slot definitions (reserved
  — see §8). E.g., `{ "toolbar": {}, "status-bar": {} }`. Must
  parse as an object; validated but ignored in 0.6.x. Reserves
  the injection point vocabulary for the augmentation system.

### 3.2 Panel Lifecycle

Panels are **Preact component trees** rendered into shell-managed
containers. They are isolated — one panel's crash does not take down
another or the shell (see §3.6 Error Boundaries).

Lifecycle events:
- `activate` — panel becomes visible (primary switch, float spawn,
  tray popover open)
- `deactivate` — panel is hidden (primary switch away, float close,
  tray popover close; float minimize does NOT deactivate)
- `destroy` — panel is removed (float closed, extension uninstalled)

Extensions receive these events through the panel SDK (§4).

### 3.3 Float Panels (Shell-Managed)

**Decision: Float panels are first-class, shell-managed entities.**
They are not extension-owned iframes or embedded components. The shell
controls their chrome, position, lifecycle, and persistence.

**Float chrome (shell-provided):**
- Title bar: icon + label + source badge (e.g., "Notes") + controls
- Controls: minimize, maximize/restore, close, **jump to app (↗)**
- Draggable, resizable (desktop viewports)
- On narrow/mobile viewports: floats become full-screen modals with a
  back button. The panel component doesn't know the difference.

**Float spawn flow:**

1. A trigger occurs (e.g., user clicks a file in the Files extension)
2. The shell (or the triggering extension via panel SDK) calls
   `plinth.panel.openFloat(contentType, context)`
3. The shell queries the capability registry: "who handles
   `text/markdown` for preview?"
4. Registry returns the best match (see §5 Handler Resolution)
5. Shell checks RBAC: does this user have permission for that
   capability?
6. Shell spawns a float panel with the resolved extension's float
   component, passing `context` (file path, record ID, etc.)
7. Shell tracks this float by `(capability, context_key)` — if the
   same file is already open in a float, focus it instead of spawning
   a duplicate

**Float limits:**
- Configurable maximum (default: 5 simultaneous floats)
- When the limit is hit, the oldest float is minimized (not closed)
  to make room. User can restore from a minimized-floats indicator.
- Floats persist across primary panel switches (switching from Files
  to Notes does not close the open floats)

**State persistence across sessions:**
- The shell persists open float state (which floats, their context,
  their position/size) to `ext_shell.user_preferences` (see §3.7).
- On session restore (browser reload, re-login), the shell reopens
  persisted floats. If the capability is no longer available (extension
  uninstalled), the float is silently dropped from the persisted state.

### 3.4 Tray Panels

Tray panels render as icons in the topbar tray zone. Clicking a tray
icon opens an anchored popover; click-outside or Escape closes it.
One popover open at a time.

**What tray panels are for:** Live status surfaces and quick controls
that don't warrant a full primary panel. System health, media
playback controls, sync status, clipboard manager, volume control —
the same class of functionality that lives in macOS menu bar extras,
Windows system tray, or GNOME top bar applets.

**Tray icon as a live surface:** The tray icon is an SVG element
that the extension controls via the panel SDK. The extension
subscribes to realtime events (via `plinth.subscribe`) and mutates
the SVG in response — animated equalizer bars when music plays,
a heartbeat line for system health, a spinner during sync. The icon
is a status indicator, not just a launcher.

**Tray states:** Extensions declare named icon states in `panels.json`
(e.g., `["idle", "playing", "paused"]` for a media player). The
extension sets the active state via `plinth.panel.setTrayState(name)`.
The shell applies the corresponding SVG/CSS class. This keeps the
shell in control of the icon container while the extension controls
the visual state.

**Badge:** Tray panels can display a numeric badge or a status dot
on their icon via `plinth.panel.setTrayBadge(value)`. Badge
rendering is shell-controlled for visual consistency (size, position,
color).

**Popover constraints:**
- Width: 280–480px (extension declares preferred width in
  `panels.json`, shell clamps)
- Height: content-driven, max 70vh
- On narrow/mobile viewports: popovers become bottom sheets with a
  drag handle

**Tray panel overflow:** When installed tray panels exceed available
topbar space, the overflow collapses into a `⋯` indicator that
opens a tray drawer (same pattern as the overflow for sub-tabs if
they ever need it).

**Extensions without a primary panel can still have a tray panel.**
This enables tray-only extensions — a system monitor, a clipboard
manager, a quick-capture tool — that live entirely in the tray zone
without needing a full primary view.

**Shell-owned tray panels (dogfooded):**

The shell's own notification bell and user avatar are tray panels
declared in the shell's own `panels.json`. They use the same SDK,
same lifecycle, same popover mechanism as any extension-provided
tray panel. This is deliberate dogfooding: if the tray system can't
support the shell's own chrome, it can't support extensions.

Shell-owned tray panels are marked `chrome_essential: true`. If the
panel component throws an unhandled exception, the shell provides a
minimal fallback renderer (the bell shows a generic icon with no
badge; the avatar shows a generic user icon with only the Sign Out
action). This ensures the user can always sign out even if the full
avatar popover is broken.

Shell-owned tray panels are hard-pinned to the right side of the
tray, after the separator. They are not user-moveable.

**Notification bell popover:** Unread notification list, mark-read,
dismiss. Subscribes to the kernel notification bus. Badge shows
unread count.

**Avatar popover contents:**
- Identity block — avatar, display name, username, primary group.
  Not interactive; confirms which account is active.
- Theme toggle — light / dark / system (three-state).
- UI scaling — slider, 80%–175%.
- User Settings — navigates to the shell's settings panel.
- Sign Out — always last, separated by divider, styled as
  destructive action.

Five items maximum. If a sixth is ever proposed, push it into
User Settings.

**Programmatic triggering:** No. Extension A cannot open extension
B's tray popover. Tray is user-controlled chrome. If an extension
needs to surface something urgent, that's what notifications are for.
The shell does not expose an `openTray` SDK method.

### 3.5 Jump to App

User has a float open showing `notes:render` for `meeting-notes.md`.
They click "jump to app" (↗). What happens:

1. Shell sends a **navigation intent**: `plinth.panel.navigate("notes:main", { file: "meeting-notes.md" })`
2. Shell switches the primary content area to the Notes extension's
   main panel
3. The Notes main panel receives the intent and opens the specified file
4. The float closes (it's now redundant — the full app is showing the
   same content)

This navigation intent mechanism is the **same system** used for:
- Deep links (URL routes → extension + context)
- Notification clicks (notification payload → extension + context)
- Cross-extension "open in..." actions
- App-name dropdown selections
- Home launcher clicks
- Command palette actions (if implemented)

The intent contract: `{ target: "extension:panel", context: Record<string, string> }`

Extensions that declare `accepts_navigation_intent: true` in
`panels.json` must implement an intent handler in the panel SDK.

### 3.6 Error Boundaries

Every panel (primary, float, tray popover) is wrapped in a Preact
error boundary. If a panel throws an unhandled exception:

- The boundary catches it and renders a fallback: "This panel
  encountered an error" + a reload button
- Other panels and the shell are unaffected
- The error is logged via the kernel audit/logging API
- The shell does not automatically retry — the user clicks reload or
  closes the panel

Exception: shell-owned tray panels with `chrome_essential: true`
use a minimal fallback renderer that preserves critical actions
(Sign Out).

### 3.7 Shell as an Extension: Own Data

The shell is a standard extension with package name `shell` and
PG schema `ext_shell`. It owns its own data through the standard
extension migration mechanism (DESIGN-packages-v04x.md §0.4.3).

**Tables in `ext_shell`:**

- `ext_shell.user_preferences` — per-user key/value storage. Keyed
  on `(user_id, key)`. Stores: tab ordering, theme choice, UI
  scaling factor, open float state, tray collection position, tray
  icon ordering, and any other per-user shell state.
- `ext_shell.default_apps` — per-user and admin-level content-type
  handler preferences. Keyed on `(scope, user_id, content_type)`.
  See §5.

The kernel does not own a `plinth.user_preferences` table. Shell
preferences are extension data. Other extensions that need per-user
preferences follow the same pattern: own your data in your own
schema.

**User deletion:** The shell follows the standard extension
user-deletion cleanup contract (`architecture/01-identity.md §4`).
Shell-side cleanup is trivial: delete rows where
`user_id = deleted_id`. Preference data has no intrinsic value after
the user is gone.

### 3.8 Mobile / Responsive

- **Desktop (>1024px):** Full topbar, float panels as draggable
  windows, all sub-tabs visible, tray popovers as anchored popups.
- **Tablet (768–1024px):** Topbar may collapse sub-tabs into
  horizontal scroll. Floats become slide-over panels from the
  right edge. Tray shows essential icons only.
- **Mobile (<768px):** App name and essential tray icons in topbar.
  Sub-tabs become horizontal scroll within the extension's content
  zone. Floats become full-screen modals with a back button. Tray
  popovers become bottom sheets with a drag handle.

The panel component **does not know** what viewport it's in. The
shell adapts the container. Extensions should use responsive CSS
within their panel but do not need to handle the float-to-modal
or popover-to-bottom-sheet transformation.

---

## 4. Panel SDK (`plinth.panel.*`)

The narrow contract between extensions and the shell. Extensions
import this SDK; they do not access shell internals directly.

### 4.1 API Surface

```
plinth.panel.navigate(target, context)
  — Send a navigation intent to another panel

plinth.panel.openFloat(contentType, context)
  — Request the shell to open a float for the given content type

plinth.panel.onActivate(callback)
plinth.panel.onDeactivate(callback)
plinth.panel.onNavigationIntent(callback)
  — Lifecycle hooks

plinth.panel.setDirty(isDirty)
  — Declare unsaved changes (shell shows confirmation on close/switch)

plinth.panel.requestFocus()
  — Ask the shell to bring this panel to front (floats)

plinth.panel.registerShortcut(combo, callback)
  — Register a keyboard shortcut (shell handles conflict resolution)

plinth.panel.getContext()
  — Get the context passed when this panel was spawned/navigated to

plinth.panel.setTrayState(stateName)
  — Set the active icon state for a tray panel

plinth.panel.setTrayBadge(value)
  — Set the badge on a tray icon (number, dot, or null to clear)
```

The SDK is injected by the shell into the panel's runtime scope. It
is not a network API — it's in-process communication between the
shell and the panel component tree.

### 4.2 Pub/Sub

Inter-panel communication uses the kernel's realtime event system,
not a shell-specific bus. Extensions publish events via the standard
kernel API; other extensions subscribe via the standard kernel API.
The shell is not a message broker — it's a panel container.

This means pub/sub works identically whether the subscriber is a
panel in the shell, a backend extension handler, or an external
client. One event system, not two.

---

## 5. Content Type Handler Resolution

When the shell (or an extension via the panel SDK) needs to open
content by type, the resolution order is:

### 5.1 Priority

1. **User preference.** The user has explicitly chosen an application
   for this content type. Stored in `ext_shell.default_apps` with
   `scope = 'user'`.
2. **Admin default.** The admin has set a global default for this
   content type. Stored in `ext_shell.default_apps` with
   `scope = 'admin'`.
3. **First-installed handler.** If no preference is set at either
   level, the first extension that registered a handler for this
   content type wins. (Stable — does not change when later extensions
   are installed.)

### 5.2 The "Default Applications" Pattern

This is the user-facing mechanism for content type resolution:

- **Which app opens my `.md` files?** User picks Notes or a
  different markdown editor extension.
- **Which app handles image preview?** Built-in Files viewer, or
  the Photos extension if installed.
- **What's my default code editor?** Code Workspace, or a lighter
  alternative.

Same UX as desktop OS "Default Apps" settings. Users understand it.

### 5.3 Conflict Resolution

When multiple extensions register handlers for the same content type
and no preference/default exists:

- The shell presents a **one-time chooser**: "Open with: [Notes]
  [MarkdownPro] — [Always use this choice]"
- If the user checks "always," it becomes their user preference for
  that content type
- If not, the choice is used once and the chooser appears again next
  time

---

## 6. Design Tokens

The shell provides a design token system via CSS custom properties.
Extensions that use these tokens get automatic theming, dark mode
support, and visual consistency.

Tokens cover:
- Colors (backgrounds, text, borders, semantic states)
- Typography (font families, sizes, weights, line heights)
- Spacing (consistent scale)
- Border radii
- Elevation / shadow (minimal — flat design)

The token system is **not enforced at runtime** — an extension can
write raw CSS. But the extension guide makes it clear: use the
tokens, and your extension looks native. Deviate, and you're on
your own.

### 6.1 Token Serving

Design tokens are shell assets served at
`/ext/shell/{version}/css/tokens.css`. Because the shell version
can change, extensions must not hardcode the version segment.

**Resolution mechanism (requires architecture decision — see §11):**
The kernel provides a stable endpoint (proposed:
`/api/frontend/tokens.css`) that redirects to the active shell's
token file. Extensions reference this stable URL in `@import` or
`<link>`. For JS-based token access, the shell exports token values
as an ES module importable by specifier.

Fonts, icons, and other shared assets follow the same resolution
pattern — stable kernel-mediated URL redirecting to the active
shell's versioned assets.

### 6.2 Theme: Light / Dark / System

Three-way toggle in the avatar popover (§3.4):
- **Light** — light background, dark text
- **Dark** — dark background, light text (established aesthetic)
- **System** — follows the OS `prefers-color-scheme` media query

System is the default for new users. The toggle swaps CSS custom
property values at the `:root` level. Extensions that use design
tokens get theme support for free.

Theme preference persists in `ext_shell.user_preferences`.

### 6.3 UI Scaling (80%–175%)

User-configurable scaling factor, applied as a CSS `font-size`
percentage on the root element (all `rem`-based sizing scales
proportionally). Range: 80% to 175%, default 100%.

Accessible from the avatar popover (§3.4). Persists in
`ext_shell.user_preferences`.

**Implementation constraint for code sessions:** Every layout
dimension that should scale with the user's preference **must** use
`rem` units, not `px`. Fixed dimensions (`px`) are reserved for
borders, hairlines, and elements that should not scale.

**Rem-vs-zoom decision pinned by ICD-0.6.2 §5.1 + §SC2 + §SC3**
(architect-confirmed at the 2026-04-29 plan-mode interaction with
the ICD-0.6.2 authoring session — popup-coordinate drift under
`zoom` worsening as the scale gradient grows is the canonical
`zoom`-vs-Floating-UI failure that disqualifies `zoom`). Validated
end-to-end in the v0.6.2 browser smoke: popover-anchor stability
gate (R.01) at 80% / 100% / 175% shows 0.30 rem delta invariant —
no `zoom`-style drift.

**Float panel positions** are the tricky part. When the scaling
factor changes, float panel positions (stored in px for drag state)
need to be recalculated relative to the new effective viewport.
The shell must clamp floats to visible bounds on scale change.
This is math-sensitive and should have explicit test coverage.

---

## 7. Files Extension: The Storage UI Primitive

Files is a **regular extension** — it happens to be the primary
consumer of the kernel's storage subsystem. It is not part of the
shell. It gets no special privileges. It uses the same panel system,
the same SDK, the same content type resolution as every other
extension.

### 7.1 What Files Does

- Navigable folder tree (extension-namespaced storage paths)
- File list/grid views with sorting, search, filtering
- Preview icons (type-appropriate thumbnails)
- Built-in viewers for common types: text, images, PDF, video
- Sharing (RBAC-based — share = grant permission)
- Desktop sync (via storage API — one sync client, not per-extension)

### 7.2 How Other Extensions Compose Through Files

When an extension writes to storage via the kernel `storage.*` API,
its content appears in the Files tree under the extension's
namespace. Install Notes → a `Notes/` folder appears. Install
Image Generator → a `Generated/` folder appears.

The extension does **not** register with Files. The extension writes
to storage. Files reads from storage. The kernel mediates. There is
no coupling.

### 7.3 Content Type Preview via Capability Registry

When a user clicks a file in the Files browser:

1. Files determines the content type (MIME type from extension or
   storage metadata)
2. Files calls `plinth.panel.openFloat(contentType, { path: "..." })`
3. The shell resolves the handler via §5 (user pref → admin default →
   first-installed → built-in)
4. If a handler is found, a float panel opens with the resolved
   extension's preview component
5. If no handler is found, Files uses its own built-in viewer (text
   reader, image viewer, etc.)

The float panel's title bar shows the source extension badge (e.g.,
"Notes") and the jump-to-app action. The user can click ↗ to switch
to the full Notes application with that file open.

### 7.4 What Files Does NOT Do

- Files does not know about Notes, Photos, or any other extension
- Files does not import extension code
- Files does not render extension-specific UI
- Files does not manage extension data models

Files is a storage browser. The capability registry and float system
handle everything else.

---

## 8. What the Shell Does NOT Decide

These are explicitly deferred to later design work or to individual
extension design docs:

- **Home screen content beyond the app launcher.** The MVP is an
  icon grid. Recent activity, dashboards, widgets — all deferred.
  The app launcher is sufficient for extension switching.
- **Command palette.** Likely valuable (Cmd+K to search across
  extensions, files, capabilities). Uses the navigation intent
  system. Deferred to a follow-on version.
- **Extension-to-extension drag and drop.** Dragging a file from
  Files into a Notes panel. Requires a shared drag context protocol.
  Complex. Deferred.
- **Tiling / split-view.** Two primary panels side by side. The
  float system covers the 80% case. Full tiling is a significant
  complexity increase. Deferred.
- **Offline / service worker.** Which extensions work offline, how
  stale state is indicated, sync-on-reconnect UX. The
  `/ext/{name}/{version}/*` immutable caching (DESIGN-packages §0.4.4)
  helps — assets are cacheable forever within a version. But the
  service worker lifecycle, offline capability declarations, and
  stale-state UX are all deferred.
- **Extension-provided themes.** Whether extensions can ship their
  own design token overrides. Deferred — design tokens come first,
  theming comes later.
- **Cross-cutting extension composition (traits and augmentation).**
  Extensions that augment other extensions' surfaces — grammar
  checkers that operate on any text editor, AI assistants that
  augment any content panel, code completion that works in any code
  surface. The mechanism: host panels declare `surface_traits` and
  `slots` in `panels.json`; augmenter extensions declare which
  traits they augment in their manifest; the registry matches them;
  the shell mediates slot injection. The `surface_traits` and `slots`
  fields are reserved in `panels.json` now (§3.1) so early
  extensions can declare them before the augmentation arc lands.
  Full design deferred to a dedicated arc (~0.9.x or later), which
  must cover: slot vocabulary standardization, augmentation RBAC
  (does the user control which augmenters attach?), event-stream
  and pipeline patterns for non-UI augmentation (LLM integration,
  content transformation), and the interaction between augmenters
  and panel lifecycle. Scale 2 design doc required. See architecture
  doc for the trait system contract.
- **System-level input/output services (STT, TTS, translation).**
  Services that operate at the boundary between the user and any
  surface, below the panel level. Distinct from in-surface
  augmentation — these don't inject into panels, they transform
  input/output at the shell level. Likely a kernel-level concept
  (input/output providers registered as capabilities, shell routes
  through them). Deferred — depends on the cross-cutting
  composition arc establishing the trait vocabulary first.

---

## 9. Version Scope (0.6.x)

Admin UI (groups management, package management, RBAC panels) is
extracted to a separate design doc: `DESIGN-admin-v06x.md`. The
admin panel is a second built-in extension, bundled and installed
at first boot alongside the shell. It has its own manifest, own
`panels.json`, own `rbac.json`, own migrations. This separation
is deliberate dogfooding: if the admin extension can't be built
from shell-SDK-only contracts, the shell SDK is wrong.

### 0.6.0 — Bootstrap and Frame

- Preact/htm scaffold, login screen wired to 0.1.x auth, session
  management client-side
- Empty topbar: Home icon, app-name zone (placeholder), empty tray
  zone, avatar button (hardcoded, not yet a tray panel)
- Primary content area with panel container and error boundary
  scaffolding
- "Hello, {user}" placeholder in content area
- **Entry:** Auth (0.1.x) and RBAC (0.1.5) complete
- **Exit:** Shell renders, user can log in, empty frame displays,
  avatar shows username and sign-out works

### 0.6.1 — Shell Schema and User Preferences

- Shell's own manifest/capabilities/panels files land
- `ext_shell` PG schema created via standard extension migration
- `ext_shell.user_preferences` table: `(user_id, key, value JSONB)`
- Get/set preference pattern implemented as kernel capability calls
- Not visible in UI except debug; infrastructure for everything after
- **Entry:** Package system (0.4.x) at least through 0.4.4
- **Exit:** Shell installs through the standard package lifecycle on
  first boot; preferences round-trip (write, reload, read back)

### 0.6.2 — Design Tokens, Theme, UI Scaling

- CSS custom properties at `:root` — full token set (colors,
  typography, spacing, radii)
- Light / dark / system toggle (in avatar popdown, hardcoded for now)
- 80%–175% UI scaling, rem-based
- All persisted to `ext_shell.user_preferences` via 0.6.1
- **Entry:** 0.6.1 complete
- **Exit:** User changes theme and scale, reloads, settings persist.
  A test extension using design tokens renders correctly in both
  themes and at multiple scale factors.

### 0.6.3 — Panel SDK and Client SDK

- `plinth.panel.*` lifecycle methods: `onActivate`, `onDeactivate`,
  `onNavigationIntent`, `setDirty`, `requestFocus`, `getContext`,
  `registerShortcut`
- `plinth.call()`, `plinth.subscribe()`, `plinth.useData()` — client
  wrappers around the capability registry and realtime APIs
- Proven with a dummy/test extension that registers a primary panel
  and exercises the SDK
- **Entry:** Capability registry (0.2.x) operational; 0.6.2 complete
- **Exit:** A test extension loads, receives lifecycle events,
  makes capability calls, subscribes to realtime events

### 0.6.4 — Extension Tabs, Sub-Tabs, Home Launcher

- Topbar reads `plinth.panels` (via kernel API, RBAC-gated)
- App-name zone shows active extension; chevron opens app-switcher
  dropdown
- Sub-tab rendering from `panels.json` declarations
- Home view: RBAC-gated icon grid of installed extensions
- Realtime subscription: tab appears/disappears live on
  install/uninstall
- Tab ordering persisted to `ext_shell.user_preferences`
- **Entry:** 0.6.3 complete
- **Exit:** Installing a test extension causes its icon to appear in
  the Home launcher and its name to appear in the app-switcher
  dropdown in realtime. Selecting it switches the primary panel.
  Sub-tabs render and switch views.

### 0.6.5 — Float System

- Float panel chrome: title bar, minimize/maximize/close, jump-to-app
- Float lifecycle: spawn, minimize, maximize, close
- Responsive transforms: desktop draggable windows, tablet slide-over,
  mobile full-screen modals
- Float state persistence across sessions (to user_preferences)
- Max-float limit with oldest-minimized behavior
- Error boundaries per float
- **Entry:** 0.6.4 complete
- **Exit:** Float panels work end-to-end. A test extension can spawn
  a float, the float survives a primary panel switch, state persists
  across reload.

### 0.6.6 — Tray System, Content Type Resolution, Navigation Intents

- Tray panel type: icon rendering, popover lifecycle, badge/state API
- Shell-owned bell and avatar converted to dogfooded tray panels
- `chrome_essential` fallback rendering for shell-owned trays
- Tray collection ordering and position persistence
- Content type handler resolution (§5): three-tier priority,
  `ext_shell.default_apps` storage, one-time chooser UI
- Navigation intent system: `navigate()`, `openFloat()`,
  `onNavigationIntent()`
- Jump-to-app action in float chrome
- **Entry:** 0.6.5 complete
- **Exit:** Full tray system operational — extension-provided tray
  panel shows live state, popover opens/closes, bell shows
  notifications with badge. Content type resolution works end-to-end.
  A test extension can open a float from another extension via
  content type. Navigation intents route correctly.

---

## 10. Constraints for Code Sessions

These apply to every 0.6.x code session:

1. **The shell is a built-in extension.** It does not bypass the
   kernel API. If a shell feature requires a kernel capability that
   doesn't exist, that's an architecture session conversation, not a
   code session invention.

2. **The panel SDK is the only contract.** Extensions never import
   shell internals. The shell never reaches into panel component
   trees. If something can't be done through the SDK, the SDK needs
   a new method — decided in an architecture session.

3. **RBAC gates everything visible.** No tab, no sub-tab, no float,
   no tray icon, no menu item renders without a permission check.
   If RBAC says no, the element does not exist in the DOM — it is
   not hidden, it is not rendered.

4. **Design tokens, not raw values.** Every color, font-size,
   spacing value in the shell and in example/built-in extensions
   uses CSS custom properties. No hex codes in component styles.

5. **Do not build the Files extension in 0.6.x.** Files is a
   separate extension with its own design doc. The 0.6.x shell uses
   test/dummy panels to prove the panel system works.

6. **Do not build content type resolution without the capability
   registry.** 0.6.6 depends on 0.2.x being operational. If it
   isn't, 0.6.6 scope reduces to tray system only, and content type
   resolution moves later.

7. **Do not decide the home screen beyond the app launcher.** The
   Home view is an icon grid. Dashboards, feeds, widgets — deferred.

8. **Shell preferences are extension data.** User preferences live
   in `ext_shell.user_preferences`, not in a kernel table. Every
   reference to "kernel storage" for preferences is wrong.

9. **Topbar is the active app's chrome.** Do not put extension tabs
   in the topbar. Extension switching goes through Home or the
   app-name dropdown.

10. **Do not foreclose cross-cutting composition.** The panel SDK's
    event model must be extensible — designing it so that surface
    events and slot injection can be added later without breaking the
    existing API. Accept `surface_traits` and `slots` in `panels.json`
    validation (store them, don't interpret them). Do not design panel
    containers in a way that prevents future component injection
    into named slots. If a design choice in 0.6.x would make the
    augmentation arc harder to build later, that's an architecture
    session conversation.

---

## 11. Open Questions Escalated to Architecture

These surfaced during the shell design review and require decisions
in the architecture decomposition session. They are not shell-doc
decisions.

1. **File upload/download HTTP surface.** The kernel Storage
   abstraction (§3.5) defines `put`/`get`/`remove`/`list` but not
   the HTTP endpoints for binary transfer. Multi-part/chunked upload,
   resumable uploads, range-request downloads, progress reporting,
   quota enforcement — all need a kernel HTTP contract. Blocks Files,
   Notes-with-attachments, Gallery, and every content-heavy extension.

2. **User deletion cleanup contract.** When a user is deleted, every
   extension with per-user data needs to clean up. Proposed: kernel
   emits `users.deleted` event (extensions subscribe, clean up async)
   plus `plinth.users.list()` capability for boot-time reconciliation.
   Needs a permanent home in the identity section of the arch doc.

3. **Design token serving mechanism.** How extensions reference the
   shell's CSS tokens across versioned URLs. Proposed: kernel-mediated
   stable endpoint (`/api/frontend/tokens.css`) redirecting to the
   active shell's versioned token file. Needs an architecture decision.

4. **Panels query API.** The shell needs a kernel endpoint for "what
   panels should I render for this user." Shape of the API (REST
   endpoint vs. capability call), RBAC filtering, response format.
   ICD-level, resolved during 0.6.x implementation.

5. **Notification data model.** The bell tray panel needs to know:
   notification structure, persistence, subscribe/read/dismiss API.
   ICD-level, resolved when the notification subsystem is built.

---

## 12. Shell Manifest Files (Sketch)

The shell's own package files, for reference. These are validated by
the package system (DESIGN-packages-v04x.md) like any other extension.

**manifest.json:**
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
  },
  "runtime": {
    "memory_limit_mb": 32,
    "cpu_time_limit_ms": 2000,
    "max_stack_depth": 500
  },
  "shareable": []
}
```

**capabilities.json:** The shell requires kernel capabilities but
provides none of its own (it is a consumer, not a provider).
```json
{
  "provides": [],
  "requires": [
    "kernel:1:storage.get",
    "kernel:1:storage.put",
    "kernel:1:pubsub.subscribe",
    "kernel:1:pubsub.publish"
  ]
}
```

**rbac.json:** The shell owns no RBAC rules. It consumes RBAC-gated
capabilities; it doesn't register new ones.
```json
{
  "rules": []
}
```

**panels.json:** The shell registers its own tray panels. Primary
panels are not declared here — the shell's content area is the
container for other extensions' primary panels.
```json
{
  "panels": [
    {
      "id": "notifications",
      "type": "tray",
      "label": "Notifications",
      "icon": "bell",
      "chrome_essential": true,
      "tray_states": ["idle", "unread"],
      "component": "client/panels/notifications.js"
    },
    {
      "id": "account",
      "type": "tray",
      "label": "Account",
      "icon": "user",
      "chrome_essential": true,
      "tray_states": ["default"],
      "component": "client/panels/account.js"
    }
  ]
}
```

**Note on `surface_traits` and `slots` (reserved fields):** The
shell's own panels don't declare traits or slots — it's the frame,
not an augmentable surface. Content extensions will. For reference,
a Notes extension might declare:
```json
{
  "id": "editor",
  "type": "primary",
  "label": "Editor",
  "surface_traits": ["text-editor", "rich-text", "markdown"],
  "slots": { "toolbar": {}, "status-bar": {}, "context-menu": {} },
  "component": "client/panels/editor.js"
}
```
These fields are validated and stored in 0.6.x but not interpreted.
The cross-cutting composition arc defines their semantics.

**config.json:** Shell-level defaults. Admin can override.
```json
{
  "defaults": {
    "theme": "system",
    "ui_scale": 100,
    "max_floats": 5,
    "tray_position": "right"
  }
}
```

---

## Appendix A: Armature Precedent

Screenshots from the Armature project inform the shell design:

**Files app:** Topbar with [Home] [Files] [◄] [List]. Left sidebar
with folder tree (extension-owned, not shell). Content area with file
grid. The dropdown-based extension switching from Armature is replaced
by the Home launcher and app-name dropdown.

**Memory app:** Topbar with sub-tabs [Active | Proposed | Promotions |
Insights]. Clean, functional, the sub-tab pattern carries forward
directly into the revised topbar model.

**Knowledge Base app:** Sub-tabs [Search | Chat | Sources | Sync Log |
Health | Analytics | Settings]. The stress case for topbar layout — 7
sub-tabs fit comfortably in the revised single-row model.

Key lessons:
- The dark theme aesthetic works and should carry forward
- Sub-tabs per extension are more effective than deep dropdown menus
- Extensions that own their content area (full control below topbar)
  produce the cleanest results
- Extension switching in a dropdown doesn't scale — the Home launcher
  is the evolution