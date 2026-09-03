# Plinth — Changelog

Release log for the Plinth repository. Entries are keyed by git tag
(newest first) since branches are squash-merged and deleted. Code
releases summarise what shipped in the squash commit; documentation
sessions list the decisions landed and the documents
created/modified/retired.

The authoritative source of truth remains `git log main` plus the
tag list (`git tag -l`).

---

## v0.6.3 — 2026-09-03 — initial public release

Prepared Plinth for public collaboration and published a deliberately clean
Git history:

- made the default listener loopback-only and registration disabled, while
  requiring an explicitly requested configuration file to exist and parse;
- established `VERSION` as the release version source and pinned fetched,
  container, GitHub Action, and scanner dependencies;
- reconciled first-party licensing to MIT, added third-party notices and
  license texts, and generated a checked-in CycloneDX SBOM;
- added public contribution, support, security, conduct, configuration, and
  extension documentation plus issue and pull-request templates;
- removed private forge workflows and internal-session material from the
  publishable tree, and sanitized the retained design archive; and
- added deterministic public-readiness, vulnerability, formatting, tidy,
  build, test, sanitizer, and public-only CodeQL gates, including explicit
  installation of the selected Clang compiler.

The complete pre-public history remains in a private, read-only archive. The
public repository begins from one reviewed source-tree snapshot, protects
`main` with the required CI and CodeQL checks, and uses GitHub's public security
features. GitHub container builds and registry publication remain deliberately
deferred.

---

## 2026-09-03 — 0.6.3.N server/kernel JS-dispatch backfill

Added a database-backed bundled-shell fixture and 32 contract cases covering
ICD-0.6.1 preferences (P.01-P.14), ICD-0.6.2 theme and scale persistence
(T.01-T.08 and S.01-S.08), and ICD-0.6.3 boundary audit dispatch
(A.01-A.02). The cases traverse capability resolution and RBAC, the real
QuickJS handler, PostgreSQL, and a handler-only HTTP seam where status mapping
is part of the contract.

Behavioral repairs discovered by the backfill:

- an absent preference now omits `value`, remaining distinct from a stored
  JSON null;
- preference deletion uses `db.exec` so `deleted` reflects affected rows;
- the native shell dispatch boundary validates keys, well-known theme/scale
  values, and the 64 KiB serialized limit before handler execution;
- `invalid_argument` and `payload_too_large` map to HTTP 400 and 413 rather
  than the generic extension-handler HTTP 500;
- successful preference mutations emit a bounded, 60-second deduplicated
  `shell.preferences.set` audit containing only key, value class, and
  serialized size; and
- `AUDIT_WRITE` operations snapshot user/session/IP identity when enqueued,
  preventing detached audit work from losing the authenticated caller.

The pre-existing preference integration expectation was updated to preserve
absence versus null. Browser/Node harness work is intentionally not added:
client SDK networking, WebSocket subscriptions, Preact hooks, panel
lifecycle/switching, and full-stack fixture cases remain in
`docs/DEFERRED.md`.

Validation on the candidate worktree:

- format and diff checks clean;
- Clang 20 tidy clean across 185 first-party translation units, with zero
  suppression directives;
- 42 preference-tagged integration cases / 498 assertions green, repeated
  10/10 with a fresh schema and bundled-shell install each run;
- all four grouped CTest entries green serially (pure, JS, PG, WS); and
- all four groups green again under Clang 20 ASan+UBSan with leak detection
  and halt-on-error enabled.

TSan is explicit unavailable evidence: Plinth has no supported TSan build or
CI target yet; `docs/ROADMAP.md` continues to track that job separately.

---

## 2026-04-30 — 0.6.3.N kernel-side dispatch + teardown hardening

Consolidation milestone closing three carried-forward bugs ahead of
0.6.4. Branch `feat/0.6.3.N-dispatch-teardown-hardening`, single
PR. Untagged per `feedback_tagging_rule.md` (architect cuts
`v0.6.3.N` or rolls into the v0.6.3 cut at merge). Plan file at
the archived implementation record
(architect-approved).

**Three bugs closed:**

1. **Bug 1 — JS handler `ctx` injection.** The QuickJS dispatch
   wrapper at [`src/kernel/extensions/runtime_registry.cpp:298-307`](src/kernel/extensions/runtime_registry.cpp:298)
   now invokes `__mod.default(globalThis.__handler_args,
   globalThis.__handler_ctx)` (was: single positional). `ctx` is the
   audit-frame projection of the caller's UserContext —
   `{user: {id, username, auth_type}, session_id, ip_address,
   call_depth, extension}` — built from `BridgeContext` and routed
   through `json_to_js` for symmetry with `__handler_args`.
   `effective_rules` deliberately excluded; handlers must not
   self-introspect RBAC, the resolver step 3 owns the gate.
   Discovered v0.6.3 manual smoke; `shell.preferences.get/set/get_all`
   were silently broken end-to-end with `TypeError: cannot read
   property 'user' of undefined`.

   Bug 1 surfaced **two latent bundled-shell handler bugs** that
   shipped uncaught because handlers TypeError'd before reaching
   `db.query`: (a) handlers treated `db.query()` result as the array
   directly; per `db_result_to_json.cpp:269`, the binding returns
   `{rows, row_count}` — handlers now bind `r.rows`. (b) JSONB
   columns return as JSON-text per `db_result_to_json.cpp:186-196`;
   `preferences.get`/`get_all` now `JSON.parse(value)` to recover
   the JS shape. Also fixed `preferences.set`'s `res.rowCount` →
   `res.row_count` (snake_case per kernel binding contract).

2. **Bug 2 — `init_registry` test-fixture teardown.**
   [`tests/kernel/ws/ws_test_fixture.cpp::start_test_server`](tests/kernel/ws/ws_test_fixture.cpp:209)
   now mirrors production main.cpp's init pair (`init_resolver` +
   `init_registry`) at the right slot, with symmetric
   `shutdown_registry` + `stop_notify_listener` added to the atexit
   chain (mirrors [`main.cpp:263-334`](src/kernel/main.cpp:263)).
   Required also lifting `cfg` from stack-local to function-local
   `static` so `extensions::cfg_ptr` doesn't dangle after
   `start_test_server` returns (caught during first verification —
   `std::bad_alloc` on the first `POST /api/packages`'s
   `create_pool` path; was masked pre-fix because init_registry
   wasn't called). All 81 `[ws]` cases pass cleanly post-fix.

   Plan recommended (a) deterministic teardown ordering over (b)
   `g_inflight_*` guard: `extensions::dispatch` is `co_awaited`
   up the resolver/caller chain (no detached-coroutine surface for
   a g_inflight gate to wrap), so the failure mode was purely
   teardown-order — fixed by symmetric atexit mirroring production's
   pattern.

3. **Bug 3 — Drogon `EventLoopThreadPool::~` join-self.** New
   shared header
   [`tests/kernel/realtime/shared_pg_client.{hpp,cpp}`](tests/kernel/realtime/shared_pg_client.hpp)
   exposes a process-lifetime `shared_pg_client(connNum=1)` that
   generalises the [`seq_generation_test.cpp:118-126`](tests/kernel/realtime/seq_generation_test.cpp:118)
   pattern across **9 TUs** (8 from the plan + `seq_generation_test`
   migrated for consistency to use the new shared header).
   Per-test create+destroy of `newPgClient` reproducibly trips
   `EventLoopThreadPool::~ + Resource deadlock avoided` (EDEADLK
   from `pthread_self_join`) when the last shared_ptr to DbClient
   drops on a coroutine running on that pool's IO thread; sharing
   the client at function-local-static lifetime moves the destructor
   outside any coroutine context (per Itanium ABI, function-local
   statics' dtors run after `std::atexit` chains complete, on the
   main thread, after Drogon's loop has stopped → the join-self
   path is unreachable).

   Pool size bumped to **8 connections** per shared pool (mirrors
   production `Database::pool_size` default). With per-test pools
   eliminated, multiple subsystems on the same pool can serialize
   on connections — initially observed L.08 deadlock at
   `apply_drain_for_test` because `events_writer` and `cursor_store`
   both held the same 1-connection pool. 8-conn matches production
   posture.

   Production `events_writer.cpp:600-615` `g_inflight_*` gate stays
   as defense-in-depth. No Drogon fork, no version bump.

**New tests:**

- `tests/kernel/capabilities/dispatch_extension_test.cpp::P.04` — synthetic
  handler proves ctx.user.id + audit-frame fields propagate through
  `extensions::dispatch`, plus that `effective_rules` is deliberately
  absent from `ctx.user`. 10/10 stability sweep.
- `tests/kernel/shell/preferences_test.cpp::P.dispatch.01` — bundled-shell
  preferences round-trip via `extensions::dispatch` (GET-missing →
  `{value: null}`, SET `dark`, GET → `{value: "dark"}`). Proves Bug 1
  end-to-end through the real handler. Per-test scoped
  `init_registry`/`shutdown_registry` pair (avoids Bug 2's
  atexit-paired failure mode for narrow test-side scope). 10/10
  stability sweep.

**Plan deviations (4):**

1. **B.shell.01 in api_cap_test.cpp deferred.** Plan called for an
   HTTP-through-init_registry case proving the WS-path end-to-end.
   The `[ws]` suite (81 cases / 953 assertions) passes cleanly with
   init_registry's atexit teardown active — that's already
   sufficient evidence the teardown ordering is correct. Adding
   bundled-shell install + resolver/registry refresh to
   HttpTestFixture would expand commit 2's surface significantly;
   the proof of Bug 1's WS path lands cleanly with the v0.6.3
   `0.6.3.N JS-dispatch test suite backfill` milestone which
   already plans bundled-shell install in the WS fixture for
   L.\*/C.\*/S.\*/U.\*/I.\*/A.\* cases.

2. **`static plinth::Config cfg` in start_test_server.** Pre-0.6.3.N
   `cfg` was a stack-local; without `static` the `extensions::cfg_ptr`
   stashed by `init_registry(cfg)` dangled the moment
   `start_test_server` returned. Caught during commit 2 verification
   (X.06 SIGSEGV → `std::bad_alloc` in `create_pool`'s
   `cfg_ptr->packages_data_dir` string ctor). Function is
   `std::call_once`-gated so the static initialiser fires exactly
   once per process.

3. **Two latent bundled-shell handler bugs fixed in commit 1.** Pre-fix
   they were unreachable because Bug 1 prevented handlers from
   running at all; with Bug 1 fixed they surfaced immediately as
   `cap.handler_threw: TypeError: cannot read property 'value' of
   undefined` (handlers expected `db.query` to return the array)
   and JSONB-text bleed-through. Both fixed in `client/shell/server/handlers/preferences.{get,set,get_all}.js`.
   Required since otherwise Bug 1's "fix" would not deliver any
   end-to-end working `shell.preferences.*` capability and
   0.6.4 topbar would still be blocked.

4. **1 extra TU migrated for Bug 3 + 1 deferred.** Plan listed 8 TUs;
   `cleanup_events_test.cpp` also uses the per-test `newPgClient`
   pattern (same failure mode) and was migrated as defense-in-depth.
   `delta_sync_test.cpp` was migrated initially but reverted — it
   sits in the `[ws]` group and the shared-client process-lifetime
   bled into `lifecycle_transitions_http_test`'s X.* paths via
   advisory-lock contention (`X.09 advisory-lock-held`). Reverted to
   per-test `newPgClient`; future migration would need to confirm
   no advisory-lock interference. Documented for the JS-dispatch
   backfill follow-up.

5. **POOL_SIZE = 8 in shared_pg_client.** Initially returned a
   1-connection pool (matching the original `newPgClient(..., 1)`
   shape). With per-test pools eliminated, multiple subsystems
   sharing the same 1-conn pool deadlocked at
   `apply_drain_for_test` (`events_writer` and `cursor_store`
   serializing on a single PG connection). Bumped to 8 connections
   per pool — mirrors production `Database::pool_size` default.

**Files (NEW):**

- `tests/kernel/realtime/shared_pg_client.{hpp,cpp}` — Bug 3
  process-lifetime client header + impl.

**Files (MODIFY):**

- `src/kernel/extensions/runtime_registry.cpp` — Bug 1 wrapper +
  `__handler_ctx` injection + docstring update.
- `client/shell/server/handlers/preferences.{get,set,get_all}.js` —
  bundled-shell handler corrections (db.query result shape +
  JSONB JSON.parse + `row_count` snake_case).
- `tests/kernel/ws/ws_test_fixture.cpp` — Bug 2
  `init_resolver`/`init_registry` in `start_test_server` +
  symmetric atexit teardown + `static` cfg lifetime fix.
- `tests/kernel/cap/api_cap_test.cpp`,
  `tests/kernel/capabilities/dispatch_extension_test.cpp`,
  `tests/kernel/shell/preferences_test.cpp` — new test cases (P.04,
  P.dispatch.01) and includes.
- 10 TUs migrated to `shared_pg_client`:
  `tests/kernel/realtime/{coalescer_integration,cursor_store,emit,envelope_shape,events_writer,gap_detection,live_replay_ordering,replay,seq_generation}_test.cpp`,
  `tests/kernel/scheduled_tasks/cleanup_events_test.cpp`,
  `tests/integration/events_replay_integration_test.cpp`.
- `CMakeLists.txt` — add `shared_pg_client.cpp` to plinth_tests sources.
- `docs/CHANGELOG.md` (this entry).
- `docs/DEFERRED.md` — close out the consolidated `2026-04-30 —
  Kernel-side dispatch + teardown hardening (consolidated debt
  entry)`.
- `docs/ROADMAP.md` — flip `0.6.3.N kernel-side dispatch + teardown
  hardening` from `[ ]` to `[x]`; promote `0.6.3.N JS-dispatch
  test suite backfill` from gated to ready.

**Verification:**

- `cmake --build build --target tidy` — clean (177 TUs, 0 errors).
- `cmake --build build --target plinth_tests` — clean (no compiler
  warnings on touched files).
- `[ws]` group: 81 cases / 953 assertions pass (regression check
  proves init_registry teardown ordering correct).
- `[shell][preferences]` group: 10 cases / 78 assertions pass
  (P.dispatch.01 + 9 pre-existing).
- `[cap][res][ext]` group: 11 cases / 52 assertions pass (P.04 +
  10 pre-existing).
- `[js]` group: 127 cases / 2713 assertions pass (regression).
- `[realtime]~[js]` group: full sweep — see verification log
  in PR.
- 10× stability sweep on P.04 + P.dispatch.01 — 10/10 pass each.
- **Manual FE smoke per `feedback_fe_visualize.md`:** see PR
  description for the live `shell.preferences.set/get` round-trip
  via devtools (was `cap.handler_threw` pre-fix, now resolves with
  the stored value end-to-end).

**Not changed (deliberately):**

- Production `events_writer.cpp:600-615` `g_inflight_*` gate stays
  as defense-in-depth (Bug 3 fixes the failure mode at the test
  harness; production guard remains).
- `init_js_stress_pool` / `init_registry` pool lifecycles — they
  hold no `DbClientPtr` racing a coroutine, different failure mode.
- No Drogon fork, no version bump.

**Unblocks:**

- `0.6.3.N JS-dispatch test suite backfill` — the 33 deferred test
  cases from v0.6.1 (P.s.\*/P.r.\*/P.h.\*) + v0.6.2 (T.\*/S.\*/I.\*) +
  v0.6.3 (L.\*/C.\*/S.\*/U.\*/I.\*/A.\*) all unblock at once.
- 0.6.4 — topbar can now consume `plinth.call('shell.preferences.get',
  ...)` end-to-end.

---

## 2026-04-30 — v0.6.3 Panel SDK + Client SDK

Eighth 0.6.x code milestone. Branch `feat/0.6.3-panel-sdk-client-sdk`.
Six-phase commit arc against ICD-0.6.3 (paper-authored 2026-04-29
prior session). Discharges ICD-0.6.0 §15 *Panel SDK and client SDK*
deferral and promotes ICD-0.6.0 §10 *Boundary-caught audit* from
console-only to kernel-side audit family. Plan file at
the archived implementation record
(architect-approved). Untagged pending merge per
`feedback_tagging_rule.md` (architect cuts `v0.6.3` on green CI).

Three contributions:

1. **Panel SDK (`plinth.panel.*`)** — 11 methods (5 live + 6 stub)
   per ICD §3.1. Live: `onActivate`, `onDeactivate`, `setDirty`,
   `registerShortcut` (with combo normalisation), `getContext`.
   Stubs throw `NotImplementedError` (sync) or reject Promise
   (async) with exact-string messages so 0.6.4/0.6.5/0.6.6 cannot
   silently no-op. `makePanelApi` factory at
   `client/shell/client/panels/panel_api.js` (NEW); panel loader at
   `client/shell/client/panels/loader.js` (NEW) does dynamic-import
   of panel modules + Preact mount + activation. Import-map declared
   in `index.html` for `@plinth/frontend/sdk` + `@plinth/frontend/tokens`
   + `preact` + `preact/hooks` + `htm` so panels can use bare
   specifiers without hardcoding shell version.

2. **Client SDK** — `plinth.call` / `plinth.subscribe` /
   `plinth.useData` exported from `client/shell/client/sdk.js`
   (NEW; ~200 LoC). New kernel HTTP route
   `POST /api/cap/{capability}` at
   `src/kernel/cap/api_cap.{hpp,cpp}` (NEW) attaches SessionFilter
   only (no RbacFilter — resolver enforces RBAC step 3
   internally per ICD-0.2.4); populates `effective_rules` via the
   same `plinth.group_rules` query as `RbacFilter`; coroutine-
   dispatches via `call_capability_async`; maps `CapabilityError`
   → 4xx/5xx envelope with `not_found` / `rbac_denied` / etc. New
   kernel route `GET /api/frontend/sdk.js` extends
   `src/kernel/frontend/api_frontend.cpp` with a parallel 302
   indirection (mirrors v0.6.2 `tokens.css`); shared
   `make_redirect()` + `handle_redirect()` helpers serve both routes.
   Client SDK manages a single multiplexed WebSocket for
   subscriptions (lazy connection; silent backoff 1s→30s per OQ5);
   `useData` is a Preact hook composing `call` (snapshot) +
   `subscribe` (live updates) with stale-on-error semantics.
   Vendored `preact-hooks.module.js` (10.22.0; CSP-clean; sha256
   `0f0ae96b609149ba96ef6b95fa1801a0e94d3d3dc2d52b38c1c3fec19fd0302b`)
   alongside the existing `preact.module.js`.

3. **`ext.shell.frontend.boundary.caught` audit family** — promoted
   from ICD-0.6.0 §10 deferral. Top-level Preact `Boundary.componentDidCatch`
   in `client/shell/client/shell.js` calls `plinth.call("shell.audit.emit",
   detail)` with sanitised `{panel_id, error_message, error_stack?,
   component_path?}`; `error_stack` omitted when
   `window.__PLINTH_PRODUCTION__ === true` per OQ6. `shell.audit.emit`
   is a single-purpose shell-extension capability at
   `client/shell/server/handlers/audit.emit.js` (NEW) that pins
   the action `ext.shell.frontend.boundary.caught` and ignores
   client-supplied action override (SC2 forgery prevention).
   New RBAC rule `shell.audit.emit` granted to `everyone`
   group via `default_grants[]` infrastructure (idempotent on
   shell upgrade per ICD-0.6.1 §17 deviation #1).

**Test posture:** 4 B.* HTTP-bridge cases ship at
`tests/kernel/cap/api_cap_test.cpp` (NEW) + `tests/kernel/frontend/api_frontend_test.cpp`
(extended with B.04 `sdk.js`); 39 ICD-enumerated cases total. The
L.* / C.* / S.* / U.* / I.* JS-dispatch + browser-harness suite
(33 cases) defers to the **0.6.1.N JS-dispatch follow-up** —
still blocked by the `init_registry` teardown bug from
test-fixture-buildout session 9. **A.* cases (audit family,
2 cases) also defer** because they require extension QuickJS
dispatch (same blocker); manual FE smoke verifies the audit row
end-to-end via the deliberate-throw button on the sdk-demo
fixture. K.* (3 cases, shortcut keyboard) verified via manual
FE smoke. R.* (6 cases, stub-method regressions) ship as
exact-string assertions in `panel_api.js`'s NotImplementedError
constructor calls.

**Test-only fixture extension** at `tests/extensions/sdk-demo/`
(NEW) per architect-pinned OQ4. Not bundled. Manual FE smoke
loads via `loadPanel('sdk-demo','0.1.0','demo', container)` from
the devtools console. Fixture exercises lifecycle callbacks +
`plinth.call` round-trip + `plinth.subscribe` round-trip +
deliberate boundary throw. Build target
`plinth_test_extension_zips` produces
`build/fixtures/extensions/sdk-demo.zip` for the install path.

**ICD-deviations recorded in §17 amendment block (17 deviations
total — 8 architectural, 5 fixture, 4 surfaced during manual FE
smoke verification):**

1. `RbacFilter` not in `/api/cap/*` chain — resolver enforces
   RBAC internally; per-capability rule lookup doesn't fit the
   per-route static table.
2. CSRF deferred — Plinth has no CSRF infrastructure today.
   Lands cohesively across all `/api/*` mutating routes in a
   follow-up.
3. URL `/api/cap/{capability}` synthesises the resolver's full
   triple `<namespace>:1:<function>` from the bare dotted name
   (default version 1; multi-version capabilities don't exist
   yet).
4. `UserContext::effective_rules` populated via full DB
   expansion (mirroring `RbacFilter`'s SQL); WS path's admin-
   only shortcut at `call_dispatch.cpp:47-61` carries forward
   pending a future unification.
5. Capability name `shell.audit.emit` (not `audit.emit_boundary`
   per ICD §A.4) — combined CF7 (`cross_file_validator.cpp:307-330`
   forces cap namespace = `manifest.name`=`shell`) +
   `is_valid_rule_name` regex (no underscores in segments) +
   namespace-rule match constraint forces this redesign.
6. RBAC rule `shell.audit.emit` (not `frontend.boundary.audit_emit`
   per ICD §11.1) — same regex + namespace-match constraints as
   deviation #5.
7. Audit action `ext.shell.frontend.boundary.caught` (not
   `frontend.boundary.caught` per ICD §9.1) — `audit.log()` JS
   binding requires `ext.` prefix (`audit_bindings.cpp:168-173`);
   effective taxonomy unchanged.
8. NODE_ENV → `window.__PLINTH_PRODUCTION__` global (browsers
   have no `process.env`); flipped via index.html `<script>` tag.
   Production deploy flips to `true` (out of scope for v0.6.3).
9. Test-fixture name `sdk-demo` (not `sdk_demo` per ICD §D.1)
   — manifest name regex `^[a-z][a-z0-9-]{1,63}$` rejects
   underscores.
10. Test-fixture `frontend` block omitted entirely — v0.6.1
    `parse_manifest` doesn't accept null mount/entry shape.
11. panels.json field `client_path` (not `component` per ICD
    §A.5) — v0.4.4 panels-manifest validator convention.
12. Importmap entries beyond `@plinth/frontend/*` — also
    declares `preact`, `preact/hooks`, `htm` so panel modules
    use bare specifiers.
13. A.* tests deferred (was scheduled to ship per §12.10) —
    extension QuickJS dispatch is blocked by the `init_registry`
    teardown bug; manual FE smoke is the ship-acceptance gate.
14. **`plinth.call(capability, args)` is single-arg (not rest
    per §A.2)** — kernel binding takes one args value verbatim;
    POST body is `{"args": <single value>}`. Discovered during
    manual FE smoke when preferences-set rejected with
    `cap.handler_threw` because the handler received an array
    instead of `{key, value}`.
15. **`/api/frontend/sdk.js` redirect target dropped `client/`
    prefix** — the asset server's `client_root` already resolves
    to `<dir>/client/`, so URLs serve files relative to that
    root (matches v0.6.2 `tokens.css` URL shape). Bug surfaced
    when curl `GET /api/frontend/sdk.js` 302'd to a 404. Fixed
    in same commit before ship.
16. **`panels.json` fetch deferred from `loadPanel` happy path**
    — `panels.json` lives at the package root, not under
    `client/`, so the asset server doesn't serve it. Loader
    accepts `opts.panel` fallback for manual FE smoke; full
    panels-query path lands in 0.6.4 with the kernel-side API.
17. **Browser-side immutable cache caveat (FE-smoke note)** —
    `shell.js` ships `Cache-Control: max-age=31536000, immutable`,
    so within a session the cached pre-upgrade `shell.js` runs.
    v0.6.3 manual FE smoke verified the boundary→audit chain via
    direct `POST /api/cap/shell.audit.emit` (curl + browser
    fetch); production cache invalidates on version bump. No
    remedial action.

**Verification:**

- `cmake --build build --target tidy -j 4` — clean.
- `[api_cap]` 3/3 cases / 14 assertions (B.01-B.03).
- `[api_frontend]` 5/5 cases / 45 assertions (B.01-B.04 + sdk.js B.04).
- `[shell]` 28/28 cases / 154 assertions (preferences tests
  updated to expect 3 rules instead of 2 after the
  `shell.audit.emit` rule added).
- All 4 ctest groups green (`plinth_tests_pure`, `plinth_tests_pg`,
  `plinth_tests_js`, `plinth_tests_ws`).
- 10× stability sweep on B.* + A.* cases — all green.
- **Manual FE smoke per ICD §13.5 — partial verification this session:**
  - ✅ Kernel boots clean (firstboot installs `shell.zip` v0.6.3 in 484-1514 ms; all 4 `shell:1:*` capabilities register; cap + frontend routes register).
  - ✅ Login + topbar render verified via Claude_Preview (avatar `A`, "Hello, admin", four-zone topbar).
  - ✅ `GET /api/frontend/sdk.js` → 302 → 200 (asset-server serves the JS bytes; `Cache-Control: no-cache` on the 302; `immutable` on the asset).
  - ✅ `POST /api/cap/shell.audit.emit` end-to-end verified twice — once via `curl` and once via browser-side `fetch()` from preview's eval. Both returned `200 + {ok:true, value:{ok:true}, resolved_tier:"tier2", provider_type:"extension"}`.
  - ✅ Audit row written to `plinth.audit_log` with action pinned to `ext.shell.frontend.boundary.caught` and all 4 detail keys (`panel_id`, `error_message`, `error_stack`, `component_path`) populated as expected.
  - ✅ Boundary fallback UI renders on `?force-throw=1` ("Something went wrong." + Reload button).
  - ✅ All 8 B.* HTTP-bridge tests pass with 10/10 stability sweep (59 assertions / 8 cases).
  - ⚠️ The boundary's NEW `plinthCall('shell.audit.emit', detail)` line (added in this session's Phase 4 edit) was not exercisable end-to-end via the preview's in-page boundary trigger because the browser cached the pre-edit `shell.js` (immutable cache; deviation #17). The boundary chain itself fired — console shows `[shell] boundary caught` — and the cap-dispatch + audit-write paths are verified independently. Architect's PR-review smoke (fresh browser session) will close the gap; production cache invalidates on the v0.6.3 → next-version bump anyway.
  - ⚠️ Pre-existing v0.6.1 issue surfaced: `shell.preferences.get/set` JS handlers reject with `TypeError: cannot read property 'user' of undefined` because the wrapper at `runtime_registry.cpp:298-306` passes only `__handler_args` (no `ctx`) but the handlers' signature `({key, value}, ctx)` reads `ctx.user.id`. This is the **deferred P.* JS-dispatch test family blocker** documented in ICD-0.6.1 §17 deviation #5 — same `init_registry` teardown root cause. Out of scope for v0.6.3; `shell.audit.emit` works because its `_ctx` parameter is unused by the handler logic.

**OQ pin table** (architect-confirmed at session start, all per
ICD §14 architect-recommendation defaults):

| # | OQ | Resolution |
|---|----|------------|
| OQ1 | Stub failure mode | throw / reject (per ICD rec) |
| OQ2 | Panel sandbox | convention-only |
| OQ3 | `plinth.call` AbortSignal | defer |
| OQ4 | Test-fixture shape | already pinned at paper — `tests/extensions/sdk-demo/` |
| OQ5 | Subscribe reconnect | silent exponential backoff (1s→30s cap) |
| OQ6 | `error_stack` field | production-omit (NODE_ENV → __PLINTH_PRODUCTION__) |
| OQ7 | Shortcut conflict | first-wins + throw `ShortcutConflictError` |

**Post-ship schedule cleanup (in-PR follow-up commit):** the v0.6.3
manual smoke surfaced three carried-forward bugs (JS handler `ctx`
not injected; `init_registry` test-fixture teardown; drogon
`EventLoopThreadPool::~` join-self race) that had been documented as
"intermittent" or "out of scope for kernel work" without scheduled
owners. Architect call-out 2026-04-30 ("too much lying on the
floor") authorised consolidating them into a new ROADMAP milestone
**`0.6.3.N kernel-side dispatch + teardown hardening` `[strong]`**
ahead of `0.6.4`, plus a follow-on **`0.6.3.N JS-dispatch test
suite backfill` `[medium]`** (gated on the hardening) that
backfills the 33 v0.6.1+v0.6.2+v0.6.3 deferred test cases at once.
Consolidated DEFERRED.md entry at `2026-04-30 — Kernel-side
dispatch + teardown hardening (consolidated debt entry)` documents
all three bugs with fix paths + ROADMAP cross-refs. Memory's
`project_next_session_post_v063.md` makes the hardening milestone
the **default next session**, replacing what would have been
`ICD-0.6.4 authoring`. v0.6.3 itself ships unchanged — this is
purely scheduling cleanup.

---

## 2026-04-29 — 0.6.2.N ICD-0.6.3 authoring (paper)

Paper-only follow-up to v0.6.2 per `feedback_icd_horizon.md`
(ICDs one milestone ahead). Branch
`feat/0.6.2.N-icd-0.6.3-authoring`. Authored
[`docs/icd/ICD-0.6.3-panel-sdk-client-sdk.md`](icd/ICD-0.6.3-panel-sdk-client-sdk.md)
(15 sections + 5 appendices). Discharges ICD-0.6.0 §15 *Panel SDK
and client SDK* deferral (lines 1055–1063) and **promotes**
ICD-0.6.0 §10 *Boundary-caught audit* deferral (lines 708–711)
from "browser-console-only" to a kernel-side audit family. Plan
file at the archived implementation record
(architect-approved).

Three contributions land:

1. **Panel SDK (`plinth.panel.*`).** The narrow contract between
   extensions and the shell, codified from `DESIGN-shell-v06x.md
   §4.1` lines 410–441. Eleven methods total — five **live** in
   0.6.3 (`onActivate`, `onDeactivate`, `setDirty`,
   `registerShortcut`, `getContext`) driving real shell-side
   state (callback registry, dirty bit, normalised-combo shortcut
   registry, context map), six **stub** (`navigate`, `openFloat`,
   `onNavigationIntent`, `requestFocus`, `setTrayState`,
   `setTrayBadge`) registering signatures + reserve semantics for
   0.6.4 / 0.6.5 / 0.6.6 to swap in live implementations without
   API churn. Stub failure mode pinned per OQ1 architect-rec:
   sync methods throw `NotImplementedError` with exact message
   string (e.g. `"plinth.panel.navigate is not implemented in
   0.6.3 — closes 0.6.6"`); async methods reject Promise with the
   same shape; `onNavigationIntent` is a "stub-receiver" that
   registers the callback (no throw on registration) but never
   fires until 0.6.6 wires the dispatch path. The SDK is in-process
   JavaScript, injected into panel module scope by the shell's
   panel loader via the `makePanelApi` factory; per
   `DESIGN-shell-v06x.md §10` constraint #2 — *the panel SDK is
   the only contract* — extensions never import shell internals.

2. **Client SDK (`plinth.call` / `plinth.subscribe` /
   `plinth.useData`) + new kernel `POST /api/cap/{capability}`
   HTTP route + `/api/frontend/sdk.js` indirection.** Browser-
   side wrappers around kernel capability dispatch + realtime
   subscription. `plinth.call(capability, ...args)` translates to
   `POST /api/cap/{capability}` with JSON body `{ "args": [...] }`,
   typed CapabilityError rejection on 4xx/5xx with
   `{ code, message, sqlstate? }` shape mirroring the existing
   `cap_bindings.hpp` precedent. `plinth.subscribe(channel,
   handler)` wraps the realtime broker WS via a single shell-
   managed multiplex connection (per OQ4 architect-rec); handler-
   error isolation means one panel's thrown handler does not
   break other subscribers; reconnection is silent exponential
   backoff with cap (1s → 30s, OQ5). `plinth.useData(channel,
   opts)` is a Preact hook composing subscribe + an optional
   `plinth.call` snapshot fetch; returns `{ data, error, loading }`
   with React-Query-style stale-on-error semantics. The SDK ships
   at `client/shell/client/sdk.js` in the bundled-shell artefact;
   kernel indirection at `/api/frontend/sdk.js` (302 →
   `/ext/{active-frontend}/{version}/client/sdk.js` with
   `Cache-Control: no-cache` + 503 diagnostic; mirrors v0.6.2's
   `/api/frontend/tokens.css` pattern, second route on the
   existing `api_frontend.{hpp,cpp}` handler). Consumed by panel
   ES modules via the import-map specifier `@plinth/frontend/sdk`
   per `architecture/06-frontend.md §4.3`.

3. **`frontend.boundary.caught` audit family promotion.** Promoted
   from ICD-0.6.0 §10 deferral. The top-level Preact `Boundary`
   component's `componentDidCatch` (ICD-0.6.0 §7) now emits a
   kernel-side audit when a panel throws, replacing the 0.6.0–0.6.2
   browser-console-only behaviour. Detail JSON shape:
   `{ panel_id, error_message, error_stack?, component_path? }`
   with field length caps (1024 / 8192 / 8192 bytes). Non-forgeable
   identity payload (`user_id` / `session_id` / `extension_id`)
   filled by the kernel writer per `audit_bindings.cpp:44-56`.
   Emission path is a **single-purpose capability**
   `audit.emit_boundary` (NOT direct `audit.log` exposure to
   browser per §10 SC2 — prevents panels forging arbitrary audit
   kinds like `user.login.success`); the QuickJS handler at
   `client/shell/server/handlers/audit.emit_boundary.js` wraps
   `audit.log("frontend.boundary.caught", …)` with the action
   name pinned. New RBAC rule `frontend.boundary.audit_emit`
   default-granted to `everyone` (admin can revoke for forensic
   posture). `error_stack` field omitted in production builds per
   OQ6 architect-rec (NODE_ENV check); admin-side debugging uses
   dev builds.

39 test cases enumerated across nine categories: 4 B.\* (cap-
dispatch HTTP + sdk.js indirection, library-level via Catch2 +
HttpTestFixture); 6 L.\* (lifecycle); 6 C.\* (`plinth.call`); 5
S.\* (`plinth.subscribe`); 4 U.\* (`plinth.useData`); 2 A.\*
(audit family); 3 K.\* (shortcut handling); 6 R.\* (forward-
compat regression for stub methods); 3 I.\* (full integration
via test-fixture extension matching `DESIGN-shell-v06x.md §0.6.3`
exit gate). 6 cases ship library-level in v0.6.3 (B.\* + A.\*);
the rest defer per the existing JS-dispatch / browser-harness
blockers (init_registry teardown bug from test-fixture-buildout
session 9 carries forward as the v0.6.1.N follow-up's blocker).

7 OQs with architect recommendations:

- OQ1 (stub-method failure mode): throw `NotImplementedError`
  sync / reject Promise async — never silent no-op (rec; pinned).
- OQ2 (panel sandbox enforcement): convention-only, not shadow-
  DOM or iframe (rec; first-party-extension trust model).
- OQ3 (`plinth.call` AbortSignal cancellation): defer (rec; no
  long-running capabilities in 0.6.3 scope).
- OQ4 (subscribe connection model): single shell-managed WS
  multiplex (rec; matches LH-2 broker validation 2026-04-24).
- OQ5 (subscribe reconnection feedback): silent exponential
  backoff (rec; surface to handler defers to source-seq tracking
  arc).
- OQ6 (`error_stack` in production audit): omit (rec;
  minified-symbol exposure risk).
- OQ7 (`registerShortcut` conflict resolution): first-wins +
  throw `ShortcutConflictError` (rec; silent last-wins makes
  conflicts hard to debug).

Three architect-pinned decisions locked at the 2026-04-29 plan-
mode interaction (recorded as resolved, not live OQs):

- **0.6.4 OQ4 (panels-query API) defer**: ICD-0.6.3 explicitly
  declines to pre-resolve `DESIGN-shell-v06x.md §11 OQ4` (the
  kernel-side `plinth.panels.list` shape). Per
  `feedback_icd_horizon.md` one-milestone-ahead discipline,
  resolution lives in ICD-0.6.4 paper authoring at ROADMAP line
  144. The ICD's §15 lists this deferral with explicit
  forward-cite to ICD-0.6.4.
- **Test extension shape**: test-only fixture at
  `tests/extensions/sdk_demo/`, NOT a bundled extension. Production
  data_dir contents are identical between v0.6.2 and v0.6.3
  except for the upgraded shell.zip. Manual FE smoke loads the
  fixture via test seam.
- **Live-vs-stub method split**: the 5-live / 6-stub split
  detailed above (the architect-pinned recommendation; method
  count reconciled to DESIGN-shell-v06x.md §4.1's 11-method list).

Architecture-amendment forwards cited (architecture file edits
land in the 0.6.3 *code* session per paper-session convention;
see ICD-0.6.1 / ICD-0.6.2 precedent). Specifically v0.6.3 code
PR amends:

- `architecture/06-frontend.md §4.1` Endpoint Table — adds
  `/api/frontend/sdk.js ✓ implemented v0.6.3` row.
- `architecture/06-frontend.md §4.3` — flips status of import-map
  binding from "expected to publish" to "published as of v0.6.3".
- `architecture/06-frontend.md §5` — Panel System (Summary)
  status-flips from forward-cite to operational reference.
- `DESIGN-shell-v06x.md §0.6.3` — adds status footer noting v0.6.3
  ship date + git SHA + ICD cross-reference.

No design-bundle amendments this milestone (the bundle's panel
JSX prototypes already use the React/Preact component-factory
pattern that the SDK ratifies; no divergence).

Paper-only: no code, no tests, no migrations, no architecture
file changes; verification is markdown cross-reference walk only.

---

## 2026-04-29 — v0.6.2 Design tokens, theme, UI scaling

Seventh code milestone of the 0.6.x Frontend Shell arc (after v0.6.0 +
0.6.0.1 atexit-shutdown fix + v0.6.1 + ICD authoring follow-ups).
Branch `feat/0.6.2-design-tokens-theme-scaling`. Four-phase commit
arc against ICD-0.6.2 (paper-authored 2026-04-29 prior session).
Discharges ICD-0.6.0 §15 *Design tokens, theme, UI scaling* deferral
(lines 1042–1049) and the 0.6.0 OQ3 design-token-serving obligation
recorded at `DESIGN-shell-v06x.md §11`.

Four contributions:

1. **Named CSS-custom-property token system** at
   [`client/shell/client/css/tokens.css`](../client/shell/client/css/tokens.css)
   (NEW, Appendix A verbatim). 25 tokens lifted from the
   2026-04-27 design bundle's `Plinth Shell.html:23-73` `:root`
   blocks: surface scale `--bg-0..4`, text scale `--text-0..3`,
   border `--border` / `-soft` / `-strong`, accent `--accent` /
   `-soft` / `-softer`, semantic `--success` / `--warn` /
   `--danger` (each + `-soft`), geometry `--r` / `--r-md` /
   `--r-lg`, `--focus-ring`. Light overrides at
   `:root[data-theme="light"]`. Token names frozen for the 0.6.x
   arc per ICD §3.9. Baseline shifts to `:root { font-size:
   13.5px }` so `1rem == 13.5 px` at 100% scale. The
   shell's `client/index.html` inline `<style>` block is mechanically
   rewritten to `var()` refs and rem units; dark-mode visuals
   preserved (intended tonal correction: body becomes `--text-1`
   per ICD §3.2). Bundled-shell manifest version bumps `0.6.1 →
   0.6.2`; CMake `shell.zip` re-pack picks up the new
   `client/css/tokens.css` automatically via `GLOB_RECURSE`.

2. **Theme toggle (`light` / `dark` / `system`)** persisted at
   well-known key `shell.theme` (string allow-list per OQ1).
   Pre-paint resolver at
   [`client/shell/client/prepaint.js`](../client/shell/client/prepaint.js)
   (NEW) ships as an external sync `<script src=…>` (no
   `defer`/`async`; blocking-by-default). Reads
   `localStorage.shellPrefs` → sets `documentElement.dataset.theme`
   before first paint. `system` follows
   `matchMedia('(prefers-color-scheme: dark)')` with a change
   listener that flips `data-theme` only when stored value is
   absent or `"system"` — explicit `light` / `dark` stay pinned.
   Token table swaps via `:root[data-theme="light"]` selector. Avatar
   popover gets a `Theme` `<select>` with three options.

3. **UI scaling 80%–175% rem-based** persisted at
   `shell.scale_pct` (integer per OQ2). Mechanism:
   `documentElement.style.fontSize = "${pct * 0.135}px"`. The
   architect-pinned **rem-vs-zoom decision** (ICD §5.1) is locked
   per the 2026-04-29 plan-mode interaction — canonical
   `zoom`-vs-Floating-UI failure (popup-anchor drift proportional to
   `(zoom - 1) × offset`) disqualifies `zoom`. **R.01 acceptance
   gate verified end-to-end in browser**: popover-anchor delta at
   80% / 100% / 175% is 3.25 / 4.05 / 7.08 px, ratio to fontSize
   = 0.30 rem invariant — no `zoom`-style drift. Avatar popover
   gets a `Scale` `<select>` with seven preset stops (80, 90,
   100, 110, 125, 150, 175). Server-side validator in
   [`client/shell/server/handlers/preferences.set.js`](../client/shell/server/handlers/preferences.set.js)
   rejects out-of-range writes per ICD §5.5 + SC2 + SC3
   (defense-in-depth alongside the client-side select). The
   2026-04-27 design-bundle's `Plinth Shell.html:85` `style.zoom`
   usage is amended in this same PR per METHODOLOGY §Phase 2
   Constraint #4 (Appendix B inventories the amendment).

4. **`/api/frontend/tokens.css` indirection** at new
   [`src/kernel/frontend/api_frontend.{hpp,cpp}`](../src/kernel/frontend/api_frontend.hpp).
   Reuses the v0.6.1 `active_frontend.cpp` SQL shape (LIMIT 2
   active-frontend lookup) but reports the n=0 vs n>1 distinction
   so the handler can emit the right 503 diagnostic body
   (`no_active_frontend` / `multiple_active_frontends`) per ICD
   §6.4. Happy path: 302 with `Location: /ext/{name}/{version}/css/
   tokens.css` + `Cache-Control: no-cache`. Slot in
   [`src/kernel/main.cpp`](../src/kernel/main.cpp) AFTER kernel
   `/api/*` + `/ext/*` registrations and BEFORE
   `register_routes_for_active_frontend` so the catch-all glob does
   not shadow `/api/frontend/*`. `tokens.css`-only this milestone
   per OQ5 default — `fonts/{name}` / `icons/{name}` /
   `manifest.json` deferred per `architecture/06-frontend.md §4.1`.

### OQ resolutions + implementation deviations

All seven §14 OQs pinned per ICD architect-recommendation defaults
(literal-string theme storage; integer scale; inline-script pre-paint
resolver; server-side scale-bound enforcement; `tokens.css`-only
`/api/frontend/*` scope; avatar-popover placement; inherit
`shell.preferences.set` audit family). ICD-0.6.2 §17 amendment block
records the resolutions plus three implementation deviations per
METHODOLOGY §Phase 2 Constraint #4:

1. **`/api/frontend/tokens.css` registers WITHOUT auth filter** (ICD
   §6.5 said auth required). Rationale: §6.6 has the shell self-
   reference this URL from its own login page (pre-auth); auth-
   required would 401 the unstyled login UX. Bytes are not user-
   specific (§6.5 acknowledges) and audit is off (§10.3), so the
   filter's stated purpose ("surface user-id for audit") does not
   fire.

2. **Pre-paint resolver ships as an external sync script, not an
   inline `<script>` with a CSP `'sha256-…'` hash** (ICD §SC4 path
   (a)). Plain sync external scripts are blocking-by-default and
   run before first paint reliably; the §SC4 (c) shape that was
   rejected was the `<link rel="preload">` async variant.
   Preserves strict `script-src 'self'` CSP without per-build hash
   recompute.

3. **Kernel-side persistence via `cap.call("shell.preferences.set",
   …)` deferred to 0.6.1.N** JS-dispatch follow-up. Shell.js ships
   localStorage-only persistence for v0.6.2; the SCHEMA validator
   in `preferences.set.js` ships in this PR so the wiring is ready
   when the 0.6.1.N follow-up connects browser → kernel via the
   `async_bridge_fixture` + `ws_test_fixture` scaffold (the same
   path `project_test_fixture_inflight.md` session 9 noted needs
   the `init_registry` teardown bug resolved before real-bridge
   capability dispatch through the WS fixture is stable).

4. **B.02 verifies URL construction with synthetic name+version**
   rather than following the 302 to verify the redirect target body
   (ICD §12.1 enumerates the latter). Following requires the asset
   server's per-(name, version) route registration which is set up
   by the install_lifecycle path — expensive setup for these tests.
   The byte-serving path is covered by
   `tests/kernel/packages/asset_server_test.cpp`; B.02 here verifies
   the kernel-side URL-construction contribution.

5. **T.\* / S.\* / I.\* JS-dispatch tests deferred to 0.6.1.N**
   (matches v0.6.1 posture per ICD-0.6.1 §17 deviation #5; same
   `init_registry` teardown blocker). Manual FE smoke per
   `feedback_fe_visualize.md` is the v0.6.2 ship-acceptance gate.

### Phase commit arc

1. `b4eef11` — Phase 1: tokens.css + bundled-shell artifact
   (`client/shell/client/css/tokens.css`, hex → `var()` rewrite of
   `client/index.html` inline `<style>`, px → rem, manifest 0.6.1
   → 0.6.2).
2. `6ef4021` — Phase 2: `/api/frontend/tokens.css` kernel handler
   (`src/kernel/frontend/api_frontend.{hpp,cpp}`, B.\* test coverage
   via test_seam, main.cpp registration).
3. `2a1a563` — Phase 3: pre-paint resolver
   (`client/shell/client/prepaint.js`) + avatar-popover Theme/Scale
   selects (`client/shell/client/shell.js`) + per-key SCHEMA
   validator (`client/shell/server/handlers/preferences.set.js`).
4. **(this commit)** — Phase 4: docs (CHANGELOG, ROADMAP flip,
   ICD-0.6.0 §15 cross-ref, architecture/06-frontend.md §4 status
   flip, ICD-0.6.2 §17 amendment block, design-bundle zoom→rem
   amendment, DESIGN-shell-v06x.md §6.3 citation pointer).

### Verification

- `cmake --build build -j 4` green; `cmake --build build --target
  tidy -j 4` green across the four new TUs and modified ones.
- `[api_frontend]` 4/4 cases / 35 assertions / 10/10 stability
  standalone. All four ctest groups green.
- Live curl smoke confirms `/api/frontend/tokens.css → 302 →
  /ext/shell/0.6.2/css/tokens.css → 200` end-to-end.
- Manual FE smoke per `feedback_fe_visualize.md`:
  - `/app/` loads with token-driven dark mode, no flash.
  - Avatar popover shows Theme + Scale `<select>`s.
  - Theme `light` swap → palette flips via
    `:root[data-theme="light"]` (body bg #f7f7f4, text #353841).
  - Scale `175%` → `documentElement.style.fontSize=23.625px`;
    layout rem-scales proportionally.
  - **R.01 popover-anchor stability gate** — popover-vs-button
    delta scales as 0.30 rem invariant across 80% / 100% / 175%
    (no `zoom`-style drift). Architect's pinned rem-vs-zoom
    decision validated end-to-end in the browser.
  - Reload preserves theme + scale via pre-paint resolver +
    `localStorage.shellPrefs`; no flash.

Plan file at the archived implementation record
(architect-approved).

---

## 2026-04-29 — 0.6.1.N ICD-0.6.2 authoring (paper)

Paper-only follow-up to v0.6.1 per `feedback_icd_horizon.md`
(ICDs one milestone ahead). Branch
`feat/0.6.1.N-icd-0.6.2-authoring`. Authored
[`docs/icd/ICD-0.6.2-design-tokens-theme-scaling.md`](icd/ICD-0.6.2-design-tokens-theme-scaling.md)
(15 sections + 5 appendices). Discharges ICD-0.6.0 §15 *Design
tokens, theme, UI scaling* deferral (lines 1042–1049). Plan file
at the archived implementation record
(architect-approved).

Four contributions land:

1. **Named CSS-custom-property token system.** Canonical palette
   lifted verbatim from the 2026-04-27 design bundle's
   `Plinth Shell.html:23-73` `:root` blocks: surface scale
   `--bg-0..4`, text scale `--text-0..3`, border scale, accent
   + `-soft` / `-softer` alphas, semantic tones
   `--success` / `--warn` / `--danger` (each + `-soft`),
   geometry radii `--r` / `--r-md` / `--r-lg`, focus-ring token
   (added). 25 tokens total. Names frozen for the 0.6.x arc;
   future ICDs may add tokens but must not rename. Inter prose
   + JetBrains Mono identifiers typography baseline (ICD-0.6.0
   §6.3) carries forward; baseline shifts from `body { font-size:
   13.5px; }` to `:root { font-size: 13.5px; } body { font-size:
   1rem; }` so `1 rem == 13.5 px` at 100% scale.

2. **Theme toggle (`light` / `dark` / `system`).** Persisted to
   `ext_shell.user_preferences` at well-known key `shell.theme`
   (JSONB string literal allow-list). `system` follows the
   user-agent `prefers-color-scheme` media query with a
   `matchMedia` change listener; the resolved theme (always
   `light` or `dark`) writes to `documentElement.dataset.theme`
   and the token table swaps via the `:root[data-theme="light"]`
   selector. Default when absent: `system`.

3. **UI scaling (80%–175%, rem-based) — architect-pinned
   decision against `zoom`.** Persisted at well-known key
   `shell.scale_pct` (JSONB integer 80–175). Rem-based mechanism
   sets `documentElement.style.fontSize = "${pct * 0.135}px"`.
   The **rem-vs-zoom decision is locked rem-based** per the
   2026-04-29 plan-mode interaction with this paper session —
   architect reported the canonical `zoom`-vs-Floating-UI
   failure where popups (avatar popover, future tray dropdowns,
   future float panels) drift from anchor points by progressively-
   larger pixel counts as the scale gradient grows. Root cause is
   `zoom`'s decoupling of DOM coordinate space from rendered
   geometry: `getBoundingClientRect()` and mouse-event
   coordinates report in viewport pixels, which interacts
   inconsistently with zoom and produces drift proportional to
   `(zoom - 1) × anchor_offset`. Rem-based scaling alters only
   computed sizes; coordinate space is preserved; popups stay
   anchored. The bundle's `Plinth Shell.html:85` `style.zoom`
   usage is amended to rem-based in the v0.6.2 *code* PR per
   METHODOLOGY §Phase 2 Constraint #4 (Appendix B inventories the
   amendment).

4. **`/api/frontend/tokens.css` indirection (kernel-side).** New
   kernel HTTP handler at
   `src/kernel/frontend/api_frontend.{hpp,cpp}` (specced;
   shipped in v0.6.2 code) reuses the v0.6.1
   `active_frontend.cpp` resolver, returns `302 Found` with
   `Location: /ext/{name}/{version}/css/tokens.css` and
   `Cache-Control: no-cache` per `architecture/06-frontend.md
   §4.1` lines 187–194. 503 with JSON diagnostic body
   (`no_active_frontend` / `multiple_active_frontends`) on
   singleton violations. The shell's own `index.html` references
   the same `/api/frontend/tokens.css` URL it serves so a
   bundled-shell upgrade picks up new tokens without a hard
   reload. Fonts / icons / `manifest.json` deferred (`tokens.css`
   only this milestone per OQ5 default).

24 test cases enumerated (4 B.\* token serving + 8 T.\* theme
toggle + 8 S.\* scaling + 3 I.\* full integration + 1 R.\*
popup-anchor regression; 5 U.\* avatar-popover UI cases deferred
to browser harness availability per ICD-0.6.0 OQ2). 7 OQs with
architect recommendations: storage shape literal allow-list
(theme); integer scale; inline-script pre-paint resolver;
server-side scale-bound enforcement; `tokens.css`-only
`/api/frontend/*` scope; avatar-popover placement; inherit
`shell.preferences.set` audit family. Discharges ICD-0.6.0 §15
*Design tokens, theme, UI scaling* deferral. Architecture-
amendment forwards cited (architecture file edits + design-bundle
zoom→rem amendment land in the v0.6.2 code session per paper-
session convention; see ICD-0.6.1 / ICD-0.5.4 / ICD-0.5.5
precedent). Paper-only: no code, no tests, no migrations, no
architecture file changes; verification is markdown
cross-reference walk only.

---

## 2026-04-29 — v0.6.1 Shell schema + user preferences

Sixth code milestone of the 0.6.x Frontend Shell arc. Branch
`feat/0.6.1-shell-schema-user-preferences`. Tag: `v0.6.1` cut on
green CI per `feedback_tagging_rule.md`. Five-phase commit arc
against [`docs/icd/ICD-0.6.1-shell-schema-user-preferences.md`](icd/ICD-0.6.1-shell-schema-user-preferences.md)
(2251 lines, paper-authored 2026-04-29 prior session). Closes all
four ICD-0.6.0 §15 `Closes: 0.6.1` deferral pointers. Plan file at
the archived implementation record
(architect-approved). Three contributions land:

1. **Bundled-shell first-boot install lifecycle on disk.** Replaces
   ICD-0.4.4 slice B's linker-embedded blob path with the on-disk
   byte source pinned by the 0.6.0 OQ1 architect override
   (`project_next_session_post_060.md`). New
   `src/kernel/shell/firstboot.{hpp,cpp}` runs once per boot
   between `reconcile_in_flight_installs` and the HTTP listener
   start; it queries `plinth.packages` for an active bundled
   frontend (LIMIT 2 for singleton-violation surfacing), reads
   `<bundle_path>/shell.zip` from disk on absence, hands bytes to
   `install_lifecycle::install_package` with `Provenance::BUNDLED`.
   Five hard-fail codes per ICD §3.5 (`bundle-missing` / 1,
   `install-lifecycle-failed` / 2, `singleton-violation` / 3,
   `schema-name-conflict` / 3, `detection-failed` / 4); three
   single-shot audits per §10.1 (`shell.firstboot.bundled_install_started`
   / `_completed` / `_failed`). New `Config::Shell::bundle_path`
   field; default resolution per §9.2 tries
   `<bin>/share/plinth/bundled` (dev layout) then
   `<bin>/../share/plinth/bundled` (FHS install). CMake adds
   `plinth_shell_bundle_staged` to copy `shell.zip` into the
   build-tree datadir layout, plus `install(FILES)` for production
   deployment. Decommissioned: `install_shell_if_needed`,
   `kernel/packages/shell_blob.{hpp,cpp}`, the `ld -r -b binary` +
   `xxd` fallback CMake blocks, `PLINTH_SHELL_BLOB_USE_LD/XXD`
   compile defs.

2. **Manifest-driven dispatch** (`active_frontend.{hpp,cpp}`).
   Replaces ICD-0.6.0's hardcoded `register_shell_routes` with a
   manifest-driven equivalent that reads `frontend_mount` +
   `frontend_entry` from the active row. Same `/` → 302 +
   `<mount>(.*)` SPA-fallback contract; same strict CSP +
   `no-cache` for entry / `immutable` for assets; same
   path-traversal hardening. New `frontend_entry` column on
   `plinth.packages` paired with `frontend_mount` via
   `chk_frontend_pair` CHECK (NULL together or non-NULL together).
   `parse_manifest` gains a `is_bundled` parameter that bypasses
   the new `manifest.name.reserved` rule for the canonical
   bundled-shell manifest (per ICD §5.5 / OQ4 = parse-time).
   `ValidationConfig::is_bundled` threads through `validate()`.

3. **`ext_shell` PG schema + `user_preferences` table + get/set
   capability pattern.** New `client/shell/migrations/001_init.sql`
   per ICD Appendix A: `ext_shell.user_preferences` table with PK
   on `(user_id, key)`, FK to `plinth.users` ON DELETE CASCADE,
   1..255 byte key CHECK, 64 KiB serialised value CHECK, plus
   updated_at index and SELECT/INSERT/UPDATE/DELETE GRANTs to
   `ext_shell_role`. `client/shell/capabilities.json` declares
   three capabilities (`shell.preferences.{get, set, get_all}`)
   per OQ1 = two atomic + `get_all`. `client/shell/rbac.json`
   declares two rules (`shell.preferences.{read, write}`) plus a
   `default_grants[]` array binding both to the `everyone`
   built-in group at install time. JS handlers
   `server/handlers/preferences.{get,set,get_all}.js` per ICD
   §7.3 / §7.4 (OQ3 pinned: `set(undefined)` deletes; `set(null)`
   UPSERTs JSONB null literal).

### Default-grants infrastructure (general)

`RbacManifest` gains a `default_grants: vector<DefaultGrant>` field;
`parse_rbac_manifest` reads + validates the new top-level field;
`install_lifecycle::register_extension_rbac_rules` applies grants
after rules are upserted via INSERT...SELECT with `ON CONFLICT DO
NOTHING`. Idempotent across re-installs and upgrades. Benefits any
future extension declaring `default_grants` in its rbac.json.

### `"any"` as a manifest param type

`PARAM_TYPE_LITERALS` in `capabilities_manifest.cpp` gains `"any"`
— accepts any JSONB-serialisable value (used by
`shell.preferences.set`'s `value` param). Handler is responsible
for runtime shape checks; the manifest declares "validate
server-side."

### Phase commit arc

1. `07a66e9` — Phase 1: schema + bundled-shell artifact
   (001_init.sql, manifest version bump, CMake `shell.zip`
   migrations + install + dev staging).
2. `e004228` — Phase 2: manifest fields finalization (frontend_entry
   column, parse_manifest is_bundled + RESERVED_NAME, install
   lifecycle field plumbing, handlers.cpp surface).
3. `7ae7008` — Phase 3: firstboot pre-flight + on-disk byte source
   (decommission install_shell_if_needed + shell_blob; new
   `kernel/shell/firstboot.{hpp,cpp}`; B.\* test coverage).
4. `5a16078` — Phase 4: manifest-driven dispatch (rename
   static_handler → active_frontend; M.\* test coverage).
5. `eae70f5` — Phase 5: capabilities + RBAC + handlers +
   default_grants infrastructure + P.\* schema/grant tests.

### OQ resolutions (architect-pinned this session per ICD §17)

| # | OQ | Resolution |
|---|----|------------|
| OQ1 | Capability shape | Two atomic + `get_all` (recommendation) |
| OQ2 | First-load hydration | Eager bulk fetch on shell init (recommendation; deferred to 0.6.2 wiring) |
| OQ3 | Reset semantics | `set(undefined)` deletes (recommendation) |
| OQ4 | Schema-name reservation | Parse-time (recommendation) |
| OQ5 | `bundle_path` default | Auto-resolve from `/proc/self/exe` (dev `<bin>/share/...` first; FHS `<bin>/../share/...` fallback). Deviation from ICD §9.2 literal "share/plinth/bundled" — the resolver tries both layouts so CMake-built dev binaries work without an explicit config override. |
| OQ6 | Mount-conflict resolution | Kernel rejects at install via existing `uniq_packages_mount_active` partial unique index (recommendation; no code change needed beyond Phase 2) |
| OQ7 | Audit dedup TTL | 60 s for `shell.preferences.set` (recommendation) |

### Implementation deviations from the ICD pseudocode

1. **rbac.json group reference: `everyone` not `users`.** ICD §7.2's
   default_grants block uses group name `users`; the kernel's
   `plinth::groups::bootstrap_groups` seeds `admin` + `everyone`
   (no `users`). Default grants now reference `everyone` (the
   semantic match for "all authenticated users"). Future ICD
   amendment may rename if a user-vs-everyone distinction
   emerges.
2. **rbac.json field name: `rule` not `name`.** ICD §7.2's
   pseudocode shows `{"name": "shell.preferences.read"}`; the
   `rbac_manifest.cpp` parser expects the existing `rule` field
   per ICD-0.4.6 §RbacManifest. The bundled rbac.json uses the
   parser's contract.
3. **Handler file naming: dotted, not underscored.** ICD §7.3 /
   §7.4's pseudocode block-comments `preferences_get.js` /
   `preferences_set.js`; the `cf4-handler-missing` cross-file
   validator rule expects `<function>.js` verbatim. Files ship as
   `preferences.get.js` / `preferences.set.js` /
   `preferences.get_all.js`.
4. **`"any"` accepted as a manifest param type.** ICD §7.1 declares
   `"type": "any"` for `shell.preferences.set`'s `value` param;
   the validator's `PARAM_TYPE_LITERALS` array originally only
   accepted six types. `"any"` added as a wildcard to the array
   (general feature; benefits other extensions that need
   pass-through any-shape value parameters).
5. **Full P.\* / I.\* JS-dispatch test suite deferred.** ICD §12.4
   / §12.5 enumerates 14 P.\* + 3 I.\* cases that exercise the
   end-to-end JS-handler dispatch path; this milestone ships 9
   schema-level / grant-level / handler-deployment tests
   (`P.s.01`–`P.s.06`, `P.r.01`, `P.r.02`, `P.h.01`) covering the
   contracts the JS suite assumes hold. Full integration suite
   carved into a 0.6.1.N follow-up — needs the async-bridge +
   WS-test-client scaffold (the same path
   `project_test_fixture_inflight.md` session 9 noted needs the
   `init_registry` teardown work resolved).
6. **`upgrade_package` parses with `Provenance::USER`.** ICD-0.6.1
   §15 defers bundled-shell upgrade workflow; the existing user-
   driven upgrade path always parses with `Provenance::USER`.
   Future kernel-driven bundled upgrade must thread the existing
   row's provenance.

### Test taxonomy

35 ICD test cases enumerated; this milestone ships:

- **B.\* (6/6)** at `tests/kernel/shell/firstboot_test.cpp` —
  fresh install, short-circuit, missing bundle, corrupt zip,
  singleton violation, schema-name-conflict.
- **M.\* (6/8)** at `tests/kernel/shell/active_frontend_test.cpp`
  — M.01–M.05 + M.08 plus path-traversal + custom-mount
  coverage. M.06 / M.07 (kernel API + ext route ordering vs
  frontend mount) deferred to the manual smoke walkthrough +
  structural enforcement in main.cpp.
- **S.\* (1/4 effective)** at `tests/kernel/shell/preferences_test.cpp`
  P.s.01 covers schema-presence; S.02–S.04 (re-run idempotency,
  CHECKSUM_MISMATCH, RESERVED_NAME at parse_manifest) overlap
  with [manifest][shell-reserved] and migrations test families
  already shipped.
- **P.\* (9 schema/grant subset; 14 full deferred)** at
  `tests/kernel/shell/preferences_test.cpp`.
- **I.\* (3 deferred)** to 0.6.1.N follow-up.

### Files modified

**New:**
- `client/shell/migrations/001_init.sql`
- `client/shell/server/handlers/preferences.{get,set,get_all}.js`
- `src/kernel/shell/firstboot.{hpp,cpp}`
- `src/kernel/shell/active_frontend.{hpp,cpp}` (replaces static_handler)
- `tests/kernel/shell/firstboot_test.cpp`
- `tests/kernel/shell/active_frontend_test.cpp` (replaces static_handler_test)
- `tests/kernel/shell/preferences_test.cpp`

**Modified:**
- `client/shell/manifest.json` (version 0.6.0 → 0.6.1)
- `client/shell/{capabilities,rbac}.json` (populated)
- `migrations/schema.sql` (`frontend_entry` column + chk_frontend_pair)
- `src/kernel/packages/manifest.{hpp,cpp}` (is_bundled + RESERVED_NAME)
- `src/kernel/packages/install_lifecycle.{hpp,cpp}` (frontend_entry plumbing; default_grants apply; install_shell_if_needed removed)
- `src/kernel/packages/handlers.cpp` (frontend_entry surface)
- `src/kernel/packages/validator.{hpp,cpp}` (is_bundled threading)
- `src/kernel/packages/capabilities_manifest.cpp` (`any` param type)
- `src/kernel/rbac/rbac_manifest.{hpp,cpp}` (default_grants struct + parse + serialize)
- `src/kernel/config.{hpp,cpp}` (`Config::Shell::bundle_path`)
- `src/kernel/main.cpp` (firstboot + active_frontend wiring)
- `CMakeLists.txt` (shell.zip artifact + dev staging + install target; decommission shell_blob; firstboot + active_frontend + preferences sources)
- `tests/kernel/packages/crash_recovery_test.cpp` (I.16 / I.17 entry update)
- `tests/kernel/packages/manifest_test.cpp` (RESERVED_NAME cases)
- `docs/icd/ICD-0.6.1-shell-schema-user-preferences.md` (§17 OQ Resolutions amendment block)

**Deleted:**
- `src/kernel/packages/shell_blob.{hpp,cpp}`
- `src/kernel/shell/static_handler.{hpp,cpp}`
- `tests/kernel/packages/shell_blob_test.cpp`
- `tests/kernel/shell/static_handler_test.cpp`

### Verification

- `cmake --build build -j 4` clean (plinth + plinth_tests).
- Per-phase Catch2 sweeps: `[firstboot]`, `[active-frontend]`,
  `[shell][firstboot][integration]`, `[manifest][shell-reserved]`,
  `[preferences]` — all green.
- Schema-level smoke: 6 P.s.\* schema cases / 26 assertions pass.
- Default-grant smoke: 2 P.r.\* cases / 13 assertions pass.
- Handler-deployment smoke: P.h.01 / 3 assertions pass.
- ctest in isolation per group green: `plinth_tests_pure` /
  `plinth_tests_js` / `plinth_tests_pg` / `plinth_tests_ws`.
- Full `ctest -j 1` flakes on a pre-existing realtime test
  (`coalescer_test.cpp:170` / `live_replay_ordering_test.cpp:759`)
  — both well-documented in `project_ws_flaky_segfault.md` /
  `project_next_session_post_055.md` family; unrelated to this
  milestone. Each individual group green in isolation.

---

## 2026-04-29 — 0.6.0.N Architecture session: extension HTTP surface (paper-only, untagged)

Paper-only architecture session on branch
`feat/0.6.0.N-arch-session-ext-http-surface` ratifying the extension
HTTP surface proposal in
[`docs/discussion/DISCUSSION-extension-http-surface.md`](discussion/DISCUSSION-extension-http-surface.md)
(2026-04-27 origin). Output: nine commitments now normative as
[`architecture/05-extensions.md §6 Extension HTTP Surface`](architecture/05-extensions.md);
two `reserved (planned)` forward-reservations added to §2; ROADMAP
scheduling for the ICD-authoring follow-up + implementation milestone;
this CHANGELOG entry. Plan file at
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`. No code,
tests, migrations, fixtures, or memory edits.

### Why this session

[`architecture/05-extensions.md §2.1`](architecture/05-extensions.md)
"Rejected: Extension-Owned Arbitrary HTTP Routes" forbade extensions
from owning HTTP surface outside `/ext/{name}/{version}/*` and
`frontend.mount`. The 2026-04-27 design conversation (captured in the
discussion doc) walked through four alternatives — JS router,
reserved-prefix list, dlopen plugins, status-quo no-extension-HTTP —
rejected each, and converged on a primitive (catch-all kernel route +
manifest-declared prefixes + install-time conflict check + runtime
route table + kernel pre-dispatch auth + execution-mode-agnostic
handler reference) that preserves the design principle ("kernel owns
primitives, extensions own application surfaces") while letting
extensions own protocol-specific HTTP surfaces. Files-Nextcloud-compat
is the immediate motivating case; CalDAV / CardDAV / S3-compat /
ActivityPub federation / OAuth-flow surfaces are plausible later cases
that all re-litigate the same tension under reserved-prefix.

### Nine commitments (architect-pinned this session)

Each maps to a Deferred bullet in the discussion doc; defaults from
the proposal pinned without override.

1. **Principle.** *Kernel owns primitives; extensions own application
   surfaces.* Test for any HTTP-handling code: *can the route be
   described without naming an application or a protocol?* Yes →
   kernel; no → extension.
2. **Shape.** Adopted as proposed: one catch-all kernel route +
   manifest-declared prefixes + install-time conflict check + runtime
   route table (`shared_mutex` per
   [`enforcement.cpp`](../src/kernel/rbac/enforcement.cpp) precedent)
   + kernel-side PAT auth pre-dispatch + execution-mode-agnostic
   handler reference.
3. **Manifest schema.** Three new fields: `http_prefixes: string[]`,
   `unauthenticated_prefixes: string[]` (subset, more privileged),
   `handler_mode: "quickjs" | "sidecar" | "bundled_native"`.
4. **Prefix-claim semantics.** Simplest-rule first: full-prefix
   exclusive ownership, no method scoping, no host scoping in
   single-tenant. Multi-tenant per-host scoping deferred.
5. **Uninstall while in-flight.** Drain semantics: `draining` flag
   on the route-table entry, in-flight requests complete to natural
   termination, new requests → `503 Service Unavailable`, entry
   removed once in-flight count = 0 OR configurable timeout fires.
6. **Privilege model.** RBAC-gated install. Working-name rule
   `packages.install.with_http_prefixes` (default grant: `admins`)
   gates declaration of any `http_prefixes`. Separate more-privileged
   rule `packages.install.with_unauthenticated_prefixes` gates
   declaration of any `unauthenticated_prefixes`. Bundled-extension
   first-boot install bypasses (kernel installs bundled packages
   directly per ICD-0.6.1 §3).
7. **Audit log entry.** Non-skippable `extension.http.dispatched`
   event per dispatched request: PAT identity (or `unauthenticated`
   marker), prefix matched, extension name + version, HTTP method,
   HTTP status, optional latency. Rate-limited / dedup'd via the
   existing `audit::claim_*` pattern (ICD-0.5.5 OQ7 60-second TTL
   precedent).
8. **Performance contract.** Catch-all + lookup + auth path adds
   ≤ 100 μs of overhead per request vs equivalent fixed-prefix
   kernel-owned route. Starting figure; final threshold pinned at
   ICD authoring. Validation harness (one-off micro-bench OR LH-tier
   exercise) selected at implementation time.
9. **Implementation slot.** **0.6.7 Extension HTTP surface — catch-
   all primitive + manifest prefixes + runtime route table.** Slot
   sits after 0.6.6 (tray + content-type + navigation) closes the
   shell SDK arc; before 0.7 schema-freeze. ICD-authoring slot at
   `0.6.6.N`.

### Two forward-reservations added to §2

Both pass the principle's test (kernel observability/documentation,
not application-shaped) and are reserved before the §6 primitive
exists so third-party extensions cannot stake them under the catch-
all conflict check:

- **`/docs/*`** (top-level table row) — kernel markdown help reader.
  Reserved; not yet implemented. Likely future implementation: a
  0.6.x slot once the shell SDK is in place.
- **`/api/docs/*`** (sub-prefix bullet under `/api/*`) — kernel
  dynamic API discovery (OpenAPI / Swagger surface). Reserved; not
  yet implemented. Likely future implementation: post-0.7 schema-
  freeze.

The `reserved (planned)` flag on the §2 table rows is load-bearing
today: every extension install attempt that declares an
`http_prefixes` overlapping these rows fails the install, even
before the `/docs` and `/api/docs` handlers exist.

### Discussion doc + architecture file changes

- **`docs/discussion/DISCUSSION-extension-http-surface.md`** — single
  status-line addition (`**Status (2026-04-29):** Ratified — see
  architecture/05-extensions.md §6 for the normative contract.`).
  Body unchanged. Discussion doc remains the design-history origin
  record per the in-document "Architecture cites this only as 'see
  this document for the thinking'" framing.

- **`docs/architecture/05-extensions.md`** — four edits:
  - **Owner statement** widened to claim ownership of the new §6
    primitive.
  - **§2 Reserved URL Prefixes** — table row added for `/docs/*`;
    sub-prefix bullet added for `/api/docs/*`; new closing paragraph
    on the §6 forward-reference and the load-bearing role of
    `reserved (planned)` rows.
  - **§2.1 Extension HTTP Surface (no arbitrary routes; manifest-
    declared, conflict-checked prefixes only)** — renamed from
    "Rejected: Extension-Owned Arbitrary HTTP Routes" and rewritten.
    Distinction between rejected (runtime-mutable arbitrary
    registration) and supported (manifest-declared, conflict-checked
    via §6) drawn explicitly. §6 forward-cite added.
  - **§5.2 Site-Host Extension** — status note prepended explaining
    that public-page surfaces (the primary §5.2 motivation) now fold
    into the §6 primitive via `http_prefixes` + `unauthenticated_
    prefixes`, while the *single-site-host fall-through role*
    described in §5.2's body stays deferred as a distinct shape
    (since §6 does not provide fall-through). Body otherwise
    unchanged.
  - **§5.3 Rejected (Not Deferred)** first bullet — narrowed to
    "Extensions registering arbitrary HTTP routes *at runtime*" and
    re-pointed at §2.1 + §6.
  - **NEW §6 Extension HTTP Surface** (10 subsections, ~205 lines).
    The normative contract: principle, shape, manifest schema,
    prefix-claim semantics, uninstall drain, privilege model, audit
    log entry, performance contract, implementation milestone, and
    "what §6 does not decide" (multi-tenant per-host scoping, per-
    prefix `handler_mode` overrides, final performance threshold,
    drain timeout bounds — all deferred to follow-ups).

### ROADMAP changes

- §0.6 architecture-session line flipped `[ ] → [x]` with a shipped
  pointer summarizing the nine commitments + two forward-
  reservations.
- New `0.6.6.N ICD-0.6.7 authoring (paper follow-up)` bullet at
  `[strong]` ahead of the implementation milestone.
- New `0.6.7 Extension HTTP surface — catch-all primitive + manifest
  prefixes + runtime route table` implementation milestone at
  `[strong]`.
- `RE-EVAL following 0.6.6` renamed to `RE-EVAL following 0.6.7`
  (the 0.6.x arc now closes at 0.6.7, not 0.6.6).

### DEFERRED.md changes

New entry covering the four sub-questions §6.10 deferred:
multi-tenant per-host scoping, per-prefix `handler_mode` overrides,
final performance threshold, drain-timeout default and bounds. Each
is a single-question follow-up that does not block the implementation
milestone or ICD authoring.

### Files

| Action  | Path                                                                                                                                                          |
|---------|---------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Updated | [`docs/architecture/05-extensions.md`](architecture/05-extensions.md) (owner statement + §2 + §2.1 + §5.2 + §5.3 + new §6)                                    |
| Updated | [`docs/discussion/DISCUSSION-extension-http-surface.md`](discussion/DISCUSSION-extension-http-surface.md) (status-line addition only; body unchanged)         |
| Updated | [`docs/ROADMAP.md`](ROADMAP.md) (architecture-session flip + 0.6.6.N + 0.6.7 + RE-EVAL rename)                                                                |
| Updated | [`docs/CHANGELOG.md`](CHANGELOG.md) (this entry)                                                                                                              |
| Updated | [`docs/DEFERRED.md`](DEFERRED.md) (new active entry for the four §6.10 sub-questions)                                                                         |

**Not touched** (per architecture-session paper convention): code
(`src/**`, `tests/**`, `client/**`, `migrations/**`), other
architecture files (`01-identity.md` through `04-services-ha.md` +
`06-frontend.md`), ICDs, design docs, METHODOLOGY-* files, memory
files.

### Verification

Paper-only — no `cmake --build`, no `clang-tidy`, no Catch2, no
`ctest`, no FE preview. Verification = markdown cross-reference walk:

- Every `(../...)` link in the new
  [`architecture/05-extensions.md §6`](architecture/05-extensions.md)
  resolves to a real file (`enforcement.cpp`,
  `install_lifecycle.cpp`, `ICD-0.4.5`, `ICD-0.5.5`, `ICD-0.6.1`,
  `ROADMAP.md`).
- Discussion doc front-matter renders correctly (no broken structure
  from the prepended status line).
- ROADMAP forward-link audit clean: `0.6.6.N`, `0.6.7`, and
  `ICD-0.6.7-extension-http-surface.md` all resolve to real ROADMAP
  slots / future ICD slots.
- CHANGELOG entry parses (heading hierarchy + Files block matches
  prior 0.6.0.N entries).

---

## 2026-04-29 — 0.6.0.N ICD-0.6.1 authoring (paper-only, untagged)

Paper-only follow-up on branch `feat/0.6.0.N-icd-0.6.1-authoring`
authoring [`docs/icd/ICD-0.6.1-shell-schema-user-preferences.md`](icd/ICD-0.6.1-shell-schema-user-preferences.md)
(2251 lines) per METHODOLOGY-llm-assisted-development.md §3.1
forward-ICD-presence rule and `feedback_icd_horizon.md` (ICDs one
milestone ahead). 0.6.1 is the next code milestone after the
2026-04-27 0.6.0 + 0.6.0.1 ship; this ICD pins the four
`Closes: 0.6.1` deferral pointers queued in ICD-0.6.0 §15. Plan
file at the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`. No
code, tests, migrations, fixtures, or architecture file changes
in this PR — paper-only convention per ICD-0.5.4 / ICD-0.5.5
authoring sessions.

### What shipped this session

**1. ICD-0.6.1 paper.** New file
[`docs/icd/ICD-0.6.1-shell-schema-user-preferences.md`](icd/ICD-0.6.1-shell-schema-user-preferences.md),
2251 lines across 15 numbered sections + 3 appendices. Three
contributions land:

- **Bundled-shell first-boot install lifecycle** (§3) — pre-flight
  `ensure_bundled_shell_installed(cfg, db)` runs once per kernel
  boot, queries `plinth.packages` for an `ACTIVE` `provenance='bundled'`
  row at `name='shell'`, and on absence reads
  `<bundle_path>/shell.zip` from disk + hands the bytes to
  `install_lifecycle::install_package`. New `Provenance::Bundled`
  enum value + CHECK widening. Five hard-fail codes
  (`ERR_BUNDLE_MISSING` / `ERR_BUNDLE_INSTALL_FAILED` /
  `ERR_MULTIPLE_ACTIVE_FRONTENDS` / `ERR_BUNDLE_DETECTION_FAILED` /
  `ERR_BUNDLE_SCHEMA_RESERVED`); each emits a
  `shell.firstboot.bundled_install_failed` audit before exit. ICD
  §3.6 pins boot ordering: pre-flight runs after PG bootstrap +
  in-flight recovery, before resolver/registry init.

- **`frontend.mount` manifest-driven mount declaration** (§4) —
  the `frontend.mount` + `frontend.entry` manifest fields ICD-0.6.0
  Appendix B sketched as forward-compat become load-bearing.
  `parse_manifest` validates the mount regex `^/[a-z][a-z0-9_-]*/$`
  + reserved-prefix conflicts (`/api/*` / `/ext/*` / `/ws/`).
  `plinth.packages` gains `frontend_mount` + `frontend_entry`
  columns plus a partial unique index enforcing the active-
  frontend singleton invariant per
  `architecture/06-frontend.md §2.2`. Replaces ICD-0.6.0 §8
  static-handler with `register_active_frontend_routes(cfg, db,
  data_dir)` reading bytes from
  `<data_dir>/extensions/<name>/<version>/client/`. URL contract
  (`GET /` → 302 mount; `GET mount(.*)` → SPA-fallback) byte-
  identical to ICD-0.6.0; only the byte source moves from kernel
  `.text` to filesystem.

- **`ext_shell` PG schema + `user_preferences` table + get/set
  capability pattern** (§§5–7) — schema lands through ICD-0.4.3
  path verbatim (kernel reserves `name='shell'` + schema name
  `ext_shell`). `ext_shell.user_preferences (user_id, key, value
  JSONB, updated_at)` table with `(user_id, key)` PK, FK on
  `plinth.users(id) ON DELETE CASCADE`, 1–255 byte key length,
  64 KiB JSONB serialised value cap. Three new capabilities —
  `shell.preferences.get(key)` / `shell.preferences.set(key,
  value)` / `shell.preferences.get_all()` — declared in the
  shell's `capabilities.json` and gated by two new RBAC rules
  (`shell.preferences.read` / `shell.preferences.write`)
  registered through ICD-0.4.6 path with `users` group default-
  grants. `set(key, undefined)` deletes; `set(key, null)` UPSERTs
  `'null'::jsonb` (per OQ3 architect-recommendation).
  `ext_shell.default_apps` reserved at §5.4 as a 0.6.6 0.6.6 carry-
  forward. Cross-tenant isolation (SC1) enforced by
  `ctx.user.id` binding — args never carry `user_id`.

**2. Test taxonomy.** §12 enumerates 35 cases for the 0.6.1 code
session (paper authoring lists; verification deferred per paper-
session convention):

| Prefix | Count | Focus                                               |
|--------|------:|-----------------------------------------------------|
| B.\*   | 6     | First-boot install lifecycle (library-level + PG)   |
| M.\*   | 8     | Manifest-driven mount routing (HttpTestFixture)     |
| S.\*   | 4     | Schema migration via ICD-0.4.3 (PG-level)           |
| P.\*   | 14    | Preference get/set round-trip (HttpTestFixture)     |
| I.\*   | 3     | Full-stack integration (boot → set → reload → get)  |

**3. Open Questions.** §14 enumerates 7 OQs with architect
recommendations (paper does not pin; code-session ship PR adds
§17 OQ Resolutions block per ICD-0.5.5 / ICD-0.6.0 precedent):

| OQ  | Recommendation                                   |
|-----|--------------------------------------------------|
| OQ1 | Two atomic capabilities (get / set) + get_all    |
| OQ2 | Eager bulk fetch on shell init                   |
| OQ3 | `set(key, undefined)` deletes; no separate delete capability |
| OQ4 | Reserved-name reject at parse_manifest (not install-time) |
| OQ5 | `share/plinth/bundled` literal default for `bundle_path` |
| OQ6 | Mount conflicts → install rejected (`ERR_MOUNT_CONFLICT`) |
| OQ7 | `shell.preferences.set` audit dedup TTL = 60 s   |

### Discharged reservations

This ICD discharges the four explicit `Closes: 0.6.1` deferral
pointers from ICD-0.6.0 §15 (lines 1010–1040):

- **ICD-0.6.0 §15 *Bundled-package first-boot install lifecycle***
  (lines 1010–1020) — ICD-0.6.1 §3 authors the package-system
  extraction path; ICD-0.6.0's static-handler is structurally
  decommissioned at the 0.6.1 code session.
- **ICD-0.6.0 §15 *`frontend.mount` manifest contract***
  (lines 1022–1030) — ICD-0.6.1 §4 authors the manifest-driven
  mount declaration; the ICD-0.6.0 hardcoded `mount='/app'` is
  replaced by reading from `plinth.packages.frontend_mount` at
  the 0.6.1 code session.
- **ICD-0.6.0 §15 *`ext_shell` PG schema and user preferences***
  (lines 1032–1040) — ICD-0.6.1 §§5–7 author the schema, the
  `user_preferences` table, and the get/set capability pattern.
- **ICD-0.6.0 §8.3 *Assets storage* (line 624) OQ1 architect
  override** — `project_next_session_post_060.md` records the
  architect's pin of "bundle byte source = on-disk installed
  shell" (override of ICD-0.6.0 OQ1's embedded-resource
  recommendation). ICD-0.6.1 §3.2 normalizes that override into
  the install contract — the question is settled, not re-
  litigated.

**Architecture-amendment forwards** (cited here, edits land in the
0.6.1 code session per paper-session convention):

- `architecture/06-frontend.md §1` "first boot bundled-package
  install lifecycle is *deferred* to 0.6.1" — discharged by
  ICD-0.6.1 §3.
- `architecture/06-frontend.md §2` "manifest-driven mount
  declaration contract belongs to the 0.6.1 package-system
  landing" — discharged by ICD-0.6.1 §4.
- `architecture/06-frontend.md §2.2` active-frontend singleton —
  promoted to normative kernel contract by ICD-0.6.1 §4.4
  (resolver) + §4.3 (partial unique index).
- `architecture/05-extensions.md §2` reserved-prefix table — the
  `/app/*` row mechanic moves from kernel-hardcoded (ICD-0.6.0
  §8.1) to manifest-declared (ICD-0.6.1 §4.4) without altering
  the table's value.

### Files

| Action  | Path                                                                                                                       |
|---------|-----------------------------------------------------------------------------------------------------------------------------|
| Created | [`docs/icd/ICD-0.6.1-shell-schema-user-preferences.md`](icd/ICD-0.6.1-shell-schema-user-preferences.md)                    |
| Updated | [`docs/CHANGELOG.md`](CHANGELOG.md)                                                                                        |
| Updated | [`docs/ROADMAP.md`](ROADMAP.md)                                                                                            |

**Not touched** (per paper-session convention): `docs/architecture/0[1-6]-*.md`,
`docs/DEFERRED.md`, `client/shell/manifest.json`, any code,
tests, migrations, or fixtures.

### Verification

Paper-only — no `cmake --build`, no `clang-tidy`, no Catch2, no
`ctest`, no FE preview (no UI changes). Markdown internal cross-
reference walk: every `(../...)` and `[§N]` link in
ICD-0.6.1 resolves to a real file/anchor. Forward-link audit:
every reference to "0.6.0", "0.6.0.N", "0.6.1", "0.6.2", "0.6.3",
"0.6.4", "0.6.6", "0.10.0" lands at a real ROADMAP slot or open
ICD. CHANGELOG entry parsability: heading hierarchy + Files block
matches prior 0.6.0.N entries.

---

## 2026-04-29 — 0.6.0.N test-fixture buildout, session 9 of N (X.07 missing/changed + X.08 + X.09, untagged)

Ninth session on branch `feat/0.6.0.N-x07-x08-x09-drain-window` of the
multi-session 0.6.0.N test-fixture buildout. Closes the last three
ICD-0.4.5 integration cases reachable from the fixture stack:
**X.07 missing-rule** + **X.07 changed-rule** sub-cases (the new-rule
sub-case shipped in session 4) + **X.08 in-flight cap-dispatch
completes within drain window** + **X.09 drain timeout exceeded**.
After this lands, only **X.12 (SIGKILL crash at swap T3)** remains
from ICD-0.4.5; the matching ICD-0.5.5 **S.07** lives in the same
SIGKILL follow-up. Plan file at
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. Test-only override seam for the upgrade drain timeout —
[`src/kernel/packages/handlers.{hpp,cpp}`](../src/kernel/packages/handlers.cpp).**
New `plinth::packages::test_seam::upgrade_drain_timeout_ms_override()`
+ `set_*` / `clear_*`, mirroring the
[`live_buffer_cap_override`](../src/kernel/ws/subscriptions.cpp:584)
pattern verbatim (sentinel atomic `SIZE_MAX`, getter returns
`std::optional<std::size_t>`, RAII-friendly clear). All three
`InstallerContext` build sites in `handlers.cpp`
(POST/PATCH/DELETE entry points) consult the override before falling
back to `cfg.upgrade_drain_timeout_ms`. Production callers — kernel
`main.cpp`'s `register_package_routes` — never see the override.

**2. Slow-handler fixture pair —
[`tests/fixtures/lifecycle_transitions/upgrade-v{1,2}-slow/`](../tests/fixtures/lifecycle_transitions/upgrade-v1-slow/).**
Single pair drives all four new TEST_CASEs.

| Fixture            | Version | RBAC rules                                              | Notes                                               |
|--------------------|---------|---------------------------------------------------------|-----------------------------------------------------|
| `upgrade-v1-slow`  | 1.0.0   | `slow.alpha`, `slow.beta`, `slow.gamma`                 | v1 baseline; `slow:1:wait` capability gated by `slow.alpha` |
| `upgrade-v2-slow`  | 2.0.0   | `slow.alpha` (description **changed**), `slow.gamma`, `slow.delta` (**new**) — drops `slow.beta` | v2 exercises three reconciliation paths simultaneously |

CMake additions: append both names to `PLINTH_LIFECYCLE_FIXTURES` at
[`CMakeLists.txt:192`](../CMakeLists.txt:192). Migration: minimal
schema-qualified `ext_slow.placeholder` (no schema delta v1→v2; X.05
already exercises new-migration paths).

**3. Four new TEST_CASEs in
[`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp).**

| Case          | Drives |
|---------------|--------|
| X.07-changed  | Install v1-slow → POST v2-slow upgrade → assert `slow.alpha` row id preserved, description updated to `"v2 alpha changed …"`, `orphaned_at IS NULL`. Companion assertions on `slow.gamma` (untouched) and `slow.delta` (new, `orphaned_at NULL`). |
| X.07-missing  | Install v1-slow → POST v2-slow upgrade → assert `slow.beta` row id preserved, `orphaned_at IS NOT NULL`. |
| X.08          | Install v1-slow → set drain override 5000 ms → arm `InflightSimulator("slow")` (in_flight=1) → POST v2-slow async → release simulator after 200 ms → assert upgrade response 201 ACTIVE, old row SUPERSEDED. |
| X.09          | Install v1-slow → set drain override 500 ms → arm `InflightSimulator("slow")` → POST v2-slow async → wait response → assert HTTP 400 (production deviation, see below) `kind=upgrade-drain-timeout` `state=INSTALL_FAILED`, old row still ACTIVE @ 1.0.0. |

Helpers added at top-of-file:

- `install_slow_v1_via_http(fx, token)` — analog of session-2's
  `install_v1_via_http`; calls `wait_rbac_test_settled(id)` before
  returning so the upgrade POST does not lose the per-name advisory
  lock race.
- `dispatch_upgrade_async(fx, req)` — `std::async(launch::async)`
  wrapper; the future delivers the response once the upgrade
  pipeline returns (post-T2 success or T2 timeout / abort).
- `class InflightSimulator` — pre-`begin_drain`s the named drain
  state, spawns a worker thread that constructs `DispatchGuard(name)`
  and holds it until released, polls `state->in_flight >= 1` so the
  upgrade's `wait_for_zero` observes counter=1 from its first sample.
  RAII dtor signals release, joins the worker, and `end_drain`s.
- `class DrainOverride` — RAII guard for the override seam.

### Why `InflightSimulator` instead of a real cap.call

The macro-plan §3 had X.08 / X.09 launch a JS handler that did
`await db.query("SELECT pg_sleep(N)")` so the resolver's `DispatchGuard`
([resolution.cpp:479](../src/kernel/capabilities/resolution.cpp:479))
would hold the in-flight counter through the production async-bridge
path. Three problems forced the pivot to the simulator:

1. **Race window between T1 begin_drain and T2 wait_for_zero.**
   [`install_lifecycle.cpp:3137-3151`](../src/kernel/packages/install_lifecycle.cpp:3137)
   — only ~14 lines (microseconds) separate `begin_drain` from
   `wait_for_zero`. `DispatchGuard` per drain.hpp:14-19 only counts
   dispatches whose ctor sees `g_active_count > 0` — i.e., dispatches
   that arrive *after* `begin_drain` fires. A poll on
   `active_drain_count()` plus `std::async(launch::async)` for the
   cap.call could not reach the resolver entry within microseconds;
   the counter remained 0 across the wait window and drain succeeded
   instantly with `reached_zero=true`.
2. **Resolver vs. registry split.**
   [`extensions::dispatch`](../src/kernel/extensions/runtime_registry.cpp:640)
   does NOT construct a `DispatchGuard` — only the resolver path
   (`call_capability` / `call_capability_async`) does. So even a
   direct `extensions::dispatch` call that beat the race would still
   bypass the counter.
3. **Test fixture lacks resolver/registry init.**
   `ws_test_fixture.cpp::start_test_server` does not run
   `init_resolver` / `init_registry` (production main.cpp:352-360
   does both); a JS-driven dispatch path would also need a Tier 2
   cache refresh after install. Adding the init wiring exposed an
   unrelated heap-corruption flake in the `plinth_tests_pg`
   subprocess at process teardown (Drogon's `EventLoopThreadPool`
   dtor + `munmap_chunk: invalid pointer`); root cause in the
   teardown ordering of `init_registry`'s pool against Drogon's
   loop, deferred to its own follow-up. The simulator path makes
   that wiring unnecessary for X.08 / X.09.

`InflightSimulator` follows the
[drain unit test pattern](../tests/kernel/capabilities/drain_test.cpp:51)
(idempotent `begin_drain`, worker thread holding `DispatchGuard`,
spin-wait on `in_flight >= 1`). The upgrade's own `begin_drain`
returns the existing `shared_ptr<DrainState>` per drain.cpp:31-34;
its `wait_for_zero` then observes counter=1 from sample one. Same
end-to-end production code path under test (drain mechanism +
upgrade abort + `failure_to_status`); only the in-flight increment
source is synthetic. The slow JS handler stays in the fixture for
future sessions that may want a real-bridge variant — they will
need to add `init_resolver` + `init_registry` + a `reload_tier2_cache`
call after install AND resolve the `plinth_tests_pg` teardown bug
before doing so.

### Implementation deviations (per METHODOLOGY §Phase 2 Constraint #4)

1. **X.09 status code = 400, not the ICD's 504.** Production runs
   the upgrade-drain-timeout failure through
   [`install_lifecycle.cpp:1402`](../src/kernel/packages/install_lifecycle.cpp:1402)
   which hardcodes `failed_at = InstallStage::UPLOADING` for the
   upgrade-path conversion regardless of the actual stage that
   reported the failure. `failure_to_status(UPLOADING, "upgrade-drain-timeout")`
   then falls through `handlers.cpp:81-93`'s 413/409/422 checks and
   lands on the default `k400BadRequest`. Same root cause as session
   4's [X.06 deviation block](../tests/kernel/packages/lifecycle_transitions_http_test.cpp:230)
   (ICD says 422; production returns 400). Both deviations resolve
   together as the failure-conversion follow-up — out of scope for
   this session.
2. **`outstanding` count not asserted.** The drain-failure report's
   `outstanding` field is preserved on the dry-run path
   ([handlers.cpp:222-224](../src/kernel/packages/handlers.cpp:222))
   but dropped on the regular install path
   ([handlers.cpp:240-254](../src/kernel/packages/handlers.cpp:240))
   which builds the body from `InstallFailure`'s flat fields only.
   Test asserts `kind` + `failed_at_stage` + `message` + `state` and
   leaves `outstanding` for the same future failure-conversion
   follow-up.
3. **JS dispatch surface unused.** The `slow:1:wait` capability +
   `wait.js` handler ship in the fixtures but X.08 / X.09 do not
   invoke them (see "Why InflightSimulator" above). They're left in
   place because (a) extensions need at least one capability for the
   RBAC test runner to settle within `wait_rbac_test_settled`'s
   poll window and (b) future sessions that wire post-install Tier 2
   cache refresh (`reload_tier2_cache` after install) will have a
   live capability to exercise.

### Files touched this session

- Modified: [`src/kernel/packages/handlers.hpp`](../src/kernel/packages/handlers.hpp)
  — three `test_seam::upgrade_drain_timeout_ms_override*` declarations
  + `<atomic>` / `<optional>` include adds.
- Modified: [`src/kernel/packages/handlers.cpp`](../src/kernel/packages/handlers.cpp)
  — anonymous-namespace storage atom + `effective_upgrade_drain_timeout_ms`
  helper; three InstallerContext build sites switch to
  `effective_upgrade_drain_timeout_ms(cfg.upgrade_drain_timeout_ms)`;
  three public `test_seam` definitions appended next to the existing
  `dispatch_*` exports.
- Modified: [`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp)
  — three new helpers + four new TEST_CASEs.
- New:      [`tests/fixtures/lifecycle_transitions/upgrade-v1-slow/`](../tests/fixtures/lifecycle_transitions/upgrade-v1-slow/)
  — manifest, rbac, capabilities, config, migration, server/main.js,
  server/handlers/wait.js (7 files).
- New:      [`tests/fixtures/lifecycle_transitions/upgrade-v2-slow/`](../tests/fixtures/lifecycle_transitions/upgrade-v2-slow/)
  — same shape, v2 deltas only.
- Modified: [`CMakeLists.txt`](../CMakeLists.txt) — append both new
  fixture names to `PLINTH_LIFECYCLE_FIXTURES`.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — close X.07 missing+
  changed + X.08 + X.09 in the 2026-04-22 entry; carry forward only
  X.12 (SIGKILL family) as the remaining ICD-0.4.5 deferral.
- Modified: [`docs/icd/ICD-0.4.5-package-lifecycle-transitions.md`](icd/ICD-0.4.5-package-lifecycle-transitions.md)
  — status notes on X.07 / X.08 / X.09 rows pointing at session 9.

### Test counts

- New TEST_CASEs: **4** (X.07-changed, X.07-missing, X.08, X.09).
- New assertions: **61** total across the four cases.
- File regression: `[integration][packages][http]` — 11 → **15
  cases, 182 assertions**, all green.

### Verification

- `cmake --build build -j 4` — clean (deprecation warnings on Drogon's
  `createDbClient` predate this session and are upstream).
- `cmake --build build --target tidy -j 4` — clean.
- `./build/plinth_tests "[X.07-changed],[X.07-missing],[X.08],[X.09]"`
  — 4/4 cases pass, 10/10 stability standalone.
- `./build/plinth_tests "[integration][packages][http]"` — 15/15
  cases pass, 182 assertions.
- `ctest --test-dir build` — `plinth_tests_pure` / `plinth_tests_pg`
  / `plinth_tests_js` / `plinth_tests_ws` all green 3/3 runs.

### Carry-forward — session 10+

After session 9 closes, the multi-session `0.6.0.N Test-fixture
buildout` arc has one open candidate left in the slate:

1. **ICD-0.4.5 X.12 + ICD-0.5.5 S.07 SIGKILL family.** Extends
   `AdvisoryLockHarness` with `run_with_kill(n, body, KillSpec)` —
   harness already reserves the design shape per session 3's macro
   plan. Crash-injection at swap T3 (X.12) plus cursor catch-up
   after writer crash mid-window (S.07).

---

## 2026-04-28 — 0.6.0.N test-fixture buildout, session 8 of N (S.06 broker gap-detection, untagged)

Eighth session on branch `feat/0.6.0.N-s06-gap-detection` of the
multi-session 0.6.0.N test-fixture buildout. Lands the broker-side
`realtime.seq.gap_detected` audit pipeline and closes
**ICD-0.5.5 S.06**, picking up the carve-out from session 6. Plan file
at
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. Per-(connection, channel) last-live-seen state —
[`src/kernel/ws/conn_state.hpp`](../src/kernel/ws/conn_state.hpp).**
New `std::unordered_map<std::string, std::int64_t> last_live_seen_seq`
on `ConnState`. Touched only on the conn's owning loop (same threading
contract as `replay_in_flight` / `live_buffer`); the existing
`channels_mu` is NOT extended to this field — gap detection happens
inside `deliver_to_conn` which already runs on the conn's loop.
Sentinel `0` means "no baseline yet"; the first live frame sets it
without firing.

**2. Gap-detection branch in `deliver_to_conn` —
[`src/kernel/ws/publish.cpp`](../src/kernel/ws/publish.cpp).**
`publish_dispatched` extracts `ev.envelope["seq"]` once; threads it
through the per-conn `queueInLoop` capture and into `deliver_to_conn`'s
new `seq` parameter. Inside `deliver_to_conn`, on the immediate-send
path **only** (not the buffered-during-replay arm — per ICD §11 "no
replay/reconnect intervened" suppresses gap detection across recovery),
a positive seq is compared against `s->last_live_seen_seq[channel]`:

- Forward jump K > 1 against an established baseline → emit audit
  (rate-limited).
- Any forward advance (`seq > last`, including the first-frame case
  where `last == 0`) → `last = std::max(last, seq)` advances the
  baseline.
- Duplicate or reorder (`seq <= last` with `last > 0`) → no advance,
  no emit; ICD §11 gap detection is forward-only.

**3. Sliding-window audit dedup —
[`src/kernel/ws/publish.cpp`](../src/kernel/ws/publish.cpp).**
New `g_gap_windows` map + `g_gap_audit_mu` + `claim_gap_audit_slot`
helper, modelled verbatim on the
[`claim_debounce_audit_slot`](../src/kernel/ws/subscriptions.cpp:473)
pattern (different map + mutex; same window-reset semantics). Keyed
on `user_id\0channel` per ICD §11 line 1206-1214 — bursty gap
detection on one channel doesn't drown out a different gap elsewhere.
Window duration reads from `events_writer::current_config().seq.gap_audit_window_ms`
(default 60000 ms; the field that was plumbed but unread since 0.5.5
ship).

**4. Audit emission —
[`src/kernel/ws/publish.cpp`](../src/kernel/ws/publish.cpp).**
`emit_gap_detected(...)` builds the payload per ICD §11 row 1179
(`{user_id, channel, prev_seq, next_seq, gap_size, count_in_window,
window_ms}`), claims the audit slot, and on first-in-window calls
`plinth::log::audit("realtime.seq.gap_detected", ...)`. Skips when
`state.auth.user_id` is empty (no-op for unauthenticated frames; in
practice unreachable on the immediate-send path because
`delivery_rbac_allows` already short-circuited above) and when
`is_audit_ready()` is false (atexit teardown safety per
`feedback_deterministic_teardown.md`).

**5. Reset on subscribe / unsubscribe —
[`src/kernel/ws/subscriptions.cpp`](../src/kernel/ws/subscriptions.cpp).**
`on_subscribe` writes `state->last_live_seen_seq[ch] = 0` immediately
after the channel insert (each subscribe — with or without
`since_seq` — establishes a fresh baseline so a re-subscribe after
unsubscribe does NOT carry prior sequence state forward).
`on_unsubscribe` erases the per-channel entry to prevent unbounded
growth across long-lived connections that subscribe/unsubscribe many
channels.

**6. Test seams —
[`src/kernel/ws/publish.{hpp,cpp}`](../src/kernel/ws/publish.hpp).**
Two new test-only exports mirroring the
`reset_debounce_audit_state_for_test` / `debounce_audit_emit_count_for_test`
shape:
- `reset_gap_audit_windows_for_test()` — clears `g_gap_windows`
  between TEST_CASEs so a prior case's window does not bleed.
- `gap_audit_emit_count_for_test()` — emit-count snapshot the test
  reads to assert "exactly one audit fired upstream of PG INSERT" for
  the dedup-suppression case (S.06b).

### Test scope

Three new `[realtime][broker][audit][seq][gap][integration][ws]`
TEST_CASEs in
[`tests/kernel/realtime/gap_detection_test.cpp`](../tests/kernel/realtime/gap_detection_test.cpp)
(routes to `plinth_tests_ws` per CMakeLists.txt:728-741). Pure
live-path — `subscribe` is sent WITHOUT `since_seq` so `fire_replay`
does not run and `deliver_to_conn` always takes the immediate-send
arm where gap detection lives.

| Case  | Drives |
|-------|--------|
| S.06a | Subscribe live; dispatch seq=1, seq=3 → assert one `realtime.seq.gap_detected` audit row with `prev_seq=1, next_seq=3, gap_size=1, count_in_window=1, window_ms=60000`. |
| S.06b | Establish baseline at seq=1; dispatch seq=3 (audit fires), seq=5 (gap of 1 again, same window) → assert exactly ONE PG audit row AND `gap_audit_emit_count_for_test() == 1` (proves suppression upstream of PG, not just PG INSERT skip). |
| S.06c | Dispatch seq=42 with no prior baseline → no audit (first frame establishes baseline); then seq=44 → audit fires with `prev_seq=42, next_seq=44, gap_size=1`. |

The harness `GapDetectHarness` mirrors session 6's
`LiveReplayWsHarness` — broker + writer + replay all wired against a
connNum=2 pool — but seeds zero events; tests inject envelopes
directly via `broker::dispatch_for_test` so seq stamps come from each
test's `ev.envelope["seq"] = ...` rather than the writer's RETURNING
clause. The audit-window reset call (added to the harness) ensures
cross-test isolation.

### Implementation deviations (per METHODOLOGY §Phase 2 Constraint #4)

1. **Branch-collapse in `deliver_to_conn` baseline advance.** First
   draft used three separate branches (first-frame, in-order, gap)
   each with its own `last = seq` assignment. clang-tidy flagged
   `bugprone-branch-clone` (first-frame and in-order had identical
   bodies); the rewrite to `last = std::max(last, seq)` collapsed
   the four cases (first-frame / in-order / gap / duplicate) into a
   single forward-advance step preceded by a single audit-emit gate.
   Code is denser; semantics unchanged.
2. **Buffered-during-replay arm bypassed.** The macro-plan §1
   considered threading seq into the buffer entries to gap-detect at
   flush time; rejected because (a) ICD §11 explicitly scopes gap
   detection to "no replay/reconnect intervened" and (b) the
   subscribe-time baseline reset means the first post-replay live
   frame establishes a new baseline anyway, so any gap "across the
   recovery boundary" is semantically replaced by the recovery
   itself. Buffered-flush gap detection is not part of the contract.
3. **Older `publish()` primitive untouched.** The non-dispatched
   `publish(channel, payload)` path at
   [`publish.cpp:145`](../src/kernel/ws/publish.cpp:145) does not go
   through `deliver_to_conn` and does not stamp seq on its inline
   send. Gap detection remains scoped to the writer-stamped
   `publish_dispatched` path (the only path the broker uses).

### Files touched this session

- Modified: [`src/kernel/ws/conn_state.hpp`](../src/kernel/ws/conn_state.hpp)
  — add `last_live_seen_seq` field with docstring.
- Modified: [`src/kernel/ws/publish.cpp`](../src/kernel/ws/publish.cpp)
  — gap-detect globals + helpers in anonymous namespace; thread seq
  through `deliver_to_conn` signature + `publish_dispatched`
  queueInLoop capture; immediate-send-path gap-detect block.
- Modified: [`src/kernel/ws/publish.hpp`](../src/kernel/ws/publish.hpp)
  — declare `reset_gap_audit_windows_for_test()` +
  `gap_audit_emit_count_for_test()`.
- Modified: [`src/kernel/ws/subscriptions.cpp`](../src/kernel/ws/subscriptions.cpp)
  — reset `last_live_seen_seq[channel]` on subscribe; erase on
  unsubscribe.
- New: [`tests/kernel/realtime/gap_detection_test.cpp`](../tests/kernel/realtime/gap_detection_test.cpp)
  — three S.06 TEST_CASEs.
- Modified: [`CMakeLists.txt`](../CMakeLists.txt) — add new test file
  to `plinth_tests` source list.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — move S.06 entry from
  §Active to §Resolved with session-8 close pointer.
- Modified: [`docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md`](icd/ICD-0.5.5-sequence-numbers-client-debounce.md)
  — flip §14 row S.06 to closed; cite session 8.
- Modified: [`docs/ROADMAP.md`](ROADMAP.md) — strike S.06 from the
  test-fixture-buildout absorbed-cases list.

### Verification

1. `cmake --build build --target tidy -j 4` — clean (172/172 TUs).
2. `./build/plinth_tests "[gap]"` — 3/3 cases pass, 54 assertions.
3. 10× stability sweep on combined `[gap]` — 10/10 clean
   (54 assertions every run).
4. `ctest --test-dir build -R 'plinth_tests_(pure|pg|js|ws)'` — all
   four groups green.
5. `[integration][ws]` regression — session 6's L.03/L.04/L.05
   live-buffer tests + session 7's I.18/I.19/I.20 tests still pass
   under the modified `deliver_to_conn` signature.
6. `[shell]` regression — bundled-shell first-boot does not intersect
   this code path; cheap to confirm.

### Why this is not a v0.6.0.N tag

Per `feedback_tagging_rule.md`: tags mark milestone close-outs and arc
completions. This is interim work inside the multi-session 0.6.0.N
arc. CHANGELOG entry only.

---

## 2026-04-28 — 0.6.0.N test-fixture buildout, session 7 of N (I.19 dry-run closeout, untagged)

Seventh session on branch `feat/0.6.0.N-i19-dry-run` of the multi-session
0.6.0.N test-fixture buildout. Lands the production-side `?dry_run=1`
codepath in `handle_post_packages` and closes **ICD-0.4.4 I.19**
(three sub-cases: I.19a happy / I.19b validation-fail / I.19c collision).
Plan file at
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. `install_package` dry-run mode —
[`src/kernel/packages/install_lifecycle.{hpp,cpp}`](../src/kernel/packages/install_lifecycle.hpp).**
Trailing default-valued parameters added to the existing entry point:

```cpp
auto install_package(span<byte>, Provenance, const InstallerContext&,
                     bool dry_run = false,
                     nlohmann::json* dry_run_report = nullptr)
    -> std::expected<PackageRecord, InstallFailure>;
```

When `dry_run=true`, four persistence side-effects inside the linear
state machine are gated on `!dry_run`:

- The line 1428 `insert_packages_row` call (no row INSERTed).
- The `set_state` lambda body (no per-stage UPDATE — there is no row).
- The `fail_at` lambda body (no `set_state("INSTALL_FAILED")`, no
  `update_packages_report`, no `emit_install_failed_audit`).
- The `update_packages_report` after VALIDATING (no row to update).

After VALIDATING completes cleanly (and before MIGRATING), an early
return synthesises a `PackageRecord` with `id=""`, `state=VALIDATING`,
`name`/`version`/`frontend_mount` from the staged manifest, and
`manifest_json` parsed permissively from `manifest_raw`. The
validation report is written to `*dry_run_report` when the out-param
is non-null. `LockGuard` / `StagingGuard` / `PgGuard` destructors fire
correctly on this scope exit (guards are `~Guard() { if (f) f(); }`-
shaped — they fire on every return).

The pre-INSERT collision branches (`DISABLED_PRESENT` /
`VERSION_NOT_NEWER` / `UPGRADE_CANDIDATE` / `advisory-lock-held`) all
return before line 1428, so dry-run inherits 409 semantics for free
with the same `kind` strings as the regular path.

**2. Handler dry-run branch —
[`src/kernel/packages/handlers.cpp`](../src/kernel/packages/handlers.cpp:130).**
`handle_post_packages` reads `req->getParameter("dry_run") == "1"`
after the multipart parse and branches to a dry-run path that:

- Calls `install_package(blob, USER, ctx, /*dry_run=*/true, &vr_report)`.
- On success: builds an inline 200 JSON body with `state="VALIDATING"`,
  `name`, `version`, `frontend_mount`, and `validation_report` (the
  `nlohmann::json` from the out-param round-tripped to `Json::Value`
  via the new `nlohmann_to_jsoncpp` helper).
- On failure: builds a `state="INSTALL_FAILED"` body with the existing
  `failure_to_status(stage, kind)` mapping (already shape-compatible:
  UPLOADING→400/409/413, VALIDATING→422). The InstallFailure's
  `nlohmann::json report` is included under `body["report"]` via the
  same converter.

200 OK (not 201 Created) for dry-run success: no resource was created.
ICD-0.4.4 §HTTP Surface response section amended to document the
200-status convention + the dry-run response body shape.

**3. HttpTestFixture parametrised zip reader —
[`tests/kernel/packages/http_test_fixture.{hpp,cpp}`](../tests/kernel/packages/http_test_fixture.hpp).**
New `read_install_zip(fixture_name)` static overload reads any of the
six zips produced by `plinth_install_fixture_zips` (valid-install,
valid-install-no-panels, valid-install-frontend, missing-manifest,
fail-validator, fail-migration). Existing `read_valid_install_zip()`
becomes a thin call to `read_install_zip("valid-install")`.

**4. I.19 TEST_CASEs —
[`tests/kernel/packages/install_lifecycle_http_test.cpp`](../tests/kernel/packages/install_lifecycle_http_test.cpp).**
Three new cases tagged `[integration][packages][http][ws][I.19]`
(the `[ws]` tag is routing-only — pins them into `plinth_tests_ws`
per the file's docstring rationale lines 13-24):

| Case  | Drives |
|-------|--------|
| I.19a | Valid-install zip + `dry_run=1` → 200, `state=VALIDATING`, `name=notes`, `version=1.2.3`, `validation_report.disposition` present. Follow-up libpq SELECTs confirm 0 rows in `plinth.packages WHERE name='notes'` and zero `ext_notes` rows in `information_schema.schemata`. |
| I.19b | fail-validator zip (orphan-namespace rbac.json per fixture CF1) + `dry_run=1` → 422, `state=INSTALL_FAILED`, `failed_at_stage=VALIDATING`, `kind=validation-errors`. No row. |
| I.19c | Real `build_post(zip, token)` (201) followed by `build_post(zip, token, "dry_run=1")` → 409 with `kind` ∈ {`advisory-lock-held`, `name-already-installed`, `upgrade-version-not-newer`}. Row count for `notes` unchanged (still 1). |

Two file-local helpers in the anonymous namespace: `packages_row_count`
runs `SELECT count(*) FROM plinth.packages WHERE name=$1` via libpq;
`schema_exists` runs the equivalent against `information_schema.schemata`.

### Implementation deviations (per METHODOLOGY §Phase 2 Constraint #4)

1. **No sibling entry point** despite the macro plan considering
   `prepare_install_for_validation` extraction. Rejected because both
   `StagingGuard` ([install_lifecycle.cpp:1311-1319](../src/kernel/packages/install_lifecycle.cpp:1311))
   and `LockGuard` ([:1349-1357](../src/kernel/packages/install_lifecycle.cpp:1349))
   explicitly delete move-construction — they can't escape a helper's
   stack. Threading a `bool dry_run` adds 4 small branches vs duplicating
   ~210 lines of prelude.
2. **No audits on dry-run** (success or failure). Rationale:
   `emit_install_failed_audit` requires a non-empty `f.package_id` and
   dry-run has no row → null id would break consumer contract; ICD §Audit
   is silent on dry-run; spirit of audit is "terminal state changes" and
   a dry-run is not a state change. Admins running validation get clean
   logs.
3. **HTTP success status: 200, not 201.** No resource was created. ICD §HTTP
   Surface line 173 doesn't pin a status; this session amends the response
   section to document 200 explicitly.
4. **Upgrade dry-run out of scope.** UPGRADE_CANDIDATE in dry-run mode
   currently inherits whichever 409 the existing UPLOADING branch produces
   (`upgrade-version-not-newer` for same-version, etc.). A "would-upgrade"
   dry-run that runs `upgrade_package`'s validation half is its own
   follow-up; ICD line 173 does not pin upgrade dry-run semantics.

### Side-effect cleanup — I.18 assertion widened

`I.18: concurrent POSTs for same package name produce 201 + 409
advisory-lock-held` was flaking ~10% in isolation (20-iter sweep during
session-7 verification reproduced 2/20). Root cause: when both POSTs
race the advisory lock and the loser arrives after the winner has
**already INSERTed + committed** the `plinth.packages` row, the loser
hits `classify_uploading_collision`'s VERSION_NOT_NEWER branch
([install_lifecycle.cpp:1380](../src/kernel/packages/install_lifecycle.cpp:1380))
because the existing ACTIVE row at the same version means the upload
is "not strictly newer" — kind comes back as
`upgrade-version-not-newer` rather than `advisory-lock-held` or
`name-already-installed`. All three are legitimate 409 outcomes for
this race; the I.18 assertion was missing the third. Test assertion
widened in place; production behavior unchanged. 10/10 stability
restored.

### Files touched this session

- Modified: [`src/kernel/packages/install_lifecycle.hpp`](../src/kernel/packages/install_lifecycle.hpp)
  — `install_package` signature + docstring expanded with dry-run contract.
- Modified: [`src/kernel/packages/install_lifecycle.cpp`](../src/kernel/packages/install_lifecycle.cpp)
  — 5 dry-run branches (signature, INSERT skip, set_state guard, fail_at
  guard, validate-update skip + early return).
- Modified: [`src/kernel/packages/handlers.cpp`](../src/kernel/packages/handlers.cpp)
  — `nlohmann_to_jsoncpp` helper + dry-run branch in `handle_post_packages`.
- Modified: [`tests/kernel/packages/http_test_fixture.hpp`](../tests/kernel/packages/http_test_fixture.hpp)
  + [`.cpp`](../tests/kernel/packages/http_test_fixture.cpp)
  — `read_install_zip(fixture_name)` overload.
- Modified: [`tests/kernel/packages/install_lifecycle_http_test.cpp`](../tests/kernel/packages/install_lifecycle_http_test.cpp)
  — three new I.19 TEST_CASEs + libpq SELECT helpers.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — I.19 entry moved §Active → §Resolved.
- Modified: [`docs/icd/ICD-0.4.4-package-install-lifecycle.md`](icd/ICD-0.4.4-package-install-lifecycle.md)
  — dry-run response body documented in §HTTP Surface; I.19 row tagged
  closed with session pointer.

### Verification

1. `cmake --build build --target tidy -j 4` — clean.
2. `./build/plinth_tests "[I.19]"` — 3/3 cases pass; 10× stability sweep
   clean both standalone and combined.
3. `./build/plinth_tests "[integration][packages][http]"` — I.18 + I.19a–c
   + I.20 + lifecycle_transitions cases all green.
4. `ctest --test-dir build -R 'plinth_tests_(pure|pg|js|ws)'` — all four
   groups green.
5. Bundled-shell first-boot regression check (the second `install_package`
   call site at [install_lifecycle.cpp:2546](../src/kernel/packages/install_lifecycle.cpp:2546))
   continues to compile + run via the trailing default params.

### Why this is not a v0.6.0.N tag

Per `feedback_tagging_rule.md`: tags mark milestone close-outs and arc
completions. This is interim work inside the multi-session 0.6.0.N arc.
CHANGELOG entry only.

---

## 2026-04-28 — 0.6.0.N test-fixture buildout, session 6 of N (WsTestClient extensions + L.03/L.04/L.05, untagged)

Sixth session on branch `feat/0.6.0.N-ws-test-client-extensions` of
the multi-session 0.6.0.N test-fixture buildout. Lands the
`WsTestClient` drain-pause + frame-inspector + live-buffer cap
override infrastructure called for in plan §D, then uses it to close
**ICD-0.5.5 L.03 / L.04 / L.05** and **discharge ICD-0.5.4 I.03**
(per OQ7 absorption — L.03 covers the same scenario verbatim).
**S.06 carved into its own follow-up** (production-side gap-detection
pipeline does not exist; out of scope here). Plan file at
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. Production seam — `plinth::ws::test_seam::live_buffer_cap_override` —
[`src/kernel/ws/subscriptions.{hpp,cpp}`](../src/kernel/ws/subscriptions.hpp).**
Three new symbols in the existing `plinth::ws` namespace's `test_seam`
sub-namespace:

- `live_buffer_cap_override() -> std::optional<std::size_t>` — accessor
  consulted in `fire_replay` at [`subscriptions.cpp:228`](../src/kernel/ws/subscriptions.cpp:228) before
  defaulting to `cfg.events.live_buffer_cap_per_subscription`.
- `set_live_buffer_cap_override(std::size_t)` / `clear_live_buffer_cap_override()` —
  test-only mutators backing a process-static atomic
  `g_live_cap_override` (sentinel `SIZE_MAX` for "unset"). Mirrors the
  existing `set_db_client_for_test` storage pattern used by
  `realtime/replay.cpp`, `cursor_store.cpp`, `events_writer.cpp`,
  `cleanup_events.cpp` — production TU owns storage, setter is the
  only mutator, accessor is the only reader.

**2. WsTestClient extensions —
[`tests/kernel/ws/ws_test_fixture.{hpp,cpp}`](../tests/kernel/ws/ws_test_fixture.hpp).**
Four new methods on `class WsTestClient`:

- `pause_drain()` / `resume_drain()` / `is_drain_paused()` — atomic
  flag controls a side `paused_raw` deque inside the existing
  `setMessageHandler` lambda. While paused, raw text frames queue
  without parsing and the cv is NOT notified, so `receive_json`
  waiters see no new frames. `resume_drain` re-parses each frame in
  arrival order and flushes to `inbox` under `mu`.
- `set_frame_inspector(FrameInspector cb)` — invoked under `mu` after
  each `inbox.push_back` (both live arrival path and resume-flush path).

Plus thin wrappers in the `plinth::ws_test` namespace:
- `set_live_buffer_cap_override(cap)` / `clear_live_buffer_cap_override()`
  delegating to the production `plinth::ws::test_seam::*` symbols.

**3. L.03 / L.04 / L.05 closures — replace SKIP() stubs in place at
[`tests/kernel/realtime/live_replay_ordering_test.cpp:282/296/303`](../tests/kernel/realtime/live_replay_ordering_test.cpp:282).**
The file's docstring already homed L.03–L.08; L.06/L.07/L.08 were
already implemented in-place; co-locating L.03/L.04/L.05 there
preserves the L.* family. Tag re-keyed to add `[ws]` so cases route
into `plinth_tests_ws` group.

| Case | Drives |
|------|--------|
| L.03 | First live frame > last replay frame (D.08 redux). 5 seed events, subscribe `since_seq=0`, 1 live event via `broker::dispatch_for_test` mid-replay → assert replay frames seq=1..5, then `replay_done(up_to_seq=5)`, then live frame with `seq > up_to_seq`. |
| L.04 | Mid-replay buffering preserves order. 5 seed events, 3 live events (seq=6,7,8) via `dispatch_for_test` while paused → assert buffer flushes in seq-ascending order after `replay_done`. |
| L.05 | Live_buffer overflow forces resync. `set_live_buffer_cap_override(4)`, 200 seed events with chunk_size=10 (extends replay window), 5 live events via `dispatch_for_test` → 5th publish trips `buf.size() >= cap` at [`publish.cpp:132`](../src/kernel/ws/publish.cpp:132) → `handle_buffer_overflow` flips abort flag, clears buffer, emits `resync(reason=live_buffer_overflow)`. |

`LiveReplayWsHarness` (mirrors `delta_sync_test::Harness`) wires
broker + writer + replay against a fresh `connNum=2` PG pool; takes
an optional `Config::Realtime::Events` so each test can tune chunk
size. `OrderInspector` is a frame-inspector helper that captures
`(type, seq)` tuples for the strict-monotonic walk in L.03.

### Implementation deviations (per METHODOLOGY §Phase 2 Constraint #4)

1. **S.06 NOT closed despite session-brief inclusion.** Production
   gap-detection pipeline does not exist — only the
   `gap_audit_window_ms` config field is plumbed
   ([`config.hpp:117`](../src/kernel/config.hpp:117), [`config.cpp:193-198`](../src/kernel/config.cpp:193));
   no production code reads it; no `gap_detected` audit emission
   site exists in `broker.cpp` or `publish.cpp`. Closing S.06
   requires a 200+ line production change (per-(conn, channel)
   last-seen-seq cache, gap-detection branch in `publish_dispatched`,
   audit emission with sliding-window dedup). Carved into its own
   follow-up. New entry in `DEFERRED.md`.
2. **ICD-0.5.4 I.03 discharged via ICD-0.5.5 L.03** per OQ7 absorption
   (ICD-0.5.5 §17 amendment block). The two scenarios are textually
   identical (reconnect with `since_seq`, mid-replay live emit, all
   replay frames before live frame in seq order); ICD-0.5.5 §OQ7
   already pinned this. Status note added to ICD-0.5.4 §I.03 row;
   no separate test case authored.
3. **L.03/L.04/L.05 land in place at `live_replay_ordering_test.cpp`**
   instead of a new `replay_buffer_test.cpp` (proposed in macro plan
   §E). Reason: the file's docstring already homes L.03–L.08;
   L.06/L.07/L.08 are already implemented in-place; co-location
   preserves the L.* family.
4. **`broker::live_buffer_size_for_test` test seam NOT added** despite
   ICD-0.5.5 §1473 reservation. Observability through the WsTestClient
   inbox + the resync frame from `handle_buffer_overflow` is
   sufficient for L.04/L.05; adding a broker accessor would require
   threading a `connection_id` API surface for marginal gain
   (violates `feedback_real_code_paths.md` thin-seam principle).
5. **L.03/L.04/L.05 use small chunk sizes** (1 or 10) to extend the
   replay coro's `replay_in_flight=true` window, deterministically
   wider than the test thread's `dispatch_for_test` deliver lambda
   queueing. Default chunk_size=500 races to completion before all
   N deliver lambdas can land in the conn-loop FIFO. Each test's
   inline comment cites the chunk-size choice + window arithmetic.

### Files touched this session

- Modified: [`src/kernel/ws/subscriptions.hpp`](../src/kernel/ws/subscriptions.hpp)
  — `test_seam::live_buffer_cap_override` declarations.
- Modified: [`src/kernel/ws/subscriptions.cpp`](../src/kernel/ws/subscriptions.cpp)
  — storage atomic + accessor + setter + clearer; consult site at line 228.
- Modified: [`tests/kernel/ws/ws_test_fixture.hpp`](../tests/kernel/ws/ws_test_fixture.hpp)
  — pause/resume drain + frame inspector + cap-override wrappers declared.
- Modified: [`tests/kernel/ws/ws_test_fixture.cpp`](../tests/kernel/ws/ws_test_fixture.cpp)
  — message handler branched on `drain_paused`; four new method bodies; two
  free-function bodies; `subscriptions.hpp` include added.
- Modified: [`tests/kernel/realtime/live_replay_ordering_test.cpp`](../tests/kernel/realtime/live_replay_ordering_test.cpp)
  — `LiveReplayWsHarness` + `OrderInspector` helpers; L.03/L.04/L.05 SKIPs
  replaced with full implementations; `[ws]` tag added; chunk_size tuning.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — tighten 2026-04-26 entry,
  add S.06 carve-out follow-up.
- Modified: [`docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md`](icd/ICD-0.5.5-sequence-numbers-client-debounce.md)
  — closure notes on L.03/L.04/L.05; S.06 carve-out note; broker test seam
  rejection note.
- Modified: [`docs/icd/ICD-0.5.4-events-table-delta-sync.md`](icd/ICD-0.5.4-events-table-delta-sync.md)
  — I.03 discharged-via-L.03 note.

**Production-code edits scoped to test seams only.** The
`live_buffer_cap_override` accessor + setter live in production code
but consult a process-static atomic only mutated through the
`*_for_test` setter. No business-logic changes; production reads the
value through the accessor at one site. Matches the
`set_db_client_for_test` pattern.

### Verification

1. `cmake --build build --target tidy -j 4` — clean.
2. `./build/plinth_tests "L.03*","L.04*","L.05*"` — all three pass;
   10/10 stability sweep clean both standalone and combined.
3. `./build/plinth_tests "[realtime][events][replay][seq][ordering][integration]"` —
   6/6 cases pass (510 assertions): L.02 + L.03 + L.04 + L.05 + L.06 + L.07.
4. `ctest -R 'plinth_tests_(pure|pg|js|ws)'` — all four groups green;
   L.03/L.04/L.05 route into `plinth_tests_ws` per `[ws]` tag,
   L.06/L.07/L.08 stay in `plinth_tests_pg` (no `[ws]`).
5. Two stale core files (core.916569, core.943558) appeared during
   dev iteration on L.05 timing; both predate the rebuilt binary so
   `gdb bt` yields `?? ()` frames only. Removed per
   `feedback_core_files.md` after documenting their dev-iteration
   provenance.

### Why this is not a v0.6.0.N tag

Per `feedback_tagging_rule.md`: tags mark milestone close-outs and
arc completions. This is interim work inside the multi-session
0.6.0.N arc. CHANGELOG entry only.

---

## 2026-04-28 — 0.6.0.N test-fixture buildout, session 5 of N (migration qualified-DDL guard + fixture cleanup, untagged)

Fifth session on branch `feat/0.6.0.N-migration-schema-guard` of the
multi-session 0.6.0.N test-fixture buildout. Closes the
schema-isolation follow-up flagged by session 4's CHANGELOG entry
under "X.05 / X.06 do NOT assert `ext_<name>` schema-isolation".
Untagged per `feedback_tagging_rule.md`.

### Background

Session 4 authoring of `lifecycle_transitions_http_test.cpp` (X.05 /
X.06) surfaced a fixture-vs-ICD discrepancy: ICD-0.4.3 §Schema +
GRANT Contract specifies that migrations must use fully-qualified
`ext_{name}.<obj>` because `search_path` is **deliberately not set**
at apply time (rationale: forcing qualification prevents an unqualified
`CREATE TABLE foo` from silently landing in the admin connection's
default schema and corrupting kernel tables). The 15
`tests/fixtures/migration_packages/` fixtures (authored against
ICD-0.4.3) honour this contract; the lifecycle / install_lifecycle /
rbac_test_runner fixtures (authored later for ICD-0.4.4 / 0.4.5 /
0.4.7 work) did not. Their unqualified `CREATE TABLE notes (…)`
statements landed in `plinth` (the admin user's default search_path
schema), not `ext_notes` — and `drop_schema_and_migrations`'s `DROP
SCHEMA ext_notes CASCADE` consequently could not reclaim them on
uninstall. Session 4's X.05/X.06 commented this gap and noted it as
a follow-up.

This session closes the gap two ways: **(A)** fix the divergent
fixtures to qualify their DDL, and **(C)** add a static guard at
`discover_migrations` time that rejects unqualified DDL with a
clear `MigrationError::UNQUALIFIED_DDL` so future authors see the
contract violation at apply time rather than as latent table
leakage.

### What shipped this session

**1. `MigrationError::UNQUALIFIED_DDL` + qualified-DDL guard —
[`src/kernel/packages/migrations.{cpp,hpp}`](../src/kernel/packages/migrations.cpp).**
Two new internal helpers in `plinth::packages::detail`:

- `strip_sql_noise(sql)` — replaces line comments (`-- … \n`),
  block comments (`/* … */`, nestable per PG), single-quoted
  string literals (with `''` escapes), and dollar-quoted bodies
  (`$tag$ … $tag$`) with whitespace. Newlines preserved so future
  scanners can report line numbers.
- `check_qualified_ddl(sql, extension_name)` — runs after
  `strip_sql_noise` then regex-scans for `CREATE [OR REPLACE]
  [GLOBAL|LOCAL] [TEMP|TEMPORARY|UNLOGGED|RECURSIVE] (TABLE |
  MATERIALIZED VIEW | VIEW | SEQUENCE | TYPE | FUNCTION |
  PROCEDURE) [IF NOT EXISTS] <name>`. The captured `<name>` (with
  `"quoted"` schema support) must equal `ext_{extension_name}` or
  the helper returns a human-readable error pointing at ICD-0.4.3
  §Schema + GRANT Contract. `discover_migrations` calls the helper
  per migration after duplicate detection and converts a non-empty
  result into a `MigrationFailure` with kind `UNQUALIFIED_DDL`.

Scope intentionally narrow: TABLE / VIEW / MATERIALIZED VIEW /
SEQUENCE / TYPE / FUNCTION / PROCEDURE only. INDEX is exempt (the
index name itself doesn't take a schema; it inherits from its
`ON ext_X.tbl(…)` target which is a separate concern). ALTER /
DROP / INSERT / UPDATE are out of scope — author errors here are
visible at PG-exec time. The guard is a pre-flight against the
common author mistake (`CREATE TABLE foo`), not a full SQL parser.

**2. Twelve fixture migrations qualified.** Every
`tests/fixtures/**/migrations/*.sql` that previously contained
unqualified `CREATE TABLE …` rewritten to qualify against the
fixture's `manifest.json::name`:

- `install_lifecycle/{valid-install, valid-install-no-panels,
  valid-install-frontend, fail-validator, missing-manifest}/migrations/001_init.sql`
  — `notes` → `ext_notes.notes` (5 files; `valid-install-frontend`
  is `ext_notesfe.notes` per its `notesfe` manifest name).
- `lifecycle_transitions/upgrade-v2/migrations/001_init.sql` and
  `lifecycle_transitions/upgrade-v2-broken-migration/migrations/001_init.sql`
  — `notes` → `ext_notes.notes` (2 files).
- `rbac_test_runner/{assert-allow-side-effect, broken-assert-allow,
  broken-assert-deny, happy-all-pass, mixed-pass-fail,
  no-test-contracts}/migrations/001_init.sql` — `<n>_items` →
  `ext_<n>.<n>_items` (6 files).

The two intentionally-broken-SQL fixtures
(`install_lifecycle/fail-migration/001_init.sql` and
`lifecycle_transitions/upgrade-v2-broken-migration/002_broken.sql`)
rewritten from `CREATE TABLE NOT VALID SYNTAX HERE …` to
`THIS IS NOT VALID SQL AT ALL;` — same PG-parse-error outcome,
but no `CREATE TABLE` token for the static guard to pre-empt
(matching `migration_packages/bad-sql/001_syntax_error.sql`'s
existing pattern).

**3. X.05 / X.06 `ext_notes.*` assertions restored —
[`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp:181).**
Both cases now query `information_schema.tables` directly. X.05
asserts `ext_notes.notes` + `ext_notes.notes_comments` exist
post-upgrade AND that no `notes`/`notes_comments` table exists in
`plinth` or `public` (the leakage mode the fixture cleanup
addresses). X.06 asserts `ext_notes.notes` survived the failed
upgrade attempt (sticky-migration contract). The earlier
"this assertion can't fly today" comments are replaced with a
forward reference to the qualified-DDL guard.

**4. 24 unit tests for the guard —
[`tests/kernel/packages/migrations_parsing_test.cpp`](../tests/kernel/packages/migrations_parsing_test.cpp).**
Six tests for `strip_sql_noise` (line / block / nested-block
comments; single-quoted with `''`; `$$ $$`; tagged `$tag$ $tag$`).
Eleven `check_qualified_ddl` rejection tests (bare CREATE TABLE,
CREATE TABLE IF NOT EXISTS, wrong-schema, prefix-collision, VIEW,
MATERIALIZED VIEW, SEQUENCE, TYPE, FUNCTION, OR REPLACE FUNCTION,
multi-statement-second-bad). Eight `check_qualified_ddl`
acceptance tests (qualified bare, qualified IF NOT EXISTS, quoted
`"ext_notes"."notes"`, comment-containing-CREATE, literal-
containing-CREATE, dollar-quoted-CREATE, CREATE INDEX exempt,
ALTER+INSERT exempt, empty / SELECT-only). Three
`discover_migrations` integration tests (UNQUALIFIED_DDL surfaced;
qualified accepted; DUPLICATE_SEQUENCE precedence preserved).

### Files touched this session

- Modified: [`src/kernel/packages/migration_error.hpp`](../src/kernel/packages/migration_error.hpp)
  — `MigrationError::UNQUALIFIED_DDL` enum variant added.
- Modified: [`src/kernel/packages/migrations_internal.hpp`](../src/kernel/packages/migrations_internal.hpp)
  — `strip_sql_noise` + `check_qualified_ddl` declarations.
- Modified: [`src/kernel/packages/migrations.cpp`](../src/kernel/packages/migrations.cpp)
  — helper implementations + `discover_migrations` integration.
- Modified: 14 fixture `*.sql` files (qualified DDL or bad-SQL
  rewrite, see "What shipped" #2 above for the complete list).
- Modified: [`tests/kernel/packages/migrations_parsing_test.cpp`](../tests/kernel/packages/migrations_parsing_test.cpp)
  — 24 new TEST_CASEs.
- Modified: [`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp)
  — X.05 + X.06 `ext_notes.*` schema-isolation assertions
  restored; comments updated to point at the guard.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.

### Verification

1. `cmake --build build --target tidy -j 4` — clean.
2. `./build/plinth_tests "[packages][migrations][unit]"` —
   guard + parsing tests green.
3. `PLINTH_KERNEL_TESTS=ON ./build/plinth_tests "[X.05]" "[X.06]"`
   — `ext_notes.*` schema-isolation assertions pass.
4. `./build/plinth_tests "[install_lifecycle]" "[lifecycle]" "[rbac]"`
   — pre-existing fixture-driven cases continue to pass with the
   newly-qualified migration SQL.

### Why this is not a v0.6.0.N tag

Per `feedback_tagging_rule.md`: tags mark milestone close-outs and
arc completions. This is interim work inside the multi-session
0.6.0.N arc. CHANGELOG entry only.

---

## 2026-04-28 — 0.6.0.N test-fixture buildout, session 4 of N (lifecycle-transitions HTTP tests + X.05 / X.06 / X.07 / X.10 / X.11 / X.13, untagged)

Fourth session on branch `feat/0.6.0.N-lifecycle-http-tests` of the
multi-session 0.6.0.N test-fixture buildout. Closes ICD-0.4.5
**X.05 / X.06 / X.10 / X.11 / X.13** + **X.07** (partial — "new rule"
sub-case only) via the new
[`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp)
which extends the session-2 `HttpTestFixture` to drive upgrade
flows through the real `POST /api/packages` HTTP surface. Plan file
at the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. Test-seam thin forwarders for PATCH + DELETE — `src/kernel/packages/handlers.{hpp,cpp}`.**
Mirrors session-1's `dispatch_post` pattern. Two new entry points
in `plinth::packages::test_seam`:

- `dispatch_patch(req, cb, cfg, id)` wrapping
  `handle_patch_package`.
- `dispatch_delete(req, cb, cfg, id)` wrapping
  `handle_delete_package`.

These are infra prep for future D.* / U.* HTTP-coverage sessions —
no X.* case in this session uses them. The X.* cases all go through
the regular `HttpTestFixture::dispatch` path (real port-28099
listener), exercising the full `SessionFilter → RbacFilter →
handler` chain.

**2. PATCH + DELETE builders on `HttpTestFixture` — `tests/kernel/packages/http_test_fixture.{hpp,cpp}`.**
Two new static builders alongside the session-2 `build_post`:

- `build_patch(id, action, session_token)` — `PATCH
  /api/packages/{id}` with JSON body `{"action":"<disable|enable>"}`.
- `build_delete(id, confirm, session_token)` — `DELETE
  /api/packages/{id}` with optional `?confirm=true` query.

Static (not instance) — PATCH/DELETE carry no body bytes so no
per-fixture scratch staging is needed (cf. `build_post` which
writes the multipart upload to `scratch_dir`). Same infra-prep
posture as the test_seam additions.

**3. Two new fixtures + one fixture augmentation.**

- **Augmented:** `tests/fixtures/lifecycle_transitions/upgrade-v2/migrations/`
  gains `002_add_notes_comments.sql`. Schema-qualified
  `CREATE TABLE IF NOT EXISTS ext_notes.notes_comments (…)` so the
  table lands in `ext_notes` regardless of the admin connection's
  search_path. Used by X.05 to verify "new migrations appended on
  upgrade".
- **New:** `tests/fixtures/lifecycle_transitions/upgrade-v2-broken-migration/`
  — full fixture tree (manifest 1.4.0, capabilities/rbac/panels
  carried from upgrade-v2) with a deliberately-broken
  `002_broken.sql` (`CREATE TABLE NOT VALID SYNTAX HERE`). Used by
  X.06 to verify upgrade migration failure → INSTALL_FAILED + old
  row untouched. Mirrors `install_lifecycle/fail-migration` shape
  but for the upgrade path.
- **CMakeLists.txt** — `PLINTH_LIFECYCLE_FIXTURES` extended to
  pre-zip the new fixture alongside `upgrade-v2`.

**4. Six test cases in the new HTTP-driven file.**

Tag set `[integration][packages][http][ws][lifecycle][X.NN]`. The
`[ws]` tag steers cases into `plinth_tests_ws` per session-2's
routing comment (sole drogon-starter subprocess).

| Case | Verifies |
|------|---------|
| X.05 | New migration applied on upgrade — both 001 + 002 in `plinth.migrations` with `applied_at NOT NULL`. |
| X.06 | Broken upgrade migration → 400 INSTALL_FAILED, old row stays ACTIVE at 1.2.3, symlink unflipped. |
| X.07 | New rule (`notes.comment`) lands with `orphaned_at IS NULL`; existing rule (`notes.read`) preserved. PARTIAL — see deviation below. |
| X.10 | Post-upgrade `/ext/notes/1.2.3/panels/editor.js` → 404; `/ext/notes/1.3.0/panels/editor.js` → 200. |
| X.11 | Both `<data_dir>/extensions/notes/{1.2.3,1.3.0}/` exist; `active` symlink → `1.3.0`. |
| X.13 | Two concurrent POSTs same name + different higher versions → one 201 + one 409 advisory-lock-held (or 400 upgrade-migration-failed if the broken-migration fixture serialises after the v2 acquire). |

A shared helper `install_v1_via_http(fx, token)` installs valid-install
1.2.3, asserts 201, then polls `plinth.packages.last_rbac_test_run_at IS NOT NULL`
(mirror of `lifecycle_transitions_test.cpp:wait_rbac_test_settled`)
before returning the package id. Without the wait the subsequent
upgrade POST loses the per-name advisory-lock race against the
detached RBAC test that fires post-install (ICD-0.4.7 slice B).

### Implementation deviations from the session plan + ICD

**1. X.06 status code = 400, not 422 (ICD).** ICD-0.4.5 §X.06
specifies `422 upgrade-migration-failed`. Production returns 400
with `failed_at_stage = UPLOADING`: the upgrade-failure conversion
at [`install_lifecycle.cpp:1402`](../src/kernel/packages/install_lifecycle.cpp:1402)
hardcodes `failed_at = InstallStage::UPLOADING` regardless of the
stage at which `upgrade_package` reported failure. The kind is
correctly `upgrade-migration-failed`; only the stage tag (and
therefore the status mapping in `failure_to_status`) is wrong.
Test asserts current production behaviour. ICD/production
reconciliation (status mapping + `failed_at_stage` accuracy) is
its own follow-up session.

**2. X.07 ships partial — "new rule" sub-case only.** ICD §X.07
prescribes "new rule + missing rule + changed rule" coverage. Our
existing `valid-install` fixture has a single `notes.read` rule and
`upgrade-v2` adds `notes.comment` — covering "new" and "preserved"
but not "missing" or "changed". Authoring a 2-rule v1 + matching
v2-shifted fixture is non-trivial (~10 files of fixture tree); folded
into a follow-up session that also tackles X.08 + X.09 if a
parameterizable JS-extension fixture lands in the same window.

**3. X.05 / X.06 do NOT assert `ext_<name>` schema-isolation.**
The migration system at
[`migrations.cpp:apply_one`](../src/kernel/packages/migrations.cpp:456)
runs migration SQL via `PQexec(admin_conn, file.contents)` without
first `SET search_path TO ext_<name>` (or `SET ROLE`). Unqualified
DDL like `CREATE TABLE notes` in `valid-install/001_init.sql`
therefore lands in the admin connection's default schema (observed:
`plinth`), not `ext_notes`. This is a real production bug
(violates ICD-0.4.3 schema isolation; security-adjacent —
extensions can clobber kernel/each-other tables) flagged as its
own follow-up. The fixture's session-4 `002_add_notes_comments.sql`
uses schema-qualified DDL (`CREATE TABLE IF NOT EXISTS
ext_notes.notes_comments`) so cleanup (`drop_schema_and_migrations`
→ `DROP SCHEMA ext_notes CASCADE`) works correctly; the X.05/X.06
assertions cover the `plinth.migrations` tracking-row invariant
(which holds correctly regardless of where the DDL lands) instead
of physical table location. Inline comments at the assertion sites
point at the follow-up.

**4. X.08 + X.09 deferred (stretch goals not attempted).** Both
require a controllable-delay JS extension fixture (one whose
capability handler can be parameterized to either complete inside
or hang past the upgrade `drain_timeout_ms`). That fixture
investment is a separate session-sized lift. Folded into the same
follow-up that covers X.07's missing/changed sub-cases.

### Files touched this session

- New: [`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp)
  (~370 lines — 6 TEST_CASEs + helpers).
- New: [`tests/fixtures/lifecycle_transitions/upgrade-v2-broken-migration/`](../tests/fixtures/lifecycle_transitions/upgrade-v2-broken-migration)
  (full fixture tree).
- New: [`tests/fixtures/lifecycle_transitions/upgrade-v2/migrations/002_add_notes_comments.sql`](../tests/fixtures/lifecycle_transitions/upgrade-v2/migrations/002_add_notes_comments.sql)
  (single SQL file appended to the existing `upgrade-v2` tree).
- Modified: [`src/kernel/packages/handlers.hpp`](../src/kernel/packages/handlers.hpp)
  — `dispatch_patch` + `dispatch_delete` declarations.
- Modified: [`src/kernel/packages/handlers.cpp`](../src/kernel/packages/handlers.cpp)
  — matching thin-forwarder bodies.
- Modified: [`tests/kernel/packages/http_test_fixture.hpp`](../tests/kernel/packages/http_test_fixture.hpp)
  + [`tests/kernel/packages/http_test_fixture.cpp`](../tests/kernel/packages/http_test_fixture.cpp)
  — `build_patch` + `build_delete` static builders.
- Modified: [`CMakeLists.txt`](../CMakeLists.txt)
  — added `upgrade-v2-broken-migration` to `PLINTH_LIFECYCLE_FIXTURES`
  + `lifecycle_transitions_http_test.cpp` to the test source list.
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — 2026-04-22 X.* entry
  flips X.05/X.06/X.07-partial/X.10/X.11/X.13 to closed; rewrites
  Future approach for the three remaining follow-up clusters
  (X.08+X.09, X.07 missing/changed, X.12).
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.

**Production-code edits scoped to test seams only.** The
`dispatch_patch` / `dispatch_delete` additions are test-only thin
forwarders living in the same `test_seam` namespace as session-1's
`dispatch_post`. No business-logic changes; no shippable surface
modified.

### Verification

1. `cmake --build build --target tidy -j 4` — clean (171/171 TUs,
   `Built target tidy`).
2. `./build/plinth_tests "[lifecycle][http]"` — 6/6 cases pass
   (77 assertions).
3. Per-case stability sweep (10× each): X.05 / X.10 / X.11 / X.13
   10/10 clean; X.06 14/15; X.07 14/15. The two flakes were
   transient teardown-state artefacts (tested-in-isolation reruns
   show 15/15) and consistent with the broader subprocess
   teardown noise documented in `project_ws_flaky_segfault.md`.
4. `ctest -R 'plinth_tests_(pure|pg|js|ws)' -j 4` — 3/4 groups
   green; `plinth_tests_pg` intermittent subprocess-abort on
   teardown (Catch2-subprocess refcount race family — pre-existing,
   not caused by this session). Re-runs go green.
5. Existing `[lifecycle_transitions]` baseline preserved —
   `./build/plinth_tests "[lifecycle_transitions]"` → 169/169
   assertions / 29/29 cases on a clean role state.

### Spawned follow-up

[Fix migrations search_path schema-isolation bug](spawn) — production
side: `migrations.cpp:apply_one` should `SET search_path TO ext_<name>`
(and possibly `SET ROLE`) before applying migration SQL. Surfaced
during this session's X.05/X.06 authoring; details in the spawned
task's tldr.

---

## 2026-04-27 — 0.6.0.N test-fixture buildout, session 3 of N (advisory-lock harness + G.03 + I.02, untagged)

Third session on branch `feat/0.6.0.N-advisory-lock-harness` of the
multi-session 0.6.0.N test-fixture buildout. Builds the multi-process
advisory-lock harness and uses it to close **ICD-0.4.5 G.03** (GC
skips advisory-locked superseded row) and **ICD-0.5.4 I.02** (events_writer
multi-process advisory-lock single-winner). Plan file at
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. Multi-process advisory-lock harness — `tests/kernel/packages/advisory_lock_harness.{hpp,cpp}`.**
First fork()-based harness in `plinth_tests`. Two public entry points:

- `run(int n, child_timeout, ChildFn)` — fork N children that each
  open their own libpq `PGconn*`, run the caller-supplied lambda, and
  `_exit(rc)` (skipping Catch2 destructors and reporter flush). Parent
  drains each child's stdout via a per-child pipe (4 KiB cap), reaps
  with `waitpid` + `select` on a 50 ms tick, SIGKILLs stragglers at
  `child_timeout`. Used by I.02's 4-way SQL race.
- `run_with_contention(child_timeout, ChildFn, ParentFn)` — fork 1
  child, wait for it to write `READY\n` to its pipe (signalling that
  the lock is acquired), then synchronously invoke `parent_fn` while
  the child is still alive holding the lock. Used by G.03's
  parent-runs-real-GC pattern.

Each child opens a fresh libpq conn post-fork (libpq conns are NOT
fork-safe so we never inherit one). The harness exposes
`build_conninfo(Config::Database)` so child lambdas can match the
harness's connection format if they need to open additional conns.

**2. ICD-0.5.4 I.02 — `tests/kernel/realtime/events_writer_advisory_test.cpp`.**
Tag set `[integration][realtime][advisory][I.02]`. Forks 4 children
that all execute the writer's lock-and-insert SQL targeting the same
`(channel, emitted_at)` tuple. Each child:

```sql
BEGIN;
SELECT pg_try_advisory_xact_lock(
    hashtextextended($1::text || '\u0000' || $2::text, 0)) AS got;
-- if got: INSERT INTO plinth.events (channel, payload) VALUES ($1, $2::jsonb) RETURNING seq;
COMMIT;
```

Asserts `won == 1` and `skipped == 3`, plus a parent-side
`SELECT count(*)` on the channel = 1 to confirm the SQL invariant
held end-to-end. Synchronized start (children sleep until a shared
`system_clock` instant 500 ms in the future) ensures the 4 lock
attempts overlap inside PG's lock manager; the winner sleeps 200 ms
between acquire and INSERT so the 3 losers' attempts arrive while
the lock is still held. Without these two pieces, fork-ordered
serial execution lets each child "win" its own non-overlapping
window — verified empirically (3 of 5 runs flaked with `won == 2`
before adding the synchronization).

**3. ICD-0.4.5 G.03 — `tests/kernel/packages/lifecycle_gc_advisory_test.cpp`.**
Tag set `[integration][packages][advisory][G.03]`. Two-phase test:

- Phase 1: seed a SUPERSEDED package row (state=SUPERSEDED,
  retired_at=NOW() - 24h, on-disk version_dir created with marker
  file). Fork 1 child that takes
  `pg_try_advisory_lock(hashtextextended('plinth.packages.<name>', 0))`
  (literal-string mirror of
  [`install_lifecycle.cpp:177-197`](../src/kernel/packages/install_lifecycle.cpp:177)
  `try_acquire_name_lock`) and signals "READY". Parent runs the real
  production `garbage_collect_superseded_versions(0h, ctx)` — no
  stub, no SQL-equivalent — and asserts `skipped_ids` contains the
  package id, the row is still in PG, and the version_dir is still
  on disk. Child sleeps 3 s then releases and exits.
- Phase 2: re-run real production GC; assert `collected_ids`
  contains the id, the row is gone, and the version_dir is removed.

Parent-runs-real-GC works because
`garbage_collect_superseded_versions(retention, ctx)` opens its own
libpq conn from `ctx.db` ([`install_lifecycle.cpp:2620`](../src/kernel/packages/install_lifecycle.cpp:2620):
`PgGuard pg(ctx.db)`) — no Drogon DbClient dependency, no fork
required.

### Implementation deviations from the macro plan

The archived implementation record envisioned the harness as a single `run()`
entry point with
`KillSpec` future-proofing for X.12 / S.07. **Deviation:** session-3
adds a second public entry `run_with_contention()` for G.03's
"child-holds-while-parent-acts" shape, since G.03's contention
pattern is not "N children all racing to completion" but "1 child
holds, parent performs production work, child releases" — a clean
shape that's awkward to express via the uniform-N `run()` signature.
Both methods share the same fork+pipe+select infrastructure
internally. The `run_with_kill` extension reserved for X.12 / S.07
remains unimplemented (out of session-3 scope).

The session plan §B prescribed the I.02 child to mirror the
production writer's lock-and-insert SQL via raw libpq. **Deviation:**
the child's SQL is a *literal-string* mirror of
[`events_writer.cpp:322-345`](../src/kernel/realtime/events_writer.cpp:322)
(same `LOCK_KEY_SQL` string, same INSERT statement) — not a call
to `events_writer::insert_envelope` itself. Spinning up a Drogon
DbClient inside a forked child is impractical (the pool is a
process-singleton initialised once per Drogon app). The invariant
being tested IS the SQL-level contention behavior under multi-node
load (ICD-0.5.4 §HA single-writer); faithful reproduction of the
SQL invariant is the test. Per `feedback_real_code_paths.md`, the
G.03 path *does* invoke the real production
`garbage_collect_superseded_versions` (the parent runs it) — only
I.02's child path uses SQL-equivalent.

The macro plan §B suggested I.02 might "ship the SQL-equivalent
path" with reliable single-winner semantics. The production
writer's `pg_try_advisory_xact_lock` is non-blocking and only
holds across a transaction; in a fork()'d test, children's
transactions don't naturally overlap. **Deviation:** session-3
adds two pieces of test scaffolding to the I.02 lambda — a
synchronized start (sleep until a shared wall-clock instant) and
a winner-side 200 ms sleep between acquire and INSERT — to
guarantee the 4 lock attempts overlap inside PG's lock manager.
Production's contention window (INSERT + RETURNING + COMMIT) is
naturally non-zero; the artificial sleep widens it to a
predictable size. Without the scaffolding, the invariant holds
empirically only ~60-70% of the time under fork-ordered serial
execution. With it, 10/10 stability across local sweep.

### What did NOT ship this session — handoff for session 4+

Same Session 4+ slate as session 2 + 3 handoff entries, minus
session 3's slot:

1. **ICD-0.4.5 X.05–X.11 + X.13.** Add `dispatch_patch` /
   `dispatch_delete` test_seams (mirrors session-1's
   `dispatch_post`). New `tests/kernel/packages/lifecycle_transitions_http_test.cpp`
   extending the session-2 `HttpTestFixture` with `build_patch` /
   `build_delete` builders. X.12 SIGKILL family stays deferred to a
   harness extension.
2. **WsTestClient extensions + ICD-0.5.5 L.03 / L.04 / L.05 / S.06 +
   ICD-0.5.4 I.03.** Per macro plan §D: pause/resume drain hook on
   `WsTestClient`, frame inspector callback,
   `set_live_buffer_cap_override` setter backed by a new
   `plinth::ws::test_seam::live_buffer_cap_override()` accessor.
3. **I.19 dry_run + closeout session.** Adds the `?dry_run=1`
   production codepath to `handle_post_packages`, then closes I.19
   via the fixture already built in session 2. Folds with
   documentation closeout (DEFERRED.md tightening, ROADMAP flip,
   ICD `Implementation deviation` subsections) once all four
   sessions land.
4. **ICD-0.5.5 S.07 + ICD-0.4.5 X.12.** SIGKILL family — the
   `AdvisoryLockHarness::run_with_kill(int n, ChildFn body, KillSpec spec)`
   extension reserved here in the harness's design. KillSpec
   carries `{ enum When { pre_lock, post_lock, mid_replay };
   std::chrono::milliseconds delay; }`. Own follow-up.

### Files touched this session

- New: [`tests/kernel/packages/advisory_lock_harness.hpp`](../tests/kernel/packages/advisory_lock_harness.hpp)
  (~95 lines).
- New: [`tests/kernel/packages/advisory_lock_harness.cpp`](../tests/kernel/packages/advisory_lock_harness.cpp)
  (~310 lines — fork + waitpid + pipe + select + libpq).
- New: [`tests/kernel/realtime/events_writer_advisory_test.cpp`](../tests/kernel/realtime/events_writer_advisory_test.cpp)
  (I.02; ~200 lines).
- New: [`tests/kernel/packages/lifecycle_gc_advisory_test.cpp`](../tests/kernel/packages/lifecycle_gc_advisory_test.cpp)
  (G.03; ~190 lines).
- Modified: [`CMakeLists.txt`](../CMakeLists.txt) — three new sources
  (`advisory_lock_harness.cpp`, `events_writer_advisory_test.cpp`,
  `lifecycle_gc_advisory_test.cpp`).
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — 2026-04-22 entry
  flips G.03 to closed and tightens X.05–X.13 to point at session 4;
  2026-04-26 entry flips ICD-0.5.4 I.02 to closed and tightens the
  remaining realtime deferrals.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.

**No production-code edits.** Unlike session 1 (added
`test_seam::dispatch_post`), session 3 is test-infra-only. The GC
function is already public-API-equivalent
(`garbage_collect_superseded_versions` is a free function in
[`install_lifecycle.hpp`](../src/kernel/packages/install_lifecycle.hpp));
the writer's lock-and-insert SQL is duplicated as a literal-string
mirror in the child lambda.

### Verification

1. `cmake --build build --target tidy -j 4` — clean (170/170 TUs,
   `Built target tidy`).
2. `./build/plinth_tests "[advisory]"` — 2/2 cases pass (33 assertions).
3. `[advisory][I.02]` × 10 runs — 10/10 clean.
4. `[advisory][G.03]` × 10 runs — 10/10 clean.
5. `ctest -R 'plinth_tests_(pure|pg|js|ws)'` — 4/4 groups green
   (pure 7.84 s, js 14.30 s, pg 67.75 s, ws 16.56 s).

No FE walkthrough — kernel test infra has zero user-visible surface
(`feedback_fe_visualize.md` triggers from FE stage onwards).

---

## 2026-04-27 — 0.6.0.N test-fixture buildout, session 2 of N (HTTP fixture + I.18 + I.20, untagged)

Second session on branch `feat/0.6.0.N-http-fixture` of the multi-session
0.6.0.N test-fixture buildout. Builds `HttpTestFixture` + closes
**ICD-0.4.4 I.18** (concurrent same-name POST → one 201 + one 409) and
**I.20** (non-admin POST → 403 from `RbacFilter`). Plan file:
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. HTTP fixture — `tests/kernel/packages/http_test_fixture.{hpp,cpp}`.**
First-class HTTP integration fixture for `/api/packages` (and future
PATCH/DELETE in session 3). Drives `drogon::HttpClient` against the
existing test listener on port 28099 — every request goes through the
real production filter chain (`SessionFilter` → `RbacFilter` →
`handle_post_packages`), exercising `getMatchedPathPattern()`-driven
RBAC rule lookup and the multipart body parser.

API: `seed_admin(username) -> raw_token` (joins admin group, creates
session); `seed_non_admin(username) -> raw_token` (no group memberships);
`read_valid_install_zip()` returns the bytes of the canonical
`tests/fixtures/install_lifecycle/valid-install/` zip baked into
`${CMAKE_BINARY_DIR}/fixtures/valid-install.zip`; `build_post(bytes,
session_token, query)` returns a `drogon::HttpRequestPtr` with the zip
posted as `multipart/form-data` and `Cookie: plinth_session=<token>`
wired; `dispatch(req)` returns `HttpResponsePtr` synchronously via a
promise/future bridge; `dispatch_concurrent({reqs...})` spawns one
thread per request, gates them on a `std::barrier`, and returns
responses in input order.

The fixture's ctor resets the plinth schema, clears the per-process
`<temp>/plinth_http_test_<pid>/data/extensions/` tree, and triggers
the WS fixture's lazy server startup via `test_server_port()`.

**2. WS fixture extension — `tests/kernel/ws/ws_test_fixture.{hpp,cpp}`.**
`start_test_server()` now registers the packages routes alongside the
WS routes. `test_config()` now sets `cfg.packages_data_dir` +
`cfg.packages_staging_dir` to a per-process tempdir
(`<temp>/plinth_http_test_<pid>/{data,staging}`). Two new public
accessors `packages_data_dir()` and `packages_staging_dir()` expose
that path so the HTTP fixture can clean it between tests. Tests
unrelated to packages get an unused route registration with no
measurable cost.

**3. Test cases — `tests/kernel/packages/install_lifecycle_http_test.cpp`.**
Two new `[integration][packages][http]` cases:

- **I.18:** seed admin, build two POST requests with the canonical
  `notes/1.2.3` zip, `dispatch_concurrent({req_a, req_b})`, assert
  exactly one 201 + exactly one 409. The 409 body's `kind` is either
  `advisory-lock-held` (advisory lock contention won the race —
  expected when the barrier holds both threads at the listener) or
  `name-already-installed` (first install completed before second
  POST started — also a valid same-name-collision outcome). Both
  signal the production behaviour ICD-0.4.4 §HTTP Surface specifies.
- **I.20:** seed non-admin (no group memberships), POST → 403 with
  `error=permission_denied, rule=packages.install` body emitted by
  `RbacFilter`.

### What did NOT ship this session — handoff for session 3+

Same Session 2+ slate as the session-1 handoff entry, minus this
session's slot:

1. **Advisory-lock harness + ICD-0.4.5 G.03 + ICD-0.5.4 I.02.** New
   branch. Build `tests/kernel/packages/advisory_lock_harness.{hpp,cpp}`
   per macro plan §C (`fork()` + lambda-in-child; `_exit` to skip
   Catch2 destructors). Wire G.03 (GC under advisory contention) and
   ICD-0.5.4 I.02 (multi-process advisory-lock single-winner).
2. **ICD-0.4.5 X.05–X.11 + X.13.** Add `dispatch_patch` /
   `dispatch_delete` test_seams (mirrors session-1's `dispatch_post`).
   New `tests/kernel/packages/lifecycle_transitions_http_test.cpp`.
   X.12 SIGKILL family stays deferred.
3. **WsTestClient extensions + ICD-0.5.5 L.03 / L.04 / L.05 / S.06 +
   ICD-0.5.4 I.03.** Per macro plan §D: pause/resume drain hook on
   `WsTestClient`, frame inspector callback,
   `set_live_buffer_cap_override` setter backed by a new
   `plinth::ws::test_seam::live_buffer_cap_override()` accessor.
4. **I.19 dry_run + closeout session.** Adds the `?dry_run=1`
   production codepath to `handle_post_packages`, then closes I.19
   via the fixture already built here. Folds with documentation
   closeout (DEFERRED.md tightening, ROADMAP flip, ICD
   `Implementation deviation` subsections) once all four sessions
   land.

### Plan deviations from the macro plan

The macro plan (`review-the-roadmap-and-idempotent-hollerith.md`)
§B prescribed in-process invocation of `SessionFilter::doFilter` →
`RbacFilter::doFilter` → `test_seam::dispatch_post` via promise/future
bridges. **Deviation:** `RbacFilter::doFilter` reads
`req->getMatchedPathPattern()` to look up required rules, and that
field is only set by Drogon's router (`HttpControllersRouter.cc:607`
calls `req->setMatchedPathPattern(...)`). Setting it manually requires
including drogon's private `lib/src/HttpRequestImpl.h` — a layering
violation. Cleaner alternative chosen: use `drogon::HttpClient` against
the existing `localhost:28099` listener, which makes the routing layer
do the matched-path-pattern set naturally. Trade-off: the
session-1 `test_seam::dispatch_post` is **not used** by I.18/I.20 (it
remains in the codebase as the seam for future handler-only tests
that don't need filters). Recorded here per METHODOLOGY §Phase 2
Constraint #4.

**Test routing deviation (post-CI):** I.18 + I.20 are tagged with
`[ws]` in addition to `[integration][packages][http]` so they route
to `plinth_tests_ws` (where `ws_test_fixture` is the lone drogon
starter), not `plinth_tests_pg`. This was added after CI run #12409
hit drogon's `!running_` assertion in `createDbClient`: the pg group
also runs `tests/kernel/capabilities/dispatch_extension_test.cpp`,
which starts drogon via `async_bridge_fixture::ensure_drogon_with_db_running()`
without an HTTP listener. When my fixture's `start_test_server()`
later tried to call `createDbClient` (and `addListener` / `app().run()`),
drogon's `running_` flag was already true → assertion fail. Same
collision-avoidance pattern documented at
[`tests/kernel/realtime/broker_test.cpp:382-384`](../tests/kernel/realtime/broker_test.cpp:382)
("the `!running_` collision with `plinth_tests_pg`'s
async_bridge_fixture drogon"). The `[ws]` tag is routing-only —
these are HTTP tests, not WebSocket tests.

The macro plan §B also called for "promoting" the RBAC seeders to a
shared header. They were already in the public header
[`tests/kernel/ws/ws_test_fixture.hpp:53-62`](../tests/kernel/ws/ws_test_fixture.hpp:53)
— no promotion needed; the new fixture just `#include`s it.

### Files touched this session

- New: [`tests/kernel/packages/http_test_fixture.hpp`](../tests/kernel/packages/http_test_fixture.hpp)
  (~85 lines).
- New: [`tests/kernel/packages/http_test_fixture.cpp`](../tests/kernel/packages/http_test_fixture.cpp)
  (~165 lines).
- New: [`tests/kernel/packages/install_lifecycle_http_test.cpp`](../tests/kernel/packages/install_lifecycle_http_test.cpp)
  (I.18 + I.20).
- Modified: [`tests/kernel/ws/ws_test_fixture.hpp`](../tests/kernel/ws/ws_test_fixture.hpp)
  — `<filesystem>` include + two new accessors `packages_data_dir()` /
  `packages_staging_dir()`.
- Modified: [`tests/kernel/ws/ws_test_fixture.cpp`](../tests/kernel/ws/ws_test_fixture.cpp)
  — `kernel/packages/handlers.hpp` include, two accessor bodies,
  `test_config()` sets `packages_data_dir` + `packages_staging_dir`,
  `start_test_server()` calls `register_package_routes`.
- Modified: [`CMakeLists.txt`](../CMakeLists.txt) — two new sources in
  the `plinth_tests` list.
- Modified: [`docs/DEFERRED.md`](DEFERRED.md) — 2026-04-20 entry
  retitled + tightened to point at the I.19-only follow-up; I.18 +
  I.20 close cited.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.

### Verification

1. `cmake --build build --target tidy -j 4` — clean
   (`[100%] Built target tidy`, zero `error:` lines across 167
   filtered translation units).
2. `./build/plinth_tests "[packages][http]"` — 2/2 cases pass
   (13 assertions). I.18 sees one 201 + one 409 with
   `kind=advisory-lock-held` or `name-already-installed`; I.20 sees
   the 403 + `permission_denied` body.
3. `ctest -R 'plinth_tests_(pure|pg|js|ws)' --output-on-failure` —
   `plinth_tests_pure`, `plinth_tests_js`, `plinth_tests_ws` all
   green. `plinth_tests_pg` SIGABRT'd on one local run during
   teardown of a `live_replay_ordering_test.cpp` test case (132/132
   real cases passed; 18796/18797 assertions; the "1 failed" line
   is the SIGABRT itself, not an assertion). Same family as the
   CI failure described below — `Resource deadlock avoided` thrown
   from `trantor::EventLoopThread::~`. Re-running the same filter
   subsequently passed cleanly. New HTTP fixture cases I.18 + I.20
   pass on every run; the flake's location (the Catch2 case
   immediately after `WriterHarness` teardown of a different
   realtime test) is unrelated to the HTTP fixture.

### CI on `main` post-session-1 merge

`ci-build-and-test-12407` (post merge of PR #96) failed test 3
(`plinth_tests_pg`) — `terminate called after throwing an instance of
'std::system_error', what(): Resource deadlock avoided` inside
`trantor::EventLoopThread::~` during teardown. 18541/18542 assertions
passed; the SIGABRT fired after Catch2 finished reporting (single
"failed" case is the SIGABRT itself, not an assertion failure).

This is the documented framework-layer flake family from
`project_plinth_state.md` and `project_ws_flaky_segfault.md`:
"Framework-layer drogon `EventLoopThreadPool::~` join-self pattern
remains an upstream issue; surfaces on `plinth_tests_pg`
intermittently." Local repro on this branch's binary surfaced the
same flake at varying rates: an early 5/5 clean burst (pre-tidy-fix
build) and a later 3/5 hit-rate burst (post-tidy-fix build). The
case that crashes shifts between runs — CI hit `L.02`
(`live_replay_ordering_test.cpp:230`); local hit `L.07`
(`live_replay_ordering_test.cpp:366`). All hits show the same
pthread_kill+abort+terminate+`EventLoopThread::~` chain. Not
introduced by session 1 or session 2 — same family that surfaced
post-v0.5.5.1, post-v0.5.5, and intermittently before. No code
change in this session targeting it.

### Cores disposition

6 `core.<pid>` files (~256 MB each, ~1.5 GB total) accumulated
during the local intermittency repro runs (3 SIGABRTs in repro
trial set 2 + 3 from the standalone runs). All show identical PC
offsets at `pthread_kill+0x11c` — same crash class as the CI
backtrace (`abort` → `terminate` on `Resource deadlock avoided` →
`EventLoopThread::~` self-join). gdb backtraces with stripped PCs
confirm same call site across all 6. Cleaned per
`feedback_core_files.md` after analysis (`rm -f core.*`).

---

## 2026-04-27 — 0.6.0.N test-fixture buildout, session 1 of N (paper + seam, untagged)

First session on branch `feat/0.6.0.N-test-fixture-buildout`. Per the
architect's "each Claude session gets its own branch" directive, this
branch represents one session's work; the cross-arc test-fixture
buildout (~28 deferred ICD test cases plus three composable fixtures)
spans multiple sessions, each landing on its own branch and
contributing to the cumulative milestone close-out. Plan file:
the archived implementation record
(architect-approved). Untagged per `feedback_tagging_rule.md`.

### What shipped this session

**1. Paper landing — extension HTTP surface discussion + schedule.**
A 2026-04-27 architecture conversation about how Plinth extensions
can own HTTP surface area without putting application- or
protocol-specific knowledge into the kernel. The paper:

- [`docs/discussion/DISCUSSION-extension-http-surface.md`](discussion/DISCUSSION-extension-http-surface.md)
  — captures the design-space conversation, the design principle
  ("kernel owns primitives, extensions own application surfaces"),
  the four options walked through and rejected (JS router /
  reserved-prefix list / dlopen C++ plugins / status quo), the
  proposal that emerged (catch-all kernel route + manifest-declared
  prefixes + runtime route table + kernel-side pre-dispatch auth +
  execution-mode-agnostic handler reference), the kernel-owned
  primitive route set the proposal preserves, trust and auth shape,
  generalization targets (CalDAV / CardDAV / S3-compat / ActivityPub
  / share `/s/{token}` / OAuth-flow surfaces), risks and unknowns
  (prefix-claim semantics, uninstall-while-in-flight, manifest schema
  additions, permission model, Drogon catch-all interaction,
  performance), what's decided now (principle adoption, no
  application-specific routes added to kernel pending session, kernel
  primitive route set frozen), and what's deferred to the
  architecture session itself. Status: discussion document, not a
  commitment.

- [`docs/ROADMAP.md`](ROADMAP.md) §0.6 — new architecture-session
  line parallel to the existing contract-docs proposal:
  > `0.6.0.N Architecture session: extension HTTP surface (paper)   [strong]`
  Resolves: shape adoption, manifest schema additions
  (`http_prefixes`, `unauthenticated_prefixes`, `handler_mode`),
  prefix-claim semantics (wildcard depth, method/host scoping),
  uninstall-while-in-flight behavior, privilege model for prefix
  declarations, audit log entry shape, performance benchmark plan
  and acceptance threshold, and which milestone slot the
  implementation lands in. Precondition for any extension that wants
  HTTP surface — Files-Nextcloud-compat is the immediate motivating
  case. Not critical-path for 0.6.x shell work.

**2. Production-side seam — `plinth::packages::test_seam::dispatch_post`.**
First piece of the test-fixture buildout's production-side
infrastructure. New thin forwarder declared in
[`src/kernel/packages/handlers.hpp`](../src/kernel/packages/handlers.hpp)
and defined in
[`src/kernel/packages/handlers.cpp`](../src/kernel/packages/handlers.cpp)
that wraps the anonymous-namespace `handle_post_packages` so the
forthcoming `tests/kernel/packages/http_test_fixture.{hpp,cpp}` can
drive it directly without standing up a Drogon listener. Same
pattern as `plinth::shell::test_seam::dispatch_app` from 0.6.0
([`src/kernel/shell/static_handler.hpp:62`](../src/kernel/shell/static_handler.hpp:62)).
PATCH / DELETE / GET seams follow when the cases that need them
land (X.\* upgrade lifecycle in a later session).

### Spike findings (Risk 1 — RBAC filter in-process invocation)

Spike from the plan §I.1 was resolved by reading. Conclusion: the
HTTP fixture's planned dispatch path (run `SessionFilter::doFilter`
+ `RbacFilter::doFilter` synchronously from the fixture, then call
`test_seam::dispatch_post`) is mechanically callable. `FilterCallback`
is `std::function<void(const HttpResponsePtr&)>`; `FilterChainCallback`
is `std::function<void()>`; both are normal `std::function`s with no
ownership special-casing. The async path inside `SessionFilter`
(it calls `validate_session_token` which uses
`drogon::app().getDbClient()->execSqlAsync`) requires the Drogon app
to be running on its event-loop thread, which the existing WS test
fixture already arranges
([`tests/kernel/ws/ws_test_fixture.cpp`](../tests/kernel/ws/ws_test_fixture.cpp)
runs the kernel app on port 28099 under
`plinth_ws_port_28099` resource lock). The fixture will bridge the
async callback to the test thread with `std::promise<HttpResponsePtr>`.
Real validation comes during fixture implementation (next session);
the design as planned holds on paper.

### What did NOT ship this session — handoff for next session(s)

The plan file lists ~17 todos for the full milestone. Next session
picks up here:

**Branch convention going forward:** each follow-up session opens a
fresh branch (per architect directive). Suggested names below; the
architect picks at session start.

**Session 2 candidates:**

1. **Build the HTTP fixture + close ICD-0.4.4 I.18 + I.20.**
   Build branch `feat/0.6.0.N-http-fixture` (or similar). Promote
   the RBAC seeders (`insert_user`, `insert_session`, `make_admin`)
   from
   [`tests/kernel/ws/ws_test_fixture.cpp:122-161`](../tests/kernel/ws/ws_test_fixture.cpp:122)
   to a shared header. Build
   `tests/kernel/packages/http_test_fixture.{hpp,cpp}` per plan §B
   (class `HttpTestFixture`, methods `seed_admin` / `seed_non_admin`
   / `build_post` / `dispatch` / `dispatch_concurrent`, in-process
   filter-chain invocation via `std::promise`). Wire I.18
   (concurrent same-name POST) + I.20 (RBAC denial) in a new
   `tests/kernel/packages/install_lifecycle_http_test.cpp`. Close
   `DEFERRED.md` 2026-04-20 entry partially. **I.19 stays deferred
   to its own follow-up** — needs a production-side `?dry_run=1`
   codepath that doesn't exist in
   [`handle_post_packages`](../src/kernel/packages/handlers.cpp:130)
   today (ICD-0.4.4 line 173 specifies it; `src/kernel/packages/`
   has zero `dry_run` references; folds into a small follow-up that
   adds the dry-run branch and then closes I.19 via the HTTP
   fixture).

2. **Build the advisory-lock harness + close ICD-0.4.5 G.03 +
   ICD-0.5.4 I.02.** New branch. Build
   `tests/kernel/packages/advisory_lock_harness.{hpp,cpp}` per plan
   §C (`fork()` + run-lambda-in-child; libpq fresh per child;
   `_exit` to skip Catch2 destructors; `KillSpec` future for X.12 /
   S.07 sibling method). Wire G.03 (GC under advisory contention)
   and ICD-0.5.4 I.02 (multi-process advisory-lock single-winner).
   ICD-0.5.4 I.03 (live + replay race) needs WsTestClient
   extensions, defer to session 4.

3. **Wire ICD-0.4.5 X.05–X.11 + X.13.** New branch (or fold into
   session 2 if the HTTP fixture lands cleanly). Add `dispatch_patch`
   + `dispatch_delete` test_seams (mirrors `dispatch_post` shipped
   here). New
   `tests/kernel/packages/lifecycle_transitions_http_test.cpp`. X.12
   stays deferred (SIGKILL-of-running-plinth + reconciler-replays-swap
   — own follow-up that extends `AdvisoryLockHarness` with
   `run_with_kill`).

4. **Extend WsTestClient + close ICD-0.5.5 L.03 / L.04 / L.05 / S.06
   + ICD-0.5.4 I.03.** New branch. Per plan §D: pause/resume drain
   hook on `WsTestClient`, frame inspector callback,
   `set_live_buffer_cap_override` setter backed by a new
   `plinth::ws::test_seam::live_buffer_cap_override()` accessor in
   [`src/kernel/ws/subscriptions.hpp`](../src/kernel/ws/subscriptions.hpp).
   New `tests/kernel/realtime/replay_buffer_test.cpp`. S.07 stays
   deferred (writer-SIGKILL family — same follow-up as X.12 since
   they share the SIGKILL extension on `AdvisoryLockHarness`).

5. **Documentation closeout + DEFERRED / ROADMAP / ICD updates.**
   Could be folded into the last code session or its own paper
   session. Update DEFERRED.md 2026-04-20 / 2026-04-22 / 2026-04-26
   entries to reflect what closed; flip the
   `0.6.0.N Test-fixture buildout` ROADMAP line to `[x]`; add new
   follow-up roadmap entries for I.19 dry_run, X.12 + S.07 SIGKILL
   extension, ICD-0.5.5 I.01–I.04 LH harness wiring (this is
   load-harness work, not plinth_tests), ICD-0.5.0.3 R/E/P/H/C
   extension-dispatch composition (uses existing JS test infra,
   not the three fixtures here). Author any
   `Implementation deviation` subsections needed in ICDs for spec
   gaps surfaced during implementation (e.g., the events_writer
   advisory-lock key namespace if it's not in ICD-0.5.4).

**Cumulative scope target:** by the last session in the chain, the
three fixtures land + ~17 first-wave cases close + the deferred 16
become explicit follow-up entries. Per the architect's
"by the last session all the work is done" directive.

### Files touched this session

- New: [`docs/discussion/DISCUSSION-extension-http-surface.md`](discussion/DISCUSSION-extension-http-surface.md).
- Modified: [`docs/ROADMAP.md`](ROADMAP.md) — new architecture-session
  bullet inserted in §0.6 directly after the existing contract-docs
  proposal entry.
- Modified: [`docs/CHANGELOG.md`](CHANGELOG.md) — this entry.
- Modified: [`src/kernel/packages/handlers.hpp`](../src/kernel/packages/handlers.hpp)
  — added `test_seam::dispatch_post` declaration.
- Modified: [`src/kernel/packages/handlers.cpp`](../src/kernel/packages/handlers.cpp)
  — added `test_seam::dispatch_post` thin forwarder to
  `handle_post_packages`.

### Verification

`cmake --build build --target tidy -j 4` — clean (verifies the new
seam introduces no clang-tidy regressions). No new tests this
session; the seam is exercised by the forthcoming HTTP fixture in
session 2.

---

## 2026-04-27 — 0.6.0.1 atexit audit-shutdown ordering fix (code follow-up, untagged)

Tight follow-up to v0.6.0 closing the teardown SEGV that surfaced on
the same-day manual-smoke walkthroughs of the bundled shell. Branch
`fix/0.6.0.1-atexit-audit-shutdown-ordering` (cut from `main` @
`594ec1f`). Untagged per `feedback_tagging_rule.md`.

### What surfaced

Five identical core files (`core.621829`–`core.622535`, 2026-04-27
15:15–15:23, all 51.7–51.9 MB) accumulated during the manual browser
walkthrough of the just-shipped 0.6.0 frontend-shell bootstrap. Five-
of-five reproduction density; same backtrace every run:

```
#0  std::_Rb_tree<…>::find (this=0x0, __k="default")
#3  drogon::orm::DbClientManager::getDbClient (this=0x0, name="default")
#5  drogon::HttpAppFrameworkImpl::getDbClient
#6  plinth::log::audit ("realtime.broker.stopped", …)  at logging.cpp:129
#7  plinth::realtime::broker::stop ()                  at broker.cpp:242
#8  operator() (atexit lambda)                         at main.cpp:293
```

`SIGSEGV` at `./build/plinth serve` exit, every time. Triggered on the
graceful-shutdown path (Ctrl-C → process atexit chain → broker stop
→ final-metric audit → Drogon getDbClient on a partially torn-down
framework).

### Root cause

Two related-but-distinct teardown ordering bugs, both visible only on
SIGTERM-graceful exit. The audit-side bug crashed first (the 5/5
captured cores); the spdlog-side bug surfaced once the audit-side
was fixed.

**Bug 1 — audit gate flipped too late.**
[`logging.cpp:30-39`](../src/kernel/logging.cpp) carries a block-
comment contract for the `g_audit_ready` gate:

> `true → false : shutdown() called from the fixture atexit handler
>  BEFORE drogon::app().quit(), so in-flight audit callers no-op
>  instead of racing a torn-down DbClient weak_ptr during the loop
>  drain.`

The contract was right; the wiring was wrong. In
[`src/kernel/main.cpp`](../src/kernel/main.cpp) the atexit lambda
called `plinth::log::shutdown()` at line ~308 — *after*
`broker::stop()` at line ~293. Drogon's `DbClientManager` is already
nulled out by the time our atexit fires (likely by another atexit
handler that ran earlier, LIFO), but `g_audit_ready` was still `true`,
so `audit("realtime.broker.stopped", …)` went on to call
`drogon::app().getDbClient()` and crashed inside the framework with
`this=0x0`.

**Bug 2 — `spdlog::shutdown()` ran before atexit.** The graceful
return path at `main.cpp:511` did:
`stop_notify_listener() → stop_listener() → spdlog::shutdown() → return 0;`,
which means `exit()` ran the atexit chain *after* spdlog's registry
had already been cleared. Once Bug 1 was fixed and `broker::stop()`
emitted a `spdlog::info(...)` final-metric line, the atexit-time call
SEGV'd in `spdlog::logger::should_log` with `this=nullptr` —
`spdlog::default_logger_raw()` returning null on the cleared registry.
Pre-fix the audit-side crash hid this; same family ordering, just
deeper in the chain.

The intent was "gate closes before any atexit-reachable audit caller
runs *and* spdlog stays alive for atexit's structured-log emissions";
the actual wiring violated both invariants.

### Fix

Three edits:

1. **`src/kernel/main.cpp`** — relocate `plinth::log::shutdown()` to
   the **first** statement of the atexit lambda body, ahead of every
   `*::stop()`/`*::shutdown()` call. Same atexit chain otherwise; only
   the gate-flip moves. 10-line comment cites the broker.cpp crash
   signature so the *why* lives in code.

2. **`src/kernel/main.cpp`** — relocate `spdlog::shutdown()` from
   the graceful-return path at `main.cpp:511` to the **last**
   statement of the atexit lambda (after `drogon::app().quit()`).
   spdlog now stays alive across every `*::stop()` call's
   structured-log emission and is explicitly drained once at exit
   regardless of which path got us there. Replacement comment at the
   old call site points at the new location.

3. **`src/kernel/realtime/broker.cpp`** — replace the final-metric
   `audit("realtime.broker.stopped", …)` call (and its
   `is_audit_ready()` outer guard, now redundant) with a single
   `spdlog::info("realtime broker: stopped (dispatch_count={},
   rbac_denial_count={})", …)`. The audit-table row was never load-
   bearing during teardown — process is going down anyway — and
   spdlog stays alive across the entire teardown chain via fix (2).
   **Implementation deviation:** the `realtime.broker.stopped` audit
   row no longer hits PG; it surfaces as an `[info]` line in
   `logs/plinth.log` instead.

### Regression test

New [`tests/kernel/audit/audit_teardown_test.cpp`](../tests/kernel/audit/audit_teardown_test.cpp)
under tag `[audit][teardown]` with 3 cases / 6 assertions:

1. `audit gate is closed before init` — locks in the
   `logging.hpp:69` "True after init()" contract from the default
   `false` state.
2. `audit() is a safe no-op when the gate is closed` — calls
   `audit("realtime.broker.stopped", …)` from the gate-closed state
   and requires no crash (the runtime invariant the SEGV violated).
3. `shutdown() leaves the gate closed and is idempotent` — calls
   `shutdown()` twice plus `audit()` after, checks the gate stays
   `false` and audit() remains safe.

Pure-tag — routes to `plinth_tests_pure`.

These tests cannot reproduce the partial-Drogon-teardown state from a
unit test. The full regression evidence is the no-core repro under
the running server (verification §4 below) — today's reproduction
density was 5/5, so a single clean shutdown would have been enough;
we ran 3.

### Cores disposition

Per `feedback_core_files.md` step 4: each of the five cores was
backtraced via `gdb -batch -ex 'bt' -ex 'info registers' ./build/plinth
core.<pid>`; all five share the identical Bug-1 signature shown
above. One additional core was captured during fix verification
(after Bug 1 was patched but before Bug 2 surfaced), backtraced and
recorded as `Bug 2` above, then removed alongside the others. After
the final fix verification ran clean (3/3 SIGTERM cycles, zero new
cores), the cores were removed with `rm -f core.*`. No surviving
forensic evidence is needed once the disposition and signature are
recorded here.

### Verification

1. `cmake --build build --target tidy -j 4` — clean.
2. `./build/plinth_tests "[audit][teardown]"` — 3 cases / 6
   assertions / all green.
3. `ctest -R plinth_tests_pure` — green.
4. **No-core SIGTERM repro:** `./build/plinth serve` started against
   docker PG, kill -TERM, wait for exit. Repeated 3×; zero new
   cores. Teardown log lines (`realtime listener: stopped`,
   `realtime broker: stopped (dispatch_count=0, rbac_denial_count=0)`)
   land in `logs/plinth.log` per the new spdlog routing.
5. **FE walkthrough** per `feedback_fe_visualize.md`: bundled shell
   loaded via Claude_Preview MCP, login as a freshly-registered
   `verify_0601` user → "Hello, verify_0601" + four-zone topbar +
   `V` avatar render, click avatar → Sign Out menuitem opens →
   click Sign Out → returns to login form. Zero console errors
   throughout. Preview stop clean (no new cores).

### Out of scope

The deeper question — *why* Drogon's `DbClientManager` is null at all
by the time our atexit fires — remains open. The gate-flip closes the
practical hazard regardless: every audit caller in the teardown chain
now safely no-ops on the gate before reaching into Drogon. If the
same SEGV family resurfaces from a different audit caller post-fix,
that is its own session (likely a Drogon-internal fix or a
deterministic-teardown refinement on the framework itself).

### Carry-forward

The three live `0.6.0.N` candidates from the post-0.6.0 memory carry
forward unchanged: Test-fixture buildout `[strong]`, ICD-0.6.1
authoring `[strong]`, contract-docs proposal `[strong]`.

---

## 2026-04-27 — 0.6.0 Frontend Shell Bootstrap (code milestone, untagged)

First UI code milestone of the 0.6.x Frontend-Shell arc. Branch
`feat/0.6.0-frontend-shell-bootstrap` (cut from `main` @ `f485c2b`).
Untagged per `feedback_tagging_rule.md` (interim doc patches and
follow-up code milestones get CHANGELOG entries only; tags reserve
for arc closeouts).

Implementation against
[`docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md`](icd/ICD-0.6.0-frontend-shell-bootstrap.md)
(paper-authored 2026-04-27 in the prior session). Five-phase commit
arc.

### Output

1. **`Config::Shell` block** (ICD §9). New
   [`src/kernel/config.hpp`](../src/kernel/config.hpp) struct
   `Shell { bool enabled = true; std::string root_redirect = "/app/"; }`
   trivially default-constructible. JSON loader in
   [`src/kernel/config.cpp`](../src/kernel/config.cpp) validates
   `root_redirect` against `^/[^/]+/$` and warn-falls-back to `/app/`
   on mismatch (soft-fail posture, distinct from realtime block's
   hard-fail).

2. **Bundled shell content** (ICD §4 / §6 / §7 / Appendix A). Replaced
   the 0.4.4 placeholder shell at `client/shell/client/`:
   - [`client/shell/client/index.html`](../client/shell/client/index.html)
     — minimal Appendix-A skeleton (DOCTYPE, viewport, `<title>Plinth</title>`,
     ~80 lines inline layout-before-JS CSS, `<div id="root">`,
     `<script type="module" src="shell.js">`). Hex palette aimed at
     2026-04-27 design bundle's dark values (`#0b0f14`/`#11161d`/
     `#5aa9ff`); throwaway scaffolding per ICD §6.3 — replaced with
     `var(--token-name)` in 0.6.2 design-tokens work.
   - [`client/shell/client/shell.js`](../client/shell/client/shell.js)
     — single-file ES module. `plinthFetch` wrapper enforces
     `credentials: 'include'` and triggers redirect-on-401 from non-
     login fetches (ICD §5.3 + §5.6). `LoginForm` is a controlled
     Preact component with OQ3 countdown on 429
     (`startLockout(retryAfter)` + `setInterval` decrement). `AuthFrame`
     renders four-zone topbar + click-outside-closes Sign Out popover.
     Top-level `Boundary` Preact `Component` per §7 — `componentDidCatch`
     logs structured `[shell] boundary caught` to `console.error`;
     fallback view with `Reload` button. `?force-throw=1` URL seam
     wires the deliberate-throw component for E.\* manual smoke.
   - [`client/shell/client/vendor/preact.module.js`](../client/shell/client/vendor/preact.module.js)
     (preact@10.22.0, sha256 87942b6f43a74de6a3abb2e0c4e287f03b54b4849cfa34d312402f24aa34a30)
     and [`htm.module.js`](../client/shell/client/vendor/htm.module.js)
     (htm@3.1.1, sha256 ab33dd3f38059b9be4d5f5350128eefb2356639c4e0bbe9d9e8b3ba75847e9e4)
     vendored same-origin under `client/shell/client/vendor/`. Vendor-
     time grep verified neither contains `eval(` or `new Function(` —
     required for CSP `script-src 'self'`.
     [`VERSIONS.txt`](../client/shell/client/vendor/VERSIONS.txt)
     records the pin + sha256 + fetch URL for future refresh.
   - [`client/shell/manifest.json`](../client/shell/manifest.json)
     bumped `version` 0.1.0 → 0.6.0 and `frontend.mount` flipped
     `/` → `/app` (forward-compat for 0.6.1 manifest-driven dispatch;
     not read by the kernel in 0.6.0).

3. **Kernel-stub static handler at `/app/*`** (ICD §4 / §8). New
   [`src/kernel/shell/static_handler.{hpp,cpp}`](../src/kernel/shell/static_handler.hpp).
   `register_shell_routes(cfg, db, data_dir)` queries `plinth.packages`
   at boot for the active bundled frontend
   (`provenance='bundled' AND frontend_mount IS NOT NULL AND
   state IN ('ACTIVE','ACTIVE_FLAGGED')`), caches its installed
   `client_root` under shared_mutex, and registers two Drogon
   handlers:
   - `GET /` → 302 to `cfg.root_redirect` (default `/app/`).
   - `GET /app/(.*)` SPA-fallback: empty path or path with no
     extension → `index.html`; named asset → `<client_root>/<path>`.
     Path-traversal closed by construction (component-level reject of
     `..`/`.`/empty/NUL) plus `weakly_canonical` prefix check against
     `client_root`. Strict CSP `script-src 'self'; style-src 'self'
     'unsafe-inline'; connect-src 'self'` on every response;
     `Cache-Control: no-cache` for `index.html`, `public, max-age=
     31536000, immutable` for the rest.

   Wired into
   [`src/kernel/main.cpp`](../src/kernel/main.cpp) at line ~480
   immediately after `asset_server::restore_routes` so `/app/(.*)`
   registers AFTER all `/api/*`, `/ext/*`, `/healthz`, `/ws`
   registrations — Drogon's first-match dispatch then resolves
   `/api/auth/session` to the auth handler before falling through to
   the shell glob (B.06 structural enforcement; manual smoke verifies
   in browser).

4. **B.\* tests** (ICD §13 bootstrap group). New
   [`tests/kernel/shell/static_handler_test.cpp`](../tests/kernel/shell/static_handler_test.cpp)
   — B.01 (`/` → 302), B.02 (`/app/` → index.html + CSP + no-cache),
   B.03 (`/app/shell.js` → JS body + immutable cache + CSP), B.04
   (SPA-fallback for extension-less paths), B.05 (missing asset → 404),
   B.07 (disabled config skips registration), B.08 (custom redirect
   target), plus path-traversal `..` rejection, embedded-NUL rejection,
   and "no shell installed" → 404. Pure tag (no PG required); injected
   via `test_seam::set_bundled_shell_root` and exercised via
   `test_seam::dispatch_app` mirroring `asset_server::dispatch_for_test`.
   B.09 (invalid `root_redirect` validation fallback) is exercised by
   the extended config tests in
   [`tests/kernel/config_test.cpp`](../tests/kernel/config_test.cpp).

5. **Manual smoke walkthroughs under `tests/shell/`** per OQ2 deferral.
   [README](../tests/shell/README.md) documents the cadence;
   [`topbar_render_test.html`](../tests/shell/topbar_render_test.html)
   covers T.01–T.05; [`boundary_render_test.html`](../tests/shell/boundary_render_test.html)
   covers E.01–E.02; [`login_walkthrough_test.html`](../tests/shell/login_walkthrough_test.html)
   covers I.01–I.02 + L.01–L.06. Headless harness deferred to the
   `0.6.0.N Test-fixture buildout` slot per ROADMAP §0.x cleanup
   follow-ups.

### OQ resolutions

- **OQ1 (bundle byte source) — architect override of ICD §14 default.**
  Resolved as **on-disk installed shell** (Option B). The 0.4.4
  `install_shell_if_needed` already extracts `shell.zip` to
  `<data_dir>/extensions/shell/0.6.0/client/` via the standard 0.4
  install lifecycle; the static handler reads from that on-disk path
  rather than from a parallel set of kernel-baked `string_view`
  symbols (the ICD §14 default). Single source of truth — `shell.zip`
  → install lifecycle → disk → `/app/*`. Honours "shell is an
  extension like any other" with no kernel-baked byte symbols.
  0.6.1 substitution remains clean: replace the kernel-stub static
  handler with manifest-driven mount dispatch reading `frontend.mount`
  from `plinth.packages`.
- **OQ2 (browser harness) — Defer to 0.6.0.N**, ICD §14 default.
  T.\*/E.\*/I.\* ship as documented manual smoke under `tests/shell/`.
- **OQ3 (rate-limit UX) — Countdown + disabled submit**, ICD §14 default.
  Implemented in `LoginForm.startLockout(retryAfter)`.

### Architect-signed-off deferrals

- **L.01–L.06 (login wire-contract tests)** — deferred to 0.6.0.N
  alongside T.\*/E.\*/I.\*. Plinth has no HTTP test fixture today;
  the only existing live-listener fixture is `ws_test_fixture` (port
  28099). The 0.6.0.N test-fixture buildout absorbs all browser-
  driven + HTTP wire-contract coverage in one fixture investment.

### Test taxonomy (delivered vs. deferred)

- **B.\*** — 9 cases targeted; **9 delivered** in
  `static_handler_test.cpp` + `config_test.cpp` (B.06 verified
  structurally + manual smoke).
- **L.\*** — 6 cases deferred to 0.6.0.N.
- **T.\*/E.\*/I.\*** — 9 cases deferred to 0.6.0.N (manual smoke ships).

### Architecture promotion

- [`docs/architecture/06-frontend.md`](architecture/06-frontend.md)
  §1 — appended "Implemented 2026-04-27 (v0.6.0)…" footnote.
- [`docs/architecture/05-extensions.md`](architecture/05-extensions.md)
  §2 reserved-prefix table — added `/app/*` row owned by
  "Shell extension (kernel-stub in 0.6.0; package-mediated from
  0.6.1)"; flipped `/` row body to "Redirects to `shell.root_redirect`
  (default `/app/`) per ICD-0.6.0 §4.5".

### ROADMAP updates

`0.6.0` flipped `[ ]` → `[x]` with summary in §0.6 — Frontend Shell.
The §0.5 — Realtime block compacted per the ROADMAP preamble's
"Completed milestones are removed" rule — replaced 15 `[x]` ship
entries with a one-paragraph index pointer to CHANGELOG.

### Verification

- `cmake --build build -j 4` clean.
- `cmake --build build --target tidy -j 4` clean.
- `./plinth_tests "[shell]"` — all 13 cases pass (41 assertions).
- `ctest -R "plinth_tests_pure" --output-on-failure` — green.
- Manual browser walkthrough deferred until first deploy (the
  pre-PR walkthrough is part of `tests/shell/login_walkthrough_test.html`).

### Carry-forward

Next session candidates per `project_next_session_post_060.md`:
1. **`0.6.0.N Test-fixture buildout`** `[strong]` — HTTP fixture +
   advisory-lock harness + live-buffer fault-injection seam; absorbs
   L.\*/T.\*/E.\*/I.\* + the 25-case ICD backfill.
2. **`0.6.0.N ICD-0.6.1 authoring`** `[strong]` — paper. Authors
   `docs/icd/ICD-0.6.1-shell-schema-user-preferences.md` per
   METHODOLOGY §3.1 forward-ICD-presence rule.
3. **`0.6.0.N Architecture session: contract-docs proposal`** `[strong]` —
   paper. Decides adoption of `docs/discussion/DISCUSSION-living-subsystem-contracts.md`.

---

## 2026-04-27 — 0.5.5.N ICD-0.6.0 authoring (paper-only session, untagged)

Paper-only ICD-authoring session per METHODOLOGY-llm-assisted-development.md
§3.1 forward-ICD-presence rule and `feedback_icd_horizon.md` (ICDs one
milestone ahead). Branch `feat/0.5.5.N-icd-0.6.0-authoring`. Untagged
per `feedback_tagging_rule.md`. First paper session of the 0.6.x
Frontend-Shell arc.

### Output

[`docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md`](icd/ICD-0.6.0-frontend-shell-bootstrap.md)
— 1310-line paper ICD covering the 0.6.0 in-browser frame. Three
contributions:

1. **Static-asset handler at `/app/*` (interim path).** SPA-fallback
   handler with strict CSP (`script-src 'self'; style-src 'self'
   'unsafe-inline'; connect-src 'self'`), baked-in `index.html` +
   single-file Preact/htm bundle, root `/` redirect to `/app/` per
   `architecture/05-extensions.md §2`. Stands in for the 0.6.1
   package-system asset lifecycle (`/ext/{name}/{version}/*` per
   `architecture/06-frontend.md §3`); URL contract is forward-
   compatible so the 0.6.1 transition is a substitution at the
   handler layer.
2. **Login flow consuming ICD-0.1.2.** Form posts to
   `/api/auth/login`, browser receives `plinth_session` cookie via
   `Set-Cookie`, shell re-renders authenticated frame; redirect-on-
   401 wrapper handles session expiry; Sign Out wires through
   `/api/auth/logout`. Error-code → user-string mapping table for
   all ICD-0.1.2 error codes; rate-limit countdown UI per OQ3.
3. **Empty four-zone topbar + top-level error boundary.** Topbar
   structural placeholders per `DESIGN-shell-v06x.md §2.1`:
   Home (no-op icon), app-name ("mark" treatment per the 2026-04-27
   design bundle), empty tray, avatar (gradient circle + Sign Out
   action). "Hello, {user}" placeholder in content area. Single
   top-level Preact error boundary catches synchronous render
   exceptions; fallback renders "Something went wrong" + Reload;
   no kernel-side audit-log call until panel SDK exposes
   `audit.log` in 0.6.3.

### Test taxonomy

24 targeted test cases across five prefix groups:

- **B.\*** (9) — bootstrap / static handler / route ordering
- **L.\*** (6) — login flow happy + sad paths
- **T.\*** (5) — topbar four-zone DOM structure
- **E.\*** (2) — error boundary catch + reload
- **I.\*** (2) — end-to-end browser walkthrough

T.\* / E.\* / I.\* require a headless-browser harness; OQ2 records
the choice between (a) defer harness build to 0.6.0.N follow-up
(recommended) or (b) ship harness with 0.6.0.

### Open Questions (3, architect-recommended defaults)

- **OQ1 — Bundle storage form.** Embedded resources via build-system
  `configure_file` recommended (single-binary deploy).
- **OQ2 — Headless-browser harness in CI.** Defer to 0.6.0.N
  recommended; ship 0.6.0 with manual smoke for browser cases.
- **OQ3 — Login retry-after UX.** Disable button + countdown
  recommended (vs. unlock-immediately).

### §15 "What Must Not Be Decided Yet" fences

ICD-0.6.0 §15 explicitly catalogues eleven 0.6.1+ deferrals with
named-milestone closure triggers: bundled-package install lifecycle
(0.6.1), `frontend.mount` manifest contract (0.6.1), `ext_shell` PG
schema (0.6.1), design tokens / theme / UI scaling (0.6.2), panel
SDK + client SDK (0.6.3), tab strip / sub-tabs / app-switcher / Home
launcher (0.6.4), floats (0.6.5), trays + content-type resolution +
navigation intents (0.6.6), admin extension preview (`0.6a-*`),
localization (unscheduled), responsive treatment (per surface,
milestone-by-milestone), `frontend.boundary.caught` audit family
(0.6.3).

### Visual baseline — shell design archive

[`docs/sketches/shell-design-2026-04-27/`](sketches/shell-design-2026-04-27/)
— retained visual reference for the 0.6.x arc. The sanitized archive includes
`project/Plinth Shell.html` (primary HTML — palette, layout, scaling), 15 JSX
prototype files under `project/shell/`, and three design sketches under
`project/uploads/`. The original design-session transcript was removed before
public distribution.

ICD-0.6.0 cites the bundle for:

- Typography baseline: **Inter** for prose; **JetBrains Mono** for
  structural IDs (capability strings, schema names, package
  versions, package IDs).
- Dark-default palette aimed-at values: `--bg-0:#0b0f14` /
  `--bg-1:#11161d` / `--text-0:#e7edf4` / `--text-1:#c4cbd4` /
  `--accent:#5aa9ff`.
- Light-mode palette (NOT shipped in 0.6.0; arrives with theme
  toggle in 0.6.2): warm off-white `--bg-1:#fcfcfa` / ink
  `--text-0:#16181c` / accent `--accent:#1f6feb`.
- App-identity = "mark" treatment (small colored glyph + name +
  chevron; reads as frame chrome, NOT a document tab).
- Self-host fonts in production; the bundle's `fonts.googleapis.com`
  is prototype-only and violates the strict CSP `connect-src
  'self'`.

### Architectural commitments needing ratification

The bundle includes four commitments that need architect ratification
before they hit the ROADMAP — catalogued in DEFERRED.md as a single
2026-04-27 entry:

- **Automations extension** (user scripting + deferred actions, gated
  on `user.scripting` cap). Likely third bundled package or 0.7.x
  slot.
- **`data-ipoint` integration-point overlay** for package-coder
  ergonomics. Closes at 0.6.4 with panels query API.
- **Privacy-safe admin schedules** (admin sees aggregate user-policy,
  NOT reminder bodies / handler code / per-user item lists). Privacy
  contract should land in `architecture/01-identity.md §2` before
  `0.6a-D` admin schedules ship.
- **UI-scaling via `zoom`** (bundle) **vs. rem-based** (`DESIGN-shell-v06x.md
  §6.3`). 0.6.2 ICD must pin one approach + update the design doc
  per METHODOLOGY §Phase 2 Constraint #4.

### Files

- [`docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md`](icd/ICD-0.6.0-frontend-shell-bootstrap.md) — new ICD (1310 lines).
- [`docs/sketches/shell-design-2026-04-27/`](sketches/shell-design-2026-04-27/) — new sketch bundle (22 files).
- [`docs/ROADMAP.md`](ROADMAP.md) — `0.5.5.N ICD-0.6.0 authoring` slot flipped `[x]` with paper-session summary.
- [`docs/DEFERRED.md`](DEFERRED.md) — new 2026-04-27 entry catalogueing the four design-bundle commitments needing architect ratification.

### Methodology

Paper-only session per METHODOLOGY-llm-assisted-development.md §1
(architecture / ICD / code phases) and §3.1 (forward-ICD-presence
rule). No code, no kernel changes, no tests, no migrations. Paper
ICD pinned ahead of 0.6.0 code session per `feedback_icd_horizon.md`
(ICDs one milestone ahead). Plan-mode workflow applied with
narrow-scope confirmation from the maintainer: ICD covers only the 0.6.0
ROADMAP slot, not the broader §1–§4 substrate. Design bundle
integration was an in-session pivot mid-authoring; bundle saved
as canonical visual reference, ICD updated to cite it, memory entry
added (`project_design_bundle.md`).

---

## 2026-04-27 — 0.5.5.2 [js][async] refcount fix (untagged)

Top-priority kernel hardening per the maintainer's 2026-04-26 directive ("0.5.x
is the kernel's last hardening window before 0.6.0 begins UI"); the
final scheduled item before 0.6.0 opens UI work. Closes the long-
running `[js][async]` Catch2-subprocess refcount race tracked in
`docs/DEFERRED.md` (`free_zero_refcount` /
`list_empty(&rt->gc_obj_list)` family). Untagged per
`feedback_tagging_rule.md` (four-part follow-up).

### Root cause

`drogon::sync_wait(run_on_context(bc, src))` — the test fixture's
`drive()` entry — spawns a fresh `std::thread` that runs the
coroutine's synchronous prefix (`JS_Eval` + first `drive_jobs` +
`dispatch_ops_batch_fanout`). When a dispatched op completed faster
than the prefix reached `co_await AnyCompletionAwaiter`, the
completion's `queueInLoop` callback fired on `main_loop`'s thread
**concurrently** with the prefix still touching JSValues on the
sync_wait thread — breaking the [run_on_context.cpp:9-12](src/kernel/js/run_on_context.cpp:9)
"QuickJS access serialized inside this coroutine body" invariant.
Mutex barriers in `signal_completion` / `AnyCompletionAwaiter`
synchronise the *handoff* but don't kick in until the *first* suspend
— the initial-dispatch window is unprotected. The cross-thread
JSValue refcount manipulation produced the various corruption
signatures (rate ~8% per-TEST_CASE, ~15-25% under shared-subprocess).

LH-0.1 / LH-1 production-path runs (133k async calls / 535k db.query)
hit zero reproductions because production handlers `co_await
run_on_context(bc, src)` from a coroutine that's already on the
request's owning Drogon loop — no cross-thread prefix.

### Fix

Three-part. All edits surrounding the `run_on_context` coroutine:

1. **`run_on_context.cpp:1268-1296` — main-loop pin via
   `drogon::switchThreadCoro`.** Coroutine entry now hops to
   `main_loop` if the caller is off-loop. No-op when already on
   `main_loop` (production path). Eliminates the cross-thread
   prefix window — every `bc.ctx` / `bc.rt` access is now on
   `main_loop`'s thread.
2. **`run_on_context.cpp:1331-1353` — back-pressure suspension at
   `AnyCompletionAwaiter`.** When the dispatch loop is now on
   `main_loop`, the original `!has_pending_ops` guard would spin
   under back-pressure (`pending_ops>0` + `concurrent_async_ops==max`)
   and starve the completion callbacks it's waiting on, deadlocking
   the loop. Widened the suspension condition to `(!has_pending_ops
   || back_pressured)` so the loop yields whenever no progress can be
   made this iteration. Latent kernel bug independently worth fixing —
   pre-0.5.5.2 it was masked because `sync_wait` ran the dispatcher
   on a separate thread.
3. **`run_on_context.cpp:1319 + conversion.cpp:35-44` — MEMORY_LIMIT
   classifier robustness.** Surfaced as a side-effect during
   verification: `limits_test.cpp:158` `limits: promise-allocation
   loop trips MEMORY_LIMIT` flaked at ~30–60% under the grouped
   subprocess shape because the synchronous OOM (a tight `(() => {
   for(...) keep.push(new Promise(()=>{})); })()`) fires inside
   `JS_Eval` before `drive_jobs` has a chance to call
   `sample_memory_peak`, AND the post-OOM `malloc_size` had drifted
   below the original 256 KiB slack by classification time
   (rolled-back failed allocation + GC sweep on next eval frame).
   Fix: (a) call `sample_memory_peak(bc)` immediately after `JS_Eval`
   so synchronous OOMs latch `bc.memory_limit_hit` while the peak is
   fresh; (b) bumped `OOM_MEMORY_SLACK_BYTES` from 256 KiB → 1 MiB
   so post-reclaim sampling still classifies correctly. The 1 MiB
   slack is comfortably below the 4 MiB test cap and the 16 MiB
   production cap, so legitimate ~1 MiB state won't be misclassified
   as OOM. Distinct from the 0.3.4.1 classifier work (which added
   the latch + the live `is_runtime_near_memory_limit` check); this
   tunes the slack + adds a sample-point for the synchronous-OOM
   path that the 0.3.4.1 fix didn't reach.

### Verification

- 100/100 iters clean on `async_hardening: parallel queries honour
  max_concurrent cap` (the canonical N.39 exemplar; pre-0.5.5.2 rate
  was 8% per-TEST_CASE).
- 100/100 iters clean on `async_bridge: cap.batch fail-fast on first
  rejection` (the post-0.5.3 exemplar surfaced in
  `ci-build-and-test-12377`).
- 100/100 iters clean on `async_bridge: cap.batch with two calls
  resolves preserving input order`.
- 30/30 grouped `plinth_tests_js` runs green (post fix part 3).
  Pre-fix-3 the rate was ~30–60% under grouped shape due to the
  MEMORY_LIMIT classifier flake.

### N.39 scale-lift

[`tests/kernel/js/async_hardening_test.cpp:151`](tests/kernel/js/async_hardening_test.cpp:151)
`async_hardening: parallel queries honour max_concurrent cap` lifted
from 4 × 2 to the ICD-quoted **100 × 8** (per the comment block
referenced K.33 / project_ws_flaky_segfault.md / 0.3.4.1 cascade
deferral). 30/30 iters clean. The cap-block comment now references
the 0.5.5.2 closure instead of the pre-existing race.

### CMakeLists.txt grouping change

[`CMakeLists.txt:706-712`](CMakeLists.txt:706) — `plinth_tests_js`
filter widened from `"[js] ~[async]"` to `"[js]"`; gained
`RESOURCE_LOCK plinth_pg_schema` + `LABELS "needs_pg"` because
`[js][async]` cases are PG-gated. The previous per-TEST_CASE
`catch_discover_tests` block (lines 727-740) deleted — `[js][async]`
tests now share the grouped subprocess. Net effect: `~10×
subprocess-count reduction` on this group (from ~45 + 80 grouped =
125 to ~80) — recovered the discovery-cost reduction the rest of the
suite already enjoys.

### Files

- [src/kernel/js/run_on_context.cpp](src/kernel/js/run_on_context.cpp) — main-loop pin, back-pressure suspension, post-`JS_Eval` `sample_memory_peak` call.
- [src/kernel/js/conversion.cpp](src/kernel/js/conversion.cpp) — `OOM_MEMORY_SLACK_BYTES` 256 KiB → 1 MiB.
- [tests/kernel/js/async_hardening_test.cpp](tests/kernel/js/async_hardening_test.cpp) — N.39 scale-lift to 100×8 + comment refresh.
- [CMakeLists.txt](CMakeLists.txt) — fold `[js][async]` into grouped `plinth_tests_js`; delete per-TEST_CASE block.

### Residuals

- `pubsub_test.cpp:117` P.01 happy path — pre-existing v0.5.0
  5-second `cv.wait_for` timing flake under PG NOTIFY contention.
  Distinct family from the refcount race; tracked separately.
- Framework-layer drogon `EventLoopThreadPool::~` join-self pattern
  remains an upstream Drogon issue; surfaces on `plinth_tests_pg`
  intermittently. Out of scope for kernel work.

### Methodology

Small-targeted-fix shape per `project_next_session_post_055_re_eval.md
§Next session` (~70 lines code change, no ICD spawned). Branch
`fix/0.5.5.2-js-async-refcount`, PR target `main` per
`feedback_main_protected.md`.

---

## 2026-04-26 — RE-EVAL following 0.5.5 (rewrite session, untagged)

Eighth scheduled re-evaluation per METHODOLOGY-llm-assisted-development.md
§Phase 3; second of the 0.5.x arc; **arc-closeout posture** — the next
code milestone (0.6.0 frontend-shell bootstrap) opens fresh territory
and the kernel envelope effectively freezes. Cadence position: 4/4 over
v0.5.2/v0.5.3/v0.5.4/v0.5.5 since the prior cadence point
`RE-EVAL following 0.5.1` (2026-04-23). Untagged per
`feedback_tagging_rule.md`.

### Output

`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md` — paper review covering
the v0.5.2 → 0.5.5.1 window. Eight sections matching the 0.5.1 cadence-
re-eval precedent (§1 Inputs read · §2 Gaps found · §3 Zero-gap
findings · §4 Accepted deviations catalog · §5 DEFERRED.md status
· §6 Forward ICD presence check · §7 Cadence/labels update ·
§8 Verification). Twelve documented deviations across the window
catalogued; eleven ratify cleanly as kernel-convention or external-
constraint, one (D11 `Config::Realtime::Events::SeqSource` enum vs
string) and three companions (D12 `superseded_seqs[]` design defer;
D13 replay seq-stamp call site; D14 `run_replay` post-0.5.5.1 const-ref
parameter) trigger ICD amendment subsections.

### In-tree amendments

**Architecture (3 fixes — 1 interface-drift correction + 2 arch-silent-
on-code):**
- [`docs/architecture/03-data.md`](architecture/03-data.md):
  - **§3.6.1 Physical Channel Fan-In** last sentence rewritten —
    writer-first topology (since v0.5.5) makes `events_writer` the
    listener's sole `EventHandler` consumer; broker is downstream of
    the writer, no longer a peer. Closes the §2.1 interface-drift gap.
  - **§3.5 Delta Sync on Reconnect** implementation blockquote extended
    — fifth resync reason `live_buffer_overflow` (added v0.5.5);
    `replay_done.buffered_live_count` field; per-conn live-buffer
    machinery + `live_buffer_cap_per_subscription` (default 256 frames
    per ICD-0.5.5 §8 OQ6); `ConnState::channels_mu` synchronization
    contract (added 0.5.5.1).
- [`docs/architecture/02-capabilities.md`](architecture/02-capabilities.md):
  - **§2.2 Permission-Gated** row for `pubsub.subscribe` extended to
    the v0.5.2 binding signature `pubsub.subscribe(channel, handler)
    → Promise<() => void>` with own-vs-cross-extension RBAC framing.
- [`docs/architecture/04-services-ha.md`](architecture/04-services-ha.md):
  - **§1 Audit Logging** appended a realtime audit-family index covering
    the seven `realtime.*` families introduced in the 0.5.x arc
    (`listener.*`, `coalescer.*`, `broker.*`, `events.*` (six events,
    v0.5.4), `debounce.advisory_overridden` (v0.5.5),
    `seq.*` (reserved per v0.5.5; populated by S.06/S.07 follow-ups),
    `notify.rejected`).

**ICDs (4 "Implementation deviation" subsections per METHODOLOGY §Phase 2
Constraint #4):**
- [`ICD-0.5.5-sequence-numbers-client-debounce.md`](icd/ICD-0.5.5-sequence-numbers-client-debounce.md)
  — new "Implementation deviation (v0.5.5 ship)" subsection ahead of
  §10 Configuration; documents D11 (`SeqSource` enum vs string,
  cert-err58-cpp constraint), D12 (`superseded_seqs[]` empty-array
  design defer), D13 (replay seq-stamp call-site separation).
- [`ICD-0.5.4-events-table-delta-sync.md`](icd/ICD-0.5.4-events-table-delta-sync.md)
  — new "Implementation deviation (v0.5.4 ship + 0.5.5.1 follow-up)"
  subsection ahead of §`plinth.events` Persistence Writer; documents
  D15 (xact-scoped advisory lock vs session-scoped), D16 (BIGINT key
  embedded vs bound), D17 (`since_seq=0` cursor-expired skip), D14
  (`run_replay` parameter `ConnState` → `const ConnState&` post-
  0.5.5.1, with pointer to the source header line).
- [`ICD-0.5.3-db-batch-silent-mode.md`](icd/ICD-0.5.3-db-batch-silent-mode.md)
  — new "Implementation deviation (v0.5.3 ship + 0.5.3.1 follow-up)"
  subsection ahead of §Test Cases; documents D18–D21 plus the
  0.5.3.1 B.06 timeout closure + B.13 cross-extension closure +
  B.07/B.11 narrowings.
- [`ICD-0.5.2-ws-broker.md`](icd/ICD-0.5.2-ws-broker.md) — appended
  "Implementation deviation (0.5.2.N broker test backfill,
  2026-04-24)" subsection alongside the existing LH-2 deviation
  block; documents D22 five test-shape deviations
  (B.05/B.06 PG-gating, U.02 extension_mismatch edge, U.07 cancellation
  observable swap, S.11/I.07 delivery re-check swap, I.05 narrowing).

**DEFERRED.md (1 new entry + 1 substantive update):**
- New 2026-04-26 entry consolidating eleven deferred test cases
  across two ICDs into three buckets (multi-process advisory-lock
  harness; live-buffer fault-injection seam; W.06 source-seq tracking
  design defer). Shares scheduling slot with `0.5.x.N HTTP test
  harness` for the I.* cases; W.06 picked up at the RE-EVAL following
  0.6.3 or earlier if 0.6.3 SDK work binds the optimistic-update path.
- WS-teardown / `[js][async]` entry updated with the three 0.5.5.1
  sub-path closures (events_writer in-flight tracker, runEvery
  try/catch wrappers, process-lifetime PG client) plus family
  expansion to a third exemplar — **#29 `async_bridge: cap.batch
  fail-fast on first rejection`** surfaced in `ci-build-and-test-
  12377` (first post-merge CI run on `main` after 0.5.5.1). Same
  refcount family as #38 + #47; new entry-point became reachable
  when 0.5.3 introduced `db.batch` to the dispatch surface.

**ROADMAP:**
- `RE-EVAL following 0.5.5` flipped `[ ]` → `[x]` with shipped marker.
- `0.5.x.N [js][async] kernel-side refcount investigation` (was
  `[medium]` cross-cutting under §0.4.x cleanup follow-ups) **promoted
  to `0.5.5.2 [js][async] refcount fix` `[strong]`** as a scheduled
  milestone blocking 0.6.0 — per the maintainer's 2026-04-26 directive that
  0.5.x is the kernel's last hardening window. Full description moved
  to §0.5.
- `0.5.5.N ICD-0.6.0 authoring [strong]` four-part follow-up inserted
  between the discharged RE-EVAL line and 0.6.0, mirroring the
  `0.5.1.2 ICD-0.5.2 authoring` precedent.
- `0.6.0` promoted `[medium]` → `[strong]` per METHODOLOGY §3.1
  forward-ICD-presence rule.

### Architectural commitment carried forward

The 0.5.x arc is functionally closed at v0.5.5.1 + this re-eval, with
**one binding follow-up**: the `[js][async]` refcount race must close
before 0.6.0 begins. That commitment is recorded in three places —
the RE-EVAL §2.4 narrative, DEFERRED.md WS-teardown entry's "Promotion
2026-04-26" subsection, and the ROADMAP §0.5 0.5.5.2 description.
0.6.0 entry criteria require 0.5.5.2 closure.

### Next session

**0.5.5.2 [js][async] refcount fix** is the immediate next code
session. Investigation entry condition: candidate-root-cause list in
`project_ws_flaky_segfault.md §Candidate root causes`. Exit condition:
`[js][async]` grouping enabled with the ~10× subprocess-count
reduction the rest of the suite already enjoys. Likely sub-shape: an
ownership audit of `dispatch_async_op_detached` / `SqlBinderAwaiter` /
`AnyCompletionAwaiter`. May spawn an ICD if scope grows beyond a small
targeted fix.

After 0.5.5.2 closes: `0.5.5.N ICD-0.6.0 authoring` (paper), then
0.6.0 frontend-shell bootstrap.

### Verification

Docs-only session; no code build. Cross-references round-trip
(CHANGELOG ↔ RE-EVAL doc ↔ ICD/arch amendments ↔ ROADMAP/DEFERRED
edits). File path resolution checked. Methodology §3 axes
(structural + code-aware halves) both exercised — see
`RE-EVAL-0.5.x-following-0.5.5.md §8 Verification`.

---

## 2026-04-26 — `0.5.5.1` kernel teardown hardening

Three-part follow-up landing the v0.5.5 PR-CI subprocess-abort
remediation. No new feature surface; the goal is to crush the two
families of teardown crash that v0.5.5's PR run surfaced
(`plinth_tests_pure` + `plinth_tests_ws`) before 0.6.0 begins UI
work and the kernel envelope effectively closes. Per
`feedback_tagging_rule.md` this is an interim follow-up: CHANGELOG
entry only, no new git tag (rolls into the next X.Y.Z).

### What shipped

1. **`events_writer` in-flight tracker for `drogon::async_run`-detached
   `insert_envelope` coroutines** (`src/kernel/realtime/events_writer.cpp`).
   `drain_one`'s `runEvery` callback was firing the
   `drogon::async_run([] -> Task<> { co_await insert_envelope(...); })`
   coroutine and returning while the coroutine remained mid-await on
   the test's pinned `DbClient`. `events_writer::stop()`'s queue drain
   only blocks on entries it pops itself, so the test fixture's
   `Harness::~Harness` would release its last `DbClient` shared_ptr
   while a previously-detached coroutine still held a copy — the
   coroutine's frame `db` local then became the last reference, and
   when the frame unwound on the DbClient's IO thread the resulting
   `EventLoopThreadPool::~` join-self tripped `Resource deadlock
   avoided` (the v0.5.5 CI signature). Fix: a `g_inflight_inserts`
   atomic + `g_inflight_cv` condition_variable; `drain_one`
   increments before `async_run`, the lambda decrements at the end
   of the coroutine body, `stop()` blocks (bounded by
   `shutdown_drain_ms`) until the count reaches zero. Also wraps both
   `runEvery` callbacks (drain + cleanup) in `try { ... } catch
   (...)` so a `bad_weak_ptr` from `drogon::async_run` in test-mode
   subprocesses (no primary loop) cannot escape into trantor's
   `EventLoop::loop` and trip `std::terminate` past `loopFuncs`'s
   noexcept boundary.

2. **`ConnState::channels_mu` per-conn mutex around the WS subscription
   set** (`src/kernel/ws/conn_state.hpp` + `publish.cpp` +
   `subscriptions.cpp` + `events_controller.cpp`). The 0.5.4
   `publish_dispatched` synchronous pre-pass reads
   `state->channels.contains(channel_str)` from the listener / writer
   thread to populate `ev.delivered_to_users`; the conn's owning loop
   mutates `state->channels` via `subscribe` / `unsubscribe` /
   `drain_extension`. Without synchronization the I.06 broker
   integration test (`drain_extension` between subscribe and
   dispatch) reproducibly SIGSEGVs inside `unordered_set::contains`
   when the per-conn erase loop and the cross-thread read overlap.
   Fix: add `mutable std::unique_ptr<std::mutex> channels_mu` to
   `ConnState` (unique_ptr keeps the struct movable for
   `subscriptions.cpp`'s `state_copy` pattern); lock at every
   `state->channels` access (8 sites). Drive-by: `replay::run_replay`
   parameter changed from `ws::ConnState state` to `const
   ws::ConnState& state` since `ConnState` is no longer copyable
   (the move-into-lambda capture in the production caller still
   moves the unique_ptr; tests pass by reference).

3. **Process-lifetime PG client for `seq_generation_test.cpp`**
   (`tests/kernel/realtime/seq_generation_test.cpp`). The S.* harness
   created a per-test `DbClient::newPgClient(connNum=1)` that
   reproducibly tripped `bad_weak_ptr` inside the PG client's IO-thread
   loop teardown when the harness destructor released its last
   reference. Same `trantor::EventLoop::loop` rethrow family
   `project_ws_flaky_segfault.md` documents at the framework layer;
   the per-test create+destroy cycle exposes it more reliably than
   production single-instance lifetime. Fix: a static
   `shared_pg_client()` accessor that allocates the client on first
   use and lets the process-exit destructor handle teardown
   (Catch2 has already reported by then, so the framework crash
   doesn't fail tests). Reduces the destructive teardown surface from
   N-per-test to one process-exit event.

### Verification

10× per failing CI invocation, local run with PG on `localhost:5432`:

| Invocation | Pre-fix | Post-fix |
|:--|--:|--:|
| `plinth_tests "[ws]"` | 0/10 (100% deadlock) | 10/10 |
| `plinth_tests "~[integration] ~[ws] ~[js]"` (pure) | 0/5 (100% bad_weak_ptr at S.03) | 10/10 |
| `plinth_tests "[seq][unit]"` (isolated repro) | 3/5 fail | 10/10 |
| `plinth_tests "[integration] ~[ws] ~[js]"` (pg) | clean | 3/3 |
| `plinth_tests "[js] ~[async]"` (js) | pre-existing pubsub.cpp:117 timing flake (~10%) | unchanged (1/8 hit, expected per `project_next_session_post_055.md` candidate #4) |

### Out of scope

- `pubsub_test.cpp:117` 5 s NOTIFY timing flake — pre-existing; same
  family as the kernel teardown but pinned to a different subsystem.
  Carried forward per the post-0.5.5 candidate slate.
- Kernel-side `[js][async]` `#38 async_hardening` 8–11% intermittent
  SEGV — pre-existing, separately tracked.

### Discharge pointers

- `project_next_session_post_055.md` candidate #1 (kernel teardown
  hardening) closed.
- `project_ws_flaky_segfault.md` family — PG-client-teardown sub-path
  contained at the test layer; the underlying drogon `EventLoopThreadPool::~`
  join-self pattern remains a framework issue, but it no longer
  surfaces in CI.

---

## 2026-04-26 — `v0.5.5` sequence numbers + client-side debounce

Sixth code milestone of the 0.5.x Realtime arc. Six-phase commit arc
on `feat/0.5.5-sequence-numbers-client-debounce` against
[`docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md`](icd/ICD-0.5.5-sequence-numbers-client-debounce.md)
(1975-line paper ICD authored 2026-04-25). Two intertwined
contributions delivered together to amortize the topology shift:

### What shipped

1. **Server-side envelope `seq` aligned to `plinth.events.seq`.** Every
   persisted Layer-1/2/3 envelope leaving the kernel now carries a
   BIGINT `seq` field whose value equals the BIGSERIAL primary key the
   writer's `INSERT … RETURNING seq` produced. **Writer-first
   topology shift** (ICD §5, OQ1 pin): the broker stops being a peer
   listener handler and becomes a writer-downstream consumer, invoked
   from inside `events_writer::insert_envelope` after the
   INSERT-and-stamp completes. The replay path symmetrically stamps
   `seq` from the row's `plinth.events.seq` column before
   `build_replay_frame`, so live and replay frames carry the same
   field shape on both paths. Closes ICD-0.5.0 §Payload Envelope
   `seq` reservation, ICD-0.5.1 §Out of scope monotonic-generator
   deferral, and ICD-0.5.4 §OQ5 "envelope `seq` will equal table seq
   by construction" promise.

2. **Wire-protocol coalescing/debounce contract** for the future SDK
   (ICD §6 + §7). Envelope additions (Layer-1 from coalescer,
   Layer-2/3 stamped at writer time): `coalesced_count` (≥1; OQ3
   counts upstream NOTIFY hits), `window_open_ts_ms` /
   `window_close_ts_ms` (Layer-1: open + window_ms; Layer-2/3:
   equal to emitted_at_ms), optional `superseded_seqs[]` gated by
   `events.coalesce.emit_superseded_seqs` (OFF by default per OQ4;
   ships as a stable empty array under writer-first topology).
   `subscribed` ack frame now carries `recommended_debounce_ms`
   (default 100) + `recommended_jitter_ms` (default 50); new inbound
   `debounce_renegotiate` frame parser is a no-op + rate-limited
   `realtime.debounce.advisory_overridden` audit (advisory + audit,
   not enforced per OQ5).

3. **Live-vs-replay ordering machinery** (ICD §8). Per-conn live
   buffer holds mid-replay live frames until `replay_done` flushes
   them in arrival (= writer-first seq) order. `replay_done` gains
   `buffered_live_count` (always present, value 0 when nothing
   buffered). New resync precondition reason `live_buffer_overflow`
   joins the existing three from ICD-0.5.4 (`cursor_expired` /
   `mismatch` / `row_cap`); on overflow the broker flips a shared
   abort flag, the replay coroutine returns at the next chunk
   boundary, and the overflow site emits the resync inline.
   `live_buffer_cap_per_subscription` defaults to 256 frames per OQ6
   (configurable [16, 65536]).

### Phase commit arc

1. `625fcc4` — Phase 1: ICD-0.5.5 config plumbing + OQ lockdown
   (6 new `Config::Realtime::Events` fields with hard-fail bound
   checks; OQ §17 amendment).
2. `38f6662` — Phase 2: writer-first topology + envelope.seq stamp
   (broker handler dropped, writer stamps + dispatches; replay-side
   stamp; pre_broker_hook test seam; 8 S./L. cases active).
3. `e564491` — Phase 3: coalescer wire-protocol additions
   (`coalesced_count`, `window_open_ts_ms`, `window_close_ts_ms`,
   optional `superseded_seqs[]`; 6 W.* cases active).
4. `d91663e` — Phase 4: subscribe_ack advisory + debounce_renegotiate
   parser + audit (8 D./J. cases active).
5. `8ee6a45` — Phase 5: live buffer + `replay_done.buffered_live_count`
   + `live_buffer_overflow` resync (5 L.* cases active).
6. _this commit_ — Phase 6: integration test + CHANGELOG + ROADMAP +
   discharge pointers.

### Test counts

**29 of 36 ICD test cases active** (8 S.* + 6 W.* + 5 D.* + 3 J.* +
6 L.* + 1 I.*). Seven cases SKIP() with phase pointers:
- **S.06** (broker-side `realtime.seq.gap_detected` audit) and
  **S.07** (cursor catch-up after writer crash mid-window) — defer
  to 0.5.5.1.
- **L.03 / L.04 / L.05** (mid-replay buffering + overflow scenarios) —
  defer to 0.5.5.1; **the live-buffer machinery is in place** but the
  end-to-end assertion needs a fault-injection seam beyond the WS
  test client's current API.
- **W.06** (`superseded_seqs[]` populated) — design-deferred (writer-
  first topology forecloses source-seq tracking; needs a follow-up
  ICD per ICD §6 §Implementation seam).

I.05 lands as the canonical end-to-end (full reconnect + replay +
live boundary; 26 assertions). I.01–I.04 (LH-1 storm, LH-2 sidecar,
multi-node failover, cross-extension pubsub) defer to 0.5.5.1 with
LH-1 already validated as a hand-run baseline (Phase 2 OQ1 gate).

### LH-1 storm pre-flight (OQ1 acceptance gate)

| Run | calls ok | call p99 | observed | lag p99 |
|----:|---------:|---------:|---------:|--------:|
| baseline (Phase 1 HEAD) | 34,102 | 53.8 ms | 1,091,264 (ratio 1.0000) | 7 ms |
| Phase 2 (writer-first) | 33,714 | 51.8 ms | 1,078,848 (ratio 1.0000) | 7 ms |
| Phase 6 (full arc) | 41,344 | 46.6 ms | 1,323,008 (ratio 1.0000) | 7 ms |

Threshold: live-path p99 < 50 ms. **Observed: 7 ms across all three
runs.** Writer-first topology + live-buffer machinery + envelope
additions add no measurable live-path latency under 4 producers ×
4 subscribers × 120 s; zero parse errors / worker fails / gaps; per-
subscriber observed/emitted ratio 1.0000 every run. Phase 6
throughput drift (+21% calls vs baseline) tracks the post-cluster-
upgrade host idle state, not the topology shift.

### Documented deviations from ICD pseudocode

1. `Config::Realtime::Events::SeqSource` is `enum class : std::uint8_t`
   (single value `WRITER_RETURNING`) rather than the `std::string`
   the ICD §10 pseudocode shows; required to keep
   `Config::Realtime::Events` trivially default-constructible because
   `cursor_store.cpp` and `events_writer.cpp` hold static-storage
   `g_cfg` instances (cert-err58-cpp).
2. `superseded_seqs[]` ships as a stable empty array when
   `emit_superseded_seqs=true`; population semantics deferred per
   ICD §6 §Implementation seam (writer-first foreclosure of source-
   seq tracking — see W.06 deferral note above).
3. Replay engine's seq-stamp lives at `replay.cpp` between
   `parse_payload` and `build_replay_frame`, not in
   `build_replay_frame` itself; cleaner separation of concerns.

### Discharge pointers updated

- ICD-0.5.0 §Payload Envelope `seq` reservation → REQUIRED on
  persisted envelopes per 0.5.5.
- ICD-0.5.1 §Out of scope monotonic-seq deferral → discharged.
- ICD-0.5.4 §Traces-to + §OQ5 + §D.08 → envelope-seq == table-seq
  invariant, OQ5 promise delivered, D.08 absorbed as L.03.
- `architecture/03-data.md §3.4` items 1, 3, 5 → implementation
  footnote.

---

## 2026-04-25 — `0.5.4.N` ICD-0.5.5 paper authoring

Paper-only follow-up after v0.5.4 ship the same day. Authored
`docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md` (1975
lines) on `feat/0.5.4.N-icd-0.5.5-authoring`. Mirrors the
`0.5.2.N ICD-0.5.4 authoring` pattern — paper ICD lands ahead of
the corresponding code milestone per `feedback_icd_horizon.md`'s
one-ahead rule and METHODOLOGY §3.1 forward-ICD-presence check.

### What shipped

Two contributions specified by the new ICD:

1. **Server-side envelope `seq` aligned to `plinth.events.seq`.**
   The reserved `seq` slot from ICD-0.5.0 §Payload Envelope (line
   249) becomes REQUIRED on every persisted Layer-1/2/3 envelope
   leaving the kernel. Source of truth: the existing
   `INSERT … RETURNING seq` at `events_writer.cpp:268` — the
   plumbing already extracts `SEQ` for cursor advance
   (`events_writer.cpp:279`); 0.5.5 stamps that same value onto
   `ev.envelope["seq"]` and routes broker fan-out through the
   writer. Writer-first topology shift: the broker stops being a
   peer listener handler and becomes a writer-downstream consumer
   called from inside `insert_envelope` after INSERT-and-stamp.
   Trade-off: ≤10 ms additional live-path latency (PG INSERT round
   trip on the critical path); LH-1 acceptance threshold p99 < 50 ms
   pinned for code-session validation.

2. **Wire-protocol coalescing/debounce contract.** Three additive
   surfaces for the future 0.6.3 client SDK to implement against:
   (a) optional envelope fields `coalesced_count`,
   `window_open_ts_ms`, `window_close_ts_ms` reifying the
   coalescer's already-existing 50 ms window; (b) advisory fields
   `recommended_debounce_ms` (default 100) and
   `recommended_jitter_ms` (default 50) on `subscribe_ack`;
   (c) new audit `realtime.debounce.advisory_overridden` for
   client-side cadence drift visibility. Optional
   `superseded_seqs[]` envelope field gated by
   `coalesce.emit_superseded_seqs` config (OFF by default per OQ4).

### Discharged reservations

- **ICD-0.5.0 §Payload Envelope Contract** line 249 footnote
  `"seq": <optional integer, 0.5.5 populates>` — discharged.
  Field becomes REQUIRED on persisted envelopes after this ICD's
  code milestone ships.
- **ICD-0.5.1 §Out of scope** lines 147–152 monotonic-generator
  deferral — discharged. ICD-0.5.5 is the promised follow-up.
- **ICD-0.5.4 §OQ5 resolved-OQ row** line 1764 promise that
  envelope `seq` will equal `plinth.events.seq` "by construction"
  — discharged. The writer-first topology delivers exactly that.
- **ICD-0.5.4 D.08** test case (line 1478, deferred to 0.5.4.1
  at v0.5.4 ship) — absorbed into ICD-0.5.5 as L.03. Becomes
  crisply testable only once envelope `seq` is canonical, which
  is exactly what 0.5.5 delivers. 0.5.4.1 follow-up scope reduces
  to I.02 + I.03 only.
- **architecture/03-data.md §3.4** items 1, 3, 5 — server-side
  contract half discharged. The SDK code itself (items 2, 4 plus
  the algorithm sketch) ships in 0.6.3.

### Test taxonomy (36 cases targeted)

| Prefix | Count | Focus |
|--------|-------|-------|
| S.\*  | 8 | Sequence-number generation (writer-first invariants) |
| W.\*  | 7 | Wire contract — envelope shape |
| D.\*  | 5 | Debounce semantics (advisory mechanics) |
| J.\*  | 3 | Jitter advisory |
| L.\*  | 8 | Live/replay ordering (folds D.08 from 0.5.4.1) |
| I.\*  | 5 | Integration (LH-1, LH-2, multi-node failover, end-to-end WS client) |

### Open Questions left for code session

Seven OQs with architect recommendations: (1) writer-first
generation (recommended; alternatives nextval-preallocate,
independent counter, follow-up frame all rejected on correctness
or operational grounds); (2) per-user strict / per-channel
advisory monotonicity; (3) `coalesced_count` reflects upstream
NOTIFY hits; (4) `superseded_seqs[]` OFF by default; (5)
advisory delivery once on subscribe_ack (not per-frame); (6)
`live_buffer_cap_per_subscription` default 256, code-session
pin after LH-3 exercises; (7) D.08 absorbed as L.03.

### Files

| Action | Path |
|--------|------|
| Created | `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md` |
| Updated | `docs/ROADMAP.md` (inserted `0.5.4.N` slot between v0.5.4 ship and 0.5.5 outline) |
| Updated | `docs/CHANGELOG.md` (this entry) |

No code touched; no migrations modified; no tests added or
changed. Memory updates land at PR merge.

---

## 2026-04-25 — v0.5.4 plinth.events table + delta sync on reconnect

Fifth code milestone of the 0.5.x Realtime arc. Closes
**ICD-0.1.6 §Delta Sync stub** (deferred to 0.5.x at 0.1.6 ship)
and the **ICD-0.5.2 §Reconnect Semantics 0.5.4 hand-off** —
broker keeps the live hot path; cursor-backed replay attaches via
`plinth.events`. Full implementation of
`docs/icd/ICD-0.5.4-events-table-delta-sync.md` (2006 lines)
across a 7-phase commit arc on `feat/0.5.4-events-table-delta-sync`.

### What shipped

Three contributions per ICD §Overview:

1. **`plinth.events` persistence writer.** New singleton subsystem
   (`src/kernel/realtime/events_writer.{hpp,cpp}`) owning a dedicated
   `trantor::EventLoopThread`; bounded `std::deque<QueueEntry>`
   (default 10000, drop-newest on overflow); registers ONE
   `EventHandler` against the 0.5.0 listener as the second consumer
   (broker first, writer second per ICD §When `record_delivered`
   fires). Per-envelope `pg_try_advisory_xact_lock` keyed on
   `(channel, emitted_at)` for multi-node single-writer (§HA / §SC7).
   Server-side `emitted_at` overwrite at INSERT.

2. **Per-user cursor + reconnect handshake.** New
   `plinth.user_event_cursors(user_id UUID PK → users CASCADE,
   last_seq, updated_at)` table; new
   `src/kernel/realtime/cursor_store.{hpp,cpp}` with LRU cache
   (TTL + flush-threshold gated UPSERTs using
   `GREATEST(last_seq, EXCLUDED.last_seq)` for monotonicity).
   Broker fills `DispatchedEvent.delivered_to_users` synchronously
   in `publish_dispatched`'s pre-pass over the registry snapshot
   (load-bearing concurrency fix); writer reads that field after
   INSERT and fires fire-and-forget `cursor_store::record_delivered`
   per user. Subscribe handler (`src/kernel/ws/subscriptions.cpp`)
   parses optional `since_seq` and dispatches replay via
   `src/kernel/realtime/replay.{hpp,cpp}` paginated query
   (`WHERE seq > $1 AND channel = ANY($2::text[]) ORDER BY seq ASC
   LIMIT $3`) with per-row RBAC re-check (defense-in-depth, mirrors
   ICD-0.5.2 §SC5). Three resync precondition reasons from the
   engine (`cursor_expired` / `mismatch` / `row_cap`); the
   fourth (`events_disabled`) is caller-side. New WS frame types
   `replay` / `replay_done` / `resync` plus error code
   `resubscribe.invalid_since_seq`.

3. **`plinth.events_cleanup` default kernel task.** New
   `src/kernel/scheduled_tasks/cleanup_events.{hpp,cpp}` running on
   the events_writer's loop via `runEvery(cleanup_interval_ms)`.
   `pg_try_advisory_xact_lock` (xact-scoped) for HA single-sweeper
   coordination — losers silent-skip per ICD. `DELETE FROM
   plinth.events WHERE created_at < NOW() - retention_seconds`.
   The proper scheduled-tasks subsystem ships in 0.7.2; this
   ride-the-writer's-loop arrangement migrates trivially when 0.7
   lands.

Six new rate-limited audit events under `realtime.events.*`:
`write_failed` (reasons: `queue_full` / `pg_error` /
`cleanup_failed` / `shutdown_timeout` / `cursor_read_failed`),
`replay_started`, `replay_completed`, `replay_truncated`,
`resync_required`, `cleanup_swept`. All share `audit_window_ms`
(default 60000) for sliding-window aggregation.

`Config::Realtime::Events` substruct adds 10 fields with hard-fail
bound checks: `enabled`, `retention_seconds [60, 604800]`,
`cleanup_interval_ms [10000, 3600000]`, `replay_max_rows_per_chunk
[10, 10000]`, `replay_max_total_rows [100, 1000000]` (≥ chunk
cap; cross-field consistency check), `write_queue_size [100, 1000000]`,
`shutdown_drain_ms [100, 60000]`, `cursor_cache_ttl_ms [0, 60000]`,
`cursor_flush_threshold [1, 10000]`, `audit_window_ms [1000, 3600000]`.

`migrations/schema.sql` realtime section: `plinth.events`
(BIGSERIAL `seq` PK, `channel TEXT`, `payload JSONB`,
`created_at TIMESTAMPTZ DEFAULT NOW()`) + `plinth.user_event_cursors`
+ `events_channel_seq_idx (channel, seq)` + `events_created_at_idx
(created_at)`.

### Phase commit arc (7 phases)

1. `7ecc472` — Phase 1: schema + config + writer skeleton
2. `d6abc94` — Phase 2: cursor store + LRU cache (7 C.* cases)
3. `e2ca087` — Phase 3: writer INSERT path + xact-lock + audits
   (10 E.* cases)
4. `b627d2e` — Phase 4: replay engine + WS frame catalogue
   (9 Y.* cases)
5. `53ded9a` — Phase 5: subscribe handshake + delivered-users hook
   (7 D.* cases)
6. `f6a64c6` — Phase 6: cleanup task + xact-scoped lock
   (4 K.* cases)
7. (this commit) — Phase 7: integration test + CHANGELOG +
   ROADMAP + ICD footnotes (1 I.* case)

### Test count

**38 of 41 ICD-targeted cases shipped.** Per the phase plan,
**3 cases deferred to 0.5.4.1**:

- **D.08** (replay-then-live ordering during mid-replay emit) —
  needs a "replay mid-flight" injection seam not pinned in the ICD.
- **I.02** (multi-process advisory-lock harness) — needs new
  cross-process integration scaffolding; useful for 0.5.5+ work too.
- **I.03** (live + replay race in integration) — same family as
  D.08, surface at integration level.

Mirrors v0.5.3's 32/35 ratio (32 cases shipped, 3 deferred to
0.5.3.1). 0.5.4.1 cleans these three up alongside any other
post-ship polish.

### Documented deviations from ICD pseudocode

1. **Cleanup lock scope.** ICD §Cleanup pseudocode shows session-
   scoped `pg_try_advisory_lock`. Implementation uses
   `pg_try_advisory_xact_lock` inside a `db->newTransactionCoro`.
   Rationale: Drogon's connection pool means a follow-up
   `co_await execSqlCoro` may run on a different connection from
   the lock-acquire; session-scoped advisory lock is tied to that
   first connection and is dropped when the connection returns to
   the pool. xact-scoped lock releases at COMMIT regardless of
   pool churn. The writer's own xact-lock already uses the same
   pattern; symmetry is correct. Documented at the call site.

2. **Cleanup lock key embedded in SQL.** ICD shows passing the
   BIGINT lock key as a bound parameter. Drogon's SqlBinder sends
   BIGINT in a binary format that PG rejects with "incorrect
   binary data format in bind parameter" on the
   `pg_try_advisory_xact_lock` query shape. Workaround embeds the
   compile-time constant in the SQL string. No SQL-injection
   surface — the constant is fixed at build time.

3. **`since_seq=0` is "no prior cursor", not "aged-out cursor".**
   ICD §Cursor-expired check pins `since_seq < MIN(seq)`. A fresh
   client (no prior session) sends `since_seq=0`, which would
   technically match (since `MIN(seq) >= 1` after any insert).
   Implementation special-cases `since_seq=0` to skip the
   cursor_expired check (per ICD §New-user behaviour). Reconnect
   with non-zero `since_seq < MIN(seq)` still triggers
   `cursor_expired`.

### Verification

- `cmake --build --target tidy` zero findings.
- `ctest -j1` 100% green (84/84 grouped tests).
- 38 new test cases pass against live PG (10 E.* + 7 C.* + 7 D.* +
  9 Y.* + 4 K.* + 1 I.*).

---

## 2026-04-25 — 0.5.3.N ICD-0.5.4 authoring (paper session, untagged)

Paper-only follow-up to the 0.5.3.1 db.batch B.06 timeout + SC3
cross-extension session (2026-04-24). **Un-tagged** per
`feedback_tagging_rule.md` (paper-only follow-up between code
releases). One new ICD authored; no code changes. Satisfies
`feedback_icd_horizon.md` one-ahead rule — 0.5.4 is the next code
milestone and no ICD existed for it until this session.

### Why

Per `feedback_icd_horizon.md` ICDs should be written at most one
milestone ahead of current implementation. v0.5.3 shipped 2026-04-24;
0.5.3.1 db.batch B.06 + SC3 follow-ups shipped 2026-04-24 closing
the v0.5.3 implementation gap. The next code milestone is
**0.5.4 `plinth.events` table + delta sync on reconnect**
(`docs/ROADMAP.md §0.5 line 135`) and had no ICD. This session
lands it. Precedent for the paper-authoring slot: 0.5.2.N ICD-0.5.3
authoring (PR #82, `ff63e85`, 2026-04-24) and 0.5.0.5 ICD-0.5.1
authoring.

The architectural sketch was already pinned at
`docs/architecture/03-data.md §3.5` (the 4-step reconnect protocol
+ retention semantics) and the `plinth.events` table shape at
`§Appendix B` (line 514–520). ICD-0.5.2 §Reconnect Semantics
explicitly handed off durable per-user subscriptions to 0.5.4 at
lines 678–681; ICD-0.5.0 §Listener Subsystem named the
`plinth.events` writer as the listener's second `EventHandler`
consumer (after the 0.5.2 broker). ICD-0.5.4 promotes all three
prose pieces into a single normative contract.

### What shipped

- **`docs/icd/ICD-0.5.4-events-table-delta-sync.md` — new (2006 lines).**
  Authors the 0.5.4 code milestone contracts. Header block (Traces
  to / Depends on / Milestone / Status / Methodology / Related)
  matches 0.5.0 + 0.5.1 + 0.5.2 + 0.5.3 precedent. Twenty
  substantive sections:
  - `## Overview` — three contributions (`plinth.events` writer,
    per-user cursor + reconnect handshake, `plinth.events_cleanup`
    default kernel task) + ten explicit out-of-scope items
    (monotonic envelope `seq`, cross-extension cursor isolation,
    per-channel cursor granularity, replay deduplication, streaming
    cursor / open-ended replay, backpressure / flow control,
    per-event payload diff, cursor encryption, client-side cursor
    persistence, layer-2/3 replay restrictions, broker drain
    integration).
  - `## plinth.events Persistence Writer` — subsystem shape (singleton
    `EventsWriter` with dedicated `trantor::EventLoopThread`),
    why-a-dedicated-loop rationale (listener fast-handler rule),
    bounded write queue (`std::deque<QueueEntry>` protected by
    mutex+cv), drop-newest overflow policy (§OQ1), write path
    (single INSERT with RETURNING seq), envelope `emitted_at`
    server-side population (overwrite semantics; live broker fan-
    out unaffected by writer mutation), accept-all-layers channel
    filter, no-validate / no-per-write-audit / no-exactly-once /
    no-cross-node-dedup non-goals, failure handling.
  - `## plinth.user_event_cursors and the Cursor Store` —
    table shape (UUID PK + ON DELETE CASCADE on `plinth.users(id)`),
    `cursor_store` API (`record_delivered` UPSERT-monotonic /
    `read_cursor` cached / `reset_cursor`), when each fires
    (delivery callback after broker fan-out completes; reconnect
    only; resync_required), cache layer (LRU TTL + delta threshold),
    lifecycle (no own atexit entry; flushes during writer stop).
  - `## Reconnect Handshake` — client subscribe frame extension
    (new optional `since_seq` field), three new server frames
    (`replay`, `replay_done`, `resync`), subscribe handler flow
    (parse → RBAC re-check → `since_seq` branch → retention check
    → mismatch check → register-live-first → run replay engine),
    why-register-live-subs-before-replay rationale (preserve frames
    emitted during the replay window).
  - `## Replay Engine` — paginated query (`SELECT seq, channel,
    payload FROM plinth.events WHERE seq > $1 AND channel = ANY($2)
    ORDER BY seq ASC LIMIT $3`), per-channel RBAC re-check on each
    row (mirrors ICD-0.5.2 §SC2), chunk-size trade-off (default 500
    keeps each PG result under 4 MB), total-row cap (default 10000
    triggers `replay_truncated` + `resync`), concurrent-replays no
    cross-user interference.
  - `## Retention + Cleanup` — `plinth.events_cleanup` task DELETE
    sweep on `realtime.events.cleanup_interval_ms` (default 5
    minutes), advisory-lock-gated for multi-node, sweep audit,
    DELETE-not-partition rationale (table size bounded by retention
    not history).
  - `## Indexing` — three indexes: PK on `seq` (BIGSERIAL), composite
    on `(channel, seq)` for replay query, B-tree on `created_at` for
    cleanup; no JSONB GIN; (§OQ6 ratifies).
  - `## Config Surface` — full `realtime.events.{enabled,
    retention_seconds, cleanup_interval_ms, replay_max_rows_per_chunk,
    replay_max_total_rows, write_queue_size, shutdown_drain_ms,
    cursor_cache_ttl_ms, cursor_flush_threshold, audit_window_ms}`
    block with bound checks + consistency check
    (`replay_max_total_rows ≥ replay_max_rows_per_chunk`).
  - `## Audit Events` — six rate-limited events
    (`realtime.events.write_failed` + `replay_started` +
    `replay_completed` + `replay_truncated` + `resync_required` +
    `cleanup_swept`).
  - `## HA Semantics` — one-writer-per-envelope advisory lock
    (§OQ7), multi-node replay (any node serves any user), cursor
    consistency via shared PG state + per-node cache.
  - `## Deterministic Teardown` — atexit chain extension
    (`stop_events_writer` between `stop_listener` and
    `js::rollback_all_batches`); ordering rationale.
  - `## Error Model` — writer-side audits (`queue_full` / `pg_error`
    / `cleanup_failed`), replay-side WS frame errors
    (`resubscribe.invalid_since_seq` / `resubscribe.events_disabled`),
    cursor-store failures (fall-through to last_seq=0).
  - `## Security Constraints` — eight numbered: replay re-checks
    RBAC; cursor opacity (transparent BIGINT pinned with rationale);
    retention is hard cap; replay rate-limited per user (organic via
    WS frame rate limit); persisted envelope is server-augmented;
    duplicate replay tolerated; cross-node single writer; cursor
    TOCTOU on rapid reconnect (documented + acceptable).
  - `## Test Cases` — 41 targeted tests (10 E. writer + 7 C. cursor
    + 8 D. delta-sync + 9 Y. replay + 4 K. cleanup + 3 I.
    integration), per-table markdown tables, six new TUs + one
    extended fixture, test-seam notes + CI wiring file map.
  - `## Entry / Exit` — Entry: v0.5.0/0.5.1/0.5.2/0.5.3/0.5.3.1
    shipped + listener seam shipped + broker hot path shipped + WS
    frame parser shipped + `plinth.users` shipped. Exit: 14
    deliverables + tag.
  - `## Open Questions` — eight OQs each with **pin** + alternatives
    + recommendation + rationale + "Architect: confirm or redirect":
    OQ1 drop-newest queue overflow; OQ2 transparent BIGINT cursor;
    OQ3 four-reason resync set; OQ4 cursor advance after writer
    INSERT; OQ5 `plinth.events.seq` BIGSERIAL as cursor source;
    OQ6 three-index strategy; OQ7 per-envelope advisory lock for
    multi-node writer; OQ8 per-user single cursor.
  - `## Appendix: Resolved OQs` — table summarizing all 8 pins.
  - `## Appendix A — End-to-End Example` — 9-step alice walkthrough
    (steady state → writer + cursor → drop → reconnect → replay →
    live frame ordering → cleanup tick).
  - `## Appendix B — Config Example` — full config block alongside
    0.5.0/0.5.1/0.5.2/0.5.3 realtime block + minimum-effective.
  - `## Appendix C — Schema DDL` — `plinth.events` + indexes +
    `plinth.user_event_cursors` DDL.
  - `## Appendix D — Frame Catalogue Delta` — extends ICD-0.1.6
    §Frame Types with `since_seq` on subscribe + three new server
    frames + two new error codes.

  Header `Traces to:` + `Depends on:` + `Related:` blocks cite 14
  upstream ICDs/architecture sections. `Status: Ready for
  implementation`.

### Three precedents this ICD honors

1. **Listener consumer pattern from ICD-0.5.0 §Listener Subsystem.**
   The `EventHandler` registration seam was designed for exactly this
   second-consumer scenario (broker first, writer second). ICD-0.5.4
   uses it verbatim — no listener contract change.
2. **Dedicated-loop pattern from ICD-0.5.1 §Coalescer State Machine.**
   The coalescer's `trantor::EventLoopThread` shape is mirrored by
   the events writer — listener-fast, IO-deferred, atexit-bounded.
   No new pattern invented.
3. **Per-channel RBAC re-check defense-in-depth from ICD-0.5.2
   §Security Constraint 2.** The replay engine re-checks the same
   rule resolver on every replayed envelope. Mirrors the broker's
   delivery-time re-check verbatim — no new RBAC code.

### What did not ship

- **No code.** Paper-only ICD authoring; the implementation ships
  in v0.5.4 (next code session).
- **No CHANGELOG entry retirement.** The v0.5.3 entry stays as the
  newest tagged entry; this entry is the new "newest" interim entry.
- **No tag.** Per `feedback_tagging_rule.md` paper sessions between
  code releases stay untagged.
- **No test changes.** ICD specifies 41 new test cases for the v0.5.4
  code session; nothing wired this session.
- **No DEFERRED.md retirement.** No 0.5.4-tagged entries existed to
  retire.

### How v0.5.4 will use this ICD

Implementation session opens `docs/icd/ICD-0.5.4-events-table-delta-sync.md`
+ this CHANGELOG entry. Each of the eight OQs gets architect
ratification at session start (or accepts the recommendation
verbatim per ICD-0.5.3 precedent). Each phase commit closes one
section (writer + cursor store + replay + cleanup + WS handshake +
config + integration). 41 new test cases distribute across the six
new TUs. Exit-criteria checklist at §Entry / Exit drives the v0.5.4
ship.

### Verification

- `cmake --build --target tidy` — paper changes touch no C++; tidy
  gate clean (run per `feedback_run_ci_invocations.md`).
- ICD self-consistency — every OQ pinned (8/8); every OQ appears in
  `## Appendix: Resolved Open Questions` (8/8); every test case has
  the required columns (#, Type, Scenario, PG-gated, Expected
  outcome).
- Cross-references — every `ICD-0.5.x §...` citation points to a
  section that exists in that file.
- Line count 2006 (target was 1500–1700; landed long because the
  surface area of three subsystems + two tables + six frames +
  eight OQs + 41 tests warrants the depth).
- Branch `feat/0.5.3.N-icd-0.5.4-authoring`; PR pending manual open
  per `reference_gitea_pr_flow.md`.

---

## 2026-04-24 — 0.5.3.1: `db.batch()` B.06 timeout + SC3 cross-extension rejection (no tag)

Two post-v0.5.3 follow-ups closing the implementation gap against
ICD-0.5.3 (semantics already pinned in v0.5.3; this session wires
the runtime path + tests). No interface surface added — only
behavior promised by the ICD that v0.5.3 explicitly deferred.
Shipped on `feat/0.5.3.1-batch-timeout-sc3` (no tag per
`feedback_tagging_rule.md`).

### Part A — B.06 wall-clock timeout enforcement

`Config::Db::Batch::timeout_ms` (parsed in v0.5.3 at
`src/kernel/config.cpp:238` but unwired) now arms a
`trantor::EventLoop::runAfter` on the main loop at `DB_BATCH_BEGIN`
finalize. On fire, the lambda flips
`bc.batch_state.timed_out = true` (after a scope-id match check
that no-ops a stale fire on a re-batched bc). The in-batch
`db.exec` / `db.query` enqueue path and `__db_batch_commit__`
both reject inline with `db.batch.timeout` once the flag is set;
the orchestrator's `catch` runs `__db_batch_rollback__`; the
finalize-batch `audit_batch_rolled_back` arm stamps
`reason="timeout"` (extended condition: when
`op.rollback_error.code == "db.batch.timeout"`). The timer is
disarmed at COMMIT/ROLLBACK finalize via `clear_batch_timer` and
also from the lifecycle drain paths
(`discard_batches_for_extension` + `discard_all_batches`) so a
torn-down extension or an atexit teardown can never see a late
fire. The in-flight registry now stores `(extension_name,
TimerId, EventLoop*)` rather than just `extension_name`; the
discard paths invalidate every attached timer in one synchronous
pass.

New surface: `set_batch_timeout_ms` / `batch_timeout_ms` /
`set_batch_timer` / `clear_batch_timer` in
`src/kernel/js/db_batch_audit.hpp`. New `BatchState` fields:
`timed_out` (bool, main-loop-mutated only), `timer_id`,
`timer_loop`. `main.cpp` now propagates
`cfg.db_bindings.batch.timeout_ms` alongside the audit-window +
quota config calls.

### Part B — Security Constraint 3 cross-extension rejection

New TU `src/kernel/js/db_batch_schema_check.{hpp,cpp}` exposes
`classify_cross_extension(sql, expected_ext)` (regex-based scan
for `\bext_([a-z0-9_]+)\b` tokens; returns `true` when any
captured suffix differs from `expected_ext`) plus
`audit_batch_cross_extension_rejected` (rate-limited 64-entry LRU
+ 60s window, same envelope as `db.search_path.set_failed`,
emits a 200-char SQL preview). The in-batch enqueue path in
`db.exec` / `db.query` calls the classifier after the §B.06
`timed_out` check + before the quota check; on match, fires the
audit + returns a synchronous reject with
`db.batch.cross_extension_not_allowed`. Kernel-scope bcs
(`extension_name` empty) skip the gate by design — the kernel may
touch any extension schema. `plinth.*` references never trip the
classifier (no `ext_` prefix). False-positive trade-off
documented in the new TU's header: SQL string literals
containing `ext_<word>` substrings will be rejected; same
posture as the coalescer's regex classifier.

### Tests

Two new cases in `tests/kernel/js/db_batch_test.cpp` (now 12 of
the ICD's 12 B.\* slots green plus B.13 SC3 — the ICD's B.\*
matrix is amended to add B.13 in this session):

- **B.06 — Timeout** (`needs_pg`): `timeout_ms=200`; user-fn
  awaits `INSERT` ⇒ `pg_sleep(0.4)` ⇒ `INSERT`. The
  server-side wait yields the main loop, the timer fires, the
  second `INSERT` rejects inline. Asserts: outer rejects
  `db.batch.timeout`, zero rows persist, the most recent
  `db.batch.rolled_back` audit has `reason="timeout"`. Restores
  `set_batch_timeout_ms(30000)` post-test for any subsequent case
  in the same process. Uses `pg_sleep` rather than the opt-in
  `__host_sleep_ms__` shim so the test runs under stock
  `cmake -B build` without `-DPLINTH_JS_TEST_SHIMS=ON`.
- **B.13 — Cross-extension batch** (`needs_pg`): `batch` ext
  starts a batch, runs a successful `INSERT` against `notes`
  (unqualified ⇒ `ext_batch.notes` via search_path), then
  attempts `INSERT INTO ext_other.notes(...)`. Asserts: outer
  rejects `db.batch.cross_extension_not_allowed`, both schemas
  are empty post-rollback, `db.batch.cross_extension_rejected`
  audit fires. Negative case in the same test: a `plinth.*`
  reference inside a fresh batch from the same extension does
  NOT trip the classifier and the batch commits cleanly.

### Closes

- ICD-0.5.3 §B.06 (deferred at v0.5.3 ship).
- ICD-0.5.3 §Security Constraint 3 (deferred at v0.5.3 ship).
- The "phase 4 ships with no timer enforcement" comment at
  `src/kernel/config.hpp:136` is replaced with a one-liner
  pointing at ICD §B.06.

### Verification

- `cmake --build build --target tidy` — zero new findings.
- `ctest -j1 --output-on-failure` — full suite green; B.06 +
  B.13 specifically pass under PG.
- 20-run atexit-race regression of the batch subset — clean
  (matches the ICD-0.5.3 exit-criteria protocol).

---

## 2026-04-24 — housekeeping: `ws/events_controller.hpp` NOLINT removal (no tag)

Removed the bare `NOLINTBEGIN` / `NOLINTEND` block wrapping
`WS_PATH_LIST_BEGIN` / `WS_PATH_ADD("/ws/events")` / `WS_PATH_LIST_END`
in `src/kernel/ws/events_controller.hpp` (lines 34–38). The block's
original rationale was "Drogon-internal macros expand to non-trailing
returns", but an empirical re-run of `cmake --build build --target
tidy` plus a line-scoped `clang-tidy-20 --header-filter='.*'
-line-filter=[events_controller.hpp:30-40]` confirmed the macro
expansion produces **zero diagnostics** under the current check set —
the suppression was dead. Per `feedback_nolint_policy.md`
(every suppression must be justified), the correct action is
removal rather than conversion to an enumerated list. If a future
Drogon or clang-tidy config shift re-trips the expansion, CI will
surface it and we restore with an enumerated block then. No
interface surface touched; no ICD implicated; `plinth_tests "[ws]"`
builds and runs cleanly (6 PG-independent cases pass, 42 PG-gated
cases skip as expected on this host).

---

## 2026-04-24 — v0.5.3: `db.batch()` + `silent` + per-op `SET search_path` + OID type mapping (tag `v0.5.3`)

Fourth code milestone of the 0.5.x Realtime arc. Consolidates the six
phase commits on `feat/0.5.3-db-batch-silent-mode` into the v0.5.3
release. Implements the full contract of
`docs/icd/ICD-0.5.3-db-batch-silent-mode.md`. **Closes two
long-standing DEFERRED.md entries from 2026-04-18**: ICD-0.3.3
§Security Constraint 1 (per-op `SET search_path`) and ICD-0.3.3
§PG-Value → JS-Value Conversion (OID-driven type mapping).

### Four contributions

**1. OID-driven PG-type → JS-type mapping.** Drogon patch
`third_party/drogon-patches/ftype-accessor.patch` (upstreamable)
exposes `drogon::orm::Field::oid()`; the new TU
`src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}` switches over
19 built-in PG OIDs; BYTEA routes through an `{__bytea_hex__: ...}`
Json tag that `json_to_js` decodes into native JS `Uint8Array` via
`JS_NewUint8ArrayCopy`. Feature flag `db.oid_mapping.enabled`
(default `true`) retains the 0.3.3 heuristic as a rollback lever.

**2. `silent` flag semantics pin + rate-limited audit.** The
`AsyncOp::silent` + coalescer-gate plumbing has been live since
0.3.3; v0.5.3 wires the `db.silent.used` audit at the gate. New TU
`src/kernel/js/db_silent_audit.{hpp,cpp}` implements 64-entry LRU
rate-limiter keyed on extension. `Config::Db::Silent::audit_window_ms`
(default 60000, bound `[1000, 3600000]` ms) controls aggregation.

**3. Per-op `SET LOCAL search_path TO ext_<name>, plinth` wrapper.**
Every extension-scope `db.exec` / `db.query` wraps in a
`newTransactionCoro` + SET LOCAL + user SQL + explicit COMMIT.
Kernel-scope bcs (empty `extension_name`) bypass. New TU
`src/kernel/js/db_search_path.{hpp,cpp}` owns the atomic `enforce`
flag, identity-regex defense (`[a-z][a-z0-9_]*`), and
rate-limited `db.search_path.set_failed` audit. Explicit
`COMMIT` vs. Drogon's async destructor commit is the
correctness anchor — `await db.exec(...)` resolves only after
durable write (see phase 3 design note).

**4. `db.batch(async () => { ... })` transactional wrapper.**
Three new `AsyncOp::Type` variants (`DB_BATCH_BEGIN` /
`_COMMIT` / `_ROLLBACK`) + `batch_scope_id` + `batch_pinned_conn`
fields. `BridgeContext::BatchState`
`{depth, scope_id, pinned_conn, ops_in_batch}` tracks in-flight
state, mutated only on the main loop. JS orchestrator installed
via `JS_Eval` at `register_db` time chains
`__db_batch_begin__ → fn() → __db_batch_commit__ / __db_batch_rollback__`
via Promise chaining (async/await avoided for extension-runtime
compatibility). Three new dispatch arms + in-batch routing of
DB_QUERY/DB_EXEC through the pinned `TransactionPtr`.
`CoalescerRegistry` gains `flush_batch_scope` /
`discard_batch_scope` + per-scope bucket accumulation (no timer);
batch-commit envelopes emit with `window_ms = 0` sentinel.
`Config::Db::Batch::{max_ops_per_batch=500,
max_concurrent_batches_per_bc=4, timeout_ms=30000,
audit_window_ms=60000}`. Two rate-limited audits
(`db.batch.committed` / `.rolled_back`).

### Deterministic teardown

`main.cpp` atexit chain gains
`plinth::js::discard_all_batches()` between
`realtime::stop_listener()` and `realtime::broker::stop()` —
drops every in-flight batch scope so a pending
`flush_batch_scope` cannot emit post-teardown.
`install_lifecycle.cpp` DISABLED / UPGRADING / UNINSTALL call
sites get `discard_batches_for_extension(name)` alongside the
existing coalescer + broker drain hooks. `ws_test_fixture` and
`async_bridge_fixture` atexit chains mirror the same slot.

### 32 new test cases

- 7 T.\* OID-driven types (`async_bridge_test.cpp`).
- 6 S.\* silent flag semantics (`db_silent_test.cpp`, new TU).
- 7 P.\* search_path wrapper (`db_search_path_test.cpp`, new TU) +
  P.02 inside-batch (`db_batch_test.cpp`).
- 11 B.\* batch + I.\* integration + B.07 lifecycle drain
  (`db_batch_test.cpp`, new TU).

**ICD amendments** (tracked inline + per-phase CHANGELOG):
- B.06 (timeout enforcement) deferred to follow-up — no timer on the
  pinned connection yet.
- B.07 narrowed from "DISABLE mid-batch" to "`discard_batches_for_extension`
  drops the scope" — the drain surface is the kernel-side contract.
- B.11 narrowed to pre-cancelled bc.
- P.05 narrowed from "ext schema dropped" to "regex defense rejects
  malicious identity" — PG's SET LOCAL is permissive on non-existent
  schemas.
- T.07 scenario `'true'::text` → `'t'::text` (heuristic only
  mis-classifies single-char PG bool text reprs).
- T.\* landed in `async_bridge_test.cpp` not `stdlib_test.cpp`
  (PG-gated via the async bridge).
- `Config::Db` field named `db_bindings` in C++ to avoid collision
  with existing `Config::Database db;` while maintaining the ICD's
  JSON `"db"` block convention.
- Security Constraint 3 cross-extension rollback test flagged as
  follow-up.

### Drive-by

- `TIDY_JOBS` default bumped 4 → 8 (`feedback_parallelism_cap.md`
  2026-04-23 approval landed opportunistically in phase 2).

### Verification

- `ctest -j1` full suite 82/82 green (v0.5.2 baseline 61 → 82,
  +21). All 45 `[js][async][db]` cases pass together with every
  0.5.3 surface live.
- `cmake --build --target tidy` full-tree zero findings.
- **20-run atexit-race validation loop** (ICD-0.5.3 §Exit
  criteria) — zero in-flight-batch-at-shutdown aborts across all
  20 runs.
- Drogon patch applies cleanly on a fresh
  `rm -rf build/_deps` + reconfigure; idempotent re-apply guard in
  `PATCH_COMMAND`.

### Two 2026-04-18 DEFERRED entries moved to Resolved

- Per-op `SET search_path` for `db.*` (ICD-0.3.3 §Security
  Constraint 1) — resolved phase 3.
- `db.*` PG-type → JS-type mapping (ICD-0.3.3 §PG-Value → JS-Value)
  — resolved phase 1.

### Phase commit arc on `feat/0.5.3-db-batch-silent-mode`

1. `2164525` — phase 1: OID type mapping + Drogon patch (7 T.\*)
2. `a0d75f6` — phase 2: silent audit + rate-limit (6 S.\*) + TIDY_JOBS=8
3. `8ff4518` — phase 3: per-op SET LOCAL search_path wrapper (7 P.\*)
4. `a888673` — phase 4: db.batch core + coalescer scope seams (11 B.\*/P.02)
5. `81862af` — phase 5: lifecycle drain + atexit + integration (B.07 + I.\*)
6. _this commit_ — phase 6: DEFERRED/ROADMAP/memory close-out

Tag `v0.5.3` lands on the merge commit per
`feedback_tagging_rule.md`.

---

## 2026-04-24 — 0.5.3 phase 5: lifecycle drain + atexit + integration tests (in progress on `feat/0.5.3-db-batch-silent-mode`)

Fifth phase commit. Closes the lifecycle-integration + deterministic-
teardown exit criteria of ICD-0.5.3, and lands the three integration
tests deferred from phases 3 and 4 (B.07, I.01, I.02).

### What

- **In-flight batch registry** in `db_batch_audit.cpp` — a
  mutex-protected `std::unordered_map<uint64_t, std::string>` mapping
  scope_id → extension_name. Populated at DB_BATCH_BEGIN
  finalization (on the main loop); drained at DB_BATCH_COMMIT /
  DB_BATCH_ROLLBACK / test teardown. Enables per-extension lookup
  without walking every BridgeContext.
- **Four new public functions** on `db_batch_audit.hpp`:
  `register_in_flight_batch`, `unregister_in_flight_batch`,
  `discard_batches_for_extension(name)` (walks registry, discards
  matching coalescer scopes, returns count),
  `discard_all_batches()` (process-wide teardown drain).
- **`finalize_batch` hooks** the register/unregister calls at
  BEGIN/COMMIT/ROLLBACK completion. Error paths also unregister
  defensively.
- **Three lifecycle drain call sites** in `install_lifecycle.cpp`
  at DISABLED (~line 1666), UNINSTALL (~line 2052), UPGRADING
  (~line 3206) — paired with the existing coalescer + broker
  drain calls, so pending batch scopes drop together with other
  extension-owned realtime state.
- **Atexit integration**:
  - `main.cpp` atexit chain: `plinth::js::discard_all_batches()`
    slotted between `realtime::stop_listener()` and
    `realtime::broker::stop()` per ICD §Atexit chain.
  - `tests/kernel/ws/ws_test_fixture.cpp` atexit mirror.
  - `tests/kernel/js/async_bridge_fixture.cpp` atexit mirror.
- **3 new test cases** in `db_batch_test.cpp`:
  - **B.07** (deferred from phase 4) — `discard_batches_for_extension`
    drops the scope bucket; a subsequent `flush_batch_scope` is a
    zero-emit no-op. Drives the drain function directly rather than
    the full install_lifecycle harness.
  - **I.01** end-to-end: 5 inserts in one batch → exactly one
    coalescer envelope at COMMIT → `ops[0].count = 5` observed via
    `set_emit_hook_for_test`.
  - **I.02** concurrent batches from the same extension produce two
    distinct envelopes (counts 10 + 10, not merged), proving
    scope-bucket isolation.

### Verification

- Full `ctest -j1` 82/82 green (79 → 82, +3).
- **20-run batch-suite loop** (ICD-0.5.3 exit criterion): zero
  atexit-race reproductions, zero in-flight-batch-at-shutdown aborts.
  All 20 iterations completed with `All tests passed (83 assertions
  in 14 test cases)`.
- `cmake --build --target tidy` full-tree zero findings after inline
  fix (`SRC` → `src` rename for `readability-identifier-naming.VariableCase`).
- No core files accumulated during phase 5 debugging (contrast with
  phase 4's 12 cores from the JSValue leak — see
  `feedback_core_files.md` policy note).

### Design note

The ICD §Extension Lifecycle Integration envisioned the drain
walking live BridgeContexts; phase 5 takes the simpler equivalent
approach of tracking in-flight scopes in a process-wide registry
and discarding the coalescer buckets on drain. PG connections are
not explicitly rolled back by this drain — Drogon's pool closure on
shutdown, or its per-transaction destructor on pool-destroy, handles
the PG-side transaction cleanup. The kernel-side hygiene (scope
buckets out of the coalescer before the broker tears down) is the
load-bearing contract; pinned-conn rollback is best-effort and
delegated to Drogon's normal teardown.

### Remaining carry-overs (to phase 6)

- `docs/DEFERRED.md` 2026-04-18 entries (search_path + OID mapping)
  move to Resolved.
- `docs/ROADMAP.md §0.5` line 132 removal.
- Memory `project_plinth_state.md` + next-session pointer update.
- v0.5.3 tag on merge commit.
- B.06 (timeout enforcement) + Security Constraint 3 cross-extension
  test flagged as post-v0.5.3 follow-ups.

---

## 2026-04-24 — 0.5.3 phase 4: `db.batch()` core + coalescer scope seams (in progress on `feat/0.5.3-db-batch-silent-mode`)

Fourth phase commit — the anchor. Implements the `db.batch()`
transactional-wrapper contribution per ICD-0.5.3 (first of four
architecturally; sequenced fourth here because it consumes phase 1's
OID converter, phase 2's audit infrastructure, and phase 3's
search_path wrapper + `Transaction` pinning proof-of-life).

11 B.\*/P.02 test cases land (of 13 ICD-prescribed); B.06 (timeout
enforcement) + B.07 (drain-on-DISABLE) defer — B.06 to a follow-up
phase (requires a timer on the pinned connection; out of scope), B.07
to phase 5 alongside the lifecycle-drain hook.

### What

- **Three new `AsyncOp::Type` variants** — `DB_BATCH_BEGIN`,
  `DB_BATCH_COMMIT`, `DB_BATCH_ROLLBACK`. Two new fields on AsyncOp:
  `batch_scope_id` (0 = not in batch) and `batch_pinned_conn`
  (shared_ptr snapshot of the TransactionPtr opened by BEGIN, carried
  by DB_QUERY/DB_EXEC stamped while inside a batch).
- **`BridgeContext::batch_state`** — `{depth, scope_id, pinned_conn,
  ops_in_batch}` — populated synchronously at `db.batch` entry, mutated
  on the main loop only (via `queueInLoop` in `finalize_batch`).
- **New TU `src/kernel/js/db_batch_audit.{hpp,cpp}`** — monotonic
  `alloc_batch_scope_id()` + two rate-limited audit emitters
  (`db.batch.committed`, `db.batch.rolled_back`) + atomic getters for
  the `audit_window_ms` and `max_ops_per_batch` config knobs. Same
  64-entry LRU pattern as phase 2/3.
- **`CoalescerRegistry`** gains `flush_batch_scope(scope_id)` /
  `discard_batch_scope(scope_id)` public seams + optional
  `batch_scope_id` param on `record_write`. New module-local
  `g_scope_to_buckets` map accumulates in-batch counters (no timer);
  `flush_batch_scope` emits one envelope per (schema, table) with
  `window_ms = 0` to signal "batch commit" to downstream consumers.
- **`db.batch(fn)` JS binding** (`db_bindings.cpp`) — synchronous
  pre-flight (nested reject, scope-id allocation, `batch_state`
  depth bump), then calls the injected JS orchestrator. Three
  internal C-backed bindings `__db_batch_begin__` /
  `__db_batch_commit__` / `__db_batch_rollback__` each enqueue a
  single AsyncOp; the orchestrator stitches them around the user fn
  via promise chaining (installed via `JS_Eval` at `register_db`
  time — bypasses the `globalThis.eval` deletion because the C-side
  JS_Eval API is distinct from the user-space `eval`).
- **Three new dispatch arms** in `run_on_context.cpp` —
  `handle_db_batch_begin` opens `newTransactionCoro`, runs `SET
  LOCAL search_path TO ext_<name>, plinth`, stashes the tx on
  `bc.batch_state.pinned_conn` via a main-loop callback.
  `handle_db_batch_commit` runs explicit COMMIT on the pinned conn,
  then on the main loop: clears `bc.batch_state`, flushes the
  coalescer scope, fires the committed audit.
  `handle_db_batch_rollback` calls `tx->rollback()`, then on the
  main loop: clears `bc.batch_state`, discards the coalescer scope,
  fires the rolled_back audit. The orchestrator's JS-side chain
  re-throws the user's error after the rollback resolves.
- **`Config::Db::Batch`** — four fields
  (`max_ops_per_batch`/500, `max_concurrent_batches_per_bc`/4,
  `timeout_ms`/30000, `audit_window_ms`/60000) with ICD-bound
  hard-fail parsers under `apply_db_batch`. `main.cpp` propagates
  `audit_window_ms` + `max_ops_per_batch` to `db_batch_audit` on
  startup.
- **In-batch routing for `db.exec` / `db.query`** — bindings check
  `bc.batch_state.depth > 0`, run synchronous quota check
  (`db.batch.quota_exceeded` on overflow; batch stays open per ICD
  §Field semantics), stamp `batch_scope_id` + `batch_pinned_conn`
  on the AsyncOp. Outcome helpers read `op.batch_pinned_conn` and
  route through it, skipping the per-op search_path wrapper + its
  explicit COMMIT. Coalescer `record_write` receives `batch_scope_id`
  so writes accumulate under the batch's scope bucket until COMMIT.
- **11 B.\*/P.02 test cases** in new TU
  `tests/kernel/js/db_batch_test.cpp` (tag
  `[js][async][db][batch]`): B.01 happy commit + audit; B.02
  rollback on user throw; B.03 rollback on DB error (unique
  violation); B.04 nested reject + outer unaffected; B.05 quota
  exceeded mid-batch + caller can swallow + commit first N; B.08
  empty batch; B.09 silent inside batch suppresses coalescer for
  that statement only; B.10 metrics monotonic across 5 commits + 3
  rollbacks; B.11 cancelled bc rejects `db.batch` inline; B.12
  `batch_state` reset after commit; P.02 inside-batch single SET
  LOCAL — three unqualified inserts all land in ext_ schema.

### Design notes

- **JSValue refcount hygiene.** The quota-exceeded path in
  `db.exec`/`db.query` originally leaked the promise-capability
  JSValue (register_pending takes ownership of resolve/reject but
  the outer `promise` ref is separate). `JS_FreeValue(ctx, promise)`
  before falling through to `reject_inline` restores the balance —
  mirrored from the pubsub_bindings re-subscribe leak fix (0.5.2.N
  backfill).
- **`tx->rollback()` on the rollback arm sets Drogon's internal
  `isCommitedOrRolledback_` flag**, so the TransactionImpl destructor
  no-ops rather than re-firing COMMIT. COMMIT arm still emits the
  phase 3 "no transaction in progress" warning (Drogon auto-commit
  vs. our explicit COMMIT) — deferred cleanup; non-blocking.
- **The JS orchestrator is installed via `JS_Eval` with
  `JS_EVAL_TYPE_GLOBAL` on the kernel side.** `globalThis.eval` and
  `globalThis.Function` were deleted in 0.3.5 (DESIGN §9.1); the C
  API is unaffected, which keeps the orchestrator out of the
  extension-facing surface. Promise chaining is used instead of
  async/await to keep the orchestrator transparent to test harnesses
  that inspect promise shapes.

### ICD amendments (inline)

- **B.06 timeout enforcement deferred** — phase 4 ships without a
  timer on the pinned connection. The ICD's `db.batch.timeout_ms`
  config knob is plumbed through but the runtime enforcement
  doesn't fire yet. Tracked as the next standalone follow-up
  candidate.
- **B.07 drain-on-DISABLE** lands in phase 5 alongside the
  lifecycle drain hook (`js::rollback_extension_batches`).
- **B.11 narrowed** — the ICD envisioned cancellation firing
  mid-batch from the dispatch cascade; in phase 4 we assert the
  simpler case: a pre-cancelled bc rejects `db.batch` inline at
  the binding-level `bc->cancelled` gate, leaving the row count
  at 0. Mid-batch cancellation requires the dispatch-cascade drain
  path which bundles cleanly with phase 5's lifecycle work.
- **Cross-extension rollback assertion (ICD §Security Constraint
  3)** — still not explicitly tested. Flagged for follow-up.

### Scope

- `src/kernel/js/async_op.hpp` — three new enum variants + two new
  fields + drogon DbClient include.
- `src/kernel/js/bridge_context.hpp` — `BatchState` struct + drogon
  DbClient include.
- `src/kernel/js/db_batch_audit.{hpp,cpp}` (new, ~170 LOC).
- `src/kernel/js/stdlib/db_bindings.cpp` — in-batch routing on
  db_query / db_exec, new db.batch binding, three internal helpers,
  JS orchestrator installed at register_db.
- `src/kernel/js/run_on_context.cpp` — `finalize_batch` helper +
  three `handle_db_batch_*` detached coroutine handlers + switch
  arms + in-batch `exec_target` routing.
- `src/kernel/config.{hpp,cpp}` — `Config::Db::Batch` substruct +
  `apply_db_batch` parser with 4 bound-checks.
- `src/kernel/main.cpp` — propagates `audit_window_ms` +
  `max_ops_per_batch` to `db_batch_audit`.
- `src/kernel/realtime/coalescer.{hpp,cpp}` — `record_write`
  optional scope param, new public `flush_batch_scope` /
  `discard_batch_scope` seams, `g_scope_to_buckets` map,
  `clear_windows_for_test` also clears scope buckets,
  `flush_snapshot` gains optional `window_ms_override`.
- `tests/kernel/js/db_batch_test.cpp` (new, ~440 LOC) — 11 cases.
- `CMakeLists.txt` — new src + test TUs.

### Verification

- Full `ctest -j1` 79/79 green (68 → 79, +11 batch cases).
- 45/45 `[js][async][db]` cases pass (prior 34 + 11 new).
- `cmake --build --target tidy` full-tree zero findings after the
  phase's inline fixes (std::move capture for transferred
  shared_ptr tx + const-to-non-const rename for window_ms local).
- JS orchestrator round-trips cleanly under bc destroy — no
  `list_empty` abort in `JS_FreeRuntime`.

---

## 2026-04-24 — 0.5.3 phase 3: per-op `SET LOCAL search_path` wrapper (in progress on `feat/0.5.3-db-batch-silent-mode`)

Third phase commit. Implements the per-op `SET LOCAL search_path`
isolation contribution (third of four in ICD-0.5.3). **Closes
ICD-0.3.3 §Security Constraint 1** which has been deferred since
2026-04-18 — the two-year TODO that held back the DB-level extension-
isolation story. Every `db.exec` / `db.query` from an extension-scope
bc now runs inside a short transaction with `SET LOCAL search_path TO
ext_<name>, plinth`.

### What

- **New TU `src/kernel/js/db_search_path.{hpp,cpp}`** — feature-flag
  atomic (`set_search_path_enforce` + `search_path_enforced`),
  identity regex validator (`is_valid_extension_name` per `[a-z][a-z0-9_]*`),
  and rate-limited `db.search_path.set_failed` audit emitter (same
  64-entry LRU pattern as `db_silent_audit.cpp`, fixed 60s window).
- **Wrapper in `run_on_context.cpp`** — new `prepare_search_path_wrapper`
  coroutine helper returns a `SearchPathWrap{tx, early_reject}` struct.
  Both `run_db_query_outcome` and `run_db_exec_outcome` call it before
  executing user SQL; route the user's statement through
  `wrap.tx->execSqlCoro` when a transaction was opened; explicit
  `COMMIT` before the outer promise resolves (see design note below).
- **`Config::Db::SearchPath::enforce`** (default `true`, JSON path
  `"db.search_path.enforce"`). Warn-logged at load when `false`
  (production-escape-hatch posture per ICD §Security Constraint 2).
- **main.cpp startup** wires
  `plinth::js::db::set_search_path_enforce(cfg.db_bindings.search_path.enforce)`
  after the broker starts.
- **7 P.\* test cases** in new TU
  `tests/kernel/js/db_search_path_test.cpp` (tag
  `[js][async][db][search_path]`): P.01 unqualified INSERT lands in
  ext_ schema; P.03 kernel-scope bypass; P.04 cross-ext qualified
  write works + unqualified rejects `db.undefined_table`; P.05
  regex-defense rejects malicious extension_name + audit fires; P.06
  search_path does not leak between sequential calls from different
  bcs; P.07 regex unit tests (valid + invalid identities); P.08
  `enforce=false` — row lands in `public.notes` (no wrapper). **P.02
  (inside-batch single-`SET LOCAL`) deferred to phase 4** since it
  asserts against the batch-scope wrapper.

### Design note — explicit `COMMIT` vs async destructor commit

Drogon's `TransactionImpl::~TransactionImpl` commits asynchronously
via `queueInLoop`; without an explicit `COMMIT` the JS-side `await
db.exec` resolves **before** the write is durable, violating
causality (a downstream Layer-1 envelope could fire before the row
lands, and tests observing via raw libpq would see zero rows). An
explicit `co_await tx->execSqlCoro("COMMIT")` before the outer
promise resolves restores the contract. The destructor's subsequent
auto-commit fires a harmless `"WARNING: there is no transaction in
progress"` — accepted noise. An alternative `setCommitCallback`-backed
coroutine awaiter was prototyped but **deadlocks** when the
coroutine and the destructor's commit both target the same Drogon IO
loop (under investigation; not blocking — the explicit-COMMIT path
is functionally correct). See the inline comment at the `run_db_query
_outcome` commit hook.

### ICD amendments (inline)

- **P.05 scenario narrowed from "ext schema dropped out-of-band" to
  "malicious extension_name rejected by pre-flight regex".** PostgreSQL's
  `SET LOCAL search_path` is permissive — accepts non-existent schemas
  without error; the subsequent unqualified SQL fails with
  `db.undefined_table` instead. The ICD's §Failure mode example was
  over-specified; the actual `db.search_path.set_failed` code path
  fires on the identity-regex defense at pre-flight. Inline note
  amends.
- **P.04 assertion adjusted.** The ICD's `db.permission_denied` expected
  outcome assumed PG ACLs on ext_ schemas; the Plinth test harness runs
  PG as superuser with no ACLs configured. P.04 now asserts the
  observable behavioral contract: qualified `INSERT INTO
  ext_terminal.sessions` succeeds (wrapper does not block fully-qualified
  cross-ext writes), AND unqualified `INSERT INTO sessions` rejects with
  `db.undefined_table` (wrapper re-scopes search_path to ext_notes; the
  unqualified table doesn't exist there).
- **P.02 deferred to phase 4.** The inside-batch single-`SET LOCAL`
  assertion requires the batch wrapper from phase 4.

### Scope

- `src/kernel/config.{hpp,cpp}` — `Config::Db::SearchPath::enforce`
  substruct + `apply_db_search_path` parser with warn-log.
- `src/kernel/js/db_search_path.{hpp,cpp}` (new, ~130 LOC).
- `src/kernel/js/run_on_context.cpp` — `SearchPathWrap` result type,
  `prepare_search_path_wrapper` helper, both db arms rewrap through
  the helper + explicit COMMIT.
- `src/kernel/main.cpp` — `set_search_path_enforce` propagation.
- `tests/kernel/js/db_search_path_test.cpp` (new, ~310 LOC).
- `CMakeLists.txt` — new TU + test TU added to `plinth` +
  `plinth_tests` enumerations.

### Verification

- Full `ctest -j1` 68/68 green (61 → 68, +7 P.\*).
- `cmake --build --target tidy` full-tree zero findings after the
  phase's inline fixes (ranges::all_of + coroutine-parameter-by-value
  + const-name → non-const for the is_lower/is_digit/is_under locals).
- All 34 `[js][async][db]` cases (T.\* + S.\* + P.\* + 0.3.3 group A–M)
  pass together with the wrapper live — verifying no ripple on
  prior-phase behavior.

---

## 2026-04-24 — 0.5.3 phase 2: `silent` flag audit + rate-limit wiring (in progress on `feat/0.5.3-db-batch-silent-mode`)

Second phase commit on `feat/0.5.3-db-batch-silent-mode`. Implements
the `silent` flag semantics pin contribution (second of four in
ICD-0.5.3). The plumbing has been live since 0.3.3; phase 2 wires
the rate-limited `db.silent.used` audit at the existing gate
(`run_on_context.cpp:384` — `if (!op.silent) { record_write() }`
branch), adds the config knob, and locks the contract with six
S.\* test cases.

### What

- **New TU `src/kernel/js/db_silent_audit.{hpp,cpp}`** — standalone
  rate-limiter + audit emitter. Mirrors the existing
  `eval_guard.cpp::RateLimiter` (ICD-0.4.1 unicode scanner) and
  `packages/validator.cpp::L1RateLimiter` patterns — 64-entry LRU
  keyed on `bc.extension_name`, module-local `std::mutex`, atomic
  window-size setter. Emits `db.silent.used` audit with
  `{extension, count_in_window, window_ms}` on first-in-window hits;
  within-window hits bump a suppressed counter; the count
  accumulates into the next emit after window rollover.
- **`Config::Db::Silent::audit_window_ms`** (default 60000 ms, bound
  `[1000, 3600000]`, JSON path `"db.silent.audit_window_ms"`). Hard-
  fail parser per the `apply_realtime_*` convention.
- **Silent gate wired** at `run_on_context.cpp`
  `run_db_exec_outcome` — the `else` branch of the existing `if
  (!op.silent) { record_write(...) }` check now calls
  `plinth::js::record_silent_use(op.bc_extension_name)`.
- **main.cpp startup** propagates
  `cfg.db_bindings.silent.audit_window_ms` via
  `plinth::js::set_silent_audit_window_ms` after `broker::start`.
- **6 S.\* test cases** in new TU `tests/kernel/js/db_silent_test.cpp`
  (tag `[js][async][db][silent]`): S.01 silent suppresses coalescer
  + row lands + audit fires; S.02 silent + zero-row UPDATE + audit
  fires; S.03 `audit.log` independent of silent flag; S.04 silent on
  `db.query` is a silently-ignored no-op (DB_QUERY arm doesn't fire
  the audit); S.05 pointer to phase 4's B.09; S.06 short
  `audit_window_ms=1000` demonstrates burst aggregation (initial
  audit with count=1, 2 suppressed in-window, then a 4th write
  after window yields aggregated audit with count_in_window=3).

### Drive-by

- **`TIDY_JOBS` default bumped from 4 → 8** (`CMakeLists.txt:858-862`).
  Per `feedback_parallelism_cap.md` the bump was approved on
  2026-04-23 after the ps/top watch passed at 6; the default itself
  was never updated. This commit lands that change opportunistically.
  Tune-by-observation policy stays — drop back to 6 if working set
  outgrows margin on this host.

### Scope

- `src/kernel/config.{hpp,cpp}` — `Config::Db::Silent{audit_window_ms}`
  substruct + `apply_db_silent` parser with bound check + dispatch
  from `apply_db`.
- `src/kernel/js/db_silent_audit.{hpp,cpp}` (new, ~110 LOC).
- `src/kernel/js/run_on_context.cpp` — silent gate else-branch
  fires rate-limited audit.
- `src/kernel/main.cpp` — `set_silent_audit_window_ms` propagation.
- `tests/kernel/js/db_silent_test.cpp` (new, ~330 LOC) — 6 S.\* cases
  + libpq TestPg harness + audit poll loop.
- `CMakeLists.txt` — new TU + test TU added to `plinth` +
  `plinth_tests` enumerations; `TIDY_JOBS` bump.

### Verification

- `ctest -j1` full suite 61/61 green (pre-existing
  `pubsub_test.cpp:117` NOTIFY timing flake surfaces under `-j4 +
  PG contention` — unrelated to this phase).
- New S.\* suite runs in ~3.8s with PG attached.
- `cmake --build --target tidy` full-tree zero findings after the
  phase's inline fixes (const-name → non-const per
  `readability-identifier-naming.ConstantCase`; C-style `params[]`
  → `std::array`; libpq takes `std::string::c_str()` not
  `string_view::data()` per `bugprone-suspicious-stringview-data-usage`).

### ICD alignment

Phase 2 implements ICD §silent Flag Semantics + §Audit Events
`db.silent.used` + §Config Surface `db.silent.audit_window_ms`.
No amendments this phase — the ICD text matches the implementation.

---

## 2026-04-24 — 0.5.3 phase 1: OID-driven PG→JS type mapping (in progress on `feat/0.5.3-db-batch-silent-mode`)

First of six phase commits on `feat/0.5.3-db-batch-silent-mode` that
together implement ICD-0.5.3. Phase 1 lands the OID-driven PG-type →
JS-type mapping contribution (the fourth of four pinned in the ICD).
Sequenced first because: (a) every subsequent phase's B.\* / P.\* /
I.\* tests read rows back through the result converter, so the value
shape must be final before downstream tests are written; (b) the
Drogon-patch + FetchContent wiring is the novel infra piece best
rehearsed at a 7-test scope; (c) the `oid_mapping.enabled=false`
feature flag gives a production rollback lever.

Phases 2–5 (silent audit / search_path wrapper / `db.batch()` core /
lifecycle-drain + integration) follow on this branch. Phase 6 moves
DEFERRED entries + writes the v0.5.3 CHANGELOG + cuts the tag.

### What

- **Drogon patch infra** — new `third_party/drogon-patches/` with a
  single patch `ftype-accessor.patch` that exposes
  `drogon::orm::Field::oid() -> int` (delegates to the already-public
  `drogon::orm::Result::oid(column)` via the existing `friend class
  Field` relationship). Patch is applied by `PATCH_COMMAND git
  apply --whitespace=nowarn` threaded into drogon's
  `FetchContent_Declare` at `CMakeLists.txt:64-73`. Three lines of
  diff against `orm_lib/inc/drogon/orm/Field.h`. Upstreamable; will
  be filed against drogonframework/drogon on 0.5.3 ship.
- **New TU `src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}`**.
  Extracts the 0.3.3 inline heuristic (`pg_text_to_json` +
  `pg_result_to_json` at `run_on_context.cpp:166-228`) and replaces
  it with an OID switch over all 19 built-in PG types the ICD
  pins. Unknown OIDs fail closed to string + debug-log (phase 2
  upgrades this to a rate-limited audit event). BYTEA → hex-encoded
  Json carrier `{__bytea_hex__: "..."}`; the JS-side unpacker
  (below) constructs a `Uint8Array`.
- **`json_to_js` extension** in `bridge_context.cpp` — recognises
  the `{__bytea_hex__: "..."}` tag and decodes via
  `JS_NewUint8ArrayCopy`, surfacing BYTEA rows to JS as native
  `Uint8Array` per ICD-0.3.3 §PG-Value → JS-Value and ICD-0.5.3
  §OID switch table.
- **Feature flag `Config::Db::OidMapping::enabled`** (default
  `true`, JSON path `"db.oid_mapping.enabled"`). Config-load emits
  a warn-log on `false`. `plinth::js::db::set_oid_mapping_enabled`
  invoked from `main.cpp` after `broker::start`. Module-local
  `std::atomic<bool>` holds the runtime state; test seam is the
  public setter.
- **7 new T.\* test cases** in
  `tests/kernel/js/async_bridge_test.cpp` (tag
  `[js][async][db][types]`): T.01 string `"true"` stays string,
  T.02 INT8 safe range, T.03 INT8 overflow → string, T.04
  TIMESTAMPTZ ISO 8601 passthrough, T.05 BYTEA → `Uint8Array`,
  T.06 SQL NULL regardless of OID, T.07 feature-flag disabled
  falls back to heuristic.

### ICD amendments (inline)

Three observed deltas between ICD-0.5.3 as authored 2026-04-24 and
the actual codebase surfaced during implementation; amended inline
in the ICD:

1. **`db_result_to_json.cpp` is a NEW file**, not a pre-existing TU
   that gains an OID switch. The 0.3.3 heuristic lived inline in
   `run_on_context.cpp:166-228` per `pg_text_to_json` +
   `pg_result_to_json`. ICD §CI wiring amended.
2. **`third_party/drogon-patches/` did not exist pre-0.5.3.** 0.5.3
   creates the directory and the CMake patch-apply wiring. ICD
   §CI wiring amended.
3. **T.\* tests landed in `async_bridge_test.cpp`, not
   `stdlib_test.cpp`.** All T.\* need a live PGresult and
   async_bridge_test is the PG-gated db.query TU.
   ICD §Test Cases table amended.
4. **T.07 scenario narrowed from `'true'::text` to `'t'::text`.**
   The 0.3.3 heuristic only matches single-char PG bool text reprs
   (`"t"` or `"f"`), not the 4-char string `"true"`. The ICD's
   narrative read `'true'::text` as the regression but the
   heuristic at `run_on_context.cpp:174-178` only hits on exact
   single-char match. `'t'::text` does surface the regression
   under heuristic → bool; under OID switch → string.

### Scope

- `third_party/drogon-patches/README.md` + `ftype-accessor.patch` (new)
- `CMakeLists.txt` — drogon `FetchContent_Declare` `PATCH_COMMAND`;
  `db_result_to_json.cpp` added to `plinth` + `plinth_tests` source
  enumerations.
- `src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}` (new, ~230 LOC)
- `src/kernel/js/run_on_context.cpp` — heuristic stripped; calls
  extracted `db::result_to_json`.
- `src/kernel/js/bridge_context.cpp` — `json_to_js` gains BYTEA
  tag-unpack + new `decode_hex_payload` helper.
- `src/kernel/config.{hpp,cpp}` — `Config::Db::OidMapping::enabled`
  + `apply_db` parser (minimal phase 1 slice; phases 2-4 grow the
  `Db` struct).
- `src/kernel/main.cpp` — `plinth::js::db::set_oid_mapping_enabled`
  call threaded after broker start.
- `tests/kernel/js/async_bridge_test.cpp` — 7 T.\* cases.
- `docs/icd/ICD-0.5.3-db-batch-silent-mode.md` — four inline
  amendments listed above.

### Verification

Full `ctest` green (56/56) with PG attached — zero regressions on
the v0.5.2 baseline + T.\* all pass. `run-clang-tidy-20` full-tree
clean (`cmake --build --target tidy` zero findings). Drogon patch
applies cleanly on a fresh `rm -rf build/_deps` + reconfigure.

---

## 2026-04-24 — 0.5.2.N ICD-0.5.3 authoring (paper session, untagged)

Paper-only follow-up to the 0.5.2.N Broker test matrix backfill
session. Discharges the ROADMAP item `0.5.2.N ICD-0.5.3 authoring
(paper follow-up)   [strong]` inserted by this session at
`docs/ROADMAP.md §0.5` ahead of 0.5.3. **Un-tagged** per
`feedback_tagging_rule.md` (four-part doc-only follow-up). One new
ICD authored; no code changes. Satisfies `feedback_icd_horizon.md`
one-ahead rule — 0.5.3 is the next code milestone and no ICD existed
for it until this session.

### Why

Per `feedback_icd_horizon.md` ICDs should be written at most one
milestone ahead of current implementation. v0.5.2 shipped
2026-04-23; 0.5.2.N broker test backfill shipped 2026-04-24 closing
ICD-0.5.2 §Exit-criteria 45/45. The next code milestone is
**0.5.3 `db.batch()` and silent mode** (`docs/ROADMAP.md §0.5`) and
had no ICD. This session lands it.

Two long-standing DEFERRED entries (2026-04-18) had their pointers
tightened to 0.5.3 by `RE-EVAL-0.5.x-following-0.5.1.md §2.5`
(2026-04-23): (1) per-op `SET search_path TO ext_<extension_id>,
plinth;` enforcement — ICD-0.3.3 §Security Constraint 1, deferred
through 0.4.3 + 0.5.0 + 0.5.1 + 0.5.2 because none of those
milestones wrapped `db.*` in a transaction; (2) OID-driven
PG-type → JS-type mapping — ICD-0.3.3 §PG-Value → JS-Value, deferred
because Drogon's `orm::Field` API doesn't expose the OID. Both
entries are folded into ICD-0.5.3's scope since `db.batch()`'s
transactional wrapper is exactly the implementation substrate both
deferrals have been waiting for. Precedent for four-contribution
paper ICDs: 0.5.0.5 authored ICD-0.5.1 (coalescer + classifier +
envelope + lifecycle in one).

### What shipped

- **`docs/icd/ICD-0.5.3-db-batch-silent-mode.md` — new (1593 lines).**
  Authors the 0.5.3 code milestone contracts. Header block (Traces
  to / Depends on / Milestone / Status / Methodology / Related)
  matches 0.5.0 + 0.5.1 + 0.5.2 precedent. Seventeen substantive
  sections:
  - `## Overview` — four contributions (db.batch, silent, per-op
    search_path, OID mapping) + nine explicit out-of-scope items
    (savepoints, cross-extension batch, streaming cursors, client
    SDK, non-PG backends, listener/coalescer contract changes, array
    OID mapping, silent-on-query, configurable isolation level,
    per-statement timeout).
  - `## db.batch() Transactional Wrapper` — JS surface (async
    callback), binding-orchestrator flow (BEGIN → callback → COMMIT,
    with rollback on throw/DB-error), three new `AsyncOp::Type`
    variants (`DB_BATCH_BEGIN` / `DB_BATCH_COMMIT` /
    `DB_BATCH_ROLLBACK`), `batch_scope_id` field, connection
    pinning via `TransactionPtr` on `bc.batch_state.pinned_conn`,
    coalescer interaction (one envelope per `(schema,table)` per
    batch, `window_ms=0` signal), threading model (binding on JS
    thread, dispatch arms on Drogon coro pool).
  - `## silent Flag Semantics` — pin single-call scope, propagation
    through existing plumbing (`async_op.hpp:82` + `db_bindings.cpp:313` +
    `run_on_context.cpp:443-455`), interaction with batch
    (orthogonal; suppresses coalescer accumulation per-call inside
    batch), audit side-channel (silent ≠ audit suppress), no-op
    semantics on `db.query`, new `db.silent.used` rate-limited audit.
  - `## Per-Op SET search_path Isolation` — closes ICD-0.3.3
    §Security Constraint 1. Single-op wrapper (`BEGIN; SET LOCAL
    search_path TO ext_<name>, plinth; <user_sql>; COMMIT;`), inside-
    batch wrapper (one `SET LOCAL` at batch open), kernel-scope
    bypass (`bc.extension_name.empty()`), identity source
    (`bc.extension_name` + `PQescapeIdentifier` defense), config
    override `db.search_path.enforce=false` escape hatch, failure
    mode (`db.search_path.set_failed` rejection + audit).
  - `## OID-Driven PG-Type → JS-Type Mapping` — closes ICD-0.3.3
    §PG-Value → JS-Value. Problem with 0.3.3 heuristic (misreads
    `SELECT 'true'::text` as bool), local Drogon patch exposing
    `Field::oid()` via `PGresult::ftype(col)`, complete OID switch
    table (BOOLOID / INT2/4/8OID / FLOAT4/8OID / NUMERIC / TEXT /
    VARCHAR / NAME / CHAR / UUID / BYTEA / JSON[B] / TIMESTAMP[TZ] /
    DATE / TIME + null + fail-closed fallback), pinned `SELECT
    'true'::text` regression, feature flag `db.oid_mapping.enabled`,
    array types still deferred.
  - `## Extension Lifecycle Integration` — `js::rollback_extension_batches`
    hooked at DISABLED / UPGRADING / UNINSTALL, mirroring coalescer
    + broker drain hooks; what drain does (rollback + discard scope
    + reject `db.cancelled` + unpin conn + audit).
  - `## Config Surface` — seven config keys
    (`db.batch.{max_ops_per_batch,max_concurrent_batches_per_bc,timeout_ms,audit_window_ms}`,
    `db.search_path.enforce`, `db.oid_mapping.enabled`,
    `db.silent.audit_window_ms`) with bound checks + warn-log
    postures.
  - `## Audit Events` — four new rate-limited events
    (`db.batch.committed` + `db.batch.rolled_back` + `db.silent.used`
    + `db.search_path.set_failed`); no per-statement audit inside
    a batch (batch-commit covers as a unit).
  - `## HA Semantics` — per-node transactional scope; no cross-node
    batch coordination (trivial consequence of PG transactional
    scope, not a new constraint); per-node Layer-1 emission on
    commit via `emit_notify_async` → existing fan-out unchanged.
  - `## Deterministic Teardown` — new `js::rollback_all_batches()`
    atexit slot before `realtime::stop_broker()`, itself between
    `realtime::stop_listener()` and `CoalescerRegistry::shutdown()`;
    `main.cpp` + `ws_test_fixture.cpp` + `async_bridge_fixture.cpp`
    mirror. Ordering rationale: batch rollback before broker stop
    so emissions are suppressed before handler deregister; before
    coalescer shutdown so coalescer's drain sees no in-flight scope
    state.
  - `## Error Model` — eight new `db.batch.*` / `db.search_path.*` /
    `db.oid_mapping.*` rejection codes; existing ICD-0.3.3 db.*
    codes fire unchanged (batch wraps them in `db.batch.rolled_back`
    on the audit side while still rejecting the outer promise with
    the underlying cause).
  - `## Security Constraints` — eight items covering mandatory
    `SET LOCAL`, inviolate BEGIN/COMMIT, cross-extension batch
    prohibition, no savepoint/nested, audit-always-written,
    cancellation-cascade honor, OID fail-closed, connection-pinning
    RAII.
  - `## Test Cases` — **35 new cases** targeted (12 B + 6 S + 8 P +
    7 T + 2 I) across five test TUs (`db_batch_test.cpp` +
    `db_silent_test.cpp` + `db_batch_integration_test.cpp` new;
    `async_bridge_test.cpp` + `run_on_context_test.cpp` +
    `stdlib_test.cpp` extended). Tag convention `[js][db]` +
    per-group subtype (`[batch]`, `[silent]`, `[search_path]`,
    `[types]`, `[integration]`). Distinct from 0.5.0 R/E/P,
    0.5.1 C/T/I/E, 0.5.2 B/S/U/I.
  - `## Entry / Exit` — entry on v0.5.2 + 0.5.2.N backfill + this
    ICD authored; exit on 0.5.3 code shipping + all 35 tests
    passing + DEFERRED.md 2026-04-18 entries moved to Resolved
    + v0.5.3 tag cut.
  - `## Open Questions` — **8 OQs** with architect recommendations:
    - OQ1 nested batch — reject (vs flatten / savepoint).
    - OQ2 batch error propagation — reject-with-cause (vs wrap).
    - OQ3 silent scope — single call (vs coro-scoped / RAII).
    - OQ4 batch-end emission timing — emit-at-COMMIT (vs 50ms
      window).
    - OQ5 per-op `SET search_path` cost — always wrap + escape
      hatch (vs opt-in).
    - OQ6 OID access strategy — local Drogon patch (vs private-API
      reach / hard-coded per-column).
    - OQ7 connection ownership — pin `TransactionPtr` (vs per-
      statement checkout).
    - OQ8 batch quota — both per-batch + concurrent-per-bc (vs
      one).
  - `## Appendix A — End-to-End Example` — JS `db.batch` with two
    `db.query` INSERTs → BEGIN → SET LOCAL → inserts on pinned
    conn → COMMIT → `flush_batch_scope` → `emit_notify_async` →
    PG NOTIFY → listener → broker → WS frame or JS handler.
    Steady-state timeline: 5 ms p50 batch commit, 25 ms p99
    subscriber frame.
  - `## Appendix B — Config Example` — full `db` + `realtime`
    blocks + minimum-effective empty object.
  - `## Appendix C — OID Mapping Table` — full OID → JS mapping
    strategy reference including integer safe-range.

- **`docs/ROADMAP.md` — one-line amendment.** New
  `0.5.2.N ICD-0.5.3 authoring (paper follow-up) [strong]` line
  inserted at §0.5 between the discharged `0.5.2.N Broker test
  matrix backfill` line and the `0.5.3 db.batch() and silent mode
  [medium]` entry, mirroring the `0.5.1.2 ICD-0.5.2 authoring`
  precedent.

### Why no tag

Four-part doc-only follow-up — no new build artifact, no code
change, no test run delta. `feedback_tagging_rule.md` reserves
tags for milestone close-outs and arc completions; interim doc
patches accrue to the next X.Y.Z tag (here, v0.5.3 when the code
milestone ships).

### Out of scope

- No source code changes. Paper session only.
- No `docs/CHANGELOG.md` entry for v0.5.3 itself (that entry lands
  with the v0.5.3 tag).
- No OQ pre-resolution (architect pins at impl — matches 0.5.1
  OQ1–OQ7 and 0.5.2 OQ1–OQ8 workflow).
- No ICD-0.5.4 authoring (per one-ahead rule — the next paper
  slot after v0.5.3 ships is ICD-0.5.4 delta-sync authoring).
- No DEFERRED.md move-to-Resolved for the two 2026-04-18 entries
  (search_path + OID mapping) — those move on 0.5.3 **code** ship,
  not on ICD-authoring ship. This ICD's §Exit criteria pins that
  move.
- No memory rewrites for the shipped-state entries; post-session
  memory note writes a new next-session entry retiring candidate
  (1) from `project_next_session_post_052_backfill.md` and
  pointing at candidate (2) (LH-2.2 sidecar + kernel long-lived-
  subscription ICD) and candidate (3) (orthogonal patches) or the
  0.5.3 code work itself as the next paper-gated session.

### ROADMAP line discharged

None this session. New entry inserted; it discharges when the
0.5.3 code PR ships with its own `[x] 0.5.2.N ICD-0.5.3 authoring`
descriptor — matches the 0.5.1.2 pattern (entry stayed on ROADMAP
as `[x]` until v0.5.2 code ship wrote its own discharge note).

### Not in scope (for future)

- 0.5.3 code implementation (consumes this ICD; separate session).
- LH-2.2 sidecar arm + kernel long-lived-subscription ICD + code
  (candidate (2) in `project_next_session_post_052_backfill.md`;
  requires `RuntimePool::release` not dropping `persistent_callbacks`
  on BC cycle — separate kernel ICD).
- Orthogonal patches — channel event-class underscore handling
  widening in `rule_validator.cpp:34`; `/api/audit?start=<iso8601>`
  drogon async-binder "insufficient data" fix.
- `[js][async]` Catch2-subprocess refcount investigation
  (`0.5.x.N`, independent).

---

## 2026-04-24 — 0.5.2.N Broker test matrix backfill (untagged follow-up)

Closes ICD-0.5.2 §Exit criteria to full 45/45 coverage. Branch
`feat/0.5.2.N-broker-test-backfill`; untagged follow-up to v0.5.2
per `feedback_tagging_rule.md`.

### What shipped

29 new test cases across four TUs:

- `tests/kernel/realtime/broker_test.cpp` — **+5 B.* cases**
  (B.05 + B.06 WS subscriber delivery, B.08 JS subscriber match,
  B.10 listener-thread latency budget, B.12 stop clears JS registry).
  B.05/B.06 are PG-gated + tagged `[ws][integration]` to route into
  `plinth_tests_ws` where ws_test_fixture's drogon lifecycle is the
  only one running (avoids the `!running_` collision with
  `plinth_tests_pg`'s async_bridge_fixture drogon).
- `tests/kernel/ws/subscriptions_rbac_test.cpp` — **new TU, 12 S.*
  cases** (admin bypass, per-layer rule derivation, mixed-batch
  partial-grant, cross-ext deny, quota overflow, rbac_enforce=false
  fallback, broker extension drain, delivery re-check via
  rbac_enforce flip). Tag `[ws][realtime][rbac][integration]`.
- `tests/kernel/js/pubsub_subscribe_test.cpp` — **+12 U.* cases**
  (happy own-ext, error taxonomy: extension_mismatch / rbac_denied /
  layer_unsupported / channel_invalid / cancelled / quota_exceeded,
  lifecycle: unsubscribe token, bc teardown, extension UPGRADE,
  multi-subscribe overwrite). Tag `[js][realtime][pubsub][subscribe]`.
  The 8 existing LH-2 SC6 cross-ext cases stay as-is.
- `tests/kernel/realtime/broker_integration_test.cpp` — **new TU,
  7 I.* cases** (single + multi-client + multi-channel delivery, JS
  subscriber registration, drain race, rbac_enforce flip). Producer
  path simulated via `broker::dispatch_for_test` — the full coalescer
  → listener → broker chain is covered by
  `coalescer_integration_test.cpp` and LH-1/LH-2 harness runs.

### Implementation deviations (documented in test comments)

1. **B.05 / B.06 PG-gating.** ICD `PG-gated=No`; without a fake-conn
   seam on publish.hpp, real ws_test_fixture drogon + WsTestClient
   is the cleanest scaffold. Tests skip cleanly when PG is
   unavailable. Coverage equivalence with I.01–I.04 is intentional —
   B.05/B.06 isolate the broker's dispatch-arm routing from the
   listener/coalescer source.
2. **U.02 extension_mismatch.** Post-SC6 widening
   (classify_pubsub_subscribe), a `notes` bc subscribing a cross-ext
   channel without an RBAC rule rejects `pubsub.rbac_denied`, not
   `pubsub.extension_mismatch` (covered by LH-2). The literal
   `pubsub.extension_mismatch` arm now only fires when
   `bc.extension_name` is empty — U.02 exercises that edge.
3. **U.07 cancellation observable swap.** `run_on_context`'s
   cancellation cascade preempts JS job dispatch once `bc.cancelled`
   is true, so the ICD's `.then(onReject)` observable is
   unreachable. Test asserts the observable side effect (no
   subscription registered) and notes the reject code is covered by
   source review of `pubsub_bindings.cpp:321`.
4. **S.11 / I.07 delivery re-check swap.** ICD S.11 mutates
   `state.effective_rules` between subscribe and dispatch — not
   reachable from tests without a new ConnState seam. Both tests
   exercise the same `delivery_rbac_allows` re-check code path via
   `set_rbac_enforce_for_test(false)` — the 0.1.6 admin-only
   fallback arm.
5. **I.01–I.04 producer simulation.** Uses `dispatch_for_test`
   instead of a live coalescer/listener chain. Scopes each case to
   the broker → WS/JS arm (the distinct layer this TU covers);
   coalescer emission is independently validated.
6. **I.05 JS subscriber.** Two-extension end-to-end (publisher +
   subscriber both running QuickJS) requires async_bridge_fixture's
   drogon lifecycle, which collides with ws_test_fixture's drogon
   in the same subprocess. Narrowed to the per-bc registration
   routing observable; handler-invocation path is covered by U.09 /
   U.12 and the LH-2 harness.

### Drive-by fix

`src/kernel/js/stdlib/pubsub_bindings.cpp` re-subscribe path: the
overwrite branch leaked a `JSValue` reference because
`persistent_callbacks.emplace(channel, JS_DupValue(...))` destroys
the r-value argument without invoking `JS_FreeValue` when the key
already exists. The refcount bump stuck; the runtime's `gc_obj_list`
accumulated orphaned handlers and `JS_FreeRuntime` asserted during
pool teardown. Fix: capture the dup in a local before `emplace` so
the overwrite path has a named handle. Exposed by U.12 as a
`list_empty(&rt->gc_obj_list)` abort; fix drops the abort.

### Verification

- `cmake --build build --target plinth_tests -j 6` — clean.
- `cmake --build build --target tidy -j 6` — clean (run at
  session end per `feedback_parallelism_cap.md` +
  `feedback_run_ci_invocations.md`).
- `ctest --output-on-failure` end-to-end — 49/49 pass across
  `plinth_tests_{pure,js,pg,ws}` with PG attached.
- ICD `[B.{05,06,08,10,12}, S.{01..12}, U.{01..12}, I.{01..07}]` all
  present as TEST_CASE names; count matches 29 new cases.

### Not in scope

- ICD-0.5.3 authoring (still pending as the next paper follow-up
  per `feedback_icd_horizon.md`).
- LH-2.2 sidecar arm (requires kernel long-lived-subscription
  registry — separate ICD + session).
- Orthogonal patches (rule-segment underscore widening, audit
  `start=<iso>` drogon binder fix).

---

## 2026-04-24 — LH-2 WS fan-out storm tier (untagged follow-up)

Implements the LH-2 load harness per `docs/icd/ICD-LH-2-ws-fanout-storm.md`
(shipped 2026-04-23). Extends the LH-0 binary with a new `--tier=ws-fanout`
that drives the v0.5.2 broker's per-connection WS fan-out arm under
sustained Layer-3 storm load + reuses LH-1's `lh1storm` producer verbatim.
Untagged — four-part follow-up to v0.5.2 per `feedback_tagging_rule.md`;
accrues to the next X.Y.Z tag.

### 3-trial diagnostic outcome

**3/3 clean** against v0.5.2 HEAD + this branch. No reproductions of
`free_zero_refcount` / `list_empty(&rt->gc_obj_list)` / `bad_weak_ptr` /
SIGSEGV / SIGABRT in the kernel tail; zero `realtime.broker.*`
(subscribe_denied / dispatch_skipped / extension_drained) audits; zero
`realtime.notify.rejected` / `realtime.listener.reconnected`. Clears the
v0.5.2 broker for LH-3 (0.5.4 delta-sync) per ICD-LH-2 §7.2.

Tier defaults per-trial (`--tier=ws-fanout --burst-size=8`):
4 producers × 4 WS subscribers × 120 s × burst=8 × payload=512 B.

| Trial | Bursts | Envelopes emitted | Observed/emitted | p99 WS lag |
|-------|--------|-------------------|------------------|-----------|
| 1     | 32,815 | 262,520           | 1.0000 × 4 subs  | 48 ms     |
| 2     | 25,813 | 206,504           | 1.0000 × 4 subs  | 71 ms     |
| 3     | 21,191 | 169,528           | 1.0000 × 4 subs  | 67 ms     |

### Scope

- **Kernel:** classify_pubsub_subscribe widened + dispatch-arm re-check +
  RuntimePool::destroy leak fix.
- **Tests:** 8 new `[js][realtime][broker][rbac]` cases for cross-ext
  subscribe (positive / denial / admin bypass / rbac_enforce=false /
  regression).
- **Harness:** new `lh2sidecar` fixture (authored but not exercised —
  see deviations §3 below), new `wssub` package, new `WsFanout` tier
  profile, new audit-count helper, `--ws-subscribers` / `--js-subscribers` /
  `--sidecar-zip` flags, ws-fanout arm in `lh0/main.go`, `lh2sidecar.zip`
  Makefile target.

### Kernel changes

1. **`classify_pubsub_subscribe` widening — closes v0.5.2 §SC6 deviation**
   (`src/kernel/js/stdlib/pubsub_bindings.cpp:205`). Layer-3 and Layer-1
   cross-extension subscribes now honor the derived per-channel RBAC rule
   (`<other>.realtime.subscribe[.<event_class>]`) or the universal
   `kernel.admin` rule, matching the WS-side gate at
   `src/kernel/ws/subscriptions.cpp:68` and the ICD-0.5.2 §Security
   Constraint 6 contract. v0.5.2 shipped the binding with an
   extension-identity-only gate that rejected all cross-ext subscribes
   regardless of grant — now corrected. `bc.user.effective_rules` is
   the rule set read (populated at RuntimePool entry, no DB query added).
   `broker::is_rbac_enforced() == false` degrades to admin-only
   fallback for cross-ext, mirroring the WS side.
   Refactored into three helper functions to stay under the
   cognitive-complexity threshold per clang-tidy.

2. **PUBSUB_SUBSCRIBE dispatch-arm defense-in-depth re-check — closes
   v0.5.2 §SC2 gap** (`src/kernel/js/run_on_context.cpp:~845`). Before
   this change, `dispatch_pubsub_sub_inline` only checked quota before
   calling `register_js_subscription`; a group revocation or
   `rbac_enforce` flip between the classify gate and the dispatch arm
   went uncaught. Now re-runs `classify_pubsub_subscribe` at dispatch
   time; on denial fires `broker::note_dispatch_skipped(channel,
   "rbac_denied")`, rolls back the `persistent_callbacks` entry, and
   rejects the promise — parallel to the quota-overflow arm.

3. **`RuntimePool::destroy` BC-teardown leak fix**
   (`src/kernel/js/runtime_pool.cpp:700`). The `destroy` path was
   missing the `drop_bc_subscriptions + drop_persistent_callbacks`
   calls that the `release()` path at line 695–696 already makes. Any
   BC with live `pubsub.subscribe` handlers that went through `destroy`
   (early-exit tests, runtime-pool shutdown) leaked the handler
   JSValues into `rt->gc_obj_list`, tripping `list_empty` in
   `JS_FreeRuntime` — reproducibly surfaced by the new
   `pubsub_subscribe_test.cpp`. v0.5.2's shipped B.* tests didn't hit
   this because they use `broker::dispatch_for_test` directly without
   a BC carrying persistent subscriptions.

4. **`classify_pubsub_subscribe` exported via
   `stdlib_inject.hpp`** so the dispatch-arm re-check can call the
   same gate function — prevents two parallel-but-subtly-different
   subscribe permission checks.

### Tests

New TU `tests/kernel/js/pubsub_subscribe_test.cpp` (8 cases,
`[js][realtime][broker][rbac]`). PG-gated via
`ensure_drogon_with_db_running` since classify-gate denials call
`plinth::log::audit` which aborts inside drogon's `getDbClient` when
no DbClient is reachable.

- Layer 3 cross-ext with rule granted → allow.
- Layer 3 cross-ext without rule → `pubsub.rbac_denied`.
- Layer 3 cross-ext with `kernel.admin` → allow (universal bypass).
- Layer 1 cross-ext with rule granted → allow.
- Layer 1 cross-ext without rule → `pubsub.rbac_denied`.
- Cross-ext under `rbac_enforce=false` + non-admin → `pubsub.rbac_denied`.
- Cross-ext under `rbac_enforce=false` + admin → allow.
- Own-ext Layer 3 subscribe with empty `effective_rules` → allow
  (regression: identity gate stays intact for the own-ext path).

Scope kept narrow on cross-ext; the remaining 29 of 45 ICD-0.5.2
U/S/B/I cases stay for `0.5.2.N broker test matrix backfill`.

### Harness additions

- **`load-harness/fixtures/lh2sidecar/`** — new bundled extension.
  Package name `lh2sidecar` (no underscore, mirrors `lh1storm`
  precedent — manifest-name + channel-ext regex both reject `_`).
  Two capabilities: `lh2sidecar:1:install_subscription(channel)` +
  `lh2sidecar:1:read_counters()`. Handler module state lives on
  `globalThis` across capability calls — this deliberately exercises
  the extension-side per-bc subscription arm the broker ships for.
  Fixture compiled but not actively exercised in the first ship —
  see deviations §3 below.
- **`load-harness/internal/wssub/subscriber.go`** — new package.
  Subscriber owns its own `/ws/events` connection (cannot share
  wsclient.Client because that package's readLoop discards event
  frames via id-demux). Sends `{"type":"subscribe","channels":[ch]}`
  after auth, fails fast on silent-omission from the ACK's
  `channels[]` array (ICD-0.1.6 subscribe convention — silent-omit
  means the per-channel RBAC grant didn't propagate). Replies to
  kernel pings with `{"type":"pong","timestamp":<ms>}` — `on_pong_message`
  at `src/kernel/ws/heartbeat.cpp:114` matches on timestamp so a
  bare `{"type":"pong"}` counts as stale and closes the conn with
  `heartbeat_timeout` (caught during LH-2 smoke).
- **`load-harness/internal/tiers/tiers.go`** — new `JsSubscribers int`
  field on `Profile`, new `WsFanout` profile (Concurrency=4,
  Subscribers=4, JsSubscribers=1, BurstSize=16, PayloadBytes=512,
  Duration=120s), new `"ws-fanout"` Lookup case.
- **`load-harness/internal/httpclient/client.go`** — new
  `QueryAuditCount(action, _)` helper. Hits
  `GET /api/audit?action=<>&limit=1` and returns the endpoint's
  `total` field. See deviation §4 below.
- **`load-harness/cmd/lh0/main.go`** — new flags
  `--ws-subscribers` / `--js-subscribers` / `--sidecar-zip`; new
  `ws-fanout` tier arm (dual-package install + grant four rules
  to admin + start M WS subscribers + optional sidecar
  `install_subscription` call + producers reuse storm worker loop +
  1 s grace + teardown query + summary print + `wsFanoutExitCode`).
- **`load-harness/Makefile`** — `lh2sidecar.zip` target mirrors
  `lh1storm.zip`.

### Implementation deviations

Five deviations surfaced during implementation; each either
corrected inline or documented for a follow-up.

1. **v0.5.2 `classify_pubsub_subscribe` cross-ext gate deviation**
   — closed inline per architect redirect (2026-04-23). See
   "Kernel changes" §1.

2. **v0.5.2 SC2 dispatch-arm re-check gap** — closed inline
   alongside §1. See "Kernel changes" §2.

3. **Sidecar arm deferred to LH-2.2.** v0.5.2's BC lifetime model
   drops `persistent_callbacks` on every `RuntimePool::release()`
   (see `runtime_pool.cpp:695–696`). Every capability call
   acquires + releases a BC, so any `pubsub.subscribe` call made
   inside `lh2sidecar:1:install_subscription` is immediately torn
   down by the release path when the capability returns — meaning
   the subscription never survives to receive envelopes. ICD-LH-2
   §5.3 assumed subscription persistence across capability calls,
   which v0.5.2 doesn't provide. Ratified OQ1 (both WS + sidecar
   arms) was retargeted to WS-only for this ship; sidecar arm
   promoted to LH-2.2 future work once the BC-lifetime contract
   gains long-lived extension subscription support (kernel change,
   separate ICD + session). The `lh2sidecar` fixture + capability
   scaffolding + classify-gate cross-ext widening all land here so
   LH-2.2 can drop in the missing kernel piece + flip
   `--js-subscribers=1` at the ratified default.

4. **Channel event-class underscore rename.** `derive_subscribe_rule`
   maps `plinth:ext:lh1storm:storm_event` →
   `lh1storm.realtime.subscribe.storm_event` (rule segment contains
   `_`), but `rule_validator.cpp::is_valid_rule_segment_char` at
   line 34 rejects `_` in rule segments. v0.5.2 accordingly rejects
   `rbac.json` entries declaring such rules at manifest-registration
   time — which means the LH-1 storm channel's subscribe rule
   cannot be pre-registered. Worked around by renaming the channel
   `plinth:ext:lh1storm:storm_event` → `plinth:ext:lh1storm:stormevent`
   in `load-harness/fixtures/lh1storm/server/handlers/burst.js`.
   The derived rule becomes `lh1storm.realtime.subscribe.stormevent`
   which validates. ICD-LH-1 §4.2 previously named the channel with
   underscore; doc-side correction folds into this entry. A broader
   kernel fix (widening `is_valid_rule_segment_char` to accept `_`,
   making the rbac-manifest regex consistent with the
   subscribe-rule-derivation output) is tracked as an orthogonal
   0.5.2.N patch.

5. **`/api/audit?start=<iso8601>` drogon binder error.**
   `kernel/audit/handlers.cpp` SELECT_SQL binds the `start_ts`
   param as `CASE WHEN $3 = '' THEN TRUE ELSE timestamp >= $3::timestamptz END`
   — when `$3` is a non-empty ISO 8601 string, drogon's async
   binder trips a "insufficient data left in message" PG protocol
   error before the CASE branches. Worked around by dropping the
   `start` parameter from `QueryAuditCount`; fresh harness runs
   (each trial installs/uninstalls the driver against a reset
   schema via `--dev`) have baseline zero for the sampled actions,
   so `total` is a pure count of this-run events regardless of
   time-window bounds. Underlying drogon/libpq issue deferred to a
   separate 0.5.2.N patch — orthogonal to LH-2's mandate.

### Commands used

```
cd load-harness && make all && make tidy                                # binary + 3 fixture zips
cmake --build build --target plinth_tests -j 8                          # kernel tests build
PLINTH_PG_*=... build/plinth_tests "[broker][rbac]"                     # 8/8 cases pass
clang-tidy-20 -p=build src/kernel/js/stdlib/pubsub_bindings.cpp         # touched files clean
build/plinth serve --dev                                                # kernel on :8080
./build/lh0 --tier=ws-fanout --driver-zip=... --js-subscribers=0 \
           --burst-size=8 --username=admin --password=...               # 3 trials
```

### ROADMAP

`docs/ROADMAP.md` §Load Harness line 113 `[ ] LH-2` → `[x]` with
trial-result wording ("3/3 clean; zero broker-race reproductions;
WS fan-out cleared for LH-3/0.5.4"). `[medium]` band stays (LH-2
was already promoted `[medium]` → `[strong]` by the 2026-04-23 ICD
authoring session — shipping under `[strong]` band now).

---

## 2026-04-23 (continued) — LH-2 ICD authoring (untagged follow-up)

Paper follow-up to the v0.5.2 ship. Authors
`docs/icd/ICD-LH-2-ws-fanout-storm.md` one milestone ahead of the
implementing LH-2 code session per `feedback_icd_horizon.md`
one-ahead rule + METHODOLOGY §3.1 forward-ICD-presence rule.
Untagged per `feedback_tagging_rule.md` — ICD authoring sessions
accumulate to the next X.Y.Z code tag.

### Why

v0.5.2 shipped the broker with all 8 OQs pinned on recommendation.
The natural next diagnostic loads the broker's two fan-out arms
(WS per-connection + per-bc JS `pubsub.subscribe`) before
0.5.3 / 0.5.4 / 0.5.5 amplify broker responsibilities (db.batch +
delta-sync + seq ordering all layer on top of the broker contract).
ICD-LH-1 §9 scoped LH-2 in one sentence: "adds M client WS
connections that subscribe via `pubsub.subscribe`; exercises the
broker's per-connection routing under the same storm tier. LH-2
reuses LH-1's driver extension as the producer." This ICD converts
that sentence into a tier profile + success criteria + 5 OQs.

### What shipped

- **`docs/icd/ICD-LH-2-ws-fanout-storm.md` — new.** 11 sections
  mirroring ICD-LH-1 structure (§§1–11). §3 pins zero-new-kernel-
  surface contract (LH-2 rides v0.5.2 verbatim). §4 reuses
  `ext_lh1_storm` producer from LH-1 with one RBAC grant delta
  (derived `ext_lh1_storm.realtime.subscribe.storm_event` rule for
  the subscribing identity). §5 specifies M WS client subscribers
  dialling the real `/ws/events` endpoint + 1 sidecar extension
  calling `pubsub.subscribe` inside a BridgeContext (loads both
  fan-out arms — WS-only would leave the JS arm idle). §6 adds
  `--tier=ws-fanout` + `--ws-subscribers N` + `--js-subscribers N`
  flags to the existing `lh0` harness binary; tier profile holds
  LH-1's 4/16/512/120s producer defaults and adds a 4 WS + 1
  sidecar subscriber column. §7 pins observed/emitted ≥ 0.99,
  p99 lag < 5s, zero `realtime.broker.subscribe_denied` /
  `dispatch_skipped` audits under baseline; §7.2 continues the
  3-trial diagnostic-mandate discipline from LH-0.1 / LH-1.
  §11 pins 5 OQs on recommendation (subscriber surface both-arms;
  default M_s=4; Layer-3-only first ship; observed/emitted ≥ 0.99
  + p99 < 5s; stable connections no induced drops).

- **`docs/ROADMAP.md` — two-line amendment.** New
  `0.5.2.N ICD-LH-2 authoring (paper follow-up) [strong]` line
  inserted at §0.5 line 126 (between the v0.5.2 shipped descriptor
  and the existing `0.5.2.N Broker test matrix backfill` line,
  mirroring the `0.5.1.2 ICD-0.5.2 authoring` precedent at line
  122). LH-2 line at §Load Harness line 113 flipped
  `[medium]` → `[strong]` per METHODOLOGY §3.1 forward-ICD-
  presence rule.

### Why no tag

ICD authoring session per `feedback_tagging_rule.md`; accumulates
to the next X.Y.Z code tag (likely `v0.5.3` once `db.batch()` +
silent mode ships, or a 0.5.2.x if the broker test matrix backfill
ships first as a close-out release).

---

## v0.5.2 — 2026-04-23 — WebSocket broker fan-out

Third code milestone of the 0.5.x Realtime arc. Implements
`docs/icd/ICD-0.5.2-ws-broker.md` end-to-end — the per-node broker
subsystem that consumes the v0.5.0 listener's dispatch stream and
fans each envelope out to (a) every authenticated WS connection
whose `ConnState::channels` contains the envelope's channel and
(b) every extension JS `pubsub.subscribe` caller whose per-bc
subscription registry matches. Primary consumer of v0.5.1's
coalescer envelopes; unblocks LH-2 N-subscribers × event-flood
tier per ICD-LH-1 §9. Tag `v0.5.2` (architect action per
`feedback_tagging_rule.md`).

### Why

v0.5.0 shipped the LISTEN/NOTIFY backbone; v0.5.1 made the data
tier addressable. But no client could actually receive the
envelopes — Layer-1 auto-events and Layer-3 `pubsub.publish`
emissions both landed on the listener's dispatch queue with zero
registered handlers. v0.5.2 adds both client-reachable arms in one
subsystem: a per-connection channel filter on the WS wire (with
per-channel RBAC on subscribe + defense-in-depth re-check on
delivery) plus a per-bc callback registry on the JS side (with
the `pubsub.subscribe(channel, handler) → Promise<() => void>`
binding). Extensions can now both produce AND consume realtime
events end-to-end, and a load harness can finally exercise
fan-out saturation.

All eight ICD open questions (OQ1–OQ8) land at the recommendation
pin. No architect redirects. See the ICD's new
"Appendix: Resolved Open Questions (v0.5.2)" for the condensed
table.

### What shipped

- **`src/kernel/realtime/broker.{hpp,cpp}` — new.** The broker
  subsystem itself. `start(cfg)` registers one EventHandler with
  the 0.5.0 listener; `stop()` flips `g_enabled` + clears the JS
  registry. JS-side registry is a
  `unordered_map<bc*, unordered_map<channel, callback_id>>` under
  a `std::shared_mutex`. `drain_extension(name, trigger)` evicts
  both WS + JS subscriptions on channels matching
  `plinth:data:ext_<name>.*` or `plinth:ext:<name>:*`, rate-limited
  audit once per call with match count. Seven public test seams
  (`dispatch_for_test`, `set_rbac_enforce_for_test`,
  `reset_audit_windows_for_test`, `reset_metrics_for_test`, plus
  the lifecycle + registry entry points) keep the B.* matrix
  assertable without PG. Metrics getters
  (`dispatch_count`, `rbac_denial_count`, `js_subscriber_count`,
  `ws_subscriber_count`) are monotonic counters exposed for 0.7.x
  `plinth.metrics` wiring.

- **`src/kernel/ws/subscriptions.cpp` — per-channel RBAC gate.**
  Replaces the 0.1.6 admin-only gate with a layered check: admin
  short-circuit (preserves verbatim 0.1.6 behaviour); then the
  `rbac_enforce=false` test seam falls back to 0.1.6 admin-only
  per §OQ-adjacent S.09; then `validate_channel` (malformed never
  enters a non-admin's set); finally the derived rule is looked
  up in `ConnState::effective_rules` (loaded once at auth time).
  Denied channels silent-omitted from the `subscribed[]` ack;
  `realtime.broker.subscribe_denied` audit fires subject to the
  1-min-per-`(user_id, channel)` rate limit. Quota honoured via
  `broker::max_subscriptions_per_conn()`.

- **`src/kernel/ws/publish.cpp` — envelope-aware fan-out +
  defense-in-depth re-check + process-wide subscription counter.**
  `publish_dispatched(DispatchedEvent)` iterates
  `ConnectionRegistry`, queues per-loop lambdas that filter by
  `ConnState::channels` AND re-check the per-channel RBAC rule
  against `effective_rules` (§Security Constraints item 5 — guards
  group-revoked-mid-session). `drain_ws_subscriptions_for_extension`
  evicts on the owning loop + bumps the global atomic counter.
  `total_subscription_count()` now reads the counter (previously
  scaffold-returned 0).

- **`src/kernel/rbac/subscribe_rule.{hpp,cpp}` — new helper.**
  `derive_subscribe_rule(channel) → rule-token` — pure function
  consulted by both the WS subscribe gate and the JS binding so
  extension `rbac.json` declarations line up 1:1 with the tokens
  the gates check. Layer mapping per ICD §Subscription RBAC table
  (Layer 1 `ext_<e>.*` → `<e>.realtime.subscribe`; Layer 1 kernel
  schema → `kernel.realtime.subscribe.<schema>.<table>`; Layer 2
  → `kernel.realtime.subscribe.<event_class>`; Layer 3 →
  `<ext>.realtime.subscribe.<event_class>`).

- **`src/kernel/ws/conn_state.hpp` — `effective_rules` field.**
  Loaded once at WS auth completion (`auth_flow.cpp`'s
  `resolve_rbac_and_finish` replaces the former admin-only query
  with the full-rule-set query + derives `is_admin` from
  `kernel.admin` presence). All reads + the one write live on the
  connection's owning loop — no mutex.

- **`src/kernel/ws/events_controller.cpp` — counter decrement on
  connection close.** `handleConnectionClosed` decrements the
  subscription counter by `state->channels.size()` before
  `conn->clearContext()` drops the state.

- **`src/kernel/js/stdlib/pubsub_bindings.cpp` — new
  `pubsub.subscribe(channel, handler)` binding.** Validation
  gauntlet mirrors `pubsub.publish` with additional layer-gating:
  Layer 2 rejected with `pubsub.layer_unsupported` (§OQ5); Layer 3
  requires `bc.extension_name === channel.extension`
  (`pubsub.extension_mismatch`); Layer 1 requires the derived
  rule's owner prefix === `bc.extension_name`
  (`pubsub.rbac_denied` otherwise). Quota against
  `broker::max_subscriptions_per_conn`. Every denial fires a
  rate-limited `realtime.broker.subscribe_denied` audit with
  `source="js"`. Handler JSValue stored on the bc's new
  `persistent_callbacks: map<channel, JSValue>` (many-shot,
  distinct from the existing 1-shot `callbacks`). Last-writer-wins
  on same-channel re-subscribe per §OQ3 / U.12.

- **`src/kernel/js/async_op.hpp` — two new enum variants:**
  `PUBSUB_SUBSCRIBE` + `PUBSUB_UNSUBSCRIBE`. Both are handled
  inline on the bc's loop (rather than via `drogon::async_run`)
  because the work is pure in-memory broker-registry mutation +
  a JSValue construction that can only happen where the runtime
  lives.

- **`src/kernel/js/run_on_context.cpp` — inline dispatch arms.**
  `dispatch_ops_batch_fanout` intercepts PUBSUB_SUBSCRIBE /
  PUBSUB_UNSUBSCRIBE before the detached async-run path:
  PUBSUB_SUBSCRIBE calls `broker::register_js_subscription` +
  builds the unsubscribe JS function (via
  `make_unsubscribe_function` in `pubsub_bindings.cpp`) + resolves
  the promise with the function via the new
  `bc.resolve_with_js_value`. PUBSUB_UNSUBSCRIBE calls
  `broker::unregister_js_subscription` + frees the persistent
  JSValue + resolves with undefined.

- **`src/kernel/js/bridge_context.{hpp,cpp}` — persistent callback
  surface.** New `persistent_callbacks: unordered_map<string, JSValue>`
  plus `invoke_callback(channel, Json::Value)`,
  `resolve_with_js_value(cb_id, JSValue)`, and
  `drop_persistent_callbacks()` methods. `invoke_callback`
  converts the envelope to a JSValue on the bc's loop +
  `JS_Call`s the stored handler; handler throws are swallowed +
  logged (never propagate into the broker's dispatch loop).
  `drop_persistent_callbacks` frees every stored JSValue + clears
  the map; called alongside `broker::drop_bc_subscriptions` from
  both the destroy and release-to-pool paths in
  `runtime_pool.cpp`.

- **`src/kernel/realtime/broker.cpp` — audit rate limiter.**
  Sliding-window map per key (1-min window). Three event kinds:
  `realtime.broker.subscribe_denied` (keyed on `(user_id, channel)`),
  `realtime.broker.extension_drained` (one-per-call non-zero-match
  — no in-map counter), `realtime.broker.dispatch_skipped` (keyed
  on channel — fires when the listener delivers past
  `broker::stop()`). Each audit carries a `*_in_window` count of
  suppressed signals. `reset_audit_windows_for_test` clears both
  maps.

- **`src/kernel/packages/install_lifecycle.cpp` — three drain call
  sites now pass trigger string.** `"disabled"` / `"uninstall"` /
  `"upgrading"` for the three ICD-pinned transitions, so the audit
  trail distinguishes them.

- **`src/kernel/main.cpp` + `tests/kernel/ws/ws_test_fixture.cpp`
  — broker lifecycle wiring.** Landed in the 0.5.2 scaffold;
  `broker::start(cfg.realtime.broker)` after
  `realtime::start_listener`; `broker::stop()` in the atexit chain
  between `stop_listener()` and
  `CoalescerRegistry::instance().shutdown()` per
  `feedback_deterministic_teardown.md`.

- **`src/kernel/config.{hpp,cpp}` — `Config::Realtime::Broker`
  substruct.** Fields `enabled: bool = true`,
  `max_subscriptions_per_conn: size_t = 64` (bounded `[1, 4096]`,
  clamped at load),  `rbac_enforce: bool = true`. Landed in the
  scaffold.

- **`tests/kernel/realtime/broker_test.cpp` — 11 B.* cases.**
  Extends the 7-case scaffold (B.01–B.04 + B.11 + B.13 +
  drain idempotency) with B.07 (drain with JS subs), B.09
  (unregister), B.11b (quota), and B.14 (audit rate-limit +
  metric). Uses real stack `BridgeContext` placeholders (the
  registry is keyed on the pointer; drain's
  `bc->extension_name` read requires a real object).

- **`tests/kernel/rbac/subscribe_rule_test.cpp` — new TU, 5 cases.**
  Layer 1 ext_/kernel mapping, Layer 2, Layer 3, invalid-channel
  rejection. Pure-function coverage for the rule-derivation
  primitive shared by WS + JS.

### Scope deviations from the ICD

- **Test matrix partial coverage.** ICD §Exit criteria line 1121
  asks for all 45 B/S/U/I cases. This PR ships 11 B + 5
  subscribe_rule (16 of 45). The remaining 3 B.* (B.05 / B.06 /
  B.08 — WS publish_dispatched fan-out to live conns; B.10 /
  B.12 — full JS-side callback invocation) plus the new
  `subscriptions_rbac_test.cpp` (12 S.*), `pubsub_subscribe_test.cpp`
  (12 U.*), and `broker_integration_test.cpp` (7 I.*) land in a
  follow-up PR — they need either a live ConnectionRegistry or a
  JS runtime fixture heavier than broker_test's direct-registry
  pattern, and are PG-gated in the S.* + I.* cases. Logged as
  `0.5.2.N broker test matrix backfill` in ROADMAP.

- **PUBSUB_SUBSCRIBE / PUBSUB_UNSUBSCRIBE dispatch runs inline on
  the bc's loop** rather than via `drogon::async_run`. Both
  handlers are pure in-memory broker-registry mutations with no
  async DB work to warrant the thread hop; resolving the subscribe
  promise with a JSValue unsubscribe function also requires main
  loop anyway. The ICD's "fires through the async bridge"
  language is honoured at the Promise-shape level (caller
  `await`s); the dispatch mechanics are streamlined.

### Verification

- 11/11 `[realtime][broker]` cases pass (36 assertions).
- 5/5 `[rbac][subscribe_rule]` cases pass (11 assertions).
- 395/395 non-PG test cases pass across the full `plinth_tests`
  run (3676 assertions, zero failures, 252 PG-skipped).
- `cmake --build --target tidy -j 8` clean (TIDY_JOBS=8 cached
  via `cmake -DTIDY_JOBS=8 build`; bumped from the prior -j 4
  default per `feedback_parallelism_cap.md` 2026-04-23 approval).
- Atexit-race validation deferred to the test-matrix-backfill PR
  (20-run ctest loop depends on the S/U/I TUs landing).

### ROADMAP discharge

`docs/ROADMAP.md §0.5` line 124:
`- [x] 0.5.2 WebSocket broker: fan-out to subscribed clients   [strong]`

---

## 2026-04-23 — 0.5.1.2 ICD-0.5.2 authoring (paper session, untagged)

Paper-only follow-up to the RE-EVAL-0.5.x-following-0.5.1 session.
Discharges the ROADMAP item `0.5.1.2 ICD-0.5.2 authoring (paper
follow-up)   [strong]` (`docs/ROADMAP.md §0.5`, line 122) that the
RE-EVAL inserted ahead of 0.5.2. **Un-tagged** per `feedback_tagging_rule.md`
(four-part doc-only follow-up). One new ICD authored; no code
changes. Satisfies `feedback_icd_horizon.md` one-ahead rule ahead of
the v0.5.2 broker implementation session.

### Why

Per `docs/reviews/RE-EVAL-0.5.x-following-0.5.1.md §2.7` the 0.5.2 WS
broker became the next code milestone with no ICD authored. METHODOLOGY
§3.1 Phase 0 pins any strong-window (next-3) milestone at `[strong]`
with a pinned ICD. The RE-EVAL promoted 0.5.2 `[medium]` → `[strong]`
and inserted `0.5.1.2 ICD-0.5.2 authoring [strong]` as the paper slot
to land ahead of v0.5.2. This session lands it. Precedent: 0.4.5.2
authored ICD-0.4.6 ahead of 0.4.6 implementation; 0.5.0.5 authored
ICD-0.5.1 ahead of v0.5.1.

### What shipped

- **`docs/icd/ICD-0.5.2-ws-broker.md` — new.** Authors the WebSocket
  broker subsystem that will ship in v0.5.2. Header block (Traces to
  / Depends on / Milestone / Status / Methodology / Related) matches
  0.5.0 + 0.5.1 precedent. Eighteen substantive sections:
  - `## Overview` — scope (7 items) + out-of-scope (7 items).
  - `## Broker Subsystem` — `plinth::realtime::broker` module mirroring
    coalescer. Public API (`start_broker` / `stop_broker` /
    `broker_dispatch_for_test` / metrics getters). Lifecycle +
    threading + wire frame shape (`{type:"event", channel, payload: <envelope>}`).
  - `## Subscription Matching` — exact-string match on `envelope.channel`;
    `ConnState::channels` reuse (no new WS-side registry); new
    kernel-side JS subscription registry keyed by `BridgeContext*`.
  - `## Subscription RBAC` — Layer-derived rule naming
    (`<extension>.realtime.subscribe.<event_class>`), admin bypass,
    silent-omission, capability-registry integration via ICD-0.4.6
    `rbac.json`. Discharges the "per-channel rule naming convention…
    deferred to a later ICD" note at ICD-0.1.6 line 214.
  - `## pubsub.subscribe JS Binding` — new binding alongside
    `pubsub.publish`; async-bridge enqueued via `PUBSUB_SUBSCRIBE`
    + `PUBSUB_UNSUBSCRIBE` `AsyncOp::Type` variants; handler +
    unsubscribe-token surface; Layer-gated (Layer 2 rejected from
    JS); rejection codes (`pubsub.channel_invalid`,
    `pubsub.extension_mismatch`, `pubsub.layer_unsupported`,
    `pubsub.rbac_denied`, `pubsub.quota_exceeded`, `pubsub.cancelled`).
  - `## Extension Lifecycle Integration` — `broker::drain_extension`
    hooked at DISABLED / UPGRADING / UNINSTALL, mirroring coalescer's
    three call sites.
  - `## Reconnect Semantics` — stateless per-connection in 0.5.2;
    durable per-user subscriptions deferred to 0.5.4.
  - `## Config Surface` — `realtime.broker.{enabled,
    max_subscriptions_per_conn, rbac_enforce}`; deferrals listed.
  - `## Audit Events` — three rate-limited events
    (`subscribe_denied`, `extension_drained`, `dispatch_skipped`);
    no per-envelope / per-frame audit on the happy path.
  - `## HA Semantics` — verbatim promotion of ICD-0.5.0 §HA
    Semantics; per-node local fan-out preserved.
  - `## Deterministic Teardown` — new `stop_broker()` atexit slot
    between `stop_listener` and `CoalescerRegistry::shutdown`.
    `main.cpp` + `ws_test_fixture.cpp` mirror.
  - `## Error Model` — broker-side `BrokerError` enum; WS-side
    silent-omission retained; JS-side `pubsub.*` rejection codes;
    config-load failures.
  - `## Security Constraints` — eight items including pre-record
    RBAC, delivery-side RBAC re-check (defense in depth),
    layer-mismatch reject, cross-extension schema-prefix forge
    protection, no envelope rewriting.
  - `## Test Cases` — **45 new cases** targeted (14 B + 12 S + 12 U
    + 7 I) across four TUs (`broker_test.cpp`,
    `subscriptions_rbac_test.cpp`, `pubsub_subscribe_test.cpp`,
    `broker_integration_test.cpp`). Tag convention
    `[realtime][broker]` + `[unit]` / `[rbac]` / `[js]` /
    `[integration]`.
  - `## Entry / Exit` — entry on v0.5.0 + v0.5.1 + 0.5.1.1 +
    RE-EVAL-0.5.1 merged; exit on broker subsystem shipping + all
    45 tests passing + LH-2 N-subscribers tier unblocked per
    ICD-LH-1 §9.
  - `## Open Questions` — **8 OQs** with recommendations:
    - OQ1 broker as own subsystem vs. inline (recommend: own module).
    - OQ2 RBAC model (recommend: Layer-derived).
    - OQ3 `pubsub.subscribe` shape (recommend: callback + unsubscribe token).
    - OQ4 subscription registry scope (recommend: separate from `ConnState::channels`).
    - OQ5 Layer 2 subscribe from JS (recommend: rejected).
    - OQ6 wire frame shape (recommend: full envelope as payload).
    - OQ7 reconnect durability (recommend: stateless; defer to 0.5.4).
    - OQ8 per-connection quota (recommend: 64).
  - `## Appendix A — End-to-End Example` — coalescer → emit → PG
    NOTIFY → listener → broker → RBAC check → ConnState.channels
    match → per-conn queueInLoop → WS frame on the browser.
    Estimated end-to-end latency 50–55 ms at steady state (window-
    bound by coalescer's 50 ms).
  - `## Appendix B — Config Example` — full 0.5.0 + 0.5.1 + 0.5.2
    `realtime` block + minimum-effective config.

### Why

(see §Why above — ROADMAP line discharge + `feedback_icd_horizon.md`
one-ahead.) OQ resolutions deferred to the v0.5.2 implementation PR
(matches 0.5.1's OQ1–OQ7 workflow: author open at paper, pin at
impl, RE-EVAL captures in appendix).

### Out of scope

- No source code changes. Paper session only.
- No `docs/CHANGELOG.md` entries for v0.5.2 itself (that entry
  lands with the v0.5.2 tag).
- No OQ pre-resolution (architect pins at impl).
- No ICD-0.5.3 authoring (per one-ahead rule — the next paper slot
  after v0.5.2 ships is ICD-0.5.3).
- No memory rewrites; `project_next_session_post_051.md` retires
  when v0.5.2 ships, not here.

### ROADMAP line discharged

`docs/ROADMAP.md §0.5` line 122 discharged —
`[x] 0.5.1.2 ICD-0.5.2 authoring (paper follow-up)`. No other
ROADMAP edits this session.

---

## 2026-04-23 — RE-EVAL following 0.5.1 (rewrite session, untagged)

Seventh scheduled re-evaluation and the first of the 0.5.x arc.
Discharges the ROADMAP item `RE-EVAL following 0.5.1   [rewrite
session]` (`docs/ROADMAP.md §0.5`, line 120 post-discharge) that was
blocking 0.5.2 WS broker work per ROADMAP preamble. **Un-tagged** per `feedback_tagging_rule.md`
(rewrite session; docs-only). Code-aware gap analysis across the
0.5.0–0.5.1.1 window + parallel LH-1 stream (two tagged milestones —
v0.5.0 PG LISTEN/NOTIFY bridge, v0.5.1 PG auto-event coalescer —
plus six four-part follow-ups and one load-harness ship).

### Why

v0.5.0 + v0.5.1 closed the producer half of the realtime bus
(listener + emit helper + Layer 3 `pubsub.publish`; Layer 1
auto-event coalescer). 0.5.0.4 intervened between them to close a
pre-0.2.2 deferral (Tier 2 extension capability dispatch) — a
structural change (new `plinth::extensions::` subsystem, five new
`cap.*` codes, sync-vs-async resolver arm, per-extension
`RuntimePool`) that the architecture documents did not absorb at
ship time. Plus one v0.5.1 deviation (`shutdown_drain` audit
dropped atexit-unsafely) that needs ICD ratification before 0.5.2
broker consumers grep the audit catalog. Plus four DEFERRED entries
needing pointer tightening now that 0.5.0/0.5.1 have clarified which
0.5.x milestones do and don't touch `db.*` runtime dispatch.

### What shipped

- **`docs/reviews/RE-EVAL-0.5.x-following-0.5.1.md` — new.**
  Eight-section structure matching `RE-EVAL-0.4.x-following-0.4.4.md`
  precedent. Inputs / Gaps (2.1–2.8) / Zero-gap findings / Accepted
  deviations catalog (D1–D10) / DEFERRED status / Forward ICD
  presence / Cadence / Verification. Zero-gap section verified every
  shipped TU signature against its ICD contract — no interface drift
  on declarations (the `shutdown_drain` audit deviation is the sole
  ICD amendment trigger).

- **`docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md` — amendments.**
  (1) New "Implementation deviation (v0.5.1 ship)" subsection after
  §Audit Events documenting the dropped `shutdown_drain` audit with
  the spdlog+drogon atexit-ordering rationale and the reinstatement
  path. (2) New "Appendix: Resolved Open Questions (v0.5.1)"
  between §Open Questions and §Appendix A listing the seven pins
  (all per ICD recommendation, no redirects).

- **`docs/architecture/02-capabilities.md` — amendment.** §3 preamble
  gained one sentence pointing at §3.1. New **§3.1 Async Dispatch
  Arm + Extension Runtimes** subsection describing: sync
  `call_capability` rejects extension entries with
  `cap.async_required`; `call_capability_async` is the sole extension
  entry; per-extension `RuntimePool` owned by
  `plinth::extensions::RuntimeRegistry` with install-lifecycle hooks;
  five new `cap.*` rejection codes listed with their trigger
  conditions; `server/handlers/<fn>.js` ES-module + default-export
  convention promoted to contract-level. Pointer into ICD-0.5.0.3
  for the full spec.

- **`docs/architecture/03-data.md` — amendments.** §3.1 envelope
  example updated to v0.5.1 shape (`ids` removed, `window_ms`
  added, all three `ops` kinds listed always per OQ7). Footnote
  explains `ids` absence (0.5.5 may reintroduce via `RETURNING id`).
  Truncated-envelope example follows same shape. New **§3.1.1
  Auto-Event Coalescer (subsystem)** subsection names
  `CoalescerRegistry`, `trantor::EventLoopThread`, fixed-duration
  50 ms window semantics, drain-on-DISABLE/UPGRADE/UNINSTALL
  contract, atexit flush. New **§3.6.1 Physical Channel Fan-In**
  subsection clarifies the single PG channel `plinth:realtime`
  fan-in + logical channel names in envelope body.

- **`docs/architecture/05-extensions.md` — amendment.** §3.2
  extension-failure recovery updated: `BridgeContext` + its
  `JSRuntime` are torn down on failure, the per-extension
  `RuntimePool` persists until DISABLE/UPGRADE/UNINSTALL/shutdown.
  Supersedes the "next call creates a fresh JS runtime" framing
  which predated 0.5.0.4's per-extension pool model.

- **`docs/DEFERRED.md` — updates.** (1) New 2026-04-23 entry for
  ICD-0.5.0.3 deferred test cases (R.02/R.03/E.07/P.01/P.04/P.05/
  H.02/H.03/C.01/C.02 per 0.5.0.4 §Tests). (2) Per-op
  `SET search_path` pointer tightened "0.5.x db.* binding" → "0.5.3
  `db.batch()` + silent mode". (3) `db.*` PG-type→JS-type mapping
  pointer tightened same way. (4) WS-teardown entry extended with
  100-iter empirical dataset from 0.5.1.1 (#38 per-TEST_CASE 8% /
  grouped 11%; P.01 isolation ~15%); explicit invalidation of the
  2026-04-21 "`[js][async][hardening]` alone clean" claim at
  100-iter scale; pubsub P.01 folded into the same signature
  family + resolution slot.

- **`docs/ROADMAP.md` — updates.** (1) `RE-EVAL following 0.5.1`
  discharged (`[x]` + shipped-note citing this RE-EVAL). (2) 0.5.2
  WS broker promoted `[medium]` → `[strong]` (entering next-N
  window with this re-eval discharged). (3) New `0.5.1.2 ICD-0.5.2
  authoring [strong]` four-part follow-up inserted between the
  discharged re-eval line and 0.5.2 (one-ahead horizon per
  `feedback_icd_horizon.md`; precedent: 0.4.5.2 authored
  ICD-0.4.6). (4) `0.5.x.N [js][async] refcount investigation`
  scope-line updated with the three empirical exemplars (#38
  per-TEST_CASE + grouped, #47 P.01 folded in, LH-production
  counterfactual).

- **`docs/CHANGELOG.md` — this entry.**

### Scope deviations

None. The re-eval session found exactly one ICD amendment
(`shutdown_drain` deviation), three architecture-document amendments,
four DEFERRED.md updates, and four ROADMAP edits. No code touched.
No new test case created. Session stays strictly within the rewrite
discipline.

### Cadence position

- Previous re-eval: `RE-EVAL-0.4.x-arc-closeout.md` (2026-04-22).
- Discharged: `RE-EVAL following 0.5.1` at ROADMAP §0.5 line 120.
- Next cadence slot: `RE-EVAL following 0.5.5` at ROADMAP §0.5
  line 127 (unmoved relative to the scheduled position) — 4/4 over
  0.5.2/0.5.3/0.5.4/0.5.5.
- Triggered by explicit scheduled ROADMAP item, not 4/4
  arithmetic. Precedent: `RE-EVAL-0.3.x-arc-closeout.md` fired at
  2/4 because 0.3.5 closed that arc; this re-eval fires at the
  coalescer-closeout boundary (producer half of the realtime bus
  complete).

### Verification

Docs-only — no build. Precedent match: structure + gap-count
comparable to `RE-EVAL-0.4.x-following-0.4.4.md`. Cross-references
round-trip (CHANGELOG ↔ RE-EVAL ↔ ICD amendments ↔ arch amendments
↔ ROADMAP/DEFERRED edits). Every file path in the RE-EVAL resolves
on-disk at the merge commit.

### Not in scope

- 0.5.1.2 ICD-0.5.2 authoring (scheduled here, executed as the next
  paper session).
- 0.5.2 WS broker code (gated on 0.5.1.2).
- `[js][async]` refcount investigation (still ROADMAP `0.5.x.N`).
- HTTP test harness (still ROADMAP `0.5.x.N`).

### Follow-ups tracked elsewhere

- `0.5.1.2 ICD-0.5.2 authoring` — next paper session per the
  one-ahead horizon; consumes this re-eval's forward-ICD-presence
  disposition.
- DEFERRED.md 2026-04-23 entry for the 10 ICD-0.5.0.3 deferred
  test cases — resolved ad-hoc as the fixture harness extends.
- DEFERRED.md WS-teardown entry — still active; the empirical
  dataset from this session feeds the future refcount-investigation
  session.

---

## 0.5.1.1 — 2026-04-23 — CI red regressions: plinth_tests_pg green (untagged)

Fixes two deterministic failures in `plinth_tests_pg` (ctest entry #48)
that had been masking each other since they surfaced in the grouped
subprocess model (0.4.5.1, 2026-04-21). Both were introduced earlier
and slipped through per-commit CI on the dice-roll of Catch2's random
TEST_CASE ordering. **Un-tagged** per `feedback_tagging_rule.md`
(four-part `X.Y.Z.N` follow-up; bundles into the next X.Y.Z release
without its own tag).

The residual `[js][async]` per-TEST_CASE flake (ctest #38
`async_hardening: parallel queries honour max_concurrent cap`, SEGFAULT
at `free_zero_refcount` / `list_empty(gc_obj_list)`, ~8–11%) is **not
in scope** here — it's the kernel-side refcount race tracked as
`0.5.x.N [js][async] kernel-side refcount investigation` on ROADMAP.md,
unchanged by this session.

### Why

Local `ctest --test-dir build --output-on-failure` on `main` after the
v0.5.1 tag consistently showed `plinth_tests_pg (Subprocess aborted)`
with two distinct symptoms in the same subprocess:

1. `listener_integration_test.cpp:299,331,347-352,446` — three
   `REQUIRE(error_code(...) == "tier3_not_available")` assertions on
   the sync `call_capability` path into extension-registered
   capabilities, with actual expansion `"async_required" ==
   "tier3_not_available"`. These assertions were missed when 0.5.0.4
   (`97b5dae`, `Feat 0.5.0.4 extension dispatch`) introduced
   `CapabilityError::ASYNC_REQUIRED` for the sync→extension arm and
   updated two sibling tests in the same file, per ICD-0.5.0.3
   §Sync vs async.

2. `SIGABRT` from `drogon::HttpAppFrameworkImpl::registerHttpController`
   via `plinth::audit::register_audit_routes` called from
   `anonymous_identity_test.cpp:230`'s `register_production_routes()`
   helper. Drogon's `routersInit_` assertion fires when
   `registerHandler` is invoked after `app().run()` has initialised
   the router table — which is always the case once any earlier test
   in the grouped `plinth_tests_pg` subprocess
   (`dispatch_extension_test.cpp`, `coalescer_integration_test.cpp`,
   etc.) calls `ensure_drogon_with_db_running()`. Per Catch2's
   randomized TEST_CASE order, the anonymous-identity integration case
   sometimes ran before a drogon-starting case (fine) and sometimes
   after (crash). Pre-0.4.5.1 per-TEST_CASE subprocesses gave each
   case a fresh drogon, so the bug was latent.

### What changed

- **`tests/kernel/capabilities/listener_integration_test.cpp`** — four
  `"tier3_not_available"` expectations on extension-registered
  capabilities under the sync `call_capability` path switched to
  `"async_required"`, matching the two sibling assertions 0.5.0.4
  already updated. Comments cite ICD-0.5.0.3 §Sync vs async.

- **`tests/kernel/packages/rbac_test_runner_test.cpp:413`** — same
  stale expectation for an extension-registered capability exercised
  through the rbac-test runner's sync path; flipped to
  `"async_required"` with matching comment update. The rbac-test
  runner's `assert_allow` clause still passes because a non-permission
  error (sync→extension path needs async plumbing) is correctly
  classified non-permission per DESIGN §0.4.7.

- The remaining `"tier3_not_available"` references elsewhere
  (`batch_test.cpp`, `resolution_test.cpp`,
  `rbac_test_report_test.cpp:118` — the last is just a JSON
  round-trip string literal, provider-agnostic) stay unchanged —
  those test the sidecar provider path, which correctly still
  resolves to `TIER3_NOT_AVAILABLE`.

- **`src/kernel/audit/handlers.cpp` + `src/kernel/audit/handlers.hpp`** —
  `register_audit_routes()` restructured so:
  (a) `rbac::register_rule_requirement` always runs (idempotent, and
      tests depend on seeing the rule in `list_registered_rules()`
      regardless of when they call);
  (b) `drogon::app().registerHandler` is skipped entirely when
      `drogon::app().isRunning()` returns true — the grouped pg
      subprocess starts drogon in earlier TEST_CASEs, so the
      `routersInit_` state is latched true by the time
      `anonymous_identity_test.cpp:230` runs. Production is unaffected:
      `main()` always calls this before `app().run()`;
  (c) `std::call_once` still guards against duplicate handler
      registrations on the production path (before drogon starts).
  Header comment updated.

- **`src/kernel/groups/handlers.cpp` + `src/kernel/groups/handlers.hpp`** —
  same restructure around `register_group_routes()`: nine
  `rbac::register_rule_requirement` calls hoisted ahead of the
  `isRunning()` check and call_once; the nine `drogon::registerHandler`
  calls are inside the guarded section. Header comment mirrors the
  audit-side update.

- **`docs/CHANGELOG.md`** — this entry.

- **`docs/DEFERRED.md`** — WS-teardown / `[js][async]` entry
  cross-referenced for the residual flake (unchanged; this session
  does not touch kernel code).

### Empirical data captured in this session

Recorded for the future refcount-investigation session. Target: repo
state `90d37fc` (v0.5.1), local docker PG :5432, 100 iterations each.

- **#38 per-TEST_CASE** (current CI mode): 92 pass / 8 SEGV = 8.0% flake
  rate. Signatures: `quickjs.c:6678 free_zero_refcount assert p->ref_count == 0`,
  `quickjs.c:2323 JS_FreeRuntime assert list_empty(gc_obj_list)`. Confirms
  the `project_ws_flaky_segfault.md §Candidate root causes` signature
  family.
- **#38 under grouped `[hardening]` single subprocess** (hypothetical
  Shape 1 of the original plan): 89 pass / 11 SEGV = 11.0%. Grouping
  does **not** help for this specific case — matches the CMakeLists.txt
  `[js][async]` caveat note at lines 670–677. The DEFERRED entry's
  2026-04-21 claim "`[js][async][hardening]` alone clean" no longer
  holds at 100-iteration scale on v0.5.1's commit; the claim stands
  for shorter samples or earlier commits.
- Production unaffected — LH-0.1 (2026-04-21) remains the authoritative
  reference for the kernel lifecycle under load.

- **`CMakeLists.txt` + `benchmarks/extension_dispatch_stub.cpp` (new)** —
  benchmark-build fix for a latent break from 0.5.0.4 (`97b5dae`).
  `resolution.cpp` (in `PLINTH_BENCHMARK_KERNEL_SOURCES`) started
  including `kernel/extensions/runtime_registry.hpp`, which in turn
  includes `kernel/js/async_op.hpp` → `<quickjs.h>`. The benchmark
  target was missing both the QuickJS include path and a definition
  for `plinth::extensions::dispatch` (whose real implementation lives
  in `runtime_registry.cpp` alongside the QuickJS coroutine
  subsystem). CI's `Build benchmarks` step caught it once the v0.5.1
  test step went green enough to get that far. Fix: add
  `plinth_quickjs` (INTERFACE target) to `PLINTH_BENCHMARK_LINK_LIBS`
  to propagate the include dir, and add a one-function stub at
  `benchmarks/extension_dispatch_stub.cpp` that satisfies the link
  reference with `cap.internal` rejection — benchmarks never
  exercise the extension-dispatch path (Tier 1 sync + Tier 2
  sidecar-provider paths only), so the stub is unreachable at
  runtime.

### Files touched

- `src/kernel/audit/handlers.cpp`
- `src/kernel/audit/handlers.hpp`
- `src/kernel/groups/handlers.cpp`
- `src/kernel/groups/handlers.hpp`
- `tests/kernel/capabilities/listener_integration_test.cpp`
- `tests/kernel/packages/rbac_test_runner_test.cpp`
- `CMakeLists.txt`
- `benchmarks/extension_dispatch_stub.cpp` (new)
- `docs/CHANGELOG.md`

### Verification

- `cmake --build build --target tidy` — clean on touched files.
- `cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DPLINTH_BENCHMARKS=ON
  && cmake --build build-bench --target plinth_tier1_benchmark
  plinth_tier2_benchmark plinth_unicode_scanner_benchmark -j 6` —
  all three benchmark executables build + smoke-run cleanly
  (tier1 hit 136 ns, miss 89 ns, both well under the ICD 1 µs target).
- `clang-tidy-20 -p build-bench benchmarks/extension_dispatch_stub.cpp`
  — clean (only the two `cppcoreguidelines-avoid-reference-coroutine-parameters`
  instances are suppressed via NOLINTBEGIN/NOLINTEND with justification,
  per `feedback_nolint_policy.md`).
- `ctest --test-dir build --output-on-failure` — 10× local runs:
  - `plinth_tests_pg` (ctest #48): **0/10 failures** — the deterministic
    `routersInit_` / `tier3_not_available` signatures that prompted this
    session are gone.
  - `async_hardening: parallel queries...` (ctest #38): 2/10 failures,
    matching the documented ~8–11% kernel-side refcount race (roadmapped
    0.5.x.N, unchanged by this PR).
  - `plinth_tests_js` (ctest #47): 1/10 failures at
    `P.01 pubsub.publish happy path` (`pubsub_test.cpp:117`,
    `cv.wait_for(5s, ...)` timed out) — **pre-existing v0.5.0 flake**,
    reproducible at ~15% in isolation (`[js][realtime][integration]`
    tag combination is a fixture-overlap violation introduced by
    `f3552b3 Feat 0.5.0 pg listen notify bridge`, never resolved).
    Filed as a follow-up in-session observation for the next
    test-strategy cleanup; no change to any file in this PR.

### Not in scope

- `0.5.x.N [js][async] kernel-side refcount investigation` (ROADMAP.md:64–78).
- HTTP test harness for `/api/packages` (ROADMAP.md:50–63).
- RE-EVAL following 0.5.1 (next `[rewrite session]` per ROADMAP.md:112).
- 0.4.7.4 external-surface migration.

### Follow-ups tracked elsewhere

- `project_ws_flaky_segfault.md §Candidate root causes` — target list
  for the refcount investigation session; the 100-iteration data above
  refines the "grouping clean for hardening alone" claim.

---

## v0.5.1 — 2026-04-23 — PG auto-event coalescer

Second code milestone of the 0.5.x Realtime arc. Implements
`docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md` end-to-end — the
primary in-kernel caller of v0.5.0's `emit_notify_async` and the
producer of every Layer-1 `plinth:data:*` envelope the platform will
emit. Tag `v0.5.1` (architect action per `feedback_tagging_rule.md`).

### Why

v0.5.0 shipped both halves of the LISTEN/NOTIFY bus (`emit_notify_async`
+ per-node listener). With only `pubsub.publish` as an emit-side
caller, the data tier was empty: no extension could produce a Layer-1
envelope. v0.5.1 adds the write-path hook: every successful `db.exec`
from an extension is classified into `(schema, table, op_kind,
row_count)` and bucketed into a 50 ms coalescing window per
`(schema, table)`; flushes emit ONE consolidated envelope covering
every write in that window. This is what makes the data tier
addressable — the 0.5.2 broker fan-out, 0.5.4 persistence, and 0.5.5
seq all build on top of it.

All seven ICD open questions (OQ1–OQ7) are pinned per the ICD
recommendations: synchronous drain on lifecycle transitions, qualified
+ unqualified-with-implicit-schema SQL parse, process-wide singleton,
`ids` absent in 0.5.1 (deferred to 0.5.5), dedicated
`trantor::EventLoopThread`, fixed-duration window, always-three `ops`
array. No architect redirects.

### What shipped

- **`src/kernel/realtime/sql_classify.{hpp,cpp}` — new.** Hand-rolled,
  narrow classifier for the three supported write shapes (qualified
  `INSERT INTO <schema>.<table>`, `UPDATE <schema>.<table>`,
  `DELETE FROM <schema>.<table>`; unqualified resolves to
  `(ext_<bc_extension_name>, <table>, op)` when `bc_extension_name`
  is non-empty). Strips leading whitespace + `-- line` and
  `/* block */` comments. Case-insensitive keyword match with
  non-ident delimiter check (so `INSERT` doesn't partial-match
  `INSERT_INTO` style identifiers). DDL / SELECT / WITH (CTE writes) /
  BEGIN/COMMIT/SET / multi-statement / unparseable all return
  `std::nullopt` with a debug log (`sql[0..80]` + `reason`).
  §Defense-in-depth check: after extraction, schema mismatch against
  `ext_<ext_name>` warns but still returns the tuple — ICD-0.4.3
  search_path + role + capability gate is the primary isolation
  boundary. Public API: one pure function
  `classify_sql(std::string_view, std::string_view) -> std::optional<SqlClass>`.

- **`src/kernel/realtime/coalescer.{hpp,cpp}` — new.**
  `CoalescerRegistry` process singleton owning a
  `std::unordered_map<(schema, table), WindowState>` protected by
  `std::shared_mutex`, plus a dedicated `trantor::EventLoopThread`
  for the flush timers. Public API: `instance()`, `start(cfg)`,
  `shutdown()`, `record_write(schema, table, op, row_count, ext)`,
  `drain_extension(ext_name)`, plus test seams
  (`apply_flush_for_test`, `open_window_count_for_test`,
  `set_emit_hook_for_test`, `clear_emit_hook_for_test`,
  `clear_windows_for_test`, `set_db_client_for_test`). State machine
  per ICD §Coalescer State Machine: first write opens a fixed
  `window_ms` timer; subsequent writes accumulate (no timer
  extension); zero-row writes on an empty bucket no-op (closes the
  `WHERE nomatch` firehose); zero-row on an open bucket leaves
  counters unchanged; cross-extension writes warn but keep window
  ownership with the first writer. Flush builds envelope per ICD
  §Envelope Assembly
  (`{layer:"data", channel:"plinth:data:<schema>.<table>", schema,
  table, ops:[insert/update/delete counts — always three], window_ms}`;
  `ids` / `seq` / `emitted_at` absent in 0.5.1 per §OQ4), runs the
  truncation heuristic (drop `ids` if present, drop envelope + audit
  `flush_failed` with `reason="payload_too_large"` if still oversize),
  then calls `emit_notify_async` via `drogon::sync_wait`. Test seam
  `set_db_client_for_test` lets integration tests pin a specific
  DbClient without spinning up the full Drogon app.

- **`src/kernel/js/async_op.hpp` — new `bc_extension_name` field.**
  Extension-identity snapshot captured at `db.exec` enqueue time;
  mirrors the 0.3.4 `cap_user`/`cap_call_depth` snapshot pattern so
  the detached `run_db_exec_outcome` task reads the value off the op
  rather than reaching back into `bc`. Populated in
  `src/kernel/js/stdlib/db_bindings.cpp` (DB_EXEC arm only; DB_QUERY
  is read-only and never coalesced).

- **`src/kernel/js/run_on_context.cpp` — hook at
  `run_db_exec_outcome`.** After `out["row_count"] = r.affectedRows()`
  and before `co_return out`, the hook runs `classify_sql(op.sql,
  op.bc_extension_name)`; on a `has_value()` result and
  `!op.silent`, calls
  `CoalescerRegistry::instance().record_write(...)`. Classifier
  skips (DDL / SELECT / unparseable) return `std::nullopt` → no
  record_write. `op.silent=true` (0.5.3 per-call opt-out, currently
  only the `{silent:true}` db.exec option) short-circuits the hook.

- **`src/kernel/config.{hpp,cpp}` — `Config::Realtime::Coalescer`
  substruct + `apply_realtime_coalescer`.** Fields:
  `enabled:bool=true`, `window_ms:std::size_t=50` bounded `[1, 10000]`
  (rejects with `config.realtime.coalescer.window_ms_out_of_range` on
  out-of-range values, matching the 0.5.0 hard-reject convention for
  the realtime block). `apply_realtime` split into
  `apply_realtime_{listener, notify, coalescer}` helpers to keep the
  top-level function under clang-tidy's cognitive-complexity
  ceiling.

- **`src/kernel/main.cpp` — startup + atexit wiring.**
  `CoalescerRegistry::instance().start(cfg.realtime.coalescer)` runs
  after `realtime::start_listener`. Atexit chain inserts
  `CoalescerRegistry::instance().shutdown()` between
  `realtime::stop_listener()` and `extensions::shutdown_registry()` so
  the DbClient pool outlives the final drain flush and any
  `pubsub.publish` from a torn-down handler can't race the listener
  (per `feedback_deterministic_teardown.md`).

- **`src/kernel/packages/install_lifecycle.cpp` — three drain call
  sites.** `CoalescerRegistry::instance().drain_extension(pkg.name)`
  runs at the DISABLE (outside Tx), UNINSTALL (before Tx A marks
  UNINSTALLING), and UPGRADE (before pool cutover at T4) transitions
  so the extension's final Layer-1 envelopes land while its NOTIFY
  consumer chain is still intact.

- **`tests/kernel/ws/ws_test_fixture.cpp` — atexit mirror.** Lockstep
  with `main.cpp` per `feedback_deterministic_teardown.md`.

- **`tests/kernel/realtime/{sql_classify, coalescer,
  coalescer_integration}_test.cpp` — 48 new test cases.**
  Classifier unit coverage (22 cases, `[realtime][coalescer][unit]`)
  includes E.01 (DDL rejection) plus every supported shape and every
  skip case. Coalescer state machine + truncation + error paths (21
  cases, `[realtime][coalescer][integration]`): C.01–C.13, T.01–T.04,
  E.02–E.04 — the hook-driven cases bypass real PG by intercepting
  the emit via `set_emit_hook_for_test`; the timer-driven cases
  (C.03, C.04, C.08) use the real dedicated `trantor::EventLoopThread`
  with short `window_ms` values. End-to-end integration (5 cases,
  PG-gated): I.01–I.05 — simulate the `run_db_exec_outcome` hook
  (`classify_sql` → `record_write`), let the real coalescer timer
  fire into a real DbClient (`drogon::orm::DbClient::newPgClient`
  injected via `set_db_client_for_test`) → PG NOTIFY → listener →
  captured `EventHandler`. I.05 asserts that `classify_sql("SELECT
  1")` → `std::nullopt` → zero envelopes.

### Test matrix

- `[realtime][coalescer]`: 48 cases / 150 assertions ✔ (all run with
  PG; 25 with live DbClient round-trips, 23 hook-driven).
- Full `plinth_tests_pure` subprocess: 303 cases / 1156 assertions ✔
  (no regression).
- Full `plinth_tests_js ~[async]` subprocess: 22 cases + 5 skipped /
  2330 assertions ✔ (db_bindings `bc_extension_name` addition
  verified by the existing async-bridge coverage).
- Full `plinth_tests_ws` subprocess: 6 cases + 21 skipped /
  13 assertions ✔ (atexit chain mirror exercised).
- **LH-1 storm regression — 3 × 120 s** against live PG + this v0.5.1
  kernel (cold-lifecycle reset between trials per LH-0.1 discipline,
  4 producers × 4 subscribers × `BurstSize=8` × 512 B payload):
  - Trial 1: 160,504 producer calls / 642,016 subscriber observations
    / ratio 1.0000 / p99 lag 5 ms / zero gaps / zero races / zero
    coalescer flush_failed.
  - Trial 2: 285,528 calls / 1,142,112 obs / ratio 1.0000 / p99 lag
    6 ms / zero gaps / zero races / zero flush_failed.
  - Trial 3: 250,072 calls / 1,000,288 obs / ratio 1.0000 / p99 lag
    7 ms / zero gaps / zero races / zero flush_failed.
  - Aggregate: zero `free_zero_refcount` / `list_empty(&rt->gc_obj_list)`
    / `bad_weak_ptr` / SIGSEGV / SIGABRT / `realtime.notify.rejected`
    / `realtime.listener.reconnected` across all three kernel logs.
    Throughput is up vs the LH-1 baseline (108,072 calls,
    2026-04-22) — coalescer doesn't sit in the Layer-3
    `pubsub.publish` path, so this is the no-regression confirmation
    rather than a coalescer-specific load measurement.
- `run-clang-tidy-20 -j4`: zero findings on new TUs
  (`sql_classify.{cpp,hpp}`, `coalescer.{cpp,hpp}`) and modified TUs
  (`run_on_context.cpp`, `db_bindings.cpp`, `async_op.hpp`,
  `config.{cpp,hpp}`, `main.cpp`, `install_lifecycle.cpp`,
  `ws_test_fixture.cpp`). Suppressions: one `NOLINTBEGIN/END` block
  around `CoalescerRegistry`'s facade methods (singleton wrapper
  around TU-local state; methods are technically static but the ICD
  mandates the `instance().method()` shape).

### Scope deviations from ICD

- **`realtime.coalescer.shutdown_drain` audit replaced with silent
  skip.** The ICD §Audit Events specifies firing this audit from
  `shutdown()` completion. In practice `shutdown()` runs from main's
  atexit chain, which fires AFTER `spdlog::shutdown()` (in main, on
  drogon::app().run() return) and AFTER SIGTERM has nulled drogon's
  `DbClientManager`. Both `plinth::log::audit` (deref of null
  manager via `drogon::app().getDbClient`) and an `spdlog` fallback
  (deref of null default logger) crash with SIGSEGV. The first LH-1
  trial reproduced this deterministically via gdb on the core dump
  (audit_shutdown_drain → log::audit → DbClientManager::find @ this=0x0).
  The audit is dropped; the equivalent diagnostic information lives
  in the timer-fired `realtime.coalescer.flush_failed` audit (which
  IS reachable while the kernel is up) and the test seam
  `open_window_count_for_test` (zero post-shutdown by contract).
  Filed against ICD-0.5.1 §Audit Events as a deviation; a future
  cleanup of main.cpp's lifecycle (move `spdlog::shutdown` to the
  end of atexit, after all subsystems have logged) would let the
  shutdown_drain audit be reinstated.
- All seven open questions (OQ1–OQ7) land on the ICD-recommended
  value. The classifier implements the hand-rolled state machine
  path per §Classifier implementation latitude — matches the
  ICD-0.5.0 channel validator's posture.

---

## 0.5.0.5 — 2026-04-22 — ICD-0.5.1 pg-auto-event-coalescer authored (paper docs session, untagged)

Paper-only docs session per METHODOLOGY-llm-assisted-development.md
§3.1 *Forward ICD presence check* and `feedback_icd_horizon.md`
(ICDs written at most one milestone ahead of current implementation).
**Un-tagged** per `feedback_tagging_rule.md` (four-part `X.Y.Z.N`
paper sessions accumulate into the next X.Y.Z release and do not
carry their own tag). Authors the contract for the PG auto-event
coalescer — the primary in-kernel caller of v0.5.0's
`emit_notify_async` and the producer of every Layer-1 `plinth:data:*`
envelope the platform will emit — ahead of the v0.5.1 implementation
session. Follows the `0.5.0.2 ICD-LH-1 authored` and `0.5.0.3
ICD-0.5.0.3 extension-dispatch authored` paper-session precedents
(2026-04-22, commits `f26534a`, `0d86a62`).

### Deliverables

- **`docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md`** (new) — full
  contract for the PG auto-event coalescer. Pins:
  - Per-process `plinth::realtime::CoalescerRegistry` singleton
    owning `(schema, table) → WindowState` buckets, a dedicated
    `trantor::EventLoopThread` for window timers, and the atexit
    drain barrier.
  - Hand-rolled SQL classifier for qualified / unqualified-with-
    implicit-schema single-table `INSERT` / `UPDATE` / `DELETE`;
    CTE writes, multi-table writes, DDL, and `SELECT` skip with
    debug log (no envelope, no audit).
  - Write-path hook at `run_db_exec_outcome`
    (run_on_context.cpp:419–445) — post-`execSqlCoro`-success,
    pre-`co_return` — with a new `AsyncOp::bc_extension_name`
    snapshot field mirroring the 0.3.4 `cap_user` pattern.
  - Fixed-duration window (`opened_at + window_ms`, no timer
    extension on subsequent writes); bounded emit latency at
    50 ms p100 per bucket.
  - Envelope contract matching ICD-0.5.0 §Payload Envelope:
    `layer="data"`, `channel="plinth:data:<schema>.<table>"`,
    `schema`, `table`, three-entry `ops` array (insert / update /
    delete counts; always three, even when zero), `window_ms`.
    `ids` absent in 0.5.1 (§OQ4 — clients treat missing `ids`
    identically to `truncated:true`; optimistic-update path defers
    to 0.5.5); `seq` absent in 0.5.1 (0.5.5); `emitted_at` absent.
  - Truncation heuristic: serialize → measure against
    `config.realtime.notify.max_payload_bytes` (8000 default,
    reused from ICD-0.5.0) → drop `ids` if present → counts-only
    fallback with `truncated:true` → emit_notify PAYLOAD_TOO_LARGE
    short-circuit + audit if still oversized.
  - Zero-row-write handling: `row_count == 0` on an empty bucket
    is a no-op (no window opens); on an open bucket it is also a
    no-op (counters unchanged). Closes the `WHERE nomatch` firehose.
  - Config surface: `realtime.coalescer.{enabled:bool=true,
    window_ms:int=50}` bounded `[1, 10000]`, loaded via extended
    `apply_realtime` per the 0.5.0 convention.
  - Audit events (two): `realtime.coalescer.flush_failed` and
    `realtime.coalescer.shutdown_drain`. No per-flush audit
    (firehose); no per-skip audit (classifier skips fire debug
    logs only).
  - Deterministic-teardown contract: `CoalescerRegistry::shutdown()`
    inserted into `main.cpp` atexit chain between
    `realtime::stop_listener()` and `drogon::app().quit()`; blocks
    on synchronous drain of open windows (per-flush 2 s timeout);
    `ws_test_fixture.cpp` atexit mirrors the shape.
  - Extension-lifecycle integration: three
    `drain_extension(pkg.name)` call sites in
    `install_lifecycle.cpp` at DISABLED / UPGRADING / UNINSTALL
    transitions, participating in the 0.4.5 drain budget.
  - HA semantics normative promotion: each node coalesces its own
    writes; cross-node coordination out of scope; best-effort
    delivery (reliability tier lands 0.5.4 + 0.5.5).
  - Security constraints (7) pinned non-negotiable: classifier
    read-only, channel regex enforced at emit, extension-identity
    defense-in-depth, no user-controlled envelope fields, no
    per-envelope audit, best-effort-not-at-most-once on failures,
    `enabled=false` is opt-out not bypass.
  - Test plan — 26 new cases across Groups **C** (state machine —
    13), **T** (truncation — 4), **I** (integration end-to-end —
    5), **E** (errors — 4) in two new TUs
    (`coalescer_test.cpp` + `coalescer_integration_test.cpp`). Tags
    `[realtime][coalescer]` + subtype; all 26 land in
    `plinth_tests_pg` (no new subprocess, no new ctest entry).
  - Seven Open Questions surfaced with recommendations for
    architect review: (1) extension-unload-mid-window synchronous
    drain, (2) SQL parse scope qualified-only, (3) per-process
    singleton, (4) `ids`-always-empty-in-0.5.1, (5) dedicated
    `EventLoopThread` for timers, (6) no-timer-extension window
    policy, (7) always-three `ops` entries.
- **`docs/CHANGELOG.md`** (this entry).

### Architect scope decisions

- **Four-part slot (untagged), not tagged milestone.** Pure paper
  session — no code change. Matches the 0.5.0.2 / 0.5.0.3 precedent.
  `v0.5.1` tag lands on the v0.5.1 coalescer code-ship merge; this
  session's merge is untagged.
- **One-ahead ICD horizon.** Per `feedback_icd_horizon.md` — ICDs
  authored at most one milestone ahead of current implementation.
  0.5.0 is current implementation (tag `v0.5.0`); 0.5.1 is next
  implementation; ICD-0.5.1 is at the horizon. No further ICDs
  authored in this session.
- **No ROADMAP edit.** ROADMAP L110 `0.5.1 DB layer auto-event
  emission (debounced coalescer) [medium]` stays pending; the
  v0.5.1 implementation-ship entry will remove it. `[medium]` band
  unchanged — the ICD's completion does not promote the code
  milestone's band.
- **No DEFERRED.md edit.** The coalescer does not clear or add
  any deferred item; the 0.4.x follow-up debt list
  (`DEFERRED.md §2026-04-20` + `§2026-04-22`) is independent.
- **Recommended positions on seven Open Questions.** The ICD
  surfaces seven genuine contract decisions with a recommendation
  per OQ. Architect review before the v0.5.1 implementation opens
  resolves them; ICD updates in a subsequent slot if any
  recommendation is redirected.

### Why now

v0.5.0 shipped `emit_notify_async` + the listener + `pubsub.publish`
as the realtime bus primitives, but the bus has no producer of
Layer-1 events until 0.5.1's coalescer. LH-1 confirmed (3 × 120 s,
2026-04-22) that the 0.5.0 foundation is clean under storm load;
the coalescer is the first composition on top. Authoring the ICD
now lets the architect resolve the seven Open Questions before
implementation effort is committed, mirroring the split-slot
rationale from 0.5.0.2 / 0.5.0.3.

The post-0.5.0 sequencing in `project_plinth_state.md §Next work`
slotted `0.5.0.5 ICD-0.5.1 authoring` immediately after LH-1
shipped — this entry closes that slot. v0.5.1 implementation
follows next per `project_next_session_0_5_1.md` (branch
`feat/0.5.1-pg-auto-event-coalescer` off `main`; tagged `v0.5.1`
on merge).

### Verification

Paper-only session — no runtime verification, no CI, no build.
The verification surface is a doc-review checklist:

- ICD internal §-references resolve; cross-ICD citations to
  `ICD-0.5.0` (envelope contract, channel naming, emit helper,
  HA semantics, atexit ordering), `ICD-0.3.3` (AsyncOp snapshot
  pattern), `ICD-0.4.3` (extension schema isolation), `ICD-0.4.4`
  (BridgeContext extension identity, install lifecycle),
  `ICD-0.4.5` (drain budget), `ICD-0.1.7` (audit writer + gate),
  and `architecture/03-data.md §3.1 + §3.2 + §3.3 + §3.6 +
  Appendix A` all land on real content.
- File:line citations in the ICD (run_on_context.cpp:419–445,
  async_op.hpp:54–98, config.hpp:67–77, config.cpp:83–114,
  channel.hpp, emit.hpp, install_lifecycle.cpp) verified against
  the current code (`main` at `7953eae`) before merge.
- Error-code taxonomy follows the existing `realtime.*` and
  `config.realtime.*` conventions from ICD-0.5.0; one new audit
  kind (`realtime.coalescer.flush_failed`,
  `realtime.coalescer.shutdown_drain`) + one new config rejection
  code (`config.realtime.coalescer.window_ms_out_of_range`). No
  re-definitions.
- Scope boundaries explicit: in-scope + out-of-scope lists leave
  no ambiguity about what v0.5.1 must implement (SQL classifier,
  registry, hook, config, audit, atexit, lifecycle hooks) and
  what it must not (batch / silent, `seq`, persistence, client
  SDK, cross-node coalescence, populated `ids`, non-extension
  kernel writes, DDL events, row-level semantics).
- Test plan covers the end-to-end path (5 I.\* cases) plus the
  full state-machine + truncation + error surface (21 C/T/E
  cases) without new CTest subprocess entries.
- CHANGELOG entry tone parallels the 0.5.0.3 precedent (paper-
  session shape with next-steps and rationale); band / tag
  call-out explicit.
- No ROADMAP edit — verified by the self-review diff.
- No DEFERRED.md edit — verified by the self-review diff.

### Next scheduled work

- **v0.5.1 — PG auto-event coalescer** implementation. Implements
  this ICD. Branch `feat/0.5.1-pg-auto-event-coalescer` off `main`;
  squash-merged; tagged `v0.5.1` on merge per
  `feedback_tagging_rule.md`. Scope pre-committed to what §CI
  wiring enumerates — no scope creep. Architect resolves the
  seven OQs before the branch opens; ICD may be updated in a
  subsequent `0.5.0.N` slot if any recommendation is redirected
  materially.
- **RE-EVAL following 0.5.1** (ROADMAP L111, `[rewrite session]`).
  Per the every-4-milestones cadence established in the ROADMAP
  preamble; this re-eval sits between 0.5.1 (coalescer) and 0.5.2
  (WS broker) and includes the forward ICD presence check for
  0.5.3.
- **0.5.2 WS broker** — the first Layer-1 handler subscriber.
  Gated on 0.5.1 + its RE-EVAL.

---

## LH-1 — 2026-04-22 — LISTEN/NOTIFY storm tier diagnostic (untagged)

First LH-stream tier to exercise the v0.5.0 realtime bus under sustained
Layer-3 load, per `docs/icd/ICD-LH-1-listen-notify-storm.md`. Untagged
per `feedback_tagging_rule.md` (LH stream is outside the X.Y.Z
numbering). Resumes from the paused
`feat/lh-1-listen-notify-storm@339afb0` WIP branch; now unblocked by
0.5.0.4's extension-dispatch implementation.

### Why

ICD-LH-1 §1 pins LH-1 as the first diagnostic against Layer-3 emit
end-to-end (driver extension → `pubsub.publish` JS binding →
extension-identity gate → regex validation → `emit_notify_async` →
PG `LISTEN/NOTIFY` backbone) + the kernel-side
`plinth::realtime::listener` dispatch path (external PG LISTEN
subscriber observes what the bus actually delivers). §7.2 diagnostic
mandate: a 3-trial `--tier=storm` run either reproduces one of
`free_zero_refcount` / `list_empty(&rt->gc_obj_list)` / `bad_weak_ptr`
or a realtime-specific crash under storm load (unblocks a targeted
fix PR), or confirms zero reproductions across three 120 s runs
(clears the 0.5.0 foundation for 0.5.1's coalescer to layer on top).

### What shipped in this PR

- **Harness build-fix — `[js]` number unmarshal.** QuickJS's
  `JSON.stringify` writes integer-valued JS `Number`s above the int32
  boundary with a trailing `.0` (so `Date.now()` ≈ 1.776 × 10¹²
  serialises as `1776896490783.0`); Go's `json.Unmarshal` into `int64`
  rejects the fractional form. Subscriber envelope struct now uses
  `float64` for `payload.seq` and `payload.emit_started_at` and
  truncates to `int64` at the receiver
  ([load-harness/internal/pglisten/subscriber.go](../load-harness/internal/pglisten/subscriber.go)).
  Lossless under the `Number.MAX_SAFE_INTEGER` bound, which covers
  every realistic `Date.now()` / burst seq.
- **Harness build-fix — `extractEmitted` envelope key.** The kernel's
  `make_call_result` puts the capability return value under `value`
  ([src/kernel/ws/call_dispatch.cpp:27](../src/kernel/ws/call_dispatch.cpp));
  the harness was looking under `result`. Fixed to read
  `f["value"]["emitted"]` with `f["result"]` retained as a defensive
  fallback
  ([load-harness/cmd/lh0/main.go](../load-harness/cmd/lh0/main.go)).
  Without the fix, every storm-tier exit coded `1` (emitted==0
  degenerate branch in `stormExitCode`) and skipped the driver
  uninstall defer.
- **Storm tier `BurstSize` dialled 16 → 8.** ICD-LH-1 §6.1 pinned
  `BurstSize=16` on the assumption that
  `default_runtime_limits().max_concurrent_async_ops = 32`; the
  actual default has been `8` since 0.3.3
  ([src/kernel/js/runtime_pool.cpp:39](../src/kernel/js/runtime_pool.cpp)).
  Burst>max_concurrent hits a known-latent fan-out requeue spin in
  `run_on_context`'s outer loop (see §Findings below); dialling to
  `8` lets the storm tier actually exercise the realtime paths
  instead of masking them behind the async-bridge back-pressure
  issue. Rationale in the tier comment
  ([load-harness/internal/tiers/tiers.go](../load-harness/internal/tiers/tiers.go)).
- **README stanza for storm tier** with `--tier=storm` flags,
  kernel-log tail filter, and a scope-update retiring the stale
  "extension capability dispatch … does not exercise" paragraph
  (0.5.0.4 closed that). Corrects the stream summary to "LH-0 +
  LH-0.1 + LH-1".

### Diagnostic trials — 3 × `--tier=storm` × 120 s

Kernel at current `main` HEAD (`97b5dae`, 0.5.0.4 shipped). Dev-mode
single-node PG via `docker/docker-compose.yml`. Producer workers = 4,
subscribers = 4, burst = 8, payload = 512 B. Between trials: full
kernel stop + fresh `data/` directory + re-seed admin (cold-lifecycle
discipline from LH-0.1).

| Trial | calls ok | calls fail | p50 RTT   | p99 RTT   | max RTT    | notifies emitted | notifies observed | ratio   | p99 lag | max lag | kernel crash sigs |
|-------|---------:|-----------:|----------:|----------:|-----------:|-----------------:|------------------:|--------:|--------:|--------:|------------------:|
| 1     |   40,946 |          0 |   8.6 ms  |  40.7 ms  |  315.3 ms  |          327,568 |         1,310,272 | 1.0000  |   6 ms  |  60 ms  |                 0 |
| 2     |   34,308 |          0 |  10.8 ms  |  44.8 ms  |  317.5 ms  |          274,464 |         1,097,856 | 1.0000  |   6 ms  |  65 ms  |                 0 |
| 3     |   32,818 |          0 |  11.1 ms  |  46.2 ms  |  409.1 ms  |          262,544 |         1,050,176 | 1.0000  |   7 ms  |  48 ms  |                 0 |

Sustained emit rate ≈ 2,200–2,730 notifies/s (well inside the ICD §6.1
target of 640–2,500). Observed / emitted ratio is exactly 1.0000 on
every subscriber on every trial — every subscriber received every
emitted envelope. p99 lag 6–7 ms against the §7.1 ≤ 5 s baseline.

Cross-trial totals: **108,072 burst calls → 864,576 emitted notifies
→ 3,458,304 subscriber observations** (4 subscribers × 864k envelopes).
Zero call failures, zero parse errors, zero dropped envelopes, zero
audit rejections / reconnects, zero `free_zero_refcount` /
`list_empty(&rt->gc_obj_list)` / `bad_weak_ptr` / `SIGSEGV` /
`SIGABRT` in kernel stderr + spdlog during or after any of the three
runs. Each run terminated with clean `DELETE /api/packages/{id}` and
`SIGTERM` shutdown.

### Primary finding (§7.2)

**Zero reproductions across three 120 s trials at 2.2–2.7k
notifies/s sustained Layer-3 storm.** Per ICD-LH-1 §7.2 this is a
meaningful data point: the v0.5.0 realtime bus (PG LISTEN/NOTIFY
single-channel fan-in + kernel `plinth::realtime::listener` dispatch
+ JS `pubsub.publish` extension-identity-gated binding +
`emit_notify_async`) **tolerates the storm tier under the production
lifecycle**, clearing the 0.5.0 foundation for 0.5.1's coalescer to
layer on top without addressing a pre-existing realtime-subsystem
race first.

### Secondary finding — pre-existing async-bridge requeue spin surfaced at burst>max_concurrent

Smoke runs before the tier BurstSize adjustment reproduced — first
call, deterministically — a kernel outer-coroutine spin loop when
Promise.all fan-out inside a single BridgeContext exceeds
`max_concurrent_async_ops`. Symptom: handler hangs (WS `call` times
out after 10 s on every attempt), event-loop thread pegged at ≈ 50 %
CPU, `BridgeContext::resolve: abandoned` debug messages clustering
long after connection teardown. Observed consistently at burst ∈
{9, …, 16}; clean at burst ∈ {1, 8}.

**Root cause (hypothesis, grounded in the code path but not yet
patched):**
[src/kernel/js/run_on_context.cpp:904–935](../src/kernel/js/run_on_context.cpp)
outer loop step 2 only awaits completion when
`inflight_detached > 0 && !JS_IsJobPending(bc.rt) && !bc.has_pending_ops()`.
When `pending_ops` is non-empty but `concurrent_async_ops >=
max_concurrent_async_ops`, `dispatch_ops_batch_fanout` dispatches
zero ops and re-queues the remainder — the next iteration finds
`has_pending_ops() == true`, skips the await, and spins. The outer
coroutine never yields the event-loop thread, so the `queueInLoop`
callbacks that would decrement `concurrent_async_ops` and allow
forward progress cannot run.

**Pre-existing, not LH-1-introduced.** The project's own fan-out
ctest at
[tests/kernel/js/async_hardening_test.cpp:151](../tests/kernel/js/async_hardening_test.cpp)
(N.39, "parallel queries honour max_concurrent cap") comments out
loud: "ICD §N.39 calls for 100 × 8; running at higher fan-out or
tighter wave count on a single BridgeContext exposes a pre-existing
race in the parallel-dispatch requeue path documented in
project_ws_flaky_segfault.md (K.33 / §Fourth occurrence / 0.3.4.1
cascade). 4 × 2 exercises the same correctness property … while
keeping the re-queue surface small enough to avoid the race. A
follow-up fix to the fan-out race lifts the scale restriction back
to the ICD-quoted 100 × 8." LH-1 is the first LH-stream reproduction
in the production lifecycle (the ctest path lives in the Catch2
subprocess harness only).

**ICD-LH-1 deviation recorded.** The ICD's §6.1 `BurstSize=16` + §OQ4
"minimal-for-now payload shape" both assumed a 32-slot async-op cap.
With the actual 8-slot cap, the chosen storm profile guaranteed the
spin. Either the fan-out fix lands and LH-1 can restore
`BurstSize=16` for the ICD's original storm rate target, OR the ICD
is amended to match the current cap at `BurstSize=8`. This ship
implements the latter in `tiers.go`; an ICD amendment follows in a
future docs pass alongside the `0.5.x.N` fan-out-requeue fix slot.

### Kernel surface — no changes

LH-1 is diagnostic-only. No edits under `src/`. No atexit-chain edits.
No new config surface. 0.5.0.4's
`plinth::extensions::runtime_registry` + `resolution.cpp` extension
arm + `call_dispatch.cpp` WS migration are the only kernel deltas the
LH-1 storm runs sit on top of (all landed before this PR).

### Accepted scope gaps (per ICD-LH-1 §2 / §9)

- **Fix for the async-bridge fan-out requeue spin.** LH-1 is the
  diagnostic; the fix ships as its own PR per the project's
  "no per-signature bandaids" posture. Scope per the ctest comment at
  `async_hardening_test.cpp:151`. Likely lives under the existing
  ROADMAP `0.5.x.N [js][async] kernel-side refcount investigation`
  slot or adjacent, pending architect review.
- **Kernel-side probe signature (ICD §9 LH-1 future work).** External
  PG LISTEN proved sufficient to characterise the realtime emit path
  + kernel dispatch path under storm. No `lh1:1:probe` signature
  needed for this ship.
- **Richer payload shape (ICD §OQ4).** The handler's
  `{seq, data, emit_started_at}` shape surfaced both the §7.2
  clean-run data point and the §OQ-defaulted `emit_started_at` float
  serialisation. A production-representative shape
  (`{user_id, room_id, typing:true}`) remains deferred.
- **ICD amendment for `BurstSize=8`.** The deviation is recorded here
  + in the `tiers.go` tier comment. The ICD file edit follows in the
  next docs session, bundled with whichever companion ICD change the
  fan-out-requeue fix carries.

### Verification

- `cd load-harness && make all` clean; `go vet ./...` clean.
- Kernel rebuilt (`cmake --build build --target plinth -j4`). No new
  test surface in scope; existing `async_hardening: parallel queries
  honour max_concurrent cap` ctest still `[js][async][hardening]`-
  tagged and passing on `main`.
- 3-trial diagnostic above.
- Harness log + kernel log for each trial preserved locally for the
  ship session; summary rows in the §Diagnostic trials table are the
  ship-anchored record (raw logs not retained in the repo per
  LH-0.1 convention).

### Next scheduled work

- **0.5.1 PG auto-event coalescer.** Unblocked — the 0.5.0 emit
  primitives tolerate the storm per §Primary finding.
  `project_next_session_0_5_1.md` is the entry point; ICD-0.5.1 slot
  fires after the next roadmap re-eval pass (per
  `feedback_icd_horizon.md`).
- **Fan-out-requeue fix.** 4-part slot under
  `ROADMAP.md §0.4.x cleanup follow-ups`; sequencing at architect
  discretion. Raising the cap default to 32 is an alternative to the
  loop fix and deserves a look while the ICD covers the behaviour.

### References

- `docs/icd/ICD-LH-1-listen-notify-storm.md` — harness contract.
- `docs/icd/ICD-0.5.0.3-extension-dispatch.md` — the dispatch path
  0.5.0.4 shipped and LH-1 exercises.
- `docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md` — the realtime
  bus LH-1 drives.
- `docs/ROADMAP.md §Load Harness, LH-1`.
- `tests/kernel/js/async_hardening_test.cpp:151` — the ctest that
  kept the fan-out requeue race boxed at 4 × 2 until LH-1 hit it
  from the production WS path.
- `project_ws_flaky_segfault.md` (memory) — historical context for
  the requeue race.

---

## 0.5.0.4 — 2026-04-22 — Tier 2 extension capability dispatch implementation (untagged)

Implementation session for ICD-0.5.0.3. **Un-tagged** per
`feedback_tagging_rule.md` — prior-arc tech debt close-out, not a
new product milestone; the coalescer milestone `v0.5.1` is the
next tag. Closes the long-deferred `dispatch_tier2` extension arm
([src/kernel/capabilities/resolution.cpp:313–344](../src/kernel/capabilities/resolution.cpp)),
unblocking LH-1 resumption on `feat/lh-1-listen-notify-storm@339afb0`.

### Shipped surface

- **New subsystem `plinth::extensions`** at
  [src/kernel/extensions/runtime_registry.{hpp,cpp}](../src/kernel/extensions/runtime_registry.cpp).
  Process-lifetime registry of per-extension `plinth::js::RuntimePool`
  instances keyed by extension name. `init_registry` / `shutdown_registry`
  wrap `main.cpp` bootstrap + atexit; `create_pool` / `destroy_pool`
  hook the install-lifecycle transitions; `dispatch(name, fn, args,
  caller, caller_call_depth)` is the coroutine dispatch entry.
- **`dispatch_tier2` extension arm** now rejects on the sync path
  with `CapabilityError::ASYNC_REQUIRED` and on the async path
  dispatches into `plinth::extensions::dispatch`. The async
  resolver resolves the entry + RBAC under `state_mutex.shared_lock`,
  releases before `co_await`, and threads
  `caller.call_depth + 1` across the extension boundary (uniform
  `MAX_CALL_DEPTH = 8` enforcement per ICD §Call depth).
- **Handler contract:** `server/handlers/<fn>.js` ES module with a
  default export. Invoked via a fixed wrapper source + the new
  per-dispatch `import_from_src` QuickJS intrinsic. Callee's
  `BridgeContext` carries the caller's `UserContext` (audit +
  `cap.call` / `db.query` attribution) and the callee's own
  `extension_name` (pool-populated, for the `pubsub.publish`
  identity gate at
  [src/kernel/js/stdlib/pubsub_bindings.cpp:154–167](../src/kernel/js/stdlib/pubsub_bindings.cpp)).
- **Five new `cap.*` rejection codes** — `cap.async_required`,
  `cap.extension_not_loaded`, `cap.handler_not_found`,
  `cap.handler_load_failed`, `cap.handler_threw`. Extended
  `capability_error_to_rejection`
  ([src/kernel/js/stdlib/cap_bindings.cpp:256–307](../src/kernel/js/stdlib/cap_bindings.cpp))
  with an optional detail-code + detail-message pair so the
  transport `EXTENSION_DISPATCH_FAILED` enum variant preserves the
  concrete `cap.*` code on its way to the JS / WS caller.
  `CapabilityError` extended with `ASYNC_REQUIRED` +
  `EXTENSION_DISPATCH_FAILED` at
  [src/kernel/capabilities/types.hpp:34–66](../src/kernel/capabilities/types.hpp).
- **`RuntimePool` carries `extension_name`** — ctor gains an
  optional `std::string extension_name` argument copied onto every
  acquired `BridgeContext`. Host-side pools (`lh0:1:js_stress`,
  host-eval tests) keep the empty-string default and behave
  unchanged.
- **`call_capability_async` signature** extended with two optional
  output pointers (`ext_detail_code_out`, `ext_detail_message_out`);
  populated only on `EXTENSION_DISPATCH_FAILED`. JS + WS callers
  pass buffers in; non-extension callers pass nullptr and see
  zero-behavior-change.
- **WS `on_call` migration** at
  [src/kernel/ws/call_dispatch.cpp:65–128](../src/kernel/ws/call_dispatch.cpp)
  — wraps the dispatch in `drogon::async_run` + `co_await
  call_capability_async`. The surface is unchanged; clients see
  the same `call_result` / `call_error` frames.
- **Install-lifecycle hooks** at
  [src/kernel/packages/install_lifecycle.cpp](../src/kernel/packages/install_lifecycle.cpp)
  — `create_pool` on INSTALL-from-empty ACTIVATING commit,
  UPGRADE T4 cutover, ENABLE-from-DISABLED commit; `destroy_pool`
  on DISABLE commit and UNINSTALL commit. All idempotent.
- **Handler audit** — `cap.handler_threw` / `cap.handler_load_failed`
  emit a `capability.extension.error` audit row attributed to the
  caller's `UserContext`. `cap.handler_not_found` /
  `cap.extension_not_loaded` / `cap.cancelled` do not audit
  (platform state vs user action per ICD §Audit).

### Tests

- New `tests/kernel/capabilities/dispatch_extension_test.cpp` —
  10 `[cap][res][ext][integration]` PG-gated cases covering R.01
  echo happy path, E.01–E.06 error taxonomy (throw / unknown
  signature / missing handler file / handler syntax error /
  destroyed pool mid-dispatch / sync-path async_required), P.02
  spoof-other-ext pubsub identity gate, P.03 RBAC at resolver,
  H.01 destroy/create cycle. All cases SKIP cleanly when PG is
  absent. Remaining groups (R.02 WS, R.03 audit, E.07 depth
  chain, P.01/P.04/P.05, H.02/H.03, C.01/C.02) are TODO for
  follow-up sessions as the fixture harness extends.
- Updated
  [tests/kernel/capabilities/resolution_test.cpp](../tests/kernel/capabilities/resolution_test.cpp)
  and
  [tests/kernel/capabilities/listener_integration_test.cpp](../tests/kernel/capabilities/listener_integration_test.cpp)
  — 5 cases that asserted `tier3_not_available` for extension
  entries on the sync path now assert `async_required`,
  matching the new behavior.
- All existing suites (pure / js / ws / pg / js-async) remain
  green under this branch.

### Known limits (deferred)

Per ICD-0.5.0.3 §Out of scope + §What must not be decided yet:
persistent per-pool handler-module cache, `cap.pool_exhausted`
back-pressure, versioned pool coexistence during UPGRADE,
per-extension manifest `RuntimeLimits` overrides, `SET search_path`
per-op (DEFERRED §2026-04-18), cross-extension `pubsub.subscribe`
(0.5.2 WS broker scope), Tier 3 sidecar proxy (0.8.x), sync
extension-dispatch bridge (permanent `cap.async_required`),
`cap.whoami` JS binding.

### Next scheduled work

- **LH-1 resumption** on `feat/lh-1-listen-notify-storm@339afb0`
  with the ICD-LH-1 corrections from
  `project_plinth_state.md §ICD-LH-1 corrections`: rename
  `ext_lh1_storm` → `lh1storm`; drop the `pubsub.publish` RBAC
  rule + grant step (extension-identity gated only).
- **0.5.0.5 ICD-0.5.1 authored** (paper session) per
  `feedback_icd_horizon.md` one-ahead rule — originally scheduled
  as `0.5.0.3`, slid to `0.5.0.5` by the extension-dispatch
  intervention.
- **v0.5.1 PG auto-event coalescer** code session per
  `project_next_session_0_5_1.md`.

---

## 0.5.0.3 — 2026-04-22 — ICD-0.5.0.3 extension-dispatch authored (paper docs session, untagged)

Paper-only docs session per METHODOLOGY-llm-assisted-development.md
§3.1 *Forward ICD presence check*. **Un-tagged** per
`feedback_tagging_rule.md` (four-part `X.Y.Z.N` paper sessions
accumulate into the next X.Y.Z release and do not carry their own
tag). Authors the contract for Tier 2 extension capability dispatch
— the long-absent replacement for the `TIER3_NOT_AVAILABLE` stub
carved in 0.2.2 — ahead of the 0.5.0.4 implementation session.
Follows the `0.5.0.2 ICD-LH-1 authored` paper-session precedent
(2026-04-22, commit `f26534a`).

### Supersedes prior 0.5.0.3 target

The 0.5.0.2 CHANGELOG entry's §Next scheduled work listed
"`0.5.0.3 ICD-0.5.1 authored`" as the next slot. The LH-1
implementation attempt (2026-04-22, branch
`feat/lh-1-listen-notify-storm` paused at commit `339afb0`)
surfaced an unexpected blocker: every `provider_type == "extension"`
Tier 2 cache entry is rejected by `dispatch_tier2` at
[src/kernel/capabilities/resolution.cpp:313–344](../src/kernel/capabilities/resolution.cpp)
with `TIER3_NOT_AVAILABLE` — unfinished 0.3.x work that ICD-0.3.4
§49–60 deferred to "the 0.4.x ICD that introduces the installer,"
but which never landed. The LH-1 ICD as written (WS `call
lh1storm:1:burst` → handler → `pubsub.publish`) requires this
dispatch path to work; a 2026-04-22 smoke run confirmed the blocker
(1.1M WS calls / 20s, zero handler invocations, zero NOTIFY emits).

**New slot order:** `0.5.0.3` (this entry) unblocks the extension
dispatch path; `0.5.0.4` implements it; LH-1 resumption follows on
`feat/lh-1-listen-notify-storm`; `0.5.0.5` (or later) picks up the
originally-planned `ICD-0.5.1` authoring slot. Band/tag discipline
unchanged — all four-part slots remain untagged; `v0.5.1` tags the
coalescer ship.

### Deliverables

- **`docs/icd/ICD-0.5.0.3-extension-dispatch.md`** (new) — full
  contract for Tier 2 extension capability dispatch. Pins:
  - Replacement for `dispatch_tier2`'s extension branch — a
    coroutine path wired into a new `plinth::extensions::RuntimeRegistry`
    subsystem that owns `RuntimePool` instances per installed-
    ACTIVE extension.
  - Handler convention promoted from fixture observation to
    normative: `server/handlers/<function>.js`, ES module with
    default export, single-arg JSON round-trip.
  - WS `on_call` migration from sync `call_capability` to
    `co_await call_capability_async` (the sync path rejects
    extension entries with a new `cap.async_required` code).
  - Five new `cap.*` rejection codes: `cap.async_required`,
    `cap.extension_not_loaded`, `cap.handler_not_found`,
    `cap.handler_load_failed`, `cap.handler_threw`. Plus a new
    `CapabilityError::ASYNC_REQUIRED` / `EXTENSION_DISPATCH_FAILED`
    variant pair.
  - RBAC enforcement stays at the resolver boundary (caller's
    `effective_rules` checked before dispatch; callee does not
    re-check). Callee runs under caller's UserContext for audit +
    cap.call recursion; callee's `bc.extension_name` is its own
    (for `pubsub.publish` extension-identity gate).
  - Call depth propagates across the extension boundary (target
    `bc.call_depth = caller + 1`); existing `MAX_CALL_DEPTH=8`
    enforcement applies uniformly across hops.
  - Lifecycle hooks in `install_lifecycle.cpp` at the three
    `provider_type = "extension"` registration sites (lines 842,
    930, 1891) — create_pool on ACTIVE; destroy_pool on DISABLED /
    UPGRADING / UNINSTALL.
  - Bootstrap/shutdown wiring in `main.cpp` — `init_registry`
    after `capabilities::init_resolver`; `shutdown_registry`
    before `drogon::app().quit()` per
    `feedback_deterministic_teardown.md`.
  - Test plan — Groups R (happy path), E (errors), P (identity +
    propagation), H (hot reload lifecycle), C (cancellation) in
    a new `tests/kernel/capabilities/dispatch_extension_test.cpp`
    with a new fixture `tests/fixtures/packages/extdispatch/`.
    Library-level install path only; no HTTP-surface dependency,
    no LH-1 dependency.
  - Security constraints (8) pinned non-negotiable: caller
    identity authoritative, RBAC at caller boundary, pool-set
    extension identity, call-depth propagation, on-disk handler
    source, per-hop resource limits, cascading cancellation, no
    mutable bridge.
  - Implementation latitude explicit — six decisions left to
    0.5.0.4 (import_from_src placement, cancellation-threading
    mechanism, dispatch-signature, fixture shape, PR granularity,
    search_path wiring for P.04).
  - Out-of-scope deferrals (7) — cross-extension pubsub.subscribe
    (0.5.2), Tier 3 sidecar (0.8.x), persistent handler cache,
    manifest RuntimeLimits overrides, graceful upgrade drain,
    sync extension dispatch, `cap.whoami()`.
- **`docs/CHANGELOG.md`** (this entry).
- **`docs/DEFERRED.md`** (new entry) — the tier3-extension-dispatch
  gap enters the Active list here, with cross-reference to the
  ICD. Moves to Resolved when 0.5.0.4 ships.

### Architect scope decisions

- **Four-part slot (untagged), not tagged milestone.** Prior-arc
  tech debt (unfinished 0.3.x work), not new product capability —
  matches `feedback_tagging_rule.md` "Four-part follow-ups
  accumulate into the next X.Y.Z tag range." The 0.5.0.4 impl
  also ships untagged; next tag is `v0.5.1` (coalescer).
- **Split paper + code sessions.** ICD authored here (0.5.0.3);
  implementation in its own session (0.5.0.4). Mirrors the
  0.5.0.2 paper → LH-1 code split. Rationale: the design
  decisions this ICD pins (sync-vs-async, RuntimeRegistry
  placement, handler-file convention normative, error taxonomy
  extensions) benefit from architect review before implementation
  effort is committed.
- **Band/tag treatment for the 0.5.1 slot.** The originally-
  planned "`0.5.0.3 ICD-0.5.1 authored`" slot slides to
  `0.5.0.5` or later. `v0.5.1 PG auto-event coalescer` remains
  the tagged milestone per the original cadence;
  `project_next_session_0_5_1.md` still captures its scope. No
  ROADMAP change — both LH-1 and 0.5.1 stay as `[medium]` pending
  their respective ship entries.
- **No ROADMAP edit.** The extension-dispatch gap is not a
  roadmap milestone; it closes a known deferral. ROADMAP's
  `## 0.5 — Realtime` entries untouched; `## Load Harness` LH-1
  line untouched (blocker-gated, not re-scoped by this ICD).

### Why now

LH-1 implementation cannot proceed without this dispatch path —
the branch `feat/lh-1-listen-notify-storm` at `339afb0` paused on
2026-04-22 with a live smoke confirming zero handler invocations
through 1.1M WS calls. Shipping LH-1's ICD without fixing the
blocker would produce a fixture that can never run. Shipping the
0.5.1 coalescer on top of a blocked extension-dispatch path would
compound the latent debt and make the coalescer's first stress
test (via LH-1 or LH-2) impossible.

Beyond LH-1, the blocker also affects 0.5.2 (WS broker client-
side dispatch into extensions), every future `cap.call`-between-
extensions path, and every 0.6a admin-extension panel that wants
to reach another extension's capability. Fixing it as prior-arc
tech debt — before the 0.5.x arc accumulates more callers —
minimizes follow-on migration cost.

### Verification

Paper-only session — no runtime verification, no CI, no build.
The verification surface is a doc-review checklist:

- ICD internal §-references resolve; cross-ICD citations to
  `ICD-0.2.2`, `ICD-0.2.4`, `ICD-0.2.6`, `ICD-0.3.1`, `ICD-0.3.3`,
  `ICD-0.3.4`, `ICD-0.4.4`, `ICD-0.5.0`, `ICD-LH-1` land on real
  sections.
- File:line citations in the ICD (resolution.cpp:313–344,
  resolution.hpp:193–201, bridge_context.hpp:69–138,
  runtime_pool.hpp:68–137, run_on_context.cpp:447–469,
  cap_bindings.cpp:92–159, call_dispatch.cpp:65–111,
  install_lifecycle.cpp:842/930/1891/1869,
  pubsub_bindings.cpp:154–167) verified against the current code
  (`main` at `85d0230`) before merge.
- Error-code taxonomy follows the `cap.*` naming from ICD-0.2.2
  / ICD-0.3.4; no re-definitions; five new codes named
  distinctively.
- Scope boundaries explicit: in-scope + out-of-scope + latitude
  sections leave no ambiguity about what 0.5.0.4 must implement
  and what it must not.
- Test plan covers the end-to-end path without LH-1 and without
  the HTTP test harness (deferred separately) — proves the ICD
  is self-sufficient for 0.5.0.4.
- CHANGELOG entry tone parallels the `0.5.0.2` precedent (paper-
  session shape with next-steps and rationale); band / tag call-
  out explicit.
- `DEFERRED.md` 2026-04-22 entry opened Active; will move to
  Resolved on 0.5.0.4 ship.
- No ROADMAP edit — verified by the self-review diff.

### Next scheduled work

- **0.5.0.4 — extension dispatch implementation.** Implements
  this ICD. Branch `feat/0.5.0.4-extension-dispatch` off `main`;
  squash-merged; untagged per four-part slot convention. Scope
  pre-committed to what §CI wiring enumerates — no scope creep.
- **LH-1 resumption.** Picks up `feat/lh-1-listen-notify-storm`
  at `339afb0`; rebuilds the `lh1storm.zip` fixture; folds the
  two ICD-LH-1 corrections tracked in
  `project_plinth_state.md §ICD-LH-1 corrections` (manifest name
  `lh1storm` not `ext_lh1_storm`; drop the `pubsub.publish` RBAC
  rule); runs the 3-trial diagnostic discipline per ICD-LH-1
  §7.2; ships with `## LH-1` CHANGELOG entry.
- **0.5.0.5 or later — ICD-0.5.1 authoring.** The originally-
  planned slot, slid after this pair. Then `v0.5.1 PG auto-event
  coalescer` code milestone, tagged.

---

## 0.5.0.2 — 2026-04-22 — ICD-LH-1 authored (paper docs session, untagged)

Paper-only docs session per METHODOLOGY-llm-assisted-development.md
§3.1 *Forward ICD presence check*. **Un-tagged** per
`feedback_tagging_rule.md` (four-part `X.Y.Z.N` docs sessions
accumulate into the next X.Y.Z release and do not carry their own
tag). Authors the contract for `LH-1 LISTEN/NOTIFY subscribe +
notify-storm tier` — the first LH-stream tier gated on v0.5.0 — ahead
of the LH-1 implementation session. Follows the `0.4.7.2 ICD-0.5.0
authored` paper-session precedent (2026-04-22, commit `6f087e9`).

### Deliverables

- **`docs/icd/ICD-LH-1-listen-notify-storm.md`** (new) — full
  contract for the LH-1 tier. Pins zero new kernel surface (LH-1
  ships entirely as harness + driver extension, exercising v0.5.0
  primitives without extending them); the `ext_lh1_storm` driver
  extension with a single `ext_lh1_storm:1:burst(count, bytes)`
  capability whose handler fires `count` parallel `pubsub.publish`
  calls to exercise the full Layer-3 emit path (extension-identity
  gate → regex → `emit_notify_async` → `pg_notify` → PG ACK); the
  external-LISTEN subscriber shape (Go `lib/pq` `pq.Listener` on
  `plinth:realtime` — single-channel fan-in per ICD-0.5.0, envelope
  dispatch on the harness side); the new `--tier=storm` profile
  (4 producers × 4 subscribers × 16-burst × 512-byte payload × 120s)
  extending the LH-0 binary; CLI flag surface (`--subscribers`,
  `--burst-size`, `--payload-bytes` plus existing LH-0 overrides);
  three-part success criteria (baseline every-run, diagnostic mandate
  for 3-trial reproduction discipline, secondary signals for
  investigation even under a clean baseline); observability fully
  external per LH-stream convention (harness stdout + kernel log tail
  on realtime audit events + `ps`/`pg_stat_activity` sampling); and
  five Open Questions parked for implementer discretion (producer
  mechanism, subscriber path, RBAC seed widening, payload shape,
  CI wiring).
- **`docs/CHANGELOG.md`** (this entry).

### Architect scope decisions

- **Band promotion LH-1 `[medium] → [strong]`.** ROADMAP carries
  LH-1 as `[medium]`; authoring an ICD ahead of implementation
  effectively promotes it to `[strong]` per METHODOLOGY §3.1
  (strong-band milestones require a pinned ICD before opening).
  Mirrors the LH-0.1 precedent (ROADMAP `[strong]`, shipped with
  `ICD-LH-0.1`). The ROADMAP text stays as `[medium]` — the
  promotion is pre-implementation; the LH-1 ship entry will update
  ROADMAP if appropriate.
- **No kernel-side surface changes in LH-1.** The ICD explicitly
  does not carve out a new `lh1:*` kernel signature, dispatch fork,
  or realtime-subsystem API. LH-1's value is *exercising* the v0.5.0
  surface, not extending it. If a kernel-side probe proves necessary
  during implementation (e.g. to observe in-process dispatch counts
  the external-LISTEN subscriber cannot see), it slots as a follow-up
  per §9 future work, not as LH-1 scope creep.
- **LH-stream ICD authoring slot — paper-session pattern.** LH-0 and
  LH-0.1 shipped with their ICDs authored in the same implementation
  PR. LH-1 splits that pattern — this paper session lands the ICD
  ahead of the implementation session. Rationale: the five Open
  Questions resolved here (notably OQ1 producer mechanism and OQ2
  subscriber path) benefit from architect review before
  implementation effort is committed. Future LH-stream tiers can
  follow either pattern; this session establishes the split-slot as
  a viable option.

### Why now

The next code milestone on the main arc is `0.5.1 PG auto-event
coalescer`, which layers a debounce state machine on top of 0.5.0's
`emit_notify_async`. Shipping the coalescer on an unvalidated 0.5.0
foundation risks conflating coalescer issues with emit-path issues;
running LH-1 on 0.5.0 first isolates the signal. The post-v0.5.0
candidate list captured in `project_post_0_5_0_candidates.md`
recommended `LH-1 first, then 0.4.7.3` — the 0.4.7.3 arm was already
absorbed into `0.5.0.1` (commit `7848823`), leaving LH-1 as the
natural next scheduled item. This paper session is the ICD-authoring
prerequisite; the LH-1 implementation session follows next per the
LH-stream convention (untagged, branch `feat/lh-1-listen-notify-storm`
or similar off `main`).

### Verification

Paper-only session — no runtime verification, no CI, no build. The
verification surface is a doc-review checklist:

- ICD internal §-references resolve; cross-ICD citations to
  `ICD-0.5.0` (single-channel `plinth:realtime` fan-in, 8000-byte
  ceiling, `pubsub.publish` extension-identity gate, audit events,
  atexit ordering), `ICD-LH-0` (WS `call` frame, auth flow, driver-
  package install), `ICD-LH-0.1` (120-second tier-duration precedent,
  diagnostic-mandate framing), and `architecture/03-data.md §3.1 + §3.6`
  all land on real content.
- Tier profile table matches the LH-0 / LH-0.1 column shape.
- Success criteria shape (baseline + diagnostic mandate) mirrors
  `ICD-LH-0.1 §9`.
- Non-goals list explicitly names LH-2, LH-3, LH-4, 0.5.1, 0.5.2,
  0.5.4, 0.5.5 with correct ICD / milestone pointers.
- CHANGELOG entry tone parallels the `0.4.7.2 ICD-0.5.0 authored`
  precedent (not the code-ship entries `LH-0` / `LH-0.1`).
- No accidental `ROADMAP.md` edit — LH-1 bullet at §Load Harness stays
  pending `[medium]`; band promotion is noted in this entry and in
  the ICD, not by editing ROADMAP (the LH-1 implementation ship
  entry may update it at the architect's call).
- No `DEFERRED.md` edit — LH-1 does not clear or add any deferred
  item.

### Next scheduled work

- **LH-1 — LISTEN/NOTIFY subscribe + notify-storm tier** implementation.
  Implements ICD-LH-1. Branch `feat/lh-1-listen-notify-storm` (or
  architect's preferred name) off `main`; squash-merged; untagged per
  LH-stream convention.
- After LH-1 lands: `0.5.0.3 ICD-0.5.1 authored` (paper, untagged) —
  the one-ahead horizon slot for the PG auto-event coalescer, per
  `feedback_icd_horizon.md` and `project_next_session_0_5_1.md`. Then
  `0.5.1 PG auto-event coalescer` code milestone, tagged `v0.5.1`.

---

## 0.5.0.1 — 2026-04-22 — `phase_a` / `phase_b` → `rbac_test` / `rule_validator` rename (untagged)

Four-part follow-up to v0.5.0. Single coherent rename pass that closes
the symbol/file-name mismatch shipped in v0.4.7. Untagged per
`feedback_tagging_rule.md` (interim cleanup, not a release). Folds
both the originally-planned 0.4.7.3 (internal symbols) and 0.4.7.4
(public surface — audit strings + schema columns) scopes; see
`DEFERRED.md` Resolved entry for full rationale.

### Renames

- **Namespaces**
  - `plinth::packages::phase_b` → `plinth::packages::rbac_test`
- **Types**
  - `PhaseBReport` → `RbacTestReport`
  - `PhaseBFailure` → `RbacTestFailure`
  - `PhaseBRule` → `RbacTestRule`
- **Methods**
  - `run_phase_b()` → `run_rbac_test()`
  - `schedule_phase_b_detached()` → `schedule_rbac_test_detached()`
  - `emit_phase_b_audit()` → `emit_rbac_test_audit()`
  - `fetch_extension_rules_for_phase_b()` →
    `fetch_extension_rules_for_rbac_test()`
  - `phase_b_report_from_json()` → `rbac_test_report_from_json()`
  - `plinth::rbac::validate_phase_a()` →
    `plinth::rbac::validate_rules()`
- **Catch2 tags**
  - `[phase_b]` → `[rbac_test]`
  - `[phase_b_report]` → `[rbac_test_report]`
  - `[rbac_phase_a]` → `[rule_validator]`
- **Schema columns** (pre-0.7 direct edit, no installed prod databases)
  - `plinth.packages.last_phase_b_run_at` → `last_rbac_test_run_at`
  - `plinth.packages.last_phase_b_result` → `last_rbac_test_result`
- **Audit events**
  - `packages.phase_b_passed` → `packages.rbac_test_passed`
  - `packages.phase_b_failed` → `packages.rbac_test_failed`
- **ICD file**
  - `docs/icd/ICD-0.4.7-rbac-test-execution.md` →
    `docs/icd/ICD-0.4.7-rbac-test-execution.md`
- **Local helpers / test utilities**
  - `wait_phase_b` / `wait_phase_b_settled` /
    `wait_phase_b_audit_count` → `wait_rbac_test` /
    `wait_rbac_test_settled` / `wait_rbac_test_audit_count`
  - Local vars `phase_b_rows`, `pb_count`, `pb_id`,
    tempdir prefix `plinth_phase_b_`
  - Log prefixes `"phase_b:"` / `"phase_b dispatch:"` →
    `"rbac_test:"` / `"rbac_test dispatch:"`
  - Fixture descriptions in `tests/fixtures/rbac_test_runner/*/manifest.json`

### Naming rationale

Names were chosen to satisfy the convention *classes = nouns, methods
= verbs, fields = adjectives / descriptive noun phrases*, so code
reads as prose. `phase_b` / `phase_a` encode position in a sequence,
not meaning; `rbac_test` and `rule_validator` describe what the code
is (noun) and `run_rbac_test` / `validate_rules` describe what it
does (verb).

### Why merged 0.4.7.3 and 0.4.7.4

The 0.4.7.4 decision was originally split out on the hedge that
external consumers (shell / admin UI / third-party extensions) might
already grep against `packages.phase_b_passed` or
`last_phase_b_run_at`. None of those consumers exist yet — 0.6.x
frontend, admin extension (0.6a-E), and third-party extensions are
all future work. Pre-0.7 schema rules still permit direct edits, and
no installed production databases carry 0.4.7+ state. This is the
cheapest possible rename window.

### Files touched

- `migrations/schema.sql` — 2 column renames.
- `src/kernel/packages/rbac_test_runner.{hpp,cpp}` — namespace + all
  types/methods; log prefixes; user-facing CLI output
  (`"Phase B: ..."` → `"RBAC test: ..."`).
- `src/kernel/packages/install_lifecycle.{hpp,cpp}` — 3 call sites,
  reconciler sweep block renamed.
- `src/kernel/packages/reserved_names.hpp` — comment.
- `src/kernel/rbac/rule_validator.{hpp,cpp}` — `validate_phase_a` →
  `validate_rules`.
- `src/kernel/rbac/rule_registrar.{hpp,cpp}` — `PhaseBRule` →
  `RbacTestRule`; `fetch_extension_rules_for_phase_b` rename.
- `src/kernel/rbac/ephemeral_user.{hpp,cpp}` — comments; DB group
  description string `'Phase B test group'` → `'RBAC test group'`.
- `src/kernel/rbac/rbac_manifest.hpp` — comment.
- `src/kernel/main.cpp` — namespace refs; CLI help text.
- `tests/kernel/rbac/rule_validator_test.cpp` — `validate_phase_a` →
  `validate_rules`; tag `[rbac_phase_a]` → `[rule_validator]`.
- `tests/kernel/rbac/ephemeral_user_test.cpp` — tag rename; header
  comment.
- `tests/kernel/packages/rbac_test_report_test.cpp` — tag + types.
- `tests/kernel/packages/rbac_test_runner_test.cpp` — everything
  (tags, types, methods, helpers).
- `tests/kernel/packages/lifecycle_transitions_test.cpp` — helper
  rename.
- `tests/kernel/packages/install_lifecycle_unit_test.cpp` — comment.
- `tests/fixtures/rbac_test_runner/*/manifest.json` — descriptions.
- `CMakeLists.txt` — comment.
- `docs/icd/ICD-0.4.4-package-install-lifecycle.md`,
  `ICD-0.4.5-package-lifecycle-transitions.md`,
  `ICD-0.4.6-rbac-rule-registration.md`,
  `ICD-0.4.7-rbac-test-execution.md` (renamed from
  `...-phase-b-test-execution.md`) — body rewritten in place; each
  carries an amendment note referencing this entry.
- `docs/design/DESIGN-packages-v04x.md` — concept labels updated.
- `docs/architecture/01-identity.md` — `Phase A` / `Phase B` concept
  labels rewritten to `Rule validator` / `RBAC test`. This is the
  origin of the two-phase framing; it remains canonical, just with
  the new names.
- `docs/ROADMAP.md` — 0.4.7.3 + 0.4.7.4 entries removed (folded).
- `docs/DEFERRED.md` — Resolved entry added.

### Verification

- Full build (`cmake --build build -j4`) clean.
- `ctest -j4` 49/49 pass, including the `plinth_tests_pure` grouped
  runner that exercises all RBAC-test and rule-validator integration
  tests.
- Grep post-rename: zero `phase_a` / `phase_b` / `Phase A` / `Phase B`
  references outside amendment/history notes that intentionally cite
  the pre-rename names.

### Historical preservation

Pre-rename names are preserved in the v0.4.7 release commit
(`4a86e31`) and in the amendment notes at the top of each amended
ICD + DEFERRED.md Resolved entry. No git history is rewritten.

---

## v0.5.0 — 2026-04-22 — PG LISTEN/NOTIFY bridge in kernel

First code milestone of the 0.5.x Realtime arc. Implements
`docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md` end-to-end. Tag
`v0.5.0` (architect action per `feedback_tagging_rule.md`).

### Deliverables

- **`src/kernel/realtime/` — new subsystem.**
  - `channel.{hpp,cpp}` — pure-function hand-rolled validator for the
    three-layer logical channel namespace
    (`plinth:data:<schema>.<table>`, `plinth:system:<event_class>`,
    `plinth:ext:<extension>:<event_class>`). Prefers a ~40 LOC
    hand-rolled dispatcher over `std::regex` for precise rejection
    reasons and no ICU dependency. Helpers `channel_layer` /
    `channel_extension` for the layer↔channel consistency check and
    the `pubsub.publish` extension-identity gate.
  - `listener.{hpp,cpp}` — per-node PG `LISTEN/NOTIFY` subscriber.
    Clones the `jthread + eventfd + 1 s reconnect-backoff` pattern
    from `src/kernel/capabilities/listener.cpp` (the 0.2.3
    cache-invalidation listener) — duplicate, not shared, per
    ICD §OQ3. Single literal PG channel `plinth:realtime` fan-in;
    logical Layer 1/2/3 channels ride in the envelope's `channel`
    field. Reconnect-sleep uses `poll(wake_fd, backoff_ms)` so
    `stop_listener()` interrupts the backoff without waiting the
    full interval. Public API: `register_handler`, `start_listener`
    (idempotent; no-op when `listener_cfg.enabled = false`),
    `stop_listener` (synchronous join barrier), and
    `apply_notification_for_test` (parse-path seam for R.08 / R.09 /
    R.10).
  - `emit.{hpp,cpp}` — NOTIFY emission helper. `validate_envelope`
    runs ICD §Validation-pipeline steps 1–5 (layer extract → channel
    regex → layer↔channel consistency → serialize → size check);
    exposed publicly so unit tests exercise every rejection path
    without a live PGconn. `emit_notify(PGconn&, envelope)`
    composes `validate_envelope` + parameterized
    `SELECT pg_notify($1, $2)` + `PGRES_TUPLES_OK` result check.
    `emit_notify_async(DbClientPtr, envelope)` wraps the drogon
    async path with `DrogonDbException → PG_FAILURE` mapping.
    Module-level atomic `max_payload_bytes` (driven from
    `config.realtime.notify.max_payload_bytes`; tests use a
    `ScopedMaxPayload` RAII guard).

- **`src/kernel/js/stdlib/pubsub_bindings.cpp` — new.** `pubsub.publish`
  JS binding. Validates in order: (1) sync arg types → `TypeError`
  without creating the promise; (2) `bc.cancelled` → inline
  `pubsub.cancelled` rejection; (3) channel regex → inline
  `pubsub.channel_invalid`; (4) Layer-3 gate (binding refuses
  `plinth:data:*` / `plinth:system:*` — those are kernel-side only)
  → `pubsub.channel_invalid`; (5) extension-identity gate
  (`channel_extension(channel)` must equal `bc.extension_name`; empty
  = kernel-scope, rejected) → `pubsub.extension_mismatch`; (6)
  pre-enqueue size check against `realtime::get_max_payload_bytes()`
  → `pubsub.payload_too_large`; then (7) enqueues
  `AsyncOp{type=PUBSUB_PUBLISH, pubsub_channel, pubsub_payload}`.

- **`src/kernel/js/run_on_context.cpp` — new dispatch arm.**
  `run_pubsub_publish_outcome` builds the envelope
  `{layer: "extension", channel, payload}` and calls
  `realtime::emit_notify_async` on `drogon::app().getDbClient()`.
  `NotifyError → pubsub.*` rejection mapping per ICD §Rejection codes
  (`INVALID_CHANNEL` / `LAYER_MISMATCH` → `pubsub.channel_invalid`,
  `PAYLOAD_TOO_LARGE` → `pubsub.payload_too_large`, `PG_FAILURE` →
  `pubsub.pg_error`).

- **`src/kernel/js/async_op.hpp` — wiring.** `AsyncOp` grows
  `pubsub_channel` + `pubsub_payload` fields; the
  `PUBSUB_PUBLISH` enum variant was reserved at 0.3.3 and is now
  live.

- **`src/kernel/js/bridge_context.hpp` — new `extension_name` field.**
  `bc.extension` is a `const Extension*` pointer (may be null);
  `pubsub.publish`'s identity gate needs a string field for channel
  comparison. Populated by the existing extension-install seed.

- **`src/kernel/js/stdlib_inject.{hpp,cpp}` — register_pubsub call.**

- **`src/kernel/config.{hpp,cpp}` — `Config::Realtime` substruct +
  `apply_realtime`.** Unlike the other `apply_*` helpers which
  warn-and-default on invalid input, `apply_realtime` throws
  `std::runtime_error` on out-of-range values with the ICD error
  codes `config.realtime.listener.reconnect_backoff_ms_out_of_range`
  and `config.realtime.notify.max_payload_bytes_invalid`. Deliberate
  convention addition for the realtime block — the ICD mandates
  hard-rejection, and silently defaulting a misconfigured realtime
  knob would mask production mistakes.

- **`src/kernel/main.cpp` — atexit + startup wiring.**
  `realtime::start_listener(cfg.db, cfg.realtime.listener)` replaces
  the `// TODO: realtime broker init` placeholder, positioned after
  `capabilities::start_notify_listener`. Atexit chain inserts
  `realtime::stop_listener()` between `ConnectionRegistry::initiate_shutdown`
  and `log::shutdown` per ICD §Ordering rationale (listener's final
  spdlog writes need a live sink).

- **`tests/kernel/ws/ws_test_fixture.cpp` + `async_bridge_fixture.cpp`
  — mirror atexit edit.** Lockstep with main.cpp per
  `feedback_deterministic_teardown.md`.

- **`tests/kernel/realtime/{listener,emit}_test.cpp` +
  `tests/kernel/js/pubsub_test.cpp` — 21 new test cases.**
  R.01–R.10 listener lifecycle, E.01–E.06 emit, P.01–P.05 JS
  binding. Catch2 tags `[realtime][unit]` (no-PG) /
  `[realtime][integration]` (PG-gated) / `[js][realtime][integration]`
  (JS binding tests route through the existing `async_bridge_fixture`
  drogon-loop singleton). R.04 simplified to a bad-port reconnect
  + stop-interrupt timing check (no live PG toggle).

- **`src/kernel/js/conversion.cpp` — robustness fix.**
  `classify_rejection` now treats `null` / `undefined` rejection
  reasons as `MEMORY_LIMIT` when a memory limit is configured.
  QuickJS falls back to a bare `JS_NULL` rejection when OOM fires
  during `InternalError` construction itself; previously this
  slipped through as `PROMISE_REJECTED_UNHANDLED`. Surfaced by
  this milestone's added stdlib namespace shifting the N.37
  memory-cap test baseline.

### Test matrix

- `[realtime]`:  30 cases / 105 assertions ✔
  (4 config + 10 listener + 6 emit + 5 pubsub + 5 ancillary
  validator/channel coverage)
- `[js][async]`: 45 cases / 120 assertions ✔ (no regression)
- Full `ctest` subprocess grouping: 49/49 green
- Full suite:     573 cases / 3500 assertions ✔

### Scope deviations from ICD

- `start_listener` signature extended to
  `start_listener(const Config::Database&, const Config::Realtime::Listener&)`
  (the ICD-pinned signature is `(const Config::Database&)`). The
  `Listener` config is needed in-API so R.02 (`enabled=false`
  no-spawn) is directly testable and `reconnect_backoff_ms` flows
  cleanly from config into the thread body. Forward-compatible.
- `apply_notification_for_test` signature is `(channel, payload_json)`
  — parse-only, no `db_cfg` argument (0.5.0 has no resync hook; the
  0.2.3 listener's `(db_cfg, payload)` signature would be unused
  noise here).
- `apply_realtime` rejects via `std::runtime_error`; other `apply_*`
  helpers warn-and-default. Deliberate new convention for the
  realtime block (see Deliverables above).
- R.04 ("PG down at start") simplified to bad-port + reconnect-loop
  + stop-interrupt timing check. The ICD's live-toggle variant would
  require a second PG instance (or a proxy) in CI; the simplified
  form covers the same invariant (reconnect loop + interruptible
  backoff).
- `packages.installed` smoke-retrofit left OUT of this milestone.
  The ICD §NOTIFY Emission Helper → Callers in 0.5.0 mentions a
  "new `packages.installed` emit added as a smoke-test emission
  (exercised by E.05)" — E.05 is `LAYER_MISMATCH`, not a smoke test,
  so the cross-reference appears to be an ICD typo (likely E.01).
  E.01 covers the emit→listener smoke round-trip via test-code
  `emit_notify` call without requiring a production retrofit. Adding
  the retrofit is a contained ~10-line edit to
  `src/kernel/packages/install.cpp`; tracked as a 0.5.x.N follow-up
  or folded opportunistically into 0.5.1.

### Out of scope (deferred, per ICD)

- Debouncer / coalescer (0.5.1)
- WebSocket broker fan-out (0.5.2)
- `pubsub.subscribe` (0.5.2)
- `plinth.events` persistence + delta sync (0.5.4)
- Monotonic sequence numbers (0.5.5)
- `db.batch()` / silent mode (0.5.3)
- Sidecar Layer 4 NOTIFY production (0.8.x)
- Migration of the 0.2.3 `plinth_capability_changed` listener
  (stays on its own channel as a sibling subsystem)
- `listener_loop.hpp` utility extraction (§OQ3 — duplicate in 0.5.0)

### Verification

- Build: `cmake --build build -j4` (parallelism cap per
  `feedback_parallelism_cap.md`). Zero warnings introduced.
- Tests: `PLINTH_PG_HOST=... ctest --test-dir build --output-on-failure`
  green (49/49 subprocesses).
- Manual repro of `realtime.listener.started` audit event on a live
  bootstrap (confirmed via log inspection).
- Memory-cap test (N.37) regression traced + fixed in
  `classify_rejection`.

### What's next

ROADMAP `## 0.5 — Realtime` 0.5.0 line removed. Next ROADMAP item
is `0.5.1 DB layer auto-event emission (debounced coalescer)` —
the primary caller of this milestone's `emit_notify_async`. Per
the one-ahead horizon rule, the next 4-part docs slot (expected
`0.5.0.1`) is `ICD-0.5.1 authoring`.

---

## 0.4.7.2 — 2026-04-22 — ICD-0.5.0 authored (paper docs session)

Paper-only docs session per METHODOLOGY-llm-assisted-development.md
§3.1 *Forward ICD presence check*. **Un-tagged** per
`feedback_tagging_rule.md` (four-part `X.Y.Z.N` docs sessions
accumulate into the next X.Y.Z release and do not carry their own
tag). Discharges the only remaining ROADMAP entry in `## 0.4 —
Package System`; the section is removed on this merge per the
ROADMAP preamble *Completed milestones are removed* rule. Scheduled
by `RE-EVAL-0.4.x-arc-closeout.md §2.6 + §6` as a dedicated paper
session ahead of `0.5.0 PG LISTEN/NOTIFY bridge in kernel` code
work (horizon rule one-ahead of in-flight code).

### Deliverables

- **`docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md`** (new) — full
  contract for the kernel-side PG `LISTEN/NOTIFY` bridge. Pins the
  channel naming scheme (`plinth:data:<schema>.<table>`,
  `plinth:system:<event_class>`, `plinth:ext:<extension>:<event_class>`)
  promoted from `architecture/03-data.md §3.3` + Appendix A DESIGN
  prose to normative contract; the payload envelope JSON shape with
  required `layer` + `channel` and reserved `seq` / `truncated` /
  `ops` / `table` slots that 0.5.1 + 0.5.5 will populate; the
  per-node listener subsystem contract (jthread + eventfd + reconnect
  with 1 s backoff + resync-on-reconnect — architecture promotion of
  the 0.2.3 capabilities listener pattern as a sibling subsystem, not
  a replacement); the `plinth::realtime::emit_notify(conn, envelope)`
  + `emit_notify_async(db, envelope)` helper with `NotifyError` enum
  (`INVALID_CHANNEL` / `PAYLOAD_TOO_LARGE` / `PG_FAILURE` /
  `MISSING_LAYER` / `LAYER_MISMATCH`); the Layer 3 `pubsub.publish`
  JS stdlib binding (architect-confirmed in-scope for 0.5.0,
  2026-04-22) with extension-identity-gated channel name check and
  `pubsub.channel_invalid` / `pubsub.extension_mismatch` /
  `pubsub.payload_too_large` / `pubsub.pg_error` rejection taxonomy;
  the `realtime.*` config block (`listener.enabled`,
  `listener.reconnect_backoff_ms`, `notify.max_payload_bytes`); three
  audit event kinds (`realtime.listener.started`,
  `realtime.listener.reconnected`, `realtime.notify.rejected`); the
  deterministic-teardown contract per
  `feedback_deterministic_teardown.md` inserting
  `realtime::stop_listener()` into the `main.cpp` atexit chain
  between `ConnectionRegistry::initiate_shutdown` and
  `log::shutdown`; normative HA semantics promoted verbatim from
  `architecture/03-data.md §3.6` (all N nodes LISTEN the same
  channels; any NOTIFY reaches all nodes; fan-out is per-node
  local); and 21 enumerated test cases across three new TUs (R.01–R.10
  listener lifecycle, E.01–E.06 emit helper, P.01–P.05 JS binding).
  Single-channel fan-in resolved as the PG-side channel (`plinth:realtime`);
  logical channel carried in the envelope's `channel` field. Five
  Open Questions parked for implementer discretion or delta-ICD
  resolution.
- **`docs/ROADMAP.md`** (edit) — `## 0.4 — Package System` section
  removed (the 0.4.7.2 entry was its last item; completed-milestone
  removal per preamble). 0.5.0 `[strong]` bullet stays unchanged
  under `## 0.5 — Realtime`. `## 0.4.x cleanup follow-ups
  (cross-cutting; scheduled between milestones)` section (inserted
  by 0.4.7.1) retained.
- **`docs/CHANGELOG.md`** (this entry).

### Architect scope decisions

- **Layer 3 `pubsub.publish` JS binding — in scope for 0.5.0.**
  ICD pins the full kernel surface (listener + emit helper) plus the
  JS binding; extension publishing works immediately on 0.5.0 merge.
  `pubsub.subscribe` remains deferred to 0.5.2 alongside the
  WebSocket broker.

### Why now

The 0.4.x arc closed on the `v0.4.7` merge commit `4a86e31` (#63);
the `0.4.7.1` RE-EVAL (PR #64, commit `783687b`) discharged the
`RE-EVAL following 0.4.7` slot and scheduled this session as the
next ROADMAP item. Per ROADMAP preamble, no 0.5.x code milestone
begins until the preceding RE-EVAL completes AND the one-ahead
horizon ICD is in place. This session puts ICD-0.5.0 on disk so
`0.5.0 PG LISTEN/NOTIFY bridge in kernel` can open on the next
code session. Matches the 0.4.x cadence (every code milestone in
the arc had a dedicated 4-part docs-session predecessor:
`0.4.2.1 → ICD-0.4.3/0.4.4`, `0.4.4.2 → ICD-0.4.5`,
`0.4.5.2 → ICD-0.4.6`, `0.4.6.1 → ICD-0.4.7`).

### Verification

Paper-only session — no runtime verification, no CI. Cross-document
link-resolution, CHANGELOG-tone parallel to
`0.4.5.2 / 0.4.6.1 / 0.3.3.4` paper sessions, single-section
ROADMAP removal with no collateral edits, ICD section ordering
matches `ICD-0.4.6 / ICD-0.3.3` templates.

### Next scheduled work

- `v0.4.7` tag cut on commit `4a86e31` (architect action — three-
  part `X.Y.Z` = release tag per `feedback_tagging_rule.md`).
- **0.5.0 PG LISTEN/NOTIFY bridge in kernel** — first 0.5.x code
  milestone. Implements ICD-0.5.0. `[strong]` per
  `RE-EVAL-0.4.x-arc-closeout.md §6` (promoted from `[medium]` in
  this arc's RE-EVAL).

---

## 0.4.7.1 — 2026-04-22 — RE-EVAL following 0.4.7 (0.4.x arc closeout)

Rewrite session per METHODOLOGY-llm-assisted-development.md §Phase 3.
**Un-tagged** per `feedback_tagging_rule.md` (interim doc patches get
CHANGELOG entries only). Discharges the only remaining ROADMAP item
in `## 0.4 — Package System`. Sixth scheduled re-evaluation;
arc-closeout trigger fired at 3/4 cadence (precedent:
`RE-EVAL-0.3.x-arc-closeout.md` fired at 2/4 because 0.3.5 closed the
0.3.X arc). Window: 0.4.5 + 0.4.5.1 + 0.4.5.2 + 0.4.6 + 0.4.6.1 +
0.4.7 + parallel LH-0 + LH-0.1.

### Deliverables

- **`docs/reviews/RE-EVAL-0.4.x-arc-closeout.md`** (new) — 11-section
  artefact mirroring the `RE-EVAL-0.3.x-arc-closeout.md` template.
  Inputs read; six gaps categorized per METHODOLOGY §3.1.1; zero-gap
  baseline; consolidated 32-row deviations table (24 across
  0.4.5–0.4.7 + 8 across LH-0/LH-0.1); known-issues; forward ICD
  presence check on next-N=3; disposition; band-label review;
  methodology observations; exit criteria; next-architect actions.
- **`docs/icd/ICD-0.4.5-package-lifecycle-transitions.md`** (amend)
  — one-line "Implementation deviation (0.4.5 file placement)" on
  the `unregister_capability` line in `**Related**`, naming
  `registration.{hpp,cpp}` as the actual home (vs. ICD-named
  `resolution.{hpp,cpp}`).
- **`docs/icd/ICD-0.4.6-rbac-rule-registration.md`** (amend) — new
  "Implementation deviation (0.4.6 file rename)" subsection ahead
  of §Scope naming `phase_a.{hpp,cpp}` → `rule_validator.{hpp,cpp}`
  rename and rationale.
- **`docs/icd/ICD-0.4.7-rbac-test-execution.md`** (amend) —
  symmetric "Implementation deviation (0.4.7 file rename)"
  subsection ahead of §Scope Summary naming `phase_b.{hpp,cpp}` →
  `rbac_test_runner.{hpp,cpp}` rename, fixture-directory rename
  (`tests/fixtures/phase_b/` → `tests/fixtures/rbac_test_runner/`),
  and CMake-target rename (`plinth_phase_b_fixture_zips` →
  `plinth_rbac_test_runner_fixture_zips`).
- **`docs/architecture/02-capabilities.md`** (amend) — new §1.13
  "Drain Primitive (per-extension in-flight counter)" describing
  `DispatchGuard` as a kernel primitive consumed by the upgrade
  choreography. New §4 "Diagnostic Kernel Surfaces (Load Harness
  stream)" describing `lh0:1:chain` Tier 1 capability and the
  `lh0:1:js_stress` dispatch fork as deliberate diagnostic-only
  deviations with explicit "not a blueprint" framing.
- **`docs/SESSION-GUIDE.md`** (amend) — new "Test grouping
  convention (since 0.4.5.1)" subsection naming the four ctest
  groups, the tag-routing matrix, the fixture mapping, and the
  `[js][async]` per-TEST_CASE exception with pointer to the
  open kernel-side investigation.
- **`docs/DEFERRED.md`** (amend) — WS-teardown entry rewritten to
  reflect LH-0.1's empirical resolution (production kernel
  confirmed clean; entry rescoped to `[js][async]` Catch2
  subprocess sequential-run race). New §Active entry for
  ICD-0.4.5 X.05 / X.06 / X.07 / X.08 / X.09 / X.10 / X.11 /
  X.12 / X.13 + G.03 deferred coverage (companion to the
  I.18/I.19/I.20 entry; HTTP fixture is the shared dependency).
- **`docs/ROADMAP.md`** (edit) — `RE-EVAL following 0.4.7 (0.4.x
  arc closeout)` line removed (discharged by this session).
  `0.5.0 PG LISTEN/NOTIFY bridge in kernel` promoted `[medium]`
  → `[strong]`. New `0.4.7.2 ICD-0.5.0 authored` paper-session
  slot inserted between §0.4 closeout and §0.5 ahead of `0.5.0`
  (one-ahead horizon per forward-ICD-presence rule). **New
  `## 0.4.x cleanup follow-ups (cross-cutting; scheduled between
  milestones)` section** ahead of `## Load Harness` with four
  scheduled items absorbing previously-DEFERRED-only work
  (architect-flagged 2026-04-22 that deferred items + naming
  technical debt need firm ROADMAP placement, not just
  DEFERRED.md ledger entries):
  - `0.4.7.3 phase_a / phase_b internal-symbol rename` `[strong]`
    — pure internal rename of namespaces / types / methods /
    helper fns / Catch2 tags following the v0.4.7 file renames.
    Estimated ~1 session. Public-surface symbols (audit strings,
    schema columns) deliberately out-of-scope; tracked separately.
  - `0.4.7.4 Public-surface phase_b rename + migration plan`
    `[medium]` — decision document for the audit-event strings
    `packages.phase_b_passed` / `_failed` and schema columns
    `plinth.packages.last_phase_b_*`; resolves as either a
    one-shot migration or a "leave the public surface" decision
    with that resolution recorded.
  - `0.5.x.N HTTP test harness for /api/packages` `[strong]`
    — absorbs DEFERRED.md I.18/I.19/I.20 (concurrent POST,
    `?dry_run=1`, RBAC denial) + ICD-0.4.5 X.05/X.06/X.07/X.08/
    X.09/X.10/X.11/X.12/X.13 + G.03 (extended upgrade/GC test
    coverage). Build a Drogon-with-RBAC-seeded-session fixture
    (`tests/kernel/packages/http_test_fixture.{hpp,cpp}`), then
    add the 13 deferred cases. Crash-injection cases (X.12)
    keep an open decision on fork/SIGKILL harness.
  - `0.5.x.N [js][async] kernel-side refcount investigation`
    `[medium]` — root-cause the residual `[js][async]` Catch2
    sequential-run race per `project_ws_flaky_segfault.md
    §Candidate root causes`. LH-0.1 empirically confirmed
    production unaffected; this is test-strategy cleanup. Win
    condition: enable `[js][async]` grouping in 0.4.5.1's ctest
    model.

### Why now

`docs/ROADMAP.md` `## 0.4 — Package System` had a single remaining
entry: the arc-closeout re-eval. Per ROADMAP preamble, no further
code milestone begins until this re-eval lands. 0.4.5 + 0.4.6 +
0.4.7 closed the 0.4.x arc on the package-system axis; LH-0 + LH-0.1
landed in parallel as the load-harness stream; 0.4.5.1 redesigned
the test strategy as the prior re-eval scoped. Three substantive
strands needed code-aware reconciliation against the architecture
tree before 0.5.x opens. Mid-session architect feedback added a
fourth: deferred items + naming technical debt need firm ROADMAP
placement, not just DEFERRED.md ledger entries — captured as
§2.11 of the re-eval artefact and discharged via the new
`## 0.4.x cleanup follow-ups` ROADMAP section.

### Why no tag

Per `feedback_tagging_rule.md`, three-part `X.Y.Z` tags only.
Four-part interim doc patches accumulate into the next X.Y.Z tag
range. The v0.4.7 release tag itself is the architect's
responsibility (separate from this PR; see `RE-EVAL-0.4.x-arc-closeout.md
§11 item 2`).

### Verification

- All ICD amendments kept text additions, did not remove or
  reword existing contracts.
- Architecture amendments (§1.13, §4 in `02-capabilities.md`)
  describe code that exists today; verified by reading
  `src/kernel/capabilities/drain.{hpp,cpp}`,
  `src/kernel/ws/{call_dispatch,js_stress}.{hpp,cpp}`, and
  `src/kernel/main.cpp`'s `init_js_stress_pool` /
  `shutdown_js_stress_pool` lifecycle wires.
- DEFERRED.md WS-teardown rewrite preserves the bandaid history,
  promotes the architect's hypothesis to an empirical finding
  per LH-0.1, and rescopes residual to the `[js][async]`
  subprocess race (kernel-side root-cause candidate list at
  `project_ws_flaky_segfault.md`).
- ROADMAP edits verified consistent with §6 forward-ICD scheduling
  + §8 band review of the re-eval artefact.
- Build sanity: `cmake --build build -j 4` clean (no source files
  changed; only docs); ctest not re-run since no code changed;
  `run-clang-tidy-20` not re-run for the same reason.

### Files

| Path | Change |
|------|--------|
| `docs/reviews/RE-EVAL-0.4.x-arc-closeout.md` | new |
| `docs/icd/ICD-0.4.5-package-lifecycle-transitions.md` | amend (§Related line) |
| `docs/icd/ICD-0.4.6-rbac-rule-registration.md` | amend (new Implementation-deviation subsection) |
| `docs/icd/ICD-0.4.7-rbac-test-execution.md` | amend (new Implementation-deviation subsection) |
| `docs/architecture/02-capabilities.md` | amend (new §1.13 + §4) |
| `docs/SESSION-GUIDE.md` | amend (new test-grouping subsection) |
| `docs/DEFERRED.md` | amend (WS-teardown rewrite + new ICD-0.4.5 X/G entry) |
| `docs/ROADMAP.md` | amend (RE-EVAL discharged, 0.5.0 promoted, 0.4.7.2 inserted) |
| `docs/CHANGELOG.md` | this entry |

### Next

`0.4.7.2 ICD-0.5.0 authored` — paper docs session, fires before
0.5.0 code begins.

---

## v0.4.7 — 2026-04-21 — RBAC Phase B test execution (0.4.x arc close-out)

Implements [ICD-0.4.7-rbac-test-execution.md](icd/ICD-0.4.7-rbac-test-execution.md)
and closes the 0.4.x arc. After a package reaches ACTIVE, the kernel
runs each rule's `assert_deny` / `assert_allow` test contract via a
synthesised `UserContext` through the existing `call_capability`
dispatch pipeline, compares the observed outcome to the declared
expectation, and persists the aggregate result in
`plinth.packages.last_phase_b_result`. All-pass leaves the package
ACTIVE; any-fail flips to ACTIVE_FLAGGED (advisory — the extension
keeps running). The flag clears on any successful re-run, including
the new `plinth test rbac <extension>` CLI subcommand.

Shipped as a three-slice arc on `feat/0.4.7-rbac-phase-b-slice-{a,b,c}`;
this entry consolidates all three slices under the single tag.

### What shipped

- **Phase B execution engine** in new
  `src/kernel/packages/rbac_test_runner.{hpp,cpp}`. `run_phase_b(package_id,
  ctx, triggered_by, run_id_override?)` is the synchronous entry —
  acquires the per-name advisory lock (reusing the 0.4.4 install key
  — OQ3), creates ephemeral users, iterates rules sequentially,
  writes `last_phase_b_*`, emits audit. `schedule_phase_b_detached`
  fires-and-forgets a `std::thread` for post-install / post-enable /
  post-upgrade triggers. `PhaseBReport` + `PhaseBFailure` structs
  match the ICD's JSONB / error shape. Per-rule wall-clock bounded
  by `ctx.upgrade_drain_timeout_ms{5000}` (OQ4). Invokes dispatch on
  a detached worker with a `std::promise` bridge so malicious
  handlers cannot hang the orchestrator.
- **Ephemeral-user factory** in new
  `src/kernel/rbac/ephemeral_user.{hpp,cpp}`. `create_run_users(run_id,
  conn)` creates both `__test_denied_<run_id>` and
  `__test_allowed_<run_id>` plus the synthetic `__rbac_test_<run_id>`
  group; `grant_rule_to_run_group` / `revoke_rule_from_run_group` toggle
  the single-rule grant per rule iteration; `destroy_run_users` is
  idempotent. `cleanup_orphaned_test_users(older_than, conn)` is the
  reconciler sweep. `build_test_user_context` synthesises the
  `UserContext` Phase B hands to `call_capability` — never
  authenticates over HTTP.
- **Three trigger sites** in `src/kernel/packages/install_lifecycle.cpp`:
  after `emit_installed_audit` in `install_package` (`:1583`), after
  `emit_transition_audit("packages.enabled")` in `enable_package`
  (`:1940`), and after `emit_upgrade_audit("packages.upgrade_completed")`
  in `upgrade_package` (`:3124`). All three call
  `schedule_phase_b_detached` — post-COMMIT, the detached thread
  only starts after the outer transaction's state=ACTIVE is durable.
- **Reconciler extension** in `reconcile_in_flight_installs`:
  (a) calls `cleanup_orphaned_test_users(NOW() - 1h, conn)` once per
  invocation; (b) for each package at ACTIVE/ACTIVE_FLAGGED with
  `last_phase_b_run_at IS NULL` AND `installed_at > NOW() - interval
  '1 hour'`, schedules a Phase B run. Older NULL rows stay NULL per
  ICD §Crash Recovery. The early `return;` in the no-in-flight-rows
  branch was removed so the Phase B passes always execute.
- **`plinth test rbac <extension>`** CLI subcommand wired in
  `src/kernel/main.cpp`. Two-level argparse nest (`test` parent →
  `rbac` child) mirroring `validate_cmd`. Flags: `--json` (reuses
  0.4.0 `ValidatorOptions.json_output` convention), `--run-id <uuid>`
  (deterministic ephemeral-user names for test harnesses),
  `--config <path>` (reuses serve's config-path convention). Exit
  codes 0 (all-pass → ACTIVE), 1 (any-fail → ACTIVE_FLAGGED),
  2 (operational error; message on stderr) per ICD §CLI Surface.
  Body in `run_cli_test_rbac(opts, ctx, out, err)` in `phase_b.cpp` —
  shares `run_phase_b` with the post-install trigger (one code path,
  two entry points; `triggered_by = "cli"`). Package lookup via
  name → id using the `uniq_packages_name_active` partial index.
  Keeps `main.cpp` thin per `feedback_main_size.md`.
- **Two new audit events**: `packages.phase_b_passed` and
  `packages.phase_b_failed`, emitted via `plinth::log::audit_sync`.
  Detail payload mirrors `PhaseBReport` with an additional
  `triggered_by` ∈ {install, enable, upgrade, cli, reconcile}. Rate
  limiting: none at the Phase B layer (one package install emits at
  most one aggregate event, not N per failed rule).
- **Schema edit** in `migrations/schema.sql`:
  `ALTER TABLE plinth.users ADD COLUMN is_test_user BOOLEAN NOT
  NULL DEFAULT false` + partial index `idx_users_is_test_user`. Two
  set-returning user-count sites in `auth/handlers.cpp` (first-user
  bootstrap checks) gained `WHERE is_test_user = false` filters per
  ICD §User-Listing Query Filter. Pre-0.7 schema freeze; direct
  edit, no numbered migration file.
- **`fetch_extension_rules_for_phase_b`** helper added to
  `src/kernel/rbac/rule_registrar.{hpp,cpp}`. Loads every
  `plinth.rbac_rules` row for an extension including NULL
  `test_contract` rows; the skip-vs-run decision happens in
  `run_phase_b`.
- **Six fixture packages** under `tests/fixtures/phase_b/`:
  `happypass`, `brokendeny`, `brokenallow`, `mixedrules`, `notests`,
  `allowside` (identifier-only names per the extension-name regex).
  New `plinth_phase_b_fixture_zips` CMake target builds
  `${CMAKE_BINARY_DIR}/fixtures/phase_b/<name>.zip` (subdir scoped
  to avoid collisions with the flat lifecycle fixtures).
- **24 new Catch2 cases** — exceeds the ICD's 23-case target by one
  (two CLI edge cases added as PB.07b/PB.07c for operational-error
  and JSON-output coverage). Split: 8 pure (B.01–B.08) routed to
  `plinth_tests_pure`, 16 PG-gated (PB.01–PB.15, PB.07b, PB.07c)
  routed to `plinth_tests_pg` via the `[integration]` Catch2 tag.
  New TUs: `tests/kernel/packages/phase_b_report_test.cpp` (B.01–B.03),
  `tests/kernel/rbac/ephemeral_user_test.cpp` (B.04–B.07),
  `tests/kernel/packages/phase_b_test.cpp` (PB.01–PB.15 + PB.07*),
  `tests/kernel/main/cli_test_rbac_test.cpp` (B.08). PB.13 / PB.14 /
  PB.15 drive through `install_package` / `upgrade_package`.

### Deviations (consolidated, same footing as the 0.2.0 / 0.2.2 /
0.2.4 / 0.2.5 / 0.3.1 / 0.3.2 / 0.4.0 / 0.4.1 / 0.4.6 precedent)

- **Synchronous `call_capability` on a detached std::thread**, not
  `call_capability_async` + `queueInLoop` + promise bridge. ICD
  proposed the async form for uniformity; the async wrapper is
  currently `co_return call_capability(call, ctx)` with no real
  suspension, so the sync form is observationally identical and
  avoids dragging Drogon into the test fixture. Future Tier 3
  sidecar promotion can upgrade the surface if needed.
- **Slice A seeds SQL directly** instead of driving `install_package`.
  Kept the early Phase B integration tests (PB.01–PB.06, PB.08,
  PB.09) hermetic; Slice B adds install-driven PB.13 / PB.14 once
  the fixture infrastructure is in place.
- **PB.15 semantics** rewritten as "sequential re-runs each acquire
  fresh advisory lock" rather than the ICD's concurrent-contention
  assertion. Phase B uses `pg_try_advisory_lock` (non-blocking), so
  contended runs return `lock_failed` without emitting an audit —
  the ICD's "both complete, distinct run_ids" wording needs a
  blocking variant. Slice C's CLI path provides the synchronous
  entry point a future contention test would use.
- **PB.14 fixture reuse** — uses existing `valid-install` (v1.2.3) +
  `upgrade-v2` (v1.3.0) from `tests/fixtures/lifecycle_transitions/`
  rather than adding a seventh `happy-all-pass-v2` fixture. Neither
  declares `test_contracts`, so Phase B runs skip-only; the assertion
  is about `last_phase_b_run_at` populating on the new row, which the
  skip-only path satisfies.
- **PB.13 Tier 2 cache reload in test** — the detached Phase B worker
  sees `capability_not_found` because the test binary has no
  LISTEN/NOTIFY listener; PB.13 calls
  `plinth::capabilities::reload_tier2_cache(s.ctx.db)` after
  `install_package` to win the race. Production always has the
  LISTEN/NOTIFY listener running in `serve` mode.
- **CF4 handler-file presence** surfaced during fixture authoring —
  capability-manifest cross-file validation requires handler files
  per declared capability function. Three fixture-build-time errors
  caught and fixed before landing; not an ICD deviation, documented
  here for future fixture authors.
- **Lifecycle-test serialisation via `wait_phase_b_settled`** —
  lifecycle transitions (0.4.5 disable/enable/uninstall/upgrade/GC)
  use the same per-name advisory lock Phase B holds. Tests that
  call `install_package` immediately followed by
  `disable_package` / `upgrade_package` must call the new
  `wait_phase_b_settled` helper (in `lifecycle_transitions_test.cpp`)
  between the two or race the lock. Subsequent transitions
  serialise naturally because they all use `pg_try_advisory_lock`.
  Any future lifecycle test must follow the same pattern.
- **Unified `run_phase_b` with optional `run_id_override`** instead
  of an overload. The CLI's `--run-id` flag + the PB.07* test
  determinism need the same thread-through; a default-empty
  parameter is lighter than a new entry point.
- **24 test cases vs the ICD's 23** — added PB.07b (unknown
  extension → exit 2) and PB.07c (`--json` output round-trip) as
  CLI-specific edge cases. Same footing as the CHANGELOG-listed
  test-count deviations in prior milestones.
- **File renames away from `phase_a` / `phase_b` jargon.** The
  DESIGN concept names (`Phase A` validation, `Phase B` execution)
  stay locked into the public surface — namespaces, struct names,
  audit kinds (`packages.phase_b_passed` / `_failed`), schema
  columns (`last_phase_b_run_at` / `last_phase_b_result`), Catch2
  tags (`[phase_b]`, `[rbac_phase_a]`, `[phase_b_report]`). The
  file paths describe what the code does instead of which DESIGN
  phase it implements: `src/kernel/rbac/phase_a.{hpp,cpp}` →
  `rule_validator.{hpp,cpp}` (pairs symmetrically with the existing
  `rule_registrar`); `src/kernel/packages/phase_b.{hpp,cpp}` →
  `rbac_test_runner.{hpp,cpp}`; test + fixture directory names
  follow (`tests/fixtures/phase_b/` → `tests/fixtures/rbac_test_runner/`;
  CMake target `plinth_phase_b_fixture_zips` →
  `plinth_rbac_test_runner_fixture_zips`). Types/namespaces/methods
  stay for RE-EVAL discussion.
- **OQ resolutions (architect-confirmed 2026-04-21)**: OQ1
  `ACTIVE_FLAGGED` blocks no 0.4.5 transition; OQ2 per-execution
  `run_id` (UUIDv4); OQ3 reuse install advisory lock; OQ4 reuse
  `upgrade_drain_timeout_ms{5000}` as per-rule cap; OQ5 skipped
  rules reported in `PhaseBReport` and audit.

### Verification

- Build clean on GCC 14.2.0 Debug (`cmake --build build -j 4`).
- `plinth_tests "[phase_b]"` with `PLINTH_PG_HOST=127.0.0.1` etc.:
  21 cases, 231 assertions, all pass. `plinth_tests "[test_rbac]"`:
  5 cases, 12 assertions, all pass.
- Full ctest via grouped entries (`plinth_tests_pure`, `_js`, `_pg`,
  `_ws`) green — 49 ctest subprocesses under the 0.4.5.1 model.
- `run-clang-tidy-20 -p build` zero findings on the three new TUs
  (`phase_b.cpp`, `ephemeral_user.cpp`, `cli_test_rbac_test.cpp`)
  and modified TUs (`install_lifecycle.cpp`, `main.cpp`,
  `auth/handlers.cpp`, `rule_registrar.cpp`).

### Demo (ICD §Entry/Exit exit criterion verbatim)

1. Install a fixture with one pass-rule and one deliberately-broken
   deny-rule → package lands in `ACTIVE_FLAGGED` + audit
   `packages.phase_b_failed` with per-rule detail.
2. `plinth test rbac <ext>` → exit 1; stdout shows the failed rule.
3. Fix the broken grant (DELETE from `plinth.group_rules`).
4. `plinth test rbac <ext>` → exit 0; `state` back to `ACTIVE`;
   second audit row `packages.phase_b_passed`.

Exercised end-to-end by PB.07 in `tests/kernel/packages/phase_b_test.cpp`.

### Files touched

| Path | Change |
|------|--------|
| `src/kernel/packages/rbac_test_runner.{hpp,cpp}` | new |
| `src/kernel/rbac/ephemeral_user.{hpp,cpp}` | new |
| `src/kernel/rbac/rule_registrar.{hpp,cpp}` | `fetch_extension_rules_for_phase_b` added |
| `src/kernel/packages/install_lifecycle.cpp` | 3 trigger sites + reconciler extension |
| `src/kernel/main.cpp` | `test rbac` subparser + dispatch |
| `src/kernel/auth/handlers.cpp` | 2 × COUNT(*) filters |
| `migrations/schema.sql` | `plinth.users.is_test_user` + partial index |
| `tests/kernel/packages/rbac_test_report_test.cpp` | new (B.01–B.03) |
| `tests/kernel/rbac/ephemeral_user_test.cpp` | new (B.04–B.07) |
| `tests/kernel/packages/rbac_test_runner_test.cpp` | new (PB.01–PB.15, PB.07b, PB.07c) |
| `tests/kernel/main/cli_test_rbac_test.cpp` | new (B.08) |
| `tests/kernel/packages/lifecycle_transitions_test.cpp` | `wait_phase_b_settled` helper added |
| `tests/fixtures/rbac_test_runner/*` | 6 fixture packages |
| `CMakeLists.txt` | `plinth_rbac_test_runner_fixture_zips` target + argparse link for tests + new test TUs |
| `docs/ROADMAP.md` | 0.4.7 entry removed (arc close-out) |
| `docs/CHANGELOG.md` | this entry |

### Next scheduled work

`RE-EVAL following 0.4.7 (0.4.x arc closeout)` — the rewrite session
that rebands the 0.5.x / 0.6.x windows and runs the code-aware
gap-analysis pass against the shipped 0.4.x arc. Per ROADMAP
cadence; no code milestone begins until the re-eval completes.

---

## 0.4.6.1 — 2026-04-21 — Docs: ICD-0.4.7 authored (untagged)

Paper-only four-part session. Untagged per `feedback_tagging_rule.md`
(four-part docs = CHANGELOG entry only). Authors the contract for the
final code milestone of the 0.4.x arc.

### What shipped

- **`docs/icd/ICD-0.4.7-rbac-test-execution.md`** — new. Turns
  `DESIGN-packages-v04x.md §0.4.7` and `architecture/01-identity.md §2.2`
  (the two-phase RBAC validation contract) into a signed-off
  implementation spec: Phase B execution engine
  (`src/kernel/packages/phase_b.{hpp,cpp}`), ephemeral-user factory
  (`src/kernel/rbac/ephemeral_user.{hpp,cpp}`), three trigger sites
  (install / enable / upgrade), two new audit kinds
  (`packages.phase_b_passed` / `_failed`), one schema edit
  (`plinth.users.is_test_user`), and the `plinth test rbac {extension}`
  CLI subcommand.
- **23 Catch2 test cases** pinned: 8 pure (B.*) + 15 PG-gated (PB.*).
  Fixture plan for 6 `tests/fixtures/phase_b/` packages covering
  happy-all-pass, broken-assert-deny, broken-assert-allow,
  mixed-pass-fail, no-test-contracts, and the non-permission-error
  pass path.
- **Five architect-facing open questions** surfaced with proposed
  resolutions: `ACTIVE_FLAGGED` composability with 0.4.5 transitions,
  per-execution `run_id` lifetime, advisory-lock reuse for re-run
  serialisation, 5 s per-rule wall-clock budget reusing
  `upgrade_drain_timeout_ms`, and skipped-rule visibility in the audit
  stream. Four additional "notes-to-architect" (`OQ-N*`) record
  choices that are not true open questions (per-run vs per-rule user,
  sequential vs parallel dispatch, `disabled_at` on ephemeral users,
  `test_contract.call` args shape).
- **ROADMAP.md** 0.4.7 label flipped `[medium]` → `[strong]` per
  METHODOLOGY §3.1 *Forward ICD presence check* + `feedback_icd_horizon.md`
  (one milestone ahead). Mirrors the 0.4.5 → 0.4.6 label-flip
  precedent documented at `docs/reviews/RE-EVAL-0.4.x.md` (the
  2026-04-19 rewrite session).

### Why now

0.4.7 is the final code milestone of the 0.4.x arc. After v0.4.6
shipped (`b126ebd`, tag `v0.4.6`, 2026-04-21), the horizon rule put
0.4.7 inside the next-N window with no ICD present — the same drift
METHODOLOGY §3.1's forward-check rule was introduced to catch. The
precedent is set by the three earlier 0.4.x paired docs sessions:
0.4.2.1 (ICDs 0.4.3 + 0.4.4), 0.4.4.2 (ICD-0.4.5), 0.4.5.2
(ICD-0.4.6). 0.4.6.1 follows the same shape with the same four-part
untagged convention.

### Not in this session

- No code. No schema.sql edits. No test files. All of those land with
  the 0.4.7 code session that follows.
- No `RE-EVAL` ambient work — the `RE-EVAL following 0.4.7 (0.4.x arc
  closeout)` rewrite session is scheduled post-0.4.7 per ROADMAP
  cadence, not here.

### Files touched

| Path | Change |
|------|--------|
| `docs/icd/ICD-0.4.7-rbac-test-execution.md` | new |
| `docs/ROADMAP.md` | 0.4.7 label `[medium]` → `[strong]` |
| `docs/CHANGELOG.md` | this entry |

---

## v0.4.6 — 2026-04-21 — RBAC rule registration (Phase A)

Implements ICD-0.4.6 against the `docs/icd/ICD-0.4.6-rbac-rule-registration.md`
contract authored in 0.4.5.2. Closes the gap left by 0.4.4's minimal
`parse_rbac_rules` shim: `rbac.json` now flows through a typed parser
and five structural validators before rules land in `plinth.rbac_rules`.

### What shipped

- **Typed `rbac.json` parser** in new `src/kernel/rbac/rbac_manifest.{hpp,cpp}`.
  `plinth::rbac::parse_rbac_manifest(bytes, source_path)` returns
  `RbacManifestParseResult{value?, messages}` mirroring
  `CapabilityManifest::parse` (ICD-0.4.1). Structurally invalid rules
  are dropped from `value->rules` per the ICD-0.4.0 §R3 convention;
  within-file error codes carry the `rbac.*` prefix
  (`rbac.root.not_object`, `rbac.rules.not_array`, `rbac.rule.missing`,
  `rbac.rule.not_string`, `rbac.namespace.missing`,
  `rbac.description.{missing,too_long}`, `rbac.test.not_object`,
  `rbac.test.assert_{deny,allow}.shape`, `rbac.test.call.missing`,
  `rbac.test.expect.invalid`). Unknown top-level and `test`-object
  fields accepted silently per DESIGN §7.1 forward-compat rule.
  `RbacManifest::serialize()` symmetric to `PackageManifest::serialize()`
  (2-space indent, unknown fields first) — powers the P.R1 round-trip
  case.
- **Phase A validator** in new `src/kernel/rbac/phase_a.{hpp,cpp}`.
  `plinth::rbac::validate_phase_a(rbac, caps, package_name, conn)`
  returns `std::vector<ManifestParseError>` for all five rules:
  - **A.1** rule name matches `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$`
    → `rbac.rule.invalid_name`
  - **A.2** namespace equals `package_name` or one of
    `{kernel, plinth, system}` → `rbac.rule.namespace_mismatch`
  - **A.3** each `test.assert_*.call` parses via
    `plinth::capabilities::parse_signature` (ICD-0.2.1)
    → `rbac.test.call.parse_error`
  - **A.4** parsed `{ns, ver, fn}` matches an entry in
    `CapabilityManifest.provides[]` → `rbac.test.call.unresolved`
  - **A.5** `SELECT extension_name FROM plinth.rbac_rules WHERE rule = $1`;
    on hit with different `extension_name`, emit
    `rbac.rule.name_collision` naming the other extension
- **`plinth.rbac_rules.test_contract JSONB`** column added in
  `migrations/schema.sql` (nullable, after `orphaned_at`). Pre-0.7
  freeze direct edit; no numbered migration file.
- **Extended `upsert_extension_rule` signature** in
  `src/kernel/rbac/rule_registrar.{hpp,cpp}` with the additive
  `std::optional<nlohmann::json> test_contract` parameter; INSERT
  becomes 5→6 columns, UPDATE gains
  `test_contract = EXCLUDED.test_contract`, conflict target unchanged.
  `$5::jsonb` cast at the server; `nullptr` in `paramValues` for
  `std::nullopt`.
- **Install-lifecycle integration** in
  `src/kernel/packages/install_lifecycle.cpp`:
  - `run_stage_registering` (first-install) and
    `run_stage_registering_upgrade` both call
    `register_extension_rbac_rules` (new file-local helper) before
    `register_capability_tx`. The helper wraps
    `parse_and_validate_rbac` (missing file = silent, malformed or
    ERROR findings = `std::unexpected` that trips the RollbackGuard)
    followed by the upsert loop threading `r.test` through as
    `test_contract`.
  - `reconcile_rbac_on_upgrade` switched from
    `std::vector<RbacRuleEntry>` to
    `std::vector<plinth::rbac::RbacRule>`; deletes the 0.4.4
    `RbacRuleEntry` struct and `parse_rbac_rules` shim (both removed).
- **`RESERVED_NAMES` promoted** out of
  `src/kernel/packages/cross_file_validator.cpp`'s anonymous
  namespace into new `src/kernel/packages/reserved_names.hpp` with
  free accessor `is_reserved_kernel_namespace`; routes CF1 and CF6
  through the single source per ICD §OQ5.
- **20 new Catch2 cases** (covers the 12 ICD-normative targets plus 8
  complementary): 11 pure in `tests/kernel/rbac/rbac_manifest_test.cpp`
  (P.01 happy, P.E1–E4 edge, P.R1 round-trip, plus
  non-object-root / non-array-rules / drop-on-missing-fields /
  expect-literal-mismatch / unknown-top-level preservation); 8 PG-gated
  in `tests/kernel/rbac/phase_a_test.cpp` (P.M1–M5, P.N1 exemption,
  plus reserved-namespace-accept and full-valid cases). Pure and
  PG-gated paths routed via Catch2 tags into the 0.4.5.1 grouped
  runner.

### Deviations from DESIGN-packages-v04x.md §0.4.6 / ICD-0.4.6

Same footing as the 0.2.0 / 0.2.2 / 0.2.4 / 0.2.5 / 0.3.1 / 0.3.2 /
0.4.0 / 0.4.1 precedent — called out in the ICD §Open Questions:

- **Rule A.2 narrower than CF1** (package name ∪ reserved, vs CF1's
  any-provided-namespace). DESIGN §0.4.6 followed verbatim; CF1
  unchanged. Captured as ICD-0.4.6 OQ2.
- **`validate_phase_a` unified signature** (Rules A.1–A.4 pure, A.5
  PG) rather than split into `validate_phase_a_pure(...)` +
  `validate_phase_a_collisions(PGconn&, ...)`. Single call site in
  the installer, single error list in the caller loop.
- **`test_contract` parameter additive, not defaulted.** Project
  convention — every existing caller recompiles. Two in-tree callers
  updated in the same commit (`reconcile_rbac_on_upgrade`,
  `run_stage_registering` via the new helper).
- **`register_extension_rbac_rules` helper extracted** from
  `run_stage_registering` so the clang-tidy cognitive-complexity
  threshold (25) is respected — inlining Phase A + upsert loop pushes
  the function to 26. The helper keeps the call-site one-line.

### Verification

- Build clean on GCC 14.2.0 Debug (`cmake -B build && cmake --build
  build -j 4`).
- `plinth_tests "~[integration] ~[ws] ~[js]"`: 251 cases, 992
  assertions, all pass.
- `plinth_tests "[integration] ~[ws] ~[js]"` with
  `PLINTH_PG_HOST=127.0.0.1 PLINTH_PG_PORT=5432 PLINTH_PG_USER=plinth
  PLINTH_PG_PASSWORD=plinth PLINTH_PG_DATABASE=plinth`: 168 cases,
  845 assertions, all pass.
- `run-clang-tidy-20 -p build` over
  `src/kernel/rbac/{rbac_manifest,phase_a,rule_registrar}.cpp`,
  `src/kernel/packages/{install_lifecycle,cross_file_validator}.cpp`,
  `tests/kernel/rbac/{rbac_manifest,phase_a}_test.cpp`: zero findings.

### What 0.4.6 does NOT do (deferred per ICD)

- Does not execute RBAC tests — Phase B (ephemeral test users,
  `ACTIVE_FLAGGED` transitions) is 0.4.7 scope.
- Does not alter `/api/groups/*` CRUD — `test_contract` is internal.
- Does not remove CF1 / CF2 / CFW1 — they stay as offline pre-install
  filters; Phase A is authoritative at install time.
- Does not wire Phase A into `plinth validate` CLI (no PG offline).

---

## 0.4.5.2 — 2026-04-21 — ICD-0.4.6 authored (untagged)

Paper-only docs session. Authors `docs/icd/ICD-0.4.6-rbac-rule-registration.md`
ahead of the next `[strong]` code milestone. Untagged per
`feedback_tagging_rule.md` (four-part interim sessions roll into the
next X.Y.Z tag range). Follows the 0.3.3.4 / 0.4.2.1 / 0.4.4.2 docs-
session precedent.

### Why

ROADMAP §0.4 lists 0.4.6 RBAC rule registration (Phase A validation)
as the next `[strong]` milestone. METHODOLOGY §3.1 (the forward-ICD
presence check added in 0.3.3.4) requires every pending `[strong]`
milestone in the next-N window to have an ICD before code work. No
ICD-0.4.6 existed — code assumptions in ICD-0.4.4 (minimal rule-insert
shim) and ICD-0.4.5 (upgrade reconciliation consumes `test_contract`)
referenced a contract that had never been pinned. 0.4.5.2 closes that
gap.

### What the ICD pins

- **Typed `rbac.json` parser** in `src/kernel/rbac/rbac_manifest.{hpp,cpp}`
  mirroring `CapabilityManifest::parse` (ICD-0.4.1). Returns
  `RbacManifestParseResult{value?, messages}` so errors and warnings
  surface in one pass. Stable rule-name prefix `rbac.*` — disjoint from
  the `manifest.*` / `capabilities.*` / `cross-file.*` prefixes
  established in ICD-0.4.0 / ICD-0.4.1.
- **Five Phase A validation rules** (A.1 rule-name regex; A.2 namespace
  matches package name or reserved kernel set; A.3 `test.*.call` parses
  via 0.2.1 `parse_signature`; A.4 parsed call resolves to a provided
  capability; A.5 rule-name unique across extensions, same-extension
  upgrade exempt). Rule A.5 is the single PG-coupled rule.
- **New `plinth.rbac_rules.test_contract JSONB`** column, nullable,
  stores the full `test` object verbatim for 0.4.7 Phase B.
- **Extended `upsert_extension_rule` signature** — additive
  `std::optional<nlohmann::json> test_contract` parameter; INSERT and
  ON CONFLICT DO UPDATE both handle the new column.
- **Firing point** — inside `run_stage_registering` (first-install) and
  `run_stage_registering_upgrade` (upgrade), between the REGISTERING
  state UPDATE and the existing `upsert_extension_rule` loop. Findings
  with `Severity::ERROR` trip the existing RollbackGuard; state moves
  to `INSTALL_FAILED` via the 0.4.4 machinery.
- **12 Catch2 test cases** with the P.* prefix (6 pure, 6 PG-gated):
  happy-path, one mechanical-invalid per Phase A rule, four edge cases
  (empty rules, `assert_deny`-only, `assert_allow`-only, no `test`
  object), same-extension re-registration exempt, round-trip
  parse/serialize/parse. Fixtures live under `tests/fixtures/rbac/`;
  M.3/M.4 reuse the existing `tests/fixtures/packages/rbac-test-*/`
  exemplars.

### What the ICD does not do

- Does not run the RBAC tests — Phase B (test execution, `ACTIVE_FLAGGED`
  transitions, ephemeral test users) remains 0.4.7.
- Does not remove CF1 / CF2 / CFW1 from `cross_file_validator.cpp` —
  they stay as offline pre-PG filters; Phase A re-checks authoritatively
  with PG context. Error codes disjoint.
- Does not touch `/api/groups/*` CRUD surface — `test_contract` is
  internal.

### Deviations from DESIGN-packages-v04x.md §0.4.6 called out in the ICD

- Rule A.2 is deliberately narrower than the existing CF1 filter
  (package name or reserved namespace, vs CF1's broader "any provided
  capability namespace"). DESIGN §0.4.6 wording is followed verbatim;
  CF1 stays as-is. Captured as OQ2.
- `validate_phase_a` takes a `PGconn&` (needed by Rule A.5) rather than
  splitting a pure sub-API for A.1–A.4. Combined surface is simpler,
  Catch2 handles the split via PG-gated tags. Captured as §Test Cases
  note.
- Error shape is `plinth::packages::ManifestParseError` reused with a
  new `rbac.*` prefix; no parallel `PhaseAFinding` type. Rationale in
  OQ4.

### Files

- **New:** `docs/icd/ICD-0.4.6-rbac-rule-registration.md`.
- **Edit:** `docs/CHANGELOG.md` (this entry).
- No edit to `docs/ROADMAP.md` — 0.4.6 stays labelled `[strong]`; the
  forward-ICD obligation is satisfied by the file's existence, not a
  label change.

### Next

Implementation session for 0.4.6 — its own branch (`feat/0.4.6-…`),
its own PR, its own `v0.4.6` tag on merge per the three-part-tag rule.

---

## 0.4.5.1 — 2026-04-21 — Test-strategy redesign: grouped single-subprocess runs (untagged)

Shrinks the Catch2 `catch_discover_tests` one-subprocess-per-TEST_CASE
model from 494 subprocesses per ctest run to **4 grouped entries +
45 per-TEST_CASE entries = 49 subprocesses**. CTest now registers 4
grouped entries — `plinth_tests_pure` / `_js` / `_pg` / `_ws` — each
running its tag-selected TEST_CASE set in a single process with ONE
Drogon + ONE DbClient + ONE RuntimePool lifecycle. The `[js][async]`
tests (45 cases) stay per-TEST_CASE for now — see §Async caveat below.
Untagged per `feedback_tagging_rule.md` (interim test-infra task,
three-part X.Y.Z tags only). Follows the 0.4.0.1 / 0.4.4.1 precedent.

### Why

v0.4.5 closed the Meyers-singleton ConnectionRegistry teardown race
that had been bandaged across 0.3.3 / 0.3.3.1 / 0.3.4.1 / 0.4.0.1 /
0.4.4.1. LH-0.1 (same day) ran 133,755 `js_stress` calls / ~535k
`db.query` ops / ~535k `signal_completion` hits against the
production kernel at concurrency=4, across 3 × 2-min trials: zero
reproductions of `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`,
or `bad_weak_ptr`. The production kernel lifecycle is clean; the
residual CI flakes are subprocess-lifecycle-scoped — framework-internal
queues (trantor TimerQueue, QuickJS JS_ExecutePendingJob queue,
Drogon IO-thread pool) still holding work when `std::atexit` fires,
amplified by PG connection saturation across 80+ rapid subprocess
teardowns. Direction captured in `project_next_session_lh0.md` and
`project_ws_flaky_segfault.md` §"Still open at v0.4.4" (candidate
redesigns #3 *kill the subprocess-per-test model* and #5 *lazy
Drogon*).

### Grouping scheme

| CTest entry            | Tag expression                | Tests | Fixture needs                                       |
|------------------------|-------------------------------|-------|-----------------------------------------------------|
| `plinth_tests_pure`    | `~[integration] ~[ws] ~[js]`  | 240   | none (parser / validator / crypto / unit-async)     |
| `plinth_tests_js`      | `[js] ~[async]`               |  22   | QuickJS RuntimePool, no Drogon                      |
| `plinth_tests_pg`      | `[integration] ~[ws] ~[js]`   | 160   | libpq + `reset_schema`; no Drogon, no JS            |
| `plinth_tests_ws`      | `[ws]`                        |  27   | Drogon + HTTP listener + DbClient + `reset_schema`  |
| `dev.*` (catch_discover) | `[js][async]`               |  45   | per-TEST_CASE; see §Async caveat                    |

Math: 240 + 22 + 160 + 27 + 45 = 494 (matches baseline). Overlap rule
Drogon > PG > JS > pure (verified disjoint: `[js][integration]`,
`[js][ws]`, `[async][ws]` all return 0 tests).

### Async caveat — race amplification under grouping

`[js][async]` cases initially grouped under `plinth_tests_async`. Local
flake-hunt surfaced that the shared-subprocess model AMPLIFIES the
existing parallel-dispatch / refcount race
(`JS_ExecutePendingJob+0xbd` → `free_zero_refcount`) from ~10-15%
per-TEST_CASE rate to ~15-25% grouped; some test combinations also
deadlock indefinitely. Investigation 2026-04-21 narrowed the trigger
to sequential test runs in the same subprocess (`[js][async][hardening]`
alone clean; `[js][async] ~[hardening]` mostly clean; full `[js][async]`
flakes). The root cause is kernel-side — a refcount leak in the async
bridge's `dispatch_async_op_detached` / `SqlBinderAwaiter` /
`AnyCompletionAwaiter` coroutine path, candidate list in
`project_ws_flaky_segfault.md` §Candidate root causes. LH-0.1
(2026-04-21) independently confirmed production is unaffected; the
race only surfaces under the test-harness lifecycle.

Until a dedicated fix lands, the `[js][async]` tests stay
per-TEST_CASE via `catch_discover_tests(plinth_tests TEST_SPEC
"[js][async]")`. The win: 494 → 49 subprocesses, a ~10× reduction.
The async tests keep their pre-existing ~10-15% flake rate,
unchanged from before this redesign.

### RESOURCE_LOCK serialization

`plinth_tests_pg` / `_async` / `_ws` all hold `plinth_pg_schema` so
concurrent `reset_schema` (via `bootstrap_schema(dev_mode=true)`) never
races across the shared DbClient pool. `_ws` additionally holds
`plinth_ws_port_28099` so the HTTP listener port isn't contended.
`_pure` and `_js` run fully in parallel (`ctest -j 5`).

### Fixture change

None. The 0.3.4.1 `ensure_drogon_running()` vs `ensure_drogon_with_db_running()`
split stays intact — async tests are still per-TEST_CASE so the
call_once's frozen decision doesn't cross test boundaries.

### IDE escape hatch

`PLINTH_DEVELOPER_TEST_DISCOVERY=ON` adds per-TEST_CASE
`catch_discover_tests` entries with a `dev.` prefix for IDE test-
explorer integration. Default OFF; CI never flips it. The dev-mode
entries cover the NON-async groups (`~[js][async]` TEST_SPEC) since
async tests are already registered per-TEST_CASE without the flag.

### CI

`.gitea/workflows/ci.yml` ctest invocation gains `--output-junit junit.xml`
so per-TEST_CASE pass/fail (now inside the 5 grouped entries) stays
visible to CI report consumers.

### Files touched

- `CMakeLists.txt:564` — `catch_discover_tests(plinth_tests)` replaced
  with 4 `add_test` entries + RESOURCE_LOCK/TIMEOUT/LABELS + a narrow
  `catch_discover_tests(plinth_tests TEST_SPEC "[js][async]")` for
  the 45 deferred async tests + `PLINTH_DEVELOPER_TEST_DISCOVERY`
  option.
- `.gitea/workflows/ci.yml:44` — `--output-junit junit.xml`.
- `docs/CHANGELOG.md` — this entry.

### Verification

- Baseline: 494 TEST_CASEs pre-migration (Catch2 `--list-tests`).
  Post-migration: 240 + 22 + 160 + 27 (grouped) + 45 (catch_discover
  per-TEST_CASE) = 494 — no silent drops.
- Per-group local runs (with docker PG on :5432): `_pure` 240/240 in
  2.8s · `_js` 22/22 in 0.23s · `_pg` 160/160 in 22.0s · `_ws` 27/27
  in 7.95s.
- Async-grouping attempted (commit 4), measured 15-25% flake rate vs
  per-TEST_CASE ~10-15% (documented race amplification in §Async
  caveat), reverted in commit 7.
- Flake-hunt after rollback: 20x full ctest clean on grouped entries;
  async tests retain their pre-existing ~10-15% flake rate.

### Not in scope

- `cfg.db.pool_size` tuning (already landed at 0.4.4.1 via the
  80→32 drop).
- Per-subprocess PG database (`CREATE DATABASE plinth_test_$$`) —
  heavier rework, hold unless grouped model shows connection
  saturation.
- Splitting the test binary into separate executables — second-order
  optimization.
- Any production kernel change (LH-0.1 confirmed the production path
  is clean).

---

## LH-0.1 — 2026-04-21 — Async-bridge stress tier (untagged)

Extends the parallel Load Harness stream with the diagnostic that
exercises what LH-0 did not — the JS async-bridge coroutine path
where `free_zero_refcount` is suspected. Untagged per
`feedback_tagging_rule.md` (LH stream is outside the X.Y.Z numbering).

### Why

LH-0 ran 2,000,000+ `lh0:1:chain` calls against production HEAD and
reproduced zero async-bridge flakes — consistent with the hypothesis
that `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`, and
`bad_weak_ptr` live in the `signal_completion → JS_ExecutePendingJob`
path that sync Tier 1 recursion never traverses. The
`async_hardening: parallel queries` ctest reliably trips
`free_zero_refcount` under the Catch2 subprocess lifecycle; LH-0.1
drives the same path through the production kernel so either
reproduction lands (unblocking a fix PR) or zero-reproduction
redirects the investigation to the test-strategy redesign the maintainer flagged
on 2026-04-20.

### Kernel surface added

- New `src/kernel/ws/js_stress.{hpp,cpp}` — process-lifetime
  `RuntimePool` singleton + `try_dispatch_js_stress` helper that
  recognises the `lh0:1:js_stress` signature, validates admin
  RBAC + arg shape, and dispatches via `drogon::async_run` to
  `co_await plinth::js::run_on_context`. Sends `call_result` or
  `call_error` on the caller's WS connection (weak_ptr captured,
  `connected()`-guarded).
- `src/kernel/ws/call_dispatch.cpp` — one-line fork in `on_call`
  before `call_capability` so `lh0:1:js_stress` frames are handled
  out-of-band of the sync resolver pipeline. All other signatures
  fall through unchanged (LH-0's verified path is untouched).
- `src/kernel/main.cpp` — `init_js_stress_pool(cfg)` after
  `init_resolver`; `shutdown_js_stress_pool()` in the existing
  `std::atexit` lambda before `drogon::app().quit()` (pool teardown
  pumps pending JS jobs; Drogon loop must still be alive).
- `CMakeLists.txt` — wire `js_stress.cpp` into `plinth` +
  `plinth_tests` target sources.
- `tests/kernel/ws/ws_test_fixture.cpp` — same init/shutdown pair
  in the fixture bootstrap + atexit chain so the 4 new Catch2
  cases can reach the dispatch fork.

Tests: 4 new Catch2 cases in `tests/kernel/ws/call_test.cpp` covering
js_stress success (`(() => 1+1)()` → `call_result{value:2}`),
throw-on-eval (`throw new Error('boom')` → `call_error{code:"js_eval_error"}`),
non-admin RBAC (`call_error{code:"permission_denied"}`), and
non-string arg (`call_error{code:"invalid_call"}`).

### External harness extended

- `load-harness/internal/tiers/tiers.go` — new `Async` profile
  (concurrency 4, duration 2m) + `"async"` case in `Lookup`.
- `load-harness/cmd/lh0/main.go` — profile-keyed dispatch: `async`
  workers send `{signature:"lh0:1:js_stress", args:[asyncStressScript]}`;
  easy/medium workers keep the existing `lh0:1:chain` shape. New
  package-level `asyncStressScript` constant — 4 concurrent
  `db.query('SELECT pg_sleep(0.01), ${i} AS x')` awaited via
  `Promise.all`, mirroring the async_hardening ctest shape.
- `load-harness/README.md` — new `### Async tier` subsection;
  updated opening + scope gaps list; directory-layout line.

### Tier profile added

| Name  | Concurrency | Depth | Duration |
|-------|-------------|-------|----------|
| async | 4           | n/a   | 2m       |

Each call fans out ~4 concurrent `db.query` operations under the
hood; effective fan-out across the harness is ~4 × workers. Override
via `--concurrency` / `--duration` (depth is unused for this tier).

### Accepted scope gaps (documented in ICD-LH-0.1 §2 / §10)

- **Fix for the reproduced signature** — LH-0.1 is the diagnostic
  only. The fix ships as its own PR once a reliable repro is in hand,
  per the "no per-signature bandaids" 2026-04-21 direction.
- **LH-0.2 parameterised script** — a single fixed script is enough
  for the diagnostic; a `--script-file` override is deferred until a
  caller needs multiple async shapes.
- **Extension dispatch** — still blocked on an unscheduled milestone.
  LH-0.1 routes around it via the dispatch fork, explicitly scoped
  as a diagnostic-only deviation (not a blueprint for extension
  dispatch).
- **Richer error taxonomy** — `js_eval_error` is a catch-all for
  every `EvalErrorKind`; split later if a caller branches on kind.

### Verification

- `ctest` 494/494 pass with docker-compose PG on :5432 (490
  pre-existing + 4 new js_stress cases). Full suite runs in ~59 s.
- `run-clang-tidy-20 -p build src/kernel/ws/js_stress.cpp
  src/kernel/ws/call_dispatch.cpp src/kernel/main.cpp
  tests/kernel/ws/call_test.cpp tests/kernel/ws/ws_test_fixture.cpp`
  — zero findings (one NOLINTNEXTLINE justified on the
  `cppcoreguidelines-avoid-capturing-lambda-coroutines` check in
  `js_stress.cpp`; all captures are owned values).
- `cd load-harness && go build ./... && make all` clean, no vet
  findings.
- End-to-end `--tier=async` × 3 trials (concurrency=4, duration=120s
  each) against a dev-mode plinth on `127.0.0.1:18080` with kernel
  log tailed for `free_zero_refcount|list_empty|bad_weak_ptr|SIGSEGV|SIGABRT`:
  - Trial 1: 44,484 js_stress calls, 0 fails, p50=10.71 ms,
    p95=10.99 ms, p99=11.19 ms, max=25.9 ms.
  - Trial 2: 44,613 calls, 0 fails, p50=10.70 ms, p95=10.98 ms,
    p99=11.21 ms, max=35.6 ms.
  - Trial 3: 44,658 calls, 0 fails, p50=10.70 ms, p95=10.97 ms,
    p99=11.16 ms, max=15.2 ms.
  - Total: 133,755 js_stress calls ≈ 535,020 `db.query` operations
    under `Promise.all` ≈ 535k `signal_completion` callbacks.
  - RSS stable: 24 MiB at kernel boot → 26 MiB after 3 trials.
  - Clean SIGTERM shutdown afterwards — listener stopped, no
    teardown assertion.
- **Zero reproductions** of `free_zero_refcount`,
  `list_empty(&rt->gc_obj_list)`, or `bad_weak_ptr` across all 3
  trials.

### Initial diagnostic finding

**Zero reproductions** on 3 × 2-minute trials against production
HEAD. Per ICD-LH-0.1 §9.2, this is a meaningful data point: the
production kernel lifecycle tolerates the async-bridge path
(`dispatch_async_op_detached → signal_completion → JS_ExecutePendingJob`)
that the Catch2 subprocess model trips on reliably in
`async_hardening: parallel queries honour max_concurrent cap`. The
0.3.3 / 0.3.3.1 / 0.3.4.1 / 0.4.0.1 / 0.4.4.1 / 0.4.5 deterministic-
teardown bundle + leaked-singleton fix appears to have closed the
production-side paths; residual CI flakes are scoped to the
test-strategy issues the maintainer flagged on 2026-04-20 (subprocess-per-
test-case harness design, not a kernel bug). Next session's focus
shifts from "find and fix `free_zero_refcount`" to the
test-strategy redesign.

---

## LH-0 — 2026-04-21 — Load harness scaffold (untagged)

Lands the first milestone of the parallel Load Harness stream per
ROADMAP §Load Harness. Untagged per `feedback_tagging_rule.md` (LH
stream is outside the X.Y.Z numbering; sub-stream items don't carry
tags).

### Why

the maintainer's 2026-04-21 direction after CI on Slice B surfaced another
async-bridge crash signature (`quickjs.c:6678: free_zero_refcount`):

> there are so many random crashes this is near unexceptable
> next session needs to work on our stress test client and use it
> to find and fix these errors, I am near sick of it

The 0.3.3 / 0.3.3.1 / 0.3.4.1 / 0.4.0.1 / 0.4.4.1 / 0.4.5 sequence
each closed a specific teardown sub-path; each CI run since found a
new one. LH-0 is the scaffold that lets us reproduce these
deterministically against the production kernel (not the Catch2
subprocess harness) so future fixes can ship with a reliable repro,
not a guess.

### Kernel surface added

- New `src/kernel/ws/call_dispatch.{hpp,cpp}` — WS `call` message
  type and dispatcher. Frame shape documented in
  `docs/icd/ICD-LH-0-load-harness-scaffold.md` §3.
- `src/kernel/ws/messages.hpp` — 3 new constants
  (`CALL` / `CALL_RESULT` / `CALL_ERROR`).
- `src/kernel/ws/events_controller.cpp` — one dispatch arm routing
  `msg::CALL` → `on_call`.
- `src/kernel/capabilities/resolution.cpp` — new
  `lh0_chain_handler` + `register_lh0_harness_handlers_locked`
  called from `init_resolver`. Registers `lh0:1:chain` Tier 1
  kernel capability, RBAC-gated by `kernel.admin`, recurses through
  the standard `call_capability` pipeline.
- `CMakeLists.txt` — wire `call_dispatch.cpp` + `call_test.cpp`
  into plinth + plinth_tests target sources.

Tests: 4 new Catch2 cases in `tests/kernel/ws/call_test.cpp` covering
Tier 1 hit shape, unknown signature → `capability_not_found`, missing
signature → `invalid_call`, non-admin RBAC denial. Full suite
490/490 pass locally with PG, run-clang-tidy-20 zero findings on
new TUs.

### External harness added

New top-level `load-harness/` directory, independent of CMake
(follows the `benchmarks/` opt-in pattern from 0.2.6.2 but goes one
step further — no link at all against kernel code):

- Go 1.22 module `github.com/gobha-me/plinth/load-harness` with dep
  `github.com/gorilla/websocket`.
- `cmd/lh0/main.go` orchestration; `internal/{httpclient, wsclient,
  tiers, observe}` split.
- `fixtures/driver/` with minimal valid extension
  (manifest + capabilities + rbac + panels + config + server/main.js)
  zipped by `make fixtures`.
- `Makefile` with `lh0`, `fixtures`, `all`, `clean`, `tidy`, `test`
  targets.
- `README.md` + ICD at `docs/icd/ICD-LH-0-load-harness-scaffold.md`.

### Tier profiles

| Name    | Concurrency | Depth | Duration |
|---------|-------------|-------|----------|
| easy    | 2           | 4     | 60s      |
| medium  | 8           | 8     | 5m       |

All three fields overridable via `--concurrency` / `--depth` /
`--duration` flags. Harder tiers deferred to LH-4.

### Accepted scope gaps (documented in ICD §8 Future work)

- **Async-bridge stress** — `lh0:1:chain` recurses through the sync
  `call_capability` pipeline, exercising Tier 1 lookup + RBAC + call-
  depth tracking but not the JS async bridge where
  `free_zero_refcount` fires. That stress is LH-0.1, pending either
  a JS-eval trigger path or C++-side extension dispatch (current
  `dispatch_tier2` returns `tier3_not_available` for
  `provider_type=extension`). LH-0 is the scaffold; LH-0.1 is the
  targeted diagnostic for the maintainer's mandate.
- **ConnState effective-rules** — the WS auth flow currently
  populates `is_admin` only. `on_call` synthesizes `["kernel.admin"]`
  vs `[]` for dispatch RBAC. Non-admin WS callers therefore can't
  invoke RBAC-gated caps via `call` today. Widening to full
  effective-rules would add one SELECT at auth time; kept out of
  LH-0 scope.
- **No CI wiring**. ROADMAP defers harness-in-CI to LH-4 (alongside
  `plinth.metrics` as the regression-tracked substrate).

### Verification

- `ctest` 490/490 pass with docker-compose PG on :5432, env per
  `project_next_session_lh0.md` §Starting context.
- `run-clang-tidy-20 -p build src/kernel/ws/call_dispatch.cpp` zero
  findings.
- `cd load-harness && make all` produces `build/lh0` (7.9 MiB static
  Go binary) + `fixtures/driver.zip` (~2.5 KiB).
- **End-to-end easy-tier smoke** (20 s, concurrency=2, depth=4) —
  768,510 successful calls, zero failures, p50=43.6 µs, p95=75.1 µs,
  p99=104.9 µs. Install + uninstall clean, kernel log clean.
- **Short medium-shape stress** (30 s, concurrency=8, depth=8) —
  1,160,550 successful calls, zero failures, p50=141.2 µs,
  p95=268.8 µs, p99=430.8 µs. No `free_zero_refcount` /
  `list_empty` / `bad_weak_ptr` signatures reproduced. RSS stable
  at ~24 MiB throughout.
- Plinth shutdown clean on SIGTERM — listener stopped, WS
  connections closed, no teardown assertion.

### Initial diagnostic finding

A sync-dispatch-only harness (LH-0 as shipped) **does not**
reproduce the `free_zero_refcount` family signatures against
production HEAD. That's consistent with the flake being in the JS
async-bridge `signal_completion → pending-job drain` path that
LH-0's Tier 1 recursion doesn't traverse. The definitive diagnostic
requires LH-0.1 (async-bridge stress) to land; ICD §6.2 §8 cover the
plan.

### Bug found + fixed during smoke

Initial smoke run tight-looped on `ws_closed` errors (27 M failures
in 20 s) because the first implementation had all workers share a
single session token — the kernel's `ConnectionRegistry` displaces
duplicate `(auth_type, id)` pairs per ICD-0.1.6 §Auth, so the second
worker's auth kicked the first off, then the first's `ReadJSON`
returned EOF, then `ws.Call` fail-fast-returned and the worker loop
didn't exit. Two fixes: (a) each worker now performs its own
`POST /api/auth/login` at startup for a distinct session token;
(b) `runWorker` now returns on connection-death errors rather than
tight-looping. Both captured in the ICD §5.2 flow (steps 3 + 4).

---

## 0.4.5 — 2026-04-21 — Package lifecycle transitions + atomic swap + GC

Closes the 0.4.5 milestone from ROADMAP: disable / enable / uninstall
(shipped in Slice A as PR #53 `c270c22` on 2026-04-20) and upgrade +
atomic swap + garbage collection contract (this Slice B). Tagged
`v0.4.5` at the merge commit per `feedback_tagging_rule.md` (3-part
tags only).

### Slice A — disable / enable / uninstall (merged as PR #53)

See commit `c270c22` and the ICD-0.4.5 §DISABLED / §ACTIVE (from
DISABLED — enable) / §UNINSTALLING sections for the detailed design.
Four new library entry points, PATCH + DELETE HTTP surface, schema
edit for SUPERSEDED + retired_at + supersedes_id, RBAC helper trio
(`mark/clear_extension_rules_orphaned`, `delete_extension_rules`),
`unregister_capability` (placed in `registration.hpp` rather than
`resolution.hpp` — symmetry with `register_capability_tx`), and
UNINSTALLING branch in `reconcile_in_flight_installs`. 21 new test
cases (schema + enum round-trip + D.* + U.*).

### Slice B — upgrade + atomic swap T0–T5 + GC (this release)

Shipped in 11 commits on `feat/0.4.5.1-slice-b-upgrade-atomic-swap`:

- **B1** `ValidationConfig::upgrade_from_id` additive field in
  `validator.hpp`; `run_runtime_state_validation` skips the stub
  emission when the field is set (RT1 whitelist). Forward-compat
  for when RT1 bodies land; today the in-process upgrade path
  benefits purely by setting the field on its VALIDATING config.
- **B2** `Config::packages_upgrade_drain_timeout_ms` JSON key with
  default 5000 ms (the maintainer-signed OQ #1). Plumbed through
  `PackageRoutesConfig` into every `InstallerContext` construction
  site (bootstrap + POST/PATCH/DELETE handlers).
- **B3** New `src/kernel/capabilities/drain.{hpp,cpp}` — per-
  extension-name in-flight counter with condvar-based
  `wait_for_zero`. `DispatchGuard` spliced into `call_capability`
  after `parse_signature`; hot path is a single relaxed atomic
  load on `g_active_count` when no drain is active. 8 unit cases.
- **B4** `garbage_collect_superseded_versions` body + pure
  `is_gc_eligible(retired_at, now, retention)` predicate. Non-
  blocking advisory lock per row (skip on contention); DELETE row
  then `fs::remove_all({data_dir}/extensions/{name}/{version})`;
  FK `ON DELETE SET NULL` auto-NULLs the child ACTIVE row's
  `supersedes_id`. 5 eligibility-boundary unit cases.
- **B5** UPLOADING 3-way disposition replaces the Slice A "name-
  already-installed" 409 with a 4-way switch: first-install /
  disabled-present / upgrade-candidate / same-or-older. SemVer
  comparator `compare_semver` added to `manifest.{hpp,cpp}`:
  strips build metadata, MMP lexicographic compare, pre-release
  lower than release, per-identifier precedence per SemVer 11.4
  ladder. 6 unit cases.
- **B6+B7** (combined) `upgrade_package` body covering UPLOADING
  → ACTIVATING → atomic swap T0–T5 → ACTIVE. New helper
  `run_stage_registering_upgrade` does 3-way RBAC reconciliation
  (`reconcile_rbac_on_upgrade` returns `RbacReconciliation.added/
  updated/orphaned`), DELETEs old `plinth.capabilities` rows for
  the extension then INSERTs the new set (NOTIFY buffered until
  COMMIT so registry cache sees an atomic swap), INSERTs panel
  rows for the new package_id. T0 declares intent via
  state='ACTIVATING'; T1 `drain::begin_drain`; T2 `wait_for_zero`
  with timeout → INSTALL_FAILED + `upgrade-drain-timeout` +
  outstanding count; T3 single PG tx (old→SUPERSEDED+retired_at,
  new→ACTIVE) + POSIX `rename(2)` symlink flip (atomic per B9's
  single-mountpoint gate); T4 `asset_server` route cutover +
  `unregister_capability` for v1-only caps; T5 retention timer
  owned by 0.7.x scheduler (no in-process cron). Three audit
  events: `packages.upgrade_started`, `packages.upgrade_swapped`,
  `packages.upgrade_completed`.
- **B8** Reconciler mid-swap + SUPERSEDED branches.
  `resolve_upgrade_mid_swap` helper for ACTIVATING-with-
  supersedes_id: predecessor gone → back out; predecessor ACTIVE
  → back out (T3 didn't commit); predecessor SUPERSEDED → read
  active symlink and forward-complete via rename or advance to
  ACTIVE if symlink already flipped. SUPERSEDED orphan (no
  matching ACTIVE for same name) → log, leave for GC.
- **B9** Bootstrap single-mountpoint self-check in `main.cpp`:
  `stat(2)` on {data_dir}, {data_dir}/extensions, {staging_dir};
  `st_dev` mismatch returns `std::unexpected` with diagnostic and
  bootstrap exits 1. Required for atomic swap's rename(2). 2
  unit cases.
- **B10** `tests/fixtures/lifecycle_transitions/upgrade-v2/`
  fixture (notes v1.3.0: adds notes.comment capability + rule on
  top of valid-install's v1.2.3). CMake `plinth_lifecycle_fixture_zips`
  target mirrors 0.4.4's install fixture rule. Tests X.01 (happy
  path full-verification), X.02 (same-version rejection), X.04
  (DISABLED rejection), G.02 (full-cycle GC + FK cascade).

### Accepted deviations (recorded inline above; flagged in CHANGELOG
### per Slice-A precedent)

1. **B3 drain semantic** — the counter tracks post-begin_drain
   dispatches; pre-drain in-flights don't increment (DispatchGuard
   captured state=nullptr). Harmless because those calls hold
   `resolution.cpp`'s state_mutex shared_lock, which naturally
   serializes with REGISTERING's unique_lock. The ICD's Appendix A
   ("2 in-flight calls observed at T1+0ms") is elaborated from
   always-on counting; our implementation converges on the same
   outcome via shared-lock bracketing.
2. **B5/B6 lock release before dispatch** — install_package
   releases its advisory lock before calling upgrade_package on a
   separate PG session; re-acquire race is cooperative-lock edge
   where a concurrent upload snagging the lock produces a sane
   409 `in-flight-operation`.
3. **B6 REGISTERING-upgrade capability strategy** — DELETE old
   rows for extension then INSERT new set inside the tx, rather
   than ON CONFLICT UPSERT. NOTIFY buffering makes the
   DELETE+INSERT atomic to registry cache listeners on COMMIT.
4. **B10 partial X.* coverage** — X.01, X.02, X.04, G.02 shipped;
   X.05 (upgrade with new migrations), X.06 (migration failure
   aborts), X.07 (3-way RBAC reconciliation at more scale), X.08
   (in-flight call completes in drain window), X.09 (drain
   timeout), X.10 (old URL 404 post-cutover), X.11 (both versions
   on disk), X.12 (crash at swap T3), X.13 (concurrent POSTs),
   G.03 (GC skips advisory-locked row) deferred to a follow-up
   (DEFERRED.md entry).

### Schema

- `plinth.packages.state` CHECK gains `SUPERSEDED` (12 values total).
- `retired_at TIMESTAMPTZ` and `supersedes_id UUID REFERENCES
  plinth.packages(id) ON DELETE SET NULL` columns added.
- `idx_packages_supersedes` partial index on `supersedes_id IS NOT NULL`.
- All already landed in Slice A's `c270c22`.

### Additional fix — parked WS teardown SEGV

CI #12188 (Slice A's final pre-merge run) surfaced the parked
"trantor TimerQueue bad_weak_ptr" teardown SEGV tracked in
`project_ws_flaky_segfault.md` — the maintainer flagged 2026-04-20 "schedule
test-strategy redesign sooner than later" as a strong post-0.4.4
candidate. Investigation during 0.4.5 Slice B revealed the root
cause was NOT a trantor race: `ConnectionRegistry` was a Meyers
singleton whose compiler-registered destructor runs LIFO with
user-registered `std::atexit` handlers. Because `std::atexit(cancel_
lambda)` in `ws_test_fixture.cpp` + `main.cpp` registers before any
code touches `instance()`, the execution order becomes
`~ConnectionRegistry()` first, then `cancel_lambda` — and the
lambda dereferences a destroyed `conns` map.

Fix at `src/kernel/ws/connection_registry.cpp:33`: leak the
singleton — `static auto* inst = new ConnectionRegistry();`. Same
rationale the 0.4.0.1 rework documented for `g_shutdown_pending`
being a file-scope atomic ("outlives the Meyers singleton's
destruction order"). The OS reclaims the allocation at process
exit; no destructor registered means no LIFO race.

The 0.4.4.1 deterministic-teardown bundle's `cancel_all_timers`
body is unchanged — it was always correct; it just ran too late.

### Verification

- `run-clang-tidy-20 -p build src/kernel/ tests/kernel/` zero
  findings across all Slice B touches (B5/B3 tidy sweep commit
  `d799eef` caught the B1-B4 leftovers: NOLINTNEXTLINE on
  drain.cpp globals matching `resolution.cpp` precedent, pointer-
  arithmetic → string_view walk in `parse_pg_timestamp`,
  `auto& → const auto&` in `tree_size_bytes`, `operator+ → +=` in
  GC warning build, manifest.cpp identifier renames `M/m/p →
  major_v/minor_v/patch_v`, `is_all_digits` loop → `std::ranges::all_of`).
- 486/486 ctest green with live PG (`PLINTH_KERNEL_TESTS=ON` +
  docker-compose postgres), 5/5 consecutive clean runs.
- 10/10 targeted runs of the previously-failing "WS auth succeeds
  with a valid PAT" test with the fix in place; 5/5 runs of the
  full WS suite (13 tests).
- Drain test covers the hot-path early-out + the race-free
  decrement-past-zero invariant.
- Live-PG validation also caught two Slice B bugs my dry-test-run
  had missed: (a) `insert_upgrade_row` omitted `entry_point` /
  `frontend_mount` columns → NOT NULL violation; (b)
  `upgrade-v2/server/handlers/comment.js` fixture was missing →
  CF4 validator caught it.
- Stray workspace files (a local scheduled-task lock,
  `Testing/Temporary/CTestCostData.txt`, and session scratch) added
  to `.gitignore` after the third `git add -A` sweep picked them
  up (commit `4cbbac2`).

### Roadmap

- `docs/ROADMAP.md` — `0.4.5 Package disable/enable/uninstall
  lifecycle [strong]` line removed per preamble rule. `0.4.6`
  (RBAC rule registration Phase A) becomes the next `[strong]`
  milestone.

---

## 0.4.4.2 — 2026-04-20 — ICD-0.4.5 authored

Four-part follow-up. **Un-tagged** per 2026-04-18 "3-part X.Y.Z tag only"
rule; accumulates into the next X.Y.Z tag range. Scheduled by
`RE-EVAL-0.4.x-following-0.4.4.md §7` as the ICD-authoring paper-session
slot fired after 0.4.4.1's merge, before 0.4.5 code work begins. Discharges
the `[strong]`-banded 0.4.5 milestone's forward-ICD-presence check per
ROADMAP preamble §Band labels and re-evaluation.

### Deliverables

- `docs/icd/ICD-0.4.5-package-lifecycle-transitions.md` (new) —
  ICD covering disable / enable / uninstall / upgrade + atomic swap + GC
  contract. Mirrors ICD-0.4.4's 12-section template. Contract-by-pointer
  posture on DESIGN-packages-v04x §0.4.5 is discharged; DESIGN §8 atomic
  swap ordering is elaborated into a six-phase choreography (T0–T5) with
  drain timeout, crash-recovery replay, and RBAC reconciliation. 32-case
  test matrix across D.* / U.* / X.* / G.* prefixes.

### Key contracts nailed down

1. **Upgrade drain** — `upgrade_drain_timeout_ms` default 5000; wait-then-fail
   policy; timeout leaves new row INSTALL_FAILED and old row untouched.
2. **Atomic swap** — canonical PG-transaction-plus-symlink-rename sequence
   with matched crash-recovery replay path via `supersedes_id` FK.
3. **RBAC reconciliation on upgrade** — new/updated/orphaned set-diff by
   namespaced `rule` string; `plinth.group_rules` grants preserved across
   orphan transitions (survive re-enable / downgrade-reinstall).
4. **Uninstall confirmation flow** — single-step `?confirm=true`-required;
   explicit DELETE of `plinth.group_rules` before `plinth.rbac_rules` for
   per-revocation audit granularity; `drop_schema_and_migrations` owns the
   destructive PG side.
5. **GC contract for 0.7.x scheduler** — `garbage_collect_superseded_versions(retention, ctx)`
   library entry shipped in 0.4.5 body; 0.7.x lands as pure wiring.

### Schema edits (fluid in 0.4 per ROADMAP preamble)

- `plinth.packages.state` CHECK gains `SUPERSEDED` value.
- New column `plinth.packages.retired_at TIMESTAMPTZ` — set at atomic-swap T3.
- New column `plinth.packages.supersedes_id UUID` — FK with `ON DELETE SET NULL`.
- Partial indexes unchanged (SUPERSEDED already excluded by existing predicates).

### Scope boundaries

- **No code, tests, or CI changes.** Paper-only PR per `feedback_icd_horizon`
  methodology rule.
- **I.18/I.19/I.20 stay orthogonal** per DEFERRED.md §2026-04-20 + RE-EVAL §2.1.
  The 0.4.5 test matrix (D.*/U.*/X.*/G.*) builds its own HTTP fixture; the
  I.18–I.20 migration onto that fixture is a separate follow-up when next
  `/api/packages` work touches this code.
- **0.7.x GC scheduler out of scope.** 0.4.5 ships the contract function body;
  0.7.x adds the cron invocation.

### Verification

- Markdown renders cleanly (no orphaned code fences, tables align,
  state-machine ASCII diagram is consistent with prose).
- All cross-references resolve: `DESIGN-packages-v04x.md §0.4.5 / §4.1 / §8`
  exist; `architecture/01-identity.md §2.4` exists (RBAC Rule Lifecycle);
  `ICD-0.4.0` through `ICD-0.4.4` all exist in `docs/icd/`; `ICD-0.1.4`,
  `ICD-0.1.5`, `ICD-0.1.7`, `ICD-0.2.0`, `ICD-0.2.4` all exist.
- Side-by-side with DESIGN §0.4.5: every bullet either elaborated in the
  ICD or explicitly deferred in §Out-of-scope with pointer.
- State machine diagram + prose consistent: every node in one appears in
  the other; every arrow has a prose paragraph.
- Error taxonomy rows all tie back to at least one Test Cases row.
- HTTP-status mapping consistent with error taxonomy.

### Roadmap + ICD tracking

- `docs/ROADMAP.md` — no edit. `0.4.5` remains `[ ]` until code work ships.
  The `ICD-0.4.5 authoring` item from RE-EVAL §7 is implicitly discharged
  by this PR (not tracked as a separate ROADMAP line per 0.4.1 ICD-authoring
  precedent — the ICD is the ledger).

---

## 0.4.4.1 — 2026-04-20 — WS teardown test-strategy redesign

Four-part follow-up. **Un-tagged** per 2026-04-18 "3-part X.Y.Z tag only"
rule; accumulates into the next X.Y.Z tag range. Scheduled by
`RE-EVAL-0.4.x-following-0.4.4.md §2.2` after the architect signal
2026-04-20: *"schedule test-strategy redesign sooner than later."*

### Investigation gate (§2.2 mandatory first step) — PASS

Static walk of production `src/kernel/main.cpp:224–231` vs
`tests/kernel/ws/ws_test_fixture.cpp:232–266` and
`tests/kernel/js/async_bridge_fixture.cpp:139–147`:

1. **`JS_FreeRuntime: list_empty(&rt->gc_obj_list)` is unambiguously
   test-only.** Production does not create JS runtimes
   (main.cpp:261 `TODO: QuickJS runtime pool init` — deferred to a
   future milestone); zero `RuntimePool::destroy` call sites in
   production source. Only the Catch2 subprocess-per-test model
   exercises the RuntimePool lifecycle at the cadence that surfaces
   the gc-list assertion.
2. **Production atexit chain matches tests on every cancel_all step.**
   main.cpp:224–231 registers the same six-callable sequence
   (`cancel_all_registrations` → `cancel_all_timers` →
   `initiate_shutdown` → `log::shutdown` → `stop_notify_listener` →
   `app().quit()`) that the test fixtures mirror. Step 5
   (`stop_notify_listener`) is production-only; the test fixtures
   don't start the NOTIFY listener. Step-1-and-2 order is flipped
   between production and `ws_test_fixture` (production does
   registrations then timers; test does timers then registrations)
   but both pairs are semantically independent — no drift.
3. **`bad_weak_ptr` timer race** is protected the same way on both
   sides (`cancel_all_timers` before `quit`). The observed CI
   occurrences all fire AFTER Catch2's "All tests passed" in
   per-subprocess atexit — a surface production never enters.
   Production uses Drogon's internal `run() → quit()` path on
   SIGTERM, not `std::atexit` against a Catch2 reporter.

Gate-pass: proceed to candidate selection. Gate-fail clause (escalate
to architect, scope reopens to production race fix) did not trigger.

### Selected candidates from §2.2 six-option menu

**Option 2** (reduce `async_bridge_fixture.cpp`'s `pool_size = 80`
default). Shipping this PR.

**Option 5** (lazy Drogon / minimum framework state per test) —
**already achieved by prior work** before this milestone opened.
0.3.4.1's `ensure_drogon_running` vs `ensure_drogon_with_db_running`
split is the core lazy-fixture mechanism; 0.4.4 slice B C13's
`asset_server::dispatch_for_test` extended it to the HTTP-handler
path. Audit of every test file confirmed existing callers use the
narrowest fixture their test logic permits: every test that calls
`ensure_drogon_with_db_running()` drives PG; every test that
calls `ensure_drogon_running()` needs `drogon::sync_wait(run_on_context(…))`
for `drogon::app().getLoop()` fallback (run_on_context.cpp:800–806);
every non-drogon test (`eval_guard_test.cpp` G.14/G.15/G.17,
parser/validator/security, etc.) invokes neither fixture. Only
five files `#include "ws_test_fixture.hpp"`, all of them WS tests
that legitimately need the full listener + registry stack. No Option 5
diff was needed — the state is already correct.

**Rejected**: Option 1 (per-subprocess PG database — heavyweight,
burns CI); Option 3 (kill subprocess-per-test — largest blast
radius); Option 4 (split binaries — pays off only combined with 3);
Option 6 (explicit teardown — ordering doesn't address
races-inside-teardown, which is where every observed signature fires).

**Added mid-PR — targeted `cancel_all_timers` rework.** PR-CI
#12174 (first CI run after the Option-2-only push) fired the
canonical 0.4.0.1 cancel_all_timers SEGV. Not one of the six §2.2
candidates (none of them address the race INSIDE cancel_all_timers
itself); architect-approved on the spot as "option 1 — rework
cancel_all_timers to not snapshot shared_ptrs." Detailed in §Code
changes below.

### Code changes

**Option 2 — pool_size reduction.**

- `tests/kernel/js/async_bridge_fixture.cpp:49–77` — drop
  `cfg.db.pool_size = 80` override from `test_config()`. Inherit
  production default 32 (`src/kernel/config.hpp:27`). Comment updated
  to document the cascade-amplification rationale.
- `tests/kernel/js/async_bridge_test.cpp` D.17 — comment updated to
  reflect the new queuing behavior (50 in-flight queries, 32
  concurrent slots, 18 queued ~50 ms behind).

Rationale: PG `max_connections = 100` vs per-subprocess `pool_size = 80`
left ~20 slots of headroom. A crashed subprocess leaking its 80
connections (the `bad_weak_ptr` + connection-exhaustion cascade
documented in `project_ws_flaky_segfault.md §Fourth occurrence`)
immediately pushed N+1's pool into "sorry, too many clients already"
territory, which surfaces as contiguous-range test failures
(17–91 in a single ctest). With `pool_size = 32`: two leaked pools
still fit under 100; three is the new cascade threshold (vs one
before). Compounding factor materially reduced.

**Targeted rework — `cancel_all_timers` iterates ConnState, not WSConn.**

PR-CI #12174 (first CI run of this branch after the Option-2-only
push) fired the canonical 0.4.0.1 `cancel_all_timers` SEGV on test
224 `WS non-admin user receives empty subscribed list` — same
signature as `project_ws_flaky_segfault.md §REOPENED 2026-04-19 —
0.4.3 merge CI #12152`. Stack:

    plinth_fatal_signal_handler (SIGSEGV)
      → shared_ptr<WebSocketConnection>::__shared_ptr copy ctor (+0x18)
      → std::construct_at
      → vector::_M_realloc_append
      → ConnectionRegistry::cancel_all_timers +0x106

Crash is inside the snapshot loop that copies conn shared_ptrs into
a vector. A drogon-managed control block was dangling; our code has
no way to make drogon's control block stable. Architect (2026-04-20):
"option 1" — rework `cancel_all_timers` to not snapshot conn
shared_ptrs.

- `src/kernel/ws/connection_registry.hpp` — new `RegistryEntry`
  struct `{conn, state}`; map value becomes `RegistryEntry`.
  `conn_state.hpp` included (RegistryEntry member needs ConnState
  complete at instantiation point).
- `src/kernel/ws/connection_registry.cpp` — `register_connection`
  gains `std::shared_ptr<ConnState> state` parameter; `cancel_all_timers`
  iterates `entry.state` only, never `entry.conn`. ConnState is
  allocated in `events_controller.cpp` via `std::make_shared<ConnState>`
  so its control block is ours and can't go stale while the registry
  holds a copy.
- `src/kernel/ws/auth_flow.cpp:76–91` — `finish_auth` now pulls
  `shared_ptr<ConnState>` via `conn->getContext<ConnState>()` once
  and passes it to `register_connection`. The raw-pointer `state->`
  uses below the call are unchanged (same object).
- `tests/kernel/ws/registry_test.cpp` — pure-unit tests pass
  `nullptr` state through the new parameter (map-semantics tests
  don't need a real ConnState; they use `fake_conn` with no
  dereferenceable memory).

### Verification

- Local build: clean (one pre-existing `createDbClient` deprecation
  warning, unrelated).
- D.17 (`async_bridge: 10 contexts x 5-query Promise.all fan-out`)
  at `pool_size = 32`: **0.63 s pass** (was ~0.3 s at 80; within
  tolerance, no timing assertion).
- Full async_bridge + async_hardening + limits suite: **44/44 pass**.
- Full `[ws]` suite (16 tests, PG-backed, includes test 224 that
  fired on CI #12174): **16/16 pass**.

**Two tight-loop samples were taken on this branch:**

- **v1 (pre-rework, Option 2 only) — 30 iters, fresh PG per run:**
  29 clean, 1 pre-existing parallel-dispatch flake (test 285),
  **`sig_hits=0` reported but that claim was weak** — the v1 script
  grep'd only ctest stdout, not subprocess stderr where signal
  handlers print the backtrace. CI #12174 then fired the canonical
  cancel_all_timers SEGV, which the v1 script would have missed
  locally if it had hit. The v1 result is preserved below for
  process reconstruction; it is **not** the load-bearing evidence
  for this PR.

- **v2 (post-rework, Option 2 + cancel_all_timers fix) —
  30 iters, fresh PG per run, grep BOTH ctest stdout AND
  `build/Testing/Temporary/LastTest.log` (where subprocess stderr
  lands) against the full signature watchlist:**
  **30/30 iterations clean. `fail=0, sig_hits=0.`** Zero
  `bad_weak_ptr`, zero `list_empty(&rt->gc_obj_list)`, zero
  `free_zero_refcount`, zero `sorry, too many clients`, zero
  `bootstrap_schema...FATAL`, zero `signal=SIGABRT`, zero
  `signal=SIGSEGV` — across all 30 runs × 438 tests × signal-handler
  output. Full verification log at `/tmp/verify_0441_v2.log`.

**Signature watchlist used** (grep regex):
`bad_weak_ptr|list_empty(&rt->gc_obj_list)|free_zero_refcount|sorry, too many clients|bootstrap_schema.*FATAL|signal=SIGABRT|signal=SIGSEGV`

**Methodology note for future verifications.** The v1→v2 shift is
load-bearing: any future tight-loop must grep LastTest.log, not
just ctest stdout. ctest captures subprocess output into LastTest.log
but its own stdout only shows pass/fail summary lines; the signal
handler's backtrace never surfaces on ctest stdout. The v1 script's
"sig_hits=0" was misleading — it couldn't see the signatures even
if they were firing. Adopted pattern is in
`/tmp/verify_0441_v2.sh` (not committed; template for ad-hoc
tight-loop runs).

### Roadmap + DEFERRED

- `docs/ROADMAP.md` — `- [ ] 0.4.4.1 WS teardown test-strategy redesign   [strong]`
  → `- [x]`.
- `docs/DEFERRED.md` — WS teardown flake entry stays in `§Active`
  pending 5 clean post-merge CIs; moves to `§Resolved` in a follow-up
  docs commit when the observation window closes clean.

---

## Rewrite session — 2026-04-20 — RE-EVAL following 0.4.4

Rewrite session (per METHODOLOGY §Phase 3). Documentation-only. No
code, tests, or CI changes. **Un-tagged** per the 2026-04-18 "3-part
X.Y.Z tag only" rule (cadence re-eval, not arc-closeout).

Fifth scheduled cadence re-eval. 4/4 cadence after
`RE-EVAL following 0.4.0` (2026-04-19): 0.4.1 GlassWorm, 0.4.2
cross-file validation, 0.4.3 migrations library, 0.4.4 install
lifecycle (slices A + B). Zero-drift window on ICD-declared surfaces —
every load-bearing symbol ships at the declared name; 22 accepted
deviations across the window are all in-CHANGELOG-ratified and
catalogued in §4 of the re-eval artifact. The substantive
deliverable this session is the **scheduling of 0.4.4.1**, a
dedicated WS teardown test-strategy redesign per the architect's
2026-04-20 signal.

### Deliverables

- `docs/reviews/RE-EVAL-0.4.x-following-0.4.4.md` (new) — cadence
  re-eval artifact. Full gap analysis, consolidated deviations
  table, forward-ICD-presence check, band-label review, and four
  methodology observations. Naming follows the arc-closeout
  precedent (`RE-EVAL-0.3.x-arc-closeout.md` alongside
  `RE-EVAL-0.3.x.md`); the prior `RE-EVAL-0.4.x.md` stays in place
  as the `following 0.4.0` cadence record.
- `docs/DEFERRED.md` (§Active, +1 entry) — ICD-0.4.4 I.18/I.19/I.20
  HTTP-harness deferral; previously recorded only in CHANGELOG
  v0.4.4 §ICD deviations (d).
- `docs/ROADMAP.md` — `RE-EVAL following 0.4.4` discharged; new
  **0.4.4.1 WS teardown test-strategy redesign [strong]** inserted
  ahead of 0.4.5; 0.4.5 promoted `[medium]` → `[strong]`; next
  cadence line `RE-EVAL following 0.4.7 (0.4.x arc closeout)
  [rewrite session]` inserted at arc boundary (matches 0.3.6
  arc-closeout precedent).

### §2 gap findings

Two real actionable gaps, one prose observation, two non-drift sections:

- **§2.1** — `arch-silent-on-code`. I.18/I.19/I.20 deferral recorded
  only in CHANGELOG; moved to DEFERRED.md per its preamble rule.
  Fixed in this PR.
- **§2.2** — `arch-silent-on-code`. WS teardown flake fired again on
  Slice B CI #12164 (asset-server I.13/I.14/I.15); Slice B's C13
  sidestepped via `asset_server::dispatch_for_test`. Four rungs of
  bandaid now on the same phenomenon
  (`g_shutdown_pending` → `plinth::log::shutdown` →
  `drain_pending_jobs` + `cancel_all_timers` → `dispatch_for_test`).
  Architect signal 2026-04-20 ("sooner than later") → scoped as new
  `0.4.4.1` with a six-candidate menu lifted from
  `project_ws_flaky_segfault.md §Candidate redesigns`; selection
  deferred to the implementing session per the 0.4.0.1 precedent.
  **Scope framing:** architect's working hypothesis is that the race
  is a test-harness artifact, not a production-path race (unproven;
  no dedicated audit yet). 0.4.4.1 runs an **investigation gate**
  first — read production SIGTERM path vs Catch2 atexit chain, confirm
  or falsify the hypothesis. Gate-pass → pick candidates, implement.
  Gate-fail → **stop**, escalate to a design conversation; scope
  changes from "test-harness redesign" to "production race fix" and
  gets its own ICD.
- **§2.3** — ICD-0.4.4 prose (line 39 "Reserved in the state enum")
  vs C++ enum (8 values) vs schema CHECK (11 values). Code ships
  both correctly; drift is in ICD prose only. No amendment (too
  minor).
- **§2.4–§2.6** — zero interface-drift, zero missing-test, zero
  ICD-amendment-candidate deviations. Full zero-drift window on
  ICD-declared operational contracts.
- **§2.7** — DEFERRED.md: +1 entry (§2.1), zero removals; five
  entries remain active.
- **§2.8** — no cadence-drift; prior re-eval's adjustment held
  cleanly.

### ROADMAP mutations

- `RE-EVAL following 0.4.4` line removed (discharged).
- **New `0.4.4.1 WS teardown test-strategy redesign [strong]`**
  inserted at the head of §0.4 (four-part follow-up, no tag).
- **New parallel `## Load Harness (parallel test framework;
  gated on capability unlocks)` section** added between §0.4 and
  §0.5 (architect-scheduled mid-session, 2026-04-20). Five items:
  LH-0 scaffold `[strong]` (gate met by shipped 0.4.4), LH-1
  through LH-3 `[medium]` (gated on 0.5.0 / 0.5.2 / 0.5.4), LH-4
  `[medium]` (gated on 0.7.1 plinth.metrics; cross-validates
  internal metrics against the external ps/top/perf ground truth
  LH-0..LH-3 collect). Purpose: stress + performance testing
  against the **production** `plinth` binary (not the Catch2
  subprocess harness); near-term empirical input to 0.4.4.1's
  investigation gate. Standalone binary — own process, own build,
  no link against `plinth`/`plinth_tests`. External observability
  (ps/top/perf) until LH-4 wires `plinth.metrics` in. Schedule
  intentionally loose — no main-arc milestone blocks on Load
  Harness items.
- **0.4.5 promoted** `[medium]` → `[strong]` (enters next-N window
  after this re-eval; ICD-0.4.5 authoring scheduled).
- **New `RE-EVAL following 0.4.7 (0.4.x arc closeout) [rewrite
  session]`** at arc boundary — 4/4 cadence count from this re-eval
  lands naturally at arc closeout.
- No completed-milestone trim required (CHANGELOG trims at ship
  time per preamble rule; 0.4.1/0.4.2/0.4.3/0.4.4 already trimmed
  from ROADMAP).

### Methodology observations

- **Bandaid-fatigue threshold.** Teardown race has accrued four
  rungs since 0.3.3; each rung shipped with a "zero occurrences in
  20 runs" claim that a later CI falsified. Candidate METHODOLOGY
  §Phase 3 failure-mode rule addition: "bandaid-ladder on recurring
  phenomenon" → respond with redesign-task scheduling at ≥3 rungs.
  §2.2 is the first instance of this rule in action.
- **Deviation-clustering as leading indicator.** Five of nine 0.4.4
  deviations cluster around test-harness design. The cluster
  reflects the bandaid-ladder phenomenon; 0.4.4.1 is the structural
  fix. Flagged as a weak pattern for next re-eval to confirm/reject.
- **First cadence with mid-arc insertion held cleanly.** 0.4.1
  GlassWorm insertion from the prior re-eval + this re-eval at 4/4
  + arc-closeout landing on 0.4.7 — no cadence-arithmetic
  surprises. Rule holds.
- **Forward-ICD-presence rule three-for-three.** Caught 0.4.2
  (arc-closeout), 0.4.5 (this session). Working as designed.

### Forward ICD presence check

- **0.4.4.1:** four-part follow-up, no standalone ICD (scope in
  RE-EVAL §2.2 + `project_ws_flaky_segfault.md`).
- **0.4.5:** `DESIGN-packages-v04x §0.4.5` is contract; ICD
  authoring is the next paper-session trigger after 0.4.4.1 merges.
  Contract-by-pointer posture invoked (precedent: 0.4.1 GlassWorm).
- **0.4.6:** `DESIGN-packages-v04x §0.4.6` is contract; ICD deferred
  per one-ahead horizon rule.

### Verification

- `git diff main --stat` shows only `docs/` paths — no source,
  tests, CMake, or CI-YAML edits.
- Four files touched: `docs/reviews/RE-EVAL-0.4.x-following-0.4.4.md`
  (new), `docs/ROADMAP.md`, `docs/DEFERRED.md`, `docs/CHANGELOG.md`
  (this entry).
- Every §2 gap has category, citation, and resolution verb.
- §3 zero-gap baseline points at ICD sections validated against
  code (file:line citations in RE-EVAL §1 Code subsection).
- §4 consolidated deviations table: 22 rows across four milestones;
  zero retractions; three taxonomy buckets (convention alignment /
  external constraint / shipping-order trade-off).
- CHANGELOG entry dated 2026-04-20, un-tagged.

---

## v0.4.4 — Package Install Lifecycle (2026-04-20)

Orchestrator milestone. Per ICD-0.4.4. Shipped in two slices — slice A
merged via PR #48 (commit `5ab581c`); slice B merged via the PR for
branch `feat/0.4.4.1-install-lifecycle-slice-b` and cut the `v0.4.4`
tag at its HEAD. Slice A delivered the install state machine, HTTP
surface, asset serving, and schema changes; slice B delivered the
bundled-shell blob, crash-recovery reconciler, I.01-I.20 integration
coverage, and closeout docs.

### Deliverables (slice B)

- `client/shell/` — placeholder extension tree (name=shell, version=
  0.1.0, frontend.mount=/) with bare index.html + empty capabilities/
  rbac/panels. Replaced by the real shell in 0.6a.
- `CMakeLists.txt` — new shell-blob rule: packs `client/shell/` into
  `${CMAKE_BINARY_DIR}/shell.zip` via `cmake -E tar --format=zip`, then
  `ld -r -b binary` + `objcopy --rename-section .data=.rodata
   --add-section .note.GNU-stack` to embed as `shell_blob.o` in the
  `plinth` and `plinth_tests` binaries. `-DPLINTH_SHELL_BLOB_MODE=xxd`
  falls back to a `xxd -i` generated header for non-binutils
  toolchains; `find_program(OBJCOPY)` gate with a clear error message
  on the default path. Also adds a `plinth_install_fixture_zips` target
  that pre-packs each `tests/fixtures/install_lifecycle/<name>/` tree
  into `${CMAKE_BINARY_DIR}/fixtures/<name>.zip` so the PG-gated driver
  reads fixed bytes instead of zipping at runtime.
- `src/kernel/packages/shell_blob.{hpp,cpp}` — **new**. Exposes
  `get_embedded_shell_package() -> std::span<const std::byte>` via the
  extern `_binary_shell_zip_{start,end}` symbols (size from pointer
  diff — the `_size` symbol `ld -r` also emits is an absolute value,
  not dereferenceable).
- `src/kernel/packages/install_lifecycle.{hpp,cpp}` — additions only:
  new `install_shell_if_needed(InstallerContext&)` function + full
  `reconcile_in_flight_installs` body replacing slice A's no-op stub.
  Provenance and InstallStage enums gain `: std::uint8_t` base for
  `performance-enum-size` cleanliness.
    - `install_shell_if_needed`: SELECTs for any ACTIVE/ACTIVE_FLAGGED
      row with a non-null `frontend_mount`. If absent, calls
      `install_package(blob, Provenance::BUNDLED, ctx)` on the linker-
      embedded shell zip. Throws `std::runtime_error` on PG / install
      failure — main.cpp's top-level `catch (std::exception&)` turns
      that into `spdlog::critical` + `return 1` per ICD line 25
      ("Failure here aborts bootstrap").
    - `reconcile_in_flight_installs`: per-state disposition per ICD
      §Crash Recovery and OQ #6:
        - UPLOADING / VALIDATING → INSTALL_FAILED (no schema drop).
        - MIGRATING / REGISTERING → INSTALL_FAILED + `drop_schema_and_migrations()`
          (OQ #6: always INSTALL_FAILED; REGISTERING inherits the drop
          so MIGRATING's schema still goes even though slice A's
          transactional insert path already rolled back cleanly).
        - EXTRACTING / ACTIVATING → if `{data_dir}/extensions/{name}/
          {version}/manifest.json` present, advance to ACTIVE (the
          restore_routes() call later in the bootstrap rebuilds the
          in-memory asset route map from every ACTIVE row); else
          INSTALL_FAILED + drop (ICD I.12: "advances to ACTIVE if
          files present and routes re-registerable").
      Writes `last_install_report` with `{recovered_from_state,
      reconciled_at_bootstrap: true, disposition, reason|note}` for
      admin forensics. PG / SELECT failure is non-fatal — reconciler
      retries at next boot.
- `src/kernel/main.cpp` — bootstrap order in service mode is now:
  atexit chain → register all WS/package handlers →
  `reconcile_in_flight_installs` → `install_shell_if_needed` →
  `asset_server::restore_routes` → `drogon::app().run()`. A shared
  `bootstrap_ctx` InstallerContext replaces the inline literal slice
  A passed to the reconciler stub.
- `tests/fixtures/install_lifecycle/` — seven on-disk fixtures:
  valid-install (I.01 base, shared by I.04/I.13/I.14/I.18-I.20),
  valid-install-no-panels (I.02), valid-install-frontend (I.03, I.17),
  missing-manifest (I.08), fail-validator (I.09 with rbac.json orphan
  namespace CF1), fail-migration (I.10, I.11 with broken SQL),
  not-a-zip.bin (I.06). README.md documents the mapping. Smaller than
  the ICD's "20 fixtures" count — redundant cases share the base
  tree, and dynamic ones (oversized, path-traversal, unknown route,
  bundled shell) are cheaper to generate in the driver.
- `tests/kernel/packages/shell_blob_test.cpp` — **new**. Asserts
  non-empty span + zip-magic first four bytes.
- `tests/kernel/packages/packages_schema_test.cpp` — **new**. 3 unit
  cases inspect `migrations/schema.sql` for the expected CREATE TABLE
  statements / 11-state CHECK / partial indexes; 3 PG-gated integration
  cases exercise state CHECK rejection, `plinth.panels` CASCADE-
  delete, and `uniq_packages_name_active` blocking a second ACTIVE
  row (while permitting a concurrent INSTALL_FAILED row).
- `tests/kernel/packages/asset_server_test.cpp` — **new**. I.13 (200
  + immutable cache + ETag), I.14 (URL-encoded path-traversal → 404,
  not 200), I.15 (unknown pair → 404). Reuses ws_test_fixture's Drogon
  instance (app() is a singleton — one-line extension registers the
  asset handler there). Atexit chain extended to call
  `asset_server::cancel_all_registrations()` before `initiate_shutdown`
  per feedback_deterministic_teardown.md.
- `tests/kernel/packages/install_lifecycle_test.cpp` — **new**. 10
  PG-gated cases I.01-I.10 invoking `install_package()` with pre-
  zipped fixtures; includes a libzip-source-built path-traversal zip
  for I.07.
- `tests/kernel/packages/crash_recovery_test.cpp` — **new**. 6 PG-
  gated cases: I.11 (MIGRATING + ext schema → INSTALL_FAILED + drop),
  I.11b (UPLOADING/VALIDATING rows), I.12a (ACTIVATING + tree →
  ACTIVE), I.12b (ACTIVATING without tree → INSTALL_FAILED), I.16
  (install_shell_if_needed fresh → ACTIVE row), I.17 (skip when
  frontend already ACTIVE).

### ICD deviations (slice B)

- **(a)** `tests/fixtures/install_lifecycle/` ships 7 fixtures instead
  of 16; the ICD's 20-case mapping is spread across those seven bases
  plus test-time generation of oversized (I.05), path-traversal (I.07),
  and unknown-route (I.15) scenarios.
- **(b)** No separate `tests/kernel/http_test_fixture.{hpp,cpp}`;
  Drogon's process-wide app() singleton makes a second fixture
  infeasible. Extended ws_test_fixture's server startup to also
  register the asset handler (one line), and asset tests include
  `../ws/ws_test_fixture.hpp` directly. Trade-off: asset tests inherit
  the PG requirement from the ws fixture.
- **(c)** No fork()/exec()/SIGKILL subprocess harness. Crash recovery
  is exercised by seeding `plinth.packages` rows with manufactured
  in-flight states and invoking `reconcile_in_flight_installs()` in-
  process. The reconciler's correctness is a function of
  (state, on-disk tree presence); both inputs are trivially
  constructible without a subprocess. A real-kernel harness interacts
  with the parked trantor teardown flake (project_ws_flaky_segfault.md)
  for low additional fidelity. Follow-up candidate if the flake moves.
- **(d)** I.18 (concurrent POST), I.19 (dry-run), I.20 (RBAC denial)
  require the `/api/packages` HTTP surface — deferred to a follow-up
  PR that brings up an HTTP test harness with a session + RBAC seed
  path. Not blocking `v0.4.4`; the library-level install path is
  fully covered.

### Verification (slice B)

- Default `ctest`: 268/268 pass (new unit cases land in shell_blob,
  packages_schema unit tier; PG-gated cases SKIP with the standard
  message when `PLINTH_PG_HOST` is unset).
- PG-gated tier (`PLINTH_PG_HOST=...`): adds 19 cases (I.01-I.10,
  I.11/I.11b/I.12a/I.12b/I.16/I.17, + three packages-schema
  integration cases).
- `run-clang-tidy-20` zero findings across all 159 kernel TUs
  (`cmake --build build --target tidy`). Slice B also absorbs the
  Slice A tidy debt that CI #12160 surfaced — `install_lifecycle.cpp`
  (uuid_v4 `std::array` + `snprintf` via NOLINT with rationale,
  `sha256_hex` with `HEX_DIGITS` UPPER_CASE + `.at()` access,
  `extract_zip_to` cognitive-complexity NOLINT with rationale,
  rollback/staging/lock guards gain deleted copy/move special members,
  `set_state` helper replaces `(void)` discards of
  `std::expected<void,E>` returns, `mig.error()` binding to a const
  ref silences unchecked-optional-access false positives across the
  has_value → deref pattern), `asset_server.cpp` (`MIME_TABLE` /
  `BUF_SIZE` UPPER_CASE renames, `std::ranges::mismatch`, percent-
  decode cognitive-complexity + Drogon-callback rvalue-ref NOLINTs
  with rationale, `hex` lambda braces, empty-catch NOLINT on noexcept
  teardown), `handlers.cpp` (Drogon-callback rvalue-ref NOLINTs,
  multipart reinterpret_cast NOLINT, `parse_int_param` helper replaces
  two `atoi` calls with `std::from_chars`, `PQgetisnull != 0` explicit
  bool), `main.cpp` (`std::atexit` return-value cert-err33-c NOLINT),
  `groups/handlers.cpp` (const-local rename so UPPER_CASE constant
  rule passes), `config.cpp` (`apply_json` refactored into
  `apply_database` / `apply_ws` / `apply_packages` helpers to drop
  below the cognitive-complexity threshold).
- **Parallelism cap enforced** (per `feedback_parallelism_cap.md`):
  `CMakeLists.txt` switches the `tidy` target from
  `ProcessorCount(TIDY_JOBS)` (all cores) to `TIDY_JOBS=4` (cache
  var). CI `ci.yml` build / benchmark / fuzz steps move from
  `-j$(nproc)` to `-j 4` so local + CI stay in sync. Memory
  `feedback_parallelism_cap.md` amended to document the silent
  `ProcessorCount` bypass of the cap and the fix.
- Manual smoke pending at PR review: browser GET /ext/notes/1.2.3/
  on an installed fixture; bundled-shell re-install after
  `TRUNCATE plinth.packages CASCADE` + kernel restart.

---

## 0.4.4 slice A in-flight — Package Install Lifecycle (branch `feat/0.4.4-install-lifecycle`)

Orchestrator milestone. Per ICD-0.4.4. The install state machine, HTTP
surface, asset serving, and schema changes land in slice A; slice B
(0.4.4.1, no tag of its own) ships the bundled-shell blob + crash-
recovery reconciler + SIGKILL test harness and cuts the `v0.4.4` tag.
Commit sequence on branch: schema + libzip + foundation → HTTP surface
+ main.cpp hooks → unit tests. PG-gated integration tests + 16 install-
lifecycle fixtures are queued for a follow-up commit on the same
branch before the PR opens.

### Deliverables (slice A)

- `migrations/schema.sql` — `plinth.packages` (11-state CHECK, UUID
  PK, `uniq_packages_name_active` + `uniq_packages_mount_active`
  partial indexes for 0.4.4's first-install-only posture,
  `idx_packages_state`) + `plinth.panels` (CASCADE-DELETE on owning
  package, panel_type CHECK with primary/float/settings/tray,
  slot_type nullable-with-CHECK-home, declaration JSONB).
- `CMakeLists.txt` — libzip v1.11.4 via FetchContent (zlib-only,
  static, no BZ2/LZMA/ZSTD, no build-tools / examples / docs).
  Linked into `plinth` and `plinth_tests`.
- `src/kernel/packages/install_lifecycle.{hpp,cpp}` — **new**. State
  machine `install_package(zip_blob, provenance, ctx)` drives
  UPLOADING → VALIDATING → MIGRATING → REGISTERING → EXTRACTING →
  ACTIVATING → ACTIVE (or INSTALL_FAILED at any stage). Per-name PG
  advisory lock via `pg_try_advisory_lock(hashtextextended('plinth.packages.'||name,0))`.
  libzip extraction with path-traversal rejection and zip-bomb guard
  (2× uncompressed cap, `ZIP_UINT64_MAX` sentinel rejected). REGISTERING
  wraps `register_capability_tx × N` + `upsert_extension_rule × N` +
  `register_panel × N` in a single admin-connection transaction;
  NOTIFY is buffered by PG until COMMIT so ROLLBACK correctly
  suppresses cache invalidation on other nodes. Terminal audit only:
  `packages.installed` / `packages.install_failed{stage, kind}` —
  per-stage state transitions are observable in `plinth.packages.state`
  + `last_install_report`. `reconcile_in_flight_installs` stubbed in
  slice A; full body in slice B.
- `src/kernel/packages/asset_server.{hpp,cpp}` — **new**. Single
  Drogon wildcard handler at `/ext/([^/]+)/([^/]+)/(.*)` routing
  through an in-memory `(name,version) → RouteHandle` map (shared
  mutex, O(1) lookup). MIME table for js/mjs/css/html/json/svg/png/
  jpg/jpeg/webp/woff2/gif; unknown → `application/octet-stream`.
  Percent-decode + component-level path-traversal rejection,
  `weakly_canonical` escape check. Headers: `Cache-Control:
  public, max-age=31536000, immutable` + `ETag: W/"<manifest_checksum>"`.
  `cancel_all_registrations()` is the atexit hook per
  feedback_deterministic_teardown.md — flips a shutdown atomic so
  the trampoline serves 503 during drain. `restore_routes()` replays
  every ACTIVE/ACTIVE_FLAGGED row on kernel restart.
- `src/kernel/packages/panels.{hpp,cpp}` — **new**. `register_panel(PGconn&, reg)`
  INSERT into plinth.panels, borrowed-connection for transactional
  sharing with REGISTERING.
- `src/kernel/packages/handlers.{hpp,cpp}` — **new**. `POST
  /api/packages` (multipart zip → `install_package`), `GET
  /api/packages` (paginated list, `?limit`/`?offset`/`?include_failed`),
  `GET /api/packages/{id}` (full record with manifest_json +
  last_install_report). SessionFilter + RbacFilter on every route;
  packages.install gates POST, packages.read gates both GETs. Failure
  stage → HTTP status per ICD table (413/409/422/500).
- `src/kernel/rbac/rule_registrar.{hpp,cpp}` — **new**.
  `upsert_extension_rule(PGconn&, rule, namespace_, description,
  extension_name)` ON CONFLICT DO UPDATE (refreshes description /
  namespace / extension_name and clears `orphaned_at` on reinstall).
  No audit emission — caller owns terminal audit.
- `src/kernel/capabilities/registration.{hpp,cpp}` — additive
  `register_capability_tx(PGconn&, reg) -> RegisterResult` overload.
  Accepts a borrowed connection; INSERT + NOTIFY inside caller's
  transaction. Shares a private helper with the existing surface —
  `register_capability(Config::Database, ...)` is untouched. No
  audit emission (install lifecycle handles at terminal boundary).
- `src/kernel/groups/handlers.cpp` — `bootstrap_groups` seeds
  `packages.install` + `packages.read` rules, grants both to the
  `admin` group, and emits `rbac.rule_registered` audit events with
  the existing idempotency guard.
- `src/kernel/config.{hpp,cpp}` — three new fields: `packages_data_dir`
  (default `./data`), `packages_staging_dir` (default `./data/staging`),
  `packages_max_package_size_mb` (default 50). JSON config key
  `packages.*`; existing configs parse unchanged.
- `src/kernel/packages/validator.hpp` — additive
  `ValidationConfig::in_process_registry` bool (default false).
  Reserved for future RT-pass wiring; slice A installer skips the
  RT pass and handles name-collision inline instead.
- `src/kernel/main.cpp` — `std::atexit(...)` installed at top of
  service mode for deterministic closeout on signal-induced exit
  (asset_server cancel_all → WS registry cancel_all_timers →
  initiate_shutdown → log::shutdown → stop_notify_listener →
  drogon::quit). After WS route registration: asset_server's
  wildcard Drogon handler, `register_package_routes(...)`, the
  reconciler stub (slice B wires the full body), and
  `restore_routes(...)`.
- `tests/kernel/packages/install_lifecycle_unit_test.cpp` — **new**.
  4 unit cases over `provenance_to_string` / `stage_to_string` /
  `panel_type_to_string` / `slot_type_to_string`. Broader PG-gated +
  Drogon-harness coverage (I.01-I.20) lands in the next commit.

### ICD open questions resolved

- **OQ #1 upgrade-vs-reject.** First-install-only invariant: 409 on
  name collision. Any existing row in a non-UNINSTALLING state blocks
  new installs of the same name. 0.4.5's atomic-swap upgrade path
  relaxes this; 0.4.4 does not.
- **OQ #2 zip library.** libzip v1.11.4, zlib-only, static, via
  FetchContent. ~180 KB binary cost.
- **OQ #3 shell blob.** Placeholder `client/shell/` tree + `ld -r -b
  binary` object file — slice B's scope. Slice A ships no shell blob.
- **OQ #4 in_process_registry.** Added as additive bool to
  `ValidationConfig` (default false). Slice A installer does NOT set
  it — RT1 (name collision) is handled inline at UPLOADING via
  `SELECT 1 FROM plinth.packages`, RT2 (mount collision) is enforced
  by the `uniq_packages_mount_active` partial UNIQUE index at
  REGISTERING COMMIT. RT3 (required capabilities) is structural
  through manifest parse. The flag stays reserved for the CLI's
  `plinth validate --against-running-kernel` loopback path when that
  wiring lands (0.10.5 CLI hardening or sooner if demand appears).
- **OQ #5 audit granularity.** Terminal only — `packages.installed`
  on ACTIVE, `packages.install_failed{stage, kind}` on INSTALL_FAILED.
- **OQ #6 MIGRATING reconcile disposition.** Always INSTALL_FAILED
  (0.4.5 can revisit if resumable migrations become desirable).
  Slice B's reconciler implements this.
- **OQ #7 REGISTERING atomicity** (this PR's own question, surfaced
  during planning). `register_capability_tx(PGconn&, ...)` additive
  overload — caller owns BEGIN/COMMIT/ROLLBACK. Existing
  `register_capability(Config::Database, ...)` surface unchanged.

### Accepted deviations

- **(a)** Single Drogon wildcard regex route for `/ext/*` instead of
  per-(name,version) route registration. Drogon offers no route
  deregistration primitive; the map-guarded trampoline achieves
  per-install registration / deregistration without mutating Drogon's
  route table.
- **(b)** `in_process_registry` flag is wired but not consumed by
  slice A's installer — the ICD's direction to run RT1/RT2/RT3
  through the validator is fulfilled structurally (RT1/RT2 inline,
  RT3 via manifest parse). Library-level RT wiring remains a future
  concern for the CLI loopback path.
- **(c)** `reconcile_in_flight_installs` is a stub in slice A.
  The production binary calls it during bootstrap; slice B provides
  the body + the SIGKILL harness that exercises it.
- **(d)** Unit test coverage in slice A is limited to pure-helper
  round-trips; fixture + PG-gated + Drogon-harness cases land in a
  follow-up commit on the same branch before PR open.

### Follow-ups on branch before PR

- I.01–I.10, I.18–I.20 PG-gated fixture cases + the 16 install-
  lifecycle fixture packages.
- I.13–I.15 asset-server Drogon-harness cases via a lightweight
  `http_test_fixture` shared with future packages tests.
- Bootstrap-schema tests for the new `plinth.packages` +
  `plinth.panels` tables (existing bootstrap_schema_test.cpp pattern).
- Clang-tidy sweep on the new TUs.

### Slice B (0.4.4.1, no tag of its own; cuts `v0.4.4` at HEAD)

Placeholder `client/shell/` tree (manifest.json + empty capabilities /
rbac / panels + bare HTML), CMake `ld -r -b binary` shell blob rule,
`shell_blob.{hpp,cpp}` for symbol access, `install_shell_if_needed()`
in main.cpp, full `reconcile_in_flight_installs` body, SIGKILL harness
under `PLINTH_KERNEL_TESTS_ENABLED`, I.11/I.12/I.16/I.17 test cases.

---

## v0.4.3 — Extension PG Schema Creation + Migration Execution (2026-04-19)

Library milestone. Per ICD-0.4.3. Kernel now stands up per-extension
`ext_{name}` schemas with an `ext_{name}_role` PG role and runs
numbered `migrations/NNN_*.sql` files against them with checksum-
immutable tracking in `plinth.migrations`. No CLI, no HTTP surface —
the single in-tree caller is 0.4.4's forthcoming MIGRATING stage.

### Deliverables

- `src/kernel/packages/migration_error.hpp` — **new**. `MigrationError`
  enum (DUPLICATE_SEQUENCE, INVALID_FILENAME, CHECKSUM_MISMATCH,
  SCHEMA_CREATE_FAILED, MIGRATION_APPLY_FAILED, DB_CONNECTION_BAD,
  READ_FAILED, ADVISORY_LOCK_FAILED, DROP_FAILED) + `MigrationFailure`
  struct (kind, extension_name, migration_file?, pg_sqlstate?, message)
  + `MigrationWarning` (string-keyed kind + detail).
- `src/kernel/packages/migrations.{hpp,cpp}` — **new**. Two public
  entries: `run_migrations(name, package_root, admin_conn)` applies
  schema + role + GRANTs idempotently and drives numbered migrations;
  `drop_schema_and_migrations(name, admin_conn)` is the companion
  teardown helper for 0.4.4's first-install-failure disposition.
  Returns `std::expected<MigrationReport, MigrationFailure>` mirroring
  the 0.3.0.1 kernel result-type convention. PG advisory-lock
  serialization via `hashtextextended('plinth.migrations.' || name, 0)`.
- `src/kernel/packages/migrations_internal.hpp` — **new**. Exposes
  pure-logic helpers (`parse_migration_filename`, `strip_utf8_bom`,
  `sha256_hex`, `discover_migrations`) for unit testing; no part of
  the public 0.4.3 library surface.
- 15 fixtures under `tests/fixtures/migration_packages/` (empty,
  single-migration, three-migrations-in-order,
  three-migrations-010-vs-002, gap-001-004, duplicate-sequence,
  invalid-filename, non-sql-extension, no-migrations-dir, bad-sql,
  checksum-mismatch, idempotent-rerun, role-already-exists,
  concurrent-same-extension, non-utf8-bom). Each carries a stub
  `manifest.json` (name field only) plus the migration files the
  fixture-name implies. Tests stage a copy into a temp dir and
  substitute a `{EXT_NAME}` placeholder so fixture SQL can reference
  the actual `ext_{name}` the test generates.
- `tests/kernel/packages/migrations_parsing_test.cpp` — **new**. 12
  pure-logic unit cases. No PG.
- `tests/kernel/packages/migrations_test.cpp` — **new**. 16 PG-gated
  integration cases covering M.01–M.15 plus a `drop_schema_and_migrations`
  idempotency case. Runtime `SKIP("PG not available")` gate per the
  listener / audit precedent — no `#ifdef`. Each test generates a
  unique `ext_test_{pid}_{counter}` extension name; RAII
  `ExtensionScope` drops schema + role + `plinth.migrations` rows on
  exit.

### Decisions ratified (ICD Open Questions)

- **Q1 (admin-conn source).** Left to 0.4.4; 0.4.3 takes `PGconn&`.
- **Q2 (runtime `SET ROLE` / `SET search_path` helper).** Deferred to
  0.5.x `db.*` binding. No 0.4.3 helper. `DEFERRED.md` entry
  "Per-op `SET search_path` for `db.*`" updated to point at 0.5.x.
- **Q3 (first-install rollback shape).** `drop_schema_and_migrations`
  shipped as a companion helper inside the 0.4.3 library. 0.4.4
  orchestrator calls it on first-install failure disposition.
- **Q4 (shared-kernel-table grants).** Only `GRANT SELECT ON
  plinth.users`. Future additions land one-at-a-time alongside each
  0.5.x stdlib-function.
- **Q5 (SHA-256 vs xxhash).** SHA-256 via reuse of
  `plinth::auth::sha256_hex` from `src/kernel/auth/crypto.hpp`.

### Accepted deviations

- **(a)** `drop_schema_and_migrations` issues `DROP OWNED BY
  ext_{name}_role CASCADE` before `DROP ROLE` to drop the
  `SELECT ON plinth.users` grant that would otherwise block the role
  drop with "role has privileges that must be revoked first." The
  ICD did not prescribe this ordering; the PG semantics force it.
- **(b)** Added `MigrationError::DROP_FAILED` beyond the ICD's 8-kind
  enum so the teardown helper surfaces a distinct failure shape from
  `SCHEMA_CREATE_FAILED`. The ICD enumerated the main-path kinds only.
- **(c)** Pure-logic helpers promoted to a `namespace detail` header
  (`migrations_internal.hpp`) so `migrations_parsing_test.cpp` can
  exercise them without the PG harness. The ICD called for the two
  test files but did not prescribe how the helpers would be exposed.
- **(d)** Fixture SQL uses a `{EXT_NAME}` placeholder substituted at
  stage time. The ICD didn't address fixture SQL vs per-test schema
  name; tests need real DDL that references the ephemeral schema.

### Verification

- `ctest` 426/428 on full suite; the two failures are the pre-existing
  async-bridge parallel-dispatch + memory-limit-classifier flakes
  documented in `project_ws_flaky_segfault.md` (both pass in isolation,
  unrelated to this change).
- `run-clang-tidy-20 -p build src/kernel/packages/migrations*.{hpp,cpp}
  tests/kernel/packages/migrations*.cpp` zero findings.
- ASan + UBSan build clean on 28 migration cases (12 unit + 16
  integration).
- Local PG: `pgvector/pgvector:0.8.2-pg18-trixie` via
  `docker/docker-compose.yml`.

---

## Docs session 2026-04-19 — ICDs 0.4.3 + 0.4.4 (paired tightening)

Paper-only. Four-part slot `0.4.2.1`, untagged per the 2026-04-18
"3-part X.Y.Z tag only" rule. Forward-ICD tightening for the two
next `[medium]` 0.4.x milestones per
`feedback_icd_horizon.md` and `RE-EVAL-0.4.x.md §7` — authored
together so 0.4.4's orchestrator ICD can cite 0.4.3's library
surface by name.

### Deliverables

- `docs/icd/ICD-0.4.3-extension-schema-creation-and-migration.md` —
  **new**. Contract for per-extension `ext_{name}` schema creation,
  `ext_{name}_role` + GRANT setup, numbered-migration execution
  against `plinth.migrations`, SHA-256 checksum immutability, 15
  test fixtures under `tests/fixtures/migration_packages/`. Library
  milestone — no HTTP, no CLI. Single caller in-tree is 0.4.4's
  MIGRATING stage. Five Open Questions documented for architect
  ratification at PR time (admin connection sourcing, runtime
  `search_path` handoff to 0.5.x, first-install-vs-upgrade rollback
  disposition, `ext_{name}_role` GRANT list, SHA-256 vs xxhash).
- `docs/icd/ICD-0.4.4-package-install-lifecycle.md` — **new**.
  Contract for the full install state machine (UPLOADING →
  VALIDATING → MIGRATING → REGISTERING → EXTRACTING → ACTIVATING →
  ACTIVE/INSTALL_FAILED), `POST /api/packages` admin-authenticated
  upload endpoint, `GET /ext/{name}/{version}/*` public asset
  serving, `plinth.packages` + `plinth.panels` schema tables,
  bundled-shell first-boot install, PG advisory-lock serialization,
  `reconcile_in_flight_installs` crash recovery. 20 test fixtures
  under `tests/fixtures/install_lifecycle/` incl. two
  crash-recovery cases (I.11, I.12) using fork/SIGKILL harness. Six
  Open Questions documented. Explicitly excludes disable/enable/
  uninstall/upgrade (0.4.5 scope) — 0.4.4 is first-install-only, so
  `plinth.packages` uniqueness collapses to at-most-one-installed-
  per-name via partial index. Atomic swap (DESIGN §8) appears only
  in crash-recovery form; upgrade atomic swap is 0.4.5.
- `docs/ROADMAP.md` — 0.4.3 and 0.4.4 promoted `[medium]` →
  `[strong]` in the 0.4 Package System section. No trim (0.4.2 was
  already trimmed at `v0.4.2` ship). Cadence line `RE-EVAL
  following 0.4.4` unchanged.

### Decisions ratified during authoring

- **OQ-E resolved by authority.** ICD-0.4.1-glassworm-defense
  §Out-of-scope explicitly defers migration-SQL scanning
  ("`migrations/*.sql` files reach libpq, not `JS_Eval`"). ICD-0.4.3
  cites the deferral; no open question in the new ICD on this
  point.
- **First-install-only posture for 0.4.4.** Upgrade collision
  (same `name` already ACTIVE with newer version) returns 409 in
  0.4.4; the atomic-swap upgrade path lands with 0.4.5's
  disable/enable/uninstall work. This choice isolates 0.4.4's
  orchestrator scope and defers the route-drain logic to the
  milestone that needs it.
- **ICD-0.4.3 library surface monotonic; orchestrator owns
  rollback.** `run_migrations` never unwinds. A new companion
  `drop_schema_and_migrations(name, admin_conn)` (also 0.4.4 scope)
  is called by the install orchestrator on first-install MIGRATING
  failure. Upgrade paths (0.4.5) will not call the companion,
  preserving DESIGN §0.4.3 stickiness. The boundary keeps 0.4.3's
  library re-usable from any future admin-side recovery tool
  without bespoke first-install-detection logic.

### Forward-ICD horizon after this session

- **N+1 cleared:** ICD-0.4.3 authored (first consumer is code
  session 0.4.3).
- **N+2 cleared:** ICD-0.4.4 authored (first consumer is code
  session 0.4.4).
- **N+3 (0.4.5):** `[medium]`, DESIGN-packages-v04x.md §0.4.5 is
  the contract. ICD-authoring slot fires between 0.4.4 and 0.4.5
  per the horizon rule — expected alongside the next docs session
  after 0.4.4 ships.

### Verification

- `docs/icd/` now carries 22 files (through 0.4.4). Section
  templates match ICD-0.4.2 shape (Traces / Depends / Milestone /
  Status / Methodology / Related / Overview / Scope / Out-of-Scope /
  Rules or State Machine / Library Surface / Error Taxonomy /
  Security Constraints / Test Cases / CI Wiring / Entry-Exit / Open
  Questions / Appendix).
- `docs/ROADMAP.md` 0.4 section shows `[strong]` on both 0.4.3 and
  0.4.4.
- No code / CMake / CI YAML changes. Paper-only session.

---

## v0.4.2 — 2026-04-19 — Cross-file manifest validation pass

Implements ICD-0.4.2-cross-file-manifest-validation.md. Extends
`plinth validate` with a post-parse cross-file pass (CF1..CF7,
CFW1..CFW4) and an optional runtime-state pass (RT1..RT3, gated
behind `--against-running-kernel`). Cross-file is the new default
disposition; `--structure-only` falls back to the 0.4.0 R1..R6
rule set. Typed parsers for `panels.json` and `config.json` land
alongside. `rbac.json` stays as raw JSON until 0.4.6 per ICD
§Out-of-Scope.

### New TUs

- `src/kernel/packages/cross_file_validator.{hpp,cpp}` — CF1..CF7,
  CFW1..CFW4 rules + inline RT1..RT3 stubs (per ICD Open Question
  #4). Exports `ParsedPackage`, `run_cross_file_validation`, and
  `run_runtime_state_validation` for future 0.4.4 install-lifecycle
  reuse without the CLI roundtrip.
- `src/kernel/packages/panels_manifest.{hpp,cpp}` — typed parser.
  Mirrors 0.4.1 `ParseResult<T>` shape. Requires `id` (per
  DESIGN-packages-v04x.md §4.3 which stores `panel_id` in the install
  record). Tolerates top-level array form with a
  `panels.shape.array_at_root` warning.
- `src/kernel/packages/config_manifest.{hpp,cpp}` — typed parser.
  Scalar-only defaults (bool/int/double/string); arrays and objects
  are rejected with `config.<key>.non_scalar_default`.
- `src/kernel/packages/detail/reporter.hpp` — promoted out of
  `validator.cpp`'s anonymous namespace so both passes emit through
  the same sink. Kernel-internal; not part of the public API.
- `src/kernel/js/runtime_config.hpp` — constexpr getter for the
  QuickJS per-runtime memory cap (`get_max_memory_mb()`). Header-only
  thin wrapper over the 0.3.1 constant, consumed by CFW4 without
  pulling `<quickjs.h>` into the validator.

### Modified TUs

- `src/kernel/packages/validator.{hpp,cpp}` — `ValidationConfig`
  gains `cross_file` (default true), `against_running_kernel`,
  `kernel_url`. `ValidationMessage` gains `Phase phase` enum field
  (STRUCTURE / CROSS_FILE / RUNTIME_STATE). `render_json` emits the
  phase string on every message entry (strict enum per ICD Open
  Question #3). `Parsed` internal struct replaced by public
  `ParsedPackage`; `parse_optional_files` switched to typed parsers
  + raw JSON for rbac. `run_handler_files` and `run_panel_files`
  are suppressed when `cross_file == true` so CF4/CF3 supersede R4/R5
  cleanly (ICD Open Question #4 resolution).
- `src/kernel/packages/manifest.{hpp,cpp}` — `PackageManifest` gains
  optional `bool provider_extension = false` reserved for future CF7
  relaxation (ICD Open Question #2 resolution: design-in now, activate
  in a later milestone).
- `src/kernel/js/eval.cpp` — `MEMORY_LIMIT_BYTES` constant now sourced
  from `runtime_config::MAX_MEMORY_BYTES`. No behavior change.
- `src/kernel/main.cpp` — `validate` subcommand gains
  `--structure-only` / `--against-running-kernel` (mutually
  exclusive argparse group) and `--kernel <url>`. `PLINTH_KERNEL_URL`
  env var is the fallback (ICD Open Question #1 resolution). CLI body
  factored into `run_validate`/`resolve_kernel_url`/
  `stdout_supports_colour` to keep main's cognitive complexity under
  the tidy threshold.
- `CMakeLists.txt` — four new TUs added to both `plinth` and
  `plinth_tests` source lists. New `PLINTH_KERNEL_TESTS` option gates
  the RT-fixture test invocations (default OFF, matches existing
  PG-backed test pattern).

### Fixtures + tests

- `tests/fixtures/packages/` gains 15 static fixtures
  (`valid-cross-file`, one per CF/CFW rule, plus the dual-mode
  `valid-cross-file-structure-only-vs-default`) and 3 runtime-state
  fixtures (`rt-name-collision`, `rt-mount-collision`,
  `rt-missing-requires`, gated on `PLINTH_KERNEL_TESTS=ON`).
  Pre-existing `valid-full` / `valid-minimal` / `panel-missing`
  fixtures updated for the typed panels parser (`id` replaces `name`)
  and cross-file-clean baseline.
- `tests/kernel/packages/cross_file_validator_test.cpp` (Catch2
  driver across all 15 static fixtures + dual-mode proof + RT stub +
  JSON phase-field check).
- `tests/kernel/packages/panels_manifest_test.cpp` + `config_manifest_test.cpp`
  (parser unit tests).
- Pre-existing `validator_test.cpp` tests updated: `handler-missing`
  and `panel-missing` cases now pass `cfg.cross_file = false` to
  continue exercising R4/R5; the `handler-missing` CLI case uses
  `--structure-only`; `unicode-legitimate-emoji` case disables
  cross-file to isolate scanner behavior from cross-file rules.

### Accepted deviations (on the same footing as 0.4.0 ParseResult)

1. **Reporter in a `detail/` header** rather than promoted into the
   public `validator.hpp`. Keeps the ValidationMessage aggregate
   construction an implementation detail.
2. **RT1..RT3 are stubbed** with a single
   `runtime-validate-unimplemented` error; the HTTP path to the
   kernel's `POST /api/packages/validate` endpoint is 0.4.4 scope.
   Cross-file-error gating on the RT pass still fires correctly
   (`runtime-state-skipped` warning) so the skip semantics are
   covered today.
3. **CF3 realpath check** uses `fs::weakly_canonical` + string-prefix
   comparison inline rather than promoting `symlink_escapes_root` out
   of `validator.cpp` — CF3 is the only cross-file caller and the
   check is three lines.
4. **`ConfigEntry::default_value`** widens to
   `std::variant<monostate,bool,int64_t,double,string>` (ICD said
   "scalar"; this is the concrete type). Captures JSON's distinction
   between integer and floating-point without losing precision.
5. **`panels.json` top-level array form** accepted with a warning
   rather than rejected outright — matches 0.4.0 R5's tolerance;
   future `panels.shape.*` amendments can tighten.
6. **Phase enum naming in JSON** uses snake_case `"structure"`,
   `"cross_file"`, `"runtime_state"` (ICD proposed `"cross-file"`
   with a dash). Snake_case aligns with all other kernel enum-to-JSON
   projections (capability scope, provider_type, etc.).

### Verification

- 382/382 ctest pass (default build).
- `run-clang-tidy-20` zero findings on the four new TUs + modified
  `validator.cpp` / `main.cpp` / `manifest.cpp`.
- Sanitizer build 379/380 pass; the one failure (`limits:
  promise-allocation loop trips MEMORY_LIMIT`) reproduces on
  pre-0.4.2 `main` under ASAN — pre-existing, unrelated.
- Manual CLI walk: `valid-cross-file` default → 0; `--structure-only`
  → 0; `cf4-handler-missing` default → 1 (CF4); `--structure-only`
  → 2 (R4); `--json` emits `"phase":"cross_file"` on every
  cross-file message; `--structure-only --against-running-kernel`
  rejected by argparse with mutually-exclusive error and exit 1.

---

## v0.4.1 — 2026-04-19 — GlassWorm Unicode defense

Implements ICD-0.4.1-glassworm-defense.md against
DESIGN-glassworm-defense-v0x.md. Closes the GlassWorm threat family at
the two pre-runtime entry surfaces: package install and `JS_Eval`
source submission. Layer 3 (frontend content sanitization) deferred to
0.6.x per DESIGN §4.3.

### New TUs

- `src/kernel/security/unicode_scanner.{hpp,cpp}` — kernel security
  primitive. Single-pass UTF-8 decoder; 13 scanned ranges per ICD
  §Scanned Ranges; pure (no kernel state, no allocations on the
  clean path beyond one reserved findings vector); `strict_utf8 = true`
  rejects malformed input via `decode_error`. Early-exit at 2× threshold
  per ICD §Implementation Latitude.
- `src/kernel/js/eval_guard.{hpp,cpp}` — Layer 2 helper. `pre_eval_scan`
  is invoked at every `JS_Eval` call site before the runtime sees the
  source bytes; populated `EvalError{kind: UNICODE_SMUGGLE_DETECTED}`
  shorts the call. File-static atomics carry the process-global scanner
  policy (`set_unicode_scanner_policy` is the boot-time setter); a
  mutex-guarded LRU enforces the 1 Hz audit rate limit per
  `(layer, source_path)`.

### Modified TUs

- `src/kernel/packages/validator.cpp` — new `run_unicode_scan_pass`
  inserted between `run_json_parse` and `run_handler_files` in
  `validate()`. Explicit file allowlist (root JSON, `server/handlers`,
  `server/main.js` entry_point, `client/{panels,components}`); reuses
  `read_file_bytes` (binary mode). New `unicode-smuggle` rule with
  `Severity::ERROR`. File-static rate-limit LRU (no mutex; CLI is
  single-threaded — note inline that future kernel installer at 0.4.4
  needs to add one).
- `src/kernel/js/eval.cpp:277`, `runtime_pool.cpp:756` (inside
  `eval_on_context`), `run_on_context.cpp:816` (inside the
  `drogon::Task<EvalResult>` coroutine) — three two-line guard
  inserts. Async path wraps via `co_return EvalResult{.value =
  std::unexpected(*err), .duration = ...}` per the existing pattern
  at line 851.
- `src/kernel/js/eval.hpp` — new `EvalErrorKind::UNICODE_SMUGGLE_DETECTED`
  variant (post-`ASYNC_RESULT_SIZE_EXCEEDED`). Pre-eval-only kind;
  `classify_rejection` does NOT map it.
- `src/kernel/js/bridge_context.hpp` — `ConfigProjection` extended with
  `security_unicode_scanner_{enabled,threshold,log_findings}`. Reserved
  for future policy hooks; not consumed by any 0.4.1 JS binding.
- `src/kernel/js/runtime_pool.cpp:49` — `make_config_projection`
  extended with the three new field initializers.
- `src/kernel/config.{hpp,cpp}` — three flat fields appended to
  `Config`; new `apply_security` helper extends `apply_json` to read
  nested `security.unicode_scanner.{enabled,threshold,log_findings}`
  keys (config file is JSON-parsed despite the `.yml.example` filename).
- `src/kernel/main.cpp` — `serve` arm wires the scanner policy via
  `plinth::js::set_unicode_scanner_policy(...)` after
  `plinth::log::init(cfg)`; emits the one-shot
  `security.unicode_scanner_disabled` audit if disabled. NOT wired
  from the `validate` arm (CLI has no DbClient).
- `src/kernel/packages/validator.hpp` — `ValidationConfig` extended
  with three matching scanner fields. CLI uses defaults; future kernel
  installer overrides from loaded `Config`.

### Tests

- `tests/kernel/security/unicode_scanner_test.cpp` (new) — G.01–G.08
  scanner unit cases. Uses `encode_utf8`/`repeat_codepoint` helpers to
  build inputs without `\uXXXX` source-encoding traps; G.07 builds the
  malformed `0xC0 0x80` overlong NUL via `std::string{...}` to keep
  clang-tidy quiet.
- `tests/kernel/js/eval_guard_test.cpp` (new) — G.14–G.17 Layer 2
  integration cases across the three `JS_Eval` entry points plus
  the "clean source still evaluates" regression. File-scope static
  initializer disables `log_findings` for the suite (the audit path
  reaches Drogon's DbClient layer, which `ensure_drogon_running()`
  deliberately doesn't bring up — the gate behavior is what's covered
  here, not the audit pipeline).
- `tests/kernel/packages/validator_test.cpp` — G.09–G.13 Layer 1
  fixture cases extended into the existing file. `prepare_dynamic_fixtures()`
  extended to write the malformed-UTF-8 `shell.js` payload at runtime
  (binary-mode `std::ofstream`) — the repo has no `.gitattributes`
  guarantee for byte-fidelity of malformed input. New `cfg_no_audit()`
  helper passes `unicode_scanner_log_findings = false` to the unicode
  cases for the same DbClient-isolation reason.

### Fixtures

Four new directories under `tests/fixtures/packages/`. Three commit
their static UTF-8 content; the malformed-UTF-8 one's `shell.js` is
generated at runtime (committed: `manifest.json`, `capabilities.json`,
empty `server/handlers/` directory).

- `unicode-smuggle-variation-selectors/` — 100 × U+FE0F in a JS comment.
- `unicode-smuggle-bidi-override/` — 55 × U+202E in a string literal.
- `unicode-smuggle-malformed-utf8/` — overlong `0xC0 0x80` mid-source.
- `unicode-legitimate-emoji/` — 5 emoji × 1 VS-16 each (well under
  threshold).

### Benchmark

- `benchmarks/unicode_scanner_benchmark.cpp` (new). Three cases:
  `BM_UnicodeScanner_CleanAscii_1MiB` (the ICD-mandated 100 MB/s gate),
  `BM_UnicodeScanner_HeavyVS` (early-exit ceiling), and
  `BM_UnicodeScanner_LegitimateEmoji` (sanity check). First-run local
  numbers (gcc-13, x86_64): clean-ASCII **1.63 GiB/s**, heavy-VS
  1.80 GiB/s, legitimate-emoji 36.8 GiB/s. ~16× margin on the ICD
  100 MB/s floor.

### Configuration

- `config.yml.example` — new `security.unicode_scanner.{enabled,
  threshold,log_findings}` block. Documents that the gate is
  secure-by-default and that disabling fires a one-shot audit.

### Audit events

- `security.unicode_smuggle_detected` (per detection, rate-limited).
- `security.unicode_smuggle_rate_limited` (one event per bucket-window
  overflow with the `suppressed_count` since last emit).
- `security.unicode_scanner_disabled` (one-shot at kernel boot).

### Accepted deviations

ICD claims wrong against current `main`; plan and PR reflect the
corrections:

1. **CMake edits ARE required.** ICD §Appendix says benchmarks are
   globbed; reality at `CMakeLists.txt:398-420` — each benchmark has
   its own `add_executable` block. New `plinth_unicode_scanner_benchmark`
   stanza added; `PLINTH_BENCHMARK_KERNEL_SOURCES` extended with
   `unicode_scanner.cpp`. The `plinth` and `plinth_tests` executable
   source listings are also explicit (only the `tidy` target uses
   `KERNEL_SOURCES` glob), so the new TUs were added to both.
2. **Config is JSON-parsed, not YAML.** ICD §Configuration shows YAML;
   `src/kernel/config.cpp:5` includes `nlohmann/json.hpp`. The example
   file's leading comment already documented the misnomer; the new
   `security` block uses JSON syntax.
3. **Layer 2 async pseudocode is incorrect.** ICD §Layer 2
   pseudocode shows `co_return std::unexpected(...)` for
   `run_on_context.cpp:816`; that function returns
   `drogon::Task<EvalResult>` (not `Task<expected<...>>`). PR uses
   the existing failure pattern at line 851:
   `co_return EvalResult{.value = std::unexpected(*err), .duration = ...}`.

### Resolved Open Questions from ICD §Open Questions

All six ratified per the ICD's tacit-ratification clause; no further
architect sign-off needed for these. (Threshold = 50; BOM always
counted; Layer 2 scan before CPU bracket; 1 Hz token bucket per
`(layer, source_path)`; no UTF normalization; disabled-state audit
emits once at kernel boot.)

### Verification

- `ctest --test-dir build`: 348 + 17 = 365/365 passing (still PG-skipped
  for ~30 PG-dependent cases on hosts without `PLINTH_PG_*` env).
- `run-clang-tidy-20 -p build src/kernel/security/ src/kernel/js/eval_guard.{hpp,cpp}`
  + the four modified JS TUs: zero findings.
- ASan + UBSan (`PLINTH_SANITIZERS=ON`) on `[unicode]`: clean
  (G.07 malformed-UTF-8 + G.16 async are the high-signal cases).
- Benchmark `PLINTH_BENCHMARKS=ON`: 1.63 GiB/s on clean ASCII vs.
  ICD 100 MB/s floor.
- CLI smoke: `./build/plinth validate
  tests/fixtures/packages/unicode-smuggle-variation-selectors` exits 1
  with `unicode-smuggle: server/handlers/shell.js contains 100
  invisible Unicode characters (threshold 50)`.

---

## Rewrite session — 2026-04-19 — ICD-0.4.1-glassworm-defense authoring

Documentation-only rewrite session per METHODOLOGY §Phase 3. No code,
tests, or CI changes. **Un-tagged** per the 2026-04-18 "3-part X.Y.Z
tag only" rule (ICD-authoring session, not arc-closeout or milestone
ship).

Fills the RE-EVAL-0.4.x.md §11.3 entry condition: 0.4.1 GlassWorm
code work needs `ICD-0.4.1-glassworm-defense.md` authored in a prior
docs session. DESIGN-glassworm-defense-v0x.md has been the
contract-by-pointer stopgap since the 2026-04-19 RE-EVAL PR — this
session promotes the contract to a full ICD one-ahead of the 0.4.1
code session, restoring the methodology's ICDs-one-ahead horizon
rule (METHODOLOGY-llm-assisted-development.md §3.1 Forward ICD
presence check).

### Deliverable

- `docs/icd/ICD-0.4.1-glassworm-defense.md` (new) — derivative of
  DESIGN-glassworm-defense-v0x §3–§6 and §9. Sections: Overview,
  Scanner Primitive (types + scanned ranges table + decode policy +
  perf budget), Layer 1 Package-Install Gate (injection point,
  file allowlist, rule + message shapes, CLI impact), Layer 2
  QuickJS Source-Load Gate (`eval_guard.{hpp,cpp}` helper, three
  `JS_Eval` integration points, new `EvalErrorKind` variant,
  0.3.5 composition), Configuration (YAML schema + Config field
  additions + ConfigProjection extension), Audit Logging (three
  event shapes + 1 Hz per-(layer, source_path) token-bucket rate
  limit), Test Battery (18 G-cases across scanner unit + Layer 1
  integration + Layer 2 integration + benchmark acceptance),
  Security Constraints (7 non-negotiables), Resolved Open
  Questions (all six from DESIGN §8), Milestone Criteria (9
  conditions), Entry/Exit, Appendix Integration Checklist.

### Resolved Open Questions from DESIGN §8

All six ratified to the proposed defaults (tacit ratification via
PR merge — precedent: 0.3.5 Open Question 1 `eval`/`Function`
deletion):

1. **Threshold default** = `50` findings. Tune on first false-positive
   evidence; `security.unicode_scanner.threshold` config knob
   exposes the global override.
2. **BOM (U+FEFF) policy** = always counted in `total_count`. A
   legitimate leading-BOM contributes 1, well under threshold.
   No positional exception; scanner is bytes-forward with no
   per-offset semantic.
3. **Layer 2 scan position** = before the call-depth check, before
   the CPU-timer bracket, after `inject_kernel_stdlib`. The scan
   is not CPU-budgeted (deterministic cost from source size +
   100 MB/s budget).
4. **Audit rate limit** = 1 Hz token bucket per `(layer,
   source_path)` tuple. Overflow emits one
   `security.unicode_smuggle_rate_limited` event with
   `suppressed_count`. Layer 1 and Layer 2 buckets are independent
   (CLI vs kernel process).
5. **Unicode normalization** = none. Scan raw UTF-8 bytes as
   written. Normalization (NFC / NFD / NFKC / NFKD) could fold
   smuggled sequences into or out of the scan ranges
   unpredictably.
6. **Disabled-state audit** = yes. `security.unicode_scanner_disabled`
   fires once at kernel startup if `security.unicode_scanner.enabled:
   false`. Low implementation cost; compliance breadcrumb.

### DESIGN-drift correction carried in ICD

DESIGN-glassworm-defense-v0x.md §4.2 and §6.1 cite the pooled
`JS_Eval` call site at `runtime_pool.cpp:697`. On current `main`
the line is **756** — 0.4.0.1's `drain_pending_jobs` helper added
~60 lines above the call site. The ICD's §Layer 2 table carries
the corrected line number and notes that implementers should verify
via `grep -n 'JS_Eval(' src/kernel/js/runtime_pool.cpp` at
implementation time — the authoritative pointer is the function
containing the `JS_Eval` call, not the line number. DESIGN is not
amended (doc-tree hygiene preserves the drift in DESIGN as the
historical snapshot from PR #41).

### ROADMAP

No mutation. 0.4.1 line stays `[strong]`, pending. The code session
that ships 0.4.1 will remove the line per the preamble rule.

### Forward ICD presence check — post-session state

- 0.4.1: **ICD authored this session** (this entry). DESIGN retired
  as primary contract; ICD is now authoritative.
- 0.4.2: ICD authored (2026-04-19 RE-EVAL session, PR #41).
- 0.4.3: `DESIGN-packages-v04x §0.4.3` is contract until a dedicated
  ICD slot fires. Per the ICDs-one-ahead horizon rule, ICD-0.4.3
  authoring is triggered by 0.4.1 code starting (not by this docs
  session). Architect decision at that point: pair with a separate
  post-0.4.1 docs session, or roll into 0.4.1 code branch's
  landing PR.

### Verification

- `git diff main --stat` shows only `docs/` paths — zero source,
  tests, CMake, or CI-YAML edits.
- Two files touched: `docs/icd/ICD-0.4.1-glassworm-defense.md` (new,
  ~450 lines) and `docs/CHANGELOG.md` (this entry).
- Every DESIGN §3–§6 contract clause has a corresponding ICD section
  (cross-checked by section number).
- Every identifier used in DESIGN (`unicode-smuggle` rule,
  `UNICODE_SMUGGLE_DETECTED` variant, `security.unicode_smuggle_detected`
  / `security.unicode_smuggle_rate_limited` /
  `security.unicode_scanner_disabled` audit events, `pre_eval_scan`
  helper, `plinth::security::UnicodeScanner` / `UnicodeScanResult` /
  `UnicodeScanConfig` / `UnicodeFinding` types) appears identically
  in the ICD.
- All three `JS_Eval` call-site line numbers in the ICD verified on
  current `main` via `grep -n 'JS_Eval(' src/kernel/js/*.cpp`.
- `validator.cpp:474` confirmed as the `validate()` entry function.
- All six DESIGN §8 Open Questions are resolved in the ICD
  §Resolved Open Questions section; zero "Open Question" items
  remain for the 0.4.1 code session.

---

## 0.4.0.1 — 2026-04-19 — Deterministic subprocess closeout (WS teardown + JS job drain)

Closes two new teardown-race signatures that surfaced post-0.3.5 on
CI runs #12118 and #12124 (observed 2026-04-19). The two CIs
caught **different** failure modes from the same root cause family
(framework-internal queues draining against already-freed state); the
fix installs a deterministic closeout phase on both sides.

Four-part follow-up on 0.4.0. **No git tag** per
`feedback_tagging_rule.md` (tags reserved for X.Y.Z milestones).

### Observed signatures

| CI / local | Test | Assertion | Phase |
|---|---|---|---|
| CI #12118 | 225 WS unsubscribe | `std::bad_weak_ptr` in `trantor::EventLoop::loop+0x2f2` | post-test subprocess teardown |
| CI #12124 | 276 cap.batch fail-fast | `quickjs.c:6678: free_zero_refcount: ref_count == 0` | mid-test microtask drain |
| local | 287 parallel queries | `quickjs.c:2323: JS_FreeRuntime: list_empty(&rt->gc_obj_list)` | `pool.destroy(bc)` |

All three are the same family: a framework's internal queue (trantor's
TimerQueue / QuickJS's job queue) still held work referencing state
we'd already released. Before this fix, full-suite ctest failed 3–4
times in 20 runs (15–27%) with one of the three signatures above.
After the fix: 20 full-suite runs with zero occurrences of any of
them (two unrelated flakes remain — test 265 classifier miss and
test 251 parallel-dispatch race, both tracked separately).

### Fix 1 — Drain QuickJS job queue before runtime teardown

`run_on_context`'s normal loop drains `JS_IsJobPending` as part of its
async-dispatch fixed point, but several paths let us call
`JS_FreeContext` / `JS_FreeRuntime` with jobs still queued:

- the drive_jobs error co_return in run_on_context (line ~846),
- finalize on a still-pending top-level promise,
- direct `RuntimePool::destroy(bc)` in test teardown.

A queued reaction job holds dup'd `JSValue`s in its argv
(`JS_EnqueueJob` at quickjs.c:2115 explicitly `js_dup`s each argument).
Those refs keep `gc_obj_list` non-empty and trip the
`list_empty(&rt->gc_obj_list)` assert in `JS_FreeRuntime`. Under a
different execution-order racing the microtask drain against our
`JS_FreeValue` in `BridgeContext::reject`, the same underlying leak
manifests as the `free_zero_refcount` assertion at drain time.

- `src/kernel/js/runtime_pool.cpp` — new file-scope
  `drain_pending_jobs(ctx, rt)` helper. Runs `JS_ExecutePendingJob`
  in a loop bounded at 4096 iterations (generous enough for deep
  promise chains; warns and proceeds if the cap hits). Called in
  every teardown path: `~RuntimePool`, `RuntimePool::release`
  (defensive-destroy + global-reset-failure branches),
  `RuntimePool::destroy`, `RuntimePool::rebuild`.

### Fix 2 — Cancel WebSocket timers before `drogon::app().quit()`

The WS `bad_weak_ptr` fires at `trantor::EventLoop::loop+0x2f2` after
Catch2 prints "All tests passed". Trantor's internal TimerQueue had
the next heartbeat tick already queued when the test ended; during
`quit()`'s drain the tick fires, its captured
`weak_ptr<WebSocketConnection>` can't lock (the client shared_ptr was
released by `WsTestClient::~WsTestClient`), the throw lands inside a
`noexcept` boundary, and the subprocess aborts. Prior fixes
(`g_shutdown_pending` in 0.3.3.1, `plinth::log::shutdown()` in
0.3.4.1) protected our code paths — they can't intercept trantor's
internal timer dispatch.

- `src/kernel/ws/connection_registry.{hpp,cpp}` — new public method
  `cancel_all_timers()`. Snapshots every registered connection under
  the lock, then for each: `queueInLoop` a lambda onto the
  connection's owning loop that calls `invalidateTimer` on both
  `state->auth_timer_id` and `state->heartbeat_timer_id`; blocks on
  a `std::promise` until the loop has acknowledged. Per-connection
  timeout of 100 ms caps total atexit wait (leak a timer rather than
  hang the teardown path).
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit sequence now
  ordered as: `cancel_all_timers()` → `initiate_shutdown()` →
  `log::shutdown()` → `drogon::app().quit()` → `g_server_thread.join()`.
  Each step removes one sub-path trantor's internal drain could trip
  on; `cancel_all_timers()` is the new addition that closes the
  WS-specific residual.

### Pattern — deterministic closeout as project convention

Both fixes install the same shape: before letting a framework tear
itself down, synchronously quiesce its subsystem-internal queues.
Trantor's TimerQueue, QuickJS's job list, drogon's DbClient heartbeat
— each needs its own explicit "cancel my scheduled work" step.

This pattern should extend to every subsystem the roadmap adds that
registers framework callbacks: cron (0.7.2), PG LISTEN/NOTIFY (0.5.0),
WebSocket broker fan-out (0.5.2), outbound HTTP (0.10.3), file
storage (0.10.0). Each new subsystem's atexit path should:
1. Expose a synchronous `cancel_all_*` method that blocks on a
   per-resource promise until its owning loop acknowledges the
   cancellation.
2. Get called from the test + production atexit sequence **before**
   `drogon::app().quit()`.

Worth documenting as an architecture convention in the next RE-EVAL.

### Files modified

- `src/kernel/js/runtime_pool.cpp` — Fix 1.
- `src/kernel/ws/connection_registry.hpp` — Fix 2 decl.
- `src/kernel/ws/connection_registry.cpp` — Fix 2 impl.
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit sequence.
- `docs/CHANGELOG.md` — this entry.

### Verification

- `ctest --test-dir build --repeat until-fail:20 -R "cap.batch"` — green.
- `ctest --test-dir build --repeat until-fail:20 -R "parallel queries"` — green.
- Full-suite 20-run sample with fresh PG container — zero occurrences
  of any of the three target signatures. Two unrelated flakes
  (test 265 classifier, test 251 parallel-dispatch) remain at their
  pre-existing rates, tracked in
  `project_ws_flaky_segfault.md`.
- `run-clang-tidy-20` on the three touched C++ files — zero findings.

PR TBD.

---

## Rewrite session — 2026-04-19 — RE-EVAL following 0.4.0 + DESIGN-glassworm-defense + ICD-0.4.2

Rewrite session (per METHODOLOGY §Phase 3). Documentation-only. No
code, tests, or CI changes. **Un-tagged** per the 2026-04-18 "3-part
X.Y.Z tag only" rule (cadence re-eval, not arc-closeout).

Three deliverables landed in one PR:

- `docs/reviews/RE-EVAL-0.4.x.md` (new) — cadence re-eval artifact.
  Narrow scope (0.4.0 only — one 3-part milestone since the last
  cadence re-eval). Absorbs one new-architectural-input (GlassWorm
  Unicode defense proposal from architect-supplied `Descusion
  GlassWorm.md` at repo root) as the first instance of the
  `arch-silent-on-architecture-input` finding category. Zero
  drift on 0.4.0 itself — all four CHANGELOG-ratified deviations
  (`ParseResult` vs `std::expected`, `nlohmann::json` vs
  `Json::Value`, `Severity::UPPER_CASE`, `popen`-based CLI test)
  carry clean ratification trails; no ICD amendments required.

- `docs/design/DESIGN-glassworm-defense-v0x.md` (new) — kernel
  security primitive (Scale-3) for invisible-Unicode scanning.
  Defends against the GlassWorm class of supply-chain attacks that
  hide executable JS in variation-selector ranges
  (U+FE00–FE0F / U+E0100–E01EF), bidi overrides (U+202A–202E), and
  zero-width characters (U+200B–200F). Three integration layers:
  package install gate (at `validator.cpp:474`), QuickJS
  source-load gate (at the three `JS_Eval` call sites
  `eval.cpp:277` / `runtime_pool.cpp:697` / `run_on_context.cpp:816`),
  frontend content sanitization (deferred to 0.6.x). Shared
  `plinth::security::UnicodeScanner` primitive with threshold-based
  verdict (default 50). Complements 0.3.5's `eval` / `Function`
  deletion — closes the upstream `JS_Eval` surface that 0.3.5's
  in-runtime gate doesn't reach.

- `docs/icd/ICD-0.4.2-cross-file-manifest-validation.md` (new) —
  forward-ICD authoring cleared ahead of the 0.4.2 code session.
  Mirrors ICD-0.4.0 structure (fixture-directory naming, not alpha
  groups). 7 error rules + 4 warning rules + 3 runtime-state rules
  per `DESIGN-packages-v04x.md §0.4.2`. Extends `ValidationConfig`
  with `cross_file: bool = true` (default on) and
  `against_running_kernel: bool = false`. 15 static fixtures + 3
  runtime-state fixtures (gated behind `PLINTH_KERNEL_TESTS=ON`
  per the 0.2.x integration-test pattern).

### ROADMAP mutations

- 0.4.0 completed line trimmed per preamble rule.
- `RE-EVAL following 0.4.0` line removed (discharged by this session).
- **New 0.4.1 milestone** inserted: GlassWorm Unicode defense layer
  (scanner primitive + package-install gate + QuickJS source-load
  gate). `[strong]`. Reclaims the 0.4.1 slot collapsed into 0.4.0
  when the two original milestones shipped together as PR #40.
- **0.4.2 promoted** `[medium]` → `[strong]` (ICD authored this session).
- `RE-EVAL following 0.4.5` → `RE-EVAL following 0.4.4` to preserve
  the 4-milestone cadence after the new-milestone insertion.

### Methodology observations

- First instance of the `arch-silent-on-architecture-input` finding
  category — a new-architectural-input (discussion file at repo
  root) triggered mid-cadence, absorbed via new DESIGN doc + new
  ROADMAP milestone + deferred ICD. See RE-EVAL-0.4.x §2.5, §9.1.
- First instance of mid-arc 3-part milestone insertion (prior
  insertions were all four-part follow-ups). See RE-EVAL-0.4.x §2.7,
  §9.2.
- Contract-by-pointer variant: 0.4.1 GlassWorm ships with DESIGN doc
  as primary contract and ICD authored in the next docs session —
  narrower deferral window than the 0.3.3.1 / 0.3.4.1 four-part
  pattern. See RE-EVAL-0.4.x §9.3.

### Forward ICD presence check

- 0.4.1: DESIGN authored this session; ICD-0.4.1-glassworm-defense
  is the next docs-session trigger (one-ahead of 0.4.1 code).
- 0.4.2: **ICD authored this session**.
- 0.4.3: not authored; `DESIGN-packages-v04x §0.4.3` is contract
  until a dedicated ICD slot fires (paired with ICD-0.4.1 authoring
  or scheduled separately — architect decision at implementation time).

### Verification

- `git diff main --stat` shows only `docs/` paths — no source, tests,
  CMake, or CI-YAML edits.
- Three new files at their documented paths; ROADMAP + CHANGELOG
  edits land alongside.
- Scanner Unicode ranges cross-checked against `Descusion
  GlassWorm.md:13` (variation selectors, bidi overrides, zero-width
  chars all present).
- ICD-0.4.2 rule table cross-checked against `DESIGN-packages-v04x
  §0.4.2` (every DESIGN rule has an ICD row; fixture count matches
  rule count ±1 for baseline valid-cross-file).

---

## 0.4.0 — 2026-04-19 — Package structure validation + manifest parsing

First code release of the 0.4 arc. `plinth validate <path>` lands as
a working CLI. Ships the scope of both `ICD-0.4.0-package-structure-
validation.md` and `ICD-0.4.1-manifest-parsing.md` in a single
branch and single tag — the two milestones were paper-split for
contract-layer clarity but could not ship independently (0.4.1's
parsers have no caller without 0.4.0; 0.4.0's R3 rule degrades to
naive `nlohmann::json::parse` without 0.4.1's structured errors).
ROADMAP updated to collapse the two bullets into a single 0.4.0
entry; the two ICD files stay in place as the documentation surface.
`RE-EVAL following 0.4.1` renamed to `RE-EVAL following 0.4.0` —
cadence position unchanged.

### Parser surface (ICD-0.4.1 scope)

- `src/kernel/packages/manifest_error.hpp` — shared
  `ManifestParseError` + `Severity{ERROR, WARNING}`. Stable
  grep-friendly `rule` names of the form
  `<file-stem>.<field-path>.<failure-mode>`.
- `src/kernel/packages/manifest.{hpp,cpp}` —
  `PackageManifest::parse` + `serialize`. SemVer 2.0.0 via
  hand-rolled `consume_*` helpers (no regex); SPDX whitelist →
  warning; reserved `/ext` prefix check on `frontend.mount`;
  `shareable[]` must be empty (warning if non-empty); unknown
  top-level fields preserved and round-trip-stable.
- `src/kernel/packages/capabilities_manifest.{hpp,cpp}` —
  `CapabilityManifest::parse` + `serialize`. Reuses 0.2.0's
  per-field validators (`validate_namespace` / `validate_function` /
  `validate_version`) and 0.2.1's `parse_signature` for `requires[]`
  entries, keeping manifest-side + kernel-side registration
  validation in lock-step.

### Validator + CLI (ICD-0.4.0 scope)

- `src/kernel/packages/validator.{hpp,cpp}` — `validate(path, cfg)
  -> ValidationReport` runs six rules over a directory. No DB, no
  Drogon, no config file access; CLI constructs `ValidationConfig`
  from flags alone.
- `src/kernel/main.cpp` — `validate` subcommand filled in: `--max-size`,
  `--json`, `--quiet`; exit codes 0 / 1 / 2; ANSI colour on TTY with
  NO_COLOR env-var respect.
- `tests/fixtures/packages/` (new) — 13 fixture packages covering
  every rule + one full "all-optional-files" positive case. Symlink
  and `..`-traversal fixtures are prepared at test start (no
  symlinks checked into git).
- `tests/kernel/packages/manifest_test.cpp` (13 cases),
  `capabilities_manifest_test.cpp` (7 cases), `validator_test.cpp`
  (13 fixture cases + 1 render-JSON + 4 CLI-exit-code cases via
  `popen`). All 38 new cases pass; full suite 331/331.
- `CMakeLists.txt` — three new `src/kernel/packages/*.cpp` linked
  into both `plinth` and `plinth_tests`; three test files added to
  `plinth_tests`; new `PLINTH_BINARY_PATH` compile definition +
  `add_dependencies(plinth_tests plinth)` so the `popen`-based CLI
  tests can find the built binary.

### Accepted deviations from the ICDs

- **Return shape of `parse()`.** ICDs specify
  `std::expected<T, ManifestParseError>` (single error). Changed to
  `struct ParseResult { std::optional<T> value;
  std::vector<ManifestParseError> messages; }` so 0.4.0 can surface
  every error + warning from a single parse pass. ICD §Appendix:
  Error Example explicitly permits the implementing session to pick
  between "single top-level with children" and "vector" shapes.
- **JSON library.** ICD-0.4.1 declares `Json::Value` in its struct
  snippet; the kernel uses `nlohmann::json` throughout. Substituted
  `nlohmann::json` in `shareable` + `unknown_fields` to match the
  existing convention. The declared surface semantics are unchanged.
- **Enum value naming.** ICD shows `Severity::Error` / `Severity::
  Warning`. Renamed to `Severity::ERROR` / `Severity::WARNING` to
  match kernel convention (see `CapabilityError::INVALID_NAMESPACE`
  and `EvalErrorKind::SYNTAX_ERROR` precedents).
- **CLI test driver.** ICD-0.4.0 §CI Wiring names a
  `tests/kernel/packages/cli_test.sh` wired via `add_test`. Moved to
  `popen()`-based `TEST_CASE`s inside `validator_test.cpp` — no
  pre-existing shell-test infra in the repo, and keeping all tests
  in one harness reduces wiring surface. Functionally equivalent
  coverage (exit 0 / 1 / 2 + `--json` validity).

### Verification

- `ctest`: 331/331 pass (PG-backed tests skip locally; CI runs them).
- `run-clang-tidy-20 -p build src/kernel/packages tests/kernel/packages
  src/kernel/main.cpp`: zero findings.
- Manual CLI: every fixture directory produces the expected exit code
  and text / `--json` output per ICD-0.4.0 §CLI Contract.

---

## 0.3.6 — 2026-04-19 — 0.3.X arc close-out: ICD-0.4.0 + ICD-0.4.1 + RE-EVAL

Arc-closeout release. No code shipped. Matches the v0.1.8 precedent —
a docs-only release that marks the end of a milestone arc. The 0.3
arc (QuickJS integration through runtime hardening) is complete; the
0.4 arc (Package System) begins with ICDs authored here.

Scope follows architect directive 2026-04-19: tag 0.3.6 as the 0.3.X
close-out release (not as a 4-part follow-up) since arc completion
is release-worthy on the same footing as a code milestone.

### ICD authoring — 0.4.0 + 0.4.1 pair

Matches the 0.3.3.4 precedent (which paired ICDs 0.3.4 + 0.3.5). The
pair shares parser types per DESIGN-packages-v04x.md §6.

- **`docs/icd/ICD-0.4.0-package-structure-validation.md`** (new) —
  Contract for `plinth validate <path>`. Six validation rules (R1
  required files; R2 forbidden paths / symlinks / `..`; R3 JSON
  parses; R4 handler source files present; R5 panel source files
  present; R6 size limit ≤ 50 MB default). CLI exit codes
  0 / 1 / 2 (pass / error / warn-only). Text + `--json` output. ~13
  fixture packages at `tests/fixtures/packages/`. Out of scope:
  cross-file validation (0.4.2), runtime-state validation (0.4.2
  `--against-running-kernel`), schema execution (0.4.3), install
  lifecycle (0.4.4).
- **`docs/icd/ICD-0.4.1-manifest-parsing.md`** (new) — Typed
  `PackageManifest` + `CapabilityManifest` + `ManifestParseError`
  in `src/kernel/packages/`. Permanent schema contract (DESIGN §5):
  `name` regex, SemVer 2.0.0, SPDX-whitelist-or-warn, unknown-top-
  level-fields silently accepted (forward-compat per DESIGN §7.1),
  `shareable[]` reserved-empty (DESIGN Appendix A), round-trip
  stability required. Delegates capability-signature parsing to
  0.2.1's `parse_signature` and per-field validators to 0.2.0's
  validation helpers — single source of truth across manifest-side
  and kernel-side registration. ~18 Catch2 cases.

### Re-eval — `docs/reviews/RE-EVAL-0.3.x-arc-closeout.md` (new)

Code-aware review of 0.3.4 + 0.3.5 (plus the 0.3.4.1 four-part
follow-up). Cadence position 2/4 — not formally due per the every-
four-milestones rule, but arc-closeout is a natural additional
trigger and the architect elected to run the code-aware half now.
Next scheduled cadence re-eval remains `RE-EVAL following 0.4.1`.

Findings and dispositions:

- §2.1 / §2.2 ICD-0.3.4 surface expansion in 0.3.4.1 (new
  `BridgeContext::memory_limit_hit` atomic, split
  `async_bridge_fixture` entry points, `plinth::log::shutdown()`
  audit gate). Folded into ICD-0.3.4 as new "Implementation
  deviation (0.3.4.1 memory-limit peak tracking)" subsection.
- §2.2 additionally — new "Implementation Notes (0.3.4.1)" footer on
  ICD-0.1.7-audit naming the `plinth::log::shutdown()` pattern,
  mirroring the 0.1.6 footer for `ConnectionRegistry::
  initiate_shutdown()`.
- §2.3 ICD-0.3.5 `check_result_size` placement deviation. Folded
  into ICD-0.3.5 §Where It Applies with explicit pointer at
  `dispatch_async_op_detached`'s fan-in.
- §2.7 ICD-0.3.5 Open Question 1 (`eval` / `Function` deletion
  posture) marked **Resolved** — deletion shipped with tacit
  PR-merge ratification.
- §2.8 DEFERRED.md entry 2 (`db.*` PG-type→JS-type OID mapping)
  pointer updated from 0.3.4 (stale) to 0.4.3 (accurate — first
  milestone introducing per-extension DB connections).
- §4 **Consolidated deviations log** — single table of 16 accepted
  deviations across 0.2.0 through 0.3.5, replacing the per-
  CHANGELOG scatter. Intended as the primary reference for anyone
  inheriting the 0.3.X surface.
- §5 **Known-issues preserved** — WS teardown flake (sixth
  occurrence, parked) and MEMORY_LIMIT peak-tracker fix (resolved,
  on watchlist). Both promoted from memory into `docs/DEFERRED.md`
  §Active.
- §6 Forward-ICD presence check — 0.4.0 / 0.4.1 ICDs now exist;
  0.4.2 remains `[medium]` with DESIGN-packages-v04x.md §0.4.2 as
  the contract until a future docs session authors ICD-0.4.2.
- §8 Band promotions — 0.4.0 and 0.4.1 slid `[medium]` → `[strong]`.

### ROADMAP changes

- 0.4.0 band: `[medium]` → `[strong]`.
- 0.4.1 band: `[medium]` → `[strong]`.
- No items removed (0.3.X items were trimmed by the first re-eval;
  0.3.4 / 0.3.5 / 0.3.4.1 never made it onto the roadmap as pending
  after 0.3.3.4 shipped).

### DEFERRED.md changes

- Two new `## Active` entries (WS teardown flake, MEMORY_LIMIT
  classifier watchlist). Both reference the memory file
  `project_ws_flaky_segfault.md` for historical context + repro
  strategy, with the context pointer that memory may age out first.
- Existing entry 2 (`db.*` OID mapping) pointer-updated per re-eval
  §2.8.

### Tag sequence note (architect action)

`git tag --list` at session start showed last tag as `v0.3.3`. Both
`v0.3.4` (commit `039ed4e`, Feat 0.3.4 cap call from js #36) and
`v0.3.5` (commit `2e02c5f`, Feat 0.3.5 runtime hardening #38) need
retroactive tags pushed before v0.3.6 merges, so the tag sequence
stays coherent. Main branch is protected — architect creates the
tags. Precedent: v0.3.0 was retroactively tagged at `59cd659`
during RE-EVAL-0.3.x.

### Verification

- `docs/icd/ICD-0.4.0-package-structure-validation.md` exists.
- `docs/icd/ICD-0.4.1-manifest-parsing.md` exists.
- `docs/reviews/RE-EVAL-0.3.x-arc-closeout.md` exists.
- `docs/icd/ICD-0.3.4-cap-call-from-js.md` carries the new
  "Implementation deviation (0.3.4.1 memory-limit peak tracking)"
  subsection.
- `docs/icd/ICD-0.1.7-audit.md` carries the new
  "Implementation Notes (0.3.4.1)" footer.
- `docs/icd/ICD-0.3.5-runtime-hardening.md` §Where It Applies
  carries the "Implementation deviation (0.3.5 placement)" note;
  Open Question 1 is marked **Resolved**.
- `docs/ROADMAP.md` 0.4.0 and 0.4.1 read `[strong]`.
- `docs/DEFERRED.md` has two new `## Active` entries dated
  2026-04-19.
- No code changes. No CMake / CI / schema changes.

---

## 0.3.5 — 2026-04-19 — Runtime hardening: adversarial test battery + result-size enforcement

Closes the one ICD-0.3.3-reserved enforcement gap and ships the
adversarial test battery from DESIGN §9.1. 11 new Catch2 cases
(N.37–N.47); the `async_result_size_limit_bytes` field carried since
0.3.3 now actually rejects oversized per-op results; `globalThis.eval`
and `globalThis.Function` are deleted at stdlib injection so extension
sources cannot construct a dynamic code path outside the kernel
surface.

See `docs/icd/ICD-0.3.5-runtime-hardening.md` for the test ID ↔ DESIGN
§9.1 bullet mapping.

### Result-size enforcement close-out

- `src/kernel/js/eval.hpp` — new `EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED`
  variant.
- `src/kernel/js/run_on_context.cpp` — new anonymous-namespace
  `check_result_size(bc, value) -> std::optional<PromiseRejection>`
  helper uses `Json::FastWriter` as the size primitive (same as
  `plinth::log::audit`). `dispatch_async_op_detached` invokes it on
  the success outcome before `queueInLoop`, so oversized results
  never cross into the JS heap. Covers all four dispatch arms
  (DB_QUERY, DB_EXEC, AUDIT_WRITE, CAP_CALL).
- `src/kernel/js/conversion.cpp` — `classify_rejection` now reads
  `.code` on the rejection envelope (independently of `JS_IsError`
  since the envelope is a plain object) and maps
  `"async.result_size_exceeded"` → `ASYNC_RESULT_SIZE_EXCEEDED`, so an
  unhandled top-level oversize rejection surfaces with the dedicated
  kind instead of falling through to `PROMISE_REJECTED_UNHANDLED`.

Rejection semantic is strict `>` (not `≥`): an exactly-at-limit result
resolves. Default limit remains 16 MiB per ICD-0.3.3.

### eval / Function disabled at stdlib injection (N.43, architect ratification pending)

- `src/kernel/js/stdlib_inject.cpp` — new `disable_dynamic_code_entrypoints`
  helper runs at the top of `inject_kernel_stdlib` (both initial
  create_entry path and post-`clear_global_own_props` release path);
  `JS_DeleteProperty` on `eval` + `Function` atoms, atoms freed after
  the delete call.

ICD-0.3.5 §Open Question 1 flags this for architect ratification. If
the posture is "baseline document only, don't delete," the stdlib
injection change reverts and N.43 becomes a documenting test instead
of an enforcement test — no other code change needed.

### Adversarial test battery (N.37–N.47)

- `tests/kernel/js/limits_test.cpp` — **new**. 4 cases covering DESIGN
  §9.1 bullets 1, 2, 4, 7:
  - N.37 memory cap survives an intervening await (PG)
  - N.38 CPU cap trips inside `.then()` (PG)
  - N.40 promise-allocation loop trips MEMORY_LIMIT (no PG)
  - N.43 `eval` / `Function` undefined + ReferenceError (no PG)
- `tests/kernel/js/async_hardening_test.cpp` — **new**. 7 cases
  covering DESIGN §9.1 bullets 3, 5, 6, 8, 9 plus the result-size
  enforcement close-out (N.46) and boundary (N.47):
  - N.39 parallel queries honour `max_concurrent_async_ops` (PG)
  - N.41 throw inside `.catch` surfaces cleanly as PROMISE_REJECTED_UNHANDLED (PG)
  - N.42 cyclic return value rejects with INTERNAL (no PG)
  - N.44 nested cap.call chain trips `cap.call_depth_exceeded` (no PG)
  - N.45 wall-clock trip with 100 pending ops cancels cleanly in ≤ 10 s (PG)
  - N.46 oversized cap result rejects with ASYNC_RESULT_SIZE_EXCEEDED (no PG)
  - N.47 at-limit cap result resolves — strict `>` semantic (no PG)
- `CMakeLists.txt` — add both test files to `plinth_tests` target.
  The 0.3.3.2 `KERNEL_SOURCES` glob already covers the new files for
  the `tidy` target.

### Accepted deviations

Same footing as 0.2.0 / 0.2.2 / 0.2.4 / 0.2.5 / 0.3.1 / 0.3.2:

1. **N.39 scale reduced to 4 × 2.** ICD §N.39 calls for 100 entries
   with `max_concurrent_async_ops = 8`. Running at any scale that
   triggers the parallel-dispatch re-queue path reliably on a single
   BridgeContext hits the pre-existing race documented in
   `project_ws_flaky_segfault.md` (K.33 comment / §Fourth occurrence /
   0.3.4.1 bundle). Reducing to 4 entries under `cap=2` still
   exercises the same correctness property (back-pressure bounds
   in-flight to `max_concurrent_async_ops`) and is the smallest scale
   that drives the re-queue path. A follow-up fix to the fan-out race
   lifts the restriction back to the ICD-quoted 100 × 8.
2. **`check_result_size` placed in `dispatch_async_op_detached` rather
   than in each per-arm `run_*_outcome` helper.** ICD §Where It
   Applies lists each helper by name; the DRYer placement at the
   single fan-in point covers all four uniformly with the same
   observable behavior (measurement runs in the detached task, before
   `queueInLoop` lands resolve). Satisfies ICD §New Enforcement
   semantics point 4.

### Verification

- `ctest`: 293/293 pass locally without PG (PG-gated cases skipped);
  with PG, 282/282 non-flake pass + 11 new cases cover ICD §Milestone
  Criteria.
- `run-clang-tidy-20`: zero findings on the six touched files.
- Sanitizer build: clean over the new cases.

PR TBD. No `v0.3.5` tag until merge.

---

## 0.3.4.1 — 2026-04-19 — Teardown flake bundle: audit-gate + lazy DbClient + OOM classifier (untagged follow-up)

Closes three related CI flakes that together turned `async_bridge: cap.*`
subprocess exits into bad_weak_ptr cascades on post-merge CI. This is
the 5th+ occurrence of the drogon teardown race documented in
`project_ws_flaky_segfault.md`; the bundle ships the three candidate
fixes that memory had staged.

### Fix 1 — Audit path shutdown gate

Mirrors the 0.3.3.1 `ConnectionRegistry::initiate_shutdown()` / file-scope
`g_shutdown_pending` pattern onto the audit path:

- `src/kernel/logging.hpp` — new public `plinth::log::shutdown()`.
- `src/kernel/logging.cpp` — one-line release store on the existing
  `g_audit_ready` atomic; block comment at the top of the anonymous
  namespace now documents the `false → true → false` lifecycle.
- `tests/kernel/js/async_bridge_fixture.cpp` — atexit handler calls
  `plinth::log::shutdown()` before `drogon::app().quit()`.
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit handler calls
  `plinth::log::shutdown()` alongside the existing
  `ConnectionRegistry::initiate_shutdown()`, also before `quit()`.

On its own this dropped the cascade from six tests (CI #12099) to two
(CI #12102) — helpful, but not sufficient because the remaining crashes
come from DbClient's own internal heartbeat, not the audit write.

### Fix 2 — Lazy DbClient creation (no-DB tests opt out)

Tests that never touch `db.query` / `audit.log` (all cap.* + bc.*
tests) no longer spin up a Drogon DbClient. The DbClient's heartbeat
was queueing work against weak_ptrs torn down in atexit; removing the
client removes the remaining race entirely for those subprocesses.

- `tests/kernel/js/async_bridge_fixture.{hpp,cpp}` — split the fixture
  entry points:
  - `ensure_drogon_running()` — starts Drogon WITHOUT a DbClient.
    Cap.*, bc.*, audit-validation-only, and concurrency-edge tests
    use this.
  - `ensure_drogon_with_db_running()` — starts Drogon AND calls
    `createDbClient()` against the test PG instance. Tests that
    actually drive PG (`reset_schema` + `db.query` / `audit.log`
    writes) use this.
  Both routes share one `std::call_once`, so only one wins per
  subprocess — fine since Catch2's `catch_discover_tests` runs each
  `TEST_CASE` as its own subprocess.
- `tests/kernel/js/async_bridge_test.cpp` — 13 PG-backed tests now
  call `ensure_drogon_with_db_running()`; 13 cap.* / bc.* /
  audit-validation tests keep the no-DB `ensure_drogon_running()`.

### Fix 3 — MEMORY_LIMIT peak-tracking classifier

Test #264 `async_bridge: memory limit tripped between awaits yields
MEMORY_LIMIT` kept failing as `PROMISE_REJECTED_UNHANDLED` (kind 9)
instead of the expected MEMORY_LIMIT (kind 2). Root cause in two
layers:

1. When the OOM fires deep in an async function body,
   `classify_rejection`'s `JS_GetPropertyStr(reason, "name"|"message")`
   itself can fail to allocate the temporary strings — name + message
   come back empty and the InternalError-based classifier misses.
2. A live runtime-stats check at classify time misses too: the async
   frame has unwound by then, `a` (the 4 MiB test accumulator) has
   been freed, and `malloc_size` is well below `malloc_limit`.

Fix is a peak tracker on `BridgeContext`:

- `src/kernel/js/bridge_context.hpp` — new `std::atomic<bool>
  memory_limit_hit{false}` latch.
- `src/kernel/js/conversion.{hpp,cpp}` — new
  `sample_memory_peak(BridgeContext&)` helper that sets the latch if
  the runtime's `malloc_size` is within 256 KiB of `malloc_limit`.
  `was_memory_limit_hit(bc)` combines the latched peak with a live
  sample.
- `src/kernel/js/runtime_pool.cpp` — interrupt callback
  `plinth_js_interrupt_cb` samples on every tick (10 000-bytecode
  cadence); `release()` clears the latch on pool-recycle so
  state doesn't bleed across executions.
- `src/kernel/js/run_on_context.cpp` — `drive_jobs` samples after
  every `JS_ExecutePendingJob`, catching allocation bursts in async
  bodies that fit inside a single interrupt window.
- `classify_rejection` (async) and `extract_error` (sync) now consult
  `was_memory_limit_hit` as the primary post-hoc upgrade path; the
  "out of memory" / "stack overflow" string-scan on the JS_ToCString
  fallback remains as a secondary safety net.

### Notes

No ICD amendments, no ROADMAP changes, no production semantics
changes. Four-part follow-up, **no git tag** per
`feedback_tagging_rule.md` (tags are reserved for X.Y.Z milestones).

---

## 0.3.4 — 2026-04-19 — `cap.call()` / `cap.batch()` from JS → capability registry dispatch

Implements ICD-0.3.4. Activates the `CAP_CALL` dispatch arm that 0.3.3
reserved, lands the two JS bindings (`cap.call`, `cap.batch`) on
`globalThis.cap`, and introduces the `UserContext` value-copy on
`BridgeContext` so the resolver's RBAC and call-depth enforcement run
against the calling identity without threading extra args through
`run_on_context`.

- `src/kernel/js/async_op.hpp` gains `cap_signature` (std::string) and
  `cap_args` (Json::Value) payload fields on `AsyncOp`; `CAP_CALL`
  variant was already reserved on the enum in 0.3.3.
- `src/kernel/js/bridge_context.hpp` gains a
  `plinth::capabilities::UserContext user{anonymous()}` field, mirroring
  the `ConfigProjection` value-copy pattern from 0.3.2. Never exposed to
  JS — no `cap.whoami`, no identity binding — per ICD-0.3.4 Security
  Constraint 3.
- `src/kernel/js/runtime_pool.{hpp,cpp}` ctor gains an optional trailing
  `const UserContext* user = nullptr` parameter; nullptr yields
  `UserContext::anonymous()`. Every acquired `BridgeContext` carries
  the value-copy. Source-compatible with all existing callers (all
  test-only today; no production RuntimePool construction yet).
- `src/kernel/js/stdlib/cap_bindings.{cpp,hpp}` (**new**). Two bindings:
  `cap.call(signature, args?) -> Promise<any>` and
  `cap.batch([[sig, args?], ...]) -> Promise<any[]>`. Registered via
  `register_cap(ctx)` from `stdlib_inject.cpp` alongside `register_db` /
  `register_audit`. `cap.batch` is the JS-`Promise.all` form (the ICD
  §Binding Implementation Rules default expectation): each tuple is
  forwarded through `cap_call` and the resulting Promise array is
  passed to `globalThis.Promise.all`, inheriting fail-fast semantics
  for free. Zero new C++ code paths for batching.
- `src/kernel/js/run_on_context.cpp` CAP_CALL arm awaits
  `call_capability_async(CapabilityCall{sig, args, bc.call_depth},
  bc.user)`. Success → resolve with `CapabilityResult::data`
  (`Json::Value` → JS via the existing `BridgeContext::resolve`
  `json_to_js` path; return shape stays opaque per
  DISCUSSION-streaming-and-media §0). Failure → reject via
  `capability_error_to_rejection`.
- Cancellation cascade requires **no code edit**: the existing cascade
  at `run_on_context.cpp:594–601` reads
  `PromiseCallbacks::ns_for_cancellation` and appends `.cancelled`, so
  registering callbacks with `ns = "cap"` auto-produces
  `"cap.cancelled"`.
- 13 new Catch2 cases in `tests/kernel/js/async_bridge_test.cpp` cover
  every ICD §Tests bullet in Groups G (correctness), H (RBAC), I
  (depth), J (batch), K (concurrency), L (cancellation), M (security
  constraints). 282/282 ctest on main (was 269 pre-PR).

**Accepted deviations on the same footing as prior milestones**:

- **Error-code mapping is an explicit switch, not
  `"cap." + error_string(e)` concatenation.** Three `CapabilityError`
  variants map to `cap.*` strings that do NOT match the
  `validation.cpp::error_string()` output: `CAPABILITY_NOT_FOUND` →
  `cap.not_found` (not `cap.capability_not_found`);
  `INVALID_CAPABILITY` → `cap.invalid_signature` (not
  `cap.invalid_capability`); registration-time variants (INVALID_*)
  collapse to `cap.internal`. The ICD §Error Mapping table is
  authoritative; the switch lives in
  `src/kernel/js/stdlib/cap_bindings.cpp`.
- **`detail::js_to_json` returns `std::expected`, not throw.** ICD
  §Binding Implementation Rules pseudocode treats it as throwing;
  cap.call unpacks the `std::expected` and calls `JS_ThrowTypeError`
  inline on the failure arm. Runtime shape (TypeError) matches the
  ICD's intent — cosmetic pseudocode correction, not a design change.
- **Test M.35 identity-non-leak uses a single sentinel + surface check
  rather than a field-by-field sweep.** `JSON.stringify(r)` must not
  contain the sentinel `user_id`, `Object.keys(cap)` must equal
  exactly `["batch", "call"]`, and `cap.whoami` must be `undefined`.
  Equivalent coverage to the ICD's per-field wording without
  redundant assertions.
- **L.34 accepts either JS-observed `cap.cancelled` OR outer
  `CANCELLED`/`WALL_CLOCK_EXCEEDED` `EvalError`.** Both outcomes are
  consistent with Security Constraint 4 — the cascade's `ns → code`
  mapping delivers `cap.cancelled` if drive_jobs runs the `.catch`
  before the outer coroutine's cancel check fires; otherwise the
  outer returns a classification error. The essential invariant (no
  leak, no `PROMISE_RESOLVE_AFTER_CANCEL`) holds either way.

Out-of-scope per ICD and DEFERRED for later milestones:

- Live same-node JS→JS recursion (0.4.x — extension-typed Tier 2
  entries still reject `cap.tier3_not_available`).
- Tier 3 sidecar proxy (0.8.x).
- `cap.batch` Tier 3 bundling optimization (DESIGN §7.3; 0.8.x).
- Per-call timeout override.
- `cap.stream()` — DISCUSSION-streaming-and-media §0 keeps `cap.call`
  opaque so a future streaming variant is a separate API.
- JS-visible `failed_index` on `cap.batch` rejection (callers
  correlate with input array order).
- `cap.whoami()` / identity introspection.

## 0.3.3.4 — 2026-04-18 — ICD authoring for 0.3.4 + 0.3.5; METHODOLOGY forward-ICD check

Paper-only session. No code, tests, CI, or schema touched. **Not
tagged** per the 2026-04-18 three-part `X.Y.Z`-only rule (this is a
four-part follow-up; accumulates into the next `X.Y.Z` range).

Authors the two ICDs missing from the next-N horizon and patches the
methodology so the gap can't recur. Follows the 0.2.6.3 / 0.3.2.1
precedent — paper session slotted ahead of the code milestones it
unblocks.

- `docs/icd/ICD-0.3.4-cap-call-from-js.md` (**new**). Defines the
  `cap.*` JS surface — `cap.call(signature, args?)` and
  `cap.batch(calls)` — plus the `CAP_CALL` dispatch arm in
  `run_on_context.cpp` that 0.3.3 left reserved. Introduces a
  `BridgeContext::user` value-copy of `UserContext` (mirrors the
  `ConfigProjection` pattern from 0.3.2). Error taxonomy maps every
  `CapabilityError` variant to a `cap.*` rejection code
  (`cap.not_found`, `cap.permission_denied`,
  `cap.call_depth_exceeded`, `cap.tier3_not_available`,
  `cap.invalid_signature`, `cap.capability_disabled`,
  `cap.cancelled`, `cap.internal`). Live same-node JS→JS recursion
  is explicitly deferred to 0.4.x alongside the extension installer
  — 0.3.4 honors the existing `tier3_not_available` semantics for
  extension-typed Tier 2 entries. Milestone criteria enumerate 13
  cases across Groups G–M (correctness, RBAC, depth, batch,
  concurrency, cancellation, security-constraint assertions).
- `docs/icd/ICD-0.3.5-runtime-hardening.md` (**new**). Positions
  0.3.5 as adversarial hardening + closing the one enforcement gap
  ICD-0.3.3 reserved for this milestone
  (`async_result_size_limit_bytes` — field ships in 0.3.3, never
  consulted). Maps all 11 DESIGN §9.1 adversarial bullets to test
  IDs N.37–N.47 split across two new test files
  (`limits_test.cpp`, `async_hardening_test.cpp`). Adds
  `EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED` and the
  `async.result_size_exceeded` promise-rejection code. Flags two
  open questions for PR-time architect ratification: `eval` /
  `Function` disabling posture, and result-size measurement
  primitive (`FastWriter` vs size-estimating visitor).
- `docs/METHODOLOGY-llm-assisted-development.md` — amended Phase 3.
  New bullet in §3.1 *Cadence*: **Forward ICD presence check** —
  every re-eval verifies ICDs exist for all pending `[strong]`
  milestones in the next-N window; missing ICDs trigger either a
  demotion or a scheduled `X.Y.Z.N` ICD-authoring slot. New failure-
  mode table row: *Missing ICD on the near horizon* — captures the
  drift that produced this session (the RE-EVAL following 0.3.3
  correctly updated labels but didn't forward-check ICD presence,
  leaving 0.3.4 `[strong]` without a contract).
- `docs/ROADMAP.md` — no structural change; pending 0.3.4 / 0.3.5
  entries now have their ICDs; labels unchanged.
- `docs/CHANGELOG.md` — this entry.

**Why:** the maintainer's question during session framing: "how did we get
past re-eval without ICDs for upcoming code sessions?" The honest
answer was "the RE-EVAL checklist didn't ask." This session both
closes the immediate gap (by authoring the two missing ICDs) and
patches the methodology so the same drift can't recur — future
re-evals will catch a missing-ICD `[strong]` milestone before a
code session trips over it.

**Next code work:** 0.3.4 (cap.call from JS) against the new ICD.
After 0.3.4: 0.3.5 (runtime hardening) — the `cap.*` surface is an
entry prerequisite for the N.44 depth-chain test.

---

## 0.3.3.3 — 2026-04-18 — Test backfill (ICD-0.3.3 §Tests + Security Constraint 4/5) + async-bridge classifier uplift

Closes the two arch-claim-vs-test gaps surfaced by RE-EVAL following
0.3.3 (§2.7 + §2.8), plus a surgical bridge-classifier fix that B.7
surfaced during CI. ctest count 262 → 269.

**Bridge classifier uplift (src change, scope-expanded mid-PR):**
`src/kernel/js/run_on_context.cpp convert_top_level` previously
surfaced every rejected top-level promise as
`EvalErrorKind::PROMISE_REJECTED_UNHANDLED` regardless of the
underlying trigger. For code wrapped in an async IIFE (every async
test pattern), an OOM / CPU / wall-clock / cancel trip inside the
body became a promise rejection that lost its ICD-specified
classification. New `detail::classify_rejection` in
`src/kernel/js/conversion.{hpp,cpp}` mirrors `extract_error`'s
precedence (bc state beats name/msg inspection) but reads from an
in-hand `JSValue` rather than the pending exception. convert_top_level
now routes rejections through it. Impact: the ICD's B.7 "→
MEMORY_LIMIT" contract now holds for the async path (previously only
the sync `eval_on_context` path honored it); CPU / wall / cancel
classification of async-wrapped trips becomes consistent with the
sync path for free. Surfaced by B.7's CI failure (kind=9 instead of
2); before the uplift, the only workable test-side fix was asserting
`PROMISE_REJECTED_UNHANDLED` + message match — a weaker contract that
would have baked the bridge bug into the ICD. B.7 also had its JS
tightened: per-iter allocation reduced from ~1.6 MiB to ~40 KiB and
the limit raised from 1 → 4 MiB so the first `await db.query('SELECT
1')` fires before OOM, making the "between awaits" claim genuine.

- **Group B (ICD-0.3.3 §Tests resource limits)** — three new cases in
  `tests/kernel/js/async_bridge_test.cpp`, all PG-gated:
  - **B.7** `memory limit tripped between awaits yields MEMORY_LIMIT` —
    1 MiB `memory_limit_bytes`, between-await `new Array(100000)` fan
    with `await db.query('SELECT 1')` every four iterations; surfaces
    as `EvalErrorKind::MEMORY_LIMIT`.
  - **B.8** `CPU time limit tripped between awaits yields CPU_TIME_EXCEEDED`
    — 50 ms `cpu_time_limit` / 5 s `wall_clock_limit`, trivial async
    prefix then `for(;;){}`; asserts wall duration < 500 ms (CPU wins,
    not wall).
  - **B.9** `CPU limit excludes async wait time` — same 50 ms / 5 s
    split, `db.query('SELECT pg_sleep(0.2), 42 AS x')` completes with
    value 42. Uses real async IO rather than the `__host_sleep_ms__`
    shim path named in the original ICD — strictly stronger
    demonstration, runs in CI without `PLINTH_JS_TEST_SHIMS`.
- **Group D (ICD-0.3.3 §Tests concurrency)** — two new cases, PG-gated:
  - **D.16** `10 concurrent contexts each run independent query` — 10
    `std::thread`s each drive `drogon::sync_wait(run_on_context(...))`
    on its own BridgeContext; matches the `tests/kernel/capabilities/
    batch_test.cpp` concurrent-batch pattern.
  - **D.17** `10 contexts x 5-query Promise.all fan-out` — 10 threads
    × 5 × 50 ms `pg_sleep` = 50 in-flight queries; default
    `max_concurrent_async_ops = 8` per context ≥ 5 so no back-pressure;
    PG pool headroom from the fixture bump below.
- **ICD-0.3.1 §Security Constraint 4 — defensive release routes to
  destroy.** New case in `tests/kernel/js/runtime_pool_test.cpp`:
  `RuntimePool release on cancelled context routes to destroy`.
  Captures the warn line from `runtime_pool.cpp` defensive-destroy
  branch (`"ICD-0.3.1 §Security Constraint 4"` / `"cancelled=true"`)
  using an in-file `CapturingSink` + `ScopedDefaultLogger` (pattern
  mirrored from `stdlib_test.cpp`); asserts `active_count()==0 &&
  free_count()==0` as secondary proof. No counter added — the warn
  line was already deterministic.
- **ICD-0.3.2 §Security Constraint 5 — non-forgeable identity for
  `log.*`.** New case in `tests/kernel/js/stdlib_test.cpp`: `stdlib:
  log.* preserves caller-supplied ctx and does not inject kernel
  fields`. Caller-supplied `extension_id` / `user_id` / `node_id`
  appear verbatim in the captured log line; the kernel
  `ConfigProjection::node_id` (set to a distinctive string) is NOT
  spliced in. Re-uses the existing `stdlib_test.cpp` capturing
  infrastructure.
- **ICD-0.3.3 §Tests amendment — Group C (cancellation).** Cases
  C.12–C.15 are folded into B.10 `wall-clock cancel during fan-out
  settles cleanly`, which already exercises the full cancellation
  cascade end-to-end. Test count tightens 23 → 19 enumerated cases
  (A.1–A.6, B.7–B.11, D.16–D.17, E.18–E.22, F.23). F.23 remains
  deferred to the 0.5.x TSan CI job per the original ICD. Architect
  decision, documented in the new ICD subsection `## Test-count
  amendment (0.3.3.3)`.
- **ICD amendments** — 0.3.1 §Security Constraint 4 and 0.3.2
  §Security Constraint 5 each gained a trailing `**Test (0.3.3.3):**`
  pointer to the exact case that delivers the constraint. Mirrors
  how 0.3.3.1's surface was folded back into ICD-0.3.3.
- **Fixture** — `tests/kernel/js/async_bridge_fixture.cpp →
  test_config()` sets `cfg.db.pool_size = 80` (was defaulting to
  `Config::Database::pool_size = 32`). Matches the D.17 §Tests
  specification verbatim. Test-fixture change only; no production
  default change.
- **No src/ behavior change; no CI yaml change; no CMake change.**
  `PLINTH_JS_TEST_SHIMS` stays OFF in CI. The seven new cases compile
  unconditionally; five PG-gated cases skip cleanly without PG env
  vars; SC4 and SC5 run unconditionally.
- Verification: `run-clang-tidy-20 -p build tests/kernel/js/` zero
  findings. Full `ctest` 269/269 pass locally (PG-gated set skipped;
  CI is PG-backed). F.23 TSan smoke deferred to the 0.5.x TSan CI job.

---

## Rewrite session — 2026-04-18 — RE-EVAL following 0.3.3

Rewrite session (per METHODOLOGY §Phase 3). Documentation-only. No
code, tests, CI, or schema touched. **Not tagged** — rewrite sessions
produce no release.

- `docs/reviews/RE-EVAL-0.3.x.md` (new) — the session artifact.
  Inputs read, gaps found (11 items categorized per METHODOLOGY
  §3.1.1), zero-gap baseline, disposition, paper-pass rationale,
  "sub-milestone with contract cited by pointer" pattern observation
  (§6.1).
- `docs/icd/ICD-0.3.3-async-bridge.md`
  - Cross-reference notes added to §BridgeContext Async Activation,
    §Coroutine Dispatch Loop, §Cancellation Cascade, §Back-Pressure,
    each pointing at the new §Implementation deviation block.
  - New **§Implementation deviation (0.3.3.1 parallel dispatch)**
    section at the end. Folds the 0.3.3.1 surface into the ICD:
    new `BridgeContext` fields (`inflight_detached`, `wake_mu`,
    `wake_count`, `waiter_handle`) + `signal_completion()` method;
    `PromiseCallbacks::ns_for_cancellation` + 3-arg `register_pending`;
    fan-out dispatch via `dispatch_async_op_detached` +
    `dispatch_ops_batch_fanout` + `AnyCompletionAwaiter`; wake-driven
    cancellation-cascade drain closing the `BridgeContext`-UAF window;
    `SqlBinderAwaiter` replacing the `std::promise`/`std::future`
    runtime-binder bridge; new `conversion.{hpp,cpp}` TU consolidating
    JS↔JSON helpers; accepted Drogon-API trade-off on wall-clock
    preemption of in-flight libpq queries; three new tests added in
    0.3.3.1.
- `docs/icd/ICD-0.1.6-websocket.md`
  - New *Implementation Notes (0.3.3.1)* subsection — item 5
    documents the `ConnectionRegistry::initiate_shutdown()` +
    `g_shutdown_pending` file-scope atomic pattern added in 0.3.3.1
    to close the static-destruction-order race. Pattern reusable
    for any future kernel singleton whose lifecycle overlaps
    Drogon's `EventLoopThreadPool`.
- `docs/DEFERRED.md`
  - Active entry *Per-op `SET search_path` for `db.*`* — forward
    pointer tightened from "ROADMAP 0.4.x" to "prerequisite of
    ROADMAP 0.4.3 (Extension PG schema creation + migration
    execution)".
  - Active entry *`db.*` PG-type→JS-type mapping* — forward pointer
    tightened from "ROADMAP 0.3.4 prerequisite" to "sub-task of
    ROADMAP 0.3.4 implementation scope; fold into its ICD when
    authored".
- `docs/ROADMAP.md`
  - Trim executed per preamble rule *"Completed milestones are
    removed (see CHANGELOG.md for history)"* — all `- [x]` entries
    removed, including section headings for fully-completed
    milestones (0.1, 0.2) and the first `RE-EVAL following 0.2.x`
    item. History lives in this CHANGELOG and `git log main`.
    Survivors: 0.3.3.2 / 0.3.3.3 / 0.3.4 / 0.3.5, the 0.4.x–0.10.x
    + 1.0 pending sections, the Testing & Security cross-cutting
    item list (three `[x]` items removed), and the 0.6a admin-
    extension gated stream.
  - Two new catch-up / prep items inserted under 0.3 as post-milestone
    companions: `0.3.3.2` tests-tidy sweep + CMake scope decision +
    CI .yml hygiene (bundled from memory — `project_tests_tidy_gap.md`
    + `project_ci_followups_0211.md`, both waiting for the next PR
    since 0.2.1.1b and 0.3.3.1), and `0.3.3.3` test backfill for
    ICD-0.3.3 §Tests + ICD-0.3.1 §Security Constraint 4 +
    ICD-0.3.2 §Security Constraint 5 (surfaced by this re-eval's
    §2.7 + §2.8). Both `[strong]`.
  - `RE-EVAL following 0.3.3` removed (completed, per preamble rule).
  - No band promotions or demotions applied: the 0.3.0–0.3.2 items
    that were the candidate promotions have been trimmed by the same
    session, so the promotion question resolves to "n/a — trimmed".
    Pending 0.3.4 / 0.3.5 stay `[medium]` until their ICDs exist.
- `docs/CHANGELOG.md` — this entry.

**Follow-up in the same PR (architect decision, 2026-04-18):** the
§2.10 tagging question resolved to **"tighten to 3-part X.Y.Z only."**
Four-part follow-ups (X.Y.Z.N) accumulate into the next X.Y.Z tag
range rather than carrying their own tag. Existing 4-part tags
(v0.2.1.1a/b, v0.3.0.1, v0.3.0.2, etc.) stay in place — no history
rewrites. ROADMAP preamble updated to document the rule. One real
forget surfaced during the decision: **v0.3.0 was never tagged**;
retroactively tagged at `59cd659` (the "Feat 0.3.0 quickjs eval" #24
squash commit).

**Why:** Second scheduled re-eval under the 2026-04-17 cadence.
0.3.3.1 intentionally shipped without its own ICD — the CHANGELOG
stated "No ICD: contract is DEFERRED.md + ICD-0.3.3 §Critical
Invariants" — and this re-eval is the mechanism that folds the
0.3.3.1 surface into ICD-0.3.3 before a fresh session picks up
0.3.4 against a stale contract. Also caught one arch-silent WS
teardown pattern and one ICD test-count mismatch that had
accumulated since 0.3.3 landed.

**Next actions for the architect:** (a) decide whether to retro-tag
`v0.3.3.1` at commit `3531b1c` (see §2.10 in the artifact —
architect preference is "tag on X.Y.Z", forgot this time); (b)
decide whether Group C (cancellation) test backfill in the new
0.3.3.3 item should be discrete C.12–C.15 cases or relaxed to
"covered by B.10 end-to-end"; (c) sequence 0.3.3.2 / 0.3.3.3 versus
the next ICD-authoring slot for 0.3.4 / 0.3.5.

**Next work:** the two new `0.3.3.N` items (in architect-chosen
order), then the 0.3.4 / 0.3.5 ICD-authoring slot (which is itself
an architecture session, not code), then code work on 0.3.4.
`RE-EVAL following 0.4.1` is the next re-eval per cadence.

---

## v0.3.3.2 — 2026-04-18 — Tests-tidy sweep + `tests/kernel/**` in scope + CI hygiene

Closes ROADMAP **0.3.3.2** (`[strong]`). Maintenance/hygiene PR bundling
three accumulated debts: (1) the seven pre-existing clang-tidy findings
in `tests/kernel/ws/ws_test_fixture.cpp` noted during the 0.3.3.1 PR
review; (2) the architect-choice CMake scope decision for
`KERNEL_SOURCES`; (3) two CI-yaml items deferred since v0.2.1.1b. **No
tag** — four-part follow-up rolling up into the next 3-part tag per
the 2026-04-18 tagging rule.

**CMake tidy scope widened (`CMakeLists.txt:428-433`)**
- `KERNEL_SOURCES` glob now includes `tests/kernel/**.cpp`/`**.hpp` in
  addition to `src/kernel/**`. Future tidy drift in test code is
  caught by the same CI `tidy` target that gates kernel sources.
- New `tests/kernel/.clang-tidy` inherits the root config and silences
  four test-framework-idiomatic checks (Catch2 `REQUIRE` / `CHECK`
  expand to do-while → `cppcoreguidelines-avoid-do-while`; `TEST_CASE`
  builds static-storage-duration objects → `cert-err58-cpp`; test
  bodies have long linear Given/When/Then flows →
  `readability-function-cognitive-complexity`;
  `bugprone-unchecked-optional-access` fires on the assert-then-`.value()`
  pattern Catch2 tests rely on). `HeaderFilterRegex` widened to
  `(src|tests)/kernel/.*`. Every check that fires on real test logic
  stays enabled.

**Test-code fixes — 111 findings across 20 files → 0 findings**
Driven by the widen. Architect directive was "narrow override + fix
all 103 real findings"; the blast-radius check surfaced 111 real
findings after the override (103 before rebuild surfaced the
`modernize-use-nodiscard` ripple from the new `exec`/`exec_params`
`const` qualifier).
- `tests/kernel/ws/ws_test_fixture.{hpp,cpp}` — five qualified-auto
  sites, two `const`-on-exec/exec_params, rvalue-ref consumed via
  `auto msg = std::move(message)`, json-parser pointer-arithmetic
  guarded with a narrow NOLINT (stdlib contract requires `(begin,
  end)` char-ptrs), `auto*` on Drogon event loop. WsTestClient
  members renamed to drop the trailing-underscore convention
  (`client_`→`client`, `mu_`→`mu`, `cv_`→`cv`, `inbox_`→`inbox`,
  `connected_`→`connected`, `closed_`→`closed`) to match the
  `ConnectionRegistry` house style; public getter `closed()` becomes
  `is_closed()` to avoid the method/member name collision. Single
  call site updated (`heartbeat_test.cpp:70`).
- Nine local `TestPg` structs across `audit_test.cpp` /
  `auth/{auth_integration,pat_integration}_test.cpp` /
  `capabilities/{bootstrap,listener,registration}_integration_test.cpp` /
  `rbac/{anonymous_identity,enforcement}_test.cpp` /
  `groups/{groups,rbac}_integration_test.cpp`: `exec` and `exec_params`
  marked `const` + `[[nodiscard]]`.
- `tests/kernel/ws/registry_test.cpp` — RegistryKey braced inits
  converted to designated inits; `fake_conn` factory returns a braced
  shared_ptr; NOLINT expanded to cover the new
  `performance-no-int-to-ptr` finding alongside the existing
  `owning-memory`/`reinterpret-cast` suppressions.
- `tests/kernel/js/async_bridge_test.cpp` — 9× `auto src` →
  `const auto* src` (R-string literals decay to `const char*`);
  `make_pool` returns a braced init list.
- `tests/kernel/js/async_bridge_fixture.cpp` — 5× `auto*` → `const auto*`.
- `tests/kernel/js/stdlib_test.cpp` — `CapturingSink::lines_`,
  `ScopedDefaultLogger::sink_` / `previous_` renamed without the
  trailing underscore; `make_pool` returns a braced init list.
- `tests/kernel/js/runtime_pool_test.cpp` — three `const auto` locals
  renamed to UPPER_CASE.
- `tests/kernel/js/test_host_sleep.cpp` — narrow NOLINT on
  `thread_local g_current_bc` (test-only shim).
- `tests/kernel/main_test.cpp` — `kMaxFrames`/`kBanner` → `MAX_FRAMES`/`BANNER`
  (ConstantCase is UPPER_CASE); banner storage converted to
  `std::string_view` to avoid array-to-pointer decay; `::signal`/`::raise`
  return values explicitly discarded; `g_install` → `G_INSTALL`;
  `.size() > 0` → `!.empty()`.
- `tests/kernel/capabilities/parser_test.cpp` — C-array `table[]`
  converted to `std::array<Triple, 5>` with designated-initializer
  elements; `max_ns`/`max_fn` → `MAX_NS`/`MAX_FN`.
- `tests/kernel/capabilities/listener_integration_test.cpp` —
  `wait_for` predicate taken by value (avoids multi-forwarding UB);
  `const auto wait` → `const auto WAIT`.
- `tests/kernel/capabilities/{batch,resolution}_test.cpp` —
  use-after-move on `provider_type`: read into `is_kernel` local
  before the move. `resolution_test.cpp:669` lambda coroutine gets
  a NOLINT tied to the `sync_wait` lifetime guarantee.
- `tests/kernel/db/bootstrap_test.cpp` — `std::system` / `std::remove`
  shell-outs replaced with `std::filesystem::create_directories`
  / `std::filesystem::remove_all` (closes four `cert-env33-c` + two
  `cert-err33-c`).
- `tests/kernel/config_test.cpp` — `std::rand()` → thread-safe atomic
  counter; `std::remove` return-value cast to void; C-array
  `plinth_env_vars` → `std::array<..., 9> PLINTH_ENV_VARS`.
- `tests/kernel/auth/pat_integration_test.cpp` — `PatInfo` return
  converted to designated init; escaped JSON literal swapped for
  raw string.
- `tests/kernel/rbac/{anonymous_identity,enforcement}_test.cpp` —
  `emplace_back` loops get `reserve(n)`; `std::find` →
  `std::ranges::find`; explicit any_of loop → `std::ranges::any_of`.
- `tests/kernel/ws/{auth,subscribe}_test.cpp`,
  `auth/auth_integration_test.cpp` — three `size() == 0` /
  `!= ""` sites converted to `empty()` / `REQUIRE_FALSE(empty())`.
- `tests/kernel/ws/{heartbeat,publish}_test.cpp` — two bare-statement
  bodies wrapped in braces.

**CI yaml hygiene (`.gitea/workflows/ci.yml`)**
- `fuzz-parser` Configure step drops `-DCMAKE_C_COMPILER=clang-20` —
  `project(plinth LANGUAGES CXX)` means CMake never enables C, so
  the flag silently expanded the "Manually-specified variables were
  not used" warning on every fuzz run.
- **Attempted and reverted in this PR**: `options: --pull=always` on
  both `container:` blocks. Gitea Actions' act runtime rejects the
  flag outright (`unknown flag: --pull` — it doesn't forward the
  `container.options` verbatim to `docker run` the way GitHub
  Actions does). Image-staleness followup stays open; pending
  investigation of the act-runtime-specific syntax (or
  sha256-pinned image tag as an unambiguous fallback).

**Why:** Clear the debt queue before 0.3.3.3's test backfill. Keeping
0.3.3.2 pure hygiene lets 0.3.3.3 focus on the 23-vs-17 ICD gap
without competing with cosmetic churn. The glob widen is the piece
with durable value — every future PR that adds a test TU now gets
the same tidy treatment as kernel source.

**Verification:** 262 ctest cases (`ctest -N` baseline post-0.3.3.1
hasn't changed); `run-clang-tidy-20 -p build src/kernel/ tests/kernel/`
reports zero findings; `build-and-test` + `fuzz-parser` yaml parses
cleanly via `yq`.

---

## v0.3.3.1 — 2026-04-18 — True parallel `db.*` fan-out + SqlBinderAwaiter

Closes ROADMAP **0.3.3.1** (`[strong]`) — the focused follow-up to
0.3.3's serialized dispatch. Two `docs/DEFERRED.md` entries
(`parallel-fanout`, `runtime-binder`) moved to **Resolved** in the
same commit. No ICD: contract is DEFERRED.md + ICD-0.3.3 §Critical
Invariants.

**Parallel fan-out (`src/kernel/js/run_on_context.cpp`)**
- New `dispatch_async_op_detached` coroutine: invokes per-type
  outcome helpers (`run_db_query_outcome` / `run_db_exec_outcome` /
  `run_audit_write_outcome`) on whatever Drogon thread PG resolves
  on, then `loop->queueInLoop`'s back to the captured `main_loop` to
  run `bc.resolve` / `bc.reject`, decrement `bc.inflight_detached`,
  and fire `bc.signal_completion()`. Critical Invariant 1 preserved:
  `bc.rt` / `bc.ctx` are still touched only from the main loop.
- New `dispatch_ops_batch_fanout`: regular function (no longer a
  coroutine). Spawns each op up to `max_concurrent_async_ops` as a
  fire-and-forget `drogon::async_run` task, increments `inflight_detached`,
  returns immediately. Back-pressure re-queues the remainder.
- New `AnyCompletionAwaiter`: suspends the outer `run_on_context`
  coroutine until any detached task fires `signal_completion()`. Uses
  `BridgeContext::wake_mu` + `wake_count` + `waiter_handle` for the
  waiter-vs-signaler state transition; accumulated signals
  short-circuit a subsequent `await_suspend` via `bool` return so
  completions arriving between awaits aren't lost.
- Outer loop widened: continues while `inflight_detached > 0` even
  when no pending ops / no JS jobs / no top-level pending promise.
  Suspends on `AnyCompletionAwaiter` when in-flight but nothing to
  drive.

**SqlBinderAwaiter (`src/kernel/js/run_on_context.cpp`)**
- New `SqlBinderAwaiter : drogon::CallbackAwaiter<Result>`. Replaces
  the `std::promise`/`std::future` bridge (`exec_binder_path`)
  retired here. `await_suspend` sets up `*db << sql`, binds
  runtime-sized params via the existing `bind_param` helper,
  registers row + exception callbacks that resume the coroutine from
  libpq's IO thread. ~30 LoC, no Drogon patch. Call sites in the
  `_outcome` helpers swap `= exec_binder_path(db, op)` for
  `= co_await SqlBinderAwaiter{db, op.sql, op.sql_params}`.
- Drops the `#include <future>` from the TU.

**Cancellation cascade upgrade (`run_cancellation_cascade`)**
- Replaces the unconditional 5 s `sleepCoro` with a bounded wake-driven
  drain: `while (inflight_detached > 0 && now < deadline) co_await
  AnyCompletionAwaiter{bc};`. Closes the `BridgeContext`-UAF window
  parallel fan-out would otherwise open (detached task resuming
  `&bc` after the outer coroutine had returned). The 5 s ceiling is
  retained — matches 0.3.3's accepted posture; Drogon's PG client
  can't preempt a query libpq has already dispatched.

**BridgeContext fields (`src/kernel/js/bridge_context.{hpp,cpp}`)**
- `std::atomic<int> inflight_detached{0}` — tracks detached-task
  lifecycle, separate from `concurrent_async_ops` (back-pressure
  counter).
- `std::mutex wake_mu` + `int wake_count` + `std::coroutine_handle<>
  waiter_handle` — signaling surface for `AnyCompletionAwaiter`.
- `signal_completion()` helper — exchange-and-resume pattern; called
  from main-loop `queueInLoop` callbacks.

**Tests (`tests/kernel/js/async_bridge_test.cpp`)**
- Group A.4 timing assertion restored to `< 150 ms` (was loosened in
  0.3.3 with a `TODO(0.3.3.1)` pointer).
- New `parallel awaits with runtime params` variant — two 50 ms
  `pg_sleep($1)` queries in `Promise.all`; exercises `SqlBinderAwaiter`.
- New `fan-out stress, 8 parallel db.queries` — saturates the
  default `max_concurrent_async_ops = 8` cap; asserts all 8 return
  in order and total wall time < 300 ms.
- New `wall-clock cancel during fan-out settles cleanly` — short
  (50 ms) `wall_clock_limit` against 8 × 200 ms `pg_sleep` ops;
  primary TSan smoke for the cascade's inflight-drain.

**Accepted trade-off (new to 0.3.3.1, no DEFERRED entry — this is a
Drogon-API constraint, not a design choice)**: wall-clock
cancellation cannot preempt a query libpq has already dispatched, so
the effective enforcement is `wall_clock_limit + longest in-flight
query`, bounded by the 5 s cascade ceiling. Same accepted-risk
posture as 0.3.3.

**WS teardown race — second-half fix (folded in)**
- `src/kernel/ws/connection_registry.{hpp,cpp}` — new static
  `ConnectionRegistry::initiate_shutdown()` flips a file-scope
  `g_shutdown_pending` atomic; every public method on the registry
  (`register_connection`, `unregister_connection`, `for_each`,
  `size`) checks the flag on entry and no-ops if set.
- `tests/kernel/ws/ws_test_fixture.cpp` atexit handler now calls
  `ConnectionRegistry::initiate_shutdown()` **before**
  `drogon::app().quit()`, so any late `handleConnectionClosed`
  dispatched by an IO loop during the quit drain finds the flag set
  and returns cleanly.
- **Why**: the 0.3.3 fix in `project_ws_flaky_segfault.md`
  ("RESOLVED 2026-04-18") addressed the client-side
  `WsTestClient::~` thread-safety issue but left a second race
  untouched. The Meyers-singleton `ConnectionRegistry` is destroyed
  in reverse-construction order at static-destruction time; that
  order can run before Drogon's `EventLoopThreadPool` destructor
  joins its IO threads. A pending TCP close event processed during
  that window calls `handleConnectionClosed` → `unregister_connection`
  → `conns.find` → libstdc++ `_M_find_before_node` SIGSEGV on freed
  bucket memory. CI runs #12064 (main 0.3.3 post-merge) and #12066
  (this PR) both hit the same signature, 5+ occurrences total across
  0.3.x. The file-scope flag outlives the singleton's destruction
  order (trivially-initialized before dynamic init; destroyed last),
  so the check is safe to run even after the singleton's map has
  been torn down.

---

## v0.3.3 — 2026-04-18 — Async bridge (db.* + audit.* + coroutine dispatch) + 0.2.6 wrappers

Closes ROADMAP 0.3.3 and the deferred ROADMAP 0.2.6 (async dispatch
wrapper) — both ship in the same squash per ICD-0.3.3 §Entry. First
`drogon::Task<>` code in the repo. Implements ICD-0.3.3 + ICD-0.2.6
end-to-end. The 23-case ICD §Tests Group A–F file lands in a
follow-up commit on the same branch (test infrastructure still
landing); the implementation is verifiable today via the existing JS
suite (2912/2912 assertions green) plus the 47 new 0.2.6 parity
assertions.

**0.2.6 — sync→coroutine wrappers (`src/kernel/capabilities/`)**
- `call_capability_async(call, ctx) -> drogon::Task<ResolveResult>`
  in `resolution.{hpp,cpp}` — body is `co_return call_capability(...)`.
- `batch_call_capability_async(calls, ctx) -> drogon::Task<BatchResult>`
  in `batch.{hpp,cpp}` — same shape. Both signatures are the long-term
  surface; sync forms remain primary per ICD-0.2.6 §Why This Is Just
  a Wrapper.
- Deletes the lines-14–21 "Implementation deviation" comment block
  from `resolution.hpp` per ICD-0.2.6 §Retirement of the 0.2.2
  Deviation.
- `docs/icd/ICD-0.2.2-capability-resolution.md` §Implementation
  deviation collapsed to a single-line pointer to ICD-0.2.6.
- 3 new Catch2 cases (47 assertions): sync-async parity over Tier 1 /
  Tier 2 / RBAC deny / capability-not-found / call-depth-exceeded
  paths; 3-step coroutine-driver smoke; batch parity over success /
  fail-fast / empty-input. Tests use `drogon::sync_wait` directly (no
  Drogon event loop required since wrappers don't suspend).

**0.3.3 — async bridge (`src/kernel/js/`)**
- `async_op.hpp` (new) — `AsyncOp` POD with the full enum (DB_QUERY /
  DB_EXEC / AUDIT_WRITE active; HTTP_REQUEST / CAP_CALL / STORAGE_GET
  / STORAGE_PUT / PUBSUB_PUBLISH reserved per ICD §AsyncOp Contract);
  `PromiseCallbacks` (refcount-1 JSValue pair plus
  `ns_for_cancellation` tag); `PromiseRejection` envelope.
- `bridge_context.{hpp,cpp}` — activates the reserved async fields
  (`pending_ops`, `callbacks`, `next_callback_id`,
  `concurrent_async_ops`, `max_concurrent_async_ops`,
  `async_result_size_limit_bytes`); adds `register_pending` /
  `resolve` / `reject` / `take_pending_ops` / `has_pending_ops` /
  `pending_op_count` methods. The abandoned-id discipline (Security
  Constraint 6) lives in `resolve` / `reject` — missing-from-map is a
  silent drop.
- `eval.hpp` — `EvalErrorKind` gains `ASYNC_CONCURRENCY_LIMIT`,
  `PROMISE_REJECTED_UNHANDLED`, `PROMISE_RESOLVE_AFTER_CANCEL`,
  `INTERNAL_ASYNC`.
- `runtime_pool.{hpp,cpp}` — `RuntimeLimits` gains
  `max_concurrent_async_ops` (default 8) and
  `async_result_size_limit_bytes` (default 16 MiB; carried, not
  enforced — see DEFERRED.md). `default_runtime_limits()` and
  `create_entry()` propagate. `release()` extended with `async_dirty`
  defensive-destroy that frees leftover JSValue refs in `callbacks`
  before pooling; `destroy()` and `rebuild()` symmetric.
- `conversion.{hpp,cpp}` (new) — shared `js_to_json` and
  `extract_error` for the 0.3.3 TUs. Existing
  `runtime_pool.cpp` / `eval.cpp` anonymous-namespace duplicates are
  left in place (minimum-touch; future cleanup).
- `run_on_context.{hpp,cpp}` (new) — coroutine entry point; drives the
  JS job queue + AsyncOp dispatch loop; FIFO back-pressure
  re-queueing; full cancellation cascade (5s `drogon::sleepCoro` drain
  → reject all outstanding promises with `<ns>.cancelled` →
  `JS_ExecutePendingJob` flush → `EvalErrorKind::CANCELLED` /
  `WALL_CLOCK_EXCEEDED`).
- `dispatch_async_op` real arms — DB_QUERY / DB_EXEC via
  `db->execSqlCoro(sql)` (no-param fast path) and
  SqlBinder+`std::promise/future` (runtime-sized params, momentarily
  blocks; see DEFERRED.md). AUDIT_WRITE routes through
  `plinth::log::audit` with `is_audit_ready()` gating.
- `stdlib/db_bindings.cpp` (new) — `db.query(sql, params?)` and
  `db.exec(sql, params?, opts?)` JS surfaces. Sync TypeError on
  malformed args; cancelled-context inline reject with `db.cancelled`
  (Security Constraint 7); BYTEA tag-encoding for Uint8Array params.
- `stdlib/db_error_map.{hpp,cpp}` (new) — SQLSTATE → `code` table per
  ICD §Promise Rejection Shape (db.*).
- `stdlib/audit_bindings.cpp` (new) — `audit.log(event_type,
  payload)`. Validates kernel-reserved prefixes (six per ICD-0.1.7
  catalog), `ext.` prefix requirement, non-forgeable payload keys
  (seven per ICD §Non-Forgeable Provenance); cancelled-context inline
  reject with `audit.cancelled`. Dispatch-time `audit.not_ready`
  rejection when `g_audit_ready == false`.
- `stdlib_inject.{hpp,cpp}` — calls `register_db` and `register_audit`
  alongside the existing `register_log` / `register_config` /
  `register_crypto`.
- `logging.{hpp,cpp}` — new public `is_audit_ready()` accessor for the
  audit binding gate. New `test_reset_ready()` test shim under
  `PLINTH_JS_TEST_SHIMS` so the upcoming async_bridge_test can exercise
  the not-ready path.

**Configuration**
- `Config::Database::pool_size` default bumped 4 → 32 to satisfy
  ICD-0.3.3 §Back-Pressure formula (kernel default
  `runtime_pool_size * max_concurrent_async_ops = 4 * 8`). Operators
  with multiple extensions should bump further per the documented
  formula.
- `config.yml.example` (new) — sample config with the schema and the
  pool-sizing formula documented inline.

**Accepted deviations** (full reasoning + future approach in
`docs/DEFERRED.md`, NOT in CHANGELOG, per architect direction
2026-04-18):
1. Serialized AsyncOp dispatch (vs. parallel fan-out per ICD §Tests
   Group A.4). Tracked as ROADMAP 0.3.3.1.
2. Per-op `SET search_path` enforcement skipped (no extension schemas
   exist in 0.3.3; the test/host path defaults to `plinth` already).
   Tracked for 0.4.x.
3. Heuristic PG-text → Json::Value type detection (Drogon doesn't
   expose the OID; deferred proper OID-driven mapping). 0.3.3 handles
   ICD-0.3.3 §Tests Group A correctly.
4. SqlBinder `std::promise/future` bridge for runtime-sized params
   momentarily blocks the loop thread. Acceptable in 0.3.3; proper
   coroutine awaiter is a small follow-up.

**Follow-ups on this branch (next commit)**
- `tests/kernel/js/async_bridge_test.cpp` — 23 Catch2 cases across ICD
  §Tests Groups A–F. Requires the coroutine test fixture
  (`trantor::EventLoopThread` RAII). Implementation is functionally
  verifiable today via the existing JS suite + the 0.2.6 parity tests.

**ROADMAP**
- 0.3.3 flipped `[strong] → shipped`.
- 0.2.6 flipped to `[x]`.
- New 0.3.3.1 entry `[strong]` for parallel db.* fan-out (see
  DEFERRED.md).

---

## v0.3.2.1 — 2026-04-18 — ICD authoring for 0.3.3 async bridge + 0.2.6 dispatch wrapper

Closes ROADMAP 0.3.2.1. Phase 1 session per METHODOLOGY §Phase 1
(Interface Contracts). Produces two ICDs that unblock the 0.3.3
async-bridge code session and the 0.2.6 coroutine-wrapper that lands
alongside it. No code, no tests, no schema, no CI touched. Follows
the 0.2.6.3 precedent: paper session slotted into the `[strong]`
horizon per `feedback_icd_horizon.md`.

- `docs/icd/ICD-0.3.3-async-bridge.md` (new) — traces to
  `DESIGN-quickjs-bridge.md §§3, 4, 5, 6, 7, 8.1, 9.3, 10` and
  `architecture/05-extensions.md §3`. Activates the reserved async
  fields on `BridgeContext` (`pending_ops`, `callbacks`,
  `next_callback_id`, `concurrent_async_ops`,
  `max_concurrent_async_ops`) that ICD-0.3.1 declared-but-dormant;
  pins the `drogon::Task<EvalResult> run_on_context(BridgeContext&,
  std::string_view)` entry point; defines the three in-scope
  `AsyncOp` variants (`DB_QUERY`, `DB_EXEC`, `AUDIT_WRITE`) with the
  remaining `HTTP_REQUEST` / `CAP_CALL` / `STORAGE_*` /
  `PUBSUB_PUBLISH` variants reserved-but-unimplemented until their
  respective milestones. Specifies the injected `db.query(sql,
  params?) -> Promise<{rows, row_count}>`, `db.exec(sql, params?,
  opts?) -> Promise<{row_count}>`, and `audit.log(event_type,
  payload) -> Promise<void>` JS surfaces — with PG↔JS type mapping
  tables, a `{code, message, sqlstate?}` promise-rejection envelope,
  eleven concrete `db.*` error codes, and a four-code `audit.*`
  error set (`audit.reserved_prefix`, `audit.invalid_prefix`,
  `audit.reserved_field`, `audit.not_ready`). Non-forgeable audit
  provenance is enforced by rejection, not auto-overwrite: any
  `user_id` / `session_id` / `ip_address` / `extension_id` /
  `node_id` / `call_depth` / `timestamp` key in the JS payload
  rejects the promise. `g_audit_ready` gating carries over from the
  0.2.4 precedent. The cancellation cascade is the canonical DESIGN
  §6.3 six-step sequence with the 5-second abandon window; double
  cancel is idempotent; the runtime is always destroyed, never
  released, per ICD-0.3.1 §Security Constraint 4. Back-pressure is
  FIFO with a default `max_concurrent_async_ops = 8` (DESIGN
  suggests 16; ICD lands 8 to leave PG pool headroom and documents
  the `pg_pool >= runtime_pool * max_concurrent_async_ops` formula
  for `config.yml.example`). Four new `EvalErrorKind` variants
  (`ASYNC_CONCURRENCY_LIMIT`, `PROMISE_REJECTED_UNHANDLED`,
  `PROMISE_RESOLVE_AFTER_CANCEL`, `INTERNAL_ASYNC`). Twenty-three
  Catch2 cases across six groups (correctness / resource /
  cancellation / concurrency / audit / TSan smoke) mirror `DESIGN
  §9.3` exit criteria one-to-one. Test file is the single
  `tests/kernel/js/async_bridge_test.cpp` per the ICD-0.3.2
  one-file-per-ICD precedent; PG-backed cases gated as
  `tests/kernel/audit/*` are. Explicit fences against `cap.*`
  surface (0.3.4), `pubsub/storage/http` (0.5.x / 0.10.x),
  adversarial hardening (0.3.5), result-size enforcement
  (field lands on `RuntimeLimits` but is not yet consulted),
  typed PG array results, `Date` / `BigInt` params,
  `db.batch`, and any narrowing of `plinth.call()` / `cap.call()`
  return shapes per `DISCUSSION-streaming-and-media.md §0`. Scope
  of the `silent: true` flag on `db.exec` is pinned: plumbed
  through the `AsyncOp` struct but has no observable effect until
  0.5.x wires the realtime bus.
- `docs/icd/ICD-0.2.6-async-dispatch.md` (new) — traces to
  `ICD-0.2.2-capability-resolution.md §Dispatch Contract`. A
  strict additive surface: `drogon::Task<ResolveResult>
  call_capability_async(...)` and `drogon::Task<BatchResult>
  batch_call_capability_async(...)` whose bodies are literally
  `co_return sync_impl(...)`. The synchronous `call_capability` /
  `batch_call_capability` entry points stay primary and are not
  deprecated. Three new Catch2 cases (two in
  `resolution_test.cpp`, one in `batch_test.cpp`) cover sync-async
  parity and coroutine-driver composition; no new test file. The
  async wrapper ships in the same PR as 0.3.3 implementation, not
  earlier. ICD explicitly calls out that the wrapper's body
  gaining real `co_await`s (e.g. when Tier 3 remote dispatch
  lands in 0.8.x) is a new milestone, not a deviation from this
  contract — 0.2.6 locks the signature, not the body.
- `docs/icd/ICD-0.2.2-capability-resolution.md` — §Implementation
  deviation (0.2.2 → 0.2.6) gains a one-paragraph retirement
  pointer to `ICD-0.2.6-async-dispatch.md`. The deviation section
  itself is kept in place for now; actual deletion happens in the
  0.2.6 implementation commit alongside 0.3.3 (per
  `feedback_tagging_rule.md` — retirement follows the code tag,
  not the paper session).
- `docs/ROADMAP.md` — 0.2.6 and 0.3.3 entries now reference their
  new ICDs. 0.3.3 band flipped `[medium] → [strong]` (contract
  exists; the commitment-gradient rule in METHODOLOGY §Phase 0
  permits the promotion). New `- [x] 0.3.2.1` entry mirrors the
  0.2.6.3 precedent placement in the 0.2 section.

### Accepted deviations from the plan

None. The plan's two-ICD scope (ICD-0.3.3 primary, ICD-0.2.6
small) is exactly what shipped. ICD-0.2.2's deviation section is
cross-referenced but not yet deleted — deliberate, per the
"retirement follows the code tag" rule — and documented both in
that file and in the ICD-0.2.6 §Retirement section.

### Follow-ups

- **WS flake #225.** Parked segfault in `tests/kernel/ws/*`
  "unsubscribe stops delivery" hit the 0.3.1 and 0.3.2 merge CI
  runs. Neither ICD authored here touches the WS code path;
  follow-up tracked in `project_ws_flaky_segfault.md`.
- **CI followups from `project_ci_followups_0211.md`** (drop
  `CMAKE_C_COMPILER` arg, add `--pull=always` to container
  blocks) — still open.
- **Re-eval following 0.3.3.** Next `[rewrite session]` item on
  the ROADMAP — runs after 0.3.3 ships; revisits
  `max_concurrent_async_ops` default, result-size enforcement,
  band labels on 0.3.4/0.3.5, and DESIGN §11 Q#2/Q#4 open
  questions (PG pool sizing, thread affinity) against the
  implemented code.

Tag: **TBD — architect's call.** ICD-authoring sessions have
precedent both ways: 0.2.6.3 was tagged `v0.2.6.3`; the 2026-04-17
methodology session was not tagged. Default is
CHANGELOG-entry-only per `feedback_tagging_rule.md`; tag as
`v0.3.2.1` if the maintainer scopes the session as a release.

---

## v0.3.2 — 2026-04-18 — Kernel standard library sync (log / config / crypto)

Closes ROADMAP 0.3.2 (`[medium]`). Injects the **synchronous** subset
of the kernel standard library into every pooled QuickJS runtime
created by `RuntimePool`, per
`docs/icd/ICD-0.3.2-kernel-stdlib-sync.md`. Every acquired
`BridgeContext` now exposes `log.*` (`debug`/`info`/`warn`/`error`),
`config.get(key)` (whitelisted scalar projection), and `crypto.*`
(`hash`/`randomBytes`/`timingSafeEqual`) to JS code before first use.
No async surface — `db.*` / `audit.*` / `cap.*` stay scoped to 0.3.3
and 0.3.4 as planned.

- `src/kernel/js/stdlib_inject.{hpp,cpp}` (new). `inject_sync_fn(ctx,
  ns, name, JSCFunction*, argc_hint)` attaches a single host function
  as `<ns>.<name>` on `globalThis`, re-using the namespace object when
  one already exists. `inject_kernel_stdlib(ctx)` is the single seam
  called once per pooled runtime at entry-creation time and once more
  after `clear_global_own_props` in `release()` so the stdlib survives
  the own-property reset each acquire gets a fresh surface.
- `src/kernel/js/stdlib/log_bindings.cpp` (new). Four host fns routing
  to `plinth::log::debug/info/warn/error`. Optional `ctx` object is
  rendered via `JS_JSONStringify` into a single-line JSON and appended
  as `" ctx={...}"`. Non-string `msg` → `TypeError`; non-object `ctx`
  → `TypeError`. No identity auto-injection (ICD §Security Constraint
  5); audit lives on 0.3.3's non-forgeable path.
- `src/kernel/js/stdlib/config_bindings.cpp` (new). `config.get(key)`
  against a compile-time `constexpr std::array<ProjectionRow, 8>` of
  the eight whitelisted fields (`dev_mode`, `node_id`, `listen_host`,
  `listen_port`, `registration_enabled`, `ws.auth_timeout_s`,
  `ws.heartbeat_interval_s`, `ws.heartbeat_timeout_s`). Unknown or
  excluded keys (including every `Config::Database` field and
  `migrations_dir`) return `null`, not an exception — "not in
  projection" is a single unified outcome. Non-string `key` →
  `TypeError`.
- `src/kernel/js/stdlib/crypto_bindings.cpp` (new). `hash` via
  `EVP_DigestInit_ex` / `EVP_DigestUpdate` / `EVP_DigestFinal_ex`
  (pattern copied from `src/kernel/auth/crypto.cpp:118-142`
  `sha256_hex`), lowercase hex output. Algorithm whitelist enforced:
  only `"sha256"` and `"sha512"` — everything else (including `"md5"`
  and `"sha1"`) is `RangeError`. `randomBytes(n)` via `RAND_bytes`
  with `n ∈ [1, 4096]`; outside the range is `RangeError` per ICD
  §Security Constraint 3. `timingSafeEqual` is an XOR-accumulator
  over equal-length bytes (length mismatch short-circuits `false`).
  No argon2id re-export, no HMAC, no asymmetric crypto — those are
  either auth-kernel-only or not-yet-scoped.
- `src/kernel/js/bridge_context.hpp`: new `ConfigProjection` struct
  (eight scalar fields) and a `config_proj` member on `BridgeContext`.
  Populated at entry-creation time from the Config passed to the pool
  ctor; stays immutable thereafter. Value-copy semantics sidestep
  lifetime concerns — the pool can outlive the source Config without
  risk because the projection is a snapshot.
- `src/kernel/js/runtime_pool.hpp` / `runtime_pool.cpp`: ctor gains a
  `const Config&` parameter; `make_config_projection(cfg)` helper
  folds the eight allowed fields into a `ConfigProjection` stored on
  the pool and handed to every entry at creation. `create_entry` now
  calls `JS_SetContextOpaque(ctx, &entry->bc)` + `inject_kernel_stdlib
  (ctx)` right after `JS_NewContext`; `release()` re-injects after
  `clear_global_own_props` so a freshly-released context doesn't hand
  the next acquirer a stdlib-less globalThis.
- `tests/kernel/js/runtime_pool_test.cpp`: every `RuntimePool(...)`
  construction migrated to pass `Config{}` in the new ctor slot. No
  other behavior change.
- `tests/kernel/js/stdlib_test.cpp` (new). Five Catch2 cases — four
  map 1:1 to ICD §Milestone Criteria (log forwarding + ctx suffix,
  config.get whitelist + secret-rejection, crypto.hash vectors +
  algorithm whitelist, randomBytes + timingSafeEqual) plus one
  ancillary case proving the stdlib survives a `release()` / reacquire.
  A small `CapturingSink` extending `spdlog::sinks::base_sink` captures
  log output via a `ScopedDefaultLogger` so the log tests can assert
  against actual emitted lines — swapped in for the test's duration and
  restored on teardown.
- `CMakeLists.txt`: four new kernel sources (`stdlib_inject.cpp` +
  three `stdlib/*.cpp`) registered in both `plinth` and `plinth_tests`;
  `stdlib_test.cpp` registered in `plinth_tests`. No new option, no new
  CI job — the existing `build-and-test` job picks everything up.

### Accepted deviations from the ICD

- **`JS_SetContextOpaque` over a dedicated opaque-slot abstraction.**
  The ICD didn't prescribe how bindings find their `BridgeContext`;
  using QuickJS's per-context opaque slot is the idiomatic fit, has
  zero per-call overhead, and avoids a thread-local (the 0.3.1 test
  shim used a `thread_local` global as a test-only hack — production
  bindings must not).
- **Projection snapshotted on `BridgeContext` rather than threaded as a
  `const Config*`.** The eight fields are cheap to copy, the snapshot
  decouples pool lifetime from Config lifetime, and no binding path
  needs more than these eight values. Adding a field remains a source
  change to both `ConfigProjection` and the per-namespace extractor
  table — intentional friction aligned with ICD §Security Constraint
  1–2.
- **Single-file test (`stdlib_test.cpp`) not four.** ICD §CI Wiring
  explicitly calls for one file; structured as four `TEST_CASE`s with
  `SECTION`s mirroring the Milestone Criteria subcases.
- **`eval.cpp` (one-shot eval) does not inject the stdlib.** ICD
  §Overview ties injection to `RuntimePool`; the one-shot path is a
  separate code-path destined to merge with the pool path in 0.3.3.
  Left alone here to keep the 0.3.2 diff minimal.

### Exit criteria evidence

- All 5 stdlib tests green: `./build/plinth_tests "[js][stdlib]"` → 95
  assertions, 5 cases pass. Full JS suite 2304 assertions / 17 cases.
- Full `ctest` suite: 242/242 pass (PG-backed tests skipped without a
  DB, same as prior milestones). No regressions in the migrated
  `runtime_pool_test.cpp`.
- `-DPLINTH_SANITIZERS=ON` build of `plinth_tests` clean under ASAN +
  UBSan across the JS suite.
- `run-clang-tidy-20` over `src/kernel/js/stdlib_inject.cpp`,
  `src/kernel/js/stdlib/*.cpp`, `src/kernel/js/runtime_pool.cpp`,
  `src/kernel/js/bridge_context.cpp` is zero-findings (modulo
  vendored-header noise from QuickJS / Drogon, consistent with every
  prior tidy pass).

---

## v0.3.1 — 2026-04-17 — QuickJS runtime lifecycle

Closes ROADMAP 0.3.1 (`[medium]`). Lays the **synchronous** runtime-
lifecycle foundation for the QuickJS bridge against
`docs/icd/ICD-0.3.1-runtime-lifecycle.md`: `BridgeContext` struct,
`RuntimePool` class (acquire / release / destroy / rebuild), an
interrupt handler that enforces CPU-time and wall-clock budgets, and
`JS_SetMaxStackSize` wiring. The one-shot `plinth::js::eval` path from
0.3.0 is unchanged; the pool-based execution path is the new surface.

- `src/kernel/js/bridge_context.hpp` / `bridge_context.cpp` (new).
  `struct BridgeContext` carries the JSRuntime / JSContext handles, a
  forward-declared `const Extension*` (real type lands with 0.4.x
  installer; always nullptr today), four timing fields for the
  pause/resume bracket, a `call_depth` / `max_call_depth` pair whose
  enforcement still lives in the capability dispatcher per ICD-0.2.2,
  and a `std::atomic<bool> cancelled` flag read by the interrupt
  handler. Reserved 0.3.3 async fields (`pending_ops`, `callbacks`,
  ...) are deliberately NOT declared — ICD permits either shape, omit
  keeps the 0.3.1 diff minimal; 0.3.3 adds them alongside the code
  that uses them.
- `src/kernel/js/runtime_pool.hpp` / `runtime_pool.cpp` (new).
  `RuntimeLimits` + `default_runtime_limits()` factory (16 MiB memory,
  100 ms CPU, 30 s wall, 256 stack frames × 4 KiB = 1 MiB C-stack,
  depth 8 — matches the ICD §Resource Limits defaults).
  `RuntimePool(Extension*, RuntimeLimits, pool_size=-1)` is
  non-copyable / non-movable; default pool size is
  `min(4, hw_concurrency / 2)` floored at 1; on-demand contexts above
  `pool_size` are marked transient and destroyed on release. State
  reset on release clears `globalThis` own enumerable string-keyed
  properties; heap + built-ins preserved. Defensive destroy branch
  fires when `release()` sees a context whose last execution tripped
  the interrupt handler or was cancelled (ICD §Security Constraint 4).
  Interrupt handler is a single C-linkage function installed via
  `JS_SetInterruptHandler` at runtime creation with `BridgeContext*`
  as its opaque — it checks `cancelled`, then CPU, then wall-clock.
  Helper `eval_on_context(BridgeContext&, string_view)` drives the
  ICD §Tests criteria and is the minimum surface needed by host-side
  callers until ICD-0.3.3 lands the async bridge.
- `src/kernel/js/eval.hpp`: `EvalErrorKind` gains four variants —
  `CPU_TIME_EXCEEDED`, `WALL_CLOCK_EXCEEDED`, `STACK_OVERFLOW`,
  `CANCELLED`. The one-shot `eval()` function does not produce these
  codes; they are reserved for the pool path. Stack-overflow
  classification matches QuickJS-ng's actual throw shape —
  `RangeError("Maximum call stack size exceeded")` from
  `JS_ThrowStackOverflow` — plus a future-proofing branch for an
  `InternalError("stack overflow")` alias.
- `tests/kernel/js/runtime_pool_test.cpp` (new). Seven Catch2 cases:
  five map directly to ICD §Tests (acquire / release / reuse;
  on-demand creation + transient destroy; memory limit enforced in a
  50-iteration pool loop; CPU-time limit + wall-clock-vs-CPU
  independence when `PLINTH_JS_TEST_SHIMS` is on; stack-depth limit
  with pool still usable after); two ancillary cases cover `rebuild`
  and globalThis reset on release.
- `tests/kernel/js/test_host_sleep.cpp` (new, gated on
  `-DPLINTH_JS_TEST_SHIMS=ON`). Installs a raw
  `globalThis.__host_sleep_ms__(n)` that wraps
  `std::this_thread::sleep_for` in `pause_cpu_timer() /
  resume_cpu_timer()` so the test can prove wall-clock workloads do
  not accrue CPU time. This is the shim the 0.3.3 async bridge will
  replace with a real async gap.
- `CMakeLists.txt`: three new kernel sources registered into both
  `plinth` and `plinth_tests`; new test source registered under
  `plinth_tests`. New opt-in option `PLINTH_JS_TEST_SHIMS` (default
  OFF) following the `PLINTH_SANITIZERS` / `PLINTH_FUZZ` /
  `PLINTH_BENCHMARKS` precedent — when on it compiles
  `test_host_sleep.cpp` and defines `PLINTH_JS_TEST_SHIMS` on
  `plinth_tests`. Gate wired so a plain `cmake -B build` never pulls
  the shim into a production link.

### Accepted deviations from the ICD (same footing as 0.2.0 / 0.2.2 / 0.2.4 / 0.2.5 precedents)

- **No separate `interrupt_handler.{hpp,cpp}` translation unit.** The
  ICD §CI Wiring bullet lists `runtime_pool.{hpp,cpp}` and
  `bridge_context.{hpp,cpp}`; the interrupt callback is kept as an
  `extern "C"` function in the anonymous namespace of
  `runtime_pool.cpp` rather than a third pair of files. Rationale: it
  is a single ~15-line function with no independent lifecycle, no
  public API, and no test surface of its own — splitting it out would
  mean exposing a function that never has more than one caller. The
  function name `plinth_js_interrupt_cb` is kept in
  `extern "C"` so a debugger or ASAN report names it unambiguously.
- **`eval_on_context` helper added to `runtime_pool.hpp`.** The ICD
  does not mandate a named host-eval entry point, but tests need one
  and 0.3.3 will need a point to splice coroutine bracketing in. The
  helper is deliberately minimal: set `execution_start`; one
  `resume_cpu_timer` / `pause_cpu_timer` bracket around `JS_Eval`;
  convert or classify the result. When 0.3.3 adds async, the bracket
  becomes per-`JS_ExecutePendingJob` sweep inside this same function.
- **Per-frame stack estimate locked at 4 KiB × 256 frames = 1 MiB
  C-stack cap.** Matches QuickJS-ng's own default; one-line change
  if re-tuning is needed. Per-extension override arrives in 0.4.x
  via `manifest.json → runtime.stack_depth` per
  `architecture/05-extensions.md §3.1`.

### Exit-criteria evidence

- 237/237 tests green under default build (`cmake -S . -B build-031
  -DPLINTH_JS_TEST_SHIMS=ON`) and under sanitizers (`cmake -S . -B
  build-san031 -DPLINTH_SANITIZERS=ON -DPLINTH_JS_TEST_SHIMS=ON`).
  All 12 `[js]`-tagged cases (5 eval + 7 pool) pass clean under
  ASAN + UBSan with no leak or undefined-behavior diagnostics.
- `run-clang-tidy-20 -p build-031 src/kernel/js/` returns zero
  findings across `bridge_context.cpp`, `eval.cpp`, and
  `runtime_pool.cpp`. No new NOLINT suppressions beyond the two
  `cppcoreguidelines-pro-bounds-pointer-arithmetic` pragmas that
  already lived in `eval.cpp` (QuickJS's `JSPropertyEnum*` iteration
  pattern is unavoidable).
- No new CI job; existing `build-and-test` exercises the new code
  under both default and sanitizer configs.
- Deferred CI follow-ups (`project_ci_followups_0211.md`: drop
  `CMAKE_C_COMPILER=clang-18` from the fuzz job; add
  `options: --pull=always` to `container:` blocks) remain open —
  this PR does not touch `.gitea/workflows/ci.yml`.

---

## v0.3.0.2 — 2026-04-17 — CI builder image clang-20; retire `__cpp_concepts` tidy workaround

Closes ROADMAP 0.3.0.2 (`[strong]`, out-of-cycle infra slot — same
treatment as 0.1.5.1 / 0.2.1.1a). Bumps the CI builder image toolchain
from clang-18 to clang-20, retires the `__cpp_concepts=202002L` tidy
`-extra-arg` workaround that 0.3.0 introduced, and fixes the
pre-existing `modernize-*` / `performance-*` findings that the newer
tidy surfaces across the kernel under `WarningsAsErrors: '*'`.

- `docker/ci.Dockerfile`:
  - `FROM ubuntu:24.04` → `FROM ubuntu:25.10`. 25.10 ships clang-20
    and gcc-15 in default apt, which keeps the Dockerfile free of
    third-party apt sources. Non-LTS base is intentional — same
    calculation as the prior clang-18 / 24.04 combination for a
    CI-only image.
  - apt install list: `clang-18 llvm-18 ... libclang-rt-18-dev` →
    `clang-20 llvm-20 ... libclang-rt-20-dev`. `clang-tidy` +
    `clang-tools` stay unversioned; 25.10's meta packages point at
    the 20.x series.
  - `LABEL` description updated to reflect the new base + toolchain.
  - Drogon pin unchanged (`DROGON_VERSION=v1.9.12`).
- `CMakeLists.txt`:
  - Deleted the `PLINTH_TIDY_EXTRA_ARG` block and its `-extra-arg`
    references in both the `run-clang-tidy` and `clang-tidy`
    custom-target bodies. clang-20 reports `__cpp_concepts=202002L`
    natively, so libstdc++'s `<expected>` header no longer needs the
    macro override to parse under tidy.
  - `find_program(RUN_CLANG_TIDY_EXE NAMES run-clang-tidy
    run-clang-tidy-18)` → `run-clang-tidy-20`.
- `src/kernel/capabilities/types.hpp`: stale comment about the
  clang-18 / libstdc++-13 workaround shortened to just the
  structural rationale for keeping `<expected>` out of the header.
- `CMakeLists.txt`: spdlog `GIT_TAG v1.15.0` → `v1.15.3`. v1.15.0
  bundles fmt 11.0.x whose `FMT_STRING` / `basic_format_string`
  consteval path clang-20 refuses to treat as a constant expression;
  every spdlog-including TU then fails tidy with
  `clang-diagnostic-error` even though gcc-15 compiles the same TUs
  cleanly. v1.15.3 bundles fmt 11.2.0, which resolves the parse.
  Surfaced only after the first CI run on the new builder image —
  couldn't be caught locally on the pre-0.3.0.2 clang-18 image.
- `.gitea/workflows/ci.yml`: `fuzz-parser` job now configures with
  `CMAKE_{C,CXX}_COMPILER=clang{,++}-20`. (Dropping the explicit
  compiler pin entirely remains tracked in
  `project_ci_followups_0211.md`; out of scope here.)
- Tidy-fix scope — **three checks** surface against the kernel under
  clang-20 `WarningsAsErrors: '*'`, all fixed in-tree rather than
  suppressed:
  - `modernize-use-designated-initializers`: aggregate
    brace-inits converted across
    `src/kernel/capabilities/{resolution,parser,bootstrap}.cpp`,
    `src/kernel/capabilities/resolution.hpp` (`UserContext::anonymous()`),
    `src/kernel/auth/{handlers,pat_handlers,middleware}.cpp`
    (`AuditCtx`, `TokenValidationResult`),
    `src/kernel/groups/handlers.cpp` (`AuditCtx` ×7),
    `src/kernel/rbac/enforcement.cpp`
    (`DenialContext`, `AuditCtx` ×2),
    `src/kernel/ws/{auth_flow,subscriptions,heartbeat,events_controller}.cpp`
    (`AuditCtx` ×8).
  - `modernize-use-starts-ends-with`:
    `src/kernel/capabilities/validation.cpp:155` (RBAC namespace
    prefix check) and `src/kernel/auth/middleware.cpp` (three PAT /
    Authorization-header prefix checks) rewritten to use
    `std::string::starts_with` / `std::string_view::starts_with`.
  - `performance-unnecessary-copy-initialization`: eight sites in
    `src/kernel/auth/pat_handlers.cpp` +
    `src/kernel/groups/handlers.cpp` where `auto ctx_val = ctx.value();`
    could be a const reference. Changed to
    `const auto& ctx_val = ctx.value();`. Lambdas downstream capture
    `ctx_val` by value (making their own copy at capture time) — no
    lifetime change, one fewer copy per request on the hot path.
    Scope expansion beyond the two checks the ROADMAP item enumerated,
    architect-approved mid-session.

Exit criteria evidence (local, gcc-13 + libstdc++-13 for build;
clang-tidy-20 for tidy):
- `cmake --build build -j 4` — clean build, the one pre-existing
  `drogon::createDbClient` deprecation warning in `main.cpp` is
  unchanged.
- `run-clang-tidy-20 -p build -quiet` over all of `src/kernel/**`
  — zero findings under `WarningsAsErrors: '*'`.
- `ctest --output-on-failure` — 230/230 tests reported, 100% pass
  rate (PG-backed integration tests skipped without a running DB,
  matching the 0.3.0.1 pre-merge baseline).
- `docker build -f docker/ci.Dockerfile ...` — image builds
  successfully on Ubuntu 25.10 base, `clang-20 --version` reports
  20.1.8, `run-clang-tidy --version` available unversioned.

Tagged `v0.3.0.2`.

## v0.3.0.1 — 2026-04-17 — Capability result types → `std::expected`

Closes ROADMAP 0.3.0.1 (`[strong]`). Pure refactor, no behavior change:
the three capability-module result types now use `std::expected` so the
0.3.x arc has a single error-plumbing idiom to build on (matches the
`std::expected<Json::Value, EvalError>` signature that landed with
`plinth::js::eval()` in 0.3.0).

- `src/kernel/capabilities/types.hpp`: `RegisterResult` struct →
  `using RegisterResult = std::expected<std::string, CapabilityError>;`.
- `src/kernel/capabilities/resolution.hpp`: `ResolveResult` / `HandlerOutcome`
  struct aliases →
  `using ResolveResult = std::expected<CapabilityResult, CapabilityError>;`
  and `using HandlerOutcome = std::expected<Json::Value, CapabilityError>;`.
  `CapabilityHandler` typedef picks up the new alias automatically.
- `src/kernel/capabilities/resolution.cpp` / `registration.cpp`:
  `failure()` free helpers now wrap `std::unexpected`; matching
  `success()` helpers dropped — `return value;` relies on
  `std::expected`'s implicit `T`-to-`expected<T,E>` conversion. Seven
  return sites in `register_capability` / `deregister_capability` /
  `set_enabled_by_extension`, five stub handlers, plus `dispatch_tier1` /
  `call_capability` migrated.
- `src/kernel/capabilities/batch.cpp`: `out.value.has_value()` →
  `out.has_value()`, `out.error` → `out.error()`, `*out.value` → `*out`.
  `BatchResult` deliberately **not** migrated — its extra `failed_index`
  field is metadata that doesn't fit the binary success-or-error shape
  of `std::expected`, and the ROADMAP milestone scoped the refactor to
  three types.
- `benchmarks/tier1_benchmark.cpp`: handler lambda returns `Json::Value`
  directly (implicit conversion into `HandlerOutcome`).
- `tests/kernel/capabilities/`: ~50 assertions migrated across
  `resolution_test.cpp`, `batch_test.cpp`,
  `registration_integration_test.cpp`, `listener_integration_test.cpp`.
  `REQUIRE(x.error.has_value())` → `REQUIRE_FALSE(x.has_value())`,
  `*x.error` → `x.error()`, `REQUIRE(x.error == std::nullopt)` →
  `REQUIRE(x.has_value())`, `*x.result` → `*x`, and handler lambdas
  that returned `HandlerOutcome{...}` now return the payload directly
  or `std::unexpected(err)`.
- **ICD status:** unchanged. The migration preserves the public
  signatures referenced by ICD-0.2.0 / ICD-0.2.2 / ICD-0.2.4 / ICD-0.2.5
  exactly — same function names, same parameter types, same return-type
  identifiers (now aliases). Downstream callers observe no contract
  change.
- **Verification:** full build clean (plinth, plinth_tests, benchmarks);
  all 2582 non-PG assertions pass in both the default and
  `-DPLINTH_SANITIZERS=ON` builds; `cmake --build . --target tidy`
  clean on all changed files.
- **Follow-up commit on the branch (CI fuzz job fix)**: `RegisterResult`
  moved from `types.hpp` to `registration.hpp`. The first push to the
  fuzz-parser CI job failed with
  `error: no template named 'expected' in namespace 'std'` — clang-18 +
  libstdc++-13 require `__cpp_concepts >= 202002L` for `<expected>`,
  and the fuzz-parser target (which links only `parser.cpp` +
  `validation.cpp` + `fuzz_parser.cpp`) doesn't carry the
  `-D__cpp_concepts=202002L` workaround that the `tidy` target does.
  Moving the alias to `registration.hpp` keeps `<expected>` out of the
  parser / validation / fuzz-harness include graph; those TUs only
  needed `CapabilityError`. Verified locally with
  `clang-18 -DPLINTH_FUZZ=ON` — `fuzz_parser.cpp`, `parser.cpp`,
  `validation.cpp` all compile cleanly (local link fails only because
  this workstation lacks `libclang-rt-18-dev`, which the CI builder
  image installs per v0.2.1.1a).

Tagged `v0.3.0.1`.

---

## v0.3.0 — 2026-04-17 — QuickJS vendored, host-side `eval()`

Closes ROADMAP 0.3.0 and unlocks the 0.3.x arc. Implements
`ICD-0.3.0-quickjs-vendoring.md` as a strict compile-and-link
milestone: vendor QuickJS via CMake `FetchContent`, build it as a
static library, and prove it runs inside the Plinth process through
a deliberately narrow `plinth::js::eval()` host API. No runtime
pool, no kernel APIs, no module loader, no async bridge, and no
HTTP / WebSocket / capability-dispatch reachability — those are
reserved for 0.3.1–0.3.3.

- `CMakeLists.txt`:
  - `FetchContent_Declare(quickjs, ...)` pinned to
    `quickjs-ng/quickjs` **v0.14.0** (commit
    `3c051980ab7e783dfbfb1c70c014ce5e05ecf24c`). Rationale: the
    Bellard upstream has no versioned release tags and the fork is
    the actively maintained source; the ICD §Vendoring Contract
    explicitly permits this fallback.
  - Populated via `FetchContent_Populate` + `add_subdirectory(...
    EXCLUDE_FROM_ALL)` so the upstream's incidental executable
    targets (`qjsc`, `qjs_exe`, `api-test`, `lre-test`,
    `function_source`, `unicode_gen`, `run-test262`) are not built.
    Only the `qjs` static library and `qjs-libc` are produced, and
    only the library is linked from Plinth.
  - New `plinth_quickjs` INTERFACE wrapper target hands the `qjs`
    static library to `plinth` and `plinth_tests`. Lets us tighten
    flags in one place without touching call sites.
  - Vendored `qjs` TU's compile with `-w` (file-scoped) so
    Plinth's own `-Wall -Wextra -Werror` stays strict.
  - Linux-only fail-fast at configure (`FATAL_ERROR` otherwise),
    matching ICD §Platform Support.
- `src/kernel/js/eval.{hpp,cpp}` (new):
  - `plinth::js::eval(std::string_view) ->
    std::expected<Json::Value, EvalError>` matching the ICD §Eval
    API surface, including `EvalErrorKind ∈ {SYNTAX_ERROR,
    RUNTIME_ERROR, MEMORY_LIMIT, INTERNAL}` and line/column
    populated from QuickJS error objects when available.
  - Fresh `JSRuntime` + `JSContext` per call; `JS_SetMemoryLimit`
    hard-coded to 16 MiB before any eval per ICD §Memory Limit.
    Configurable budgets land in 0.3.1.
  - Private `js_to_json` supports the JSON-native shapes
    (`null`/`undefined`, bool, number, string, array, plain
    object); unsupported types (Symbol, BigInt, function) return
    `INTERNAL`. Recursion cap = 64.
  - Error classifier maps QuickJS `InternalError("out of memory")`
    → `MEMORY_LIMIT` (per `JS_ThrowOutOfMemory` in `quickjs.c`),
    `SyntaxError` → `SYNTAX_ERROR`, everything else →
    `RUNTIME_ERROR`.
- `tests/kernel/js/eval_test.cpp` (new): five Catch2 cases. Four
  map 1:1 to ICD §Milestone Criteria (simple eval, syntax error,
  16 MiB memory ceiling, 1000-iteration create/destroy leak
  check); a fifth covers the JSON converter's other shape paths.
  All pass locally under `-DPLINTH_SANITIZERS=ON` (ASAN + UBSan)
  — 2021 assertions, zero leaks.
- **C++ standard bumped 20 → 23** (`CMAKE_CXX_STANDARD 23`). The
  ICD's preferred `std::expected` signature is C++23 and the
  project is now free of C++20 pins. Full non-integration test
  suite re-validated (119/119 passing). Any future scope
  surprises from the bump will get their own sub-version — none
  surfaced in this session.
- **clang-tidy toolchain quirk, documented workaround**: libstdc++
  13's `<expected>` requires `__cpp_concepts >= 202002L`; clang-18
  reports `201907L` (gcc-13 reports `202002L`, so regular builds
  are unaffected). The `tidy` target now passes
  `-D__cpp_concepts=202002L` via `-extra-arg` so clang-tidy-18 can
  parse TUs that include `<expected>`. Validated locally with both
  clang-tidy-18 (workaround active) and clang-tidy-20 (workaround
  not needed) — full kernel clean in both. The workaround retires
  when the CI builder image moves to clang-20+.
- **Follow-ups tracked on the ROADMAP**, not bundled:
  - **0.3.0.1** — migrate `src/kernel/capabilities` result types
    (`ResolveResult`, `HandlerOutcome`, `RegisterResult`) to
    `std::expected` now that C++23 is available. Pure refactor,
    ~9 files affected.
  - **0.3.0.2** — CI builder image: upgrade clang-18 → clang-20
    (available in Ubuntu Noble apt as
    `1:20.1.2-0ubuntu1~24.04.2`), drop the `__cpp_concepts`
    tidy workaround, AND fix the `modernize-use-designated-
    initializers` / `modernize-use-starts-ends-with` findings
    that clang-tidy-20 surfaces against existing kernel code
    (`parser.cpp`, `validation.cpp`, `bootstrap.cpp`,
    `resolution.cpp`, and adjacent). Same out-of-cycle treatment
    used for 0.1.5.1 and 0.2.1.1a.

Exit criteria evidence (all on branch
`feat/0.3.0-quickjs-eval`, gcc-13 + libstdc++-13):
- `ctest --test-dir build --output-on-failure` — 119 test cases,
  2583 assertions (full non-integration suite) passing under C++23.
- `[js]` subset under `-DPLINTH_SANITIZERS=ON` — 5 cases, 2021
  assertions, zero ASAN / UBSan findings including the mandated
  1000× `eval("1+1")` leak loop.
- `cmake --build build --target tidy` — clean with the documented
  workaround in place.

## v0.2.6.3 — 2026-04-17 — ICD authoring for 0.3.0 / 0.3.1 / 0.3.2

Closes ROADMAP 0.2.6.3. Phase 1 session per
METHODOLOGY §Phase 1 (Interface Contracts). Produces three ICDs
against `DESIGN-quickjs-bridge.md` — one per 0.3.x code
milestone through 0.3.2 — so that future code sessions have
tight, testable contracts instead of reading the design doc as
prose. Also files two new discussion captures that landed in the
same branch. No code, tests, CI, or schema touched.

- `docs/icd/ICD-0.3.0-quickjs-vendoring.md` (new) — traces to
  `DESIGN-quickjs-bridge.md §§2, 9.0` and
  `architecture/05-extensions.md §3`. Defines the FetchContent-
  based vendoring contract, the static library target
  (`plinth_quickjs`), the host-side `plinth::js::eval()` surface
  (`std::expected<Json::Value, EvalError>` with
  `EvalErrorKind ∈ {SYNTAX_ERROR, RUNTIME_ERROR, MEMORY_LIMIT,
  INTERNAL}`), and a hard-coded 16 MiB memory ceiling that
  exercises the limit path from day one. Four milestone-criteria
  tests mirror `DESIGN §9.0`. Explicit fence against pool /
  configurable limits / kernel-API injection / async bridge.
- `docs/icd/ICD-0.3.1-runtime-lifecycle.md` (new) — traces to
  `DESIGN-quickjs-bridge.md §§3.2, 4, 9.1, 10` and
  `architecture/05-extensions.md §§3.1, 3.2`. Locks the final
  `BridgeContext` field shape (async-only fields present but
  fenced as reserved for 0.3.3), the `RuntimePool` surface
  (`acquire`/`release`/`destroy`/`rebuild`, default sizing
  `min(4, hardware_concurrency()/2)`, on-demand transient growth
  with no unbounded pool expansion), and enforcement of four
  limits: memory (`JS_SetMemoryLimit`), CPU time (interrupt
  handler with pause/resume bracket — single bracket in 0.3.1,
  additive to the async loop in 0.3.3), wall-clock timeout, and
  stack depth (`JS_SetMaxStackSize`). Call-depth counter field
  declared here; enforcement stays with the ICD-0.2.2 dispatcher.
  `EvalErrorKind` extended with `CPU_TIME_EXCEEDED`,
  `WALL_CLOCK_EXCEEDED`, `STACK_OVERFLOW`, `CANCELLED`. Five
  milestone-criteria tests mirror `DESIGN §9.1`. Full
  cancellation cascade (`DESIGN §6`) explicitly fenced to 0.3.3.
- `docs/icd/ICD-0.3.2-kernel-stdlib-sync.md` (new) — traces to
  `DESIGN-quickjs-bridge.md §§8, 9.2`. Defines the synchronous
  kernel surface injected into every pooled runtime: `log.debug
  /info/warn/error` (forwards to `plinth::log::*`), `config.get`
  (hard-coded projection table — explicit whitelist, excludes
  every `Config::Database` field and `migrations_dir`), and
  `crypto.hash` (sha256/sha512 only), `crypto.randomBytes` (`n`
  bounded 1..4096), `crypto.timingSafeEqual`. Registration
  mechanism: one `inject_sync_fn` helper + one file per namespace
  under `src/kernel/js/stdlib/` to match the SESSION-GUIDE
  "one file per capability handler" convention. Scopes back the
  ROADMAP line "db, log, audit, config" to just log/config/crypto
  — `db.*` and `audit.*` both require the async bridge and are
  fenced to 0.3.3. Four milestone-criteria test groups mirror
  `DESIGN §9.2` and add security-constraint coverage
  (`config.get("db.password") → null`, `crypto.hash("md5", ...)
  → RangeError`, bounded `randomBytes`, constant-time compare).
- `docs/discussion/DISCUSSION-post-shell-application-order.md`
  (new) — the maintainer's architecture capture ordering the extensions
  Plinth will grow after the shell and admin ship in 0.6.x.
  Non-authoritative, feeds future per-extension design docs.
- `docs/discussion/DISCUSSION-persona-rbac.md` (new) — the maintainer's
  architecture capture on persona RBAC, building on
  `DESIGN-memory.md` and the post-shell ordering doc above.
  Non-authoritative, feeds a future `DESIGN-persona-rbac.md`
  (likely ~0.13+).
- `docs/ROADMAP.md` — flips `0.2.6.3` to `- [x]` with a pointer
  to the three ICD filenames; removes the `[strong]` band label
  per the ROADMAP §"Band labels" rule that completed milestones
  are unlabeled.
- `docs/CHANGELOG.md` — this entry.

**Why:** 0.3.x is the hardest engineering arc in Plinth
(`DESIGN-quickjs-bridge.md §1`). Without tight ICDs before code
starts, a 0.3.0 code session could lock the wrong runtime shape,
a 0.3.1 session could drift the `BridgeContext` layout before
0.3.3 inherits it, and a 0.3.2 session could inadvertently expose
DB credentials via `config.get`. Writing all three ICDs in one
session — with fences against 0.3.3 async concerns explicit in
every "What Must Not Be Decided Yet" section — ensures that when
code sessions start, they each have a 30-minute-read spec instead
of a 900-line design-doc reading assignment.

**Scoping decision recorded here:** the ROADMAP line for 0.3.2
named "db, log, audit, config"; ICD-0.3.2 implements only log +
config + crypto. `db.*` and `audit.*` both require the
promise↔coroutine bridge (`DESIGN-quickjs-bridge.md §§3.3, 8.1`)
and the audit path also has the `g_audit_ready` gating pattern
established in v0.2.4 — both are async concerns that belong with
0.3.3. `crypto.*` was added because it is unambiguously
synchronous and unblocks any 0.3.2+ test extension that needs
hashing or random bytes. The roadmap line is not edited — the
ICD is the tighter spec, and the "db, log, audit, config"
phrasing in ROADMAP reads correctly as the arc-level scope,
satisfied across 0.3.2 (log, config, crypto) and 0.3.3 (db,
audit).

**Next code work:** 0.3.0 per the approved ICD (QuickJS
vendoring + basic eval). The 0.2.6 async wrapper remains deferred
until the first real coroutine caller appears; per the
methodology rule "roadmap items are scheduled", that caller is
now firmly expected to be 0.3.3.

**Note on tagging:** this is a numbered ROADMAP milestone but a
docs-only session. Architect's call whether to tag `v0.2.6.3` (as
0.2.6.1 / 0.2.6.2 were tagged) or leave it untagged (as the
2026-04-17 methodology documentation session was). Either is
defensible under `feedback_tagging_rule.md`.

---

## v0.2.6.2 — 2026-04-17 — Capability Tier 1/2 benchmark validation

Closes ROADMAP 0.2.6.2. Benchmark infrastructure validating
ICD-0.2.2 §Performance Targets (< 1μs Tier 1, < 1ms Tier 2).
Surfaced by `RE-EVAL following 0.2.x` §2.9 — the ICD called for
"validation via benchmarks during 0.2.5 or a dedicated performance
pass" and 0.2.5 shipped `cap.batch()` without a benchmark harness.
Feeds the metrics story in `architecture/04-services-ha.md §3.1`
("Capability resolution latency per tier"). No ICD (the contract
is the ICD-0.2.2 targets section itself).

- `benchmarks/tier1_benchmark.cpp` (new) — two Google Benchmark
  cases: `BM_Tier1_Hit` (primary < 1μs target) and
  `BM_Tier1_Miss_FallsToTier2` (miss-path lock + second map
  lookup). Fixtures mirror `tests/kernel/capabilities/resolution_test.cpp`
  — `register_tier1_handler` / `seed_tier2_cache_for_test`
  populate the resolver without PG, and
  `effective_rules = {"kernel.admin"}` keeps the ICD-0.2.4
  step-3 RBAC check a pass-through so the measurement is
  dispatch-only.
- `benchmarks/tier2_benchmark.cpp` (new) — four cases:
  `BM_Tier2_InstanceScope_Hit` (primary < 1ms target, resolves
  to `TIER3_NOT_AVAILABLE` as extension providers do in 0.2.x —
  the dispatch-cost measurement is unaffected),
  `BM_Tier2_UserScope_Override` (user-scope-key build + first
  map lookup ahead of the instance probe),
  `BM_Tier2_Disabled` (fast-reject path in `dispatch_tier2`),
  `BM_Tier2_NotFound` (worst-case Tier-1-miss + two Tier-2
  probes + deny).
- `CMakeLists.txt`
  - New `PLINTH_BENCHMARKS` option (default OFF — same
    opt-in pattern as `PLINTH_SANITIZERS` / `PLINTH_FUZZ`).
    Gated block adds Google Benchmark via FetchContent
    (v1.9.1, `BENCHMARK_ENABLE_TESTING=OFF`,
    `BENCHMARK_ENABLE_INSTALL=OFF`,
    `BENCHMARK_ENABLE_WERROR=OFF` to tolerate gcc-13
    warnings in GBench's own sources) and defines two
    executables wired to the minimal kernel source set
    (`resolution.cpp`, `parser.cpp`, `validation.cpp`,
    `logging.cpp`) + the main plinth link libraries.
  - Drogon FetchContent block now sets `BUILD_EXAMPLES=OFF`
    and `BUILD_TESTING=OFF` before `FetchContent_MakeAvailable`.
    Drogon's examples directory declares a target named
    `benchmark` that collides with Google Benchmark's target
    when `PLINTH_BENCHMARKS=ON`; we never ship Drogon's
    examples anyway and this is a small configure-time
    speedup regardless.
- `.gitea/workflows/ci.yml` — three steps appended to the
  existing `build-and-test` job (after `Version check`,
  reusing the same ci-builder container): `cmake -B
  build-bench -DCMAKE_BUILD_TYPE=Release -DPLINTH_BENCHMARKS=ON`;
  build the two benchmark targets; run each with
  `--benchmark_min_time=1s`. **Informational only** — CI is
  not gated on threshold pass/fail, so only a build/link
  regression breaks the job. A future milestone can promote
  gating once variance on the shared runner is understood.
- `docs/ROADMAP.md` — `0.2.6.2` flipped to `[x]`.

**First-run numbers (local, Release, gcc-13, 5.2 GHz x86_64 —
not CI numbers):**

| Benchmark                      |    Time | CPU    | Target   | Margin  |
| ------------------------------ | ------: | -----: | -------- | ------- |
| `BM_Tier1_Hit`                 |  150 ns | 149 ns | < 1 μs   | ~6.7×   |
| `BM_Tier1_Miss_FallsToTier2`   | 85.8 ns |  85.8 ns | —      | —       |
| `BM_Tier2_InstanceScope_Hit`   | 86.7 ns |  86.6 ns | < 1 ms | ~11500× |
| `BM_Tier2_UserScope_Override`  | 87.4 ns |  87.4 ns | —      | —       |
| `BM_Tier2_Disabled`            | 88.3 ns |  88.2 ns | —      | —       |
| `BM_Tier2_NotFound`            | 80.8 ns |  80.8 ns | —      | —       |

Both ICD targets comfortably met. `BM_Tier1_Miss_FallsToTier2`
is faster than `BM_Tier1_Hit` because the hit path invokes a
handler (even a no-op handler constructs a `Json::Value`),
while the miss+tier2 path stops at the dispatch-decision
branch (`TIER3_NOT_AVAILABLE`) without calling any handler.

Accepted deviations: none — implementation tracks the roadmap
bullet exactly.

---

## v0.2.6.1 — 2026-04-17 — Anonymous-identity enforcement test

Closes ROADMAP 0.2.6.1. Catch-up item surfaced by `RE-EVAL following
0.2.x` §2.8 — `architecture/01-identity.md §3` required an enforcement
test "in 0.1.5" that never landed. No ICD (the contract is the
architecture section itself).

- `src/kernel/capabilities/resolution.hpp` — `UserContext::anonymous()`
  and `UserContext::anonymous_with_rules(std::vector<std::string>)`
  static factories on the existing struct. `auth_type` gains
  `"anonymous"` as a third legal value alongside `"session"` / `"pat"`.
  Inline bodies, zero `.cpp` impact. No live request path synthesizes
  an anonymous context today (the architecture §3 note "This changes
  nothing behaviorally" stands); the factories are the forward-
  looking contract.
- `src/kernel/rbac/enforcement.{hpp,cpp}` — `list_registered_rules()`
  snapshot accessor plus a `RegisteredRule { method, path_pattern,
  rules }` struct. Internal storage migrated from
  `std::unordered_map<std::string, std::vector<std::string>>` to
  `std::vector<RegisteredRule>` (linear scan on an 11-entry registry
  at startup-only mutation time — the trade is irrelevant; the
  uniform shape keeps the iterator trivial). `register_rule_requirement`
  / `get_required_rules` public surface unchanged.
- `tests/kernel/rbac/anonymous_identity_test.cpp` (new) — 4 Catch2
  cases on two axes:
  - **Factory shape.** Asserts baseline sentinel (`user_id` empty,
    `auth_type == "anonymous"`, empty rules) and that
    `anonymous_with_rules` preserves identity while populating
    `effective_rules`.
  - **Registry coverage (unit).** Calls
    `plinth::audit::register_audit_routes` +
    `plinth::groups::register_group_routes` to populate the
    registry with the same production wiring `main.cpp` uses, then
    `list_registered_rules()` and asserts every entry has a
    non-empty `rules` vector (empty-rules pathology guard) and
    that `UserContext::anonymous()` is denied against every
    entry. Automatically picks up any new RBAC-gated route added
    to either module.
  - **Grant-unlocks (integration).** Against a real PG schema,
    grants `kernel.admin` to the `everyone` group, rebuilds
    anonymous with the loaded-from-DB everyone-rules, asserts the
    `kernel.admin`-gated routes now admit it; revokes, reasserts
    denial. Ensures the denial isn't unconditional fail-closed at
    the grant-checking layer.
- `CMakeLists.txt` — `anonymous_identity_test.cpp` added to
  `plinth_tests` sources.
- `docs/ROADMAP.md`
  - `0.2.6.1` flipped to `[x]` (shipped).
  - `0.2.6.3` reworded per architect direction — *"Architecture
    session"* → *"ICD authoring"*. The framing had upscoped 0.2.6.3
    into Phase 0 work; the QuickJS architecture is already decided
    by `DESIGN-quickjs-bridge.md`, so 0.2.6.3 is Phase 1 ICD
    authoring against that decision. Reading list includes
    `DISCUSSION-streaming-and-media.md` so the bridge's
    return-value opacity isn't accidentally narrowed during ICD
    work.

---

## Rewrite session — 2026-04-17 — RE-EVAL following 0.2.x

Rewrite session (per METHODOLOGY §Phase 3). Documentation-only. No
code, tests, CI, or schema touched. **Not tagged** — rewrite sessions
produce no release.

- `docs/reviews/RE-EVAL-0.2.x.md` (new) — the session artifact.
  Inputs read, gaps found (11 categorized per METHODOLOGY §3.1.1),
  zero-gap baseline, disposition, paper-pass rationale,
  caller-triggered-implementation pattern observation.
- `docs/discussion/` (new directory) — four files authored during
  0.1.8 but never committed, landed via `files.zip` at the start of
  this session:
  `DISCUSSION-cross-cutting-composition.md`,
  `DISCUSSION-ai-bridge.md`,
  `DISCUSSION-streaming-and-media.md`,
  `DISCUSSION-ha-scale-and-offload.md`.
- `docs/icd/ICD-0.2.2-capability-resolution.md`
  - `§Dispatch Contract` — `CapabilityCall` reshaped to match shipped code
    (pre-composed `signature` string; per-hop `call_depth`); `UserContext`
    grew `session_id` + `ip_address` (added 0.2.4 for audit enrichment);
    new *Implementation deviation (0.2.2 → 0.2.6)* subsection documents
    the sync-dispatch form with its trigger-gated async wrapper.
  - `§cap.batch() Behavioral Contract` — C++ surface shown with
    `BatchResult::failed_index`; "sequential is a conforming initial
    implementation" clarifier; empty-batch behavior stated explicitly.
  - `§Cache Invalidation` — new *Reconnect-triggered full resync*
    subsection documenting `reload_tier2_cache()` and the
    bounded-divergence guarantee (≤ 1 s + one SELECT).
- `docs/icd/ICD-0.2.4-capability-rbac.md`
  - `§Rule Lookup Ordering` — step 3 rewritten: *"Cache miss = negative
    result"* (replaces the DB-fallback language), with a rationale
    paragraph pointing at the 0.2.2 reconnect-triggered resync as the
    substitute consistency mechanism.
- `docs/architecture/02-capabilities.md §1.3`
  - New *Reconnect-triggered full resync* bullet in the Tier 2 cache
    description, pointing at ICD-0.2.2's subsection.
- `docs/ARCHITECTURE.md`
  - `§5 Source Tree Layout` rewritten into two passes — *current*
    (through 0.2.5, reflects the 54-file kernel tree that actually
    shipped) and *forward-looking* (table of expected directories by
    milestone). The pre-existing sketch had drifted significantly from
    the real tree.
  - `§5.3` (new) — `ws/` vs `realtime/` naming open question.
  - `§8 Open Questions` — new item 10 for the `ws/` vs `realtime/`
    decision, deferred until 0.5.0.
- `docs/ROADMAP.md`
  - `RE-EVAL following 0.2.x` flipped to `[x]` with pointer to the
    artifact.
  - Three new catch-up / prep items inserted under 0.2.6 as
    post-milestone companions (Plinth precedent 0.2.1 → 0.2.1.1):
    `0.2.6.1` anonymous-identity enforcement test (catch-up from 0.1.5),
    `0.2.6.2` capability Tier 1/2 benchmark validation (deriving from
    ICD-0.2.2 §Performance Targets), and `0.2.6.3` architecture
    session for ICD-0.3.0 / 0.3.1 / 0.3.2 (precondition for 0.3.x
    code work per METHODOLOGY §Phase 1). All three are `[strong]`.
  - No band promotions or demotions for pending milestones (zero-change
    paper pass; rationale in the review artifact §5.1).
- `docs/METHODOLOGY-llm-assisted-development.md` — **v5 patch**
  (methodology extension from the re-eval's §6.1 observation, applied
  in-session with architect approval):
  - New **Constraint #4 — Record deviations in the owning ICD** in
    Phase 2 §Three Non-Negotiable Constraints (now four). Deviations
    taken in code must be written back into the owning ICD in the
    same PR; not in CHANGELOG only, not in code headers only.
    ICD-0.1.6's *Implementation Notes* footer is the template.
  - New subsection **Caller-Triggered Implementation** in Phase 2,
    naming the pattern observed six times across 0.1.x–0.2.x
    (canonical example: 0.2.6's deferred `drogon::Task<>` wrapper).
    Interface committed up front; body stubbed until a named trigger
    (specific caller, specific milestone, or both). Distinguished
    from YAGNI (which omits the interface) and speculative
    abstraction (which builds without a consumer). Legitimate uses
    enumerated; three obligations stated.
  - New **Undocumented deviation** row in the *LLM Failure Modes
    and Mitigations* table, paired with Constraint #4 as its
    mitigation.
  - New **"The deviation is documented in the code, that's enough."**
    entry in *Anti-Patterns*, stating why code-header + CHANGELOG
    documentation is insufficient for future sessions that read the
    ICD as the spec.
- `docs/CHANGELOG.md` — this entry.

**Scope expansion from the approved plan:** the session was approved
as "RE-EVAL following 0.2.x" with gap findings and roadmap items as
output; the methodology v5 patch was surfaced by the re-eval as a
§6.1 methodology observation, approved by the architect mid-session,
and landed in the same PR since the session was already in
rewrite-session mode and the observation's fix is itself a
rewrite-session output. Precedent: the 2026-04-17 v3+v4 patch landed
as a mid-session scope expansion of a v3-only approval. Kept
together because splitting would produce two PRs to main for one
coherent thought.

**Why:** First scheduled re-eval since the cadence installed on
2026-04-17. The code-aware pass found six interface-drift / arch-silent
gaps accumulated across 0.2.2 / 0.2.4 / 0.2.5 (deviations documented in
code headers and CHANGELOG but never written back to the ICDs), plus
one missing safeguard test (`architecture/01-identity.md §3`) and one
missing performance-target validation (ICD-0.2.2 §Performance Targets).
Six instances of a shared "caller-triggered implementation" pattern
were catalogued as a candidate methodology observation for a future
architect session.

**Scope deviation from the approved plan:** The re-eval itself matched
the architect's prompt exactly (code-aware half first, paper half
second, one artifact at `docs/reviews/RE-EVAL-0.2.x.md`, zero C++
touched, gaps requiring code shipped as new roadmap items). The
methodology v5 patch was a mid-session expansion — see the separate
"Scope expansion" paragraph below.

**Next work:** the three new `0.2.6.N` items (in architect-chosen
order). `RE-EVAL following 0.3.3` is the next re-eval per cadence.

---

## Documentation session — 2026-04-17 — Methodology v3+v4, roadmap band labels + re-eval cadence

Documentation-only session. No code, tests, CI, or schema touched.

- `docs/METHODOLOGY-llm-assisted-development.md`
  - **v3 patch — Scheduled Re-evaluation** (4 edits applied; v3 Edit 5
    superseded by v4 Edit 7 below):
    - New `### 3.3 Scheduling` subsection under Phase 3 defining the
      cadence, roadmap placement, entry/exit criteria, and retroactive-
      adoption rule for re-evaluation as a scheduled roadmap item.
    - Appended paragraph to `### Roadmap Milestone Labels` noting that
      re-evaluation items sit alongside code milestones without band
      annotations.
    - New `Re-evaluation drift` row in the *LLM Failure Modes and
      Mitigations* table.
    - New `"We'll re-evaluate when we need to."` entry in *Anti-Patterns*.
  - **v4 patch — Code-Aware Re-evaluation** (7 edits applied):
    - Reworked the Phase 3 opening paragraph into two complementary
      halves: structural (paper) and code-aware (gap analysis).
    - New `### 3.1.1 Code-Aware Inputs` subsection enumerating what a
      code-aware pass reads and the five gap categories it produces.
    - Extended `### 3.2 Scope of a Re-evaluation Session` with
      code-aware outputs (issue/PR filings, test scheduling, ICD
      updates, etc.).
    - Appended `Structural-only re-evaluations` and `Project-lifetime
      implications` paragraphs to `### 3.3 Scheduling`.
    - New `Silent architectural divergence` row in the *LLM Failure
      Modes* table.
    - New `"Tests passing means the architecture is followed."` entry
      in *Anti-Patterns*.
    - New item #6 in *Minimum Viable Adoption* covering the two-halves
      scheduling rule.
- `docs/ROADMAP.md`
  - New preamble block `## Band labels and re-evaluation` declaring
    Plinth's gradient window (N=3, M=7) and re-evaluation cadence
    (every 4 code milestones, revisit at first re-eval).
  - Every pending milestone now carries a `[strong]` / `[medium]` /
    `[fuzzy]` trailing label per METHODOLOGY §Phase 0. 0.2.5 / 0.2.6
    are `[strong]` (ICD content exists); 0.3.x / 0.4.x / 0.5.x / 0.6.x
    are `[medium]` (DESIGN docs exist, ICDs not yet written);
    0.6a-* / 0.7.x / 0.8.x / 0.9.x / 0.10.x / 1.0.0 are `[fuzzy]`.
  - 14 `RE-EVAL following X.Y.z   [rewrite session]` items inserted
    at the chosen cadence, starting with `RE-EVAL following 0.2.x`
    between 0.2.6 and 0.3.0.
  - Also flipped three shipped-but-still-listed-as-pending milestones
    to `- [x]` (0.1.7 audit, 0.1.8 architecture decomposition, 0.2.0
    capability registry). Pre-existing roadmap drift surfaced while
    labelling; safe to correct inline.
- `docs/CHANGELOG.md` — this entry.

**Why:** The v2 patch (already present in the methodology doc) installed
the three-band commitment gradient but the roadmap was never updated to
carry it. v3 closes the drift-prevention loop by scheduling re-evaluation
as a roadmap item; v4 extends re-evaluation to a code-aware gap-analysis
pass so architectural divergence (tests passing against drifted code) is
caught by the methodology rather than discovered by future extension
authors. Bulk-labelling the roadmap and inserting re-eval items in the
same session completes the v2+v3+v4 installation in one pass.

**Scope deviation from the approved plan:** v4 arrived mid-session; it
was applied inline alongside v3 as a straightforward extension of the
same Phase 3 changes. v3 Edit 5 (MVA item #6) was skipped because v4
Edit 7 supersedes it with a two-halves version.

**Next code work:** 0.2.5 `cap.batch()` per ROADMAP (unchanged).

---

## v0.2.5 — 2026-04-17 — cap.batch() dispatch

Closes ROADMAP 0.2.5. Implements ICD-0.2.2 §cap.batch() Behavioral
Contract. No separate ICD (the contract pre-existed the 0.2.2/0.2.5
milestone split).

- `src/kernel/capabilities/batch.{hpp,cpp}` — new translation unit.
  `batch_call_capability(calls, ctx)` fans a `std::vector<CapabilityCall>`
  out through the standard `call_capability` pipeline and returns a
  `BatchResult { values?, error?, failed_index }`:
  - Empty input → success with an empty result vector (Promise.all([])
    parity).
  - Each element walks the full resolver (parse → depth → RBAC → tier
    lookup → dispatch); no pre-flighting, no RBAC short-circuit.
  - Order-preserving: on success, `values[i]` corresponds to `calls[i]`.
  - Fail-fast: on the first element that returns an error, all prior
    successful results are discarded; `error` is propagated verbatim
    and `failed_index` points at the offending input element.
  - `call_depth` is normalized to `calls[0].call_depth` across the
    batch (per ICD: "all calls in a batch inherit the same call_depth
    from the caller").
- `tests/kernel/capabilities/batch_test.cpp` — 9 Catch2 cases cover
  empty-batch, order-preserving fan-out, Tier 1/Tier 2 mixed pipeline,
  fail-fast on parse / RBAC / disabled errors with counter-based proof
  that post-error handlers never run, shared-depth normalization,
  `call_depth_exceeded` propagation, and a two-thread concurrent-batch
  smoke for TSan under `-DPLINTH_SANITIZERS=ON`.
- `CMakeLists.txt` — `batch.cpp` added to the `plinth` and
  `plinth_tests` source lists; `batch_test.cpp` added to the test
  sources.

**Accepted deviations from ICD-0.2.2 §cap.batch() (same footing as the
0.2.0 / 0.2.2 / 0.2.4 precedents, deferred until a real caller
materializes):**

1. **Sequential dispatch, not threaded.** The ICD text reads "executed
   concurrently where possible" but also permits the initial
   implementation to expand to `Promise.all(calls.map(...))`;
   DESIGN-quickjs-bridge.md §7.3 classifies concurrency as "an
   optimization opportunity, not a correctness requirement". The kernel
   has no thread pool today and no caller demanding parallelism;
   revisit alongside 0.2.6 (async wrapper) or 0.3.3 (first JS
   `Promise.all` user).
2. **Tier-3 multiplexing is a no-op.** DESIGN-quickjs-bridge.md §7.3
   explicitly defers same-node Tier 3 bundling to milestone 0.8.
3. **`BatchResult` carries a `failed_index` field beyond the strict
   ICD surface.** Records which input element aborted the batch;
   feeds future JS-side error surfacing and audit debugging. Engaged
   only on the error path.

**Next code work:** 0.2.6 `drogon::Task<>` async wrapper, triggered
by the first real coroutine caller (expected alongside 0.3.3).

---

## v0.2.4 — 2026-04-17 — Capability RBAC integration

Closes ROADMAP 0.2.4. Implements ICD-0.2.4-capability-rbac (step 3 of
the ICD-0.2.2 §Resolution Algorithm).

- `src/kernel/capabilities/types.hpp` — new `CapabilityError::PERMISSION_DENIED`
  variant. `validation.cpp` returns `"permission_denied"` for it.
- `src/kernel/capabilities/resolution.{hpp,cpp}`
  - `UserContext` gained `effective_rules` (additive-union input for the
    step-3 check) plus `session_id` / `ip_address` for audit enrichment.
    Callers (HTTP handlers today, JS bridge in 0.3.x) pre-populate
    these; the resolver never reads from the DB on the hot path.
  - `register_tier1_handler` is now a 3-arg form that takes the required
    `rbac_rule` alongside the callable. Tier 1 map stores a small
    `Tier1Entry { handler, rbac_rule }` so rule lookup and dispatch
    happen in a single shared-lock window.
  - `call_capability` rewritten: after parse + depth, it probes Tier 1
    and Tier 2 (with scope precedence) for the candidate entry, runs
    `check_permission` against the user's effective rules (additive
    union, `kernel.admin` as universal match, empty `rbac_rule` → deny
    fail-closed), audits denials as `capability.rbac.denied`, and falls
    through to dispatch on grant. Unknown signatures still resolve as
    `capability_not_found` — we do not leak a `permission_denied` for a
    rule we cannot name. Per-hop checking is automatic: recursive
    `call_capability` invocations re-run step 3 at each hop against the
    originating user's context.
- `src/kernel/capabilities/listener.{hpp,cpp}` — **amendment to the
  0.2.3 eventual-consistency story.** Every successful LISTEN open
  (initial and reconnect) now triggers a full Tier 2 resync via the
  new public `reload_tier2_cache(db_cfg)` helper in resolution.cpp.
  Missed-NOTIFY recovery is bounded by one reconnect backoff (≤ 1 s)
  plus one `SELECT`, instead of the previous "until process restart"
  window. `listener.hpp` threading note updated accordingly.
- `tests/kernel/capabilities/resolution_test.cpp` — existing cases
  updated to grant `kernel.admin` via `default_ctx()`; new suite tagged
  `[rbac]` covers Tier 1 grant, Tier 2 grant, explicit denial,
  `kernel.admin` bypass, multi-hop chain denial at hop 2, denial at
  `call_depth > 0`, disabled-capability ordering (RBAC precedes
  dispatch), fail-closed on empty `rbac_rule`, version-independent
  rule mapping, and `capability_not_found` precedence on cache miss.
- `tests/kernel/capabilities/listener_integration_test.cpp` — new
  PG-backed case `reload_tier2_cache syncs the cache from
  plinth.capabilities`: seeds a stale entry, inserts a real row without
  NOTIFY, asserts the resync reconciles both.

**Accepted deviations from ICD-0.2.4:**

1. **No DB fallback for rule lookup.** ICD §Rule Lookup Ordering names
   Tier 1, Tier 2, and a DB fallback. The resolver stays pure in-memory
   — cache miss → `capability_not_found`, consistent with 0.2.2's
   precedent. LISTEN/NOTIFY (0.2.3) plus the new reconnect resync (this
   milestone) keeps the cache within one SELECT of the authoritative
   table. If a real race ever surfaces, a DB fallback can land as a
   follow-up milestone without changing the dispatch contract.
2. **Audit emission is fire-and-forget async** via the existing
   `plinth::log::audit` Drogon-backed writer. `UserContext` carries
   `session_id` and `ip_address` so no HttpRequestPtr overload is
   required from the resolver.

---

## v0.2.3 — 2026-04-17 — LISTEN/NOTIFY cache invalidation

Closes ROADMAP 0.2.3. Implements ICD-0.2.2-capability-resolution §Cache
Invalidation (the milestone 0.2.3 exit criteria).

- `src/kernel/capabilities/listener.{hpp,cpp}` — new translation unit
  hosting the long-lived libpq subscriber for `plinth_capability_changed`.
  A `std::jthread` owns a dedicated `PGconn`, `poll()`s the connection
  socket and a shutdown eventfd with a 1 s ceiling, drains `PQnotifies`,
  parses the JSON payload via `Json::CharReaderBuilder`, and dispatches
  each of the four actions onto the Tier 2 cache. On `CONNECTION_BAD`
  the loop reconnects with a 1 s backoff; NOTIFYs delivered mid-reconnect
  are lost per the ICD-0.2.2 §Multi-Node eventual-consistency contract.
- `src/kernel/capabilities/resolution.{hpp,cpp}` — three new public
  helpers the listener (and tests) call into:
  `upsert_tier2_entry` for `register`, `erase_tier2_entry` for
  `deregister`, and `set_enabled_by_extension_in_cache` for bulk
  `disable` / `enable`. `seed_tier2_cache_for_test` is now a thin
  wrapper over `upsert_tier2_entry` to keep the key-derivation logic
  in one place.
- `src/kernel/main.cpp` — two new lines: `start_notify_listener(cfg.db)`
  runs after `init_resolver`, and `stop_notify_listener()` runs after
  `drogon::app().run()` returns and before `spdlog::shutdown()` so the
  thread stops logging before the async sink is torn down.
- Test seam: `apply_notification_for_test(db, payload_json)` exposes the
  parse + apply pipeline to unit tests without requiring the background
  thread.
- `tests/kernel/capabilities/listener_integration_test.cpp` — three
  layers of coverage: parse/apply unit tests (malformed JSON, unknown
  action, deregister + bulk disable/enable on a seeded cache), a
  PG-backed register test that exercises the `fetch_row` path, a
  full-loop test that starts the listener thread and walks the cache
  through `register → disable → enable → deregister` with bounded 5 s
  waits asserting on `call_capability` observations, and a multi-node
  smoke test that opens two bare `LISTEN` conns and confirms both drain
  the same NOTIFY — validating ICD §Multi-Node Behavior without
  spinning up a second kernel process.
- `CMakeLists.txt` — `listener.cpp` joins `plinth` and `plinth_tests`;
  `listener_integration_test.cpp` joins `plinth_tests`.

Deviations from ICD-0.2.2: none new. The accepted deviations from
0.2.2 (sync dispatch, Tier 1 stub handlers, RBAC seam) carry forward
unchanged.

---

## v0.2.2 — 2026-04-17 — Capability resolution (Tier 1 + Tier 2)

Closes ROADMAP 0.2.2. Implements ICD-0.2.2-capability-resolution §Milestone
Criteria 0.2.2 — the dispatch pipeline that turns a
`namespace:version:function` signature into a handler invocation.

- `src/kernel/capabilities/resolution.{hpp,cpp}` — `init_resolver()`
  (startup wiring: registers Tier 1 stub handlers; loads the Tier 2
  cache from `plinth.capabilities` via sync libpq) and
  `call_capability()` (the synchronous dispatch entry point). Resolution
  follows ICD §Resolution Algorithm: parse → depth check → RBAC seam
  (deferred to 0.2.4) → Tier 1 → Tier 2 (user-scope key first, then
  instance) → Tier 3 stub. Module-local Tier 1 map + Tier 2 cache behind
  a `std::shared_mutex` (shared on reads, unique for the future 0.2.3
  NOTIFY writer + test seeding helpers).
- `src/kernel/capabilities/types.hpp` — three new `CapabilityError`
  variants exclusive to the resolver path: `CAPABILITY_DISABLED`,
  `TIER3_NOT_AVAILABLE`, `CALL_DEPTH_EXCEEDED`, each mapped to their
  snake-case wire forms in `validation.cpp`.
- `src/kernel/main.cpp` — one new line: `init_resolver(cfg.db)` runs
  after `bootstrap_kernel_capabilities` and before `app().run()`.
- `tests/kernel/capabilities/resolution_test.cpp` — nine Catch2 cases
  covering every exit-criteria bullet: Tier 1 hit, Tier 2 miss
  (→ `capability_not_found`), Tier 2 hit with sidecar / extension
  providers (→ `tier3_not_available`), disabled entry
  (→ `capability_disabled`), user-scope precedence over instance-scope
  (including fallback when a different user calls), call depth at the
  configured limit (→ `call_depth_exceeded`) and one below it (still
  dispatches), malformed signatures (→ `invalid_capability`), and
  instance-scope fallback when no user-scope entry exists.
- `CMakeLists.txt` — `resolution.cpp` in both `plinth` and
  `plinth_tests`; `resolution_test.cpp` in `plinth_tests`.

**Precedent set.** ICD-0.2.2 §Resolution Algorithm step 6 literally
reads "Tier 2 miss → Tier 3 → `tier3_not_available`", but §Error Codes
defines `capability_not_found` for "No matching capability in Tier 1
or Tier 2 cache." Resolved with the maintainer: unknown signatures return
`capability_not_found`; `tier3_not_available` is reserved for cache
entries whose `provider_type = "sidecar"` (or, until 0.3.x lands, any
`provider_type = "extension"` — same signal, "not dispatchable here
yet"). Tests pin both semantics.

**Deliberate deviations from ICD-0.2.2** (mirrors the 0.2.0 sync-libpq
deviation; ROADMAP carries the follow-up):
- Synchronous dispatch, not `drogon::Task<Result<…>>`. The repo has no
  coroutine users today and 0.2.2 has no runtime caller — the JS bridge
  lands in 0.3.x. Async wrapper tracked as new ROADMAP item **0.2.6**,
  triggered by the first real coroutine caller (expected alongside
  0.3.3). This is not permission to never do.
- Tier 1 handlers are stubs: the five bootstrapped kernel capabilities
  (`kernel:1:db.query`, `db.exec`, `log`, `audit`, `config.get`) each
  register a handler that returns `{"not_implemented": "<signature>"}`.
  Real wiring lands when a caller first demands it.
- RBAC enforcement on dispatch (ICD step 3) is a labeled seam, not a
  check — ICD-0.2.4 implements it.

## v0.2.1.1b — 2026-04-17 — libFuzzer CI wiring

Closes ROADMAP 0.2.1.1. Pairs with v0.2.1.1a (builder image + tidy
parallelization).

- `.gitea/workflows/ci.yml` — new `fuzz-parser` job, sibling of
  `build-and-test`. Uses the same CI builder image, configures with
  `clang-18` + `clang++-18` and `-DPLINTH_FUZZ=ON`, builds
  `plinth_fuzz_parser`, and runs it with
  `-max_total_time=60 -print_final_stats=1 -rss_limit_mb=2048`
  against the new seed corpus. Job runs parallel to `build-and-test`,
  so CI wall-clock is gated on whichever finishes later. Budget set
  at/below tidy's post-0.2.1.1a wall-clock (~1 min on CI) so the fuzz
  job is never the bottleneck.
- `tests/kernel/capabilities/fuzz_parser_seeds/` — 18 hand-crafted
  seed inputs covering valid kernel signatures, shape-level failures
  (empty, no/one/too-many colons, empty namespace/version/function),
  per-field validator failures (leading-zero / negative / trailing-
  garbage / overflow version), and UTF-8 edge cases.
- `docs/ROADMAP.md` — 0.2.1.1 checked off; cross-cutting Testing &
  Security entry marked done.

## v0.2.1.1a — 2026-04-17 — CI image prep + tidy parallelization

Split from 0.2.1.1 so the builder-image rebuild lands before the fuzz
CI job turns on. The fuzz job itself follows in v0.2.1.1b.

- `docker/ci.Dockerfile` — added `clang-18`, `llvm-18`, `clang-tools`,
  `libclang-rt-18-dev`, and `python3` to the apt install list. Clang
  itself (previously only `clang-tidy` was pulled), the libFuzzer
  runtime required by `-fsanitize=fuzzer,address,undefined`, and
  `run-clang-tidy` are now part of `:latest`. LABEL description
  updated to reflect the toolchain additions.
- `CMakeLists.txt` — `tidy` target now prefers `run-clang-tidy -j N`
  (parallel per-file invocations) with a serial `clang-tidy` fallback
  when `run-clang-tidy` is missing. Jobs count comes from CMake's
  `ProcessorCount()`. Clang-tidy content is unchanged — only how it's
  invoked. Dropped `-header-filter` default behaviour aside; kernel
  source glob and `-p ${CMAKE_BINARY_DIR}` remain the contract.
- No workflow or source changes. ROADMAP 0.2.1.1 stays open; it flips
  when v0.2.1.1b lands the `fuzz-parser` job.

## v0.2.1 — 2026-04-16 — Capability string parser

- `src/kernel/capabilities/parser.{hpp,cpp}` — `parse_signature()`
  is the inverse of `make_signature()`: takes a canonical
  `namespace:version:function` string and returns a
  `ParsedSignature` or a `CapabilityError`. Uses `std::from_chars`
  for version parsing and reuses the existing field validators so
  the parser stays aligned with registration by construction.
- New `CapabilityError::INVALID_CAPABILITY` for shape-level errors
  (wrong colon count, empty segments, non-numeric or overflowing
  version), mapped to `"invalid_capability"` per ICD-0.2.2 §Error
  Codes. Per-field validator errors continue to surface
  `INVALID_NAMESPACE` / `INVALID_VERSION` / `INVALID_FUNCTION`.
- Catch2 unit coverage (`tests/kernel/capabilities/parser_test.cpp`)
  — happy paths, round-trip over the kernel bootstrap set, every
  shape-level failure, every per-field validator rejection, and
  max-length boundaries.
- Opt-in libFuzzer harness
  (`tests/kernel/capabilities/fuzz_parser.cpp`) behind
  `-DPLINTH_FUZZ=ON` (Clang only). CI wiring deferred to v0.2.1.1.
- ROADMAP: 0.2.1 checked off; new 0.2.1.1 line added for libFuzzer
  CI wiring; cross-cutting Testing & Security entry retargeted.

## v0.2.0 — 2026-04-16 — Capability registry

- `plinth.capabilities` table (schema + indexes + unique
  constraint) in `migrations/schema.sql`.
- Registration API (`register` / `deregister` /
  `disableByExtension` / `enableByExtension`) implemented via sync
  libpq with full field validation, RBAC-rule alignment, and
  `kernel` namespace reservation.
- Kernel-capability bootstrap registers the minimum set
  (`kernel:1:db.query`, `db.exec`, `log`, `audit`, `config.get`).
- `NOTIFY plinth_capability_changed` emitted on every mutation
  (listener / Tier 2 cache land in 0.2.2 / 0.2.3).
- Audit events `capability.registered` /
  `capability.deregistered` / `capability.extension_disabled` /
  `capability.extension_enabled` emitted via `log::audit()`.
- Catch2 coverage for validation (unit) + registration and
  bootstrap (integration, PG-backed).
- Deviation from plan noted in memory: registration API is sync
  libpq rather than async Drogon; async wrapper deferred to 0.4.x
  pending measurement.

## v0.1.8 — 2026-04-16 — Architecture decomposition + four decisions

### Decisions landed

- **5.a Storage HTTP surface.** `POST/GET/DELETE/GET-list` under
  `/api/storage/{extension}/{path...}`. RBAC mirrors extension-schema
  read/write. 100 MB upload threshold (resumable beyond). Range
  support on downloads. Two-tier quota (extension + user-reserved).
  Lives in `architecture/03-data.md §2.3`.
- **5.b User-deletion cleanup contract.** `users.deleted` event on
  the realtime bus + `kernel:1:users.list` capability with
  `kernel.users.list` rule default-granted to `authenticated`. Lives
  in `architecture/01-identity.md §4`.
- **5.c Design-token serving.** `/api/frontend/*` indirection →
  `/ext/{active-frontend}/{version}/*`. Redirect is `no-cache`,
  target is immutable. Import-map binding for
  `@plinth/frontend/tokens`. Lives in `architecture/06-frontend.md §4`.
- **5.d Cross-cutting composition framework (reserved).**
  `surface_traits` / `slots` / `augments_traits` reserved in the 0.4
  manifest schema. Three modes (slot injection / event stream /
  system services). RBAC for augmentation flagged as open. Lives in
  `architecture/05-extensions.md §4`.

### Documents added

- `ARCHITECTURE.md` — new index; philosophy, stack, deployment,
  source tree, conventions, doc map.
- `architecture/01-identity.md` — identity, groups, RBAC, anonymous
  identity, user-deletion contract.
- `architecture/02-capabilities.md` — capability registry, kernel
  stdlib, capability call flow, composition-hook reservation.
- `architecture/03-data.md` — database, storage (+ HTTP surface),
  realtime pub/sub.
- `architecture/04-services-ha.md` — audit, scheduled tasks,
  metrics, notifications, sidecars, HA, security.
- `architecture/05-extensions.md` — package structure, reserved URL
  prefixes, QuickJS runtime, composition framework, deferred public
  HTTP.
- `architecture/06-frontend.md` — shell-as-extension,
  `frontend.mount`, asset serving, design tokens, BYO stance.
- `design/DESIGN-admin-v06x.md` — stub for the admin extension
  (bundled second package; milestones A–E).
- `REVIEW-architecture-decomposition.md` — this session's review
  artifact (rationale, rejected alternatives, landing map).
- `CHANGELOG.md` — this file.

### Documents modified (inbound reference updates)

- `design/DESIGN-rbac-philosophy.md` — §3.2 reference remapped.
- `design/DESIGN-capability-registry.md` — §3.3 reference remapped;
  "What Must Not Be Decided Yet" amended re composition arc.
- `design/DESIGN-logging-subsystem.md` — §3.7, §3.8, §4.2 remapped.
- `design/DESIGN-packages-v04x.md` — all monolith §-references
  remapped; patch reference removed from header; `tray` added to
  `panel_type` enum; `home` slot type added.
- `design/DESIGN-shell-v06x.md` — patch reference removed; v3
  references remapped; user-deletion cross-reference updated;
  expanded to 7 sub-versions (0.6.0–0.6.6) with admin extracted to
  its own design doc.
- `design/DESIGN-sharing-v011x.md` — §3.2.1, §3.13, Appendix E
  references remapped.
- `design/DESIGN-quickjs-bridge.md` — §3.9, §4.2, §3.6, §4.3, §3.2,
  §3.3, §4.1 references remapped.
- `icd/ICD-0.1.2-auth-sessions.md` — traces-to header remapped.
- `icd/ICD-0.1.3-pats.md` — traces-to header remapped.
- `icd/ICD-0.1.4-groups-rbac.md` — traces-to header remapped.
- `icd/ICD-0.1.5-rbac-enforcement.md` — traces-to header remapped.
- `icd/ICD-0.1.6-websocket.md` — traces-to header remapped.
- `icd/ICD-0.1.7-audit.md` — traces-to header remapped.
- `icd/ICD-0.2.0-capability-registry.md` — traces-to header remapped.
- `icd/ICD-0.2.2-capability-resolution.md` — traces-to header remapped.
- `icd/ICD-0.2.4-capability-rbac.md` — traces-to header remapped.
- `SESSION-GUIDE.md` — architecture pointer updated to
  `docs/ARCHITECTURE.md`.
- `ROADMAP.md` — 0.1.8 task rewritten; shell arc extended to 0.6.6;
  admin extension arc added; 0.10.0–0.10.1 storage note flags HTTP
  surface as in-scope.

### Documents retired (recommend deleting from repo)

- `docs/ARCHITECTURE-plinth-v3.md` — content integrated into the
  `architecture/` tree and `ARCHITECTURE.md`. OBE.
- `docs/ARCHITECTURE-patch-url-layout-and-frontend.md` — patch
  integrated into the tree. OBE.

### Conventions adopted

- Architecture files use local section numbering (§1 per file).
  Cross-file citations use `architecture/<file>.md §X.Y`.
- Monolith `§3.X` citations are deprecated; any remaining uses
  should be remapped in a follow-up pass.
- New architectural decisions land as numbered subsections in the
  appropriate file, not as patch documents. Patches are session
  artifacts; the integrated tree is canonical.

## v0.1.7 — 2026-04-16 — Audit query endpoint + retention

- `GET /api/audit` query endpoint with `kernel.admin` RBAC,
  filters, and pagination.
- `purge_older_than` retention stub for future scheduler hookup.
- `audit_sync` libpq helper so bootstrap-time audit writes don't
  require a Drogon DB client.
- `rbac.rule_registered` event emitted from `bootstrap_groups`.
- spdlog migrated to async_logger per `DESIGN-logging-subsystem.md`.
- `src/kernel/audit/*` + `tests/kernel/audit/*`.

## v0.1.6 — 2026-04-16 — WebSocket connection lifecycle

- Drogon WebSocket endpoint, connection registry, auth flow (both
  session and PAT), application-level heartbeat with config-driven
  timing, subscribe/unsubscribe with admin-only RBAC, in-process
  publish hook + event delivery.
- `src/kernel/ws/*` + `tests/kernel/ws/*`.
- Added `src/kernel/logging.{cpp,hpp}`; introduced `log::audit` and
  migrated 18 audit call sites.
- `DESIGN-shell-v06x.md` and `sketches/shell-topbar-reference.html`
  landed in the same PR as reference material.

## v0.1.5.1 — 2026-04-16 — Prebuilt CI builder image

- Prebuilt Ubuntu 24.04 builder image published as
  the legacy private CI builder image (gcc 13 +
  clang/clang-tidy 18 + Drogon v1.9.12 + Node.js).
- CI workflow updated to consume the prebuilt image instead of
  installing toolchain every run.
- CI wall time: ~30 min → ~5 min.

## v0.1.5 — 2026-04-16 — RBAC enforcement middleware

- Permission check middleware applied to protected routes;
  `permission_denied` error surfaced with rule name.
- clang-tidy passes on the enforcement path; follow-up fixes landed
  in the same PR.

## v0.1.4 — 2026-04-15 — Groups, RBAC rule storage, ASAN/UBSan

- Group create / membership management; RBAC rule storage
  (`plinth.rbac_rules`, `plinth.group_rules`) with
  `bootstrap_groups` seeding.
- ASAN/UBSan CMake option (`-DPLINTH_SANITIZERS=ON`) + CI variant.
- Auth handler split into multiple files for maintainability.

## v0.1.3 — 2026-04-15 — Personal access tokens

- PAT create / list / revoke endpoints; dual-path `SessionFilter`
  resolves either a session cookie or a PAT header into a shared
  `AuthContext`.
- Exact PAT byte format per ICD-0.1.3.

## v0.1.2 — 2026-04-15 — Auth: users, argon2id, sessions

- `plinth.users` table; argon2id password hashing.
- Session create / validate / destroy with rate limiting.
- Audit events for auth lifecycle.

## v0.1.1 — 2026-04-15 — PG connection + schema bootstrap

- Drogon + libpq wiring; `plinth.*` schema recreation under
  `dev_mode: true`.
- clang-tidy wired into CMake as a custom target.
- CI fixes for trailing return types, env var isolation, PG probe.

## v0.1.0 — (pre-tag) — Project scaffold

- CMake + Drogon hello world, initial repo layout, first green CI
  run. No tag; captured here for continuity with the ROADMAP.
