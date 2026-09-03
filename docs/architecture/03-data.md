# Architecture 03 — Data: Database, Storage, Realtime

**Owner:** this document. Authoritative for the PostgreSQL commitment,
extension schema isolation, the storage abstraction, the kernel HTTP
surface for file upload/download, and the realtime event model.

**Depends on:**
- `architecture/01-identity.md §2` (RBAC gates cross-extension data
  access and every storage HTTP request).
- `architecture/02-capabilities.md §2` (`db.*`, `storage.*`, `pubsub.*`
  are part of the kernel standard library).

**Related:**
- `DESIGN-capability-registry.md` (registry uses PG; `plinth.capabilities`
  schema lives there).
- ICD-0.1.6 (WebSocket connection management; realtime protocol full
  specification lands in 0.5.x ICDs).

---

## 1. Database (PostgreSQL — Committed)

**PostgreSQL only. No abstraction layer. No hypothetical future backends.**

The kernel uses PG-specific features aggressively:

- `LISTEN/NOTIFY` for realtime event bus.
- Advisory locks for scheduled task coordination.
- UNLOGGED tables for node registry (performance).
- JSONB operators for flexible metadata.
- Table partitioning (via inheritance) for metrics time-series.
- Window functions and CTEs where appropriate.
- `ON CONFLICT` for upserts.
- PG schemas for extension isolation.

If someone wants SQLite later, that's a fork, not an implementation.

### 1.1 Schema Layout

Every component gets its own PG schema. This is real isolation, not
application-level name-prefix checking.

```sql
-- Kernel schema
CREATE SCHEMA plinth;

-- Kernel tables live in plinth.*
plinth.users
plinth.sessions
plinth.groups
plinth.group_members
plinth.group_rules
plinth.capabilities
plinth.packages
plinth.panels
plinth.migrations
plinth.audit_log
plinth.node_registry     -- UNLOGGED
plinth.notifications
plinth.scheduled_tasks
plinth.sidecar_registry
plinth.events            -- realtime event log for delta sync

-- Each extension gets its own schema
CREATE SCHEMA ext_terminal_core;
CREATE SCHEMA ext_notes;
CREATE SCHEMA ext_shell;
```

### 1.2 Extension Database Isolation

Each extension gets its own PG schema. When the kernel executes an
extension's database query, it sets `search_path` to the extension's
schema:

```sql
SET search_path TO ext_terminal_core, plinth;
```

This means:

- Extensions can only see their own tables by default.
- Cross-schema references require explicit `schema.table` syntax.
- The kernel **rejects** any extension query containing a schema
  qualifier that isn't the extension's own.
- PG's own permission system enforces isolation at the database level.
- Extensions CAN read from `plinth.*` tables that the kernel exposes
  (e.g., `plinth.users` for user lookup) — controlled by PG `GRANT`.

### 1.3 Extension Migration Tracking

The kernel tracks which migrations have been applied per extension:

```sql
CREATE TABLE plinth.migrations (
  extension_name TEXT NOT NULL,
  migration_file TEXT NOT NULL,
  applied_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  checksum       TEXT NOT NULL,
  PRIMARY KEY (extension_name, migration_file)
);
```

On extension install/upgrade:

1. Kernel reads `migrations/` directory, sorts files numerically.
2. Compares against `plinth.migrations` for this extension.
3. Runs unapplied migrations in order, each within a transaction.
4. If any migration fails, that transaction rolls back, install fails,
   admin sees the error with the failing SQL and PG error message.
5. Checksums detect if a previously-applied migration was modified
   (error — migrations are immutable once applied).

---

## 2. Storage (File/Blob)

**Separate from the database layer.** Storage is for files, blobs,
uploads, and large binary data.

Unlike the database layer, file storage DOES use an abstract interface.
Files are not PG-specific. S3, GCS, or other blob stores are plausible
future backends. The interface is thin:

```cpp
class Storage {
  virtual Result<void> put(string_view key, span<byte> data) = 0;
  virtual Result<vector<byte>> get(string_view key) = 0;
  virtual Result<void> remove(string_view key) = 0;
  virtual Result<vector<string>> list(string_view prefix) = 0;
};
```

### 2.1 Default Implementation: Local Filesystem

- Configurable root path (default: `/data/storage/`).
- Extension-scoped prefixes: `{root}/{extension_name}/`.
- Kernel-scoped directories: `{root}/_plinth/uploads/`,
  `{root}/_plinth/assets/`.

### 2.2 Extension Storage API (QuickJS)

