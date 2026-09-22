#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace plinth::auth {

// Thread-safe in-memory sliding-window rate limiter.
// Tracks failed login attempts per IP address.
class RateLimiter {
 public:
  struct Options {
    std::size_t max_attempts = 5;
    std::chrono::seconds window{60};
    std::size_t max_keys = 4096;
  };

  RateLimiter();
  explicit RateLimiter(Options options);

  // Returns true if the IP is allowed to attempt, false if rate-limited.
  auto check(const std::string& ip) -> bool;

  // Atomically check and record an attempt. Used for admission limits where
  // every request, not merely failures, consumes capacity.
  auto consume(const std::string& key) -> bool;

  // Record a failed attempt for the given IP.
  auto record_failure(const std::string& ip) -> void;

  // Seconds until the IP is allowed to retry. Returns 0 if not limited.
  auto retry_after(const std::string& ip) -> int;

  // Forget one key after a successful authentication.
  auto reset(const std::string& key) -> void;

  // Test/diagnostic seam: number of bounded keys currently retained.
  auto tracked_keys() const -> std::size_t;

 private:
  auto purge_expired(
      std::deque<std::chrono::steady_clock::time_point>& entries) const -> void;
  auto purge_stale_keys() -> void;
  auto ensure_key_available(const std::string& key) -> bool;

  Options options_;
  mutable std::mutex mtx;
  std::unordered_map<std::string,
                     std::deque<std::chrono::steady_clock::time_point>>
      attempts;
};

} // namespace plinth::auth
