#include "kernel/realtime/events_writer.hpp"

#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/scheduled_tasks/cleanup_events.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <functional>
#include <iomanip>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <string_view>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace plinth::realtime::events_writer {

namespace {

// ── State ───────────────────────────────────────────────────────────

struct QueueEntry {
  DispatchedEvent ev;
  std::chrono::system_clock::time_point received_at;
};

struct SlidingWindow {
  std::chrono::steady_clock::time_point first_ts;
  std::uint64_t count{0};
};

// Test-only seams.
using InsertHook = std::function<std::expected<std::int64_t, std::string>(
    const std::string& /*channel*/, const Json::Value& /*envelope*/)>;
using LockHook = std::function<bool(const std::string& /*key*/)>;
// ICD-0.5.5 §14 — fires after the writer has stamped `ev.envelope["seq"]`
// from the RETURNING result, BEFORE the broker's `dispatch` call. S.01
// uses it to assert that the canonical seq is on the envelope by the
// time fan-out begins; S.06 uses it to inject a synthetic gap.
using PreBrokerHook = std::function<void(const DispatchedEvent& /*ev*/)>;

// module-local writer state, mirrors coalescer.cpp shape
std::mutex g_lifecycle_mu;
std::atomic<bool> g_running{false};
std::atomic<bool> g_shutting_down{false};
std::mutex g_config_mu;
Config::Realtime::Events g_cfg;
std::optional<trantor::EventLoopThread> g_loop_thread;
trantor::TimerId g_drain_timer{0};
trantor::TimerId g_cleanup_timer{0};

std::mutex g_queue_mu;
std::condition_variable g_queue_cv;
// conditionally-noexcept; clang-tidy-20 conservatively flags the static
// initializer. The default-allocator path here cannot throw.
std::deque<QueueEntry> g_queue;

// In-flight tracker for every coroutine launched from the writer's timers.
// Without this, a test
// that pins a private DbClient via `set_db_client_for_test(db)` can drop
// its last shared_ptr (via Harness destruction) while a coroutine is
// mid-await; the coroutine's local `db = db_client()` then becomes the
// last ref, and when the frame unwinds on the DbClient's IO thread the
// resulting `EventLoopThreadPool::~` join-self trips
// `Resource deadlock avoided`. See feedback_deterministic_teardown.md.
std::mutex g_inflight_mu;
std::condition_variable g_inflight_cv;
std::atomic<std::size_t> g_inflight_jobs{0};
// Serializes timer-side task admission against synchronous drains. A claimed
// queue entry must be visible as in-flight before a shutdown/test drain can
// decide that all asynchronous work is quiescent.
std::mutex g_dispatch_mu;

std::mutex g_audit_mu;
std::unordered_map<std::string, SlidingWindow> g_write_failed_windows;

// Test seams.
std::mutex g_test_mu;
drogon::orm::DbClientPtr g_test_db_client;
InsertHook g_insert_hook;
LockHook g_lock_hook;
PreBrokerHook g_pre_broker_hook;

// Process-wide success counter — Phase 3 seam for E.* assertions.
std::atomic<std::uint64_t> g_writes_persisted{0};

// ── Helpers ─────────────────────────────────────────────────────────

auto db_client() -> drogon::orm::DbClientPtr {
  {
    std::lock_guard lock(g_test_mu);
    if (g_test_db_client) {
      return g_test_db_client;
    }
  }
  return drogon::app().getDbClient();
}

// ISO-8601 UTC with millisecond precision: "YYYY-MM-DDTHH:MM:SS.fffZ".
auto format_emitted_at(std::chrono::system_clock::time_point tp)
    -> std::string {
  auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(tp - secs).count();
  std::time_t t = std::chrono::system_clock::to_time_t(secs);
  std::tm g{};
  // std::gmtime is not thread-safe but the *_r form is. clang-tidy can't always
  // tell.
  (void)gmtime_r(&t, &g);
  std::ostringstream os;
  os << std::put_time(&g, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
     << std::setfill('0') << ms << 'Z';
  return os.str();
}

// Lock-key composition pinned in plan §Phase 3 implementation sketch:
// hashtextextended(channel || '\0' || emitted_at_iso, 0) yields a
// single BIGINT for `pg_try_advisory_xact_lock(BIGINT)`. The PG-side
// computation is portable across nodes; doing it server-side avoids
// drift between what the C++ hash and PG hashes produce.
constexpr auto LOCK_KEY_SQL =
    "hashtextextended($1::text || '\\u0000' || $2::text, 0)";

// Audit emitter — rate-limited per (reason) sliding window. Mirrors
// `note_subscribe_denied` shape in broker.cpp:396-429.
auto audit_write_failed(std::string_view channel, std::string_view reason,
                        std::string_view sqlstate, std::size_t queue_depth)
    -> void {
  bool emit = false;
  std::uint64_t cnt = 0;
  {
    std::lock_guard lock(g_audit_mu);
    const std::string KEY{reason};
    auto& win = g_write_failed_windows[KEY];
    const auto NOW = std::chrono::steady_clock::now();
    if (win.count == 0 ||
        NOW - win.first_ts > std::chrono::milliseconds(g_cfg.audit_window_ms)) {
      win.first_ts = NOW;
      win.count = 1;
      emit = true;
      cnt = 1;
    } else {
      win.count += 1;
      cnt = win.count;
    }
  }
  if (!emit) {
    return;
  }
  if (g_shutting_down.load() || !plinth::log::is_audit_ready()) {
    // Mirrors coalescer.cpp:143 — audit + log pipelines unsafe
    // during shutdown; the counter still moves so operators can
    // see the burst via the next audit window.
    return;
  }
  Json::Value payload(Json::objectValue);
  if (!channel.empty()) {
    payload["channel"] = std::string{channel};
  }
  payload["reason"] = std::string{reason};
  if (!sqlstate.empty()) {
    payload["sqlstate"] = std::string{sqlstate};
  }
  if (reason == "queue_full") {
    payload["queue_depth_at_drop"] = static_cast<Json::UInt64>(queue_depth);
  }
  payload["count_in_window"] = static_cast<Json::UInt64>(cnt);
  payload["window_ms"] = static_cast<Json::UInt64>(g_cfg.audit_window_ms);
  plinth::log::audit("realtime.events.write_failed", payload,
                     plinth::log::AuditCtx{});
}

// ── INSERT path ─────────────────────────────────────────────────────

auto stamp_emitted_at(Json::Value& envelope, std::string_view iso) -> void {
  envelope["emitted_at"] = std::string{iso};
}

// ICD-0.5.5 §6 — stamp the coalesce-window wire fields on Layer-2/3
// envelopes (kernel system events + extension `pubsub.publish`) that
// bypass the coalescer entirely. When the field is already present
// (Layer-1 envelopes pre-stamped by the coalescer), leave it. When
// absent, default to coalesced_count=1 + window_open_ts_ms ==
// window_close_ts_ms == emitted_at_ms — matching ICD §6 §Layer-2/3
// envelope shape so SDK code can rely on field presence without a
// layer-conditional branch.
auto stamp_coalesce_fields(Json::Value& envelope, std::int64_t emitted_at_ms)
    -> void {
  if (!envelope.isMember("coalesced_count")) {
    envelope["coalesced_count"] = static_cast<Json::UInt64>(1);
  }
  if (!envelope.isMember("window_open_ts_ms")) {
    envelope["window_open_ts_ms"] = static_cast<Json::Int64>(emitted_at_ms);
  }
  if (!envelope.isMember("window_close_ts_ms")) {
    envelope["window_close_ts_ms"] = static_cast<Json::Int64>(emitted_at_ms);
  }
}

auto serialize_envelope(const Json::Value& env) -> std::string {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, env);
}

auto try_acquire_lock(const std::string& key) -> bool {
  LockHook hook;
  {
    std::lock_guard lock(g_test_mu);
    hook = g_lock_hook;
  }
  if (hook) {
    return hook(key);
  }
  // Real implementation: caller wraps the call site in a transaction
  // and runs `SELECT pg_try_advisory_xact_lock(<lock_key>)` against
  // the same connection as the subsequent INSERT. Returning true here
  // is the no-hook default for production paths that don't simulate
  // contention.
  return true;
}

// Run the INSERT through the test hook when set; otherwise issue the
// real INSERT against PG. The advisory lock is taken inside the same
// transaction as the INSERT so it releases at COMMIT/ROLLBACK
// regardless of which pooled connection processes the next coroutine.
auto insert_envelope(QueueEntry entry) -> drogon::Task<void> {
  auto& ev = entry.ev;
  auto iso = format_emitted_at(entry.received_at);
  stamp_emitted_at(ev.envelope, iso);
  // ICD-0.5.5 §6 — Layer-2/3 envelopes never went through the
  // coalescer and arrive here without `coalesced_count` /
  // `window_open_ts_ms` / `window_close_ts_ms`. Stamp the trivial
  // window so every persisted envelope has a uniform shape.
  const auto EMITTED_AT_MS =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          entry.received_at.time_since_epoch())
          .count();
  stamp_coalesce_fields(ev.envelope, EMITTED_AT_MS);

