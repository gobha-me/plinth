# Shell browser smoke

Install with `npm ci --prefix tests/browser` and
`cd tests/browser && npx playwright install --with-deps chromium`.
Run `npm test --prefix tests/browser` to package the actual shell ZIP, extract
it unchanged, and serve its files under the production CSP. Set
`PLINTH_SHELL_ZIP=/absolute/path/to/shell.zip` to test an existing build artifact.

For the production gate, the launcher creates a unique disposable database,
starts the real kernel from its staged shell bundle, runs the smoke, and checks
bounded clean shutdown before dropping only its database:

```sh
python3 tests/browser/run-production.py --binary build/plinth
python3 tests/browser/run-production.py --binary build/plinth --realtime
python3 tests/browser/run-production.py --binary build/plinth --upgrade-cache
python3 tests/browser/run-client-contracts.py --binary build/plinth
```

Set `PLINTH_PG_HOST`, `PLINTH_PG_PORT`, `PLINTH_PG_USER`,
`PLINTH_PG_PASSWORD`, and `PLINTH_PG_DATABASE` for the PostgreSQL service.
The test role needs permission to create a database. To use an already
running task-owned kernel with a fresh shell installation, run:

```sh
PLINTH_BASE_URL=http://127.0.0.1:8080 npm test --prefix tests/browser
```

This mode obtains the document, assets, redirect, CSP and initial session
response from the running kernel. It does not rewrite production assets.
The demonstration panel file, mock capability responses, WebSocket transport, and boundary
audit sink are controlled by the browser harness. Authentication, realtime
protocol integration, and audit persistence require their separate server
suites; this gate proves startup and module/hook execution.

Every entry-point check uses a fresh browser context. Failures include script,
stylesheet, or font HTTP/network errors, uncaught exceptions, CSP violations, a
missing sign-in screen, broken documented/versioned SDK imports, and failed
rendering of the maintained SDK demo with both Preact and SDK hooks. The
boundary test verifies stack omission in the shipped configuration and stack
inclusion with an explicit development configuration.

The upgrade case packages two versions of the real shell, adding observable
version markers to their document, JavaScript files and token stylesheet.
An owned loopback HTTP fixture forwards the actual kernel's responses. During
warm-up the installed document omits the versioned-base marker, and the fixture
reproduces the historical immutable `/app/*` asset headers; document bytes,
MIME, CSP, and auth still come from the kernel. It visits this legacy package
in a persistent Chromium profile, stops the kernel,
simulates the completed frontend replacement by changing the task-owned
installed package snapshot, and restarts on the same origin. Ordinary navigation
must execute only the second version throughout the startup module graph and
styles, using `/ext/shell/{version}/*` URLs from the server-rendered asset base.
The fixture passes candidate responses unchanged after the restart. No browser
routing, cache override, hard reload or profile clearing is used. Mutable
`/app/*` aliases retain `no-cache`; versioned assets remain immutable.
`--upgrade-cache --legacy-cache-negative-control` removes the replacement
document's opt-in too and must fail by detecting executed stale module markers.
This checks caching after replacement;
operator installation/upgrade workflows are tested separately.

`PLINTH_BROWSER` selects an existing Chromium-compatible executable. When
required by a container that cannot run the Chromium sandbox,
`PLINTH_BROWSER_NO_SANDBOX=1` explicitly disables it.


The production launcher requires Linux with `prctl(PR_SET_CHILD_SUBREAPER)`
and procfs child enumeration (the Ubuntu CI environment provides both). A
separate supervisor owns only the browser command and adopts orphaned Node
or Chromium descendants, including Playwright's detached browser sessions.
On success, failure, or timeout it terminates and reaps that tree before the
launcher proceeds. Browser temporary files use the launcher's disposable
`TMPDIR`; forced termination cannot leave those profiles outside its cleanup.
Direct `npm test` remains usable on Playwright-supported platforms; the owned
production launcher fails before starting a browser on unsupported platforms.

Run `python3 tests/browser/process_cleanup_test.py` after installing the browser
to verify same-group, detached-child, abandoned-child, and actual Playwright
timeout cleanup. The real-browser probe disables Playwright's signal handlers
so its detached Chromium process must be cleaned by the external supervisor.

