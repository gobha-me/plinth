#pragma once

// plinth::capabilities — PG LISTEN/NOTIFY cache invalidation (ICD-0.2.3).
//
// The producer side (registration.cpp) emits `pg_notify(
//   'plinth_capability_changed', <json>)` after each successful mutation.
// This module hosts the consumer: a long-lived libpq connection that
// subscribes to the channel and keeps the Tier 2 cache in resolution.cpp
// in sync with the authoritative plinth.capabilities table.
//
// Payload shape per ICD-0.2.2 §Cache Invalidation:
//   { "action": "register" | "deregister" | "disable" | "enable",
//     "signature": "namespace:version:function",
//     "scope":     "instance" | "user",
//     "extension_name": <string | null> }
//
// Threading model — a single std::jthread owns a dedicated PGconn. The
// listener thread poll()s the connection socket and an eventfd used as
// the shutdown wake-up. On CONNECTION_BAD the loop reconnects with a
// 1 s backoff; NOTIFYs delivered during the reconnect window are lost
// on the wire, but the listener calls plinth::capabilities::
// reload_tier2_cache() after every successful LISTEN open (initial and
// reconnect) so the cache catches up from the authoritative table. The
// upper bound on cache divergence is therefore one reconnect backoff
// plus one SELECT (ICD-0.2.4 amendment).

#include "kernel/config.hpp"

#include <chrono>
#include <string_view>

namespace plinth::capabilities {

// Spawn the listener thread. Idempotent within a process: a second call
// while the thread is already running is a no-op. Errors are logged at
// error but never thrown — registration continues to function even if
// the listener cannot connect; the cache simply diverges until a future
// restart re-syncs via init_resolver.
auto start_notify_listener(const Config::Database& db_cfg) -> void;

// Signal the listener to stop, wake the poll, and join the thread within
// `timeout`. A timeout leaves the thread owned and retryable; callers must not
// tear down its database or logging dependencies until this returns true.
// Safe to call multiple times.
auto stop_notify_listener(
    std::chrono::milliseconds timeout = std::chrono::seconds{5}) -> bool;

// Test seam — parse a raw NOTIFY payload and apply it to the Tier 2
// cache exactly as the listener loop would. Opens its own short-lived
// PGconn when the `register` action requires fetching the new row.
// Returns true on a valid + applied payload, false on parse error or
// unknown action (both paths are logged at warn).
auto apply_notification_for_test(const Config::Database& db_cfg,
                                 std::string_view payload_json) -> bool;

} // namespace plinth::capabilities
