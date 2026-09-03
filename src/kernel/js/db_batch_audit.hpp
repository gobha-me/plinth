// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §`db.batch()` §Audit Events.
//
// Two rate-limited audit events fired by the DB_BATCH_COMMIT and
// DB_BATCH_ROLLBACK dispatch arms respectively. Also hosts the
// process-wide monotonic scope-id allocator the db.batch binding
// draws from (mirrors ICD §Binding implementation step 3).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <trantor/net/EventLoop.h>

namespace plinth::js {

// Allocate a fresh, process-wide-unique scope id. Monotonic uint64;
// never returns 0 (0 means "not in a batch" on AsyncOp). Thread-safe
// (atomic increment).
[[nodiscard]] auto alloc_batch_scope_id() -> std::uint64_t;

// Configure aggregation window for `db.batch.committed` /
// `db.batch.rolled_back` audits. Called from main.cpp after config
// load. Default 60 s matches ICD §Config Surface.
auto set_batch_audit_window_ms(std::size_t window_ms) -> void;

// Set / get max ops per batch (ICD §Config Surface
// `db.batch.max_ops_per_batch`, default 500). Enforced
// synchronously at db.exec / db.query enqueue while inside a batch.
auto set_batch_max_ops_per_batch(std::size_t max_ops) -> void;
[[nodiscard]] auto batch_max_ops_per_batch() -> std::size_t;

// ICD-0.5.3 §`db.batch()` §Config Surface `db.batch.timeout_ms`.
// Set / get the batch wall-clock deadline (default 30 000 ms, clamped
// to [100, 600000] at config-load). The BEGIN finalize arm reads
// this to schedule a `trantor::EventLoop::runAfter` that flips
// `bc.batch_state.timed_out` on fire; in-batch `db.exec` / `db.query`
// and `__db_batch_commit__` reject inline with `db.batch.timeout`
// when the flag is set.
auto set_batch_timeout_ms(std::size_t timeout_ms) -> void;
[[nodiscard]] auto batch_timeout_ms() -> std::size_t;

// `db.batch.committed` audit. Rate-limited (64-entry LRU keyed on
// extension). Detail carries the in-batch op count so operators can
// spot unusual fan-out patterns.
auto audit_batch_committed(const std::string& extension_name,
                           std::size_t ops_in_batch) -> void;

// `db.batch.rolled_back` audit. Same rate-limit envelope. `reason`
// is one of: `user_callback_threw`, `db_error`, `extension_drained`,
// `bc_destroyed`, `timeout`, or a specific rejection code when
// available. `error_code` is the JS-visible rejection code (empty if
// N/A).
auto audit_batch_rolled_back(const std::string& extension_name,
                             const std::string& reason,
                             const std::string& error_code) -> void;

// Test seam — clear the rate limiter state.
auto reset_batch_audit_for_test() -> void;

// ICD-0.5.3 §`db.batch()` §Extension Lifecycle Integration.
// Per-process registry of in-flight batches so the lifecycle drain
// (DISABLED / UPGRADING / UNINSTALL) and process shutdown can
// drop the corresponding coalescer scope buckets cleanly — preventing
// a late `flush_batch_scope` from emitting envelopes after the
// owning extension has been torn down (or after shutdown has begun).
//
// `register_in_flight_batch` is called from the DB_BATCH_BEGIN
// finalization callback (on the main loop), `unregister_in_flight_batch`
// from the DB_BATCH_COMMIT / DB_BATCH_ROLLBACK finalization
// callbacks. Both are thread-safe (internal mutex).
auto register_in_flight_batch(std::uint64_t scope_id,
                              const std::string& extension_name) -> void;
auto unregister_in_flight_batch(std::uint64_t scope_id) -> void;

// ICD-0.5.3 §B.06 — attach / detach the wall-clock timer
// associated with a live in-flight batch. `set_batch_timer` is
// called from the BEGIN finalize arm immediately after
// `register_in_flight_batch`; `clear_batch_timer` is called from
// the COMMIT / ROLLBACK finalize arms before `unregister_in_flight_batch`.
// The discard paths below invalidate any attached timer
// synchronously, so a freed bc can never see a late fire.
auto set_batch_timer(std::uint64_t scope_id, trantor::TimerId timer_id,
                     trantor::EventLoop* loop) -> void;
auto clear_batch_timer(std::uint64_t scope_id) -> void;

// Discard scopes for a single extension — called from
// install_lifecycle.cpp alongside the existing coalescer
// drain_extension hook. Returns the count of discarded scopes.
auto discard_batches_for_extension(const std::string& extension_name)
    -> std::size_t;

// Discard every in-flight batch scope — called by the coordinator after the
// events writer drains and before `realtime::broker::stop()`. Idempotent.
// Returns the count of discarded scopes.
auto discard_all_batches() -> std::size_t;

// Test seam — reset the in-flight registry.
auto reset_in_flight_batches_for_test() -> void;

} // namespace plinth::js
