// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/js/bridge_context.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <trantor/net/EventLoop.h>
#include <unordered_set>
#include <utility>

namespace plinth::js {

// Independently alive across JS retirement. A native worker may read
// only immutable snapshots. JS access is exclusively through visit() on the
// owner loop; retire() fences that access before the context can be freed.
// This does not cancel SQL or own a process-wide DbClient.
class ExecutionLifetime
    : public std::enable_shared_from_this<ExecutionLifetime> {
 private:
  // Declared first, hence destroyed last: dependency snapshots and queued
  // native owners are released BEFORE the process drain lease publishes zero.
  std::shared_ptr<lifecycle::AsyncTaskLease> task;

 public:
  ExecutionLifetime(BridgeContext& context, trantor::EventLoop& loop,
                    std::shared_ptr<lifecycle::AsyncTaskLease> admitted)
      : task(std::move(admitted)),
        result_size_limit(context.async_result_size_limit_bytes),
        database_owner(context.extension_database_clients), owner_loop(loop),
        current(&context) {}

  auto accepted_lease() const -> std::shared_ptr<lifecycle::AsyncTaskLease> {
    return task;
  }

  auto begin_commit(std::uint64_t scope) -> void {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    pending_commits.insert(scope);
  }
  auto finish_commit(std::uint64_t scope) -> void {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    pending_commits.erase(scope);
  }
  auto commit_pending(std::uint64_t scope) const -> bool {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return pending_commits.contains(scope);
  }

  // Called without RuntimePool mutex held. It never writes through a stale
  // caller BC pointer after a separate active() observation.
  auto request_cancel() -> bool {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current == nullptr) {
      return false;
    }
    current->cancelled.store(true, std::memory_order_release);
    wake();
    return true;
  }

  template <typename Function> auto visit(Function&& function) -> bool {
    if (!owner_loop.isInLoopThread()) {
      return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current == nullptr) {
      return false;
    }
    std::forward<Function>(function)(*current);
    return true;
  }

  [[nodiscard]] auto active() const -> bool {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return current != nullptr;
  }

  // The timer callback holds only a weak lease. Timer cancellation cannot
  // keep global task admission permanently active after a normal return.
  auto arm_wake(std::chrono::steady_clock::time_point deadline) -> void {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current == nullptr) {
      return;
    }
    if (timer != trantor::InvalidTimerId) {
      owner_loop.invalidateTimer(timer);
    }
    const auto epoch = ++timer_epoch;
    const auto seconds = std::chrono::duration<double>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
    std::weak_ptr<ExecutionLifetime> weak = shared_from_this();
    timer = owner_loop.runAfter(seconds > 0.0 ? seconds : 0.0, [weak, epoch] {
      if (auto state = weak.lock()) {
        std::lock_guard<std::recursive_mutex> guard(state->mutex);
        if (state->current != nullptr && state->timer_epoch == epoch) {
          state->timer = trantor::InvalidTimerId;
          state->wake_locked();
        }
      }
    });
  }

  auto wake() -> void {
    if (!owner_loop.isInLoopThread()) {
      std::weak_ptr<ExecutionLifetime> weak = shared_from_this();
      owner_loop.queueInLoop([weak] {
        if (auto state = weak.lock()) {
          state->wake();
        }
      });
      return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current != nullptr) {
      wake_locked();
    }
  }

  [[nodiscard]] auto consume_wake() -> bool {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current == nullptr || pending_wake) {
      pending_wake = false;
      return true;
    }
    return false;
  }

  // Returns false if a wake arrived before suspension. Exact wait leases
  // fence destruction/cancellation; no old coroutine handle survives revoke.
  auto suspend(std::coroutine_handle<> handle, std::size_t& lease) -> bool {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (current == nullptr || pending_wake) {
      pending_wake = false;
      return false;
    }
    lease = ++wait_epoch;
    waiter = handle;
    return true;
  }

  auto clear_waiter(std::size_t lease) -> void {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (lease != 0 && lease == wait_epoch) {
      waiter = {};
    }
  }

  template <typename ReleaseContext>
  auto retire(ReleaseContext&& release_context) -> void {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    current = nullptr;
    waiter = {};
    pending_wake = false;
    ++wait_epoch;
    ++timer_epoch;
    if (timer != trantor::InvalidTimerId) {
      owner_loop.invalidateTimer(timer);
      timer = trantor::InvalidTimerId;
    }
    // LAST BC access, performed by the caller under this same lifetime lock.
    // A defensive destroy seeing a null atomic may free BC immediately;
    // after this callback retirement touches only independent state.
    std::forward<ReleaseContext>(release_context)();
  }

  const std::size_t result_size_limit;
  // Retains the actual pool DB domain, never a replacement credential/client.
  // RuntimePool closes this domain on its lifecycle owner before releasing
  // its durable owner; a native worker must not initiate whole-client close.
  const std::shared_ptr<ExtensionDatabaseClients> database_owner;

 private:
  auto wake_locked() -> void {
    auto suspended = std::exchange(waiter, {});
    pending_wake = !suspended;
    if (suspended) {
      suspended.resume();
      // No BC access follows resume: the caller may synchronously destroy it.
    }
  }

  trantor::EventLoop& owner_loop;
  mutable std::recursive_mutex mutex;
  BridgeContext* current;
  std::coroutine_handle<> waiter{};
  trantor::TimerId timer{trantor::InvalidTimerId};
  std::size_t timer_epoch = 0;
  std::size_t wait_epoch = 0;
  bool pending_wake = false;
  std::unordered_set<std::uint64_t> pending_commits;
};

} // namespace plinth::js
