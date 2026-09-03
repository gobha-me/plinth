# ICD-0.5.5-sequence-numbers-client-debounce

**Traces to:** architecture/03-data.md §3.4 (Frontend SDK — Debounced
Smart Re-Query — items 1, 3, 5: client-side debounce default 100 ms,
monotonic `seq` per event, thundering-herd jitter 0–50 ms — this ICD
discharges the *server-side wire-protocol commitments* those items
imply, not the SDK code itself which ships in 0.6.3); architecture/03-data.md
§3.5 (Delta Sync on Reconnect — already discharged by ICD-0.5.4 at
the cursor-and-replay layer; this ICD aligns the envelope's `seq`
field with the cursor's `plinth.events.seq` source so the wire
ordering contract is self-consistent across live and replay paths);
architecture/03-data.md §3.3 (Four Realtime Layers — `seq` populates
on every persisted Layer-1/2/3 envelope; Layer-4 sidecar events
defer to 0.8.x); architecture/03-data.md §3.6.1 (Physical Channel
Fan-In — the listener's `plinth:realtime` PG channel; this ICD's
generation pin lives at the writer downstream of the listener, not
at the emitter side, so cross-extension publishers see no API
change); ICD-0.5.0-pg-listen-notify-bridge §Payload Envelope
Contract (the reserved `seq` slot at line 249 — `"seq": <optional
integer, 0.5.5 populates>` — this ICD lifts the reservation:
`seq` becomes REQUIRED on every persisted envelope after writer
INSERT, OPTIONAL on the upstream emit-side and on non-persisted
system frames; the field type stays BIGINT, monotonicity guarantees
documented below); ICD-0.5.0 §`emit_notify_async` (unchanged —
emitters never set `seq`; the writer is the sole authority);
ICD-0.5.0 §Listener Subsystem (the `EventHandler` registration seam
this ICD repurposes — see §5 for the writer-first topology shift
that moves the broker from a peer listener handler to a writer-
downstream consumer); ICD-0.5.1-pg-auto-event-coalescer §Out of
scope (the explicit "Monotonic `seq` generation. 0.5.5. The
envelope's `seq` slot (reserved in ICD-0.5.0 §Payload Envelope)
stays absent in 0.5.1 envelopes — 0.5.5 introduces the generator
in a follow-up ICD" deferral at lines 147–152 — this ICD discharges
that deferral; coalescer envelope shape is otherwise unchanged);
ICD-0.5.1 §Cursor (line 460 "**`seq`** — 0.5.5." — this ICD pins
the cursor advance semantics: `seq` populates on every coalescer-
emitted Layer-1 envelope after the writer INSERTs and stamps);
ICD-0.5.1 §Truncation Heuristic (a `truncated:true` envelope still
gets a `seq` — truncation is independent of sequencing); ICD-0.5.2-ws-broker
§Wire contract (this ICD AMENDS: persisted envelopes leaving the
broker on the live path now carry `seq`; subscribe_ack frames now
carry `recommended_debounce_ms` and `recommended_jitter_ms`
advisory fields; the broker's `dispatch_for_test` seam reads `seq`
through the same envelope reference); ICD-0.5.2 §Subscribe RBAC
(unchanged — `seq` does not bypass per-channel rule checks; replay
re-check still owns visibility); ICD-0.5.2 §Audit Events (rate-
limited aggregation pattern this ICD's audits inherit verbatim);
ICD-0.5.3-db-batch-silent-mode §Silent Flag (silent operations
generate no envelope and therefore no `seq` — silent stays under
the radar, no cursor advance); ICD-0.5.3 §Per-Op `SET search_path`
Isolation (unchanged — extensions still do not query `plinth.events`
directly); ICD-0.5.4-events-table-delta-sync §Traces-to (this ICD
amends ICD-0.5.4's footnote at lines 30–32: "the 0.5.4 cursor uses
`plinth.events.seq` BIGSERIAL, NOT envelope `seq`" → after this ICD,
envelope `seq` == `plinth.events.seq` by construction; cursor
remains the same BIGINT, but the envelope and cursor agree);
ICD-0.5.4 §OQ5 (the resolved-OQ row at line 1764 promised
"0.5.5 envelope `seq` will equal it by construction" — this ICD
delivers); ICD-0.5.4 §When `record_delivered` fires (the writer
INSERT-then-cursor-advance sequence is preserved; this ICD inserts
a stamp-then-fan-out step between INSERT and cursor advance);
ICD-0.5.4 §Test Cases D.08 (the deferred test "Replay then live
frame ordering" at line 1478 is ABSORBED into this ICD as L.03 —
becomes crisply testable only once envelope `seq` is canonical);
ICD-0.1.6-websocket §`/ws` Frame Types (the new `subscribe_ack`
fields `recommended_debounce_ms` + `recommended_jitter_ms` join
the existing 0.1.6/0.5.4 frame catalogue — additive only); ICD-0.1.7-audit
§Audit Writer (the new rate-limited `realtime.seq.*` and
`realtime.debounce.*` audits ride existing infrastructure);
ICD-0.4.4-package-install-lifecycle (extension lifecycle hooks
unchanged — `seq` generation is a kernel-internal concern with
no per-extension state).

**Depends on:** ICD-0.5.0 (envelope contract this ICD lifts the `seq`
reservation from; listener subsystem this ICD's writer-first
topology shift restructures); ICD-0.5.1 (coalescer envelope shape —
the `window_ms` + (now) `seq` fields appear together on Layer-1
envelopes; coalescer's own emit-side `emit_notify_async` is
unchanged); ICD-0.5.2 (broker as the live-path fan-out — this ICD
restructures so the writer is upstream of the broker rather than a
peer; broker's per-connection registry, RBAC checks, and frame
parser are otherwise unchanged); ICD-0.5.4 (writer subsystem +
`plinth.events` table — this ICD's `seq` source is `RETURNING seq`
from the writer's existing INSERT path; cursor store untouched);
ICD-0.1.6 (WS frame catalogue + auth gate this ICD extends additively);
ICD-0.1.7 (audit writer infrastructure).

**Milestone:** 0.5.5 — sequence numbers + client-side debounce
protocol. Sixth 0.5.x code milestone (after v0.5.0 bridge + v0.5.1
coalescer + v0.5.2 broker + v0.5.3 batch + v0.5.4 events-table). The
piece that closes the ICD-0.5.0 §Payload Envelope `seq` reservation
deferred since 2026-04-22, the ICD-0.5.1 §Out of scope monotonic-
generator deferral deferred since 2026-04-23, and the ICD-0.5.4 D.08
test deferred at v0.5.4 ship 2026-04-25. Paper-only ICD authoring
slot `0.5.4.N` precedes this code work per METHODOLOGY §3.1
forward-ICD-presence rule and `feedback_icd_horizon.md`.

**Status:** Paper. Authored 2026-04-25 on
`feat/0.5.4.N-icd-0.5.5-authoring`. Code session pins OQ1–OQ7 then
implements; expected 5–6 phase commit arc.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
[src/kernel/realtime/listener.hpp](../../src/kernel/realtime/listener.hpp) +
[src/kernel/realtime/listener.cpp](../../src/kernel/realtime/listener.cpp)
(handler registration topology — restructured by §5);
[src/kernel/realtime/events_writer.hpp](../../src/kernel/realtime/events_writer.hpp) +
[src/kernel/realtime/events_writer.cpp](../../src/kernel/realtime/events_writer.cpp)
(writer subsystem — already does `INSERT … RETURNING seq` at
events_writer.cpp:268; this ICD adds envelope-stamp + downstream
broker call between INSERT and cursor advance);
[src/kernel/realtime/broker.hpp](../../src/kernel/realtime/broker.hpp) +
[src/kernel/realtime/broker.cpp](../../src/kernel/realtime/broker.cpp)
(fan-out subsystem — moves from listener-handler peer to writer-
downstream consumer; `publish_dispatched` API surface unchanged);
[src/kernel/realtime/cursor_store.hpp](../../src/kernel/realtime/cursor_store.hpp)
(cursor advance — unchanged; reads same `seq` value from same
INSERT result);
[src/kernel/ws/subscriptions.cpp](../../src/kernel/ws/subscriptions.cpp)
(subscribe handler — adds `recommended_debounce_ms` +
`recommended_jitter_ms` to `subscribe_ack` payload);
[src/kernel/config.hpp](../../src/kernel/config.hpp)
(`Config::Realtime::Events` — adds 4 fields per §10);
[src/kernel/realtime/coalescer.cpp](../../src/kernel/realtime/coalescer.cpp)
(coalescer — unchanged; emit-side never sees `seq`);
[migrations/schema.sql](../../migrations/schema.sql)
(schema — UNCHANGED; `plinth.events.seq` BIGSERIAL is already the
generator).

---

## Overview

ICD-0.5.5 lands two named contributions, both of them wire-protocol
commitments rather than user-facing features:

1. **Server-side envelope `seq` generation, aligned to
   `plinth.events.seq`.** Every persisted Layer-1/2/3 envelope leaves
   the kernel carrying a BIGINT `seq` field whose value is exactly
   the BIGSERIAL primary key the writer's INSERT returned. The seq
   is monotonic per PostgreSQL instance (gapless modulo cluster
   restart, per `plinth.events_seq_seq` BIGSERIAL semantics),
   strictly monotonic per-(user, cursor) on the live path, and
   strictly monotonic per-(user, cursor) on the replay path. This
   discharges the 0.5.0 envelope reservation, the 0.5.1 out-of-scope
   deferral, and the 0.5.4 OQ5 promise that envelope `seq` ==
   `plinth.events.seq` by construction. Implementation: re-route the
   listener → handler topology so the writer is the sole listener
   handler and the broker is a writer-downstream consumer (called
   from inside `insert_envelope` after INSERT + stamp). See §5 for
   the topology pin and rejected alternatives.

2. **Wire-protocol coalescing/debounce contract for the future SDK.**
   Three additive surfaces: (a) optional envelope fields
   `coalesced_count`, `window_open_ts_ms`, `window_close_ts_ms` that
   reify the coalescer's already-existing window for SDK consumers;
   (b) advisory fields `recommended_debounce_ms` (default 100) and
   `recommended_jitter_ms` (default 50) on the `subscribe_ack` frame,
   communicating the cadence the kernel expects clients to follow;
   (c) a new audit event `realtime.debounce.advisory_overridden` for
   visibility into clients that ignore the advisory. This is a
   kernel-side commitment the 0.6.3 client SDK will implement
   against. The SDK code itself is OUT OF SCOPE for 0.5.5; this ICD
   pins ONLY the wire surface so the SDK can be authored against a
   stable contract.

The two contributions are intertwined at the wire-format layer (both
extend the envelope; both touch the broker fan-out path) but
semantically independent (sequence numbers work standalone for
ordering; debounce advisories work standalone for cadence). They
ship together to amortize the topology shift in §5 over both
deliverables.

The hard architectural call lives in §5: how the kernel obtains the
canonical `seq` value before the broker fans an envelope to live
subscribers. The recommendation is **writer-first topology** —
the listener dispatches only to the writer; the writer INSERTs,
stamps `seq` from the RETURNING result, and then calls the broker.
This costs the live path one PG INSERT round-trip in latency
(≤ 5–10 ms in steady state, validated against LH-1) but trivially
satisfies the envelope-seq == cursor-seq invariant the 0.5.4 OQ5
already promised.

---

## Glossary

This ICD uses several near-synonyms for sequence-related concepts.
The terms below are pinned for the rest of the document and the
implementing code.

- **envelope-seq** — the optional BIGINT `seq` field on the JSON
  envelope. Reserved at ICD-0.5.0; populated by this ICD. After
  0.5.5 ships, REQUIRED on every persisted envelope leaving the
  kernel; OPTIONAL on emitter-side `emit_notify_async` calls
  (callers never set it); ABSENT from non-persisted system frames
  like `subscribe_ack`, `replay_done`, `resync`, `ping`/`pong`.

- **table-seq** — the BIGSERIAL primary key column on the
  `plinth.events` table. Pinned at ICD-0.5.4. Generated by PG at
  INSERT time via the `plinth.events_seq_seq` sequence.

- **cursor-seq** — the per-user BIGINT stored in
  `plinth.user_event_cursors.last_seq`. Pinned at ICD-0.5.4.
  Advanced by the writer after a successful INSERT-and-deliver.

- **canonical seq** — after this ICD ships,
  envelope-seq == table-seq == cursor-seq when measured at the same
  point in the dispatch flow. The single canonical value lives in
  the `RETURNING seq` result of the writer's INSERT; it propagates
  forward as both the envelope stamp (for live and replay subscribers)
  and the cursor advance value (for reconnect bookkeeping).

- **live path** — the listener-→-writer-→-broker dispatch flow that
  delivers an envelope to currently-connected WS subscribers. After
  this ICD, the writer INSERT is on the critical path before broker
  fan-out (see §5).

- **replay path** — the cursor-driven `replay::run_replay` engine
  pinned at ICD-0.5.4. Reads `plinth.events` `WHERE seq > since_seq`,
  emits `replay` frames in seq order, terminates with `replay_done`.
  Per-row RBAC re-check unchanged.

- **coalesce window** — the 50 ms fixed-duration aggregation window
  per `(schema, table)` pinned at ICD-0.5.1 §Coalescer Subsystem.
  Reified in the envelope post-ICD-0.5.5 via `coalesced_count` +
  `window_open_ts_ms` + `window_close_ts_ms`. The window IS the
  server-side debounce; client-side debounce stacks on top.

- **jitter band** — the 0–50 ms random offset clients apply before
  re-querying after a coalesced event arrives, per
  `architecture/03-data.md §3.4 item 5`. The kernel publishes a
  `recommended_jitter_ms` advisory on `subscribe_ack`; clients
  draw uniformly from `[0, recommended_jitter_ms]`.

- **debounce contract** — the wire-protocol commitments this ICD
  pins so the future SDK has a stable surface to implement against.
  The kernel COMMITS to: (a) `coalesced_count` reflects upstream
  window aggregation honestly (≥ 1, no inflation); (b) advisory
  fields are consistent across reconnects of the same user;
  (c) when client behaviour drifts from advisory, the kernel logs
  the deviation but does not enforce. The kernel does NOT commit
  to: a specific debounce algorithm, or any retry/back-off cadence
  beyond the published advisory.

- **gap** — a missing `seq` value in a contiguous envelope sequence
  observed by a single subscriber on a single channel. After this
  ICD, gaps on the live path are an audit-worthy kernel bug
  (`realtime.seq.gap_detected`); gaps on the replay path occur
  legitimately when `plinth.events` rows are RBAC-skipped or have
  been retention-swept, and do NOT audit.

- **superseded seq** — when the coalescer aggregates N upstream
  events into one outgoing envelope, the (N − 1) source events that
  did not get their own envelope. Optional field
  `superseded_seqs[]` is OFF by default for frame-size cost (see
  OQ4); when ON, lists the source-event seqs the SDK can mark as
  observed without separate frames.

- **node-seq** — informally, an envelope's `seq` value attributed
  to the PG instance that produced it. In a multi-PG-instance HA
  topology (0.9.x), envelope-seq monotonicity holds per-PG-instance
  only; cross-instance ordering is a 0.9.x consensus concern out of
  scope for this ICD.

---

## §4 — Wire Contract: Envelope `seq` Field

### Field shape

The reserved `seq` slot at ICD-0.5.0 §Payload Envelope Contract
(line 249) becomes:

```json
{
  "layer":     "data" | "system" | "extension",
  "channel":   "<full channel name>",
  "schema":    "<string, optional>",
  "table":     "<string, optional>",
  "ops":       [<optional array>],
  "seq":       <BIGINT, REQUIRED on persisted envelopes>,
  "truncated": <optional boolean>,
  "payload":   <optional arbitrary JSON, Layer 2/3>,
  "emitted_at":"<ISO-8601 string>",
  "coalesced_count":   <optional integer ≥ 1>,
  "window_open_ts_ms": <optional integer, ms since epoch>,
  "window_close_ts_ms":<optional integer, ms since epoch>,
  "superseded_seqs":   [<optional array of BIGINT>]
}
```

The `seq` field type is **JSON Number serialized as integer**. JSON
Number cannot represent the full BIGINT range losslessly in
JavaScript (which uses IEEE 754 doubles with a 2^53 mantissa). Per
JS practice, BIGINT cursors should fit comfortably within 2^53
(9.0 × 10^15) for the lifetime of any practical Plinth deployment;
the kernel emits `seq` as a JSON integer within that range. If a
deployment ever approaches 2^53 events (centuries at 1000/s), the
emitter switches to a string-serialized BIGINT — an additive
contract change deferred to whatever 0.10.x or beyond covers it.

### Required vs. optional

`seq` is REQUIRED on:
- Every Layer-1 envelope emitted by the coalescer after writer INSERT.
- Every Layer-2 envelope emitted by kernel system events after
  writer INSERT.
- Every Layer-3 envelope emitted by extension `pubsub.publish` after
  writer INSERT.
- Every `replay` frame's `envelope` field on the replay path.

`seq` is OPTIONAL (typically absent) on:
- Emitter-side `emit_notify_async` calls — emitters never set seq;
  setting it client-side is silently ignored by the writer (writer
  always overwrites with the RETURNING value).
- Non-persisted system frames: `subscribe_ack`, `subscribe_denied`,
  `replay_done`, `resync`, `ping`/`pong`, `unsubscribe_ok`,
  `error`. These frames have no associated `plinth.events` row.
- Envelopes on the listener thread BEFORE the writer's INSERT
  completes (the in-memory `DispatchedEvent` at this point has
  `seq = 0` per the default `Json::Value` integer; the writer
  stamps it before fanning out).

`seq` is FORBIDDEN on:
- The `since_seq` request field of the `subscribe` frame — that's
  client→server, not server→client. (The 0.5.4 frame parser already
  treats `since_seq` as a separate field; this ICD does not change
  that.)

### Monotonicity guarantees

The kernel commits to the following monotonicity properties.
"Strictly monotonic" means `seq[N+1] > seq[N]`; "weakly monotonic"
means `seq[N+1] ≥ seq[N]`; "advisory" means typically holds but no
contract.

| Scope | Guarantee | Source |
|-------|-----------|--------|
| Per single PG instance, all writers | Strictly monotonic, gapless | `plinth.events_seq_seq` BIGSERIAL |
| Per single PG instance, single channel | Strictly monotonic | derives from the above |
| Per (user, cursor) on live path | Strictly monotonic | per-user cursor advances only forward; broker re-orders by seq before fan-out (§8) |
| Per (user, cursor) on replay path | Strictly monotonic | replay query `ORDER BY seq ASC` |
| Per (user, cursor) across live + replay boundary | First live `seq` after `replay_done` > last replay `seq` | §8 ordering contract; pins D.08 |
| Per single channel across PG instances (0.9.x HA) | Advisory; cross-instance interleaving allowed | 0.9.x consensus concern; out of scope |
| Across channels | Advisory | OQ2 leaves per-channel monotonicity strict for single-instance only |

The replay path's per-(user, cursor) strict monotonicity matches the
0.5.4 cursor invariant verbatim: cursor-seq advances using
`GREATEST(last_seq, EXCLUDED.last_seq)` per the cursor-store UPSERT.
This ICD's contribution is the live-path equivalent: by stamping
envelope-seq from the same INSERT that drives the cursor advance,
the live and replay paths now agree on the per-user ordering.

### Gap semantics

Three causes of perceived gaps in `seq` from a subscriber's
perspective:

1. **RBAC-denied envelopes.** When the broker's per-channel rule
   check or the replay engine's per-row re-check denies an envelope
   for a user, that envelope's `seq` is skipped in that user's
   stream. The cursor still advances to that seq (per 0.5.4
   behaviour) — the user is "caught up to that seq" without having
   seen its content. This is the intended security boundary; gap
   audits do NOT fire.
2. **Retention-swept rows on replay.** When a client reconnects with
   `since_seq` older than `MIN(seq) FROM plinth.events`, the
   `cursor_expired` resync precondition fires (per 0.5.4) and the
   client is told to re-query. Any seqs the client missed are
   skipped silently. Gap audit does NOT fire — this is the expected
   retention contract.
3. **Live-path bug.** If a subscriber observes seq[N+1] = seq[N] + K
   where K > 1 within their accessible channel set on the live
   path *and* no RBAC denial happened *and* no replay/reconnect
   intervened, that's a kernel bug — likely a missed broker dispatch
   or a torn writer-then-broker handoff. The `realtime.seq.gap_detected`
   audit fires with `prev_seq` + `next_seq` + `channel` + `user_id`
   for diagnostic purposes (see §11 for rate-limit + payload). The
   kernel does not attempt to repair the gap — clients reconnect to
   resolve.

The third case is detection-only because the kernel does not have
authoritative per-(user, channel) bookkeeping in memory (it would
not scale; the cursor is per-user, not per-(user, channel)). The
audit is fired by the broker at fan-out time, comparing the
outgoing envelope's `seq` against a per-(user, channel) last-seen
cache scoped to the connection's lifetime. On disconnect, the cache
goes away with the connection.

### Equality with table-seq

Post-ICD-0.5.5 invariant: for every persisted envelope, the
envelope's `seq` field equals the value of
`plinth.events.seq WHERE channel = envelope.channel AND payload @>
envelope.payload` for the corresponding row. The implementation
guarantee is stronger: envelope-seq is the literal `RETURNING seq`
result from the INSERT that produced that row, never recomputed.
This makes any divergence between envelope-seq and table-seq an
unambiguous bug — no nuanced "but PG might have…" cases. The L.08
test asserts this invariant after every persisted dispatch.

### Equality with cursor-seq

Post-ICD-0.5.5 invariant: after the writer processes an envelope
that delivered to user U, `plinth.user_event_cursors.last_seq[U]`
equals the envelope's `seq` (assuming no concurrent cursor advance
from a later envelope racing the UPSERT — `GREATEST` semantics
from 0.5.4 handle the race). Cursor-seq advances atomically with
envelope-seq stamp via the same INSERT; they cannot diverge.

### Wire size impact

A BIGINT serialized as a JSON integer adds at most 19 bytes
(`"seq":9007199254740992,`) per envelope. The 0.5.0 §Payload
Envelope 8000-byte ceiling stays in force; coalescer truncation
heuristic continues to count `seq` toward the budget. In practice,
typical envelopes run 200–800 bytes, and the seq field is < 0.1%
overhead. No truncation tuning needed.

---

## §5 — Sequence-Number Generation (Server Side)

### Topology pin

**Recommendation: writer-first topology.** The listener has exactly
one registered `EventHandler` after this ICD ships — the writer's
`handler` function. The broker is no longer a peer listener handler
but is invoked directly by the writer's `insert_envelope` coroutine
after the INSERT-and-stamp completes. Pseudocode:

```cpp
// In events_writer.cpp::insert_envelope, after the existing
// `INSERT … RETURNING seq` at events_writer.cpp:265-269:

const auto SEQ = ins[0]["seq"].as<std::int64_t>();
ev.envelope["seq"] = static_cast<Json::Int64>(SEQ);  // STAMP

// Now fan out to live subscribers from the writer's coroutine.
plinth::realtime::broker::publish_dispatched(ev);    // CALL DOWNSTREAM

// Cursor advance follows verbatim as today (0.5.4).
for (const auto& user_id : ev.delivered_to_users) {
    co_await plinth::realtime::cursor_store::record_delivered(
        user_id, SEQ);
}
co_await tx->execSqlCoro("COMMIT");
```

The change at the listener side: drop the `register_handler(&broker_handler)`
call from `broker.cpp`. Keep `register_handler(&writer_handler)` from
`events_writer.cpp`. The writer is now the sole listener consumer;
the broker is a writer-downstream consumer.

### Why writer-first

1. **Trivially satisfies envelope-seq == table-seq == cursor-seq.**
   The same `RETURNING seq` value is the source for all three. No
   reconciliation logic, no race window, no eventual consistency.
   The 0.5.4 OQ5 promise that envelope `seq` will equal table seq
   "by construction" is delivered without effort.

2. **Collapses the dispatch race to zero.** Today (0.5.4), the
   broker dispatches to live subscribers BEFORE the writer's INSERT
   has happened. A subscriber on the live path sees envelope without
   `seq`; a subscriber on the replay path (because they reconnected
   between broker dispatch and writer INSERT) might see the same
   envelope WITH a `seq`. After writer-first, both paths are
   downstream of the INSERT — same `seq`, no divergence possible.

3. **Reuses existing plumbing.** The writer's `insert_envelope`
   already extracts `SEQ` from `RETURNING` (events_writer.cpp:279)
   and consumes it for cursor advance. The new work is two lines:
   stamp `ev.envelope["seq"] = SEQ` and call
   `broker::publish_dispatched(ev)`. No new subsystem boundaries.

4. **D.08 falls out for free.** The replay engine's `ORDER BY seq
   ASC` and the live path's writer-first stamping mean that the
   first live envelope after `replay_done` has, by construction, a
   `seq` strictly greater than the last replay envelope's `seq`. No
   additional buffering or re-ordering logic required at the broker.
   See §8 for the full ordering proof.

5. **Cleaner failure handling.** When the writer's INSERT fails
   (PG down, advisory lock not granted, etc.), the broker NEVER
   dispatches — there's no envelope-seq to leak, no client-side
   "I saw a seq for an event that doesn't exist" hazard. Writers
   that lose the advisory lock under HA contention silently skip the
   dispatch entirely (consistent with 0.5.4 §HA Semantics —
   "losers skip silently"; the winner's writer fan-outs to live
   subscribers).

### Latency cost analysis

The live path today: listener thread → broker → WS write. End-to-
end latency from PG NOTIFY to subscriber receipt is typically
< 10 ms (broker dispatch is in-memory; WS write is async). The
writer's INSERT happens asynchronously on a separate event-loop
thread; live subscribers receive the envelope before the row exists
in `plinth.events`.

After writer-first: listener thread → writer's enqueue (5 ms drain
timer ceiling) → writer's `insert_envelope` coroutine →
INSERT-RETURNING (typically 1–3 ms PG round-trip on local PG;
3–8 ms on networked PG) → stamp + broker → WS write. End-to-end
latency increases by approximately 5–10 ms in steady state.

This is the trade-off OQ1 leaves open for the code session to
validate against the LH-1 storm tier (4 producers × N subscribers,
sustained for 120 s). Acceptance threshold: p99 live-path latency
remains under 50 ms at observed rate. If LH-1 shows p99 > 50 ms
under writer-first, the code session escalates to OQ1 (a) and
considers `nextval` pre-allocation as a fallback (rejected here on
correctness grounds, but the perf data may force the trade).

### Drain-timer impact

The writer today uses a 5 ms `runEvery` drain timer
(events_writer.cpp:326). Under writer-first, this timer is also the
floor on live-path latency. The code session may consider lowering
to 1–2 ms or replacing `runEvery` with a `notify_one`-driven drain
(via the existing `g_queue_cv` already in events_writer.cpp:66).
This is an OQ — see OQ6.

### Rejected alternatives

**Alternative (a): `nextval` pre-allocation.** The listener (or a
new pre-writer hook) calls `SELECT nextval('plinth.events_seq_seq')`
to obtain the next BIGINT, stamps the envelope with that seq, fans
out via the broker immediately, then enqueues for the writer's
INSERT.

Trade-offs: zero added live-path latency before broker fan-out;
preserves the listener's parallel-handlers topology unchanged.

Rejected because: when the subsequent INSERT fails (PG transiently
down, advisory lock contention pushes the row to a different node,
schema migration in progress, etc.), the envelope-seq has been
broadcast but the corresponding `plinth.events` row never lands.
Replay clients reconnecting later will not see seq N (it's not in
the table); live clients will have seen it. Cursor advances to N
on the live side but `plinth.events` lacks the row. This violates
the canonical-seq invariant and creates a debugging hazard ("client
swears it saw seq N; PG has no row N") that's expensive to track
down.

A weaker variant — pre-allocate seq, fan out, then INSERT — and
accept that gaps may occur on the live path under failure — could
work IF gap-tolerance is built into clients. But the architecture
already commits to gapless live-path delivery (architecture/03-data.md
§3.4 "monotonic sequence number" with no qualifier about gaps).
Building gap tolerance into clients adds debounce-protocol
complexity without a corresponding kernel-side simplification.
Rejected.

**Alternative (c): writer-side independent generator.** Maintain a
separate per-process `std::atomic<int64_t>` counter as the canonical
seq source. Writer stamps from this counter, then INSERTs (with
seq as an explicit value, not auto-generated).

Rejected because: counter does not survive process restart;
multi-node coordination requires an external source of monotonicity
anyway (back to PG); divorces envelope-seq from table-seq, breaking
the 0.5.4 OQ5 promise and forcing the cursor to track a different
source. Operationally heavier with no benefit over writer-first.

**Alternative (d): broker-first with follow-up `seq_assigned`
frame.** Broker dispatches envelope without seq immediately; writer
INSERTs and asynchronously sends a `seq_assigned(envelope_hash, seq)`
frame to subscribers who saw the envelope.

Rejected because: requires a per-envelope identity (hash) that
clients can correlate; doubles the WS frame count for every
persisted envelope; complicates replay path (does replay also send
`seq_assigned` separately, or include seq inline?); creates an
"envelope arrived but not yet seq-stamped" intermediate state on
the client side that the SDK has to handle. All to avoid a 5–10 ms
latency cost. Rejected.

### Topology shift implementation order

Phase boundary: this is a one-shot restructure, not a gradual
migration. The phase 1 commit on the implementation branch should
do the full swap atomically:

1. Move `broker::publish_dispatched` invocation from
   `broker.cpp::handler` (the listener-handler entry point — to be
   deleted) to `events_writer.cpp::insert_envelope` (after the
   stamp).
2. Remove `register_handler(&broker_handler)` from broker startup.
3. Add `ev.envelope["seq"] = SEQ` after the RETURNING extract.
4. Update tests that hooked broker-as-listener-handler (inspect
   `tests/kernel/realtime/broker_test.cpp` for any `register_handler`
   call patterns; these likely become `enqueue_for_test` →
   `apply_drain_for_test` invocations on the writer).

The change is mechanically small but topologically large. The code
session pins this in the first phase and validates LH-1 + LH-2 +
the existing 0.5.4 integration suite before moving to §6/§7's
debounce-advisory work.

---

## §6 — Wire Contract: Coalescing Fields

### Reified coalescer window

The coalescer (ICD-0.5.1) aggregates upstream events into a single
emitted envelope per `(schema, table)` per 50 ms window. Today the
envelope carries `window_ms` (the window duration) but not the
specific window boundaries or the coalesced count. This ICD adds
three optional fields to surface the window:

```json
{
  "coalesced_count":   12,
  "window_open_ts_ms": 1714060800000,
  "window_close_ts_ms":1714060800050
}
```

- **`coalesced_count`**: Integer ≥ 1. Counts the number of
  upstream NOTIFY events the coalescer aggregated into this
  envelope. A single un-coalesced NOTIFY emits with
  `coalesced_count: 1`. Layer-2 and Layer-3 envelopes (kernel and
  extension events that bypass the coalescer) emit
  `coalesced_count: 1`.
- **`window_open_ts_ms`**: Integer milliseconds since Unix epoch.
  Timestamp of the first event in the coalesce window. For
  Layer-2/3 envelopes, equals `emitted_at` (zero coalesce delay).
- **`window_close_ts_ms`**: Integer milliseconds since Unix epoch.
  Timestamp of the window close (50 ms after `window_open_ts_ms` for
  Layer-1 envelopes; equals `window_open_ts_ms` for Layer-2/3).

These fields are OPTIONAL on every envelope but the kernel commits
to populating them whenever the coalescer was involved (Layer-1)
and to populating them with `coalesced_count: 1` and the trivial
window for Layer-2/3 envelopes. The future SDK can rely on the
fields' presence on Layer-1 envelopes; SDKs that wish to remain
agnostic across layers should default-handle absence as
`coalesced_count: 1` and treat the envelope's `emitted_at` as both
window endpoints.

### `coalesced_count` semantics (OQ3 recommendation)

The recommendation is that `coalesced_count` reflects upstream
coalescer hits **honestly** — exactly the count of NOTIFY events
that arrived in the 50 ms window, not the count of underlying
SQL operations or the count of post-broker fan-outs. The
coalescer's `flush` path already knows this number internally;
this ICD asks it to surface that count on the emitted envelope.

Why "honestly":
- A `coalesced_count` that reflects post-broker fan-out would
  conflate the coalescer's work with broker fan-out and confuse
  SDK debounce logic.
- A `coalesced_count` that reflects underlying SQL operations
  would require coalescer instrumentation per row affected — out of
  scope for 0.5.5.
- A `coalesced_count` of "the coalescer hit count" matches what the
  SDK actually needs: "did multiple notifications collapse here?"

### Optional `superseded_seqs[]` field

When `realtime.coalesce.emit_superseded_seqs = true`, the envelope
carries a `superseded_seqs` array listing the seqs of the upstream
events that did not get their own envelope:

```json
{
  "seq": 1052,
  "coalesced_count": 4,
  "superseded_seqs": [1049, 1050, 1051]
}
```

The implication: the SDK can mark seqs 1049, 1050, 1051 as observed
without receiving separate envelopes for each. `last-seen seq`
advances to 1052. A client reconnecting with `since_seq: 1051` would
see no replay frame for 1052 (assuming no further events) because
seq 1052 is the only persisted envelope.

OFF by default per OQ4 — the array is per-event variable-size and
adds frame-size cost that most SDKs don't need (the SDK can infer
the seqs covered by the window from `coalesced_count` alone if it
trusts the coalescer's emit cadence). Operators enable per-channel
or globally when SDK requirements demand it.

### Implementation seam

The coalescer's `flush` path (coalescer.cpp) already constructs
the envelope before emission. This ICD adds:

```cpp
// In coalescer.cpp::flush, after envelope assembly, before
// emit_notify_async:
envelope["coalesced_count"]   = static_cast<Json::UInt>(window.event_count);
envelope["window_open_ts_ms"] = window.first_ts_ms;
envelope["window_close_ts_ms"]= window.first_ts_ms + window_ms;
if (g_cfg.coalesce.emit_superseded_seqs) {
    Json::Value arr(Json::arrayValue);
    for (auto s : window.superseded_seqs) {  // populated upstream
        arr.append(static_cast<Json::Int64>(s));
    }
    envelope["superseded_seqs"] = std::move(arr);
}
```

For Layer-2/3 emit paths (`emit_notify_async` callers): the kernel
emit-side stamps `coalesced_count: 1` and sets both window
timestamps to `emitted_at`. This stamping happens at
`emit_notify_async` entry, not at the coalescer.

`superseded_seqs` requires upstream tracking the source seqs across
the coalesce window. If the coalescer does not currently track
source seqs (it processes raw NOTIFY payloads, not pre-stamped
envelopes), the implementation either: (a) tracks them by adding a
buffered-seq list to `WindowEntry`, or (b) defers `superseded_seqs`
to a follow-up. The recommendation is (a) — the buffer is
bounded by the 50 ms window and `coalesced_count` is naturally also
the buffer size. See OQ4.

### Layer-2/3 envelope shape

Layer-2 (kernel system events) and Layer-3 (extension `pubsub.publish`)
envelopes emit through `emit_notify_async` directly, skipping the
coalescer. After this ICD:

```json
{
  "layer":   "system",
  "channel": "plinth:system:user_login",
  "payload": {...},
  "seq":     <stamped by writer>,
  "emitted_at": "2026-...",
  "coalesced_count":   1,
  "window_open_ts_ms": <equal to emitted_at ms>,
  "window_close_ts_ms":<equal to emitted_at ms>
}
```

The coalesce-window fields stay populated for shape consistency.
SDK code can rely on the fields being present on every persisted
envelope without a layer-conditional branch.

---

## §7 — Wire Contract: Jitter and Recommended Client Cadence

### `subscribe_ack` advisory fields

When a subscribe handshake completes successfully, the kernel
already responds with a `subscribe_ack` frame (per ICD-0.5.2 §Wire
contract). This ICD adds two advisory fields:

```json
{
  "type": "subscribe_ack",
  "channels": ["plinth:data:ext_notes.notes"],
  "recommended_debounce_ms": 100,
  "recommended_jitter_ms":   50
}
```

- **`recommended_debounce_ms`**: Integer milliseconds. The cadence
  the kernel expects clients to debounce re-queries. Default 100 ms
  per architecture/03-data.md §3.4 item 1. Operator-tunable via
  `realtime.debounce.recommend_ms`.
- **`recommended_jitter_ms`**: Integer milliseconds. The maximum
  random jitter clients should add before re-querying. Default
  50 ms per architecture/03-data.md §3.4 item 5. Operator-tunable
  via `realtime.debounce.jitter_max_ms`.

Both fields are **advisory only** — the kernel does not enforce
client compliance. The SDK is expected to: (a) read the advisory
on subscribe; (b) cache it for the subscription lifetime;
(c) debounce re-query timers using `recommended_debounce_ms` as the
window; (d) draw a uniform random offset from
`[0, recommended_jitter_ms]` and add it before querying.

### Why `subscribe_ack` and not per-frame

Per OQ5: the recommendation is to publish advisories once at
subscribe time, not on every envelope.

Reasons:
- Advisories rarely change. Publishing per-frame burns frame size
  and parser cycles for no information gain.
- Per-subscription caching on the SDK side is trivial (one map).
- The single source-of-truth is operator config; per-subscription
  publish keeps the source clear.

Tradeoff: when an operator changes `realtime.debounce.recommend_ms`
mid-session, existing subscriptions don't pick up the new value
until next reconnect. This matches how config reloads work
elsewhere in the kernel (changes to `realtime.events.retention_seconds`
also only apply to new sessions, per 0.5.4 norms). Acceptable.

### Renegotiation audit

The SDK MAY send a `debounce_renegotiate` frame to override the
advisory for its connection:

```json
{
  "type": "debounce_renegotiate",
  "channel": "plinth:data:ext_notes.notes",
  "debounce_ms": 250
}
```

The kernel's response: log the renegotiation via
`realtime.debounce.advisory_overridden` audit, then ignore the
frame (the kernel does not enforce client debounce regardless).
The audit gives operators visibility into clients drifting from
recommended cadence; alarming on the audit count over time can
surface badly-behaved SDK versions or third-party tooling.

The kernel does NOT track per-subscription debounce overrides
(no per-channel state to maintain, no enforcement to do). The
audit is the sole observable.

### Why advisory and not enforced

Enforcement would mean the kernel rate-limits envelope dispatch
to honor a published debounce. This is either:
- **Per-channel rate-limit at the broker** — would require
  per-(connection, channel) timers and queueing logic. Adds
  latency and memory cost to a hot path. Rejected.
- **Per-channel rate-limit at the listener** — would slow ALL
  subscribers down to the slowest, defeating the purpose. Rejected.

Advisory + audit is the right level for a kernel-side commitment.
Clients get the info they need; operators get visibility into
drift; the kernel stays out of the timer business.

### Default values

| Field | Default | Range | Why |
|-------|---------|-------|-----|
| `recommended_debounce_ms` | 100 | [0, 60000] | architecture/03-data.md §3.4 item 1; matches typical UI debounce ergonomics |
| `recommended_jitter_ms` | 50 | [0, 5000] | architecture/03-data.md §3.4 item 5; thundering-herd mitigation in the 1000-client range |

Setting `recommended_debounce_ms = 0` disables client debounce;
useful for testing or for high-frequency-update extensions. Setting
`recommended_jitter_ms = 0` disables jitter; useful for deterministic
test scenarios. Operator-tuning beyond defaults is a 0.9.x perf
concern.

---

## §8 — Live-vs-Replay Ordering Contract (Pins D.08)

### The problem D.08 surfaces

ICD-0.5.4 D.08 (line 1478): "Reconnect with `since_seq:100`;
mid-replay an event emits live" — expected outcome: "Replay frames
in seq order, then `replay_done`, then live frame (queued during
replay)."

Without canonical envelope-seq, this test is structurally hard:
the replay engine emits frames with table-seq values; the live
path emits frames without seq. The "live frame queued during
replay" might have a smaller cursor-seq than the last replay
frame's table-seq — depending on the writer's INSERT timing
relative to the replay query — and clients have no way to tell.

After this ICD, every frame on both paths carries the canonical
seq. The ordering contract becomes crisply pinnable:

> **Ordering Invariant.** For any reconnecting subscriber:
> (a) Replay frames are dispatched in `seq` ascending order.
> (b) The `replay_done` frame is dispatched after the last replay
>     frame and before any live frame.
> (c) Every live frame dispatched to that subscriber after
>     `replay_done` has `seq` strictly greater than the last
>     replay frame's `seq`.

### Why the writer-first topology guarantees (c)

The proof is short. The replay engine's query is
`SELECT … FROM plinth.events WHERE seq > since_seq AND … ORDER BY
seq ASC LIMIT N` (per ICD-0.5.4 §Replay engine). The query reads
a snapshot at execution time; let `MAX_REPLAY_SEQ` be the largest
seq returned.

Under writer-first topology, every live frame's seq is generated
by the writer's `RETURNING seq` after the corresponding INSERT.
The INSERT happens AFTER the live envelope leaves the writer's
queue. For a live frame to reach the broker, its INSERT must
have committed. Therefore:

- When the replay query executes, it captures all rows with
  `seq ≤ MAX_REPLAY_SEQ`.
- Any envelope whose INSERT commits AFTER the replay query
  finishes has `seq > MAX_REPLAY_SEQ` (BIGSERIAL is monotonic
  per-PG-instance).
- Live frames dispatched after `replay_done` are precisely those
  envelopes whose INSERT committed after the replay query
  finished — by construction.

Therefore live-frame `seq > MAX_REPLAY_SEQ`. QED.

### Mid-replay live-emit handling

When the replay query is in flight (chunk 1 of N) and a new
envelope arrives at the writer:

1. Writer enqueues the new envelope on the write queue.
2. Writer's drain timer pops, INSERT commits, `seq` is generated
   (call it `S_new`).
3. Writer stamps envelope with `seq = S_new`.
4. Writer calls `broker::publish_dispatched(envelope)`.
5. Broker fans to live subscribers — INCLUDING the subscriber
   whose replay is still in flight.

The replay-in-flight subscriber now has a live frame with seq
S_new while the replay engine is still emitting frames with
seq < S_new. The ordering contract says live frames must come
AFTER `replay_done`. To honor this, the broker buffers live
frames addressed to that subscriber while their replay is in
flight, and flushes the buffer immediately after `replay_done`.

The broker maintains a per-(connection, subscription) flag:
`replay_in_flight: bool`. When true, live frames for that
subscription queue into a per-connection buffer (
`live_buffer: std::deque<Envelope>`) instead of being dispatched.
The replay engine sets `replay_in_flight = false` and flushes
the buffer atomically as the last step of `run_replay` before
emitting `replay_done`.

Wait — that ordering would put the buffered live frames BEFORE
`replay_done`. Re-stating: replay engine's last step is
`emit replay_done`, then `replay_in_flight = false`, then flush
the buffer in seq order. Under the writer-first topology, every
buffered live frame has `seq > MAX_REPLAY_SEQ` (proven above), so
flushing the buffer in seq order is guaranteed correct ordering.

### Live-buffer cap (OQ6)

The per-(connection, subscription) buffer is bounded. If a
slow-replay client's live buffer fills up while the replay engine
chugs through a large chunk, we don't want unbounded memory
growth. The recommendation is a configurable cap (default 256
frames). On overflow, the broker:

1. Discards the buffer.
2. Sets `replay_in_flight = false`.
3. Aborts the in-flight replay (the `run_replay` coroutine
   detects the abort flag at next chunk boundary and returns
   early without `replay_done`).
4. Emits a `resync` frame with `reason = "live_buffer_overflow"`.
5. Marks the cursor as advanced to `MAX(MAX_REPLAY_SEQ_so_far,
   first_buffered_live_seq − 1)` — whichever is the safer
   "you've definitely seen up through here" point.
6. Audits `realtime.events.resync_required reason=live_buffer_overflow`.

Client receives `resync` with the new reason, knows to re-query
all subscribed data, and proceeds. This is a fourth resync
precondition reason joining the three from 0.5.4 (`cursor_expired`
/ `mismatch` / `row_cap`) plus the caller-side `events_disabled`.
The 0.5.4 ICD's resync-reason taxonomy gains one entry.

### Why a buffer-cap matters: the slow-replay pathology

Without a cap, a client whose connection is degraded during a
large replay (10000 rows × 5 ms each = 50 s of replay) could
accumulate 50 s of live frames in memory per subscription. With
1000 subscribers in this state, kernel RSS grows by 50 s ×
1000 ×  envelope size = potentially hundreds of MB. The cap is
a backpressure signal: if a client can't keep up with replay,
they re-query rather than letting the kernel hold their state.

256 frames is a reasonable starting point: a 50 ms coalesce
window emits roughly 1 frame per channel; 256 frames would
correspond to 12.8 s of full-rate emission per channel; clients
who fall behind by more than that are clearly broken or
disconnected. Code session pins the actual default after LH-3
exercises the path.

### Ordering across multiple channels

The ordering contract is **per (user, cursor)**, not per (user,
channel). When a user reconnects subscribing to channels A and B
both with `since_seq: 100`:

- Replay query reads ALL `plinth.events` rows with seq > 100 and
  channel ∈ {A, B}, ordered by seq ascending. Frames interleave
  by seq, not grouped by channel.
- Per-channel monotonicity holds within each channel's subset.
- Cross-channel monotonicity holds via the canonical seq.

The L.06 test exercises this: 2 channels, 50 events each
interleaved (alternating A and B writes), reconnect — replay
emits in seq order regardless of channel, cursor advances to
the max seq.

### `replay_done` payload extension

The `replay_done` frame carries an `up_to_seq` field per ICD-0.5.4
(line 1472). After this ICD, that field equals `MAX_REPLAY_SEQ`
(the max seq the replay engine emitted). Live subscribers can
sanity-check: their next live frame's seq must be > `up_to_seq`.

Optional addition: `replay_done` also carries
`buffered_live_count: <int>` indicating how many live frames the
broker's buffer is about to flush. SDK uses this to size its
processing queue. Default: include the field always; value is 0
when no live frames were buffered.

---

## §9 — Multi-Channel and Per-User Ordering

### Per-user strict monotonicity

The cursor-store invariant pinned at ICD-0.5.4: for any user U,
`plinth.user_event_cursors.last_seq[U]` is strictly monotonic
across time (UPSERTs use `GREATEST(last_seq, EXCLUDED.last_seq)`).
This ICD does not change cursor semantics; cursor-seq advances
exactly when an envelope is delivered to U via the live path AND
that envelope's INSERT has committed (writer-first guarantees
both happen together).

The user-visible promise: across reconnects, replays, and live
delivery, U's seen-seq stream is strictly monotonic.

### Per-channel advisory only

The kernel does NOT commit to strict per-channel monotonicity in
all topologies. Specifically:

- **Single PG instance:** per-channel monotonicity holds (BIGSERIAL
  is global per-instance; channel filter doesn't break ordering).
- **Multi-PG-instance HA (0.9.x):** each PG instance generates its
  own seq sequence; cross-instance per-channel ordering becomes a
  consensus problem. Out of scope for this ICD.

The advisory level of commitment lets 0.9.x design the
cross-instance ordering without breaking the 0.5.5 contract.

### Cross-channel ordering

Across distinct channels within one PG instance, BIGSERIAL gives
us total ordering by seq. The kernel exposes this via the cursor
mechanism (cursor advances across all channels' seqs). SDKs that
want cross-channel ordering can use seq directly; SDKs that don't
care can ignore.

### Implications for OQ2

OQ2 leaves the per-channel-vs-per-user pinning as: per-user
strict, per-channel advisory (single-instance-strict; multi-
instance-loose). The recommendation rationale is that pinning
per-channel strict across instances would require either:

- A global PG instance acting as the seq authority (single point
  of failure; latency cost on every emit).
- Distributed consensus per channel (Paxos/Raft scope; out of
  proportion to the realtime use case).

Neither is justified by current 0.5.x requirements. Per-user
strict (pinned today) covers the SDK's per-subscription cursor
needs; per-channel advisory is "good enough" for cross-channel
analytics within a single-instance deployment, which is the
current Plinth deployment shape.

---

## §10 — Configuration

### Implementation deviation (v0.5.5 ship)

Recorded per METHODOLOGY §Phase 2 Constraint #4 + ratified at
`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md §4.1`.

1. **`SeqSource` is `enum class : std::uint8_t`, not `std::string`.**
   The pseudocode below shows `seq_source: std::string` for symmetry
   with the other text-keyed `Config::Realtime::Events` fields. The
   shipped surface is
   `enum class SeqSource : std::uint8_t { WRITER_RETURNING }` with a
   single value. Rationale: `cursor_store.cpp` and `events_writer.cpp`
   hold static-storage `g_cfg` instances of `Config::Realtime::Events`
   (cert-err58-cpp); a `std::string` field would require a non-trivial
   default constructor. The enum keeps the type trivially default-
   constructible. Future source values (e.g. `INDEPENDENT_COUNTER`,
   `NEXTVAL_PREALLOCATE`) extend the enum rather than introduce a string
   keyspace.
2. **`superseded_seqs[]` ships as a stable empty array** when
   `emit_superseded_seqs=true`. Population semantics deferred per
   §6 §Implementation seam — writer-first topology forecloses
   source-seq tracking through the existing coalescer→writer
   boundary. W.06 test case carries the deferral; a follow-up ICD
   (post-0.5.5) will pick up the design question. See DEFERRED.md
   2026-04-26 entry.
3. **Replay-side seq stamp lives between `parse_payload` and
   `build_replay_frame` in `replay.cpp`**, not inside
   `build_replay_frame` itself. The pseudocode at §5 shows the stamp
   call inside the frame builder; the shipped separation is cleaner —
   `build_replay_frame` stays pure and the stamp is a caller-side
   responsibility. Functionally equivalent; the invariant
   envelope-seq == `plinth.events.seq` holds.

### New `Config::Realtime::Events` fields

Added to the existing `Config::Realtime::Events` substruct
(per ICD-0.5.4 §Configuration). All fields are operator-tunable
via the existing config loader; bound checks are hard-fail at
startup.

| Field | Type | Default | Range | Purpose |
|-------|------|---------|-------|---------|
| `seq.source` | enum | `"writer_returning"` | `"writer_returning"` only (room for future variants) | Topology pin per OQ1; current ICD ships only the writer-first variant |
| `live_buffer_cap_per_subscription` | int | 256 | [16, 65536] | OQ6 — max live frames buffered per (connection, subscription) during replay |
| `coalesce.emit_superseded_seqs` | bool | false | — | OQ4 — emit `superseded_seqs[]` array on coalescer envelopes |
| `debounce.recommend_ms` | int | 100 | [0, 60000] | `recommended_debounce_ms` advisory on `subscribe_ack` |
| `debounce.jitter_max_ms` | int | 50 | [0, 5000] | `recommended_jitter_ms` advisory on `subscribe_ack` |
| `seq.gap_audit_window_ms` | int | 60000 | [1000, 3600000] | Sliding-window dedup for `realtime.seq.gap_detected` audits |

### Why no `seq.enabled` toggle

A "disable seq" config knob would let operators turn off the wire
contract. This ICD does NOT include such a toggle because:

- Once SDKs depend on `seq` for cursor tracking (post-0.6.3),
  toggling it off is a breaking client-side change.
- The 0.5.4 cursor-store and replay path already depend on
  `plinth.events.seq`; envelope-seq is just propagation. Turning
  off envelope-seq while keeping cursor-seq creates the same
  inconsistency this ICD eliminates.
- No operational scenario justifies disabling. seq adds < 0.1%
  envelope size and the writer-first topology is uniform.

If a future scenario requires gating, the config knob is added at
that time, not preemptively.

### Why `seq.source` is a single-value enum today

Forward-looking: the enum lets future ICDs add variants
(`nextval_preallocate`, `independent_counter`) without breaking
the config schema. The 0.5.5 implementation rejects existing
config files that set `seq.source` to anything other than
`"writer_returning"` with a clear hard-fail error message.

### Operator UX

A typical operator config snippet:

```yaml
realtime:
  events:
    enabled: true
    retention_seconds: 3600
    # ... existing 0.5.4 fields ...
    seq:
      source: writer_returning
      gap_audit_window_ms: 60000
    live_buffer_cap_per_subscription: 256
    coalesce:
      emit_superseded_seqs: false
    debounce:
      recommend_ms: 100
      jitter_max_ms: 50
```

The defaults are correct for the typical Plinth deployment;
operators tune only when SDK requirements or telemetry justify.

---

## §11 — Audit Events

### New `realtime.seq.*` events

| Event | Trigger | Payload | Rate-limit |
|-------|---------|---------|------------|
| `realtime.seq.gap_detected` | Live-path gap observed at broker fan-out (per-(connection, channel) last-seen cache) | `{user_id, channel, prev_seq, next_seq, gap_size, count_in_window, window_ms}` | `seq.gap_audit_window_ms` per (user_id, channel) |
| `realtime.seq.replay_seq_mismatch` | Replay engine observed envelope-seq != table-seq during run_replay (kernel bug indicator) | `{user_id, channel, table_seq, envelope_seq, count_in_window, window_ms}` | `seq.gap_audit_window_ms` global |

### New `realtime.debounce.*` events

| Event | Trigger | Payload | Rate-limit |
|-------|---------|---------|------------|
| `realtime.debounce.advisory_overridden` | Client sent `debounce_renegotiate` frame | `{user_id, channel, advisory_ms, override_ms, count_in_window, window_ms}` | `audit_window_ms` per (user_id, channel) — reuses existing knob from 0.5.4 |

### Why three audits (not more)

- `seq.gap_detected` covers the live-path observability gap. The
  detection mechanism is per-(connection, channel) state at the
  broker; small memory cost, high diagnostic value.
- `seq.replay_seq_mismatch` is a kernel-bug canary. Should fire
  zero in normal operation; firing means envelope-seq diverged
  from table-seq, indicating a writer-first topology violation.
- `debounce.advisory_overridden` gives operators visibility into
  client-side compliance with the published cadence. Aggregate
  trends over time surface SDK version drift.

Other potential audits (e.g., per-coalesce-window emit count,
per-subscriber buffer depth) are out of scope — they belong in
metrics (0.7.1), not audit log. The audit pipeline is reserved
for events with security or correctness implications.

### Rate-limit posture

All three events ride the existing rate-limited audit pipeline
(coalescer.cpp / events_writer.cpp pattern: per-key sliding
window, suppress non-first emit within window, count-in-window
tracked). The window key for `seq.gap_detected` and
`debounce.advisory_overridden` is `(user_id, channel)` — bursty
gap detection on one channel doesn't drown out a different gap
elsewhere. The window key for `seq.replay_seq_mismatch` is global
because any non-zero rate of this audit is already a fire-alarm
condition; operators see it once and investigate.

---

## §12 — HA Considerations

### Single-PG-instance topology (today)

The 0.5.4 advisory-lock-per-envelope pattern carries forward:
multi-node Plinth deployments share one PG instance; the writer's
`pg_try_advisory_xact_lock(channel || \\0 || emitted_at)` ensures
exactly one node INSERTs each envelope. After this ICD, the
winning node also does the broker fan-out (because the broker is
now downstream of the writer). Losing nodes silent-skip both the
INSERT and the fan-out (consistent — no duplicate live delivery,
no duplicate cursor advance).

The implication: when Plinth deploys with 2+ kernel nodes against
1 PG instance, exactly one node delivers each live envelope. The
0.5.2 broker's design accommodates this (each node's broker has
its own per-connection registry; only the winning node fans to its
local subscribers; users connected to the losing node receive
nothing on the live path for that envelope but pick it up on next
reconnect via the cursor/replay path).

This is a design carry-forward, not a regression — 0.5.4 already
had this property because the writer (which advances the cursor)
ran only on the winning node. After 0.5.5, the live broker fan-out
also runs only on the winning node; before 0.5.5, the broker fan-
out ran on every node (each node's listener received the NOTIFY
and dispatched). Subscribers connected to the non-winning node
previously got a "broker live frame without cursor advance"; now
they get nothing live, which is consistent with their cursor not
advancing.

The user-visible difference: previously, a multi-node deployment
would show users connected to non-winning nodes a "phantom" live
frame (no cursor advance, would replay on reconnect). Now, those
users see nothing live until they reconnect to the winning node or
the winning-node leadership rotates (e.g., advisory lock transfer
on next envelope to a different `(channel, emitted_at)` pair).

This is an improvement (no phantom frames) and consistent with the
0.5.4 cursor invariant.

### Multi-PG-instance topology (0.9.x)

Out of scope. Cross-PG-instance seq monotonicity requires either
a global seq authority or a consensus protocol; neither belongs
in 0.5.5. The 0.9.x HA milestone owns this.

The 0.5.5 ICD pins the per-PG-instance seq monotonicity guarantee
explicitly so 0.9.x has a clear contract to extend.

### Failover scenarios

When the winning node crashes mid-INSERT (advisory lock holder
dies), the next envelope on a different `(channel, emitted_at)`
key may be processed by a different node. The 0.5.4 design
handles this — the dead node's lock releases on connection
teardown; a different node grabs the next envelope. After 0.5.5,
the same applies: the new winning node also handles the broker
fan-out. Live subscribers connected to the new winner see the
envelope; subscribers connected to other nodes see nothing live
but pick up via cursor/replay.

The dead node's INSERT-in-flight either committed (and the row
exists with a seq, but the dispatch never happened — the next
arriving envelope or a reconnecting subscriber's replay picks it
up) or did not commit (no seq generated, no cursor advance —
subscribers see no gap because the envelope simply did not happen
from their perspective).

### Cross-node cursor consistency

The cursor store is per-PG (single shared `plinth.user_event_cursors`
table). Cursor advances monotonically per ICD-0.5.4 regardless of
which node performed the advance. After 0.5.5, the writer-first
topology ensures the cursor advance happens on the same node as
the INSERT; cross-node cursor reads are unaffected.

---

## §13 — Security Constraints

### SC1 — Per-user seq isolation

`seq` is a per-PG-instance global ordinal. Users observe only the
seqs of envelopes they're authorized to receive (per-channel RBAC
gate at subscribe + per-row re-check at replay, both inherited
from 0.5.2 / 0.5.4). The global-seq value itself is NOT secret —
it's a counter — but the implication that "user U received seq N"
discloses the existence of N to U.

The threat model accepts this: the existence of a seq value
within the per-instance global counter is not a security boundary.
Plinth's 0.5.4 §Security Constraint 2 already accepted the
"transparent BIGINT cursor" trade-off for the same reason.

**SC1 holds.** Continue the transparent-cursor posture; do not
attempt to per-user randomize or salt the seq value.

### SC2 — RBAC re-check on replay

Inherited verbatim from ICD-0.5.4 §Security Constraint 5 / ICD-0.5.2
§Security Constraint 5. Per-row re-check on replay is unchanged
by this ICD. The cursor advance still happens for RBAC-skipped
envelopes (consistent with 0.5.4 — "you're caught up to here, you
just didn't see this one").

### SC3 — Live-path RBAC re-check

Inherited from ICD-0.5.2 §SC2. The broker's live-path RBAC re-check
fires before fan-out per the 0.5.2 design. Writer-first topology
does not change this; the broker still does its check before the
WS write.

### SC4 — Coalesce-window timestamp leakage

The new `window_open_ts_ms` and `window_close_ts_ms` fields expose
when the kernel processed a window. This is not a security
boundary — emit timestamps are already exposed via `emitted_at`.
No additional disclosure.

### SC5 — `superseded_seqs[]` skipped-RBAC concern

When `coalesce.emit_superseded_seqs = true` and the coalescer
aggregated 4 source events into envelope seq 1052 with
`superseded_seqs: [1049, 1050, 1051]`, the array is computed at
the coalescer (pre-RBAC). A subscriber to whom envelope 1052 is
delivered learns that seqs 1049–1051 existed for the same channel,
even if those individual seqs would have been RBAC-denied to them.

The disclosure is bounded: the subscriber learns "events at these
seqs existed on this channel" but not their content. The threat
model accepts this for the same reason as SC1: seq existence is
not a security boundary.

If a deployment determines the disclosure is unacceptable, set
`coalesce.emit_superseded_seqs = false` (the default). This trade-
off matches the 0.5.1 `truncated:true` posture: clients see "an
event happened" without seeing the content.

### SC6 — Debounce advisory injection

The `recommended_debounce_ms` and `recommended_jitter_ms` fields
are operator-controlled (config); clients do not influence them.
A malicious client cannot set their own advisory; they can only
ignore the published one (audited via SC7).

### SC7 — Renegotiation audit

The `debounce.advisory_overridden` audit logs renegotiation
attempts but does not enforce any rate limit on the renegotiation
itself (clients can renegotiate as fast as they want — the kernel
ignores the requests). To prevent log flooding, the audit fires
once per `(user_id, channel)` per `audit_window_ms` window
(default 60 s, reusing the existing 0.5.4 knob).

A malicious client cannot DoS the audit pipeline: bursty
renegotiation collapses into one audit per window per channel.

---

## §14 — Test Cases

### Sequence generation — `tests/kernel/realtime/seq_generation_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| S.01 | Happy | Writer INSERTs envelope; broker fan-out called; envelope.seq stamped | Yes | `envelope.seq == plinth.events.seq` for that row; broker observes the stamped value |
| S.02 | INSERT failure | Mock DbClient raises on INSERT; writer catches | Synthetic | `audit_write_failed reason=pg_error` fires; broker NOT called; envelope.seq remains absent; cursor unchanged |
| S.03 | Advisory lock loss | E.07-style lock hook returns false | Synthetic | Writer silent-skips; broker NOT called; cursor unchanged; no audit (matches 0.5.4 §HA losers) |
| S.04 | Concurrent writers | Two writer threads race on different envelopes | Yes | Each thread's seq returns a distinct BIGSERIAL value; per-thread INSERTs commit; per-thread broker fan-out happens once per envelope |
| S.05 | Sequence inspection | Insert 100 envelopes; verify `plinth.events.seq` strictly monotonic | Yes | `MAX(seq) - MIN(seq) == 99`; no gaps under no-failure conditions |
| S.06 | Gap audit fires | Inject a gap by manually deleting a row, then forcing a live emit | Yes | Broker's per-(connection, channel) cache observes gap; `realtime.seq.gap_detected count_in_window=1` fires [Closed 0.6.0.N session 8 (2026-04-28) via per-conn cache + immediate-send-path detection in `deliver_to_conn` (`src/kernel/ws/publish.cpp`). Tests at `tests/kernel/realtime/gap_detection_test.cpp` (S.06a/b/c). Implementation deviation: the per-(connection, channel) cache lives on `ConnState.last_live_seen_seq` rather than in broker.cpp — equivalent contract, co-located with existing per-conn delivery state. See CHANGELOG `2026-04-28 — 0.6.0.N test-fixture buildout, session 8 of N`] |
| S.07 | Cursor catches up after writer crash | Simulate writer process restart mid-window; envelopes during downtime persist via 2nd-node | Yes | On reconnect, cursor advances correctly from `plinth.user_event_cursors`; live path resumes from new node's writer |
| S.08 | Events disabled emits no seq | `realtime.events.enabled=false`; emit Layer-1 event | Yes | No INSERT into `plinth.events`; envelope reaches broker with `seq` ABSENT; live subscribers see envelope without seq (graceful degradation); no cursor advance |

### Wire contract — `tests/kernel/realtime/envelope_shape_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| W.01 | Persisted envelope has seq | Layer-1 emit after writer INSERT | Yes | envelope JSON contains `"seq": <int64>` matching `plinth.events.seq` |
| W.02 | Non-persisted frame has no seq | `subscribe_ack`, `replay_done`, `resync`, `error` frames | Yes | None contain a `seq` field; frame parser does not require seq on these |
| W.03 | `coalesced_count` ≥ 1 | Layer-1 envelope from coalescer; Layer-2/3 envelope from emit_notify_async | Yes | `coalesced_count` present and ≥ 1 on all persisted envelopes |
| W.04 | Window timestamps consistent | Layer-1 envelope from a 50ms coalesce window | Yes | `window_close_ts_ms - window_open_ts_ms == 50`; Layer-2/3: equal |
| W.05 | `superseded_seqs[]` absent when off | `coalesce.emit_superseded_seqs=false` | Yes | Field absent from envelope JSON |
| W.06 | `superseded_seqs[]` populated when on | `coalesce.emit_superseded_seqs=true`; coalesce 4 events | Yes | Field is array of 3 BIGINTs (the superseded seqs) |
| W.07 | Frame size under cap | Worst-case envelope (8000 bytes - existing fields) + new fields | Synthetic | Total < 8000 bytes; truncation heuristic still operates correctly |

### Debounce semantics — `tests/kernel/realtime/debounce_advisory_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| D.01 | `subscribe_ack` carries advisory | Client sends `subscribe`; receives ack | Synthetic | `subscribe_ack.recommended_debounce_ms == 100` (default); `recommended_jitter_ms == 50` |
| D.02 | Operator config override | Set `realtime.debounce.recommend_ms=250`; client subscribes | Synthetic | `subscribe_ack.recommended_debounce_ms == 250`; default jitter still 50 |
| D.03 | Renegotiation audits | Client sends `debounce_renegotiate` frame | Synthetic | Kernel ignores frame (no state change); `realtime.debounce.advisory_overridden count_in_window=1` audit fires |
| D.04 | Renegotiation rate-limited | Client sends 100 renegotiate frames in 1 s | Synthetic | One audit fires (window dedup); `count_in_window=100` |
| D.05 | Zero debounce honored | `realtime.debounce.recommend_ms=0` | Synthetic | `subscribe_ack.recommended_debounce_ms == 0` |

### Jitter — `tests/kernel/realtime/jitter_advisory_test.cpp` (folded into debounce TU above as J.\*)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| J.01 | Default jitter populated | Client subscribes with default config | Synthetic | `subscribe_ack.recommended_jitter_ms == 50` |
| J.02 | Operator zero jitter | `realtime.debounce.jitter_max_ms=0` | Synthetic | `subscribe_ack.recommended_jitter_ms == 0` |
| J.03 | Operator max jitter | `realtime.debounce.jitter_max_ms=5000` | Synthetic | `subscribe_ack.recommended_jitter_ms == 5000` |

### Live-vs-replay ordering — `tests/kernel/realtime/live_replay_ordering_test.cpp` (new TU; absorbs D.08)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| L.01 | Live path strictly monotonic | Subscriber receives 100 live envelopes | Yes | Every envelope's seq > previous envelope's seq |
| L.02 | Replay path strictly monotonic | Reconnect with `since_seq=100`; replay 50 envelopes | Yes | Every replay frame's seq > previous frame's seq; cursor advances |
| L.03 | First live > last replay (D.08 redux) | Reconnect with `since_seq=100`; mid-replay an envelope emits live | Yes | Replay frames in seq order, then `replay_done`, then live frame; live frame's seq > `replay_done.up_to_seq` [Closed in 0.6.0.N session 6 (2026-04-28) at `tests/kernel/realtime/live_replay_ordering_test.cpp` — replaces SKIP stub] |
| L.04 | Mid-replay buffering preserves order | Reconnect; 3 live envelopes emit during replay | Yes | All 3 buffered; flushed in seq ascending order after `replay_done` [Closed in 0.6.0.N session 6 (2026-04-28) at `tests/kernel/realtime/live_replay_ordering_test.cpp`] |
| L.05 | Live buffer overflow forces resync | `live_buffer_cap_per_subscription=4`; emit 5 live envelopes mid-replay | Yes | 4th overflows; replay aborts; `resync reason=live_buffer_overflow` emitted; cursor advanced safely [Closed in 0.6.0.N session 6 (2026-04-28) at `tests/kernel/realtime/live_replay_ordering_test.cpp` via the new `plinth::ws::test_seam::live_buffer_cap_override` seam] |
| L.06 | Multi-channel reconnect interleaves correctly | Subscribe to A and B; reconnect with same `since_seq`; alternating writes | Yes | Replay frames interleave by global seq order; per-channel monotonicity holds |
| L.07 | RBAC-denied row in replay does not bump cursor | Mid-replay, mock revoke rule for one channel | Yes | Replays before revoke deliver; replays after skip; cursor advances to last DELIVERED seq, not last queried seq |
| L.08 | envelope.seq == cursor.seq invariant | After every persisted dispatch | Yes | `cursor[user].last_seq == envelope.seq` for the most recent envelope delivered to user |

### Integration — `tests/integration/seq_debounce_integration_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| I.01 | LH-1 storm + envelope-seq monotone | Run LH-1 fixture (4 producers × N subscribers × 60 s) under writer-first topology | Yes | Per-subscriber observed seqs strictly monotonic; observed/emitted ≥ 0.99; no `seq.gap_detected` audits in steady state |
| I.02 | LH-2 fanout + ordering | Run LH-2 fixture (M subscribers per envelope) | Yes | All M subscribers observe same seq for same envelope; no cross-subscriber divergence |
| I.03 | Reconnect after writer failover preserves cursor | Two-node deployment; reconnect after node failover | Yes | Cursor invariant holds across failover; envelope.seq monotonic from each surviving node's perspective |
| I.04 | Cross-extension publish + subscribe ordering | Ext A publishes; Ext B subscribes via `pubsub.subscribe` | Yes | Ext B's callback receives envelopes in seq order; per-extension cursor (if applicable) advances monotonically |
| I.05 | Real WS client end-to-end | Real WS client subscribes, ext writes, client receives, drops, reconnects with `since_seq`, receives replay, more writes happen, client receives live | Yes | All envelopes received exactly once; seq monotonic across replay → live boundary; `replay_done.up_to_seq` < first live seq |

### Test case totals

- **S.\*** sequence generation — 8 cases
- **W.\*** wire contract — 7 cases
- **D.\*** debounce semantics — 5 cases
- **J.\*** jitter — 3 cases
- **L.\*** live/replay ordering (folds D.08) — 8 cases
- **I.\*** integration — 5 cases
- **Total: 36 cases**

### Test-seam notes

- **S.\*** (8) — 6 PG-gated; S.02 + S.03 use the existing
  `set_insert_hook_for_test` / `set_advisory_lock_hook_for_test`
  seams from events_writer.hpp lines 71–83.
- **W.\*** (7) — 6 PG-gated; W.07 pure (envelope JSON
  serialization size check).
- **D.\*** (5) — all synthetic (config-driven; no PG required).
- **J.\*** (3) — all synthetic.
- **L.\*** (8) — all PG-gated. L.05 needs a `live_buffer_overflow_for_test`
  seam on the broker (new — broker.hpp addition); L.07 reuses the
  0.5.4 RBAC mock.
- **I.\*** (5) — all PG-gated; I.01–I.04 reuse existing LH
  fixtures; I.05 reuses the WS test client from
  `tests/integration/events_replay_integration_test.cpp` (added
  in 0.5.4 for I.01).
- New broker test seam: `broker::live_buffer_size_for_test(connection_id, channel) -> std::size_t` for L.04 / L.05. [Rejected in 0.6.0.N session 6 (2026-04-28): observability through the WsTestClient inbox + the resync frame from `handle_buffer_overflow` is sufficient; threading a `connection_id` API surface through `broker.hpp` for marginal observability gain violates `feedback_real_code_paths.md` thin-seam principle. Instead, L.04 / L.05 use the new `plinth::ws::test_seam::live_buffer_cap_override` accessor at `subscriptions.hpp` to drive overflow with a small cap (4) and assert via the resync frame.]
- New writer test seam: `events_writer::set_pre_broker_hook_for_test(...)` to inject between INSERT-stamp and broker call (S.01 / S.06 use this to assert envelope.seq is set before broker receives it).

### CI wiring

- `migrations/schema.sql` — UNCHANGED. `plinth.events.seq` is
  already BIGSERIAL.
- `src/kernel/realtime/events_writer.cpp` — add 2 lines (stamp +
  broker call); remove broker handler registration from broker
  startup.
- `src/kernel/realtime/broker.cpp` — remove listener-handler
  registration; export `publish_dispatched` as the new public
  entry point; add per-connection live-buffer + flush-after-replay
  hook; add `live_buffer_overflow` resync reason.
- `src/kernel/realtime/coalescer.cpp` — populate
  `coalesced_count` + `window_open_ts_ms` + `window_close_ts_ms`
  + optional `superseded_seqs[]` on emitted envelopes.
- `src/kernel/realtime/listener.cpp` — UNCHANGED (the change is
  in the consumer registration, not the listener itself).
- `src/kernel/realtime/replay.cpp` — UNCHANGED (replay query
  reads `plinth.events.seq`, which is the canonical source).
- `src/kernel/ws/subscriptions.cpp` — add `recommended_debounce_ms`
  + `recommended_jitter_ms` to subscribe_ack payload; add
  `debounce_renegotiate` frame parser (no-op + audit).
- `src/kernel/config.hpp` — add 4 fields per §10.
- `tests/kernel/realtime/seq_generation_test.cpp` — new TU.
- `tests/kernel/realtime/envelope_shape_test.cpp` — new TU.
- `tests/kernel/realtime/debounce_advisory_test.cpp` — new TU
  (covers D.\* and J.\*).
- `tests/kernel/realtime/live_replay_ordering_test.cpp` — new
  TU.
- `tests/integration/seq_debounce_integration_test.cpp` — new TU.

---

## §15 — Entry / Exit Criteria

### Entry criteria

- v0.5.4 shipped (tag `v0.5.4`). Confirmed.
- ICD-0.5.5 (this document) merged to `main` via paper-only PR.
- Architect has resolved OQ1–OQ7 in writing (PR review or
  follow-up commit on the implementation branch).

### Exit criteria

The 0.5.5 code milestone ships when all of the following are true:

1. **All 36 test cases pass.** S.\*, W.\*, D.\*, J.\*, L.\*, I.\*.
   Deferrals to a follow-up `0.5.5.1` are permitted only with
   architect sign-off (mirrors the 0.5.4 D.08/I.02/I.03 deferral
   pattern).

2. **Writer-first topology landed.** `register_handler` calls in
   broker.cpp removed; broker invocation in events_writer.cpp's
   `insert_envelope` added; full LH-1 + LH-2 + LH-3 (when
   available) suite green against the new topology.

3. **Envelope-seq invariant holds.** L.08 passes for every
   persisted dispatch in I.01–I.05 traces.

4. **Discharge pointers updated.**
   - ICD-0.5.0 §Payload Envelope Contract (line 249) reservation
     footnote becomes "0.5.5 populates" → "REQUIRED on persisted
     envelopes (ICD-0.5.5)".
   - ICD-0.5.1 §Out of scope monotonic-seq deferral (lines
     147–152) gets a discharged-by-0.5.5 footnote.
   - ICD-0.5.4 §Traces-to footnote (lines 30–32) updated to
     reflect envelope-seq == table-seq invariant.
   - ICD-0.5.4 §OQ5 resolved-row (line 1764) footnoted with
     "Discharged 0.5.5".
   - ICD-0.5.4 D.08 (line 1478) marked moved to ICD-0.5.5 L.03
     (or directly closed if absorption pattern is "ICD-0.5.5
     L.03 covers this scenario verbatim").

5. **Architecture promotion.** `architecture/03-data.md §3.4`
   items 1, 3, 5 gain an "Implemented 2026-04-XX (v0.5.5).
   Normative contract pinned in ICD-0.5.5" footnote (mirrors
   the 0.5.4 §3.5 promotion pattern).

6. **CHANGELOG entry.** `docs/CHANGELOG.md` gets a new dated
   entry above the v0.5.4 entry summarizing what shipped, the
   topology shift, the discharged reservations, and any
   documented pseudocode deviations.

7. **Memory updates.** `project_plinth_state.md` gains a
   v0.5.5 entry; `project_next_session_post_054.md` is replaced
   with `project_next_session_post_055.md` pointing at the next
   work (likely RE-EVAL following 0.5.5 per ROADMAP line 138 +
   any 0.5.5.1 deferrals).

### Non-exit criteria (NOT required for ship)

- 0.6.3 client SDK debounce implementation. Out of scope.
- Cross-PG-instance seq monotonicity (0.9.x).
- Optimistic local updates (architecture/03-data.md §3.4 item 2).
- Smart filtering (architecture/03-data.md §3.4 item 4).
- LH-3 reconnect-under-storm tier — gated on 0.5.4 per
  ROADMAP line 115; can ship alongside or after 0.5.5
  depending on architect priority.

---

## §16 — Open Questions

**OQ1 — Sequence-number generation strategy.** The ICD pins
**writer-first topology**: the writer's existing `INSERT … RETURNING
seq` is the canonical source; envelope-seq is stamped from that
RETURNING result; broker fan-out happens downstream of the writer
inside `insert_envelope`. Alternatives:
(a) `nextval` pre-allocation — listener calls `nextval` before
broker fan-out, writer INSERTs with explicit seq later;
(b) writer-first as pinned;
(c) writer-side independent counter — `std::atomic<int64_t>` per
process, writer stamps then INSERTs;
(d) broker-first with `seq_assigned` follow-up frame.
**Recommendation:** (b). Rationale: trivially satisfies the
canonical-seq invariant promised by ICD-0.5.4 §OQ5; collapses the
dispatch race to zero; reuses existing INSERT plumbing; D.08 falls
out for free. Latency cost (≤10 ms live-path round-trip) is
validated against LH-1 acceptance threshold (p99 < 50 ms). If LH-1
shows p99 > 50 ms, code session escalates back to (a) with explicit
gap-tolerance commitment in the SDK contract. Architect: confirm
or redirect.

**OQ2 — Per-channel monotonicity guarantee.** The ICD pins
**per-user strict monotonicity; per-channel monotonicity advisory
(strict within a single PG instance, loose across instances)**.
Alternatives:
(a) per-user strict only; per-channel undefined;
(b) per-user strict; per-channel advisory as pinned;
(c) per-channel strict across all instances (requires a global
seq authority or consensus protocol — out of scope).
**Recommendation:** (b). Rationale: matches the BIGSERIAL semantics
naturally; doesn't lock 0.9.x into a heavyweight cross-instance
consensus; gives single-instance deployments the cross-channel
ordering they expect. Architect: confirm.

**OQ3 — `coalesced_count` semantics.** The ICD pins
**`coalesced_count` reflects upstream coalescer hits in the 50 ms
window — the count of NOTIFY events the coalescer aggregated**.
Alternatives:
(a) coalesced_count reflects underlying SQL row counts (requires
per-row instrumentation in coalescer; out of scope);
(b) upstream NOTIFY hits as pinned;
(c) post-broker fan-out count (conflates coalescer with broker;
SDK confusion).
**Recommendation:** (b). Rationale: matches the SDK's actual need
("did multiple events collapse here?"); coalescer already knows
this number internally; minimal implementation effort. Architect:
confirm.

**OQ4 — `superseded_seqs[]` opt-in default.** The ICD pins
**OFF by default** (`coalesce.emit_superseded_seqs = false`).
Alternatives:
(a) ON by default (every coalesced envelope carries the array;
frame size cost on every Layer-1 envelope);
(b) OFF by default as pinned (operators enable per-deployment when
SDK requirements demand it);
(c) per-channel toggle (more granular but adds config complexity).
**Recommendation:** (b). Rationale: most SDKs can infer covered
seqs from `coalesced_count` alone; the array adds variable per-
envelope cost (4-byte BIGINT × N entries); operators who need it
opt in. Code session may revisit if 0.6.3 SDK design surfaces a
strong dependency. Architect: confirm.

**OQ5 — Advisory delivery mechanism.** The ICD pins **kernel
publishes `recommended_debounce_ms` + `recommended_jitter_ms` once
on `subscribe_ack`, not on every frame**. Alternatives:
(a) per-frame publish (advisory on every envelope; cost per frame);
(b) once on subscribe_ack as pinned (SDK caches per-subscription);
(c) periodic re-publish via a dedicated `advisory_update` frame
(handles config reload mid-session; complexity not justified).
**Recommendation:** (b). Rationale: advisory rarely changes; per-
subscription caching trivial; mid-session config reload is a known
limitation matching other 0.5.x patterns. Architect: confirm.

**OQ6 — Mid-replay live-frame buffering bound.** The ICD pins
**configurable cap (`live_buffer_cap_per_subscription`, default 256
frames). On overflow, force resync with reason
`live_buffer_overflow`**. Alternatives:
(a) unbounded buffer (memory hazard under slow-replay clients);
(b) configurable cap as pinned;
(c) hard-coded 256 (no operator control);
(d) drop-oldest within buffer (silent loss; client never knows it
missed events; ordering contract violation).
**Recommendation:** (b). Rationale: bounded; operator-tunable;
overflow case has a clean recovery via existing resync mechanism;
adds one resync reason to the existing 0.5.4 set. Architect:
confirm; pin actual default after LH-3 exercises the path.

**OQ7 — D.08 absorption.** The ICD pins **D.08 absorbed into
this ICD as L.03**; the deferred 0.5.4.1 follow-up reduces to
I.02 + I.03 only. Alternatives:
(a) D.08 stays in 0.5.4.1; ICD-0.5.5 covers a non-overlapping
ordering case (artificial split; tests overlap);
(b) D.08 absorbed as L.03 as pinned (becomes crisply testable
only once envelope-seq is canonical, which is exactly what this
ICD delivers);
(c) D.08 deferred further (delays the live/replay ordering
verification beyond 0.5.5; not recommended).
**Recommendation:** (b). Rationale: the deferred D.08 was deferred
in 0.5.4 specifically because it needed a "replay mid-flight
injection seam not pinned in the ICD" — this ICD provides that
seam (the `live_buffer` + `pre_broker_hook_for_test` mechanism)
naturally as part of the topology shift. Tests overlap because
the underlying scenario is the same. Architect: confirm.

---

## Appendix A — Race-Window State Diagram

The race-window analysis for the writer-first topology, walked
through step by step.

### Time-line events

```
t0: Listener thread receives PG NOTIFY for envelope E.
t1: Listener invokes writer's handler; writer enqueues E onto
    g_queue (bounded by write_queue_size=10000).
t2: Writer's drain timer fires (5 ms ceiling); pops E from queue.
t3: Writer's insert_envelope coroutine begins; opens DB transaction.
t4: Writer issues SELECT pg_try_advisory_xact_lock(channel, emitted_at).
    Path A: lock granted → continue. Path B: lock not granted →
    silent skip; broker NOT called; co_return.
t5 (Path A): Writer issues INSERT INTO plinth.events RETURNING seq.
t6 (Path A): RETURNING result yields SEQ.
t7 (Path A): Writer stamps ev.envelope["seq"] = SEQ.
t8 (Path A): Writer calls broker::publish_dispatched(ev). Broker
             fans to live subscribers using the stamped envelope.
t9 (Path A): Writer iterates ev.delivered_to_users; calls
             cursor_store::record_delivered(user_id, SEQ) for each.
t10 (Path A): Writer issues COMMIT.
```

### Pre-ICD-0.5.5 timeline (for contrast)

```
t0: Listener receives PG NOTIFY.
t1: Listener invokes broker handler FIRST (broker registered first
    per ICD-0.5.4); broker fans to live subscribers using envelope
    WITHOUT seq.
t2: Listener invokes writer handler; writer enqueues.
t3-t10 (writer): same as above, but envelope-seq never propagates
    to live subscribers (they already received the envelope at t1).
t11: Subscriber on REPLAY (a different subscriber reconnecting)
    queries plinth.events; sees the envelope WITH seq stamped from
    INSERT.
```

The pre-0.5.5 path produces an asymmetric envelope: live
subscribers see no seq, replay subscribers see seq. The 0.5.5 path
unifies: all subscribers see the same envelope including seq.

### Failure modes

**INSERT failure (PG transient error, schema migration in progress,
…):** Writer catches the exception, fires `write_failed reason=pg_error`
audit, never calls broker. Live subscribers see nothing for E.
Replay subscribers also see nothing (no row in plinth.events).
Cursor unchanged. The envelope effectively "did not happen" from
any subscriber's perspective.

**Advisory lock not granted (multi-node HA, lock held by a
different node):** Writer silent-skips at t4 Path B. Broker not
called; cursor unchanged on this node. The winning node's writer
processes the envelope and delivers to its connected subscribers.
Subscribers connected to the losing node see nothing live but
pick up via cursor/replay on next reconnect.

**Cursor advance failure (cursor_store::record_delivered raises):**
Cursor advance is best-effort per ICD-0.5.4 §When `record_delivered`
fires; failures audit `cursor_read_failed` but never raise to the
writer. Subscriber's next reconnect re-queries from the prior
cursor (will see this envelope on replay).

**Broker fan-out failure (per-subscriber WS write error, single
user disconnect):** Broker handles per-subscriber; one user's
disconnect doesn't affect others. Writer's call to broker is
fire-and-forget at the writer level; no impact on cursor advance
or subsequent envelopes.

### Concurrency invariants

- The advisory xact-lock at t4 is HELD across t5-t10. PG releases
  it only at t10's COMMIT (or on rollback if t5-t9 raises).
  Meaning: while one writer thread (or one node) is processing
  envelope E, no other writer thread can interleave another INSERT
  for the same `(channel, emitted_at)` key. This is the 0.5.4 §HA
  guarantee and remains intact.
- Broker's per-subscription state (the live-buffer-during-replay
  from §8) is per-(connection, subscription); concurrent
  subscriptions on the same connection have independent buffers;
  concurrent connections are independent.
- Cursor-store UPSERT uses `GREATEST(last_seq, EXCLUDED.last_seq)`
  per 0.5.4 — concurrent advances for the same user collapse to
  the maximum seq seen.

### What can still go wrong

- A bug in the writer-first restructure (e.g., forgetting to
  remove the old broker listener-handler registration) would
  produce DOUBLE delivery: broker fans both as a listener handler
  AND as a writer downstream call. The L.\* tests catch this:
  L.01's "100 envelopes, every seq monotonic" would observe
  duplicate seqs.
- A bug where the writer stamps envelope.seq with the wrong value
  (e.g., previous envelope's seq from a stale variable) would
  show up in S.01 as `envelope.seq != plinth.events.seq` and fire
  the `realtime.seq.replay_seq_mismatch` audit on next replay.
- An advisory-lock release leak (transaction abandoned without
  COMMIT/ROLLBACK) would block subsequent INSERTs for the same
  (channel, emitted_at) key. The 0.5.4 transaction-pattern from
  events_writer.cpp:250 (`co_await tx->execSqlCoro("COMMIT")`)
  guards against this.

---

## Appendix B — SDK Reference Debounce Algorithm (Informative)

This appendix is INFORMATIVE, not normative. It sketches the
algorithm the 0.6.3 client SDK is expected to implement based on
this ICD's wire contract. The SDK code itself is out of scope for
0.5.5; this sketch documents the kernel's expectation so the SDK
author has a target.

### State per subscription

```typescript
interface SubscriptionState {
  channel: string;
  recommendedDebounceMs: number;   // from subscribe_ack
  recommendedJitterMs: number;     // from subscribe_ack
  lastSeenSeq: bigint;             // from latest envelope.seq
  pendingTimer: TimerHandle | null;// for debounced re-query
}
```

### On envelope receipt

```typescript
function onEnvelope(env: Envelope, state: SubscriptionState) {
  // 1. Update last-seen seq.
  if (env.seq && env.seq > state.lastSeenSeq) {
    state.lastSeenSeq = env.seq;
  }

  // 2. Optionally apply optimistic local update (out of scope for
  //    this ICD; SDK item 2 from architecture/03-data.md §3.4).
  //    Layer-1 envelopes with `coalesced_count: 1` and `ops` may
  //    qualify; otherwise fall through to debounce.

  // 3. Schedule a debounced re-query.
  if (state.pendingTimer) {
    clearTimeout(state.pendingTimer);
  }
  const jitter = Math.random() * state.recommendedJitterMs;
  const delay = state.recommendedDebounceMs + jitter;
  state.pendingTimer = setTimeout(() => {
    requery(state);
    state.pendingTimer = null;
  }, delay);
}
```

### On replay

```typescript
function onReplayFrame(env: Envelope, state: SubscriptionState) {
  // Replay frames don't trigger debounce — apply directly.
  applyToLocalState(env);
  if (env.seq > state.lastSeenSeq) {
    state.lastSeenSeq = env.seq;
  }
}

function onReplayDone(frame: ReplayDone, state: SubscriptionState) {
  // After replay, request one final re-query to sync any state
  // the replay couldn't reconstruct from envelopes alone.
  requery(state);
}
```

### On reconnect

```typescript
function onReconnect(state: SubscriptionState) {
  // Send subscribe with last-seen seq to trigger replay.
  send({
    type: "subscribe",
    channels: [state.channel],
    since_seq: state.lastSeenSeq,
  });
}
```

### Renegotiation (rare)

```typescript
function renegotiateDebounce(state: SubscriptionState, newMs: number) {
  // Ignore the kernel's advisory; use newMs locally.
  // Send the renegotiate frame so operators can audit.
  send({
    type: "debounce_renegotiate",
    channel: state.channel,
    debounce_ms: newMs,
  });
  state.recommendedDebounceMs = newMs;  // SDK-local override
}
```

### Notes for the SDK author

- The kernel does not enforce; clients that ignore the advisory
  simply trigger more re-queries. Operators see the audit and can
  alarm.
- `lastSeenSeq` is BIGINT — JavaScript's `bigint` type required for
  values approaching 2^53.
- Replay frames are applied DIRECTLY (no debounce). The reasoning:
  the SDK is reconstructing missed state; debouncing during replay
  delays state reconstruction without saving any work.
- The single `requery` after `replay_done` is for state the
  envelopes alone can't reconstruct (e.g., aggregate counts,
  joins). SDK may skip this for read-models that are fully
  envelope-driven.

---

## Appendix C — D.08 Redux Timeline

ICD-0.5.4 D.08 (line 1478): "Reconnect with `since_seq:100`;
mid-replay an event emits live → Replay frames in seq order, then
`replay_done`, then live frame (queued during replay)."

This appendix walks the test scenario step by step under the
0.5.5 writer-first topology, demonstrating the ordering invariant
holds.

### Setup

- User A is subscribed to channel `plinth:data:ext_notes.notes`.
- A's last-seen seq before disconnect: 100.
- `plinth.events` contains rows with seqs 50–150 for that channel.
- A reconnects, sends `subscribe { channels: ["..."], since_seq: 100 }`.

### Timeline

```
t0: Subscribe handler receives the frame.
t1: Subscribe handler validates RBAC; user A has permission.
    Registers live subscription for channel; sets
    replay_in_flight = true on the subscription.
t2: Subscribe handler dispatches to replay::run_replay(user_id=A,
    channel=..., since_seq=100).
t3: Replay engine queries:
    SELECT seq, payload FROM plinth.events
    WHERE seq > 100 AND channel = $1 ORDER BY seq ASC LIMIT 500;
    Returns rows for seqs 101–150 (50 rows).
t4: Replay engine begins emitting `replay` frames in seq order:
    101, 102, 103, …
    Each frame includes envelope with envelope.seq = (table seq).
t5: At seq 130, an extension `ext_notes` calls
    db.exec("INSERT INTO ext_notes.notes ...").
t6: Coalescer's 50ms window opens; window collects this single
    insert.
t7: Window closes; coalescer assembles envelope with
    coalesced_count=1 (or higher if other inserts piled in);
    coalescer calls emit_notify_async.
t8: PG NOTIFY fires; listener picks it up.
t9: Listener invokes writer's handler; writer enqueues envelope.
t10: Writer's drain timer fires; insert_envelope coroutine begins.
t11: Writer's INSERT into plinth.events RETURNING seq → seq=151.
t12: Writer stamps ev.envelope["seq"] = 151.
t13: Writer calls broker::publish_dispatched(ev).
t14: Broker's per-subscription dispatch logic checks: A's
     subscription has replay_in_flight=true. Frame buffered into
     A's live_buffer (currently empty, so buffer = [seq=151]).
t15: Cursor advance: cursor_store::record_delivered(A, 151) runs
     fire-and-forget. (Note: this advances A's cursor to 151
     even though A hasn't received the live frame yet. This is
     fine — when A reconnects again later, replay starts from
     seq 152, never re-sending seq 151.)

[Replay engine continues emitting seqs 131-150 as frames…]

t16: Replay engine completes all 50 rows.
t17: Replay engine emits `replay_done { up_to_seq: 150,
     row_count: 50, buffered_live_count: 1 }`.
t18: Replay engine sets replay_in_flight = false on A's
     subscription.
t19: Broker's flush logic detects replay_in_flight=false and
     buffer is non-empty. Flushes buffer in seq ascending order.
t20: A receives the live frame for seq 151.
```

### Invariants observed

- A receives 50 replay frames (seqs 101–150) in seq order. ✓
- A receives `replay_done { up_to_seq: 150 }` after the last
  replay frame. ✓
- A receives the live frame (seq 151) AFTER `replay_done`. ✓
- 151 > 150 (last replay seq < first live seq). ✓
- Cursor advances correctly to 151. ✓
- No duplicate frames. No missed frames. ✓

### What if the replay is large and many live frames arrive?

Suppose the replay takes 5 seconds (10 chunks of 500 each, with a
slow PG) and during that time 200 live envelopes arrive. With
`live_buffer_cap_per_subscription = 256`, all 200 fit. Buffer
flushes in seq order after `replay_done`. A receives 5000 replay
frames, then 200 live frames in order. ✓

If 300 live envelopes arrive: at 257, buffer overflows. Broker
discards buffer, sets `replay_in_flight=false`, aborts the
in-flight replay (next chunk boundary detects abort flag), emits
`resync { reason: "live_buffer_overflow" }`. A receives partial
replay then `resync`, knows to re-query everything. Cursor
advances safely (per §8). No invariant violation. ✓

### What if the user disconnects mid-replay?

Subscribe handler's connection-teardown cleanup tears down A's
subscription, including discarding A's live buffer. The replay
coroutine's next chunk boundary detects the connection is gone
and returns early. No frames are sent to A; A's cursor stays at
whatever its last advance was (cursor advances are fire-and-forget
per ICD-0.5.4 — they may have advanced beyond what A actually
received, which is the accepted trade-off). A reconnects later,
replay begins from the cursor-advanced point. ✓

---

## §17 — OQ Resolutions (Code Session, Phase 1)

Pinned 2026-04-25 on `feat/0.5.5-sequence-numbers-client-debounce`
Phase 1. All seven OQs accepted on the §16 architect recommendation;
no redirects. Recorded here so subsequent phases land against a
fixed contract and the post-merge ICD reflects the as-built state.

| OQ | Pin | Lands as |
|----|-----|----------|
| OQ1 | (b) writer-first topology | `events.seq.source = "writer_returning"` (single-value enum, loader rejects others); LH-1 storm pre-flight at end of Phase 2 gates the topology shift on observed p99 < 50 ms |
| OQ2 | (b) per-user strict; per-channel advisory | Stated invariant; no config knob (per-PG-instance BIGSERIAL semantics) |
| OQ3 | (b) `coalesced_count` = upstream NOTIFY hits | Phase 3 wires the count from the coalescer's existing internal state |
| OQ4 | (b) `superseded_seqs[]` OFF by default | `events.coalesce.emit_superseded_seqs = false`; Phase 3 emits gated |
| OQ5 | (b) advisory once on `subscribe_ack`, no per-frame | Phase 4 only extends `make_ack`; `debounce_renegotiate` parser is a no-op + audit |
| OQ6 | (b) `live_buffer_cap_per_subscription` default 256, range [16, 65536] | `events.live_buffer_cap_per_subscription = 256`; Phase 5 reads it |
| OQ7 | (b) D.08 absorbed as L.03 | Phase 5's `live_replay_ordering_test.cpp` carries the test; 0.5.4.1 reduces to I.02 + I.03 |

End of ICD-0.5.5.
