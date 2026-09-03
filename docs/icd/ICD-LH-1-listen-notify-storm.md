# ICD-LH-1 — LISTEN/NOTIFY Subscribe + Notify-Storm Tier

**Status**: Active (diagnostic — infrastructure, not a kernel feature)
**Authored**: 2026-04-22
**Roadmap**: `docs/ROADMAP.md` §Load Harness, LH-1 `[strong]` (promoted
from `[medium]` by ICD authoring — see §1.1)
**Depends on**: ICD-LH-0 (WS `call` frame, `/ws/events` auth flow, driver
package install pattern); ICD-LH-0.1 (tier-profile table format,
diagnostic-mandate framing); ICD-0.5.0 (single-channel `plinth:realtime`
fan-in, envelope contract, 8000-byte ceiling, `pubsub.publish`
extension-identity gate, audit events); ICD-0.4.4 (extension install
lifecycle — how the driver extension is installed under LH-0 flow);
ICD-0.2.4 (RBAC rule registration — `pubsub.publish` grant on the driver
extension's group).

## 1. Purpose

LH-1 is the first LH-stream tier to exercise the v0.5.0 realtime bus
under sustained load. It drives two paths that LH-0 / LH-0.1 never
touched:

- **Layer-3 emit path end-to-end.** Driver extension → `pubsub.publish`
  JS binding → extension-identity gate → regex validation → envelope
  serialization → `emit_notify_async` → Drogon DbClient →
  `SELECT pg_notify('plinth:realtime', $envelope)` → PG ACK → JS
  `await` resolves. Every step is new in v0.5.0.
- **Kernel-side LISTEN + dispatch.** The v0.5.0 `plinth::realtime::listener`
  jthread receives every NOTIFY the harness storm produces, parses the
  envelope, validates the channel regex, and invokes registered
  `EventHandler`s. LH-1 ships with zero in-process consumers registered
  (the 0.5.2 broker isn't shipped yet), so the listener exercises its
  dispatch path against the default no-op handler path.

The subscriber side is **external to the kernel** — LH-1's harness
opens its own `PGconn` and `LISTEN "plinth:realtime"` directly,
bypassing the kernel's in-process dispatch. Two reasons: (1) the JS-side
`pubsub.subscribe` binding is 0.5.2 scope (not shipped); (2) external
LISTEN is a cleaner diagnostic — the harness observes exactly what PG
delivers, independent of the kernel's dispatch state.

### 1.1 Band promotion (medium → strong)

ROADMAP carries LH-1 as `[medium]`. Authoring this ICD ahead of the
implementation session effectively promotes the milestone to `[strong]`
per METHODOLOGY §3.1 — strong-band milestones require a pinned ICD
before opening. LH-0.1 set the precedent (ROADMAP `[strong]`, shipped
with `ICD-LH-0.1`). The CHANGELOG 0.5.0.2 entry records the promotion
so a future RE-EVAL sees it.

## 2. Non-goals for LH-1

- **Fixing any kernel issue LH-1 surfaces.** LH-1 is the diagnostic;
  fixes ship as their own 0.5.0.N follow-ups or 0.5.1-adjacent PRs once
  a reliable reproduction is in hand. Matches the LH-0 / LH-0.1 posture
  exactly.
- **WebSocket broker fan-out to subscribed clients.** 0.5.2 / LH-2
  scope. LH-1 has no WS-connected "subscriber" clients on the kernel
  side — the harness subscribes via raw PG LISTEN.
- **`plinth.events` persistence + delta-sync-on-reconnect.** 0.5.4 /
  LH-3 scope. LH-1 does not write or replay events from `plinth.events`
  (the table doesn't exist yet).
- **Monotonic `seq` ordering verification.** 0.5.5 scope. Envelopes
  emitted under LH-1 storm have no `seq` field; cross-worker ordering
  is not under test.
- **Debouncer / coalescer correctness.** 0.5.1 scope. LH-1 produces
  one NOTIFY per `pubsub.publish` call (no coalescing exists yet on
  0.5.0).
- **Extension visibility into its own NOTIFY fan-out.** `pubsub.subscribe`
  is 0.5.2. Driver extensions under LH-1 emit-only; they cannot observe
  the notifies they produce.
- **Widening `ConnState::is_admin` → full RBAC rule vector on the WS
  side.** Same admin-gate posture as LH-0 / LH-0.1. The driver
  extension's `pubsub.publish` grant still flows through the full
  ICD-0.2.4 RBAC pipeline — LH-1 exercises that path; it does not
  modify it.
- **Cross-node HA propagation under storm.** LH-1 runs against a
  single-node `plinth` instance. Cross-node LISTEN/NOTIFY fan-out per
  `architecture/03-data.md §3.6` is left as a future LH-N tier once a
  multi-node CI fixture exists.

## 3. Kernel contract — no changes

LH-1 ships **zero new kernel surface**. The entire diagnostic rides on
v0.5.0 primitives + a new driver extension + the LH-0 harness binary
with one new tier profile. Specifically:

- No new `lh1:*` kernel signature (contrast LH-0 `lh0:1:chain` and
  LH-0.1 `lh0:1:js_stress`).
- No new dispatch fork in `plinth::ws::on_call` (contrast LH-0.1 §4).
- No new `plinth::realtime::*` API surface; no new audit events; no
  config-surface changes.
- No atexit-chain edits.

This is deliberate: the v0.5.0 surface is complete for Layer-3
emission; LH-1's value is *exercising* it, not extending it. If a
reproduction forces a kernel-side fix, that fix ships in its own PR
with its own ICD (or inline if trivially scoped).

## 4. Harness producer — driver extension `ext_lh1_storm`

### 4.1 Purpose

A new bundled driver extension that exposes one Tier 2 capability
whose handler runs `N` parallel `pubsub.publish` calls per invocation.
The harness drives this capability over WS; the resulting notifies
form the storm.

### 4.2 Package layout

`load-harness/fixtures/ext_lh1_storm/` — new package directory,
packed into `ext_lh1_storm.zip` by the harness build step (extends
the LH-0 `fixtures/driver.zip` pattern):

```
ext_lh1_storm/
├── plinth.json         # manifest
├── capabilities.json   # capability registration
├── rbac.json           # declared rules
└── index.js            # Tier 2 handler
```

- `plinth.json`: name `ext_lh1_storm`, version `0.1.0`, standard
  manifest shape per ICD-0.4.1.
- `capabilities.json`: registers one capability:
  ```json
  { "capabilities": [
      { "signature": "ext_lh1_storm:1:burst",
        "handler": "index.js#burst",
        "rbac": "ext_lh1_storm.burst" } ] }
  ```
- `rbac.json`: declares two rules — `ext_lh1_storm.burst` (for the
  capability itself) and `pubsub.publish` (for the binding the handler
  calls). The harness grants both to the admin group at setup time.
- `index.js`: handler body shown below (§4.3).

### 4.3 Handler surface

```javascript
// ext_lh1_storm/index.js
export async function burst(args) {
  const count = args?.count ?? 1;
  const bytes = args?.bytes ?? 512;

  const payload = makePayload(bytes);
  const promises = [];
  for (let i = 0; i < count; i++) {
    promises.push(pubsub.publish(
      "plinth:ext:ext_lh1_storm:storm_event",
      { seq: i, data: payload }
    ));
  }
  await Promise.all(promises);
  return { emitted: count };
}
```

- `count` — number of notifies this invocation emits. Harness-tunable.
- `bytes` — target payload size in bytes. Handler constructs a
  deterministic string of `bytes` length (e.g. `'x'.repeat(bytes)`);
  envelope overhead (channel + layer + `seq`) keeps total under the
  8000-byte ceiling for `bytes ≤ 7800`. Handler rejects `bytes > 7800`
  with a clear error to fail-fast rather than hit the `PAYLOAD_TOO_LARGE`
  binding reject path.
- Returns `{emitted: count}` on success; rejects if any child
  `pubsub.publish` rejects (the first rejection propagates via
  `Promise.all`).

### 4.4 Per-call fan-out

Each WS `call ext_lh1_storm:1:burst {count: 16, bytes: 512}` produces
16 `pubsub.publish` → 16 `emit_notify_async` → 16 `pg_notify` ACKs
→ 16 envelopes on PG's `plinth:realtime` channel. Worker-level RTT for
the WS call includes all 16 PG round-trips in parallel (one per
`pubsub.publish` child promise).

### 4.5 Install under LH-0 flow

LH-1 harness startup:

1. Admin login via `POST /api/auth/login` (LH-0 flow).
2. `POST /api/packages` with `ext_lh1_storm.zip` — installs through
   the standard pipeline (ICD-0.4.4 / 0.4.5 / 0.4.6 / 0.4.7).
3. Admin grants `ext_lh1_storm.burst` + `pubsub.publish` rules to the
   admin group per the harness's existing seed flow (same pattern LH-0
   uses for `fixtures/driver.zip` ephemeral-user seeding).
4. Harness reaches the workers' `call` loop; each call targets
   `ext_lh1_storm:1:burst`.

On harness teardown (unless `--keep-driver`): `DELETE
/api/packages/{id}?confirm=true`, matching LH-0 convention.

## 5. Harness subscriber — external PG LISTEN

### 5.1 Shape

S dedicated goroutines, each owning a private `*sql.DB` / `pq.Listener`
(one connection per subscriber). Each subscriber:

1. Dials PG with the same connection info the kernel uses (read from
   the same config path or a harness-supplied `--pg-dsn` override).
2. `LISTEN "plinth:realtime"` (single-channel — the only PG channel
   per ICD-0.5.0 §Channel Subscription).
3. Enters a `for { select { <- notifier.Notify } }` goroutine loop that:
   - Parses the envelope JSON.
   - Discriminates by `envelope.channel` (expects
     `plinth:ext:ext_lh1_storm:storm_event`).
   - Records an observation: `(subscriber_id, envelope.payload.seq,
     received_at, channel)`.
4. Exits on context cancellation (SIGINT / `--duration` elapsed).

### 5.2 Why external LISTEN and not `pubsub.subscribe`

`pubsub.subscribe` is 0.5.2 scope (not shipped). Even once shipped, an
external LISTEN is the cleaner diagnostic — it observes exactly what
PG delivered, before any in-process dispatch/filter logic. If LH-2
(WS broker) needs a JS-visible subscriber for its own stress pattern,
that tier can layer on; LH-1's storm tier is PG-surface-scoped.

### 5.3 Subscriber metrics

Per subscriber:

- Observed notify count.
- Per-notify lag: `received_at − envelope.payload.emit_started_at`
  (handler populates `emit_started_at` in the payload before
  `pubsub.publish`; harness serializes it as milliseconds since
  harness start to avoid clock-skew concerns on a single-node run).
- Gap count: difference between maximum observed `seq` and count of
  observed notifies (detects any lost envelopes across the PG LISTEN
  path).

Per run (all subscribers):

- Total observed, total emitted (from handler `{emitted: count}` return
  values summed across workers), ratio.
- p50 / p95 / p99 lag across all subscribers.
- Gap breakdown per subscriber.

## 6. Harness binary — `--tier=storm`

Extends the LH-0 binary (`load-harness/cmd/lh0/`) with one new tier
and the subscriber goroutine fleet.

### 6.1 Tier profile

Added to `load-harness/internal/tiers/tiers.go`:

| Name    | Producer workers | Subscribers | Burst size | Payload bytes | Duration |
|---------|------------------|-------------|------------|---------------|----------|
| storm   | 4                | 4           | 16         | 512           | 120s     |

- **Producer workers (M)**: number of parallel WS workers calling
  `ext_lh1_storm:1:burst`.
- **Subscribers (S)**: number of parallel PG LISTEN subscribers.
- **Burst size (K)**: `count` argument passed to each `burst` call.
  16 targets a realistic per-call fan-out that exercises `Promise.all`
  without tipping the QuickJS runtime into its `max_concurrent`
  async-op cap (0.4.6 `runtime_limits` default is 32).
- **Payload bytes (B)**: target payload body size; full envelope sits
  well under the 8000-byte ceiling.
- **Duration**: 120s — matches LH-0.1's two-minute diagnostic window.

Target emit rate at defaults: M × (calls/sec/worker) × K. With ~10–40
calls/sec/worker sustainable on a single-node kernel (subject to PG
LISTEN/NOTIFY backbone throughput), defaults aim at
~640–2,500 notifies/sec for the 120s window — well below the
saturation rate at which PG's NOTIFY queue begins logging
slow-consumer warnings, so the tier's diagnostic signal is storm
*propagation*, not storm *overflow*.

### 6.2 CLI flags

New flags on the existing `lh0` binary:

- `--tier=storm` — selects the tier profile above.
- `--subscribers N` — override subscriber count (default 4).
- `--burst-size K` — override per-call burst (default 16).
- `--payload-bytes B` — override payload body length (default 512).

Existing LH-0 flags that apply: `--concurrency` (aliases `--producers`
for the storm tier; override M), `--duration`, `--keep-driver`.

### 6.3 Exit codes

- `0` — clean run: zero worker-level WS errors, zero subscriber parse
  errors, zero gap rows, observed/emitted ratio ≥ 0.99 (tolerates
  the ctrl-C edge where a few in-flight notifies land after the
  subscriber cancels).
- `1` — worker or subscriber errors observed; summary prints
  breakdown.
- `2` — setup failure (package install failed, RBAC grant failed,
  listener dial failed).

Matches LH-0 convention.

## 7. Success criteria

### 7.1 Baseline (every run)

- Harness exits 0 under `--tier=storm` default.
- Kernel process does not crash (`SIGSEGV` / `SIGABRT` absent from the
  kernel log during the run).
- Kernel `realtime.notify.rejected` audit event count stays at zero
  during the run (emit path is validated — no payload oversize, no
  regex reject, no layer mismatch).
- Kernel `realtime.listener.reconnected` audit event count stays at
  zero under the default tier (the listener's PG connection should
  remain stable; a reconnect under the 2-minute tier points at a PG
  backbone issue worth surfacing).
- Observed notify count ≥ 0.99 × emitted count across all subscribers
  combined.
- p99 subscriber lag < 5 s.

### 7.2 Diagnostic mandate (driving the 2026-04-22-onward session)

Under `--tier=storm` × 3 trials, paired with kernel-side log tailing:

- **Reproduction** of any of
  `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`, `bad_weak_ptr`,
  or any realtime-subsystem-specific crash on current `main` HEAD
  confirms a race in the 0.5.0 emit/dispatch paths under sustained
  Layer-3 load and unblocks a targeted fix PR.
- **Zero reproductions** on 3 trials is also a meaningful data
  point: it indicates the v0.5.0 realtime bus tolerates the storm
  tier under the production lifecycle, clearing the path for 0.5.1's
  coalescer to layer on top without first addressing a foundation
  issue.

Both outcomes are useful diagnostic outputs — the session records
which occurred in the CHANGELOG's LH-1 ship entry.

### 7.3 Secondary signals

The harness summary records these even when the baseline is clean;
unexpected values warrant investigation even if they don't fail the
run:

- Observed/emitted ratio < 1.0 under `--keep-driver` + no cancel
  (points at lost notifies in the PG LISTEN backbone).
- Subscriber p99 lag > 1 s (points at PG `NOTIFY` queue backlog
  — possible if the kernel's slow consumer pattern emerges on a
  single-node machine).
- Non-zero `realtime.listener.reconnected` audit count (points at
  an unhealthy PG connection under the harness's own load — PG
  connection limit, `pg_terminate_backend` side effect, etc.).

## 8. Observability

All external for LH-1 (matches LH-0 / LH-0.1 convention):

- **Harness stdout**: periodic progress ticks + final summary table
  (per-worker emit counts, per-subscriber observed counts, p50/p95/p99
  lag, gap breakdown, exit-code rationale).
- **Kernel stderr / log** tailed for crash signatures + realtime
  audit events:
  ```
  tail -f <log> | grep -E \
    'free_zero_refcount|list_empty|bad_weak_ptr|SIGSEGV|SIGABRT|realtime\.'
  ```
  The `realtime.listener.started`, `realtime.listener.reconnected`,
  `realtime.notify.rejected` audit events from ICD-0.5.0 §Audit Events
  surface on every run; an LH-1 run under the default tier expects
  exactly one `realtime.listener.started` and zero of the other two.
- **Process**: `ps -o rss,%cpu,pid -C plinth` sampled every 5 s.
- **DB state**: `SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE 'plinth%';`
  before / during / after — verifies no leaked listener or DbClient
  connections from the storm tier.

No `plinth.metrics` — LH-4 wires harness → metrics after the metrics
subsystem lands (gated on 0.7.1). LH-1 operates in the same
`ps`/stdout/log-tail regime as LH-0 and LH-0.1.

## 9. Future work / deferred

- **LH-2 — WS fan-out.** Gated on 0.5.2. Adds M client WS connections
  that subscribe via `pubsub.subscribe` (once shipped); exercises the
  broker's per-connection routing under the same storm tier. LH-2
  reuses LH-1's driver extension as the producer.
- **LH-3 — Reconnect-under-storm.** Gated on 0.5.4. Adds a subscriber
  churn pattern (periodic disconnect + reconnect) to stress the
  delta-sync path against `plinth.events`.
- **Cross-node HA storm.** Once a multi-node CI fixture exists, add a
  tier that runs LH-1 producers against node A while subscribers
  connect to node B — validates `architecture/03-data.md §3.6` fan-out
  under load.
- **Shared LH-stream listener utility.** LH-1 is the second LH-stream
  tier to open a PGconn (LH-0 used HTTP+WS only). If LH-3 / future
  tiers need their own LISTEN path, extract the shared shape into
  `load-harness/internal/pglisten/`. Not required by LH-1 itself.
- **LH-1 kernel-side probe signature.** If the storm tier proves
  insufficient to surface races that only fire when the in-process
  listener-dispatch path is under load (because LH-1's external
  subscriber bypasses that path), add an `lh1:1:probe` kernel
  signature that registers an `EventHandler` on the production
  listener and reports dispatch counts back over WS. Not scoped
  for the first LH-1 ship — add only if the diagnostic needs it.
- **Promote `pubsub.subscribe` storm tier.** Once 0.5.2 ships the JS
  subscribe binding, a future LH-1.1 tier can add a driver extension
  that calls `pubsub.subscribe` and records observations from inside
  the kernel's dispatch path — complementary to the external LISTEN
  subscriber this ICD pins.

## 10. References

- `docs/ROADMAP.md` §Load Harness, LH-1 — stream definition + gating.
- `docs/icd/ICD-LH-0-load-harness-scaffold.md` — base WS `call` frame,
  auth flow, driver-package install pattern, tier-profile precedent.
- `docs/icd/ICD-LH-0.1-async-bridge-stress.md` — diagnostic-mandate
  framing, 120-second tier-duration precedent, 3-trial reproduction
  discipline.
- `docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md` — the contract LH-1
  exercises: single-channel fan-in (`plinth:realtime`), envelope
  contract, 8000-byte ceiling, `pubsub.publish` extension-identity
  gate, audit events, atexit ordering.
- `docs/architecture/03-data.md` §3.1 + §3.6 — realtime design prose
  the bus implements.
- `docs/icd/ICD-0.4.4-package-install-lifecycle.md` — install pipeline
  the `ext_lh1_storm.zip` flows through.
- `docs/icd/ICD-0.2.4-capability-rbac.md` — RBAC grant semantics for
  `ext_lh1_storm.burst` + `pubsub.publish`.
- `project_ws_flaky_segfault.md` (session memory) — flake-family
  history the diagnostic mandate continues.
- `feedback_deterministic_teardown.md` (session memory) — atexit
  convention LH-1's driver extension + harness client connections
  follow.

## 11. Open questions

**OQ1 — Producer mechanism: driver extension vs kernel signature.**
ICD pins: **driver extension via `pubsub.publish`**. Rationale: mirrors
LH-0.1's "drive the production path" posture — the entire Layer-3
emit chain is exercised exactly as a real extension would use it. A
kernel-scope signature (`lh1:1:notify_burst` via dispatch fork) would
reach a higher raw notify rate but bypass the regex, identity gate,
and async-bridge path that are the new surface under test. If the
driver-extension path cannot reach a rate where the diagnostic mandate
fires, add the kernel signature as a second sub-tier in a follow-up
(`lh1.1` or similar).

**OQ2 — Subscriber path: external LISTEN vs kernel in-process probe.**
ICD pins: **external LISTEN via `github.com/lib/pq`**. Rationale: the
subscriber is a diagnostic observer, not a production consumer; it
observes what PG delivered. An in-process probe (a kernel signature
that registers an `EventHandler` and streams dispatch counts over WS)
is listed in §9 future work — add only if LH-1 fails to surface a race
that exists in the in-process dispatch path specifically.

**OQ3 — Driver extension RBAC grant path.** LH-1's harness must grant
`ext_lh1_storm.burst` + `pubsub.publish` to the admin group at setup.
LH-0's ephemeral-user seed handles similar grants for
`fixtures/driver.zip`; the LH-1 session extends the same seed. The ICD
does not pin the exact seed-script edit — implementer's call whether
to add a new seed target or widen the existing one.

**OQ4 — Payload shape beyond `{seq, data}`.** The handler in §4.3
emits minimal envelope bodies. A richer payload shape (e.g. mimicking
a real extension event like
`{"user_id":"u1","room_id":"r7","typing":true}`) would be more
representative of production Layer-3 traffic. ICD pins minimal-for-now;
the implementing session may opt to parameterize if the representative
shape matters for surfacing a specific race.

**OQ5 — CI wiring.** LH-1 does not run in CI today (LH-0 / LH-0.1 are
manual-run-only per the harness README). A future 0.7.1 / LH-4
milestone wires harness metrics into `plinth.metrics` and CI gates
on regression; LH-1's CI wiring inherits that path. Not LH-1 scope.
