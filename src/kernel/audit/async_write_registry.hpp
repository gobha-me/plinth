// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace plinth::log::detail {

// Separate from root task admission: final database producers may still audit
// after root admission closes. The coordinator retains Drogon's database until
// this registry settles. Neither the registry nor its leases owns that client.
class AuditWriteRegistry {
  struct State {
    std::mutex mutex;
    std::condition_variable settled;
    std::atomic<bool> accepting{false};
    std::atomic<bool> failed{false};
    bool initialized{false};
    bool sealed{false};
    std::size_t active{0};
  };

 public:
  class Lease {
    friend class AuditWriteRegistry;
    struct ConstructionKey {};
    enum class Outcome { Pending, Success, Failure, Skipped };

   public:
    Lease(std::shared_ptr<State> state, ConstructionKey)
        : state_(std::move(state)) {}
    Lease(const Lease&) = delete;
    auto operator=(const Lease&) -> Lease& = delete;
    Lease(Lease&&) = delete;
    auto operator=(Lease&&) -> Lease& = delete;

    ~Lease() {
      if (!armed_) {
        return;
      }
      if (outcome_.load(std::memory_order_acquire) == Outcome::Pending) {
        record_failure();
      }
      {
        std::lock_guard lock(state_->mutex);
        --state_->active;
      }
      state_->settled.notify_all();
    }

    [[nodiscard]] auto succeed() noexcept -> bool {
      return finish(Outcome::Success);
    }
    [[nodiscard]] auto fail() noexcept -> bool {
      if (!finish(Outcome::Failure)) {
        return false;
      }
      record_failure();
      return true;
    }
    [[nodiscard]] auto skip() noexcept -> bool {
      return finish(Outcome::Skipped);
    }
    auto record_failure() noexcept -> void {
      state_->failed.store(true, std::memory_order_release);
    }

   private:
    auto finish(Outcome outcome) noexcept -> bool {
      auto expected = Outcome::Pending;
      return outcome_.compare_exchange_strong(expected, outcome,
                                              std::memory_order_acq_rel);
    }
    std::shared_ptr<State> state_;
    std::atomic<Outcome> outcome_{Outcome::Pending};
    bool armed_{false};
  };

  AuditWriteRegistry() : state_(std::make_shared<State>()) {}
  ~AuditWriteRegistry() = default;
  AuditWriteRegistry(const AuditWriteRegistry&) = delete;
  auto operator=(const AuditWriteRegistry&) -> AuditWriteRegistry& = delete;
  AuditWriteRegistry(AuditWriteRegistry&&) = delete;
  auto operator=(AuditWriteRegistry&&) -> AuditWriteRegistry& = delete;

  [[nodiscard]] auto initialized() const -> bool {
    std::lock_guard lock(state_->mutex);
    return state_->initialized;
  }
  [[nodiscard]] auto start() -> bool {
    std::lock_guard lock(state_->mutex);
    if (state_->sealed) {
      return false;
    }
    state_->initialized = true;
    state_->accepting.store(true, std::memory_order_release);
    return true;
  }
  [[nodiscard]] auto accepting() const noexcept -> bool {
    return state_->accepting.load(std::memory_order_acquire);
  }
  auto pause() noexcept -> void {
    std::lock_guard lock(state_->mutex);
    state_->accepting.store(false, std::memory_order_release);
  }
  auto close() noexcept -> void {
    std::lock_guard lock(state_->mutex);
    state_->sealed = true;
    state_->accepting.store(false, std::memory_order_release);
  }
  [[nodiscard]] auto closed() const -> bool {
    std::lock_guard lock(state_->mutex);
    return state_->sealed;
  }
  [[nodiscard]] auto try_acquire() -> std::shared_ptr<Lease> {
    if (!accepting()) {
      return nullptr;
    }
    // Allocation precedes registration, so an allocation failure cannot leave
    // an orphan count. Admission is rechecked under the same lock as close().
    auto lease = std::make_shared<Lease>(state_, Lease::ConstructionKey{});
    std::lock_guard lock(state_->mutex);
    if (!state_->accepting.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    ++state_->active;
    lease->armed_ = true;
    return lease;
  }
  [[nodiscard]] auto active() const -> std::size_t {
    std::lock_guard lock(state_->mutex);
    return state_->active;
  }
  [[nodiscard]] auto failed() const noexcept -> bool {
    return state_->failed.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto wait_for_settlement(std::chrono::milliseconds timeout)
      -> bool {
    if (timeout < std::chrono::milliseconds::zero()) {
      timeout = std::chrono::milliseconds::zero();
    }
    std::unique_lock lock(state_->mutex);
    if (!state_->settled.wait_for(lock, timeout,
                                  [this] { return state_->active == 0; })) {
      return false;
    }
    return !failed();
  }

 private:
  std::shared_ptr<State> state_;
};

} // namespace plinth::log::detail
