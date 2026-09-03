#include "kernel/js/eval_guard.hpp"

#include "kernel/logging.hpp"
#include "kernel/security/unicode_scanner.hpp"

#include <json/value.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace plinth::js {

namespace {

// ─── Process-global scanner policy ─────────────────────────────────
//
// Per DESIGN §5.1 the scanner config is uniform across the kernel.
// Atomics are written once at kernel boot from the loaded plinth::Config
// and read on every JS_Eval. Defaults match plinth::Config so the gate
// stays secure even if the kernel main forgets to wire boot-time policy
// load (or if a test uses pre_eval_scan without bootstrap).
// process-wide scanner policy; std::atomic so visibility under the multi-thread
// JS_Eval call sites is well-defined.
std::atomic<bool> g_enabled{true};
// process-wide scanner policy; std::atomic for the same reason as g_enabled
// above.
std::atomic<std::size_t> g_threshold{50};
// process-wide scanner policy; std::atomic for the same reason as g_enabled
// above.
std::atomic<bool> g_log_findings{true};

auto current_policy() -> plinth::security::UnicodeScanConfig {
  return plinth::security::UnicodeScanConfig{
      .threshold = g_threshold.load(std::memory_order_acquire),
      .record_first_n = 5,
      .strict_utf8 = true,
  };
}

// ─── Per-(label, source_path) rate-limit LRU ──────────────────────
//
// 1 Hz token bucket per ICD §Audit / Rate-limit policy. Multi-thread
// safe: pre_eval_scan runs on Drogon IO threads (via run_on_context
// coroutines) and on RuntimePool consumer threads, so the LRU must
// carry its own mutex. 64-entry cap; LRU eviction protects against an
// adversary spamming distinct labels to thrash the table.
constexpr std::size_t RATE_LIMIT_CAP = 64;
constexpr std::chrono::seconds EMIT_WINDOW{1};

struct RateEntry {
  std::chrono::steady_clock::time_point last_emit;
  std::size_t suppressed_since_last = 0;
};

struct RateLimiter {
  std::mutex mu;
  std::list<std::string> order; // LRU front=oldest
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

// process-wide audit-emit rate limit; threadsafe via internal mutex (Layer 2
// runs on Drogon IO threads + RuntimePool consumer threads).
RateLimiter g_rate_limiter;

// build_audit_detail: shape per ICD §Audit / Event schemas.
auto build_audit_detail(std::string_view layer, std::string_view source_path,
                        const plinth::security::UnicodeScanResult& r,
                        std::size_t threshold) -> Json::Value {
  Json::Value d{Json::objectValue};
  d["layer"] = std::string{layer};
  d["source_path"] = std::string{source_path};
  d["total_count"] = static_cast<Json::UInt64>(r.total_count);
  d["threshold"] = static_cast<Json::UInt64>(threshold);
  Json::Value findings{Json::arrayValue};
  for (const auto& f : r.first_findings) {
    Json::Value entry{Json::objectValue};
    entry["byte_offset"] = static_cast<Json::UInt64>(f.byte_offset);
    entry["codepoint"] = static_cast<Json::UInt64>(f.codepoint);
    entry["range_name"] = std::string{f.range_name};
    findings.append(entry);
  }
  d["first_findings"] = findings;
  if (r.decode_error.has_value()) {
    d["decode_error"] = *r.decode_error;
  } else {
    d["decode_error"] = Json::Value{Json::nullValue};
  }
  return d;
}

auto rate_key(std::string_view layer, std::string_view source_path)
    -> std::string {
  std::string k{layer};
  k += '|';
  k += source_path;
  return k;
}

// emit_with_rate_limit: emits the per-detection event if the bucket
// allows; otherwise increments the suppressed counter. When the bucket
// next opens, emits a single rate-limited event citing the suppressed
// count. Always-on spdlog::warn fires regardless (developer-diagnostic;
// audit is compliance).
auto emit_with_rate_limit(std::string_view layer, std::string_view source_path,
                          const plinth::security::UnicodeScanResult& r,
                          std::size_t threshold) -> void {
  plinth::log::warn(
      "unicode-smuggle detected: layer={} path={} count={} threshold={}",
      std::string{layer}, std::string{source_path}, r.total_count, threshold);

  if (!g_log_findings.load(std::memory_order_acquire)) {
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto key = rate_key(layer, source_path);
  std::lock_guard<std::mutex> g(g_rate_limiter.mu);
  auto& entry = g_rate_limiter.touch(key);
  bool window_open = entry.last_emit.time_since_epoch().count() == 0 ||
                     (now - entry.last_emit) >= EMIT_WINDOW;
  if (!window_open) {
    ++entry.suppressed_since_last;
    return;
  }
  if (entry.suppressed_since_last > 0) {
    Json::Value rl{Json::objectValue};
    rl["layer"] = std::string{layer};
    rl["source_path"] = std::string{source_path};
    rl["suppressed_count"] =
        static_cast<Json::UInt64>(entry.suppressed_since_last);
    plinth::log::audit("security.unicode_smuggle_rate_limited", rl,
                       plinth::log::AuditCtx{});
    entry.suppressed_since_last = 0;
  }
  plinth::log::audit("security.unicode_smuggle_detected",
                     build_audit_detail(layer, source_path, r, threshold),
                     plinth::log::AuditCtx{});
  entry.last_emit = now;
}

auto format_eval_error_message(std::string_view source_label,
                               const plinth::security::UnicodeScanResult& r,
                               std::size_t threshold) -> std::string {
  std::string s = "unicode-smuggle: ";
  s += source_label;
  s += " ";
  if (r.decode_error.has_value()) {
    s += "failed UTF-8 decode (";
    s += *r.decode_error;
    s += ")";
    return s;
  }
  s += "contains ";
  s += std::to_string(r.total_count);
  s += " invisible Unicode characters (threshold ";
  s += std::to_string(threshold);
  s += ")";
  if (!r.first_findings.empty()) {
    s += "; first finding at offset ";
    s += std::to_string(r.first_findings.front().byte_offset);
    s += " — range '";
    s += std::string{r.first_findings.front().range_name};
    s += "'";
  }
  return s;
}

} // namespace

auto pre_eval_scan(std::string_view src, std::string_view source_label)
    -> std::optional<EvalError> {
  if (!g_enabled.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  auto cfg = current_policy();
  auto result = plinth::security::scan_for_invisible_unicode(src, cfg);
  if (!result.exceeds_threshold) {
    return std::nullopt;
  }
  emit_with_rate_limit("eval", source_label, result, cfg.threshold);
  return EvalError{
      .kind = EvalErrorKind::UNICODE_SMUGGLE_DETECTED,
      .message = format_eval_error_message(source_label, result, cfg.threshold),
      .line = 0,
      .column = 0,
  };
}

auto set_unicode_scanner_policy(bool enabled, std::size_t threshold,
                                bool log_findings) -> void {
  g_enabled.store(enabled, std::memory_order_release);
  g_threshold.store(threshold == 0 ? 1 : threshold, std::memory_order_release);
  g_log_findings.store(log_findings, std::memory_order_release);
}

} // namespace plinth::js
