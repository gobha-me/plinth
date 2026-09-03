// SPDX-License-Identifier: MIT
//
// BridgeContext — the per-execution state carried alongside a pooled
// JSRuntime + JSContext pair. See ICD-0.3.1-runtime-lifecycle.md
// §BridgeContext Contract and ICD-0.3.3-async-bridge.md §BridgeContext
// Async Activation.
//
// Scope after 0.3.4: 0.3.1's synchronous fields (runtime/context handles,
// resource tracking, cancellation flag, call-depth shape) PLUS the async
// state activated by ICD-0.3.3 (pending_ops queue, callbacks map,
// per-execution concurrent-async-op counter, result-size cap) PLUS the
// UserContext value-copy field activated by ICD-0.3.4 for the CAP_CALL
// dispatch arm. The HTTP_REQUEST / STORAGE_* / PUBSUB_PUBLISH op variants
// are still reserved on the AsyncOp::Type enum but their dispatch arms
// are not implemented — see async_op.hpp for the full contract.

#pragma once

#include "kernel/capabilities/resolution.hpp"
#include "kernel/js/async_op.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <string>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <vector>

struct JSContext;
struct JSRuntime;

namespace plinth::js {

// Forward-declared only. The real Extension type lands with the 0.4.x
// extension installer. 0.3.1 callers (tests + future host-eval paths)
// pass nullptr.
struct Extension;

// ConfigProjection — the value-copy slice of `plinth::Config` that
// `config.get()` from JS is allowed to read. Populated by RuntimePool
// at entry-creation time from the Config reference passed to the ctor.
// This is the compile-time, hard-coded, secret-free projection required
// by ICD-0.3.2 §Security Constraint 1–2. Adding a field here is a
// source change (code-review gated); there is no runtime allowlist.
//
// Never add: any Config::Database field, `migrations_dir`, or any
// future field flagged secret. See ICD-0.3.2 §Security Constraints.
struct ConfigProjection {
  bool dev_mode = false;
  std::string node_id;
  std::string listen_host;
  uint16_t listen_port = 0;
  bool registration_enabled = false;
  double ws_auth_timeout_s = 0.0;
  double ws_heartbeat_interval_s = 0.0;
  double ws_heartbeat_timeout_s = 0.0;
  // ICD-0.4.1 §Configuration. Reserved fields for future policy hooks
  // (no JS binding consumes them in 0.4.1; the scanner config is read
  // at the C++ guard layer). Non-secret bool/size_t — meets the
  // "secret-free projection" rule above.
  bool security_unicode_scanner_enabled = true;
  std::size_t security_unicode_scanner_threshold = 50;
  bool security_unicode_scanner_log_findings = true;
};

struct BridgeContext {
  // --- QuickJS handles (owned by the RuntimePool, not by this struct) ---
  JSRuntime* rt = nullptr;
  JSContext* ctx = nullptr;
  const Extension* extension = nullptr; // may be nullptr for host-side eval

  // ICD-0.5.0 §`pubsub.*` JS Stdlib → Extension-identity gate. The
  // `pubsub.publish` binding reads this to verify the Layer-3 channel
  // prefix matches the caller's extension name. Empty string denotes
  // a kernel-scope BridgeContext (no extension identity) — the
  // binding rejects `pubsub.extension_mismatch` in that case. The
  // 0.4.4 extension-install seed is the authoritative populator; the
  // kernel-internal host-eval path leaves this empty, matching
  // ICD-0.5.0 §`pubsub.*` JS Stdlib → BridgeContext extension identity.
  std::string extension_name;

  // --- Resource tracking ---
  // `execution_start` is set once per execution (wall-clock reference).
  // `cpu_timer_start` is reset on each resume_cpu_timer() call; the gap
  // between resume and pause is folded into `cpu_time_accumulated`.
  // The two time_point members rely on std::chrono's default ctor
  // (epoch) — explicit `{}` would trip readability-redundant-member-init.
  std::chrono::steady_clock::time_point execution_start;
  std::chrono::steady_clock::time_point cpu_timer_start;
  std::chrono::nanoseconds cpu_time_accumulated{0};
  std::chrono::milliseconds cpu_time_limit{0};
  std::chrono::milliseconds wall_clock_limit{0};