```javascript
await storage.put("reports/q1.pdf", buffer);
const data = await storage.get("reports/q1.pdf");
await storage.delete("reports/q1.pdf");
const files = await storage.list("reports/");
```

Extensions can only access their own prefix by default. Cross-extension
storage access is RBAC-gated (see `architecture/02-capabilities.md §2.2`).

### 2.3 File Upload / Download HTTP Surface

The `Storage` abstraction and the QuickJS `storage.*` API cover
extension-code-to-kernel access. Streaming binary data between the
frontend (or any HTTP client) and the kernel needs an HTTP contract —
JSON-based capability dispatch cannot carry a 500MB upload.

The kernel provides a dedicated HTTP surface, reserved under the `/api/*`
prefix (see `architecture/05-extensions.md §2`).

**Endpoints:**

| Method | Path | Purpose |
|--------|------|---------|
| `POST` | `/api/storage/{extension}/{path...}` | Upload (multipart / chunked / resumable) |
| `GET` | `/api/storage/{extension}/{path...}` | Download (streaming, Range-supported) |
| `DELETE` | `/api/storage/{extension}/{path...}` | Delete |
| `GET` | `/api/storage/{extension}/?prefix=...` | List (JSON response) |

**Authentication.** Session cookie or PAT, as with any other
authenticated kernel endpoint. Anonymous identity
(`architecture/01-identity.md §3`) is denied by default and stays that
way unless the user is explicitly granted storage rules.

**RBAC (default-deny, read-your-own-free-at-the-API-level).** The
request is RBAC-gated on a per-extension basis:

- **Calling user operates on their own extension's storage.** The
  kernel dispatches as though the call originated from within that
  extension's runtime: no additional rule required beyond the base
  authenticated-user baseline — matches the §2.2 "read-your-own is
  free" pattern.
- **Cross-extension access.** A user editing their notes wants a
  photos-extension image rendered. The calling context is the active
  extension (e.g. Notes); accessing `/api/storage/photos/...` requires
  a per-extension rule the photos extension declared (e.g.
  `photos.storage.read`) and that the user's groups have been granted.
- **Unauthenticated access.** Never permitted on this surface. Public
  sharing has a separate architectural mechanism
  (`architecture/05-extensions.md §Deferred`) that does not reuse
  `/api/storage`.

The enforcement point is `RbacFilter` (ICD-0.1.5); the rule derivation
is identical to capability dispatch. The surface does not introduce a
new RBAC path.

**Upload semantics.**

- **Small to medium (default ≤100MB, configurable).** Multipart
  form-data or chunked `Transfer-Encoding`. Single request.
- **Large (>100MB).** Resumable upload protocol (Tus or equivalent;
  exact wire protocol is an ICD decision at implementation time). The
  architectural commitment is: large-upload clients can resume after a
  connection drop without re-uploading completed chunks.
- **Quota enforcement at upload time.** The kernel checks extension
  quota before accepting the first byte. Exceeded quota → HTTP 413
  with the current usage and limit in the response body. Storage
  quota is kernel policy (§2.4 below).

**Download semantics.**

- **`Range` header support** for seekable media.
- **`Content-Disposition: attachment; filename=...`** unless the query
  string includes `?inline=1`.
- **`Content-Type`** derived from the MIME table used for extension
  asset serving (`architecture/06-frontend.md §3`) with stored metadata
  override when available.
- **ETag.** Derived from storage metadata (mtime + size, or content
  hash if the backend supports one cheaply).
- **`Cache-Control: private, max-age=0`** by default. Extensions may
  request longer caching via storage metadata — reserved, not
  implemented in v1.

**List semantics.**

- Returns JSON: `{ entries: [ { path, size, modified_at, etag }, ... ], truncated: bool, next_prefix?: string }`.
- Pagination is prefix-based for large directories (implementation
  detail; the architectural commitment is that `list()` is bounded).

### 2.4 Quota Model (architectural sketch)

Kernel policy, applied at upload time. Two tiers:

- **Per-extension quota.** Instance-level policy. Admin sets a default;
  per-extension overrides in the extension's `config.json` are clamped
  to the admin maximum.
- **Per-user quota (reserved).** Not implemented in v1. The shape is
  committed here so the v1 quota check doesn't preclude it: a request
  is accepted only if both extension-quota and user-quota checks pass.
  v1 hardcodes user-quota to unlimited.

Quota is calculated by summing `storage.put` sizes on success.
`storage.delete` releases quota **asynchronously** — the release is
eventually consistent; a storm of deletes followed by an immediate
large upload may see stale quota and 413. This is acceptable for v1;
administrators can trigger quota recalculation manually.