  InsertHook hook;
  {
    std::lock_guard lock(g_test_mu);
    hook = g_insert_hook;
  }

  // ── Test-hook arm ────────────────────────────────────────────────
  if (hook) {
    // Test-mode lock check: still go through `try_acquire_lock` so
    // E.07 can simulate the loser without a hook on the INSERT.
    const std::string LOCK_KEY = ev.channel + '\0' + iso;
    if (!try_acquire_lock(LOCK_KEY)) {
      co_return;
    }
    auto result = hook(ev.channel, ev.envelope);
    if (!result.has_value()) {
      audit_write_failed(ev.channel, "pg_error", result.error(),
                         /*queue_depth=*/0);
      co_return;
    }
    // ICD-0.5.5 §5 — mirror the production stamp + broker call so
    // synthetic-INSERT tests still exercise the writer-downstream
    // fan-out path. The hook returned the synthetic seq; stamp it,
    // fire the optional pre-broker test seam, then dispatch.
    const auto SEQ = result.value();
    ev.envelope["seq"] = static_cast<Json::Int64>(SEQ);
    PreBrokerHook pre_broker;
    {
      std::lock_guard tlock(g_test_mu);
      pre_broker = g_pre_broker_hook;
    }
    if (pre_broker) {
      pre_broker(ev);
    }
    plinth::realtime::broker::dispatch(ev);
    if (!ev.delivered_to_users.empty()) {
      for (const auto& user_id : ev.delivered_to_users) {
        co_await plinth::realtime::cursor_store::record_delivered(user_id, SEQ);
      }
    }
    g_writes_persisted.fetch_add(1, std::memory_order_relaxed);
    co_return;
  }

