#pragma once

// In-process event publish hooks.
//
// `publish(channel, payload)` — the original 0.1.6 primitive. Used by
// any kernel caller that wants to fan out a raw payload to every
// subscriber of `channel`.
//
// `publish_dispatched(DispatchedEvent&)` — ICD-0.5.2 §WS fan-out.
// Envelope-aware fan-out used by the realtime broker: snapshots the
// registry, per-conn filters by `ConnState::channels` AND re-runs the
// per-channel RBAC check, then queues a `{type:"event", channel,
// payload: <envelope>}` frame onto each matching conn's loop.
//
// The original bridge design (PG LISTEN/NOTIFY reader, coalescer,
// plinth.events table, sequence numbers) arrived with v0.5.0 + v0.5.1;
// 0.5.2 is the client-reachable fan-out layer.

#include "kernel/realtime/listener.hpp" // DispatchedEvent

#include <cstddef>
#include <json/value.h>
#include <string_view>

namespace plinth::ws {

// Fan out a payload to every authenticated connection that has subscribed
// to `channel`. Safe to call from any thread; each delivery is queued
// onto the subscriber's owning event loop.
auto publish(std::string_view channel, const Json::Value& payload) -> void;

// Envelope-aware fan-out. Per-conn filters by channel membership AND
// re-checks the per-channel RBAC grant (defense in depth per
// ICD-0.5.2 §Security Constraints item 2). Safe to call from any
// thread; each delivery is queued onto the subscriber's owning loop.
auto publish_dispatched(const plinth::realtime::DispatchedEvent& ev) -> void;

// Extension-lifecycle drain — evict every `ConnState::channels` entry
// whose channel belongs to extension `name` (Layer-1 `plinth:data:ext_<name>.`
// or Layer-3 `plinth:ext:<name>:`). Returns the number of
// (conn, channel) pairs removed. Called from the broker's
// `drain_extension` hook at DISABLED / UPGRADING / UNINSTALL.
auto drain_ws_subscriptions_for_extension(std::string_view name) -> std::size_t;

// Test seam — total (conn, channel) pairs currently subscribed
// across every live WebSocket connection. Exposed so the broker's
// `ws_subscriber_count()` metric has a single source of truth.
auto total_subscription_count() -> std::size_t;

// Counter-maintenance hooks. Called from `on_subscribe`'s insert
// site, `on_unsubscribe`'s erase site, `drain_ws_subscriptions_for_extension`'s
// per-loop erase, and `EventsController::handleConnectionClosed`
// (bulk decrement on connection tear-down). All sites run on the
// subscriber's owning event loop so the underlying atomic bumps are
// race-free against other per-conn mutations; inter-conn totals
// still read monotonically.
auto note_subscriptions_added(std::size_t n) -> void;
auto note_subscriptions_removed(std::size_t n) -> void;

// Test seam — zero the subscription counter. Used by ws_test_fixture
// between test cases so leaked ConnStates from prior cases do not
// pollute later assertions.
auto reset_subscription_count_for_test() -> void;

// Test seam — clear the per-(user_id, channel) audit-window state
// backing `realtime.seq.gap_detected` dedup. Mirrors
// `reset_debounce_audit_state_for_test` in subscriptions.cpp.
auto reset_gap_audit_windows_for_test() -> void;

// Test seam — emit-count for `realtime.seq.gap_detected` audits.
// Bumped before the audit-ready guard so suppressed-by-shutdown
// emits still count, mirroring `debounce_audit_emit_count_for_test`.
[[nodiscard]] auto gap_audit_emit_count_for_test() -> std::uint64_t;

} // namespace plinth::ws
