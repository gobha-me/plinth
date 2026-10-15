// SPDX-License-Identifier: MIT

#include "kernel/audit/async_write_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using plinth::log::detail::AuditWriteRegistry;

TEST_CASE("audit registry retains callback tail after successful settlement",
          "[audit][ownership]") {
  AuditWriteRegistry registry;
  REQUIRE_FALSE(registry.accepting());
  REQUIRE(registry.try_acquire() == nullptr);
  REQUIRE(registry.wait_for_settlement(0ms));
  REQUIRE(registry.start());

  std::size_t active_at_tail = 0;
  struct Tail {
    AuditWriteRegistry& registry;
    std::size_t& observed;
    ~Tail() { observed = registry.active(); }
  };
  struct CallbackOwner {
    // The lease is released after all callback dependencies, not at outcome.
    std::shared_ptr<AuditWriteRegistry::Lease> lease;
    Tail tail;
  };
  auto owner = std::make_unique<CallbackOwner>(registry.try_acquire(),
                                               Tail{registry, active_at_tail});
  active_at_tail = 0;
  REQUIRE(owner->lease != nullptr);
  REQUIRE(owner->lease->succeed());
  REQUIRE_FALSE(owner->lease->succeed());
  REQUIRE_FALSE(owner->lease->fail());
  REQUIRE(registry.active() == 1);
  REQUIRE_FALSE(registry.wait_for_settlement(0ms));
  registry.close();
  REQUIRE(registry.try_acquire() == nullptr);
  owner.reset();
  REQUIRE(active_at_tail == 1);
  REQUIRE(registry.active() == 0);
  REQUIRE(registry.wait_for_settlement(0ms));
  REQUIRE_FALSE(registry.start());
}

TEST_CASE("audit registry failures stay unclean after owners retire",
          "[audit][ownership]") {
  AuditWriteRegistry registry;
  REQUIRE(registry.start());
  auto lease = registry.try_acquire();
  REQUIRE(lease != nullptr);
  SECTION("SQL failure is sticky") {
    REQUIRE(lease->fail());
    REQUIRE_FALSE(lease->fail());
    REQUIRE_FALSE(lease->succeed());
  }
  SECTION("abandoned callback is not successful settlement") {
  }
  lease.reset();
  REQUIRE(registry.active() == 0);
  REQUIRE(registry.failed());
  REQUIRE_FALSE(registry.wait_for_settlement(0ms));
  registry.pause();
  REQUIRE(registry.try_acquire() == nullptr);
  REQUIRE(registry.start());
  auto later = registry.try_acquire();
  REQUIRE(later != nullptr);
  REQUIRE(later->skip());
  later.reset();
  REQUIRE_FALSE(registry.wait_for_settlement(-1ms));
  registry.close();
  registry.close();
  REQUIRE_FALSE(registry.start());
  REQUIRE_FALSE(registry.wait_for_settlement(0ms));
}

TEST_CASE("audit registry seal linearizes concurrent submissions without "
          "losing owners",
          "[audit][ownership]") {
  for (int repetition = 0; repetition < 20; ++repetition) {
    AuditWriteRegistry registry;
    REQUIRE(registry.start());
    std::array<bool, 2> admitted{};
    std::barrier start{3};
    std::latch ready{2};
    std::promise<void> release;
    const auto released = release.get_future().share();
    auto submit = [&](std::size_t index) {
      start.arrive_and_wait();
      auto lease = registry.try_acquire();
      admitted[index] = lease != nullptr;
      if (lease != nullptr) {
        static_cast<void>(lease->succeed());
      }
      ready.count_down();
      released.wait();
    };
    std::jthread first{submit, 0};
    std::jthread second{submit, 1};
    start.arrive_and_wait();
    registry.close();
    ready.wait();
    const auto count = registry.active();
    const bool rejected_after_seal = registry.try_acquire() == nullptr;
    release.set_value();
    first.join();
    second.join();
    REQUIRE(count == static_cast<std::size_t>(admitted[0]) +
                         static_cast<std::size_t>(admitted[1]));
    REQUIRE(rejected_after_seal);
    REQUIRE(registry.active() == 0);
    REQUIRE(registry.wait_for_settlement(0ms));
    REQUIRE_FALSE(registry.failed());
  }
}
