#include <catch2/catch_test_macros.hpp>
#include <string>

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