## Realtime coverage

The `--realtime` production gate runs shell startup and then realtime coverage
against the same owned database and kernel, with accelerated heartbeat timers.
It runs separately from `--upgrade-cache`, whose legacy HTTP fixture serves
the retained-profile cache migration case. Both browser commands run under
the descendant-owning supervisor described above.

`npm run test:transport --prefix tests/browser` evaluates the actual SDK
module with linked test imports and deterministic sockets/timers. It covers
fresh session-bound CSRF cookie propagation for unsafe same-origin capability
requests, omission before login and for cross-origin requests, token rotation,
both pre-dispatch and capability error envelopes, and
authentication gating, granted/denied acknowledgements, removal during auth or
an outstanding subscribe, duplicate error/close signals, timer cancellation,
terminal auth failure/displacement, explicit retry, and per-connection-epoch
subscription readiness.

The transport command also runs the pure smart query/controller tests: captured
JSON, bounded fixed-window debounce/jitter, dirty follow-up, admission and
stale responses, and conservative/conditional eq/in view optimization. Each
actual Node file must execute its fixed named count with no fail/cancel/skip/
TODO. The fail-closed discovery parser has its own 27 regressions. Current
counts are SDK35 + controller30; Launcher model2 + owner59 (the original51
remain), plus float model19, document/frame owner46 and preference controller30.

`npm run test:launcher --prefix tests/browser` covers pure discovery/preference
normalization and the whole linked Launcher graph's original-owner boundaries.
`npm run test:launcher-browser --prefix
tests/browser` serves the packaged shell and uses deterministic HTTP/WebSocket
fixtures to exercise Home, application and panel navigation, per-epoch
subscription readiness, retained panels and LRU eviction, dirty confirmation,
panel-local failure, stale refresh, responsive layout, and terminal fail-closed
DOM cleanup. These focused fixtures supplement the real-kernel production gate;
they do not replace its SessionFilter, RBAC, package lifecycle, or durable
realtime coverage. It additionally runs four managed-owner cases, six actual
Preact smart-hook cases, five panel lifecycle/shortcut/stub cases and ten shell
preference/session cases. They check exact discovery, source identity and
independent bounded cleanup. On Linux use the descendant-owning supervisor,
as CI does, to cover hard interruption during a browser launch:

```sh
python3 tests/browser/process_cleanup.py -- npm run test:launcher-browser --prefix tests/browser
```

The same command runs 18 actual float-mechanism browser cases. They evaluate
the shipping manager, responsive chrome, interaction/reservation/model modules
and vendored Preact, with explicitly local resolved-target and panel fixtures.
They prove retained input/effect identity, 768/1024 CSS-pixel transitions,
maximize/minimize, pointer capture and keyboard geometry commit/cancel, modal
Tab/inertness, one dirty confirmation/unload warning, five live owners including
pending/failure/minimized/retiring owners, no late old-frame factories, and
synchronous pointer/DOM/provider cleanup before reentrant capacity reuse.
An integrated Launcher case also verifies that cancelling a dirty primary
confirmation after resizing restores eligible float focus and shortcut
ownership, without focusing the inert primary or stealing an eligible Cancel
trigger. Injected cleanup failures retain a document-wide fail-closed/manual-reload
warning even when no record remains. Browser/context/server/probe/DOM/capture
cleanup and unchanged source hashes are checked on the actual execution.

Production instances deliberately have no reviewed admission or persistence
ports, so the existing public `openFloat`, `navigate` and `requestFocus` stubs
remain unsupported. Float preference tests exercise safe projections and
guarded acknowledgements only through local ports, never the ordinary generic
preferences API. These checks do not prove installed-extension authorization,
resolver tiers, persisted safe descriptors or Jump to app; #46 owns those
adapters and the real installed caller/provider journey. #44 remains open
until that integration is verified. No oldest-minimized eviction is permitted:
all five live owners count, and successful explicit Close/cleanup is required.

Snapshot-backed `useData` captures one capability query and re-queries after
events; raw no-snapshot mode still forwards envelopes. Supply a new args/view
object to change an existing query; internal renders keep the captured object
immutable. Optional complete eq/in keyed views optimize only certified custom
ID/full-row events. Native counts-only events conservatively re-query. Neither
mode adds automatic cursor tracking/replay/resync policy; that remains #42.