  // ── Production arm ───────────────────────────────────────────────
  auto db = db_client();
  if (!db) {
    audit_write_failed(ev.channel, "pg_error", "connection_unavailable",
                       /*queue_depth=*/0);
    co_return;
  }
  // ICD-0.5.3-style transaction wrapper — a Drogon Transaction object
  // pins one PG connection across BEGIN/COMMIT, so the advisory
  // xact-lock holds across the INSERT regardless of pool churn. The
  // destructor issues ROLLBACK when the transaction is abandoned
  // mid-way (exception thrown, or co_return without commit).
  std::string err;
  bool committed = false;
  try {
    auto tx = co_await db->newTransactionCoro();
    // §HA — per-envelope advisory xact-lock. PG releases at
    // COMMIT/ROLLBACK regardless of which pooled connection runs
    // the next coroutine.
    auto lock_q = std::string{"SELECT pg_try_advisory_xact_lock("} +
                  LOCK_KEY_SQL + ") AS got";
    auto lock_r = co_await tx->execSqlCoro(lock_q, ev.channel, iso);
    const bool GOT = lock_r[0]["got"].as<bool>();
    if (!GOT) {
      // §HA — losers skip silently. Transaction destructor issues
      // ROLLBACK (no audit, no error per ICD).
      co_return;
    }
    auto serialized = serialize_envelope(ev.envelope);
    auto ins =
        co_await tx->execSqlCoro("INSERT INTO plinth.events (channel, payload) "
                                 "VALUES ($1, $2::jsonb) RETURNING seq",
                                 ev.channel, serialized);
    // Drogon's Transaction needs explicit COMMIT to flush the
    // INSERT (destructor under sync_wait can race the test query).
    // See run_on_context.cpp:841 for the canonical pattern.
    co_await tx->execSqlCoro("COMMIT");
    // ICD-0.5.5 §5 — stamp the canonical envelope `seq` from the
    // RETURNING result, fire the optional pre-broker test seam,
    // then invoke `broker::dispatch` so the WS + JS fan-out arms
    // see the canonical seq AND `ev.delivered_to_users` becomes
    // populated for the cursor advance below. The 0.5.4 ordering
    // (cursor advance after dispatch) is preserved; the only
    // change is that dispatch now happens here rather than in a
    // peer listener handler.
    if (!ins.empty()) {
      const auto SEQ = ins[0]["seq"].as<std::int64_t>();
      ev.envelope["seq"] = static_cast<Json::Int64>(SEQ);
      PreBrokerHook pre_broker;
      {
        std::lock_guard tlock(g_test_mu);
        pre_broker = g_pre_broker_hook;
      }
      if (pre_broker) {
        pre_broker(ev);
      }
      plinth::realtime::broker::dispatch(ev);
      // ICD-0.5.4 §When `record_delivered` fires — fire-and-forget
      // cursor advance for every user the broker matched in
      // `publish_dispatched`'s pre-pass. Failures inside
      // cursor_store audit `cursor_read_failed` and never raise.
      if (!ev.delivered_to_users.empty()) {
        for (const auto& user_id : ev.delivered_to_users) {
          co_await plinth::realtime::cursor_store::record_delivered(user_id,
                                                                    SEQ);
        }
      }
    }
    committed = true;
  } catch (const drogon::orm::DrogonDbException& e) {
    err = e.base().what();
  }
  if (committed) {
    g_writes_persisted.fetch_add(1, std::memory_order_relaxed);
  } else if (!err.empty()) {
    audit_write_failed(ev.channel, "pg_error", err, /*queue_depth=*/0);
  }
  co_return;
}

