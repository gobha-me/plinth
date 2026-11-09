// SPDX-License-Identifier: MIT
#include "kernel/db/operations.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct AllocationFailure {
  std::atomic<bool> armed{false};
  std::atomic<std::size_t> rejected{0};
};

// Only the worker installs this pointer. Its observer arms the atomic AFTER
// PostgreSQL reports the actual query waiting on the blocker's advisory lock.
thread_local AllocationFailure* allocation_failure = nullptr;

class TrackAllocations {
 public:
  explicit TrackAllocations(AllocationFailure& failure) noexcept {
    allocation_failure = &failure;
  }
  ~TrackAllocations() { allocation_failure = nullptr; }
  TrackAllocations(const TrackAllocations&) = delete;
  auto operator=(const TrackAllocations&) -> TrackAllocations& = delete;
};

auto check_allocation() -> void {
  if (allocation_failure != nullptr &&
      allocation_failure->armed.load(std::memory_order_acquire)) {
    allocation_failure->rejected.fetch_add(1, std::memory_order_relaxed);
    throw std::bad_alloc{};
  }
}

auto allocate(std::size_t size) -> void* {
  check_allocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc{};
}

auto allocate_aligned(std::size_t size, std::align_val_t alignment) -> void* {
  check_allocation();
  void* memory = nullptr;
  const auto align =
      std::max(static_cast<std::size_t>(alignment), alignof(void*));
  if (::posix_memalign(&memory, align, size == 0 ? 1 : size) == 0)
    return memory;
  throw std::bad_alloc{};
}

} // namespace

// Define every replaceable family, including sized/aligned/nothrow deletion.
// All successful allocations use malloc/posix_memalign and all frees use free;
// ASan must never see a malloc/new or malloc/delete family mismatch. libpq's
// native C allocator remains untouched. No other test executable links this TU.
auto operator new(std::size_t size) -> void* {
  return allocate(size);
}
auto operator new[](std::size_t size) -> void* {
  return allocate(size);
}
auto operator new(std::size_t size, const std::nothrow_t&) noexcept -> void* {
  try {
    return allocate(size);
  } catch (...) {
    return nullptr;
  }
}
auto operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
    -> void* {
  return ::operator new(size, tag);
}
auto operator new(std::size_t size, std::align_val_t alignment) -> void* {
  return allocate_aligned(size, alignment);
}
auto operator new[](std::size_t size, std::align_val_t alignment) -> void* {
  return allocate_aligned(size, alignment);
}
auto operator new(std::size_t size, std::align_val_t alignment,
                  const std::nothrow_t&) noexcept -> void* {
  try {
    return allocate_aligned(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
auto operator new[](std::size_t size, std::align_val_t alignment,
                    const std::nothrow_t& tag) noexcept -> void* {
  return ::operator new(size, alignment, tag);
}
auto operator delete(void* memory) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory) noexcept -> void {
  std::free(memory);
}
auto operator delete(void* memory, std::size_t) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory, std::size_t) noexcept -> void {
  std::free(memory);
}
auto operator delete(void* memory, const std::nothrow_t&) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory, const std::nothrow_t&) noexcept -> void {
  std::free(memory);
}
auto operator delete(void* memory, std::align_val_t) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory, std::align_val_t) noexcept -> void {
  std::free(memory);
}
auto operator delete(void* memory, std::size_t, std::align_val_t) noexcept
    -> void {
  std::free(memory);
}
auto operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
    -> void {
  std::free(memory);
}
auto operator delete(void* memory, std::align_val_t,
                     const std::nothrow_t&) noexcept -> void {
  std::free(memory);
}
auto operator delete[](void* memory, std::align_val_t,
                       const std::nothrow_t&) noexcept -> void {
  std::free(memory);
}

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr auto ALIGNMENT = std::align_val_t{64};
constexpr std::size_t ALLOCATION_SIZE = 97;

auto env_or(const char* name, const char* fallback) -> const char* {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : value;
}

