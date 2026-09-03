# Architecture 04 — Kernel Services, Sidecars, HA, Security

**Owner:** this document. Authoritative for the secondary kernel
services (audit logging, scheduled tasks, notifications, metrics), the
sidecar HTTP contract, the high-availability model, and the overall
security stance.

**Depends on:**
- `architecture/01-identity.md §2` (all RBAC-gated decisions).
- `architecture/02-capabilities.md` (Tier 3 resolution dispatches to
  sidecars; audit and metrics participate in the capability path).
- `architecture/03-data.md §3` (realtime event bus carries kernel
  events; HA uses `LISTEN/NOTIFY`).
- `DESIGN-logging-subsystem.md` (spdlog is the authority on logging
  and audit — this document is the contract on top of that).

**Related:**
- ICD-0.1.7 (audit log).

---

## 1. Audit Logging

Every significant action is logged to `plinth.audit_log`:

- **Who** — user id, session id.
- **What** — action, target, parameters.
- **When** — timestamp, UTC.
- **Where** — node id (in HA).
- **Result** — success/failure, error if any.

All audit calls route through the spdlog subsystem defined in
`DESIGN-logging-subsystem.md`. Extensions emit audit events through
`audit.log(action, detail)`. All RBAC denials are audited. Permission
grants are not (to avoid noise).

See `ICD-0.1.7-audit.md` for the full contract.

**Realtime audit families (0.5.x arc).** The 0.5.x Realtime arc introduced
seven `realtime.*` audit families, all rate-limited via per-family
`audit_window_ms` knobs (defaults vary; common 60000 ms). Index for
fresh-session navigation:

- `realtime.listener.*` — `started`, `reconnected`, `notify.rejected`.
  Owner: [ICD-0.5.0 §Audit Events](../icd/ICD-0.5.0-pg-listen-notify-bridge.md).
- `realtime.coalescer.*` — `flush_failed` (reasons:
  `payload_too_large` / `pg_error` / `validate_failed`). Owner:
  [ICD-0.5.1 §Audit Events](../icd/ICD-0.5.1-pg-auto-event-coalescer.md).
  The originally-specified `shutdown_drain` audit was dropped at v0.5.1
  ship per a deterministic-teardown constraint (see ICD-0.5.1
  Implementation deviation).
- `realtime.broker.*` — `subscribe_denied`, `extension_drained`,
  `dispatch_skipped`. Owner:
  [ICD-0.5.2 §Audit Events](../icd/ICD-0.5.2-ws-broker.md).
- `realtime.events.*` — `write_failed` (reasons: `queue_full` /
  `pg_error` / `cleanup_failed` / `shutdown_timeout` /
  `cursor_read_failed`), `replay_started`, `replay_completed`,
  `replay_truncated`, `resync_required`, `cleanup_swept`. Owner:
  [ICD-0.5.4 §Audit Events](../icd/ICD-0.5.4-events-table-delta-sync.md).
- `realtime.debounce.*` — `advisory_overridden` (rate-limited audit
  fired on inbound `debounce_renegotiate` frames; advisory-only,
  not enforced). Owner:
  [ICD-0.5.5 §7](../icd/ICD-0.5.5-sequence-numbers-client-debounce.md).
- `realtime.seq.*` — `gap_detected` and `replay_seq_mismatch` reserved
  per [ICD-0.5.5 §11](../icd/ICD-0.5.5-sequence-numbers-client-debounce.md);
  population deferred to 0.5.5.1+ (S.06 / S.07 follow-ups; see DEFERRED.md).
- `realtime.notify.*` — `rejected`. Owner: ICD-0.5.0 (reused from the
  listener subsystem at the emit-helper layer).

The `db.*` audit families (`db.batch.committed`, `db.batch.rolled_back`,
`db.silent.used`, `db.search_path.set_failed`,
`db.batch.cross_extension_rejected`) introduced by v0.5.3 / 0.5.3.1 are
not realtime audits but related to the `db.*` substrate the realtime
arc builds on; full list in
[ICD-0.5.3 §Audit Events](../icd/ICD-0.5.3-db-batch-silent-mode.md).

---

## 2. Scheduled Tasks

- Kernel provides a cron-like scheduler.
- Extensions register tasks with schedule expressions.
- **HA.** PG advisory locks per task — any node can grab, first wins.
- **Default kernel tasks (non-optional):**
  - `plinth.heartbeat_sweep` — remove stale nodes.
  - `plinth.session_cleanup` — expire old sessions.
  - `plinth.events_cleanup` — trim `plinth.events` to the retention
    window.

---

## 3. Metrics

**Lightweight: in-memory counters + Prometheus exposition endpoint. No
PG storage. No partitioning. No cleanup tasks.**

The kernel does NOT store metrics in PostgreSQL. Prometheus / Grafana
already exist; self-hosters who want metrics already run them.

### 3.1 What the Kernel Provides

**In-memory counters** (atomic increments, lock-free histograms):

- Request count and latency histogram (per-endpoint).
- Active WebSocket connections.
- PG connection pool stats (active, idle, total).
- Capability resolution latency per tier (1/2/3).
- Thread pool utilization.
- Node uptime, CPU, memory (RSS, heap, allocator stats).
- Extension failure counts (from `architecture/05-extensions.md §2.3`
  supervision).

**`GET /metrics` endpoint** in Prometheus exposition format:

