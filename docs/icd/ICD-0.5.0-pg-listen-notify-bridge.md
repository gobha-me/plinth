# ICD-0.5.0-pg-listen-notify-bridge

**Traces to:** architecture/03-data.md §3 (Realtime — WebSocket +
Pub/Sub) lines 271–421, authoritative design prose for the debounced
change streams, four-layer event model, HA realtime semantics, and
Appendix A event flow; architecture/03-data.md §3.1 (debounce
mechanism + 8000-byte payload ceiling, lines 275–318);
architecture/03-data.md §3.3 (four realtime layers table, lines
343–350); architecture/03-data.md §3.6 (HA realtime — N nodes LISTEN
the same channels, lines 386–392); architecture/04-services-ha.md §6.2
(State Sharing table, line 174 — "WebSocket subscriptions | PG pub/sub
bridge (`LISTEN/NOTIFY`)"); architecture/04-services-ha.md §6.3 (no
leader election — contention resolved at PG); DESIGN-capability-registry.md
§Tier 2 Cache Invalidation (the 0.2.3 precedent this ICD generalizes).
**Depends on:** ICD-0.2.2-capability-resolution (Tier 2 cache + the
`plinth_capability_changed` LISTEN/NOTIFY pattern the new subsystem
runs alongside, not replaces); ICD-0.2.4-capability-rbac (reconnect
resync amendment — full cache reload after every successful LISTEN
open, ≤ 1 s + one SELECT divergence ceiling); ICD-0.1.6-websocket
(in-process `plinth::ws::publish(channel, payload)` hook — the 0.5.2
broker will wire listener output into this function, not this ICD);
ICD-0.1.7-audit (async audit writer + `g_audit_ready` gate — listener
startup and NOTIFY rejections route through it); ICD-0.3.1-runtime-lifecycle
(`BridgeContext` stdlib injection seam the `pubsub.publish` binding
extends); ICD-0.3.2-kernel-stdlib-sync (`inject_kernel_stdlib`
convention, `JS_SetContextOpaque` BridgeContext lookup, per-namespace
files under `src/kernel/js/stdlib/` — `pubsub_bindings.cpp` follows
the pattern `log_bindings.cpp` / `config_bindings.cpp` /
`crypto_bindings.cpp` set); ICD-0.3.3-async-bridge (`run_on_context`
coroutine dispatch, `pending_ops` / `register_pending`,
`PromiseRejection` shape, `dispatch_async_op_detached` fan-in —
`PUBSUB_PUBLISH` reserves an enum variant in 0.3.3 DESIGN-quickjs-bridge.md
§10 which 0.5.0 wires up); ICD-0.4.4-package-install-lifecycle
(per-extension `BridgeContext` extension identity — the
`pubsub.publish` channel-name check reads this; atexit chain ordering
the new `realtime::` subsystem splices into).
**Milestone:** 0.5.0 — PG LISTEN/NOTIFY bridge in kernel. First 0.5.x
code milestone; opens the Realtime arc. Paired ICD authoring slot
`0.4.7.2` precedes this code work per METHODOLOGY §3.1 forward ICD
presence rule (see `RE-EVAL-0.4.x-arc-closeout.md §2.6 + §6`).
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** [src/kernel/capabilities/listener.hpp](../../src/kernel/capabilities/listener.hpp)
(the 0.2.3 jthread+eventfd+reconnect listener whose pattern 0.5.0
generalizes — the new subsystem runs as a SIBLING, not an extension);
[src/kernel/capabilities/listener.cpp:226–313](../../src/kernel/capabilities/listener.cpp)
(`run_listener` thread body, reconnect-with-resync loop, `PQsocket` +
eventfd `poll()` — pattern reused verbatim);
[src/kernel/capabilities/registration.cpp:67–95](../../src/kernel/capabilities/registration.cpp)
(`send_notify` helper — 0.5.0 generalizes this into
`realtime::emit_notify(conn, envelope)` with validation + error
taxonomy); [src/kernel/ws/publish.hpp](../../src/kernel/ws/publish.hpp)
(`plinth::ws::publish(channel, payload)` — in-process fan-out used by
0.1.6 `/ws` publishes today; the 0.5.2 broker will compose 0.5.0
listener output with this function, not this ICD — the hook exists
already); [src/kernel/js/stdlib/log_bindings.cpp](../../src/kernel/js/stdlib/log_bindings.cpp)
(per-namespace JS binding layout `pubsub_bindings.cpp` follows);
[src/kernel/js/stdlib_inject.cpp](../../src/kernel/js/stdlib_inject.cpp)
(`inject_kernel_stdlib` — new `pubsub.*` namespace registration site);
[src/kernel/main.cpp:243–258](../../src/kernel/main.cpp) (atexit chain
— new `realtime::stop_listener()` inserts between
`capabilities::stop_notify_listener()` and
`ws::shutdown_js_stress_pool()`); [src/kernel/main.cpp:286–294](../../src/kernel/main.cpp)
(startup order — new `realtime::start_listener(cfg.db)` replaces the
`// TODO: realtime broker init` line after
`capabilities::start_notify_listener` and before route registration);
[migrations/schema.sql:190](../../migrations/schema.sql) (the reserved
`-- ── Realtime ─────` section marker awaits 0.5.0; this ICD adds
zero tables — `plinth.events` persistence is 0.5.4 scope);
[docs/architecture/03-data.md §3 + Appendix A + Appendix B](../architecture/03-data.md)
(DESIGN prose this ICD promotes to normative contract — the 0.4.x
RE-EVAL flagged that §3 is "DESIGN-style, not pinned into an ICD" and
this ICD is the pin).

---

## Overview

0.5.0 lands the kernel-side PostgreSQL `LISTEN/NOTIFY` bridge: the
per-node subscriber that receives every realtime event the platform
emits, the NOTIFY emission helper every kernel and extension caller
uses to publish, and the JS-side `pubsub.publish` binding that makes
Layer 3 extension events a contracted capability. It is the backbone
of the Realtime arc — 0.5.1 (db-auto-emission coalescer), 0.5.2 (WS
broker fan-out to subscribed clients), 0.5.4 (`plinth.events`
persistence + delta sync), and 0.5.5 (monotonic sequence numbers)
each compose on top of the primitives this ICD pins.

The bridge generalizes the pattern already operating in 0.2.3 for the
capability cache invalidation channel (`plinth_capability_changed`):
a per-node `std::jthread` owning a dedicated sync-libpq PGconn,
`poll()`ing the `PQsocket` + an eventfd wake descriptor, reconnecting
with 1 s backoff on `CONNECTION_BAD`, and reloading authoritative
state after every successful LISTEN open. The 0.2.3 subsystem stays
as-is — it is frozen to the capability channel. 0.5.0 adds a new
sibling under `plinth::realtime::` that subscribes to the broader
realtime channel set (`plinth:data:*`, `plinth:system:*`,
`plinth:ext:*`) and dispatches to in-process consumers. The 0.5.2
broker is the first such consumer; 0.5.4's `plinth.events` writer is
the second.

**Layer 3 extension publishing is in scope for this milestone.** The
`pubsub.publish(channel, payload) → Promise<void>` binding lands
alongside the kernel-side emission helper; extension code gets a
working publish surface on 0.5.0 merge. `pubsub.subscribe` stays
deferred to 0.5.2 (the broker is required for client-reachable
fan-out; an in-kernel subscribe API with no fan-out is not useful).

**Scope:**