### 2.5 Scope for the Implementation ICD

This section establishes the **architectural contract**: endpoints,
headers, RBAC model, quota model. The exact ICD for this surface —
chunk-size negotiation, Tus protocol selection, pre-signed URLs,
progress-reporting channel, multipart field names, storage-metadata
schema — is written when the storage milestone begins (currently
scheduled in the 0.10.x arc). Code sessions implementing storage must
not exceed this architectural contract without an architecture session.

### 2.6 Rejected Alternatives

- **Stream through capability dispatch.** Rejected. The capability
  system is for typed JSON dispatch; shoehorning multi-gigabyte binary
  streams through it breaks the mental model and the implementation.
- **Extension-owned HTTP routes for storage.** Rejected. Matches the
  general rejection of extension-owned arbitrary HTTP routes
  (`architecture/05-extensions.md §Deferred`).

---

## 3. Realtime (WebSocket + Pub/Sub)

**Realtime is a first-class kernel primitive, not an opt-in feature.**

### 3.1 Architecture: Debounced Change Streams

The kernel database layer emits change events automatically. It does
NOT emit one PG NOTIFY per row. That would drown the system on batch
operations.

Instead, the storage layer emits **debounced change summaries:**

**Debounce mechanism:**

1. First write to table X starts a coalescing window (configurable,
   default 50ms).
2. Subsequent writes within the window are accumulated.
3. At window expiry, emit ONE PG NOTIFY:

   ```json
   {
     "layer": "data",
     "channel": "plinth:data:ext_notes.notes",
     "schema": "ext_notes",
     "table": "notes",
     "ops": [
       {"op": "insert", "count": 47},
       {"op": "update", "count": 3},
       {"op": "delete", "count": 0}
     ],
     "window_ms": 50
   }
   ```

   Notes on envelope fields: `layer` and `channel` are required
   (ICD-0.5.0 §Payload Envelope Contract). The `ops` array carries
   all three CRUD kinds always (`insert`/`update`/`delete`, even when
   count is zero — ICD-0.5.1 §OQ7). `ids` is intentionally absent in
   the v0.5.1 ship — 0.5.5 may reintroduce per-op ID arrays via
   `RETURNING id` wrapping (see ICD-0.5.1 §OQ4). `seq` is reserved
   for 0.5.5. `emitted_at` is reserved for 0.5.4 persistence.

4. For single writes (the common case), the window expires with one
   event — effectively immediate.

**PG NOTIFY payload limit: 8000 bytes.** If the accumulated change
summary exceeds this, the truncation heuristic drops `ids` (no-op in
v0.5.1 since IDs are already absent) and, if still oversize, drops
the envelope entirely and audits `realtime.coalescer.flush_failed`
with `reason="payload_too_large"`. Clients subscribed to a dropped
envelope's channel see no event and must re-query on their own
timer; the `truncated:true` counts-only fallback shape is reserved
for the future case where `ids` population lands (0.5.5) and
truncation trims them rather than dropping the envelope:

```json
{
  "layer": "data",
  "channel": "plinth:data:ext_notes.notes",
  "schema": "ext_notes",
  "table": "notes",
  "ops": [
    {"op": "insert", "count": 10000},
    {"op": "update", "count": 0},
    {"op": "delete", "count": 0}
  ],
  "window_ms": 50,
  "truncated": true
}
```

#### 3.1.1 Auto-Event Coalescer (subsystem)

Live since v0.5.1. The debounce mechanism above is implemented by
`plinth::realtime::CoalescerRegistry` — a process-lifetime singleton
owning a `(schema, table) → WindowState` map protected by
`std::shared_mutex`, with a dedicated `trantor::EventLoopThread`
driving the flush timers. The first write to a `(schema, table)`
tuple opens a **fixed-duration** window (default 50 ms, configurable
via `realtime.coalescer.window_ms`); subsequent writes within that
window accumulate counters without extending the timer. At
`opened_at + window_ms`, the flush path builds the envelope above
and calls `emit_notify_async` on drogon's DbClient pool.

**Lifecycle drain.** DISABLE / UPGRADING / UNINSTALL transitions
synchronously flush every open window owned by the affected
extension before proceeding, so final Layer-1 envelopes land while
the listener consumer chain is still intact. Process shutdown
flushes every open window via the atexit chain.

See `docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md` for the full
contract (classifier shape, per-extension identity snapshot on
`AsyncOp`, truncation heuristic, audit events, configuration surface,
HA semantics).

### 3.2 Batch Operations

Extensions can explicitly batch writes:

