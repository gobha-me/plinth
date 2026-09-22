#include "kernel/auth/rate_limiter.hpp"

#include <stdexcept>

namespace plinth::auth {

RateLimiter::RateLimiter() : RateLimiter(Options{}) {
}

RateLimiter::RateLimiter(Options options) : options_(options) {
  if (options_.max_attempts == 0 || options_.window.count() <= 0 ||
      options_.max_keys == 0) {
    throw std::invalid_argument("RateLimiter options must be positive");
  }
}

auto RateLimiter::purge_expired(
    std::deque<std::chrono::steady_clock::time_point>& entries) const -> void {
  auto cutoff = std::chrono::steady_clock::now() - options_.window;
  while (!entries.empty() && entries.front() < cutoff) {
    entries.pop_front();
  }
}

auto RateLimiter::purge_stale_keys() -> void {
  for (auto it = attempts.begin(); it != attempts.end();) {
    purge_expired(it->second);
    if (it->second.empty()) {
      it = attempts.erase(it);
    } else {
      ++it;
    }
  }
}

auto RateLimiter::ensure_key_available(const std::string& key) -> bool {
  if (attempts.contains(key)) {
    return true;
  }
  if (attempts.size() >= options_.max_keys) {
    purge_stale_keys();
  }
  return attempts.size() < options_.max_keys;
}

auto RateLimiter::check(const std::string& ip) -> bool {
  std::lock_guard lock(mtx);
  auto it = attempts.find(ip);
  if (it == attempts.end()) {
    return ensure_key_available(ip);
  }
  purge_expired(it->second);
  if (it->second.empty()) {
    attempts.erase(it);
    return true;
  }
  return it->second.size() < options_.max_attempts;
}

auto RateLimiter::consume(const std::string& key) -> bool {
  std::lock_guard lock(mtx);
  if (!ensure_key_available(key)) {
    return false;
  }
  auto& entries = attempts[key];
  purge_expired(entries);
  if (entries.size() >= options_.max_attempts) {
    return false;
  }
  entries.push_back(std::chrono::steady_clock::now());
  return true;
}

auto RateLimiter::record_failure(const std::string& ip) -> void {
  std::lock_guard lock(mtx);
  if (!ensure_key_available(ip)) {
    return;
  }
  auto& entries = attempts[ip];
  purge_expired(entries);
  entries.push_back(std::chrono::steady_clock::now());
}

auto RateLimiter::retry_after(const std::string& ip) -> int {
  std::lock_guard lock(mtx);
  auto it = attempts.find(ip);
  if (it == attempts.end()) {
    if (attempts.size() >= options_.max_keys) {
      return static_cast<int>(options_.window.count());
    }
    return 0;
  }
  purge_expired(it->second);
  if (it->second.size() < options_.max_attempts) {
    return 0;
  }
  // Oldest entry in window determines when the window slides enough
  auto oldest = it->second.front();
  auto expires_at = oldest + options_.window;
  auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
      expires_at - std::chrono::steady_clock::now());
  return std::max(1, static_cast<int>(remaining.count()));
}

auto RateLimiter::reset(const std::string& key) -> void {
  std::lock_guard lock(mtx);
  attempts.erase(key);
}

auto RateLimiter::tracked_keys() const -> std::size_t {
  std::lock_guard lock(mtx);
  return attempts.size();
}

} // namespace plinth::auth