- Per-node realtime LISTEN subscriber (`plinth::realtime::start_listener`
  / `stop_listener`) — jthread+eventfd+reconnect-with-resync, matching
  the 0.2.3 lifecycle contract.
- Channel naming scheme pinned across all three layers, with
  validating regex.
- Payload envelope JSON contract — required/optional fields, reserved
  `seq` / `truncated` slots 0.5.1 and 0.5.5 will populate.
- NOTIFY emission helper (`plinth::realtime::emit_notify`) —
  size-validates, channel-validates, emits `SELECT pg_notify($1, $2)`
  within the caller's transaction. Two overloads: `PGconn*` sync and a
  `drogon::Task<>` async wrapper over the DbClient.
- `pubsub.*` JS stdlib namespace with `publish(channel, payload)`;
  extension-identity-gated channel name check; rejection taxonomy.
- Config surface (`realtime.listener.*` + `realtime.notify.*`).
- Audit events for listener startup + reconnects + NOTIFY rejections.
- Deterministic-teardown contract per `feedback_deterministic_teardown.md`
  — `stop_listener()` barriers on thread join before
  `drogon::app().quit()` runs.
- HA semantics normative promotion of `architecture/03-data.md §3.6`.
- Ten R.##-prefixed listener-lifecycle tests + six E.##-prefixed
  emitter tests + five P.##-prefixed `pubsub.publish` JS binding
  tests.

**Out of scope (deferred, with ICD pointers):**

- **Debouncer / coalescer.** The 50 ms window + per-table
  accumulation + 8000-byte payload truncation heuristic per
  `architecture/03-data.md §3.1` is 0.5.1 scope. 0.5.0 enforces the
  size ceiling (reject emit over 8000 bytes) but does not coalesce
  and does not implement the overflow-truncation-to-counts path.
- **WebSocket broker fan-out.** Per-client subscription table,
  per-connection routing, subscription RBAC — all 0.5.2. 0.5.0's
  listener dispatches to a registered in-process handler vector; the
  0.5.2 broker registers itself at startup. 0.5.0 ships a
  no-op-default handler (logs at debug) so the subsystem is
  self-contained.
- **`plinth.events` persistence + delta sync on reconnect.** 0.5.4.
  The envelope reserves `seq` + `truncated` fields; 0.5.0 populates
  neither.
- **Monotonic `seq` number generation.** 0.5.5. The reserved field is
  pinned in the envelope contract; the generator is not.
- **`db.batch()` + `silent: true` suppression.** 0.5.3. The kernel
  NOTIFY helper does not need the suppression knob — 0.5.1's
  coalescer intercepts DB writes before they reach the helper;
  `silent` never hits 0.5.0 code.
- **Client SDK debounce, smart re-query, jitter, optimistic updates.**
  `architecture/03-data.md §3.4`. 0.6.3 Shell SDK scope.
- **`pubsub.subscribe` JS binding.** 0.5.2 (requires broker for
  client-reachable fan-out). The enum variant `PUBSUB_SUBSCRIBE` is
  not introduced; only `PUBSUB_PUBLISH` (reserved in DESIGN-quickjs-bridge.md
  §10, wired in 0.5.0).
- **Sidecar-emitted events (Layer 4).** `architecture/03-data.md
  §3.3` row 4 points at 0.8.x sidecar tier. 0.5.0 pins Layers 1–3.
- **Per-channel RBAC for extension publishes.** §OQ1 below — 0.5.0
  uses pattern-match (channel prefix must match caller's extension
  name); fine-grained per-channel grants deferred.
- **Sidecar NOTIFY production.** Kernel does not proxy Layer 4 into
  `LISTEN/NOTIFY` at 0.5.0; 0.8.x decides.

---

## Channel Naming Scheme

Three-layer namespace, colon-separated. Verbatim promotion of
`architecture/03-data.md §3.3` + Appendix A pattern, with the
naming explicit (DESIGN showed `plinth:data:ext_notes.notes` as a
single example; this ICD pins the generator).

| Layer | Pattern | Example | Producer |
|-------|---------|---------|----------|
| 1. DB events | `plinth:data:<schema>.<table>` | `plinth:data:ext_notes.notes` | 0.5.1 coalescer (kernel emits post-write) |
| 2. Kernel events | `plinth:system:<event_class>` | `plinth:system:packages.installed` | Kernel subsystems (packages, users, auth, capabilities, nodes) |
| 3. Extension events | `plinth:ext:<extension>:<event_class>` | `plinth:ext:notes:chat_typing` | Extension JS via `pubsub.publish` |

**Validation regex** (pinned — matches either layer):

```
^plinth:(?:data:[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*|system:[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*|ext:[a-z][a-z0-9]*:[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*)$
```

Breakdown:

- `plinth:` literal prefix — every realtime channel is plinth-owned.
  Scoped prefix prevents accidental collisions with extensions' own
  LISTEN channels if they ever bypass `pubsub.publish`.
- `data:`/`system:`/`ext:` — Layer 1 / 2 / 3 discriminator.
- Layer 1 body: `<schema>.<table>`, both lowercase identifiers
  (matches PG identifier conventions; schemas are extension-generated
  per ICD-0.4.3).
- Layer 2 body: one or more dot-separated event-class segments
  (`packages.installed`, `auth.session.expired`, `node.join`). Leading
  segment is a kernel subsystem name; additional segments permit
  sub-classification without new top-level identifiers.
- Layer 3 body: `<extension>:<event_class>` where `<extension>` is
  the caller's `plinth.packages.name` (regex matches the package-name
  regex from ICD-0.4.1 — lowercase alphanumeric starts, no hyphens,
  up to the package-name length ceiling) and `<event_class>` follows
  the Layer 2 body rules.

**Forbidden characters** — any character not matching the regex
above, plus explicit rejection of `;`, whitespace, NUL, and
non-ASCII. Prevents PG command injection via `NOTIFY <channel>`
(though `pg_notify($1, $2)` already parameterizes; regex is defense
in depth).

**Channel length ceiling** — 63 bytes, matching PG's identifier
limit. `pg_notify` accepts TEXT so this is a policy choice, not a PG
constraint — picked to keep channel names short and scannable and to
leave room in log lines.

**Not specified in the regex (deferred):**

- Further structure within Layer 2 event classes (e.g. enum of
  `installed` / `uninstalled` / `upgraded` / `disabled` for
  `packages.*`). Each kernel subsystem decides its own classes at its
  own milestone; 0.5.0 only pins the top-level namespace.
- Whether any extension's identifier can equal a kernel subsystem
  name (e.g. an extension named `packages`). Declined as not-a-risk
  today — package-install rejection for the `packages` reserved name
  is per ICD-0.4.4's reserved-name set (`kernel`, `plinth`, `system`
  at current scope per `reserved_names.hpp`); `packages` is not on
  that list. This ICD intentionally does not widen it — if it becomes
  real, a 0.5.x RE-EVAL can tighten `reserved_names.hpp`.

---

## Payload Envelope Contract

Every realtime `NOTIFY` payload — across all three Layers — is a JSON
object matching:

```json
{
  "layer": "data" | "system" | "extension",
  "channel": "<full channel name>",
  "schema":  "<string, optional>",
  "table":   "<string, optional>",
  "ops":     [<optional array, 0.5.1 populates>],
  "seq":     <BIGINT, REQUIRED on persisted envelopes (ICD-0.5.5)>,
  "truncated": <optional boolean, 0.5.1 populates>,
  "payload": <optional arbitrary JSON, Layer 2/3>,
  "emitted_at": "<ISO-8601 string, optional>"
}
```

