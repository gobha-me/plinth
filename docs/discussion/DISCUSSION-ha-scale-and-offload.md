# HA Scale-Out, Client Rebalance, and Peer Offload — Design Space Summary

**Status:** Discussion capture — not a design doc, not authoritative
**Source:** Architecture decomposition session (post-methodology-update), 2026-04-17
**Participants:** the maintainer (Architect) + Claude (Architecture Session)
**Feeds into:** Future DESIGN-ha-v09x.md (~0.9.x)
**References:**
- `architecture/04-services-ha.md §6` (HA model — leaderless, PG-coordinated)
- `architecture/04-services-ha.md §2` (Scheduled tasks — PG advisory locks)
- `architecture/04-services-ha.md §5` (Sidecar contract)
- `DESIGN-shell-v06x.md §4` (Client SDK — the reconnect handler lives here)

---

## 0. Scope Note

This discussion is about HA behavior patterns that will matter in
0.9.x, not about kernel work for the milestones currently in
flight. Plinth-the-kernel's job here is narrow: land a few
don't-foreclose decisions in the client SDK and sidecar contract
(see §5), and don't accidentally design those in ways that make
0.9's HA work impossible. The mechanisms themselves are 0.9.x
design work; this document exists so current milestones don't
close off options that later work depends on.

## 1. The Problem

The current HA model (`architecture/04-services-ha.md §6`) is
leaderless with PG-coordinated heartbeats and stale-node self-eviction.
It says nothing about:

- Scale-out balancing: when node N+1 joins, clients stay pinned to
  their original nodes until they reconnect for unrelated reasons.
  Load does not redistribute.
- Local saturation: a node with heavy local load continues accepting
  new work rather than deferring to a less-loaded peer.

Both are post-1.0 concerns. This doc exists so the design decisions
made in earlier milestones (SDK protocol, sidecar contract) don't
accidentally foreclose future HA work.

## 2. Idea 1 — Rebalance on Scale-Up

When a new node joins the cluster, existing clients should
(eventually) redistribute across the larger pool. The mechanism is
server-initiated graceful reconnect:

1. New node N+1 registers in `plinth.node_registry`.
2. Existing nodes detect the registration (LISTEN/NOTIFY on
   registration events).
3. Each existing node picks a fraction of its WS clients and sends
   a `graceful_reconnect` control frame.
4. Client SDK closes the WS cleanly, reconnects through the load
   balancer, and lands on whichever node the LB routes to
   (statistically some on N+1, reducing concentration).
5. Delta sync on reconnect (already required for 0.5.x) handles the
   subscription state transfer.

**Distinct from a dropped connection.** `graceful_reconnect` is a
signal, not a failure. The client SDK must treat it differently from
a dropped connection: no "connection lost" UI flash, no backoff, no
user-visible hiccup. The subscription state is preserved across the
reconnect because the client knows to preserve it.

**Constraint on earlier work.** The client SDK at 0.6.3 needs to
understand `graceful_reconnect` as a distinct control frame type.
This is one branch in the reconnection handler and one frame type
in the WS protocol spec. Adding it early is cheap; retrofitting it
after SDKs are deployed is the shell-SDK-versioning problem.

**What the HA arc decides:**
- Rebalance fraction and pacing (don't reconnect everyone at once).
- Which node initiates (the overloaded one, the newest one, or both
  through a coordination protocol).
- Interaction with sticky sessions if the deployment has any.

## 3. Idea 2 — Peer Offload for Local Saturation

Original framing: when a node is saturated, offload incoming capability
calls to a less-loaded peer.

**Why this turns out to mostly not be the right shape:**

- Cross-node routing of arbitrary extension handlers is expensive.
  Response has to come back to the original WS. Handler state (PG
  connections in schema `ext_X`, cached auth, etc.) doesn't travel.
  Fallback paths when the peer also saturates get ugly.
- Lightweight QuickJS capabilities don't benefit. At target scale,
  local queue depth is measured in milliseconds. Remote execution
  adds more latency than it saves.
- Heavy capabilities (LLM calls, big computations) belong in sidecars
  anyway, and the sidecar tier already has load-aware routing as a
  natural fit.

**The cleaner split:**
- Lightweight capabilities run locally in QuickJS, always. Queue if
  needed. At Plinth's scale the queue never gets deep.
- Heavy capabilities go to sidecars. The sidecar tier supports
  load-aware instance selection: each sidecar registers load in
  `plinth.node_registry`, and Tier 3 resolution picks the
  least-loaded instance.

The classification is per-capability, made by the extension author at
declaration time. No runtime cleverness needed.

## 4. Idea 2' — Scheduled Task Offload

The exception where peer offload is genuinely useful: scheduled tasks
(0.7.x). Tasks are asynchronous, no client is waiting, so response
routing is trivial.

Mechanism: a node that *would* grab the PG advisory lock but has high
local load writes a `deferred` marker and lets the next heartbeat
cycle's least-loaded node take it.

- Uses existing primitives (advisory locks, `plinth.node_registry`).
- No new transport.
- No handler-state transfer — tasks start fresh on whichever node
  runs them.
- Skippable if scheduled tasks stay lightweight (session cleanup,
  metrics rollup, heartbeat sweep, etc. — none of these pin a node).

## 5. What Is Decided Now

Two don't-foreclose constraints, both pointed at earlier work:

1. **Client SDK (0.6.3) reserves `graceful_reconnect` as a WS control
   frame type.** Implementation can be a no-op log-and-reconnect in
   0.6.3; the full graceful-reconnect behavior ships with HA work.
   What matters now: the frame type is reserved, the SDK distinguishes
   it from a dropped connection, and the SDK API doesn't foreclose
   preserving subscription state across a reconnect.

2. **Sidecar tier (0.8.x) supports load-aware instance selection.**
   Sidecars register load metrics (CPU, memory, in-flight request
   count, queue depth) in `plinth.node_registry`. Tier 3 resolution
   in the capability registry (0.2.x) must not foreclose the addition
   of a load-aware selection strategy; current "first healthy" is
   acceptable as 0.2.x behavior but the resolution interface should
   permit strategy swapping.

Beyond these two, everything is deferred.

## 6. What Is Deferred

To DESIGN-ha-v09x.md (Scale 2, targets ~0.9.x):

- Exact rebalance algorithm (fraction, pacing, initiator).
- Load-metric shape in `plinth.node_registry` (what dimensions, how
  aggregated, how often written).
- Selection strategy for load-aware Tier 3 routing.
- Scheduled task offload — whether to implement at all, and if so,
  the exact `deferred` marker protocol.
- Interaction with sticky sessions, WS affinity, LB behavior.
- Degraded-mode behavior when all nodes are saturated.
- Split-brain detection and recovery.

## 7. What Is Explicitly Not In Scope

These are rejected, not deferred. Adding them requires a fundamental
revision of the HA model, not an additive patch:

- **Leader election for coordination.** The HA model is leaderless
  and PG-coordinated by decision. Reintroducing a leader defeats the
  simplicity of advisory-lock coordination.
- **Cross-node shared memory.** Nodes share state through PG only.
  No Raft, no gossip, no distributed cache.
- **Runtime migration of in-flight work.** If a node dies mid-handler,
  the handler fails and the client retries. No checkpoint/restart.

## 8. What This Document Is Not

Conversation capture, not a design doc. The HA arc produces the real
design when prerequisites are met. Do not cite claims in this document
as constraints on 0.9.x work.
