#include "kernel/realtime/coalescer.hpp"

#include "kernel/logging.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/sql_classify.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <exception>
#include <functional>
#include <json/value.h>
#include <json/writer.h>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace plinth::realtime {

namespace {

// ── State ───────────────────────────────────────────────────────────

struct WindowKey {
  std::string schema;
  std::string table;
  auto operator==(const WindowKey&) const -> bool = default;
};

struct WindowKeyHash {
  auto operator()(const WindowKey& k) const -> std::size_t {
    const std::size_t H1 = std::hash<std::string>{}(k.schema);
    const std::size_t H2 = std::hash<std::string>{}(k.table);
    return H1 ^ (H2 + 0x9e3779b97f4a7c15ULL + (H1 << 6) + (H1 >> 2));
  }
};

struct WindowState {
  std::string schema;
  std::string table;
  std::chrono::steady_clock::time_point opened_at;
  std::size_t insert_rows{0};
  std::size_t update_rows{0};
  std::size_t delete_rows{0};
  std::string extension_name;
  trantor::TimerId timer_id{0};

  // ICD-0.5.5 §6 — coalesced_count is the number of upstream
  // record_write hits that would each have fired their own
  // pg_notify in the absence of coalescing. Bumped once per
  // record_write whose row_count > 0 (zero-row writes are
  // suppressed upstream per the WHERE-nomatch firehose guard, so
  // they neither trigger NOTIFY nor count toward the coalesced
  // total). Always ≥ 1 by construction once the envelope is
  // emitted (window only opens on a non-zero write).
  std::size_t notify_count{0};

