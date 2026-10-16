#include "kernel/frontend/owned_frontend_worker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace frontend = plinth::frontend;
namespace lifecycle = plinth::lifecycle;
using namespace std::chrono_literals;

struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  auto enter_and_wait() -> void {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [this] { return released; });
  }
  auto wait_entered() -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 2s, [this] { return entered; });
  }
  auto release() -> void {
    std::lock_guard lock{mutex};
    released = true;
    changed.notify_all();
  }
};

// Created after the worker, before assertions/submission. REQUIRE unwinding
// always releases both deterministic barriers BEFORE worker destruction.
struct ReleaseGates {
  std::shared_ptr<Gate> work;
  std::shared_ptr<Gate> completion;
  ~ReleaseGates() {
    if (work) work->release();
    if (completion) completion->release();
  }
};

// The destructor body releases blocked work before jthread members join,
// including partial thread construction and REQUIRE unwinding.
struct ReleaseDrainThreads {
  std::shared_ptr<Gate> work;
  std::shared_ptr<Gate> completion;
  std::array<std::jthread, 2> threads;
  ~ReleaseDrainThreads() {
    if (work) work->release();
    if (completion) completion->release();
  }
};

struct Outcome {
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t calls = 0;
  std::optional<frontend::FrontendWorkResult> result;
  auto record(frontend::FrontendWorkResult value) -> void {
    std::lock_guard lock{mutex};
    ++calls;
    result = value;
    changed.notify_all();
  }
  auto wait() -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 2s, [this] { return result.has_value(); });
  }
};

struct Batch {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::optional<frontend::FrontendWorkResult>, 64> results;
  std::array<std::size_t, 64> calls{};
  std::vector<std::size_t> order;
  Batch() { order.reserve(64); }
  auto record(std::size_t index, frontend::FrontendWorkResult result) -> void {
    std::lock_guard lock{mutex};
    ++calls[index];
    results[index] = result;
    order.push_back(index);
    changed.notify_all();
  }
  auto wait(std::size_t count) -> bool {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, 2s, [&] { return order.size() == count; });
  }
};

struct DisposeGate {
  std::shared_ptr<Gate> gate;
  ~DisposeGate() { gate->enter_and_wait(); }
};
} // namespace

