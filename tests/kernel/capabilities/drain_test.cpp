#include "kernel/capabilities/drain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace plinth::capabilities::drain;
using namespace std::chrono_literals;

TEST_CASE("drain: no active drain is a no-op guard", "[capabilities][drain]") {
  REQUIRE(active_drain_count() == 0);
  {
    DispatchGuard g("notes");
    REQUIRE(g.admitted());
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

TEST_CASE("drain: fence rejects newly arriving dispatches",
          "[capabilities][drain]") {
  auto state = begin_drain("notes-basic");
  {
    DispatchGuard g("notes-basic");
    REQUIRE_FALSE(g.admitted());
  }
  REQUIRE(state->in_flight.load() == 0);
  auto [ok, outstanding] = wait_for_zero(state, 500ms);
  REQUIRE(ok);
  REQUIRE(outstanding == 0);
  end_drain("notes-basic");
}

TEST_CASE("drain: timeout reports outstanding count", "[capabilities][drain]") {
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  std::atomic<bool> admitted{false};
  std::thread worker([&] {
    DispatchGuard g("notes-timeout");
    admitted.store(g.admitted());
    started.store(true);
    while (!release.load()) {
      std::this_thread::sleep_for(5ms);
    }
  });
  while (!started.load()) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(admitted.load());
  auto state = begin_drain("notes-timeout");
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

TEST_CASE("drain: pre-drain guard is tracked until it exits",
          "[capabilities][drain]") {
  std::shared_ptr<DrainState> state;
  {
    DispatchGuard g("notes-race");
    REQUIRE(g.admitted());
    state = begin_drain("notes-race");
    REQUIRE(state->in_flight.load() == 1);
    auto [ok, outstanding] = wait_for_zero(state, 1ms);
    REQUIRE_FALSE(ok);
    REQUIRE(outstanding == 1);
  }
  auto [ok, outstanding] = wait_for_zero(state, 100ms);
  REQUIRE(ok);
  REQUIRE(outstanding == 0);
  end_drain("notes-race");
}

TEST_CASE("drain: end_drain on unknown name is no-op",
          "[capabilities][drain]") {
  end_drain("does-not-exist");
  REQUIRE(active_drain_count() == 0);
}

TEST_CASE("drain: idle namespace state is reclaimed", "[capabilities][drain]") {
  const auto before = tracked_state_count_for_test();
  for (int i = 0; i < 1000; ++i) {
    DispatchGuard guard("unknown-" + std::to_string(i));
    REQUIRE(guard.admitted());
  }
  REQUIRE(tracked_state_count_for_test() == before);
}

TEST_CASE("drain: begin_drain twice returns same state",
          "[capabilities][drain]") {
  auto s1 = begin_drain("notes-twice");
  auto s2 = begin_drain("notes-twice");
  REQUIRE(s1.get() == s2.get());
  REQUIRE(active_drain_count() == 1);
  end_drain("notes-twice");
}

TEST_CASE("drain: lifecycle failure blocks discovery until repair completes",
          "[capabilities][drain]") {
  auto state = begin_drain("notes-blocked");
  (void)state;
  block_application("notes-blocked");
  REQUIRE(is_fenced("notes-blocked"));
  REQUIRE(is_application_blocked("notes-blocked"));
  end_drain("notes-blocked");
  REQUIRE_FALSE(is_fenced("notes-blocked"));
  REQUIRE_FALSE(is_application_blocked("notes-blocked"));
}
