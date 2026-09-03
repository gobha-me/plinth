// SPDX-License-Identifier: MIT

#include "kernel/lifecycle/async_task_registry.hpp"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace plinth::lifecycle {

struct AsyncTaskState {
  mutable std::mutex mu;
  std::condition_variable cv;
  bool accepting = true;
  std::size_t active = 0;
};

AsyncTaskLease::AsyncTaskLease(std::shared_ptr<AsyncTaskState> state_in,
                               ConstructionKey)
    : state(std::move(state_in)) {
  ++state->active;
}

AsyncTaskLease::~AsyncTaskLease() {
  {
    std::lock_guard lock(state->mu);
    --state->active;
  }
  state->cv.notify_all();
}

AsyncTaskRegistry::AsyncTaskRegistry()
    : state(std::make_shared<AsyncTaskState>()) {
}

AsyncTaskRegistry::~AsyncTaskRegistry() = default;

auto AsyncTaskRegistry::try_acquire() -> std::shared_ptr<AsyncTaskLease> {
  std::lock_guard lock(state->mu);
  if (!state->accepting) {
    return {};
  }
  auto lease = std::make_shared<AsyncTaskLease>(
      state, AsyncTaskLease::ConstructionKey{});
  return lease;
}

auto AsyncTaskRegistry::close_admission() -> void {
  std::lock_guard lock(state->mu);
  state->accepting = false;
}

auto AsyncTaskRegistry::drain(std::chrono::milliseconds timeout) -> bool {
  std::unique_lock lock(state->mu);
  return state->cv.wait_for(lock, timeout,
                            [this] { return state->active == 0; });
}

auto AsyncTaskRegistry::accepting_for_test() const -> bool {
  std::lock_guard lock(state->mu);
  return state->accepting;
}

auto AsyncTaskRegistry::active_for_test() const -> std::size_t {
  std::lock_guard lock(state->mu);
  return state->active;
}

auto async_tasks() -> AsyncTaskRegistry& {
  static AsyncTaskRegistry registry;
  return registry;
}

} // namespace plinth::lifecycle
