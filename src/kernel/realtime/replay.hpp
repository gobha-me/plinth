// SPDX-License-Identifier: MIT
//
// ICD-0.5.4-events-table-delta-sync §Replay Engine.
//
// Per-reconnect replay query against `plinth.events`. The subscribe
// handler (`ws/subscriptions.cpp`) calls `run_replay` after the live
// subscribe registration completes (so live frames queue behind
// replay frames in the WS conn's FIFO send queue per ICD §Why
// register live subs BEFORE replay). The engine paginates through
// `WHERE seq > since_seq AND channel = ANY(channels)` in fixed
// `replay_max_rows_per_chunk` batches; each row passes through a
// per-channel RBAC re-check (defense-in-depth, mirrors ICD-0.5.2
// §Security Constraint 5); on row-cap exceedance emits
// `replay_truncated` + `resync` reason=row_cap.
//
// Three resync precondition reasons handled inside the engine:
//   - cursor_expired: `since_seq < MIN(seq)`
//   - mismatch:       `|since_seq - server_cursor| > replay_max_total_rows`
//   - row_cap:        replay would exceed `replay_max_total_rows`
// The fourth (`events_disabled`) is caller-side (subscribe handler
// short-circuits before invoking replay).

#pragma once

#include "kernel/config.hpp"
#include "kernel/ws/conn_state.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace plinth::realtime::replay {

enum class ResyncReason : std::uint8_t {
  CURSOR_EXPIRED,
  ROW_CAP,
  MISMATCH,
  // ICD-0.5.5 §8 — fourth precondition reason. Fires when the
  // per-(connection, subscription) live buffer overflows during a
  // mid-flight replay. The replay coroutine detects the abort flag
  // at the next chunk boundary and returns early; the broker's
  // overflow site emits the resync frame inline so the client sees
  // it without waiting for the replay to advance.
  LIVE_BUFFER_OVERFLOW,
};

struct ReplayResult {
  std::int64_t up_to_seq{0};
  std::size_t emitted{0};
  bool truncated{false};
  std::optional<ResyncReason> resync;
  // ICD-0.5.5 §8 — set when the abort flag flipped mid-replay; the
  // caller knows to skip emitting `replay_done` (the overflow site
  // already emitted `resync`).
  bool aborted{false};
};

// Frame sink — `run_replay` calls this for every emitted server-side
// frame (replay / replay_done / resync). The Phase 5 subscribe handler
// passes a lambda that delegates to `conn->send`; tests pass a lambda
// that captures into a vector.
using SendFn = std::function<void(std::string)>;

auto run_replay(
    const ws::ConnState& state, SendFn send, std::int64_t since_seq,
    std::vector<std::string> granted_channels, Config::Realtime::Events cfg,
    std::shared_ptr<std::atomic<bool>> abort_flag = nullptr,
    std::shared_ptr<std::atomic<std::size_t>> buffered_live_count = nullptr)
    -> drogon::Task<ReplayResult>;

// ── Frame builders ──────────────────────────────────────────────────

auto build_replay_frame(const Json::Value& envelope) -> std::string;
auto build_replay_done_frame(std::int64_t up_to_seq, std::size_t row_count,
                             std::size_t buffered_live_count = 0)
    -> std::string;
auto build_resync_frame(ResyncReason reason, std::size_t retention_seconds)
    -> std::string;

// ── Test seams ──────────────────────────────────────────────────────

// Pin a specific DbClient for replay queries. When set, every PG call
// routes through this client instead of `drogon::app().getDbClient()`.
auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void;

// Reset internal audit-window state between TEST_CASEs.
auto reset_audit_state_for_test() -> void;

} // namespace plinth::realtime::replay
