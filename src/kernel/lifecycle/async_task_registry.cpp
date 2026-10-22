// SPDX-License-Identifier: MIT

#include "kernel/lifecycle/async_task_registry.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <utility>

namespace plinth::lifecycle {

struct AsyncTaskState {
  mutable std::mutex mu;
  std::condition_variable cv;
  bool accepting = true;
  std::size_t active = 0;
  std::size_t cleanup_count = 0;
  std::uint64_t cleanup_epoch = 0;
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

AsyncTaskRegistry::~AsyncTaskRegistry() {
  // An unverified native transaction cannot be released into destructor COMMIT.
  // Normal shutdown drains/retries these owners before registry destruction.
  if (cleanup_head) {
    std::terminate();
  }
}

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

auto AsyncTaskRegistry::retain_cleanup(std::shared_ptr<AsyncCleanupOwner> owner)
    -> void {
  if (!owner) {
    return;
  }
  {
    std::lock_guard cleanup_lock(cleanup_mutex);
    std::lock_guard state_lock(state->mu);
    ++owner->revision;
    ++state->cleanup_epoch;
    if (!owner->registered) {
      owner->registered = true;
      owner->next = std::move(cleanup_head);
      cleanup_head = std::move(owner);
      ++state->cleanup_count;
    }
  }
  state->cv.notify_all();
}

auto AsyncTaskRegistry::notify_cleanup_progress() -> void {
  {
    std::lock_guard lock(state->mu);
    ++state->cleanup_epoch;
  }
  state->cv.notify_all();
}

auto AsyncTaskRegistry::retry_cleanups(
    std::chrono::steady_clock::time_point deadline) -> bool {
  bool progressed = false;
  std::shared_ptr<AsyncCleanupOwner> local;
  {
    std::lock_guard lock(cleanup_mutex);
    local = std::move(cleanup_head);
  }
  while (local) {
    auto owner = std::move(local);
    std::uint64_t revision;
    {
      std::lock_guard lock(cleanup_mutex);
      local = std::move(owner->next);
      revision = owner->revision;
    }
    const bool completed = std::chrono::steady_clock::now() < deadline &&
                           owner->retry_cleanup(deadline);
    {
      std::lock_guard cleanup_lock(cleanup_mutex);
      std::lock_guard state_lock(state->mu);
      if (completed && revision == owner->revision) {
        owner->registered = false;
        --state->cleanup_count;
        progressed = true;
      } else {
        owner->next = std::move(cleanup_head);
        cleanup_head = owner;
      }
    }
    // Owner release, including its dependency/lease destruction, is outside
    // both registry locks. Driver callbacks may still retain accepted lineage.
    owner.reset();
  }
  state->cv.notify_all();
  return progressed;
}

auto AsyncTaskRegistry::drain(std::chrono::milliseconds timeout) -> bool {
  const bool unbounded = timeout == std::chrono::milliseconds::max();
  const auto deadline = unbounded ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() + timeout;
  std::unique_lock retry_lock(retry_mutex, std::defer_lock);
  if (unbounded) {
    retry_lock.lock();
  } else if (!retry_lock.try_lock() &&
             !retry_lock.try_lock_for(deadline -
                                      std::chrono::steady_clock::now())) {
    return false;
  }
  for (;;) {
    std::uint64_t before_retry;
    {
      std::lock_guard lock(state->mu);
      before_retry = state->cleanup_epoch;
    }
    const bool progressed = retry_cleanups(deadline);
    std::unique_lock lock(state->mu);
    if (state->active == 0 && state->cleanup_count == 0) {
      return true;
    }
    // Closing an already-stopping domain may make a native rollback record
    // from the same pass retryable. Retry that dependency immediately instead
    // of waiting for a callback which a completed domain will never deliver.
    // Progress may also have arrived INSIDE that pass after a record returned
    // false. Observe the epoch across the whole pass, not only before parking.
    if ((progressed || state->cleanup_epoch != before_retry) &&
        std::chrono::steady_clock::now() < deadline) {
      continue;
    }
    const auto epoch = state->cleanup_epoch;
    if (!state->cv.wait_until(lock, deadline, [this, epoch] {
          return (state->active == 0 && state->cleanup_count == 0) ||
                 state->cleanup_epoch != epoch;
        })) {
      return false;
    }
  }
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