TEST_CASE("frontend worker: physical work and completion retain one slot",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker;
  auto work_gate = std::make_shared<Gate>();
  auto completion_gate = std::make_shared<Gate>();
  const ReleaseGates release{work_gate, completion_gate};
  auto outcome = std::make_shared<Outcome>();
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  auto same_deadline = std::make_shared<std::atomic<bool>>(false);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  REQUIRE(worker.capacity() == 1);
  auto owned_payload = std::make_shared<int>(1);
  const std::weak_ptr<int> payload_lifetime = owned_payload;
  std::stop_source caller_stop;
  auto submitted = worker.try_submit(
      [work_gate, cancelled, same_deadline, deadline, payload = owned_payload](
          std::stop_token stop,
          frontend::OwnedFrontendWorker::Deadline supplied) {
        same_deadline->store(deadline == supplied);
        work_gate->enter_and_wait(); // Deliberately non-preemptible test work.
        cancelled->store(stop.stop_requested());
        static_cast<void>(payload);
      },
      [completion_gate, outcome,
       payload = owned_payload](frontend::FrontendWorkResult result) {
        completion_gate->enter_and_wait();
        outcome->record(result);
        static_cast<void>(payload);
      },
      tasks.try_acquire(), deadline, caller_stop.get_token());
  owned_payload.reset();
  REQUIRE(submitted.has_value());
  REQUIRE(work_gate->wait_entered());
  REQUIRE(same_deadline->load());
  REQUIRE(worker.active());
  REQUIRE(tasks.active_for_test() == 1);
  REQUIRE_FALSE(payload_lifetime.expired());
  const auto extra = worker.try_submit(
      [](std::stop_token, auto) {}, [](auto) {}, tasks.try_acquire(), deadline);
  REQUIRE_FALSE(extra.has_value());
  REQUIRE(extra.error() == frontend::FrontendSubmitError::BUSY);
  REQUIRE(tasks.active_for_test() == 1);
  caller_stop.request_stop();
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now()));
  REQUIRE_FALSE(worker.physical_joined());
  REQUIRE(worker.active());
  REQUIRE(tasks.active_for_test() == 1);
  work_gate->release();
  REQUIRE(completion_gate->wait_entered());
  REQUIRE(cancelled->load());
  REQUIRE(
      worker.active()); // Logical work return did NOT free the physical lane.
  REQUIRE(tasks.active_for_test() == 1);
  REQUIRE_FALSE(payload_lifetime.expired());
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now()));
  completion_gate->release();
  REQUIRE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(worker.physical_joined());
  REQUIRE_FALSE(worker.active());
  REQUIRE_FALSE(worker.completion_failed());
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE(payload_lifetime.expired());
  REQUIRE(outcome->wait());
  {
    std::lock_guard lock{outcome->mutex};
    REQUIRE(outcome->calls == 1);
    // Cancellation does not retroactively invent SQL rollback/failure for work
    // that returned normally. The owning operation supplies its actual result.
    REQUIRE(outcome->result->has_value());
  }
  SECTION("concurrent drainers keep independent steady deadlines and retry") {
    lifecycle::AsyncTaskRegistry concurrent_tasks;
    frontend::OwnedFrontendWorker concurrent_worker;
    auto blocked_work = std::make_shared<Gate>();
    auto blocked_completion = std::make_shared<Gate>();
    auto concurrent_outcome = std::make_shared<Outcome>();
    std::array<bool, 2> drained{};
    std::array<std::exception_ptr, 2> errors{};
    std::array<std::promise<void>, 2> started;
    std::array ready{started[0].get_future(), started[1].get_future()};
    ReleaseDrainThreads drainers{blocked_work, blocked_completion, {}};
    const auto concurrent_deadline = std::chrono::steady_clock::now() + 10s;
    REQUIRE(concurrent_worker.try_submit(
        [blocked_work](auto, auto) { blocked_work->enter_and_wait(); },
        [blocked_completion, concurrent_outcome](auto result) {
          blocked_completion->enter_and_wait();
          concurrent_outcome->record(result);
        },
        concurrent_tasks.try_acquire(), concurrent_deadline));
    REQUIRE(blocked_work->wait_entered());
    for (std::size_t index = 0; index < drainers.threads.size(); ++index) {
      drainers.threads[index] = std::jthread([&, index, concurrent_deadline] {
        started[index].set_value();
        try {
          drained[index] = concurrent_worker.drain(concurrent_deadline);
        } catch (...) {
          errors[index] = std::current_exception();
        }
      });
    }
    for (auto& entered : ready)
      REQUIRE(entered.wait_for(2s) == std::future_status::ready);
    REQUIRE_FALSE(concurrent_worker.drain(std::chrono::steady_clock::now()));
    REQUIRE_FALSE(
        concurrent_worker.drain(std::chrono::steady_clock::now() + 25ms));
    REQUIRE_FALSE(concurrent_worker.physical_joined());
    REQUIRE(concurrent_worker.active());
    REQUIRE(concurrent_tasks.active_for_test() == 1);
    blocked_work->release();
    REQUIRE(blocked_completion->wait_entered());
    REQUIRE_FALSE(concurrent_worker.physical_joined());
    REQUIRE(concurrent_tasks.active_for_test() == 1);
    REQUIRE_FALSE(concurrent_worker.drain(std::chrono::steady_clock::now()));
    blocked_completion->release();
    for (auto& drainer : drainers.threads)
      drainer.join();
    for (std::size_t index = 0; index < drained.size(); ++index) {
      REQUIRE_FALSE(errors[index]);
      REQUIRE(drained[index]);
    }
    REQUIRE(concurrent_worker.physical_joined());
    REQUIRE_FALSE(concurrent_worker.active());
    REQUIRE_FALSE(concurrent_worker.completion_failed());
    REQUIRE(concurrent_tasks.active_for_test() == 0);
    REQUIRE(concurrent_worker.drain(std::chrono::steady_clock::now() + 2s));
    REQUIRE(concurrent_outcome->wait());
    std::lock_guard concurrent_lock{concurrent_outcome->mutex};
    REQUIRE(concurrent_outcome->calls == 1);
    REQUIRE(concurrent_outcome->result->has_value());
  }
}

