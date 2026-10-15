// SPDX-License-Identifier: MIT

#include "kernel/lifecycle/async_task_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;
using plinth::lifecycle::AsyncCleanupOwner;
using plinth::lifecycle::AsyncTaskLease;
using plinth::lifecycle::AsyncTaskRegistry;

// A test resource implements the real production registry's cleanup contract.
// No scheduler/drain algorithm is replaced by a model. Fixed-size observations
// also avoid allocating or throwing from retry_cleanup's noexcept boundary.
struct CleanupObservations {
  bool ready = false;
  bool prerequisite_ready = false;
  bool destroyed = false;
  std::size_t active_at_destruction = 0;
  std::size_t calls = 0;
  std::array<Clock::time_point, 8> deadlines{};
  std::array<int, 8> order{};
  std::size_t order_size = 0;
};

class RetainedTestOwner final
    : public AsyncCleanupOwner,
      public std::enable_shared_from_this<RetainedTestOwner> {
 public:
  RetainedTestOwner(AsyncTaskRegistry& registry_in,
                    CleanupObservations& observations_in,
                    std::shared_ptr<AsyncTaskLease> accepted_in = nullptr)
      : accepted(std::move(accepted_in)), registry(registry_in),
        observations(observations_in) {}

  ~RetainedTestOwner() override {
    observations.active_at_destruction = registry.active_for_test();
    observations.destroyed = true;
  }

  auto retry_cleanup(Clock::time_point deadline) noexcept -> bool override {
    if (observations.calls < observations.deadlines.size()) {
      observations.deadlines[observations.calls] = deadline;
    }
    ++observations.calls;
    if (order_id != 0 && observations.order_size < observations.order.size()) {
      observations.order[observations.order_size++] = order_id;
    }
    if (republish_once) {
      republish_once = false;
      // Publish a new unresolved cleanup revision while the registry is
      // retrying this record. Returning true for the OLD revision must not
      // discard that new obligation.
      registry.retain_cleanup(shared_from_this());
      return true;
    }
    if (completes_prerequisite) {
      observations.prerequisite_ready = true;
      return true;
    }
    return requires_prerequisite ? observations.prerequisite_ready
                                 : observations.ready;
  }

  int order_id = 0;
  bool requires_prerequisite = false;
  bool completes_prerequisite = false;
  bool republish_once = false;

 private:
  // Declared first so the accepted lease remains alive through dependency
  // destruction, just as native production cleanup ownership requires.
  std::shared_ptr<AsyncTaskLease> accepted;
  AsyncTaskRegistry& registry;
  CleanupObservations& observations;
};

class CleanupOnUnwind {
 public:
  CleanupOnUnwind(AsyncTaskRegistry& registry_in,
                  CleanupObservations& observations_in)
      : registry(registry_in), observations(observations_in) {}
  ~CleanupOnUnwind() {
    observations.ready = true;
    observations.prerequisite_ready = true;
    static_cast<void>(registry.drain(100ms));
  }

 private:
  AsyncTaskRegistry& registry;
  CleanupObservations& observations;
};

} // namespace

TEST_CASE("async cleanup failure retains owner and lease until explicit retry",
          "[lifecycle][async-tasks][cleanup-retention]") {
  AsyncTaskRegistry registry;
  CleanupObservations observations;
  CleanupOnUnwind cleanup{registry, observations};
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto owner =
      std::make_shared<RetainedTestOwner>(registry, observations, accepted);
  std::weak_ptr<RetainedTestOwner> weak = owner;
  registry.retain_cleanup(owner);
  owner.reset();
  accepted.reset();
  registry.close_admission();

  const auto before = Clock::now();
  const bool first_drained = registry.drain(2ms);
  const auto after = Clock::now();
  const bool retained = !weak.expired();
  const auto active_after_failure = registry.active_for_test();
  const auto calls_after_failure = observations.calls;
  const auto first_deadline = observations.deadlines[0];
  const bool destroyed_after_failure = observations.destroyed;

  // Restore cleanup readiness BEFORE reporting assertions, so a test failure
  // cannot abandon the registry with an unresolved retained native owner.
  observations.ready = true;
  const bool retried = registry.drain(100ms);

  CHECK_FALSE(first_drained);
  CHECK(retained);
  CHECK_FALSE(destroyed_after_failure);
  CHECK(active_after_failure == 1);
  CHECK(calls_after_failure == 1);
  CHECK(first_deadline >= before);
  CHECK(first_deadline <= after);
  CHECK(retried);
  CHECK(weak.expired());
  CHECK(observations.destroyed);
  CHECK(observations.active_at_destruction == 1);
  CHECK(registry.active_for_test() == 0);
  CHECK(registry.try_acquire() == nullptr);
}

