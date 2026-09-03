# Deferred Design Decisions

Single running log of design decisions that were intentionally deferred
in a code milestone, with the reasoning and what the future implementer
needs to know. Replaces ad-hoc CHANGELOG mentions of deferred work,
which are too easily lost in release-notes scanning.

Each entry is dated and tagged with the milestone where the deferral
landed. Entries stay in this file until the deferred work is shipped
(at which point the entry is moved to a "Resolved" section at the
bottom — historical context preserved, but the active list stays
short).

Cross-referenced from:
- `docs/ROADMAP.md` — the actual work item carrying a band label.
- Source-code head comments in the relevant TUs.
- The originating ICD's §What Must Not Be Decided Yet section, when
  the deferral was anticipated at ICD-authoring time.

---

## Active

### 2026-09-03 — Browser/client portion of the 0.6.3.N JS-dispatch backfill

The executable server/kernel slice is complete: 32 bundled-shell cases now
cover ICD-0.6.1 P.01-P.14, ICD-0.6.2 T.01-T.08 and S.01-S.08, and
ICD-0.6.3 A.01-A.02 through resolver/RBAC, QuickJS, PostgreSQL, HTTP mapping,
and audit persistence.

The remaining families require a browser or client-runtime harness and were
not silently counted as server-dispatch work:

- ICD-0.6.1 I.* browser-to-kernel integration;
- ICD-0.6.2 I.* and U.* persistence/reload/UI controls;
- ICD-0.6.3 L.* panel lifecycle, C.* client HTTP behavior, S.* WebSocket
  subscription behavior, U.* Preact hooks, K.* keyboard behavior, R.* client
  stub behavior beyond existing source assertions, and I.* full-stack fixture
  behavior.

L.02/L.03 additionally depend on the later panel-switching runtime. These
families remain deferred until their owning runtime and a hermetic browser
harness exist. The older "33 deferred cases" phrase was arithmetically and
topologically ambiguous; future work must enumerate the chosen categories
from their ICD tables rather than reuse that number.

---



### 2026-04-30 — Kernel-side dispatch + teardown hardening (consolidated debt entry) [resolved in 0.6.3.N — shipped 2026-04-30]

**Milestone:** v0.6.3 ship surfaced three kernel-side bugs that had
been carried forward as "intermittent" / "out of scope for kernel
work" / "blocker for deferred tests" without scheduled owners. The
architect (2026-04-30) called this out as "too much lying on the
floor" and authorised consolidating them into a single scheduled
follow-up ahead of `0.6.4`. Tracked at ROADMAP §0.6 line
`0.6.3.N kernel-side dispatch + teardown hardening` (band `[strong]`,
blocking 0.6.4 because the topbar consumes `plinth.call` end-to-end).

**Three consolidated bugs:**

#### 1. JS handler `ctx` not injected (silent: discovered v0.6.3 manual smoke)

The QuickJS dispatch wrapper at
[`src/kernel/extensions/runtime_registry.cpp:298-306`](src/kernel/extensions/runtime_registry.cpp:298)
invokes `__mod.default(globalThis.__handler_args)` with **one**
positional argument. Every shell-extension handler signature is
`async function name({...}, ctx)` — the `ctx` parameter is undefined,
so `ctx.user.id` reads always throw `TypeError: cannot read property
'user' of undefined`.

Affected (silent — believed-working but broken end-to-end):
- `shell.preferences.get` (v0.6.1)
- `shell.preferences.set` (v0.6.1)
- `shell.preferences.get_all` (v0.6.1)

Working only because they don't read `ctx`:
- `shell.audit.emit` (v0.6.3) — handler signature is `(detail, _ctx)`
  with `_ctx` deliberately unused.

Why this hid for three milestones: ICD-0.6.1 §17 deviation #5
"deferred P.\*/I.\* JS-dispatch suite to 0.6.1.N" deferred the
*tests* without identifying the *root cause*; the handlers were
never exercised end-to-end from manual smoke either (manual smoke
gated only the static-asset path until v0.6.3 added `cap.call` to
the boundary chain).

**Fix path:** extend the wrapper to inject a second positional `ctx`
from `BridgeContext` (user.id + session_id + ip_address +
call_depth). The wrapper at runtime_registry.cpp:298-306 grows one
line:
```js
return await __mod.default(globalThis.__handler_args, globalThis.__handler_ctx);
```
plus a `JS_SetPropertyStr(bc.ctx, global, "__handler_ctx", ...)`
populated from `bc.user` ahead of `run_on_context`. Verifies via
`shell.preferences.get` round-trip from the cap-dispatch route +
the v0.6.1.N deferred tests.

