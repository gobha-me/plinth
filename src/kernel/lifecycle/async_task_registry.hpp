// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace plinth::lifecycle {

struct AsyncTaskState;

// Preallocated cleanup ownership, retried only by an explicit lifecycle drain.
// Native owners request asynchronous rollback without waiting or closing a
// running client. An actual database-domain lifecycle owner may resume its
// ALREADY STARTED failed close, inside the same drain deadline. Failure must
// retain resources and stay visible; execution cancellation never closes a
// client and no cleanup retry may create a fresh independent deadline.
class AsyncCleanupOwner {
 public:
  AsyncCleanupOwner() = default;
  AsyncCleanupOwner(const AsyncCleanupOwner&) = delete;
  auto operator=(const AsyncCleanupOwner&) -> AsyncCleanupOwner& = delete;
  AsyncCleanupOwner(AsyncCleanupOwner&&) = delete;
  auto operator=(AsyncCleanupOwner&&) -> AsyncCleanupOwner& = delete;
  virtual ~AsyncCleanupOwner() = default;
  virtual auto retry_cleanup(
      std::chrono::steady_clock::time_point deadline) noexcept -> bool = 0;

 private:
  friend class AsyncTaskRegistry;
  std::shared_ptr<AsyncCleanupOwner> next;
  std::uint64_t revision = 0;
  bool registered = false;
};

class AsyncTaskLease {
 public:
  class ConstructionKey {
   private:
    friend class AsyncTaskRegistry;
    ConstructionKey() = default;
  };

  AsyncTaskLease(std::shared_ptr<AsyncTaskState> state_in, ConstructionKey);
  ~AsyncTaskLease();

  AsyncTaskLease(const AsyncTaskLease&) = delete;
  AsyncTaskLease(AsyncTaskLease&&) = delete;
  auto operator=(const AsyncTaskLease&) -> AsyncTaskLease& = delete;
  auto operator=(AsyncTaskLease&&) -> AsyncTaskLease& = delete;

 private:
  std::shared_ptr<AsyncTaskState> state;
};

// Owns accepted asynchronous executions and their native descendants. Admission
// closes with ingress; already accepted work explicitly shares its lease
// through nested dispatch and driver callbacks. JS retirement does not end
// ownership of a native operation which still needs the process DB, logging or
// event loops.
class AsyncTaskRegistry {
 public:
  AsyncTaskRegistry();
  ~AsyncTaskRegistry();

  AsyncTaskRegistry(const AsyncTaskRegistry&) = delete;
  AsyncTaskRegistry(AsyncTaskRegistry&&) = delete;
  auto operator=(const AsyncTaskRegistry&) -> AsyncTaskRegistry& = delete;
  auto operator=(AsyncTaskRegistry&&) -> AsyncTaskRegistry& = delete;

  [[nodiscard]] auto try_acquire() -> std::shared_ptr<AsyncTaskLease>;
  // The record must have been allocated before acquiring native resources.
  // Intrusive retention does not allocate in a cleanup failure path.
  auto retain_cleanup(std::shared_ptr<AsyncCleanupOwner> owner) -> void;
  // Signal progress of an ALREADY retained cleanup obligation. This neither
  // registers an owner nor changes its revision/count, so late completion
  // cannot resurrect an owner already removed by a successful drain.
  auto notify_cleanup_progress() -> void;
  auto close_admission() -> void;
  [[nodiscard]] auto drain(std::chrono::milliseconds timeout) -> bool;

  [[nodiscard]] auto accepting_for_test() const -> bool;
  [[nodiscard]] auto active_for_test() const -> std::size_t;

 private:
  auto retry_cleanups(std::chrono::steady_clock::time_point deadline) -> bool;
  std::shared_ptr<AsyncTaskState> state;
  std::mutex cleanup_mutex;
  std::timed_mutex retry_mutex;
  std::shared_ptr<AsyncCleanupOwner> cleanup_head;
};

[[nodiscard]] auto async_tasks() -> AsyncTaskRegistry&;

} // namespace plinth::lifecycle
