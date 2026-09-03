# ICD-0.1.6-websocket

**Traces to:** architecture/03-data.md §3 (Realtime), architecture/01-identity.md §2 (Groups and RBAC), DESIGN-rbac-philosophy.md  
**Depends on:** ICD-0.1.2-auth-sessions, ICD-0.1.3-pats, ICD-0.1.4-groups-rbac, ICD-0.1.5-rbac-enforcement (shared auth middleware, user context, rule checking)  
**Milestone:** 0.1.6 — WebSocket: connection lifecycle, subscribe/publish, delta sync  
**Status:** Ready for implementation (post-review v2)  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Related:** DESIGN-logging-subsystem.md, DESIGN-rbac-philosophy.md

---

## Overview

This ICD defines the WebSocket **connection lifecycle** for realtime communication between clients and the Plinth kernel. WebSocket is a first-class kernel primitive.

The endpoint `/ws/events` is responsible for:
1. Connection authentication and session binding.
2. Heartbeat and connection health.
3. Basic subscribe/unsubscribe and event delivery.

**Scoping note:** This ICD covers connection management only. The debounced coalescer, sequence numbers, delta sync on reconnect, and the `plinth.events` table are defined in milestone 0.5.x (Realtime). The four realtime layers and smart client-side re-query are described in architecture/03-data.md §3 and will be specified in 0.5.x ICDs.

Channel subscriptions are subject to RBAC checks per DESIGN-rbac-philosophy.md (additive union of rules across groups). The `kernel.admin` rule grants access to all channels.

All audit events must be emitted exclusively via the canonical path in DESIGN-logging-subsystem.md (`log::audit()` in C++).

---

## Standardized Error Shape

All error responses in this ICD use:

```json
{
  "error": "error_code_snake_case",
  "message": "Human-readable description (optional in production builds)"
}
```

---

## Connection Lifecycle

**Endpoint:** `/ws/events`

### Authentication

Immediately after the WebSocket opens, the client **must** send an authentication message within 5 seconds:

```json
{
  "type": "auth",
  "token": "<raw_session_token_or_pat>"
}
```

The server validates the token using the shared authentication middleware (ICD-0.1.2/0.1.3). On success it sends a `connected` message. On failure it sends an error and closes the connection.

**Connected Message**
```json
{
  "type": "connected",
  "user": { "id": "...", "username": "alice" },
  "session_id": "...",
  "node_id": "node-1"
}
```

**Error Codes**
- `auth_failed`
- `auth_timeout`
- `already_connected` (same session may not have multiple concurrent WS connections)

**Close Codes**
- 4001: auth_timeout
- 4002: auth_failed
- 4003: already_connected
- 1000: normal closure

### Heartbeat

The server sends a ping every 30 seconds:

```json
{ "type": "ping", "timestamp": 1713096000000 }
```

The client must respond with a pong using the same timestamp. Failure to respond within 10 seconds results in closure with code 4004 (“heartbeat_timeout”).

---

## Subscribe / Unsubscribe

**Subscribe**
```json
{
  "type": "subscribe",
  "channels": ["db:ext_notes:notes", "kernel:audit", "ext_chat:typing"]
}
```

**Response**
```json
{
  "type": "subscribed",
  "channels": ["db:ext_notes:notes", "kernel:audit"],
  "recommended_debounce_ms": 100,
  "recommended_jitter_ms": 50
}
```

Channels the user lacks permission for (per current RBAC rules) are silently omitted from the response.

**`recommended_debounce_ms` / `recommended_jitter_ms`** *(added in
v0.5.5 per `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md`
§7)*: advisory cadence the kernel expects clients to follow when
debouncing re-queries. Default 100 ms / 50 ms; operator-tunable via
`realtime.events.debounce.recommend_ms` and `…jitter_max_ms`. Both
fields are advisory only — the kernel does not enforce. Clients
SHOULD cache the values for the subscription lifetime and SHOULD
NOT republish on subsequent envelopes.

**Unsubscribe** uses the same format with type `"unsubscribe"` (no
advisory fields).

