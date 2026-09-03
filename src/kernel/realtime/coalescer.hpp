// SPDX-License-Identifier: MIT
//
// ICD-0.5.1-pg-auto-event-coalescer §Registry + Integration Point.
//
// Per-process DB-write coalescer. After each successful `db.exec` on
// an extension's connection, `run_db_exec_outcome` calls
// `record_write(schema, table, op_kind, row_count, ext_name)`. The
// registry opens a 50 ms fixed-duration window on the first write per
// (schema, table), accumulates counters inside that window, and on
// flush emits ONE consolidated Layer-1 envelope via
// `plinth::realtime::emit_notify_async`. Timer work runs on a
// dedicated `trantor::EventLoopThread` (OQ5 pin); window lookup is
// protected by a shared_mutex.

#pragma once

#include "kernel/config.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/sql_classify.hpp"

#include <chrono>
#include <cstddef>
#include <drogon/orm/DbClient.h>
#include <expected>
#include <functional>
#include <json/value.h>
#include <string>
#include <string_view>

namespace plinth::realtime {

class CoalescerRegistry {
 public:
  // Process singleton. Lazy-init, idempotent.
  static auto instance() -> CoalescerRegistry&;

  // Start the dedicated event-loop thread. Called once from main.cpp
  // startup after `realtime::start_listener`. Idempotent — a second
  // call while running is a no-op. When `cfg.enabled == false`,
  // start() is a no-op and every subsequent `record_write` short-
  // circuits. Safe to call after a prior shutdown() (testing seam).
  auto start(const Config::Realtime::Coalescer& cfg) -> void;

  // Cancel every outstanding timer and force-flush every open window, then
  // join the loop thread within one shared deadline. False preserves
  // unflushed windows for a retry and leaves database/event-loop dependencies
  // alive. Safe to call multiple times.
  [[nodiscard]] auto shutdown(
      std::chrono::milliseconds timeout = std::chrono::seconds{40}) -> bool;

  // Record an extension-scoped DB write. See ICD §Window open /
  // accumulate / flush for the full state machine. `row_count == 0`
  // is a no-op on an empty bucket; on an open bucket it leaves
  // counters unchanged (§Envelope Assembly — Zero-row writes).
  //
  // ICD-0.5.3 §`db.batch()` §Coalescer interaction — when
  // `batch_scope_id != 0` the write accumulates under the scope
  // bucket (no timer), held until `flush_batch_scope` / `discard_batch_scope`.
  auto record_write(std::string_view schema, std::string_view table,
                    OpKind op_kind, std::size_t row_count,
                    std::string_view extension_name,
                    std::uint64_t batch_scope_id = 0) -> void;

  // ICD-0.5.3 §`db.batch()` §Coalescer interaction §flush_batch_scope.
  // Called from the DB_BATCH_COMMIT dispatch arm; emits one envelope
  // per (schema, table) tuple accumulated under `scope_id` with
  // `window_ms = 0` to signal "batch commit" to downstream consumers.
  // Returns the number of envelopes emitted.
  auto flush_batch_scope(std::uint64_t scope_id) -> std::size_t;

  // ICD-0.5.3 §`db.batch()` §Coalescer interaction §discard_batch_scope.
  // Called from the DB_BATCH_ROLLBACK dispatch arm; drops every
  // accumulated counter for `scope_id` without emitting. Returns the
  // number of (schema, table) buckets discarded.
  auto discard_batch_scope(std::uint64_t scope_id) -> std::size_t;

  // Lifecycle drain hook — DISABLED / UPGRADING / UNINSTALL
  // transitions in install_lifecycle.cpp call this before the state
  // row is updated. Synchronously flushes every open window whose
  // owner matches `extension_name`. Running windows for other
  // extensions are untouched. Idempotent.
  auto drain_extension(std::string_view extension_name) -> void;

  // Test seam — fire a window flush directly, bypassing the timer.
  // Returns true if a window existed and was flushed.
  auto apply_flush_for_test(std::string_view schema, std::string_view table)
      -> bool;

  // Test seam — currently-open window count.
  [[nodiscard]] auto open_window_count_for_test() const -> std::size_t;

  // Test seam — intercept the emit step and return a synthetic
  // result. Set to nullptr to clear. Used by E.03 to drive the
  // `flush_failed` audit path without a live PG failure.
  using EmitHook =
      std::function<std::expected<void, NotifyError>(const Json::Value&)>;
  auto set_emit_hook_for_test(EmitHook hook) -> void;
  auto clear_emit_hook_for_test() -> void;

  // Test seam — drain the registry's state so each TEST_CASE starts
  // from a known baseline (cancels timers, clears the windows map
  // without firing emits or shutdown audit). Does NOT stop the loop
  // thread; use shutdown() for that.
  auto clear_windows_for_test() -> void;

  // Test seam — pin a specific DbClient for the emit path. When set,
  // the coalescer routes every flush through this client instead of
  // `drogon::app().getDbClient()`. Integration tests construct a
  // DbClient via `drogon::orm::DbClient::newPgClient(...)` without
  // bringing up the full Drogon app. Pass nullptr to clear.
  auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void;

  // Deleted copy / move are public per modernize-use-equals-delete.
  CoalescerRegistry(const CoalescerRegistry&) = delete;
  CoalescerRegistry(CoalescerRegistry&&) = delete;
  auto operator=(const CoalescerRegistry&) -> CoalescerRegistry& = delete;
  auto operator=(CoalescerRegistry&&) -> CoalescerRegistry& = delete;

 private:
  CoalescerRegistry() = default;
  ~CoalescerRegistry() = default;
};

} // namespace plinth::realtime
