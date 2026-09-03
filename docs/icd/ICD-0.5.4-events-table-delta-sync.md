# ICD-0.5.4-events-table-delta-sync

**Traces to:** architecture/03-data.md §3.5 (Delta Sync on Reconnect —
the normative prose this ICD promotes to contract: "Client sends its
last-seen sequence number. Server queries `plinth.events` for events
since that sequence. Server sends missed events as a batch. Client
processes them through the normal event pipeline."; retention
"configurable, default 1 hour"; resync signal when client cursor is
older than retention); architecture/03-data.md §Appendix B (PG Schema
Layout — `plinth.events` table shape with `seq BIGSERIAL PRIMARY KEY`
+ `channel TEXT NOT NULL` + `payload JSONB NOT NULL` + `created_at
TIMESTAMPTZ NOT NULL DEFAULT NOW()` — pinned at 0.5.0 reservation,
implemented this milestone); architecture/03-data.md §3.6 (HA
Realtime — `plinth.events` is shared PG state; any node can serve
any client's replay because PG is the event bus, no per-node sharding
needed); architecture/03-data.md §3.6.1 (Physical Channel Fan-In —
the listener's `plinth:realtime` PG channel; `plinth.events` writer
is the second `EventHandler` per ICD-0.5.0 §Listener Subsystem,
broker is the first); architecture/04-services-ha.md §2 (Scheduled
Tasks — `plinth.events_cleanup` named as a default kernel task; this
ICD pins its schedule + sweep query); architecture/02-capabilities.md
§3.1 (Async Dispatch Arm — `plinth.events` writer rides the existing
listener thread, no new async-bridge surface required; replay query
rides `db.query` in the kernel-scope bc); ICD-0.5.0-pg-listen-notify-bridge
§Listener Subsystem (the `EventHandler` registration seam this ICD's
events writer registers against; channel regex + envelope contract
the writer reads verbatim); ICD-0.5.0 §`emit_notify_async`
(unchanged — events writer is downstream of emission, not a peer);
ICD-0.5.0 §Payload Envelope Contract (`emitted_at` reserved slot
this ICD populates per write; `seq` reserved slot stays empty in
0.5.4 envelopes — **discharged 2026-04-26 (v0.5.5)**: writer stamps
envelope `seq` from `INSERT … RETURNING seq` so envelope-seq ==
table-seq invariant holds on both live and replay paths);
ICD-0.5.1-pg-auto-event-coalescer §Envelope Assembly (the envelope
shape persisted verbatim — `layer`, `channel`, `schema`, `table`,
`ops[]`, `window_ms`, optional `truncated`); ICD-0.5.1 §Truncation
Heuristic (an envelope flagged `truncated:true` is persisted as-is;
clients seeing `truncated:true` on replay re-query per the 0.5.1
contract — replay does not "fix" truncation); ICD-0.5.2-ws-broker
§Reconnect Semantics ("Hand-off to 0.5.4. When 0.5.4 lands, durable
subscriptions attach to `plinth.events` via a per-user cursor; the
broker's 0.5.2 per-connection registry stays as the hot path + adds
a cursor-backed replay path on reconnect. No 0.5.2 contract breaks."
— this ICD is that hand-off implementation); ICD-0.5.2 §Subscribe
RBAC (per-channel rule check at subscribe; replay re-checks the same
rule per envelope on delivery — defense-in-depth, mirrors §Security
Constraint 2 of 0.5.2); ICD-0.5.2 §Audit Events (rate-limited
aggregation pattern this ICD's audits inherit verbatim); ICD-0.5.3
§Per-Op `SET search_path` Isolation (the `plinth` kernel schema is
always reachable from any extension-scope bc per ICD-0.5.3 §Security
Constraint 1's `SET LOCAL search_path TO ext_<id>, plinth;` — the
events writer runs in kernel scope so the wrapper does not apply, but
extensions querying `plinth.events` via `db.query` would, and this
ICD explicitly pins that extensions DO NOT query `plinth.events`
directly); ICD-0.1.6-websocket §Delta Sync on Reconnect (the 0.1.6
stub "In 0.1.6, reconnecting clients must re-subscribe to channels
and perform a full data re-query. The server does not track sequence
numbers or missed events. Full delta sync deferred to 0.5.x." — this
ICD is that 0.5.x discharge); ICD-0.1.6 §`/ws` Frame Types (the new
`{type:"resync"}` and `{type:"replay"}` and `{type:"replay_done"}`
frames join the existing 0.1.6 frame catalogue); ICD-0.1.6 §Auth
(the WS connection is already authenticated per session token at
connect time; this ICD's per-user cursor reads `bc.user_id` from the
connected session, never from a client-supplied field); ICD-0.1.7-audit
§Audit Writer (the new rate-limited `events.*` audits ride existing
infrastructure); ICD-0.4.3-extension-schema-creation-and-migration
§Schema layout (`plinth` is the kernel-owned schema; only kernel code
INSERTs into `plinth.events`); ICD-0.4.4-package-install-lifecycle
(extension lifecycle hooks unchanged — events writer is a kernel
subsystem with no per-extension state to drain).

**Depends on:** ICD-0.5.0 (listener subsystem this ICD's writer
registers against; envelope contract this ICD persists verbatim;
channel regex this ICD's replay query filter reuses); ICD-0.5.1
(coalescer envelope shape — the `window_ms` field appears verbatim
in persisted rows; the `emitted_at` reservation this ICD populates;
`truncated` semantics on replay); ICD-0.5.2 (broker as the hot-path
fan-out; reconnect hand-off ratified at line 678–681 of that ICD;
per-channel RBAC rule the replay re-checks); ICD-0.5.3 (no direct
dependency — `plinth.events` writes are kernel-scope, the per-op
`SET search_path` wrapper does not apply; mentioned only because
0.5.3 made the search-path posture authoritative); ICD-0.1.6 (WS
frame catalogue + auth gate this ICD extends); ICD-0.1.7 (audit
writer infrastructure); ICD-0.4.3 (the `plinth` schema's prior art
— `plinth.events` reuses the kernel-owned namespace).

**Milestone:** 0.5.4 — `plinth.events` table + delta sync on
reconnect. Fifth 0.5.x code milestone (after v0.5.0 bridge + v0.5.1
coalescer + v0.5.2 broker + v0.5.3 batch). The piece that closes the
ICD-0.1.6 §Delta Sync stub deferred to 0.5.x and the ICD-0.5.2
§Reconnect Semantics hand-off pointer to 0.5.4. Paired paper ICD
authoring slot `0.5.3.N` precedes this code work per METHODOLOGY §3.1
forward-ICD-presence rule and `feedback_icd_horizon.md`.

**Status:** Implemented 2026-04-25 (tag `v0.5.4`). 38 of 41 ICD test
cases shipped; D.08 + I.02 + I.03 deferred to 0.5.4.1. Three
documented pseudocode deviations: (1) cleanup uses
`pg_try_advisory_xact_lock` (xact-scoped) rather than the
session-scoped `pg_try_advisory_lock` shown in §Cleanup pseudocode —
required by Drogon's connection pool semantics; (2) cleanup lock key
is embedded as a SQL literal rather than a bound BIGINT param —
Drogon's SqlBinder sends BIGINT in a binary format PG rejects on
this query shape; (3) `since_seq=0` is treated as "no prior cursor"
and skips the `cursor_expired` precondition (matches §New-user
behaviour). See `docs/CHANGELOG.md` 2026-04-25 v0.5.4 entry for
full deviation rationale.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
[src/kernel/realtime/listener.hpp](../../src/kernel/realtime/listener.hpp) +
[src/kernel/realtime/listener.cpp](../../src/kernel/realtime/listener.cpp)
(the `EventHandler` registration seam — new
`plinth::realtime::events_writer::register_handler()` joins the
broker's existing registration in the listener startup path; per
ICD-0.5.0 §Listener Subsystem the listener dispatches to every
registered handler in registration order);
[src/kernel/realtime/events_writer.hpp](../../src/kernel/realtime/events_writer.hpp) +
[src/kernel/realtime/events_writer.cpp](../../src/kernel/realtime/events_writer.cpp)
(NEW TU — the `EventsWriter` subsystem this ICD pins; owns the
events-writer EventHandler, the dedicated `trantor::EventLoopThread`
draining a write queue, and the atexit barrier; mirrors
`src/kernel/realtime/coalescer.{hpp,cpp}` shape for the dedicated-
loop pattern);
[src/kernel/realtime/replay.hpp](../../src/kernel/realtime/replay.hpp) +
[src/kernel/realtime/replay.cpp](../../src/kernel/realtime/replay.cpp)
(NEW TU — the replay query + streaming engine; lives separately from
the writer because reads happen on the WS conn's loop, writes on
the writer's dedicated loop);
[src/kernel/realtime/cursor_store.hpp](../../src/kernel/realtime/cursor_store.hpp) +
[src/kernel/realtime/cursor_store.cpp](../../src/kernel/realtime/cursor_store.cpp)
(NEW TU — the per-user cursor read/write API over the new
`plinth.user_event_cursors` table; called from the WS broker on
delivery success and from `replay.cpp` on replay completion);
[src/kernel/realtime/broker.hpp](../../src/kernel/realtime/broker.hpp) +
[src/kernel/realtime/broker.cpp](../../src/kernel/realtime/broker.cpp)
(extended — broker delivery callback now calls
`cursor_store::record_delivered(user_id, seq)` after every successful
WS frame send; broker's `subscribe` handler now triggers a replay
query if the subscribe frame carries a `since_seq`);
[src/kernel/ws/frame_handler.hpp](../../src/kernel/ws/frame_handler.hpp) +
[src/kernel/ws/frame_handler.cpp](../../src/kernel/ws/frame_handler.cpp)
(extended — new client-frame parser branches for
`{type:"subscribe", since_seq:...}` and the bare `{type:"ping"}`-
style request-replay frame; new server-frame emitters for
`{type:"resync"}` and `{type:"replay"}` and `{type:"replay_done"}`);
[src/kernel/main.cpp](../../src/kernel/main.cpp) (atexit chain — new
`realtime::stop_events_writer()` between `realtime::stop_listener()`
and `js::rollback_all_batches()`; startup order — new
`realtime::start_events_writer(cfg.realtime.events)` after
`realtime::start_listener()` so the listener is already accepting
NOTIFYs before the writer registers as a handler);
[src/kernel/scheduled_tasks/cleanup_events.hpp](../../src/kernel/scheduled_tasks/cleanup_events.hpp) +
[src/kernel/scheduled_tasks/cleanup_events.cpp](../../src/kernel/scheduled_tasks/cleanup_events.cpp)
(NEW TU — the `plinth.events_cleanup` default kernel task per
architecture/04-services-ha.md §2; runs on a fixed interval, deletes
rows older than `realtime.events.retention_seconds`);
[migrations/schema.sql](../../migrations/schema.sql) (the realtime
section gains the `plinth.events` table DDL + the new
`plinth.user_event_cursors` table DDL + indexes; per
architecture/03-data.md §Appendix B `plinth.events` was reserved at
0.5.0 with zero-table commit and is implemented this milestone;
schema-freeze does NOT apply pre-0.7 per ROADMAP — schema.sql edits
allowed);
[docs/architecture/03-data.md §3.5 + §Appendix B](../architecture/03-data.md)
(normative prose promoted to contract);
[docs/icd/ICD-0.1.6-websocket.md §Delta Sync on Reconnect](ICD-0.1.6-websocket.md)
(the 0.1.6 stub this ICD discharges — pointer note to be added at
ship time);
[docs/icd/ICD-0.5.2-ws-broker.md §Reconnect Semantics](ICD-0.5.2-ws-broker.md)
(the 0.5.4 hand-off footnote at lines 678–681 — pointer this ICD
fulfills).

---

## Overview

0.5.4 lands three related contributions that together close the
ICD-0.1.6 §Delta Sync on Reconnect stub and the ICD-0.5.2 §Reconnect
Semantics 0.5.4 hand-off:

1. **`plinth.events` persistence writer.** A new in-kernel
   `EventHandler` registers against the 0.5.0 listener and INSERTs
   every dispatched envelope into the `plinth.events` table.
   Writes ride a dedicated `trantor::EventLoopThread` with a bounded
   in-memory queue so the listener thread is never blocked on PG IO.
   The table shape is the one already pinned at
   `architecture/03-data.md §Appendix B`: `seq BIGSERIAL PRIMARY KEY,
   channel TEXT NOT NULL, payload JSONB NOT NULL, created_at
   TIMESTAMPTZ NOT NULL DEFAULT NOW()`. The writer populates the
   envelope's reserved `emitted_at` field at insert time so replay
   consumers see a consistent server-side timestamp regardless of
   when the original NOTIFY landed.

2. **Per-user cursor + reconnect handshake.** A new
   `plinth.user_event_cursors` table stores `(user_id, last_seq)`
   per authenticated user. On every successful WS frame delivery to
   a user, the broker advances `last_seq` to the envelope's
   `plinth.events.seq` value. On WS reconnect, the client's
   `{type:"subscribe", channels:[...], since_seq:N}` frame triggers
   a replay query: every row in `plinth.events` with `seq > N` whose
   `channel` matches one of the requested channels (and passes the
   per-channel RBAC re-check) is streamed back as `{type:"replay",
   envelope:...}` frames, terminated by `{type:"replay_done", up_to_seq:M}`.
   If `N < min(seq)` (i.e. the cursor is older than the retention
   window), the server sends `{type:"resync", reason:"cursor_expired",
   retention_seconds:R}` and the client re-queries via `db.query`
   per the existing 0.1.6 + 0.6.3 SDK contract.

3. **`plinth.events_cleanup` default kernel task.** Per
   `architecture/04-services-ha.md §2` the events table needs a
   trim sweep. 0.5.4 wires the named task: `DELETE FROM plinth.events
   WHERE created_at < NOW() - INTERVAL '<retention>'` runs every
   `realtime.events.cleanup_interval_ms` (default 5 minutes).
   Cleanup uses the scheduled-tasks PG-advisory-lock pattern from
   `architecture/04-services-ha.md §2` so only one node runs a sweep
   at a time, even though `plinth.events` is shared.

All three compose on existing infrastructure. The listener (0.5.0)
gains one new registered handler; the broker (0.5.2) gains a
delivery-completed hook + a subscribe-frame branch; the WS
controller (0.1.6) gains three new server-frame types and one new
field on the existing client subscribe frame; no new JS binding
lands; no new capability registers. The listener's dispatch path
(0.5.0 §Listener Subsystem) sees no contract change — handlers run
in registration order on the listener thread, the writer is just
one more handler, and a slow writer cannot block the broker because
the writer enqueues to its own loop and returns immediately.

**Scope:**

1. `plinth.events` table DDL in `migrations/schema.sql` (the
   reserved row at the realtime section).
2. `plinth.user_event_cursors` table DDL in the same migration
   block — `(user_id UUID PRIMARY KEY REFERENCES plinth.users(id)
   ON DELETE CASCADE, last_seq BIGINT NOT NULL DEFAULT 0,
   updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())`.
3. `plinth::realtime::events_writer` subsystem in
   `src/kernel/realtime/events_writer.{hpp,cpp}` — owns the
   dedicated `EventLoopThread`, the bounded write queue, the
   listener-registered `EventHandler`, and the atexit barrier. Mirrors
   the `CoalescerRegistry` shape from 0.5.1.
4. `plinth::realtime::cursor_store` API in
   `src/kernel/realtime/cursor_store.{hpp,cpp}` — `record_delivered`,
   `read_cursor`, `reset_cursor`. Reads / writes the
   `plinth.user_event_cursors` table via the standard Drogon
   DbClient.
5. `plinth::realtime::replay` engine in
   `src/kernel/realtime/replay.{hpp,cpp}` — given a `(user_id,
   channels, since_seq)` triple, paginates a replay query and
   streams it through a caller-provided emit callback. Reuses the
   per-channel RBAC rule resolver from `broker.cpp`.
6. WS broker extension in `broker.cpp` — the `subscribe` handler
   branches on `since_seq` presence; the delivery callback advances
   the cursor.
7. WS frame parser / emitter in `frame_handler.{hpp,cpp}` — accepts
   `since_seq:N` on `{type:"subscribe"}`; emits `{type:"resync"}`,
   `{type:"replay"}`, `{type:"replay_done"}`.
8. `realtime.events.*` config block — `enabled`, `retention_seconds`,
   `cleanup_interval_ms`, `replay_max_rows_per_chunk`,
   `replay_max_total_rows`, `write_queue_size`, `audit_window_ms`.
9. Six new rate-limited audit events — `realtime.events.write_failed`,
   `realtime.events.replay_started`, `realtime.events.replay_completed`,
   `realtime.events.replay_truncated`, `realtime.events.resync_required`,
   `realtime.events.cleanup_swept`.
10. `plinth.events_cleanup` scheduled task in
    `src/kernel/scheduled_tasks/cleanup_events.{hpp,cpp}` —
    advisory-lock-gated DELETE sweep on the configured interval.
11. Atexit chain extension — `realtime::stop_events_writer()`
    between `stop_listener()` and `js::rollback_all_batches()`.
12. Extension-lifecycle integration — none. Events writer is
    kernel-scope; the `plinth.user_event_cursors` cascade-deletes
    on user delete (PG `ON DELETE CASCADE`); no per-extension drain.

**Out of scope (explicit):**

- **Monotonic envelope `seq` generation.** ROADMAP L136, 0.5.5
  scope. The envelope's `seq` field (reserved at ICD-0.5.0 §Payload
  Envelope) stays empty in 0.5.4 envelopes. The cursor uses
  `plinth.events.seq` (the BIGSERIAL primary key, assigned by PG at
  insert time) — NOT the envelope-internal `seq`. Clients
  reconnecting send `since_seq` matching the last `plinth.events.seq`
  they observed; the server-side replay query filters on
  `plinth.events.seq > since_seq`. Once 0.5.5 lands envelope `seq`,
  the cursor migrates trivially (the envelope's `seq` will equal the
  table's `seq` by construction). §OQ5 ratifies.
- **Cross-extension cursor isolation.** The cursor is per-user, not
  per-(user, extension). A user subscribing to `plinth:data:ext_a.t`
  + `plinth:data:ext_b.t` has ONE cursor; on reconnect the replay
  query selects rows whose channel matches EITHER (intersected with
  the requested-channels set). §OQ8 ratifies.
- **Per-channel cursor granularity.** A user's cursor is one BIGINT.
  No `(user_id, channel) → last_seq` mapping. The replay query
  resolves "missed events on channels currently subscribed" by
  filtering the row's `channel` against the subscribe frame's
  `channels[]` array; rows on channels the user did not request stay
  untouched in the table (they're for other users / sessions). The
  cursor advances on EVERY delivery, so a user that subscribes to
  channel A then later channel B will see B's events from
  `cursor_at_subscribe`, not from before — matching the 0.5.2
  per-connection model semantics.
- **Replay deduplication.** If the broker delivered an envelope and
  then the connection dropped before the cursor advanced (rare; the
  cursor advance is a fire-and-forget UPDATE — see §OQ4), the same
  envelope MAY be redelivered on reconnect. Clients tolerate
  duplicates by treating the envelope's `(channel, schema, table)`
  + `payload` as idempotent (matches the 0.5.1 coalescer's
  exactly-once-per-window semantics). §Security Constraint 6.
- **Streaming cursor / open-ended replay.** Replay is bounded —
  `replay_max_total_rows` (default 10000) hard-caps the response.
  If exceeded, the server emits `{type:"replay_truncated",
  up_to_seq:M, reason:"row_cap"}` + signals resync. The reasoning is
  that a user offline long enough to accumulate >10K events should
  re-query rather than process a backlog (it's faster, and matches
  the 0.1.6 §Delta Sync resync posture).
- **Backpressure / flow control.** The replay engine paginates in
  fixed `replay_max_rows_per_chunk` (default 500) batches; between
  chunks it yields to the WS conn's loop so other frames (ping,
  publish) interleave. There is no explicit ack-based pause/resume
  — the WS sendFrame queue absorbs bursts up to Drogon's per-conn
  buffer; if Drogon backpressures, replay slows naturally on the
  emit callback's await.
- **Per-event payload diff against last cursor.** Replay sends each
  envelope as it landed in `plinth.events`, not a delta. The
  envelope ALREADY carries counts-only deltas (the 0.5.1 ops[]
  shape) — clients re-query when they need rows. 0.5.5's monotonic
  seq enables strict ordering; 0.5.4's replay only guarantees
  same-as-original ordering by `plinth.events.seq` ASC.
- **Cursor encryption / opacity.** The cursor is a BIGINT exposed to
  the client verbatim. Forging another user's cursor would let a
  malicious client enumerate write rates, but never receive other
  users' events — every replayed envelope passes the per-channel
  RBAC rule check against the SESSION's `user_id` (which is bound
  at WS auth, not client-controlled). §Security Constraint 2.
- **Client-side persistence of cursor.** The CLIENT stores the last
  `seq` it observed across page reloads; that's a 0.6.3 SDK
  responsibility. 0.5.4 ships a server-side cursor (for sanity-
  checking against client claims and as a defense-in-depth reset
  point), but the canonical cursor for replay-query purposes is the
  client's claim on reconnect — the server never tells the client
  "you should resume from N." The server-side cursor exists to
  detect "client claims since_seq=100, server-side cursor says
  last_delivered=80" mismatches (see §Replay query) and audit them.
- **Layer 2 / Layer 3 replay restrictions.** All three layers
  persist + replay symmetrically. A `plinth:system:packages.installed`
  envelope is in `plinth.events` just like a `plinth:data:ext_notes.notes`
  envelope, and a client subscribed to `plinth:system:packages.*` on
  reconnect gets the missed system events too. RBAC rules already
  gate which channels a user can subscribe to (ICD-0.5.2 §Subscribe
  RBAC); replay re-checks the same rule.
- **Broker drain integration.** Extension UNINSTALL drains broker
  subscriptions per ICD-0.5.2 §Extension Lifecycle Integration. The
  drained subs are NOT restored on extension re-enable per ICD-0.5.2
  §Reconnect posture. 0.5.4 does not relax this — the user's
  cursor advances during the drain (cursor reflects "what was
  delivered before drain"); on re-enable, fresh subscribes start at
  the post-drain cursor. No cross-state leak.

---

## Implementation deviation (v0.5.4 ship + 0.5.5.1 follow-up)

Recorded per METHODOLOGY §Phase 2 Constraint #4 + ratified at
`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md §4.1` and §4.2.

1. **Cleanup uses `pg_try_advisory_xact_lock` (xact-scoped), not
   session-scoped `pg_try_advisory_lock`** as shown in §Cleanup
   pseudocode. Drogon's connection pool returns a connection to the
   pool after `co_await execSqlCoro` completes, which would drop a
   session-scoped lock; the xact-scoped variant releases at COMMIT
   instead and cooperates with Drogon's `newTransactionCoro` semantics.
   The writer's per-envelope advisory lock uses the same pattern;
   symmetry is correct.
2. **Cleanup advisory-lock key embedded as a SQL literal**, not as a
   bound BIGINT parameter. Drogon's `SqlBinder` sends BIGINT in a
   binary format that PG rejects on `pg_try_advisory_xact_lock` with
   "incorrect binary data format in bind parameter". The key is a
   compile-time constant; no SQL-injection surface.
3. **`since_seq=0` is treated as "no prior cursor"**, skipping the
   `cursor_expired` precondition. §Cursor-expired check pseudocode
   pins `since_seq < MIN(seq)`, which would technically trigger on a
   fresh client because `MIN(seq) >= 1` after any insert. The shipped
   behavior matches §New-user behaviour text but is not visible from
   the §Cursor-expired pseudocode alone. Reconnect with non-zero
   `since_seq < MIN(seq)` still triggers `cursor_expired`.
4. **`replay::run_replay` parameter type changed in 0.5.5.1** from
   `ws::ConnState state` (value) to `const ws::ConnState&` (const
   reference) because `ConnState` became non-copyable after the
   0.5.5.1 `mutable std::unique_ptr<std::mutex> channels_mu` field
   addition (see CHANGELOG 0.5.5.1 §What shipped 2). The `state_copy`
   move-into-lambda pattern in `subscriptions.cpp` still moves the
   `unique_ptr<std::mutex>`; `run_replay` receives a const reference.
   The §Replay Engine prose pseudocode is signature-agnostic — no
   text drift, but readers grepping for the C++ signature should land
   on [`src/kernel/realtime/replay.hpp:71`](../../src/kernel/realtime/replay.hpp).

---

## `plinth.events` Persistence Writer

### Subsystem shape

The writer mirrors `CoalescerRegistry`'s structure (ICD-0.5.1 §Coalescer
State Machine): a singleton `plinth::realtime::EventsWriter` owning a
dedicated `trantor::EventLoopThread`, an in-memory bounded write queue,
an atexit barrier, and a single registered `EventHandler` callback.

```cpp
// src/kernel/realtime/events_writer.hpp
namespace plinth::realtime::events_writer {

// Spawn the writer's loop, register the EventHandler against the
// listener, open the DbClient connection cache. Idempotent — second
// call while running is a no-op.
auto start(const Config::Realtime::Events& cfg) -> void;

// Synchronous barrier — drain the queue (bounded by
// realtime.events.shutdown_drain_ms; default 5000 ms), join the
// loop's thread, deregister from the listener (no-op since 0.5.0
// listener handler registration is "add only"; the writer's handler
// becomes a no-op-on-stopped-state). MUST be called between
// stop_listener() and js::rollback_all_batches() in the atexit
// chain.
auto stop() -> void;

// Test seam — directly enqueue a DispatchedEvent as if the listener
// had just dispatched it. Bypasses the listener; the queue + write
// path runs as in production.
auto enqueue_for_test(const DispatchedEvent& ev) -> void;

}  // namespace plinth::realtime::events_writer
```

### Why a dedicated loop

The listener thread (ICD-0.5.0 §Threading & reconnect) runs every
registered handler in registration order on its own thread. A handler
that blocks on PG IO would stall the listener — including its
ability to receive the next NOTIFY. ICD-0.5.0 §Listener Subsystem
states "Handlers run on the listener thread — they MUST be fast
(non-blocking)."

The writer satisfies this by enqueuing the dispatched envelope onto
its dedicated loop and returning immediately. The dedicated loop
drains the queue against a Drogon DbClient connection — slow PG
writes block the writer's loop only, never the listener. Mirrors
exactly what `CoalescerRegistry` does for window-flush emission
(ICD-0.5.1 §Coalescer Loop).

### Write queue

The queue is a `std::deque<QueueEntry>` protected by a `std::mutex`
and a `std::condition_variable`:

```cpp
struct QueueEntry {
    std::string channel;       // verbatim from DispatchedEvent
    Json::Value envelope;      // full parsed payload
    std::chrono::system_clock::time_point received_at;
                                // for emitted_at population +
                                // queue-age telemetry
};
```

Bounded by `realtime.events.write_queue_size` (default 10000). On
overflow:

1. **Drop newest write.** The just-enqueued envelope is dropped; the
   queue keeps its older entries. Rationale: under an emission
   storm, dropping the most-recent N events still serves replay for
   pre-storm-period reconnects. Dropping the oldest would lose the
   work that's been queued longest.
2. **Audit `realtime.events.write_failed`** with reason `queue_full`,
   the dropped envelope's channel, and a queue-depth snapshot. Rate-
   limited per the standard pattern.
3. **No retry.** PG NOTIFY semantics are best-effort (ICD-0.5.0
   §Error Model); 0.5.4 events persistence is best-effort to the
   same standard. A dropped row means the missed window for any
   reconnecting client crossing that drop becomes a `resync_required`
   case — which is the correct fallback.

§OQ1 ratifies the drop-newest policy.

### Write path

The drain loop pops a `QueueEntry`, populates `emitted_at`
server-side (the envelope may or may not have it; 0.5.4 makes it
authoritative — see §Envelope `emitted_at` population), and
INSERTs:

```sql
INSERT INTO plinth.events (channel, payload)
VALUES ($1, $2)
RETURNING seq;
```

`created_at` defaults to `NOW()` PG-side. `seq` returns to the
caller for telemetry purposes only — the listener has no consumer
of the seq today; 0.5.5 may wire it back into envelope `seq`
generation, at which point the writer becomes the source-of-truth
for monotonic seq (§OQ5 ratifies).

### Envelope `emitted_at` population

The 0.5.0 envelope contract reserves `emitted_at` as optional ISO-
8601 UTC. 0.5.0 emitters do not populate it (saves bytes). 0.5.4
makes it authoritative for any envelope landing in `plinth.events`:

1. The writer reads `received_at` from the queue entry (set at
   enqueue time, which is the listener-dispatch time, which is
   within microseconds of the PG NOTIFY landing on the listener
   socket).
2. Formats it as `YYYY-MM-DDTHH:MM:SS.fffZ` (ISO-8601 UTC,
   millisecond precision).
3. Inserts the formatted string into `envelope["emitted_at"]`. If
   the envelope already had `emitted_at` (e.g. a Layer-3
   `pubsub.publish` populated it inside the user payload —
   ICD-0.5.0 §Payload Envelope notes this is allowed), the writer
   OVERWRITES the top-level field. The user's `emitted_at` if any
   stays inside `payload` (Layer 3 envelopes have a separate
   `payload` sub-object), which the writer does not touch.

The persisted envelope (`plinth.events.payload`) is the
WRITER-AUGMENTED envelope, not the original. Replay consumers see
`emitted_at` populated with server time. Live-stream subscribers
(0.5.2 broker fan-out) see the ORIGINAL envelope — the broker fans
the envelope BEFORE the writer mutates a local copy. This is by
design: the writer's local copy is a separate `Json::Value` mutated
in the writer's thread; the listener's dispatch passes the original
envelope to handlers by const reference, so the broker handler's
view is unmodified. §Security Constraint 5 pins this.

### Channel filter

The writer accepts EVERY envelope the listener dispatches —
`plinth:data:*`, `plinth:system:*`, `plinth:ext:*`. No layer is
excluded. Rationale: replay must serve all subscribed channels, and
filtering at write time creates retention asymmetry (a reconnecting
client that subscribed to `plinth:system:packages.installed` would
not see replays if the writer skipped Layer 2). The architecture
sketch (`03-data.md §3.5`) does not exclude any layer.

### What the writer does NOT do

- **No envelope validation.** The listener already validated the
  envelope (parsed JSON, channel regex, layer-channel consistency
  per ICD-0.5.0 §Validation pipeline). The writer trusts the
  listener's parse.
- **No per-write audit.** Storing every successful write would
  drown the audit log. Only failures audit, rate-limited.
- **No exactly-once guarantee.** PG NOTIFY can deliver duplicates
  to the listener (rare; PG guarantees at-least-once on the
  LISTEN side). If the listener dispatches the same envelope
  twice, the writer inserts twice — two rows in `plinth.events`,
  different `seq`s, same `payload`. Replay consumers see both.
  Clients tolerate per the duplicate-toleration contract (§Out of
  scope).
- **No cross-node de-duplication.** Each node's listener receives
  the same NOTIFY (PG fans LISTEN/NOTIFY to every connected
  subscriber). Each node's writer would otherwise insert the same
  row N times where N is the cluster size. §OQ7 pins the
  resolution: only ONE node writes per envelope, gated by a
  `realtime.events.writer_node_id` config OR a PG-advisory-lock-
  per-channel scheme. Architect resolves on implementation; default
  recommendation is the advisory-lock scheme — it is HA-correct
  without operator config drift.

### Failure handling

PG INSERT failures (connection drop, constraint violation —
extremely rare; the table has no unique constraints beyond `seq`,
which BIGSERIAL can't violate; `channel TEXT NOT NULL` is satisfied
by listener parse) audit `realtime.events.write_failed` with the
SQLSTATE and the envelope channel. The row is dropped (no retry).

If the connection drops mid-INSERT, the writer reconnects on the
next dequeue attempt; queue entries pile up during the outage
(bounded by `write_queue_size`); on overflow, drops audit per
§Write queue. Recovery from PG outage is automatic.

---

## `plinth.user_event_cursors` and the Cursor Store

### Table shape

```sql
CREATE TABLE plinth.user_event_cursors (
    user_id     UUID PRIMARY KEY
                REFERENCES plinth.users(id) ON DELETE CASCADE,
    last_seq    BIGINT NOT NULL DEFAULT 0,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

`ON DELETE CASCADE` ties cursor lifetime to user lifetime — when a
user is deleted, their cursor disappears with them. No GC needed.

`last_seq = 0` for a newly-created user means "no events delivered
yet." On first WS subscribe, the user's `since_seq` claim is also 0
(the SDK has no prior cursor); replay returns no rows; the user
starts in fresh-subscriber state.

### `cursor_store` API

```cpp
// src/kernel/realtime/cursor_store.hpp
namespace plinth::realtime::cursor_store {

// Read the user's current server-side cursor. Returns 0 if no row
// exists (new user) or the user lookup fails. Cached for
// realtime.events.cursor_cache_ttl_ms (default 1000 ms) per user.
auto read_cursor(const std::string& user_id) -> drogon::Task<std::int64_t>;

// Advance the user's cursor to `new_seq` if it's greater than the
// stored value. Idempotent — same-or-lower seq is a no-op. UPSERT
// pattern (INSERT ... ON CONFLICT DO UPDATE).
auto record_delivered(const std::string& user_id,
                       std::int64_t new_seq)
    -> drogon::Task<void>;

// Reset the cursor (set to a specific seq, regardless of order).
// Used on resync_required → server-side cursor sync to client's
// post-resync state. Caller-driven; does not auto-fire.
auto reset_cursor(const std::string& user_id,
                   std::int64_t new_seq)
    -> drogon::Task<void>;

}  // namespace plinth::realtime::cursor_store
```

### When `record_delivered` fires

The 0.5.2 broker `dispatch_to_subscribers` (broker.cpp, the per-
envelope fan-out loop) currently calls `conn->send(frame)` per
matched subscriber. 0.5.4 extends this with a post-send hook:

```cpp
// 0.5.2 path (existing):
for (auto& sub : matched) {
    co_await sub.conn->send(frame);
}

// 0.5.4 extension:
for (auto& sub : matched) {
    co_await sub.conn->send(frame);
    // NEW — fire-and-forget cursor advance. Failures do NOT
    // re-raise to the broker; they audit and continue.
    co_await cursor_store::record_delivered(sub.user_id,
                                              ev.events_seq);
}
```

The `ev.events_seq` field is populated by the writer on successful
INSERT — but the broker fans out BEFORE the writer's INSERT
completes (the broker is the FIRST EventHandler, the writer is the
SECOND, both run on the listener thread per ICD-0.5.0 §Listener
Subsystem; the broker's fan-out scheduled a frame send onto the WS
conn's loop and returned, but the writer hasn't INSERTed yet).

To resolve the ordering: the broker DOES NOT advance the cursor on
its own send. The writer advances `ev.events_seq` after INSERT
returns, then enqueues a FOLLOW-UP `record_delivered` call per
WS-connected user that received the event. This is wired through
a `delivered_users` callback the broker fills synchronously (it
already iterates the subscribers; appending each to a list is
free). §OQ4 ratifies.

The flow:

1. Listener dispatches envelope to broker (handler 1).
2. Broker matches subscribers, schedules sends on each conn's loop.
3. Broker fills `ev.delivered_to_users.push_back(sub.user_id)` for
   each match (synchronous list append — no IO).
4. Listener dispatches the same envelope to events_writer (handler 2).
5. Writer enqueues the envelope + the delivered-users list onto its
   loop.
6. Writer INSERTs, gets back `seq`, then for each user in the list,
   issues `cursor_store::record_delivered(user, seq)`.

Cursors lag behind sends by one PG round-trip. A reconnect during
that lag causes one duplicate replay (the user's cursor still says
`last_seq=N`, but they actually saw `seq=N+1`); duplicate-tolerance
contract handles this.

### When `read_cursor` fires

ONLY on reconnect, BEFORE the broker processes the subscribe frame.
The replay query receives the client's `since_seq` claim; the
cursor store is consulted only as a defense-in-depth check (see
§Replay query). The broker's hot path NEVER reads the cursor.

### When `reset_cursor` fires

On `resync_required` emission — the server signals "your cursor is
too old, re-query." After the client re-queries (driven by the SDK,
0.6.3 territory), the server's notion of `last_seq` should match
the client's post-re-query state. The reset call sets the server-
side cursor to `min(seq) FROM plinth.events` so the next reconnect
attempt starts cleanly. §OQ3 ratifies this auto-reset behavior.

### Cache layer

To avoid hitting PG on every `record_delivered` (firehose
proportional to broker fan-out × subscribers), an in-memory LRU
cache keyed by `user_id` holds the last-written `last_seq` value
per user. Cache TTL `realtime.events.cursor_cache_ttl_ms` (default
1000 ms) — UPDATEs against PG fire every TTL or every cursor delta
> `realtime.events.cursor_flush_threshold` (default 50), whichever
first. On cache miss, `read_cursor` falls through to PG.

### Lifecycle

The cursor store has no atexit chain entry of its own. On process
shutdown, the cache flushes before `events_writer::stop()` returns;
shutdown is bounded by `events_writer.shutdown_drain_ms`. Any
in-flight `record_delivered` writes fire-and-forget — if PG is
already down, the cache flush logs a warn and proceeds.

`plinth.user_event_cursors` rows accumulate as users connect; they
are never deleted by 0.5.4 unless the user is deleted. Dormant
cursors carry minimal storage cost (BIGINT + UUID + TIMESTAMPTZ
≈ 32 bytes per user). For scale concerns at 10K+ users, §OQ8
pinning per-user-only cursors keeps the row count linear in the
user count, not the (user × channel) product.

---

## Reconnect Handshake

### Client subscribe frame extension

ICD-0.1.6 §Frame Types defines the WS subscribe shape today:

```json
{ "type": "subscribe", "channels": ["plinth:data:ext_notes.notes", ...] }
```

0.5.4 adds an optional `since_seq` field:

```json
{
  "type": "subscribe",
  "channels": ["plinth:data:ext_notes.notes", "plinth:system:packages.installed"],
  "since_seq": 4823
}
```

When `since_seq` is absent (fresh subscribe — no prior session
state), the broker's existing 0.5.2 path runs unchanged; no replay
fires. When `since_seq` is present (reconnect), the broker triggers
the replay engine BEFORE registering the live subscription, so the
client receives missed events first, then live frames in seq order.

### Server frame additions

Three new server-frame types extend ICD-0.1.6 §Frame Types:

```json
// Per replayed envelope:
{ "type": "replay", "envelope": { ... } }

// Sent after the last replay frame in a successful replay:
{ "type": "replay_done", "up_to_seq": 4901, "row_count": 78 }

// Sent INSTEAD of replay frames when client cursor is too old:
{ "type": "resync", "reason": "cursor_expired", "retention_seconds": 3600 }
```

`reason` enumerates: `cursor_expired` (the `since_seq` falls before
the retention window), `row_cap` (replay would exceed
`replay_max_total_rows`), `mismatch` (server-side cursor disagrees
with client's `since_seq` by more than `replay_max_total_rows`).

### Subscribe handler flow (0.5.4 path)

```
on_subscribe_frame(conn, frame):
    1. parse frame; extract channels[], since_seq (optional).
    2. validate channels — ICD-0.5.2 §Subscribe RBAC re-check
       (each channel passes the rule check for conn.user_id).
       Failures emit subscribe_denied per channel, then continue
       with the surviving channels.
    3. if since_seq is None:
         register subscriptions live via 0.5.2 path.
         done.
    4. (else — reconnect path)
       since_seq is an integer.
       if since_seq < 0:
         emit {type:"error", code:"resubscribe.invalid_since_seq"}
         + audit; continue with no subscriptions.
       fetch min_seq = SELECT MIN(seq) FROM plinth.events;
       if since_seq < min_seq - 1:    # cursor older than retention
         emit {type:"resync", reason:"cursor_expired",
               retention_seconds: <retention>};
         cursor_store::reset_cursor(conn.user_id, min_seq);
         audit realtime.events.resync_required;
         register subscriptions live via 0.5.2 path
           (start receiving from "now");
         done.
       else:
         server_cursor = cursor_store::read_cursor(conn.user_id);
         if abs(since_seq - server_cursor) > replay_max_total_rows:
           emit {type:"resync", reason:"mismatch", ...};
           audit realtime.events.resync_required;
           register live subs;
           done.
         (replay path)
         emit {type:"replay_started"} (audit only; not a wire frame
            unless §OQ6 picks the wire-frame variant);
         REGISTER live subscriptions FIRST (so envelopes emitted
            during the replay query land in the broker queue, not
            lost). Live frames buffer in conn's send queue ordered
            after the replay frames.
         run replay engine: paginated SELECT, emit replay frames.
         emit replay_done with the last replayed seq.
         cursor_store::record_delivered(conn.user_id, last_seq);
```

### Why register live subs BEFORE replay starts

If the replay query takes 500 ms and a new envelope on a subscribed
channel emits during that window, registering subs AFTER replay
would lose that envelope (broker doesn't have the subscription yet
when the listener dispatches). Registering first means the broker
fan-out enqueues the live frame onto the conn's send queue — but
ordering is preserved because Drogon's per-conn send queue is FIFO.
The replay engine emits frames synchronously in order, then
returns; live frames waiting in the queue (if any) flush after.

A duplicate scenario: an envelope emitted at server time T=500ms
into the reconnect; the replay query saw it (its `seq` is
`<= max_seq_at_query_time`), so it appears in the replay batch; the
broker also fans it out live, so it queues a separate `event` frame.
The client sees the replay frame and the live frame for the same
seq. Duplicate-tolerance contract handles. Optional defense: replay
emits include the `seq` and the broker tags live emits with seq;
client de-duplicates client-side. 0.6.3 SDK responsibility.

§OQ3 ratifies the resync envelope shape.

---

## Replay Engine

### Query

```sql
SELECT seq, channel, payload
FROM plinth.events
WHERE seq > $1
  AND channel = ANY($2)
ORDER BY seq ASC
LIMIT $3;
```

Parameters: `$1 = since_seq`, `$2 = subscribed channels filtered to
RBAC-approved set`, `$3 = replay_max_rows_per_chunk`.

The query runs against the kernel-default DbClient (no extension
schema; `plinth.events` is in the kernel `plinth` schema). The
classifier (ICD-0.5.1) ignores SELECTs so no coalescer accumulation
fires. The kernel-scope bc bypass (ICD-0.5.3 §Per-Op `SET
search_path`) means no `SET LOCAL` wrapper applies.

### Pagination

The engine loops:

```
last_seq = since_seq
while true:
    rows = SELECT ... WHERE seq > last_seq ... LIMIT chunk;
    if rows is empty: break.
    for row in rows:
        per-channel RBAC re-check on row.channel for conn.user_id.
        if denied: skip + audit broker.subscribe_rule_denied;
        emit {type:"replay", envelope: row.payload};
    last_seq = rows[-1].seq;
    if total_emitted > replay_max_total_rows:
        emit {type:"replay_truncated", up_to_seq: last_seq,
              reason: "row_cap"};
        emit {type:"resync", reason:"row_cap", retention_seconds: ...};
        audit realtime.events.replay_truncated;
        return.
emit {type:"replay_done", up_to_seq: last_seq, row_count: total_emitted};
audit realtime.events.replay_completed;
cursor_store::record_delivered(conn.user_id, last_seq);
```

### Per-channel RBAC re-check

ICD-0.5.2 §Security Constraint 2 pins "RBAC re-checked on delivery"
as defense-in-depth. The replay engine MUST re-check every replayed
envelope's `channel` against the user's effective rules at replay
time, not at subscribe time. A user whose rule was revoked during
the disconnect window would see the rule denial in the audit log
but no envelope on the wire. §Security Constraint 1 of THIS ICD
pins the same.

The check uses the existing `broker::resolve_subscribe_rule(user_id,
channel)` (ICD-0.5.2 §Subscribe RBAC) — no new RBAC code. Cached
rule resolution is fine because the cache invalidates on rule
mutation per the existing 0.2.3 `plinth_capability_changed` LISTEN
channel.

### Chunk-size trade-off

Default `replay_max_rows_per_chunk=500` keeps each PG query under
typical jsonb-result-set size limits (each envelope is ≤ 8000
bytes; 500 rows ≈ 4 MB max — comfortable for libpq's text
protocol). Increasing it to 5000 reduces query overhead but risks
holding the broker thread on the WS conn's loop too long. 500 is
empirically fine on commodity hardware.

### Total-row cap

Default `replay_max_total_rows=10000`. The reasoning per §Out of
scope: a user offline long enough to accumulate >10K events should
re-query rather than process a backlog. The cap fires the
`replay_truncated` audit + `resync` signal, and the client's SDK
handles the resync by re-querying.

### Concurrency

Multiple users can run replays concurrently; each has its own state
(the engine is stateless beyond function-call-local variables). The
PG server handles the parallel SELECTs via standard connection
pool. Drogon DbClient pool size is the operator's existing
configuration; no new pool added.

---

## Retention + Cleanup

### `plinth.events_cleanup` scheduled task

Architecture sketch (`04-services-ha.md §2`) names the task. ICD
pins it:

```sql
DELETE FROM plinth.events
WHERE created_at < NOW() - INTERVAL '%s seconds';
```

`%s` substituted with `realtime.events.retention_seconds` (default
3600 = 1 hour). The placeholder is on the SQL string (parameterized
at the DELETE statement build site, not interpolated into PG —
constants substitute via parameterized query).

### Schedule

`realtime.events.cleanup_interval_ms` (default 300000 = 5 minutes).
The scheduled-tasks subsystem (per architecture/04-services-ha.md
§2) is not yet implemented (0.7 milestone), so 0.5.4 uses a
trantor `runEvery` on the events_writer's loop:

```cpp
events_writer_loop.runEvery(
    config.cleanup_interval_ms,
    []{ cleanup_events::run(); }
);
```

When 0.7 wires the proper scheduler, the call migrates to it; the
function signature stays the same.

### Advisory lock

To avoid N nodes simultaneously running the same DELETE:

```sql
SELECT pg_try_advisory_lock(<plinth.events_cleanup>);
-- if locked: run DELETE, RELEASE lock; else: skip this tick.
```

The lock key is a hardcoded BIGINT identifier (e.g.
`hash('plinth.events_cleanup')` baked into the source). Per
architecture/04-services-ha.md §2 advisory locks scope to the PG
server, so even on multi-node, only one node sweeps per tick.
Skipped ticks are normal under multi-node deployments — the next
tick on the lock-winning node picks up.

### Sweep audit

`realtime.events.cleanup_swept` audit fires on every successful
DELETE with `{rows_deleted, sweep_duration_ms, oldest_remaining_seq}`.
Rate-limited per `audit_window_ms` — the per-tick first hit fires;
subsequent ticks within the window aggregate.

### Failure handling

DELETE failures (PG down, lock contention) audit
`realtime.events.write_failed` (re-using the same audit code as the
writer; reason `cleanup_failed`). The next tick retries.

### Why DELETE not partition

Architecture sketch does NOT pin partitioning — the table is
expected to stay small (bounded by retention, not history). At
1000 events/second × 3600 seconds = 3.6M rows per retention window
maximum, a B-tree on `(seq)` keeps queries fast and DELETE
proportional to swept rows. If observed retention pressure exceeds
this on actual deployments, partitioning is a 0.5.4.N follow-up;
0.5.4 ships unpartitioned.

---

## Indexing

The two new tables get the following indexes:

```sql
-- plinth.events:
PRIMARY KEY (seq);                            -- BIGSERIAL, automatic
CREATE INDEX events_channel_seq_idx
    ON plinth.events (channel, seq);          -- replay query
CREATE INDEX events_created_at_idx
    ON plinth.events (created_at);            -- cleanup sweep

-- plinth.user_event_cursors:
PRIMARY KEY (user_id);                        -- automatic
-- updated_at is for diagnostics; no index.
```

### `(channel, seq)` rationale

The replay query filters on `channel = ANY($2)` and orders by `seq`.
The composite index serves the channel filter (PG resolves
`ANY(array)` as a series of `=` lookups) and the seq order in one
index scan. EXPLAIN on a populated table confirms a Bitmap Heap Scan
+ sort by seq using the composite — no separate scan needed.

### `(created_at)` rationale

The cleanup DELETE filters on `created_at < threshold`. A separate
B-tree on `created_at` enables index-only DELETE for the
typical-case "delete the oldest N rows" pattern. PG's autovacuum
reclaims the heap+index pages on commit.

### No index on `payload->>'channel'`

The envelope's internal `channel` field equals the row's `channel`
column (the writer copies it on INSERT). No need for a JSONB
expression index — the column carries the searchable value.

§OQ6 ratifies these three indexes.

---

## Config Surface

```json
{
  "realtime": {
    "events": {
      "enabled": true,
      "retention_seconds": 3600,
      "cleanup_interval_ms": 300000,
      "replay_max_rows_per_chunk": 500,
      "replay_max_total_rows": 10000,
      "write_queue_size": 10000,
      "shutdown_drain_ms": 5000,
      "cursor_cache_ttl_ms": 1000,
      "cursor_flush_threshold": 50,
      "audit_window_ms": 60000
    }
  }
}
```

### Field semantics

- **`realtime.events.enabled`** (bool, default `true`) — master
  switch. When `false`, the writer never registers, no rows land in
  `plinth.events`, replay queries return empty, the cleanup task
  does not arm. Reconnecting clients with `since_seq` always
  receive `{type:"resync", reason:"events_disabled"}`. Intended ONLY
  for emergency-disable scenarios (PG storage exhaustion); production
  MUST set `true`. Warn-log emitted when `false`.
- **`realtime.events.retention_seconds`** (uint, default 3600) —
  retention window. Cleanup task DELETEs rows older than this. Bound
  `[60, 604800]` (1 minute to 7 days). Out-of-range fails config
  load.
- **`realtime.events.cleanup_interval_ms`** (uint, default 300000) —
  sweep interval. Bound `[10000, 3600000]`.
- **`realtime.events.replay_max_rows_per_chunk`** (uint, default
  500) — per-pagination-chunk row limit. Bound `[10, 10000]`.
- **`realtime.events.replay_max_total_rows`** (uint, default 10000)
  — total-row hard cap; exceeds → `replay_truncated` + `resync`.
  Bound `[100, 1000000]`. MUST be ≥ `replay_max_rows_per_chunk`;
  config-load fails if violated.
- **`realtime.events.write_queue_size`** (uint, default 10000) —
  in-memory queue depth before drop-newest. Bound `[100, 1000000]`.
- **`realtime.events.shutdown_drain_ms`** (uint, default 5000) —
  atexit barrier. Bound `[100, 60000]`.
- **`realtime.events.cursor_cache_ttl_ms`** (uint, default 1000) —
  cursor-cache age before forced flush. Bound `[0, 60000]`. `0`
  disables caching (every `record_delivered` writes through to PG —
  unlikely useful, intended only for diagnostic tracing).
- **`realtime.events.cursor_flush_threshold`** (uint, default 50) —
  cursor-delta count before forced flush. Bound `[1, 10000]`.
- **`realtime.events.audit_window_ms`** (uint, default 60000) —
  shared aggregation window for all six `realtime.events.*` audits.
  Bound `[1000, 3600000]`.

### Config loader extension

`Config::Realtime` gains an `Events` substruct alongside the
existing `Listener` / `Notify` / `Coalescer` / `Broker` substructs.
Loader follows the 0.5.2 pattern — bound check + ranged defaults +
warn-log on `enabled == false`.

### Defaults

`config/defaults/prod.json` populates all fields. `config/defaults/dev.json`
inherits with `retention_seconds=300` (5 minutes — dev iterations
don't need an hour) and `cleanup_interval_ms=60000` (1 minute). Test
fixtures override `enabled=false` for tests that don't exercise
events writer paths.

---

## Audit Events

All six events are rate-limited via the shared `audit_window_ms`
aggregation (per ICD-0.5.2 + ICD-0.5.3 pattern).

### `realtime.events.write_failed`

Fires on PG INSERT failure or queue overflow. Payload:

```json
{
  "channel": "plinth:data:ext_notes.notes",
  "reason": "queue_full" | "pg_error" | "cleanup_failed",
  "sqlstate": "53300",
  "queue_depth_at_drop": 10000,
  "count_in_window": 47,
  "window_ms": 60000
}
```

`sqlstate` populated only on `reason == pg_error`. `queue_depth_at_drop`
populated only on `reason == queue_full`.

### `realtime.events.replay_started`

Fires on every replay engine entry (after RBAC re-check passes, the
chunked SELECT is about to begin). Payload:

```json
{
  "user_id": "u_abc",
  "since_seq": 4823,
  "channels_count": 7,
  "count_in_window": 23,
  "window_ms": 60000
}
```

`channels_count` is the count of RBAC-approved subscribed channels
(post-`subscribe_denied` filtering). Aggregated per-user; bursts of
reconnects from one user surface as a high `count_in_window`.

### `realtime.events.replay_completed`

Fires on successful replay completion (replay_done emitted). Payload:

```json
{
  "user_id": "u_abc",
  "since_seq": 4823,
  "up_to_seq": 4901,
  "row_count": 78,
  "wall_clock_ms": 145,
  "count_in_window": 21,
  "window_ms": 60000
}
```

### `realtime.events.replay_truncated`

Fires on row-cap exceedance during replay. Payload:

```json
{
  "user_id": "u_abc",
  "since_seq": 4823,
  "up_to_seq": 14823,
  "row_cap": 10000,
  "count_in_window": 1,
  "window_ms": 60000
}
```

A truncation is a security-relevant signal (a client is far behind);
operators alert on unexpected rates.

### `realtime.events.resync_required`

Fires on every `{type:"resync"}` emission, regardless of reason.
Payload:

```json
{
  "user_id": "u_abc",
  "since_seq": 12,
  "reason": "cursor_expired" | "row_cap" | "mismatch" | "events_disabled",
  "retention_seconds": 3600,
  "count_in_window": 3,
  "window_ms": 60000
}
```

### `realtime.events.cleanup_swept`

Fires on every successful cleanup DELETE. Payload:

```json
{
  "rows_deleted": 87421,
  "sweep_duration_ms": 240,
  "oldest_remaining_seq": 90000,
  "count_in_window": 12,
  "window_ms": 60000
}
```

Per-window aggregation; first sweep in a window fires the full
payload; subsequent sweeps within the window aggregate the totals.

---

## HA Semantics

**One writer per envelope, multi-node deployments.** Each node's
listener receives the same NOTIFY (LISTEN/NOTIFY fans to every
connected subscriber). Without coordination, every node would
INSERT the same envelope, producing N rows where N is the cluster
size. §OQ7 pins resolution: a per-envelope advisory lock keyed by
the envelope's `(channel, emitted_at)` tuple — only the lock
winner INSERTs; losers skip silently. The lock is acquired with
`pg_try_advisory_xact_lock` and released at transaction commit; on
contention, losers' `try_lock` returns false and they skip without
audit.

A weaker resolution (config knob `realtime.events.writer_node_id`
selecting a single writer node) is rejected per §OQ7 — operator-
managed config drift on a critical path.

**Replay is multi-node.** Any node serving a WS reconnect can run
the replay query; `plinth.events` is shared PG state; the user's
cursor is shared via `plinth.user_event_cursors`. A user reconnecting
to Node B after disconnecting from Node A receives all missed events
without coordination.

**Cursor consistency.** `plinth.user_event_cursors` is shared PG
state; reads / writes go to the same PG. The cache is per-node, so
a `record_delivered` on Node A may stale the cache on Node B for
up to `cursor_cache_ttl_ms` (default 1 s). For the 0.5.4 use case
(reconnect — rare event; staleness only matters at reconnect), the
cache uses a write-through to PG and a TTL-based invalidation.
On Node B's reconnect handler reading the cursor, the read goes to
PG (cache TTL expired or cache miss for that user), getting the
authoritative value.

**No cross-node writer coordination beyond the advisory lock.** The
broker's per-conn fan-out is per-node (Node A's broker fans to Node
A's connected clients); the writer's per-envelope INSERT is one-of-N
gated by lock. No two-phase commit, no Raft.

---

## Deterministic Teardown

### Atexit chain (updated from 0.5.3)

```
atexit(order):
    1. capabilities::cancel_all_pending_calls();
    2. realtime::stop_listener();
    3. realtime::stop_events_writer();              ← NEW 0.5.4
    4. js::rollback_all_batches();                  // 0.5.3
    5. realtime::stop_broker();                     // 0.5.2
    6. CoalescerRegistry::instance().shutdown();    // 0.5.1
    7. drogon::app().quit();
```

### What `stop_events_writer` does

1. Sets a stopping flag; new enqueues silently drop with audit.
2. The dedicated loop drains its queue (writes pending entries to
   PG) bounded by `realtime.events.shutdown_drain_ms` (default
   5000 ms). On timeout, remaining queue entries are lost; one
   final `realtime.events.write_failed` audit fires with the count.
3. Cancels the cleanup `runEvery` timer.
4. Flushes the cursor cache (every dirty cache entry UPSERTs to
   `plinth.user_event_cursors` synchronously, bounded by
   `shutdown_drain_ms`).
5. Joins the loop's thread; cancels the registered EventHandler at
   the listener (no-op since the listener is already stopped per
   atexit ordering).
6. No-op on second call.

### Ordering rationale

- `stop_listener()` runs FIRST so no new envelopes arrive during
  drain.
- `stop_events_writer()` runs BEFORE `stop_broker()` so the writer
  drains while the broker is still alive (the broker doesn't depend
  on the writer, but symmetry: writer is a downstream consumer of
  the listener; broker is too; both drain before
  `drogon::app().quit()`).
- `stop_events_writer()` runs AFTER `js::rollback_all_batches()` ?
  No — BEFORE per the order above. Rationale: `js::rollback_all_batches`
  rolls back in-flight `db.batch` callers; the writer's own SQL
  isn't a batch (it's plain `db.execSqlCoro` inside the writer's
  loop), so it doesn't depend on rollback ordering. The order pinned
  is **listener → events_writer → batches → broker → coalescer →
  drogon::quit**, mirroring the listener-handler-registration order
  (broker first, writer second, both run at the same listener-
  dispatch tick). Stopping the writer right after the listener
  ensures the writer's loop drains while the broker is still
  serving; the broker's drain is independent.

### Mirror in test fixtures

`tests/kernel/realtime/realtime_test_fixture.cpp` (extended) calls
`realtime::stop_events_writer()` between `realtime::stop_listener()`
and `realtime::stop_broker()` in its teardown. Async-bridge
fixtures (`async_bridge_fixture.cpp`) gain the same call. Mirrors
ICD-0.5.3 §Mirror in `ws_test_fixture.cpp` posture.

---

## Error Model

### Writer-side errors (kernel C++ — audit only, no JS facing)

| Audit code | Trigger | Fire point |
|------------|---------|------------|
| `realtime.events.write_failed` reason=`queue_full` | Write queue at `write_queue_size`; enqueue dropped | Synchronous at enqueue |
| `realtime.events.write_failed` reason=`pg_error` | INSERT raises `DrogonDbException` | Async on the writer loop |
| `realtime.events.write_failed` reason=`cleanup_failed` | Cleanup DELETE raises | Async on the cleanup tick |

### Replay-side errors (WS-facing)

| Code | Trigger | Fire point |
|------|---------|------------|
| `resubscribe.invalid_since_seq` | Client subscribes with `since_seq` that is non-integer or negative | Sync at frame parse |
| `resubscribe.events_disabled` | `realtime.events.enabled == false` and client sends `since_seq` | Sync at subscribe handler entry |
| `replay_truncated` (frame, not error) | Replay exceeds `replay_max_total_rows` | Async on replay engine |
| `resync` (frame, not error) | Cursor expired / row cap / mismatch / events disabled | Sync or async per reason |

The first two emit a per-frame `error` reply per ICD-0.1.6 §Error
Frames — `{type:"error", code:"resubscribe.*", message:"..."}`.
Audit `realtime.events.resync_required` fires for the second case
(reason `events_disabled`).

### Cursor-store errors

`plinth.user_event_cursors` UPSERT failures (PG down, constraint —
none today; `ON DELETE CASCADE` removes rows on user delete only)
log warn but do not raise to the broker. The cache eats the failure;
next reconnect re-reads from PG and resumes.

`read_cursor` failures fall through to `last_seq = 0` (treat as
"no cursor recorded"), audit
`realtime.events.write_failed reason=cursor_read_failed`. The
client's `since_seq` claim then drives replay; the server-side
cursor catches up on `record_delivered` calls during the replay.

### Configuration load failures

- Bound checks fail config load with `Config::Error::OutOfRange`
  (existing family).
- `replay_max_total_rows < replay_max_rows_per_chunk` fails load
  with `Config::Error::ConsistencyViolation` (new variant — added
  to `config_error.hpp`).
- `enabled == false` succeeds load with warn-log (intentional
  emergency-disable posture).

---

## Security Constraints

1. **Replay re-checks per-channel RBAC.** Every replayed envelope's
   `channel` passes `broker::resolve_subscribe_rule(user_id,
   channel)` before the envelope hits the wire. A rule revoked
   during the disconnect window denies the replay; the audit fires
   `broker::subscribe_rule_denied` with reason `replay`. Mirrors
   ICD-0.5.2 §Security Constraint 2 verbatim.

2. **Cursor opacity.** The cursor (BIGINT `seq`) is exposed to the
   client. A malicious client forging another user's cursor cannot
   receive that user's events — the replay engine reads
   `conn.user_id` from the WS auth state (ICD-0.1.6 §Auth, bound at
   connect from session token), NEVER from the client's frame
   payload. The forged cursor only enables the attacker to enumerate
   write-rate timing for the `plinth.events` table (the gap between
   `since_seq` and the server's current `MAX(seq)` is observable).
   Per §OQ2 we accept this trade-off: cursor opacity (encryption)
   would prevent client-side persistence and is overkill for the
   threat model. Documented; revisit if a real attack surfaces.

3. **Retention is a hard cap.** Rows older than
   `retention_seconds` are unconditionally DELETEd by the cleanup
   task. Operators MUST tune `retention_seconds` to match their
   client offline-tolerance posture; defaults assume 1-hour offline-
   tolerance (matches the 0.6.x SDK's reconnect-grace-period
   default). Missing the retention by extending offline beyond it
   triggers `resync_required` — by design.

4. **Replay rate-limited per user.** A reconnect storm from one
   user (`since_seq` reconnect-reconnect-reconnect) costs PG read
   bandwidth. The per-user audit `realtime.events.replay_started`
   surfaces the rate; operators alert on sustained high rates.
   No hard rate-limit at the replay engine in 0.5.4 — the WS conn
   layer's per-connection rate-limit (ICD-0.1.6 §Per-Conn Rate
   Limit) caps frame churn organically.

5. **Persisted envelope is server-augmented.** The writer overwrites
   `envelope.emitted_at` with server time before INSERT. A client
   inspecting `plinth.events.payload` (via `db.query` from a
   privileged extension; impossible in 0.5.4 because no extension
   has `SELECT` on `plinth.events` per the kernel-schema RBAC
   posture; mentioned for completeness) sees the server-assigned
   timestamp, never a client-claimed one. Layer 3 envelopes have
   their own `payload.emitted_at` inside the user payload, which is
   client-controlled and untouched by the writer.

6. **Duplicate replay tolerated.** A reconnect during the
   broker-fan-out → cursor-advance window may replay an envelope the
   client already received. Clients MUST treat envelopes as
   idempotent (matches the 0.5.1 coalescer's per-window envelope
   semantics). 0.5.4 does NOT add a server-side dedup layer (would
   require server tracking of "delivered_seqs per user," much
   heavier state). The 0.6.3 SDK can de-duplicate client-side using
   `seq` if desired (when 0.5.5 lands envelope `seq`).

7. **Cross-node single writer.** §OQ7 advisory-lock-per-envelope
   pinned. No two nodes write the same envelope. The lock acquisition
   does not error on contention; losers skip silently — the per-node
   writer queue depth tracks at half (or 1/N) of the listener
   dispatch rate under steady state, so the queue size knob still
   works.

8. **Cursor TOCTOU on rapid reconnect.** A user reconnecting twice
   in succession (new conn, drop, new conn within `cursor_cache_ttl_ms`)
   may see the second reconnect's `read_cursor` return a stale
   value (the first reconnect's `record_delivered` has not flushed
   to PG yet). The replay engine compares the client's `since_seq`
   to the cached value — if they match, replay proceeds normally;
   if `since_seq > server_cursor`, the server treats the client's
   claim as authoritative (the client knows what they received; the
   server's stale cache must catch up). The cursor advances after
   replay completes, flushing the cache. Documented; acceptable.

---

## Test Cases

Test prefix: **E.\*** for events writer (write path); **C.\*** for
cursor store (read/write/cache); **D.\*** for delta-sync handshake
(subscribe frame + resync emission); **Y.\*** for replay engine
(query + pagination + truncation); **K.\*** for cleanup task
(retention sweep); **I.\*** for end-to-end integration. Tag
convention `[realtime][events]` + per-group subtype (`[writer]`,
`[cursor]`, `[delta]`, `[replay]`, `[cleanup]`, `[integration]`).
Distinct from 0.5.0's R/E/P, 0.5.1's C/T/I/E, 0.5.2's B/S/U/I, and
0.5.3's B/S/P/T/I prefixes (the `K.*` cleanup prefix and `Y.*`
replay prefix are new — `D.*` reuses 0.5.0's `R.*` shape but
specializes for delta-sync — see §Test-seam notes).

Total: **41 new cases** (10 E + 7 C + 8 D + 9 Y + 4 K + 3 I).
Distributed across six test TUs (five new, one extended).

### Events writer — `tests/kernel/realtime/events_writer_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| E.01 | Happy write | Listener dispatches one envelope to writer | Yes | One row in `plinth.events` matching channel + payload; `emitted_at` populated; broker handler ran first (separate test channel) |
| E.02 | Layer 1 + 2 + 3 all persist | Three envelopes (data, system, ext) | Yes | Three rows, one per layer; channel column matches each |
| E.03 | Server-side `emitted_at` overwrites client | Layer-3 envelope with `emitted_at: "2020-01-01T00:00:00Z"` | Yes | Persisted row's `payload.emitted_at` matches server time, not 2020 |
| E.04 | Queue overflow drop-newest | `write_queue_size=10`; enqueue 15 envelopes synchronously | No (uses `enqueue_for_test`) | First 10 envelopes persist; envelopes 11-15 dropped; `realtime.events.write_failed reason=queue_full count_in_window=5` audit fires |
| E.05 | INSERT failure audits | Mock DbClient raising on INSERT; one envelope | Synthetic | `realtime.events.write_failed reason=pg_error sqlstate=...` audit fires; queue advances (no retry) |
| E.06 | Truncated envelope persists | Envelope with `truncated:true` + counts-only ops | Yes | Persisted verbatim; `truncated:true` survives the round trip |
| E.07 | Cross-node single writer (advisory lock) | Two simulated writers (two test threads) compete on same envelope | Yes | Exactly one row inserted; loser logs no error; no duplicate |
| E.08 | Atexit drain bounded | 100 envelopes queued; `stop_events_writer()` called immediately | Yes | All 100 persist before stop returns; no audit fires; drain within `shutdown_drain_ms` |
| E.09 | Atexit drain timeout | 10000 envelopes queued; `shutdown_drain_ms=100` | Yes | Some subset persists (whatever fit in 100ms); one `realtime.events.write_failed reason=shutdown_timeout` audit fires with the dropped count |
| E.10 | `enabled=false` no-op | Config `realtime.events.enabled=false`; listener dispatches | Yes | Zero rows in `plinth.events`; writer subsystem never registered |

### Cursor store — `tests/kernel/realtime/cursor_store_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| C.01 | First write creates row | `record_delivered("u1", 100)` for new user | Yes | Row inserted with `last_seq=100`; `read_cursor("u1") == 100` |
| C.02 | Monotonic advance | Sequential `record_delivered("u1", 50)` then `("u1", 100)` then `("u1", 75)` | Yes | Final `last_seq=100`; the 75 was a no-op (lower than current) |
| C.03 | Cache TTL | `record_delivered("u1", 100)`; mock clock advance 500 ms; `record_delivered("u1", 200)` (no PG hit) | Yes | One PG UPDATE only; cache holds `last_seq=200` |
| C.04 | Cache flush threshold | `cursor_flush_threshold=5`; 6 sequential `record_delivered` | Yes | One PG UPDATE on the 6th |
| C.05 | Cascade on user delete | DELETE FROM plinth.users WHERE id='u1'; `read_cursor("u1")` | Yes | Returns 0; the row is gone |
| C.06 | Reset cursor | `read_cursor("u1") == 100`; `reset_cursor("u1", 50)`; re-read | Yes | Returns 50; cache flushed |
| C.07 | Concurrent advance | 10 threads × 100 `record_delivered` each, ascending seqs | Yes | Final `last_seq == 1000`; no lost UPDATEs (UPSERT semantics) |

### Delta-sync handshake — `tests/kernel/ws/delta_sync_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| D.01 | Fresh subscribe (no `since_seq`) | Client connects; `{type:"subscribe", channels:[...]}` | Yes | 0.5.2 path runs unchanged; no replay; `subscribe_ok` |
| D.02 | Reconnect with valid `since_seq` | Client reconnects with `{since_seq:100}`; channel has 5 newer events | Yes | 5 `replay` frames + `replay_done` `{up_to_seq:105, row_count:5}`; live subs registered; cursor advances to 105 |
| D.03 | `since_seq` older than retention | `MIN(seq) FROM plinth.events == 200`; client sends `{since_seq:50}` | Yes | `{type:"resync", reason:"cursor_expired", retention_seconds:3600}` emitted; no replay frames; live subs registered; cursor reset to 200; `realtime.events.resync_required` audit |
| D.04 | Negative `since_seq` | Client sends `{since_seq:-1}` | Synthetic | `{type:"error", code:"resubscribe.invalid_since_seq"}`; no subs registered; audit |
| D.05 | `events.enabled=false` | Client sends `{since_seq:100}` with config disabled | Yes | `{type:"resync", reason:"events_disabled"}`; live subs registered (broker still works) |
| D.06 | Multi-channel reconnect | Client subscribes to 3 channels with `since_seq:100`; only 2 have newer events | Yes | Replay frames only for the 2 with events; 3 live subs registered |
| D.07 | RBAC denied per channel | Reconnect with 3 channels; user has rule denying 1 | Yes | 1 `subscribe_denied` frame; replay frames for the other 2 channels' missed events; 2 live subs |
| D.08 | Replay then live frame ordering | Reconnect with `since_seq:100`; mid-replay an event emits live | Yes | Replay frames in seq order, then `replay_done`, then live frame (queued during replay) — **Moved to ICD-0.5.5 L.03** per §16 OQ7 absorption (writer-first topology + per-conn live buffer make the ordering invariant crisply pinnable; D.08 deferred at v0.5.4 ship is fully covered by 0.5.5's live-buffer + flush-after-replay machinery) |

### Replay engine — `tests/kernel/realtime/replay_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| Y.01 | Empty replay | `since_seq` equals `MAX(seq)` | Yes | No replay frames; `replay_done` `{row_count:0}`; cursor unchanged |
| Y.02 | Single-chunk replay | 250 missed events; `replay_max_rows_per_chunk=500` | Yes | One PG SELECT; 250 replay frames; `replay_done` |
| Y.03 | Multi-chunk replay | 1500 missed events; chunk=500 | Yes | 3 PG SELECTs; 1500 replay frames in seq order; `replay_done` |
| Y.04 | Row-cap truncation | 15000 missed events; `replay_max_total_rows=10000` | Yes | 10000 frames + `replay_truncated` + `resync` reason=`row_cap`; cursor reset to last replayed seq |
| Y.05 | Per-channel filter | 1000 events on channel A, 1000 on B; subscribe to A only | Yes | 1000 replay frames (all A); none from B |
| Y.06 | RBAC re-check denies mid-replay | Mid-replay, mock revoke rule for one channel | Yes | Replays before revoke deliver; replays after skip (audit `subscribe_rule_denied reason=replay`); `replay_done` reflects only delivered count |
| Y.07 | Server-cursor mismatch detection | Server cursor=200; client `since_seq=200000` (10× over `replay_max_total_rows`) | Yes | `{type:"resync", reason:"mismatch"}` emitted; no replay; live subs registered |
| Y.08 | Concurrent replays | 5 users reconnect simultaneously; each has 100 missed events | Yes | 5 independent replays complete; no cross-user interference |
| Y.09 | Replay audit fires | Single replay of 78 events | Yes | `replay_started count_in_window=1` + `replay_completed count_in_window=1 row_count=78` audits fire |

### Cleanup task — `tests/kernel/scheduled_tasks/cleanup_events_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| K.01 | Sweep deletes old rows | `retention_seconds=60`; insert rows at NOW()-120s and NOW(); run sweep | Yes | Old row deleted; new row remains; `cleanup_swept rows_deleted=1` audit |
| K.02 | Empty sweep | No rows older than retention; run sweep | Yes | Zero deletions; audit fires with `rows_deleted=0` |
| K.03 | Advisory lock contention | Two threads both call `cleanup::run()` simultaneously | Yes | One acquires the lock + sweeps; the other returns silently (no audit, no error); idempotent |
| K.04 | Sweep failure audits | Mock DbClient raising on DELETE; run sweep | Synthetic | `realtime.events.write_failed reason=cleanup_failed sqlstate=...` audit fires |

### End-to-end integration — `tests/integration/events_replay_integration_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| I.01 | Listener → writer → cursor → replay end-to-end | Real WS client A subscribes; ext writes 10 INSERTs (coalescer fans 10 envelopes); A receives 10 frames + cursor advances; A drops; A reconnects with `since_seq` (which is the LAST cursor); ext writes 5 more INSERTs while A is offline (writer persists 5); A reconnects → receives 5 replay frames | Yes | Cursor monotonic; total received = 15; final cursor = last seq |
| I.02 | Two-node concurrent writer (advisory lock) | Two test instances of writer running against same PG; one envelope dispatched to both | Yes | Exactly one row in `plinth.events` for that envelope; both writers continue serving subsequent envelopes |
| I.03 | Live + replay race | A reconnects with `since_seq`; replay query takes 200ms; during that window, ext emits one envelope; A receives all replay frames followed by the one live frame in seq order | Yes | Ordering preserved; no events lost; replay_done emitted before the live frame's emit [Discharged via ICD-0.5.5 L.03 in 0.6.0.N session 6 (2026-04-28) per OQ7 absorption — same scenario verbatim; closed at `tests/kernel/realtime/live_replay_ordering_test.cpp`. No separate test case authored.] |

### Test-seam notes

- **E.\*** (10) — 8 PG-gated against `plinth_tests_pg`; E.04 + E.05
  pure (synthetic queue + mock DbClient).
- **C.\*** (7) — all PG-gated; the cursor store cannot stub
  `plinth.user_event_cursors` UPSERT meaningfully.
- **D.\*** (8) — 7 PG-gated against `plinth_tests_pg`; D.04 pure
  (frame parser only).
- **Y.\*** (9) — all PG-gated.
- **K.\*** (4) — 3 PG-gated; K.04 pure (mock DbClient).
- **I.\*** (3) — all PG-gated; full WS broker + listener + writer
  + cursor + replay path.
- New test fixture `EventsTestFixture` — manages writer + cursor
  store + cleanup task lifetime per test case; teardown calls
  `stop_events_writer()` + truncates `plinth.events` +
  `plinth.user_event_cursors`.
- WS test seams from 0.5.2 (`broker::dispatch_for_test`) reused for
  D.\* and I.\*; the `since_seq` field is parsed by the existing
  frame parser with the new branch.
- Cursor advance in delivery callback exercised by E.01 only at
  unit level; integration coverage in I.01.

### CI wiring

- `migrations/schema.sql` — `plinth.events` table DDL +
  `plinth.user_event_cursors` table DDL + three indexes.
- `src/kernel/realtime/events_writer.{hpp,cpp}` — new TU.
- `src/kernel/realtime/cursor_store.{hpp,cpp}` — new TU.
- `src/kernel/realtime/replay.{hpp,cpp}` — new TU.
- `src/kernel/realtime/broker.cpp` — extended (delivery callback
  hook + subscribe-frame replay branch).
- `src/kernel/realtime/listener.cpp` — extended (registers
  events_writer handler at startup).
- `src/kernel/ws/frame_handler.{hpp,cpp}` — extended (parser
  branch for `since_seq`; emitters for `resync`, `replay`,
  `replay_done`).
- `src/kernel/scheduled_tasks/cleanup_events.{hpp,cpp}` — new TU.
- `src/kernel/config.{hpp,cpp}` — `Config::Realtime::Events`
  substruct + parser extension.
- `src/kernel/main.cpp` — atexit chain entry +
  `start_events_writer` startup call.
- `tests/kernel/realtime/events_writer_test.cpp` — new TU.
- `tests/kernel/realtime/cursor_store_test.cpp` — new TU.
- `tests/kernel/realtime/replay_test.cpp` — new TU.
- `tests/kernel/ws/delta_sync_test.cpp` — new TU.
- `tests/kernel/scheduled_tasks/cleanup_events_test.cpp` — new TU.
- `tests/integration/events_replay_integration_test.cpp` — new TU.
- `tests/fixtures/events_test_fixture.{hpp,cpp}` — new fixture.
- `CMakeLists.txt` — explicit test-source enumeration extended for
  the six new TUs + fixture.

### Test count target

**41 new cases.** Full suite grows by 41 TEST_CASEs distributed
across `plinth_tests_pure` + `plinth_tests_pg`. No new subprocess
count; no new ctest entry beyond the existing
`plinth_tests_pure` / `plinth_tests_pg` split.

---

## Entry / Exit

**Entry criteria:**

- v0.5.0 merged + tagged (done — commit `f3552b3`, tag `v0.5.0`).
- v0.5.1 merged + tagged (done — commit `90d37fc`, tag `v0.5.1`).
- v0.5.2 merged + tagged (done — commit `68298b4`, tag `v0.5.2`).
- v0.5.3 merged + tagged (done — commit `83cea76`, tag `v0.5.3`).
- 0.5.3.1 db.batch B.06 + SC3 follow-ups merged (done —
  `feat/0.5.3.1-batch-timeout-sc3` 2026-04-24).
- ICD-0.5.4 authored (this document, squash-merged as 0.5.3.N
  paper slot).
- Listener subsystem `EventHandler` registration seam shipped (done
  — v0.5.0).
- Broker `dispatch_to_subscribers` + per-channel RBAC rule resolver
  shipped (done — v0.5.2).
- WS frame parser / emitter shipped (done — v0.1.6).
- `plinth.users` table shipped (done — v0.1.2; cursor table
  references it).
- Drogon DbClient pool + `execSqlCoro` shipped (done — pre-0.3.x).

**Exit criteria:**

- `migrations/schema.sql` ships `plinth.events` + `plinth.user_event_cursors`
  + three indexes.
- `src/kernel/realtime/events_writer.{hpp,cpp}` TU ships and
  registers an EventHandler against the listener.
- `src/kernel/realtime/cursor_store.{hpp,cpp}` TU ships with
  `record_delivered` / `read_cursor` / `reset_cursor` API + cache.
- `src/kernel/realtime/replay.{hpp,cpp}` TU ships with paginated
  query + per-channel RBAC re-check + row-cap truncation.
- `src/kernel/realtime/broker.cpp` extended with delivery-completed
  cursor advance + subscribe-frame replay branch.
- `src/kernel/ws/frame_handler.{hpp,cpp}` extended with `since_seq`
  parse + `resync` / `replay` / `replay_done` emitters.
- `src/kernel/scheduled_tasks/cleanup_events.{hpp,cpp}` TU ships
  with advisory-lock-gated DELETE sweep.
- `Config::Realtime::Events` substruct + config-loader support with
  bound-check + consistency-check validation.
- `main.cpp` atexit chain + startup wired.
- All 41 E/C/D/Y/K/I test cases pass; PG-gated cases skip cleanly
  when PG env absent.
- `run-clang-tidy-20` zero findings on new TUs + modified TUs.
- No regressions on the v0.5.0 + v0.5.1 + v0.5.2 + v0.5.3 + 0.5.3.1
  test matrix.
- Atexit-race validation — 20-run ctest loop sample shows zero
  teardown-race reproductions (specifically: zero in-flight-write-
  at-shutdown aborts).
- HA validation — multi-node sandbox (two kernel processes against
  same PG) handles `plinth.events` writes via the advisory-lock
  scheme without duplicates.
- `CHANGELOG.md` `v0.5.4` entry describes the events writer, cursor
  store, replay engine, reconnect handshake, cleanup task, the
  config surface, the six audit events, and every accepted OQ
  deviation.
- `docs/architecture/03-data.md §3.5` updated with a footnote
  pointing to this ICD.
- `docs/icd/ICD-0.1.6-websocket.md §Delta Sync` updated with a
  footnote crediting this ICD with the discharge.
- `docs/icd/ICD-0.5.2-ws-broker.md §Reconnect Semantics` line
  678–681 updated with a footnote crediting this ICD's hand-off.
- `docs/ROADMAP.md §0.5` line for 0.5.4 is checked.
- `v0.5.4` tag cut on the merge commit.
- Memory `project_plinth_state.md` updated to reflect 0.5.4
  shipped; next-session memory entry pointing at 0.5.5 monotonic
  seq generation (or the interim paper slot for ICD-0.5.5
  authoring per horizon rule).

---

## Open Questions

**OQ1 — Write-queue overflow policy.** The ICD pins **drop-newest**.
Alternatives:
(a) drop-oldest (FIFO eviction; older entries get the drop);
(b) backpressure (block the listener thread on enqueue when full —
unacceptable per ICD-0.5.0 §Listener Subsystem fast-handler rule);
(c) drop-newest as pinned.
**Recommendation:** (c). Rationale: under emission storms, the
queue contents are mostly recent activity; dropping NEW entries
preserves the older context that's already been buffered for
replay. Backpressure (b) is forbidden by the listener contract.
Drop-oldest (a) loses pre-existing replay context which is the
opposite of what we want. Architect: confirm or redirect to (a).

**OQ2 — Cursor opacity vs. transparency.** The ICD pins
**transparent BIGINT cursor** (client sees `last_seq` directly).
Alternatives: (a) HMAC-signed opaque token (`{seq:N, sig:...}`);
(b) encrypted opaque token; (c) transparent as pinned.
**Recommendation:** (c). Rationale: transparency simplifies the
client SDK (just store the integer; no key rotation, no signature
verification); the threat model (forged cursor reveals only
write-rate timing) is acceptable; per-channel RBAC re-check
prevents data leakage. Architect: confirm or redirect to (a) if
the threat model includes client-side enumeration of write-rate
patterns.

**OQ3 — Resync envelope reason set.** The ICD pins
**four reasons: `cursor_expired`, `row_cap`, `mismatch`,
`events_disabled`**. Alternatives: (a) collapse to a single
generic reason `replay_unavailable`; (b) the four pinned.
**Recommendation:** (b). Rationale: each reason has a different
client SDK response — `cursor_expired` triggers a full re-query;
`row_cap` triggers a chunked re-query (the client knows it's
fallen far behind); `mismatch` triggers diagnostic logging;
`events_disabled` triggers a "live-only" fallback. Distinct codes
let the SDK respond appropriately. Architect: confirm.

**OQ4 — Cursor-advance ordering relative to broker fan-out.** The
ICD pins **broker fan-out first; writer-INSERT-then-cursor-advance
second**. Alternatives:
(a) broker advances cursor on send (cursor lags PG seq generation —
the broker doesn't know the seq because the writer hasn't INSERTed
yet);
(b) cursor advances after BOTH broker send AND writer INSERT (as
pinned — the writer's `delivered_users` callback advances the
cursor with the just-INSERTed seq);
(c) cursor advances on writer INSERT (broker irrelevant).
**Recommendation:** (b). Rationale: (a) needs the seq before INSERT
which requires either a separate seq generator (out of scope for
0.5.4) or a 2-step protocol (lock-the-seq, INSERT-with-seq); (c)
makes the cursor "what's been persisted" not "what's been
delivered" — the latter is the user-visible truth. (b) is the
simplest correct option. Architect: confirm.

**OQ5 — Cursor BIGINT source: `plinth.events.seq` vs. envelope `seq`.**
The ICD pins **`plinth.events.seq` (the BIGSERIAL primary key)**,
NOT the envelope-internal `seq` field reserved at ICD-0.5.0 (which
0.5.5 will populate). Alternatives: (a) wait for 0.5.5 envelope
`seq` and gate 0.5.4 on it; (b) `plinth.events.seq` as pinned;
(c) generate a new monotonic seq in the writer ahead of 0.5.5.
**Recommendation:** (b). Rationale: BIGSERIAL is monotonic per-PG
across all writers (gapless modulo cluster restart); `plinth.events.seq`
is the canonical "time at which this envelope landed in the
durable store"; once 0.5.5 generates envelope `seq`, the writer
will populate envelope `seq = plinth.events.seq` by construction
(no migration needed; the cursor stays valid). Architect: confirm.

**OQ6 — Indexing strategy for `plinth.events`.** The ICD pins
**three indexes: PK on `seq`, composite on `(channel, seq)`, B-tree
on `created_at`**. Alternatives:
(a) PK only (fewest indexes; replay query falls back to seq scan +
WHERE channel filter — slow on large tables);
(b) the three pinned;
(c) extra index on `payload->>'extension'` (JSONB GIN) for
extension-scoped queries — out of scope; `plinth.events` is not
queried by extension today.
**Recommendation:** (b). Rationale: the replay query's WHERE
clause shape matches `(channel, seq)` exactly; the cleanup query's
WHERE clause matches `(created_at)` exactly. Each query gets its
optimal index. Three indexes' write overhead is acceptable for the
event-log workload (1000 writes/s × 3 index updates = 3000 index
operations/s — well within PG capacity). Architect: confirm.

**OQ7 — Multi-node writer coordination.** The ICD pins
**per-envelope advisory lock keyed on `(channel, emitted_at)`**.
Alternatives:
(a) operator-config single-writer node (`writer_node_id`);
(b) advisory lock as pinned;
(c) PG-side dedup constraint (`UNIQUE (channel, emitted_at)`) —
rejects duplicate inserts; lock contention pushes to the next
node's INSERT attempt — lossy if two nodes both fail under
contention.
**Recommendation:** (b). Rationale: (a) is operator config drift
risk on a critical path; (c) creates a constraint that's not
guaranteed unique (two envelopes can legitimately emit on the
same channel within 1 ms — millisecond-precision timestamps may
collide); (b) is HA-correct without operator config. Architect:
confirm or redirect to (a) if simpler operationally.

**OQ8 — Cursor granularity: per-user vs. per-(user, channel).** The
ICD pins **per-user single cursor**. Alternatives:
(a) per-(user, channel) — one cursor per subscription;
(b) per-user as pinned.
**Recommendation:** (b). Rationale: (a) blows up storage by the
average channel-fan-out (10–100× per active user), and the typical
reconnect pattern resubscribes the same channel set — per-channel
granularity is YAGNI until a real driver appears (e.g. a use case
where a user subscribes to channel A, drops, channel A reaches
rate ceiling, user wants a "skip-A-replay-only" reconnect — not a
0.5.4 requirement). The per-user model serves the "just resume
where I left off" pattern correctly. Architect: confirm.

---

## Appendix: Resolved Open Questions

| OQ | Decision | Rationale | Source |
|----|----------|-----------|--------|
| OQ1 | Drop-newest queue overflow | Preserve buffered context; backpressure forbidden per listener contract | §Write queue |
| OQ2 | Transparent BIGINT cursor | Threat model accepts; per-channel RBAC re-check prevents data leak | §Security Constraint 2 |
| OQ3 | Four-reason resync set | Distinct SDK responses per reason | §Server frame additions |
| OQ4 | Cursor advance after writer INSERT (not broker send) | Simplest correct ordering | §When `record_delivered` fires |
| OQ5 | `plinth.events.seq` (BIGSERIAL PK) as cursor | Monotonic; 0.5.5 envelope `seq` will equal it by construction | §Out of scope — **Discharged 2026-04-26 (v0.5.5)** per ICD-0.5.5 §5 (writer-first topology stamps envelope `seq` from RETURNING; envelope-seq == table-seq invariant holds on both paths) |
| OQ6 | Three indexes: PK + (channel,seq) + (created_at) | Each query gets optimal index; write overhead acceptable | §Indexing |
| OQ7 | Per-envelope advisory lock for multi-node writer | HA-correct without operator config | §HA Semantics |
| OQ8 | Per-user single cursor (not per-channel) | Storage efficient; matches typical reconnect pattern | §Out of scope |

---

## Appendix A — End-to-End Example

User `alice` is subscribed to `plinth:data:ext_notes.notes`. Active
WS connection. Live activity:

**Step 1 (steady state).** Extension `notes` runs `db.exec("INSERT
INTO notes(title) VALUES('hello')")`. Coalescer 50 ms window flushes;
listener dispatches:

```json
{
  "layer": "data",
  "channel": "plinth:data:ext_notes.notes",
  "schema": "ext_notes",
  "table": "notes",
  "ops": [{"op":"insert","count":1},{"op":"update","count":0},{"op":"delete","count":0}],
  "window_ms": 50
}
```

Listener calls handler 1 (broker): broker matches alice's
subscription; queues `{type:"event", envelope:...}` on alice's WS
conn loop; appends `alice.user_id` to `delivered_users`.

Listener calls handler 2 (events_writer): writer enqueues
`(envelope, delivered_users=[alice.user_id], received_at=NOW())`
onto its loop and returns.

**Step 2 (writer loop, ~1 ms later).** Writer dequeues; populates
`envelope.emitted_at = "2026-04-25T14:30:00.123Z"`; INSERT INTO
plinth.events (channel, payload) VALUES (..., ...) RETURNING seq;
gets `seq = 4901`. Then issues
`cursor_store::record_delivered("alice.id", 4901)`. Cursor cache
records `last_seq = 4901, dirty = true`.

**Step 3 (cursor cache flush, ~1 s later or on threshold).** Cache
flushes: UPDATE plinth.user_event_cursors SET last_seq = 4901,
updated_at = NOW() WHERE user_id = 'alice.id'.

**Step 4 (alice's WS connection drops at server-side seq=4910).**
Network blip. alice's last-delivered seq is 4910; her local SDK
cursor (client-side persistence) is 4910. Server-side cursor lags
behind by up to 1 second + 50 cursor delta — last_seq stored may be
4880-ish at the moment of disconnect.

**Step 5 (alice reconnects 30 seconds later).** SDK opens new WS
conn, authenticates with her session token (`bc.user_id = alice.id`).
Server time `seq=4923` (10 events emitted while alice was offline,
plus 3 from her reconnect being slightly delayed). SDK sends:

```json
{
  "type": "subscribe",
  "channels": ["plinth:data:ext_notes.notes"],
  "since_seq": 4910
}
```

**Step 6 (broker subscribe handler).** Parses frame. `since_seq=4910`
present → reconnect path.

```sql
SELECT MIN(seq) FROM plinth.events;
-- returns: 1500 (oldest row hasn't been swept yet)
```

`since_seq=4910 > min_seq=1500` ✓ (within retention).

`server_cursor = cursor_store::read_cursor("alice.id")` returns 4880
(stale flush). `abs(4910 - 4880) = 30 < replay_max_total_rows=10000`
✓.

Register live subscription via 0.5.2 path (any envelope on
`plinth:data:ext_notes.notes` from now on queues to alice's conn).

**Step 7 (replay engine).** Audit `replay_started`. Run:

```sql
SELECT seq, channel, payload FROM plinth.events
WHERE seq > 4910 AND channel = ANY(ARRAY['plinth:data:ext_notes.notes'])
ORDER BY seq ASC LIMIT 500;
```

Returns 13 rows (seq 4911–4923). For each row, RBAC re-check passes
(alice still has the same rule). Emit 13 `replay` frames.

Loop: `last_seq=4923`. Next query: WHERE seq > 4923 returns empty.
Exit loop.

Emit `{type:"replay_done", up_to_seq:4923, row_count:13}`. Audit
`replay_completed wall_clock_ms=42`. `cursor_store::record_delivered("alice.id",
4923)`.

**Step 8 (live frame arrives during replay).** During step 7, an
extension wrote one new event at seq=4924; broker fanned it out to
alice's queue (live subs registered first). Drogon's WS conn FIFO
guarantees the replay frames flush before this live frame.

alice's SDK receives: 13 replay frames (seqs 4911–4923) → 1
replay_done → 1 live event frame (seq=4924). Cursor advances to
4924 on the live frame's delivery callback.

**Step 9 (one hour later, cleanup task ticks).** Cleanup task on
node A acquires advisory lock `pg_try_advisory_lock(<events_cleanup>)`;
runs `DELETE FROM plinth.events WHERE created_at < NOW() - INTERVAL
'3600 seconds'`. Deletes ~250000 rows. Audit `cleanup_swept
rows_deleted=250000 sweep_duration_ms=180`.

Node B's cleanup tick comes 5 seconds later (clock drift); calls
`pg_try_advisory_lock`; gets `false` (node A still holds it…
actually node A released after commit; let's say node B's tick
falls within the same `cleanup_interval_ms` window — node B's
`try_advisory_lock` returns `true` if A released, `false` if not).
Either way, no duplicate sweep.

**Timeline (steady state, localhost PG):**

- Step 1: 0–0.1 ms (listener dispatch).
- Step 2: 1–3 ms (writer INSERT + cursor cache update).
- Step 3: 1 s (cache TTL).
- Step 5: 0–10 ms (WS connect + frame parse).
- Step 6: 0.5 ms (subscribe handler).
- Step 7: 30–80 ms (replay query + frame emission for 13 rows).
- Step 8: 0–20 ms (live frame queue flush after replay frames).
- Step 9: 100–500 ms (cleanup DELETE — depends on row count).

---

## Appendix B — Config Example

Full `realtime.events` block alongside 0.5.0 / 0.5.1 / 0.5.2 / 0.5.3:

```json
{
  "db": {
    "batch": { "max_ops_per_batch": 500, "timeout_ms": 30000 },
    "search_path": { "enforce": true },
    "oid_mapping": { "enabled": true },
    "silent": { "audit_window_ms": 60000 }
  },
  "realtime": {
    "listener": { "enabled": true, "reconnect_backoff_ms": [100, 500, 2000, 5000] },
    "coalescer": { "window_ms": 50, "max_inflight_windows": 1024 },
    "broker": { "enabled": true, "max_subscriptions_per_conn": 64, "rbac_enforce": true },
    "events": {
      "enabled": true,
      "retention_seconds": 3600,
      "cleanup_interval_ms": 300000,
      "replay_max_rows_per_chunk": 500,
      "replay_max_total_rows": 10000,
      "write_queue_size": 10000,
      "shutdown_drain_ms": 5000,
      "cursor_cache_ttl_ms": 1000,
      "cursor_flush_threshold": 50,
      "audit_window_ms": 60000
    }
  }
}
```

Minimum-effective config (all defaults):

```json
{}
```

0.5.4 does not introduce any config key that MUST be set by the
operator. Defaults are safe for production.

---

## Appendix C — Schema DDL

```sql
-- Realtime (0.5.0 reservation, 0.5.4 implementation)
CREATE TABLE plinth.events (
    seq         BIGSERIAL PRIMARY KEY,
    channel     TEXT NOT NULL,
    payload     JSONB NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX events_channel_seq_idx
    ON plinth.events (channel, seq);

CREATE INDEX events_created_at_idx
    ON plinth.events (created_at);

-- Per-user reconnect cursor (0.5.4)
CREATE TABLE plinth.user_event_cursors (
    user_id     UUID PRIMARY KEY
                REFERENCES plinth.users(id) ON DELETE CASCADE,
    last_seq    BIGINT NOT NULL DEFAULT 0,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

PG ≥ 14 (per `config/docker/pg.yml` supported version pin).

---

## Appendix D — Frame Catalogue Delta

Extends ICD-0.1.6 §Frame Types. Deltas only — pre-existing frames
unchanged.

### Client → Server (extended)

```json
{
  "type": "subscribe",
  "channels": ["plinth:data:ext_notes.notes"],
  "since_seq": 4910      // OPTIONAL, NEW in 0.5.4
}
```

`since_seq` is a non-negative integer. Absent → fresh subscribe
(0.5.2 behavior unchanged). Present → reconnect path (replay query
fires).

### Server → Client (new)

```json
// One frame per replayed envelope.
{ "type": "replay", "envelope": <full envelope JSON> }

// Sent after the last replay frame in a successful replay.
{ "type": "replay_done", "up_to_seq": 4923, "row_count": 13 }

// Sent when replay cannot proceed; client must re-query.
{
  "type": "resync",
  "reason": "cursor_expired" | "row_cap" | "mismatch" | "events_disabled",
  "retention_seconds": 3600
}
```

### Server → Client (extended)

`{type:"error"}` gains two new `code` values:

- `resubscribe.invalid_since_seq` — `since_seq` is non-integer or
  negative.
- `resubscribe.events_disabled` — server config has
  `realtime.events.enabled=false` and client sent `since_seq`.

---
