#include "kernel/realtime/cursor_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <list>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace plinth::realtime::cursor_store {

namespace {

struct CacheEntry {
  std::string user_id;
  std::int64_t last_seq{0};
  std::int64_t pg_persisted_seq{0};
  std::chrono::steady_clock::time_point last_flush;
  std::size_t pending_delta{0};
};

using EntryList = std::list<CacheEntry>;
using EntryMap = std::unordered_map<std::string, EntryList::iterator>;

// module-local cursor cache + config
std::mutex g_mu;
EntryList g_entries;
EntryMap g_index;
Config::Realtime::Events g_cfg;
drogon::orm::DbClientPtr g_test_db_client;
ClockFn g_clock_fn;

auto now() -> std::chrono::steady_clock::time_point {
  if (g_clock_fn) {
    return g_clock_fn();
  }
  return std::chrono::steady_clock::now();
}

auto db_client() -> drogon::orm::DbClientPtr {
  if (g_test_db_client) {
    return g_test_db_client;
  }
  return drogon::app().getDbClient();
}

// Snapshot of a cache entry to drive a PG write outside the lock.
// `co_await execSqlCoro` cannot run with `g_mu` held — the suspension
// would block other cache readers indefinitely.
struct FlushPlan {
  std::string user_id;
  std::int64_t last_seq;
};

// Returns a flush plan when the entry needs persisting; updates
// `pg_persisted_seq` + `last_flush` + `pending_delta` to reflect
// the about-to-be-issued UPSERT. Called under `g_mu`.
auto schedule_flush_locked(CacheEntry& e) -> FlushPlan {
  FlushPlan plan{.user_id = e.user_id, .last_seq = e.last_seq};
  e.pg_persisted_seq = e.last_seq;
  e.pending_delta = 0;
  e.last_flush = now();
  return plan;
}

auto upsert_advance(drogon::orm::DbClientPtr db, FlushPlan plan)
    -> drogon::Task<void> {
  try {
    co_await db->execSqlCoro(
        "INSERT INTO plinth.user_event_cursors (user_id, last_seq) "
        "VALUES ($1::uuid, $2) "
        "ON CONFLICT (user_id) DO UPDATE SET "
        "last_seq = GREATEST(plinth.user_event_cursors.last_seq, "
        "                     EXCLUDED.last_seq), "
        "updated_at = NOW()",
        plan.user_id, plan.last_seq);
  } catch (const drogon::orm::DrogonDbException& e) {
    spdlog::warn("realtime cursor_store: advance UPSERT failed for {} "
                 "(-> {}): {}",
                 plan.user_id, plan.last_seq, e.base().what());
  }
  co_return;
}

auto upsert_reset(drogon::orm::DbClientPtr db, FlushPlan plan)
    -> drogon::Task<void> {
  try {
    co_await db->execSqlCoro(
        "INSERT INTO plinth.user_event_cursors (user_id, last_seq) "
        "VALUES ($1::uuid, $2) "
        "ON CONFLICT (user_id) DO UPDATE SET "
        "last_seq = EXCLUDED.last_seq, "
        "updated_at = NOW()",
        plan.user_id, plan.last_seq);
  } catch (const drogon::orm::DrogonDbException& e) {
    spdlog::warn("realtime cursor_store: reset UPSERT failed for {} "
                 "(-> {}): {}",
                 plan.user_id, plan.last_seq, e.base().what());
  }
  co_return;
}

auto select_cursor(drogon::orm::DbClientPtr db, std::string user_id)
    -> drogon::Task<std::int64_t> {
  try {
    auto r = co_await db->execSqlCoro(
        "SELECT last_seq FROM plinth.user_event_cursors "
        "WHERE user_id = $1::uuid",
        user_id);
    if (r.empty()) {
      co_return 0;
    }
    co_return r[0]["last_seq"].as<std::int64_t>();
  } catch (const drogon::orm::DrogonDbException& e) {
    spdlog::warn("realtime cursor_store: SELECT failed for {}: {}", user_id,
                 e.base().what());
    co_return 0;
  }
}

} // namespace

auto configure(const Config::Realtime::Events& cfg) -> void {
  std::lock_guard lock(g_mu);
  g_cfg = cfg;
}

