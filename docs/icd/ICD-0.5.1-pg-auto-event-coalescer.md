# ICD-0.5.1-pg-auto-event-coalescer

**Traces to:** architecture/03-data.md §3.1 (Debounced Change Streams,
lines 275–318 — the 50 ms window, the 8000-byte ceiling, the
per-(schema, table) accumulation rule, the truncation-to-counts
semantics, the example envelope shape); architecture/03-data.md §3.2
(Batch Operations, lines 320–341 — `db.batch()` + `silent:true`
semantics scoped to 0.5.3, not this milestone); architecture/03-data.md
§3.3 (Four Realtime Layers — row 1 "DB events, PG `LISTEN/NOTIFY`,
auto-emitted (debounced)" is what this ICD pins); architecture/03-data.md
§3.6 (HA Realtime — each node listens; each node coalesces its own
writes); architecture/03-data.md Appendix A (Realtime Event Flow
diagram — the coalescer is the "start/extend 50ms debounce window"
box between the per-extension DB write and the PG NOTIFY emit);
ICD-0.5.0-pg-listen-notify-bridge §Channel Naming Scheme (Layer 1
`plinth:data:<schema>.<table>` generator); ICD-0.5.0 §Payload Envelope
Contract (required `layer` + `channel`, reserved `seq` / `truncated`
slots — 0.5.1 is the first caller to populate `truncated`);
ICD-0.5.0 §NOTIFY Emission Helper (the `emit_notify_async` + validation
pipeline this ICD composes on); ICD-0.5.0 §HA Semantics (verbatim
promotion); ICD-0.5.0 §Deterministic Teardown (atexit chain this ICD
splices into).
**Depends on:** ICD-0.5.0 (the emit helper + channel validator + config
substruct this ICD extends); ICD-0.3.3-async-bridge (`AsyncOp` snapshot
pattern that carries per-op identity into the detached dispatcher;
`run_db_exec_outcome` is the hook site); ICD-0.4.3-extension-schema-creation-and-migration
(extension schemas live under `ext_<extension_name>.*`, matching the
coalescer's `(schema, table)` key); ICD-0.4.4-package-install-lifecycle
(`BridgeContext::extension_name` — the identity snapshotted into the
AsyncOp at enqueue time; DISABLED / UPGRADING / UNINSTALL transitions
trigger the coalescer drain hook); ICD-0.4.5-package-lifecycle-transitions
(§Atomic Swap T2 — coalescer drain participates in the upgrade drain
window); ICD-0.1.7-audit (async audit writer + `g_audit_ready` gate —
coalescer audits are rate-limited per §Audit Events).
**Milestone:** 0.5.1 — PG auto-event coalescer. Second 0.5.x code
milestone; the primary in-kernel caller of 0.5.0's
`emit_notify_async`. Paired ICD authoring slot `0.5.0.5` precedes
this code work per METHODOLOGY §3.1 forward ICD presence rule and
`feedback_icd_horizon.md`.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** [src/kernel/realtime/emit.hpp](../../src/kernel/realtime/emit.hpp)
(`emit_notify_async(DbClientPtr, Json::Value) -> Task<expected<void, NotifyError>>`
— the primitive the coalescer composes on; also
`set_max_payload_bytes` / `get_max_payload_bytes` for test fixtures);
[src/kernel/realtime/channel.hpp](../../src/kernel/realtime/channel.hpp)
(`validate_channel`, `channel_layer` — the coalescer-generated channel
name is emit-side validated here, no separate re-implementation);
[src/kernel/js/run_on_context.cpp:419–445](../../src/kernel/js/run_on_context.cpp)
(`run_db_exec_outcome` — the DB_EXEC detached-task body where the hook
lives; post-`execSqlCoro`, pre-`co_return out`);
[src/kernel/js/async_op.hpp:54–98](../../src/kernel/js/async_op.hpp)
(`AsyncOp::Type::DB_EXEC`, `silent` bool reserved for 0.5.x realtime
hook per 0.3.3 — this ICD wires it + adds the extension-identity
snapshot field);
[src/kernel/config.hpp:67–77](../../src/kernel/config.hpp)
(`Config::Realtime` substruct extended with `.coalescer`);
[src/kernel/config.cpp:83–114](../../src/kernel/config.cpp)
(`apply_realtime` helper extended with coalescer-block parsing);
[src/kernel/main.cpp:250–258](../../src/kernel/main.cpp) (atexit
chain — new `realtime::CoalescerRegistry::shutdown()` inserts between
`realtime::stop_listener()` and `drogon::app().quit()`);
[src/kernel/main.cpp:286–294](../../src/kernel/main.cpp) (startup
order — new `realtime::CoalescerRegistry::start(cfg.realtime.coalescer)`
after `realtime::start_listener` and before route registration);
[src/kernel/packages/install_lifecycle.cpp](../../src/kernel/packages/install_lifecycle.cpp)
(DISABLED / UPGRADING / UNINSTALL transitions — coalescer drain hook
call sites);
[docs/architecture/03-data.md §3.1–§3.3 + Appendix A](../architecture/03-data.md)
(normative prose promoted to contract).

---

## Overview

0.5.1 lands the **database-layer auto-event coalescer**: the kernel
component that intercepts every extension-scoped DB write, accumulates
those writes in a per-(schema, table) 50 ms debounce window, and emits
ONE Layer 1 NOTIFY per window via 0.5.0's
`plinth::realtime::emit_notify_async`. It is the primary in-kernel
caller of the emit helper and the component that earns the
"LISTEN/NOTIFY bus is useful by default" claim in
architecture/03-data.md §3.1. Every 0.5.x milestone after this —
0.5.2 WS broker (fans out the coalescer's envelopes to subscribed
clients), 0.5.4 `plinth.events` persistence (writes the envelopes for
reconnect delta sync), 0.5.5 monotonic `seq` + client-side debounce
protocol (`seq` generated post-coalescer) — composes on the envelope
contract this ICD pins.

The coalescer generalizes the pattern the capability-registry Tier 2
channel uses today (`registration.cpp:67–95` send_notify — one emit
per cache mutation): instead of one NOTIFY per DB write, coalescer
buckets writes by `(schema, table)`, opens a 50 ms window on the
first write in each bucket, and flushes one consolidated envelope
when the window closes. The window does NOT extend on subsequent
writes — a fixed 50 ms from first-write guarantees bounded emit
latency and simpler reasoning about flush ordering.

**Scope:**

- Per-process `plinth::realtime::CoalescerRegistry` subsystem — a
  singleton owning the `(schema, table) → WindowState` map, a
  dedicated `trantor::EventLoopThread` driving the window timers, and
  the atexit drain barrier.
- SQL-classification seam: qualified single-table `INSERT / UPDATE /
  DELETE` parser that yields `(schema, table, op_kind)` or skips
  (unparseable → passthrough; see §SQL Classification).
- Write-path integration at `run_db_exec_outcome` (run_on_context.cpp)
  — after `execSqlCoro` succeeds and before the JS promise resolves,
  the coalescer's `record_write` is called with the op's classified
  `(schema, table, op_kind, row_count)`.
- Envelope assembly per ICD-0.5.0 §Payload Envelope Contract — 0.5.1
  populates `layer="data"`, `channel`, `schema`, `table`, `ops`, and
  the new `window_ms` field; `truncated` is populated when the
  heuristic fires.
- Truncation heuristic — serialize, measure against
  `config.realtime.notify.max_payload_bytes` (default 8000), drop
  `ids` (absent in 0.5.1; see §OQ4 for architect review), fall back
  to counts-only + `truncated:true`.
- Config surface extension — `realtime.coalescer.{enabled, window_ms}`.
- Audit events — `realtime.coalescer.flush_failed`,
  `realtime.coalescer.shutdown_drain`; no per-flush audit (firehose).
- Extension-lifecycle drain hook — DISABLED / UPGRADING / UNINSTALL
  transitions force-flush open windows owned by that extension before
  proceeding.
- Deterministic teardown — `CoalescerRegistry::shutdown()` enters the
  atexit chain between `realtime::stop_listener()` and
  `drogon::app().quit()`. Drains open windows synchronously.
- AsyncOp extension-identity snapshot — the DB_EXEC hook needs
  `bc.extension_name` at dispatch time but `bc` is not in scope in
  `run_db_exec_outcome` (`AsyncOp` is value-copied into the detached
  task per ICD-0.3.3 §Detached Task Ownership). A new field
  `AsyncOp::bc_extension_name` is populated at enqueue time in
  `db_bindings.cpp`, mirroring the 0.3.4 `cap_user` snapshot pattern.
- Thirteen C.\* (coalescer state machine), four T.\* (truncation), five
  I.\* (end-to-end integration), four E.\* (error paths) test cases
  across `tests/kernel/realtime/coalescer_test.cpp` (new) and
  `tests/kernel/realtime/coalescer_integration_test.cpp` (new). **26
  new cases total.**

**Out of scope (deferred, with ICD pointers):**

- **`db.batch()` + `silent: true` suppression.** ROADMAP L113, 0.5.3
  scope. The `silent` bool is already reserved on `AsyncOp` (line 74);
  0.5.1 does NOT consume it — the coalescer intercepts every non-DDL
  DB_EXEC. 0.5.3 will wire `silent == true` → skip `record_write`.
- **Monotonic `seq` generation.** ~~0.5.5.~~ **Discharged 2026-04-26
  (v0.5.5)** per
  [`docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md`](ICD-0.5.5-sequence-numbers-client-debounce.md)
  §5: writer-first topology stamps `envelope["seq"]` from the
  writer's `INSERT … RETURNING seq` clause; `plinth.events.seq`
  BIGSERIAL is the canonical source. Layer-1 envelopes (coalescer
  emits) and Layer-2/3 envelopes (kernel + extension) all carry
  `seq` after 0.5.5 ship. The coalescer's `flush_snapshot` path is
  unchanged; the writer stamps `seq` after the INSERT before the
  broker's downstream fan-out.
- **`plinth.events` persistence + delta sync.** 0.5.4. 0.5.1
  envelopes fire-and-forget via `emit_notify_async` — if the
  PG `LISTEN/NOTIFY` backbone drops, the event is lost (matches PG's
  best-effort NOTIFY semantics). 0.5.4 adds the persistence tier;
  reconnect resync is 0.5.4 scope.
- **Client-side debounce, smart re-query, jitter.** architecture/03-data.md
  §3.4. Client SDK layer, 0.6.3 scope.
- **Cross-node coalescence.** Each node coalesces its own writes; if
  two nodes write to `ext_notes.notes` within 50 ms of each other,
  TWO NOTIFYs fire (one per node). Cross-node coordination is not in
  0.5.1 scope and has no concrete requirement yet (the architecture's
  HA posture is shared-primary PG — LISTEN/NOTIFY is per-instance).
- **Populated `ids` arrays.** architecture/03-data.md §3.1 shows
  `ops:[{op:"insert", count:47, ids:["a1",...]}]` with IDs. 0.5.1
  pins `ids` **always absent** — see §OQ4 for the architect call.
  Clients receiving a 0.5.1 envelope treat the missing `ids` field
  identically to a `truncated:true` envelope with elided ids: the
  SDK's optimistic-update path (architecture/03-data.md §3.4 item 2)
  is a no-op and a re-query is required. 0.5.5's `seq` + optional
  `RETURNING id` wrapping closes the gap.
- **Non-extension kernel writes.** The coalescer hooks only the
  `AsyncOp::Type::DB_EXEC` path — kernel-internal writes (schema
  bootstrap, plinth.\* table maintenance, session writes) go direct
  to the DbClient without an AsyncOp and do NOT produce coalescer
  envelopes. This is intended: Layer 1 events are extension-CRUD
  reactivity. Kernel system events use Layer 2 (`plinth:system:*`),
  emitted via direct `emit_notify_async` calls from kernel
  subsystems at their own milestones.
- **DDL events.** `CREATE TABLE`, `ALTER TABLE`, `CREATE INDEX` go
  through the DB_EXEC path too, but the SQL classifier returns
  `std::nullopt` on DDL, so no envelope fires. DDL reactivity is not
  a use case today; extension migrations are their own lifecycle.
- **Row-level event semantics.** A single `UPDATE ext_notes.notes SET
  ... WHERE id IN (...)` affecting 100 rows produces one `{op:"update",
  count:100}` entry. The coalescer does NOT emit per-row events. This
  matches architecture/03-data.md §3.1 — "change summaries", not
  row-level diffs.

---

## SQL Classification

The coalescer's first job on every DB_EXEC success is to classify the
SQL into `(schema, table, op_kind)` or decide to skip. This is a
narrow, deterministic parser — NOT a full SQL grammar. The acceptable
shapes are pinned; anything else is skip-with-debug-log.

### Supported shapes

The classifier accepts three statement-form patterns. All matching is
case-insensitive; leading / trailing whitespace and leading comments
are stripped before pattern match.

1. **INSERT (qualified)** —
   `INSERT INTO <schema>.<table> (...) VALUES (...)`
   or `INSERT INTO <schema>.<table> SELECT ...`
   → `(schema, table, "insert")`.
2. **UPDATE (qualified)** —
   `UPDATE <schema>.<table> SET ...`
   → `(schema, table, "update")`.
3. **DELETE (qualified)** —
   `DELETE FROM <schema>.<table> WHERE ...`
   → `(schema, table, "delete")`.

`<schema>` and `<table>` match the PG identifier regex
`[a-z_][a-z0-9_]*` (case-folded), consistent with ICD-0.4.3's
extension schema naming. Quoted identifiers (`"MyTable"`) are NOT
supported in 0.5.1 — extension schemas use lower_snake_case per
convention.

### Unqualified single-table fallback

`INSERT INTO notes ...`, `UPDATE notes SET ...`, `DELETE FROM notes
WHERE ...` (no schema qualifier) is a real extension-authoring
pattern — the extension's `search_path` is set to its own schema per
ICD-0.4.3, so `notes` resolves to `ext_<extension>.notes`. The
classifier resolves these to `(ext_<bc_extension_name>, <table>,
op_kind)` when `AsyncOp::bc_extension_name` is non-empty. Kernel-scope
contexts (empty `bc_extension_name`) with unqualified writes are
treated as unparseable (skip).

### Skip cases

Every other shape returns `std::nullopt` from the classifier:

- **DDL:** `CREATE TABLE`, `ALTER TABLE`, `DROP TABLE`, `CREATE
  INDEX`, etc. Extensions run their own migrations via the DDL path
  during install; DDL is out of Layer 1 scope (architecture/03-data.md
  §3.1 is about row CRUD reactivity).
- **SELECT / WITH / BEGIN / COMMIT / SET.** Read-only or transactional
  boundary; no Layer 1 semantic. (Note: DB_QUERY is a separate
  AsyncOp type — `run_db_query_outcome` — which the coalescer never
  touches. DB_EXEC dispatch can still carry `SELECT` if an extension
  author uses `db.exec` for read-only SQL; the classifier skips
  those.)
- **CTE writes** (`WITH x AS (...) INSERT INTO y ...`,
  `INSERT INTO a (SELECT FROM b WHERE ...)` with multi-table CTEs).
  PG allows writeable CTEs with multiple target tables; the classifier
  does NOT attempt to enumerate them. Skip with a debug log citing the
  unsupported shape. §OQ5 flags the potential widening.
- **Multi-statement** — multiple semicolon-separated statements in a
  single `db.exec`. The classifier examines only the first statement
  and warns if more than one is detected (debug log, not reject).
  Extensions SHOULD issue one statement per `db.exec`; multi-stmt is
  a forward-compat escape hatch we neither encourage nor deeply
  support.
- **Unparseable** — any form not matching the three supported shapes
  above. Debug log with SQL prefix (first 80 chars, truncated for
  log hygiene) + `reason="classifier_skip"`. No audit event (audit
  cost does not pay for itself on a frequently-hit skip path; see
  §Audit Events rationale).

### Defense-in-depth check

After extracting `(schema, table)`, the classifier asserts
`schema == "ext_" + bc_extension_name` OR `bc_extension_name` is
empty (kernel context). A mismatch — e.g. an extension named `notes`
writing `INSERT INTO ext_other_extension.table` — is logged at warn
and the write is **still coalesced** under the actual schema (not
the expected one). The extension-schema isolation guarantee lives in
ICD-0.4.3 (extensions cannot access other extensions' schemas via
the `search_path` enforcement + capability grants); this check is
defense-in-depth, not a substitute. A mismatch today would indicate
an ICD-0.4.3 bypass bug upstream, and the coalescer's job is to
report it, not to mask it.

### Classifier implementation latitude

The implementing session picks between:

- **Hand-rolled state-machine** — ~80 LOC, handles the three shapes
  directly, no dependency. Matches the ICD-0.5.0 channel validator's
  posture (channel.hpp:3–6 explicitly cites the hand-rolled choice).
- **`std::regex` one-pattern-per-shape** — concise but incurs ICU
  pull-in cost and makes the extraction (schema/table capture groups)
  spot-check-brittle.

Recommendation: hand-rolled. This ICD does not mandate either.

---

## Coalescer State Machine

The `CoalescerRegistry` owns one `WindowState` per `(schema, table)`
pair currently in an open window. State is keyed by
`std::pair<std::string, std::string>` (or an equivalent struct) and
stored in a `std::unordered_map` protected by `std::shared_mutex` —
reads during window flushes are many, writes (open / close) are few.

### WindowState

```cpp
struct WindowState {
    std::string      schema;
    std::string      table;
    std::chrono::steady_clock::time_point opened_at;
    // Op counters — insert / update / delete row counts accumulated
    // within this window. architecture/03-data.md §3.1 emits them
    // as three entries in the envelope's `ops` array regardless of
    // whether a given op kind had any writes in the window.
    std::size_t      insert_rows = 0;
    std::size_t      update_rows = 0;
    std::size_t      delete_rows = 0;
    // Extension that owns this window — set on the FIRST write,
    // used by the lifecycle drain hook to match windows to
    // transitioning extensions. Subsequent writes from a different
    // extension on the same (schema, table) — which should be
    // impossible per ICD-0.4.3 isolation — are logged at warn but
    // the window remains owned by the first extension.
    std::string      extension_name;
    // Timer handle for the 50 ms flush. Owned by the coalescer's
    // EventLoopThread; cancellable during shutdown drain.
    trantor::TimerId timer_id{};
};
```

### Window open / accumulate / flush

**Open (first write in bucket).** `record_write(schema, table,
extension_name, op_kind, row_count)`:

1. Take the registry write lock.
2. Check for an existing `WindowState` at `(schema, table)`.
3. If none, construct one: `opened_at = steady_clock::now()`, counters
   zeroed, `extension_name` set, timer scheduled.
4. Schedule the flush timer: `coalescer_loop.runAfter(
   config.realtime.coalescer.window_ms, [schema, table] {
   registry.flush_window(schema, table); })`.
5. Apply the op: increment `insert_rows` / `update_rows` /
   `delete_rows` by `row_count`.
6. Release the write lock.

**Accumulate (subsequent writes, same bucket).** `record_write` with
an existing `WindowState`:

1. Take the write lock.
2. Increment the appropriate `*_rows` counter by `row_count`.
3. If `extension_name` differs from the window's owner — log warn,
   do NOT change window ownership.
4. **Do NOT reschedule the timer.** The window's duration is fixed
   from `opened_at`; late writes do not extend it.
5. Release the write lock.

**Flush (timer fires, or forced drain).** `flush_window(schema, table)`:

1. Take the write lock.
2. Look up the `WindowState`. If absent — already flushed or drained;
   no-op.
3. Extract the accumulated counters + extension identity.
4. Erase the entry from the map.
5. Release the write lock.
6. Build the envelope per §Envelope Assembly.
7. Call `emit_notify_async(drogon::app().getDbClient(), envelope)`.
8. On error: audit `realtime.coalescer.flush_failed` with the
   envelope's channel + the `NotifyError` code. The event is lost;
   no retry (best-effort delivery per §Error Model).

### No timer extension — rationale

The architecture/03-data.md §3.1 prose says "First write to table X
starts a coalescing window (configurable, default 50 ms). Subsequent
writes within the window are accumulated." The prose does NOT say
the window extends. Pinning fixed-duration-from-first-write gives:

- Bounded emit latency — 50 ms p100 from the first write in any
  window, not "50 ms after the last write in a long burst".
- Simpler reasoning for the coalescer drain on teardown — every open
  window has a known remaining duration.
- Predictable test behavior — C.\* cases can wait exactly
  `window_ms + ε` to observe flushes.

Extending the window on every write is the classic debounce pattern,
but it trades emit latency for coalescer efficiency; architecture
explicitly budgets the 50 ms as "effectively immediate" for the
single-write common case. Fixed-duration honors that budget.

### Concurrency posture

- `record_write` runs on an arbitrary IO thread (wherever the
  detached `run_db_exec_outcome` coroutine was scheduled). It takes
  the registry write lock briefly and returns.
- `flush_window` runs on the coalescer's dedicated
  `trantor::EventLoopThread`. It takes the registry write lock to
  claim the `WindowState`, releases before calling `emit_notify_async`
  (which itself dispatches to a Drogon DbClient executor).
- `shutdown_drain` runs on the atexit thread (main thread after
  `drogon::app().quit()` request is queued). It cancels every
  outstanding timer and force-flushes every open window synchronously
  — see §Deterministic Teardown.

---

## Envelope Assembly

The coalescer builds envelopes that match ICD-0.5.0 §Payload Envelope
Contract — every 0.5.1 Layer 1 envelope is a valid 0.5.0 envelope.
The emit helper's validation pipeline accepts them unchanged.

### Shape

```json
{
  "layer":     "data",
  "channel":   "plinth:data:<schema>.<table>",
  "schema":    "<schema>",
  "table":     "<table>",
  "ops":       [
    {"op": "insert", "count": <nonneg_int>},
    {"op": "update", "count": <nonneg_int>},
    {"op": "delete", "count": <nonneg_int>}
  ],
  "window_ms": 50,
  "truncated": <boolean — see §Truncation Heuristic>
}
```

### Field semantics

- **`layer`** — always the literal string `"data"`. ICD-0.5.0's
  layer↔channel consistency check requires this.
- **`channel`** — generated as `"plinth:data:" + schema + "." +
  table`. Must pass `plinth::realtime::validate_channel()` per
  ICD-0.5.0 §Channel Naming Scheme. Both `schema` and `table` are
  already constrained to `[a-z][a-z0-9_]*` by the classifier, so the
  regex match is guaranteed.
- **`schema`** + **`table`** — duplicate of the channel's information,
  included as separate fields because architecture/03-data.md §3.1's
  example envelope carries them explicitly and client SDKs will
  switch on them directly (parsing the channel string is extra work).
- **`ops`** — always three entries: insert / update / delete, even
  when a given op kind had zero writes in the window. A zero-count
  entry is cheap (about 20 bytes) and makes the client-side JSON
  decoder's life easier — no optional-field branches. The three
  entries appear in a fixed order: `insert`, `update`, `delete`.
- **`window_ms`** — the coalescer's configured window, echoed so
  clients that want to back-calculate event times from
  `emitted_at` + `window_ms` (0.5.4 may add that; 0.5.1 does not
  populate `emitted_at`).
- **`truncated`** — absent when the envelope fits under
  `config.realtime.notify.max_payload_bytes`; `true` when the
  heuristic fires (see §Truncation).

### Fields absent in 0.5.1

- **`ids`** (per-op `ids` array per architecture §3.1 example) — §OQ4.
  Clients treat missing `ids` identically to a `truncated:true`
  envelope: re-query.
- **`seq`** — 0.5.5.
- **`emitted_at`** — ICD-0.5.0 §Payload Envelope reserves this field;
  0.5.1 does not populate. 0.5.4's persistence tier may add on the
  write path.
- **`payload`** — Layer 2 / Layer 3 concept; never present in Layer 1
  envelopes.

### Zero-row writes

An `UPDATE ... WHERE nomatch` or `DELETE FROM ... WHERE id=<nonexistent>`
runs through `execSqlCoro` and returns a `Result` with
`affectedRows() == 0`. The hook calls `record_write(..., row_count=0)`;
the accumulator increments by zero, which is a no-op on the counters.
**But** the window has already been opened by the classifier call. If
no further writes arrive in 50 ms, the window flushes with all three
counters at zero — `ops:[{insert:0},{update:0},{delete:0}]`. This is
an unwanted firehose (every `WHERE nomatch` write produces a NOTIFY).

**Pinned behavior:** `record_write` with `row_count == 0` returns
**without opening a window** and without incrementing counters if the
bucket is empty. If the bucket is already open (a nonzero write
already landed), the zero-row write is silently dropped — no new
state change. This closes the zero-row firehose and matches the
architecture intent ("CRUD reactivity" — no rows changed means no
reactivity).

### Envelope assembly time

The `ops` array is assembled at flush time from the snapshotted
`WindowState` counters. Order of accumulation within the window does
NOT affect the envelope — the coalescer emits summary counts, not an
event log. A burst of 20 inserts followed by 5 updates produces the
same envelope as 5 updates followed by 20 inserts: the order of the
`ops` array is fixed (insert / update / delete) and the counts are
the same.

---

## Truncation Heuristic

Every coalescer envelope passes through a size check before emission.
In 0.5.1 — with `ids` always absent and counters represented as
reasonable non-negative integers — envelope sizes are bounded by
channel length + the three ops entries, which sums to **well under
200 bytes** for every realistic `(schema, table)` name. The
truncation path is therefore **rare in 0.5.1** but pinned for two
reasons: (1) §OQ4 may flip `ids` to populated, at which point
truncation becomes load-bearing; (2) the client-side contract that
`truncated:true` means "re-query, the counts are still authoritative"
is written once here and stable across 0.5.1 → 0.5.5.

### Pipeline

1. Assemble the full envelope per §Envelope Assembly (with `ids`
   populated if §OQ4 flips; always absent in 0.5.1).
2. Serialize compact JSON (no whitespace) — this is what `emit_notify`'s
   validator will re-serialize anyway; the coalescer's pre-check
   avoids a PG round-trip on oversized payloads.
3. If `size <= config.realtime.notify.max_payload_bytes`: emit as-is.
4. If oversized and `ids` is present: remove every `ids` array. Set
   `truncated = true`. Re-serialize.
5. If still oversized after step 4 (`ids` was already absent, or
   counts-only envelope still over ceiling — extremely unlikely in
   0.5.1): log warn, audit `realtime.coalescer.flush_failed` with
   `reason="payload_too_large"`, drop the envelope. The emit helper
   would reject with `PAYLOAD_TOO_LARGE` anyway; the coalescer
   short-circuits to save the PG round-trip and emit its own audit.

### Re-emit contract

`truncated:true` is the client's signal that the envelope's `ops`
counts are authoritative but per-row IDs (if they were expected) are
not — the client should re-query affected data. architecture/03-data.md
§3.4 item 2's "optimistic local update" path is a no-op on a
truncated envelope.

### Ceiling source

The 8000-byte ceiling is PG's `NOTIFY` payload wire limit. 0.5.0's
config exposes `realtime.notify.max_payload_bytes` for lowering
(never raising — the config loader rejects `> 8000`). 0.5.1 reuses
this field verbatim; no new coalescer-specific ceiling.

---

## Registry + Integration Point

### Public API

```cpp
// src/kernel/realtime/coalescer.hpp
#pragma once

#include "kernel/config.hpp"
#include <cstdint>
#include <string>
#include <string_view>

namespace plinth::realtime {

enum class OpKind : std::uint8_t { INSERT, UPDATE, DELETE };

class CoalescerRegistry {
  public:
    // Process singleton. Thread-safe — lazy-init on first access,
    // idempotent. Matches the plinth::log::audit and
    // capabilities::resolver precedents.
    static auto instance() -> CoalescerRegistry&;

    // Start the dedicated event-loop thread for window timers. Called
    // once from main.cpp at startup after realtime::start_listener.
    // Idempotent — second call while running is a no-op. When
    // `cfg.enabled == false`, start() returns without spawning the
    // loop and subsequent record_write calls short-circuit.
    auto start(const Config::Realtime::Coalescer& cfg) -> void;

    // Cancel every outstanding window timer, force-flush every open
    // window synchronously, join the event-loop thread. Called from
    // main.cpp's atexit chain between realtime::stop_listener() and
    // drogon::app().quit(). Safe to call multiple times. Blocks until
    // all flushes complete or per-flush emit timeout elapses
    // (§Deterministic Teardown).
    auto shutdown() -> void;

    // Record an extension-scoped DB write. Called from
    // run_db_exec_outcome after a successful execSqlCoro. The
    // schema/table/op_kind tuple is the classifier's output;
    // row_count is r.affectedRows() from the Drogon Result. The
    // extension_name identifies the window owner for lifecycle
    // drain matching. row_count == 0 is a no-op if the bucket is
    // empty; otherwise the counters do not change but the existing
    // window is untouched (§Envelope Assembly Zero-row writes).
    auto record_write(std::string_view schema,
                      std::string_view table,
                      OpKind           op_kind,
                      std::size_t      row_count,
                      std::string_view extension_name) -> void;

    // Lifecycle drain hook — DISABLED / UPGRADING / UNINSTALL
    // transitions in install_lifecycle.cpp call this before
    // proceeding. Synchronously flushes every open window whose
    // owner matches `extension_name`. Returns after every matched
    // window has been flushed (or drop-audit'd on emit failure).
    // Running windows for other extensions are untouched. Idempotent.
    auto drain_extension(std::string_view extension_name) -> void;

    // Test seam — fire a window flush directly, bypassing the timer.
    // Used by C.* cases to sidestep real-clock waits while still
    // exercising the flush+emit path end-to-end. Returns true if a
    // window was flushed; false if no window existed for the key.
    auto apply_flush_for_test(std::string_view schema,
                              std::string_view table) -> bool;

    // Test seam — count of currently-open windows. Used by the drain
    // tests to assert that a shutdown barrier actually drained.
    auto open_window_count_for_test() const -> std::size_t;

  private:
    // implementation detail — see §Coalescer State Machine.
};

}  // namespace plinth::realtime
```

### Integration in `run_db_exec_outcome`

**Hook location.** The hook lives at the end of `run_db_exec_outcome`
(run_on_context.cpp:419–445), after the `co_await db->execSqlCoro`
(line 430 or 432 depending on the params branch) completes
successfully. Specifically, after line 435
(`out["row_count"] = ...`) and before line 436 (`co_return out;`),
the hook inserts:

```cpp
// ICD-0.5.1 §Registry + Integration Point. Classifier may return
// std::nullopt (skip); record_write itself tolerates row_count==0
// (no-op on empty bucket).
if (auto sql_class = classify_sql(op.sql);
    sql_class.has_value() && !op.silent) {
    CoalescerRegistry::instance().record_write(
        sql_class->schema, sql_class->table, sql_class->op_kind,
        r.affectedRows(), op.bc_extension_name);
}
```

**Why this position, not the switch in `dispatch_async_op_detached`.**

- `run_db_exec_outcome` is the single choke-point for DB_EXEC success
  (the exception paths at lines 437–444 return early without Result;
  no envelope should fire on failure).
- `r.affectedRows()` is only available here.
- The switch (run_on_context.cpp:596–642) dispatches by `AsyncOp::Type`
  — hooking there means duplicate logic for DB_EXEC vs (0.5.3's)
  `DB_QUERY_WITH_WRITE`. Hooking inside the DB_EXEC outcome keeps
  the coalescer coupled only to DB_EXEC.
- `op.bc_extension_name` is a new field on `AsyncOp` (see §AsyncOp
  extension-identity snapshot below) — populated in
  `db_bindings.cpp`'s `db.exec` binding where `bc` is in scope.

### AsyncOp extension-identity snapshot

Extends `src/kernel/js/async_op.hpp` AsyncOp struct with one new field:

```cpp
// ICD-0.5.1 §Registry + Integration Point — extension identity
// snapshotted at enqueue time for the coalescer's (schema, table)
// window ownership. Mirrors the 0.3.4 cap_user / cap_call_depth
// snapshot pattern — detached task reads the value off the op
// rather than reaching back into bc. Empty for kernel-scope
// DB_EXEC (no coalescence intended there).
std::string bc_extension_name;
```

Populated at `db.exec` binding enqueue time in
`src/kernel/js/stdlib/db_bindings.cpp` (the file already reads
`bc.extension_name` — one additional assignment). DB_QUERY
(read-only) does not populate the field; coalescer never sees query
ops.

### Startup + shutdown wiring

In `src/kernel/main.cpp`:

**Startup** — after `realtime::start_listener(cfg.db)` (around line
290 of the 0.5.0 shape) and before `register_healthz()`:

```cpp
plinth::realtime::CoalescerRegistry::instance()
    .start(cfg.realtime.coalescer);
```

**Atexit** — inside the existing atexit lambda around lines 250–258,
between `realtime::stop_listener()` and `drogon::app().quit()`:

```cpp
plinth::realtime::CoalescerRegistry::instance().shutdown();
```

Ordering rationale: the coalescer's flush path calls
`emit_notify_async`, which needs the Drogon DbClient pool alive.
Shutdown must run before `drogon::app().quit()`. It must also run
before any other subsystem that might inject a final write through
`run_db_exec_outcome` (none exist today, but the principle is
"coalescer stops accepting new writes before the DB pool closes").
It runs **after** `realtime::stop_listener()` purely for symmetry —
start order is listener-then-coalescer, stop order reverses.

### Extension-lifecycle integration

`install_lifecycle.cpp`'s DISABLED / UPGRADING / UNINSTALL state
transitions gain one pre-transition call:

```cpp
plinth::realtime::CoalescerRegistry::instance()
    .drain_extension(pkg.name);
```

This runs BEFORE the state row is updated in
`plinth.package_registrations` (so any coalesced NOTIFY the client
receives is the last one it will ever see from this extension under
this version) and BEFORE capability rows are deleted (so the NOTIFY's
consumer chain is still intact when the envelope fires). Matches
the 0.4.5 §Atomic Swap T2 drain-window posture — coalescer drain
participates in the drain budget, not in addition to it.

---

## Config Surface

Extends the `Config::Realtime` substruct at `src/kernel/config.hpp:67–77`:

```cpp
struct Realtime {
    struct Listener { /* unchanged 0.5.0 shape */ };
    struct Notify   { /* unchanged 0.5.0 shape — max_payload_bytes reused */ };
    struct Coalescer {
        bool        enabled   = true;
        std::size_t window_ms = 50;
    } coalescer;
    /* ...listener + notify as before... */
};
```

### Config JSON

```json
{
  "realtime": {
    "listener":  { "enabled": true, "reconnect_backoff_ms": 1000 },
    "notify":    { "max_payload_bytes": 8000 },
    "coalescer": { "enabled": true, "window_ms": 50 }
  }
}
```

### Field semantics

| Key | Default | Semantics |
|-----|---------|-----------|
| `realtime.coalescer.enabled` | `true` | When `false`, `record_write` short-circuits: no envelope is emitted, no window is opened. Equivalent to 0.5.3's `silent:true` applied globally — useful for legacy extensions or deployments that opt out of Layer 1 reactivity entirely. Does NOT affect Layer 2 / Layer 3 emits. |
| `realtime.coalescer.window_ms` | `50` | Fixed duration from first write in a bucket to flush. Bounded `[1, 10000]` at config load time — values outside reject with `config.realtime.coalescer.window_ms_out_of_range`. Values at the extremes are supported for test fixtures (C.08 uses `window_ms=1`); production deployments should stay near the default. |

### Config loader extension

`src/kernel/config.cpp apply_realtime` gains a coalescer-block parser
mirroring the listener/notify blocks (lines 84–113):

```cpp
if (r.contains("coalescer") && r["coalescer"].is_object()) {
    const auto& c = r["coalescer"];
    if (c.contains("enabled")) {
        cfg.realtime.coalescer.enabled = c["enabled"].get<bool>();
    }
    if (c.contains("window_ms")) {
        auto ms = c["window_ms"].get<long long>();
        if (ms < 1 || ms > 10000) {
            throw std::runtime_error(
                "config.realtime.coalescer.window_ms_out_of_range: "
                + std::to_string(ms));
        }
        cfg.realtime.coalescer.window_ms =
            static_cast<std::size_t>(ms);
    }
}
```

### Config deferrals (updated from ICD-0.5.0)

Still deferred to later milestones:

- `realtime.persistence.retention_seconds` — 0.5.4.
- `realtime.broker.*` — 0.5.2.
- `realtime.coalescer.silent_mode_default` — 0.5.3 if the default-silent
  extension opt-in becomes a feature. Not reserved.

---

## Audit Events

Two new event kinds, routed through `plinth::log::audit` per
ICD-0.1.7. `g_audit_ready` gating applies.

| Event kind | Fired from | Payload |
|------------|------------|---------|
| `realtime.coalescer.flush_failed` | `flush_window` after `emit_notify_async` returns an error | `{"channel": "<plinth:data:...>", "schema": "<>", "table": "<>", "reason": "<NotifyError enum name>", "counts": {"insert": <n>, "update": <n>, "delete": <n>}}` |
| `realtime.coalescer.shutdown_drain` | `shutdown()` completion | `{"windows_drained": <n>, "flush_failures": <n>, "duration_ms": <n>}` |

### Implementation deviation (v0.5.1 ship)

The `realtime.coalescer.shutdown_drain` audit is **not wired** in
v0.5.1. Rationale: `shutdown()` runs from main's atexit chain, which
fires *after* `spdlog::shutdown()` is invoked at `main`'s return from
`drogon::app().run()` and *after* SIGTERM has nulled drogon's
`DbClientManager`. Both audit paths crash with SIGSEGV on that
lifecycle — `plinth::log::audit` dereferences the null manager via
`drogon::app().getDbClient`, and the `spdlog` fallback dereferences
the null default logger. First LH-1 trial on the v0.5.1 branch
reproduced the crash deterministically via gdb on the core dump
(`audit_shutdown_drain → log::audit → DbClientManager::find @ this=0x0`).

`shutdown()` performs the synchronous window drain exactly as the
state machine below specifies; only the post-drain audit is dropped.
Equivalent diagnostic information lives in (a) the `flush_failed`
audit (fired from the timer path while the kernel is up, reachable
whenever a flush fails mid-lifetime); (b) the `open_window_count_for_test`
test seam which asserts zero open windows post-shutdown by contract.

A future main.cpp lifecycle cleanup — moving `spdlog::shutdown` to
the end of the atexit chain, after every subsystem has logged — would
let the `shutdown_drain` audit be reinstated. That work is out of
scope for v0.5.1 and not scheduled as a dedicated follow-up (no active
consumer; observable side-channels are sufficient).

Deviation ratified in `RE-EVAL-0.5.x-following-0.5.1.md §2.1`. Code
deviation ratified in CHANGELOG v0.5.1 §Scope deviations from ICD.

### No per-flush audit

Every successful coalescer flush emits one NOTIFY; auditing every
flush would dwarf the audit log (a busy extension with 100 writes/sec
produces 20 flushes/sec ≈ 1.7M audit rows/day). The audit pipeline
is not designed for this firehose, and the PG `LISTEN/NOTIFY`
backbone already provides an observable side-channel for operational
monitoring (`pg_stat_activity` on the listener thread, NOTIFY
statistics via `pg_stat_database`). `flush_failed` is the exception
because rejections are rare and load-bearing for debugging.

### No per-skip audit

SQL classifier skip cases (§SQL Classification) fire debug logs, not
audit events. Extension authors running under dev-mode see the log
lines in stderr; production audit trail doesn't need them. A future
metrics counter (`plinth.metrics` 0.7.1+) may track skip rates per
extension for debugging "why isn't my realtime working" — out of
0.5.1 scope.

---

## HA Semantics

Verbatim promotion of ICD-0.5.0 §HA Semantics, specialized to the
coalescer:

1. **Each node coalesces its own writes.** There is no cross-node
   coordination. A write on Node A produces an envelope from Node A's
   coalescer; Node B's coalescer never sees Node A's writes.
2. **The resulting NOTIFY reaches every node.** Via 0.5.0's listener
   — every node LISTENs on `plinth:realtime`.
3. **Identical writes on two nodes within `window_ms` produce TWO
   envelopes.** A client subscribed (via the 0.5.2 broker) to
   `plinth:data:ext_notes.notes` receives both. This is the only
   correct behavior without cross-node coordination, and matches the
   "best-effort delivery + idempotent client re-query" contract —
   the client's debounce (0.6.3 SDK) absorbs the duplicate into one
   re-query regardless.
4. **Ordering.** PG preserves NOTIFY order on a single connection;
   cross-connection ordering is undefined (ICD-0.5.0 §HA item 4).
   0.5.5 `seq` closes the client-visible ordering gap when that
   milestone lands.
5. **Split-brain posture.** Unchanged from ICD-0.5.0 §HA item 5 —
   PG-operational concern.

---

## Relationship to 0.5.0 Listener + `emit_notify_async`

0.5.0 established two halves of the bus:

- **Emit side:** `emit_notify`, `emit_notify_async` — any kernel
  caller can emit a validated envelope.
- **Receive side:** the `plinth::realtime::listener` — LISTEN on
  `plinth:realtime`, dispatch to registered handlers.

0.5.1 adds the **primary in-kernel emit caller** — the coalescer.
The listener is unchanged; it receives coalescer-produced envelopes
identically to any other Layer-1/2/3 envelope and passes them to
registered handlers. The 0.5.2 broker will be the first handler to
care about Layer-1 envelopes (subscribing clients to the Layer-1
channels); 0.5.4's persistence tier will be the second (writing
every Layer 1/2/3 envelope into `plinth.events`).

0.5.1's coalescer does NOT add any handler. It is a pure emit-side
component. The listener's fan-in / registered-handler dispatch runs
on coalescer-produced NOTIFYs as they round-trip through PG, exactly
as architecture/03-data.md Appendix A sketches.

---

## Deterministic Teardown

Per `feedback_deterministic_teardown.md`, the coalescer owns
framework-adjacent state (a `trantor::EventLoopThread` + scheduled
timers + shared mutable state) and therefore needs a `cancel_all`
entry in the atexit chain.

### Atexit chain (updated from 0.5.0)

```cpp
// BEFORE (0.5.0)
std::atexit([] {
    plinth::packages::asset_server::cancel_all_registrations();
    plinth::ws::ConnectionRegistry::instance().cancel_all_timers();
    plinth::ws::ConnectionRegistry::initiate_shutdown();
    plinth::log::shutdown();
    plinth::capabilities::stop_notify_listener();
    plinth::realtime::stop_listener();
    plinth::ws::shutdown_js_stress_pool();
    drogon::app().quit();
});

// AFTER (0.5.1)
std::atexit([] {
    plinth::packages::asset_server::cancel_all_registrations();
    plinth::ws::ConnectionRegistry::instance().cancel_all_timers();
    plinth::ws::ConnectionRegistry::initiate_shutdown();
    plinth::log::shutdown();
    plinth::capabilities::stop_notify_listener();
    plinth::realtime::stop_listener();
    plinth::realtime::CoalescerRegistry::instance().shutdown();
    plinth::ws::shutdown_js_stress_pool();
    drogon::app().quit();
});
```

### shutdown() semantics

1. Set a `std::atomic<bool>` `shutting_down_` flag — `record_write`
   from in-flight detached tasks short-circuits without opening new
   windows (existing windows are still flushable).
2. Cancel every outstanding `trantor::TimerId` on the coalescer loop.
3. Iterate the `(schema, table) → WindowState` map; force-flush every
   entry synchronously via `flush_window(schema, table)` (same code
   path as the timer fire — emits via `emit_notify_async` +
   audit-on-failure).
4. Block until every emit completes or a per-flush timeout
   (default 2 seconds, not currently configurable) elapses. If the
   timeout elapses on a flush, audit `flush_failed` with
   `reason="shutdown_timeout"` and move on.
5. Quit the event-loop thread; join.
6. Emit one `realtime.coalescer.shutdown_drain` audit summary.

### Why synchronous drain

The alternative (drop open windows at shutdown) would lose every
in-progress Layer 1 event and force clients to re-query after any
deploy — unacceptable UX. Synchronous drain caps emit latency at
~50 ms (one window duration) per open bucket at shutdown; with
typical production bucket counts (O(10) open windows), the drain
completes in well under 500 ms. The ws_test_fixture.cpp atexit mirror
(per ICD-0.5.0 §Deterministic Teardown) gets the same addition — the
test harness's lifecycle matches main.cpp's.

### Extension-uninstall drain

`drain_extension(name)` is a per-extension synchronous barrier that
reuses the same `flush_window` call path restricted to windows whose
`extension_name` matches. Called from `install_lifecycle.cpp` at the
three `provider_type = "extension"` transition points (ICD-0.5.0.3
§Lifecycle hooks — same three sites at install_lifecycle.cpp:842 /
930 / 1891). Runs under the caller's transaction wait window; the
0.4.5 drain budget of 5 s applies.

---

## Error Model

### Classifier outcomes

- `std::optional<SqlClass>` from `classify_sql(op.sql)`:
  - `has_value()` → `{schema, table, op_kind}` extracted. Coalescer
    records the write.
  - `!has_value()` → skip. Debug log at trace level with `sql[0..80]`
    and `reason`. No audit.

### `record_write` outcomes

- `cfg.coalescer.enabled == false` → no-op.
- `shutting_down_ == true` AND bucket empty → no-op (no new windows
  during drain).
- `row_count == 0` AND bucket empty → no-op.
- Otherwise → open-or-accumulate per §Coalescer State Machine. Never
  fails; no return value.

### `flush_window` outcomes

- Successful `emit_notify_async` → no action beyond the emit.
- `NotifyError::INVALID_CHANNEL` — should be impossible (classifier
  enforces the identifier regex); logged at error + audited as
  `flush_failed`; indicates a classifier / validator drift bug.
- `NotifyError::PAYLOAD_TOO_LARGE` — truncation heuristic failed to
  fit. Audit `flush_failed`; drop.
- `NotifyError::PG_FAILURE` — PG transient error (lost connection,
  `pg_notify` returned non-OK). Audit `flush_failed`; drop. The
  lost event is NOT retried — best-effort delivery matches
  architecture/03-data.md §3.1's implicit contract (Layer 1 events
  are fire-and-forget; 0.5.4 persistence + 0.5.5 seq are the
  reliability tier).
- `NotifyError::MISSING_LAYER` / `LAYER_MISMATCH` — impossible
  (coalescer always sets `layer="data"` and generates a `plinth:data:`
  channel); logged at error + audited if seen; a bug.

### Shutdown timeout

Per-flush 2 s timeout during `shutdown()`. On timeout: audit
`flush_failed` with `reason="shutdown_timeout"`, abandon the flush.
The shutdown drain continues with the next window.

### Caller-visible errors

**Extensions never see coalescer errors.** `record_write` never
rejects a DB write — it returns `void`; the JS promise already
resolved from the successful `execSqlCoro`. This is intentional: a
failed NOTIFY does not invalidate the DB write. Extensions that
need delivery guarantees for a specific event use `pubsub.publish`
(Layer 3, synchronous rejection on emit failure), not implicit
Layer 1.

### Configuration load failures

`realtime.coalescer.window_ms` out of range `[1, 10000]` → config
load rejects with
`config.realtime.coalescer.window_ms_out_of_range`. Matches the
existing 0.5.0 pattern for
`realtime.listener.reconnect_backoff_ms_out_of_range` and
`realtime.notify.max_payload_bytes_invalid`.

---

## Security Constraints

1. **Classifier is read-only.** The coalescer never constructs new
   SQL. Its only SQL interaction is reading `op.sql` as a string;
   the downstream emit path runs `SELECT pg_notify($1, $2)` with
   parameters, and `$1` is always the literal `"plinth:realtime"`
   per ICD-0.5.0 §Channel Subscription.
2. **Channel regex enforced.** Every coalescer-generated channel
   passes through `validate_channel` at emit time. `schema` + `table`
   extracted by the classifier are already constrained to the PG
   identifier regex, so regex failures indicate a classifier bug,
   not an attack vector.
3. **Extension-identity defense-in-depth.** The classifier's
   `schema == "ext_" + bc_extension_name` assertion is a bug catcher
   for ICD-0.4.3 schema-isolation drift, not the primary guarantee.
   The primary guarantee is ICD-0.4.3's `search_path` enforcement +
   per-extension role + capability gate.
4. **No user-controlled envelope fields.** Coalescer envelopes have
   no extension-supplied text content — `schema`, `table`, `ops`
   counts are all derived from SQL classification + PG result, not
   from extension-provided JSON. This means a malicious extension
   cannot craft a payload that exploits any client SDK's envelope
   parser.
5. **No per-envelope audit.** Coalescer envelopes carry row counts;
   writing the counts into the audit log on every flush would leak
   extension activity patterns into the security-audit surface.
   Operational observability is the listener's job (NOTIFY
   consumption metrics), not audit's.