TEST_CASE("frontend worker: fixed error completion and rejected admission",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker;
  auto outcome = std::make_shared<Outcome>();
  auto ran = std::make_shared<std::atomic<bool>>(false);
  const auto future = std::chrono::steady_clock::now() + 10s;
  const auto invalid =
      worker.try_submit({}, [](auto) {}, tasks.try_acquire(), future);
  REQUIRE_FALSE(invalid.has_value());
  REQUIRE(invalid.error() == frontend::FrontendSubmitError::INVALID);
  const auto no_completion =
      worker.try_submit([](auto, auto) {}, {}, tasks.try_acquire(), future);
  REQUIRE_FALSE(no_completion.has_value());
  REQUIRE(no_completion.error() == frontend::FrontendSubmitError::INVALID);
  const auto no_lease =
      worker.try_submit([](auto, auto) {}, [](auto) {}, {}, future);
  REQUIRE_FALSE(no_lease.has_value());
  REQUIRE(no_lease.error() == frontend::FrontendSubmitError::INVALID);
  const auto no_deadline = worker.try_submit([](auto, auto) {}, [](auto) {},
                                             tasks.try_acquire(), {});
  REQUIRE_FALSE(no_deadline.has_value());
  REQUIRE(no_deadline.error() == frontend::FrontendSubmitError::INVALID);
  REQUIRE(tasks.active_for_test() == 0);

  std::stop_source cancelled;
  frontend::FrontendWorkError expected = frontend::FrontendWorkError::FAILED;
  frontend::OwnedFrontendWorker::Work work = [ran](auto, auto) {
    ran->store(true);
    throw std::runtime_error("fake SQL/path detail must never escape");
  };
  auto deadline = future;
  SECTION("job exception is one redacted failure") {
  }
  SECTION("nonstandard exception is one redacted failure") {
    work = [ran](auto, auto) {
      ran->store(true);
      throw 7;
    };
  }
  SECTION("allocation exception has fixed limit taxonomy") {
    expected = frontend::FrontendWorkError::LIMIT_EXCEEDED;
    work = [ran](auto, auto) {
      ran->store(true);
      throw std::bad_alloc{};
    };
  }
  SECTION("already cancelled skips work but completes once") {
    expected = frontend::FrontendWorkError::CANCELLED;
    cancelled.request_stop();
  }
  SECTION("already expired skips work but completes once") {
    expected = frontend::FrontendWorkError::DEADLINE;
    deadline = std::chrono::steady_clock::now() - 1ms;
  }
  REQUIRE(worker
              .try_submit(
                  work, [outcome](auto result) { outcome->record(result); },
                  tasks.try_acquire(), deadline, cancelled.get_token())
              .has_value());
  REQUIRE(outcome->wait());
  REQUIRE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(tasks.active_for_test() == 0);
  std::lock_guard lock{outcome->mutex};
  REQUIRE(outcome->calls == 1);
  REQUIRE_FALSE(outcome->result->has_value());
  REQUIRE(outcome->result->error() == expected);
  REQUIRE(ran->load() == (expected != frontend::FrontendWorkError::CANCELLED &&
                          expected != frontend::FrontendWorkError::DEADLINE));
  const auto closed = worker.try_submit([](auto, auto) {}, [](auto) {},
                                        tasks.try_acquire(), future);
  REQUIRE_FALSE(closed.has_value());
  REQUIRE(closed.error() == frontend::FrontendSubmitError::CLOSED);
}