auto finish_inflight_job() noexcept -> void {
  {
    std::lock_guard lock(g_inflight_mu);
    g_inflight_jobs.fetch_sub(1, std::memory_order_acq_rel);
  }
  g_inflight_cv.notify_all();
}

struct InflightJobCompletion {
  InflightJobCompletion() {
    g_inflight_jobs.fetch_add(1, std::memory_order_acq_rel);
  }
  ~InflightJobCompletion() { finish_inflight_job(); }
};

// Drain loop body — runs on `g_loop_thread`. Pops one entry, runs the
// INSERT coroutine, repeats while the queue is non-empty.
auto drain_one() -> void {
  std::lock_guard dispatch_lock(g_dispatch_mu);
  if (g_shutting_down.load(std::memory_order_acquire) ||
      g_inflight_jobs.load(std::memory_order_acquire) != 0) {
    return;
  }
  std::optional<QueueEntry> entry;
  std::shared_ptr<InflightJobCompletion> completion;
  {
    std::lock_guard lock(g_queue_mu);
    if (!g_queue.empty()) {
      // Claim in-flight ownership while holding the queue lock. Shutdown and
      // the synchronous test drain both use this lock to decide that the queue
      // is empty; registering later would leave a gap where they could observe
      // neither queued nor in-flight ownership and release the DbClient early.
      completion = std::make_shared<InflightJobCompletion>();
      entry = std::move(g_queue.front());
      g_queue.pop_front();
    }
  }
  if (!entry.has_value()) {
    return;
  }
  try {
    drogon::async_run(
        // `e` is a self-contained QueueEntry owned by the lambda; no
        // out-of-scope references after the move into the coroutine frame.
        [e = std::move(*entry),
         completion = std::move(completion)]() mutable -> drogon::Task<> {
          co_await insert_envelope(std::move(e));
        });
  } catch (...) {
    // bad_weak_ptr if drogon's primary loop isn't initialized
    // (test-mode subprocess); without a catch the exception escapes
    // drain_one's runEvery callback, trantor's EventLoop::loop catches
    // it at EventLoop.cc:245, then rethrows past loopFuncs's noexcept
    // boundary → std::terminate.
  }
}

