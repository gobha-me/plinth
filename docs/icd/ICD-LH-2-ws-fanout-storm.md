# ICD-LH-2 — WS Fan-out Storm Tier

**Status**: Active (diagnostic — infrastructure, not a kernel feature)
**Authored**: 2026-04-23
**Roadmap**: `docs/ROADMAP.md` §Load Harness, LH-2 `[strong]` (promoted
from `[medium]` by ICD authoring — see §1.1)
**Depends on**: ICD-LH-1 (producer extension reused verbatim;
tier-profile precedent; storm duration); ICD-LH-0.1 (diagnostic-mandate
framing; 3-trial discipline; 120-second window); ICD-LH-0 (harness
binary pattern, WS `call` auth flow, driver-package install pattern);
ICD-0.5.2 (broker subsystem, `pubsub.subscribe` binding, per-channel
subscribe RBAC, drain hook, audit events — the contract under test);
ICD-0.5.0 (envelope contract the broker forwards; `plinth:realtime`
channel); ICD-0.2.4 (RBAC grant path for the derived
`*.realtime.subscribe.*` rule); ICD-0.1.6 (WS auth completion,
`subscribe` / `unsubscribe` frame shapes the harness uses).

## 1. Purpose

LH-2 is the first LH-stream tier to exercise the v0.5.2 WS broker's
two fan-out arms under sustained load. It drives two paths that LH-1
left idle:

- **Kernel-side WS fan-out (per-connection arm).** Each listener
  dispatch call invokes `publish_dispatched(DispatchedEvent)`
  (`src/kernel/ws/publish.cpp`), which snapshots `ConnectionRegistry`,
  filters by `ConnState::channels`, re-checks the per-channel
  subscribe rule against `effective_rules` (defense-in-depth per
  ICD-0.5.2 §Security Constraints item 5), and enqueues a per-loop
  lambda that writes the WS event frame. LH-1 ran with zero
  subscribed connections, so this path executed the snapshot-then-
  filter prelude against an empty set.
- **Extension-side per-bc callback arm.** When an extension's
  BridgeContext has called `pubsub.subscribe(channel, handler)`
  (ICD-0.5.2 §6), the broker looks up its `g_bc_registry`
  (`src/kernel/realtime/broker.cpp`) and schedules the stored
  `callback_id` on the owning QuickJS runtime's async arm. LH-1 had
  no in-process subscribers registered — this arm was similarly idle.

The subscriber side in LH-2 is **M WS client connections**, each
authenticated, each subscribing to `ext_lh1_storm`'s storm channel
via the WS `subscribe` frame. An optional sidecar extension (OQ1,
§11) additionally calls `pubsub.subscribe` inside a
BridgeContext to exercise the second arm. The producer
(`ext_lh1_storm` driver) and the storm tier envelope are reused from
LH-1 verbatim.

### 1.1 Band promotion (medium → strong)

ROADMAP carries LH-2 as `[medium]`. Authoring this ICD ahead of the
implementation session promotes the milestone to `[strong]` per
METHODOLOGY §3.1 — strong-band milestones require a pinned ICD before
opening. LH-0.1 and LH-1 set the precedent (both ROADMAP `[strong]`,
each shipped with their authoring ICD in hand). The 0.5.2.N authoring
CHANGELOG entry records the promotion so a future RE-EVAL sees it.

## 2. Non-goals for LH-2

- **Fixing any kernel issue LH-2 surfaces.** LH-2 is the diagnostic;
  fixes ship as their own 0.5.2.N follow-ups or 0.5.3-adjacent PRs
  once a reliable reproduction is in hand. Matches the LH-0 / LH-0.1
  / LH-1 posture exactly.