auto read_cursor(std::string user_id) -> drogon::Task<std::int64_t> {
  {
    std::lock_guard lock(g_mu);
    auto it = g_index.find(user_id);
    if (it != g_index.end()) {
      // LRU touch — move to front.
      g_entries.splice(g_entries.begin(), g_entries, it->second);
      co_return it->second->last_seq;
    }
  }
  auto db = db_client();
  if (!db) {
    co_return 0;
  }
  auto seq = co_await select_cursor(db, user_id);
  {
    std::lock_guard lock(g_mu);
    // Insert may race with concurrent reads; recheck.
    auto it = g_index.find(user_id);
    if (it == g_index.end()) {
      g_entries.push_front({.user_id = user_id,
                            .last_seq = seq,
                            .pg_persisted_seq = seq,
                            .last_flush = now(),
                            .pending_delta = 0});
      g_index[user_id] = g_entries.begin();
    }
  }
  co_return seq;
}

auto record_delivered(std::string user_id, std::int64_t new_seq)
    -> drogon::Task<void> {
  bool should_flush = false;
  FlushPlan plan{};
  {
    std::lock_guard lock(g_mu);
    auto it = g_index.find(user_id);
    if (it == g_index.end()) {
      g_entries.push_front({.user_id = user_id,
                            .last_seq = new_seq,
                            .pg_persisted_seq = 0,
                            .last_flush = now(),
                            .pending_delta = 1});
      g_index[user_id] = g_entries.begin();
    } else {
      g_entries.splice(g_entries.begin(), g_entries, it->second);
      CacheEntry& e = *it->second;
      if (new_seq <= e.last_seq) {
        // Out-of-order or stale advance — silently absorb.
        co_return;
      }
      e.last_seq = new_seq;
      e.pending_delta += 1;
    }
    CacheEntry& e = *g_index[user_id];
    bool ttl_expired = (g_cfg.cursor_cache_ttl_ms == 0) ||
                       (now() - e.last_flush >=
                        std::chrono::milliseconds(g_cfg.cursor_cache_ttl_ms));
    bool threshold_hit = (e.pending_delta >= g_cfg.cursor_flush_threshold);
    if (ttl_expired || threshold_hit) {
      plan = schedule_flush_locked(e);
      should_flush = true;
    }
  }
  if (should_flush) {
    auto db = db_client();
    if (db) {
      co_await upsert_advance(db, plan);
    }
  }
  co_return;
}

auto reset_cursor(std::string user_id, std::int64_t new_seq)
    -> drogon::Task<void> {
  {
    std::lock_guard lock(g_mu);
    auto it = g_index.find(user_id);
    if (it != g_index.end()) {
      CacheEntry& e = *it->second;
      e.last_seq = new_seq;
      e.pg_persisted_seq = new_seq;
      e.last_flush = now();
      e.pending_delta = 0;
    } else {
      g_entries.push_front({.user_id = user_id,
                            .last_seq = new_seq,
                            .pg_persisted_seq = new_seq,
                            .last_flush = now(),
                            .pending_delta = 0});
      g_index[user_id] = g_entries.begin();
    }
  }
  auto db = db_client();
  if (!db) {
    co_return;
  }
  co_await upsert_reset(
      db, FlushPlan{.user_id = std::move(user_id), .last_seq = new_seq});
}

auto flush_all_for_shutdown() -> drogon::Task<void> {
  std::vector<FlushPlan> plans;
  {
    std::lock_guard lock(g_mu);
    plans.reserve(g_entries.size());
    for (auto& e : g_entries) {
      if (e.pending_delta > 0) {
        plans.push_back(schedule_flush_locked(e));
      }
    }
  }
  if (plans.empty()) {
    co_return;
  }
  auto db = db_client();
  if (!db) {
    co_return;
  }
  for (const auto& plan : plans) {
    co_await upsert_advance(db, plan);
  }
}

auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void {
  std::lock_guard lock(g_mu);
  g_test_db_client = std::move(db);
}

auto clear_cache_for_test() -> void {
  std::lock_guard lock(g_mu);
  g_entries.clear();
  g_index.clear();
}

auto force_flush_for_test(std::string user_id) -> drogon::Task<void> {
  FlushPlan plan{};
  bool have_plan = false;
  {
    std::lock_guard lock(g_mu);
    auto it = g_index.find(user_id);
    if (it != g_index.end() && it->second->pending_delta > 0) {
      plan = schedule_flush_locked(*it->second);
      have_plan = true;
    }
  }
  if (!have_plan) {
    co_return;
  }
  auto db = db_client();
  if (!db) {
    co_return;
  }
  co_await upsert_advance(db, plan);
}

auto set_clock_for_test(ClockFn fn) -> void {
  std::lock_guard lock(g_mu);
  g_clock_fn = std::move(fn);
}

} // namespace plinth::realtime::cursor_store