  // ICD-0.5.5 §6 — window_open_ts_ms is the wall-clock ms-since-
  // epoch of the first hit in the window. Captured here at window
  // open so flush time can compute window_close_ts_ms =
  // open_ts_ms + window_ms without depending on system_clock at
  // emit time. Zero when the window has not yet been opened.
  std::int64_t window_open_ts_ms{0};
};

using WindowMap = std::unordered_map<WindowKey, WindowState, WindowKeyHash>;

// ICD §Coalescer State Machine — shared_mutex for future scale; in
// practice every access is under a unique_lock because record_write
// mutates counters. Kept as shared_mutex to match the ICD and leave
// headroom for a future read-heavy seam (metrics counters).
// module-local registry state
std::shared_mutex g_mu;
WindowMap g_windows;
Config::Realtime::Coalescer g_cfg;
std::atomic<bool> g_running{false};
std::atomic<bool> g_shutting_down{false};
std::mutex g_lifecycle_mu;
std::optional<trantor::EventLoopThread> g_loop_thread;

// ICD-0.5.3 §`db.batch()` §Coalescer interaction §scope_to_buckets_.
// Per-scope (schema, table) counter accumulation, distinct from the
// time-windowed `g_windows` map above. Lock-ordering: `g_scope_mu`
// is a leaf; never acquire `g_mu` while holding `g_scope_mu`.
std::mutex g_scope_mu;
std::unordered_map<std::uint64_t, WindowMap> g_scope_to_buckets;

// Test-only hook (nullable). Set/cleared under g_hook_mu; read under
// the same lock from flush().
std::mutex g_hook_mu;
CoalescerRegistry::EmitHook g_emit_hook;
// Test-only explicit DbClient. Bypasses `drogon::app().getDbClient()`
// so integration tests can exercise the real emit path without
// starting the full Drogon app.
drogon::orm::DbClientPtr g_test_db_client;

// ── Helpers ─────────────────────────────────────────────────────────

auto notify_error_name(NotifyError e) -> const char* {
  switch (e) {
    case NotifyError::MISSING_LAYER: return "MISSING_LAYER";
    case NotifyError::INVALID_CHANNEL: return "INVALID_CHANNEL";
    case NotifyError::LAYER_MISMATCH: return "LAYER_MISMATCH";
    case NotifyError::PAYLOAD_TOO_LARGE: return "PAYLOAD_TOO_LARGE";
    case NotifyError::PG_FAILURE: return "PG_FAILURE";
  }
  return "UNKNOWN";
}

auto build_envelope(const WindowState& w, std::size_t window_ms, bool truncated,
                    bool emit_superseded_seqs) -> Json::Value {
  Json::Value env(Json::objectValue);
  env["layer"] = "data";
  env["channel"] = "plinth:data:" + w.schema + "." + w.table;
  env["schema"] = w.schema;
  env["table"] = w.table;

  Json::Value ops(Json::arrayValue);
  const auto APPEND_OP = [&ops](const char* name, std::size_t count) {
    Json::Value entry(Json::objectValue);
    entry["op"] = name;
    entry["count"] = static_cast<Json::UInt64>(count);
    ops.append(std::move(entry));
  };
  APPEND_OP("insert", w.insert_rows);
  APPEND_OP("update", w.update_rows);
  APPEND_OP("delete", w.delete_rows);
  env["ops"] = std::move(ops);

  env["window_ms"] = static_cast<Json::UInt64>(window_ms);

  // ICD-0.5.5 §6 — reify the coalesce window for the SDK. Layer-1
  // envelopes always carry these three fields; Layer-2/3 envelopes
  // get the same shape stamped at writer-stamp time when absent.
  env["coalesced_count"] =
      static_cast<Json::UInt64>(w.notify_count == 0 ? 1U : w.notify_count);
  env["window_open_ts_ms"] = static_cast<Json::Int64>(w.window_open_ts_ms);
  env["window_close_ts_ms"] = static_cast<Json::Int64>(
      w.window_open_ts_ms + static_cast<std::int64_t>(window_ms));

  if (emit_superseded_seqs) {
    // ICD-0.5.5 §6 — `superseded_seqs[]` lists the seqs of
    // upstream events that did not get their own envelope. Under
    // the writer-first topology pinned at OQ1, the coalescer has
    // no source seqs to populate (`plinth.events.seq` is assigned
    // by the writer's INSERT … RETURNING clause, after the
    // coalescer has already emitted). The wire field ships here
    // as a stable empty array so SDK code can rely on its
    // presence; population is deferred to a follow-up ICD that
    // designs source-seq tracking compatible with writer-first.
    env["superseded_seqs"] = Json::Value(Json::arrayValue);
  }

  if (truncated) {
    env["truncated"] = true;
  }
  return env;
}

auto audit_flush_failed(const WindowState& w, std::string_view channel,
                        std::string_view reason) -> void {
  // During shutdown the normal audit path is closed to new work. Skip this
  // secondary diagnostic rather than re-entering the database while its
  // owner is draining; flush_snapshot still reports failure to the
  // coordinator so the dependency graph stops fail-closed.
  if (g_shutting_down.load() || !plinth::log::is_audit_ready()) {
    return;
  }
  Json::Value payload(Json::objectValue);
  payload["channel"] = std::string{channel};
  payload["schema"] = w.schema;
  payload["table"] = w.table;
  payload["reason"] = std::string{reason};
  Json::Value counts(Json::objectValue);
  counts["insert"] = static_cast<Json::UInt64>(w.insert_rows);
  counts["update"] = static_cast<Json::UInt64>(w.update_rows);
  counts["delete"] = static_cast<Json::UInt64>(w.delete_rows);
  payload["counts"] = std::move(counts);
  plinth::log::audit("realtime.coalescer.flush_failed", payload,
                     plinth::log::AuditCtx{});
}

// Truncation heuristic — always-three-entries counts-only envelope is
// tiny in 0.5.1 (O(200) bytes for any realistic schema/table), so this
// path is effectively never hit. The drop-on-oversize audit leaves the
// contract in place for 0.5.5 when `ids` arrives.
auto try_shrink_or_drop(const WindowState& w, std::string_view channel,
                        Json::Value& env) -> bool {
  auto size_of = [](const Json::Value& v) -> std::size_t {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v).size();
  };
  const std::size_t CEILING = plinth::realtime::get_max_payload_bytes();
  if (size_of(env) <= CEILING) {
    return true;
  }
  if (env.isMember("ids")) {
    env.removeMember("ids");
    env["truncated"] = true;
    if (size_of(env) <= CEILING) {
      return true;
    }
  }
  if (!g_shutting_down.load()) {
    spdlog::warn("realtime coalescer: envelope over ceiling; dropping. "
                 "channel={} size>{}",
                 channel, CEILING);
  }
  audit_flush_failed(w, channel, "payload_too_large");
  return false;
}