**Debounce renegotiate** *(added in v0.5.5)*: clients MAY send a
`debounce_renegotiate` frame to advertise an override:
```json
{
  "type": "debounce_renegotiate",
  "channel": "db:ext_notes:notes",
  "debounce_ms": 250
}
```
The kernel logs the override via the
`realtime.debounce.advisory_overridden` audit (rate-limited per
`(user_id, channel)` over `realtime.events.audit_window_ms`) and
ignores the frame — debounce is advisory + audit, not enforced
(ICD-0.5.5 §7 OQ5).

**Channel Naming**
- `db:<schema>:<table>` — Layer 1 debounced DB events
- `kernel:<event_type>` — Layer 2 kernel events
- `ext_<namespace>:<event>` — Layer 3 extension events

---

## Event Delivery

All events use the wrapper:

```json
{
  "type": "event",
  "channel": "...",
  "payload": { ... }
}
```

In 0.1.6, events are delivered as they arrive via PG LISTEN/NOTIFY. No sequence numbers, no coalescing, no debouncing. These are added in 0.5.x.

**Channel Naming**
- `db:<schema>:<table>` — Layer 1 DB events (raw NOTIFY payloads in 0.1.6; debounced summaries in 0.5.x)
- `kernel:<event_type>` — Layer 2 kernel events
- `ext_<namespace>:<event>` — Layer 3 extension events

The full four-layer model with debounced coalescing, sequence numbering, client-side debounce, smart re-query, and thundering-herd jitter is specified in ARCHITECTURE §3.6 and implemented in 0.5.x.

---

## Delta Sync on Reconnect (Deferred to 0.5.x)

Delta sync requires the `plinth.events` table (0.5.0) and the coalescer infrastructure (0.5.1). In 0.1.6, reconnecting clients must re-subscribe to channels and perform a full data re-query. The server does not track sequence numbers or missed events.

The full delta sync protocol (sequence numbers, `sync` messages, `resync_required`, retention window) will be specified in 0.5.x ICDs.

**Discharged 2026-04-25 (v0.5.4):** Delta sync shipped per
`docs/icd/ICD-0.5.4-events-table-delta-sync.md`. The 0.5.4
implementation extends the subscribe frame with optional `since_seq`,
adds outbound frame types `replay` / `replay_done` / `resync`, and
introduces the `plinth.events` persistence writer + per-user cursor
store. The 0.5.x sequence-number protocol will be revisited in 0.5.5.

---

## Security Constraints (Non-Negotiable)

1. Authentication is mandatory. No anonymous connections.
2. Channel subscriptions are filtered by the user’s effective permissions (additive union per DESIGN-rbac-philosophy.md). The `kernel.admin` rule grants access to all channels.
3. Single active WebSocket per session. New connection closes any existing one.
4. Heartbeat enforcement prevents zombie connections.
5. Event payloads are limited to 64KB. Larger events are rejected or truncated with audit logging.
6. Rate limiting: maximum 100 events per second per connection.

---

## What Must Not Be Decided Yet

- Binary WebSocket frames or file transfer over WS.
- Message multiplexing with correlation IDs or request/response patterns.
- Per-message compression (permessage-deflate).
- Client-side or server-side caching strategies beyond the sequence-based delta sync.
- Dynamic channel permission models or extension-defined access control beyond standard RBAC rules.
- Any change to the authentication flow, sequence number semantics, retention policy, or debounced coalescer behavior.
- Any deviation from the least-privilege additive permission model defined in DESIGN-rbac-philosophy.md.

All extensions to the realtime protocol must go through an architecture session and produce an updated ICD. The frontend SDK (`plinth.subscribe`, `plinth.useData`) is the canonical client contract and must not be altered in this milestone.

---

## Milestone Criteria

**Entry:** ICD-0.1.5-rbac-enforcement implemented and merged; shared auth middleware passes tests.

