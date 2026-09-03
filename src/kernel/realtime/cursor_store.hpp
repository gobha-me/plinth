// SPDX-License-Identifier: MIT
//
// ICD-0.5.4-events-table-delta-sync §Cursor Store.
//
// Per-user delivery cursor over `plinth.user_event_cursors`. The
// broker calls `record_delivered(user_id, new_seq)` after every
// successful WS frame send (fire-and-forget); the subscribe handler
// calls `read_cursor(user_id)` on reconnect for defense-in-depth
// (the client's `since_seq` claim is authoritative per ICD §SC8).
// `reset_cursor` is fired when a `{type:"resync"}` frame goes out so
// the next reconnect starts from a known baseline.
//
// Writes coalesce through an LRU cache. A pending-delta is flushed to
// PG when (now - last_flush > cursor_cache_ttl_ms) OR (pending_delta
// >= cursor_flush_threshold), whichever first. Failures audit and
// continue; the broker NEVER raises on cursor IO per ICD §Failure
// handling.

#pragma once

#include "kernel/config.hpp"

#include <chrono>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <string>

namespace plinth::realtime::cursor_store {

// Configure cache TTL + flush threshold from `realtime.events`. Safe
// to call before or after the first I/O. Idempotent.
auto configure(const Config::Realtime::Events& cfg) -> void;

// Read the persisted cursor for `user_id`. Falls through cache to
// `plinth.user_event_cursors`; returns 0 when the user has never
// had a delivery (matches ICD §New-user behaviour). On PG error,
// returns 0 (best-effort defense-in-depth).
auto read_cursor(std::string user_id) -> drogon::Task<std::int64_t>;

// Record a successful WS delivery. Cache absorbs the write; flushes
// to PG on TTL/threshold per ICD §Cache layer. UPSERT uses
// `GREATEST(plinth.user_event_cursors.last_seq, EXCLUDED.last_seq)`
// so concurrent advances from multiple processes are monotonic.
// Fire-and-forget: PG failures do NOT raise; audit fires per ICD
// §Failure handling.
auto record_delivered(std::string user_id, std::int64_t new_seq)
    -> drogon::Task<void>;

// Reset the persisted cursor to `new_seq`. Used on `{type:"resync"}`
// emission so the next reconnect's `since_seq` claim is sanity-
// checked against a known baseline. Bypasses GREATEST — the value is
// a server-authoritative reset, not an advance.
auto reset_cursor(std::string user_id, std::int64_t new_seq)
    -> drogon::Task<void>;

// Flush every cached pending delta to PG. Called from
// `events_writer::stop()` BEFORE the writer's loop is joined, so
// in-flight cursor advances persist across shutdown. Bounded by
// the writer's `shutdown_drain_ms` budget.
auto flush_all_for_shutdown() -> drogon::Task<void>;

// Test seam — pin a specific DbClient. When set, every PG operation
// routes through this client instead of `drogon::app().getDbClient()`.
// Pass nullptr to clear.
auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void;

// Test seam — drop every cache entry. Does not flush. Used to isolate
// state between TEST_CASEs.
auto clear_cache_for_test() -> void;

// Test seam — synchronously force-flush a single user's pending delta.
// Required for C.04 / C.06 assertions that need deterministic PG-write
// ordering without waiting on TTL.
auto force_flush_for_test(std::string user_id) -> drogon::Task<void>;

// Test seam — replace the cache's clock source. Used by C.03 to
// simulate TTL expiry without real-time sleep. Pass nullptr to
// restore `std::chrono::steady_clock::now()`.
using ClockFn = std::function<std::chrono::steady_clock::time_point()>;
auto set_clock_for_test(ClockFn fn) -> void;

} // namespace plinth::realtime::cursor_store
