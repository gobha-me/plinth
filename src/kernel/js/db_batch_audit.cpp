// SPDX-License-Identifier: MIT
//
// See header.

#include "kernel/js/db_batch_audit.hpp"

#include "kernel/logging.hpp"
#include "kernel/realtime/coalescer.hpp"

#include <json/value.h>

#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace plinth::js {

namespace {

struct RateEntry {
  std::chrono::steady_clock::time_point last_emit;
  std::size_t suppressed_since_last = 0;
};

constexpr std::size_t RATE_LIMIT_CAP = 64;

struct RateLimiter {
  std::mutex mu;
  std::list<std::string> order;
  std::unordered_map<std::string, RateEntry> entries;
  std::unordered_map<std::string, std::list<std::string>::iterator> positions;

  auto touch(const std::string& key) -> RateEntry& {
    if (auto it = positions.find(key); it != positions.end()) {
      order.splice(order.end(), order, it->second);
      return entries[key];
    }
    if (entries.size() >= RATE_LIMIT_CAP) {
      const auto& victim = order.front();
      entries.erase(victim);
      positions.erase(victim);
      order.pop_front();
    }
    order.push_back(key);
    positions[key] = std::prev(order.end());
    return entries[key];
  }
};

std::atomic<std::uint64_t> g_scope_counter{0};
std::atomic<std::size_t> g_window_ms{60000};
std::atomic<std::size_t> g_max_ops_per_batch{500};
std::atomic<std::size_t> g_timeout_ms{30000};
RateLimiter g_rate_committed;
RateLimiter g_rate_rolled_back;

// In-flight batch registry. Maps scope_id → (extension_name +
// optional wall-clock timer handle). The per-extension drain walks
// this map by extension_name; the discard paths invalidate any
// attached timer synchronously before dropping the entry so a
// pending `runAfter` callback cannot fire on a destroyed bc.
struct InFlightEntry {
  std::string extension_name;
  trantor::TimerId timer_id{trantor::InvalidTimerId};
  trantor::EventLoop* timer_loop{nullptr};
};
std::mutex g_in_flight_mu;
std::unordered_map<std::uint64_t, InFlightEntry> g_scope_to_ext;

auto emit_audit_with_rate_limit(RateLimiter& limiter, const std::string& key,
                                const std::string& action, Json::Value detail)
    -> void {
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  auto window =
      std::chrono::milliseconds{g_window_ms.load(std::memory_order_acquire)};
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> g(limiter.mu);
  auto& entry = limiter.touch(key);
  bool window_open = entry.last_emit.time_since_epoch().count() == 0 ||
                     (now - entry.last_emit) >= window;
  if (!window_open) {
    ++entry.suppressed_since_last;
    return;
  }

  std::size_t count_in_window = entry.suppressed_since_last + 1;
  entry.last_emit = now;
  entry.suppressed_since_last = 0;

  detail["count_in_window"] = static_cast<Json::UInt64>(count_in_window);
  detail["window_ms"] =
      static_cast<Json::UInt64>(g_window_ms.load(std::memory_order_relaxed));
  plinth::log::audit(action, detail, plinth::log::AuditCtx{});
}

} // namespace