  // Call depth is carried here for future cap.call-from-JS wiring
  // (ICD-0.3.4); the capability dispatcher already owns the enforcement
  // check — see ICD-0.2.2 §Call Depth Tracking.
  int call_depth = 0;
  int max_call_depth = 8;

  // Set by the host to request termination; read by the interrupt
  // handler. In 0.3.1 this is toggled only by tests; the full
  // cancellation trigger matrix is 0.3.3 scope.
  std::atomic<bool> cancelled{false};

  // Peak-memory signal. Set (and latched) by the interrupt handler
  // or by `drive_jobs` when the runtime's `malloc_size` comes within
  // `OOM_MEMORY_SLACK_BYTES` of `malloc_limit`. Read post-hoc by
  // `extract_error` / `classify_rejection` so an OOM that unwinds
  // the async frame (freeing most of the heap before the rejection
  // value can be classified) still maps to
  // `EvalErrorKind::MEMORY_LIMIT` rather than falling through to
  // `PROMISE_REJECTED_UNHANDLED`. The latch is per-execution: the
  // RuntimePool resets it on release()/acquire().
  std::atomic<bool> memory_limit_hit{false};

  // Value-copy projection of plinth::Config consulted by the
  // `config.get()` JS binding (ICD-0.3.2). Populated by
  // RuntimePool::create_entry before inject_kernel_stdlib runs.
  ConfigProjection config_proj{};

  // Value-copy of the caller's UserContext consulted by the CAP_CALL
  // dispatch arm (ICD-0.3.4 §BridgeContext Additions). Populated by
  // RuntimePool::create_entry from an optional ctor argument; defaults
  // to UserContext::anonymous() so a test or driver that forgets to
  // synthesize identity still produces well-defined RBAC behavior
  // (every RBAC-gated capability rejects — see the anonymous-identity
  // safeguard at tests/kernel/rbac/anonymous_identity_test.cpp).
  //
  // Value-copy (not pointer) mirrors the ConfigProjection decision in
  // ICD-0.3.2: decouples pool lifetime from whatever the caller's
  // context object was. Never exposed to JS — ICD-0.3.4 §Security
  // Constraint 3 forbids a cap.whoami / cap.impersonate surface.
  plinth::capabilities::UserContext user{
      plinth::capabilities::UserContext::anonymous()};

  // --- Async state (ICD-0.3.3) ---
  //
  // pending_ops:        FIFO queue of work enqueued by JS bindings
  //                     since the coroutine loop's last drain.
  // callbacks:          Live PromiseCallbacks keyed by integer id.
  //                     Entries are erased by resolve()/reject() or
  //                     by run_cancellation_cascade. A "missing id"
  //                     in resolve()/reject() is the abandoned-op
  //                     signal (PROMISE_RESOLVE_AFTER_CANCEL).
  // next_callback_id:   Monotonic counter; wraps via uint-style
  //                     overflow only if more than INT_MAX ops are
  //                     enqueued in a single execution (not a real
  //                     concern for any extension we expect).
  // concurrent_async_ops: Live in-flight count, used for back-pressure.
  //                     Decremented inside resolve()/reject().
  // max_concurrent_async_ops: Per-execution cap, copied from
  //                     RuntimeLimits at create_entry time. A value
  //                     of 0 disables async fan-out entirely; the
  //                     bindings reject any op with
  //                     async.concurrency_limit.
  // async_result_size_limit_bytes: Carried for 0.3.5 hardening
  //                     (ICD-0.3.3 §Resource Limits item 1) — not
  //                     enforced in 0.3.3.
  std::vector<AsyncOp> pending_ops;
  std::unordered_map<int, PromiseCallbacks> callbacks;

  // ICD-0.5.2 §pubsub.subscribe JS Binding → Handler invocation on
  // dispatch. Persistent (many-shot) subscriber callback JSValues
  // keyed on the full logical channel. Distinct from `callbacks`
  // above: `callbacks` is 1-shot promise resolution (erased after
  // resolve/reject); `persistent_callbacks` entries live until the
  // subscription is explicitly unregistered, the bc is torn down,
  // or the broker's extension-drain evicts them. The broker's
  // `register_js_subscription` only tracks (bc, channel) pairs; the
  // JSValue handler itself lives here so its lifetime is bound to
  // bc's runtime (avoids the cross-thread refcount hazard that an
  // alternative broker-side JSValue store would introduce).
  std::unordered_map<std::string, JSValue> persistent_callbacks;

