# Shell fullscreen mode

**Status:** Discussion. Pre-commitment thinking.
**Cited by:** `discussion/llm-augmented-game.md` (games are the canonical use case).
**Scope:** Shell + kernel (capability declaration is kernel-managed, rendering is shell-managed).

## Problem

The current shell model assumes chrome-laden windows: topbar with active app + sub-tabs, tray with notification bell and avatar menu. This is correct for productivity extensions but breaks for:

- **Games.** EotFS-style game wants the entire window (or screen) for the map.
- **Presentations.** A slideshow extension hiding the topbar during delivery.
- **Immersive tools.** Distraction-free writing, drawing canvases, video editors.
- **Kiosk-mode displays.** Single-app dedicated installations.

Without a fullscreen affordance, these extensions are second-class citizens. Worse, individual extensions might invent their own fullscreen via CSS hacks (z-index spam, position:fixed) — which fights the shell instead of cooperating with it. A first-class affordance prevents that.

## Levels of "fullscreen"

Three meaningful levels with different mechanics:

1. **Hidden topbar.** Extension fills the Plinth window canvas; topbar and tray hidden. Browser chrome and OS chrome unchanged.
2. **Browser fullscreen API.** Plinth itself goes fullscreen via the HTML5 Fullscreen API. Browser chrome hidden. Only Plinth visible.
3. **Standalone window (PWA).** Plinth installed as a PWA, runs in its own window. Different concern, beyond fullscreen scope.

These are independent. (1) is an internal-shell concern. (2) is a browser-API concern with security implications. (3) is a PWA installation concern, deferred entirely from this doc.

## Decided now

**Hidden-topbar mode is a first-class shell affordance.** Extensions can declare fullscreen capability in `panels.json`. Users trigger it via a shell control. Extensions can request it programmatically, but the user must confirm (similar to browser permission prompts).

**ESC always exits fullscreen mode.** Non-negotiable. Extensions cannot trap users.

**Browser Fullscreen API integration is layered on top.** When in hidden-topbar mode, the user can additionally trigger browser fullscreen via a shell control. Extensions don't control this — only the user, per browser security model.

## Reserved schema fields

In `panels.json`:

- `fullscreen_capable` — boolean. Extension declares whether fullscreen is supported.
- `fullscreen_default` — enum. `none` (default), `hidden_topbar`. Determines whether fullscreen-trigger does anything for this extension.
- `fullscreen_modes` — list of enum. Reserved for future expansion (e.g., presentation mode, kiosk mode); empty list today.

## Chrome-essential element behavior

The notification bell and avatar menu are flagged `chrome_essential` per existing shell architecture. In hidden-topbar fullscreen, three options exist:

- **Suppress.** No notifications during fullscreen. User must exit to check.
- **Overlay.** Bell and avatar render as floating elements over the extension's canvas. Extension must tolerate them.
- **Keyboard summon.** A keyboard shortcut (e.g., Cmd+\\) temporarily reveals the topbar.

**Decided now: keyboard summon as default.** Reveals the topbar for as long as the shortcut is held or until ESC. Avoids overlay z-index conflicts and avoids notification blackout. Configurable per-extension if pressing reasons emerge.

## Open questions (deferred)

- **Multi-user interaction.** Plinth is multi-user; "fullscreen" is per-user-session. Confirm the principal model handles this — the user's fullscreen state is on their session, not the instance. Probably trivial but worth verifying during shell implementation.
- **Tray panels in fullscreen.** Per chrome-essential behavior above, but third-party tray panels (clocks, music players, custom indicators) need clear semantics. Suppress by default? Allow extensions to opt-in to overlay rendering? Defer.
- **Notification awareness during fullscreen.** When a notification arrives during fullscreen, does the user know? Visual indicator? Audio? Browser API for badges? Defer to first fullscreen-capable extension's UX testing.
- **Print-mode and similar.** Adjacent concerns (printing, screen-sharing, recording) might want similar chrome-hiding affordances. Worth considering whether fullscreen is one mode or part of a "presentation modes" family — but premature to commit.
- **Programmatic fullscreen request from extension.** Allowed but requires user confirmation? Or only allowed in response to user gesture (browser-style)? Lean toward gesture-required at first; relax if real friction emerges.
- **Extension-declared exit affordance.** Should extensions be able to render their own "exit fullscreen" button in addition to ESC? Yes, presumably — a game's pause menu naturally has one. But the shell's ESC handling is non-bypassable regardless of what the extension does.

## Trust and safety

- **No fullscreen lock.** ESC always exits. Always. This is a security constraint, not a UX preference.
- **Browser fullscreen requires user gesture.** Per browser security; extension cannot force browser-fullscreen.
- **Audit log.** Fullscreen entry/exit logged with extension identity, principal, timestamp. Useful for debugging and security review.
- **Capability gate.** Hidden-topbar fullscreen is a capability extensions must declare. Users can revoke. The kernel's capability registry enforces this; shell renders accordingly.

## Kernel concern explanation

This is a shell-rendered feature, but the kernel is where:

- The capability declaration is registered (`fullscreen_capable` from `panels.json` flows through the kernel's capability registry).
- The capability is granted/revoked per user (RBAC concern, kernel-managed).
- The audit log entries for fullscreen entry/exit are recorded (kernel-owned audit subsystem).

The shell renders, the kernel authorizes. Standard separation.

## Dependencies and consumers

Extensions that benefit once this lands:

- The EotFS-style game (canonical use case).
- Future presentation/slideshow extensions.
- Distraction-free writing or canvas extensions.
- Any kiosk-mode deployment.

## Pressure for promotion

Discussion → fuzzy: when the shell architecture document is being updated for any of the next milestones (0.6.x window). Hidden-topbar mode does not need to be implemented in 0.6.x, but the shell's architectural commitment should not foreclose it.

Fuzzy → medium: when the first fullscreen-capable extension is on the roadmap. Game is post-0.8.x; could be earlier with a presentation extension.

## Claude Design prompt

> Generate a structural diagram showing three shell states side by side. State 1: "Default" — topbar visible at top with app name and tabs, tray visible at right with bell and avatar, extension content fills the middle area. State 2: "Hidden topbar" — topbar gone, tray gone, extension content fills the entire window, small ESC indicator in a corner. State 3: "Browser fullscreen" — no browser chrome at all, only the extension visible, ESC indicator still present. Use c-blue for shell chrome, c-purple for extension content, c-gray for the ESC indicator. Annotate the transitions between states: user gesture or extension request triggers movement; ESC always returns to default. The diagram should make clear that ESC is a non-bypassable affordance present in all fullscreen states.