```
# HELP plinth_requests_total Total HTTP requests
# TYPE plinth_requests_total counter
plinth_requests_total{method="GET",path="/api/v1/users",status="200"} 1247

# HELP plinth_capability_resolution_seconds Capability call latency by tier
# TYPE plinth_capability_resolution_seconds histogram
plinth_capability_resolution_seconds_bucket{tier="1",le="0.001"} 9800
plinth_capability_resolution_seconds_bucket{tier="2",le="0.001"} 4200
plinth_capability_resolution_seconds_bucket{tier="3",le="0.1"} 350
```

### 3.2 Extension Metrics

Extensions register custom metrics via the `metrics.*` API:

```javascript
metrics.counter("my_extension_requests_total").inc();
metrics.gauge("my_extension_queue_depth", queueLength);
metrics.histogram("my_extension_processing_seconds").observe(elapsed);
```

These appear on the `/metrics` endpoint under the extension's
namespace.

### 3.3 What the Kernel Does NOT Provide

- No `plinth.metrics` table. No PG storage for metrics.
- No built-in metrics dashboard (card → modal → charts).
- A metrics dashboard extension can be built that scrapes `/metrics`
  or queries a Prometheus instance.

---

## 4. Notifications

- Kernel provides a notification bus.
- Extensions emit notifications; kernel routes to subscribers.
- In-app notifications (via realtime WebSocket push, see
  `architecture/03-data.md §3`).
- Extension point for external delivery (email, webhook — as
  extensions).

---

## 5. Sidecar Contract

Sidecars are external processes that connect inward to the kernel.

### 5.1 4-Endpoint HTTP/JSON Contract

| Endpoint | Purpose |
|----------|---------|
| `POST /health` | Health check, returns status + metrics |
| `POST /capabilities` | Declares what the sidecar provides |
| `POST /execute` | Kernel dispatches a capability call |
| `POST /shutdown` | Graceful shutdown signal |

### 5.2 Connection Model

- Sidecar starts, calls kernel's registration endpoint with bootstrap
  token.
- Kernel validates token (single-use, time-limited), authorizes
  sidecar.
- Sidecar is inert until admin approves (or auto-approve configured).
- Sidecar registers capabilities into the same capability registry
  (`architecture/02-capabilities.md §1`).
- Kernel routes capability calls to sidecar when appropriate (Tier 3).

### 5.3 Scopes

- **Instance sidecar.** Available to all users.
- **User/personal sidecar.** Private to one user.

### 5.4 K8s Auto-Join (post-v1)

See `architecture/01-identity.md §1`.

---

## 6. High Availability

### 6.1 Architecture

- N kernel nodes, all equal (leaderless by default).
- PostgreSQL as coordination layer.
- `plinth.node_registry` table (UNLOGGED for performance).
- Heartbeat: each node writes timestamp every N seconds.
- Stale detection: sweep removes nodes that missed M heartbeats.
- Self-eviction on graceful shutdown.

### 6.2 State Sharing

| State | Mechanism |
|-------|-----------|
| User sessions | PG `plinth.sessions` (any node can validate) |
| Capability registry | PG `plinth.capabilities` + per-node cache, `LISTEN/NOTIFY` invalidation |
| WebSocket subscriptions | PG pub/sub bridge (`LISTEN/NOTIFY`) |
| Scheduled tasks | PG advisory locks (first-grab) |
| Sidecar connections | PG `plinth.sidecar_registry` (which node holds which sidecar) |
| Metrics | In-memory counters, exposed via `/metrics` endpoint per-node |
| Audit log | PG `plinth.audit_log` (each node writes own, queryable from any) |
| Realtime events | PG `plinth.events` (delta sync on reconnect) |

### 6.3 No Leader Election

No single leader. Any node can handle any request. Contention is
resolved at the PG level (advisory locks for scheduled tasks,
SERIALIZABLE for conflict-sensitive operations).

---

## 7. Security Model

### 7.1 Principles

- Extensions run in sandboxed QuickJS runtimes — no escape by default.
- Extension database access isolated by PG schema
  (`architecture/03-data.md §1.2`).
- Sidecars connect inward — nothing can just attach.
- Bootstrap tokens are single-use, time-limited.
- All capability calls pass through RBAC
  (`architecture/02-capabilities.md §1`).
- Audit everything.
- Admin capability allowlist — admin decides what capabilities exist.

### 7.2 Extension Sandboxing

QuickJS provides:

- No filesystem access (kernel-mediated `storage.*` only).
- No network access (kernel-mediated `http.*` only, RBAC-gated).
- No process spawning.
- Memory limits (runtime-enforced, configurable).
- CPU time limits (runtime-enforced, configurable).
- `eval()` disabled by default.

PG schema isolation provides:

- Extensions can only query their own tables.
- Cross-schema access rejected by kernel query filter.
- PG `GRANT` controls what kernel tables extensions can read.

### 7.3 Sidecar Security

- Bootstrap token required for registration (or K8s JWT, post-v1).
- Admin approval required (or auto-approve flag).
- Sidecar declares capabilities, admin reviews before enabling.
- mTLS optional for sidecar connections.
- Sidecar cannot access other sidecars' capabilities directly.

### 7.4 HTTP Surface Security

- Content Security Policy on all served HTML (see
  `architecture/06-frontend.md §3`).
- Reserved URL prefixes are immutable post-1.0
  (`architecture/05-extensions.md §2`).
- Storage HTTP surface is always RBAC-gated, never anonymous
  (`architecture/03-data.md §2.3`).
- Public HTTP surface options (share primitive, site-host extension)
  are deferred and, if built, use explicit anonymous identity
  (`architecture/01-identity.md §3`) routed through the same RBAC
  path — no bypass.