// Emit `env` via `emit_notify_async`, blocking the calling thread via
// drogon::sync_wait. Used by every flush path in 0.5.1 (timer fires,
// drain_extension, shutdown drain). The coalescer's dedicated loop
// absorbs the per-flush PG round-trip (single-digit ms typical); the
// drain paths already run under a caller whose 5 s drain budget
// accommodates this. Kept sync-only to avoid drogon::app()-loop
// dependencies at timer-callback fire time (tests construct a
// DbClient directly via newPgClient and wire it through the test
// seam — see set_db_client_for_test).
auto emit_result(const Json::Value& env) -> std::expected<void, NotifyError> {
  {
    std::lock_guard lock(g_hook_mu);
    if (g_emit_hook) {
      return g_emit_hook(env);
    }
  }
  drogon::orm::DbClientPtr db;
  {
    std::lock_guard lock(g_hook_mu);
    if (g_test_db_client) {
      db = g_test_db_client;
    }
  }
  if (!db) {
    db = drogon::app().getDbClient();
  }
  if (!db) {
    return std::unexpected(NotifyError::PG_FAILURE);
  }
  try {
    return drogon::sync_wait(plinth::realtime::emit_notify_async(db, env));
  } catch (const std::exception& e) {
    if (!g_shutting_down.load()) {
      spdlog::error("realtime coalescer: sync_wait threw: {}", e.what());
    }
    return std::unexpected(NotifyError::PG_FAILURE);
  }
}

// Extract + erase the WindowState at (schema, table) under the write
// lock. Returns nullopt if no window is open at the key. Cancels the
// associated trantor timer before returning (safe under shared_mutex
// but called with unique_lock). Caller builds + emits the envelope
// outside the lock to avoid holding it across a PG round-trip.
auto claim_window(const WindowKey& key) -> std::optional<WindowState> {
  std::unique_lock lock(g_mu);
  auto it = g_windows.find(key);
  if (it == g_windows.end()) {
    return std::nullopt;
  }
  WindowState w = std::move(it->second);
  g_windows.erase(it);
  if (w.timer_id != 0 && g_loop_thread.has_value()) {
    g_loop_thread->getLoop()->invalidateTimer(w.timer_id);
  }
  return w;
}

