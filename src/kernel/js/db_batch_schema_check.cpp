// SPDX-License-Identifier: MIT
//
// See header.

#include "kernel/js/db_batch_schema_check.hpp"

#include "kernel/logging.hpp"

#include <json/value.h>

#include <chrono>
#include <list>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace plinth::js::db {

namespace {

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

RateLimiter g_rate_limiter;

// Compile once. `\b` word boundaries match the start/end of an
// `ext_<word>` token; the captured group is the suffix.
auto ext_regex() -> const std::regex& {
  static const std::regex EXT_RE{R"(\bext_([a-z0-9_]+)\b)",
                                 std::regex::optimize};
  return EXT_RE;
}

} // namespace

auto classify_cross_extension(std::string_view sql,
                              std::string_view expected_ext) -> bool {
  if (expected_ext.empty()) {
    return false;
  }
  std::string s{sql};
  std::sregex_iterator it{s.begin(), s.end(), ext_regex()};
  std::sregex_iterator end{};
  while (it != end) {
    const auto& match = *it;
    if (match.size() >= 2 && match[1].str() != expected_ext) {
      return true;
    }
    ++it;
  }
  return false;
}

auto audit_batch_cross_extension_rejected(const std::string& extension_name,
                                          const std::string& sql) -> void {
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
  // Cap SQL preview at 200 chars — full statement may carry sensitive
  // values; the audit row is for operator triage, not replay.
  constexpr std::size_t SQL_PREVIEW_CAP = 200;
  detail["sql_preview"] = sql.size() > SQL_PREVIEW_CAP
                              ? sql.substr(0, SQL_PREVIEW_CAP) + "..."
                              : sql;
  detail["count_in_window"] = static_cast<Json::UInt64>(count_in_window);
  plinth::log::audit("db.batch.cross_extension_rejected", detail,
                     plinth::log::AuditCtx{});
}

auto reset_cross_extension_audit_for_test() -> void {
  std::lock_guard<std::mutex> g(g_rate_limiter.mu);
  g_rate_limiter.entries.clear();
  g_rate_limiter.order.clear();
  g_rate_limiter.positions.clear();
}

} // namespace plinth::js::db