```javascript
await db.batch(async () => {
  for (const row of data) {
    await db.exec("INSERT INTO my_table ...", row);
  }
});
// ONE change event emitted here, not N
```

Extensions can also fully suppress events for internal bookkeeping:

```javascript
await db.exec("UPDATE internal_state SET ...", { silent: true });
// No change event emitted
```

**Layer 1 events are emitted by default.** Extensions suppress them
explicitly when they know it's appropriate (`batch()` or `silent`).

### 3.3 Four Realtime Layers

| Layer | Mechanism | Purpose |
|-------|-----------|---------|
| **1. DB events** | PG `LISTEN/NOTIFY`, auto-emitted (debounced) | CRUD reactivity — default behavior, zero code |
| **2. Kernel events** | PG `LISTEN/NOTIFY`, kernel-emitted | System events: user login, package install, node join/leave, `users.deleted` |
| **3. Extension events** | PG `LISTEN/NOTIFY`, extension-emitted | Custom events: `pubsub.publish("chat:typing", ...)` |
| **4. Sidecar events** | Sidecar → kernel HTTP → PG NOTIFY | Sidecar status, long-running task progress |

### 3.4 Frontend SDK — Debounced Smart Re-Query

The client SDK handles realtime events intelligently:

1. **Client-side debounce.** `useData()` has a built-in debounce
   (default 100ms). Multiple events within the window trigger ONE
   re-query, not N. _Server-side wire commitment implemented
   2026-04-26 (v0.5.5):_ `subscribed` ack carries a
   `recommended_debounce_ms` advisory (default 100; operator-tunable
   via `realtime.events.debounce.recommend_ms`). Normative contract
   pinned in [`ICD-0.5.5 §7`](../icd/ICD-0.5.5-sequence-numbers-client-debounce.md).
2. **Optimistic local updates.** If the event payload includes changed
   IDs and the operation is `insert` or `delete`, the SDK can update
   its local state without re-querying.
3. **Sequence numbers.** Every event includes a monotonic sequence
   number (`seq`). The SDK tracks the last-seen sequence.
   _Implemented 2026-04-26 (v0.5.5)_ via writer-first topology in
   [`ICD-0.5.5 §5`](../icd/ICD-0.5.5-sequence-numbers-client-debounce.md):
   `envelope["seq"] == plinth.events.seq` BIGSERIAL by construction
   on both live and replay paths; per-PG-instance strictly monotonic.
4. **Smart filtering.** If the event includes IDs and the SDK's current
   `where` clause is simple enough to evaluate locally, the SDK
   determines whether the change is relevant without hitting the
   server.
5. **Thundering herd mitigation.** When the server broadcasts a change
   event, each client adds random jitter (0–50ms) to its debounce
   window before re-querying. _Server-side wire commitment implemented
   2026-04-26 (v0.5.5):_ `subscribed` ack carries a
   `recommended_jitter_ms` advisory (default 50; operator-tunable via
   `realtime.events.debounce.jitter_max_ms`). Normative contract
   pinned in [`ICD-0.5.5 §7`](../icd/ICD-0.5.5-sequence-numbers-client-debounce.md).

### 3.5 Delta Sync on Reconnect

When a WebSocket connection drops and reconnects:

1. Client sends its last-seen sequence number.
2. Server queries `plinth.events` for events since that sequence.
3. Server sends missed events as a batch.
4. Client processes them through the normal event pipeline.

`plinth.events` retention: configurable, default 1 hour. Events older
than the retention window are dropped. If the client's last-seen
sequence is older than the retention window, the server sends a "full
resync" signal and the client re-queries all subscribed data.

> Implemented 2026-04-25 (v0.5.4). Normative contract pinned in
> [ICD-0.5.4-events-table-delta-sync.md](../icd/ICD-0.5.4-events-table-delta-sync.md).
> The wire-format extension to the `subscribe` frame is the optional
> `since_seq` field; the server emits new frame types `replay`,
> `replay_done`, and `resync` (with `reason ∈ {cursor_expired,
> row_cap, mismatch, events_disabled, live_buffer_overflow}` —
> the fifth reason added 2026-04-26 (v0.5.5) per ICD-0.5.5 §8).
> Mid-replay live frames are buffered per-conn until `replay_done`
> flushes them in writer-first seq order; `replay_done` carries
> `buffered_live_count` (always present, value 0 when nothing
> buffered). The buffer is bounded by `live_buffer_cap_per_subscription`
> (default 256 frames per ICD-0.5.5 §8 OQ6); on overflow the broker
> flips a shared abort flag, the replay coroutine returns at the next
> chunk boundary, and the overflow site emits the resync inline.
> Conn-state subscription mutations and cross-thread reads (broker
> pre-pass for `delivered_to_users`) are serialized via
> `ConnState::channels_mu` since v0.5.5.1; future cross-thread access
> to `state->channels` must hold the mutex.