**Required fields:** `layer`, `channel`. Everything else is
layer-specific and nullable.

**Per-layer field semantics:**

- **Layer 1 (`data`).** 0.5.1 populates `schema` + `table` + `ops` +
  `seq`. `truncated` flips to `true` when the pre-truncation payload
  exceeds 8000 bytes and IDs were elided (0.5.1 behavior). 0.5.0
  never emits a Layer 1 NOTIFY directly — the coalescer owns this
  surface.
- **Layer 2 (`system`).** Kernel subsystems emit with `payload`
  carrying the event-class-specific body (e.g.
  `{"package_id": "...", "name": "...", "version": "..."}` for
  `packages.installed`). `schema` / `table` / `ops` are absent.
- **Layer 3 (`extension`).** `pubsub.publish(channel, payload)`
  produces an envelope with `layer = "extension"`, `channel` = the
  caller's argument, and `payload` = the caller's argument. `schema`
  / `table` / `ops` are absent. `seq` reserved for 0.5.5.

**8000-byte ceiling.** Before emission, the serialized envelope
(minified JSON, no whitespace) MUST fit in 8000 bytes. 0.5.0's
`emit_notify` helper enforces; 0.5.1's coalescer owns the truncation
policy (IDs elided, `truncated: true` flagged). 0.5.0 helper rejects
with `notify.payload_too_large` on overflow.

**`emitted_at`.** Optional ISO-8601 UTC timestamp. Not populated by
0.5.0's kernel emission (saves bytes); extensions populating
`pubsub.publish` payload with their own `emitted_at` inside `payload`
is fine. Pinned here so 0.5.4's persistence layer can adopt the field
later without an envelope-schema break.

**Forward compatibility.** Unknown top-level fields are accepted
silently by the listener dispatcher (matching the `architecture/03-data.md
§3` implicit contract and the ICD-0.4.1 §7.1 convention that already
operates on manifest JSON). Consumers validate what they need and
ignore the rest.

---

## Listener Subsystem

### Public API

```cpp
// src/kernel/realtime/listener.hpp
#pragma once

#include "kernel/config.hpp"

#include <json/value.h>

#include <functional>
#include <string>
#include <string_view>

namespace plinth::realtime {

// The dispatched event the listener hands to in-process consumers.
// Matches the envelope's required fields — `layer` and `channel` are
// always present; everything else is accessible via `envelope`.
struct DispatchedEvent {
    std::string layer;       // "data" | "system" | "extension"
    std::string channel;     // verbatim from NOTIFY
    Json::Value envelope;    // full parsed payload
};

// A consumer registers once at process startup. The listener invokes
// every registered handler for every parsed NOTIFY whose channel
// passes the channel-regex check. Handlers run on the listener
// thread — they MUST be fast (non-blocking). The 0.5.2 broker is the
// intended first consumer; 0.5.4's plinth.events writer is the
// second. 0.5.0 ships with zero consumers registered by default; the
// listener still starts and drains (useful for verifying PG
// connectivity + the no-consumer smoke path).
using EventHandler = std::function<void(const DispatchedEvent&)>;

auto register_handler(EventHandler h) -> void;   // idempotent add; not removable in 0.5.0

// Spawn the listener thread. Idempotent — a second call while the
// thread is running is a no-op. Errors log at error; the platform
// continues without realtime until restart. Mirrors
// plinth::capabilities::start_notify_listener's posture exactly.
auto start_listener(const Config::Database& db_cfg) -> void;

// Signal the listener to stop, wake the poll, and join the thread.
// Synchronous barrier: returns only after the thread has exited. Safe
// to call multiple times. MUST be called before
// drogon::app().quit() (atexit chain) and before log::shutdown() so
// the thread stops emitting spdlog writes before the async sink
// tears down. Mirrors plinth::capabilities::stop_notify_listener.
auto stop_listener() -> void;

// Test seam — parse a raw NOTIFY payload and dispatch it through the
// registered handlers exactly as the listener loop would. Does not
// open a PG connection. Returns true on a valid + dispatched
// payload, false on parse error / invalid channel / unknown layer.
auto apply_notification_for_test(std::string_view channel,
                                 std::string_view payload_json) -> bool;

}  // namespace plinth::realtime
```

### Threading & reconnect

Lifecycle, threading, `poll()` shape, and reconnect policy are
copied from `src/kernel/capabilities/listener.cpp`'s run_listener
pattern. The implementing session SHOULD refactor the shared shape
into a header-only `listener_loop.hpp` utility in `src/kernel/db/`
(takes a channel set + a handler functor + a resync callback),
collapsing the two listeners onto one mechanism, OR leave the
duplication in place — architect's call (see §OQ3). Either way, the
observable behavior is normative:

1. **Single `std::jthread`** per process owning a dedicated
   sync-libpq `PGconn`. No Drogon DbClient — the listener thread
   cannot be co-opted by the HTTP IO loops, and sync libpq is how
   0.2.3 does it. `PQconnectdb` with the same `build_conninfo`
   pattern as `listener.cpp:46–57`.
2. **Wake descriptor** — `eventfd(EFD_CLOEXEC | EFD_NONBLOCK)`;
   second `pollfd` entry alongside `PQsocket(conn)`.
3. **Poll timeout = 1000 ms** — matches 0.2.3. On wake-fd read, loop
   re-checks stop token and reconnect state.
4. **Reconnect backoff = 1 second** — matches 0.2.3. On
   `CONNECTION_BAD`, `PQfinish`, `sleep_for(1s)`,
   `PQconnectdb` + `LISTEN <channel>` × N (one LISTEN per
   subscribed channel class; see Channel Subscription below), back
   into the poll loop.
5. **Resync-on-reconnect hook** — the implementing session MAY
   optionally fire a consumer-supplied resync callback after every
   successful LISTEN open (mirrors the `reload_tier2_cache` pattern
   from 0.2.3). Not required by 0.5.0 — the 0.5.2 broker and 0.5.4
   persistence layer each have their own resync story. The hook is
   reserved in the `EventHandler` signature via an optional
   second-registration API; prefer not to speculate in 0.5.0.
6. **Stop barrier.** `stop_listener()` blocks until the thread has
   joined. Implementation: `request_stop()` + write one byte to the
   eventfd + `jthread::~jthread()` triggers join on optional reset.
   Matches `listener.cpp:336–351`.

### Channel subscription

0.5.0 subscribes to **three LISTEN channels** with wildcarding
delegated to PG's `pg_notify` → `LISTEN` semantics (PG 12+). Because
PG `LISTEN` does not support prefixes or wildcards, the listener
issues three literal LISTENs:

```sql
LISTEN "plinth:data:*";     -- conceptual; see implementation note
LISTEN "plinth:system:*";
LISTEN "plinth:ext:*";
```

**Implementation note — PG does not support wildcard LISTEN.** The
listener must LISTEN every full channel name it cares about. Two
options:

(a) **Single-channel fan-in.** The kernel emits every realtime
    NOTIFY on ONE channel (`plinth:realtime`) with the Layer
    encoded in the envelope's `layer` field. The listener then does
    Layer-aware dispatch. This is what DESIGN-capability-registry.md
    §Tier 2 already does (`plinth_capability_changed` as a single
    channel with `action` discriminator).

(b) **Per-event-class LISTEN.** Each event class gets its own
    channel; the listener dynamically `LISTEN`s as consumers
    register. More operationally complex; offers no latency win in
    PG (fan-out is cheap).