`npm run test:realtime --prefix tests/browser` requires `PLINTH_BASE_URL` plus
`PLINTH_PG_HOST`, `PLINTH_PG_PORT`, `PLINTH_PG_USER`, `PLINTH_PG_PASSWORD`, and
`PLINTH_PG_DATABASE` pointing to the same task-owned disposable kernel/database.
Set that kernel's `ws_heartbeat_interval_s` to `0.2` and
`ws_heartbeat_timeout_s` to `1.0` for the bounded three-heartbeat check.
The test seeds a synthetic non-admin session/grants, sets its cookie HttpOnly,
and publishes through PostgreSQL NOTIFY. The real listener, event writer,
RBAC gates and WebSocket protocol deliver the update to the browser. It checks
raw no-snapshot `useData` receiving actual published envelopes with no query,
multiple heartbeat replies, disconnect/reconnect without duplicate channel
requests, unsubscribe while another channel stays connected, and terminal
expired-session authentication. It never mocks the WebSocket transport.

The same command checks shell preference create, conflict-update, and delete
through the production capability. Each write must produce its own durable
`plinth:data:ext_shell.user_preferences` event, reach a live browser SDK
subscriber without a refresh, and replay to a second browser session. The
synthetic user gets a test-only subscribe grant; normal users do not receive
this table-wide channel by default. An actual Preact snapshot hook queries the
real preference capability: each isolated native counts-only event triggers
one bounded requery and changes the UI to the server's create/update/delete
snapshot. A raw browser socket separately sends `since_seq` and checks the
three exact persisted sequence numbers; that is not automatic SDK replay.

The same `--realtime` run also invokes `test:launcher-production`. That test
uses a separate synthetic admin session to install the built `valid-install`
package through the real multipart API, load its versioned panel module, then
disable, enable, upgrade, and uninstall it. The browser must follow each
durable application-catalog invalidation, remove retained panel DOM when
authority is withdrawn, and import the upgraded generation from its new
versioned URL; no HTTP or WebSocket request is mocked.

The final `--realtime` command installs manifest identity `sdkdemo` from its
test-only `sdk-demo.zip` build ZIP
through the real multipart API. A separate effective-nonadmin consumer opens
it in the ordinary Launcher. The installed module must equal current fixture
bytes, activate once, render the actual capability snapshot, receive a matching
durable/native counts event and update that same panel via smart requery. Real
typed 403/404/handler-500 SDK errors are checked. The handler-500 witness is
one side-effect-free TypeError in the installed test-only `sdkdemo.thrower`
capability, with only its exact rule granted to the synthetic consumer; it is
never bundled and declares no default/everyone grant. Native shell preference
validation precedes JavaScript, so null is not a thrown-handler witness.
Network failure and missing-
args cases use explicitly narrow, restored platform-fetch seams; all other
responses/transports are native. Existing Launcher writes share the channel
and are accounted for before the isolated preference-event baseline.

`run-client-contracts.py` is the joined persistence/UI gate: real registration
and login, effective-nonadmin proof plus real admin denial, concurrent SDK
writes both successful with exactly one stored row, successful theme/scale UI
mirroring, user-independent token CSS, and actual `/app` mount rejection with
bundled frontend unchanged. It SIGTERMs the first kernel, requires zero exit,
starts a distinct process against the **same database/config/root**, poisons
the local mirror, and reloads the same browser profile. Real hydration and SDK
reads must recover the exact persisted values/rows. Browser, kernel and exact
database cleanup are independently attempted and bounded.

Native commands also accept `--image exact-local-reference`, staging test
archives outside the runtime image and verifying the real kernel is PID1.
The complete current ICD case/disposition map, including superseded API/error
wording and #42/#38 boundaries, is
[`client-runtime-contracts.md`](../../docs/reviews/client-runtime-contracts.md).

C++ transport tests also cover cookie-versus-token authentication races,
missing/cross-origin/opaque/scheme-mismatched origins, invalid/expired/revoked
sessions, and configured HTTPS origin over an HTTP proxy upstream. The proxy
cases run in the isolated `plinth_tests_ws_browser_proxy` CTest process because
controller configuration is fixed at registration.
