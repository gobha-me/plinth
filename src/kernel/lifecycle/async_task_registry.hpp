// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

namespace plinth::lifecycle {

struct AsyncTaskState;

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

// Owns standalone Drogon coroutines that are not already nested beneath an
// extension RuntimePool, the JS stress pool, or the events writer. Admission
// closes with ingress; each accepted closure captures a lease until its
// coroutine frame is destroyed.
class AsyncTaskRegistry {
 public:
  AsyncTaskRegistry();
  ~AsyncTaskRegistry();

  AsyncTaskRegistry(const AsyncTaskRegistry&) = delete;
  AsyncTaskRegistry(AsyncTaskRegistry&&) = delete;
  auto operator=(const AsyncTaskRegistry&) -> AsyncTaskRegistry& = delete;
  auto operator=(AsyncTaskRegistry&&) -> AsyncTaskRegistry& = delete;

  [[nodiscard]] auto try_acquire() -> std::shared_ptr<AsyncTaskLease>;
  auto close_admission() -> void;
  [[nodiscard]] auto drain(std::chrono::milliseconds timeout) -> bool;

  [[nodiscard]] auto accepting_for_test() const -> bool;
  [[nodiscard]] auto active_for_test() const -> std::size_t;

 private:
  std::shared_ptr<AsyncTaskState> state;
};

[[nodiscard]] auto async_tasks() -> AsyncTaskRegistry&;

} // namespace plinth::lifecycle