**Decision: option (a) — single-channel fan-in.** One literal channel,
`plinth:realtime` (exactly one colon per the regex above in Layer 1's
`data:` form — note: this is a short-form special-case described in
§Channel Naming Scheme footnote). The emit helper takes the logical
channel as the envelope's `channel` field; PG-side channel is always
`plinth:realtime`. Listener dispatches by `envelope.channel` string
matching, consumers filter. Matches the 0.2.3 precedent (one channel,
rich payload) and avoids the per-client LISTEN churn option (b)
would force on 0.5.2.

> **Regex note.** The Channel Naming Scheme regex above describes the
> *logical* channel carried in the envelope's `channel` field. The PG
> wire channel is always the literal string `plinth:realtime`. The
> regex validates the logical channel at emit-time and at listener
> parse-time; it never sees PG's channel string.

### Dispatch loop

Verbatim shape from `listener.cpp:247–313`, adapted to the new
envelope:

```cpp
// sketch — implementation detail, not normative
void run_listener(std::stop_token tok, Config::Database db, int wake_fd) {
    PGconn* conn = nullptr;
    while (!tok.stop_requested()) {
        if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
            /* PQfinish + reconnect + LISTEN "plinth:realtime" */
            continue;
        }
        /* poll(PQsocket, wake_fd, 1000 ms) */
        /* on PQsocket readable: PQconsumeInput + PQnotifies loop */
        /* for each PGnotify:
             parse envelope JSON -> DispatchedEvent
             validate channel regex (reject + warn on miss)
             invoke every registered EventHandler in order
         */
    }
    PQfinish(conn); close(wake_fd);
}
```

**Parse rejections** — malformed JSON, missing required fields
(`layer` / `channel`), `channel` failing the regex, `layer` not in
`{data, system, extension}` — are logged at warn and an audit event
fires (see §Audit Events). The listener does NOT reject-kill the PG
connection or unsubscribe; a poison payload is not an attack vector
(anyone who can `NOTIFY plinth:realtime` with malformed JSON could
also DOS the database in cheaper ways).

---

## NOTIFY Emission Helper

### Public API

```cpp
// src/kernel/realtime/emit.hpp
#pragma once

#include <json/value.h>
#include <libpq-fe.h>      // PGconn forward via libpq-fe is fine; keep TU-light

#include <drogon/orm/DbClient.h>   // for the async overload
#include <drogon/utils/coroutine.h>

#include <expected>
#include <string_view>

namespace plinth::realtime {

enum class NotifyError {
    INVALID_CHANNEL,        // channel fails the regex
    PAYLOAD_TOO_LARGE,      // serialized envelope > 8000 bytes
    PG_FAILURE,             // pg_notify returned non-OK
    MISSING_LAYER,          // envelope missing 'layer'
    LAYER_MISMATCH,         // envelope.layer conflicts with channel prefix
};

// Sync emit, within the caller's transaction (or outside — pg_notify
// auto-commits its effect only at caller-commit time). Validates the
// channel regex, validates the serialized envelope size, emits
// `SELECT pg_notify('plinth:realtime', <envelope>)`. Callers SHOULD
// run inside a BEGIN / COMMIT to ensure NOTIFY fires only on
// post-write success (PG buffers NOTIFYs to COMMIT).
auto emit_notify(PGconn& conn, const Json::Value& envelope)
    -> std::expected<void, NotifyError>;

// Async emit via Drogon's PG client pool. For 0.5.1 coalescer and
// future async callers. Same validation; same errors. Convenience —
// sync overload is always available for kernel sync paths.
auto emit_notify_async(drogon::orm::DbClientPtr db,
                       Json::Value envelope)
    -> drogon::Task<std::expected<void, NotifyError>>;

}  // namespace plinth::realtime
```

### Validation pipeline

`emit_notify` runs in order:

1. **`envelope["layer"]` extraction.** Required; missing →
   `MISSING_LAYER`.
2. **`envelope["channel"]` extraction + regex check.** Required;
   invalid → `INVALID_CHANNEL`.
3. **Layer↔channel consistency.** The envelope's `layer` field must
   agree with the channel's Layer prefix (`data:` → `"data"`,
   `system:` → `"system"`, `ext:` → `"extension"`). Inconsistency →
   `LAYER_MISMATCH`. Prevents Layer-1-shaped payloads from landing
   under a `plinth:ext:*` channel.
4. **Serialize to compact JSON** (`Json::StreamWriterBuilder`
   indentation = "", matching `registration.cpp:61–65`
   `json_write`).
5. **Size check.** `payload.size() > 8000` → `PAYLOAD_TOO_LARGE`.
6. **`PQexecParams("SELECT pg_notify($1, $2)", "plinth:realtime",
   payload)`** — parameterized, identical to
   `registration.cpp:85–94`'s `send_notify` pattern.
7. **PG result check.** Non-`PGRES_TUPLES_OK` → `PG_FAILURE` + warn
   log.

The async overload runs the same steps on the DbClient's executor.

### Callers in 0.5.0

- **`pubsub.publish` JS binding** (see §`pubsub.*` JS Stdlib).
- **Kernel Layer 2 emitters** — left as implementation-level
  integration points for existing kernel subsystems. 0.5.0 does NOT
  retrofit every kernel subsystem to emit NOTIFYs; it provides the
  helper. When a subsystem (packages, auth, users, capabilities,
  nodes) wants to emit a Layer 2 event, it calls `emit_notify` with
  the envelope prepared. The 0.5.0 milestone includes ONE
  retrofit — the `capabilities` subsystem's existing
  `plinth_capability_changed` emit is NOT migrated (it stays on its
  own channel; see §Relationship to 0.2.3 Listener). A new
  `packages.installed` emit is added as a smoke-test emission
  demonstrating the helper end-to-end (exercised by E.05).

### Callers out of scope for 0.5.0

- **0.5.1 coalescer** is the primary expected caller. Its debounce
  state machine lives in the db layer; it calls `emit_notify_async`
  after each window flush.
- **Every other Layer 2 kernel emitter** — added opportunistically
  as each subsystem's next milestone touches it. Not a 0.5.0
  obligation.

---

## `pubsub.*` JS Stdlib

### Namespace + binding

Injected into every `BridgeContext` via the existing
`inject_kernel_stdlib` seam (`src/kernel/js/stdlib_inject.cpp`). New
file `src/kernel/js/stdlib/pubsub_bindings.cpp` following the shape
of `log_bindings.cpp` / `config_bindings.cpp` / `crypto_bindings.cpp`.

Exposed surface on the JS side:

```javascript
// Publishes a Layer-3 extension event. Resolves on PG acknowledgment;
// rejects with a code-tagged error on validation or PG failure.
await pubsub.publish("plinth:ext:notes:chat_typing",
                     {user_id: "u1", room: "r7"});
```

### Binding implementation

Fires through the async bridge (ICD-0.3.3) — `pubsub.publish` is an
async op of type `PUBSUB_PUBLISH` (reserved enum variant per
DESIGN-quickjs-bridge.md §10, wired up here). Flow:

1. JS `pubsub.publish(channel, payload)` → C binding.
2. Binding validates:
   - `channel` is a string; matches the Layer 3 regex; **`<extension>`
     segment equals `bc.extension`** (the caller's extension identity,
     threaded onto `BridgeContext` by ICD-0.4.4's install-lifecycle
     seed).
   - `payload` converts to `Json::Value` via the existing
     `js_to_json` helper (ICD-0.3.3.3 §conversion).
   - Post-serialization size ≤ 8000 bytes.