auto alloc_batch_scope_id() -> std::uint64_t {
  // Increment pre-returning, so the first scope id is 1, not 0.
  // `0` is the sentinel for "not in a batch" on AsyncOp.
  return g_scope_counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

auto set_batch_audit_window_ms(std::size_t window_ms) -> void {
  g_window_ms.store(window_ms, std::memory_order_release);
}

auto set_batch_max_ops_per_batch(std::size_t max_ops) -> void {
  g_max_ops_per_batch.store(max_ops, std::memory_order_release);
}

auto batch_max_ops_per_batch() -> std::size_t {
  return g_max_ops_per_batch.load(std::memory_order_acquire);
}

auto set_batch_timeout_ms(std::size_t timeout_ms) -> void {
  g_timeout_ms.store(timeout_ms, std::memory_order_release);
}

auto batch_timeout_ms() -> std::size_t {
  return g_timeout_ms.load(std::memory_order_acquire);
}

auto audit_batch_committed(const std::string& extension_name,
                           std::size_t ops_in_batch) -> void {
  Json::Value detail{Json::objectValue};
  detail["extension"] = extension_name;
  detail["ops_in_batch"] = static_cast<Json::UInt64>(ops_in_batch);
  emit_audit_with_rate_limit(g_rate_committed, extension_name,
                             "db.batch.committed", std::move(detail));
}

auto audit_batch_rolled_back(const std::string& extension_name,
                             const std::string& reason,
                             const std::string& error_code) -> void {
  Json::Value detail{Json::objectValue};
  detail["extension"] = extension_name;
  detail["reason"] = reason;
  detail["error_code"] = error_code;
  emit_audit_with_rate_limit(g_rate_rolled_back, extension_name,
                             "db.batch.rolled_back", std::move(detail));
}

auto register_in_flight_batch(std::uint64_t scope_id,
                              const std::string& extension_name) -> void {
  if (scope_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> g(g_in_flight_mu);
  g_scope_to_ext[scope_id] = InFlightEntry{.extension_name = extension_name,
                                           .timer_id = trantor::InvalidTimerId,
                                           .timer_loop = nullptr};
}

auto unregister_in_flight_batch(std::uint64_t scope_id) -> void {
  if (scope_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> g(g_in_flight_mu);
  g_scope_to_ext.erase(scope_id);
}

auto set_batch_timer(std::uint64_t scope_id, trantor::TimerId timer_id,
                     trantor::EventLoop* loop) -> void {
  if (scope_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> g(g_in_flight_mu);
  if (auto it = g_scope_to_ext.find(scope_id); it != g_scope_to_ext.end()) {
    it->second.timer_id = timer_id;
    it->second.timer_loop = loop;
  }
}

auto clear_batch_timer(std::uint64_t scope_id) -> void {
  if (scope_id == 0) {
    return;
  }
  trantor::TimerId id = trantor::InvalidTimerId;
  trantor::EventLoop* loop = nullptr;
  {
    std::lock_guard<std::mutex> g(g_in_flight_mu);
    if (auto it = g_scope_to_ext.find(scope_id); it != g_scope_to_ext.end()) {
      id = it->second.timer_id;
      loop = it->second.timer_loop;
      it->second.timer_id = trantor::InvalidTimerId;
      it->second.timer_loop = nullptr;
    }
  }
  if (id != trantor::InvalidTimerId && loop != nullptr) {
    loop->invalidateTimer(id);
  }
}

auto discard_batches_for_extension(const std::string& extension_name)
    -> std::size_t {
  // Collect matching scope ids under the lock, then discard the
  // coalescer buckets outside — coalescer's own mutex must never
  // be acquired while holding `g_in_flight_mu`. Invalidate any
  // pending B.06 timer in the same pass so a late fire cannot
  // reach a torn-down bc.
  std::vector<std::uint64_t> matching;
  std::vector<std::pair<trantor::EventLoop*, trantor::TimerId>> timers;
  {
    std::lock_guard<std::mutex> g(g_in_flight_mu);
    for (const auto& [scope_id, entry] : g_scope_to_ext) {
      if (entry.extension_name == extension_name) {
        matching.push_back(scope_id);
        if (entry.timer_id != trantor::InvalidTimerId &&
            entry.timer_loop != nullptr) {
          timers.emplace_back(entry.timer_loop, entry.timer_id);
        }
      }
    }
    for (std::uint64_t sid : matching) {
      g_scope_to_ext.erase(sid);
    }
  }
  for (auto& [loop, id] : timers) {
    loop->invalidateTimer(id);
  }
  for (std::uint64_t sid : matching) {
    plinth::realtime::CoalescerRegistry::instance().discard_batch_scope(sid);
  }
  return matching.size();
}

auto discard_all_batches() -> std::size_t {
  std::vector<std::uint64_t> all;
  std::vector<std::pair<trantor::EventLoop*, trantor::TimerId>> timers;
  {
    std::lock_guard<std::mutex> g(g_in_flight_mu);
    all.reserve(g_scope_to_ext.size());
    for (const auto& [scope_id, entry] : g_scope_to_ext) {
      all.push_back(scope_id);
      if (entry.timer_id != trantor::InvalidTimerId &&
          entry.timer_loop != nullptr) {
        timers.emplace_back(entry.timer_loop, entry.timer_id);
      }
    }
    g_scope_to_ext.clear();
  }
  for (auto& [loop, id] : timers) {
    loop->invalidateTimer(id);
  }
  for (std::uint64_t sid : all) {
    plinth::realtime::CoalescerRegistry::instance().discard_batch_scope(sid);
  }
  return all.size();
}

auto reset_in_flight_batches_for_test() -> void {
  std::lock_guard<std::mutex> g(g_in_flight_mu);
  g_scope_to_ext.clear();
}

auto reset_batch_audit_for_test() -> void {
  {
    std::lock_guard<std::mutex> g(g_rate_committed.mu);
    g_rate_committed.entries.clear();
    g_rate_committed.order.clear();
    g_rate_committed.positions.clear();
  }
  {
    std::lock_guard<std::mutex> g(g_rate_rolled_back.mu);
    g_rate_rolled_back.entries.clear();
    g_rate_rolled_back.order.clear();
    g_rate_rolled_back.positions.clear();
  }
}

} // namespace plinth::js