TEST_CASE("async cleanup duplicate retention registers only one owner",
          "[lifecycle][async-tasks][cleanup-retention]") {
  AsyncTaskRegistry registry;
  CleanupObservations observations;
  CleanupOnUnwind cleanup{registry, observations};
  observations.ready = true;
  auto owner = std::make_shared<RetainedTestOwner>(registry, observations);
  std::weak_ptr<RetainedTestOwner> weak = owner;
  registry.retain_cleanup(owner);
  registry.retain_cleanup(owner);
  owner.reset();
  const bool drained = registry.drain(100ms);
  CHECK(drained);
  CHECK(observations.calls == 1);
  CHECK(weak.expired());
}

TEST_CASE("async cleanup republished revision cannot be discarded as completed",
          "[lifecycle][async-tasks][cleanup-retention]") {
  AsyncTaskRegistry registry;
  CleanupObservations observations;
  CleanupOnUnwind cleanup{registry, observations};
  auto owner = std::make_shared<RetainedTestOwner>(registry, observations);
  owner->republish_once = true;
  std::weak_ptr<RetainedTestOwner> weak = owner;
  registry.retain_cleanup(owner);
  owner.reset();
  const bool first_drained = registry.drain(2ms);
  const bool retained = !weak.expired();
  const auto first_calls = observations.calls;
  observations.ready = true;
  const bool retried = registry.drain(100ms);
  CHECK_FALSE(first_drained);
  CHECK(retained);
  CHECK(first_calls == 1);
  CHECK(retried);
  CHECK(observations.calls == 2);
  CHECK(weak.expired());
}

TEST_CASE("async cleanup retries dependent records in either insertion order",
          "[lifecycle][async-tasks][cleanup-retention]") {
  for (const bool dependent_first : {false, true}) {
    CAPTURE(dependent_first);
    AsyncTaskRegistry registry;
    CleanupObservations observations;
    CleanupOnUnwind cleanup{registry, observations};
    auto dependent =
        std::make_shared<RetainedTestOwner>(registry, observations);
    dependent->requires_prerequisite = true;
    dependent->order_id = 1;
    auto prerequisite =
        std::make_shared<RetainedTestOwner>(registry, observations);
    prerequisite->completes_prerequisite = true;
    prerequisite->order_id = 2;
    if (dependent_first) {
      registry.retain_cleanup(prerequisite);
      registry.retain_cleanup(dependent);
    } else {
      registry.retain_cleanup(dependent);
      registry.retain_cleanup(prerequisite);
    }
    dependent.reset();
    prerequisite.reset();
    const bool drained = registry.drain(100ms);
    CHECK(drained);
    CHECK(observations.prerequisite_ready);
    CHECK(observations.order_size == (dependent_first ? 3 : 2));
    CHECK(observations.order[0] == (dependent_first ? 1 : 2));
    CHECK(observations.order[1] == (dependent_first ? 2 : 1));
    if (dependent_first) {
      CHECK(observations.order[2] == 1);
    }
    for (std::size_t i = 1; i < observations.calls; ++i) {
      CHECK(observations.deadlines[i] == observations.deadlines[0]);
    }
  }
}

TEST_CASE("async cleanup expired deadline never launches cleanup work",
          "[lifecycle][async-tasks][cleanup-retention]") {
  AsyncTaskRegistry registry;
  CleanupObservations observations;
  CleanupOnUnwind cleanup{registry, observations};
  observations.ready = true;
  auto owner = std::make_shared<RetainedTestOwner>(registry, observations);
  registry.retain_cleanup(owner);
  owner.reset();
  const bool expired_drained = registry.drain(0ms);
  const auto expired_calls = observations.calls;
  const bool retried = registry.drain(100ms);
  CHECK_FALSE(expired_drained);
  CHECK(expired_calls == 0);
  CHECK(retried);
  CHECK(observations.calls == 1);
}