// Build the envelope, run the truncation check, and emit. Returns
// true when the emit completed successfully, false on any failure
// (oversize drop, emit error). On failure, a `flush_failed` audit is
// fired with the appropriate `reason`.
//
// `window_ms_override` is the `window_ms` value written into the
// envelope. Defaults to the config'd value for timer-fired flushes;
// ICD-0.5.3 §`db.batch()` §Coalescer interaction passes 0 to signal
// "emitted synchronously at batch commit" to downstream consumers.
auto flush_snapshot(const WindowState& w,
                    std::optional<std::size_t> window_ms_override = {},
                    std::optional<NotifyError>* err_out = nullptr) -> bool {
  std::size_t window_ms = window_ms_override.value_or(g_cfg.window_ms);
  const std::string CHANNEL = "plinth:data:" + w.schema + "." + w.table;
  // ICD-0.5.5 §6 — `coalesce.emit_superseded_seqs` lives under the
  // events block (`Config::Realtime::Events::coalesce.*`) per §10.
  // Pull it via the events_writer's snapshot of the events block so
  // the coalescer doesn't need its own copy of the toggle.
  const bool EMIT_SUPERSEDED = plinth::realtime::events_writer::current_config()
                                   .coalesce.emit_superseded_seqs;
  Json::Value env =
      build_envelope(w, window_ms, /*truncated=*/false, EMIT_SUPERSEDED);
  if (!try_shrink_or_drop(w, CHANNEL, env)) {
    if (err_out != nullptr) {
      *err_out = NotifyError::PAYLOAD_TOO_LARGE;
    }
    return false;
  }
  auto res = emit_result(env);
  if (!res) {
    if (err_out != nullptr) {
      *err_out = res.error();
    }
    audit_flush_failed(w, CHANNEL, notify_error_name(res.error()));
    return false;
  }
  return true;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

// ICD mandates a `CoalescerRegistry::instance().method(...)` shape;
// state lives in this TU's anonymous namespace so the methods read as
// non-static even though they technically could be.

auto CoalescerRegistry::instance() -> CoalescerRegistry& {
  static CoalescerRegistry self;
  return self;
}

auto CoalescerRegistry::start(const Config::Realtime::Coalescer& cfg) -> void {
  std::lock_guard lock(g_lifecycle_mu);
  if (g_running.load()) {
    return;
  }
  g_cfg = cfg;
  g_shutting_down.store(false);
  if (!cfg.enabled) {
    spdlog::info("realtime coalescer: disabled by config");
    g_running.store(true);
    return;
  }
  g_loop_thread.emplace();
  g_loop_thread->run();
  g_running.store(true);
  spdlog::info("realtime coalescer: started (window_ms={})", cfg.window_ms);
}

auto CoalescerRegistry::shutdown(std::chrono::milliseconds timeout) -> bool {
  std::lock_guard lock(g_lifecycle_mu);
  if (!g_running.load()) {
    return true;
  }
  g_shutting_down.store(true);
  if (!g_cfg.enabled) {
    g_running.store(false);
    return true;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  // Cancel every timer while the loop is still alive, then stop and join the
  // loop before claiming windows. A timer callback that was already dequeued
  // either claims and flushes its window before the join completes or observes
  // that shutdown claimed it afterward; no callback can outlive this barrier.
  {
    std::unique_lock map_lock(g_mu);
    for (auto& kv : g_windows) {
      auto& w = kv.second;
      if (w.timer_id != 0 && g_loop_thread.has_value()) {
        g_loop_thread->getLoop()->invalidateTimer(w.timer_id);
      }
      w.timer_id = 0;
    }
  }
  if (g_loop_thread.has_value()) {
    g_loop_thread->getLoop()->quit();
    g_loop_thread.reset(); // joins every accepted timer callback
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return false;
  }

  std::vector<WindowState> drained;
  {
    std::unique_lock map_lock(g_mu);
    drained.reserve(g_windows.size());
    for (auto& kv : g_windows) {
      auto& w = kv.second;
      drained.push_back(std::move(w));
    }
    g_windows.clear();
  }

  auto restore_from = [&drained](std::size_t first) {
    std::unique_lock map_lock(g_mu);
    for (std::size_t i = first; i < drained.size(); ++i) {
      auto& window = drained[i];
      WindowKey key{.schema = window.schema, .table = window.table};
      g_windows.insert_or_assign(std::move(key), std::move(window));
    }
  };
  for (std::size_t i = 0; i < drained.size(); ++i) {
    if (std::chrono::steady_clock::now() >= deadline ||
        !flush_snapshot(drained[i])) {
      restore_from(i);
      return false;
    }
  }

  g_running.store(false);

  return true;
}

auto CoalescerRegistry::record_write(std::string_view schema,
                                     std::string_view table, OpKind op_kind,
                                     std::size_t row_count,
                                     std::string_view extension_name,
                                     std::uint64_t batch_scope_id) -> void {
  if (!g_cfg.enabled || g_shutting_down.load()) {
    return;
  }
  // ICD-0.5.3 §`db.batch()` §Coalescer interaction — writes tagged
  // with a non-zero batch scope accumulate under a per-scope bucket
  // map. Counters flush only at `flush_batch_scope(scope_id)` (or
  // drop at `discard_batch_scope`). The existing 50 ms window timer
  // does NOT fire for scope-tagged writes.
  if (batch_scope_id != 0) {
    if (row_count == 0) {
      return;
    }
    std::unique_lock scope_lock(g_scope_mu);
    auto& buckets = g_scope_to_buckets[batch_scope_id];
    WindowKey key{.schema = std::string{schema}, .table = std::string{table}};
    auto [it, inserted] = buckets.try_emplace(key);
    WindowState& w = it->second;
    if (inserted) {
      w.schema = std::string{schema};
      w.table = std::string{table};
      w.opened_at = std::chrono::steady_clock::now();
      w.extension_name = std::string{extension_name};
      // ICD-0.5.5 §6 — capture wall-clock open ts for the SDK.
      w.window_open_ts_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
    }
    switch (op_kind) {
      case OpKind::INSERT: w.insert_rows += row_count; break;
      case OpKind::UPDATE: w.update_rows += row_count; break;
      case OpKind::DELETE: w.delete_rows += row_count; break;
    }
    // ICD-0.5.5 §6 OQ3 — bump the upstream-NOTIFY counter once
    // per record_write hit. Zero-row writes returned early above,
    // matching the WHERE-nomatch firehose suppression.
    w.notify_count += 1;
    return;
  }
  WindowKey key{.schema = std::string{schema}, .table = std::string{table}};
  std::unique_lock lock(g_mu);
  auto it = g_windows.find(key);
  if (it == g_windows.end()) {
    // Zero-row write on empty bucket: no envelope can carry useful
    // CRUD reactivity; skip to close the WHERE-nomatch firehose.
    if (row_count == 0) {
      return;
    }
    WindowState w;
    w.schema = std::string{schema};
    w.table = std::string{table};
    w.opened_at = std::chrono::steady_clock::now();
    w.extension_name = std::string{extension_name};
    // ICD-0.5.5 §6 — capture wall-clock open ts at first hit.
    w.window_open_ts_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    switch (op_kind) {
      case OpKind::INSERT: w.insert_rows = row_count; break;
      case OpKind::UPDATE: w.update_rows = row_count; break;
      case OpKind::DELETE: w.delete_rows = row_count; break;
    }
    // ICD-0.5.5 §6 OQ3 — count this first record_write hit as 1.
    w.notify_count = 1;
    if (g_loop_thread.has_value()) {
      // Schedule the flush timer on the dedicated loop. The
      // callback captures the key by value — the coalescer may
      // rehash as it grows, so iterators / pointers into the
      // map aren't stable across a lock drop.
      const double DELAY_S = static_cast<double>(g_cfg.window_ms) / 1000.0;
      auto* loop = g_loop_thread->getLoop();
      w.timer_id = loop->runAfter(DELAY_S, [key]() {
        if (auto claimed = claim_window(key); claimed.has_value()) {
          (void)flush_snapshot(*claimed);
        }
      });
    }
    g_windows.emplace(std::move(key), std::move(w));
    return;
  }
  // Accumulate into existing window.
  WindowState& w = it->second;
  if (!extension_name.empty() && !w.extension_name.empty() &&
      extension_name != w.extension_name) {
    spdlog::warn("realtime coalescer: cross-extension write "
                 "schema={}.{} window_owner={} incoming={} — keeping owner",
                 w.schema, w.table, w.extension_name, extension_name);
  }
  if (row_count == 0) {
    return; // open bucket + zero-row — counters unchanged
  }
  switch (op_kind) {
    case OpKind::INSERT: w.insert_rows += row_count; break;
    case OpKind::UPDATE: w.update_rows += row_count; break;
    case OpKind::DELETE: w.delete_rows += row_count; break;
  }
  // ICD-0.5.5 §6 OQ3 — bump the upstream-NOTIFY counter for each
  // additional record_write the window absorbs.
  w.notify_count += 1;
}

auto CoalescerRegistry::flush_batch_scope(std::uint64_t scope_id)
    -> std::size_t {
  if (scope_id == 0) {
    return 0;
  }
  WindowMap buckets;
  {
    std::lock_guard<std::mutex> g(g_scope_mu);
    auto it = g_scope_to_buckets.find(scope_id);
    if (it == g_scope_to_buckets.end()) {
      return 0;
    }
    buckets = std::move(it->second);
    g_scope_to_buckets.erase(it);
  }
  std::size_t emitted = 0;
  // `window_ms=0` sentinel per ICD §Coalescer interaction. Emit
  // off-lock to avoid blocking scope_id allocation on the emit path.
  for (const auto& [key, w] : buckets) {
    if (flush_snapshot(w, /*window_ms_override=*/0)) {
      ++emitted;
    }
  }
  return emitted;
}

auto CoalescerRegistry::discard_batch_scope(std::uint64_t scope_id)
    -> std::size_t {
  if (scope_id == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> g(g_scope_mu);
  auto it = g_scope_to_buckets.find(scope_id);
  if (it == g_scope_to_buckets.end()) {
    return 0;
  }
  std::size_t n = it->second.size();
  g_scope_to_buckets.erase(it);
  return n;
}

auto CoalescerRegistry::drain_extension(std::string_view extension_name)
    -> void {
  if (!g_running.load()) {
    return;
  }
  // Collect matching keys under the read lock, then flush each under
  // the normal claim path so the emit runs outside the lock.
  std::vector<WindowKey> matches;
  {
    std::shared_lock lock(g_mu);
    matches.reserve(g_windows.size());
    for (const auto& kv : g_windows) {
      if (kv.second.extension_name == extension_name) {
        matches.push_back(kv.first);
      }
    }
  }
  for (const auto& key : matches) {
    if (auto claimed = claim_window(key); claimed.has_value()) {
      (void)flush_snapshot(*claimed);
    }
  }
}

auto CoalescerRegistry::apply_flush_for_test(std::string_view schema,
                                             std::string_view table) -> bool {
  WindowKey key{.schema = std::string{schema}, .table = std::string{table}};
  auto claimed = claim_window(key);
  if (!claimed.has_value()) {
    return false;
  }
  return flush_snapshot(*claimed);
}

auto CoalescerRegistry::open_window_count_for_test() const -> std::size_t {
  std::shared_lock lock(g_mu);
  return g_windows.size();
}

auto CoalescerRegistry::set_emit_hook_for_test(EmitHook hook) -> void {
  std::lock_guard lock(g_hook_mu);
  g_emit_hook = std::move(hook);
}

auto CoalescerRegistry::clear_emit_hook_for_test() -> void {
  std::lock_guard lock(g_hook_mu);
  g_emit_hook = nullptr;
}

auto CoalescerRegistry::clear_windows_for_test() -> void {
  {
    std::unique_lock lock(g_mu);
    for (auto& kv : g_windows) {
      if (kv.second.timer_id != 0 && g_loop_thread.has_value()) {
        g_loop_thread->getLoop()->invalidateTimer(kv.second.timer_id);
      }
    }
    g_windows.clear();
  }
  // ICD-0.5.3 §`db.batch()` §scope_to_buckets — test seam also
  // drops accumulated batch scopes so each TEST_CASE starts clean.
  std::lock_guard<std::mutex> scope_lock(g_scope_mu);
  g_scope_to_buckets.clear();
}

auto CoalescerRegistry::set_db_client_for_test(drogon::orm::DbClientPtr db)
    -> void {
  std::lock_guard lock(g_hook_mu);
  g_test_db_client = std::move(db);
}

} // namespace plinth::realtime