**Manual smoke verification this session (2026-04-30, v0.6.3):**
- `POST /api/cap/shell.audit.emit` → 200 (handler doesn't read ctx)
- `POST /api/cap/shell.preferences.get` → 500 with
  `cap.handler_threw: TypeError: cannot read property 'user' of undefined`
- Confirmed via curl + browser-side fetch from preview.

#### 2. `init_registry` test-fixture teardown bug (test-fixture-buildout session 9, 2026-04-29)

Adding `init_registry(cfg)` to
[`tests/kernel/ws/ws_test_fixture.cpp::start_test_server`](tests/kernel/ws/ws_test_fixture.cpp)
to align the test fixture with production
([`src/kernel/main.cpp:352-360`](src/kernel/main.cpp:352)) triggers
heap corruption at teardown:
```
EventLoopThreadPool::~EventLoopThreadPool
munmap_chunk: invalid pointer
SIGABRT
```
The registry's pool dtor races Drogon's loop dtor. Per
`project_test_fixture_inflight.md` session 9, the symptom is
deterministic on `plinth_tests_pg`'s SIGABRT path.

Currently blocking:
- v0.6.1 P.s.\* / P.r.\* / P.h.\* tests (preferences JS-dispatch)
- v0.6.2 T.\* / S.\* / I.\* tests (theme + scale JS-dispatch)
- v0.6.3 L.\* / C.\* / S.\* / U.\* / I.\* / A.\* tests (panel SDK +
  client SDK + audit dispatch via QuickJS) — 33 cases

**Fix path:** order the teardown deterministically per
`feedback_deterministic_teardown.md` (cancel-all-from-atexit pattern
established in 0.4.0.1 audit-shutdown ordering fix), OR guard the
registry behind the same `g_inflight_*` shape `events_writer` got
in 0.5.5.1 (per CHANGELOG `0.5.5.1` §1 — `g_inflight_inserts` /
`g_inflight_cv` tracking gates `stop()` on detached-coroutine
completion). Pick at session start; both patterns are precedented
elsewhere in the kernel.

#### 3. Drogon `EventLoopThreadPool::~` join-self race (3+ exemplars, 9 ship-dates carried)

Family signature: `terminate called after throwing an instance of
'std::system_error'  what(): Resource deadlock avoided` (EDEADLK
from `pthread_self_join`). DEFERRED.md line 481 + 557 has called
this "Out of scope for kernel work" / "upstream Drogon issue" since
0.5.5.2 — that posture has held through v0.5.5.2, RE-EVAL, v0.6.0,
0.6.0.1, 0.6.0.N (×9 sessions), v0.6.1, v0.6.2, v0.6.3.

Documented exemplars (per
[`project_ws_flaky_segfault.md`](memory/project_ws_flaky_segfault.md)):
- `#38 async_hardening: parallel queries honour max_concurrent cap`
- `#47 pubsub_test.cpp:117 P.01`
- `#29 async_bridge: cap.batch fail-fast on first rejection`
- v0.6.3 PR CI surfaced a fourth: `live_replay_ordering_test.cpp:703 L.06`

Flake rate: ~1/5 on `plinth_tests_pg` per the documented dataset.
Each ship has been "re-run CI" + merge-anyway.

**Fix path (architect picks at session start):**
- (a) **Upstream patch + fork in `third_party/`.** Identify the
  exact Drogon commit that carries the bad join order; patch it;
  vendor under `third_party/drogon-patches/` with a CI-enforced
  rebase guard. Largest scope, most durable.
- (b) **Re-architect the test fixture.** Process-lifetime
  EventLoop singleton, no per-test pool — eliminates the
  unreachable join-self path entirely. Smallest scope; same
  pattern v0.5.5.1's PG-client `seq_generation_test.cpp::shared_pg_client()`
  process-lifetime accessor uses.
- (c) **Pin Drogon at a version where this is fixed** (if
  upstream has). Cheapest if it works; depends on whether
  Drogon's main has the fix.

Recommend (b) — it's where the pattern is already established
and "fix the test fixture" doesn't require upstream coordination.
Architect picks at session start.

**Why all three together:** the three bugs share a kernel-side
dispatch + teardown surface and a single fix session can attack
all three without scope-overlap. Splitting into three milestones
would forecast multiple session-restarts and longer carry-forward.

**Reference:** ROADMAP §0.6 lines `0.6.3.N kernel-side dispatch +
teardown hardening` + `0.6.3.N JS-dispatch test suite backfill`;
ICD-0.6.3 §17 deviation #13 (A.* tests deferred); ICD-0.6.1 §17
deviation #5 (P.\*/I.\* tests deferred); ICD-0.6.2 §17 deviation
#3 (cap.call kernel-side persistence deferred);
`project_test_fixture_inflight.md` session 9;
`project_plinth_state.md §Post-merge CI state`;
`project_ws_flaky_segfault.md`; CHANGELOG 0.5.5.1.

**Resolution (2026-04-30):** Shipped on
`feat/0.6.3.N-dispatch-teardown-hardening`. All three bugs closed.
Bug 1 fixed via wrapper change in
[`src/kernel/extensions/runtime_registry.cpp:298-307`](src/kernel/extensions/runtime_registry.cpp:298)
+ `__handler_ctx` injection (audit-frame projection). Two latent
bundled-shell handler bugs surfaced and fixed:
[`client/shell/server/handlers/preferences.{get,set,get_all}.js`](client/shell/server/handlers/preferences.get.js)
now bind `r.rows` (not the raw query result) and `JSON.parse` the
JSONB value column. Bug 2 fixed by mirroring production atexit chain
in [`tests/kernel/ws/ws_test_fixture.cpp`](tests/kernel/ws/ws_test_fixture.cpp:209)
(deterministic ordering, fix-path (a)) plus `static plinth::Config cfg`
lifetime fix. Bug 3 fixed via fix-path (b) — new shared header
[`tests/kernel/realtime/shared_pg_client.{hpp,cpp}`](tests/kernel/realtime/shared_pg_client.hpp)
generalising the `seq_generation_test.cpp:118-126` pattern across 9
realtime TUs (production code untouched; defense-in-depth
`g_inflight_*` gate stays). New tests `P.04` + `P.dispatch.01`
prove Bug 1 end-to-end through both synthetic and bundled-shell
paths. See CHANGELOG `2026-04-30 — 0.6.3.N kernel-side dispatch +
teardown hardening` for full file list, deviations (incl. B.shell.01
deferred to backfill milestone, `delta_sync_test` migration reverted
on advisory-lock interference), and verification posture.

---

### 2026-04-29 — Extension HTTP surface §6.10 sub-questions — multi-tenant host scoping, per-prefix handler_mode overrides, final performance threshold, drain-timeout default and bounds [deferred from 0.6.0.N architecture session 2026-04-29]

**Milestone:** the 2026-04-29 paper architecture session ratifying the
extension HTTP surface primitive (now normative at
[`architecture/05-extensions.md §6`](architecture/05-extensions.md))
deliberately did not pin four sub-questions. Each is a single-question
follow-up that does not block ICD-0.6.7 authoring or the 0.6.7
implementation milestone but should be answered before the relevant
shape ships.

The four sub-questions:

- **Multi-tenant per-host scoping** for prefix claims. The 2026-04-29
  session pinned single-tenant exclusive ownership only ("no host
  scoping in single-tenant"). If multi-tenant deployments materialize
  (separate `*.plinth.example.com` per tenant, or a shared instance
  with per-host extension installations), prefix-claim resolution
  needs a host dimension. Future architecture session.
- **Per-prefix `handler_mode` overrides.** The session pinned one
  mode per extension via the manifest field. A real use case where
  one extension wants `bundled_native` for `/dav/*` (high-throughput)
  and `quickjs` for `/admin/*` (low-throughput admin UI) on the same
  manifest would re-open this. Punted to ICD-0.6.7 if a need surfaces.
- **Final performance threshold.** Starting figure ≤ 100 μs catch-all
  overhead vs fixed-prefix; the actual ICD-0.6.7 number is set after
  a measurement pass on the implemented primitive. Could be tighter
  (≤ 50 μs) or looser (≤ 200 μs) depending on what Drogon's path
  matching adds.
- **Drain timeout default and configurable bounds.** §6.5 pins the
  drain semantics ("complete in-flight, 503 new, remove on count=0
  or timeout") but punts the specific timeout value (and its lower
  / upper bounds) to ICD-0.6.7. Likely range: 5–60 s.

**Why deferred:** Each is independently scoped — none of them
foreclose the others. Pinning them in the architecture session would
have over-determined the ICD; pinning them in the ICD without
measurement (performance threshold) or without a real use case
(multi-tenant, per-prefix mode) would have been speculative.

**Future approach:** Multi-tenant scoping likely lands at whatever
arch session opens multi-tenant deployments (post-1.0). Per-prefix
mode resolves at ICD-0.6.7 authoring if a candidate use case is
already on the table; otherwise Closes-by-foreclosure (the manifest
field stays per-extension). Performance threshold pinned at
ICD-0.6.7 authoring with explicit "subject to measurement" hedge —
final number lands in the 0.6.7 ship PR's §17 amendment block per
ICD-0.5.5 / ICD-0.6.0 OQ Resolutions precedent. Drain timeout
defaults pinned at ICD-0.6.7 authoring; can be re-tuned post-ship
if a real workload demands.

**Pointers:**
- [`docs/architecture/05-extensions.md §6.10`](architecture/05-extensions.md)
  enumerates these in-place.
- [`docs/discussion/DISCUSSION-extension-http-surface.md`](discussion/DISCUSSION-extension-http-surface.md)
  Risks-and-Unknowns section frames the original questions.

### 2026-04-26 — ICD-0.5.4 I.02 / I.03 + ICD-0.5.5 S.06 / S.07 / L.03 / L.04 / L.05 / W.06 / I.01 / I.02 / I.03 / I.04 — realtime test coverage + W.06 design defer [deferred from v0.5.4 / v0.5.5 ship; ICD-0.5.4 I.02 closed in 0.6.0.N session 3; ICD-0.5.4 I.03 discharged via L.03 in session 6; ICD-0.5.5 L.03 / L.04 / L.05 closed in 0.6.0.N session 6; ICD-0.5.5 S.06 closed in 0.6.0.N session 8]

**Milestone:** v0.5.4 enumerated 41 ICD test cases and shipped 38
(D.08 absorbed into ICD-0.5.5 as L.03; **I.02 multi-process advisory-
lock harness** + **I.03 live + replay race in integration** still
active per CHANGELOG v0.5.4 §Test count). **I.02 closed in 0.6.0.N
session 3 (2026-04-27)** at
[`tests/kernel/realtime/events_writer_advisory_test.cpp`](../tests/kernel/realtime/events_writer_advisory_test.cpp)
once the multi-process advisory-lock harness landed. The test forks
4 children that all execute the writer's lock-and-insert SQL
(`BEGIN; pg_try_advisory_xact_lock(<key>); INSERT ...; COMMIT;` —
literal-string mirror of [`events_writer.cpp:322-345`](../src/kernel/realtime/events_writer.cpp:322))
against the same `(channel, emitted_at)` lock key, then asserts
exactly one INSERT committed. **I.03 stays deferred** to a session
that extends the WS test fixture with frame-inspector + buffer-cap
hooks. v0.5.5 enumerated 36 cases and shipped 29; the 7-case
deferral splits into three buckets per CHANGELOG v0.5.5 §Test counts:

- **S.06** broker-side `realtime.seq.gap_detected` audit —
  **closed 0.6.0.N session 8 (2026-04-28)** at
  [`tests/kernel/realtime/gap_detection_test.cpp`](../tests/kernel/realtime/gap_detection_test.cpp)
  via per-conn cache + immediate-send-path detection in
  [`src/kernel/ws/publish.cpp`](../src/kernel/ws/publish.cpp). See
  the dedicated Resolved entry below for the full close-out.
  **S.07** cursor catch-up after writer crash mid-window remains
  open and reuses the v0.5.4 I.02 multi-process harness's reserved
  `run_with_kill` extension once the SIGKILL-family follow-up lands.
- **L.03 / L.04 / L.05** mid-replay live-buffer integration +
  overflow scenarios — **closed 0.6.0.N session 6 (2026-04-28)** at
  [`tests/kernel/realtime/live_replay_ordering_test.cpp`](../tests/kernel/realtime/live_replay_ordering_test.cpp)
  via the new `WsTestClient` drain-pause + frame-inspector hooks
  plus the `live_buffer_cap_override` test seam (production-side at
  [`src/kernel/ws/subscriptions.hpp`](../src/kernel/ws/subscriptions.hpp)).
  Live events injected via `broker::dispatch_for_test` (skips the
  writer's PG INSERT so they cannot be picked up by replay's next
  paginated SELECT). Small chunk sizes (1 or 10) extend the
  `replay_in_flight=true` window deterministically wider than the
  test thread's deliver lambda queueing. **ICD-0.5.4 I.03
  discharged via L.03** per OQ7 absorption — same scenario verbatim,
  status note added to ICD-0.5.4.
- **W.06** `superseded_seqs[]` populated. Design-deferred per
  ICD-0.5.5 §6 §Implementation seam — writer-first topology
  forecloses source-seq tracking through the existing
  coalescer→writer boundary.
- **I.01 LH-1 storm** in CI rather than hand-run; **I.02 LH-2
  sidecar** fanout; **I.03 multi-node failover** (same harness as
  v0.5.4 I.02); **I.04 cross-extension publish + subscribe
  ordering**.

**Why deferred:** Three buckets, three different shaped follow-ups:

- **Multi-process advisory-lock harness** — v0.5.4 I.02 + v0.5.4
  I.03 + v0.5.5 S.07 + v0.5.5 I.02 + v0.5.5 I.03 (multi-node
  failover) all share the same cross-process integration scaffolding.
  Building it once retires five deferrals simultaneously. Useful for
  any future HA-posture validation.
- **Live-buffer fault-injection seam** — L.03/L.04/L.05 need a way
  for the WS test client to drive the per-conn live buffer past
  its cap mid-replay. The current `WsTestClient` emits frames
  but does not expose buffer-state hooks.
- **W.06 source-seq tracking design** — writer-first foreclosure
  means `superseded_seqs[]` cannot be populated through the existing
  pipeline. A follow-up ICD (post-0.5.5) is required to choose
  between (a) plumbing source-seq through the coalescer→writer
  boundary, (b) re-introducing a peer-listener path for the
  superseded-seq side-channel, or (c) discarding the field
  entirely. Decision out-of-scope for v0.5.5.

The shipped 29 of v0.5.5 + 38 of v0.5.4 cover every documented
invariant transitively; the deferred cases extend coverage to
specific edge conditions (audit attribution under multi-node
failover, mid-replay buffering at the cap, source-seq under
optimistic-update SDK paths) that the LH-1/LH-2 stream exercises
empirically at production-kernel scale (see v0.5.5 LH-1 pre-flight
table — 7 ms p99 across 3 trials × 4 producers × 4 subscribers ×
120 s × 1.0000 observed/emitted ratio).

**Current behavior (post-v0.5.5.1):** Both ICDs' contracts are
proven library-side; LH-1 storm tier validates the live-path under
sustained load; LH-2 fan-out tier validates broker dispatch under
sustained subscribers. Catch2 coverage of the deferred cases is the
gap.

**Future approach:** Two ROADMAP slots cover the three buckets:

- **Multi-process advisory-lock harness** landed in 0.6.0.N session 3
  (2026-04-27) at
  [`tests/kernel/packages/advisory_lock_harness.{hpp,cpp}`](../tests/kernel/packages/advisory_lock_harness.hpp);
  ICD-0.5.4 I.02 closed in the same session. Remaining harness
  consumer **S.07** (writer-crash mid-window cursor catch-up) needs
  the harness's reserved `run_with_kill` extension; **I.01–I.04** live
  in `load-harness/`, not `plinth_tests`, so they're out of the
  test-fixture-buildout scope.
- **Live-buffer fault-injection seam** — landed in 0.6.0.N session 6
  (2026-04-28) at
  [`tests/kernel/ws/ws_test_fixture.{hpp,cpp}`](../tests/kernel/ws/ws_test_fixture.hpp)
  + production-side seam at
  [`src/kernel/ws/subscriptions.{hpp,cpp}`](../src/kernel/ws/subscriptions.hpp).
  Closed L.03/L.04/L.05 + discharged ICD-0.5.4 I.03 in the same
  session.
- **S.06 broker-side gap-detection pipeline** — landed in 0.6.0.N
  session 8 (2026-04-28); production pipeline added at
  [`src/kernel/ws/publish.cpp`](../src/kernel/ws/publish.cpp)
  (immediate-send-path detection in `deliver_to_conn` against a per-
  channel cache on `ConnState`). See dedicated Resolved entry below.
- **W.06 source-seq tracking design** — `0.6.3.N ICD-0.6.x source-seq
  tracking authoring` `[fuzzy]` under `## 0.6 — Frontend Shell`.
  Authors a follow-up ICD post-0.6.3 (or earlier if SDK work binds
  the optimistic-update path tighter than expected). Three options
  on the table: (a) plumb source-seq through coalescer→writer
  boundary, (b) re-introduce a peer-listener path for the
  side-channel, (c) discard the field entirely.

**Reference:** `docs/icd/ICD-0.5.4-events-table-delta-sync.md §Test
Cases`; `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md
§Test taxonomy`; CHANGELOG v0.5.4 §Test count + v0.5.5 §Test counts;
`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md §2.3`;
`docs/ROADMAP.md` `## 0.x cleanup follow-ups` + `## 0.6 — Frontend Shell`.

---

### 2026-04-23 — ICD-0.5.0.3 R.02 / R.03 / E.07 / P.01 / P.04 / P.05 / H.02 / H.03 / C.01 / C.02 — extension-dispatch test coverage [deferred from 0.5.0.4 ship]

**Milestone:** ICD-0.5.0.3 enumerates ~20 integration test cases
across Groups R, E, P, H, C. 0.5.0.4 shipped R.01 (echo happy path),
E.01–E.06 (error taxonomy: handler throw / unknown signature /
missing handler file / handler syntax / destroyed pool race / sync
path `async_required`), P.02 (spoof-other-ext pubsub identity gate),
P.03 (RBAC at resolver), and H.01 (destroy/create cycle) — 10 cases
under `tests/kernel/capabilities/dispatch_extension_test.cpp`. The
remaining 10 cases (R.02 WS end-to-end, R.03 audit attribution,
E.07 8-deep call-depth chain, P.01 pubsub-from-handler, P.04
`db.query`-from-handler, P.05 cross-extension recursion, H.02
UPGRADE rebuild, H.03 DISABLE mid-dispatch race, C.01 cancellation
propagation, C.02 wall-clock expiry tear-down) deferred to
follow-ups per CHANGELOG 0.5.0.4 §Tests.

**Why deferred:** Three cases need specific external harness that
0.5.0.4 did not build:

- **R.02 (WS end-to-end)** — exercised through the `POST /api/packages`
  flow plus a WS session seeded with RBAC; relies on the same
  Drogon-HTTP-with-RBAC-seeded-session fixture the
  `0.5.x.N HTTP test harness` ROADMAP slot builds.
- **P.04 (`db.query` from handler)** — exercises per-op
  `SET search_path` plumbing; gated on the 2026-04-18 DEFERRED entry
  for that work (now pointer-tightened to 0.5.3 — see below).
- **P.05 (cross-extension recursion)** — installs two fixtures
  simultaneously; requires a second install-fixture helper the
  current single-fixture harness doesn't expose.
- **C.02 (wall-clock expiry)** — requires a slow-handler fixture plus
  a way to trip wall-clock without also tripping the faster CPU
  budget; needs custom fixture timing that wasn't worth the 0.5.0.4
  window.

The remaining six (R.03, E.07, H.02, H.03, C.01) are individually
achievable with the current fixture but were grouped with the
cluster items above to land as a single follow-up rather than
dripping in across multiple PRs.

**Current behavior (0.5.0.4):** Extension-dispatch contract is proven
end-to-end at library level by R.01 + E.01–E.06 + P.02 + P.03 + H.01.
LH-1 storm tier (2026-04-22 + v0.5.1 regression) exercises the WS
end-to-end path at production-kernel scale — R.02 is covered
empirically by LH-1 3-trial clean, just not by Catch2. The deferred
cases extend coverage to specific edge conditions (nested RBAC
propagation, audit-row attribution under nested dispatch, race
windows between dispatch and lifecycle transitions) that the shipped
cases cover transitively but not directly.

**Future approach:** All ten cases fold into the `0.6.0.N Test-fixture
buildout` ROADMAP slot alongside ICD-0.4.4 I.18/I.19/I.20 and
ICD-0.4.5 X.05–X.13 and ICD-0.5.4/0.5.5 deferrals. P.04 already
unblocked (per-op `SET search_path` resolved in 0.5.3 phase 3 — see
Resolved section). P.05 + the remaining six achievable once the
fixture extends with the second-install-fixture helper.

**Reference:** `docs/icd/ICD-0.5.0.3-extension-dispatch.md §Test plan`
(Groups R/E/P/H/C); CHANGELOG 0.5.0.4 §Tests;
`RE-EVAL-0.5.x-following-0.5.1.md §2.3`.

---

### 2026-04-22 — ICD-0.4.5 X.05 / X.06 / X.07 / X.08 / X.09 / X.10 / X.11 / X.12 / X.13 + G.03 — extended upgrade and GC test coverage [deferred from v0.4.5 ship; G.03 closed in 0.6.0.N session 3; X.05 / X.06 / X.07 (partial) / X.10 / X.11 / X.13 closed in 0.6.0.N session 4; X.07 missing+changed sub-cases / X.08 / X.09 closed in 0.6.0.N session 9]

**Milestone:** ICD-0.4.5 enumerates ~25 test cases across the
D.* / U.* / X.* / G.* prefixes. v0.4.5 Slice B shipped X.01 (happy-path
upgrade with full verification), X.02 (same-version rejection), X.04
(DISABLED rejection), and G.02 (full-cycle GC with FK cascade) per
CHANGELOG v0.4.5 §Slice B / B10. Deferred to a follow-up:
**X.05** (upgrade with new migrations), **X.06** (migration failure
aborts), **X.07** (3-way RBAC reconciliation at more scale), **X.08**
(in-flight call completes within drain window), **X.09** (drain
timeout exceeded), **X.10** (old URL returns 404 post-cutover),
**X.11** (both versions on disk during retention window), **X.12**
(crash at swap T3), **X.13** (concurrent POSTs serialize via
advisory lock), and **G.03** (GC skips advisory-locked row).

**G.03 closed in 0.6.0.N session 3 (2026-04-27)** at
[`tests/kernel/packages/lifecycle_gc_advisory_test.cpp`](../tests/kernel/packages/lifecycle_gc_advisory_test.cpp)
once the multi-process advisory-lock harness
([`tests/kernel/packages/advisory_lock_harness.{hpp,cpp}`](../tests/kernel/packages/advisory_lock_harness.hpp))
landed. The test forks a child that holds
`pg_try_advisory_lock(hashtextextended('plinth.packages.<name>', 0))`
(mirroring [`install_lifecycle.cpp:177-197`](../src/kernel/packages/install_lifecycle.cpp:177)
`try_acquire_name_lock`) and signals "READY" via the harness's pipe;
the parent then runs the production
`garbage_collect_superseded_versions(0h, ctx)` directly and asserts
`skipped_ids` contains the package id. Phase 2 re-runs after the
child releases and asserts the row collects on the lock-clear path.

**X.05 / X.06 / X.10 / X.11 / X.13 + X.07 (partial) closed in 0.6.0.N
session 4 (2026-04-28)** at
[`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp).
The new file extends the session-2 `HttpTestFixture` with
`build_patch` / `build_delete` builders and adds
`test_seam::dispatch_patch` / `dispatch_delete` thin forwarders to
[`src/kernel/packages/handlers.{hpp,cpp}`](../src/kernel/packages/handlers.hpp:50)
(infra prep for future D.* / U.* HTTP-coverage sessions; not used
by the X.* cases themselves). Two new fixtures wire in: a
`002_add_notes_comments.sql` migration appended to the existing
`upgrade-v2` tree (X.05's "new migration on upgrade" enabler) and a
new `upgrade-v2-broken-migration` fixture (notes 1.4.0 with a
deliberately-broken `002_broken.sql` for X.06's MIGRATING-stage
failure on upgrade). **X.07 partial: new-rule sub-case only**
(notes.comment lands with `orphaned_at IS NULL`); the missing-rule
+ changed-rule sub-cases needed a different fixture pair (v1 with
≥2 rules, v2 dropping one + changing another) which session 9
authors.

**X.07 missing + changed + X.08 + X.09 closed in 0.6.0.N session 9
(2026-04-29)** at
[`tests/kernel/packages/lifecycle_transitions_http_test.cpp`](../tests/kernel/packages/lifecycle_transitions_http_test.cpp).
New fixture pair
[`tests/fixtures/lifecycle_transitions/upgrade-v{1,2}-slow/`](../tests/fixtures/lifecycle_transitions/upgrade-v1-slow/)
ships extension `slow` v1.0.0 / v2.0.0 with the three RBAC
reconciliation deltas in one shot: `slow.alpha` description changes
(changed-rule), `slow.beta` drops in v2 (missing-rule), `slow.delta`
new in v2 (new-rule, redundant with session 4 but cheap), `slow.gamma`
unchanged. X.08 + X.09 use a new `InflightSimulator` test helper
(`tests/kernel/packages/lifecycle_transitions_http_test.cpp`) plus a
new `plinth::packages::test_seam::upgrade_drain_timeout_ms_override`
production-side override seam mirroring the
[`live_buffer_cap_override`](../src/kernel/ws/subscriptions.cpp:584)
pattern. The simulator pre-`begin_drain`s and holds a worker-thread
`DispatchGuard` so the upgrade's `wait_for_zero` observes counter=1
from sample one — bypassing the microsecond race between T1
`begin_drain` and T2 `wait_for_zero` that an async cap.call cannot
beat. CHANGELOG `2026-04-29 — 0.6.0.N test-fixture buildout, session 9
of N` carries the full deviation block. **X.09 status code asserts the
production deviation (HTTP 400 with `failed_at_stage=UPLOADING`, not
the ICD's 504)** — same root cause as session 4's X.06 deviation
(`install_lifecycle.cpp:1402` hardcodes `failed_at = UPLOADING` for
the upgrade-path conversion); both reconcile together as the
failure-conversion follow-up.

**X.12 remains deferred** — see "Future approach" below.

**Why deferred:** The deferred set splits roughly into two clusters
that each need shared infrastructure not yet in place:

1. **HTTP-fixture cluster** — X.13 (concurrent POSTs) and any future
   "drive an upgrade end-to-end via `POST /api/packages` with two
   sessions" extension of the others share the same
   Drogon-HTTP-with-RBAC-seeded-session fixture that I.18/I.19/I.20
   need (see the entry above). Building it for the deferred X-cases
   alone was judged not worth the v0.4.5 window.
2. **Crash-injection cluster** — X.12 (crash at swap T3) and
   variant crash-recovery cases need a SIGKILL-the-subprocess
   harness; v0.4.5 Slice B explicitly avoided fork/exec/SIGKILL per
   the same reasoning v0.4.4 Slice B did (sidesteps the parked
   trantor teardown flake; library-level crash-recovery exercised
   by manufacturing in-flight DB states).

The remaining cases (X.05–X.11, G.03) are individually achievable
without new fixture work but were grouped with the cluster items
to land as a single follow-up rather than dripping in across
multiple PRs.

**Current behavior (v0.4.5):** Library-level upgrade path is fully
covered by X.01/X.02/X.04 (happy / same-version / DISABLED) +
the existing reconciler tests (mid-swap crash recovery already
verified by manufactured in-flight DB states). The deferred X.*
cases extend coverage to specific edge conditions (drain timeout
boundaries, RBAC reconciliation scale, on-disk retention
verification, advisory-lock contention) that the library-level
tests cover transitively but not directly.

**Future approach:** Tracked on the ROADMAP as
**`0.6.0.N Test-fixture buildout`** under `## 0.x cleanup follow-ups
(cross-cutting)` `[strong]` — the same slot that absorbs the
I.18/I.19/I.20 entry above. Pick up the 0.4.5.1 grouped-ctest
model: `plinth_tests_pg` for PG-only cases, `plinth_tests_ws` for
tests that bring up the HTTP listener. Crash-injection cases (X.12)
need a separate decision on whether to add the fork/SIGKILL harness
or continue substituting library-level state-injection — either is
defensible; the ROADMAP slot's scope leaves this open.

**Future approach (post-session-9):** One follow-up owns the
remaining case:

1. **X.12 (crash at swap T3)** — SIGKILL extension of the session-3
   `AdvisoryLockHarness`. The harness already reserves the design
   shape (`run_with_kill(int n, ChildFn body, KillSpec spec)`) per
   the session-3 macro plan §C. Same session also closes ICD-0.5.5
   S.07 (cursor catch-up after writer crash mid-window).

**Reference:** ICD-0.4.5 §Test Cases (X.* and G.* sections);
CHANGELOG v0.4.5 §Slice B / B10 deviation 4;
`RE-EVAL-0.4.x-arc-closeout.md §2.10 + §5.3 + §7`;
`docs/ROADMAP.md` `## 0.x cleanup follow-ups`;
CHANGELOG `2026-04-27 — 0.6.0.N test-fixture buildout, session 3 of N`
(G.03 close);
CHANGELOG `2026-04-28 — 0.6.0.N test-fixture buildout, session 4 of N`
(X.05/X.06/X.07-partial/X.10/X.11/X.13 close);
CHANGELOG `2026-04-29 — 0.6.0.N test-fixture buildout, session 9 of N`
(X.07 missing+changed / X.08 / X.09 close).

---

### 2026-04-18 → 2026-04-24 — Per-op `SET search_path` for `db.*` (ICD-0.3.3) [resolved in 0.5.3 phase 3]

See the **Resolved** section below — moved 2026-04-24 on 0.5.3 phase 3
ship.

---

### 2026-04-18 → 2026-04-24 — `db.*` PG-type→JS-type mapping (ICD-0.3.3) [resolved in 0.5.3 phase 1]

See the **Resolved** section below — moved 2026-04-24 on 0.5.3 phase 1
ship.

---

### 2026-04-19 → 2026-04-27 — WS teardown flake / `[js][async]` Catch2 subprocess race [resolved in 0.5.5.2]

**Milestone:** Surfaced during 0.3.1 merge CI; cross-milestone bandaid
ladder accumulated across 0.3.3 (client `WsTestClient::stop`),
0.3.3.1 (server `ConnectionRegistry::initiate_shutdown` /
`g_shutdown_pending`), 0.3.3.2 (audit sub-path discovery), 0.3.4.1
(`plinth::log::shutdown` + lazy DbClient + MEMORY_LIMIT peak-tracking),
0.4.0.1 (`drain_pending_jobs` + `cancel_all_timers`), 0.4.4.1
(`dispatch_for_test` direct-call), 0.4.5 Slice B (**leaked-singleton
fix on `ConnectionRegistry`, `af8bbdd`** — closed the Meyers-singleton
sub-path), and 0.4.5.1 (`cancel_all_timers` ConnState-iteration
rework, mid-PR). Eight rungs total.

**Status — empirical resolution on the production-kernel side
(2026-04-21):** LH-0.1 ran 3 trials × 2 minutes against the production
kernel at concurrency=4: **133,755 `js_stress` calls / ~535,020
`db.query` operations / ~535k `signal_completion` callbacks. Zero
reproductions** of `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`,
or `bad_weak_ptr` across all 3 trials. RSS stable 24→26 MiB; clean
SIGTERM shutdown afterwards. The architect's working hypothesis
(`RE-EVAL-0.4.x-following-0.4.4 §2.2`: "the race is a test-harness
teardown artifact, not a production-path race") is now an empirical
finding, not a hypothesis.

**Residual scope — `[js][async]` Catch2 subprocess race:** Per
`docs/CHANGELOG.md` 0.4.5.1 §Async caveat, the 45 `[js][async]` test
cases retained their pre-existing ~10–15% per-TEST_CASE flake rate
under the `catch_discover_tests` per-TEST_CASE model. Grouping
these tests under `plinth_tests_async` AMPLIFIED the race to ~15–25%
per group; some test combinations deadlocked indefinitely. Investigation
2026-04-21 narrowed the trigger to **sequential test runs in the same
subprocess** (`[js][async][hardening]` alone clean; `[js][async]
~[hardening]` mostly clean; full `[js][async]` flakes). Root cause is
kernel-side — a refcount leak in the async bridge's
`dispatch_async_op_detached` / `SqlBinderAwaiter` /
`AnyCompletionAwaiter` coroutine path, candidate list in
`project_ws_flaky_segfault.md §Candidate root causes`. The race only
surfaces under the test-harness lifecycle (sequential-run-in-subprocess);
LH-0.1 confirmed production is unaffected.

**2026-04-26 — sub-paths closed by 0.5.5.1 + family expansion
observed post-merge.**

Three 0.5.5.1 fixture-layer fixes contained the test-harness lifecycle
sub-paths the kernel teardown family had been surfacing through
(CHANGELOG `0.5.5.1` §What shipped 1–3):

1. **`events_writer` `EventLoopThreadPool::~` join-self family
   closed at the test-fixture layer** via `g_inflight_inserts` /
   `g_inflight_cv` tracking. The framework-layer race (drogon's
   join-self pattern in `EventLoopThreadPool::~`) remains a Drogon
   issue, but the `events_writer` callsite no longer reproduces.
2. **`bad_weak_ptr` from `drogon::async_run` in test-mode
   subprocesses** caught + suppressed via try/catch wrappers around
   both `runEvery` callbacks (drain + cleanup) — keeps it from
   escaping into trantor's `EventLoop::loop` and tripping
   `std::terminate` past `loopFuncs`'s noexcept boundary.
3. **PG-client per-test create+destroy `bad_weak_ptr` cycle**
   closed via `seq_generation_test.cpp::shared_pg_client()`
   process-lifetime accessor — process-exit destructor handles
   teardown after Catch2 has reported.

These closures retire the events_writer / ConnState / per-test
PG-client sub-paths from this entry's scope. The entry now stays
Active *only* for the residual `[js][async]` Catch2 subprocess race
(below) and the framework-layer drogon `EventLoopThreadPool::~`
join-self pattern.

**Family expansion observed in `ci-build-and-test-12377` (first
post-merge CI run on `main` after 0.5.5.1).** A new exemplar joined
the documented family:

- **#29 `async_bridge: cap.batch fail-fast on first rejection`**
  (`tests/kernel/js/async_bridge_test.cpp:873`) — SIGSEGV with
  backtrace `JS_FreeValueRT → JS_FreeValue → JS_ExecutePendingJob
  → drogon::sync_wait`. Same kernel-side refcount family as #38 +
  #47; the `cap.batch` test became reachable when 0.5.3 introduced
  `db.batch` to the dispatch surface — the same refcount path was
  always there but the test count for it was zero before 0.5.3.
  The 0.5.5.1 verification table at CHANGELOG line 88–94 ran
  `[js] ~[async]` (excluded the async bucket) — the post-merge
  full-suite run does not exclude it.

The documented family now lists **three exemplars**: #38
`async_hardening: parallel queries honour max_concurrent cap`,
#47 `pubsub_test.cpp:117` P.01, and #29 `cap.batch fail-fast on
first rejection`. The candidate-root-cause hypothesis at
`project_ws_flaky_segfault.md` is reconfirmed (the race is
reachable via more entry points than `async_hardening` alone;
likely an ownership bug in
`dispatch_async_op_detached` / `SqlBinderAwaiter` /
`AnyCompletionAwaiter`).

**Promotion 2026-04-26.** Per the maintainer's directive at
`RE-EVAL-0.5.x-following-0.5.5.md §2.4` ("0.5.x is the kernel's
last hardening window"), the ROADMAP slot
`0.5.x.N [js][async] kernel-side refcount investigation` promotes
`[medium]` → `[strong]` and renames to **`0.5.5.2 [js][async]
refcount fix [strong]`**. Scheduled as the immediate next code
session, ahead of `0.5.5.N ICD-0.6.0 authoring`. Entry condition:
candidate-root-cause list in `project_ws_flaky_segfault.md
§Candidate root causes`. Exit condition: `[js][async]` grouping
enabled with the ~10× subprocess-count reduction the rest of the
suite already enjoys. **0.6.0 must not begin until this closes.**

**RESOLVED 2026-04-27 in 0.5.5.2** (untagged, branch
`fix/0.5.5.2-js-async-refcount`). Root cause was NOT the candidate
list above (no ownership bug in
`dispatch_async_op_detached` / `SqlBinderAwaiter` /
`AnyCompletionAwaiter`). The actual root cause: the test fixture's
`drogon::sync_wait(run_on_context(bc, src))` spawned a fresh
`std::thread` that ran the coroutine's synchronous prefix (`JS_Eval`
+ first `drive_jobs` + dispatch); when a dispatched op completed
faster than the prefix reached `co_await AnyCompletionAwaiter`, the
completion's `queueInLoop` callback fired on `main_loop`'s thread
**concurrently** with the prefix still touching JSValues on the
sync_wait thread — breaking the
[`run_on_context.cpp:9-12`](src/kernel/js/run_on_context.cpp:9)
"QuickJS access serialized inside this coroutine body" invariant
that the kernel relied on for thread-safety. The 0.3.3.1 mutex
barriers in `signal_completion` / `AnyCompletionAwaiter` synchronise
the *handoff* but don't kick in until the *first* suspend — leaving
the initial-dispatch window unprotected. Fix: pin
`run_on_context` to `main_loop` at coroutine entry via
`drogon::switchThreadCoro` (no-op in production where the handler
already runs on the loop), plus widen `AnyCompletionAwaiter`'s
suspension condition to also suspend under back-pressure (the
dispatcher on `main_loop` would otherwise spin and starve the
completions it's waiting on). 100/100 iters clean on the two
refcount-race exemplars. CMakeLists.txt folded `[js][async]` into
the grouped `plinth_tests_js` subprocess; `~10×` subprocess-count
reduction delivered. `async_hardening_test.cpp:151` N.39 scale
restored from 4 × 2 to ICD-quoted 100 × 8. Three exemplars now
green in 100-iter loops. See `docs/CHANGELOG.md` 2026-04-27 entry
for the full root-cause + fix discussion.

**Residuals (separate families, not fixed by 0.5.5.2):**
- `pubsub_test.cpp:117` P.01 happy path — pre-existing v0.5.0
  5-second `cv.wait_for` timing flake under PG NOTIFY contention.
  NOT a refcount race. Tracked separately as a low-priority follow-
  up.
- Framework-layer drogon `EventLoopThreadPool::~` join-self pattern
  remains an upstream Drogon issue, surfacing on `plinth_tests_pg`
  intermittently (~1/5). Out of scope for kernel work.

**Bonus: MEMORY_LIMIT classifier flake (closed by 0.5.5.2 part 3).**
Surfaced during the verification of parts 1 + 2 — the
`limits_test.cpp:158` `limits: promise-allocation loop trips
MEMORY_LIMIT` test was flaking at ~30–60% under the grouped
subprocess shape because the synchronous OOM (`(()=>{ for(...)
keep.push(new Promise(()=>{})); })()`) fires inside `JS_Eval`
before `drive_jobs` has a chance to call `sample_memory_peak`,
AND the post-OOM `malloc_size` had drifted below the 256 KiB
classifier slack by classification time. Closed by (a) calling
`sample_memory_peak(bc)` immediately after `JS_Eval` so the latch
catches synchronous OOMs while the peak is fresh, and (b) bumping
`OOM_MEMORY_SLACK_BYTES` from 256 KiB → 1 MiB. Distinct from the
0.3.4.1 classifier work which added the latch + the live
`is_runtime_near_memory_limit` check; this tunes the slack + adds
a sample-point that 0.3.4.1 didn't reach.

---

**2026-04-23 — empirical dataset from 0.5.1.1.** 100-iteration `ctest`
loops on the v0.5.1 tag produced three new measurements recorded
here so the future refcount-investigation session has concrete
numbers to regress against:

- **#38 `async_hardening: parallel queries honour max_concurrent cap`
  per-TEST_CASE** (current CI mode): 92 pass / 8 SEGV = **8.0%**
  flake rate. Signatures: `quickjs.c:6678 free_zero_refcount
  assert p->ref_count == 0`, `quickjs.c:2323 JS_FreeRuntime assert
  list_empty(gc_obj_list)`. Confirms the candidate-root-cause list.
- **#38 under grouped `[hardening]` single subprocess**
  (hypothetical Shape 1 of the 0.4.5.1-style redesign): 89 pass /
  11 SEGV = **11.0%**. Grouping does NOT help for this specific
  case, which **invalidates the 2026-04-21 "`[js][async][hardening]`
  alone clean" claim at 100-iter scale on v0.5.1's commit** — the
  2026-04-21 claim stands for shorter samples or earlier commits
  but does not hold at the dataset this entry now captures.
- **#47 `plinth_tests_js` P.01 `pubsub.publish happy path`**
  (`tests/kernel/js/pubsub_test.cpp:117`, `cv.wait_for(5s, ...)`
  timed out): reproducible at **~15% in isolation**. Pre-existing
  v0.5.0 flake (since commit `f3552b3`), `[js][realtime][integration]`
  tag-overlap violation via `async_bridge_fixture` composition.
  Never filed until 0.5.1.1 §Verification surfaced it. **Folded
  into this entry's scope — same signature family (sequential
  subprocess under `async_bridge_fixture`), same root-cause
  hypothesis, same resolution slot.** The scoped ROADMAP item
  `0.5.x.N [js][async] refcount investigation` absorbs P.01 as an
  exemplar under its scope.

Production unaffected — LH-0.1 (2026-04-21) + v0.5.1's LH-1
regression (2026-04-23) both ran under production lifecycle with
zero reproductions of this signature family. The refcount-investigation
session enters with empirical data from three separate commits now
(2026-04-21 initial, v0.5.1 dataset, v0.5.0 P.01 isolation).

**Why entry stays Active:** the `[js][async]` race is unresolved.
The decision to live with the per-TEST_CASE flake rate (vs. invest
in a dedicated investigation) is architect-owned. If a future session
chooses to invest, the candidate-root-cause list in
`project_ws_flaky_segfault.md` is the starting input.

**Why entry has been rescoped:** the historical bandaid ladder
addressed real production-side teardown races (Meyers-singleton
destruction order, audit-write-during-shutdown, DbClient creation
in shutdown path) — all closed by the named fixes. LH-0.1 closed
the open question of whether the production kernel still hits any
of these signatures: it does not. The DEFERRED entry's framing was
"hypothesis test-only, residual production exposure unknown" prior
to LH-0.1; it is now "confirmed test-only, residual scoped to
`[js][async]` subprocess sequential-run race."

**Current behavior:** Eight protective gates are in place
(`g_shutdown_pending` / `initiate_shutdown` / `plinth::log::shutdown` /
lazy DbClient / `drain_pending_jobs` / `cancel_all_timers` (now
ConnState-iterating) / `dispatch_for_test` / leaked-singleton).
Production kernel is clean at the saturation level LH-0.1 measured.
Test harness fires the `[js][async]` race at the rates above.

**Future approach:** Tracked on the ROADMAP as
**`0.5.x.N [js][async] kernel-side refcount investigation`** under
`## 0.4.x cleanup follow-ups (cross-cutting)` `[medium]`. Entry
condition: a tight one-TEST_CASE repro outside the
`catch_discover_tests` per-TEST_CASE escape hatch. Likely needs its
own ICD if scope grows beyond a small targeted fix
(`dispatch_async_op_detached` / `SqlBinderAwaiter` /
`AnyCompletionAwaiter` ownership audit). Win condition: enable
`[js][async]` grouping in the 0.4.5.1 grouped-ctest model and
recover the ~10× subprocess-count reduction the rest of the suite
already enjoys. Schedule alongside whichever 0.5.x milestone next
touches the async bridge; LH-stream parallel work may produce
additional empirical inputs first.

**Reference:** `project_ws_flaky_segfault.md` (memory — historical
sub-paths; updated to reflect LH-0.1's empirical finding).
`docs/CHANGELOG.md` LH-0.1 §Initial diagnostic finding.
`docs/CHANGELOG.md` 0.4.5.1 §Async caveat.
`RE-EVAL-0.4.x-arc-closeout §2.4 + §5.1`.
`docs/ROADMAP.md` `## 0.4.x cleanup follow-ups`.

---

### 2026-04-19 — MEMORY_LIMIT classifier peak-tracking (ICD-0.3.4 / 0.3.4.1) [resolved; watchlist entry]

**Milestone:** Test 264 `async_bridge: memory limit tripped between
awaits yields MEMORY_LIMIT` flaked on 0.3.4 post-merge CI.
Root-caused to classifier's inability to allocate strings post-OOM
for `reason.name` / `reason.message` reads, plus live runtime-stats
check at classify time being too late (async frame already unwound).

**Status: RESOLVED** in 0.3.4.1 via the `BridgeContext::memory_
limit_hit` atomic latch + `sample_memory_peak` / `was_memory_limit_
hit` helpers, with sampling sites in the `plinth_js_interrupt_cb`
(every 10 000 bytecodes) and in `drive_jobs` after each
`JS_ExecutePendingJob`. `classify_rejection` and `extract_error`
consult the latch as primary OOM-upgrade path; the "out of memory"
/ "stack overflow" substring scan on `JS_ToCString` fallback
remains as a secondary safety net. 0.3.5 CI ran clean on this
signal.

**Why still listed:** Watchlist entry — the fix is shape-sensitive
(sample-cadence calibration + latch reset on `release()`) and a
future change to `RuntimePool` or `run_on_context` could regress
the classifier without the test being load-bearing enough to
catch at CI time. Entry serves as a context pointer for any future
regression on "MEMORY_LIMIT" test failing as kind 9 / garbled.

**Next action:** Only if regression surfaces. No scheduled work.

**Reference:** `docs/CHANGELOG.md` 0.3.4.1 entry (Fix 3 — MEMORY_LIMIT
peak-tracking classifier). `RE-EVAL-0.3.x-arc-closeout §5.2`.

---

### 2026-04-18 — Drogon `PgBatchConnection` loses `SqlError` typing on batch abort (ICD-0.3.3) [pattern-match fallback in 0.3.3]

**Milestone:** 0.3.3 ships a message-pattern fallback in
`pg_exception_to_rejection` that recovers the canonical `db.*` `code`
from common SQL error text. Tracked as a Drogon-upstream-fix or
local-patch follow-up (no ROADMAP slot yet — small enough to land
ad-hoc when next touched).

**Why deferred:** When a `db.query`/`db.exec` runs through Drogon's
PgBatchConnection path (the default for the kernel DbClient pool) and
PG returns a syntax error, the batch is aborted and the second/Nth
statement gets `Command didn't run because of an abort earlier in a
pipeline`. Drogon wraps the original `SqlError` (which carries
`sqlState()`) inside an `InternalError` for the abort message, losing
the SQLSTATE typing in the process. `dynamic_cast<SqlError*>` against
the caught `DrogonDbException` returns nullptr.

**Current behavior (0.3.3):** `pg_exception_to_rejection` first tries
the proper `dynamic_cast<SqlError*>` path. On miss, it scrapes
`SQLSTATE NNNNN` from the message text. On second miss, it
pattern-matches three common error families (`syntax error`, `relation
... does not exist`, `column ... does not exist`) and synthesizes the
SQLSTATE + code. ICD §Tests Group A.2 (`db.query('INVALID SQL')` →
`db.syntax_error` + `42601`) passes via this fallback.

**Future approach:** Either (a) submit an upstream Drogon patch to
preserve `SqlError` typing through PgBatchConnection's pipeline-abort
path, or (b) carry a small local fork. Option (a) is cleaner; option
(b) is faster. Until then, the heuristic stays.

---

## Resolved

### 2026-04-26 → 2026-04-28 — ICD-0.5.5 S.06 broker-side `realtime.seq.gap_detected` audit pipeline [resolved in 0.6.0.N session 8]

**Background:** ICD-0.5.5 §14 row S.06 specifies "Gap audit fires —
inject a gap by manually deleting a row, then forcing a live emit;
broker's per-(connection, channel) cache observes gap;
`realtime.seq.gap_detected count_in_window=1` fires." Carved out of
the 0.6.0.N test-fixture buildout session 6 macro plan §D scope on
2026-04-28 because the production-side pipeline did not exist —
only the `gap_audit_window_ms` config field was plumbed
([`src/kernel/config.hpp:117`](../src/kernel/config.hpp:117),
[`src/kernel/config.cpp:193-198`](../src/kernel/config.cpp:193));
no production reader, no per-(connection, channel) cache, no audit
emission site.

**Resolution:** 0.6.0.N session 8 (2026-04-28, branch
`feat/0.6.0.N-s06-gap-detection`) lands the broker-side pipeline
inside `deliver_to_conn` rather than at the broker fan-out level —
co-locates with the existing per-conn delivery state
(`replay_in_flight`, `live_buffer`) and avoids a third audit-window
implementation in broker.cpp. Five-part landing:

1. **Per-conn cache** — new
   `std::unordered_map<std::string, std::int64_t> last_live_seen_seq`
   on
   [`ConnState`](../src/kernel/ws/conn_state.hpp:96).
   Touched only on the conn's owning loop.
2. **Detection** — gap-detect block in
   [`deliver_to_conn`](../src/kernel/ws/publish.cpp:166)
   on the immediate-send path. Forward jump K > 1 against an
   established baseline emits audit; in-order / first-frame /
   duplicate-or-reorder all collapse to `last = std::max(last, seq)`.
3. **Sliding-window dedup** — `g_gap_windows` map +
   `claim_gap_audit_slot` mirroring
   [`subscriptions.cpp:473`](../src/kernel/ws/subscriptions.cpp:473)
   pattern, keyed on `user_id\0channel`.
4. **Audit emission** — `emit_gap_detected(...)` with payload per
   ICD §11 row 1179 (`{user_id, channel, prev_seq, next_seq,
   gap_size, count_in_window, window_ms}`).
5. **Subscribe-time reset** —
   [`on_subscribe`](../src/kernel/ws/subscriptions.cpp:367)
   writes `last_live_seen_seq[ch] = 0` per subscribe so each
   subscription starts with a fresh baseline.

Production scope: ~80 LoC — well below the 200+ DEFERRED estimate
because (a) the existing `claim_debounce_audit_slot` pattern was
reusable verbatim and (b) the per-conn cache slots cleanly into
`ConnState`'s loop-confined fields with no new mutex.

**Tests:** 3 new `[realtime][broker][audit][seq][gap][integration][ws]`
TEST_CASEs in
[`tests/kernel/realtime/gap_detection_test.cpp`](../tests/kernel/realtime/gap_detection_test.cpp):

- **S.06a** — happy: dispatch seq=1 then seq=3 → audit row with
  `prev_seq=1, next_seq=3, gap_size=1, count_in_window=1`.
- **S.06b** — same-window dedup: dispatch 1, 3 (audit fires), 5
  (suppressed by sliding window) → exactly ONE PG audit row;
  `gap_audit_emit_count_for_test() == 1` confirms suppression
  upstream of PG.
- **S.06c** — first-frame baseline: dispatch seq=42 → no audit;
  then seq=44 → audit with `prev_seq=42, next_seq=44`.

The `GapDetectHarness` mirrors session 6's `LiveReplayWsHarness` but
seeds zero events; tests inject envelopes directly via
`broker::dispatch_for_test` so seq stamps come from each test's
`ev.envelope["seq"] = ...`. Pure live path — `subscribe` is sent
WITHOUT `since_seq` so `fire_replay` does not run and gap detection
takes the immediate-send arm.

**Implementation deviation from the carve-out plan:** The DEFERRED
entry pointed at "broker-side per-(connection, channel) cache"; the
landed implementation puts the cache on `ConnState` (touched by
`deliver_to_conn` in publish.cpp), not in broker.cpp. Equivalent
contract — the same per-(WS-conn, channel) attribution — but
co-located with the existing per-conn delivery state. broker.cpp is
now untouched.

**Reference:** ICD-0.5.5 §11 lines 1179-1214 + §14 row S.06; CHANGELOG
`2026-04-28 — 0.6.0.N test-fixture buildout, session 8 of N`; plan
file
the archived implementation record.

---

### 2026-04-20 → 2026-04-28 — ICD-0.4.4 I.19 `?dry_run=1` validation-only path [resolved in 0.6.0.N session 7]

**Background:** ICD-0.4.4 enumerates 20 integration test cases I.01–I.20.
v0.4.4 shipped I.01–I.17; I.18 + I.20 closed in 0.6.0.N session 2
(2026-04-27) once the HTTP fixture landed; I.19 remained deferred
because the `?dry_run=1` validation-only path didn't exist in
`handle_post_packages` ([handlers.cpp:130-179](../src/kernel/packages/handlers.cpp:130)).
ICD-0.4.4 §HTTP Surface line 173 specifies the contract — UPLOADING +
VALIDATING runs, no row, no schema, ValidationReport returned — but
zero `dry_run` references existed in `src/kernel/packages/` (the query
parameter was simply unread).

**Resolution:** 0.6.0.N session 7 (2026-04-28, branch
`feat/0.6.0.N-i19-dry-run`) lands the dry-run codepath as a trailing
default-valued parameter on `install_package`:

```cpp
auto install_package(span<byte>, Provenance, const InstallerContext&,
                     bool dry_run = false,
                     nlohmann::json* dry_run_report = nullptr)
    -> std::expected<PackageRecord, InstallFailure>;
```

Inside `install_package`, four persistence side-effects (the
`insert_packages_row` call, the `set_state` UPDATE, the `fail_at` audit +
report write, and the `update_packages_report` after VALIDATING) are
gated on `!dry_run`; an early return after VALIDATING synthesises a
`PackageRecord` with `id=""`, `state=VALIDATING`, and writes the
validation report to `*dry_run_report`. The pre-INSERT branches
(DISABLED_PRESENT / VERSION_NOT_NEWER / UPGRADE_CANDIDATE / advisory-
lock-held) inherit dry-run semantics for free since they all return
before line 1428's INSERT — same 409 + `kind` as the regular path.
Existing call sites (handler at line 164, bundled-shell first-boot at
line 2546, ~12 test callers) continue to compile via the defaults.

Handler at [handlers.cpp:130-179](../src/kernel/packages/handlers.cpp:130)
reads `req->getParameter("dry_run") == "1"`, branches before the
existing path, and on success builds a 200 inline JSON body with
`state="VALIDATING"`, `name`, `version`, `frontend_mount`, and
`validation_report`. A new `nlohmann_to_jsoncpp` helper round-trips
the validator's `nlohmann::json` payload into the `Json::Value`
shape the existing response builders use.

No audits fire on dry-run (success or failure). Per the design rationale
in this session's CHANGELOG entry: `emit_install_failed_audit` requires a
non-empty `package_id` (no row exists), and audit is reserved for
terminal state changes — a dry-run is not a state change. ICD-0.4.4
§Audit is silent on dry-run.

**Tests:** 3 new `[I.19]`-tagged TEST_CASEs at
[tests/kernel/packages/install_lifecycle_http_test.cpp](../tests/kernel/packages/install_lifecycle_http_test.cpp):

- **I.19a** — happy: valid-install zip + `dry_run=1` returns 200 +
  state=VALIDATING + name/version + validation_report.disposition;
  follow-up libpq SELECTs confirm zero rows in `plinth.packages` and
  no `ext_notes` schema in `information_schema.schemata`.
- **I.19b** — fail: fail-validator zip + `dry_run=1` returns 422 +
  state=INSTALL_FAILED + failed_at_stage=VALIDATING +
  kind=validation-errors; no row.
- **I.19c** — coll: real install (201) followed by dry-run of same
  zip returns 409 with `kind` ∈ {advisory-lock-held,
  name-already-installed, upgrade-version-not-newer}; row count
  unchanged.

A `read_install_zip(fixture_name)` overload was added to
[`HttpTestFixture`](../tests/kernel/packages/http_test_fixture.hpp) to
parameterise the fixture-zip reader; existing `read_valid_install_zip()`
is now a thin call to `read_install_zip("valid-install")`.

**Reference:** ICD-0.4.4 §Test Cases I.19 + §HTTP Surface line 173;
CHANGELOG `2026-04-28 — 0.6.0.N test-fixture buildout, session 7 of N`;
plan file the archived implementation record.

---

### 2026-04-18 → 2026-04-24 — Per-op `SET search_path` for `db.*` (ICD-0.3.3) [resolved in 0.5.3 phase 3]

**Background:** ICD-0.3.3 §Security Constraint 1 specified per-op
`SET search_path TO ext_<extension_id>, plinth;` enforcement on
every `db.exec` / `db.query` from an extension-scope BridgeContext.
Deferred through 0.3.3 (no installer), 0.4.3 (library-only DDL —
no `db.*` request-path touch), 0.4.4 – 0.5.0 (installer shipped but
no transactional wrapper), 0.5.0.4 (extension dispatch unblocked
`db.query` through the handler but left the search_path gap open),
0.5.1 (coalescer hooks post-exec, no wrapping), and 0.5.2 (broker,
no `db.*` path touch). Pointer tightened to **0.5.3** by
`RE-EVAL-0.5.x-following-0.5.1.md §2.5` (2026-04-23) and ratified
in ICD-0.5.3 §Per-Op `SET search_path` Isolation.

**Resolution:** 0.5.3 phase 3 (commit `8ff4518`) lands the
transactional wrapper.
[src/kernel/js/db_search_path.{hpp,cpp}](../src/kernel/js/db_search_path.cpp)
owns the `db.search_path.enforce` atomic flag, the
`is_valid_extension_name` regex gate (`[a-z][a-z0-9_]*`), and the
rate-limited `db.search_path.set_failed` audit emitter.
[src/kernel/js/run_on_context.cpp](../src/kernel/js/run_on_context.cpp)
`run_db_query_outcome` / `run_db_exec_outcome` call
`prepare_search_path_wrapper` before the user SQL — opens
`newTransactionCoro`, runs `SET LOCAL search_path TO ext_<name>,
plinth`, routes the user statement through the pinned `TransactionPtr`,
then issues explicit `COMMIT` before resolving the outer promise
(phase 3 CHANGELOG pins the Drogon async-commit rationale).
Kernel-scope bcs (`extension_name == ""`) bypass the wrapper per
§Kernel-scope bypass. `db.batch()` paths (phase 4) route through a
batch-scoped single SET LOCAL at BEGIN instead of per-op.

**Tests:** 7 P.\* in
[tests/kernel/js/db_search_path_test.cpp](../tests/kernel/js/db_search_path_test.cpp);
P.02 inside-batch assertion in
[tests/kernel/js/db_batch_test.cpp](../tests/kernel/js/db_batch_test.cpp).

**Reference:** CHANGELOG 2026-04-24 `0.5.3 phase 3`;
`docs/icd/ICD-0.5.3-db-batch-silent-mode.md §Per-Op SET search_path Isolation`.

---

### 2026-04-18 → 2026-04-24 — `db.*` PG-type→JS-type mapping (ICD-0.3.3) [resolved in 0.5.3 phase 1]

**Background:** ICD-0.3.3 §PG-Value → JS-Value Conversion specified
OID-driven mapping (BOOLOID → boolean, INT\*OID → number, TEXTOID →
string, BYTEAOID → Uint8Array, etc.). Deferred because Drogon v1.9.12's
`orm::Field` exposed `name()` + `as<T>()` but no public `oid()`
accessor. 0.3.3 shipped a string-parse heuristic that mis-typed
PG text `"t"`/`"f"` as bool (ICD's intended fail-mode on the literal
string case). Pointer tightened to **0.5.3** by the same 2026-04-23
RE-EVAL and ratified in ICD-0.5.3 §OID-Driven PG-Type → JS-Type
Mapping.

**Resolution:** 0.5.3 phase 1 (commit `2164525`) lands the OID
switch.
[third_party/drogon-patches/ftype-accessor.patch](../third_party/drogon-patches/ftype-accessor.patch)
(4-line upstreamable diff) adds `drogon::orm::Field::oid()`
delegating to the already-public `Result::oid(column)` via the
existing `friend class Field` relationship. Applied via
`PATCH_COMMAND git apply ...` threaded into the drogon
`FetchContent_Declare` at CMakeLists.txt (idempotent re-apply guard
via `git apply --reverse --check` sentinel).
[src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}](../src/kernel/js/stdlib/db_result_to_json.cpp)
is a new TU that extracts the 0.3.3 heuristic from
`run_on_context.cpp` and replaces it with an OID switch over 19
built-in PG types. BYTEA routes to a tagged
`{__bytea_hex__: "..."}` Json envelope; the `json_to_js` unpacker
in bridge_context.cpp constructs a native JS `Uint8Array` via
`JS_NewUint8ArrayCopy`. Feature flag `db.oid_mapping.enabled`
(default true) preserves the 0.3.3 heuristic as a deployment-ramp
escape hatch; emits a warn-log when `false`.

**Tests:** 7 T.\* in
[tests/kernel/js/async_bridge_test.cpp](../tests/kernel/js/async_bridge_test.cpp)
(not `stdlib_test.cpp` as the ICD originally named — amendment:
T.\* need a live PGresult and live under the `[js][async][db][types]`
tag). T.07 narrowed from the ICD's `'true'::text` scenario to
`'t'::text` — the 0.3.3 heuristic only mis-classifies single-char
PG bool text reprs, not the 4-char string.

**Reference:** CHANGELOG 2026-04-24 `0.5.3 phase 1`;
`docs/icd/ICD-0.5.3-db-batch-silent-mode.md §OID-Driven PG-Type → JS-Type Mapping`.

---

### 2026-04-22 → 2026-04-22 — Tier 2 extension capability dispatch [resolved in 0.5.0.4]

**Background:**
[src/kernel/capabilities/resolution.cpp:313–344](../src/kernel/capabilities/resolution.cpp)
returned `CapabilityError::TIER3_NOT_AVAILABLE` for every Tier 2
cache entry with `provider_type == "extension"`. Deferred in 0.2.2
with the comment "*the 0.3.x JS bridge will replace this branch
with the real bridge call*"; ICD-0.3.4 §49–60 reaffirmed the
deferral and pointed at "the 0.4.x ICD that introduces the
installer" — but 0.3.0–0.3.6 shipped without the replacement,
0.4.0–0.4.7 shipped the installer without wiring it, and 0.5.0
opened the Realtime arc with the gap still present. The blocker
was latent because LH-0's `lh0driver:1:noop` was never invoked
(`feedback_real_code_paths.md`); LH-1's 2026-04-22 attempt
surfaced it via a live smoke (1.1M WS calls / 20s, zero handler
invocations, zero NOTIFY emits).

**Resolution:** 0.5.0.4 ships
[src/kernel/extensions/runtime_registry.{hpp,cpp}](../src/kernel/extensions/runtime_registry.cpp)
per ICD-0.5.0.3. The new subsystem owns one `RuntimePool` per
installed-ACTIVE extension; `dispatch_tier2` on the async path
routes extension entries into `plinth::extensions::dispatch`
which reads `server/handlers/<fn>.js` per call, invokes the
module's default export under the caller's `UserContext`, and
maps outcomes onto five new `cap.*` rejection codes
(`cap.async_required`, `cap.extension_not_loaded`,
`cap.handler_not_found`, `cap.handler_load_failed`,
`cap.handler_threw`). Sync `call_capability` rejects extension
entries with `ASYNC_REQUIRED`; WS `on_call` migrated to
`call_capability_async`. Install lifecycle gained
`create_pool` / `destroy_pool` hooks at INSTALL / UPGRADE /
ENABLE / DISABLE / UNINSTALL commit sites. `main.cpp` wires
`init_registry` after `init_resolver` and `shutdown_registry`
into the atexit chain after `realtime::stop_listener`.

**LH-1 unblocker:** the resumption session on
`feat/lh-1-listen-notify-storm@339afb0` rebuilds the
`lh1storm.zip` fixture with the manifest-name + RBAC corrections
noted at `project_plinth_state.md §ICD-LH-1 corrections` and
runs the 3-trial diagnostic discipline per ICD-LH-1 §7.2.

**Reference:** CHANGELOG 0.5.0.4;
`docs/icd/ICD-0.5.0.3-extension-dispatch.md`;
`project_tier3_extension_dispatch_gap.md` (memory);
`feedback_real_code_paths.md` (the harness lesson that surfaced
the blocker).

---

### 2026-04-22 → 2026-04-22 — `phase_a` / `phase_b` internal + public rename (v0.4.7 ship deferral) [resolved in 0.5.0.1]

**Background:** v0.4.7 shipped with internal namespaces / types / methods
/ Catch2 tags carrying `phase_a` / `phase_b` substrings that did not
match the v0.4.7 file renames (`rule_validator.{hpp,cpp}` /
`rbac_test_runner.{hpp,cpp}`). Two adjacent surfaces — audit-event
strings `packages.phase_b_passed` / `_failed` and schema columns
`plinth.packages.last_phase_b_run_at` / `last_phase_b_result` — were
initially separated into a 0.4.7.4 "public surface" decision on
concern that external consumers (shell, admin UI, third-party
extensions) might already grep against those names.

**Resolution:** Folded both surfaces into a single 0.5.0.1 rename
pass. the maintainer's call 2026-04-22: no shell, no admin UI, no installed
third-party extensions exist — the hypothetical external consumers
the 0.4.7.4 decision was defending against do not yet exist. Pre-0.7
schema is fluid per ROADMAP §preamble; zero installed prod databases
carry 0.4.7+ state. Cheapest possible rename window. Ships as
0.5.0.1 four-part follow-up, untagged.

**Renamed surfaces:**
- **Namespaces:** `plinth::packages::phase_b` → `plinth::packages::rbac_test`
- **Types:** `PhaseBReport` / `PhaseBFailure` / `PhaseBRule` →
  `RbacTestReport` / `RbacTestFailure` / `RbacTestRule`
- **Methods:** `run_phase_b` → `run_rbac_test`;
  `schedule_phase_b_detached` → `schedule_rbac_test_detached`;
  `emit_phase_b_audit` → `emit_rbac_test_audit`;
  `fetch_extension_rules_for_phase_b` →
  `fetch_extension_rules_for_rbac_test`; `phase_b_report_from_json` →
  `rbac_test_report_from_json`; `validate_phase_a` → `validate_rules`
- **Catch2 tags:** `[phase_b]` / `[phase_b_report]` /
  `[rbac_phase_a]` → `[rbac_test]` / `[rbac_test_report]` /
  `[rule_validator]`
- **Schema columns:** `last_phase_b_run_at` /
  `last_phase_b_result` → `last_rbac_test_run_at` /
  `last_rbac_test_result`
- **Audit events:** `packages.phase_b_passed` / `_failed` →
  `packages.rbac_test_passed` / `_failed`
- **ICD file:** `ICD-0.4.7-rbac-phase-b-test-execution.md` →
  `ICD-0.4.7-rbac-test-execution.md`

All names pass the naming-convention test (classes=nouns,
methods=verbs, fields=adjectives).

**Reference:** CHANGELOG 0.5.0.1; ICD-0.4.7 amendment;
`RE-EVAL-0.4.x-arc-closeout.md §2.1`.

---

### 2026-04-18 → 2026-04-18 — Parallel `db.*` fan-out (ICD-0.3.3) [resolved in 0.3.3.1]

Shipped in 0.3.3.1. `src/kernel/js/run_on_context.cpp` now spawns each
`AsyncOp` as a fire-and-forget `drogon::async_run` task via
`dispatch_async_op_detached`. Each detached task does the DB/audit
work, then `loop->queueInLoop`'s back to the main loop to invoke
`bc.resolve` / `bc.reject`, decrement `bc.inflight_detached`, and fire
`bc.signal_completion()`. The outer coroutine suspends on
`AnyCompletionAwaiter` while in-flight tasks are running and no other
work is drivable. ICD-0.3.3 §Tests Group A.4 timing assertion
restored to `< 150 ms` (`tests/kernel/js/async_bridge_test.cpp`);
parameterized variant exercises the new SqlBinderAwaiter; 8-parallel
stress + wall-clock-cancel stress added for TSan coverage.

Cancellation cascade upgraded: instead of a blind 5 s `sleepCoro`
drain, it now awaits `inflight_detached == 0` via the same
`AnyCompletionAwaiter`, bounded by a 5 s ceiling. This closes the
BridgeContext-UAF window that naive parallel fan-out would otherwise
have opened.

Accepted trade-off (new to 0.3.3.1, no further DEFERRED entry — this
is a Drogon-API constraint, not a design choice): wall-clock
cancellation cannot preempt a query libpq has already dispatched, so
the effective enforcement is `wall_clock_limit + longest in-flight
query`. Bounded by the same 5 s cascade ceiling we accepted in 0.3.3.

### 2026-04-18 → 2026-04-18 — `db.*` runtime-sized parameter binding loop blocking (ICD-0.3.3) [resolved in 0.3.3.1]

Shipped in 0.3.3.1 alongside the parallel fan-out. New
`SqlBinderAwaiter` (inherits Drogon's `CallbackAwaiter<Result>`) in
`run_on_context.cpp` anonymous namespace. `await_suspend` sets up the
SqlBinder, binds params via the existing `bind_param` helper,
registers row + exception callbacks that capture the coroutine
handle, and returns — the binder's destructor triggers exec; the
callbacks fire async on Drogon's IO thread and resume the coroutine
there. ~30 LoC, no Drogon patch required. Call sites in the per-type
outcome helpers (`run_db_query_outcome`, `run_db_exec_outcome`)
changed from `= exec_binder_path(db, op)` to `= co_await
SqlBinderAwaiter{db, op.sql, op.sql_params}`. The
`std::promise/std::future` bridge and `exec_binder_path` helper are
gone.