auto schedule_drain_timer() -> void {
  if (!g_loop_thread.has_value()) {
    return;
  }
  auto* loop = g_loop_thread->getLoop();
  // Light-weight 5 ms tick; coalescer uses runAfter per window, but a
  // queue drain is best served by a steady poll. Each tick processes
  // one entry to keep the per-tick cost bounded.
  g_drain_timer = loop->runEvery(0.005, []() {
    // trantor's EventLoop catches exceptions from runEvery callbacks
    // and rethrows past `loopFuncs`'s noexcept boundary
    // (EventLoop.cc:263 → std::terminate). Test-mode subprocesses
    // can hit this when `drain_one`'s `drogon::async_run` fires
    // before drogon's primary loop is initialized; swallow at the
    // callback boundary so the writer's loop survives.
    try {
      drain_one();
    } catch (...) {
    }
  });
}

// ICD-0.5.4 §Cleanup Task — runs on the writer's loop via runEvery.
// Per `architecture/04-services-ha.md §2`, the proper scheduled-tasks
// subsystem ships in 0.7.2; this riding-the-writer's-loop arrangement
// migrates trivially when 0.7 lands.
auto schedule_cleanup_timer() -> void {
  if (!g_loop_thread.has_value()) {
    return;
  }
  auto* loop = g_loop_thread->getLoop();
  const double INTERVAL_S =
      static_cast<double>(g_cfg.cleanup_interval_ms) / 1000.0;
  g_cleanup_timer = loop->runEvery(INTERVAL_S, []() {
    // Same containment as the drain_timer callback — drogon's
    // async_run can throw in test-mode subprocesses without a
    // primary loop, and the runEvery callback must not propagate
    // exceptions into trantor's loop.
    try {
      std::lock_guard dispatch_lock(g_dispatch_mu);
      if (g_shutting_down.load(std::memory_order_acquire) ||
          g_inflight_jobs.load(std::memory_order_acquire) != 0) {
        return;
      }
      auto cfg_snap = g_cfg;
      auto completion = std::make_shared<InflightJobCompletion>();
      drogon::async_run(
          // — cfg_snap captured by value; coroutine is self-contained.
          [cfg_snap,
           completion = std::move(completion)]() mutable -> drogon::Task<> {
            co_await plinth::scheduled_tasks::cleanup_events::run(cfg_snap);
          });
    } catch (...) {
    }
  });
}

// ── Listener handler ────────────────────────────────────────────────

auto handler(const DispatchedEvent& ev) -> void {
  if (!g_running.load() || g_shutting_down.load()) {
    return;
  }
  std::size_t depth_at_drop = 0;
  bool dropped = false;
  {
    std::lock_guard lock(g_queue_mu);
    if (g_queue.size() >= g_cfg.write_queue_size) {
      // §OQ1 — drop-newest. Just-arrived entry is rejected; older
      // entries stay queued.
      depth_at_drop = g_queue.size();
      dropped = true;
    } else {
      g_queue.push_back(
          {.ev = ev, .received_at = std::chrono::system_clock::now()});
      g_queue_cv.notify_one();
    }
  }
  if (dropped) {
    audit_write_failed(ev.channel, "queue_full", /*sqlstate=*/"",
                       depth_at_drop);
  }
}

