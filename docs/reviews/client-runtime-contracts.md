# Client runtime contract coverage (#32)

Development 0.6.6 adds executable coverage of the shipped shell, SDK and
Launcher. This is a case-to-assertion ledger, not a release or CI receipt.
Candidate and exact-merge checks remain delivery gates. Historical ICD tables
are retained; the corrections below name current contracts instead of silently
restoring superseded APIs or counting unrelated server tests as browser proof.

## Gates and boundaries

`npm run test:transport --prefix tests/browser` runs 35 actual SDK-module cases
and 30 pure query/controller cases. `test:launcher` runs 2 model and 51 whole
Launcher-module cases. Each file is a separate real Node invocation; the
discovery gate requires exact named executed/pass counts, zero failures,
cancellations, skips and TODOs, and exit zero. Its parser has 24 regressions.
External platform/Preact imports in VM tests are seams; production module
methods are not extracted, copied, or reimplemented.

`test:launcher-browser` runs the existing Launcher journey plus four managed
owner, six smart-hook, five panel-contract and ten preference/session cases.
These use actual shipped modules and vendored Preact with controlled local
auth/capability/WebSocket fixtures. Owner/preference/Launcher fixtures enforce
the production CSP; the standalone smart/panel harnesses are module fixtures,
not additional production-CSP proofs. Their source hashes,
named case counts and independently bounded cleanup are asserted. They are not
proof of native authorization, persistence, or a deployed exploit. Linux CI
runs this command through `process_cleanup.py` to own detached Chromium even
on interruption. Direct npm invocation does not replace that supervisor.

`run-production.py --binary build/plinth --realtime` uses one owned kernel and
database for startup, raw transport, preference realtime, package lifecycle,
and the API-installed SDK demo. `run-client-contracts.py --binary build/plinth`
uses real registration/login and the same root/config/database across a
bounded SIGTERM kernel restart, retaining one browser profile. Both also
accept an exact local `--image` and verify Plinth is PID 1. Runtime candidate
and exact-digest release verification invoke both; this change does not grant
release authority. Test-only SDK/demo/conflicting packages are never bundled
into the production image. All identities and credentials are disposable.
The test fixture manifest is `sdkdemo` 0.1.0; its directory/archive retain
`sdk-demo`. The historical hyphenated manifest identity is explicitly retired
without an alias. This test-only rename uses the intersection of the current
manifest and database naming rules; it does not repair their separate backend
compatibility/cleanup mismatch.

## ICD-0.6.1 integration (3 cases)

| Case | Disposition and exact assertion |
|---|---|
| I.01 | Covered by joined `client-contracts-production.mjs`: fresh database/bundled shell, actual registration and UI login, SDK set, bounded kernel replacement against the **same database**, browser reload, SDK get returns exact original JSON. Consumer effective rules include virtual `everyone` membership and exclude orphaned rules; a real admin-only route returns 403. |
| I.02 | Covered by the same journey: `Promise.all` of two actual SDK/HTTP writes, both responses 200/success, exactly one PostgreSQL row with exactly either submitted JSON value. This is not the older sequential dispatch test. |
| I.03 | Covered by real multipart install of a valid frontend fixture changed only to `/app`; rejection reaches `ACTIVATING`, and bundled shell row/body/mount remain unchanged. Historical `ERR_MOUNT_CONFLICT` is a scenario label, not the current API literal: actual response is 500, `state: INSTALL_FAILED`, `failed_at_stage: ACTIVATING`, `kind: activation-failed`, from the active-mount uniqueness constraint. No error-envelope change is made. |

## ICD-0.6.2 integration/UI/regression (9 cases)