3. `bc.register_pending(resolve, reject)` enqueues an `AsyncOp` with
   `type = PUBSUB_PUBLISH`.
4. The async dispatcher calls `emit_notify_async(bc.db, envelope)`
   where `envelope = {layer: "extension", channel, payload}`.
5. On `std::expected<void, NotifyError>` return:
   - Success → `bc.resolve(callback_id, Json::nullValue)`.
   - Error → `bc.reject(callback_id, {code: "pubsub.<suffix>", ...})`.

### Rejection codes

| NotifyError → | JS rejection `.code` | Meaning |
|---|---|---|
| INVALID_CHANNEL | `pubsub.channel_invalid` | channel fails the regex |
| *(extension mismatch)* | `pubsub.extension_mismatch` | channel's `<extension>` segment ≠ caller's extension |
| PAYLOAD_TOO_LARGE | `pubsub.payload_too_large` | envelope > 8000 bytes |
| LAYER_MISMATCH | `pubsub.channel_invalid` | treat as invalid — JS callers cannot control `layer` field, so this fires only on a regex/layer mismatch which we surface under the same code |
| PG_FAILURE | `pubsub.pg_error` | `pg_notify` PG-side error |
| MISSING_LAYER | *(impossible)* | binding always sets `layer = "extension"` |

Two codes (`pubsub.channel_invalid`, `pubsub.extension_mismatch`) fire
from the binding before the `emit_notify` helper runs; the rest are
direct pass-throughs with a prefix rename (`notify.*` →
`pubsub.*`).

### Kernel and sidecar callers

Kernel C++ paths that want to emit a Layer 2 or Layer 3 event call
`plinth::realtime::emit_notify` directly — not through
`pubsub.publish`. The JS binding is a JS-only surface. Sidecar
emission is not in 0.5.0 scope (§Out of scope).

### BridgeContext extension identity

`pubsub.publish`'s channel-name check reads
`bc.extension` — a string field added to `BridgeContext` by ICD-0.4.4
when the runtime pool is instantiated for a specific extension. If
`bc.extension` is empty (e.g. kernel-internal test-harness context
with no extension identity) the binding rejects with
`pubsub.extension_mismatch` — kernel code uses `emit_notify`
directly, not through the JS surface. Audit: `pubsub.reject` event
with `reason = "no_extension_context"`.

---

## Config Surface

New top-level `realtime` config block, JSON-parsed per the existing
convention (`src/kernel/config.cpp apply_*` helpers — add a new
`apply_realtime`).

```json
{
  "realtime": {
    "listener": {
      "enabled": true,
      "reconnect_backoff_ms": 1000
    },
    "notify": {
      "max_payload_bytes": 8000
    }
  }
}
```

| Key | Default | Semantics |
|-----|---------|-----------|
| `realtime.listener.enabled` | `true` | When `false`, `start_listener` no-ops. Useful for stripped-down deployments where realtime is not needed (test environments, CLI-only uses, sidecar-only nodes in a future topology). |
| `realtime.listener.reconnect_backoff_ms` | `1000` | Reconnect sleep on CONNECTION_BAD. 100–60000 clamped. 1000 matches 0.2.3 capability listener default and the architecture/03-data.md §3 "one reconnect backoff + one SELECT" divergence ceiling. |
| `realtime.notify.max_payload_bytes` | `8000` | Hard ceiling for `emit_notify`'s size check. Does not exceed PG's wire-format ceiling (8000). Value must be `≤ 8000` — larger rejected at config load time; zero / negative rejected. Useful for tightening the ceiling below 8000 in compliance deployments. |

**Deferred to later milestones:**

- `realtime.coalescer.window_ms` — 0.5.1.
- `realtime.persistence.retention_seconds` — 0.5.4.
- `realtime.broker.*` — 0.5.2.

All additive when they land; 0.5.0's config shape is forward-compatible.

---

## Audit Events

Three new event kinds, all routed through the existing `plinth::log::audit`
async writer per ICD-0.1.7. `g_audit_ready` gating applies — the
listener and emitter both fire audit events only after
`plinth::log::init()` has run.

| Event kind | Fired from | Payload |
|------------|------------|---------|
| `realtime.listener.started` | `start_listener` initial LISTEN open | `{"channel": "plinth:realtime", "node_id": "<hostname>"}` |
| `realtime.listener.reconnected` | every successful LISTEN re-open after a CONNECTION_BAD | `{"channel": "plinth:realtime", "backoff_ms": <actual>, "cause": "<PQerror-message>"}` |
| `realtime.notify.rejected` | `emit_notify` validation failure; listener parse failure | `{"direction": "emit"\|"receive", "reason": "<enum>", "channel": "<if present>", "bytes": <size if relevant>}` |

**No per-notification audit** — the realtime bus is a firehose;
auditing every message would dwarf the audit log and provide no
incremental signal. The `.rejected` event is the exception because
rejection is rare and load-bearing for debugging.

**Rationale for `.started` / `.reconnected` instead of a single
`.connected` event.** First connect vs. reconnect carries different
operational meaning — a pattern of frequent `.reconnected` events
under stable load points at a PG issue that the `.started` event
(present exactly once per process lifetime) cannot surface.

---

## HA Semantics

Verbatim promotion of `architecture/03-data.md §3.6` + `architecture/04-services-ha.md
§6.2`, pinned as the 0.5.0 contract:

1. **All N kernel nodes LISTEN on `plinth:realtime`.** No election,
   no leader.
2. **A NOTIFY emitted by any node reaches all nodes.** PG is the
   event bus. Propagation latency = PG `LISTEN/NOTIFY` backbone; no
   additional coordination.
3. **Fan-out to clients is per-node local.** Each node's listener
   dispatches to the node-local consumers (0.5.2 broker for
   WS-connected clients on *this* node). No cross-node client
   routing. A client connected to Node A receives events whether the
   emit came from Node A or Node B; but Node A does not know, nor
   need to know, whether any client is connected to Node B.
4. **No ordering guarantees across channels.** PG preserves NOTIFY
   order on the same connection-session pair; cross-session or
   cross-channel order is undefined. 0.5.5 sequence numbers (per
   Layer-1-table or per plinth-wide) close this gap where needed.
5. **Split-brain behavior.** If PG itself partitions (a multi-master
   or replica PG topology), NOTIFYs do not cross partition
   boundaries — `LISTEN/NOTIFY` is per-instance in PG. Plinth's
   current HA posture is shared-primary PG (`architecture/04-services-ha.md
   §6.1` — "PostgreSQL as coordination layer"); split-brain is a PG
   operational concern, not a kernel-bridge concern. Out of scope
   for 0.5.0.

---

## Relationship to the 0.2.3 Capability Listener

Two LISTEN/NOTIFY subscribers run in the same process after 0.5.0:

| Subsystem | Channel | Purpose | Handler |
|-----------|---------|---------|---------|
| 0.2.3 `plinth::capabilities::listener` | `plinth_capability_changed` | Tier 2 cache invalidation | `apply` → `upsert_tier2_entry` / `erase_tier2_entry` / `set_enabled_by_extension_in_cache` |
| 0.5.0 `plinth::realtime::listener` | `plinth:realtime` | Realtime event bus | registered `EventHandler`s (0.5.2 broker is the first) |

**Why two listeners, not a unified one?**

