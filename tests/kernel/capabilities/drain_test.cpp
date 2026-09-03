#include "kernel/capabilities/drain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace plinth::capabilities::drain;
using namespace std::chrono_literals;

TEST_CASE("drain: no active drain is a no-op guard", "[capabilities][drain]") {
  REQUIRE(active_drain_count() == 0);
  {
    DispatchGuard g("notes");
    (void)g;
  }
  REQUIRE(active_drain_count() == 0);
}

TEST_CASE("drain: wait_for_zero returns immediately on empty counter",
          "[capabilities][drain]") {
  auto state = begin_drain("notes-empty");
  auto [ok, outstanding] = wait_for_zero(state, 100ms);
  REQUIRE(ok);
  REQUIRE(outstanding == 0);
  end_drain("notes-empty");
  REQUIRE(active_drain_count() == 0);
}

TEST_CASE("drain: guard increments then decrements, releasing waiter",
          "[capabilities][drain]") {
  auto state = begin_drain("notes-basic");
  std::atomic<bool> started{false};
  std::thread worker([&] {
    DispatchGuard g("notes-basic");
    started.store(true);
    std::this_thread::sleep_for(50ms);
  });
  while (!started.load()) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(state->in_flight.load() == 1);
  auto [ok, outstanding] = wait_for_zero(state, 500ms);
  REQUIRE(ok);
  REQUIRE(outstanding == 0);
  worker.join();
  end_drain("notes-basic");
}

TEST_CASE("drain: timeout reports outstanding count", "[capabilities][drain]") {
  auto state = begin_drain("notes-timeout");
  std::atomic<bool> release{false};
  std::thread worker([&] {
    DispatchGuard g("notes-timeout");
    while (!release.load()) {
      std::this_thread::sleep_for(5ms);
    }
  });
  while (state->in_flight.load() < 1) {
    std::this_thread::sleep_for(1ms);
  }
  auto [ok, outstanding] = wait_for_zero(state, 50ms);
  REQUIRE_FALSE(ok);
  REQUIRE(outstanding == 1);
  release.store(true);
  worker.join();
  end_drain("notes-timeout");
}

TEST_CASE("drain: guard for a different name is invisible",
          "[capabilities][drain]") {
  auto state = begin_drain("notes-A");
  {
    DispatchGuard g("other-B");
    (void)g;
  }
  REQUIRE(state->in_flight.load() == 0);
  end_drain("notes-A");
}

TEST_CASE("drain: pre-drain guard has no state and does not underflow",
          "[capabilities][drain]") {
  // Construct a guard when no drain is active; then begin a drain;
  // destruct the guard. The guard captured no state, so its dtor
  // is a no-op and the freshly-begun drain's counter stays at 0.
  DispatchGuard g("notes-race");
  auto state = begin_drain("notes-race");
  REQUIRE(state->in_flight.load() == 0);
  // g dtor fires here; should not touch state.
  end_drain("notes-race");
  REQUIRE(state->in_flight.load() == 0);
}

TEST_CASE("drain: end_drain on unknown name is no-op",
          "[capabilities][drain]") {
  end_drain("does-not-exist");
  REQUIRE(active_drain_count() == 0);
}

TEST_CASE("drain: begin_drain twice returns same state",
          "[capabilities][drain]") {
  auto s1 = begin_drain("notes-twice");
  auto s2 = begin_drain("notes-twice");
  REQUIRE(s1.get() == s2.get());
  REQUIRE(active_drain_count() == 1);
  end_drain("notes-twice");
}
