# Shell Float Contract

**Issue:** [#43](https://github.com/gobha-me/plinth/issues/43)

**Implementation owner:** [#44](https://github.com/gobha-me/plinth/issues/44)

**Status:** Independently reviewed delivery specification with a bounded shell
mechanism implementation under #44; the production extension journey and
persistence adapters remain pending #46. The five-live-owner policy was selected
by the maintainer on 2026-10-01. The remaining rules define the bounded delivery
delta, not a ratified current subsystem or an API stability promise.

**Reviewed source baseline:** `afa11538b69c6b63a4c5983211bb69c9ae5c6113`.
The documentation ownership decision in #41 is applied separately; this source
baseline identifies observations, not a future implementation completion SHA.

**Release numbering:** None. Historical “0.6.5” float milestone names are
traceability aliases, not a release or version promise.

## 1. Authority and delivery boundary

This document specifies shell-owned float chrome, admission accounting,
lifetime, responsive presentation, focus, descriptor persistence and recovery.
It explicitly replaces the float-specific proposals in
[the shell design](../design/DESIGN-shell-v06x.md), sections 3.2, 3.3, 3.5,
3.6, 3.7 and 3.8 and its float milestone summary in section 9, only for those
surfaces. In particular, automatically
minimizing an old window cannot create capacity under the approved five-live
limit. Unrelated primary, tray, resolver and navigation design is not ratified
or replaced here. Follow the
[documentation authority rules](../contracts/README.md#authority-and-conflicts)
when reconciling a later implementation with this delivery specification.

Current, independently implemented behavior remains distinct:

- The [launcher contract](ICD-application-discovery-launcher.md) owns primary
  discovery, primary retention, dirty navigation and package-generation
  quiesce. Current registration/discovery advertises primary panels only.
- The current panel loader owns primary component instances and their cleanup.
  Its clean-inactive cache and destruction of dirty inactive primaries are not
  float policy. A minimized float may remain dirty and alive.
- `plinth.panel.openFloat(contentType, context)` currently rejects with
  `NotImplementedError`; `navigate` and `requestFocus` are also stubs. There
  is no production float permission catalog or admission adapter. Internal
  mechanism and data validators do not change those SDK signatures.
- User-scoped shell preferences and theme/scale primitives exist. They are
  storage and presentation substrates, not evidence of implemented float
  restore, UTF-8 descriptor validation or five-owner enforcement.

#44 owns the float mechanism and its real-browser proof. Any typed target
discovery/admission seam it needs must receive its own implementation review;
the primary-only catalog cannot be repurposed as proof of float authorization.
[#45](https://github.com/gobha-me/plinth/issues/45) owns content-type resolver
tiers, defaults and navigation/open-float intent protocols;
[#46](https://github.com/gobha-me/plinth/issues/46) implements those adapters.
A shell-internal resolved-target fixture can verify the #44 mechanism, but
cannot be advertised as working extension `openFloat` resolution. A missing
adapter remains unsupported rather than selecting a guessed handler. The
sequencing discrepancy between #44's broad outcome and the later adapter is
an explicit review dependency, not permission to absorb #45/#46.

### 1.1 Implemented mechanism versus pending delivery

The shell owns one document-wide five-reservation controller and one
interaction dispatcher above authenticated frames. A frame owns its float
manager, retained Preact content, responsive chrome, geometry leases and shared
primary/float dirty-confirmation lease. Session replacement retires those
owners synchronously before realtime callbacks or new-session publication.
Unsettled native imports retain opaque document reservations, not old-user
labels, descriptors, APIs or DOM. Verified cleanup releases capacity;
uncertain cleanup permanently refuses admission for that document and exposes
a manual reload warning. Minimizing does not release a reservation.

`FloatManager` accepts an explicitly injected, synchronous reviewed-target
port. No production instance installs that port: admission refuses without
importing or reserving capacity. Real-browser mechanism tests supply local
resolved-target and panel fixtures, not an extension API or permission grant.
The shell does not expose the manager through globals or `plinth.panel`.

The internal preference controller implements bounded reads, sequential
restore, trailing writes and truthful timeout/acknowledgement fencing through
optional reviewed ports. Production ports remain absent. In particular,
the ordinary generic preferences capability is **not** used as a substitute
for a reviewed safe projection or owner/generation/policy-guarded commit.
All mechanism-only panels are live-only; Jump to app is visibly disabled.

#46 must deliver and independently review current target authority,
`openFloat` resolution, safe persistence projection and guarded commits,
restore/navigation adapters, and the actual API-installed caller/provider
browser journey. #44 stays open until that journey and its applicable
candidate and exact-merge gates pass. Mechanism fixtures do not prove this
remaining integration or descriptor confidentiality in unreviewed adapters.

Non-goals are tray/chrome-essential policies, new panel SDK methods, default
permission grants, resolver ranking, notification navigation, primary cache
changes, general reconnect/sequence policy, arbitrary component persistence,
kernel preference tables, release/publication and implementation in #43.

## 2. Identity, authority and finite bounds

A **float record** is one shell-owned reservation and its chrome. It owns at
most one component incarnation, load attempt, error surface and cleanup chain.
Its identity contains a fresh opaque record token, authenticated frame/authority
epoch, application ID, package generation, panel ID, resolved capability and
bounded context descriptor. Module URLs, labels and array positions are not
identity. A component incarnation has an additional fresh token on Retry.

The future admission adapter supplies an authorized resolved target, its
current generation, immutable same-origin module URL and a stable `context_key`.
This is a semantic requirement on that separately reviewed seam, not a newly
specified endpoint or public method. Context keys must be stable for a resolved
context but are not assumed collision-free: compare the complete canonical
descriptor independently, so a key/hash collision cannot merge different
contexts. #45 owns the mapping from public content-type/context arguments to
that resolved descriptor. A record uses:

```text
dedup identity = (authority epoch, application ID, generation,
                  panel ID, capability, context_key, canonical descriptor)
```

The panel receives a fresh descriptor snapshot. Later extension mutation of
that snapshot cannot change the manager's identity or persisted descriptor.
Only an explicit new intent can request a different context.

All limits below are required future behavior. Bytes mean the length of the
UTF-8 encoding; CSS pixels mean effective shell-layout coordinates after the
current browser zoom and shell scale have been applied.

| Resource | Bound and outcome |
| --- | --- |
| Live float records | Hard maximum **5**, shared by all current/retiring float owners in one browser document. A current authenticated frame can use only the remaining slots; replacing its authority epoch does not reset the budget. Loading, shown, minimized, retained failure, retrying and retiring records all count. No configurable higher ceiling is defined. Primary panels do not consume these five slots. |
| Work per record | One physically pending load or Retry and one cleanup chain. No overlapping replacement attempts, queue of components, automatic retry or background retry timer. |
| Load readiness | 15,000 ms from admission to import/factory/render readiness and, if shown, first activation. Timeout immediately invalidates the incarnation/load token and produces retained failure; it does not claim to abort module import or preempt synchronous JavaScript. |
| Context descriptor | Plain record of at most 16 own string keys and string values, depth 1; no arrays or nested objects. Key length 1..64 UTF-8 bytes, value length 0..512 UTF-8 bytes, complete canonical JSON at most 4,096 UTF-8 bytes. Invalid input is refused before reserving a slot. |
| Context key | Non-empty string, at most 512 UTF-8 bytes; supplied by the reviewed adapter and never used as an authorization credential. |
| Capability | Existing registry identity and validation; additionally at most 256 UTF-8 bytes in a float descriptor. No wildcard or inferred namespace permission. |
| Application/panel IDs | Existing launcher/package identifier grammar; at most 64 ASCII bytes each. No replacement title-derived identifiers. |
| Z-order | Dense ranks 0..4 for the live records, reindexed after focus/removal. No ever-growing CSS z-index or append-only focus history. |
| Confirmation | One shell-owned dirty-close confirmation at a time; further close/jump requests are ignored, not queued, until it resolves. Forced retirement takes precedence. |
| Persistent snapshot | One `shell.floats` value, at most 5 entries, each at most 6,144 UTF-8 bytes, aggregate at most 32,768 UTF-8 bytes. Section 7 defines the exact schema. |
| Restore/write controllers | One automatic read/restore pass per authenticated frame; at most 5 descriptors examined per pass. Explicit Retry performs one new read/pass at a time. One write in flight plus one latest replacement snapshot; intermediate snapshots are coalesced. No automatic retry after an I/O failure. |

Canonical context JSON sorts keys by bytewise ascending order and uses compact
JSON string encoding without additional whitespace. Accessor-backed objects,
non-string values, lone surrogates, NUL and `__proto__`, `prototype` or
`constructor` keys are invalid. Validate the decoded own-key/value record;
JSONB/already decoded values do not preserve the history of repeated raw keys.
If a float-owned boundary directly observes repeated keys in raw JSON, reject
them there; this does not introduce or claim a generic request-parser change.
Validation does not call extension-defined getters, `toJSON` or coercion hooks. This
bounded data shape is not permission to persist every valid descriptor:
section 7.2 requires a separately approved minimal persistence projection.

The reservation controller lives for the browser document, above authenticated
frames. Epoch replacement immediately removes old-user UI and sensitive
descriptor/component references, but retains at most five opaque outstanding
preparation/retirement tokens and their reservations until settlement/cleanup.
Pending preparation continuations capture only those tokens, never a retired
frame's context, panel API or credentials.
The new frame cannot import another five behind that controller's back. It
sees only generic cleanup-pending status, never old-user labels, context or
ownership metadata. A full-page reload destroys this document/realm before
creating a fresh controller; swapping shell frames alone does not.

Browser-local limits are not a cross-tab or server work quota, a native module
cache bound, or a sandbox/memory guarantee for arbitrary extension JavaScript.
The shell owns its recorded preparations/components and enforces their cleanup;
the record cap does not certify that unrelated extension-created work or
non-terminating module top-level code can be preempted.
Capability calls retain their independent kernel authorization and cancellation
contracts. An authorized target at admission is not an authority lease forever.
Terminal session/RBAC loss or a newer authoritative removal retires the owner.
Temporary discovery failure denies new admissions but does not invent successful
empty discovery or bypass the fail-closed terminal-authority path.

## 3. Admission, deduplication and results

The manager performs a serialized admission decision before asynchronous import:

1. Verify the calling frame is current and the future adapter is available.
2. Validate the complete bounded target/context and obtain fresh authorized
   target and generation evidence from that adapter.
3. Retire any record that a newer authoritative topology has invalidated.
4. Check the dedup identity, including loading and minimized records.
5. For a new identity, reserve one of the five slots atomically before starting
   its load. If all five remain owned, refuse admission and offer the user the
   existing float list to explicitly close one. Do not minimize, close, evict
   or overwrite another record automatically.

Two concurrent identical requests share one record and readiness attempt.
An existing shown record is raised/focused; a minimized record is restored and
focused; a loading record's existing progress surface is raised; a failed record
is raised with its Retry action, not automatically retried. A retiring duplicate
is cancelled/unavailable until cleanup releases its reservation, never a second
owner. Dedup does not repeat factory construction or lifecycle activation.

The semantic outcomes are admitted, deduplicated, limit-refused,
target/adapter-unavailable, invalid-input, cancelled/stale and load-failed.
These names describe what the UI and future adapter must distinguish; they do
not add a public enum, promise return shape or SDK method in #43. The
[SDK's eventual signature](ICD-0.6.3-panel-sdk-client-sdk.md#38-openfloatcontenttype-context--stub),
`openFloat(contentType: string, context?: unknown): Promise<{ floatId: string }>`,
is preserved; #45/#46 own its adapter mapping and errors. The bounded internal
descriptor is not a redefinition of that method's raw `context` argument. A
denied, malformed or unavailable request owns no component and consumes no new slot.
Failure after reservation keeps exactly that record's slot and closeable
fallback. Cancellation by an obsolete request cannot cancel a newer shared
owner; every completion must still match frame, generation, record and
incarnation tokens before it can commit.

The shell shows generic unavailable text without revealing denied target
metadata. Limit refusal clearly says five panels are still open, including
minimized ones. Closing one successfully releases capacity; merely pressing
Close or cancelling its dirty confirmation does not.

## 4. State machine and lifecycle ownership

Presentation and component readiness are separate dimensions. Readiness is
`loading`, `ready` or `failed`; presentation is `shown` or `minimized`, with a
desktop `maximized` flag. Every combination owns one slot until retirement
finishes. The modal/desktop container type is responsive presentation, not a
new component state.

| Input | Required transition and side effect |
| --- | --- |
| New admission | Reserve -> `loading/shown`; create closeable shell chrome, then import and construct at most one component. |
| Readiness success | `loading -> ready`; render into the owned container and fire activation once when first shown. If already minimized, retain it hidden without activation or focus until its first restore. |
| Minimize/restore | Toggle presentation only. Preserve the same component, effects, callbacks, context, dirty state and slot; no deactivation or repeated activation. First showing of a never-activated ready incarnation performs its one initial activation. Restore uses the existing owner. |
| Desktop maximize/restore | Toggle the maximized flag without remount or lifecycle event; preserve the last normal geometry. |
| Import/factory/render/activation failure or timeout | Immediately invalidate the failed incarnation token, `loading/ready -> failed`, clean up that incarnation and retain bounded shell fallback in the same record. Other records and the primary manager remain usable. |
| Explicit Retry | Once prior preparation and cleanup have settled, `failed -> loading`, same reservation, one fresh incarnation token, fresh target authorization/generation and descriptor validation. No old component or import attempt runs concurrently. |
| Explicit close | Dirty Cancel keeps the same owner; clean close or confirmed Discard -> `retiring -> destroyed`. |
| Forced authority/topology retirement | Any owned state -> `retiring -> destroyed` without a cancellable dirty veto. |

Minimize suppresses the content DOM from pointer input, shortcut dispatch and
the accessibility tree using hidden/inert semantics. Its effects and background
capability work remain alive; minimizing is neither suspension nor a claim that
work stopped. A loading minimized record may finish preparation under the same
owner, but cannot focus or become shown automatically. Primary navigation does
not deactivate, evict or destroy unrelated floats.

The first activation attempt marks that incarnation as activated before
callbacks run, so partial activation failure still has one cleanup path.
Focus changes, minimizing, responsive changes and successful dedup never
activate a second time. Restoring a never-activated ready incarnation is its
first showing, not reactivation. Retirement or failure cleanup fires
deactivation at most once for an activated incarnation, renders `null` to perform Preact
unmount/effect cleanup, unbinds callbacks/shortcuts and removes component
references. There is no new `onDestroy` SDK method. An incarnation that never
activated is unmounted/unbound without inventing a deactivation event.

Explicit close uses one modal “Discard changes and close?” prompt when dirty.
Cancel preserves component identity, state and focus. Discard retires this
record; a second Close cannot run cleanup twice. Dirty status on every live
float, including minimized ones, contributes to the shell's single page-level
`beforeunload` guard alongside the current primary guard. Remove the guard only
when no current owner remains dirty. This is a future extension to primary
behavior, not a claim that today's guard already tracks floats.

The confirmation alone owns modal focus and `aria-modal`; suspend any
underlying float dialog's trap/modal declaration and all panel shortcut
dispatch while it is open. On Cancel, restore the connected eligible control
that had focus before the prompt, otherwise use section 5's fallback. Forced
retirement dismisses an affected prompt without Discard authorization and
recomputes focus only for the still-current frame. There are never two active
modal traps or an old-session prompt capable of restoring retired focus.

Retirement first fences admission, settlement, focus, shortcuts and persistence
for that record, then requests cancellation of owned cancellable browser work,
performs deactivation/unmount/unbinding, removes chrome/DOM/references and
finally releases its slot. A stale import that completes afterwards is never
mounted or activated. If an already constructed candidate is still referenced,
it is unmounted/unbound under the old token, not transferred to another record.
Callbacks throwing cannot veto remaining shell cleanup; diagnostics are
production-redacted and bounded by the existing audit contract.

Timeout/failure fencing happens immediately, not only on Close or Retry.
If a non-abortable import remains physically pending, keep its sole attempt
and reservation; Retry is disabled with a waiting explanation until settlement.
A close request fences the record immediately but retains a truthful retiring
status and slot until that outstanding preparation has settled and cleanup
finishes. Its late settlement cannot construct, render or activate a component.
Do not abandon one import and launch unlimited replacement imports behind a
single nominal reservation. A permanently stalled import may require shell
reload; it is not falsely reported as cleaned up or free capacity.

Synchronous hostile or non-terminating extension JavaScript cannot be forcibly
joined by a browser timer. The 15-second readiness deadline and fencing apply
when the event loop runs; they are not a wall-clock preemption guarantee.
While readiness is pending, check elapsed monotonic time as well as tokens
before import settlement can construct/mount, and before a synchronous
factory/render/activation return can commit success. A delayed timer cannot
turn a post-deadline result into success. Ready-but-minimized preparation
completes this deadline; its later first-show activation is not an attempt to
reuse an expired loading token.
Cleanup invokes each owned hook/unmount once and never waits for arbitrary
extension promises. If shell cleanup itself fails, the manager fences the
record, disables new float admission and shows a bounded shell error with a
user-requested full-page reload action, retaining the unsaved-work warning where
applicable. Never start an automatic replacement float manager/frame inside
the same document to bypass outstanding owners. It must not pretend capacity
was safely reclaimed or silently leak an owner to admit a sixth.

## 5. Chrome, focus and jump

Each record's shell chrome provides title, icon, source application badge,
loading/failure status and Minimize, Maximize/Restore, Close and Jump to app.
Use authorized application/panel labels and icon fallbacks from the launcher
vocabulary; context text and extension HTML are not interpolated into chrome.
Titles are text, ellipsized visually without removing the complete accessible
name. A minimized-floats control exposes at most five named restore buttons
and close actions; a loading/failed minimized record remains discoverable there.

Focus is distinct from activation. A click or keyboard focus entering a shown
float's chrome or content raises that record and normalizes z-order. Its
readiness determines whether it can own panel shortcut dispatch; a focused
loading/failed float does not redirect content shortcuts to the primary.
Exactly one currently focus-owning primary or shown-ready float may
receive panel shortcut dispatch; shell-reserved shortcuts keep their existing
precedence. Minimized, loading, failed, inert and retired component incarnations
receive none. Unregister and retirement remove the old shortcut owner rather
than leaving a global listener behind.

User admission, restore or raise focuses the first enabled chrome control
after readiness/progress chrome is present, without requiring extension input
focus. A background load/restore completion never steals focus. Closing or
minimizing the focused float returns focus to its connected, authorized trigger;
otherwise to the most recently focused eligible shown float, otherwise to the
current primary heading or Home control. A retired trigger cannot receive it.
The manager maintains only the five-record rank order and one weak/validated
trigger reference per record, not unbounded DOM/focus history.

All chrome actions are native buttons with accessible names, visible focus and
minimum 44-by-44 CSS-pixel target areas where the viewport permits. Tooltips are
supplemental, never the only names. Desktop nonmodal windows use labelled
regions and do not trap Tab. A labelled float list lets keyboard users reach
any shown or minimized record without relying on drag or a custom global
shortcut. Shell chrome exposes keyboard Move/Resize controls with arrow-key
steps of 10 CSS pixels, Shift+arrow steps of 1 CSS pixel, Enter to accept and
Escape to revert the geometry-only edit. They obey the same clamps as pointer
operations and do not invoke content shortcuts while active.

Jump is visible but disabled with an accessible reason until the reviewed
#45/#46 navigation adapter can validate a corresponding authorized primary
target and deliver its context. A capability name is not a primary target;
the shell never guesses one or invokes the current `navigate` stub as a no-op.
Once supported, one jump intent per record may be in flight. Missing, denied,
cancelled, stale, import-failed or navigation-failed outcomes retain the source
float and its state. Only a confirmed successful navigation/context delivery
may initiate its ordinary close flow. A dirty-close Cancel then retains the
float even though primary navigation succeeded; communicate that outcome
honestly. No source destruction happens merely on dispatch or before the
navigation result. Retirement wins over a late navigation completion.

## 6. Responsive presentation and geometry

Let `W` and `H` be the available visual-viewport work area in effective shell
CSS pixels, excluding persistent shell chrome and safe-area insets. Derive them
again on viewport resize, orientation, browser zoom, virtual-keyboard viewport
change and shell scale change; do not mix physical/device pixels with stored
layout coordinates. Use post-layout viewport-relative CSS measurements; do not
apply browser zoom or shell scale a second time. Floor the available width and
height to integers before classification/defaults/clamps. A dimension below
1 CSS pixel defers placement without constructing another owner or auto-focusing
it. Stored geometry saturates each saved coordinate/dimension to the section
7 range before serialization; unusually large work areas cannot create an
out-of-schema snapshot.

| Available width | Container and input behavior |
| --- | --- |
| `W > 1024` | Nonmodal draggable/resizable desktop window. Maximize fills the work area without replacing the component. |
| `768 <= W <= 1024` | Modal right-edge slide-over, width `min(W, 640)` and height `H`, with Back and Close controls. |
| `0 < W < 768` | Modal full work-area surface with Back and Close controls. |

Every transformation preserves record token, component tree, effects, context,
dirty state and reservation. Desktop geometry and maximized preference are
retained while modal, not overwritten with the modal rectangle. Modal Maximize
is disabled with an explanation. Minimize remains available. Back, Close and
Escape in modal presentation request the same dirty-aware close, not navigation
of the primary panel or implicit discard. A geometry editor or confirmation
consumes Escape before the surrounding modal close action.

When multiple records are shown in a modal width, only the highest-ranked one
is exposed as a labelled `dialog` with `aria-modal="true"`; the shell background
and other shown containers are inert/hidden to input and accessibility. They
are retained owners, not secretly minimized or deactivated. Tab/Shift+Tab stay
within the active dialog, including its shell controls. Its bounded panel
switcher can select another shown record or restore a minimized record. Closing
or minimizing it exposes the next highest-ranked shown record; if none remains,
the shell becomes interactive. The switcher prevents an unreachable hidden
record and never implies that hidden owners freed capacity. On returning to
desktop, all shown records become nonmodal again; focus stays on the same
eligible control or its chrome fallback, not a newly created component.

Normal desktop geometry is integer `{x, y, width, height}` in CSS pixels.
Default width/height are `min(W, 640)` and `min(H, 480)`; center the first
window using floor-rounded x/y, and offset subsequent new windows by 24 CSS pixels per current record
rank before clamping. The width lower bound is `min(W, 320)`, height lower bound
`min(H, 240)`, upper bounds `W` and `H`; clamp x to `[0, W-width]` and y to
`[0, H-height]`. Resizing/zoom never leaves chrome exclusively offscreen.
At very small heights, chrome remains reachable and content scrolls separately;
controls wrap rather than dropping Close/Back. The dialog itself may scroll
when the viewport cannot contain the chrome minimum. No fixed minimum can
force the whole surface wider/taller than the available viewport.

Pointer drag starts only on non-control title-bar space; pointer resize and
keyboard geometry actions commit the same clamped integer model. Round pointer
coordinates to nearest integers before applying the integer clamps. Pointer
capture is released on completion, cancellation and retirement. Geometry is
persisted only at accepted operation end, never at every pointer move.
Browser text zoom, shell scaling, high contrast and a 320-CSS-pixel viewport
must keep names/actions reachable. Honor `prefers-reduced-motion`: transitions
have no animation when enabled, and no state/cleanup/focus outcome depends on
an animation event firing. Modal containment must be recomputed after responsive
changes without retaining an old focus trap.

## 7. Descriptor-only persistence

### 7.1 Exact stored shape and normalization

The future well-known key is `shell.floats` in existing authenticated,
user-scoped shell preferences. #44 must add explicit per-key validation at
both persistence and consumption boundaries; generic preference storage is
not float validation. No kernel preference table or local-storage copy of
private context is introduced.

```json
{
  "version": 1,
  "floats": [
    {
      "application_id": "notes",
      "panel_id": "preview",
      "capability": "notes:preview",
      "context": {"record_id": "example-record"},
      "presentation": "shown",
      "maximized": false,
      "geometry": {"x": 24, "y": 24, "width": 640, "height": 480}
    }
  ]
}
```

Only the keys shown are allowed. `floats` order is back-to-front z-order,
including minimized entries; array position is not target identity. Geometry
contains four finite integers: x/y in `[0, 65535]`, width/height in `[1, 65535]`.
They are bounded saved coordinates, then clamped against the actual work area
on restore. `presentation` is exactly `shown` or `minimized`; `maximized` is a
boolean. Current modal dimensions are never serialized in place of the saved
desktop rectangle. No module URL, generation, runtime token or `context_key`
is stored; fresh admission reconstructs those.

An absent key or valid empty array means no restore. Stored `null`, an unknown
version, wrong envelope types/keys, more than five entries or an aggregate
above 32,768 UTF-8 bytes produces an empty restore with
a generic non-destructive preference warning. Do not repair it by executing
data, truncating strings or guessing a version. Within a valid bounded
envelope, skip malformed/oversized individual entries and retain the first
valid duplicate descriptor in stored order. A duplicate descriptor means the
same application/panel/capability and canonical context. Entry and context
limits apply independently before normalization. Normalizing an invalid read
does not automatically overwrite the stored value; a later explicit float
state change or user persistence retry may write a valid snapshot.

### 7.2 Allowed descriptor data

Persistence stores only a minimal stable reopen descriptor whose keys/meaning
the target adapter's reviewed contract explicitly allows for persistence, such
as a non-secret record ID. Valid string shape alone is insufficient. Arbitrary
component props, edited/unsaved text, result rows, tokens, cookies, credentials,
private URLs, ephemeral capability handles and sensitive transient state are
forbidden. The example record ID above is fake and confers no permission.
If a target has no approved safe projection, its float is live-only: omit it
from snapshots and expose “Not restored after reload” in its shell status.
Do not guess safe keys from their names or silently strip arbitrary runtime
context and claim a correct reopen.

Reload/re-login reopens authorized descriptors as new clean component
incarnations; it does not restore component memory or unsaved edits. Persisting
a minimized dirty descriptor is not backing up that dirty state. The UI must
state that distinction; `beforeunload` remains the unsaved-work warning.

### 7.3 Read, restore and write ownership

Read once automatically for the current authenticated frame. Run at most one automatic
restore pass after its fresh float-admission authority is ready. Process at
most five validated descriptors sequentially in saved order, through the same
admission/dedup/limit path as interactive requests. Revalidate the latest target,
declaration and generation; do not import a persisted old URL or infer
permission from stored data. A descriptor no longer available/authorized is
skipped with a generic restore warning and no denied metadata. A temporary
authority/read failure performs no restore and offers an explicit Retry, not
an automatic polling loop. Restore starts minimized entries minimized and
shown entries at their saved rank; it never steals keyboard focus from the
user. Interactive admissions share the same reservations; if they consume
capacity, remaining restore items are skipped with a bounded status, not
evicted or queued indefinitely.

The initial read and automatic restore capture a float-intent revision. Any
accepted explicit close or other user float-state change invalidates that
automatic pass: discard its remaining descriptors and fence/retire any
not-yet-ready restore-owned candidate, without disturbing already ready
owners. A late preference read or sequential import cannot reopen a record
the user closed in the same frame. This needs one revision token, not an
unbounded tombstone list. Show that automatic restore was interrupted;
an explicit restore Retry is a deliberate new pass against a fresh read and
authority check, not continuation of the superseded snapshot.

Persist successful live-descriptor changes after admission readiness,
minimize/restore/maximize, accepted geometry edits or completed close. Coalesce
changes using one 250-ms trailing timer, one in-flight write and one latest
pending snapshot, all owned by the current frame. Each snapshot has a local
revision token. A response may acknowledge only its own current revision and
cannot replace newer UI state. Failed writes preserve live UI, show “Layout
not saved” and stop automatic write retries until a new user state change or
explicit persistence Retry. Read/restore/write Retry is one user-triggered
attempt at a time, with no unbounded request queue.

Logout, terminal session loss and frame retirement cancel timers, fence
responses and discard pending snapshots/restore descriptors from memory.
They must not delete an authenticated user's stored layout merely because
the session ended. A later login, even by the same user, performs a fresh read
and fresh authorization; another user's frame cannot inherit cached descriptors.
An already admitted server write may still complete for its authenticated
owner: browser fencing is not server rollback. It cannot launch a later write
or restore/focus action in the replacement frame. Existing storage supplies
no cross-tab compare-and-swap contract; different tabs use last successful
server write, without implying their local revision tokens arbitrate each other.

## 8. Authority changes, failure and recovery

Logout/session revocation, successful disable/uninstall, RBAC removal and
generation quiesce retire every affected shown, minimized, loading and failed
record under section 4. Dirty state cannot veto retirement. Remove old DOM,
callbacks, shortcuts, pending UI work and remembered frame descriptors before
admitting a replacement. Use the existing launcher/package lifecycle's
fail-closed authority and readiness ordering; old UI cannot remain a coherent
fallback while shared runtime, registry or migrations change.

A generation replacement releases the old incarnation through real cleanup
before loading a freshly authorized generation. At most one record/reservation
owns that replacement; no overlap is justified by the fifth-slot limit.
Automatic resurrection from stale callbacks or a failed upgrade is forbidden.
User Retry or a fresh restore intent may reopen only after authoritative
reconciliation declares the target ready. Returning an old generation to
readiness requires the package lifecycle's verified recovery, not a frontend
guess from a still-cached module.

Import, factory, render, activation and readiness-timeout failures retain one
closeable panel-local fallback with Retry; primary content and other floats
stay usable. Minimized failure exposes that status in the float list. Retry
cleans any prior incarnation, uses a fresh token and target evidence, and makes
one attempt. If unavailable, retain the generic fallback with Close and no
running component. No silent restart loop, timer-driven reopen or automatic
retry on primary navigation is allowed. Auditing uses the existing bounded,
redacted boundary channel; it does not dump context or raw exceptions or
invent a new public audit family in this specification.

## 9. Acceptance and implementation handoff

The following is a required future acceptance matrix, not a list of feature
tests already executed by #43. Independent specification review must cover
every row, plus link targets, source-versus-history reconciliation and the
docs-only change envelope. #44 must supply real-browser mechanism tests and
retain the applicable repository validation/CI gates. #45/#46 must separately
prove adapter/resolver/navigation rows before claiming extension-driven floats
or functional Jump. Candidate green and terminal exact-merge checks remain
required for each delivery; this document does not weaken CI for paper work.

| Family | Independently observable required cases |
| --- | --- |
| Chrome/jump (#43 first criterion) | Text labels/source badge, minimize, same-instance restore, maximize/normal geometry, clean close, dirty Cancel/Discard, disabled unsupported Jump, unavailable/denied/failed/stale Jump retains source; confirmed delivery precedes ordinary close, dirty Cancel preserves the navigated source. |
| Admission/limits | Duplicate same target/context while shown, minimized, loading and failed; different descriptors with colliding context keys; concurrent duplicate and five-to-six boundary; all five minimized, loading or retained-failure owners; no eviction; dirty-close Cancel consumes slot, successful cleanup then admits one; retiring owner never grants early capacity. |
| Lifecycle/ownership | One component/factory/activation per incarnation; minimize/restore/focus/modal changes do not deactivate/reactivate; first restore of never-shown readiness activates once; dirty minimized owner survives primary switch; close twice; cleanup callback failure; timeout/failure immediately fences late import/settlement; non-abortable pending attempt disables overlapping Retry and retains closing reservation without claiming browser preemption. |
| Responsive (#43 second criterion) | Both sides of 768/1024 widths, modal switcher and inert stacking, desktop restore of same tree and geometry, virtual keyboard, resize/orientation/zoom/scale, clamped tiny work area, zero-area deferral and pointer-capture retirement. |
| Accessibility/focus (#43 third criterion) | Keyboard-only chrome/list/move/resize, modal Tab containment, confirmation suspends underlying dialog/trap/shortcuts, desktop nonmodal Tab, Escape precedence and dirty Cancel, missing/retired trigger fallback, minimized/loading/failed shortcut exclusion, focus does not activate, no restore-completion focus steal, 320-pixel/text zoom, high contrast and reduced motion. |
| Persistence (#43 second criterion) | Empty/valid shown/minimized snapshots; exact entry/context/aggregate boundaries; duplicate order normalization; malformed/unknown-version/oversized/defaults; invalid UTF-8/key/geometry; unsafe or unapproved context excluded; no component/dirty-memory restore; fresh authorized generation; explicit close during pending read/restore cannot reopen that record; interrupted-pass Retry is deliberate; read/write failure and explicit Retry; coalescing, out-of-order acknowledgement, another user/frame and independent tabs. |
| Recovery (#43 third criterion) | Import/factory/render/activation failure, timeout and bounded Retry; minimized failure; unaffected primary/other floats; no auto retry; shell cleanup failure fails admission closed; close recovery without phantom capacity; manual real-document reload with unsaved-work warning, never an automatic manager reset. |
| Authority/replacement | Logout/revoke, unavailable authority versus successful empty discovery, disable/uninstall/RBAC removal, package quiesce/new generation/removed declaration, failed upgrade/reconciliation, stale import/navigation/preference completion, all owned states retired without dirty veto, repeated logout/login with stalled imports cannot reset document reservations or expose old descriptors, manual reload destroys old realm, no old-module resurrection. |
| Delegated seams | No float authorization inferred from primary-only discovery; unsupported mechanism fixture is not a shipped SDK claim; preserved `openFloat(contentType, context)` signature; separately reviewed adapter context-key collision cases and persistence projection; resolver/default/tray policy not silently absorbed. |

#44's historical “oldest-minimized” acceptance wording conflicts with the
approved five-live cap, as recorded in the
[maintainer decision](https://github.com/gobha-me/plinth/issues/43#issuecomment-5935390640).
Minimization is useful chrome behavior, never capacity
creation. If the mechanism-versus-later-adapter sequencing cannot satisfy an
implementable #44 outcome, return that bounded dependency question for review
before code changes rather than narrowing its outcome or inventing an SDK seam.
