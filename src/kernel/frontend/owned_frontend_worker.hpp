#pragma once

#include "kernel/lifecycle/async_task_registry.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace plinth::frontend {

enum class FrontendSubmitError { BUSY, CLOSED, INVALID, UNAVAILABLE };
enum class FrontendWorkError { CANCELLED, DEADLINE, FAILED, LIMIT_EXCEEDED };
using FrontendWorkResult = std::expected<void, FrontendWorkError>;

// One persistent physical worker for synchronous metadata/PG/capture work.
// Capacity includes queued, preparing and physically executing jobs through
// completion/closure disposal. The fixed ring never grows; default capacity1
// preserves nonqueued admission. No per-request thread/timer/detached callback.
class OwnedFrontendWorker {
 public:
  using Deadline = std::chrono::steady_clock::time_point;
  using Work = std::function<void(std::stop_token, Deadline)>;
  using Completion = std::function<void(FrontendWorkResult)>;

  // Default-off, per-instance causal test observation. BEFORE_WAIT runs with
  // the worker mutex held after a false predicate; FINISHED runs with that
  // mutex held after terminal publication. Other stages run outside it.
  // Observers must own their captures, must not throw or
  // re-enter the worker, and may block only behind test-owned bounded gates.
  enum class StopObservation {
    ADMISSION_CLOSED,
    STOP_PUBLISHED,
    BEFORE_WAIT,
    STOP_NOTIFIED,
    FINISHED,
  };
  using StopObserver = std::function<void(StopObservation)>;

  // 1..64, validated BEFORE starting the thread; invalid capacity throws.
  explicit OwnedFrontendWorker(std::size_t capacity = 1,
                               StopObserver stop_observer = {});
  ~OwnedFrontendWorker();
  OwnedFrontendWorker(const OwnedFrontendWorker&) = delete;
  auto operator=(const OwnedFrontendWorker&) -> OwnedFrontendWorker& = delete;

  // Both closures must contain owned values, never borrowed request/frame/
  // service pointers. Completion is required, exactly once for accepted work,
  // on this worker outside its mutex; it must not block or throw. Rejected
  // submissions do not invoke completion. Caller stop requests and the absolute
  // deadline never release an active slot or forcibly interrupt filesystem/PG.
  // Work must use this SAME deadline, not renew it per query/capture operation.
  // Queue waiting is part of that deadline. Canceled/expired accepted jobs
  // still complete exactly once in FIFO order, without invoking their Work.
  [[nodiscard]] auto try_submit(
      Work work, Completion completion,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted, Deadline deadline,
      std::stop_token cancellation = {})
      -> std::expected<void, FrontendSubmitError>;
  auto close_admission() -> void;
  auto request_stop() -> void;
  // False means physical join timed out OR an accepted completion threw.
  // A timed-out owner must remain alive for explicit retry/watchdog handling.
  [[nodiscard]] auto drain(Deadline deadline) -> bool;
  [[nodiscard]] auto active() const -> bool;
  [[nodiscard]] auto outstanding() const -> std::size_t;
  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return maximum;
  }
  [[nodiscard]] auto completion_failed() const -> bool;
  [[nodiscard]] auto physical_joined() const noexcept -> bool;

 private:
  struct Job {
    // Last destroyed: process drain cannot advance before closures disappear.
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
    Work work;
    Completion completion;
    Deadline deadline;
    std::stop_token caller_stop;
    std::stop_source cancellation;
  };
  auto run() -> void;
  static constexpr std::size_t MAXIMUM_CAPACITY = 64;
  const std::size_t maximum;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::array<std::optional<Job>, MAXIMUM_CAPACITY> pending;
  std::size_t head = 0;
  std::size_t waiting = 0;
  std::size_t preparing = 0;
  std::size_t occupied = 0;
  bool accepting = true;
  bool finished = false;
  bool joining = false;
  bool failed_completion = false;
  std::atomic<bool> joined{false};
  std::stop_source stop;
  const StopObserver stop_observer;
  // Declared last, started only after all state is initialized.
  std::thread worker;
};

} // namespace plinth::frontend
