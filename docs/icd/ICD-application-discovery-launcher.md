# Application Discovery and Launcher Contract

**Issue:** [#30](https://github.com/gobha-me/plinth/issues/30)

**Implementation owner:** [#31](https://github.com/gobha-me/plinth/issues/31)

**Baseline:** `478aa9cf17f1240ba3112054e735ab4dd09e5f85`

**Status:** Decision-complete contract

**Release numbering:** None. Historical references to “0.6.4” identify this
work's origin, not a release-version promise.

## 1. Authority

This document is authoritative for:

- the application and primary-panel vocabulary;
- the package metadata needed to describe an application;
- the RBAC-filtered application-discovery endpoint;
- Home, application-switcher, and primary-panel tab behavior;
- panel loading, activation, deactivation, replacement, dirty-state, and
  failure behavior exercised by the launcher;
- package-topology invalidation consumed by the launcher;
- launcher preferences;
- the `data-ipoint` vocabulary introduced with the launcher; and
- the browser, accessibility, responsive, realtime, and upgrade acceptance
  boundary for #31.

It resolves the panel-query open question left by
`ICD-0.6.3-panel-sdk-client-sdk.md` §15 and narrows the older launcher material
in `DESIGN-shell-v06x.md`. Where either historical document disagrees with this
contract, this document controls the launcher implementation.

The following shipped contracts remain authoritative and are not redefined:

- session credential/expiry/revocation semantics and the `SessionFilter`
  request context (with §6.2 adding a backend-unavailable outcome);
- additive RBAC with `kernel.admin` as the universal match;
- immutable `/ext/{name}/{version}/*` assets and strict same-origin CSP;
- the `plinth.panel` and browser client SDK method signatures;
- the durable realtime outbox and WebSocket subscription protocol; and
- package install, disable, enable, upgrade, uninstall, and recovery state
  vocabulary and safety invariants. Section 9.2 and the current-contract note
  in `ICD-0.4.5-package-lifecycle-transitions.md` extend their ordering with a
  fail-closed launcher quiesce/readiness boundary and atomic invalidation.

## 2. Current baseline

The implementation begins from these source facts:

- `plinth.panels` stores `(package_id, panel_id)`, `panel_type`, `slot_type`,
  and a JSON declaration (`migrations/schema.sql`, `plinth.panels`).
- `PanelEntry` currently recognizes `id`, `client_path`, optional `title`, and
  optional `icon`, while preserving unknown fields in memory
  (`src/kernel/packages/panels_manifest.{hpp,cpp}`).
- install and upgrade currently register every parsed entry as `primary` and
  retain only `client_path`, `title`, and `icon`
  (`src/kernel/packages/install_lifecycle.cpp`). There is no panel-query API.
- the shell's authenticated frame is static and does not invoke the panel
  loader (`client/shell/client/shell.js`).
- `loadPanel` imports a versioned module, creates a panel API, unmounts the
  prior active panel, mounts the new component, and fires activation
  (`client/shell/client/panels/loader.js`). It does not yet implement retained
  inactive instances, dirty navigation, rapid-switch arbitration, or
  generation replacement.
- the browser SDK already supplies one multiplexed authenticated WebSocket,
  observable reconnect state, and subscribe/unsubscribe handling
  (`client/shell/client/sdk.js`).
- shell preferences are stored in `ext_shell.user_preferences`, although the
  current browser theme/scale UI still uses local storage
  (`client/shell/server/handlers/preferences.*.js`,
  `client/shell/client/shell.js`).
- no production shell element currently carries `data-ipoint`.

These are migration inputs, not permission to infer unspecified behavior.

## 3. Non-goals

This contract does not define or implement:

- float panels, tray panels, settings panels, or `chrome_essential` fallback;
- content-type handler selection, default applications, `navigate`,
  `openFloat`, focus intents, or cross-application deep links; those belong to
  #45;
- nested shell-owned tabs inside a primary panel. Internal navigation within a
  panel remains extension-owned;
- a package-management/install tile, search, favorites, recent-content feed,
  grouping, dashboards, or Home widgets;
- public stability guarantees for alternative frontends;
- browser replay/resume, `since_seq`, optimistic filtering, generalized smart
  re-query, or completion of all historical browser test families; those
  belong to #32;
- source sequence or `superseded_seqs` semantics; those belong to #42;
- changing the panel SDK's public method list; or
- treating `data-ipoint` as an authorization mechanism, plugin injection API,
  or DOM-structure guarantee.

## 4. Terminology and topology

### 4.1 Package and application

One installed package contributes at most one **application** to the launcher.
The application's stable identifier is the package `name`. The package UUID is
the application's **generation** and changes when an upgrade swaps in a new
package row.

A package generation is discoverable as an application only when:

1. its durable `application_ready` marker is true, meaning its versioned panel
   assets are routable and it is the one generation selected for discovery;
2. it has at least one registered `primary` panel; and
3. at least one such panel is authorized for the requesting user.

The selected generation must be `ACTIVE` or `ACTIVE_FLAGGED`. Disabled,
uninstalling, failed, transitional, and superseded rows are never
discoverable. During upgrade quiesce there may intentionally be no selected
generation for that package. Section 9.2 owns the marker transition and its
unique-per-package invariant.

The shell package has an empty `panels.json`, so it is naturally absent from
its own launcher. Headless packages are also absent.

### 4.2 Primary panel and primary-panel tab

A registered `primary` panel is one independently loadable application view.
When an application has multiple authorized primary panels, the shell renders
one **primary-panel tab** per panel in the topbar. Historical documents and the
initial #31 wording called these controls “sub-tabs”; that alias is retired by
this contract because there is no second shell-owned tab level.

There is no separate top-level application tab strip. Applications are chosen
from Home or the app-identity switcher. The older proposed nested
`sub_tabs`-inside-a-panel shape is not adopted: it has no shipped loader,
context-update, or lifecycle semantics. A package that needs multiple
shell-owned views declares multiple flat primary-panel entries.

An application with exactly one authorized panel has no primary-panel tab
strip. Its one panel is selected directly.

### 4.3 Identifiers

- Application identity: package `name`.
- Generation identity: package UUID, exposed as an opaque string.
- Panel identity within a generation: `panel_id`.
- Runtime instance identity: `(generation, panel_id)`.

Code must not use a title, icon, array position, or module URL as identity.

## 5. Package metadata and persistence

### 5.1 Application metadata in `manifest.json`

The package manifest gains two optional fields:

```json
{
  "name": "notes",
  "display_name": "Notes",
  "icon": "edit-3"
}
```

`display_name` is a non-empty UTF-8 string of at most 128 Unicode scalar
values. `icon` is a shell icon token matching `^[a-z][a-z0-9-]{0,63}$`.
Neither field participates in package identity, capability namespaces, asset
paths, or RBAC.

Compatibility behavior is exact:

- absent `display_name` falls back to the package name with ASCII hyphens
  replaced by spaces and the first ASCII letter of each word uppercased;
- absent, invalid-in-a-legacy-row, or unknown `icon` renders a monogram derived
  from the display name;
- new package validation rejects a present field of the wrong type or outside
  its bounds; and
- the fields persist in `plinth.packages.manifest_json`; no application table
  is introduced.

Identity color is not package-controlled. The shell selects a stable color
from its design-token palette using the package name. Raw CSS colors and
extension-supplied class names are never returned by discovery or injected
into shell chrome.

### 5.2 Primary-panel metadata in `panels.json`

The launcher-owned entry shape is:

```json
{
  "id": "editor",
  "client_path": "editor.js",
  "title": "Editor",
  "icon": "edit-3",
  "rbac_rule": "notes.ui.editor",
  "order": 10
}
```

The existing `id` validation remains unchanged. The launcher adds these rules:

- `title` is optional for compatibility, but when present is a non-empty UTF-8
  string of at most 256 Unicode scalar values. Its fallback is the panel ID
  transformed by the same hyphen/underscore-to-space ASCII title algorithm.
- `icon` is optional and follows the application icon-token grammar. An absent
  or unknown icon inherits the application icon/monogram.
- `rbac_rule` is required for every newly installed panel. It must be a valid,
  non-empty rule declared by the same package's `rbac.json`. Cross-file
  validation rejects a missing, foreign, or unknown rule.
- `order` is an optional integer in `[0, 10000]`, default `0`. Duplicate order
  values are allowed and break ties by `panel_id` ascending.
- duplicate panel IDs are a structured manifest error before registration.
- `client_path` must resolve beneath `client/panels/`. The historical
  cross-file fallback to `client/components/` is retired for launcher panels
  because the shipped loader and canonical module URL always use `panels/`.
  New validation rejects a path that exists only under `client/components/`.
  It also applies the asset router's component grammar before filesystem
  lookup: the path is relative; every `/`-separated component is non-empty and
  neither `.` nor `..`; and NUL, backslash, leading slash, trailing slash, and
  consecutive slashes are forbidden.
- entries continue to register as `primary`; #31 does not activate historical
  `type`, `slot_type`, float, settings, or tray declarations.

The canonical complete entry, including recognized and forward-compatible
unknown fields, is stored in `plinth.panels.declaration`. Registration must no
longer reconstruct a lossy three-field object. `order` remains in the JSON
declaration and does not require its own kernel-schema column.

Rows installed before #31 may lack `rbac_rule`. They are omitted from every
discovery response. The server must not infer a rule from package name,
capability name, group membership, or `kernel.admin`; operators restore such a
panel by upgrading/reinstalling a conforming package. This is intentionally
fail-closed because no production launcher previously exposed these rows.

### 5.3 Discovery-readiness marker

#31 adds this kernel-owned column and index:

```sql
ALTER TABLE plinth.packages
  ADD COLUMN application_ready BOOLEAN NOT NULL DEFAULT FALSE;

CREATE UNIQUE INDEX uniq_packages_name_application_ready
  ON plinth.packages(name)
  WHERE application_ready;

ALTER TABLE plinth.packages
  ADD CONSTRAINT chk_packages_application_ready_state
  CHECK (NOT application_ready OR state IN ('ACTIVE', 'ACTIVE_FLAGGED'));
```

`application_ready` is lifecycle state, not package-authored metadata. True
means the generation's immutable versioned asset route is installed and that
generation is the sole discovery candidate for its package name. The partial
unique index makes split-brain discovery impossible, while the check constraint
prevents a transitional or superseded fallback from being advertised.

The migration does not blindly mark existing rows ready. At bootstrap,
lifecycle reconciliation first restores and verifies each eligible
`ACTIVE`/`ACTIVE_FLAGGED` panel-bearing generation's versioned asset route and
backend authority, then sets the marker and enqueues invalidation in one
transaction. Headless packages remain unready because this marker is
launcher-specific. Malformed or missing package assets remain unready and
operator-visible.

## 6. Authenticated discovery API

### 6.1 Route and response

The kernel registers:

```text
GET /api/frontend/applications
```

The route uses `SessionFilter`. It is a kernel resource endpoint, not an
extension capability and not a direct browser database query. Per-row RBAC
filtering and asynchronous database access do not fit the static route-rule or
synchronous Tier-1 handler shapes.

Success is `200 application/json`:

```json
{
  "schema_version": 1,
  "applications": [
    {
      "id": "notes",
      "generation": "5b68a4c8-30a8-49fa-9fd9-78cf58f2e734",
      "version": "1.2.3",
      "title": "Notes",
      "description": "Markdown notes",
      "icon": "edit-3",
      "panels": [
        {
          "id": "editor",
          "title": "Editor",
          "icon": "edit-3",
          "module_url": "/ext/notes/1.2.3/panels/editor.js"
        }
      ]
    }
  ]
}
```

`generation` is opaque to the client except for equality comparison. The
server constructs `module_url` from the validated package name and version,
then appends each validated relative `client_path` segment under `panels/`
with that segment encoded independently. It must not encode `/` separators as
data or concatenate an unvalidated path. The response never exposes a
filesystem path.

Applications are ordered by application ID using bytewise ascending order.
Panels are ordered by `(order, panel_id)` using integer then bytewise ascending
order. The browser may apply the user's launcher order after parsing; server
order remains the deterministic fallback.

The response includes only fields shown above. In particular it excludes:

- `rbac_rule`, group or grant data;
- denied applications, panels, counts, or placeholders;
- raw `manifest_json` or panel declarations;
- install provenance and installer identity; and
- transition, failure, or retired package rows.

Responses use `Cache-Control: no-store` and `Vary: Cookie, Authorization`. The
route accepts no pagination, search, filtering, or client-supplied identity
arguments.

### 6.2 Authorization algorithm

For the authenticated user, one database snapshot performs:

1. Load the user's distinct effective rule strings, including rules granted to
   the implicit built-in `everyone` group as specified below.
2. Select the one `application_ready` package generation per package name and
   its `primary` panel rows.
3. Validate each stored declaration defensively. A malformed or missing rule is
   denied, logged with package/panel identity, and omitted.
4. Admit a panel when its non-orphaned required rule is in the effective set or
   the set contains `kernel.admin`.
5. Discard applications with zero admitted panels.
6. Only then materialize application metadata and response objects.

Filtering is server-side. Sending all rows and hiding denied elements in
JavaScript is forbidden. The browser treats the response as display data, not
an authorization lease; every capability call remains independently
authorized by the kernel.

A failure after authentication to load effective rules, query panels, or
obtain the discovery snapshot returns `503` with exactly:

```json
{"error":"service_unavailable","message":"Application discovery is temporarily unavailable"}
```

It must not be converted to an empty authorized set. Authentication failure
returns `401` with exactly one of these two-string-field bodies:

```json
{"error":"not_authenticated","message":"No authentication token provided"}
{"error":"not_authenticated","message":"Invalid or missing authentication token"}
{"error":"session_expired","message":"Session has expired"}
{"error":"session_revoked","message":"Session has been revoked"}
```

No response includes query or exception detail. `{ "applications": [] }` is
reserved for a successful authenticated query with no authorized application.

`SessionFilter` currently maps session/PAT database exceptions to
`not_authenticated`. #31 must add the shared backend-unavailable outcome fixed
by the current-contract notes in the session and PAT ICDs. A failure while
validating credentials, before this handler runs, returns `503` with exactly:

```json
{"error":"service_unavailable","message":"Authentication service is temporarily unavailable"}
```

That generic authentication body applies to every `SessionFilter`-protected
route; the application-specific body above applies only after authenticated
discovery begins. Focused middleware tests must prove invalid tokens remain 401
while injected session and PAT database failures return the generic 503.

No denial audit is emitted merely because another application's row was
filtered from a bulk discovery response. Malformed stored declarations and
endpoint failures remain operator-visible diagnostics without disclosing them
to the requester.

## 7. Shell state machines

### 7.1 Catalog state

The catalog has these states:

```text
unrequested -> loading -> ready
                    \-> failed
ready -> refreshing -> ready
                  \-> stale
```

- Initial `loading` renders shell chrome plus an `aria-busy` main region.
- `ready` contains an authoritative catalog, including a legitimate empty
  catalog.
- Initial `failed` renders a non-destructive Retry action and no application
  metadata.
- A refresh failure produces `stale`: the last successful catalog may remain
  visible with a retry/status indication, but no newly discovered item is
  admitted. Capability calls remain kernel-authorized.
- `401` is not stale: all extension panels are destroyed and the existing
  login/session-expired flow takes ownership.
- When a later successful refresh removes a panel, removal is applied before
  rendering any other catalog change.

Only the newest refresh attempt may commit. Every request owns a monotonically
increasing token and, where supported, an `AbortController`. Completion from an
older token is ignored.

### 7.2 Navigation state

The shell is in exactly one navigation state:

```text
Home
Application(application_id, panel_id, generation)
```

Home is always available and is the initial state after successful login. The
last-used application is highlighted but is not opened automatically.

Application selection chooses the remembered still-authorized panel for that
application, otherwise its first panel in catalog order. Selecting the current
application/panel is an idempotent no-op. If a stale target is absent from the
latest catalog, the shell reports the generic text “Application unavailable”
and returns to Home; it does not distinguish denial, disable, or uninstall.

### 7.3 Panel instance state

Each `(generation, panel_id)` instance follows:

```text
unloaded -> loading -> mounted/inactive -> active
                  \-> failed
active <-> mounted/inactive
active|mounted/inactive|failed -> destroyed
```

Loading performs, in order:

1. Reconfirm that the target exists in the current catalog.
2. Import `module_url` under the existing same-origin CSP.
3. Require a default-export factory.
4. Create a fresh panel API and call the factory.
5. Render the returned Preact component in a shell-owned panel container.
6. Make the container current, then synchronously fire `onActivate`
   callbacks after `render` returns. “After render” does not promise a browser
   paint or animation frame.

Visited instances remain mounted while authorized and in the same generation.
Inactive containers have the HTML `hidden` attribute and are absent from the
accessibility tree. Returning to one removes `hidden` and fires `onActivate`
again. Only the active instance receives shortcut dispatch.

Retention is bounded to the active instance plus at most eight clean inactive
instances across the shell. After a successful activation, the shell destroys
least-recently-active clean inactive instances until the bound is restored.
An instance that becomes dirty during deactivation is destroyed instead of
entering the inactive cache; the shell never retains a dirty inactive
instance. If an already-inactive instance asynchronously calls `setDirty(true)`,
the shell destroys it immediately. Eviction uses the destruction sequence
below, removes any associated unload guard, and does not change launcher
preferences. All instances are destroyed on logout/session loss.

Destroying an instance means:

1. fire `onDeactivate` if it is active;
2. render `null` into its container so Preact effect/unmount cleanup runs;
3. unbind its panel API and clear callbacks/shortcuts; and
4. remove its container and references.

There is no new public `onDestroy` method. Preact unmount is the destruction
hook.

### 7.4 User navigation and dirty state

For a user-requested Home, application, or primary-panel tab switch:

1. If the active panel is dirty, show one modal “Discard changes and continue?”
   confirmation.
2. While that confirmation is open, navigation controls are disabled and
   additional navigation intents are ignored.
3. Cancel keeps the active instance and focus unchanged.
4. Confirm authorizes destruction of the old instance. Once the candidate has
   imported, constructed, and rendered successfully while hidden, destroy the
   old instance, recompute/remove the unload guard, make the target current,
   and fire its activation callbacks. The discarded instance is never retained
   or restored.

If the target is not yet mounted, the current authorized panel remains active
while the candidate imports and renders. The current panel is deactivated only
after that pre-commit preparation succeeds. Import, factory, or initial-render
failure leaves the current panel active. After a confirmed dirty discard, an
activation-callback failure cannot restore the destroyed panel; §7.6 owns that
post-commit outcome.

Dirty state cannot veto security or topology removal. Logout, session loss,
RBAC loss, disable, uninstall, or removal of the active panel destroys it and
returns Home (or selects the first remaining authorized panel in the same
application) without a cancellable prompt. The shell may warn that unsaved
state was discarded, but must not retain denied DOM.

When the active instance is dirty, the shell installs one page-level
`beforeunload` guard. It removes the guard when that instance becomes clean or
is destroyed. Because dirty inactive instances are forbidden, there is no
hidden source of an unload guard.

### 7.5 Rapid switches

The latest accepted navigation intent wins. Each load has a navigation token.
An import cannot necessarily be cancelled, so a stale completion must be
unmounted/unbound without becoming visible, changing preferences, or firing
activation. At most one target transition may commit at a time.

The dirty confirmation is a serialization barrier as described in §7.4. A
realtime catalog refresh may invalidate either the current or pending target;
forced removal wins over the user transition.

### 7.6 Callback and render failures

Every panel container has a panel-local Preact error boundary. A panel render
failure replaces only that panel with a fallback containing Retry and Return to
Home; topbar, Home, app switcher, and other retained panels remain usable. The
existing shell-wide boundary remains the last resort for shell faults.

Module import, missing factory, factory throw, render failure, and activation
callback failure are panel failures. They emit the boundary audit with the
authorized application and panel IDs and a production-redacted error. #31 must
extend `shell.audit.emit`'s accepted/sanitized detail with bounded
`application_id`; the current handler retains only `panel_id`. Retry destroys
the failed instance and creates a new one.

A deactivation callback failure is recorded but cannot stop hiding or
destroying a panel. Remaining deactivation callbacks follow the existing
first-throw-aborts-chain behavior; cleanup then continues.

If activation of a candidate fails while a clean prior authorized panel is
retained, the candidate is destroyed and the old panel is made active again.
If a confirmed dirty prior panel was already destroyed, or there was no prior
panel, the failed target's fallback becomes current and offers Retry and Return
to Home. The shell never resurrects the discarded instance.

## 8. Upgrade and replacement

The application ID remains stable across upgrade; `generation`, `version`, and
versioned module URLs change. Plinth's shared package RBAC, capability, runtime,
and migration state is not generation-versioned, so #31 must not present an old
UI generation as a coherent fallback while those resources change.

Upgrade is therefore an explicit fail-closed remove-then-admit replacement:

1. Before any migration, RBAC, capability, symlink, route, or runtime mutation,
   the package lifecycle fences new capability calls for the package namespace,
   drains admitted calls, clears its readiness marker, and durably invalidates
   discovery as §9.2 specifies.
2. A catalog refresh removes every old-generation instance immediately. Dirty
   state cannot defer or veto the removal. Until the new generation becomes
   ready, Home shows no tile for that application.
3. After lifecycle cutover makes the new route, authority, capabilities, and
   runtime usable, it admits the new generation and invalidates discovery.
4. The application reappears with its stable application ID and new generation,
   version, and module URL. The remembered panel is selected if it still exists
   and is authorized; otherwise the first authorized panel is selected.

If an old module import races quiesce or returns 404, its navigation token is
discarded and the shell performs one authoritative catalog refresh before
showing the generic unavailable state. It never retries an old URL. A failure
to prepare or activate the new UI uses the ordinary panel fallback; the old
generation is not resurrected.

An upgrade that fails after quiesce may re-admit the old `ACTIVE` generation
only after reconciliation has re-materialized and verified its old route,
RBAC declarations, capability registrations, runtime, and ingress fence. Sticky
schema migrations remain governed by the package lifecycle contract. If that
verification fails, the package stays unready and absent from the launcher for
operator repair.

## 9. Realtime catalog invalidation

### 9.1 Channel and authority

The kernel publishes an opaque invalidation on:

```text
plinth:system:applications.changed
```

Subscription derives the rule:

```text
kernel.realtime.subscribe.applications.changed
```

The kernel registers that rule and grants it to the built-in `everyone` group.
The event payload is an empty JSON object. It contains no package name,
version, action, panel, rule, grant, or authorized-item count. A recipient
learns only that its authoritative application view may have changed.

`everyone` is an implicit virtual membership for every authenticated user; it
does not require or create a `plinth.group_members` row. #31 updates the shared
effective-rule loaders used by HTTP capabilities, WebSocket authority, and
discovery so they always union non-orphaned rules granted to `everyone` with a
user's explicit-group rules. Anonymous semantics remain those in
`architecture/01-identity.md`; this change does not make an authenticated
route public.

The event is a refresh hint, never a catalog delta and never authorization.
The shell refetches `GET /api/frontend/applications` and applies the result
through §7.

### 9.2 Producers and ordering

Every transition that can change the discovery predicate publishes one
invalidation:

- successful install becoming `ACTIVE`;
- disable;
- enable;
- upgrade quiesce making the old generation non-discoverable;
- successful upgrade generation cutover;
- uninstall becoming non-discoverable; and
- recovery/reconciliation that completes any of those visible transitions.

Publication uses the durable realtime outbox, not direct in-process WebSocket
fan-out. The transaction that changes `application_ready` also enqueues the
invalidation; visibility never commits without its durable refresh hint.
Lifecycle transitions use this ordering:

- install and enable install and verify the immutable versioned asset route
  and its authority/runtime while the generation is still unready, then
  atomically set it ready, commit its visible state, and enqueue invalidation;
- disable and uninstall first fence new package capability calls, then
  atomically clear readiness, commit the non-visible state, and enqueue
  invalidation before draining and removing runtime/routes; and
- upgrade acquires the package-name lifecycle lock and installs the capability
  ingress fence before any shared-state mutation. It drains existing calls,
  then atomically clears old readiness and enqueues invalidation. The existing
  T3/T4 swap proceeds with both generations unready. Only after the new route,
  RBAC, capabilities, symlink, and runtime are verified does a transaction set
  the new `ACTIVE`/`ACTIVE_FLAGGED` row ready and enqueue invalidation; the
  ingress fence is released after that commit. The old row is already unready
  before it becomes `SUPERSEDED`, and its route-removal behavior remains the
  package lifecycle contract's behavior.

If initial quiesce fails, no shared-state mutation starts. If later upgrade or
route preparation fails, readiness never moves to the new generation. Recovery
either restores and verifies the old active generation as §8 requires before
re-admitting it, or leaves the application absent. Startup and lifecycle
reconciliation lock by package name and idempotently compare readiness against
state, route, authority, capability, runtime, and fence state. The state check
constraint rejects a ready transitional/superseded row and the unique index
rejects two ready generations. Garbage collection lock-rechecks and excludes
any ready row defensively. Bounded backoff and operator diagnostics apply to
repeated repair failure. Duplicate invalidations are harmless.

The readiness boundary guarantees that discovery never returns a newly
admitted generation before its module route and backend authority are usable.
A previously returned URL can still race a later quiesce; that race fails
closed through the ingress fence and the one-refetch behavior in §8.

The shell also refreshes after each successful WebSocket connection/reconnect
and when the document returns to visible state. Events may duplicate, coalesce,
or arrive after a newer refresh; request-token arbitration makes those cases
idempotent.

### 9.3 Terminal realtime states and authority changes

The current WebSocket authority closes with terminal `auth_failed` when its
effective rule snapshot changes, and the client SDK deliberately does not
reconnect terminal authentication failures automatically. Its complete
terminal set is `auth_failed`, `auth_timeout`, `already_connected`,
`not_authenticated`, `session_expired`, and `session_revoked`. The shell must
handle every one; an unknown future terminal code uses the `auth_failed`
fail-closed path.

For any terminal code, the shell immediately destroys every extension panel
instance, suppresses stale catalog DOM, and cancels catalog/navigation work.
It then applies these non-looping outcomes:

- `not_authenticated`, `session_expired`, and `session_revoked` hand control
  to the existing login/session-expired flow. They do not reconnect.
- `auth_failed` and `auth_timeout` revalidate through the HTTP session
  endpoint. An invalid session enters the login flow. For a valid session, one
  recovery attempt calls `reconnectRealtime()` exactly once, waits for the
  system-channel subscription acknowledgement, then refetches discovery and
  renders only that fresh result.
- `already_connected` renders no catalog and explains that realtime is active
  in another tab. It never reconnects automatically. An explicit “Use this
  tab” action performs the valid-session recovery sequence once, knowingly
  displacing the other tab; that tab in turn remains stopped rather than
  reclaiming the session.

If validation, catalog fetch, reconnect, or subscription fails, the shell
remains in a no-panel failed state. A bounded user Retry may start one new
recovery attempt for `auth_failed`/`auth_timeout`; it must not restore old
catalog or panel DOM or create an automatic reconnect loop. This path covers
grant, revoke, membership removal, timeout, and multi-tab displacement without
an additional RBAC event channel.

### 9.4 Subscription-ready seam

The shipped realtime state becomes `connected` before channel subscription
acknowledgements, so connection state alone cannot close the initial
snapshot/subscription race. #31 extends the existing
`subscribe(channel, handler, options)` options object with an optional
`onReady` callback; it does not add or rename an SDK method. `onReady` fires
once for each connection epoch after that entry's channel appears in a valid
`subscribed` acknowledgement. A denial or terminal/transport failure reaches
`onError` and never fires `onReady` for that epoch.

When a new entry subscribes to a channel already granted in the current
connection epoch, its `onReady` is queued immediately after registration. A
reconnect requires and produces a new acknowledgement/readiness notification;
prior readiness is never reused across epochs.

At initial authenticated shell startup and terminal recovery, the shell:

1. installs the applications-changed handler;
2. waits for its `onReady` acknowledgement;
3. starts the authoritative discovery request; and
4. renders only the newest completed refresh token.

An invalidation after acknowledgement but before or during the initial request
starts a newer refresh token. An invalidation before the initial request starts
is already reflected by that later database snapshot. This ordering loses no
committed topology change and requires no replay cursor. Unsubscribe cancels a
pending readiness waiter without firing it.

#31 does not add `since_seq`, browser replay/resume, source-sequence handling,
or optimistic delta application. #32 owns browser replay/client-runtime
reconciliation, and #42 owns source/superseded sequence policy.

## 10. Launcher preferences

The canonical user-scoped preference key is `shell.launcher`:

```json
{
  "version": 1,
  "last_application": "notes",
  "last_panels": {
    "notes": "editor"
  },
  "application_order": ["notes", "files"]
}
```

It is read and written through `shell.preferences.get/set`, hence stored in
`ext_shell.user_preferences`. Local storage is not authoritative for launcher
state.

Validation rules:

- the value must be an object with `version == 1`;
- identifiers must satisfy their package/panel grammars;
- `last_panels` and `application_order` are capped at 256 entries each;
- duplicate order entries keep their first occurrence;
- unknown, removed, or unauthorized IDs are ignored, never rendered;
- authorized IDs omitted from `application_order` append in server order; and
- malformed preference data is ignored as the empty default and does not block
  the launcher.

The shell writes after a successful committed navigation, not when a candidate
load starts. Preference-write failure is non-fatal and visible as a bounded
non-modal status; it must not revert navigation or retry forever.

The preference remembers/highlights the last application and the last panel
per application. It does not authorize access, auto-open an application after
login, preserve component memory across reload, or define content-type default
applications (#45).

## 11. `data-ipoint` contract

The launcher renders these minimum seams:

| Value | Layer | Owner/surface |
|---|---|---|
| `shell.topbar` | `shell` | topbar root |
| `shell.home` | `shell` | Home navigation control |
| `shell.appIdentity` | `shell` | active-app/switcher control |
| `shell.appSwitcher` | `shell` | application switcher surface |
| `shell.home.launcher` | `shell` | authorized Home application list |
| `shell.content` | `shell` | main shell content host |
| `ext.<application>.primaryTabs` | `extension` | shell-rendered primary-panel tablist |
| `ext.<application>.<panel>.primaryPane` | `extension` | one panel container |

Every seam has both `data-ipoint="..."` and
`data-ipoint-layer="shell|extension"`. Package and panel components in dynamic
values are the already validated IDs. Values are stable dot-separated tokens;
display strings never enter them. `data-ipoint-side`, when used, controls label
placement only and is not contractual.

Attributes exist in production so diagnostics and browser tests observe the
same DOM, but code must not use them for authorization, lifecycle selection,
or event routing. Denied panels produce no container and therefore no ipoint in
the document.

The visual blue-shell/amber-extension overlay is development-only. It may be
exposed in development UI only when `window.__PLINTH_PRODUCTION__ === false`.
URL parameters, local storage, package content, and panel code cannot switch a
production document into development mode. The overlay uses design tokens,
does not alter layout or pointer handling, and includes a text legend; color is
not its only distinction.

This vocabulary marks ownership boundaries only. It does not ratify future
slots, augmentation, or DOM injection.

## 12. Accessibility and responsive behavior

### 12.1 Semantics and keyboard

- The topbar is a `header` containing a labelled navigation region. Content is
  one `main` landmark.
- Home is a real `button` with accessible name “Home” and selected state.
- App identity is a button with `aria-expanded`, `aria-haspopup="menu"`, and
  `aria-controls`. The switcher uses menu/menuitem semantics, Up/Down,
  Home/End, Enter/Space, and Escape; closing returns focus to the trigger.
- Home applications are a semantic list of buttons/links styled with CSS Grid,
  not an ARIA `grid`. Normal Tab navigation applies.
- Multiple panels use `tablist`, `tab`, and `tabpanel`. Tabs have roving
  `tabindex`; Left/Right (or Up/Down if vertical), Home, and End move focus.
  Enter/Space performs manual activation so focus movement does not trigger a
  potentially expensive import.
- Each tab owns a stable `aria-controls` target and each panel has
  `aria-labelledby`. Inactive panels use `hidden`.
- Loading sets `aria-busy`; empty, failure, stale, and forced-removal messages
  use a bounded polite live region. Repeated realtime events do not repeat the
  same announcement.
- Dirty confirmation uses a modal dialog with initial focus, focus containment,
  Escape-as-cancel, and focus restoration.

After a Home action, focus moves to the Home heading. After application
selection from Home or the switcher, focus moves to the shell-owned active
`tabpanel` container (`tabindex="-1"`). The shell does not inspect the
extension component tree for a heading. Primary-panel tab activation retains
focus on the selected tab. Forced removal moves focus to the selected
replacement tab or Home heading.

### 12.2 Responsive behavior

- The Home list uses tokenized responsive columns and collapses to one column
  without changing item order or semantics.
- The primary-panel tab list remains one row and scrolls horizontally when it does not
  fit; authorized tabs are not silently dropped. Keyboard focus scrolls the
  focused tab into view.
- The app switcher is an anchored menu when space permits and a modal sheet on
  narrow viewports. Both forms expose the same items, ordering, names, and
  keyboard outcomes.
- No launcher action depends on hover. Touch targets and focus indicators use
  the shipped shell token contract.
- Reduced-motion preferences suppress non-essential transitions. Text zoom and
  the shipped 80–175% shell scale must not hide navigation or create page-wide
  horizontal overflow; the primary-panel tab scroller is the bounded exception.

## 13. Ownership boundaries

### #31 — required implementation

#31 owns all behavior necessary to make this contract real:

- typed manifest additions, cross-file validation, lossless declaration
  persistence, and fixtures;
- the readiness schema/index and lifecycle cutover/reconciliation ordering;
- the authenticated discovery route, server-side RBAC filtering, shared
  implicit-`everyone` effective rules, and authentication-backend error
  taxonomy;
- shell catalog, Home, app switcher, primary-panel tabs, and
  loading/error/empty states;
- bounded retained primary-panel instances, dirty discard confirmation,
  rapid-switch arbitration, panel-local boundaries, and generation
  replacement;
- opaque lifecycle invalidation production and consumption;
- launcher preferences and all `data-ipoint` seams;
- keyboard, focus, responsive, security, realtime, and upgrade browser tests
  in the bounded matrix below.

### #32 — deferred breadth, not #31 prerequisites

#32 owns re-enumeration of every remaining historical ICD browser family,
general client-runtime/smart-query reconciliation, and browser
resume/replay/resync coverage. It may extend launcher lifecycle coverage after
#31, but #31 cannot defer its core switch, dirty, boundary, realtime, upgrade,
or accessibility acceptance tests to #32.

### #42 — sequence policy

#42 decides source sequences and `superseded_seqs`. The launcher uses opaque
invalidation plus authoritative refetch and therefore does not depend on that
decision.

### #45 — later navigation surfaces

#45 owns trays, content-type/default-application resolution, navigation and
focus intents, `openFloat`, and missing-handler fallbacks. This contract's
Home/app/primary-panel tab navigation must not predefine those protocols.

## 14. Bounded acceptance matrix

| Layer | Required #31 evidence |
|---|---|
| Manifest unit | application metadata bounds/fallbacks; panel rule/order parsing; duplicate IDs; foreign/missing rule rejection; `client/panels/` accepted; components-only, empty/dot, slash-empty, trailing-slash, backslash, and NUL paths rejected; unknown-field round trip |
| Registration integration | full declaration retained on install and upgrade; legacy row without rule denied; uninstall cascade unchanged |
| RBAC integration | `everyone` grant reaches authenticated session and PAT without stored membership; historical `everyone` membership ignored/removed; add/remove membership rejected; HTTP, capability, WebSocket, and discovery effective sets agree |
| Discovery unit/integration | unauthenticated or invalid-token 401; injected auth-DB and query failures 503 with their distinct exact bodies; admin universal match; non-admin implicit-`everyone` grant; ordinary allow/deny; partial-panel grant; malformed row fail-closed; only ready active/flagged rows included and all other states excluded; deterministic ordering; no denied metadata; empty success |
| Shell unit | catalog state transitions; newest-refresh wins; preference normalization; selection fallback; stale target; same-target no-op |
| Panel lifecycle browser | first load; retained deactivate/reactivate; eight-entry LRU eviction/destruction; only active shortcuts; dirty cancel/discard with unload-guard removal; dirty-on-deactivate and inactive-asynchronous-dirty destruction; dirty-confirm plus target-activation failure; factory/import/activation/render failure; Retry; forced denied/uninstalled cleanup |
| Rapid navigation browser | two and three quick targets; stale import completion discarded; preference records only committed target; catalog removal beats pending navigation |
| Realtime integration | non-admin `everyone` subscription; initial/recovery acknowledgement before snapshot; topology change between acknowledgement and snapshot completion; install and enable appear; disable and uninstall disappear; duplicates coalesce; reconnect and visibility refetch; live grant/revoke/member-removal terminal-authority revalidation and fail-closed dirty-DOM cleanup; each terminal code; multi-tab displacement without ping-pong; active removal focus/cleanup |
| Lifecycle cutover | discovery plus module fetch at every install/enable/disable/uninstall/upgrade barrier; no newly admitted unroutable generation; upgrade quiesce/fence before migration and registration; removed/renamed panel rule and capability; injected marker/outbox transaction failure; uncertain commit outcome and restart reconciliation; failed-upgrade verified restoration or continued absence; unique-ready enforcement; zero-retention GC excludes ready rows |
| Upgrade browser | stable application ID across disappear/reappear; changed generation/versioned URL; dirty old panel forced removal; old import race refetch; new candidate failure does not resurrect old; remembered/removed panel selection fallback |
| Accessibility | landmarks/names; switcher keyboard/Escape; manual tab activation and roving focus; dialog focus; busy/live states; denied nodes absent from accessibility tree |
| Responsive | wide and narrow Home; scrollable seven-tab stress case; narrow switcher sheet; 175% scale; keyboard focus remains visible |
| Ipoints | exact value/layer vocabulary; dynamic ID escaping; denied seams absent; development overlay and production non-enableability |

The production browser gate must use a task-owned kernel/database and the real
discovery endpoint, package lifecycle, versioned asset server, SessionFilter,
RBAC tables, durable realtime path, shell module graph, and panel loader. A
mock-only browser test may supplement but cannot replace that path.

## 15. Implementation map

The expected implementation surface is bounded to:

- `src/kernel/packages/manifest.{hpp,cpp}` — typed application metadata;
- `src/kernel/packages/panels_manifest.{hpp,cpp}` and cross-file validation —
  rule/order/duplicate checks and preservation;
- `src/kernel/packages/install_lifecycle.cpp`, capability dispatch/drain,
  asset-route bootstrap, garbage collection, and schema migration — complete
  declaration registration, upgrade ingress fencing, `application_ready`
  ownership/constraints, atomic outbox publication, cutover ordering, and
  reconciliation;
- a focused kernel frontend applications handler plus bootstrap registration —
  authenticated query, effective-rule loading, response construction, and
  errors;
- auth middleware/token validation — distinguish invalid credentials from an
  unavailable authentication backend;
- shared effective-rule loading, group-membership handlers/migration, and
  RBAC/bootstrap ownership — implicit `everyone` grants, immutable virtual
  membership, and the system-channel subscription rule;
- `client/shell/client/shell.js`, `client/shell/client/sdk.js`,
  `client/shell/server/handlers/audit.emit.js`, and focused launcher
  modules/styles — shell state, rendering, preferences, subscription-ready and
  terminal-authority recovery, application-attributed boundary audit, and
  accessibility;
- `client/shell/client/panels/loader.js` and `panel_api.js` — retained
  instances, tokens, dirty state, cleanup, and replacement;
- package/parser/kernel integration tests and the real production browser
  harness.

Implementation may split modules for ownership and testability. It must not
solve the contract by exposing `plinth.panels` to extension SQL, by returning
unfiltered rows, by client-side RBAC checks, by direct WebSocket publication,
by sleeps, by retaining denied DOM, or by weakening the existing CSP and
versioned-asset contracts.