auto open_database() -> Connection {
  const std::array<const char*, 8> keywords{
      "host",    "port", "user", "password", "dbname", "connect_timeout",
      "options", nullptr};
  const std::array<const char*, 8> values{
      std::getenv("PLINTH_PG_HOST"),
      env_or("PLINTH_PG_PORT", "5432"),
      env_or("PLINTH_PG_USER", "plinth"),
      env_or("PLINTH_PG_PASSWORD", "plinth"),
      env_or("PLINTH_PG_DATABASE", "plinth"),
      "3",
      "-c statement_timeout=12000",
      nullptr};
  // No connection strings or native error text enter assertion diagnostics.
  return {PQconnectdbParams(keywords.data(), values.data(), 0), PQfinish};
}

auto scalar(PGconn* connection, const char* sql, const char* first,
            const char* second = nullptr) -> bool {
  const std::array<const char*, 2> values{first, second};
  plinth::db::OperationScope operation{std::stop_token{}, 1s};
  Result result{plinth::db::exec_params(connection, sql, second ? 2 : 1,
                                        nullptr, values.data(), nullptr,
                                        nullptr, 0),
                PQclear};
  REQUIRE(result);
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(result.get()) == 1);
  REQUIRE(PQnfields(result.get()) == 1);
  return std::string_view{PQgetvalue(result.get(), 0, 0)} == "t";
}

struct Outcome {
  bool returned = false;
  bool escaped_exception = false;
  bool fatal_result = false;
  bool cancel_failed = false;
  bool disconnected = false;
  bool checkpoint_rejected = false;
  bool denial_at_return = false;
  bool denial_after_close = false;
  std::size_t rejected_at_return = 0;
  std::size_t rejected_after_checkpoint = 0;
  Clock::time_point returned_at{};
  Clock::time_point closed_at{};
};

struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  Outcome outcome;
  bool returned = false;
  bool allow_close = false;
  bool closed = false;
};

class ReleaseWorker {
 public:
  explicit ReleaseWorker(Completion& completion) : completion(completion) {}
  ~ReleaseWorker() { release(); }
  ReleaseWorker(const ReleaseWorker&) = delete;
  auto operator=(const ReleaseWorker&) -> ReleaseWorker& = delete;

  auto release() -> void {
    {
      const std::lock_guard lock(completion.mutex);
      completion.allow_close = true;
    }
    completion.changed.notify_all();
  }

 private:
  Completion& completion;
};

auto run_query(Connection connection, std::stop_token stop,
               Clock::time_point deadline, const std::string& lock_key,
               AllocationFailure& failure, Completion& completion) -> void {
  {
    TrackAllocations tracking{failure};
    plinth::db::OperationScope operation{stop, deadline, 20s};
    Outcome outcome;
    const char* parameter = lock_key.c_str();
    try {
      Result result{plinth::db::exec_params(
                        connection.get(),
                        "SELECT pg_catalog.pg_advisory_lock(208, $1::integer)",
                        1, nullptr, &parameter, nullptr, nullptr, 0),
                    PQclear};
      outcome.returned = true;
      outcome.fatal_result =
          result && PQresultStatus(result.get()) == PGRES_FATAL_ERROR;
    } catch (...) {
      // An escaping allocation failure is the regression, even if this test's
      // connection owner can subsequently close the backend during unwinding.
      outcome.escaped_exception = true;
    }
    outcome.returned_at = Clock::now();
    outcome.cancel_failed = operation.cancellation_failed();
    outcome.disconnected = PQstatus(connection.get()) == CONNECTION_BAD &&
                           PQsocket(connection.get()) < 0;
    outcome.denial_at_return = failure.armed.load(std::memory_order_acquire);
    outcome.rejected_at_return = failure.rejected.load();
    try {
      // Native cleanup is already complete. Constructing its sticky failure
      // diagnostic now proves that the worker still rejects real C++ new.
      operation.checkpoint();
    } catch (const std::bad_alloc&) {
      outcome.checkpoint_rejected = true;
    } catch (...) {
    }
    outcome.rejected_after_checkpoint = failure.rejected.load();
    {
      const std::lock_guard lock(completion.mutex);
      completion.outcome = outcome;
      completion.returned = true;
    }
    completion.changed.notify_all();
    {
      std::unique_lock lock(completion.mutex);
      completion.changed.wait(lock,
                              [&completion] { return completion.allow_close; });
    }
    // Keep denial armed through PQfinish as well as through cancel_query.
    connection.reset();
    outcome.denial_after_close = failure.armed.load(std::memory_order_acquire);
    outcome.closed_at = Clock::now();
    {
      const std::lock_guard lock(completion.mutex);
      completion.outcome = outcome;
      completion.closed = true;
    }
  }
  completion.changed.notify_all();
}

} // namespace

