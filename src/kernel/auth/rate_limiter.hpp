#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace plinth::auth {

// Thread-safe in-memory sliding-window rate limiter.
// Tracks failed login attempts per IP address.
class RateLimiter {
 public:
  // Returns true if the IP is allowed to attempt, false if rate-limited.
  auto check(const std::string& ip) -> bool;

  // Record a failed attempt for the given IP.
  auto record_failure(const std::string& ip) -> void;

  // Seconds until the IP is allowed to retry. Returns 0 if not limited.
  auto retry_after(const std::string& ip) -> int;

 private:
  static constexpr int MAX_FAILURES = 5;
  static constexpr auto WINDOW = std::chrono::seconds{60};

  static auto purge_expired(
      std::deque<std::chrono::steady_clock::time_point>& entries) -> void;

  std::mutex mtx;
  std::unordered_map<std::string,
                     std::deque<std::chrono::steady_clock::time_point>>
      attempts;
};

} // namespace plinth::auth