  int next_callback_id = 0;
  int concurrent_async_ops = 0;
  int max_concurrent_async_ops = 8;
  std::size_t async_result_size_limit_bytes = 16ULL * 1024ULL * 1024ULL;

  // --- Parallel-dispatch signaling (ICD-0.3.3.1) ---
  //
  // 0.3.3.1 replaces 0.3.3's serialized `co_await dispatch_async_op`
  // with fire-and-forget `drogon::async_run` per op. The outer
  // run_on_context coroutine awaits an AnyCompletionAwaiter that
  // fires whenever any detached task lands its queueInLoop callback
  // on the main loop. Contract:
  //   * inflight_detached — bumped at dispatch (main loop),
  //     decremented inside the queueInLoop callback that ran
  //     resolve()/reject(). Separate from `concurrent_async_ops`
  //     (back-pressure counter, decremented in resolve/reject
  //     itself). `inflight > 0` keeps the outer loop alive and
  //     guards cancellation-cascade teardown.
  //   * wake_mu + wake_count + waiter_handle — signaling surface
  //     between detached-completion callbacks (signal_completion)
  //     and the outer coroutine's AnyCompletionAwaiter. Mutex
  //     serializes the waiter-vs-signaler state transitions; both
  //     code paths are expected to run on the main loop thread,
  //     but the mutex makes the invariant explicit.
  std::atomic<int> inflight_detached{0};
  std::mutex wake_mu;
  int wake_count = 0;
  std::coroutine_handle<> waiter_handle;

  // ICD-0.5.3 §`db.batch()` Transactional Wrapper — per-bc batch
  // state. `depth` is bumped by the binding at db.batch entry and
  // decremented after COMMIT/ROLLBACK resolves. `scope_id` is the
  // monotonic allocation from the binding; mirrors the id carried
  // on every in-batch AsyncOp. `pinned_conn` holds the transaction
  // pointer the BEGIN dispatch arm opened; DB_QUERY/DB_EXEC dispatch
  // arms route through this when `op.batch_scope_id != 0`.
  // `ops_in_batch` counts enqueued ops for the quota check per
  // §Config Surface `max_ops_per_batch`.
  //
  // ICD-0.5.3 §B.06 — `timed_out` is set by the main-loop timer
  // armed at BEGIN finalize; read by the in-batch `db.exec` /
  // `db.query` enqueue path and by `__db_batch_commit__` to
  // reject inline with `db.batch.timeout`. `timer_id` + `timer_loop`
  // let the COMMIT/ROLLBACK finalize path disarm the timer so
  // subsequent batches on the same bc start with a clean slot.
  struct BatchState {
    int depth = 0;
    std::uint64_t scope_id = 0;
    std::shared_ptr<drogon::orm::Transaction> pinned_conn;
    std::size_t ops_in_batch = 0;
    // Mutated only on the main loop (timer fires via queueInLoop /
    // runAfter on `timer_loop`; db.exec / db.query / db.batch
    // bindings also execute on the main loop), so no atomicity
    // is needed. A `bc->batch_state = BatchState{}` reset at
    // COMMIT/ROLLBACK finalize clears the slot for the next batch.
    bool timed_out = false;
    trantor::TimerId timer_id = trantor::InvalidTimerId;
    trantor::EventLoop* timer_loop = nullptr;
  } batch_state;

  // Register a fresh promise capability under a new callback_id.
  // Takes ownership of `resolve_fn` and `reject_fn` (refcount-1
  // QuickJS values from JS_NewPromiseCapability — DO NOT JS_DupValue
  // before calling). `ns_for_cancellation` should be "db", "audit", or
  // "cap" so the cancellation cascade can issue the right reject code.
  // Returns the new callback_id.
  auto register_pending(JSValue resolve_fn, JSValue reject_fn,
                        std::string ns_for_cancellation) -> int;