TEST_CASE("PostgreSQL cancellation allocator pairs every family and isolates "
          "its worker",
          "[db][allocation][unit]") {
  // Explicit calls prevent new-expression allocation elision. Exercise each
  // deallocation overload with the same C allocation family under sanitizers.
  void* memory = ::operator new(0);
  REQUIRE(memory != nullptr);
  ::operator delete(memory);
  memory = ::operator new[](ALLOCATION_SIZE);
  ::operator delete[](memory);
  memory = ::operator new(ALLOCATION_SIZE);
  ::operator delete(memory, ALLOCATION_SIZE);
  memory = ::operator new[](ALLOCATION_SIZE);
  ::operator delete[](memory, ALLOCATION_SIZE);
  memory = ::operator new(ALLOCATION_SIZE, std::nothrow);
  REQUIRE(memory != nullptr);
  ::operator delete(memory, std::nothrow);
  memory = ::operator new[](ALLOCATION_SIZE, std::nothrow);
  REQUIRE(memory != nullptr);
  ::operator delete[](memory, std::nothrow);
  for (bool array : {false, true}) {
    for (bool sized : {false, true}) {
      memory = array ? ::operator new[](ALLOCATION_SIZE, ALIGNMENT)
                     : ::operator new(ALLOCATION_SIZE, ALIGNMENT);
      CHECK(reinterpret_cast<std::uintptr_t>(memory) % 64 == 0);
      if (array && sized) {
        ::operator delete[](memory, ALLOCATION_SIZE, ALIGNMENT);
      } else if (array) {
        ::operator delete[](memory, ALIGNMENT);
      } else if (sized) {
        ::operator delete(memory, ALLOCATION_SIZE, ALIGNMENT);
      } else {
        ::operator delete(memory, ALIGNMENT);
      }
    }
    memory = array ? ::operator new[](ALLOCATION_SIZE, ALIGNMENT, std::nothrow)
                   : ::operator new(ALLOCATION_SIZE, ALIGNMENT, std::nothrow);
    REQUIRE(memory != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(memory) % 64 == 0);
    if (array) {
      ::operator delete[](memory, ALIGNMENT, std::nothrow);
    } else {
      ::operator delete(memory, ALIGNMENT, std::nothrow);
    }
  }

  AllocationFailure failure;
  failure.armed.store(true);
  std::array<bool, 8> rejected{};
  Completion completion;
  std::jthread worker{[&] {
    TrackAllocations tracking{failure};
    auto throws_bad_alloc = [](auto allocate_one) {
      try {
        allocate_one();
      } catch (const std::bad_alloc&) {
        return true;
      }
      return false;
    };
    rejected[0] = throws_bad_alloc(
        [] { ::operator delete(::operator new(ALLOCATION_SIZE)); });
    rejected[1] = throws_bad_alloc(
        [] { ::operator delete[](::operator new[](ALLOCATION_SIZE)); });
    rejected[2] = throws_bad_alloc([] {
      ::operator delete(::operator new(ALLOCATION_SIZE, ALIGNMENT), ALIGNMENT);
    });
    rejected[3] = throws_bad_alloc([] {
      ::operator delete[](::operator new[](ALLOCATION_SIZE, ALIGNMENT),
                          ALIGNMENT);
    });
    auto* scalar = ::operator new(ALLOCATION_SIZE, std::nothrow);
    rejected[4] = scalar == nullptr;
    ::operator delete(scalar, std::nothrow);
    auto* array = ::operator new[](ALLOCATION_SIZE, std::nothrow);
    rejected[5] = array == nullptr;
    ::operator delete[](array, std::nothrow);
    auto* aligned = ::operator new(ALLOCATION_SIZE, ALIGNMENT, std::nothrow);
    rejected[6] = aligned == nullptr;
    ::operator delete(aligned, ALIGNMENT, std::nothrow);
    auto* aligned_array =
        ::operator new[](ALLOCATION_SIZE, ALIGNMENT, std::nothrow);
    rejected[7] = aligned_array == nullptr;
    ::operator delete[](aligned_array, ALIGNMENT, std::nothrow);
    {
      std::unique_lock lock(completion.mutex);
      completion.returned = true;
      completion.changed.notify_all();
      completion.changed.wait(lock, [&] { return completion.allow_close; });
    }
  }};
  ReleaseWorker release_worker{completion};
  {
    std::unique_lock lock(completion.mutex);
    REQUIRE(completion.changed.wait_until(lock, Clock::now() + 5s,
                                          [&] { return completion.returned; }));
  }
  // The same armed control must not reject the Catch/observer thread's new.
  memory = ::operator new(ALLOCATION_SIZE, std::nothrow);
  const bool observer_allocation_succeeded = memory != nullptr;
  ::operator delete(memory, std::nothrow);
  release_worker.release();
  worker.join();
  CHECK_FALSE(worker.joinable());
  CHECK(observer_allocation_succeeded);
  CHECK(failure.rejected.load() == rejected.size());
  for (bool failed : rejected)
    CHECK(failed);
}

