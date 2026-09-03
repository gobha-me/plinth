// SPDX-License-Identifier: MIT
//
// See header.

#include "kernel/js/db_search_path.hpp"

#include "kernel/logging.hpp"

#include <json/value.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace plinth::js::db {

namespace {

// Mirrors `db_silent_audit.cpp` — see that file for the rationale on
// duplicating the pattern over factoring. 64-entry LRU, fixed 60s
// aggregation window (search_path_set_failed is a rare pathology;
// no need for a config knob).
struct RateEntry {
  std::chrono::steady_clock::time_point last_emit;
  std::size_t suppressed_since_last = 0;
};

constexpr std::size_t RATE_LIMIT_CAP = 64;
constexpr std::chrono::seconds EMIT_WINDOW{60};

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

std::atomic<bool> g_enforce{true};
RateLimiter g_rate_limiter;

} // namespace

auto set_search_path_enforce(bool enforce) -> void {
  g_enforce.store(enforce, std::memory_order_release);
}

auto search_path_enforced() -> bool {
  return g_enforce.load(std::memory_order_acquire);
}

auto is_valid_extension_name(std::string_view name) -> bool {
  if (name.empty()) {
    return false;
  }
  if (name.front() < 'a' || name.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '_');
  });
}

auto audit_search_path_set_failed(const std::string& extension_name,
                                  const std::string& sqlstate) -> void {
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> g(g_rate_limiter.mu);
  auto& entry = g_rate_limiter.touch(extension_name);
  bool window_open = entry.last_emit.time_since_epoch().count() == 0 ||
                     (now - entry.last_emit) >= EMIT_WINDOW;

  if (!window_open) {
    ++entry.suppressed_since_last;
    return;
  }

  std::size_t count_in_window = entry.suppressed_since_last + 1;
  entry.last_emit = now;
  entry.suppressed_since_last = 0;

  Json::Value detail{Json::objectValue};
  detail["extension"] = extension_name;
  detail["sqlstate"] = sqlstate;
  detail["count_in_window"] = static_cast<Json::UInt64>(count_in_window);
  plinth::log::audit("db.search_path.set_failed", detail,
                     plinth::log::AuditCtx{});
}

auto reset_search_path_audit_for_test() -> void {
  std::lock_guard<std::mutex> g(g_rate_limiter.mu);
  g_rate_limiter.entries.clear();
  g_rate_limiter.order.clear();
  g_rate_limiter.positions.clear();
}

} // namespace plinth::js::db