// ── Bounded shutdown drain ──────────────────────────────────────────

auto drain_until(std::chrono::steady_clock::time_point deadline) -> bool {
  while (true) {
    std::optional<QueueEntry> entry;
    {
      std::lock_guard lock(g_queue_mu);
      if (g_queue.empty()) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        // Preserve the remainder for a retry. The coordinator keeps the DB
        // and writer loop alive when this returns false.
        const std::size_t left = g_queue.size();
        audit_write_failed(/*channel=*/"", "shutdown_timeout",
                           /*sqlstate=*/"",
                           /*queue_depth=*/left);
        return false;
      }
      entry = std::move(g_queue.front());
      g_queue.pop_front();
    }
    // The coordinator keeps Drogon's database and event loops alive here;
    // sync_wait makes each accepted write a synchronous drain barrier.
    try {
      drogon::sync_wait(insert_envelope(std::move(*entry)));
    } catch (...) {
      // the audit pipeline upstream of the throw already logged.
    }
  }
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

auto start(const Config::Realtime::Events& cfg) -> void {
  std::lock_guard lock(g_lifecycle_mu);
  if (g_running.load()) {
    return;
  }
  {
    std::lock_guard config_lock(g_config_mu);
    g_cfg = cfg;
  }
  g_shutting_down.store(false);
  {
    std::lock_guard alock(g_audit_mu);
    g_write_failed_windows.clear();
  }
  if (!cfg.enabled) {
    spdlog::info("realtime events writer: disabled by config");
    g_running.store(true);
    return;
  }
  {
    std::lock_guard q_lock(g_queue_mu);
    g_queue.clear();
  }
  plinth::realtime::cursor_store::configure(cfg);
  g_loop_thread.emplace();
  g_loop_thread->run();
  schedule_drain_timer();
  schedule_cleanup_timer();
  plinth::realtime::register_handler(&handler);
  g_running.store(true);
  spdlog::info("realtime events writer: started "
               "(write_queue_size={}, retention_seconds={})",
               cfg.write_queue_size, cfg.retention_seconds);
}

auto current_config() -> Config::Realtime::Events {
  // Broker fan-out may request a config snapshot from an insert coroutine
  // while stop() holds g_lifecycle_mu and waits for that coroutine. Keep the
  // immutable runtime snapshot on its own lock to avoid that cycle.
  std::lock_guard lock(g_config_mu);
  return g_cfg;
}

auto stop(std::chrono::milliseconds timeout) -> bool {
  std::lock_guard lock(g_lifecycle_mu);
  if (!g_running.load()) {
    return true;
  }
  g_shutting_down.store(true);
  g_queue_cv.notify_all();

  if (!g_cfg.enabled) {
    g_running.store(false);
    return true;
  }

  const auto configured = std::chrono::milliseconds{g_cfg.shutdown_drain_ms};
  const auto deadline =
      std::chrono::steady_clock::now() + std::min(timeout, configured);

  if (g_loop_thread.has_value() && g_drain_timer != 0) {
    g_loop_thread->getLoop()->invalidateTimer(g_drain_timer);
    g_drain_timer = 0;
  }
  if (g_loop_thread.has_value() && g_cleanup_timer != 0) {
    g_loop_thread->getLoop()->invalidateTimer(g_cleanup_timer);
    g_cleanup_timer = 0;
  }

  {
    // Exclude timer-side admission, then wait for every task it already
    // claimed before processing the remaining queue synchronously. This keeps
    // inserts serial and closes the dequeue/in-flight observation gap before
    // the coordinator releases the DbClient.
    std::lock_guard dispatch_lock(g_dispatch_mu);
    {
      std::unique_lock inflight_lock(g_inflight_mu);
      if (!g_inflight_cv.wait_until(inflight_lock, deadline, [] {
            return g_inflight_jobs.load(std::memory_order_acquire) == 0;
          })) {
        return false;
      }
    }
    if (!drain_until(deadline)) {
      return false;
    }
  }

  // ICD-0.5.4 §Cursor cache flush at shutdown — flush every cached
  // cursor delta to PG before the loop tears down so reconnects post-
  // restart see the most recent persisted seq. Bounded by the same
  // shutdown drain budget; sync_wait blocks the calling thread.
  if (std::chrono::steady_clock::now() >= deadline) {
    return false;
  }
  try {
    drogon::sync_wait(plinth::realtime::cursor_store::flush_all_for_shutdown());
  } catch (...) {
  }

  if (g_loop_thread.has_value()) {
    g_loop_thread->getLoop()->quit();
    g_loop_thread.reset(); // joins thread
  }
  g_running.store(false);
  return true;
}