- The 0.2.3 listener has semantic responsibility for the Tier 2
  cache's correctness invariant (`reload_tier2_cache` on reconnect).
  Folding it into a general-purpose realtime subscriber couples the
  cache's correctness to the realtime bus's operational health —
  different failure domains, different restart semantics.
- The 0.2.3 channel name (`plinth_capability_changed`) predates the
  `plinth:<layer>:` scheme and is load-bearing for ICD-0.2.3
  compatibility. Renaming it would require coordinated CHANGELOG
  entries, migration notes, and a deprecation cycle 0.5.0 doesn't
  justify.
- Duplicated code shape (jthread+eventfd+reconnect) is acceptable
  for two instances. A shared `db::listener_loop` utility is
  discussed in §OQ3; the implementing session picks.

The 0.2.3 emit path (`registration.cpp:67–95 send_notify`) stays on
its own channel and stays on its own payload shape. 0.5.0's
`emit_notify` is a *new* surface, not a retrofit. Eventually 0.6.x
or 0.7.x may migrate the capability change stream under the Layer 2
kernel-events umbrella — out of 0.5.0 scope.

---

## Deterministic Teardown

Per `feedback_deterministic_teardown.md`, every new subsystem with
framework-owned thread/callback state needs a `cancel_all_*` entry
in the `std::atexit` chain before `drogon::app().quit()` runs.

**0.5.0 inserts `realtime::stop_listener()` into the atexit chain**
at [`src/kernel/main.cpp:250–258`](../../src/kernel/main.cpp):

```cpp
// BEFORE (0.4.4+)
std::atexit([] {
    plinth::packages::asset_server::cancel_all_registrations();
    plinth::ws::ConnectionRegistry::instance().cancel_all_timers();
    plinth::ws::ConnectionRegistry::initiate_shutdown();
    plinth::log::shutdown();
    plinth::capabilities::stop_notify_listener();
    plinth::ws::shutdown_js_stress_pool();
    drogon::app().quit();
});

// AFTER (0.5.0)
std::atexit([] {
    plinth::packages::asset_server::cancel_all_registrations();
    plinth::ws::ConnectionRegistry::instance().cancel_all_timers();
    plinth::ws::ConnectionRegistry::initiate_shutdown();
    plinth::realtime::stop_listener();           // ← NEW
    plinth::log::shutdown();
    plinth::capabilities::stop_notify_listener();
    plinth::ws::shutdown_js_stress_pool();
    drogon::app().quit();
});
```

**Ordering rationale.** `stop_listener()` runs:

- AFTER `ConnectionRegistry::cancel_all_timers` / `initiate_shutdown`
  — if any WS-side broker consumer (0.5.2) is registered with the
  realtime listener, the WS registry must be quiesced first so the
  broker handler never dispatches to a cancelled connection during
  teardown.
- BEFORE `log::shutdown` — the listener emits spdlog messages on
  shutdown (thread stop + final PQfinish log); logging must still be
  alive.
- BEFORE `capabilities::stop_notify_listener` — not strictly
  required, but symmetric ordering (first-created, last-stopped for
  the 0.2.3 listener; realtime listener is newer so it stops first).

Paralleled mirror edit required in `tests/kernel/ws/ws_test_fixture.cpp`
(per `feedback_deterministic_teardown.md` — prod + test atexit chains
must stay in lockstep).

**Startup placement** — [`src/kernel/main.cpp:286–294`](../../src/kernel/main.cpp)
currently carries `// TODO: realtime broker init`. 0.5.0 replaces
this with `plinth::realtime::start_listener(cfg.db)`, positioned
AFTER `plinth::capabilities::start_notify_listener(cfg.db)` and
BEFORE `register_healthz()`.

---

## Error Model

### `NotifyError` (kernel C++ emit side)

See §NOTIFY Emission Helper for the enum and semantics.

### JS `pubsub.publish` rejection codes

See §`pubsub.*` JS Stdlib → Rejection codes.

### Listener parse failures

Never surface to callers. Logged at warn, audited as
`realtime.notify.rejected` with `direction = "receive"`, then
dropped. The listener never closes the PG connection on a poison
payload.

### Configuration load failures

`realtime.listener.reconnect_backoff_ms` out of range `[100, 60000]`
→ config load rejects with
`config.realtime.listener.reconnect_backoff_ms_out_of_range`.
`realtime.notify.max_payload_bytes > 8000` or `≤ 0` → config load
rejects with `config.realtime.notify.max_payload_bytes_invalid`.
Matches the existing config-validation posture from
`src/kernel/config.cpp`.

---

## Security Constraints

1. **Channel regex enforced at emit AND receive.** An extension that
   somehow constructs a raw `NOTIFY` via a dangling SQL context
   (should be impossible per ICD-0.4.3 extension schema isolation,
   but defense-in-depth is cheap) still cannot inject malformed
   channels into the listener's dispatch — the listener re-validates
   on receive and drops rejects.
2. **Channel name is never interpolated into SQL.** `emit_notify`
   uses `SELECT pg_notify($1, $2)` parameterized — `$1` is the
   channel (always literal `plinth:realtime` from 0.5.0 kernel
   code), `$2` is the serialized envelope. No string concatenation
   into SQL. Matches `registration.cpp:85–94` `send_notify` posture.
3. **Payload size ceiling is enforced before PG call.** An oversized
   envelope never reaches PG — caller gets `PAYLOAD_TOO_LARGE`. Prevents
   a DOS pattern where an extension spams giant payloads and forces
   PG to allocate WAL / NOTIFY buffer before rejecting.
4. **Extension-identity gate on Layer 3 publishes.** A malicious
   extension cannot publish on another extension's channel — the
   `<extension>` segment of the Layer 3 channel must match the
   caller's `bc.extension`. Enforced in the `pubsub.publish` binding
   BEFORE the `emit_notify` call. Logged at warn on mismatch;
   audit-logged as `realtime.notify.rejected` with `reason =
   "extension_mismatch"`.
5. **Listener is non-blocking.** No Drogon IO thread is ever parked
   on the listener. The listener owns a dedicated thread with a
   dedicated PGconn; it never uses the Drogon DbClient pool. Matches
   the 0.2.3 posture and ensures that PG-side LISTEN slow-consumer
   warnings (PG logs these when `NOTIFY` accumulates faster than the
   client `PQconsumeInput`s) never cascade into HTTP / WS
   responsiveness.
6. **RBAC for `pubsub.publish`** — DESIGN §0.4.6 RBAC rule
   registration and ICD-0.2.4 capability-RBAC compose naturally:
   `pubsub.publish` is registered as the kernel capability
   `pubsub:1:publish` with RBAC rule `pubsub.publish`. Extensions
   that want to call `pubsub.publish` must carry the `pubsub.publish`
   rule in their `rbac.json`; admins grant the rule to groups per
   standard RBAC flow. **The extension-identity check is in
   ADDITION to the RBAC check, not instead of.** RBAC gates whether
   the extension can call `pubsub.publish` at all; the
   extension-identity check gates which channel within the Layer 3
   namespace. Standard kernel admin extensions (no RBAC grant
   needed) can publish to any `plinth:system:*` channel via
   `emit_notify` directly (C++ surface, no RBAC check — kernel-scope
   only).
7. **No per-payload audit.** The firehose would drown the audit log.
   Rejection audit is the exception; any future operational need for
   per-event auditing should live in 0.5.4 persistence, not the
   audit pipeline.

---

## Test Cases

