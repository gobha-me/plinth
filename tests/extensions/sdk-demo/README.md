# sdk-demo — installed Client SDK fixture

This is test-only, not bundled into `shell.zip` or installed at first boot.
Its manifest identity is now `sdkdemo` version `0.1.0`; the historical test-only
`sdk-demo` identity is explicitly retired, with no alias. The source directory
and ZIP basename remain `sdk-demo`. It provides exactly one test-only server
capability for a controlled genuine exception and does not change the server
event format.

Build `plinth_test_extension_zips` to produce
`build/fixtures/extensions/sdk-demo.zip`. The production browser harness
`tests/browser/sdk-installed-production.mjs` installs those exact bytes through
`POST /api/packages` with its task-owned synthetic admin operator, then uses a
different, non-admin session and the ordinary Launcher to open the panel.
The matching task-owned kernel/database and fixture build directory are
supplied through `PLINTH_BASE_URL`, `PLINTH_PG_*`, and
`PLINTH_TEST_BUILD_DIR`; the outer production supervisor owns kernel/database
shutdown. The script closes its own browser contexts and removes only its
installed package, users, sessions, and test group.

The panel declares the fixture-only visibility rule `plinth.sdkdemo.panel`
under the validator's supported reserved `plinth` namespace. It grants no
server capability authority, and `rbac.json` declares no default or `everyone`
grant. The harness refuses an existing rule, verifies its installed `sdkdemo`
ownership, and grants it only to its synthetic consumer group. Normal package
uninstall must remove that exact declared rule. The consumer's effective rules
include virtual `everyone` membership and exclude orphaned rules; admin and
configuration authority must remain absent after installation and the grant.

The fixture also declares `sdkdemo:1:thrower`, guarded by exactly
`sdkdemo.thrower`. Its handler throws a fixed `TypeError` without I/O, state
mutation, or caller-dependent data. There are no default/everyone grants. The
harness refuses an existing capability/rule, verifies installed `sdkdemo`
ownership, grants only that rule to its synthetic consumer group, and requires
normal uninstall to remove both. This is test infrastructure, not a bundled
application capability or a broad permission grant.

The consumer gets an explicit **test-only** `shell.realtime.subscribe` grant
for `plinth:data:ext_shell.user_preferences`. This is a table-wide channel,
not an ordinary-user default grant and not a per-user privacy claim. Both the
panel's raw subscription and snapshot-backed `useData` use that actual native
channel. The snapshot is the real `shell.preferences.get({key:'shell.theme'})`
capability. A successful real SDK preference write must produce a durable,
counts-only native event, reach the same panel's raw counter, and trigger a
bounded real capability requery that changes its theme UI. IDs or full rows
are not inferred from native counts. Launcher preference writes use the same
channel; the harness accounts for their real deliveries before establishing
the subsequent isolated theme-write baseline.

Stable test-only DOM IDs expose loading/error/theme, raw readiness, delivery
count/last sequence, and exact activation/deactivation/shortcut counts. They
do not replace the real module, Preact hooks, transport, or capability dispatch.
`Ctrl+Shift+D` retains the original demo console action. The "Trigger boundary
throw" button retains its deliberate panel error; the contained panel boundary
emits `shell.audit.emit` and displays its fallback. This installed journey does
not claim that it exercises that button; boundary harnesses cover it separately.

The native script also checks typed SDK rejections: C02 uses existing
`kernel.config.get` (HTTP403 / `rbac_denied`); C03 uses a missing shell capability
(HTTP404 / `not_found`). C04 uses a narrowly scoped, restored platform-fetch
failure to produce `NetworkError`, not a server-side failure. C05 supplies normal
`args:null` to the fixture's controlled throwing handler through the real native
route: actual dispatch must return HTTP500 / `cap.handler_threw`.
Each native rejection must propagate the exact nonempty HTTP error message;
the handler-throw witness must retain its actual `TypeError` and fixed fixture
marker. The former shell-preference null-argument attempt did not exercise a JS
exception: native preference validation rejects it before handler invocation
with `invalid_argument`. It is not a null-to-object normalization contract.
The controlled thrower maps the historical `quickjs_throw` scenario to the
shipped taxonomy without changing production errors or argument handling.
C06's narrowly scoped, restored
platform-fetch seam removes the required `args` member and sends that malformed
request to the real route (HTTP400 / `bad_request`); subsequent ordinary calls
still send `args:null` and succeed.

The current test-only manifest name uses lowercase letters only.
There is no `frontend` block: this is an extension panel, not an `/app` mount.
`panels.json` uses the current `client_path` and required `rbac_rule` fields.
On an unexpected package status, the harness reports only bounded envelope
identifiers and fixture validation rule/location diagnostics, not arbitrary
server messages or raw report contents. Known fixture identity/setup or
hyphenated-schema teardown messages are reduced to fixed classification codes;
a reported migration SQLSTATE is restricted to its five-character code.
Old demonstration channels
and hardcoded shell-version/manual-loader instructions are obsolete.