TEST_CASE("frontend worker: failed completion is sticky not a clean drain",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker;
  auto calls = std::make_shared<std::atomic<unsigned int>>(0);
  REQUIRE(worker
              .try_submit([](auto, auto) {},
                          [calls](auto) {
                            calls->fetch_add(1);
                            throw std::runtime_error("fake completion failure");
                          },
                          tasks.try_acquire(),
                          std::chrono::steady_clock::now() + 10s)
              .has_value());
  // Stop may precede physical execution, but an accepted request MUST still
  // invoke exactly one completion; throwing delivery cannot become success.
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(worker.physical_joined());
  REQUIRE(worker.completion_failed());
  REQUIRE_FALSE(worker.active());
  REQUIRE(calls->load() == 1);
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now() + 2s));
}

TEST_CASE("frontend worker: bounded FIFO admits parallel module metadata",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker{64};
  auto work_gate = std::make_shared<Gate>();
  auto completion_gate = std::make_shared<Gate>();
  const ReleaseGates release{work_gate, completion_gate};
  auto batch = std::make_shared<Batch>();
  auto executed = std::make_shared<std::atomic<std::size_t>>(0);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  REQUIRE(worker.capacity() == 64);
  REQUIRE(worker.try_submit(
      [work_gate, executed](auto, auto) {
        executed->fetch_add(1);
        work_gate->enter_and_wait();
      },
      [completion_gate, batch](auto result) {
        completion_gate->enter_and_wait();
        batch->record(0, result);
      },
      tasks.try_acquire(), deadline));
  REQUIRE(work_gate->wait_entered());
  for (std::size_t index = 1; index < 64; ++index) {
    REQUIRE(worker.try_submit(
        [executed](auto, auto) { executed->fetch_add(1); },
        [batch, index](auto result) { batch->record(index, result); },
        tasks.try_acquire(), deadline));
  }
  // In particular the SECOND concurrent module request is accepted, without
  // native-import retries, a second thread or weakening its work callback.
  REQUIRE(worker.outstanding() == 64);
  REQUIRE(tasks.active_for_test() == 64);
  REQUIRE(executed->load() == 1);
  const auto refused = worker.try_submit([](auto, auto) {}, [](auto) {},
                                         tasks.try_acquire(), deadline);
  REQUIRE_FALSE(refused);
  REQUIRE(refused.error() == frontend::FrontendSubmitError::BUSY);
  REQUIRE(tasks.active_for_test() == 64);
  work_gate->release();
  REQUIRE(completion_gate->wait_entered());
  REQUIRE(worker.outstanding() == 64);
  REQUIRE(tasks.active_for_test() == 64);
  completion_gate->release();
  REQUIRE(batch->wait(64));
  // Exercise ring wrap/reuse without closing admission after the first wave.
  auto second_wave = std::make_shared<Batch>();
  for (std::size_t index = 0; index < 17; ++index) {
    REQUIRE(worker.try_submit([](auto, auto) {},
                              [second_wave, index](auto result) {
                                second_wave->record(index, result);
                              },
                              tasks.try_acquire(), deadline));
  }
  REQUIRE(second_wave->wait(17));
  REQUIRE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(worker.outstanding() == 0);
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE(executed->load() == 64);
  const std::scoped_lock lock{batch->mutex, second_wave->mutex};
  for (std::size_t index = 0; index < 64; ++index) {
    REQUIRE(batch->order[index] == index);
    REQUIRE(batch->calls[index] == 1);
    REQUIRE(batch->results[index].has_value());
    REQUIRE(batch->results[index]->has_value());
  }
  for (std::size_t index = 0; index < 17; ++index) {
    REQUIRE(second_wave->order[index] == index);
    REQUIRE(second_wave->calls[index] == 1);
    REQUIRE(second_wave->results[index]);
    REQUIRE(second_wave->results[index]->has_value());
  }
}

