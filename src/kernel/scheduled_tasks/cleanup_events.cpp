#include "kernel/scheduled_tasks/cleanup_events.hpp"

#include "kernel/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>

namespace plinth::scheduled_tasks::cleanup_events {

namespace {

// Hardcoded BIGINT lock key — pre-computed off-line so we don't pay
// the PG round-trip on every tick. Keeping the hash stable across
// kernel versions matters: a multi-node rolling deploy may run two
// nodes that each compute the lock key, and they MUST agree on the
// same BIGINT or the lock no longer single-instances the sweep.
//
// Passed as a string literal in the SQL rather than as a Drogon-bound
// parameter because Drogon's SqlBinder appears to send BIGINT in a
// binary format that PG rejects with "incorrect binary data format
// in bind parameter" on this query shape. Embedding the constant
// directly in the SQL string avoids the binder entirely; the value
// is compile-time fixed so there's no SQL-injection surface.
constexpr auto CLEANUP_LOCK_KEY_SQL =
    "4032023793113114077"; // 0x37F84B1E02C6A5DD

struct SlidingWindow {
  std::chrono::steady_clock::time_point first_ts;
  std::uint64_t count{0};
};

// module-local cleanup audit + test-seam state
std::mutex g_test_mu;
drogon::orm::DbClientPtr g_test_db_client;

std::mutex g_audit_mu;
std::unordered_map<std::string, SlidingWindow> g_audit_windows;

std::atomic<std::int64_t> g_last_swept{0};

auto db_client() -> drogon::orm::DbClientPtr {
  {
    std::lock_guard lock(g_test_mu);
    if (g_test_db_client) {
      return g_test_db_client;
    }
  }
  return drogon::app().getDbClient();
}

auto audit_event(std::string_view action, const Json::Value& detail,
                 std::size_t audit_window_ms) -> void {
  bool emit = false;
  std::uint64_t cnt = 0;
  {
    std::lock_guard lock(g_audit_mu);
    const std::string KEY{action};
    auto& win = g_audit_windows[KEY];
    const auto NOW = std::chrono::steady_clock::now();
    if (win.count == 0 ||
        NOW - win.first_ts > std::chrono::milliseconds(audit_window_ms)) {
      win.first_ts = NOW;
      win.count = 1;
      emit = true;
      cnt = 1;
    } else {
      win.count += 1;
      cnt = win.count;
    }
  }
  if (!emit || !plinth::log::is_audit_ready()) {
    return;
  }
  auto payload = detail;
  payload["count_in_window"] = static_cast<Json::UInt64>(cnt);
  payload["window_ms"] = static_cast<Json::UInt64>(audit_window_ms);
  plinth::log::audit(action, payload, plinth::log::AuditCtx{});
}

} // namespace

auto run(Config::Realtime::Events cfg) -> drogon::Task<void> {
  if (!cfg.enabled) {
    co_return;
  }
  auto db = db_client();
  if (!db) {
    co_return;
  }
  const auto STARTED_AT = std::chrono::steady_clock::now();
  std::int64_t deleted = 0;
  std::int64_t oldest_remain = 0;
  bool got_lock = false;
  std::string err;
  try {
    auto tx = co_await db->newTransactionCoro();
    // Transaction-scoped advisory lock — releases at COMMIT
    // regardless of pool churn. See file comment for rationale.
    auto lock_r = co_await tx->execSqlCoro(
        std::string{"SELECT pg_try_advisory_xact_lock("} +
        CLEANUP_LOCK_KEY_SQL + "::bigint) AS got");
    got_lock = lock_r[0]["got"].as<bool>();
    if (!got_lock) {
      // Peer holds the lock; another node will sweep this tick.
      // Silent skip per ICD §Advisory lock pseudocode.
      co_await tx->execSqlCoro("COMMIT");
      co_return;
    }
    auto del_r = co_await tx->execSqlCoro(
        "DELETE FROM plinth.events "
        "WHERE created_at < NOW() - INTERVAL '1 second' * " +
        std::to_string(cfg.retention_seconds) + "::int");
    deleted = static_cast<std::int64_t>(del_r.affectedRows());

    auto remain_r = co_await tx->execSqlCoro(
        "SELECT COALESCE(MIN(seq), 0) AS oldest FROM plinth.events");
    oldest_remain = remain_r[0]["oldest"].as<std::int64_t>();

    co_await tx->execSqlCoro("COMMIT");
  } catch (const drogon::orm::DrogonDbException& e) {
    err = e.base().what();
  }
  if (!err.empty()) {
    Json::Value payload(Json::objectValue);
    payload["reason"] = "cleanup_failed";
    payload["sqlstate"] = err;
    audit_event("realtime.events.write_failed", payload, cfg.audit_window_ms);
    co_return;
  }
  if (!got_lock) {
    co_return;
  }
  g_last_swept.store(deleted, std::memory_order_relaxed);
  const auto WALL_MS = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - STARTED_AT)
                           .count();
  Json::Value swept(Json::objectValue);
  swept["rows_deleted"] = static_cast<Json::Int64>(deleted);
  swept["sweep_duration_ms"] = static_cast<Json::Int64>(WALL_MS);
  swept["oldest_remaining_seq"] = static_cast<Json::Int64>(oldest_remain);
  audit_event("realtime.events.cleanup_swept", swept, cfg.audit_window_ms);
}

auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void {
  std::lock_guard lock(g_test_mu);
  g_test_db_client = std::move(db);
}

auto reset_audit_state_for_test() -> void {
  {
    std::lock_guard lock(g_audit_mu);
    g_audit_windows.clear();
  }
  g_last_swept.store(0, std::memory_order_relaxed);
}

auto last_swept_for_test() -> std::int64_t {
  return g_last_swept.load(std::memory_order_relaxed);
}

} // namespace plinth::scheduled_tasks::cleanup_events