TEST_CASE("PostgreSQL cancellation survives worker allocation failure after "
          "native blocking",
          "[db][allocation][integration]") {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto blocker = open_database();
  if (!blocker || PQstatus(blocker.get()) != CONNECTION_OK) {
    SKIP("PG unavailable");
  }
  auto observer = open_database();
  auto connection = open_database();
  REQUIRE(observer);
  REQUIRE(PQstatus(observer.get()) == CONNECTION_OK);
  REQUIRE(connection);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  const auto blocker_pid = std::to_string(PQbackendPID(blocker.get()));
  const auto worker_pid = std::to_string(PQbackendPID(connection.get()));
  const char* key = blocker_pid.c_str();
  {
    Result locked{
        PQexecParams(blocker.get(),
                     "SELECT pg_catalog.pg_advisory_lock(208, $1::integer)", 1,
                     nullptr, &key, nullptr, nullptr, 0),
        PQclear};
    REQUIRE(locked);
    REQUIRE(PQresultStatus(locked.get()) == PGRES_TUPLES_OK);
  }

  AllocationFailure failure;
  Completion completion;
  const auto started = Clock::now();
  std::jthread worker{[connection = std::move(connection), &failure,
                       &completion, lock_key = blocker_pid,
                       deadline = started + 20s](std::stop_token stop) mutable {
    run_query(std::move(connection), stop, deadline, lock_key, failure,
              completion);
  }};
  // On every assertion unwind this releases the rendezvous BEFORE jthread
  // requests stop/joins. Both PG owners and the shared state outlive that join.
  ReleaseWorker release_worker{completion};
  constexpr auto BLOCKED =
      "SELECT EXISTS(SELECT 1 FROM pg_catalog.pg_stat_activity a "
      "WHERE a.pid=$1::integer AND a.state='active' "
      "AND a.wait_event_type='Lock' AND a.wait_event='advisory' "
      "AND $2::integer=ANY(pg_catalog.pg_blocking_pids(a.pid)))";
  bool blocked = false;
  while (!blocked && Clock::now() < started + 3s) {
    blocked = scalar(observer.get(), BLOCKED, worker_pid.c_str(), key);
  }
  REQUIRE(blocked);
  REQUIRE(failure.rejected.load() == 0);
  {
    const std::lock_guard lock(completion.mutex);
    REQUIRE_FALSE(completion.returned);
  }
  // Publication happens before request_stop; denial remains armed throughout
  // native cleanup, the post-cleanup checkpoint, and physical PGconn closure.
  failure.armed.store(true, std::memory_order_release);
  const auto stopped_at = Clock::now();
  REQUIRE(worker.request_stop());
  const auto completion_deadline = stopped_at + 5s;
  Outcome outcome;
  {
    std::unique_lock lock(completion.mutex);
    REQUIRE(completion.changed.wait_until(lock, stopped_at + 3500ms,
                                          [&] { return completion.returned; }));
    outcome = completion.outcome;
    REQUIRE_FALSE(completion.closed);
  }
  REQUIRE_FALSE(outcome.escaped_exception);
  REQUIRE(outcome.returned);
  REQUIRE(outcome.fatal_result);
  REQUIRE(outcome.denial_at_return);
  REQUIRE(outcome.returned_at < stopped_at + 3500ms);
  CHECK(outcome.checkpoint_rejected);
  CHECK(outcome.rejected_after_checkpoint > outcome.rejected_at_return);
  // The original blocker must still own the exact lock: cancellation cannot
  // pass by unlocking the query or by closing its PGconn in this test first.
  REQUIRE(scalar(observer.get(),
                 "SELECT EXISTS(SELECT 1 FROM pg_catalog.pg_locks "
                 "WHERE pid=$1::integer AND locktype='advisory' "
                 "AND classid=208 AND objid=$1::oid AND objsubid=2 "
                 "AND granted)",
                 key));
  if (outcome.cancel_failed) {
    // A refused/unconfirmed cancellation must already have aborted the native
    // socket, while its PGconn is STILL owned by the waiting worker.
    REQUIRE(outcome.disconnected);
  } else {
    bool quiescent = false;
    while (!quiescent && Clock::now() < stopped_at + 4500ms) {
      quiescent =
          scalar(observer.get(),
                 "SELECT NOT EXISTS(SELECT 1 FROM pg_catalog.pg_stat_activity "
                 "WHERE pid=$1::integer AND state='active') "
                 "AND NOT EXISTS(SELECT 1 FROM pg_catalog.pg_locks "
                 "WHERE pid=$1::integer AND NOT granted)",
                 worker_pid.c_str());
    }
    REQUIRE(quiescent);
    CHECK_FALSE(scalar(observer.get(), BLOCKED, worker_pid.c_str(), key));
  }
  release_worker.release();
  {
    std::unique_lock lock(completion.mutex);
    REQUIRE(completion.changed.wait_until(lock, completion_deadline,
                                          [&] { return completion.closed; }));
    outcome = completion.outcome;
  }
  worker.join();
  CHECK_FALSE(worker.joinable());
  CHECK(outcome.denial_after_close);
  CHECK(outcome.closed_at < completion_deadline);
  // The independent server timeout is 12s and the operation deadline is 20s.
  // All cleanup evidence and the join must precede either fallback.
  CHECK(Clock::now() < started + 10s);
  blocker.reset();
  bool backend_gone = false;
  const auto gone_deadline = Clock::now() + 2s;
  while (!backend_gone && Clock::now() < gone_deadline) {
    backend_gone =
        scalar(observer.get(),
               "SELECT NOT EXISTS(SELECT 1 FROM pg_catalog.pg_stat_activity "
               "WHERE pid=$1::integer) AND NOT EXISTS(SELECT 1 "
               "FROM pg_catalog.pg_locks WHERE pid=$1::integer)",
               worker_pid.c_str());
  }
  CHECK(backend_gone);
}