| Case | Disposition and exact assertion |
|---|---|
| I.01 | Joined real UI successfully writes `shell.theme=light`; restart/reload poisons the local mirror first, then actual `get_all` hydration and SDK get restore the persisted light value. |
| I.02 | Same journey for integer `shell.scale_pct=125`; mirror, rendered root font size and persisted rows survive exactly. |
| I.03 | Absorbs existing redirect/asset tests and adds joined authenticated/guest fetches before/after preference writes and restart: 302/no-cache, versioned immutable target, both literal palettes, byte-identical user-independent CSS. |
| U.01 | `shell-preferences-browser.mjs` P01: all three literal theme options and current selection, authoritative hydration over a stale local mirror. |
| U.02 | P01/N01: exact capability args, success changes theme and mirror; held/rejected writes do neither. P02 verifies disabled storage and explicit-theme OS changes. |
| U.03 | P01: exact seven stops 80/90/100/110/125/150/175 and current selection. |
| U.04 | P01/N01: each exact integer arg and rendered root font size, only after server success. |
| U.05 | Manual-smoke scenario absorbed into P01 mechanical browser assertions: actual body palette colors change, and 175% popover geometry stays anchored. This is not an aesthetic/human visual review. |
| R.01 | P01 measures actual avatar/popover at 80/100/175%, enforcing §17's authoritative **0.30rem right-edge offset**, stable rem vertical offset and `zoom == 1`. The older left-edge equality text is superseded, not a missing test. |

Additional named cases cover late per-key hydration after successful or failed
selection, failure banners/last-good state, retired-account queued writes and
hydration, malformed/disabled local storage, keyed login retry after a session
probe 401, and initial guest retirement followed by genuine login admission.
Local storage is a cosmetic pre-paint mirror, never authentication or the
authoritative preference store. Already-admitted HTTP requests are not claimed
to be undone on logout.

## ICD-0.6.3 client families (33 cases)

