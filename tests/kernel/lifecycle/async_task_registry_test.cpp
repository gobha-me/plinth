// SPDX-License-Identifier: MIT

#include "kernel/lifecycle/async_task_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("async task registry closes admission and drains owned leases",
          "[lifecycle][async-tasks]") {
  plinth::lifecycle::AsyncTaskRegistry registry;
  auto lease = registry.try_acquire();
  REQUIRE(lease != nullptr);
  REQUIRE(registry.active_for_test() == 1);

  registry.close_admission();
  REQUIRE_FALSE(registry.accepting_for_test());
  REQUIRE(registry.try_acquire() == nullptr);
  REQUIRE_FALSE(registry.drain(0ms));

  lease.reset();
  REQUIRE(registry.drain(10ms));
  REQUIRE(registry.active_for_test() == 0);
}