Test prefix: **R.*** for listener-lifecycle, **E.*** for emitter,
**P.*** for `pubsub.*` JS binding. Distinct from ICD-0.3.5's N.37–N.47
and ICD-0.4.6's P.01–P.N1 — the P.* prefix here carries the
`[realtime][js]` tag where ICD-0.4.6's P.* carried `[rule_validator]`. Catch2
tags keep them disjoint.

Total: **21 new cases** (10 R + 6 E + 5 P). Distributed across three
test TUs.

### Listener lifecycle — `tests/kernel/realtime/listener_test.cpp`

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| R.01 | Happy | `start_listener` then send one NOTIFY from another PGconn; registered handler is invoked exactly once | Yes | Handler called once with `DispatchedEvent{layer="system", channel="plinth:system:test", ...}` |
| R.02 | Happy | `start_listener` with `enabled=false` in config | No | No thread spawned; `stop_listener()` no-ops |
| R.03 | Reconnect | Mid-run, kill the listener's PG connection (`pg_terminate_backend` from a side PGconn) | Yes | Listener reconnects within 1×backoff + ε; next NOTIFY reaches the handler; `realtime.listener.reconnected` audit event fired |
| R.04 | Reconnect | Listener with no initial PG (PG down at start) | Yes-ish | Listener logs connect error, backs off, retries; once PG comes up, LISTEN succeeds |
| R.05 | Teardown | `stop_listener()` while a NOTIFY is in-flight (between PQconsumeInput and dispatch) | Yes | Stop-barrier waits for dispatch; no partial state; thread joined cleanly |
| R.06 | Teardown | Idempotent stop | No | Second `stop_listener()` no-ops |
| R.07 | Teardown | Double-start | No | Second `start_listener()` no-ops |
| R.08 | Parse-reject | Emit a NOTIFY with malformed JSON | Yes | Handler not invoked; warn log emitted; `realtime.notify.rejected` audit with `direction="receive", reason="json_parse_error"` |
| R.09 | Parse-reject | Emit a NOTIFY with valid JSON but missing `layer` field | Yes | Rejected; audit `reason="missing_layer"` |
| R.10 | Multi-handler | Register three handlers; emit one NOTIFY | Yes | All three called in registration order |

### Emitter — `tests/kernel/realtime/emit_test.cpp`

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| E.01 | Happy | `emit_notify` with a valid Layer-2 envelope | Yes | Returns success; the running listener receives the event (end-to-end smoke) |
| E.02 | Invalid channel | Channel `plinth:data:` (missing table) | No | `INVALID_CHANNEL`; no PG call |
| E.03 | Invalid channel | Channel `plinth:ext:notes:has spaces` | No | `INVALID_CHANNEL`; no PG call |
| E.04 | Oversized | Envelope post-serialization = 8001 bytes | No | `PAYLOAD_TOO_LARGE`; no PG call |
| E.05 | Layer mismatch | `{layer: "data", channel: "plinth:system:foo"}` | No | `LAYER_MISMATCH`; no PG call |
| E.06 | Async overload | `emit_notify_async` through Drogon DbClient | Yes | Co_await resolves; listener receives event |

### `pubsub.publish` JS binding — `tests/kernel/js/pubsub_test.cpp`

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| P.01 | Happy | Extension `notes` calls `await pubsub.publish("plinth:ext:notes:chat_typing", {user: "u1"})` | Yes | Resolves; listener receives envelope with correct layer/channel/payload |
| P.02 | Extension mismatch | Extension `notes` calls `pubsub.publish("plinth:ext:terminal:evil", {})` | No | Rejects with `.code = "pubsub.extension_mismatch"`; audit fired |
| P.03 | Invalid channel | `pubsub.publish("plinth:data:ext_notes.notes", {})` — Layer 1 shape | No | Rejects with `.code = "pubsub.channel_invalid"` (Layer 3 binding cannot emit to Layer 1) |
| P.04 | Oversized | Payload serializing to > 8000 bytes | No | Rejects with `.code = "pubsub.payload_too_large"` |
| P.05 | No extension context | Kernel-scope `BridgeContext` (empty `bc.extension`) calls `pubsub.publish` | No | Rejects with `.code = "pubsub.extension_mismatch"`; audit `reason="no_extension_context"` |

**Test-seam notes:**

- R.01 / R.03 / R.08 / R.09 / R.10 + E.01 / E.06 use
  `apply_notification_for_test` where the raw-payload shape is under
  test, and the full PG round-trip where the emit→receive smoke is
  under test. Implementing session picks per case.
- P.01 uses the 0.4.5.1 grouped-test model's `[js][realtime]`
  routing so the `plinth_tests_js` group picks them up without a
  per-TEST_CASE subprocess explosion.
- The `max_payload_bytes` knob (default 8000) is tweaked to a lower
  value in E.04 / P.04 fixtures so oversized test payloads stay
  readable. Valid per the config-validation bounds.
- No fork/SIGKILL harness is required — R.05 closes the teardown
  race via `cancel_all_timers`-style synchronous barrier, same
  pattern the 0.4.4 C13 crash-recovery tests use.

### CI wiring

- `src/kernel/realtime/` — new directory.
  - `listener.{hpp,cpp}`
  - `emit.{hpp,cpp}`
- `src/kernel/js/stdlib/pubsub_bindings.cpp` — new.
- `src/kernel/js/stdlib_inject.cpp` — edited to register
  `pubsub.*`.
- `src/kernel/js/async_op.hpp` — `PUBSUB_PUBLISH` enum variant wired
  into the dispatcher (DESIGN-quickjs-bridge.md §10 had it reserved).
- `src/kernel/config.{hpp,cpp}` — `apply_realtime` helper +
  `Config::Realtime` substruct with `listener.enabled`,
  `listener.reconnect_backoff_ms`, `notify.max_payload_bytes`.
- `src/kernel/main.cpp` — atexit edit + startup edit (drops the
  `// TODO: realtime broker init` placeholder).
- `tests/kernel/realtime/listener_test.cpp` — new.
- `tests/kernel/realtime/emit_test.cpp` — new.
- `tests/kernel/js/pubsub_test.cpp` — new.
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit-chain mirror edit.
- `CMakeLists.txt` — no new glob edit required (kernel + tests
  globs already cover `src/kernel/**`, `tests/kernel/**`). The
  `realtime_test` sources fall into the `plinth_tests_pg` +
  `plinth_tests_js` groups per their Catch2 tags; no new ctest
  grouping.
- `migrations/schema.sql` — **no edits.** `plinth.events`
  persistence is 0.5.4; no new tables.
- `docker/ci.Dockerfile` — no edits.
- `.gitea/workflows/ci.yml` — no new jobs.

### Test count target

21 new cases. Full suite grows from current 49/49 ctest groups
(49 subprocesses counting the 45 `[js][async]` per-TEST_CASE entries)
to 49/49 + 21 new TEST_CASEs distributed across the existing groups
— 10 into `plinth_tests_pg` via `[realtime]` tag, 6 into
`plinth_tests_pg` via `[realtime]` tag, 5 into `plinth_tests_js` via
`[js][realtime]` tag. No new ctest entries, no new subprocess count.

---

## Entry / Exit

**Entry criteria:**

