// SPDX-License-Identifier: MIT
//
// ICD-0.5.0-pg-listen-notify-bridge §Listener Subsystem.
//
// Per-node PostgreSQL LISTEN subscriber. Clones the jthread+eventfd+
// reconnect pattern from src/kernel/capabilities/listener.{hpp,cpp}
// (the 0.2.3 Tier-2 cache-invalidation listener) — duplicate, not
// shared, per ICD §OQ3. The 0.2.3 listener remains frozen to its
// capability channel; this subsystem runs as a sibling on the broader
// realtime channel (the single PG channel `plinth:realtime`).

#pragma once

#include "kernel/config.hpp"

#include <json/value.h>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::realtime {

// The dispatched event handed to in-process consumers. Matches the
// envelope's required fields — `layer` and `channel` are always
// present; everything else is accessible via `envelope`.
//
// ICD-0.5.4 §When `record_delivered` fires — the broker fills
// `delivered_to_users` synchronously inside `publish_dispatched` BEFORE
// the queueInLoop fan-out (so the events writer's handler sees the
// fully-populated list). The `mutable` qualifier carves a deliberate
// hole in the otherwise-const dispatch contract; alternatives (sidecar
// map, dispatch-helper-returned tuple) involve more plumbing without
// stronger invariants. See feedback at the field site for context.
struct DispatchedEvent {
  std::string layer;    // "data" | "system" | "extension"
  std::string channel;  // logical channel (NOT the PG wire channel)
  Json::Value envelope; // full parsed payload

  // Filled by the WS broker's pre-fan-out pass (publish.cpp). Used by
  // the events writer (events_writer.cpp) to advance per-user cursors
  // after the INSERT of `payload` lands. Empty when no WS subscriber
  // matches; that's the normal Layer-2/3 system-event case.
  mutable std::vector<std::string> delivered_to_users;
};

// A consumer registers once at process startup. The listener invokes
// every registered handler for every parsed NOTIFY whose envelope
// passes validation. Handlers run on the listener thread; they MUST be
// fast (non-blocking). 0.5.0 ships with zero default consumers — the
// 0.5.2 WS broker will be the first.
using EventHandler = std::function<void(const DispatchedEvent&)>;

// Add a handler. Idempotent add; not removable in 0.5.0 (ICD §OQ5).
// Safe to call before or after start_listener.
auto register_handler(EventHandler h) -> void;

// Test-only: clear all registered handlers. Isolates state between
// TEST_CASEs; not called from production.
auto clear_handlers_for_test() -> void;

// Spawn the listener thread. Idempotent — a second call while the
// thread is running is a no-op. No-op (without error) if
// `listener_cfg.enabled` is false. Errors log at error; the platform
// continues without realtime. Mirrors plinth::capabilities::
// start_notify_listener's posture.
auto start_listener(const Config::Database& db_cfg,
                    const Config::Realtime::Listener& listener_cfg) -> void;

// Signal the listener to stop, wake the poll, and join the thread within
// `timeout`. A timeout leaves the thread owned and retryable. Idempotent. The
// caller must keep Drogon and logging alive until this returns true.
auto stop_listener(std::chrono::milliseconds timeout = std::chrono::seconds{5})
    -> bool;

// Test seam — parse a raw NOTIFY payload and dispatch it through the
// registered handlers exactly as the listener loop would. Does not
// open a PG connection. Returns true on a valid-and-dispatched
// payload, false on parse error / invalid channel / unknown layer.
// Used by R.08, R.09, R.10 in Slice 4.
auto apply_notification_for_test(std::string_view channel,
                                 std::string_view payload_json) -> bool;

} // namespace plinth::realtime
