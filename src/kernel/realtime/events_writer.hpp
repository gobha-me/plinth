// SPDX-License-Identifier: MIT
//
// ICD-0.5.4-events-table-delta-sync §Events Writer Subsystem.
//
// Per-process events-table writer. Registers as the second
// `EventHandler` against the 0.5.0 listener (broker is the first per
// ICD-0.5.4 §When `record_delivered` fires); enqueues each dispatched
// envelope onto a bounded `std::deque<QueueEntry>` (default 10000,
// drop-newest on overflow); a dedicated `trantor::EventLoopThread`
// drains the queue and INSERTs into `plinth.events`. Writes are
// guarded by per-envelope advisory xact-lock keyed on
// `(channel, emitted_at)` for multi-node single-writer (§HA / §SC7).
// Lifecycle mirrors the coalescer (`coalescer.{hpp,cpp}`).

#pragma once

#include "kernel/config.hpp"
#include "kernel/realtime/listener.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <expected>
#include <functional>
#include <json/value.h>
#include <string>
#include <string_view>

namespace plinth::realtime::events_writer {

// Spawn the dedicated event-loop thread + register the writer's
// `EventHandler` against the listener. Idempotent — a second call
// while running is a no-op. When `cfg.enabled == false`, start() is
// a no-op (no thread, no handler registration); listener dispatch
// short-circuits at the writer's handler entry.
auto start(const Config::Realtime::Events& cfg) -> void;

// Drain the in-flight queue, cancel timers, flush cursors, and join the loop
// thread. The smaller of `timeout` and `cfg.shutdown_drain_ms` is used. False
// leaves the remaining work owned and retryable; its database and event-loop
// dependencies must remain alive. Idempotent.
[[nodiscard]] auto stop(
    std::chrono::milliseconds timeout = std::chrono::seconds{40}) -> bool;

// Process-wide config snapshot. Set by `start()`; consumed by the
// subscribe handler to gate the replay branch + by the replay engine
// (chunk size, total cap, retention, audit window). Returns the
// default-constructed config when `start()` has not been called.
[[nodiscard]] auto current_config() -> Config::Realtime::Events;

// ── Test seams ──────────────────────────────────────────────────────

[[nodiscard]] auto is_running_for_test() -> bool;

// Push a synthetic queue entry without going through the listener's
// `EventHandler` dispatch. Used by E.04 (queue overflow drop-newest)
// to drive the writer in isolation. Returns true if accepted, false
// if dropped (queue full).
auto enqueue_for_test(DispatchedEvent ev) -> bool;

[[nodiscard]] auto queue_size_for_test() -> std::size_t;

// Pin a specific DbClient. When set, every PG operation routes through
// this client instead of `drogon::app().getDbClient()`. Pass nullptr
// to clear.
auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void;

// Hook the INSERT path. The hook returns either the synthetic seq or
// an error reason; on error the writer audits `pg_error` and skips
// the row. Used by E.05 (PG INSERT failure audits).
auto set_insert_hook_for_test(
    std::function<std::expected<std::int64_t, std::string>(
        const std::string& /*channel*/, const Json::Value& /*envelope*/)>
        hook) -> void;
auto clear_insert_hook_for_test() -> void;

// Hook the advisory xact-lock acquire. Returning false simulates a
// loser in the multi-writer race (§HA). Used by E.07 (cross-node
// single writer — advisory lock).
auto set_advisory_lock_hook_for_test(
    std::function<bool(const std::string& /*key*/)> hook) -> void;
auto clear_advisory_lock_hook_for_test() -> void;

// ICD-0.5.5 §14 — fires after the writer stamps `ev.envelope["seq"]`
// from the RETURNING result, BEFORE `broker::dispatch` is invoked.
// S.01 uses this to assert the canonical seq is on the envelope by
// the time fan-out begins; S.06 uses it to inject a synthetic gap.
auto set_pre_broker_hook_for_test(
    std::function<void(const DispatchedEvent& /*ev*/)> hook) -> void;
auto clear_pre_broker_hook_for_test() -> void;

// Synchronously drain every queued entry on the calling thread.
// Bypasses the runEvery timer so tests can assert deterministic
// post-drain state without sleeping.
auto apply_drain_for_test() -> void;

// Process-wide successful-INSERT counter snapshot.
[[nodiscard]] auto writes_persisted_for_test() -> std::uint64_t;

// Reset counters + audit windows between TEST_CASEs.
auto reset_counters_for_test() -> void;

} // namespace plinth::realtime::events_writer