- **`plinth.events` persistence + delta-sync-on-reconnect.** 0.5.4 /
  LH-3 scope. LH-2 does not write or replay events from
  `plinth.events` (the table doesn't exist yet) and does not test
  durable subscription state across reconnect (ICD-0.5.2 §OQ7 pinned
  stateless per-connection for v0.5.2).
- **Monotonic `seq` ordering verification across subscribers.** 0.5.5
  scope. Envelopes emitted under LH-2 inherit LH-1's minimal
  `{seq, data}` shape; `seq` uniqueness within a single producer
  worker is preserved, but cross-subscriber ordering consistency is
  not asserted.
- **Reconnect-under-storm.** LH-3 / 0.5.4 scope. LH-2 holds WS
  connections stable for the full 120-second window (see §11 OQ5).
- **Quota-exceeded stress.** ICD-0.5.2 §OQ8 default is 64 channels
  per connection. LH-2 subscribes each client to exactly one channel;
  the quota guard is exercised incidentally (every subscribe checks
  it) but not driven.
- **Cross-node HA fan-out under storm.** LH-2 runs against a single-
  node `plinth` instance. Cross-node broker fan-out per
  `architecture/03-data.md §3.6.1` waits on a multi-node CI fixture.
- **Coalescer (Layer-1) storm.** 0.5.1 scope. LH-2 reuses LH-1's
  Layer-3 producer; a Layer-1 leg is listed as §9 future work
  (LH-2.1), OQ3 pins Layer-3-only for the first ship.
- **Widening the admin gate or RBAC grant pipeline.** LH-2 grants the
  derived `ext_lh1_storm.realtime.subscribe.storm_event` rule via the
  same seed path LH-1 uses for `ext_lh1_storm.burst` +
  `pubsub.publish`. No new RBAC surface.

## 3. Kernel contract — no changes

LH-2 ships **zero new kernel surface**. The entire diagnostic rides on
v0.5.2 primitives + LH-1's driver extension + the LH-0 harness binary
with one new tier profile (and, per OQ1, a small sidecar extension
that calls `pubsub.subscribe`). Specifically:

- No new `lh2:*` kernel signature (contrast LH-0 `lh0:1:chain` and
  LH-0.1 `lh0:1:js_stress`).
- No new dispatch fork in `plinth::ws::on_call`.
- No new `plinth::realtime::broker::*` API surface; no new audit
  events; no config-surface changes.
- No atexit-chain edits.

This is deliberate: the v0.5.2 surface is complete for the broker
contract; LH-2's value is *exercising* it, not extending it. If a
reproduction forces a kernel-side fix, that fix ships in its own PR
with its own ICD (or inline if trivially scoped).

## 4. Harness producer — reuses `ext_lh1_storm`

### 4.1 Reuse rationale

LH-2's producer is **LH-1's driver extension verbatim** —
`ext_lh1_storm` (ICD-LH-1 §4.2), same `ext_lh1_storm:1:burst`
capability, same handler body (ICD-LH-1 §4.3), same channel
`plinth:ext:ext_lh1_storm:storm_event`. LH-2's delta from LH-1 is
the subscriber surface (§5), not the producer. This keeps the
variable-vs-LH-1 axis single: the broker's fan-out arms are the only
new loaded path.

### 4.2 RBAC delta

LH-1's harness grants `ext_lh1_storm.burst` + `pubsub.publish` to the
admin group. LH-2 additionally grants the derived subscribe rule
**`ext_lh1_storm.realtime.subscribe.storm_event`** (per ICD-0.5.2
§Subscription RBAC table, Layer 3 row:
`<ext>.realtime.subscribe.<event_class>`) to the subscribing identity's
group. The derivation is pure — `derive_subscribe_rule(channel)` in
`src/kernel/rbac/subscribe_rule.cpp` returns the token; the harness
seed script grants it explicitly. No change to the grant pipeline
itself.

### 4.3 No handler surface change

`ext_lh1_storm/index.js#burst` runs unchanged from ICD-LH-1 §4.3. No
change to `plinth.json`, `capabilities.json`, or `rbac.json` either —
the package installs the same way under LH-0's flow.

## 5. Harness subscriber — M WS clients

### 5.1 Shape

M dedicated goroutines, each owning a private `*websocket.Conn`
(implementer's WS client library choice — `gorilla/websocket` or
`nhooyr.io/websocket` both work against Drogon's `/ws/events`). Each
subscriber:

1. Authenticates via `POST /api/auth/login` with a harness-seeded
   identity that has been granted
   `ext_lh1_storm.realtime.subscribe.storm_event` (§4.2).
2. Dials `/ws/events` with the session token per ICD-0.1.6 §WS auth.
3. Sends one `{"type":"subscribe","channels":["plinth:ext:ext_lh1_storm:storm_event"]}`
   frame per ICD-0.1.6's subscribe/unsubscribe contract; verifies the
   acknowledgement frame lists the channel under `subscribed[]` (not
   silent-omitted — silent-omission means the RBAC grant didn't
   propagate; fail fast).
4. Enters a receive loop that parses each `{"type":"event","channel",
   "payload":<envelope>}` frame per ICD-0.5.2 §OQ6 pin (full envelope
   as `payload`) and records an observation: `(subscriber_id,
   envelope.payload.seq, received_at, channel)`.
5. Exits on context cancellation (SIGINT / `--duration` elapsed) and
   sends a `{"type":"unsubscribe",...}` frame before closing the WS
   — clean teardown exercises the `ConnState::channels.erase` + global
   counter decrement path in `events_controller.cpp:handleConnectionClosed`.

The subscriber dials the **real `/ws/events` endpoint** and sends the
**real subscribe frame** — the broker's real `publish_dispatched`
path + real `delivery_rbac_allows` re-check + real per-loop enqueue
run on every envelope. No observer-only mock; no `dispatch_for_test`
short-circuit. (Per `feedback_real_code_paths.md`: installed-but-
uncalled capabilities are tech debt, not an acceptable scoping call.)

### 5.2 Why WS clients and not external PG LISTEN

LH-1 used external PG LISTEN for exactly the reason LH-2 must not:
PG LISTEN bypasses the kernel's in-process dispatch, which is the
whole path LH-2 is trying to load. LH-1's §9 explicitly scoped LH-2 as
"adds M client WS connections that subscribe via `pubsub.subscribe`…
exercises the broker's per-connection routing under the same storm
tier" — LH-2 honours that scope directly.

### 5.3 Dual-surface sidecar extension (OQ1 pinned)

Per §11 OQ1, LH-2 installs a second bundled extension
`ext_lh2_sidecar` alongside `ext_lh1_storm`. The sidecar's handler
runs once at harness setup, calls
`pubsub.subscribe("plinth:ext:ext_lh1_storm:storm_event", handler)`
inside its BridgeContext, and the handler body appends each received
envelope to an in-memory counter the harness reads back via a
separate `call` at teardown. This loads the broker's second fan-out
arm (per-bc callback invocation on the JS async arm) — the arm LH-1
left idle and the WS-client fleet alone doesn't touch.

The sidecar is a minimal package (single Tier 2 capability, single
`pubsub.subscribe` call in its handler body — no loop, no chain).
Its package layout mirrors `ext_lh1_storm` but with capability
signature `ext_lh2_sidecar:1:install_subscription` and rbac rule
`ext_lh2_sidecar.install_subscription`. Install flow reuses the
LH-0 `POST /api/packages` + admin grant seed path.

### 5.4 Subscriber metrics

Per WS subscriber:

- Observed event count.
- Per-event lag: `received_at − envelope.payload.emit_started_at`
  (same emit-time convention LH-1 uses at ICD-LH-1 §5.3 — the handler
  populates `emit_started_at` in ms since harness start).
- Gap count: difference between maximum observed `seq` and count of
  observed events (detects envelopes lost on the WS fan-out path).

Per run (all WS subscribers + sidecar combined):

- Total observed (WS fleet), total observed (sidecar), total emitted
  (from handler return values), two ratios.
- p50 / p95 / p99 WS-client lag.
- Gap breakdown per subscriber.
- **Broker-specific counters sampled at teardown** (via a small
  `ext_lh2_sidecar:1:read_broker_counters` capability the sidecar
  exposes — or equivalent admin-side path; implementer's call):
  `realtime.broker.dispatch_skipped` count during the run (expected
  zero — non-zero points at the RBAC re-check denying a delivery,
  which means a setup bug in §4.2's grant path, not a kernel bug);
  `realtime.broker.subscribe_denied` count (expected zero — non-zero
  points at the same setup bug caught earlier on the subscribe frame).

## 6. Harness binary — `--tier=ws-fanout`

Extends the LH-0 binary (`load-harness/cmd/lh0/`) with one new tier,
an M-WS-subscriber goroutine fleet, and the sidecar-extension install
step.

### 6.1 Tier profile

Added to `load-harness/internal/tiers/tiers.go`, extending LH-1's
column shape with two new subscriber columns:

| Name       | Producer workers | WS subscribers | JS subscribers (sidecar) | Burst size | Payload bytes | Duration |
|------------|------------------|----------------|--------------------------|------------|---------------|----------|
| ws-fanout  | 4                | 4              | 1                        | 16         | 512           | 120s     |

- **Producer workers (M_p)**: number of parallel WS workers calling
  `ext_lh1_storm:1:burst`. Unchanged from LH-1 `storm` tier.
- **WS subscribers (M_s)**: number of parallel WS client subscribers.
  Default 4 matches LH-1 S=4 verbatim for cross-tier comparability.
- **JS subscribers (sidecar)**: number of sidecar-extension
  BridgeContexts running `pubsub.subscribe` against the storm channel.
  Default 1 exercises the per-bc registry path with at least one
  handler installed; 0 disables the sidecar entirely (debugging
  convenience).
- **Burst size (K)**, **payload bytes (B)**, **duration**: inherited
  from LH-1 `storm` tier defaults (16 / 512 / 120 s).

Target emit rate at defaults remains LH-1's M_p × (calls/sec/worker)
× K → ~640–2,500 notifies/sec. The new axis is fan-out multiplier:
each envelope is delivered M_s + (sidecar count) times across the
broker, so the kernel-side enqueue volume at defaults is ~5× the
envelope count. This sits well below the per-loop queue-saturation
threshold on a single-node machine, preserving the storm's
diagnostic-propagation posture (not overflow).

### 6.2 CLI flags

New flags on the existing `lh0` binary:

- `--tier=ws-fanout` — selects the tier profile above.
- `--ws-subscribers N` — override WS subscriber count (default 4).
- `--js-subscribers N` — override sidecar count (default 1; `0`
  disables the sidecar extension install entirely).

Existing flags that apply: `--concurrency` (aliases `--producers`;
overrides M_p), `--burst-size K`, `--payload-bytes B`, `--duration`,
`--keep-driver` (also keeps the sidecar extension on teardown).

### 6.3 Exit codes

- `0` — clean run: zero worker-level WS errors, zero WS-subscriber
  parse errors, zero sidecar handler errors, zero
  `realtime.broker.subscribe_denied` / `dispatch_skipped` audit
  events recorded, observed/emitted ratio ≥ 0.99 across WS
  subscribers, sidecar observed/emitted ratio ≥ 0.99.
- `1` — worker, WS subscriber, or sidecar errors observed; summary
  prints breakdown.
- `2` — setup failure (package install failed, RBAC grant failed, WS
  dial failed, sidecar handler install failed).

Matches LH-0 / LH-1 convention.

## 7. Success criteria

### 7.1 Baseline (every run)

- Harness exits 0 under `--tier=ws-fanout` default.
- Kernel process does not crash (`SIGSEGV` / `SIGABRT` absent from the
  kernel log during the run).
- Kernel `realtime.broker.subscribe_denied` audit count stays at zero
  during the run (RBAC grant for the derived rule propagates).
- Kernel `realtime.broker.dispatch_skipped` audit count stays at zero
  during the run (defense-in-depth re-check passes — no
  group-revocation mid-session, no stale-rule race).
- Kernel `realtime.notify.rejected` audit count stays at zero
  (Layer-3 emit path clean; same as LH-1 baseline).
- Kernel `realtime.listener.reconnected` audit count stays at zero.
- Observed notify count ≥ 0.99 × emitted count across all WS
  subscribers combined.
- Sidecar observed notify count ≥ 0.99 × emitted count.
- p99 WS-subscriber lag < 5 s (matches LH-1 §7.1).

### 7.2 Diagnostic mandate

Under `--tier=ws-fanout` × 3 trials, paired with kernel-side log
tailing:

- **Reproduction** of any of
  `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`, `bad_weak_ptr`,
  or any broker-subsystem-specific signature (e.g. `ConnectionRegistry`
  snapshot invariant violation, `persistent_callbacks` map iterator
  invalidation, or `effective_rules` read-after-move) on v0.5.2 HEAD
  confirms a race in the broker's snapshot-then-dispatch path under
  sustained Layer-3 fan-out and unblocks a targeted fix PR.
- **Zero reproductions** on 3 trials clears the v0.5.2 broker for
  LH-3 (0.5.4 delta-sync) to layer on top without first addressing
  a foundation issue.

Both outcomes are useful diagnostic outputs — the session records
which occurred in the CHANGELOG's LH-2 ship entry, matching the
LH-0.1 / LH-1 precedent.

### 7.3 Secondary signals

The harness summary records these even when the baseline is clean;
unexpected values warrant investigation even if they don't fail the
run:

- WS-subscriber observed/emitted ratio monotonically declining during
  the 120-second window (points at broker enqueue backpressure or
  per-loop queue depth growth under sustained fan-out).
- Non-zero `realtime.broker.dispatch_skipped` count on an otherwise-
  clean run (points at an RBAC reload race — the group-revoked-mid-
  session guard at ICD-0.5.2 §Security Constraints item 5 firing
  unexpectedly).
- `broker::ws_subscriber_count()` ending below the expected
  M_s × 1-channel value without a corresponding WS client close
  (points at the `subscriptions.cpp` silent-omission path misbehaving
  or a subscribe ACK that reports success but doesn't persist the
  channel into `ConnState::channels`).
- Sidecar `broker::js_subscriber_count()` ending non-zero after
  teardown (points at a `drain_extension` leak on sidecar uninstall
  — orthogonal to LH-2's mandate but worth surfacing).
- WS-fleet p95 lag > 1 s while the sidecar's p95 lag is < 100 ms
  (points at a per-loop enqueue hotspot — the broker's per-loop
  lambda path is behind the per-bc callback path unexpectedly).

## 8. Observability

All external for LH-2 (matches LH-0 / LH-0.1 / LH-1 convention):

- **Harness stdout**: periodic progress ticks + final summary table
  (per-worker emit counts, per-WS-subscriber observed counts, sidecar
  observed count, p50/p95/p99 lag per subscriber group, gap
  breakdown, broker counter deltas, exit-code rationale).
- **Kernel stderr / log** tailed for crash signatures + realtime
  + broker audit events:
  ```
  tail -f <log> | grep -E \
    'free_zero_refcount|list_empty|bad_weak_ptr|SIGSEGV|SIGABRT|realtime\.(broker|listener|notify)\.'
  ```
  The `realtime.broker.*` audit events from ICD-0.5.2 §Audit Events
  (`subscribe_denied`, `dispatch_skipped`, `extension_drained`) are
  the new codes vs LH-1's tail pattern.
- **Process**: `ps -o rss,%cpu,pid -C plinth` sampled every 5 s.
- **DB state**: `SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE 'plinth%';`
  before / during / after — verifies no leaked DbClient connections
  under the fan-out tier.
- **Broker counter sampling**: `broker::dispatch_count()`,
  `broker::rbac_denial_count()`, `broker::ws_subscriber_count()`,
  `broker::js_subscriber_count()` sampled at harness teardown via the
  sidecar's `read_broker_counters` capability (or an equivalent
  admin-side read path — implementer's call). Recorded in the
  summary table alongside the harness-side counts.

No `plinth.metrics` — LH-4 wires harness → metrics after the metrics
subsystem lands (gated on 0.7.1).

## 9. Future work / deferred

- **LH-3 — Reconnect-under-storm.** Gated on 0.5.4. Adds a WS-client
  churn pattern (periodic disconnect + reconnect mid-storm) to stress
  the delta-sync path against `plinth.events`. Listed as the next
  natural LH tier; no sketch here (ICD horizon one-ahead rule).
- **LH-2.1 — Coalescer-origin storm.** If LH-2's Layer-3-only tier
  proves insufficient to surface broker races that only manifest with
  Layer-1 coalescer-origin envelopes, a follow-up tier drives `INSERT`
  stress through 0.5.1's coalescer and observes the same broker fan-
  out surface. Gated on nothing beyond v0.5.2 + v0.5.1 (both shipped).
  Opens only if LH-2 first-ship data warrants it.
- **Shared Go WS-client utility.** LH-2 is the first LH-stream tier
  to open WS subscriber connections. If LH-3 / later tiers need their
  own WS client path, extract the shared shape into
  `load-harness/internal/wsclient/`. Not required by LH-2 itself.

## 10. References

- `docs/ROADMAP.md` §Load Harness, LH-2 — stream definition + gating.
- `docs/icd/ICD-LH-0-load-harness-scaffold.md` — base WS `call` frame,
  auth flow, driver-package install pattern, tier-profile precedent.
- `docs/icd/ICD-LH-0.1-async-bridge-stress.md` — diagnostic-mandate
  framing, 120-second tier-duration precedent, 3-trial reproduction
  discipline.
- `docs/icd/ICD-LH-1-listen-notify-storm.md` — producer reuse
  (`ext_lh1_storm`); §4.2–4.5 package layout + handler; §6 tier-profile
  shape; §9 explicit LH-2 scope line LH-2 honours.
- `docs/icd/ICD-0.5.2-ws-broker.md` — the contract LH-2 exercises:
  broker subsystem, per-channel subscribe RBAC, `pubsub.subscribe`
  binding, drain hook, audit events, resolved OQ1–OQ8 pins.
- `docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md` — envelope contract
  the broker forwards.
- `docs/icd/ICD-0.1.6-websocket.md` — WS auth, subscribe/unsubscribe
  frame shapes, ACK discipline the WS-subscriber fleet uses.
- `docs/icd/ICD-0.2.4-capability-rbac.md` — RBAC grant semantics for
  the derived subscribe rule.
- `src/kernel/realtime/broker.hpp` + `broker.cpp` — the broker
  subsystem. Monotonic counters
  (`dispatch_count` / `rbac_denial_count` / `js_subscriber_count` /
  `ws_subscriber_count`) are the observable surface LH-2 samples at
  teardown.
- `src/kernel/ws/publish.cpp` — `publish_dispatched(DispatchedEvent)`
  fan-out entrypoint LH-2 loads; per-connection `delivery_rbac_allows`
  re-check path.
- `src/kernel/rbac/subscribe_rule.cpp` — `derive_subscribe_rule`
  token derivation; the rule the harness seed grants in §4.2.
- `docs/architecture/03-data.md` §3.3 + §3.6 + §3.6.1 — realtime +
  physical-channel fan-in design prose.
- `project_ws_flaky_segfault.md` (session memory) — flake-family
  history the diagnostic mandate continues.
- `feedback_real_code_paths.md` (session memory) — LH harnesses must
  invoke the real production paths.
- `feedback_deterministic_teardown.md` (session memory) — atexit
  convention the harness teardown observes.

## 11. Open questions

**OQ1 — Subscriber surface: WS-only, sidecar-only, or both?**
ICD pins: **both.** M WS client connections drive the per-connection
arm + 1 sidecar extension calling `pubsub.subscribe` drives the per-bc
arm. Rationale: ICD-0.5.2 §OQ4 pinned the per-bc JS registry as an
independent code path from `ConnState::channels` — they share the
broker's dispatch entry but diverge at delivery (WS enqueue on the
connection's owning loop vs per-bc callback invocation on the JS
async arm). LH-1 taught us idle arms are invisible; a WS-only tier
would leave the JS arm unloaded and a sidecar-only tier would leave
the WS arm unloaded. Alternative: WS-only for first ship, sidecar in
a follow-up LH-2.2 — rejected because the sidecar is a small
extension (single handler, single `pubsub.subscribe` call) and the
diagnostic value of cross-arm comparison at the same tier is high.

**OQ2 — Default M_s (WS subscribers).**
ICD pins: **4.** Matches LH-1 `S=4` verbatim for cross-tier
comparability — the maintainer reads LH-1 and LH-2 summary tables side-by-side;
holding the subscriber column constant isolates the broker-arm
variable. The `--ws-subscribers N` flag allows climbing to 16 / 64 /
more if the baseline at 4 is clean and the implementer wants to
stress per-loop queue depth. Alternative: default 16 closer to the
ICD-0.5.2 §OQ8 64-channel/conn quota ceiling — rejected because the
quota is per-channel not per-subscriber and isn't under test here
(non-goal §2).

**OQ3 — Layer-1 coalescer leg in the first LH-2 ship.**
ICD pins: **Layer-3 only** (reuse LH-1 producer verbatim). Rationale:
adds zero new harness surface vs LH-1; isolates the broker fan-out
arms as the single variable changed vs LH-1. 0.5.1's coalescer
already has its own test matrix, and a Layer-1 stress path would
conflate broker-arm diagnostics with coalescer-window diagnostics.
Alternative: add an `INSERT` storm worker that drives the coalescer
and fans out on `plinth:data:ext_*` — listed as §9 future work
(LH-2.1). Promote to first-ship only if LH-2 baseline is clean and
the architect wants the cross-layer coverage in the same session.

**OQ4 — Success signal: exact delivery-count match, or sampled
ratio?** ICD pins: **observed/emitted ≥ 0.99 across WS subscribers +
across sidecar, p99 lag < 5 s** (matches LH-1 §6.3 and §7.1
verbatim). Rationale: consistency with LH-1's pass/fail budget keeps
the cross-tier summary tables comparable. A strict 1.0 match is
brittle against ctrl-C cancel + in-flight frames (same reason LH-1
chose 0.99). Alternative: 0.999 + strict p99 < 1 s — would surface
broker enqueue backpressure earlier but would convert the 2-minute
diagnostic window into a pass-rate test rather than a
crash-reproduction diagnostic. The diagnostic mandate is the
mandate; latency is a secondary signal (§7.3).

**OQ5 — WS-client reconnect behavior during the storm.**
ICD pins: **stable connections; no induced drops.** Rationale:
ICD-0.5.2 §OQ7 explicitly pinned stateless per-connection reconnect
for v0.5.2; durable replay is 0.5.4. Reconnect stress is LH-3 scope
(§2 non-goals; §9 future work). Including it in LH-2 would cross the
scope boundary and conflate the broker's normal-lifecycle fan-out
diagnostic with the delta-sync path, which doesn't exist yet.
Alternative: add a `--reconnect-interval T` flag that drops + redials
each WS subscriber every T seconds — defer to LH-3 where the
delta-sync path is the thing under test.
