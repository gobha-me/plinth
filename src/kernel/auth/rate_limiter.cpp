#include "kernel/auth/rate_limiter.hpp"

namespace plinth::auth {

auto RateLimiter::purge_expired(
    std::deque<std::chrono::steady_clock::time_point>& entries) -> void {
  auto cutoff = std::chrono::steady_clock::now() - WINDOW;
  while (!entries.empty() && entries.front() < cutoff) {
    entries.pop_front();
  }
}

auto RateLimiter::check(const std::string& ip) -> bool {
  std::lock_guard lock(mtx);
  auto it = attempts.find(ip);
  if (it == attempts.end()) {
    return true;
  }
  purge_expired(it->second);
  if (it->second.empty()) {
    attempts.erase(it);
    return true;
  }
  return static_cast<int>(it->second.size()) < MAX_FAILURES;
}

auto RateLimiter::record_failure(const std::string& ip) -> void {
  std::lock_guard lock(mtx);
  auto& entries = attempts[ip];
  purge_expired(entries);
  entries.push_back(std::chrono::steady_clock::now());
}

auto RateLimiter::retry_after(const std::string& ip) -> int {
  std::lock_guard lock(mtx);
  auto it = attempts.find(ip);
  if (it == attempts.end() || it->second.empty()) {
    return 0;
  }
  purge_expired(it->second);
  if (static_cast<int>(it->second.size()) < MAX_FAILURES) {
    return 0;
  }
  // Oldest entry in window determines when the window slides enough
  auto oldest = it->second.front();
  auto expires_at = oldest + WINDOW;
  auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
      expires_at - std::chrono::steady_clock::now());
  return std::max(1, static_cast<int>(remaining.count()));
}

} // namespace plinth::auth