auto is_running_for_test() -> bool {
  return g_running.load();
}

auto enqueue_for_test(DispatchedEvent ev) -> bool {
  if (!g_running.load() || g_shutting_down.load() || !g_cfg.enabled) {
    return false;
  }
  std::lock_guard lock(g_queue_mu);
  if (g_queue.size() >= g_cfg.write_queue_size) {
    return false;
  }
  g_queue.push_back(
      {.ev = std::move(ev), .received_at = std::chrono::system_clock::now()});
  g_queue_cv.notify_one();
  return true;
}

auto queue_size_for_test() -> std::size_t {
  std::lock_guard lock(g_queue_mu);
  return g_queue.size();
}

auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void {
  std::lock_guard lock(g_test_mu);
  g_test_db_client = std::move(db);
}

auto set_insert_hook_for_test(
    std::function<std::expected<std::int64_t, std::string>(const std::string&,
                                                           const Json::Value&)>
        hook) -> void {
  std::lock_guard lock(g_test_mu);
  g_insert_hook = std::move(hook);
}

auto clear_insert_hook_for_test() -> void {
  std::lock_guard lock(g_test_mu);
  g_insert_hook = nullptr;
}

auto set_advisory_lock_hook_for_test(
    std::function<bool(const std::string&)> hook) -> void {
  std::lock_guard lock(g_test_mu);
  g_lock_hook = std::move(hook);
}

auto clear_advisory_lock_hook_for_test() -> void {
  std::lock_guard lock(g_test_mu);
  g_lock_hook = nullptr;
}

auto set_pre_broker_hook_for_test(
    std::function<void(const DispatchedEvent&)> hook) -> void {
  std::lock_guard lock(g_test_mu);
  g_pre_broker_hook = std::move(hook);
}

auto clear_pre_broker_hook_for_test() -> void {
  std::lock_guard lock(g_test_mu);
  g_pre_broker_hook = nullptr;
}

auto apply_drain_for_test() -> void {
  // Exclude timer-side admission and wait for any entry it already claimed.
  // The background timer keeps firing after this function returns; while this
  // lock is held it cannot pluck an entry from the synchronous snapshot.
  std::lock_guard dispatch_lock(g_dispatch_mu);
  {
    std::unique_lock inflight_lock(g_inflight_mu);
    g_inflight_cv.wait(inflight_lock, [] {
      return g_inflight_jobs.load(std::memory_order_acquire) == 0;
    });
  }
  std::deque<QueueEntry> snapshot;
  {
    std::lock_guard lock(g_queue_mu);
    snapshot = std::move(g_queue);
    g_queue.clear();
  }
  while (!snapshot.empty()) {
    auto entry = std::move(snapshot.front());
    snapshot.pop_front();
    drogon::sync_wait(insert_envelope(std::move(entry)));
  }
}

auto writes_persisted_for_test() -> std::uint64_t {
  return g_writes_persisted.load(std::memory_order_relaxed);
}

auto reset_counters_for_test() -> void {
  g_writes_persisted.store(0, std::memory_order_relaxed);
  {
    std::lock_guard alock(g_audit_mu);
    g_write_failed_windows.clear();
  }
}

} // namespace plinth::realtime::events_writer