  // Resolve the promise behind `callback_id` with `result`. Calls
  // JS_Call on the stored resolve callback, JS_FreeValue both
  // callbacks regardless of JS_Call's result, erases the map entry,
  // decrements concurrent_async_ops. If `callback_id` is not in the
  // map (cancelled-cascade abandon path) this is a no-op that emits
  // a debug log line; PROMISE_RESOLVE_AFTER_CANCEL surfaces from the
  // coroutine loop, not from here.
  auto resolve(int callback_id, const Json::Value& result) -> void;

  // Reject the promise behind `callback_id` per ICD §Promise
  // Rejection Shape. Symmetric ownership / abandon semantics with
  // resolve() above.
  auto reject(int callback_id, const PromiseRejection& err) -> void;

  // ICD-0.5.2 §pubsub.subscribe — resolve the 1-shot promise with
  // an arbitrary JSValue instead of a Json::Value. Takes ownership
  // of `value` (JS_FreeValue'd after JS_Call). The caller MUST be on
  // the bc's owning loop (JSValue construction + JS_Call are not
  // thread-safe). Same abandon semantics as `resolve` above: a
  // callback_id that was already cancelled out of `callbacks` is
  // silently dropped + `value` is still freed.
  auto resolve_with_js_value(int callback_id, JSValue value) -> void;

  // ICD-0.5.2 §pubsub.subscribe — many-shot invocation of the
  // persistent subscriber callback for `channel` with `arg` (marshalled
  // JSON → JSValue inside this call, so the broker can hand over a
  // Json::Value snapshot that is safe to copy across threads without
  // touching QuickJS state on the listener thread). Silent no-op when
  // `channel` is not in `persistent_callbacks` (subscriber was torn
  // down mid-dispatch; this can legitimately happen between the
  // broker's registry snapshot + the queueInLoop hop). Caller MUST be
  // on bc's owning loop.
  auto invoke_callback(const std::string& channel, const Json::Value& arg)
      -> void;

  // ICD-0.5.2 §pubsub.subscribe — free every persistent subscriber
  // JSValue under `persistent_callbacks` and clear the map. Called
  // from the bc-teardown path (RuntimePool slot eviction) alongside
  // `broker::drop_bc_subscriptions`. MUST be on bc's owning loop.
  auto drop_persistent_callbacks() -> void;

  // Move out the queue of ops enqueued since the previous call. The
  // returned vector is processed by the coroutine loop; new ops can
  // be pushed back into pending_ops while the loop is iterating —
  // they get drained on the next loop iteration.
  [[nodiscard]] auto take_pending_ops() -> std::vector<AsyncOp>;

  [[nodiscard]] auto has_pending_ops() const noexcept -> bool;

  // Total count of in-flight (already co_awaited) plus queued
  // (back-pressure-deferred) ops. Used by both back-pressure
  // decisions and the cancellation cascade's drain step.
  [[nodiscard]] auto pending_op_count() const noexcept -> int;

  // Signal one completion to any suspended AnyCompletionAwaiter.
  // Called from the main-loop queueInLoop callback of a detached
  // task after it invoked resolve/reject. If a waiter is suspended
  // we resume it; otherwise the signal is stored for the next
  // await. See the 0.3.3.1 wake-state comment above.
  auto signal_completion() -> void;

  // --- Timer bracket (§Timer Semantics) ---
  // Start of an execution phase: capture cpu_timer_start = now.
  auto resume_cpu_timer() noexcept -> void;
  // End of an execution phase: fold (now - cpu_timer_start) into
  // cpu_time_accumulated. Safe to call unpaired (no-op when
  // cpu_timer_start is the zero time_point).
  auto pause_cpu_timer() noexcept -> void;

  // Predicates evaluated mid-execution by the interrupt handler.
  // cpu_limit_exceeded() counts the currently-running phase via
  // (now - cpu_timer_start) on top of cpu_time_accumulated.
  [[nodiscard]] auto cpu_limit_exceeded() const noexcept -> bool;
  [[nodiscard]] auto wall_clock_exceeded() const noexcept -> bool;
};

} // namespace plinth::js