| Case | Disposition and exact assertion |
|---|---|
| L.01 | `panel-contract-browser.mjs` P32.01 exact initial activation once; installed SDK demo independently asserts visible ordinary-Launcher panel/activation 1. |
| L.02 | P32.01 actual A→B→A manager transition increments A activation to 2. Dependency #31 is implemented; this is no longer deferred. |
| L.03 | P32.01 exact ordered deactivate-A before activate-B and deactivate-B before reactivate-A. |
| L.04 | P32.02 public dirty true→false removes before-unload guard and permits Home without dirty dialog. Existing Launcher dirty-cancel/discard cases remain. |
| L.05 | P32.02 literal `TypeError` and message for nonboolean `setDirty`. |
| L.06 | P32.01/P32.04 supplied context identity survives retained reactivation and actual compatibility `loadPanel`; default context is `{}`. |
| C.01 | Absorbs native preference SDK success/readback and installed demo's real snapshot value. |
| C.02 | Installed demo consumer's actual `kernel.config.get` is HTTP403, decoded as `CapabilityError/rbac_denied`; not a mocked decoder-only result. |
| C.03 | Same actual SDK route for missing shell capability is HTTP404, typed `not_found`. |
| C.04 | Installed journey's narrow restored platform-fetch rejection produces actual SDK `NetworkError` with TypeError cause, exactly one attempt, then ordinary success. Transport VM additionally covers fetch and JSON boundaries. A deterministic network seam is explicit. |
| C.05 | Actual installed test-only `sdkdemo.thrower` handler deliberately throws a side-effect-free TypeError: HTTP500, typed `cap.handler_threw`, controlled marker and nonempty message exactly matching the native response. Its exact rule is granted only to the fake consumer and removed by normal uninstall. Shell preferences reject invalid args in native validation before entering JS, so null is not a throw witness. The ICD's older `quickjs_throw` literal is retired; no production capability or wire change. |
| C.06 | One restored platform-fetch seam removes `args`; real kernel returns HTTP400, typed `bad_request`. Subsequent ordinary SDK call still sends `args:null` and succeeds. |
| S.01 | Absorbs `preferences-realtime.mjs` and adds installed fixture: SDK write→durable native counts event→actual raw SDK delivery in the same panel. |
| S.02 | Absorbs real transport unsubscribe/removal in `realtime-smoke.mjs`; VM tests also ensure removed callbacks and pending ready tasks remain inert. |
| S.03 | `sdk-transport.test.mjs` exact two-consumer delivery with one physical channel request; installed fixture has independent raw and snapshot consumers on one real channel grant. |
| S.04 | Named transport case: first handler throws, dispatch does not throw, second and third receive exactly one identical outer envelope; removal keeps surviving consumers. |
| S.05 | Historical Promise rejection/`CapabilityError` API is retired. Shipped `subscribe` returns a removal function and reports a missing channel in the actual grant ACK as `RealtimeError/subscription_denied` through `onError`; actual linked SDK tests assert denial, absent readiness and safe removal. No incompatible Promise API is restored. |
| U.01 | `smart-data-browser.mjs` H01 exact initial data/loading/error before real Preact effects, snapshot resolution and no later initial-data flash. |
| U.02 | H01 exact typed snapshot failure/last-good stale state/recovery, plus pure controller's separate query/live/adapter errors. |
| U.03 | Historical envelope replacement retained for **raw no-snapshot mode** (H04 and native raw transport). Snapshot mode is intentionally smart invalidation/requery: native preference create/update/delete yields one bounded real capability request per isolated counts event and changes UI to the **server snapshot**, not the envelope. |
| U.04 | Absorbs native unsubscribe and adds H02/H04/H05: unmount and render-before-effect owner changes drop late ignored-abort responses, old ready/ACK/event/removal, and post-retirement admissions; new session current-grant positive controls remain. |
| K.01 | P32.03 actual DOM keyboard event dispatches to active panel only; caller owns default-prevention choice. |
| K.02 | P32.03 literal `ShortcutConflictError` for duplicate same-panel registration. |
| K.03 | P32.03 modifier normalization/conflict and stale repeated unregister cannot remove a replacement even reusing the callback. |
| R.01 | P32.04 actual `navigate` synchronous `NotImplementedError` with exact message. |
| R.02 | P32.04 actual `openFloat` rejected Promise with exact error/message. |
| R.03 | P32.04 `onNavigationIntent` registers and remains dormant through ordinary runtime actions. |
| R.04 | P32.04 exact `requestFocus` stub error/message. |
| R.05 | P32.04 exact `setTrayState` stub error/message. |
| R.06 | P32.04 exact `setTrayBadge` stub error/message. No navigation/float/tray implementation is inferred. |
| I.01 | `sdk-installed-production.mjs`: real multipart installs test-only SDK ZIP, actual catalog discovers it, ordinary Launcher loads byte-identical versioned module, activation 1 and visible panel. |
| I.02 | Same installed panel's actual `shell.preferences.get` request/body/value and live Preact output; no mocked snapshot. |
| I.03 | Same installed panel's SDK write, durable/live matching sequence and raw counter, counts-only smart requery changes theme UI. Test-only nonadmin table-channel grant is explicit, not an ordinary-user default or per-user privacy claim. |

The previously automated B.*/A.* and server preference/token dispatch families
are absorbed, not refiled or counted as new browser work. Existing real package
disable/enable/upgrade/uninstall and retained-profile cache tests remain gates.

## Smart-query additions and remaining owner

The 30 controller cases prove fixed-window debounce plus per-grant bounded
jitter, one request/timer/dirty follow-up, stale success/failure isolation,
immutable JSON capture, exact strict-ID eq/in filtering, complete delete and
full-row insert adapter barriers, and conservative requery for native counts,
updates, mixed/truncated/invalid/duplicate metadata. The six real-Preact cases
add args/scope/view first-render races and capture across automatic re-renders,
typed preparation failures with zero dispatch, raw mode, unmount and managed
session/grant replacement. Custom ID/full-row fixtures certify only the
conditional client contract; native producers do not supply those IDs/rows.

Native browser `since_seq` replay of the three persisted preference events
remains covered with exact sequence numbers and ops. Automatic SDK source
sequence/cursor tracking, replay ordering/deduplication, reconnect resume,
retention overflow/full-resync behavior and resync-driven query policy are
**still unavailable in the shipped browser** and explicitly deferred to
[#42](https://github.com/gobha-me/plinth/issues/42). They are not hidden inside
this debounce/query owner. Published-release retained-digest evidence belongs
to #38; a development version bump is not publication and cannot close it.
