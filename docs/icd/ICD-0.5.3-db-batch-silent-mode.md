# ICD-0.5.3-db-batch-silent-mode

**Traces to:** architecture/03-data.md §3.2 (Batch Operations — the
normative prose this ICD promotes to contract; `db.batch(async () => {
... })` emits one coalesced event; `db.exec(..., { silent: true })`
suppresses event emission entirely; Layer 1 events emitted by default);
architecture/03-data.md §3.1 + §3.1.1 (Debounced Change Streams +
Auto-Event Coalescer — the existing 50 ms per-(schema,table) window the
batch-end flush composes over; one envelope per `(schema,table)` per
batch commit); architecture/02-capabilities.md §2.1 (Always Available —
`db.query(sql)` + `db.exec(sql, opts?)` with `opts.silent` + `db.batch(fn)`
"execute multiple writes, emit single coalesced event" — the three rows
this ICD pins in full); architecture/02-capabilities.md §3.1 (Async
Dispatch Arm + Extension Runtimes — `db.*` already composes on the
async-bridge arm; the new `DB_BATCH_*` variants extend the existing
`AsyncOp::Type` enum, same pattern `DB_QUERY` / `DB_EXEC` take today);
architecture/05-extensions.md §3.2 (supervision + runtime-lifecycle —
the drain hook on DISABLED / UPGRADING / UNINSTALL this ICD specifies
mirrors the coalescer / broker drain pattern from ICD-0.5.1 §Extension-
lifecycle integration and ICD-0.5.2 §Extension Lifecycle Integration);
ICD-0.3.3-async-bridge §Security Constraint 1 (the per-op `SET
search_path TO ext_<extension_id>, plinth;` enforcement deferred in
0.3.3 with the text: "At connection checkout, the bridge issues `SET
search_path TO ext_<extension_id>, plinth;` before the user's `sql`
runs. For test/host contexts where `extension == nullptr`, the search
path is `plinth` only" — **this is the ICD that finally implements
that constraint**; tracked as the first 2026-04-18 DEFERRED entry,
pointer tightened to 0.5.3 by `RE-EVAL-0.5.x-following-0.5.1.md §2.5`);
ICD-0.3.3-async-bridge §PG-Value → JS-Value Conversion (the OID-driven
mapping table deferred in 0.3.3 to a heuristic string-parse —
**this ICD promotes the table to OID-driven contract**; tracked as the
second 2026-04-18 DEFERRED entry, pointer tightened to 0.5.3 by the
same RE-EVAL §2.5); ICD-0.5.0-pg-listen-notify-bridge §`emit_notify_async`
(the emission primitive batch-commit flush calls once per
`(schema, table)` tuple accumulated in the batch); ICD-0.5.1-pg-auto-event-coalescer
§Registry + Integration Point (the `CoalescerRegistry::record_write`
path — `silent` already short-circuits this at
`src/kernel/js/run_on_context.cpp:444`; batch-commit flush composes
over the coalescer's existing `flush_window` primitive by calling the
public flush seam the coalescer exposes for batch end); ICD-0.5.1 §Extension-lifecycle
integration (the drain pattern DISABLED / UPGRADING / UNINSTALL —
batch rollback mirrors coalescer's final flush posture); ICD-0.4.3
§Schema creation (extension schemas created as `ext_<extension_id>`
at install; `SET search_path` target matches this schema naming
exactly); ICD-0.4.4-package-install-lifecycle
(`BridgeContext::extension_name` populated per-pool at install —
the identity the `SET LOCAL search_path` wrapper reads to format
`ext_<extension_name>`); ICD-0.1.7-audit (async audit writer +
`g_audit_ready` gate — batch + silent audit events rate-limited per
§Audit Events).

**Depends on:** ICD-0.5.0 (listener + emit + envelope + channel-naming
regex — unchanged; batch emits via `emit_notify_async` exactly as the
coalescer does); ICD-0.5.1 (the coalescer the batch-end flush
coordinates with — batch emits one envelope per `(schema,table)` per
commit, deferring to the coalescer's envelope shape verbatim; `silent`
flag's existing coalescer gate at `run_on_context.cpp:444`); ICD-0.5.2
(the broker that fans out any batch-emitted envelope to subscribers —
broker sees batch-commit envelopes identically to coalescer-window
envelopes); ICD-0.3.3 (the db.* binding surface the new
`db.batch` joins + the search_path + OID constraints this ICD finally
implements); ICD-0.4.3 (extension schema creation — `ext_<id>` naming
the `SET search_path` wrapper targets); ICD-0.4.4 + ICD-0.5.0.4
(`bc.extension_name` populated per-pool — the identity token the
per-op wrapper consumes); ICD-0.1.7 (audit writer — the four new rate-
limited audit events ride existing infrastructure).

**Milestone:** 0.5.3 — `db.batch()` and silent mode. Fourth 0.5.x code
milestone (after v0.5.0 bridge + v0.5.1 coalescer + v0.5.2 broker).
The piece that closes ICD-0.3.3's two long-standing DEFERRED entries
— per-op `SET search_path` isolation (Security Constraint 1) and
OID-driven PG-type → JS-type mapping (PG-Value → JS-Value). Paired
paper ICD authoring slot `0.5.2.N` precedes this code work per
METHODOLOGY §3.1 forward-ICD-presence rule and `feedback_icd_horizon.md`.

**Status:** Ready for implementation

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
[src/kernel/js/run_on_context.cpp](../../src/kernel/js/run_on_context.cpp)
(`DB_QUERY` / `DB_EXEC` dispatch arms at lines ~395–460 — new
`DB_BATCH_BEGIN` / `DB_BATCH_COMMIT` / `DB_BATCH_ROLLBACK` arms land
alongside; existing `op.silent` gate at line 443–455 is this ICD's
test vector for §Silent Flag Semantics; per-op `SET LOCAL search_path`
wrapper injects into the existing `run_db_query_outcome` /
`run_db_exec_outcome` arms before `execSqlCoro`);
[src/kernel/js/async_op.hpp](../../src/kernel/js/async_op.hpp)
(`AsyncOp::Type` enum — new `DB_BATCH_BEGIN` / `DB_BATCH_COMMIT` /
`DB_BATCH_ROLLBACK` variants join; `silent` field already present at
line 82 with the 0.5.x-realtime-hook comment; new `batch_scope_id`
field added this milestone for connection pinning);
[src/kernel/js/stdlib/db_bindings.cpp](../../src/kernel/js/stdlib/db_bindings.cpp)
(`db.exec` already parses `{silent:true}` at lines 313–332; this ICD
adds the new `db.batch` binding in the same TU, joining the
`register_db` seam at line 364; existing `db_query` / `db_exec`
arms extended with in-batch routing at enqueue time);
[src/kernel/js/stdlib/cap_bindings.cpp](../../src/kernel/js/stdlib/cap_bindings.cpp)
(existing `cap.batch` at line 161 is the Promise.all form — parallel
capability calls, NOT a transactional wrapper; `db.batch` this ICD
pins is semantically distinct and is explicitly NOT a naming collision
— see §3 `db.batch()` Transactional Wrapper rationale);
[src/kernel/js/bridge_context.hpp](../../src/kernel/js/bridge_context.hpp)
(new per-bc `batch_state` field tracking in-flight batch —
`{pinned_conn, depth, scope_id}`; §7 Extension Lifecycle Integration
reads this to roll back on DISABLED / UPGRADING / UNINSTALL);
[src/kernel/realtime/coalescer.hpp](../../src/kernel/realtime/coalescer.hpp) +
[src/kernel/realtime/coalescer.cpp](../../src/kernel/realtime/coalescer.cpp)
(new `CoalescerRegistry::flush_batch_scope(scope_id)` public seam —
invokes `flush_window` on every `(schema,table)` tuple accumulated
under the scope; mirrors the existing `drain_extension` pattern;
existing `record_write` path extended with an optional
`batch_scope_id` parameter so coalescer can tag accumulated counters
by scope);
[src/kernel/main.cpp](../../src/kernel/main.cpp) (atexit chain — new
`plinth::js::rollback_all_batches()` call inserted before
`realtime::stop_broker()`, itself between `realtime::stop_listener()`
and `CoalescerRegistry::instance().shutdown()`; per
`feedback_deterministic_teardown.md` every future subsystem with
framework callbacks needs `cancel_all_*` called from atexit);
[src/kernel/packages/install_lifecycle.cpp](../../src/kernel/packages/install_lifecycle.cpp)
(DISABLED / UPGRADING / UNINSTALL — new `js::rollback_extension_batches(pkg.name)`
call site alongside the coalescer + broker drain hooks);
[docs/architecture/03-data.md §3.1 + §3.1.1 + §3.2](../architecture/03-data.md)
(normative prose promoted to contract — §3.2 is the primary source
for this ICD); [docs/DEFERRED.md:197-288](../DEFERRED.md)
(the two 2026-04-18 entries this ICD resolves — move to Resolved on
0.5.3 ship); [docs/icd/ICD-0.3.3-async-bridge.md](ICD-0.3.3-async-bridge.md)
§Security Constraint 1 + §PG-Value → JS-Value Conversion (the exact
contract text this ICD promotes from "deferred heuristic" to
"implemented per this milestone");
[docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md](ICD-0.5.1-pg-auto-event-coalescer.md)
§Envelope Assembly + §Registry + Integration Point (the envelope
shape + write-recording path the batch-end flush must preserve
verbatim); [docs/icd/ICD-0.5.2-ws-broker.md](ICD-0.5.2-ws-broker.md)
(the consumer — subscribers see batch-commit envelopes identically to
window-flush envelopes; no broker change required by 0.5.3).

---

## Overview

0.5.3 lands four related contributions that together close ICD-0.3.3's
two standing DEFERRED entries and complete the `db.*` surface that
`architecture/02-capabilities.md §2.1` enumerates as "Always
Available":

1. **`db.batch(async () => { ... })`** — transactional wrapper.
   Inside the callback, every `db.exec` / `db.query` executes on a
   pinned PG connection under one `BEGIN` / `COMMIT` bracket.
   Coalescer writes accumulate under a batch scope tag; at COMMIT the
   coalescer emits ONE `plinth:data:*` envelope per `(schema, table)`
   tuple touched in the batch, not one per statement and not one per
   50 ms window. User-callback throws or DB errors roll back the
   transaction and reject the batch promise.

2. **`db.exec(sql, params, { silent: true })`** — per-call opt-out
   of Layer-1 event emission. The plumbing is already live
   (`async_op.hpp:82` field; `db_bindings.cpp:313-354` JS parse;
   `run_on_context.cpp:443-455` coalescer gate). 0.5.3 pins the
   *semantics* (scope is one call; orthogonal to `batch`; audit row
   still written), adds the `db.silent.used` rate-limited audit
   event, and lands the regression tests that lock the contract.

3. **Per-op `SET LOCAL search_path TO ext_<extension_name>, plinth;`
   isolation.** Every `db.exec` / `db.query` from an extension-scope
   `bc` runs inside a transaction — either the user-opened batch
   transaction, or an auto-opened single-op wrapper — that issues
   `SET LOCAL search_path` as its first statement. For kernel-scope
   bc (`bc.extension_name.empty()` — test/host paths), the wrapper
   is skipped and the schema default (`plinth`) holds. Closes
   ICD-0.3.3 §Security Constraint 1.

4. **OID-driven PG-type → JS-type mapping.** Replaces the
   heuristic string-parse detector at
   `src/kernel/js/stdlib/db_result_to_json.cpp` (heuristic that
   misreads literal string `"true"` as bool). The new implementation
   reads each column's PG OID via a local Drogon patch exposing
   `PGresult::ftype(col)`, switching on OID to the mapping table
   ICD-0.3.3 §PG-Value → JS-Value defines. Unknown OIDs fall back
   to string (fail-closed) with a rate-limited log warning. Closes
   ICD-0.3.3 §PG-Value → JS-Value.

All four compose on the existing `AsyncOp` dispatch surface. No new
kernel thread is introduced; no new config block beyond four keys
under the existing `db.*` section; no new audit subsystem; no new
capability. The broker (0.5.2) and the listener (0.5.0) see batch-
commit envelopes as ordinary Layer-1 envelopes — no contract change
downstream.

**Scope:**

1. `db.batch(async () => { ... })` binding in
   `src/kernel/js/stdlib/db_bindings.cpp`; three new `AsyncOp::Type`
   variants (`DB_BATCH_BEGIN`, `DB_BATCH_COMMIT`, `DB_BATCH_ROLLBACK`);
   per-bc `batch_state` field in `BridgeContext` tracking pinned
   connection + scope id; three dispatch arms in
   `run_on_context.cpp`.
2. `db.exec` / `db.query` dispatch arms extended with: (a) in-batch
   connection routing (re-use `bc.batch_state.pinned_conn`);
   (b) auto-wrapper when not in batch (`BEGIN; SET LOCAL search_path;
   user_sql; COMMIT;`); (c) `SET LOCAL search_path TO ext_<id>, plinth`
   as the first statement inside any wrapper when
   `bc.extension_name.non_empty()`.
3. `silent` flag semantics pinned — already plumbed; this ICD
   specifies scope-is-one-call, orthogonality with batch, audit-row-
   always-written. Adds `db.silent.used` rate-limited audit.
4. OID-driven type mapping replaces the heuristic — local Drogon
   patch in `third_party/drogon-patches/ftype-accessor.patch`;
   `src/kernel/js/stdlib/db_result_to_json.cpp` switches on OID.
5. `CoalescerRegistry::flush_batch_scope(scope_id)` public seam.
6. `db.*` config block: `db.batch.max_ops_per_batch`,
   `db.batch.timeout_ms`, `db.search_path.enforce`,
   `db.oid_mapping.enabled`.
7. Four new rate-limited audit events — `db.batch.committed`,
   `db.batch.rolled_back`, `db.silent.used`, `db.search_path.set_failed`.
8. Extension-lifecycle drain — DISABLED / UPGRADING / UNINSTALL rolls
   back in-flight batches owned by the affected extension and flushes
   any accumulated coalescer scope. Mirrors ICD-0.5.1 / ICD-0.5.2
   drain shape.
9. Atexit chain extension — `js::rollback_all_batches()` before
   `realtime::stop_broker()`.

**Out of scope (explicit):**

- **Savepoints / nested batches.** Nested `db.batch` rejects with
  `db.batch.nested_not_allowed`. Savepoint API (`db.savepoint`,
  `SAVEPOINT / RELEASE / ROLLBACK TO`) is a future milestone — 0.5.3
  pins one layer of transaction nesting. OQ1 ratifies.
- **Cross-extension batch.** `db.batch` pins to `bc.extension_name`;
  every `db.exec` inside must classify to the same extension schema
  or kernel (`plinth.*`) tables. A cross-extension write inside a
  batch rejects with `db.batch.cross_extension_not_allowed`. The
  search_path wrapper already enforces schema isolation; this rule
  makes the attempt-detection explicit rather than silently failing
  at the OID level.
- **Streaming cursors / `db.cursor` / `db.stream`.** 0.3.3
  explicitly deferred these and they remain deferred. `db.batch`
  does not expose rows incrementally — the user callback runs, each
  `db.exec` resolves in order, and COMMIT lands at callback resolve.
- **Client-side batch SDK** (`plinth.batch()` or similar in the
  frontend). The 0.6.x shell SDK may expose a batched form of its
  `plinth.call()` later; 0.5.3 is kernel-JS only.
- **Non-PG backends.** `db.batch` depends on PG transactional
  semantics. The `DbClient` abstraction in Drogon nominally supports
  SQLite / MySQL / OracleCL — 0.5.3 pins PG-only behavior; the
  sqlite path trips `db.batch.backend_unsupported`.
- **Listener / coalescer contract changes.** Envelope shape stays
  ICD-0.5.1 §Envelope Assembly exact. `ops[]` array still carries
  all three CRUD kinds always. Batch commits land through the same
  `emit_notify_async` path — downstream broker (0.5.2) + WS frame
  (0.1.6) see no difference.
- **OID mapping for array types** (`TEXT[]`, `INT4[]`, etc.). ICD-0.3.3
  §PG-Value → JS-Value explicitly leaves arrays at "any other type"
  (→ string). 0.5.3 replaces the scalar heuristic only; array
  promotion is a future milestone when a real caller needs them.
- **`silent` on `db.query`.** SELECTs don't emit Layer-1 events in
  the first place (the classifier at
  `src/kernel/realtime/sql_classifier.cpp` skips non-INSERT / UPDATE /
  DELETE). Passing `silent:true` to `db.query` is a no-op and neither
  errors nor warns — matches the "silently ignored unknown opts"
  Node-convention ICD-0.3.3 §opts specifies.
- **Configurable isolation level per batch.** 0.5.3 batches are PG
  default (READ COMMITTED). `SERIALIZABLE` / `REPEATABLE READ` is
  future work — OQ2 ratifies.
- **Timeout enforcement inside batch** (per-statement vs per-batch
  wall-clock). Batch-level timeout via `db.batch.timeout_ms` lands;
  per-statement keeps ICD-0.3.3's existing posture (bc wall-clock).

---

## `db.batch()` Transactional Wrapper

### Surface

```javascript
// Execute multiple writes atomically; emit ONE coalesced Layer-1
// envelope per (schema, table) at commit — not one per statement.
// Rollback on any throw or DB error. Returns the user callback's
// resolved value (or rejects with the batch error).

await db.batch(async () => {
  for (const row of data) {
    await db.exec("INSERT INTO my_table(a, b) VALUES($1, $2)",
                   [row.a, row.b]);
  }
});

// Capturing the result of the callback:
const new_ids = await db.batch(async () => {
  const r = await db.query(
      "INSERT INTO my_table(x) VALUES($1) RETURNING id", [42]);
  return r.rows.map(row => row.id);
});
```

### Binding implementation

Fires through the async bridge (ICD-0.3.3 pattern), but with an
internal orchestrator that runs the user callback *between* two
async ops. The binding is:

```
db.batch(fn) →
    1. sync validation (fn is a callable)
    2. nested-batch check (bc.batch_state.depth > 0 → synchronous
       reject pubsub.batch_nested_not_allowed)
    3. synchronous batch-scope allocation (id = fresh monotonic uint64,
       bump bc.batch_state.depth = 1)
    4. enqueue AsyncOp{type: DB_BATCH_BEGIN, batch_scope_id = id,
       callback_id = <resolve_batch_begin>}
    5. the DB_BATCH_BEGIN dispatch arm:
         - pins a DbConnection for the duration
         - issues BEGIN; SET LOCAL search_path TO ext_<id>, plinth;
         - resolves the <resolve_batch_begin> promise
    6. on that promise resolve, the binding invokes fn() and awaits
       the result. All db.exec / db.query inside fn route to the
       pinned connection (dispatch arms check bc.batch_state.pinned_conn
       and skip the auto-wrapper — they're already in a transaction).
       Every record_write tags with batch_scope_id = id so the
       coalescer accumulates under the scope without timer flush.
    7. user callback resolves:
         - enqueue AsyncOp{type: DB_BATCH_COMMIT, batch_scope_id = id,
           callback_id = <resolve_batch>}
         - dispatch arm: COMMIT; unpin conn; call
           CoalescerRegistry::flush_batch_scope(id) — emits one
           envelope per (schema, table) touched in the batch; audit
           db.batch.committed; resolve the outer batch promise with
           the user-callback value
    8. user callback rejects (throws or DB error during batch body):
         - the binding-side catch enqueues
           AsyncOp{type: DB_BATCH_ROLLBACK, batch_scope_id = id,
           callback_id = <reject_batch>, rejection = <err>}
         - dispatch arm: ROLLBACK; unpin conn; discard coalescer
           scope (no emit); audit db.batch.rolled_back with the
           error's code; reject the outer batch promise with the
           underlying error
```

### `AsyncOp` additions

Three new enum variants join `AsyncOp::Type`:

```cpp
enum class Type {
    DB_QUERY,
    DB_EXEC,
    DB_BATCH_BEGIN,     // NEW 0.5.3
    DB_BATCH_COMMIT,    // NEW 0.5.3
    DB_BATCH_ROLLBACK,  // NEW 0.5.3
    CAP_CALL,
    PUBSUB_PUBLISH,
    PUBSUB_SUBSCRIBE,
    PUBSUB_UNSUBSCRIBE,
    AUDIT_LOG,
    // ...
};
```

One new field joins `AsyncOp`:

```cpp
struct AsyncOp {
    // ... existing fields ...
    std::uint64_t batch_scope_id = 0;  // 0 = not in a batch
};
```

`batch_scope_id` is set by the binding on every `DB_EXEC` / `DB_QUERY`
enqueued while `bc.batch_state.depth > 0` (snapshot at enqueue time,
not dispatch time — so a synchronous `bc.batch_state` reset can't
race with in-flight ops). The coalescer's `record_write` signature
grows a single `batch_scope_id` parameter with a default of 0;
writes tagged 0 follow the existing 50 ms-window flush, writes
tagged non-0 accumulate under the scope without timer.

### Connection pinning

The pinned `DbConnection` pointer is held on `bc.batch_state.pinned_conn`
(nullable `std::shared_ptr<drogon::orm::DbConnection>`). The
`DB_BATCH_BEGIN` dispatch arm calls
`drogon::app().getDbClient()->newTransaction()` to allocate a
transactional connection; the returned `TransactionPtr` is stored as
the pinned connection. Subsequent `db.exec` / `db.query` inside the
batch do `co_await pinned->execSqlCoro(sql)` instead of
`drogon::app().getDbClient()->execSqlCoro(sql)` — routing every
statement through the same connection guarantees the
`BEGIN`/`COMMIT` bracket holds. At COMMIT / ROLLBACK, the dispatch arm
releases the pinned pointer (Drogon's `TransactionPtr` destructor
issues `COMMIT` implicitly on drop, but this ICD pins explicit
`COMMIT` / `ROLLBACK` statements for determinism — see §OQ7).

`bc.batch_state` RAII — the `BridgeContext` destructor rolls back
any in-flight batch (issues `ROLLBACK` on the pinned conn + audits
`db.batch.rolled_back` with reason `bc_destroyed`). §Extension
Lifecycle Integration ratifies this.

### Coalescer interaction

Writes executed inside a batch accumulate into the coalescer's
`(schema, table)` buckets exactly as they do today, BUT tagged with
the batch's `scope_id`. The existing 50 ms window timer does NOT
fire for scope-tagged writes — the writes are deliberately held
until COMMIT regardless of how long the user callback takes. At
`DB_BATCH_COMMIT`, the dispatch arm calls
`CoalescerRegistry::flush_batch_scope(scope_id)`:

```cpp
void flush_batch_scope(std::uint64_t scope_id) {
    std::unique_lock lock(registry_mutex_);
    auto it = scope_to_buckets_.find(scope_id);
    if (it == scope_to_buckets_.end()) return;
    for (auto& [schema_table, counts] : it->second) {
        build_and_emit_envelope(schema_table.schema,
                                 schema_table.table,
                                 counts,
                                 /* window_ms = 0 */ 0);
    }
    scope_to_buckets_.erase(it);
}
```

`window_ms = 0` on the emitted envelope signals "batch commit" to
downstream consumers — the broker sees it as "immediate" rather than
"windowed," but its fan-out semantics are unchanged. Clients can
read the zero as "emitted synchronously at batch commit" if they
care; the standard interpretation (fields are counts only, not
ordering signals) holds. §OQ4 ratifies.

On `DB_BATCH_ROLLBACK`, the scope's accumulated counters are
discarded — no envelope emits. Writes that were executed against the
pinned connection never hit the table (transaction rolled back), so
the discarded counters are the correct accounting.

### Threading model

The binding orchestrator code (validation + nested check + scope
allocation + callback invocation + result promise wiring) runs on
the JS thread. `DB_BATCH_BEGIN` / `DB_BATCH_COMMIT` /
`DB_BATCH_ROLLBACK` dispatch arms run on Drogon's `DbClient`
coroutine thread pool, identical to existing `DB_QUERY` / `DB_EXEC`
arms. The pinned `TransactionPtr` is safe to pass between these
threads because all access is serialized through the async-bridge
resolve/reject path — the JS thread never touches the pinned
connection directly; only the dispatch arms do.

---

## `silent` Flag Semantics

### What it is

`db.exec(sql, params, { silent: true })` suppresses the Layer-1
realtime event the coalescer would otherwise emit for this write.
The write still executes, the row still lands, and the audit row
still fires. Only the `plinth:data:<schema>.<table>` PG NOTIFY is
omitted.

### Scope

**One call.** `silent: true` on a single `db.exec` call suppresses
that call's write and nothing else. The next `db.exec` without
`silent` (whether in the same coroutine, the same extension, or
inside the same batch) emits normally. This matches the ICD-0.3.3
existing plumbing (field lives on one `AsyncOp`) and the 02-capabilities.md
§2.1 table ("`opts.silent` suppresses events" on the `db.exec` row
specifically). §OQ3 ratifies.

### Propagation

`silent` already propagates through the existing plumbing:

- [db_bindings.cpp:313-332](../../src/kernel/js/stdlib/db_bindings.cpp) —
  JS-side parse from `argv[2].silent`.
- [async_op.hpp:82](../../src/kernel/js/async_op.hpp) — field on
  `AsyncOp` (comment reads "DB_EXEC; carried for 0.5.x realtime
  hook" — this ICD is that hook).
- [run_on_context.cpp:443-455](../../src/kernel/js/run_on_context.cpp) —
  the coalescer gate (`if (!op.silent) { ... record_write ... }`).

0.5.3 does not alter any of this. 0.5.3 adds:

1. The `db.silent.used` rate-limited audit (see §Audit Events) —
   auditors can alert on unexpected silent-flag rates without
   drowning the audit log on legitimate use.
2. The test cases (S.\*) locking the contract.

### Interaction with `db.batch`

`silent` inside a batch suppresses THAT statement's coalescer
accumulation. All other statements in the batch (without `silent`)
still accumulate under the batch scope. At `DB_BATCH_COMMIT`, the
coalescer flushes only the accumulated (non-silent) counters. A
batch with every statement silent → zero envelopes emit — the batch
still COMMITs, the audit row for each write still fires.

### Audit side-channel

`silent: true` **does NOT suppress audit.** `audit.log` calls from
extension JS are independent; the kernel's per-`db.exec` audit hooks
(written whenever 0.3.4 wires them; today audit hooks fire from
capability dispatch, not `db.*`) are independent. Every INSERT /
UPDATE / DELETE leaves an audit trail at the row level if the
extension writes one. Silent suppresses Layer-1 bus fan-out only.

### No `silent` on `db.query`

SELECTs don't emit Layer-1 events (the classifier skips non-write
shapes). Passing `silent:true` to `db.query` is a silently-accepted
no-op per ICD-0.3.3's "unknown opts silently ignored" convention.
The ICD does not add a warning or error here — the flag is simply
unread on the `DB_QUERY` arm.

### Rate-limited `db.silent.used` audit

Each `db.exec` with `silent:true` increments an in-memory counter
keyed by `bc.extension_name`. At first-hit-in-window and every
`db.silent.audit_window_ms` afterward (default 60000 ms), the
`db.silent.used` audit fires with `{extension: ..., count_in_window: N,
window_ms: 60000}`. This lets operators detect extensions that
silence every write (suggesting a misconfigured extension) without
audit-log floods. Parameters are configurable — §Config Surface
`db.silent.audit_window_ms`.

---

## Per-Op `SET search_path` Isolation

Closes ICD-0.3.3 §Security Constraint 1, deferred 2026-04-18, pointer
tightened to this milestone 2026-04-23 by
`RE-EVAL-0.5.x-following-0.5.1 §2.5`.

### Single-op wrapper

Every `db.exec` / `db.query` from an extension-scope `bc` that is
NOT inside a batch wraps in a short transaction:

```sql
BEGIN;
SET LOCAL search_path TO ext_<extension_name>, plinth;
<user_sql>;
COMMIT;
```

Four statements on the wire for every single-op call; one round trip
per statement under PG's typical connection → at most 4 round trips
per `db.exec` / `db.query`. Drogon's transactional client pipelines
these when possible — the cost is empirically closer to 1.5–2 round
trips amortized. The `BEGIN; ... COMMIT;` bracket uses `SET LOCAL`
specifically (scope to this transaction; resets on transaction end)
— no leakage to next checkout. §Security Constraint 1.

### Inside-batch wrapper

Inside a batch, the wrapper is already open — the `DB_BATCH_BEGIN`
dispatch arm issues `BEGIN; SET LOCAL search_path TO ext_<id>, plinth;`
as its atomic pair. Every subsequent `db.exec` / `db.query` in the
batch runs bare (no re-wrap), trusting the pinned connection's
session state. At `DB_BATCH_COMMIT` / `_ROLLBACK`, the transaction
ends and `search_path` resets automatically.

### Kernel-scope bypass

`BridgeContext::extension_name` is empty for kernel-scope `bc`
(test fixtures, host integration paths, the async-bridge driver
where no extension has been attached). For these, the wrapper is
**skipped entirely** — the raw `user_sql` runs against the default
schema (`plinth`), matching ICD-0.3.3 §Security Constraint 1
verbatim: "For test/host contexts where `extension == nullptr`, the
search path is `plinth` only."

No `SET LOCAL` emits. Nothing wraps. This preserves the 0.3.3 test
harness behavior: tests that use the default connection for
fixture setup continue to work without any ext_ schema requirement.

### Identity source

The `<extension_name>` token substituted into `SET LOCAL search_path
TO ext_<extension_name>, plinth;` reads from
`bc.extension_name` at enqueue time (same place the existing
`op.bc_extension_name` snapshot pulls from at
`db_bindings.cpp:357`). This matches the identity the
`pubsub.publish` extension-identity gate uses (ICD-0.5.0 §`pubsub.*`
JS Stdlib) and the value the coalescer tags accumulated writes with
(ICD-0.5.1 §AsyncOp extension-identity snapshot).

**Validation on the identity.** `<extension_name>` MUST match the
regex `[a-z][a-z0-9_]*` (enforced at extension install per
ICD-0.4.1). The wrapper passes it through `PQescapeIdentifier`
(libpq) so even an escaped identity cannot SQL-inject. If the
identity fails the regex defensively inside the dispatch arm, the
wrapper aborts with `db.search_path.set_failed` and the audit fires
— a correctness bug on the kernel side, never expected.

### Config override

`db.search_path.enforce: false` disables the wrapper kernel-wide.
Single-op `db.exec` / `db.query` runs bare against the connection's
default schema. `db.batch` still BEGINs/COMMITs but omits the `SET
LOCAL`. Intended ONLY as a deployment-ramp / perf-diagnostic escape
hatch — **production MUST set `enforce: true`**. Config-load emits
a warn-log when `enforce == false`. §Security Constraint 2 pins
this. OQ5 ratifies.

### Failure mode

If `SET LOCAL search_path` fails (e.g. the extension schema
`ext_<id>` was dropped out-of-band and the PG user lacks
`CREATE`), the dispatch arm catches the `DrogonDbException`, issues
`ROLLBACK` on the single-op wrapper (or rolls back the whole batch
for batch-open failures), audits `db.search_path.set_failed` with
the PG SQLSTATE, and rejects the JS promise with
`db.search_path.set_failed` — new rejection code joining the
ICD-0.3.3 db.* rejection table.

---

## OID-Driven PG-Type → JS-Type Mapping

Closes ICD-0.3.3 §PG-Value → JS-Value Conversion, deferred 2026-04-18,
pointer tightened to this milestone 2026-04-23 by
`RE-EVAL-0.5.x-following-0.5.1 §2.5`.

### Problem with the 0.3.3 heuristic

Current behavior (`src/kernel/js/stdlib/db_result_to_json.cpp`):

```cpp
std::string s = field.as<std::string>();    // PG text representation
if (s == "t") return Json::Value(true);
if (s == "f") return Json::Value(false);
int64_t i;
if (parse_int(s, i)) return Json::Value(i);
double d;
if (parse_float(s, d)) return Json::Value(d);
return Json::Value(s);                       // fall-through: string
```

Misreads: `SELECT 'true'::text` → `Json::Value(true)` (should be
string `"true"`). `SELECT 'f'::text` → `Json::Value(false)`. Safe
for ICD-0.3.3 §Tests Group A but empirically user-facing on real
data (names containing `"t"`, configuration strings etc.).

### Implementation strategy

Local Drogon patch in `third_party/drogon-patches/` exposes
`drogon::orm::Field::oid()` returning the PG OID (`Oid` / `uint32_t`)
from the underlying `PGresult::ftype(col)`. The patch is 4 lines —
one public method signature in `orm/Field.h`, three-line
implementation delegating to `result_.getOid(col)` (Drogon already
exposes the inner Result via a friend relationship with Row; the
patch adds the OID accessor that rides the existing seam).

Upstreamable; filed against drogon at <https://github.com/drogonframework/drogon>
on ship. Plinth pins to the patched build via
`cmake/FetchContent_MakeAvailable(drogon)` + patch-apply in the
same sequence 0.5.1 landed for the connection-checkout hook (see
`third_party/drogon-patches/README.md`).

### OID switch table

`src/kernel/js/stdlib/db_result_to_json.cpp` replaces the heuristic
with an OID switch. The full table:

| PG OID name | OID value | JS value | Notes |
|-------------|-----------|----------|-------|
| `BOOLOID` | 16 | `boolean` | `as<bool>()` |
| `INT2OID` | 21 | `number` | `as<int16_t>()` |
| `INT4OID` | 23 | `number` | `as<int32_t>()` |
| `INT8OID` | 20 | `number` / `string` | `as<int64_t>()`; `number` if in JS safe-integer range `[-2^53+1, 2^53-1]`, else `string` |
| `FLOAT4OID` | 700 | `number` | `as<float>()` → `double` widen |
| `FLOAT8OID` | 701 | `number` | `as<double>()` |
| `NUMERICOID` | 1700 | `number` | `as<std::string>()` → `std::strtod`; lossy for arbitrary precision, documented inline |
| `TEXTOID` | 25 | `string` | `as<std::string>()` — **string `"true"` stays string** |
| `VARCHAROID` | 1043 | `string` | `as<std::string>()` |
| `NAMEOID` | 19 | `string` | `as<std::string>()` |
| `CHAROID` | 18 | `string` | `as<std::string>()` (single char → one-char string) |
| `UUIDOID` | 2950 | `string` | `as<std::string>()` |
| `BYTEAOID` | 17 | `Uint8Array` | `as<std::vector<uint8_t>>()` then construct via `JS_NewArrayBufferCopy` + Uint8Array wrap; fresh copy (no aliasing) per ICD-0.3.3 |
| `JSONOID` | 114 | structural | `as<std::string>()` → `JSON.parse`; rejects with `db.internal` if parse fails (driver returned malformed JSON) |
| `JSONBOID` | 3802 | structural | same as JSON |
| `TIMESTAMPOID` | 1114 | `string` (ISO 8601) | `as<std::string>()` — PG's native text format already ISO 8601-like; passed through verbatim |
| `TIMESTAMPTZOID` | 1184 | `string` (ISO 8601, UTC) | `as<std::string>()` |
| `DATEOID` | 1082 | `string` (YYYY-MM-DD) | `as<std::string>()` |
| `TIMEOID` | 1083 | `string` (HH:MM:SS) | `as<std::string>()` |
| (anything else) | — | `string` (fail-closed) | `as<std::string>()` + rate-limited `db.oid_mapping.unknown` log warn |

SQL `NULL` (regardless of OID) → `null`. Detected before the OID
switch via `field.isNull()`.

### Pinned regression: `SELECT 'true'::text`

B.05 / T.01 in the test matrix: `SELECT 'true'::text AS t, 't'::text
AS s;` yields `{t: "true", s: "t"}`. Under the 0.3.3 heuristic
this was `{t: true, s: true}`. Under the 0.5.3 OID switch, both
columns are `TEXTOID` → the string stays string.

### Feature flag for staged rollout

`db.oid_mapping.enabled: true` (default) uses the OID switch.
`db.oid_mapping.enabled: false` falls back to the 0.3.3 heuristic.
Intended as a quick-disable for any post-ship regression; targeted
removal one milestone after 0.5.3 ships if no issues surface.
Config-load emits a warn-log when `enabled == false`. §OQ6 ratifies.

### Array types still deferred

`TEXT[]`, `INT4[]`, `UUID[]` and other array OIDs stay at the
default (fail-closed → string). Array promotion is a future
milestone — the JS-side shape would be a proper `Array<T>` but this
requires PG array-format parsing (libpq exposes the text format; a
parser that handles NULL elements, quoted strings with commas,
nested arrays, etc.) that's non-trivial and no real caller has
surfaced a dependency. ICD-0.3.3 §"any other type" posture holds.

### OID identity assumptions

The OID table values above are PostgreSQL's built-in type OIDs,
stable since PG 8.0. Plinth's supported PG version is ≥ 14 per the
compose file (`config/docker/pg.yml`); the OID values above are
pinned. Future PG releases may add new built-in types — new entries
land via the "anything else → string" fallback without a code
change, and the mapping table is extended per-PR as callers need.

---

## Extension Lifecycle Integration

### Drain hook

`js::rollback_extension_batches(const std::string& extension_name)`
is called at DISABLED / UPGRADING / UNINSTALL transitions from
`src/kernel/packages/install_lifecycle.cpp`, alongside the
coalescer's and broker's existing drain hooks. The call order:

```
on_disable(pkg):
    broker::drain_extension(pkg.name);                   // 0.5.2
    js::rollback_extension_batches(pkg.name);            // 0.5.3 NEW
    CoalescerRegistry::instance().drain_extension(pkg.name);  // 0.5.1
    bc_registry::teardown_all_for_extension(pkg.name);   // existing
```

Batch drain must run **before** coalescer drain so the rollback's
"discard accumulated scope" happens before the coalescer's drain
tries to flush any unclosed windows.

### What drain does

For each `bc` whose `bc.extension_name == extension_name`:

1. Inspect `bc.batch_state`. If not in a batch (`depth == 0`), skip.
2. Issue `ROLLBACK` synchronously on the pinned `TransactionPtr`
   (this is called from the install_lifecycle thread, not the JS
   thread — the rollback runs on Drogon's coroutine pool in a
   scheduled wakeup; the synchronous-looking call here blocks on
   that resolution via `drogon::app().getLoop()->runInLoop` and a
   promise).
3. `CoalescerRegistry::instance().discard_batch_scope(scope_id)` —
   drops accumulated counters; no envelope emits.
4. Reject the outer `db.batch` promise with `db.cancelled` (existing
   rejection code — batch drain is semantically identical to
   cancellation).
5. Unpin the connection; return it to Drogon's pool.
6. Audit `db.batch.rolled_back` with reason `extension_drained`.

A JS `db.exec` / `db.query` that is currently in-flight on the
pinned connection (caller awaiting) rejects with `db.cancelled` via
the existing `bc.cancelled` cascade (ICD-0.3.3 §Cancellation) — no
new cancellation machinery.

### Reconnect posture

Batches do not survive extension UPGRADE. A new RuntimePool slot
for the upgraded extension starts with no in-flight batches; any
caller awaiting a batch from the pre-upgrade slot receives
`db.cancelled`. Extensions that need idempotency around UPGRADE can
re-issue the batch from their `on_install` / `on_upgrade` hook.

---

## Config Surface

```json
{
  "db": {
    "batch": {
      "max_ops_per_batch": 500,
      "max_concurrent_batches_per_bc": 4,
      "timeout_ms": 30000,
      "audit_window_ms": 60000
    },
    "search_path": {
      "enforce": true
    },
    "oid_mapping": {
      "enabled": true
    },
    "silent": {
      "audit_window_ms": 60000
    }
  }
}
```

### Field semantics

- **`db.batch.max_ops_per_batch`** (uint, default 500) — maximum
  number of `db.exec` / `db.query` calls permitted inside one
  `db.batch` callback. Overflow rejects the offending
  `db.exec` / `db.query` with `db.batch.quota_exceeded`; the batch
  itself stays open (caller can await the overflow rejection and
  proceed to COMMIT with what succeeded, or throw to trigger
  ROLLBACK). Bound check `[1, 100000]`; out-of-range config fails
  load.
- **`db.batch.max_concurrent_batches_per_bc`** (uint, default 4) —
  maximum concurrent `db.batch` calls per `BridgeContext`. Since
  0.5.3 rejects nested batches anyway, this bounds parallel `Promise.all`-style
  batch usage from a single bc. Typically 1 is sufficient; 4
  accommodates early patterns that want to fan-out per-key. Bound
  `[1, 64]`.
- **`db.batch.timeout_ms`** (uint, default 30000) — batch wall-clock
  timeout from `DB_BATCH_BEGIN` resolve to the first of
  `DB_BATCH_COMMIT` / `DB_BATCH_ROLLBACK`. On timeout, the orchestrator
  enqueues a `DB_BATCH_ROLLBACK` with reason `timeout` and the outer
  promise rejects `db.batch.timeout`. Bound `[100, 600000]` (100 ms
  to 10 min).
- **`db.batch.audit_window_ms`** (uint, default 60000) — aggregation
  window for `db.batch.committed` / `.rolled_back` audits. Per-
  `(extension_name, outcome)` rate limit. Bound `[1000, 3600000]`.
- **`db.search_path.enforce`** (bool, default `true`) — see
  §Per-Op `SET search_path` Isolation §Config override. Warn-log
  emitted when `false`.
- **`db.oid_mapping.enabled`** (bool, default `true`) — see
  §OID-Driven PG-Type → JS-Type Mapping §Feature flag. Warn-log
  emitted when `false`.
- **`db.silent.audit_window_ms`** (uint, default 60000) — aggregation
  window for `db.silent.used` audits. Bound `[1000, 3600000]`.

### Config loader extension

`Config::apply_db` gains a new sub-parser for the `db` block — the
existing `Config::Db` struct (if present from 0.3.3) grows or is
created with the nested `batch` / `search_path` / `oid_mapping` /
`silent` substructs. If 0.3.3 left the `db` block out of the
config schema entirely (plausible — `db.*` worked off default
connection), the loader adds it as a first-class block alongside
`realtime`, `broker`, etc. Full parser follows the 0.5.2 pattern —
bound check + ranged defaults + warn-on-override.

### Defaults live in `config/defaults/prod.json`

All four sub-objects populate there. `config/defaults/dev.json`
inherits; no per-env overrides expected in 0.5.3 (timeouts/quotas
apply uniformly).

---

## Audit Events

All four new events are rate-limited via the same `audit_window_ms`
aggregation pattern ICD-0.5.2 §Audit Events established. Each event
type has a per-window counter keyed by `(extension_name, ...)` and
fires `at-first + at-window-end` with an aggregated payload
`{count_in_window: N, window_ms: <window>}`.

### `db.batch.committed`

Fires on every `DB_BATCH_COMMIT` dispatch arm completion (one per
batch). Payload:

```json
{
  "extension": "notes",
  "ops_in_batch": 47,
  "tables_touched": 3,
  "wall_clock_ms": 213,
  "count_in_window": 12,
  "window_ms": 60000
}
```

Aggregated per-`(extension)` — the `count_in_window` + `window_ms`
fields fire on the `N%60000ms == 0` boundaries. The first commit
hit in a fresh window fires the full per-commit payload; subsequent
commits within the window increment the counter; at window close,
an aggregate event fires with the total. Matches the posture
0.5.1 / 0.5.2 established.

### `db.batch.rolled_back`

Fires on every `DB_BATCH_ROLLBACK` arm. Payload:

```json
{
  "extension": "notes",
  "reason": "user_callback_threw" | "db_error" | "timeout" | "extension_drained" | "bc_destroyed",
  "error_code": "db.constraint_violation",
  "wall_clock_ms": 47,
  "count_in_window": 3,
  "window_ms": 60000
}
```

`reason` enumerates the five rollback triggers. `error_code` is
populated when `reason` is `user_callback_threw` or `db_error`
(carries the underlying `db.*` or user-thrown code); empty for the
other three reasons.

### `db.silent.used`

Fires on `db.exec(..., {silent:true})`. Payload:

```json
{
  "extension": "notes",
  "count_in_window": 8321,
  "window_ms": 60000
}
```

Aggregated only — no per-call audit (the counts would drown the log).
Operators alert on unexpectedly-high rates. Defaults to "fire first
hit in window + at window close with aggregate."

### `db.search_path.set_failed`

Fires on `SET LOCAL search_path` failure. Payload:

```json
{
  "extension": "notes",
  "sqlstate": "42P01",
  "message": "schema \"ext_notes\" does not exist",
  "count_in_window": 1,
  "window_ms": 60000
}
```

Rate-limited because a systematically-misconfigured extension
(wrong schema name, dropped schema, etc.) would otherwise flood the
audit log. Each aggregation carries the most-recent `sqlstate` +
`message` for debug context.

### No per-statement audit

No audit event fires per `db.exec` / `db.query` inside a batch — the
batch-commit audit covers the batch as a unit. Matches ICD-0.5.1's
"no per-flush audit" posture.

---

## HA Semantics

**Per-node transactional scope.** Batches are bounded to the PG
connection serving a single kernel node — a batch cannot span two
nodes. This is a trivial consequence of PG's transactional scope
(a transaction is one connection's state), not a new constraint.
Operators running multi-node Plinth already scope transactions
per-node at the SQL level; `db.batch` preserves that posture.

**No cross-node batch coordination.** 0.5.3 does NOT introduce
distributed-transaction semantics (no two-phase commit, no
external transaction coordinator, no Raft-committed batch log).
Extensions that need cross-node atomicity must either (a) pin all
relevant state to one node's schema, or (b) use application-level
compensation patterns outside the `db.batch` surface.

**Per-node Layer-1 emission on commit.** Batch-commit envelopes
emit via `emit_notify_async` on the commit node's PG connection.
They fan out through `LISTEN/NOTIFY` to every node (ICD-0.5.0 §HA
Semantics invariant, verbatim), so subscribers on any node receive
the envelope. This matches coalescer-window emission exactly —
the broker (0.5.2) sees no difference between a window-flushed and
a batch-flushed envelope beyond the `window_ms` field value.

---

## Deterministic Teardown

### Atexit chain (updated from 0.5.2)

Per `feedback_deterministic_teardown.md` — every future subsystem with
framework callbacks needs `cancel_all_*` called from atexit before
`drogon::app().quit()`.

```
atexit(order):
    1. capabilities::cancel_all_pending_calls();
    2. realtime::stop_listener();
    3. js::rollback_all_batches();              ← NEW 0.5.3
    4. realtime::stop_broker();                 // 0.5.2
    5. CoalescerRegistry::instance().shutdown();  // 0.5.1
    6. drogon::app().quit();
```

### What `rollback_all_batches` does

1. Walks every `RuntimePool`; for each pool, walks every `bc`;
   for each `bc` whose `batch_state.depth > 0`, issues `ROLLBACK`
   synchronously on the pinned connection.
2. Discards every coalescer batch scope (calls
   `CoalescerRegistry::instance().discard_all_batch_scopes()` —
   public seam mirroring `flush_batch_scope` but no emit).
3. Awaits all rollback coroutines (bounded by
   `db.batch.timeout_ms`; process teardown doesn't wait longer
   than one batch's timeout).
4. Audits `db.batch.rolled_back` with reason `process_shutdown`
   for each rolled-back batch.

### Ordering rationale

- Batches roll back **before** broker stop so any emission the
  broker would have fanned out is suppressed before the broker's
  EventHandler de-registers.
- Batches roll back **before** coalescer shutdown so the
  coalescer's final drain sees no in-flight scope state (simpler
  shutdown contract — coalescer drains windows only, not batches).
- Batches roll back **after** listener stop (nothing left listening
  for PG NOTIFY; no new envelopes arriving) and **after**
  `cancel_all_pending_calls` (any in-flight `cap.call` awaiting a
  `db.batch` inside its callee already cancelled).

### Mirror in `ws_test_fixture.cpp`

Test fixtures (`tests/kernel/ws/ws_test_fixture.cpp`,
`tests/kernel/js/async_bridge_fixture.cpp`) gain a mirror call —
`js::rollback_all_batches()` in the per-fixture teardown path,
before the drogon `quit()` the fixtures do manually. Matches the
0.5.2 pattern (broker stop + listener stop in fixtures).

---

## Error Model

### Batch-side errors (JS-facing)

| Code | Trigger | Fire point |
|------|---------|------------|
| `db.batch.nested_not_allowed` | `db.batch` called while `bc.batch_state.depth > 0` | Synchronous (binding-level) |
| `db.batch.quota_exceeded` | `op_count_in_batch > db.batch.max_ops_per_batch` | Synchronous at `db.exec` / `db.query` enqueue |
| `db.batch.concurrent_limit_exceeded` | More than `db.batch.max_concurrent_batches_per_bc` batches in flight for one bc | Synchronous at `db.batch` enqueue |
| `db.batch.timeout` | Batch wall-clock exceeds `db.batch.timeout_ms` | Async at the timeout wakeup |
| `db.batch.rolled_back` | Wrapper for underlying `db.*` error rolled back the batch | Async at the rollback arm |
| `db.batch.cross_extension_not_allowed` | A `db.exec` inside a batch touches a schema not matching `ext_<bc.extension_name>` or `plinth.*` | Async at classify-time in the dispatch arm |
| `db.batch.backend_unsupported` | Batch attempted on a non-PG DbClient | Synchronous at `db.batch` enqueue |
| `db.search_path.set_failed` | `SET LOCAL search_path` PG call failed | Async at the dispatch arm |

### Silent-flag errors

None. `{silent: true}` is always accepted. `{silent: "truthy"}` is
coerced per `JS_ToBool` (ICD-0.3.3 convention — no error).

### OID-mapping errors

- `db.oid_mapping.json_parse_failed` — new rejection code when a
  `JSONB` column's text parse fails. Rare in practice (PG guarantees
  syntactic validity); a true driver bug would surface.
- Unknown OIDs never raise — fall-through to string with a
  rate-limited log warn (not an audit event; `log.warn` only).

### Existing `db.*` errors

Every ICD-0.3.3 rejection code (`db.syntax_error`, `db.undefined_table`,
`db.constraint_violation`, `db.permission_denied`,
`db.serialization_failure`, `db.deadlock_detected`, `db.timeout`,
`db.cancelled`, `db.connection_error`, `db.internal`) fires unchanged.
Inside a batch, any of these triggers the rollback path; the outer
batch promise rejects `db.batch.rolled_back` with the underlying
code on `error_code` in the audit payload.

### Configuration load failures

- `db.batch.max_ops_per_batch` out of `[1, 100000]` → config-load
  fails with the existing `Config::Error::OutOfRange` family.
- `db.batch.timeout_ms` out of `[100, 600000]` → same.
- `db.oid_mapping.enabled == false` → load succeeds with warn-log
  (staged rollout; not an error).
- `db.search_path.enforce == false` → load succeeds with warn-log.

---

## Security Constraints

1. **`SET LOCAL search_path` mandatory for extension-scope `db.*`.**
   Every `db.exec` / `db.query` from a `bc` with non-empty
   `extension_name` runs inside a transaction (single-op or batch)
   that issues `SET LOCAL search_path TO ext_<name>, plinth;` as its
   first statement. Kernel-scope bc (`extension_name.empty()`)
   explicitly skips the wrapper. No other path from an extension JS
   program reaches `execSqlCoro` without the wrapper. Escape hatch
   via `db.search_path.enforce: false` requires deployment config
   change + warn-log + documented regression posture.

2. **`BEGIN` / `COMMIT` bracket inviolate per batch.** A `db.batch`
   that reaches `DB_BATCH_COMMIT` completes or rolls back. No
   partial commit, no caller-triggered half-open state. Atexit +
   lifecycle drain roll back any in-flight batch atomically. The
   Drogon `TransactionPtr` destructor is a belt-and-braces rollback
   (issues implicit rollback) but the code path pins explicit
   `COMMIT` / `ROLLBACK` for determinism.

3. **Cross-extension batch prohibited.** A `db.exec` inside a
   `db.batch` whose target schema does not match
   `ext_<bc.extension_name>` (or `plinth.*` for kernel tables) is
   rejected with `db.batch.cross_extension_not_allowed`. The
   search_path wrapper already isolates writes to the extension's
   schema by default (the extension couldn't name another schema
   without a qualified `ext_other.table` reference), but this
   constraint makes the attempt-detection explicit at classify-
   time and fires a security-event audit.

4. **No savepoint / nested batches.** `db.batch` inside `db.batch`
   rejects synchronously with `db.batch.nested_not_allowed`. No
   `SAVEPOINT` API, no implicit flattening. One layer of transaction
   depth only — the whole batch is one atomic unit.

5. **Audit row always written for completed batches.** The
   `db.batch.committed` and `db.batch.rolled_back` audits are NOT
   subject to `silent`-style suppression. Every batch that opens
   produces one of these two audits at close (rate-limited per
   `db.batch.audit_window_ms`). Silent only suppresses Layer-1
   realtime emission on the write side — NEVER audit.

6. **Batch honors cancellation cascade.** `bc.cancelled` → `true`
   mid-batch → the next `db.exec` / `db.query` rejects with
   `db.cancelled`; the binding-orchestrator's catch enqueues
   `DB_BATCH_ROLLBACK`; the batch promise rejects `db.cancelled`.
   Matches ICD-0.3.3 §Cancellation Cascade verbatim.

7. **OID mapping fail-closed.** Unknown OID → `string` (fail-closed,
   never bool/number). Matches ICD-0.3.3 §"any other type → string"
   posture. A rate-limited log warn fires; operators can choose to
   widen the OID table per-PR when a caller needs new types.

8. **Connection pinning RAII.** The pinned `TransactionPtr` is held
   on `bc.batch_state.pinned_conn`. The `BridgeContext` destructor
   issues `ROLLBACK` + unpins + returns conn to pool. No reachable
   path can leak a pinned connection across `bc` teardown.

---

## Implementation deviation (v0.5.3 ship + 0.5.3.1 follow-up)

Recorded per METHODOLOGY §Phase 2 Constraint #4 + ratified at
`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md §4` rows D18–D21
(plus 0.5.3.1's B.06 + B.13 closures).

1. **T.\* test cases live in `async_bridge_test.cpp`**, not
   `stdlib_test.cpp` as the §Test Cases table cell originally
   suggested. T.\* require a live `PGresult` and the OID-driven type
   conversion only fires through the async bridge; tagged
   `[js][async][db][types]`.
2. **T.07 `'true'::text` → `'t'::text`.** The 0.3.3 string-parse
   heuristic only mis-classifies *single-character* PG bool text
   reprs (the literal `t` / `f`); the 4-char string `'true'::text`
   passes through correctly. T.07 narrowed to the regression case
   the OID switch actually fixes.
3. **P.05 narrowed.** ICD §Test Cases originally pinned "ext schema
   dropped" as the failure mode. PG's `SET LOCAL` is permissive on
   non-existent schemas (no error until a query references the
   schema), so the test assertion shifted to "regex defense rejects
   malicious identity" — same security invariant, more direct
   observable.
4. **`Config::Db` field named `db_bindings` in C++.** The JSON key
   `"db"` matches the ICD §Config Surface; the C++ field name
   `db_bindings` avoids collision with the existing
   `Config::Database db;` member. Forward-compatible — the JSON
   shape is unchanged.
5. **B.06 timeout enforcement deferred at v0.5.3 ship; closed in
   0.5.3.1.** Phase 4 shipped the `db.batch.timeout_ms` config knob
   without runtime enforcement; 0.5.3.1 added the
   `trantor::EventLoop::runAfter` timer + `BatchState::timed_out`
   flag + `db.batch.timeout` rejection code + `clear_batch_timer`
   discard hooks. New public surface in
   `src/kernel/js/db_batch_audit.hpp`:
   `set_batch_timeout_ms(std::size_t)`, `batch_timeout_ms()`,
   `set_batch_timer(...)`, `clear_batch_timer(...)`. See CHANGELOG
   2026-04-24 §0.5.3.1 Part A.
6. **§Security Constraint 3 cross-extension assertion deferred at
   v0.5.3 ship; closed in 0.5.3.1.** Phase 5 shipped without an
   explicit cross-extension test; 0.5.3.1 added
   `src/kernel/js/db_batch_schema_check.{hpp,cpp}` with
   `classify_cross_extension(sql, expected_ext)` regex scan +
   `audit_batch_cross_extension_rejected` rate-limited audit + B.13
   test case. False-positive trade-off documented inline (SQL
   string literals containing `ext_<word>` substrings will be
   rejected; same posture as the coalescer's regex classifier).
   See CHANGELOG 2026-04-24 §0.5.3.1 Part B.
7. **B.07 narrowed** from "DISABLE mid-batch" to
   "`discard_batches_for_extension` drops the scope" — drives the
   drain function directly rather than the full `install_lifecycle`
   harness. The drain surface is the kernel-side contract.
8. **B.11 narrowed** from "cancellation fires mid-batch from
   dispatch cascade" to "pre-cancelled bc rejects `db.batch`
   inline" — mid-batch cascade requires the dispatch-cascade drain
   path; covered by phase 5's lifecycle work.

---

## Test Cases

Test prefix: **B.\*** for batch semantics (open/commit/rollback);
**S.\*** for silent flag; **P.\*** for per-op `SET search_path`
isolation; **T.\*** for OID-driven type mapping; **I.\*** for
end-to-end integration (batch → coalescer → listener → WS frame).
Tag convention `[js][db]` + per-group subtype (`[batch]`, `[silent]`,
`[search_path]`, `[types]`, `[integration]`). Distinct from 0.5.0's
R/E/P, 0.5.1's C/T/I/E, and 0.5.2's B/S/U/I prefixes.

Total: **36 new cases** (13 B + 6 S + 8 P + 7 T + 2 I). Distributed
across five test TUs (two new, three extended). The 13th B case
(B.13 cross-extension) was added 2026-04-24 in the 0.5.3.1
follow-up alongside B.06 timer enforcement.

### Batch semantics — `tests/kernel/js/db_batch_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| B.01 | Happy commit | Batch with 3 inserts; callback returns normally | Yes | All 3 rows present post-commit; one envelope per table emits; `db.batch.committed` audit fires; outer promise resolves with callback return value |
| B.02 | Rollback on user throw | Callback runs 2 inserts then throws `Error("boom")` | Yes | 0 rows post-rollback (both inserts undone); no envelope emits; `db.batch.rolled_back` audit reason=`user_callback_threw` error_code populated; outer promise rejects with the thrown error |
| B.03 | Rollback on DB error | Callback runs INSERT then UPDATE causing constraint violation | Yes | 0 rows post-rollback; audit reason=`db_error` error_code=`db.constraint_violation`; outer promise rejects `db.batch.rolled_back` |
| B.04 | Nested batch rejects | Callback calls `db.batch(...)` inside `db.batch(...)` | No | Inner rejects synchronously `db.batch.nested_not_allowed`; outer still completes normally (if outer doesn't re-throw) |
| B.05 | Quota exceeded mid-batch | Batch with 501 inserts; `max_ops_per_batch=500` | Yes | 501st rejects `db.batch.quota_exceeded`; batch is still open; if caller awaits + throws, batch rolls back; if caller swallows + exits, batch commits the first 500 |
| B.06 | Timeout | Batch callback sleeps 200ms; `timeout_ms=100` | Yes | Outer promise rejects `db.batch.timeout`; rollback runs; audit reason=`timeout`. **Implementation deviation 2026-04-24** (0.5.3.1): the test substitutes a server-side `pg_sleep(0.4)` between two `INSERT`s with `timeout_ms=200` so it runs without the opt-in `__host_sleep_ms__` shim — same observable contract (timer fires while the main loop is free during pg_sleep; second op rejects inline). |
| B.07 | Drain on DISABLE | `notes` ext starts batch; DISABLE fires mid-batch | Yes | Rollback runs; audit reason=`extension_drained`; outer promise rejects `db.cancelled` |
| B.08 | Empty batch | `db.batch(async () => {})` | Yes | BEGIN; COMMIT runs (empty transaction); no envelope emits; `db.batch.committed` audit ops_in_batch=0 |
| B.09 | Silent inside batch | Batch with 2 inserts, second has `silent:true` | Yes | Both rows present; ONE envelope emits (from the first insert's table); `db.silent.used` audit fires |
| B.10 | Metrics counters monotonic | 5 batches commit, 3 batches rollback | Yes | `db.batch.committed` count = 5; `db.batch.rolled_back` count = 3 (across the window) |
| B.11 | Cancellation during batch | Batch in flight; `bc.cancelled = true` | Yes | Next `db.exec` inside batch rejects `db.cancelled`; rollback runs; batch rejects `db.cancelled` |
| B.12 | Rollback resource leak | 100 batches each rolled back | Yes | Post-test: `bc.batch_state.depth == 0`; no orphaned TransactionPtr; DbClient pool connection count unchanged |
| B.13 | Cross-extension batch | `batch` ext starts batch; INSERT into `notes` (own schema) succeeds; INSERT into `ext_other.notes` rejects | Yes | Outer rejects `db.batch.cross_extension_not_allowed`; both schemas empty post-rollback; `db.batch.cross_extension_rejected` audit fires. Negative arm: `plinth.*` reference inside the same ext's batch does NOT trip the classifier. **Added 2026-04-24** (0.5.3.1) to close the §Security Constraint 3 deferral. |

### Silent flag — `tests/kernel/js/db_silent_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| S.01 | Silent suppresses coalescer | `db.exec("INSERT...", [], {silent:true})` | Yes | Row present; NO envelope emits; `db.silent.used` audit fires |
| S.02 | Silent + zero rows | `db.exec("UPDATE...WHERE false", [], {silent:true})` | Yes | 0 rows affected; no envelope (matches non-silent zero-row behavior); audit fires |
| S.03 | Silent does not suppress audit.log | Extension JS calls `audit.log` AND `db.exec silent:true` | Yes | Audit row from `audit.log` still written (unchanged); `db.silent.used` audit also fires |
| S.04 | Silent on SELECT no-op | `db.query("SELECT...", [], {silent:true})` | Yes | Query succeeds; no audit; no envelope (SELECT never emits); no error on the unknown opt |
| S.05 | Silent inside batch | Covered by B.09 — referenced here for completeness |  | (pointer) |
| S.06 | Config override `silent.audit_window_ms` | Set `audit_window_ms=10000`; 3 silent writes inside window | Yes | One initial audit + one aggregated audit at 10s boundary |

### Per-op search_path — extends `tests/kernel/js/async_bridge_test.cpp` + `tests/kernel/js/run_on_context_test.cpp`

Closes ICD-0.5.0.3 §P.04 test deferred-on-search_path.

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| P.01 | Single-op wrapper applied | `notes` ext runs `db.exec("INSERT INTO notes...")` (unqualified) | Yes | Wire trace shows `BEGIN; SET LOCAL search_path TO ext_notes, plinth; INSERT INTO notes...; COMMIT;`; row lands in `ext_notes.notes` (not `public.notes`) |
| P.02 | Inside-batch single `SET LOCAL` | `notes` ext runs batch with 3 unqualified inserts | Yes | Wire trace shows `BEGIN; SET LOCAL search_path ...; INSERT...; INSERT...; INSERT...; COMMIT;` — ONE `SET LOCAL`, not three |
| P.03 | Kernel-scope bypass | Host-context bc (`extension_name==""`) runs `db.exec("INSERT INTO plinth.events...")` | Yes | No wrapper; raw exec against default schema `plinth` |
| P.04 | Cross-extension schema isolation | `notes` ext runs `db.exec("INSERT INTO ext_terminal.sessions...")` | Yes | Rejects `db.permission_denied` (PG permissions enforce, not this ICD's code) |
| P.05 | `SET LOCAL` failure | `notes` ext with `ext_notes` schema dropped out-of-band; runs `db.exec` | Yes | Rejects `db.search_path.set_failed`; audit fires with sqlstate `3F000` or `42P01` |
| P.06 | search_path resets between ops | Two sequential `db.exec` calls from same bc | Yes | Each runs its own wrapper; schema state does not leak between them (new session — PG pool connection checkout) |
| P.07 | Identity regex defense | Inject `bc.extension_name = "; DROP TABLE --"` synthetically in test | No | `PQescapeIdentifier` returns a quoted identifier; `SET LOCAL` fails safely; `db.search_path.set_failed` |
| P.08 | `enforce=false` disables wrapper | Config with `db.search_path.enforce=false`; `notes` ext runs `db.exec` | Yes | No `BEGIN/COMMIT` wrap; no `SET LOCAL`; raw exec |

### OID-driven type mapping — extends `tests/kernel/js/async_bridge_test.cpp`

Landed in `async_bridge_test.cpp` on implementation (0.5.3 phase 1),
not `stdlib_test.cpp` as originally named — all T.\* need a live
PGresult (OID comes from libpq's `PQftype`) and `async_bridge_test.cpp`
is the TU wired for PG-gated `db.query` calls. Amendment 2026-04-24.

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| T.01 | String `"true"` stays string | `SELECT 'true'::text AS t;` | Yes | `{t: "true"}` (NOT bool) — locks the 0.3.3 heuristic regression |
| T.02 | INT8 safe range | `SELECT 42::int8 AS n;` | Yes | `{n: 42}` (JS number) |
| T.03 | INT8 overflow | `SELECT 9999999999999999::int8 AS n;` | Yes | `{n: "9999999999999999"}` (string — outside safe-integer range) |
| T.04 | TIMESTAMPTZ → ISO8601 | `SELECT '2026-04-24T12:00:00Z'::timestamptz AS t;` | Yes | `{t: "2026-04-24 12:00:00+00"}` (PG's ISO 8601 text format) |
| T.05 | BYTEA → Uint8Array | `SELECT '\\xdeadbeef'::bytea AS b;` | Yes | `{b: Uint8Array([0xde, 0xad, 0xbe, 0xef])}` |
| T.06 | Null preserved | `SELECT NULL::int4 AS n;` | Yes | `{n: null}` |
| T.07 | `oid_mapping.enabled=false` heuristic | Config override; `SELECT 't'::text AS t;` | Yes | `{t: true}` — legacy heuristic confirmed. Amendment 2026-04-24: the 0.3.3 heuristic only matches single-char `"t"`/`"f"` PG bool text reprs, NOT the 4-char string `"true"`; scenario narrowed to `'t'::text` which does surface the regression. |

### End-to-end integration — `tests/integration/db_batch_integration_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| I.01 | Batch → coalescer → listener → WS | Admin WS client subscribes `plinth:data:ext_i01.notes`; ext runs `db.batch` with 5 INSERTs | Yes | Client receives ONE `{type:"event"}` frame within `batch_commit + 100 ms`; payload envelope `ops[0].count=5` and `window_ms=0` (batch boundary) |
| I.02 | Concurrent batches no cross-contamination | Two bcs from same extension each run a batch of 10 INSERTs concurrently (in parallel event loops) | Yes | 20 rows total; each batch commits; two envelopes emit (one per batch), each with `ops[0].count=10` — counts do not merge |

### Test-seam notes

- **B.\*** (12) in `plinth_tests_pg` — batch semantics need real PG
  transactions; the in-memory stub approximation from 0.3.3 doesn't
  capture `BEGIN/COMMIT` semantics.
- **S.\*** (6) mostly in `plinth_tests_pg`; S.04 pure (no PG since
  no envelope emits).
- **P.\*** (8) in `plinth_tests_pg`; P.07 pure (synthetic identity).
- **T.\*** (7) all in `plinth_tests_pg` — OID comes from a live
  PGresult.
- **I.\*** (2) in `plinth_tests_pg`.

- `broker::dispatch_for_test` + the 0.5.2 broker-test seams remain
  usable — batch-commit envelopes reach the broker identically to
  window-flushed ones.
- New test helper `BatchTestFixture` — manages a kernel-JS bc with
  `async_bridge_fixture.cpp` scaffolding extended for batch open +
  assert-rollback patterns. Lives in the same fixture TU.

### CI wiring

- `src/kernel/js/stdlib/db_bindings.cpp` — `db.batch` binding
  added; existing `db_query` / `db_exec` extended with in-batch
  routing.
- `src/kernel/js/async_op.hpp` — three new enum variants +
  `batch_scope_id` field.
- `src/kernel/js/run_on_context.cpp` — three new dispatch arms;
  existing `run_db_query_outcome` / `run_db_exec_outcome` extended
  with single-op wrapper injection + pinned-connection routing.
- `src/kernel/js/bridge_context.hpp` / `.cpp` — `batch_state`
  field + destructor rollback logic.
- `src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}` — **new TU**
  (amendment 2026-04-24; the pre-0.5.3 heuristic lived inline in
  `run_on_context.cpp` as `pg_text_to_json` + `pg_result_to_json`,
  not in a standalone file). 0.5.3 extracts into the new TU and
  replaces the heuristic with the OID switch (heuristic retained
  behind `oid_mapping.enabled=false`).
- `third_party/drogon-patches/` — **new directory** (amendment
  2026-04-24; no existing patch-apply machinery in 0.5.2). 0.5.3
  adds the directory + `README.md` + `ftype-accessor.patch`, and
  wires `PATCH_COMMAND git apply ...` into drogon's
  `FetchContent_Declare` at top-level `CMakeLists.txt`.
- `src/kernel/realtime/coalescer.hpp` / `.cpp` —
  `flush_batch_scope(scope_id)` + `discard_batch_scope(scope_id)`
  public seams; `record_write` gains optional `batch_scope_id`
  parameter.
- `src/kernel/config.hpp` / `.cpp` — `Config::Db` nested substructs
  + parser extension.
- `src/kernel/main.cpp` — atexit chain entry.
- `src/kernel/packages/install_lifecycle.cpp` — three
  `js::rollback_extension_batches(pkg.name)` call sites.
- `third_party/drogon-patches/ftype-accessor.patch` — new
  upstreamable patch. (See `drogon-patches` directory amendment
  above.)
- `tests/kernel/js/db_batch_test.cpp` — new TU.
- `tests/kernel/js/db_silent_test.cpp` — new TU.
- `tests/integration/db_batch_integration_test.cpp` — new TU.
- `tests/kernel/js/async_bridge_test.cpp` — extended with P.\*
  cases.
- `tests/kernel/js/run_on_context_test.cpp` — extended with P.\*
  cases.
- `tests/kernel/js/async_bridge_test.cpp` — extended with T.\*
  cases (amendment 2026-04-24; see the T.\* test-case table above).
- `tests/kernel/ws/ws_test_fixture.cpp` + async bridge fixture —
  atexit mirror updated.
- `CMakeLists.txt` — explicit test-source enumeration (amendment
  2026-04-24: no glob is used; each new TU is added to the
  `plinth_tests` target's explicit source list at the top-level
  `add_executable` block).
- `migrations/schema.sql` — no edits. No new tables.

### Test count target

**35 new cases.** Full suite grows by 35 TEST_CASEs distributed
across `plinth_tests_pure` + `plinth_tests_pg`. No new subprocess
count; no new ctest entry.

---

## Entry / Exit

**Entry criteria:**

- v0.5.0 merged + tagged (done — commit `f3552b3`, tag `v0.5.0`).
- v0.5.1 merged + tagged (done — commit `90d37fc`, tag `v0.5.1`).
- v0.5.2 merged + tagged (done — commit `68298b4`, tag `v0.5.2`).
- 0.5.2.N broker test backfill merged (done — `feat/0.5.2.N-broker-test-backfill`
  2026-04-24).
- ICD-0.5.3 authored (this document, squash-merged as 0.5.2.N paper
  slot).
- `BridgeContext::extension_name` populated per-pool at install-
  lifecycle (done — v0.4.4 + v0.5.0.4).
- `AsyncOp::silent` field plumbed (done —
  `src/kernel/js/async_op.hpp:82` since 0.3.3).
- CoalescerRegistry public API shipped (done — v0.5.1).
- `emit_notify_async` primitive shipped (done — v0.5.0).
- Extension schemas (`ext_<id>`) creation path shipped (done —
  v0.4.3).

**Exit criteria:**

- `src/kernel/js/stdlib/db_bindings.cpp` `db.batch` binding ships.
- `src/kernel/js/async_op.hpp` three new enum variants +
  `batch_scope_id` field.
- `src/kernel/js/run_on_context.cpp` three new dispatch arms; existing
  DB_QUERY / DB_EXEC arms extended with single-op wrapper.
- `src/kernel/js/bridge_context.hpp/.cpp` `batch_state` field +
  destructor rollback.
- `src/kernel/js/stdlib/db_result_to_json.cpp` OID-driven mapping
  replaces heuristic (heuristic behind feature flag).
- `third_party/drogon-patches/ftype-accessor.patch` committed and
  applied in the drogon fetch.
- `src/kernel/realtime/coalescer.{hpp,cpp}` `flush_batch_scope` +
  `discard_batch_scope` + `record_write` extension.
- `Config::Db` + nested substructs + config-loader support with
  bound-check validation.
- `main.cpp` + fixture teardown mirror the atexit chain.
- `install_lifecycle.cpp` three transitions call
  `js::rollback_extension_batches`.
- All 35 B/S/P/T/I test cases pass; PG-gated cases skip cleanly
  when PG env absent.
- `run-clang-tidy-20` zero findings on new TUs + modified TUs.
- No regressions on the v0.5.0 + v0.5.1 + v0.5.2 + 0.5.0.4 test
  matrix.
- Atexit-race validation — 20-run ctest loop sample shows zero
  teardown-race reproductions (specifically: zero in-flight-batch-
  at-shutdown aborts).
- `CHANGELOG.md` `v0.5.3` entry describes `db.batch`, `silent`
  semantics pin, per-op search_path wrapper, OID mapping migration,
  the coalescer seam extensions, the config surface, the four audit
  events, and every accepted OQ deviation.
- `docs/DEFERRED.md` 2026-04-18 entries (search_path + OID mapping)
  moved to the Resolved section with references to this ICD.
- `docs/ROADMAP.md §0.5` line for 0.5.3 is removed.
- `v0.5.3` tag cut on the merge commit.
- Memory `project_plinth_state.md` updated to reflect 0.5.3 shipped;
  next-session memory entry pointing at 0.5.4 delta-sync
  work (or the interim paper slot for ICD-0.5.4 authoring per
  horizon rule).

---

## Open Questions

**OQ1 — Nested batch flatten vs. reject.** The ICD pins
**reject synchronously with `db.batch.nested_not_allowed`**.
Alternatives:
(a) silently flatten (inner batch is a no-op; statements run in the
outer transaction);
(b) implicit SAVEPOINT (nested batch uses `SAVEPOINT`/`RELEASE`;
nested rollback only undoes inner work);
(c) the reject pinned here.
**Recommendation:** (c). Rationale: (a) hides intent and makes the
"one batch = one unit of atomicity" model leaky; (b) is a 0.5.5+
feature when a real caller needs partial rollback. Architect:
confirm or redirect to (a) or (b).

**OQ2 — Batch error propagation shape.** The ICD pins
**rejection with the underlying error** — `db.batch.rolled_back`
wraps the original user-thrown or DB error on the audit payload
side, but the outer promise rejects with the caller-visible
cause (the thrown `Error` or the `db.*` code). Alternative: always
reject `db.batch.rolled_back` and put the cause on a `.cause`
field. **Recommendation:** reject-with-cause, as pinned.
Rationale: caller's existing `try/catch` around `db.batch` expects
to see the same error it would see without the wrapper; a uniform
`db.batch.rolled_back` code loses information. Architect: confirm.

**OQ3 — `silent` flag scope — single call vs. coro-scoped.**
The ICD pins **single-call scope**. Alternatives: (a) "rest of this
coroutine" (binding stores silent-mode on bc until coro yields);
(b) RAII-style (`using (db.silent())` — requires JS syntax support);
(c) single-call as pinned. **Recommendation:** (c). Rationale: (a)
creates hard-to-debug "silent leak" bugs when a coro yields then
resumes. (b) needs JS syntax plinth doesn't ship. (c) is explicit.
Architect: confirm.

**OQ4 — Batch-end emission timing vs. 50ms coalescer window.** The
ICD pins **emit-at-COMMIT** (one envelope per `(schema,table)` per
batch commit, `window_ms=0`). Alternative: write-into-existing-
windows (let the 50 ms timer capture the batch alongside any
co-temporal non-batch writes). **Recommendation:** emit-at-COMMIT.
Rationale: explicit batch boundary should win over timer; a batch of
1000 inserts shouldn't trickle out 50 ms-windowed envelopes as the
callback runs — the user expects one envelope at commit.
Architect: confirm.

**OQ5 — Per-op `SET search_path` cost vs. opt-in.** The ICD pins
**always wrap + `enforce=false` escape hatch**. Alternative: opt-in
at `db.exec` call site (`{search_path: true}`). **Recommendation:**
always wrap. Rationale: ICD-0.3.3 §Security Constraint 1 says the
isolation MUST hold; opt-in makes it a caller-enforcement pattern
that's easy to forget. The perf cost (1–2 extra round trips per
single-op call, amortized via pipelining) is acceptable given the
security posture. Architect: confirm. Measured cost to re-validate
on ship.

**OQ6 — OID access strategy.** The ICD pins **local Drogon patch
exposing `Field::oid()`**. Alternatives: (a) reach into Drogon's
private `PGresult` (e.g. via `reinterpret_cast` on the inner
`Result::getResult()` pointer — the pointer is accessible via the
already-friend `Row` seam); (b) hard-code `as<T>()` per expected
column type (defeats the generic `db.query` shape); (c) the local
patch pinned here. **Recommendation:** (c). Rationale: smallest
upstreamable patch, no fragile private-API reach, keeps the
generic `db.query` shape. Architect: confirm. File against drogon
upstream on ship.

**OQ7 — Batch connection ownership model.** The ICD pins
**pin `TransactionPtr` on `bc.batch_state` for the batch's duration**.
Alternative: rely on Drogon's per-statement connection checkout
(each `db.exec` inside the batch asks the pool for a connection;
the pool must route to the same one — probably not possible without
additional Drogon work). **Recommendation:** pin as pinned.
Rationale: the alternative is not reachable without upstream Drogon
changes; pinning is the clean implementation in the Drogon 1.9.x
surface. Architect: confirm.

**OQ8 — Batch quota shape — both or one?** The ICD pins
**both `max_ops_per_batch=500` and
`max_concurrent_batches_per_bc=4`**. Alternative: drop
`max_concurrent_batches_per_bc` (bound is implicit — one bc, one
coro, sequential awaits; parallel batches require `Promise.all` and
are unusual). **Recommendation:** both, as pinned. Rationale: cheap
defensive bound; zero cost if callers never hit it. Architect: confirm
or drop one.

---

## Appendix A — End-to-End Example

Extension `notes` runs:

```javascript
const [a_id, b_id] = await db.batch(async () => {
  const a = await db.query(
    "INSERT INTO notes(title) VALUES('a') RETURNING id");
  const b = await db.query(
    "INSERT INTO notes(title) VALUES('b') RETURNING id");
  return [a.rows[0].id, b.rows[0].id];
});
```

**Step 1 (JS thread).** Binding validates:
- `fn` is a function ✓
- `bc.batch_state.depth == 0` ✓
- `bc.batch_state.in_flight_count < 4` ✓

Allocates `scope_id = 171`. Sets `bc.batch_state.depth = 1`. Enqueues
`AsyncOp{DB_BATCH_BEGIN, batch_scope_id=171, callback_id=91}`.
Returns a promise `P_batch` to the JS caller; returns another promise
`P_begin_resolved` back to the orchestrator.

**Step 2 (Drogon coro pool).** `DB_BATCH_BEGIN` dispatch arm:
- Allocates `txn = db_client->newTransaction()`.
- Awaits `txn->execSqlCoro("SET LOCAL search_path TO ext_notes, plinth")`.
- Stores `txn` on `bc.batch_state.pinned_conn`.
- Resolves `P_begin_resolved`.

**Step 3 (JS thread).** `P_begin_resolved` resolves → orchestrator
invokes `fn()`.

**Step 4 (JS thread → Drogon coro pool → JS thread, twice).** User
awaits two `db.query` calls. Each:
- Binding notices `bc.batch_state.depth > 0`, skips auto-wrapper.
- Enqueues `AsyncOp{DB_QUERY, batch_scope_id=171, sql="INSERT ..."}`.
- Dispatch arm: `co_await txn->execSqlCoro(sql)` (the pinned conn);
  classifier sees INSERT + table=`notes`, schema=`ext_notes`;
  `CoalescerRegistry::record_write` accumulates into scope 171 for
  (`ext_notes`, `notes`): `{insert: 1}`, then `{insert: 2}`.
- Returns the row to JS.

**Step 5 (JS thread).** Both queries resolved. `fn()` returns
`[row_a.id, row_b.id]`. Orchestrator enqueues
`AsyncOp{DB_BATCH_COMMIT, batch_scope_id=171, callback_id=<resolve_P_batch>}`.

**Step 6 (Drogon coro pool).** `DB_BATCH_COMMIT` dispatch arm:
- `co_await txn->execSqlCoro("COMMIT")`.
- `bc.batch_state.pinned_conn.reset()` → txn destructor returns
  conn to pool.
- `bc.batch_state.depth = 0`.
- `CoalescerRegistry::instance().flush_batch_scope(171)` →
  builds envelope `{layer:"data", channel:"plinth:data:ext_notes.notes",
  schema:"ext_notes", table:"notes", ops:[{op:"insert",count:2},
  {op:"update",count:0},{op:"delete",count:0}], window_ms:0}`;
  calls `emit_notify_async`.
- Audit `db.batch.committed` (rate-limited).
- Resolves `P_batch` with `[row_a.id, row_b.id]`.

**Step 7 (PG + listener thread, per the existing LISTEN/NOTIFY path).**
PG NOTIFY fans the envelope to every node. Listener picks it up;
broker (0.5.2) fans it to subscribed WS clients; kernel-side
JS subscribers receive via `bc.invoke_callback`.

**Step 8 (user-facing).** The JS caller's `await db.batch(...)`
resolves with `[row_a.id, row_b.id]`.

**Timeline (steady state, localhost PG):**
- Step 1: 0–0.1 ms (JS sync).
- Step 2: 0.5–1 ms (BEGIN + SET LOCAL round trips, pipelined to
  ~1 round trip).
- Step 3: 0.1–0.2 ms (promise resolve).
- Step 4: 1–3 ms per INSERT ✕ 2 = 2–6 ms (on the pinned conn).
- Step 5: 0.1 ms (JS sync enqueue).
- Step 6: 0.5–1 ms (COMMIT + coalescer flush).
- Step 7: subscriber receipt bounded by LISTEN/NOTIFY + fan-out =
  5–20 ms (0.5.0 + 0.5.2 contract).
- Total batch commit → JS resolve: ~5 ms p50; ~25 ms p99.
- Subscriber-side frame: ~25 ms p50; ~50 ms p99.

---

## Appendix B — Config Example

Full `db` block alongside 0.5.0 / 0.5.1 / 0.5.2 realtime config:

```json
{
  "db": {
    "batch": {
      "max_ops_per_batch": 500,
      "max_concurrent_batches_per_bc": 4,
      "timeout_ms": 30000,
      "audit_window_ms": 60000
    },
    "search_path": {
      "enforce": true
    },
    "oid_mapping": {
      "enabled": true
    },
    "silent": {
      "audit_window_ms": 60000
    }
  },
  "realtime": {
    "listener": { "enabled": true, "reconnect_backoff_ms": [100, 500, 2000, 5000] },
    "coalescer": { "window_ms": 50, "max_inflight_windows": 1024 },
    "broker": { "enabled": true, "max_subscriptions_per_conn": 64, "rbac_enforce": true }
  }
}
```

Minimum-effective config (all defaults, omit everything):

```json
{}
```

0.5.3 does not introduce any config key that MUST be set by the
operator. Defaults are safe for production.

---

## Appendix C — OID Mapping Table

Full OID-driven mapping the 0.5.3 implementation ships with. See
§OID-Driven PG-Type → JS-Type Mapping for rationale.

| PG type name | OID | Mapping strategy | JS result |
|--------------|-----|------------------|-----------|
| `BOOL` | 16 | `Field::as<bool>()` | `boolean` |
| `BYTEA` | 17 | `Field::as<std::vector<uint8_t>>()` | `Uint8Array` |
| `CHAR` | 18 | `Field::as<std::string>()` | `string` |
| `NAME` | 19 | `Field::as<std::string>()` | `string` |
| `INT8` | 20 | `Field::as<int64_t>()` + JS safe-range check | `number` or `string` |
| `INT2` | 21 | `Field::as<int16_t>()` | `number` |
| `INT4` | 23 | `Field::as<int32_t>()` | `number` |
| `TEXT` | 25 | `Field::as<std::string>()` | `string` |
| `JSON` | 114 | text → `JSON.parse` | structural |
| `FLOAT4` | 700 | `Field::as<float>()` | `number` |
| `FLOAT8` | 701 | `Field::as<double>()` | `number` |
| `VARCHAR` | 1043 | `Field::as<std::string>()` | `string` |
| `DATE` | 1082 | `Field::as<std::string>()` (YYYY-MM-DD) | `string` |
| `TIME` | 1083 | `Field::as<std::string>()` (HH:MM:SS) | `string` |
| `TIMESTAMP` | 1114 | `Field::as<std::string>()` (ISO 8601 naïve) | `string` |
| `TIMESTAMPTZ` | 1184 | `Field::as<std::string>()` (ISO 8601 UTC) | `string` |
| `NUMERIC` | 1700 | `Field::as<std::string>()` → `std::strtod` | `number` (lossy) |
| `UUID` | 2950 | `Field::as<std::string>()` | `string` |
| `JSONB` | 3802 | text → `JSON.parse` | structural |
| SQL `NULL` | — | `field.isNull()` short-circuit | `null` |
| Array types (any `*_ARRAY` OID) | 1000s | fall-through | `string` (deferred to future milestone) |
| Anything else | — | `Field::as<std::string>()` + rate-limited log warn | `string` |

Integer safe-range for `INT8`: `[-9007199254740991, 9007199254740991]`
(`Number.MIN_SAFE_INTEGER` / `Number.MAX_SAFE_INTEGER`). Outside this
range, the value is returned as a `string` to avoid silent precision
loss.
