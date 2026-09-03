# ICD-0.5.2-ws-broker

**Traces to:** architecture/03-data.md §3.3 (Four Realtime Layers — row
1 + row 2 + row 3 are the three producer families this broker fans out;
0.5.2 is the "Each node's WS Broker checks client subscriptions" box in
§Appendix A line 478); architecture/03-data.md §3.4 (Frontend SDK —
Debounced Smart Re-Query; clients consume the frames this broker emits,
so the wire-frame shape this ICD pins is the contract the 0.6.3 SDK
codes against); architecture/03-data.md §3.5 (Delta Sync on Reconnect;
this ICD explicitly defers durable per-user subscriptions to 0.5.4,
preserving that section's invariant); architecture/03-data.md §3.6 (HA
Realtime — per-node local fan-out is the invariant this ICD promotes);
architecture/03-data.md §3.6.1 (Physical Channel Fan-In — names the
0.5.2 broker as the first real consumer of the listener's registered
`EventHandler` slot, lines 440–454); architecture/02-capabilities.md
§3.1 (Async Dispatch Arm + Extension Runtimes — `pubsub.subscribe`
composes on the async-bridge arm this subsection contracts, so the new
binding's enqueue pattern must match); architecture/05-extensions.md
§3.2 (supervision + runtime-lifecycle — the drain hook on DISABLED /
UPGRADING / UNINSTALL this ICD specifies mirrors the coalescer
`drain_extension` pattern from ICD-0.5.1 §Extension-lifecycle
integration); ICD-0.5.0-pg-listen-notify-bridge §Channel Naming Scheme
(the regex + three-layer name shape the broker matches subscriptions
against; verbatim reuse — no new channel namespace); ICD-0.5.0 §Payload
Envelope Contract (required `layer` + `channel`; optional `schema` /
`table` / `ops` / `seq` / `truncated` / `payload` / `emitted_at` — the
broker forwards whole envelopes as `payload` in the client frame, see
§Subscription Matching); ICD-0.5.0 §Listener Subsystem (the
`register_handler(EventHandler)` slot the broker consumes at startup,
lines 295–356); ICD-0.5.0 §`pubsub.*` JS Stdlib (the existing
`pubsub.publish` binding this ICD extends with `pubsub.subscribe`,
lines 573–648; rejection code prefix `pubsub.*` continues here);
ICD-0.5.0 §HA Semantics (per-node local fan-out — verbatim promotion);
ICD-0.5.1-pg-auto-event-coalescer §Appendix A step 10 (the broker is
the pipe from `realtime::listener` → `ConnState::channels` match →
`ws::publish`; the Layer-1 envelopes ICD-0.5.1 builds are this ICD's
primary input); ICD-0.5.1 §Extension-lifecycle integration (the drain
pattern on DISABLED / UPGRADING / UNINSTALL — broker mirrors coalescer
here for open subscriptions); ICD-0.1.6-websocket §Subscribe /
Unsubscribe (the existing frame shape + silent-omission convention; the
"per-channel rule naming convention... deferred to a later ICD" at
line 214 — **this is that later ICD**); ICD-0.1.6 §Security Constraints
item 2 (the RBAC additive-union invariant the broker enforces);
ICD-0.4.6-rbac-rule-registration (the registration path the new subscribe
rules ride — one `rbac.json` line per extension, registered via the
standard extension-install flow); ICD-0.4.4-package-install-lifecycle
(`BridgeContext::extension_name` — the identity the `pubsub.subscribe`
binding reads for the Layer-3 extension-identity gate, mirroring
`pubsub.publish`); ICD-LH-1-listen-notify-storm §9 Future work (LH-2
reuses LH-1's driver + adds N WS subscribers; 0.5.2 broker is the LH-2
gate).

**Depends on:** ICD-0.5.0 (listener + emit + envelope + `pubsub.publish`
+ channel-naming regex — upstream contract; broker is a consumer of
0.5.0's `register_handler` slot); ICD-0.5.1 (the Layer-1 producer this
broker fans out by default; envelope shape already pinned); ICD-0.5.0.3
(extension-dispatch + `RuntimePool` + install-lifecycle hooks — the
drain-on-extension-lifecycle call sites this broker plugs into, alongside
coalescer); ICD-0.4.6-rbac-rule-registration (the `rbac.json` surface the
new per-channel subscribe rules register against — no new infrastructure,
this ICD just names the rule tokens); ICD-0.4.4-package-install-lifecycle
(`BridgeContext::extension_name` populated per-pool at install —
`pubsub.subscribe` reads this for the Layer-3 identity gate);
ICD-0.3.3-async-bridge (`AsyncOp` snapshot pattern — new
`PUBSUB_SUBSCRIBE` enum variant extends the existing dispatch surface,
same pattern `PUBSUB_PUBLISH` takes in 0.5.0); ICD-0.1.6-websocket
(`ConnState::channels` + existing `on_subscribe` / `on_unsubscribe` + the
`{type:"event", channel, payload}` wire frame — broker composes on, does
not replace); ICD-0.1.7-audit (async audit writer + `g_audit_ready` gate
— broker audit events rate-limited per §Audit Events).

**Milestone:** 0.5.2 — WebSocket broker fan-out. Third 0.5.x code
milestone (after v0.5.0 bridge + v0.5.1 coalescer). The listener→client
wiring: registers an `EventHandler` with `realtime::register_handler`
at startup, composes over the existing `ws::publish` primitive for
per-connection fan-out, and authors the missing `pubsub.subscribe` JS
binding. Paired paper ICD authoring slot `0.5.1.2` precedes this code
work per METHODOLOGY §3.1 forward-ICD-presence rule and
`feedback_icd_horizon.md`. Promoted `[medium]` → `[strong]` by
`docs/reviews/RE-EVAL-0.5.x-following-0.5.1.md §2.7`.

**Status:** Ready for implementation

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
[src/kernel/realtime/listener.hpp](../../src/kernel/realtime/listener.hpp)
(`register_handler(EventHandler)` at line 42 — the public slot the broker
registers its handler into at `start_broker()`; `DispatchedEvent` at
lines 27–31 — the handler's input);
[src/kernel/ws/publish.hpp](../../src/kernel/ws/publish.hpp) +
[src/kernel/ws/publish.cpp](../../src/kernel/ws/publish.cpp) (`publish(channel, payload)`
— the existing lock-snapshot-queueInLoop-per-conn fan-out the broker
reuses; no new fan-out primitive);
[src/kernel/ws/conn_state.hpp](../../src/kernel/ws/conn_state.hpp)
(`ConnState::channels` at line 20 — the `unordered_set<string>` of
subscribed channel names the broker reads via the registry's `for_each`
snapshot);
[src/kernel/ws/connection_registry.hpp](../../src/kernel/ws/connection_registry.hpp)
(`ConnectionRegistry::for_each` — the lock-safe snapshot primitive
`ws::publish` already uses);
[src/kernel/ws/subscriptions.hpp](../../src/kernel/ws/subscriptions.hpp) +
[src/kernel/ws/subscriptions.cpp](../../src/kernel/ws/subscriptions.cpp)
(`on_subscribe` / `on_unsubscribe` — the admin-only 0.1.6 handlers this
ICD extends with per-channel RBAC checks);
[src/kernel/js/stdlib/pubsub_bindings.cpp](../../src/kernel/js/stdlib/pubsub_bindings.cpp)
(existing `pubsub.publish` binding in the 7-step validation chain; the
new `pubsub.subscribe` joins this TU with the same `inject_kernel_stdlib`
registration seam);
[src/kernel/js/async_op.hpp](../../src/kernel/js/async_op.hpp)
(`AsyncOp::Type::PUBSUB_PUBLISH` — the enum the new `PUBSUB_SUBSCRIBE`
variant joins; `bc_extension_name` snapshot field from 0.5.1 also
populated for `PUBSUB_SUBSCRIBE`);
[src/kernel/realtime/channel.hpp](../../src/kernel/realtime/channel.hpp)
(`validate_channel`, `channel_layer`, `channel_extension` — the broker
uses these for layer discrimination + the Layer-derived RBAC rule
generator; no new parser);
[src/kernel/realtime/coalescer.cpp:99](../../src/kernel/realtime/coalescer.cpp)
(`build_envelope` — the Layer-1 producer the broker's end-to-end flow
starts from; §Appendix A traces this);
[src/kernel/main.cpp](../../src/kernel/main.cpp) (atexit chain — new
`realtime::stop_broker()` inserts between `realtime::stop_listener()`
and `CoalescerRegistry::instance().shutdown()`; startup order — new
`realtime::start_broker()` after `realtime::start_listener` and before
route registration);
[src/kernel/packages/install_lifecycle.cpp](../../src/kernel/packages/install_lifecycle.cpp)
(DISABLED / UPGRADING / UNINSTALL — broker drain hook call sites,
mirroring the coalescer's three call sites);
[docs/architecture/03-data.md §3.3–§3.6.1 + Appendix A](../architecture/03-data.md)
(normative prose promoted to contract).

---

## Overview

0.5.2 lands the **WebSocket broker**: the kernel component that consumes
0.5.0's listener dispatch stream and fans each envelope out to every
authenticated WebSocket connection whose `ConnState::channels` contains
the envelope's logical channel, subject to per-channel RBAC. It is the
first real `EventHandler` registered against
`plinth::realtime::listener::register_handler` — the slot 0.5.0 reserved
for this milestone.

The broker also authors **`pubsub.subscribe`**, the JS-side counterpart
to `pubsub.publish`. Kernel and extension JS code can subscribe to
realtime channels and receive a handler invocation per matching envelope.
Subscribe from JS is async-bridge-enqueued (`PUBSUB_SUBSCRIBE`) exactly
as publish is (`PUBSUB_PUBLISH`); the per-`BridgeContext` subscription
registry is a kernel-side peer to the WS-side `ConnState::channels`, and
the broker fans out to both via two distinct dispatch arms from a single
`EventHandler`.

The third contribution is **per-channel subscribe RBAC**. ICD-0.1.6
§Implementation Notes item 2 wrote: "The per-channel rule naming
convention… is deferred to a later ICD written alongside real channel
producers." With 0.5.0 + 0.5.1 shipped, the producers are real; this
ICD is that later ICD. The rule-naming convention is layer-derived —
the broker computes the required RBAC rule token from the channel
string itself, no per-channel manifest ceremony.

The broker does NOT introduce a new fan-out primitive: it reuses
`plinth::ws::publish(channel, Json::Value)` (lock + snapshot +
`queueInLoop` per subscriber conn — the primitive 0.1.6 shipped and
that has been in-process-only until now). The broker extends this
with an envelope-aware overload `publish_dispatched(DispatchedEvent&)`
that also performs the per-subscriber RBAC re-check the §Security
Constraints require, while reusing the identical lock-snapshot-queue
skeleton.

**Scope:**

1. `plinth::realtime::broker` subsystem — `start_broker`,
   `stop_broker`, `broker_dispatch_for_test` test seam, metrics
   getters; registers one `EventHandler` with
   `realtime::register_handler` at start; dispatches every envelope by
   calling `ws::publish_dispatched(event)` + the kernel-side
   `pubsub::dispatch_to_js_subscribers(event)` peer.
2. `plinth::ws::publish_dispatched(DispatchedEvent&)` — envelope-aware
   fan-out. Snapshots the registry, per-conn filters by
   `ConnState::channels` AND re-checks per-channel RBAC AND queues a
   `{type:"event", channel, payload: <envelope>}` frame onto the conn's
   `loop`.
3. Per-channel subscribe RBAC — Layer-derived rule naming
   (§Subscription RBAC); admin bypass preserves 0.1.6 contract;
   silent-omission on denial preserves 0.1.6 ack shape.
4. `pubsub.subscribe(channel, handler) → unsubscribe_fn` JS binding in
   `src/kernel/js/stdlib/pubsub_bindings.cpp`; async-bridge enqueued
   via `AsyncOp{PUBSUB_SUBSCRIBE}`; kernel-side per-`bc` subscription
   registry; handler invoked via the existing async-callback path.
5. `realtime.broker.*` config block — `enabled`,
   `max_subscriptions_per_conn`, `rbac.enforce`.
6. Extension-lifecycle drain — DISABLED / UPGRADING / UNINSTALL drain
   both WS and kernel-side subscriptions whose channels match the
   extension's schema prefix or Layer-3 extension segment. Mirrors
   ICD-0.5.1 §Extension-lifecycle integration shape.
7. Atexit chain extension — `stop_broker()` between `stop_listener()`
   and `CoalescerRegistry::shutdown()`.

**Out of scope (explicit):**

- **Durable per-user subscriptions** (subscriptions surviving WS
  reconnect by being tracked server-side per user id). Deferred to
  0.5.4 where `plinth.events` persistence already pays the per-user
  durability cost. 0.5.2 subscriptions are per-connection — clients
  resubscribe on reconnect (matches 0.1.6 implicit contract).
- **Wildcard / pattern subscriptions.** Exact-string match against
  envelope `channel` only. Wildcards (`plinth:data:ext_notes.*` etc.)
  may land in 0.5.4 or later; 0.5.2 keeps the match function simple.
- **Sequence numbers + client-side debounce protocol.** 0.5.5 work.
  Frames carry the envelope's `seq` field only if the producer sets
  it; 0.5.2 has no producer that populates `seq` (0.5.1 coalescer
  defers `seq` to 0.5.5 per its OQ4).
- **Delta sync on reconnect.** 0.5.4 work (architecture/03-data.md §3.5).
- **Cross-node fan-out semantics change.** Per-node local fan-out is
  the 0.5.0 HA invariant and stays (§HA Semantics); a client connected
  to Node A receives events whether emit came from Node A or Node B,
  but routing across nodes is PG's job, not the broker's.
- **Sidecar subscribers.** 0.8.x work; broker is kernel-local.
- **Subscription introspection admin API** (list subscribers,
  count-per-channel, etc.). A future admin-extension panel (0.6a-\*)
  can consume the in-memory counts the broker already tracks; 0.5.2
  ships the counts as metrics getters only.
- **`pubsub.subscribe` from sidecar JS runtimes.** 0.8.x work; binding
  is kernel-JS only in 0.5.2.

---

## Broker Subsystem

### Public API

```cpp
// src/kernel/realtime/broker.hpp
#pragma once

#include "kernel/config.hpp"
#include "kernel/realtime/listener.hpp"     // DispatchedEvent

namespace plinth::realtime {

// Spawn the broker. Registers one EventHandler with listener's
// register_handler(). Idempotent — second call while started is a
// no-op. No-op (without error) if broker_cfg.enabled is false.
// MUST be called after start_listener() so the registration lands
// before any NOTIFY arrives (handler-registration race is benign —
// register_handler is safe pre/post start per 0.5.0 — but startup
// ordering is the intent and is what main.cpp pins).
auto start_broker(const Config::Realtime::Broker& broker_cfg) -> void;

// Signal the broker to stop accepting dispatches and clear its
// JS-side subscription registry. Does NOT deregister the
// EventHandler (0.5.0 does not support deregister — see §OQ1 path).
// Idempotent. MUST be called after stop_listener() so no new
// dispatch races a partially-cleared broker. Mirrors the
// realtime::stop_listener + CoalescerRegistry::shutdown posture.
auto stop_broker() -> void;

// Test seam — dispatch a DispatchedEvent through the broker exactly
// as the registered EventHandler would, skipping listener wire
// parsing. Covers B.* cases without PG. Returns the number of
// connections matched AND RBAC-allowed (useful for assertions).
auto broker_dispatch_for_test(const DispatchedEvent& ev) -> std::size_t;

// Metrics — cheap in-memory counters. Monotonic across the process
// lifetime; not reset between tests except by the explicit
// `reset_metrics_for_test` seam. Intended for 0.7.x plinth.metrics
// wiring; 0.5.2 ships them as getters only.
auto dispatch_count()      -> std::uint64_t;
auto rbac_denial_count()   -> std::uint64_t;
auto js_subscriber_count() -> std::size_t;   // live per-bc subscriptions
auto ws_subscriber_count() -> std::size_t;   // total (conn, channel) pairs

auto reset_metrics_for_test() -> void;

}  // namespace plinth::realtime
```

### Lifecycle integration

The broker's `EventHandler` closure is registered at `start_broker` via
`listener::register_handler(broker_event_handler)`. The handler body:

```
void broker_event_handler(const DispatchedEvent& ev) {
    if (!broker_enabled.load(std::memory_order_acquire)) return;
    ws::publish_dispatched(ev);
    pubsub::dispatch_to_js_subscribers(ev);
    dispatch_count_.fetch_add(1, std::memory_order_relaxed);
}
```

`stop_broker` flips `broker_enabled` to `false` (relaxed store +
release fence), clears the kernel-side JS subscription registry, and
returns. It does NOT attempt handler deregistration: 0.5.0 pins
"handlers idempotent add; not removable" (listener.hpp:42). The
`broker_enabled` flag short-circuits post-stop dispatches cheaply.

### Threading model

- The handler body **runs on the listener thread** (the `std::jthread`
  spawned by `realtime::start_listener`). Per ICD-0.5.0 §Listener
  Subsystem the contract is: handlers "MUST be fast (non-blocking)".
- The handler itself does **zero blocking work**:
  - `ws::publish_dispatched` takes the registry's shared_mutex briefly,
    snapshots the `(conn, state)` pairs under lock, releases lock,
    then posts one lambda per subscriber onto that conn's `loop`
    (`queueInLoop`). The RBAC check and the `ConnState::channels`
    `contains` check run on the conn's loop — NOT on the listener
    thread — matching the existing `ws::publish` pattern exactly.
  - `pubsub::dispatch_to_js_subscribers` takes the kernel-side
    subscription registry's shared_mutex, snapshots the
    `(bc, channel, callback_id)` triples, releases lock, then
    dispatches via each bc's async-bridge arm (`bc.invoke_callback`
    on the runtime's owning loop).
- **No dedicated broker thread.** The listener thread's work per
  NOTIFY is bounded (O(subscribers) mutex-held time for snapshot,
  then O(1) per `queueInLoop`). If profiling ever shows listener
  stalls, a single-slot broker event loop thread can be inserted
  transparently between listener and publish without changing the
  contract (§OQ1 path).

### Frame shape on the wire

Per envelope the broker delivers, the client receives:

```json
{
  "type": "event",
  "channel": "<logical channel string, verbatim from envelope.channel>",
  "payload": { <full envelope, inline> }
}
```

`payload` is the WHOLE envelope (preserves `layer`, `schema`, `table`,
`ops`, `seq`, `truncated`, `window_ms`, `emitted_at`, and any future
top-level fields the producer adds). This unifies Layer 1 / 2 / 3
wire-contracts for clients: every `event` frame carries the same
top-level `{type, channel, payload}` shape and parsers walk into
`payload` for the layer-specific fields.

Rationale for shipping whole envelope vs. unwrapping: (a) the 0.5.5
`seq` field and the 0.5.4 `emitted_at` field must be reachable by
clients without widening the frame shape later; (b) Layer 1
coalescer envelopes have no natural "payload" in ICD-0.5.1 sense
(they're counts, not user data) — forwarding the envelope verbatim
sidesteps "what does payload mean for Layer 1" synthesis work;
(c) ICD-0.1.6 §Event Delivery already says `payload: { ... }` with
an open shape — the 0.5.0 envelope is exactly that "open shape."
See §OQ6 for the ratifying pin.

**Deviation from the stale ICD-0.1.6 channel naming example.**
ICD-0.1.6 §Subscribe / Unsubscribe line 98 shows
`"db:ext_notes:notes"` (old naming). The 0.5.0 Channel Naming Scheme
supersedes this — broker matches `"plinth:data:ext_notes.notes"`
(new canonical form). 0.1.6 RE-EVAL pass at 0.5.5 should retrofit
the 0.1.6 example to the canonical form; 0.5.2 treats the 0.5.0
scheme as authoritative.

---

## Subscription Matching

### Exact-string match

The broker matches `ConnState::channels.contains(envelope.channel)`
for WS subscribers and the equivalent `bc_subscriptions` set-contains
check for kernel-JS subscribers. No wildcards, no prefix match, no
regex — exact string equality on the envelope's `channel` field.

### `ConnState::channels` reuse (no new registry for WS)

WS subscriptions live where they always have — in
`ConnState::channels` (`src/kernel/ws/conn_state.hpp:20`). 0.5.2 does
NOT introduce a parallel per-connection registry. The only
0.5.2-scope change to `on_subscribe` / `on_unsubscribe` is the
per-channel RBAC check before the `channels.insert` (see
§Subscription RBAC).

### JS-side subscription registry (new)

`pubsub.subscribe` stores its subscriptions in a kernel-side
registry keyed by `BridgeContext*` → `map<channel, callback_id>`.
This is a separate structure from `ConnState::channels` because:

- **Different identity model** — `BridgeContext` lives per-extension-runtime
  (per `RuntimePool` slot per ICD-0.5.0.3), not per-WS-connection.
  A single WS connection may have zero or many active
  `BridgeContext`s depending on extension activity; a single
  `BridgeContext` may have zero associated WS connections.
- **Different lifetime** — `ConnState` dies on WS close;
  `BridgeContext` dies on runtime-pool slot eviction (per ICD-0.5.0.3
  or on extension DISABLED / UPGRADED / UNINSTALLED). The broker
  walks the JS registry during dispatch AND during extension-lifecycle
  drain.
- **Different reachability** — WS subscribers get `send()` frames;
  JS subscribers get `bc.invoke_callback(callback_id, envelope)`
  posted to the runtime's owning loop.

Both registries are read by the single `broker_event_handler`
invocation per envelope; the handler does two parallel fan-outs.

### Frame delivery path (WS)

Per matching `(conn, state)`:

```
listener thread:
    ws::publish_dispatched(event)
        shared_lock on registry
        for (c, s) in for_each_snapshot:
            capture c, s, event into lambda
            s->loop->queueInLoop(lambda)
        release lock
conn's loop:
    lambda runs
        if (!s->authenticated) return
        if (!s->channels.contains(event.channel)) return
        if (!broker_rbac::allow(s->auth, event.channel)) return   ← NEW
        c->send(build_event_frame(event))
```

The RBAC re-check on delivery is **defense in depth**. The primary
RBAC gate is at `on_subscribe` time — a denied channel never enters
`ConnState::channels`. The re-check guards the case where a user's
group membership is revoked between subscribe and emit; §Security
Constraints item 5 pins this.

---

## Subscription RBAC

### Layer-derived rule naming

The broker derives the required RBAC rule token from the channel
string itself, using the existing `channel_layer` + `channel_extension`
helpers from `src/kernel/realtime/channel.hpp`:

| Layer | Channel example | Derived rule | Notes |
|-------|----------------|--------------|-------|
| 1 (`data`) | `plinth:data:ext_notes.notes` | `notes.realtime.subscribe` | `<extension>` = `schema` with `ext_` prefix stripped |
| 1 (`data`) | `plinth:data:plinth.users` | `kernel.realtime.subscribe.plinth.users` | kernel-schema Layer 1 — admin-gated by default (no `<extension>` strip possible) |
| 2 (`system`) | `plinth:system:packages.installed` | `kernel.realtime.subscribe.packages.installed` | admin-only surface by default (Layer 2 is kernel-only producer per ICD-0.5.0 §Channel Naming Scheme) |
| 3 (`ext`) | `plinth:ext:notes:chat_typing` | `notes.realtime.subscribe.chat_typing` | `<extension>` = first `:`-segment after `plinth:ext:` |

Extensions declare the Layer-1 + Layer-3 rules in their `rbac.json`
exactly like any other RBAC rule per ICD-0.4.6. A typical `notes`
extension manifest declares:

```json
{
  "rules": [
    "notes.realtime.subscribe",
    "notes.realtime.subscribe.chat_typing",
    "notes.realtime.subscribe.post_edited"
  ]
}
```

Group grants work the standard additive-union way per
ICD-0.1.5-rbac-enforcement (verbatim; no change).

### Admin bypass

`is_admin == true` grants all channels across all three layers,
preserving ICD-0.1.6 §Security Constraints item 2 verbatim. An admin
subscribing to `plinth:ext:notes:chat_typing` gets the subscription
even if `notes` is not installed (the subscription just never
matches any envelope until `notes` starts publishing).

### Silent-omission on denial

`on_subscribe` filters denied channels out of the `subscribed[]`
ack, matching the 0.1.6 contract verbatim. No explicit
`denied:[...]` field, no per-channel error frame, no audit on the
common case. A rate-limited `realtime.broker.subscribe_denied` audit
fires when the denial rate exceeds a configurable threshold (see
§Audit Events).

### Capability-registry integration

These rules ride the standard ICD-0.4.6 rule registration path —
kernel treats `<extension>.realtime.subscribe.*` like any other
extension-declared rule; no special subsystem, no new dispatcher,
no migration. An extension that forgets to declare the rule simply
cannot subscribe to its own Layer-3 channels (the broker denies
all groups except admin).

### `kernel.*` Layer 2 rule naming

Layer 2 channels (`plinth:system:packages.installed`,
`plinth:system:auth.session.expired`, etc.) derive rules under the
`kernel.realtime.subscribe.<event_class>` namespace. These are
kernel-owned rules, declared at kernel-schema bootstrap per
ICD-0.1.5 (NOT per-extension). The default grant set is `admin` only
— no group receives Layer 2 subscribe rules by default. A 0.6a-A or
0.6a-E admin extension panel (future work) can UI-grant specific
`kernel.realtime.subscribe.*` rules to groups as needed.

---

## `pubsub.subscribe` JS Binding

### Surface

```javascript
// Subscribe to a realtime channel; returns an unsubscribe function.
// The handler is invoked with the full envelope for every emit that
// matches the channel. Resolves with the unsubscribe token on
// successful registration; rejects with a pubsub.* code on
// validation or RBAC failure.

const unsub = await pubsub.subscribe(
    "plinth:ext:notes:chat_typing",
    (envelope) => {
        // envelope: { layer:"extension", channel, payload, ... }
        console.log("chat_typing:", envelope.payload);
    });

// Later:
unsub();
```

### Binding implementation

Fires through the async bridge (ICD-0.3.3 pattern) — `pubsub.subscribe`
is an async op of type `PUBSUB_SUBSCRIBE` (new enum variant added to
`AsyncOp::Type`, joining `PUBSUB_PUBLISH`). Flow:

1. JS `pubsub.subscribe(channel, handler)` → C binding.
2. Binding validates:
   - `channel` is a string; matches the ICD-0.5.0 channel regex via
     `validate_channel`.
   - `handler` is a JS callable.
   - Subscription-count quota: current `bc_subscription_count(bc)` <
     `max_subscriptions_per_conn` (or its per-bc equivalent, see
     §Config Surface). Reject with `pubsub.quota_exceeded` on overflow.
   - Layer gating:
     - Layer 1 from JS — allowed; RBAC-checked via
       `notes.realtime.subscribe` derived rule.
     - Layer 2 from JS — **rejected** with `pubsub.layer_unsupported`
       (Layer 2 is kernel-only on the receive side in 0.5.2; future
       admin-extension may relax).
     - Layer 3 from JS — allowed; RBAC-checked via
       `<extension>.realtime.subscribe.<event_class>` derived rule;
       extension-identity gate (the `<extension>` segment must equal
       `bc.extension_name` — mirror of `pubsub.publish`'s gate at
       ICD-0.5.0 §`pubsub.*` JS Stdlib §Binding implementation step 2).
   - Cancellation: `bc.cancelled` → reject with `pubsub.cancelled`.
3. `bc.register_pending(resolve, reject)` enqueues an `AsyncOp` with
   `type = PUBSUB_SUBSCRIBE`, `pubsub_channel = channel`,
   `bc_extension_name = bc.extension_name`, `callback_id = <handler's
   id>`.
4. The async dispatcher records the subscription in the broker's
   kernel-side registry under `bc → {channel → callback_id}`.
5. Resolves with a JS-side unsubscribe function that, when invoked,
   enqueues a `PUBSUB_UNSUBSCRIBE` op removing the registration.

### Handler invocation on dispatch

When the broker handler matches a JS subscriber:

```
broker thread (listener thread):
    pubsub::dispatch_to_js_subscribers(event)
        shared_lock on bc_registry
        for each (bc, callback_id) subscribed to event.channel:
            capture bc, callback_id, event into a post task
            bc's loop->queueInLoop(post_task)
        release lock
bc's loop:
    post_task runs
        if (bc.cancelled) return
        bc.invoke_callback(callback_id, js_value_from(event))
```

The handler receives the full envelope (same shape as the WS frame's
`payload` field — so the kernel/WS/JS-side view of the envelope is
identical). JS-side conversion uses the existing `json_to_js` helper
(inverse of `js_to_json`, already shipped per ICD-0.3.3.3).

### Rejection codes

| Code | Trigger |
|------|---------|
| `pubsub.channel_invalid` | channel fails the regex |
| `pubsub.extension_mismatch` | Layer-3 channel's `<extension>` segment ≠ `bc.extension_name` |
| `pubsub.layer_unsupported` | Layer 2 subscribe from JS |
| `pubsub.rbac_denied` | RBAC check on derived rule fails for the bc's user |
| `pubsub.quota_exceeded` | `bc_subscription_count(bc)` ≥ quota |
| `pubsub.cancelled` | `bc.cancelled` during enqueue |

`pubsub.extension_mismatch`, `pubsub.channel_invalid`,
`pubsub.quota_exceeded`, `pubsub.cancelled` fire synchronously from
the binding before async dispatch. `pubsub.rbac_denied` and
`pubsub.layer_unsupported` fire from the async arm (the RBAC resolver
is not on the JS thread).

### Per-`bc` lifetime + eviction

The broker's kernel-side subscription registry holds `bc →
{channel → callback_id}` entries. Eviction happens at:

- `PUBSUB_UNSUBSCRIBE` dispatch (caller-invoked).
- `bc` teardown (the bc's eviction hook calls
  `broker::drop_bc_subscriptions(bc)`).
- Extension DISABLED / UPGRADING / UNINSTALL drain (§Extension
  Lifecycle Integration).
- `stop_broker()` clears all JS subscriptions.

---

## Extension Lifecycle Integration

### Drain hook

`plinth::realtime::broker::drain_extension(pkg_name)` is called from
three sites in `src/kernel/packages/install_lifecycle.cpp`:

- DISABLED transition.
- UPGRADING transition (before schema-swap).
- UNINSTALL transition (before schema-drop).

Mirrors the coalescer's three call sites per ICD-0.5.1
§Extension-lifecycle integration. The broker-side drain MUST run
alongside the coalescer-side drain (no ordering requirement between
the two — they touch disjoint state — but both must run before the
schema-drop half of UNINSTALL).

### What drain does

For each matching subscription (WS-side AND JS-side):

- Channels matching `plinth:data:ext_<pkg_name>.*` — the extension's
  schema prefix.
- Channels matching `plinth:ext:<pkg_name>:*` — the extension's
  Layer-3 namespace.

Actions:

- **WS-side:** `ConnState::channels.erase(channel)` via a
  `queueInLoop` onto the conn's loop. No close frame; no
  notification to the client. On the next emit the client just stops
  receiving that channel's frames. A rate-limited
  `realtime.broker.extension_drained` audit fires once per drain call
  with the matched-subscription count.
- **JS-side:** `broker::bc_registry.erase` for every `bc` with a
  matching subscription; the bc's callbacks are orphaned (not
  explicitly invoked; standard bc-teardown path clears them).

### Reconnect posture

Drained subscriptions are NOT restored on extension re-enable. The
client must re-subscribe. This keeps drain cheap + simple — the
broker does not track "was subscribed before drain" state. 0.5.4
may relax this via `plinth.events` replay + sequence cursors; 0.5.2
does not.

---

## Reconnect Semantics

**Stateless per-connection.** Clients reconnecting after a WS drop
must re-issue `{type:"subscribe", channels:[...]}` frames. The
broker carries no per-user durable subscription state in 0.5.2.

**Rationale:**

- ICD-0.1.6 §Delta Sync on Reconnect already defers durable reconnect
  semantics to 0.5.x — this ICD narrows that to 0.5.4 where
  `plinth.events` persistence pays the durability cost anyway.
- Tracking per-user durable subscriptions in-memory would double
  every subscription (conn-state + user-state) with no client-
  visible benefit until 0.5.4 ships the replay path.
- The per-connection model matches how the client SDK already
  behaves (subscribe on connect, resubscribe on reconnect; trivial
  10-line pattern). 0.6.3 SDK codifies it.

**Hand-off to 0.5.4.** When 0.5.4 lands, durable subscriptions
attach to `plinth.events` via a per-user cursor; the broker's 0.5.2
per-connection registry stays as the hot path + adds a cursor-
backed replay path on reconnect. No 0.5.2 contract breaks.

**Discharged 2026-04-25 (v0.5.4):** Hand-off implemented per
`docs/icd/ICD-0.5.4-events-table-delta-sync.md`. The 0.5.2 broker
registry stays the live hot path; the 0.5.4 replay engine
(`src/kernel/realtime/replay.{hpp,cpp}`) attaches at the WS
subscribe handler when the frame carries `since_seq`. No 0.5.2
contract was broken by the 0.5.4 ship.

---

## Config Surface

Extends the 0.5.0 + 0.5.1 `realtime.*` JSON block with a new
`broker` sub-block:

```json
{
  "realtime": {
    "listener": { "enabled": true, "reconnect_backoff_ms": 1000 },
    "notify":   { "max_payload_bytes": 8000 },
    "coalescer": { "enabled": true, "window_ms": 50 },
    "broker": {
      "enabled": true,
      "max_subscriptions_per_conn": 64,
      "rbac_enforce": true
    }
  }
}
```

| Key | Default | Semantics |
|-----|---------|-----------|
| `realtime.broker.enabled` | `true` | When `false`, `start_broker` no-ops. Useful for test deployments where no broker is needed (e.g. a node serving only HTTP with no WS clients). Per-node setting; does not affect cross-node NOTIFY propagation. |
| `realtime.broker.max_subscriptions_per_conn` | `64` | Per-connection subscribe quota. Also applies to per-`bc` JS subscriptions. Bounds memory + prevents runaway from malicious clients. 1–4096 clamped at config load; zero / negative rejected. |
| `realtime.broker.rbac_enforce` | `true` | When `false`, the broker degrades to admin-only subscribe (ICD-0.1.6 posture). Exists as a deployment-ramp escape hatch — flipping off restores 0.1.6 semantics cleanly while the operator investigates an RBAC misconfiguration. Flip back to `true` after root-cause. MUST be `true` in production per §Security Constraints item 4. |

**Deferred to later milestones:**

- `realtime.broker.durable_subscriptions` — 0.5.4.
- `realtime.broker.wildcard_match` — 0.5.4 / 0.5.5.
- `realtime.persistence.retention_seconds` — 0.5.4.

All additive when they land; 0.5.2's config shape is
forward-compatible.

---

## Audit Events

Three new event kinds, all routed through `plinth::log::audit` per
ICD-0.1.7. `g_audit_ready` gating applies — audits before the audit
writer is ready are dropped (matches 0.5.0 + 0.5.1 posture).

| Event | Trigger | Fields |
|-------|---------|--------|
| `realtime.broker.subscribe_denied` | `on_subscribe` or `pubsub.subscribe` RBAC check denies a channel. **Rate-limited** — one audit per (user_id, channel) per minute; repeat denials within the window are counted but not audited. | `user_id`, `channel`, `layer`, `reason` (e.g. `"rbac_denied"`, `"layer_unsupported"`, `"extension_mismatch"`), `denied_in_window` (count within the minute), `source` (`"ws"` or `"js"`) |
| `realtime.broker.extension_drained` | `drain_extension` removes at least one subscription. One audit per call site, NOT per-subscription. | `extension_name`, `ws_subscriptions_removed`, `js_subscriptions_removed`, `trigger` (`"disabled"` / `"upgrading"` / `"uninstall"`) |
| `realtime.broker.dispatch_skipped` | Broker handler enters but aborts early (not-enabled, envelope-validation post-condition fail). **Rate-limited** — one per minute at most; the field `skipped_in_window` reports the count. | `channel`, `reason`, `skipped_in_window` |

**No per-envelope audit on the happy path.** Dispatch is a firehose
(up to 20 envelopes per second per coalescer bucket × N buckets); per-
envelope audits would flood the audit pipeline and add latency to
the listener thread. The `dispatch_count` metric counter is the
hot-path observability surface; audits cover the exception paths.

**No per-frame-send audit.** Each `ws::publish_dispatched` frame is
in-memory-only from the broker's view; audit coverage of delivery is
a future 0.7.x metrics concern. 0.5.2 ships the dispatch_count /
rbac_denial_count / subscriber_count counters.

---

## HA Semantics

Verbatim promotion of ICD-0.5.0 §HA Semantics:

1. **All N kernel nodes LISTEN on `plinth:realtime`.** (Inherited
   unchanged from 0.5.0.)
2. **A NOTIFY emitted by any node reaches all nodes.** (Inherited.)
3. **Fan-out to clients is per-node local.** The broker on Node A
   fans out to clients connected to Node A only. Node B's broker
   fans out to clients connected to Node B. A client connected to
   one node receives events regardless of which node emitted, but
   the broker never crosses node boundaries to deliver.
4. **No ordering guarantees across channels.** 0.5.5 sequence
   numbers close this gap.
5. **Split-brain behavior.** (Inherited; PG operational concern.)

**0.5.2 addition:** broker dispatch counts are per-node — aggregated
metrics across nodes are a 0.7.1 `plinth.metrics` concern, not a
broker concern. Each node's `dispatch_count` getter returns its own
count.

---

## Deterministic Teardown

Per `feedback_deterministic_teardown.md`, every new subsystem with
framework-owned thread/callback state needs a `cancel_all_*` entry in
the `std::atexit` chain before `drogon::app().quit()` runs.

### Atexit chain (updated from 0.5.1)

```cpp
// BEFORE (0.5.1+)
std::atexit([] {
    plinth::packages::asset_server::cancel_all_registrations();
    plinth::ws::ConnectionRegistry::instance().cancel_all_timers();
    plinth::ws::ConnectionRegistry::initiate_shutdown();
    plinth::realtime::stop_listener();
    plinth::realtime::CoalescerRegistry::instance().shutdown();
    plinth::extensions::shutdown_registry();
    plinth::ws::shutdown_js_stress_pool();
    plinth::capabilities::stop_notify_listener();
    drogon::app().quit();
    plinth::log::shutdown();
});

// AFTER (0.5.2)
std::atexit([] {
    plinth::packages::asset_server::cancel_all_registrations();
    plinth::ws::ConnectionRegistry::instance().cancel_all_timers();
    plinth::ws::ConnectionRegistry::initiate_shutdown();
    plinth::realtime::stop_listener();
    plinth::realtime::stop_broker();                             // ← NEW
    plinth::realtime::CoalescerRegistry::instance().shutdown();
    plinth::extensions::shutdown_registry();
    plinth::ws::shutdown_js_stress_pool();
    plinth::capabilities::stop_notify_listener();
    drogon::app().quit();
    plinth::log::shutdown();
});
```

### Ordering rationale

- `stop_broker()` runs **after** `stop_listener()` so no new
  `DispatchedEvent` can race a partially-cleared broker registry.
  The listener's `stop_listener()` is a synchronous join — on
  return, the listener thread is gone, so no more handler
  invocations can fire.
- `stop_broker()` runs **before**
  `CoalescerRegistry::instance().shutdown()` because the coalescer
  may emit one last set of envelopes during its drain. Those
  envelopes go to PG NOTIFY (not to the broker directly), so the
  broker on the same node actually does NOT receive them (the
  listener is already stopped). The ordering is defensive: any
  future test-seam path that synchronously dispatches through the
  broker must find the broker already stopped.
- `stop_broker()` runs **after** `ConnectionRegistry::initiate_shutdown`
  because WS subscribers are going away anyway; the broker drops
  them (ConnectionRegistry's teardown cancels per-conn state first).

### What `stop_broker` does

- Flips the `broker_enabled` atomic bool to `false` (release fence).
- Clears the kernel-side JS subscription registry
  (`bc_registry.clear()` under unique_lock).
- Does NOT deregister the `EventHandler` from the listener (the
  listener's 0.5.0 contract forbids deregistration — handlers are
  idempotent add). The `broker_enabled` short-circuit at the top of
  the handler is the deregistration-equivalent.
- Audits `realtime.broker.stopped` with the final metric counts.

### Mirror in `ws_test_fixture.cpp`

The atexit chain mirror in `tests/kernel/ws/ws_test_fixture.cpp` MUST
include `realtime::stop_broker()` at the same slot. This matches the
0.5.1 pattern.

---

## Error Model

### Broker-side errors (C++)

| Error | Shape | Origin |
|-------|-------|--------|
| `BrokerError::NotStarted` | enum | `broker_dispatch_for_test` called before `start_broker` |
| `BrokerError::Disabled` | enum | `start_broker` called with `broker_cfg.enabled == false` |
| `BrokerError::QuotaExceeded` | enum | per-`bc` subscription add would exceed `max_subscriptions_per_conn` |

Broker-side errors are internal; they don't surface to clients. The
WS / JS-binding layers convert them to wire-visible codes below.

### WS-side subscribe errors

The 0.1.6 ack shape is preserved — denied channels are silently
omitted from the `subscribed[]` list. No new WS-side error codes.

The audit event `realtime.broker.subscribe_denied` is the only
denial surface (rate-limited, per §Audit Events).

### JS `pubsub.subscribe` rejection codes

| Code | Meaning |
|------|---------|
| `pubsub.channel_invalid` | channel fails the regex |
| `pubsub.extension_mismatch` | Layer-3 channel's `<extension>` ≠ `bc.extension_name` |
| `pubsub.layer_unsupported` | Layer 2 subscribe from JS |
| `pubsub.rbac_denied` | RBAC check on the Layer-derived rule failed |
| `pubsub.quota_exceeded` | subscription count at quota |
| `pubsub.cancelled` | `bc.cancelled` during enqueue |

All prefixed `pubsub.*` to match the 0.5.0 `pubsub.publish`
convention — cross-binding code-catalog unification.

### Configuration load failures

| Condition | Load-time behavior |
|-----------|---------------------|
| `realtime.broker.max_subscriptions_per_conn` ≤ 0 or > 4096 | hard reject with `std::runtime_error` (kernel-convention for out-of-range realtime knobs, matches 0.5.0 D2) |
| `realtime.broker.rbac_enforce` absent | defaults to `true` |
| `realtime.broker.enabled` absent | defaults to `true` |
| Additional unknown keys under `realtime.broker.*` | warn-log + ignore (matches 0.5.0 + 0.5.1 convention; forward-compat) |

---

## Security Constraints

1. **RBAC enforced pre-record.** `on_subscribe` and `pubsub.subscribe`
   MUST run the per-channel RBAC check before inserting into
   `ConnState::channels` / `bc_registry`. A denied channel never
   enters either registry. Exception: admin bypass retained per
   ICD-0.1.6.
2. **RBAC re-checked on delivery.** `ws::publish_dispatched` re-runs
   the RBAC check per `(conn, channel)` match before sending the
   frame. Defense in depth against revoked-grants-between-subscribe-
   and-emit. Equivalent re-check runs in the JS dispatch arm.
3. **Layer-mismatch reject.** A subscribe request for a channel with
   a layer prefix the binding does not support (Layer 2 from JS)
   rejects immediately — does not fall through to RBAC as a denial.
4. **`rbac_enforce = false` forbidden in production.** The
   deployment-ramp escape hatch (§Config Surface) MUST be `true` in
   production. Operators flipping to `false` take on a documented
   regression to ICD-0.1.6 admin-only posture for the duration.
   Config-load emits a warn-log when `rbac_enforce == false`.
5. **No cross-extension Layer-1 subscribe via schema-prefix forge.**
   The Layer-1 rule derivation uses schema with `ext_` stripped. A
   user in group `terminal-users` holding `terminal.realtime.subscribe`
   cannot subscribe to `plinth:data:ext_notes.notes` — the derived
   rule is `notes.realtime.subscribe`, unrelated to
   `terminal.realtime.subscribe`. The regex enforces the `ext_`
   prefix structure (ICD-0.5.0 §Channel Naming Scheme).
6. **No cross-extension Layer-3 publish via subscribe.**
   `pubsub.subscribe` does NOT grant publish access — only subscribe.
   An extension can subscribe to its own Layer-3 channels and to
   other extensions' Layer-3 channels (RBAC permitting via the
   `<other>.realtime.subscribe.<event_class>` rule) but cannot
   emit into another extension's channel (blocked by
   `pubsub.publish`'s extension-identity gate from 0.5.0 — unchanged).
7. **No envelope rewriting.** The broker forwards the envelope
   verbatim. Clients receive exactly what the producer emitted
   (modulo the `{type:"event", channel, payload}` wrapper). The
   broker does NOT inject, rewrite, filter, or augment any envelope
   fields. Forward-compat invariant — future envelope fields flow
   through untouched.
8. **Kernel-side subscription registry not extension-reachable.**
   An extension JS program cannot enumerate other extensions'
   subscriptions (no `pubsub.list_subscribers` binding, no
   kernel capability exposed). The metric getters
   (`js_subscriber_count`, `ws_subscriber_count`) are C++-only
   until 0.7.1 admin-extension wire-up.

---

## Test Cases

Test prefix: **B.\*** for broker subsystem (state machine, lifecycle,
metrics); **S.\*** for subscribe RBAC (wrapping the 0.1.6 admin-only
semantics with the new per-channel layer); **U.\*** for
`pubsub.subscribe` JS binding; **I.\*** for end-to-end integration
(producer → listener → broker → WS frame or JS handler). Tag
convention `[realtime][broker]` + per-group subtype (`[unit]`,
`[rbac]`, `[js]`, `[integration]`). Distinct from 0.5.0's R/E/P and
0.5.1's C/T/I/E prefixes — B / S / U land cleanly.

Total: **45 new cases** (14 B + 12 S + 12 U + 7 I). Distributed
across four test TUs.

### Broker subsystem — `tests/kernel/realtime/broker_test.cpp`

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| B.01 | `start_broker` idempotent | Call twice; inspect metrics | No | No duplicate handler registration (listener's internal handler vector has size +1 only once); second call no-op |
| B.02 | `start_broker` enabled=false | `broker_cfg.enabled=false` | No | Handler not registered; `dispatch_count()` stays 0 after simulated dispatch |
| B.03 | `stop_broker` before start | Call `stop_broker()` with broker never started | No | No crash; no error; no audit (broker_enabled was already false) |
| B.04 | Dispatch with zero subscribers | `broker_dispatch_for_test(ev)` with empty registries | No | Returns 0 matches; `dispatch_count += 1`; no audit |
| B.05 | Dispatch one WS subscriber, match | Pre-seed conn with `channels = {"plinth:data:ext_x.t"}`, admin auth; dispatch matching ev | No | Conn's loop receives one frame; returns 1 |
| B.06 | Dispatch one WS subscriber, no match | Same seed but ev.channel = `"plinth:data:ext_y.t"` | No | No frame; returns 0 |
| B.07 | Dispatch N WS subscribers on M loops | Seed 8 conns across 3 trantor event loops, all subscribed | No | Each loop receives correct frame count; no cross-loop race |
| B.08 | Dispatch one JS subscriber, match | `pubsub.subscribe` an ext_x channel, dispatch matching ev | No | `bc.invoke_callback` called once; returns 1 |
| B.09 | Dispatch 1 WS + 1 JS subscriber | Both subscribe same channel; dispatch | No | Both receive; returns 2 |
| B.10 | `broker_dispatch_for_test` from listener thread | Simulate listener-thread call | No | Dispatch completes without blocking listener > 1 ms (mutex-held time budget) |
| B.11 | Metrics counters monotonic | Dispatch 5 times; read `dispatch_count` | No | Returns ≥5 (monotonic); `reset_metrics_for_test` returns counts to 0 |
| B.12 | `stop_broker` clears JS registry | `pubsub.subscribe` 3 channels; `stop_broker`; check `js_subscriber_count` | No | Returns 0 post-stop |
| B.13 | Handler short-circuit post-stop | `stop_broker`; simulated dispatch via registered handler | No | Handler body early-returns on `broker_enabled=false`; no audit; no counter increment |
| B.14 | `broker_event_handler` robust to bad envelope | Dispatch envelope with missing `layer` field | No | Returns 0; no crash; no counter increment; `dispatch_skipped` audit fires (rate-limited) |

### Subscribe RBAC — `tests/kernel/ws/subscriptions_rbac_test.cpp` (new TU)

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| S.01 | Admin all-channels | Admin user subscribes to Layer 1 + 2 + 3 channels | No | All three in `subscribed[]` ack; ConnState.channels contains all three |
| S.02 | Non-admin Layer 1 grant | User in group with `notes.realtime.subscribe`; subscribes `plinth:data:ext_notes.notes` | No | Channel in `subscribed[]`; ConnState.channels contains it |
| S.03 | Non-admin Layer 1 deny | Same user; subscribes `plinth:data:ext_terminal.sessions` | No | Omitted from `subscribed[]`; ConnState.channels absent; `subscribe_denied` audit fires (first in window) |
| S.04 | Non-admin Layer 3 grant | User has `notes.realtime.subscribe.chat_typing`; subscribes `plinth:ext:notes:chat_typing` | No | In ack; in channels |
| S.05 | Non-admin Layer 3 deny | Same user; subscribes `plinth:ext:terminal:typing` | No | Omitted; `subscribe_denied` audit |
| S.06 | Non-admin Layer 2 deny | Non-admin user subscribes `plinth:system:packages.installed` | No | Omitted; `subscribe_denied` with reason=`rbac_denied` |
| S.07 | Mixed batch | User with `notes.realtime.subscribe`; batch `[notes-L1-channel, terminal-L1-channel, notes-L3-chat_typing]`; the latter two lacking grants | No | Only the first in `subscribed[]`; three audits (or one aggregated depending on rate limiter — test asserts the aggregated form) |
| S.08 | Cross-extension Layer-1 attempt | `notes` user requesting `plinth:data:ext_terminal.foo` | No | Denied — `notes.realtime.subscribe` ≠ `terminal.realtime.subscribe` |
| S.09 | `rbac_enforce=false` | Start broker with `rbac_enforce=false`; non-admin subscribes Layer 1 | No | Denied (0.1.6 admin-only semantics) — ConnState.channels absent |
| S.10 | Subscription quota overflow | Set `max_subscriptions_per_conn=3`; admin subscribes 4 channels | No | First 3 in `subscribed[]`; 4th omitted; `subscribe_denied` with reason=`quota_exceeded` |
| S.11 | RBAC re-check on delivery | Subscribe channel; revoke grant (mutate user's groups in-memory); dispatch | No | Frame NOT delivered; `subscribe_denied` audit on delivery path (rate-limited with reason=`rbac_denied_on_deliver`) |
| S.12 | Drain-on-disable | `notes` ext disabled; dispatch `plinth:ext:notes:chat_typing` | No | No frame; ConnState.channels no longer contains the channel; `extension_drained` audit with `ws_subscriptions_removed=1` |

### `pubsub.subscribe` JS binding — `tests/kernel/js/pubsub_subscribe_test.cpp`

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| U.01 | Happy Layer 3 | Extension `notes` subscribes its own `plinth:ext:notes:chat_typing`; dispatch via broker | No | Handler invoked with envelope |
| U.02 | Extension mismatch | `notes` extension subscribes `plinth:ext:terminal:x` | No | Rejects `pubsub.extension_mismatch` |
| U.03 | Layer 1 with RBAC | `notes` extension with `notes.realtime.subscribe` subscribes `plinth:data:ext_notes.notes` | No | Accepts; dispatch invokes handler |
| U.04 | Layer 1 cross-ext deny | `notes` extension subscribes `plinth:data:ext_terminal.foo` | No | Rejects `pubsub.rbac_denied` (notes has no `terminal.realtime.subscribe` rule) |
| U.05 | Layer 2 from JS | Any extension subscribes `plinth:system:packages.installed` | No | Rejects `pubsub.layer_unsupported` |
| U.06 | Channel-invalid | Subscribe malformed channel `"not:a:channel"` | No | Rejects `pubsub.channel_invalid` |
| U.07 | Cancelled bc | `bc.cancelled=true`; subscribe call | No | Rejects `pubsub.cancelled` |
| U.08 | Quota exceeded | Subscribe `max_subscriptions_per_conn + 1` channels | No | Last rejects `pubsub.quota_exceeded` |
| U.09 | Unsubscribe | Subscribe + dispatch (handler fires); invoke unsubscribe token; dispatch again | No | First dispatch: handler fires. Post-unsub dispatch: handler NOT invoked |
| U.10 | bc teardown drops subs | Subscribe from bc; destroy bc; check `js_subscriber_count` | No | Count drops by 1 |
| U.11 | Extension UPGRADE drops subs | `notes` extension with 2 subs; UPGRADE transition | No | `extension_drained` audit with `js_subscriptions_removed=2`; bc_registry empty for that extension |
| U.12 | Multi-subscribe same channel | Subscribe same channel twice from same bc | No | Both handlers fire on dispatch (or: second subscribe reuses first — pinned by §OQ3 resolution) |

### End-to-end integration — `tests/kernel/realtime/broker_integration_test.cpp` (new TU)

All cases run live PG + Drogon DbClient + `realtime::start_listener` +
`realtime::start_broker` + a test WS fixture client.

| # | Type | Scenario | PG-gated | Expected outcome |
|---|------|----------|:--------:|------------------|
| I.01 | Coalescer → broker → WS | Admin WS client subscribes `plinth:data:ext_i01.notes`; ext runs `db.exec("INSERT INTO ext_i01.notes ...")` | Yes | Client receives one `{type:"event", channel, payload}` frame within `coalescer window_ms + 200 ms`; payload contains full envelope including `ops[0].count=1` |
| I.02 | Layer 3 pubsub.publish → broker → WS | Admin WS client subscribes `plinth:ext:i02:x`; ext runs `pubsub.publish("plinth:ext:i02:x", {hello:1})` | Yes | Client receives event frame with payload.layer="extension" + payload.payload={hello:1} |
| I.03 | Two clients, one channel | Two authenticated WS conns subscribe same Layer-1 channel; producer emits once | Yes | Both receive the frame |
| I.04 | One client, two channels | Client subscribes Layer 1 AND Layer 3; producers emit each | Yes | Client receives both frames, correct layer in each payload |
| I.05 | JS subscriber end-to-end | Extension JS `pubsub.subscribe` Layer 3; another extension `pubsub.publish` same | Yes | Subscriber's handler invoked with envelope |
| I.06 | Drain race | Client subscribed; extension UNINSTALL fires between NOTIFY and dispatch | Yes | No frame delivered (drain wins race via `broker_enabled` short-circuit OR pre-dispatch registry check); `extension_drained` audit |
| I.07 | rbac_enforce flip | Start with rbac_enforce=false; non-admin subscribe Layer 1; flip to true; new subscribe | Yes | Second subscribe denied (new post-config restart); document this is NOT a hot-reload — config change requires process restart per 0.5.0 convention |

### Test-seam notes

- B.\* cases (14) use `broker_dispatch_for_test` to bypass real
  listener on all but B.10 (which specifically exercises listener-
  thread call).
- S.\* cases (12) use the existing `ws_test_fixture.cpp` WS
  client-simulator pattern from 0.1.6 + the RBAC user-group mutation
  seam from 0.1.5. Admin-bypass validated in S.01; RBAC-re-check
  validated in S.11.
- U.\* cases (12) use the kernel-JS test fixture from 0.5.0 `pubsub`
  tests + a new `test_dispatch_envelope(bc, envelope)` seam on the
  broker for JS-path coverage without real NOTIFY.
- I.\* cases (7) end-to-end PG-gated; reuse `plinth_tests_pg`
  subprocess group.
- **No new CTest subprocess.** B/S/U pure cases run in
  `plinth_tests_pure` where possible (most are; the ones requiring
  `plinth::log::audit` pipeline-up need `plinth_tests_pg` — about
  half). I cases all in `plinth_tests_pg`.

### CI wiring

- `src/kernel/realtime/broker.{hpp,cpp}` — new.
- `src/kernel/ws/publish.{hpp,cpp}` — extended with
  `publish_dispatched(DispatchedEvent&)` overload + per-match
  RBAC re-check.
- `src/kernel/ws/subscriptions.cpp` — per-channel RBAC gate added
  to `on_subscribe` + `on_unsubscribe`.
- `src/kernel/js/async_op.hpp` — new `PUBSUB_SUBSCRIBE` +
  `PUBSUB_UNSUBSCRIBE` enum variants; `pubsub_channel` field already
  present (0.5.0).
- `src/kernel/js/stdlib/pubsub_bindings.cpp` — `pubsub.subscribe` +
  `pubsub.unsubscribe` bindings added; joins existing
  `pubsub.publish` in the same TU.
- `src/kernel/js/run_on_context.cpp` — new dispatch arm for
  `PUBSUB_SUBSCRIBE` + `PUBSUB_UNSUBSCRIBE`.
- `src/kernel/config.{hpp,cpp}` — `Config::Realtime::Broker`
  substruct + `apply_realtime` broker-block parser.
- `src/kernel/main.cpp` — startup `realtime::start_broker(...)` after
  `realtime::start_listener`; atexit `realtime::stop_broker()`
  insertion between `stop_listener` and `CoalescerRegistry::shutdown`.
- `src/kernel/packages/install_lifecycle.cpp` — three
  `broker::drain_extension(pkg.name)` call sites alongside the
  coalescer's.
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit mirror.
- `tests/kernel/realtime/broker_test.cpp` — new TU.
- `tests/kernel/ws/subscriptions_rbac_test.cpp` — new TU.
- `tests/kernel/js/pubsub_subscribe_test.cpp` — new TU.
- `tests/kernel/realtime/broker_integration_test.cpp` — new TU.
- `CMakeLists.txt` — no glob edit (kernel + tests globs already cover
  the new TUs).
- `migrations/schema.sql` — no edits. `plinth.events` is 0.5.4.

### Test count target

**45 new cases.** Full suite grows by 45 TEST_CASEs distributed across
`plinth_tests_pure` + `plinth_tests_pg`. No new subprocess count; no
new ctest entry.

---

## Entry / Exit

**Entry criteria:**

- v0.5.0 merged + tagged (done — commit `f3552b3`, tag `v0.5.0`).
- v0.5.1 merged + tagged (done — commit `90d37fc`, tag `v0.5.1`).
- 0.5.1.1 CI red regressions fix merged (done — commit `ef0190a`).
- RE-EVAL-0.5.x-following-0.5.1 merged (done — commit `d661396`,
  PR #76).
- ICD-0.5.2 authored (this document, squash-merged as 0.5.1.2 paper
  slot).
- LH-1 shipped (done — commit `7953eae`).
- `BridgeContext::extension_name` populated per-pool at install-
  lifecycle (done — v0.4.4 + v0.5.0.4).
- `ConnState::channels` + `on_subscribe` / `on_unsubscribe` shipped
  (done — v0.1.6).
- `ws::publish(channel, payload)` shipped (done — v0.1.6).
- `pubsub.publish` JS binding shipped (done — v0.5.0).

**Exit criteria:**

- `src/kernel/realtime/broker.{hpp,cpp}` ships.
- `src/kernel/ws/publish.{hpp,cpp}` `publish_dispatched` overload
  ships with per-match RBAC re-check.
- `src/kernel/ws/subscriptions.cpp` per-channel RBAC gate on
  `on_subscribe` + `on_unsubscribe`.
- `src/kernel/js/stdlib/pubsub_bindings.cpp` `pubsub.subscribe` +
  `pubsub.unsubscribe` bindings ship.
- `src/kernel/js/async_op.hpp` `PUBSUB_SUBSCRIBE` +
  `PUBSUB_UNSUBSCRIBE` enum variants.
- `src/kernel/js/run_on_context.cpp` dispatch arms for both.
- `Config::Realtime::Broker` substruct + config-loader support with
  bound-check validation.
- `main.cpp` + `ws_test_fixture.cpp` atexit chains include
  `realtime::stop_broker()`; startup includes `start_broker()`.
- `install_lifecycle.cpp` three transitions call
  `broker::drain_extension` (alongside coalescer's
  `drain_extension`).
- All 45 B/S/U/I test cases pass; PG-gated cases skip cleanly when
  PG env absent.
- `run-clang-tidy-20` zero findings on new TUs (`broker.cpp`) and
  modified TUs (`publish.cpp`, `subscriptions.cpp`,
  `pubsub_bindings.cpp`, `run_on_context.cpp`, `async_op.hpp`,
  `config.cpp`, `main.cpp`, `install_lifecycle.cpp`,
  `ws_test_fixture.cpp`).
- No regressions on the v0.5.0 + v0.5.1 + 0.5.0.4 test matrix.
- Atexit-race validation: 20-run ctest loop sample shows zero
  teardown-race reproductions.
- `CHANGELOG.md` `v0.5.2` entry describes the broker subsystem, the
  `publish_dispatched` overload, the per-channel RBAC gate, the
  `pubsub.subscribe` binding, the atexit chain extension, the
  config surface, the audit events, the three install-lifecycle
  drain hook sites, and every accepted OQ deviation.
- `docs/ROADMAP.md §0.5` line for 0.5.2 is removed.
- `v0.5.2` tag cut on the merge commit.
- Memory `project_plinth_state.md` updated to reflect 0.5.2 shipped;
  `project_next_session_post_051.md` retired (replaced by a pointer
  to the next paper session, likely `0.5.2.N ICD-0.5.3 authoring`).
- **LH-2 N-subscribers × event-flood tier unblocked** per ICD-LH-1
  §9 Future work (gating dependency satisfied).
- `docs/DEFERRED.md` P.01 pubsub entry not regressed (broker's RBAC
  layer composes cleanly over the existing `pubsub.publish` path).

---

## Open Questions

**OQ1 — Broker as its own subsystem vs. inline registration.** The
ICD pins **`plinth::realtime::broker` as its own subsystem module**
(`src/kernel/realtime/broker.{hpp,cpp}`) mirroring
`plinth::realtime::coalescer` from 0.5.1: `start_broker` /
`stop_broker` / `broker_dispatch_for_test` seam + metrics getters.
Alternative: broker as a free function / lambda in `main.cpp` closing
over `ws::publish_dispatched` and the kernel-side JS subscription
registry. **Recommendation:** own subsystem. Rationale: (a)
symmetric with coalescer's posture; (b) testability — `broker_test.cpp`
can exercise the broker without booting the full kernel; (c) natural
home for the metrics getters + `drain_extension` entry point.
Architect: confirm or redirect to inline.

**OQ2 — Subscribe RBAC model.** The ICD pins **Layer-derived rule
naming** (§Subscription RBAC). Alternatives:
(a) admin-only retained (punt RBAC to a later ICD entirely);
(b) explicit per-channel rules declared in extension `rbac.json` for
every channel the extension emits OR consumes (no derivation —
manifest lists exact rule tokens);
(c) the Layer-derivation pinned here.
**Recommendation:** (c). Rationale: (a) leaves the ICD-0.1.6 "deferred
to a later ICD" note undischarged and blocks every non-admin RBAC'd
Plinth deployment; (b) is more explicit but forces extension authors
to keep a per-channel rule list in sync with their emit sites — O(N)
per-channel manifest ceremony that the derivation model sidesteps
with zero loss of precision; (c) consistent with `pubsub.publish`'s
extension-identity gate (Layer 3 channel's `<extension>` segment must
equal `bc.extension_name`), deterministic, and operators can always
widen a rule downstream via additional groups. Architect: confirm or
redirect. If (c): pin the exact rule-naming token
(`<extension>.realtime.subscribe` vs.
`<extension>.realtime.subscribe.<event_class>` vs. some other spelling).

**OQ3 — `pubsub.subscribe` return shape.** The ICD pins
**callback + unsubscribe function** (`pubsub.subscribe(channel,
handler) → Promise<() => void>`). Alternatives:
(a) async iterator (`for await (const ev of
pubsub.subscribe(...))` — idiomatic for modern JS);
(b) return a subscription-id number; unsubscribe via
`pubsub.unsubscribe(id)` — matches some pubsub libraries;
(c) the callback + token pinned here.
**Recommendation:** (c). Rationale: (a) couples handler lifetime to a
JS for-loop's break/throw semantics — easy to leak subscriptions on
early-exit paths, hard to reason about under QuickJS's bridge; (b)
splits the API surface across two bindings and adds an
identifier-tracking concern to the kernel-side registry (callback_id
already serves this role internally); (c) mirrors DOM `addEventListener`
+ `AbortController` patterns already idiomatic in the wider JS
ecosystem, lets the per-`bc` registry use callback_id internally
without exposing it. Also allows same-channel multi-subscribe
trivially (two calls → two handler registrations → both fire). Cost:
callers must hold onto the unsubscribe token; a short lifecycle
callback hook (e.g. panel unmount in 0.6.3 SDK) is the natural site.
Architect: confirm or redirect.

**OQ4 — `pubsub.subscribe` registry scope.** The ICD pins **separate
per-`bc` subscription registry** in the broker, orthogonal to
`ConnState::channels`. Alternative: fold JS subscribes into the same
`ConnState::channels` set shared with WS. **Recommendation:** separate.
Rationale: ConnState is per-WS-connection; a JS-side `pubsub.subscribe`
from extension JS is per-`BridgeContext` (no WS connection — kernel-
side subscriber). Merging them would require synthesising a fake
ConnState for every `bc`, plus teaching `ws::publish_dispatched` to
distinguish "send a WS frame" from "invoke a JS callback." Cleaner
to keep two registries + one handler fan-out path. Architect: confirm.

**OQ5 — Layer 2 subscribe access from JS.** The ICD pins
**Layer 2 rejected from JS** with `pubsub.layer_unsupported`.
Alternatives:
(a) allow Layer 2 from JS with `kernel.realtime.subscribe.<event_class>`
RBAC rule (admin grants opt-in);
(b) rejected (pinned).
**Recommendation:** (b) for 0.5.2. Rationale: Layer 2 carries kernel-
internal events (`packages.installed`, `auth.session.expired`,
`users.deleted`); exposing to extension JS widens the attack surface
(an extension handler reacting to another user's session expiry is
a capability that should require explicit design). A 0.6a-\* admin
extension panel can relax later if real demand emerges. Architect:
confirm or widen.

**OQ6 — Frame shape on the wire: full envelope as `payload` vs.
unwrap.** The ICD pins **full envelope as `payload`**. Alternatives:
(a) unwrap to `{type:"event", channel, <envelope fields>...}` (flat);
(b) send `{type:"event", channel, payload: <envelope.payload>}`
discarding the wrapper fields;
(c) full envelope as `payload` pinned here.
**Recommendation:** (c). Rationale: (a) flattens with future field
conflicts (a Layer-3 user `payload` containing `{layer:"v2"}` would
collide); (b) loses Layer 1 `ops`, `window_ms`, `truncated` fields
(critical for 0.5.5 `seq` + coalescer-aware clients); (c) preserves
forward compat for `seq` / `emitted_at` / `truncated` and unifies
Layer 1/2/3 client parsing. Minor cost: ~20 extra bytes per frame
(envelope metadata overhead). Architect: confirm.

**OQ7 — Reconnect durability: stateless in 0.5.2 vs. user-scoped
durable.** The ICD pins **stateless per-connection**. Clients
resubscribe after reconnect. Alternative: broker tracks per-user
durable subscription sets indexed by `user_id`; re-applies on auth
success. **Recommendation:** stateless in 0.5.2. Rationale: durable
reconnect without `plinth.events` replay gives no client-visible
benefit (missed events between drop + reconnect are still missed);
0.5.4 brings both `plinth.events` AND the opportunity to add per-
user subscription cursors in one coherent change. Per-connection
model matches how the 0.1.6 client pattern already behaves + how
the 0.6.3 SDK will codify it. Architect: confirm.

**OQ8 — Per-connection subscription quota value.** The ICD pins
**64 channels per conn** (`realtime.broker.max_subscriptions_per_conn`
default). Also applies as per-`bc` quota for JS subscribers.
Alternatives: unbounded (trust clients); 16 (tight); 256 (loose).
**Recommendation:** 64. Rationale: typical extension's realtime
surface area is single-digit channels; 64 buys headroom for
aggregators (an admin panel listening on `kernel.*` events might hit
double digits); cap prevents memory-grief attack vectors; operators
can bump via config without recompile. Architect: confirm or set.

---

## Appendix: Resolved Open Questions (v0.5.2)

All eight OQs land at the ICD-recommended pin. No architect
redirects. Resolution captured here so future readers do not need
to cross-reference the CHANGELOG v0.5.2 §Why to confirm the contract.

| OQ | Pin | Rationale source |
|----|-----|------------------|
| OQ1 | **Broker as its own subsystem** (`plinth::realtime::broker` — module pair `broker.{hpp,cpp}`). | Recommendation in §Open Questions; symmetric with `CoalescerRegistry`; gives the drain hook + metrics a single home. |
| OQ2 | **Layer-derived rule naming.** Rule token computed from the channel itself (`<ext>.realtime.subscribe[.<event_class>]`, `kernel.realtime.subscribe.<event_class>`). Extension-authored `rbac.json` declares them verbatim. | Recommendation. Discharges ICD-0.1.6 "per-channel rule convention deferred" note; keeps naming deterministic + scope-preserving. |
| OQ3 | **Callback + unsubscribe-token return shape.** `pubsub.subscribe(channel, handler) → Promise<() => void>`. Unsubscribe token is a `JS_NewCFunctionData` closure enqueueing `PUBSUB_UNSUBSCRIBE`. | Recommendation. Mirrors `addEventListener` + `AbortController`; trivial multi-subscribe composition; SDKs already assume this shape. |
| OQ4 | **Separate per-`bc` registry** (`broker::g_bc_registry: bc → {channel → callback_id}`). Distinct from `ConnState::channels`. | Recommendation. `BridgeContext` ≠ WS connection (different identity + lifetime + reachability); co-locating would compromise both. |
| OQ5 | **Layer 2 rejected from JS** with `pubsub.layer_unsupported`. Future admin-extension may relax once `kernel.realtime.subscribe.*` rule-grant UI ships. | Recommendation. Layer 2 is kernel-producer-only; exposing from a JS extension widens the attack surface in 0.5.2 with no user-visible gain. |
| OQ6 | **Full envelope as `payload`** on the WS wire frame (`{type:"event", channel, payload: <envelope>}`). Unifies Layer-1/2/3 client parsing. | Recommendation. ~20 bytes/frame overhead in exchange for single-parser-path clients + forward-compat for `seq`/`emitted_at`/`truncated`. |
| OQ7 | **Stateless per-connection reconnect** for 0.5.2. Client re-issues `subscribe` after reconnect. Durable replay lands at 0.5.4 alongside `plinth.events` persistence. | Recommendation. No durability benefit without `plinth.events`; doubling subscription state (conn + user) pays no dividend until 0.5.4. |
| OQ8 | **Per-connection subscription quota default: 64.** Config-tunable via `realtime.broker.max_subscriptions_per_conn` (clamped 1–4096). | Recommendation. Typical extension is single-digit channels; 64 is the headroom sweet spot; bounded clamp prevents memory-grief. |

Appendix added at the v0.5.2 code cadence (2026-04-23), alongside
the `v0.5.2` tag cut. Matches the ICD-0.5.1 precedent set by the
post-v0.5.1 re-eval.

---

## Appendix A: End-to-End Example

Extension `ext_notes` installed on Node A of a three-node deployment.
A `notes` user subscribes via WebSocket from the browser and the
extension JS inserts a row:

```javascript
// 1. WS client connects + authenticates (per ICD-0.1.6).
// 2. WS client sends:
{
  "type": "subscribe",
  "channels": ["plinth:data:ext_notes.notes"]
}
// 3. Broker's on_subscribe:
//    - validate_channel passes.
//    - Layer-derived rule: "notes.realtime.subscribe".
//    - User's groups contain rule → granted.
//    - ConnState::channels.insert("plinth:data:ext_notes.notes").
// 4. Ack: {"type":"subscribed", "channels":["plinth:data:ext_notes.notes"]}.

// Meanwhile in ext_notes' JS:
await db.exec("INSERT INTO ext_notes.notes (id, title) VALUES ($1, $2)",
              ["n42", "Hello"]);
```

Kernel-side flow:

1. `db.exec` binding enqueues `AsyncOp{Type::DB_EXEC, sql, params,
   bc_extension_name="ext_notes", silent=false}` on `bc.pending_ops`.
2. `run_db_exec_outcome` hits the coalescer hook (ICD-0.5.1):
   `classify_sql` → `{"ext_notes", "notes", INSERT}`;
   `CoalescerRegistry::instance().record_write(...)` opens a window.
3. 50 ms later, coalescer's flush timer fires (ICD-0.5.1
   §Coalescer State Machine): `build_envelope` →
   `{layer:"data", channel:"plinth:data:ext_notes.notes", schema:"ext_notes",
   table:"notes", ops:[{insert:1},{update:0},{delete:0}], window_ms:50}`.
4. `emit_notify_async(db, env)` writes
   `SELECT pg_notify('plinth:realtime', <env>)` (ICD-0.5.0).
5. PG propagates NOTIFY to all three nodes' `plinth::realtime::listener`
   jthreads (each LISTENing on `plinth:realtime`).
6. On each node, `listener` dispatcher fires every registered
   `EventHandler`. Node A's broker handler runs (Node B + Node C's
   brokers also run, but find zero matching subscribers locally —
   the user is connected to Node A).
7. Node A's `broker_event_handler(ev)`:
   - `broker_enabled` check passes.
   - `ws::publish_dispatched(ev)`:
     - snapshot ConnectionRegistry.
     - for the user's conn: `ConnState::channels.contains("plinth:data:ext_notes.notes")` → true.
     - `queueInLoop(lambda)` onto the conn's `loop`.
   - `pubsub::dispatch_to_js_subscribers(ev)`:
     - kernel-side bc_registry has no subscribers for this channel → no-op.
   - metrics: `dispatch_count += 1`.
8. On the conn's event loop, the lambda runs:
   - `authenticated` = true.
   - `channels.contains(...)` = true.
   - `broker_rbac::allow(auth, "plinth:data:ext_notes.notes")` = true
     (re-check; defense in depth per §Security Constraints 2).
   - `conn->send(frame)` where `frame = {type:"event", channel:"plinth:data:ext_notes.notes", payload:<envelope>}`.
9. Browser WS receives:

```json
{
  "type": "event",
  "channel": "plinth:data:ext_notes.notes",
  "payload": {
    "layer": "data",
    "channel": "plinth:data:ext_notes.notes",
    "schema": "ext_notes",
    "table": "notes",
    "ops": [{"insert":1},{"update":0},{"delete":0}],
    "window_ms": 50
  }
}
```

Total end-to-end latency (coalescer window = 50 ms):
`50 ms + PG NOTIFY propagation (~1 ms) + listener dispatch (~0.1 ms)
+ WS queue (~0.1 ms) + frame send (~0.1 ms)` ≈ `50–55 ms` from
`db.exec` return to browser frame receipt at steady state.

---

## Appendix B: Config Example

Full `realtime` block with broker added (0.5.0 + 0.5.1 + 0.5.2
composed):

```json
{
  "realtime": {
    "listener": {
      "enabled": true,
      "reconnect_backoff_ms": 1000
    },
    "notify": {
      "max_payload_bytes": 8000
    },
    "coalescer": {
      "enabled": true,
      "window_ms": 50
    },
    "broker": {
      "enabled": true,
      "max_subscriptions_per_conn": 64,
      "rbac_enforce": true
    }
  }
}
```

Minimum effective config (all 0.5.2 broker knobs at defaults):

```json
{ "realtime": {} }
```

All four sub-blocks default-constructable; each sub-block's internal
keys also default per §Config Surface. Operators only override knobs
they need.


---

## Implementation deviation (LH-2 ship, 2026-04-24)

Two contract corrections landed inside the LH-2 harness PR, closing
gaps the 2026-04-23 v0.5.2 ship left between ICD prose and shipped
behavior. Both are code fixes — the ICD prose holds unchanged.

### §Security Constraint 6 — cross-extension subscribe honors
per-channel rule

v0.5.2 shipped `src/kernel/js/stdlib/pubsub_bindings.cpp:classify_pubsub_subscribe`
with an extension-identity-only gate: any Layer 3 channel whose
`<extension>` segment did not equal the caller's `bc.extension_name`
was rejected with `pubsub.extension_mismatch`, and any Layer 1 DATA
channel whose derived rule's namespace prefix did not match was
rejected with `pubsub.rbac_denied` — unconditionally, regardless of
whether the caller held the per-channel subscribe rule. That
contradicted §Security Constraint 6 ("An extension can subscribe
to... other extensions' Layer-3 channels (RBAC permitting via the
`<other>.realtime.subscribe.<event_class>` rule)") and blocked the
LH-2 §5.3 sidecar design.

LH-2 ship (2026-04-24) widens both layer arms to honor
`bc.user.effective_rules` for cross-extension subscribes, matching
the WS-side `subscribe_allowed` function at
`src/kernel/ws/subscriptions.cpp:68`. `broker::is_rbac_enforced() ==
false` degrades to `kernel.admin`-only fallback for cross-ext, also
mirroring WS-side behavior. Helper `has_rule_or_admin` bundles the
universal `kernel.admin` bypass with the per-rule check, matching
`resolution.cpp::check_permission` semantics. The gate is exported
via `stdlib_inject.hpp` so the `PUBSUB_SUBSCRIBE` dispatch arm in
`run_on_context.cpp` can re-run it for defense-in-depth (see §SC2
note below).

### §Security Constraint 2 — JS dispatch arm now re-checks

The JS `PUBSUB_SUBSCRIBE` dispatch arm at
`dispatch_pubsub_sub_inline` previously only checked the broker-side
quota before registering the subscription. A group revocation or
`rbac_enforce=false` flip between the classify gate and the dispatch
arm would therefore land an unauthorized subscription. LH-2 ship
adds a `classify_pubsub_subscribe` re-check at the top of
`dispatch_pubsub_sub_inline`; on denial the arm fires
`broker::note_dispatch_skipped(channel, "rbac_denied")`, rolls back
the `persistent_callbacks` entry, and rejects the promise — now
parallel to the quota-overflow arm. Satisfies §SC2's "RBAC
re-checked on delivery" invariant for the JS-side arm, mirroring
the WS-side `delivery_rbac_allows` re-check at `publish.cpp:56`.

### `RuntimePool::destroy` BC-teardown leak (drive-by)

Not strictly a §SC deviation but surfaced during LH-2 test
authoring: the `RuntimePool::destroy` path at
`src/kernel/js/runtime_pool.cpp:700` was missing the
`broker::drop_bc_subscriptions` + `bc.drop_persistent_callbacks`
calls that the `release()` path at lines 695–696 already make. Any
BC carrying live `pubsub.subscribe` handlers that went through
`destroy` (early-exit tests, runtime-pool shutdown) leaked the
handler JSValues, tripping `list_empty(&rt->gc_obj_list)` in
`JS_FreeRuntime`. Fix applied inline alongside the classify
widening. The 16 `broker_test.cpp` B.* cases that shipped in v0.5.2
didn't reproduce this because they use `broker::dispatch_for_test`
directly — no BC carrying persistent subscriptions enters the
destroy path there.


---

## Implementation deviation (0.5.2.N broker test backfill, 2026-04-24)

Five test-shape deviations from the ICD's §Test Cases pseudocode
landed during the 29-case backfill that closed §Exit criteria from
16/45 to 45/45. All ratified at
`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md §4` row D22. None
constitutes contract drift; each documents the harness-composition
choice that landed coverage of the same invariant.

1. **B.05 / B.06 PG-gating.** The ICD marks both as `PG-gated=No`.
   Without a fake-conn seam on `publish.hpp`, the cleanest scaffold
   was the live `ws_test_fixture` drogon + `WsTestClient` chain;
   tests are tagged `[ws][integration]` to route into
   `plinth_tests_ws` (avoids the `!running_` collision with
   `plinth_tests_pg`'s `async_bridge_fixture` drogon). Both skip
   cleanly when PG is unavailable.
2. **U.02 `extension_mismatch` edge.** Post-SC6 widening (LH-2
   ship), a `notes` bc subscribing a cross-ext channel without an
   RBAC rule rejects `pubsub.rbac_denied` not
   `pubsub.extension_mismatch`. The literal `extension_mismatch`
   arm now only fires when `bc.extension_name` is empty; U.02
   exercises that edge.
3. **U.07 cancellation cascade observable swap.**
   `run_on_context`'s cancellation cascade preempts JS job dispatch
   once `bc.cancelled` is true, so the ICD's `.then(onReject)`
   observable is unreachable. Test asserts the side effect (no
   subscription registered) and notes the reject code is covered by
   source review of `pubsub_bindings.cpp:321`.
4. **S.11 / I.07 delivery re-check swap.** ICD S.11 mutates
   `state.effective_rules` between subscribe and dispatch — not
   reachable from tests without a new ConnState seam. Both tests
   exercise the same `delivery_rbac_allows` re-check path via
   `set_rbac_enforce_for_test(false)` — the 0.1.6 admin-only
   fallback arm.
5. **I.05 narrowed.** Two-extension end-to-end (publisher +
   subscriber both running QuickJS) requires
   `async_bridge_fixture`'s drogon lifecycle, which collides with
   `ws_test_fixture`'s drogon in the same subprocess. Narrowed to
   the per-bc registration routing observable; handler-invocation
   path is covered by U.09 / U.12 and the LH-2 harness.