6. **Shutdown-drain is best-effort, not at-most-once.** On flush
   failure during shutdown, the event is lost. This is the same
   best-effort posture the steady-state coalescer applies; no new
   security constraint.
7. **`cfg.coalescer.enabled = false` is an opt-out, not a bypass.**
   When disabled, no Layer 1 event fires — clients subscribed to
   Layer 1 channels receive nothing. This is not a security
   escalation path (extensions still cannot observe each other's
   writes); it is an operational switch for deployments that don't
   want the bus.

---

## Test Cases

Test prefix: **C.\*** for coalescer state machine, **T.\*** for
truncation heuristic, **I.\*** for end-to-end integration, **E.\***
for error paths. Tag convention `[realtime][coalescer]` + per-group
subtype (`[unit]`, `[pg]`, `[integration]`). Distinct from 0.5.0's
R/E/P prefixes (R = realtime listener, E = emitter, P = pubsub) —
0.5.1's C/T/I/E use different first letters except E, which carries
the `[coalescer][errors]` tag where 0.5.0's E.\* carried
`[realtime][emit]`. Catch2 tag-disjointness keeps them distinct.

Total: **26 new cases** (13 C + 4 T + 5 I + 4 E). Distributed across
two test TUs.

### Coalescer state machine — `tests/kernel/realtime/coalescer_test.cpp`

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| C.01 | Single-write flush | `record_write(ext_notes, notes, INSERT, 1, "notes")` then wait `window_ms+10`; capture flushed envelope via `apply_flush_for_test` OR an in-process listener handler | No | One envelope with `ops:[{insert:1},{update:0},{delete:0}]`, `channel="plinth:data:ext_notes.notes"` |
| C.02 | Two-write accumulate | Two `record_write` calls within the window (both INSERT, counts 5 + 3) | No | One envelope with `ops:[{insert:8},{update:0},{delete:0}]` |
| C.03 | No-extend window | Three `record_write` calls at t=0, t=window_ms/2, t=window_ms-1; flush at t=window_ms | No | Envelope flushes at t=window_ms (not t=2×window_ms-1) — asserts fixed-duration-from-first-write |
| C.04 | Separate windows | `record_write` at t=0; wait `window_ms+10`; `record_write` again | No | Two envelopes, each with count=1 |
| C.05 | Per-table isolation | Concurrent `record_write(ext_notes, notes, ...)` + `record_write(ext_notes, tags, ...)` | No | Two independent envelopes, one per table; neither envelope mentions the other's table |
| C.06 | Mixed ops | `record_write` for INSERT (5), UPDATE (2), DELETE (1) in a single window | No | One envelope with `ops:[{insert:5},{update:2},{delete:1}]` |
| C.07 | enabled=false | `start({enabled: false})`; `record_write` → no envelope | No | `apply_flush_for_test` returns false (no window); `open_window_count_for_test() == 0` |
| C.08 | window_ms=1 | `start({window_ms: 1})`; `record_write`; wait 5 ms | No | Envelope flushed; `window_ms` in envelope reads `1` |
| C.09 | Zero-row write (empty bucket) | `record_write(..., row_count=0)` with no prior write in bucket | No | No window opened; `open_window_count_for_test() == 0`; no envelope fires later |
| C.10 | Zero-row write (open bucket) | `record_write(INSERT, 3)` at t=0; `record_write(UPDATE, 0)` at t=5ms; flush at t=window_ms | No | One envelope, `ops:[{insert:3},{update:0},{delete:0}]` — zero-row write neither opened nor disturbed the bucket |
| C.11 | Cross-extension warning | `record_write(schema="ext_a", ..., extension_name="a")` at t=0; `record_write(schema="ext_a", ..., extension_name="b")` at t=5ms (illegal per ICD-0.4.3 — this test exercises the defense-in-depth warn path) | No | Warn log emitted; envelope still fires with combined counts; window ownership stays "a" |
| C.12 | drain_extension flushes matching | `record_write` for ext_a (twice) + ext_b (once); call `drain_extension("a")` | No | Envelope for ext_a fires synchronously; envelope for ext_b stays open until its timer fires |
| C.13 | drain_extension idempotent | Call `drain_extension("nobody")` with no matching windows | No | Returns without error; no envelope fired; no audit fired |