- `RE-EVAL following 0.4.7 (0.4.x arc closeout)` merged (done —
  PR #64, commit `783687b`).
- `0.4.7.2 ICD-0.5.0 authored` docs session merged (this document).
- `v0.4.7` tag cut on the Slice C merge commit (architect action —
  tag lands on `4a86e31` per `feedback_tagging_rule.md`).
- ICD-0.2.2's LISTEN/NOTIFY reconnect pattern is stable in
  production (done — shipped in v0.2.3, amended in v0.2.4, unchanged
  through v0.4.7).
- ICD-0.3.3's `BridgeContext` + async bridge pattern is stable
  (done — shipped in v0.3.3, reinforced in v0.3.3.1).
- ICD-0.4.4's `BridgeContext::extension` identity field populated
  per install-lifecycle (done — shipped in v0.4.4).

**Exit criteria:**

- `src/kernel/realtime/` subsystem ships with `start_listener` /
  `stop_listener` / `register_handler` / `emit_notify` /
  `emit_notify_async`.
- `pubsub.publish` JS binding ships; `pubsub.subscribe` NOT present
  (ReferenceError on call).
- All 21 R/E/P test cases pass; PG-gated cases skip cleanly when PG
  env is absent; grouped-test ctest count is 49/49 green.
- `run-clang-tidy-20` zero findings on new TUs (`listener.cpp`,
  `emit.cpp`, `pubsub_bindings.cpp`) and modified TUs (`main.cpp`,
  `stdlib_inject.cpp`, `config.cpp`, `async_op.hpp`,
  `ws_test_fixture.cpp`).
- No regressions on the v0.4.7 test matrix (49/49 grouped subprocess
  runs + 45 per-`[js][async]` subprocess runs).
- Atexit chain in `main.cpp` + `ws_test_fixture.cpp` updated in
  lockstep; 20-run ctest-loop sample shows zero teardown-race
  reproductions (matching 0.4.0.1 validation bar for new subsystems).
- `CHANGELOG.md` v0.5.0 entry describes the listener, the emit
  helper, the `pubsub.publish` binding, the atexit chain edit, the
  config surface, the audit events, and every accepted deviation.
- `docs/ROADMAP.md §0.5` line for 0.5.0 is removed.
- `v0.5.0` tag cut on the merge commit.
- Memory `project_plinth_state.md` updated to reflect 0.5.0
  shipped; `project_next_session_0_5_0.md` retired (replaced by
  `project_next_session_0_5_1.md` pointing at the coalescer work).

---

## Open Questions

**OQ1 — Per-channel RBAC for `pubsub.publish`, or pattern-match
only?** The current ICD pins **pattern-match only** (channel prefix
must match caller's extension). A finer-grained model would require
manifest declaration: every channel an extension wants to publish
on listed in `capabilities.json` (new field, or overloading
`provides[]`), admin visibility into extension channel usage, and
kernel enforcement on each publish. Recommendation: defer to
0.6a-C RBAC administration UI — an admin visibility pattern makes
sense alongside a channel-browser extension, and 0.5.0's pattern
rule is safe by construction (can't forge another extension's
identity). If a concrete security or visibility requirement
emerges at 0.6.x, a delta ICD patches this contract.

**OQ2 — Dedicated sync-libpq PGconn vs Drogon DbClient pool for
the listener.** Pinned: dedicated sync-libpq. Rationale — mirrors
0.2.3 precedent, avoids mixing Drogon's callback-driven IO model
with libpq's blocking LISTEN, keeps the listener's thread and
connection lifecycle under `plinth::realtime::` control. The Drogon
DbClient is used only by `emit_notify_async` which is fire-and-forget;
never held by the listener.

**OQ3 — Factor the jthread+eventfd+reconnect loop into a shared
utility, or leave it duplicated in `listener.cpp` (0.2.3) +
`realtime::listener.cpp` (0.5.0)?** Implementer's call. Pro-shared:
two consumers is enough to justify; the loop body is ~80 lines of
nearly-identical code; a third listener at 0.5.4 persistence or
elsewhere is likely. Pro-duplicated: the two subsystems are
semantically distinct (cache invalidation vs event bus), and the
0.2.3 listener has amendments (resync-on-reconnect per ICD-0.2.4)
that are not 0.5.0 concerns. Recommendation: duplicate in 0.5.0
(lowest-risk diff); the implementing session may opt to pre-factor
if the duplication reads worse in practice than the 0.2.3 version
it's modeled on. A follow-up `0.5.x.N` or 0.5.1-adjacent cleanup
slot can extract the shared utility once it earns its third caller.

**OQ4 — `plinth:realtime` single-channel fan-in vs per-class
channels.** Pinned: single-channel fan-in. See §Channel Subscription
→ Implementation note. The alternative (per-class LISTEN) is
revisitable at 0.5.2 if broker subscription churn becomes a
latency concern; 0.5.0 does not foreclose it — the listener's
dispatch already filters by envelope `channel`.

**OQ5 — Consumer-removable handler registration.** `register_handler`
is additive-only in 0.5.0. A `deregister_handler(handle)` form is
useful for dynamic subsystem teardown but has no caller in 0.5.0
(0.5.2 broker registers once per process). Recommendation: leave
additive-only; add `deregister` when a second use case emerges.

---

## Appendix: End-to-End Example

Extension `notes` on a three-node deployment, RBAC grants the `notes`
extension the `pubsub.publish` rule. JS path:

```javascript
// In an extension handler on Node B
await pubsub.publish("plinth:ext:notes:chat_typing",
                     {user_id: "u1", room_id: "r7"});
```

Kernel-side flow:

1. JS binding validates the channel regex, extracts
   `bc.extension == "notes"`, confirms the `<extension>` segment
   (`notes`) matches. OK.
2. Payload serializes to compact JSON: ~80 bytes. Under ceiling.
3. `dispatch_async_op_detached` routes to
   `emit_notify_async(bc.db, envelope)`. Envelope:

   ```json
   {
     "layer": "extension",
     "channel": "plinth:ext:notes:chat_typing",
     "payload": {"user_id": "u1", "room_id": "r7"}
   }
   ```

4. `emit_notify_async` → Drogon DbClient → `SELECT pg_notify('plinth:realtime', <envelope>)`
   → PG ACK. JS `await` resolves.

5. PG LISTEN/NOTIFY backbone delivers the NOTIFY to all three
   nodes' `plinth::realtime::listener`s.

6. On each node, the listener's dispatch loop:
   - `PQconsumeInput` + `PQnotifies` yields the envelope.
   - Parses; validates the channel regex.
   - Invokes every registered `EventHandler`. On 0.5.0, zero
     handlers are registered unless another subsystem's init added
     one — e.g. the 0.5.2 broker would be a handler that matches the
     channel against its subscription table and queues fan-out onto
     each matching WS connection's owning event loop.

7. From the extension author's perspective: one JS call, events
   arrive on every subscribed client anywhere in the HA deployment.
   The 0.5.0 milestone delivers steps 1-6; step 7 requires 0.5.2's
   broker.

---

## Appendix: Config Example

`config.json` example with realtime configured:

```json
{
  "database": { "host": "127.0.0.1", "port": 5432, "database": "plinth", ... },
  "logging":  { "level": "info", ... },
  "realtime": {
    "listener": {
      "enabled": true,
      "reconnect_backoff_ms": 1000
    },
    "notify": {
      "max_payload_bytes": 8000
    }
  }
}
```

Minimal valid realtime block (all defaults):

```json
"realtime": {}
```

Stripped deployment disabling realtime:

```json
"realtime": { "listener": { "enabled": false } }
```

The `apply_realtime(const Json::Value&)` helper in
`src/kernel/config.cpp` reads the block; absent → all defaults.
