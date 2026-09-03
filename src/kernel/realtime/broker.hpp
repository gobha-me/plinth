// SPDX-License-Identifier: MIT
//
// ICD-0.5.2-ws-broker §Broker Subsystem.
//
// The WebSocket broker: per-node consumer of the listener's dispatch
// stream, fanning each envelope out to every authenticated WS
// connection whose `ConnState::channels` contains the envelope's
// logical channel (subject to per-channel RBAC) AND to every JS
// `pubsub.subscribe` caller whose per-bc subscription registry matches.
//
// Mirrors the `CoalescerRegistry` shape from ICD-0.5.1 — own subsystem
// module, lifecycle-aware drain hook, test seam + metrics getters.
// §OQ1 pins the subsystem-not-inline choice.

#pragma once

#include "kernel/config.hpp"
#include "kernel/logging.hpp"           // AuditCtx
#include "kernel/realtime/listener.hpp" // DispatchedEvent

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace plinth::js {
struct BridgeContext;
} // namespace plinth::js

namespace plinth::realtime::broker {

// Spawn the broker. As of ICD-0.5.5 §5 the broker is a writer-
// downstream consumer rather than a peer listener handler — `start`
// only flips `broker_enabled` and pulls config; the events writer
// invokes `broker::dispatch` from inside `insert_envelope` after the
// `INSERT … RETURNING seq` stamp. Idempotent — a second call while
// started is a no-op. No-op (without error) if broker_cfg.enabled is
// false. Startup ordering is still listener → coalescer → broker →
// events_writer per main.cpp; only the registration call goes away.
auto start(const Config::Realtime::Broker& broker_cfg) -> void;

// Signal the broker to stop accepting dispatches and clear its
// JS-side subscription registry. Does NOT deregister the
// EventHandler (0.5.0 listener does not support deregister).
// Idempotent. MUST be called after stop_listener() so no new
// dispatch races a partially-cleared broker. Atexit placement:
// between `realtime::stop_listener()` and
// `CoalescerRegistry::instance().shutdown()` per §Deterministic
// Teardown.
auto stop() -> void;

// Extension-lifecycle drain hook — DISABLED / UPGRADING / UNINSTALL
// transitions in install_lifecycle.cpp call this alongside the
// coalescer's drain_extension. Synchronously evicts every WS-side
// subscription on channels matching `plinth:data:ext_<name>.*` or
// `plinth:ext:<name>:*`, and every JS-side subscription on the same
// channel prefixes (including `bc_registry` entries for bcs whose
// extension_name matches). Idempotent. Fires one
// `realtime.broker.extension_drained` audit per call when the match
// count is non-zero.
//
// `trigger` labels the call site for the audit's `trigger` field —
// stable strings: `"disabled"` / `"upgrading"` / `"uninstall"` / `"test"`.
auto drain_extension(std::string_view name, std::string_view trigger) -> void;

// Overload for scaffolds + test seams that don't carry a lifecycle
// trigger; logs as `trigger="manual"`.
auto drain_extension(std::string_view name) -> void;

// JS-side subscription registration / deregistration. Called by the
// `PUBSUB_SUBSCRIBE` / `PUBSUB_UNSUBSCRIBE` dispatch arms in
// run_on_context after the binding-side validation gauntlet has
// passed. Returns false on quota overflow (caller reports
// `pubsub.quota_exceeded`).
auto register_js_subscription(plinth::js::BridgeContext* bc,
                              std::string_view channel, int callback_id)
    -> bool;
auto unregister_js_subscription(plinth::js::BridgeContext* bc,
                                std::string_view channel) -> void;

// bc-teardown hook — evict every subscription owned by `bc`. Called
// from BridgeContext's destructor path / RuntimePool slot eviction.
auto drop_bc_subscriptions(plinth::js::BridgeContext* bc) -> void;

// ICD-0.5.5 §5 — writer-downstream entry point. Called by
// `events_writer::insert_envelope` after the `INSERT … RETURNING seq`
// stamp on the production path; called by `dispatch_for_test` on the
// synthetic path. Honours the `broker_enabled` flag (post-stop
// invocations short-circuit + audit `broker_disabled`), bumps the
// process-wide `dispatch_count` metric, and fans out to both arms:
// the WebSocket arm via `plinth::ws::publish_dispatched` (which also
// populates `ev.delivered_to_users` for the writer's subsequent
// cursor-advance loop) and the JS-side `pubsub.subscribe` arm. Safe
// to call from any thread; the WS arm hops onto each subscriber's
// owning loop and the JS arm hops onto drogon's main loop. Returns
// the JS-side dispatch count (WS-side fan-out happens
// asynchronously and is reported separately via `ws_subscriber_count`).
auto dispatch(const DispatchedEvent& ev) -> std::size_t;

// Test seam — dispatch a DispatchedEvent through the broker exactly
// as the writer-downstream call site would, skipping listener wire
// parsing AND the writer's INSERT path. Covers B.* cases without PG.
// Returns the JS-side dispatch count.
auto dispatch_for_test(const DispatchedEvent& ev) -> std::size_t;

// Test seam — synthesize an RBAC enforcement flip for S.11 / I.07
// style tests. Toggles the broker's effective rbac_enforce without a
// full process restart. NOT exposed via production API.
auto set_rbac_enforce_for_test(bool on) -> void;

// Current RBAC-enforcement state. Read by the WS-side subscribe gate
// + the per-conn delivery re-check (publish_dispatched) + the
// `pubsub.subscribe` JS binding to honour the `broker.rbac_enforce`
// config flag and the test seam above. Defaults to `true` before
// `broker::start(...)` is called so any pre-start subscribe path is
// also gated.
auto is_rbac_enforced() -> bool;

// Current per-connection subscription quota. Honours the broker's
// `Config::Realtime::Broker::max_subscriptions_per_conn` clamp set at
// `broker::start(...)` time (defaults to 64 before start).
auto max_subscriptions_per_conn() -> std::size_t;

// Bump the RBAC-denial metric from the WS-side subscribe gate +
// the `pubsub.subscribe` JS binding. Keeps the counter authoritative
// on the broker module even though the enforcement points live in ws/
// and js/stdlib/.
auto note_rbac_denial() -> void;

// ICD-0.5.2 §Audit Events. Emit a `realtime.broker.subscribe_denied`
// audit subject to a 1-minute-per-`(user_id, channel)` sliding-window
// rate limit. Always bumps the process-wide `rbac_denial_count`
// metric even when the audit itself is suppressed. `source` is
// `"ws"` or `"js"`, `layer` is `"data"` / `"system"` / `"extension"`
// / `"invalid"`, `reason` is one of the stable codes per ICD
// (`rbac_denied` / `layer_unsupported` / `extension_mismatch` /
// `channel_invalid` / `quota_exceeded`). `ctx` is forwarded to the
// audit row's user + session + IP fields; empty fields are allowed
// (JS-side denials may not have a session).
auto note_subscribe_denied(const plinth::log::AuditCtx& ctx,
                           std::string_view channel, std::string_view layer,
                           std::string_view reason, std::string_view source)
    -> void;

// ICD-0.5.2 §Audit Events. Emit a `realtime.broker.dispatch_skipped`
// audit subject to a 1-minute-per-`channel` sliding-window rate
// limit. Called from the listener-dispatch arm when the broker
// handler enters but aborts early (post-stop short-circuit, malformed
// envelope post-condition fail).
auto note_dispatch_skipped(std::string_view channel, std::string_view reason)
    -> void;

// Test seam — reset the in-process rate-limit windows for audits.
// Calling `broker::drain_extension` or an RBAC denial twice in the
// same TEST_CASE would otherwise suppress the second audit by the
// per-(user,channel) or per-call rate limiter.
auto reset_audit_windows_for_test() -> void;

// Metrics — cheap in-memory counters. Monotonic across the process
// lifetime; not reset between tests except by the explicit
// `reset_metrics_for_test` seam. Intended for 0.7.x `plinth.metrics`
// wiring; 0.5.2 ships them as getters only.
auto dispatch_count() -> std::uint64_t;
auto rbac_denial_count() -> std::uint64_t;
auto js_subscriber_count() -> std::size_t; // live per-bc subscriptions
auto ws_subscriber_count() -> std::size_t; // total (conn, channel) pairs

auto reset_metrics_for_test() -> void;

} // namespace plinth::realtime::broker