### Truncation heuristic — `tests/kernel/realtime/coalescer_test.cpp` (same TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| T.01 | Envelope under ceiling | Default `max_payload_bytes=8000`; normal envelope | No | Envelope size << 8000; `truncated` field absent |
| T.02 | Envelope over ceiling, 0.5.1 counts-only path | `set_max_payload_bytes(80)` to force the threshold below a normal envelope; `record_write`; flush | No | Envelope emits with `truncated:true` OR audit `flush_failed` fires (§OQ4 determines which; recommendation: for ids-absent 0.5.1, audit+drop since removing already-absent ids can't shrink the payload) |
| T.03 | max_payload_bytes config knob | `set_max_payload_bytes(200)`; envelope just over 200 bytes | No | Oversize path exercised — either truncate-retry or audit+drop per §OQ4 resolution |
| T.04 | `set_max_payload_bytes` round-trip | Save `get_max_payload_bytes()`; set 200; restore original in test teardown | No | No leak into subsequent tests (RAII-guarded) |

### End-to-end integration — `tests/kernel/realtime/coalescer_integration_test.cpp` (new TU)

All cases run a live PG + Drogon DbClient + `realtime::start_listener` with a test-registered `EventHandler` that captures envelopes into a `std::vector<DispatchedEvent>`.

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| I.01 | One JS write → one envelope | Extension `ext_it01` runs `await db.exec("INSERT INTO ext_it01.t VALUES (1,'a')")` | Yes | Handler receives exactly one `DispatchedEvent` with `envelope.layer=="data"`, `envelope.channel=="plinth:data:ext_it01.t"`, `envelope.ops[0].count==1` within `window_ms + 100 ms` |
| I.02 | Three JS writes → one envelope | Three `db.exec("INSERT ...")` within `window_ms/2` of each other | Yes | Handler receives exactly ONE envelope with `ops[0].count==3` |
| I.03 | Two tables → two envelopes | `db.exec("INSERT INTO ext_it03.a...")` + `db.exec("INSERT INTO ext_it03.b...")` within `window_ms` | Yes | Handler receives exactly TWO envelopes, one per table |
| I.04 | Mixed ops in a window | `db.exec` INSERT + UPDATE + DELETE on same table within `window_ms` | Yes | One envelope with nonzero counts for all three op kinds |
| I.05 | Unparseable SQL → no envelope | `db.exec("SELECT 1")` via DB_EXEC (degenerate — extensions SHOULD use `db.query`, but DB_EXEC tolerates it per current code) | Yes | Zero envelopes received within `window_ms + 100ms`; JS promise resolves normally |

### Error paths — `tests/kernel/realtime/coalescer_test.cpp` (same TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| E.01 | Unparseable SQL (DDL) | `classify_sql("CREATE INDEX ... ON ext_notes.notes(id)")` | No | Returns `std::nullopt`; no envelope |
| E.02 | Zero-row update | `record_write(UPDATE, row_count=0)` with empty bucket | No | No window opened; `open_window_count_for_test() == 0` |
| E.03 | Emit failure | Inject a `emit_notify_async`-returning-`PG_FAILURE` (via a test fixture that substitutes the DbClient for one returning an error); flush | Yes | Audit `realtime.coalescer.flush_failed` fires; no crash; subsequent writes still work |
| E.04 | Shutdown drains open window | Open a window; call `shutdown()`; wait | No | One envelope emitted during drain (captured by test handler); `shutdown_drain` audit fires with `windows_drained=1`; no envelope timer fires post-shutdown |

### Test-seam notes

- C.\* cases (13) and T.\* cases (4) use `apply_flush_for_test` to
  bypass real-clock waits on all but C.03 / C.04 / C.08 / C.10 (those
  need actual timer behavior).
- I.\* cases (5) are end-to-end PG-gated — use the existing
  `plinth_tests_pg` group via `[realtime][coalescer][pg]` tag.
  Requires `LISTEN/NOTIFY` + DbClient + an `EventHandler` registered
  at test setup. Mirrors the 0.5.0 R.01 / E.01 smoke pattern —
  registered handler captures envelopes into a vector, test asserts
  on the vector's contents.
- E.03 requires substituting the DbClient with a failure-returning
  mock. Cleanest shape: a test-only `set_emit_hook_for_test` on the
  coalescer that intercepts the emit call and returns a synthetic
  `NotifyError`. Add the seam alongside `apply_flush_for_test`.
- **No new CTest subprocess.** C / T / E cases (22) join
  `plinth_tests_pg` via tag `[realtime][coalescer]` where PG-gated
  (the subprocess skips cleanly when PG is absent); I cases (5) also
  join `plinth_tests_pg`. No `plinth_tests_js` additions — the
  coalescer is entirely kernel-C++; JS surface is unchanged.
  `plinth_tests_pure` does NOT pick up coalescer cases (none of them
  are pure — all need either the registry singleton or the `log::audit`
  pipeline ready).
- `max_payload_bytes` manipulation in T.\* uses the 0.5.0-shipped
  `set_max_payload_bytes` / `get_max_payload_bytes` test seams
  (emit.hpp lines 42–45) with RAII restore.

### CI wiring

- `src/kernel/realtime/coalescer.{hpp,cpp}` — new.
- `src/kernel/realtime/sql_classify.{hpp,cpp}` — new (separable
  library; unit-testable).
- `src/kernel/js/async_op.hpp` — `bc_extension_name` field added
  (one line).
- `src/kernel/js/stdlib/db_bindings.cpp` — populate
  `op.bc_extension_name = bc.extension_name` at enqueue time.
- `src/kernel/js/run_on_context.cpp` — hook block added after line
  435 in `run_db_exec_outcome`.
- `src/kernel/config.{hpp,cpp}` — `Config::Realtime::Coalescer`
  substruct + `apply_realtime` coalescer-block parser.
- `src/kernel/main.cpp` — startup `CoalescerRegistry::instance().start(...)`
  + atexit `shutdown()` additions.
- `src/kernel/packages/install_lifecycle.cpp` — three
  `drain_extension(pkg.name)` call sites at the DISABLED / UPGRADING /
  UNINSTALL transitions.
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit-chain mirror edit
  (match main.cpp).
- `tests/kernel/realtime/coalescer_test.cpp` — new.
- `tests/kernel/realtime/coalescer_integration_test.cpp` — new.
- `CMakeLists.txt` — no new glob edit (kernel + tests globs already
  cover `src/kernel/**`, `tests/kernel/**`).
- `migrations/schema.sql` — no edits. `plinth.events` persistence
  is 0.5.4.
- `docker/ci.Dockerfile` — no edits.
- `.gitea/workflows/ci.yml` — no new jobs.

### Test count target

26 new cases. Full suite grows by 26 TEST_CASEs distributed across
existing CTest groups. All 26 land in `plinth_tests_pg` (PG-gated
cases run there; pure cases also run there alongside — no hard
separation needed since the group already handles mixed tags). No
new subprocess count; no new ctest entry.

---

## Entry / Exit

**Entry criteria:**

- v0.5.0 merged + tagged (done — commit `f3552b3`, tag `v0.5.0`).
- 0.5.0.1–0.5.0.4 merged untagged (done — commits `7848823`,
  `f26534a`, `0d86a62`, `97b5dae`).
- LH-1 storm diagnostic shipped (done — commit `7953eae`,
  PR #72).
- ICD-0.5.1 authored (this document, squash-merged as 0.5.0.5 paper
  slot).
- `BridgeContext::extension_name` populated per install-lifecycle
  (done — v0.4.4 + v0.5.0.4).

**Exit criteria:**

- `src/kernel/realtime/coalescer.{hpp,cpp}` +
  `src/kernel/realtime/sql_classify.{hpp,cpp}` ships.
- `AsyncOp::bc_extension_name` populated in `db_bindings.cpp`;
  consumed in `run_db_exec_outcome`.
- `Config::Realtime::Coalescer` substruct + config-loader support
  with bound-check validation.
- `main.cpp` + `ws_test_fixture.cpp` atexit chains include
  `CoalescerRegistry::shutdown()`; startup includes `start()`.
- `install_lifecycle.cpp` three transitions call `drain_extension`.
- All 26 C/T/I/E test cases pass; PG-gated cases skip cleanly when
  PG env absent.
- `run-clang-tidy-20` zero findings on new TUs (`coalescer.cpp`,
  `sql_classify.cpp`) and modified TUs (`run_on_context.cpp`,
  `db_bindings.cpp`, `async_op.hpp`, `config.cpp`, `main.cpp`,
  `install_lifecycle.cpp`, `ws_test_fixture.cpp`).
- No regressions on the v0.5.0 + 0.5.0.4 test matrix.
- Atexit-race validation: 20-run ctest loop sample shows zero
  teardown-race reproductions (matching the 0.4.0.1 / 0.5.0
  validation bar for new subsystems).
- `CHANGELOG.md` `v0.5.1` entry describes the coalescer, the SQL
  classifier, the integration hook, the AsyncOp extension, the
  atexit edit, the config surface, the audit events, the three
  install-lifecycle hook sites, and every accepted OQ deviation.
- `docs/ROADMAP.md §0.5` line for 0.5.1 is removed.
- `v0.5.1` tag cut on the merge commit.
- Memory `project_plinth_state.md` updated to reflect 0.5.1 shipped;
  `project_next_session_0_5_1.md` retired (replaced by a pointer to
  the `RE-EVAL following 0.5.1` slot that blocks 0.5.2).

---

## Open Questions

**OQ1 — Extension unload mid-window.** The ICD pins **synchronous
drain on DISABLED / UPGRADING / UNINSTALL** via
`drain_extension(pkg.name)` at the three install-lifecycle call
sites. Alternatives considered: (a) drop-and-audit (faster uninstall,
one lost envelope per open window per uninstalled extension;
unacceptable UX on upgrade — 0.4.5's whole point is no lost work);
(b) async best-effort flush after uninstall commits (race-y; the
coalescer envelope's `schema` would still read `ext_<name>` even
though the schema may have been dropped by a concurrent uninstall —
creates a phantom-table event). **Recommendation:** (a) synchronous
drain. Cost: up to `window_ms` × (open windows for extension) added
to the uninstall transaction — with typical single-digit open windows
at steady state, well under the 5 s drain budget from 0.4.5.
Architect: confirm the synchronous-drain choice or redirect to (a)
or (b).

**OQ2 — SQL parse scope: qualified-only, or unqualified + implicit
schema, or full SQL-parser?** The ICD pins **qualified single-table
+ unqualified single-table with implicit `ext_<bc_extension_name>`
schema**. Writeable CTEs + multi-table writes + quoted identifiers
are skipped with debug log. **Recommendation:** pin as written; the
90%+ common case is qualified single-table writes in extension code.
Architect: confirm or widen to include CTEs (would require a real
PG-dialect parser — `libpg_query` or similar; cost: one dependency,
one compile-time bump).

**OQ3 — Coalescer state lives where: per-process singleton, or
per-DbClient?** The ICD pins **process-wide singleton with
`std::shared_mutex`**. Per-DbClient would fragment state across the
Drogon pool (a burst of writes striping across 4 connections would
open four independent windows for the same table, producing four
NOTIFYs). **Recommendation:** pin singleton. Architect: confirm.

**OQ4 — `ids` array in 0.5.1 envelopes: always empty (pinned), or
populate via implicit `RETURNING id` in the classifier?** The ICD
pins **always empty in 0.5.1**; architecture/03-data.md §3.1's
example shows `ids:["a1","a2","..."]`, so the ICD is a deliberate
deviation with two consequences:

  - **Client-side:** the 0.5.5 client SDK's optimistic-update path
    (architecture/03-data.md §3.4 item 2) has nothing to optimize
    against in a 0.5.1-era envelope — every Layer 1 event triggers
    a re-query. This is effectively identical to the `truncated:true`
    fallback the architecture already expects clients to handle.
  - **Server-side to populate `ids` would require:** (1) rewriting
    INSERT statements to append `RETURNING id` (needs an `id` column
    to exist — not universally true for extension tables); (2)
    collecting the returned IDs from the Drogon `Result`; (3) keeping
    them on the `WindowState`; (4) matching UPDATE/DELETE IDs via
    `WHERE` clause introspection (much harder). The cost is
    non-trivial; the client-side benefit is deferred to 0.5.5 + the
    0.6.3 SDK anyway.

  **Recommendation:** pin empty. Revisit at 0.5.5 when `seq` arrives
  and the SDK's optimistic-update path actually ships. Architect:
  confirm or implement now (cost estimated: +80 LOC in the classifier
  + coalescer, ~6 additional test cases).

**OQ5 — Window timer: Drogon `runAfter` on the DB IO loop, or a
dedicated `trantor::EventLoopThread`?** The ICD pins **dedicated
`trantor::EventLoopThread`**. Drogon's `runAfter` on an arbitrary IO
loop risks cross-thread deadlocks if `flush_window` calls
`emit_notify_async` which dispatches back onto the same IO loop. A
dedicated loop isolates coalescer-internal timer behavior from the
HTTP IO fleet — matches the 0.5.0 listener's posture (one subsystem,
one thread). Cost: +1 thread per process; ~4 KB stack + minimal
kernel overhead. **Recommendation:** pin dedicated. Architect:
confirm.

**OQ6 — No-timer-extension window policy.** The ICD pins **fixed
duration from first write, no extension on subsequent writes** —
flush exactly `window_ms` after `opened_at`. Alternative (classic
debounce): extend the timer on every write; flush `window_ms` after
the last write. Classic debounce trades latency (unbounded; a sustained
write burst never flushes) for coalescer efficiency. Architecture
§3.1 prose supports either reading; fixed-duration is simpler and
bounds emit latency. **Recommendation:** pin fixed-duration.
Architect: confirm — this is a first-order design pin.

**OQ7 — Envelope `ops` array: always three entries (insert / update /
delete), or only non-zero op kinds?** The ICD pins **always three
entries**, even when a given op kind had zero writes. Alternative:
omit zero-count entries. Cost of always-three: ~60 extra bytes per
envelope (three entries × ~20 bytes each). Benefit: clients don't
branch on missing ops; simpler schema. **Recommendation:** pin
always-three. Architect: confirm.

---

## Appendix: Resolved Open Questions (v0.5.1)

All seven OQs land at the ICD-recommended pin. Resolution captured
here so future readers do not need to cross-reference CHANGELOG
v0.5.1 §Why to confirm the contract. No architect redirects. See
`RE-EVAL-0.5.x-following-0.5.1.md §3 Zero-gap findings` for the
cross-check that each pin matches the shipped code.

| OQ | Pin | Rationale source |
|----|-----|------------------|
| OQ1 | **Synchronous drain on DISABLED / UPGRADING / UNINSTALL** via three `drain_extension(pkg.name)` call sites in `install_lifecycle.cpp`. | Recommendation in §Open Questions; matches 0.4.5 atomic-swap "no lost work" invariant. |
| OQ2 | **Qualified single-table + unqualified single-table with implicit `ext_<bc_extension_name>` schema.** CTE writes / multi-table writes / quoted identifiers skip with debug log. | Recommendation. 90%+ coverage of real extension writes; defers full SQL parser dependency. |
| OQ3 | **Process-wide singleton** with `std::shared_mutex`. | Recommendation. Per-DbClient alternative would fragment state across the Drogon pool. |
| OQ4 | **`ids` absent in v0.5.1 envelopes.** Clients treat as equivalent to `truncated:true` ID-elided fallback. Revisit at 0.5.5 alongside `seq` + SDK optimistic-update. | Recommendation. Server-side cost non-trivial; client-side benefit deferred to 0.5.5 + 0.6.3 SDK. |
| OQ5 | **Dedicated `trantor::EventLoopThread`** for the coalescer flush-timer loop. | Recommendation. Matches 0.5.0 listener's "one subsystem, one thread" posture; avoids cross-thread deadlock risk of Drogon `runAfter` on an IO loop. |
| OQ6 | **Fixed-duration window from first write, no extension on subsequent writes.** Flush exactly `window_ms` after `opened_at`. | Recommendation. Simpler than classic debounce; bounds emit latency; matches architecture/03-data.md §3.1 prose. |
| OQ7 | **Always-three `ops` array** entries (`insert`/`update`/`delete`), even when a kind had zero writes. | Recommendation. ~60 extra bytes/envelope in exchange for schema simplicity (no client branching on missing ops). |

Appendix added by `RE-EVAL-0.5.x-following-0.5.1.md §2 Phase 5` at
the post-v0.5.1 cadence re-eval (2026-04-23).

---

## Appendix A: End-to-End Example

Extension `ext_notes` on a three-node deployment, extension JS:

```javascript
// In a handler for POST /api/notes
await db.exec("INSERT INTO ext_notes.notes (id, title) VALUES ($1, $2)",
              ["n42", "Hello"]);
```

Kernel-side flow (coalescer portion — pre-coalescer is 0.3.3 +
0.5.0.4 path):

1. `db.exec` binding enqueues `AsyncOp{Type::DB_EXEC, sql, params,
   bc_extension_name="ext_notes", silent=false}` on `bc.pending_ops`.
2. `run_on_context` drains the queue; dispatches via
   `dispatch_async_op_detached` → `run_db_exec_outcome(op)`.
3. `run_db_exec_outcome` runs `co_await db->execSqlCoro(...)` →
   `Result r` with `affectedRows()==1`.
4. Hook runs: `classify_sql("INSERT INTO ext_notes.notes ...")` →
   `{"ext_notes", "notes", INSERT}`. `silent==false`. Call
   `CoalescerRegistry::instance().record_write("ext_notes", "notes",
   INSERT, 1, "ext_notes")`.
5. Coalescer takes the write lock; no existing `WindowState` at
   (ext_notes, notes); opens one with `opened_at = now`, sets
   `insert_rows = 1`, extension_name = "ext_notes"; schedules
   `flush_window` at `opened_at + 50 ms` on the coalescer event
   loop; releases write lock.
6. `run_db_exec_outcome` returns `out = {row_count: 1}`; JS promise
   resolves; extension JS receives the result.
7. T=50 ms: timer fires on the coalescer loop. `flush_window` takes
   the write lock; extracts `WindowState {insert_rows:1, ...}`;
   erases the map entry; releases lock.
8. Build envelope:

   ```json
   {
     "layer": "data",
     "channel": "plinth:data:ext_notes.notes",
     "schema": "ext_notes",
     "table": "notes",
     "ops": [
       {"op": "insert", "count": 1},
       {"op": "update", "count": 0},
       {"op": "delete", "count": 0}
     ],
     "window_ms": 50
   }
   ```

9. `emit_notify_async(db, envelope)` → validates → runs
   `SELECT pg_notify('plinth:realtime', <envelope>)`. PG ACK.
10. PG LISTEN/NOTIFY backbone delivers the NOTIFY to all three nodes'
    `realtime::listener`s. Each node's listener parses, validates
    channel, dispatches to every registered `EventHandler`.
11. On 0.5.1: zero Layer-1 handlers registered (0.5.2 broker is not
    yet shipped). The envelope arrives at each node's listener and
    drops into the no-consumer sink (R.04 coverage in 0.5.0's test
    suite).

From the extension author's perspective: one `db.exec`, ~50 ms
later the entire HA cluster has seen the event. Step 12 (client-
connected fan-out) requires 0.5.2's broker; step 13 (client-side
re-query) requires 0.6.3's SDK; steps 14 (delta-sync on reconnect)
+ 15 (monotonic `seq`) require 0.5.4 / 0.5.5. 0.5.1 delivers the
producer side of the Layer 1 auto-event bus.

---

## Appendix B: Config Example

```json
{
  "database": { "host": "127.0.0.1", ... },
  "logging":  { "level": "info" },
  "realtime": {
    "listener":  { "enabled": true, "reconnect_backoff_ms": 1000 },
    "notify":    { "max_payload_bytes": 8000 },
    "coalescer": { "enabled": true, "window_ms": 50 }
  }
}
```

Stripped deployment disabling Layer 1 auto-events:

```json
"realtime": {
  "coalescer": { "enabled": false }
}
```

Minimal-valid coalescer block (all defaults):

```json
"realtime": { "coalescer": {} }
```

Tightened window for latency-sensitive deployments:

```json
"realtime": { "coalescer": { "window_ms": 10 } }
```

---

## Appendix C: Window State Lifecycle (ASCII)

```
                ┌──────────────────────────────────────────────┐
                │                                              │
                │  No window at (schema, table)                │
                │                                              │
                └──────────────────────────────────────────────┘
                    │
   record_write ───┤ (row_count > 0)
                    ▼
                ┌──────────────────────────────────────────────┐
                │                                              │
                │  Window OPEN                                 │
                │  opened_at = now                             │
                │  counters: insert=I, update=U, delete=D      │
                │  timer: runAfter(window_ms, flush)           │
                │                                              │
                └──────────────────────────────────────────────┘
                    │               │                │
     record_write──┤ (in window)    │                │ shutdown()
     (accumulate)   │                │ timer fires    │ drain_extension(name)
                    │                │ at             │ (if matches)
                    │                │ opened_at      │
                    │                │ + window_ms    │
                    ▼                ▼                ▼
                ┌──────────────────────────────────────────────┐
                │                                              │
                │  Counters updated in place.                  │
                │  Timer NOT rescheduled.                      │
                │                                              │
                └──────────────────────────────────────────────┘
                                     │
                                     │ flush_window
                                     ▼
                ┌──────────────────────────────────────────────┐
                │                                              │
                │  Build envelope, call emit_notify_async.     │
                │  On success: done.                           │
                │  On error: audit flush_failed; drop.         │
                │  Erase map entry.                            │
                │                                              │
                └──────────────────────────────────────────────┘
                                     │
                                     ▼
                          (back to No window)
```

Notes:

- `record_write` with `row_count == 0` on an empty bucket is a no-op
  (short-circuits before opening a window); on an open bucket, it
  is also a no-op (counters unchanged, timer untouched).
- `drain_extension(name)` only fires the flush path for windows
  whose `extension_name` matches — other extensions' windows stay
  open under their timers.
- `shutdown()` cancels every timer + flushes every open window
  synchronously. Subsequent `record_write` calls during shutdown
  short-circuit without opening new windows.