**Exit:**
- WebSocket connection lifecycle: open, authenticate (within 5s), connected response, close.
- Authentication via shared middleware (ICD-0.1.2/0.1.3 branching: session token or PAT with `plinth_` prefix detection).
- Heartbeat ping/pong with 30s interval and 10s timeout enforced.
- Subscribe/unsubscribe with RBAC channel filtering (silently omit unauthorized channels).
- Basic event delivery (passthrough from PG LISTEN/NOTIFY to subscribed clients).
- Single-connection-per-session enforced.
- All error/close codes as specified.
- Catch2 + integration tests covering: auth success, auth failure, auth timeout, heartbeat timeout, subscribe with RBAC filtering, event delivery, single-connection enforcement.
- Audit events emitted via DESIGN-logging-subsystem.md.
- Human approval of implementation plan obtained before merge.
- CI green; tests pass.

---

## Open Questions (Deferred)

- Support for binary frames or large payload streaming (post-1.0).
- Multiplexing and request/response correlation over a single WS connection.
- WebSocket compression strategy (evaluate during 0.10 security audit).

---

**This document is the permanent authority on the Plinth WebSocket realtime protocol.** Any code session implementing 0.1.6 or working on realtime, the frontend SDK, or extension event publishing **must** read this ICD, architecture/03-data.md §3, DESIGN-rbac-philosophy.md, and DESIGN-logging-subsystem.md before beginning work. Changes to this contract require a new architecture session.

---

## Implementation Notes (0.1.6)

Architect scoping decisions made during the 0.1.6 code session:

1. **PG LISTEN/NOTIFY: stub only.** Event delivery is implemented via an in-process `plinth::ws::publish(channel, payload)` hook rather than a real libpq LISTEN reader. The reader, coalescer, sequence numbers, and `plinth.events` table remain 0.5.0 work per §Event Delivery. The wire format and subscriber fan-out are in place today; a 0.5.0 reader can call the same `publish()` entry point once it exists.

2. **Channel RBAC: admin-only.** For 0.1.6, subscribe is gated on whether the authenticating user holds `kernel.admin`. Non-admin users receive an empty `subscribed[]` list (silent omission per the wording above). The per-channel rule naming convention (e.g. `db.<schema>.<table>.subscribe`) is deferred to a later ICD written alongside real channel producers.

3. **Single-WS-per-key extended to PATs.** The ICD text talks about "session"; the implementation enforces single-active per `(auth_type, session_id or pat_id)`. A second WS authenticated with the same PAT displaces the first with close code 4003, same as sessions.

4. **Application-level ping/pong only.** Drogon's protocol-level ping is disabled via `conn->disablePing()` at handshake; heartbeats are JSON text frames as specified, so the JS SDK will observe them.

---

## Implementation Notes (0.3.3.1)

Added by RE-EVAL following 0.3.3 (2026-04-18). 0.3.3.1 shipped a second-half fix for a WS teardown race that was not anticipated by the 0.1.6 contract. Documented here so future code sessions touching kernel-level Meyers singletons (or any singleton whose destruction ordering interacts with Drogon's `EventLoopThreadPool`) reuse the pattern.

5. **ConnectionRegistry shutdown gate.** The `ConnectionRegistry` Meyers singleton is destroyed in reverse-construction order at program exit. That order places it before Drogon's `EventLoopThreadPool` destructor joins its IO threads. A pending TCP close event processed during that window would call `handleConnectionClosed` → `unregister_connection` → `conns.find` on freed bucket memory. The fix is a file-scope `std::atomic<bool> g_shutdown_pending` in `src/kernel/ws/connection_registry.cpp`, flipped by a public static `ConnectionRegistry::initiate_shutdown()`. Every public method on the registry (`register_connection`, `unregister_connection`, `for_each`, `size`) checks the flag on entry (`memory_order_acquire`) and no-ops if set. The file-scope flag is zero-initialized before dynamic init and has trivial destruction, so its storage outlives the singleton. The test fixture's `atexit` handler calls `initiate_shutdown()` **before** `drogon::app().quit()`. Any future kernel singleton whose lifecycle overlaps Drogon's IO threads should adopt the same pattern.

5. **`log::audit()` pulled forward from 0.1.7.** The canonical audit primitive referenced in DESIGN-logging-subsystem.md is shipped as part of this milestone (the audit_log helpers in auth/, groups/, and rbac/ were already duplicated and needed consolidation). The audit query endpoint and retention task stay in 0.1.7.