### 3.6 HA Realtime

PG `LISTEN/NOTIFY` is the backbone. All nodes listen on the same
channels. A write on Node A triggers a NOTIFY that Node B receives and
fans out to its connected WebSocket clients. No additional coordination
needed — PG is the event bus. See
`architecture/04-services-ha.md §5` for the HA model.

#### 3.6.1 Physical Channel Fan-In

Live since v0.5.0. Every realtime NOTIFY the platform emits rides
the **single physical PG channel `plinth:realtime`** — Layer 1/2/3
logical channel names (`plinth:data:<schema>.<table>`,
`plinth:system:<event_class>`, `plinth:ext:<extension>:<event_class>`)
live in the envelope's `channel` field, not as PG channel names.
Each node's `plinth::realtime::listener` (ICD-0.5.0 §Listener
Subsystem) opens one PG connection that `LISTEN "plinth:realtime"`s
and dispatches incoming envelopes to registered in-process
`EventHandler`s after channel regex validation. This fan-in design
avoids the per-logical-channel LISTEN proliferation that would
otherwise scale poorly under cross-extension subscription patterns.
Since v0.5.5, `plinth::realtime::events_writer` is the listener's sole
`EventHandler` consumer (writer-first topology; ICD-0.5.5 §5). After
persisting the envelope and stamping `seq` from `INSERT … RETURNING seq`,
the writer dispatches the envelope to `realtime::broker` for in-process
WS / JS fan-out and to `cursor_store::record_delivered` for per-user
cursor advance. The broker is no longer a peer listener handler — it is
downstream of the writer, so envelope-`seq` equals `plinth.events.seq`
by construction on both live and replay paths.

---

## Appendix A: Realtime Event Flow

```
Extension: db.exec("INSERT INTO notes ...")
    │
    ▼
Kernel DB Layer (within ext_notes schema)
    │
    ├── Execute SQL on PostgreSQL
    └── Coalescer: start/extend 50ms debounce window
              │
              ▼  (window expires)
         Emit ONE PG NOTIFY:
         channel: 'plinth:data:ext_notes.notes'
         payload: { table, schema, ops, seq }
              │
              ▼
         PG LISTEN/NOTIFY propagates to ALL kernel nodes
              │
              ▼
         Each node's WS Broker checks client subscriptions
              │
              ├── Subscribed client → jitter → debounce → re-query or optimistic update
              └── Unsubscribed → skip
```

---

## Appendix B: PG Schema Layout

```sql
CREATE SCHEMA plinth;

-- Identity (0.1.2, 0.1.3)
CREATE TABLE plinth.users ( ... );
CREATE TABLE plinth.sessions ( ... );
CREATE TABLE plinth.pats ( ... );

-- Groups & RBAC (0.1.4)
CREATE TABLE plinth.groups ( ... );
CREATE TABLE plinth.group_members ( ... );
CREATE TABLE plinth.rbac_rules ( ... );
CREATE TABLE plinth.group_rules ( ... );

-- Capability Registry (0.2.0)
CREATE TABLE plinth.capabilities ( ... );

-- Packages (0.4.0)
CREATE TABLE plinth.packages ( ... );
CREATE TABLE plinth.panels ( ... );          -- 0.4.4, see DESIGN-packages-v04x.md §4.3
CREATE TABLE plinth.migrations ( ... );

-- HA (0.9.0)
CREATE UNLOGGED TABLE plinth.node_registry ( ... );
CREATE TABLE plinth.sidecar_registry ( ... );

-- Realtime (0.5.0)
CREATE TABLE plinth.events (
  seq        BIGSERIAL PRIMARY KEY,
  channel    TEXT NOT NULL,
  payload    JSONB NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Audit (0.1.7)
CREATE TABLE plinth.audit_log ( ... );

-- Notifications (0.11 / 0.10.x)
CREATE TABLE plinth.notifications ( ... );

-- Scheduled Tasks (0.7)
CREATE TABLE plinth.scheduled_tasks ( ... );

-- Metrics: NOT stored in PG. In-memory + GET /metrics (Prometheus).

-- Extension schemas created at package install:
-- CREATE SCHEMA ext_terminal_core;
-- CREATE SCHEMA ext_notes;
-- CREATE SCHEMA ext_shell;
```