TEST_CASE("frontend worker: queued expiry and caller cancellation skip work",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker{3};
  auto gate = std::make_shared<Gate>();
  const ReleaseGates release{gate, {}};
  auto batch = std::make_shared<Batch>();
  auto skipped_ran = std::make_shared<std::atomic<bool>>(false);
  const auto future = std::chrono::steady_clock::now() + 10s;
  REQUIRE(worker.try_submit([gate](auto, auto) { gate->enter_and_wait(); },
                            [batch](auto result) { batch->record(0, result); },
                            tasks.try_acquire(), future));
  REQUIRE(gate->wait_entered());
  const auto expiry = std::chrono::steady_clock::now() + 20ms;
  REQUIRE(
      worker.try_submit([skipped_ran](auto, auto) { skipped_ran->store(true); },
                        [batch](auto result) { batch->record(1, result); },
                        tasks.try_acquire(), expiry));
  std::stop_source cancellation;
  REQUIRE(
      worker.try_submit([skipped_ran](auto, auto) { skipped_ran->store(true); },
                        [batch](auto result) { batch->record(2, result); },
                        tasks.try_acquire(), future, cancellation.get_token()));
  cancellation.request_stop();
  // Wait for the actual absolute deadline, not a guessed workload sleep.
  {
    std::unique_lock lock{gate->mutex};
    static_cast<void>(
        gate->changed.wait_until(lock, expiry, [] { return false; }));
  }
  REQUIRE(worker.outstanding() == 3);
  gate->release();
  REQUIRE(batch->wait(3));
  REQUIRE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE_FALSE(skipped_ran->load());
  REQUIRE(tasks.active_for_test() == 0);
  std::lock_guard lock{batch->mutex};
  REQUIRE((batch->order == std::vector<std::size_t>{0, 1, 2}));
  for (std::size_t index = 0; index < 3; ++index) {
    REQUIRE(batch->calls[index] == 1);
    REQUIRE(batch->results[index]);
  }
  REQUIRE(batch->results[0]->has_value());
  REQUIRE_FALSE(batch->results[1]->has_value());
  REQUIRE(batch->results[1]->error() == frontend::FrontendWorkError::DEADLINE);
  REQUIRE_FALSE(batch->results[2]->has_value());
  REQUIRE(batch->results[2]->error() == frontend::FrontendWorkError::CANCELLED);
}

TEST_CASE("frontend worker: stop physically drains every queued closure FIFO",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker{3};
  auto gate = std::make_shared<Gate>();
  auto completion_gate = std::make_shared<Gate>();
  const ReleaseGates release{gate, completion_gate};
  auto batch = std::make_shared<Batch>();
  auto payload = std::make_shared<int>(1);
  const std::weak_ptr<int> retained = payload;
  auto queued_ran = std::make_shared<std::atomic<bool>>(false);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  REQUIRE(worker.try_submit([gate](auto, auto) { gate->enter_and_wait(); },
                            [completion_gate, batch](auto result) {
                              completion_gate->enter_and_wait();
                              batch->record(0, result);
                            },
                            tasks.try_acquire(), deadline));
  REQUIRE(gate->wait_entered());
  for (std::size_t index = 1; index < 3; ++index) {
    REQUIRE(worker.try_submit(
        [queued_ran, payload](auto, auto) {
          queued_ran->store(true);
          static_cast<void>(payload);
        },
        [batch, index, payload](auto result) {
          batch->record(index, result);
          static_cast<void>(payload);
        },
        tasks.try_acquire(), deadline));
  }
  payload.reset();
  worker.request_stop();
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now()));
  REQUIRE_FALSE(worker.physical_joined());
  REQUIRE(worker.outstanding() == 3);
  REQUIRE(tasks.active_for_test() == 3);
  REQUIRE_FALSE(retained.expired());
  gate->release();
  REQUIRE(completion_gate->wait_entered());
  REQUIRE(worker.outstanding() == 3);
  completion_gate->release();
  REQUIRE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(batch->wait(3));
  REQUIRE_FALSE(queued_ran->load());
  REQUIRE(retained.expired());
  REQUIRE(tasks.active_for_test() == 0);
  std::lock_guard lock{batch->mutex};
  REQUIRE((batch->order == std::vector<std::size_t>{0, 1, 2}));
  for (std::size_t index = 0; index < 3; ++index) {
    REQUIRE(batch->calls[index] == 1);
    REQUIRE(batch->results[index]);
    if (index == 0)
      REQUIRE(batch->results[index]->has_value());
    else {
      REQUIRE_FALSE(batch->results[index]->has_value());
      REQUIRE(batch->results[index]->error() ==
              frontend::FrontendWorkError::CANCELLED);
    }
  }
}

