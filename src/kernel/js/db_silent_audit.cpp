// SPDX-License-Identifier: MIT
//
// See header.

#include "kernel/js/db_silent_audit.hpp"

#include "kernel/logging.hpp"

#include <json/value.h>

#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace plinth::js {

namespace {

// Per-extension rate-limit entry. Mirrors the eval_guard.cpp::RateEntry
// pattern (ICD-0.4.1 unicode scanner) and
// packages/validator.cpp::L1RateLimiter.
struct RateEntry {
  std::chrono::steady_clock::time_point last_emit;
  std::size_t suppressed_since_last = 0;
};

// Bounded LRU to guard against a malicious extension spamming distinct
// identity tokens and thrashing the table. 64 is the same cap the
// existing eval_guard RateLimiter uses.
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

// module-local audit-rate state
RateLimiter g_rate_limiter;
std::atomic<std::size_t> g_window_ms{60000};

} // namespace

auto record_silent_use(const std::string& extension_name) -> void {
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  auto window =
      std::chrono::milliseconds{g_window_ms.load(std::memory_order_acquire)};
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> g(g_rate_limiter.mu);
  auto& entry = g_rate_limiter.touch(extension_name);
  bool window_open = entry.last_emit.time_since_epoch().count() == 0 ||
                     (now - entry.last_emit) >= window;

  if (!window_open) {
    ++entry.suppressed_since_last;
    return;
  }

  // Window open → emit a fresh audit row. Count aggregates the
  // within-window suppressed hits (plus this one).
  std::size_t count_in_window = entry.suppressed_since_last + 1;
  entry.last_emit = now;
  entry.suppressed_since_last = 0;

  Json::Value detail{Json::objectValue};
  detail["extension"] = extension_name;
  detail["count_in_window"] = static_cast<Json::UInt64>(count_in_window);
  detail["window_ms"] =
      static_cast<Json::UInt64>(g_window_ms.load(std::memory_order_relaxed));
  plinth::log::audit("db.silent.used", detail, plinth::log::AuditCtx{});
}

auto set_silent_audit_window_ms(std::size_t window_ms) -> void {
  g_window_ms.store(window_ms, std::memory_order_release);
}

auto reset_silent_audit_for_test() -> void {
  std::lock_guard<std::mutex> g(g_rate_limiter.mu);
  g_rate_limiter.entries.clear();
  g_rate_limiter.order.clear();
  g_rate_limiter.positions.clear();
}

auto silent_audit_window_ms() -> std::size_t {
  return g_window_ms.load(std::memory_order_acquire);
}

} // namespace plinth::js
