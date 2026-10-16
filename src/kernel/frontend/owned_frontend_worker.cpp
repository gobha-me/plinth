#include "kernel/frontend/owned_frontend_worker.hpp"

#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace plinth::frontend {
namespace {
auto checked_capacity(std::size_t capacity) -> std::size_t {
  if (capacity == 0 || capacity > 64)
    throw std::invalid_argument("frontend.worker.capacity");
  return capacity;
}

struct ForwardStop {
  std::stop_source target;
  auto operator()() const noexcept -> void { target.request_stop(); }
};

auto execute(OwnedFrontendWorker::Work& work, std::stop_token cancellation,
             OwnedFrontendWorker::Deadline deadline) -> FrontendWorkResult {
  if (cancellation.stop_requested())
    return std::unexpected(FrontendWorkError::CANCELLED);
  if (std::chrono::steady_clock::now() >= deadline)
    return std::unexpected(FrontendWorkError::DEADLINE);
  try {
    work(cancellation, deadline);
    // No post-hoc timeout override: an admitted write may actually commit after
    // cancellation/deadline. Work owns its domain result/ACK fence; this
    // success says only that the closure returned normally, NOT that SQL was
    // rolled back or that a disconnected browser received an acknowledgement.
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(FrontendWorkError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(FrontendWorkError::LIMIT_EXCEEDED);
  } catch (...) {
    // Task boundary: accepted work must deliver one fixed, redacted failure,
    // including unknown exception types. Never forward SQL/path/exception text.
    return std::unexpected(FrontendWorkError::FAILED);
  }
}
} // namespace

OwnedFrontendWorker::OwnedFrontendWorker(std::size_t capacity)
    : maximum(checked_capacity(capacity)), worker([this] { run(); }) {
}

OwnedFrontendWorker::~OwnedFrontendWorker() {
  static_cast<void>(
      drain(std::chrono::steady_clock::now() + std::chrono::seconds{5}));
  if (!physical_joined()) std::terminate();
  // Delivery failure is a separate sticky diagnostic; successful destruction
  // never claims clean delivery. Production must check explicit drain's result.
}

auto OwnedFrontendWorker::try_submit(
    Work work, Completion completion,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted, Deadline deadline,
    std::stop_token cancellation) -> std::expected<void, FrontendSubmitError> {
  static_assert(std::is_nothrow_move_constructible_v<Job>);
  if (!work || !completion || !accepted || deadline == Deadline{})
    return std::unexpected(FrontendSubmitError::INVALID);
  {
    std::lock_guard lock{mutex};
    if (!accepting || stop.stop_requested())
      return std::unexpected(FrontendSubmitError::CLOSED);
    if (occupied == maximum) return std::unexpected(FrontendSubmitError::BUSY);
    // Reserve BEFORE allocating stop state. User std::function captures are
    // constructed by the caller before this API; their size remains its bound.
    ++occupied;
    ++preparing;
  }
  // Construct allocating stop state OUTSIDE the mutex. Preparation reservations
  // participate in capacity AND drain until every rejected closure is disposed.
  std::optional<Job> next;
  const auto abandon = [&] {
    next.reset();
    work = nullptr;
    completion = nullptr;
    accepted.reset();
    std::lock_guard lock{mutex};
    --preparing;
    --occupied;
    changed.notify_all();
  };
  try {
    next.emplace(Job{.accepted = std::move(accepted),
                     .work = std::move(work),
                     .completion = std::move(completion),
                     .deadline = deadline,
                     .caller_stop = cancellation,
                     .cancellation = {}});
  } catch (const std::bad_alloc&) {
    abandon();
    return std::unexpected(FrontendSubmitError::UNAVAILABLE);
  } catch (const std::length_error&) {
    abandon();
    return std::unexpected(FrontendSubmitError::UNAVAILABLE);
  }
  {
    std::lock_guard lock{mutex};
    if (accepting && !stop.stop_requested()) {
      pending[(head + waiting) % MAXIMUM_CAPACITY].emplace(std::move(*next));
      ++waiting;
      --preparing;
      changed.notify_all();
      return {};
    }
  }
  abandon();
  return std::unexpected(FrontendSubmitError::CLOSED);
}

auto OwnedFrontendWorker::close_admission() -> void {
  std::lock_guard lock{mutex};
  accepting = false;
}

auto OwnedFrontendWorker::request_stop() -> void {
  close_admission();
  // Stop callbacks may run synchronously. They must remain bounded and must
  // never perform the blocked IO itself; do not run them under our mutex.
  stop.request_stop();
  changed.notify_all();
}

auto OwnedFrontendWorker::drain(Deadline deadline) -> bool {
  request_stop();
  std::unique_lock lock{mutex};
  if (!changed.wait_until(lock, deadline, [this] { return !joining; }))
    return false;
  joining = true;
  bool delivered = false;
  try {
    if (changed.wait_until(lock, deadline, [this] { return finished; })) {
      delivered = !failed_completion;
      lock.unlock();
      // std::thread has no timed join. Terminal state is published; the
      // production watchdog remains the absolute OS/thread-library exit bound.
      if (worker.joinable()) worker.join();
      joined.store(true);
    }
  } catch (...) {
    if (!lock.owns_lock()) lock.lock();
    joining = false;
    lock.unlock();
    changed.notify_all();
    throw;
  }
  if (!lock.owns_lock()) lock.lock();
  joining = false;
  lock.unlock();
  changed.notify_all();
  return delivered;
}

auto OwnedFrontendWorker::active() const -> bool {
  std::lock_guard lock{mutex};
  return occupied != 0;
}
auto OwnedFrontendWorker::outstanding() const -> std::size_t {
  std::lock_guard lock{mutex};
  return occupied;
}
auto OwnedFrontendWorker::completion_failed() const -> bool {
  std::lock_guard lock{mutex};
  return failed_completion;
}
auto OwnedFrontendWorker::physical_joined() const noexcept -> bool {
  return joined.load();
}

auto OwnedFrontendWorker::run() -> void {
  for (;;) {
    std::unique_lock lock{mutex};
    changed.wait(lock, [this] {
      return waiting != 0 || (stop.stop_requested() && preparing == 0);
    });
    if (waiting == 0) {
      finished = true;
      changed.notify_all();
      return;
    }
    {
      Job current = std::move(*pending[head]);
      pending[head].reset();
      head = (head + 1) % MAXIMUM_CAPACITY;
      --waiting;
      lock.unlock();
      // Both stop sources feed the same job token, including a caller already
      // cancelled before this physical start. Callback destruction precedes
      // job/lease disposal and is never performed under the worker mutex.
      const std::stop_callback owner_stop{stop.get_token(),
                                          ForwardStop{current.cancellation}};
      const std::stop_callback caller_stop{current.caller_stop,
                                           ForwardStop{current.cancellation}};
      const auto result = execute(
          current.work, current.cancellation.get_token(), current.deadline);
      try {
        current.completion(result);
      } catch (...) {
        // Completion itself failed, so invoking it a second time would violate
        // exactly-once delivery. Retain one fixed diagnostic, never raw text.
        {
          std::lock_guard failure_lock{mutex};
          failed_completion = true;
          accepting = false;
        }
        // Cancel accepted queued work too, without invoking synchronous stop
        // callbacks under the ring mutex or retrying the failed completion.
        stop.request_stop();
      }
    } // Dispose BOTH closures and the accepted lease before freeing capacity.
    lock.lock();
    --occupied;
    changed.notify_all();
  }
}

} // namespace plinth::frontend
