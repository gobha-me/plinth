#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "kernel/auth/handlers.hpp"
#include "kernel/auth/rate_limiter.hpp"

TEST_CASE("RateLimiter allows first attempts", "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter;
  REQUIRE(limiter.check("192.168.1.1"));
}

TEST_CASE("RateLimiter allows up to 4 failures", "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter;
  std::string ip = "10.0.0.1";

  for (int i = 0; i < 4; ++i) {
    limiter.record_failure(ip);
  }
  REQUIRE(limiter.check(ip));
}

TEST_CASE("RateLimiter blocks after 5 failures", "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter;
  std::string ip = "10.0.0.2";

  for (int i = 0; i < 5; ++i) {
    limiter.record_failure(ip);
  }
  REQUIRE_FALSE(limiter.check(ip));
}

TEST_CASE("RateLimiter tracks IPs independently",
          "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter;

  // Exhaust limit for ip1
  for (int i = 0; i < 5; ++i) {
    limiter.record_failure("ip1");
  }
  REQUIRE_FALSE(limiter.check("ip1"));

  // ip2 should still be allowed
  REQUIRE(limiter.check("ip2"));
}

TEST_CASE("RateLimiter retry_after returns 0 when not limited",
          "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter;
  REQUIRE(limiter.retry_after("unknown-ip") == 0);
}

TEST_CASE("RateLimiter retry_after returns positive when limited",
          "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter;
  std::string ip = "10.0.0.3";

  for (int i = 0; i < 5; ++i) {
    limiter.record_failure(ip);
  }
  auto wait = limiter.retry_after(ip);
  REQUIRE(wait > 0);
  REQUIRE(wait <= 60);
}

TEST_CASE("RateLimiter consume atomically admits only its configured bound",
          "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter(
      {.max_attempts = 2, .window = std::chrono::seconds{60}, .max_keys = 8});
  REQUIRE(limiter.consume("peer"));
  REQUIRE(limiter.consume("peer"));
  REQUIRE_FALSE(limiter.consume("peer"));
}

TEST_CASE("RateLimiter fails closed when its key bound is full",
          "[auth][rate_limiter][unit]") {
  plinth::auth::RateLimiter limiter(
      {.max_attempts = 1, .window = std::chrono::hours{1}, .max_keys = 2});
  limiter.record_failure("one");
  limiter.record_failure("two");
  REQUIRE(limiter.tracked_keys() == 2);
  REQUIRE_FALSE(limiter.check("three"));
  REQUIRE_FALSE(limiter.consume("three"));
  REQUIRE(limiter.tracked_keys() == 2);
}

TEST_CASE("password hashing admits exactly two concurrent operations",
          "[auth][hash_admission][unit]") {
  using plinth::auth::test_seam::active_password_hash_slots;
  using plinth::auth::test_seam::release_password_hash_slot;
  using plinth::auth::test_seam::try_acquire_password_hash_slot;

  REQUIRE(active_password_hash_slots() == 0);
  constexpr auto contender_count = 8;
  std::latch start{1};
  std::latch attempted{contender_count};
  std::latch release{1};
  std::atomic<unsigned int> acquired{0};
  std::vector<std::jthread> contenders;
  contenders.reserve(contender_count);
  for (auto index = 0; index < contender_count; ++index) {
    contenders.emplace_back([&] {
      start.wait();
      const bool owns_slot = try_acquire_password_hash_slot();
      if (owns_slot) {
        acquired.fetch_add(1, std::memory_order_relaxed);
      }
      attempted.count_down();
      if (owns_slot) {
        release.wait();
        release_password_hash_slot();
      }
    });
  }

  start.count_down();
  attempted.wait();
  const auto admitted = acquired.load(std::memory_order_relaxed);
  const auto active = active_password_hash_slots();
  release.count_down();
  contenders.clear();

  REQUIRE(admitted == 2);
  REQUIRE(active == 2);
  REQUIRE(active_password_hash_slots() == 0);
  REQUIRE(try_acquire_password_hash_slot());
  release_password_hash_slot();
  REQUIRE(active_password_hash_slots() == 0);
}