TEST_CASE("frontend worker: closure disposal retains occupied capacity",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker;
  auto gate = std::make_shared<Gate>();
  auto work_gate = std::make_shared<Gate>();
  auto owner = std::make_shared<DisposeGate>();
  owner->gate = gate;
  // Release BOTH barriers before destroying the local payload on REQUIRE
  // unwind; normal work also cannot dispose the payload on the calling thread.
  const ReleaseGates release{work_gate, gate};
  const std::weak_ptr<DisposeGate> lifetime = owner;
  auto outcome = std::make_shared<Outcome>();
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  REQUIRE(worker.try_submit(
      [owner, work_gate](auto, auto) {
        work_gate->enter_and_wait();
        static_cast<void>(owner);
      },
      [owner, outcome](auto result) {
        outcome->record(result);
        static_cast<void>(owner);
      },
      tasks.try_acquire(), deadline));
  REQUIRE(work_gate->wait_entered());
  owner.reset();
  work_gate->release();
  REQUIRE(gate->wait_entered());
  REQUIRE(outcome->wait());
  REQUIRE(worker.outstanding() == 1);
  REQUIRE(tasks.active_for_test() == 1);
  const auto refused = worker.try_submit([](auto, auto) {}, [](auto) {},
                                         tasks.try_acquire(), deadline);
  REQUIRE_FALSE(refused);
  REQUIRE(refused.error() == frontend::FrontendSubmitError::BUSY);
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now()));
  REQUIRE_FALSE(worker.physical_joined());
  gate->release();
  REQUIRE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(lifetime.expired());
  REQUIRE(tasks.active_for_test() == 0);
}

TEST_CASE("frontend worker: capacity is finite before thread construction",
          "[frontend][owned_frontend_worker]") {
  REQUIRE_THROWS_AS(frontend::OwnedFrontendWorker{0}, std::invalid_argument);
  REQUIRE_THROWS_AS(frontend::OwnedFrontendWorker{65}, std::invalid_argument);
}

TEST_CASE("frontend worker: failed delivery cancels queued work without retry",
          "[frontend][owned_frontend_worker]") {
  lifecycle::AsyncTaskRegistry tasks;
  frontend::OwnedFrontendWorker worker{3};
  auto gate = std::make_shared<Gate>();
  const ReleaseGates release{gate, {}};
  auto calls = std::make_shared<std::atomic<std::size_t>>(0);
  auto ran = std::make_shared<std::atomic<bool>>(false);
  auto batch = std::make_shared<Batch>();
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  REQUIRE(worker.try_submit([gate](auto, auto) { gate->enter_and_wait(); },
                            [calls](auto) {
                              calls->fetch_add(1);
                              throw std::runtime_error(
                                  "fake owned completion error");
                            },
                            tasks.try_acquire(), deadline));
  REQUIRE(gate->wait_entered());
  for (std::size_t index = 1; index < 3; ++index) {
    REQUIRE(worker.try_submit(
        [ran](auto, auto) { ran->store(true); },
        [batch, index](auto result) { batch->record(index, result); },
        tasks.try_acquire(), deadline));
  }
  gate->release();
  REQUIRE(batch->wait(2));
  REQUIRE_FALSE(worker.drain(std::chrono::steady_clock::now() + 2s));
  REQUIRE(worker.physical_joined());
  REQUIRE(worker.completion_failed());
  REQUIRE(worker.outstanding() == 0);
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE(calls->load() == 1);
  REQUIRE_FALSE(ran->load());
  std::lock_guard lock{batch->mutex};
  REQUIRE((batch->order == std::vector<std::size_t>{1, 2}));
  for (std::size_t index = 1; index < 3; ++index) {
    REQUIRE(batch->calls[index] == 1);
    REQUIRE(batch->results[index]);
    REQUIRE_FALSE(batch->results[index]->has_value());
    REQUIRE(batch->results[index]->error() ==
            frontend::FrontendWorkError::CANCELLED);
  }
}
