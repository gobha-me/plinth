// SPDX-License-Identifier: MIT

#include "kernel/js/owned_database_awaiters.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

// A suspended coroutine is only a resume oracle. Production completion state
// owns/revokes its handle; this helper supplies no replacement state machine.
class ResumeProbe {
 public:
  struct promise_type {
    auto get_return_object() -> ResumeProbe {
      return ResumeProbe{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    auto initial_suspend() noexcept -> std::suspend_always { return {}; }
    auto final_suspend() noexcept -> std::suspend_always { return {}; }
    auto return_void() noexcept -> void {}
    auto unhandled_exception() noexcept -> void { std::terminate(); }
  };

  explicit ResumeProbe(std::coroutine_handle<promise_type> handle_in)
      : handle(handle_in) {}
  ResumeProbe(const ResumeProbe&) = delete;
  auto operator=(const ResumeProbe&) -> ResumeProbe& = delete;
  ResumeProbe(ResumeProbe&& other) noexcept
      : handle(std::exchange(other.handle, {})) {}
  auto operator=(ResumeProbe&&) -> ResumeProbe& = delete;
  ~ResumeProbe() { destroy(); }
  auto get() const -> std::coroutine_handle<> { return handle; }
  auto destroy() -> void {
    if (handle) {
      handle.destroy();
      handle = {};
    }
  }

 private:
  std::coroutine_handle<promise_type> handle;
};

auto resume_probe(std::size_t& resumed) -> ResumeProbe {
  ++resumed;
  co_return;
}

// Implements the pinned driver's public injection interface. All SQL binding,
// completion revocation, native rollback retention and registry draining below
// call actual production components; only native callback timing is controlled.
class CallbackDatabase final : public drogon::orm::Transaction {
 public:
  CallbackDatabase() { type_ = drogon::orm::ClientType::PostgreSQL; }

  auto newTransaction(const std::function<void(bool)>&)
      -> std::shared_ptr<drogon::orm::Transaction> override {
    throw std::logic_error("unexpected synchronous transaction acquisition");
  }
  auto newTransactionAsync(
      const std::function<
          void(const std::shared_ptr<drogon::orm::Transaction>&)>& callback)
      -> void override {
    acquisition = callback;
    if (throw_after_acquisition_capture) {
      throw std::runtime_error("injected acquisition setup failure");
    }
  }
  auto hasAvailableConnections() const noexcept -> bool override {
    return true;
  }
  auto setTimeout(double) -> void override {}
  auto closeAll() -> void override { ++closes; }
  auto closeAllFor(std::chrono::milliseconds) -> bool override {
    closeAll();
    return true;
  }
  auto rollback() -> void override {
    ++rollbacks;
    if (throw_rollback) {
      throw std::runtime_error("injected rollback request failure");
    }
  }
  auto setCommitCallback(const std::function<void(bool)>& callback)
      -> void override {
    commit = callback;
  }

  std::function<void(const std::shared_ptr<drogon::orm::Transaction>&)>
      acquisition;
  drogon::orm::ResultCallback success;
  std::function<void(const std::exception_ptr&)> failure;
  std::function<void(bool)> commit;
  bool throw_after_acquisition_capture = false;
  bool throw_rollback = false;
  std::size_t dispatched = 0;
  std::size_t rollbacks = 0;
  std::size_t closes = 0;

 private:
  auto execSql(const char*, std::size_t, std::size_t,
               std::vector<const char*>&&, std::vector<int>&&,
               std::vector<int>&&, drogon::orm::ResultCallback&& result,
               std::function<void(const std::exception_ptr&)>&& error)
      -> void override {
    ++dispatched;
    success = std::move(result);
    failure = std::move(error);
  }
};

class RollbackCleanupOnUnwind {
 public:
  explicit RollbackCleanupOnUnwind(
      std::shared_ptr<CallbackDatabase> transaction_in)
      : transaction(std::move(transaction_in)) {}
  ~RollbackCleanupOnUnwind() {
    transaction->throw_rollback = false;
    auto& registry = plinth::lifecycle::async_tasks();
    // First retry requests rollback and removes the retained record. Clearing
    // the genuine fake-driver callback then releases its accepted task lease.
    static_cast<void>(registry.drain(2ms));
    transaction->commit = {};
    static_cast<void>(registry.drain(100ms));
  }

 private:
  std::shared_ptr<CallbackDatabase> transaction;
};

} // namespace

TEST_CASE("owned SQL callbacks cannot resume a destroyed revoked frame",
          "[js][async][owned-completion]") {
  plinth::lifecycle::AsyncTaskRegistry registry;
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto database = std::make_shared<CallbackDatabase>();
  std::size_t resumed = 0;
  auto probe = resume_probe(resumed);
  {
    plinth::js::OwnedSqlAwaiter<int> awaiter{database, accepted, "SELECT $1",
                                             1};
    awaiter.await_suspend(probe.get());
    REQUIRE(database->dispatched == 1);
    REQUIRE(database->success != nullptr);
    REQUIRE(database->failure != nullptr);
  }
  // The actual awaiter destructor revokes the callback handle before the
  // coroutine allocation goes away. Both callbacks remain genuinely retained.
  probe.destroy();
  accepted.reset();
  registry.close_admission();
  CHECK_FALSE(registry.drain(0ms));
  database->success(drogon::orm::Result{nullptr});
  database->failure(
      std::make_exception_ptr(std::runtime_error("injected late SQL error")));
  CHECK(resumed == 0);
  CHECK(registry.active_for_test() == 1);
  database->success = {};
  CHECK_FALSE(registry.drain(0ms));
  database->failure = {};
  CHECK(registry.drain(100ms));
  CHECK(database->closes == 0);
}

TEST_CASE("owned SQL completion consumes exactly one successful wake",
          "[js][async][owned-completion]") {
  plinth::lifecycle::AsyncTaskRegistry registry;
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto state = std::make_shared<plinth::js::OwnedSqlCompletion>(accepted);
  std::size_t resumed = 0;
  auto probe = resume_probe(resumed);
  state->arm(probe.get());
  state->succeed(drogon::orm::Result{nullptr});
  CHECK(resumed == 1);
  state->succeed(drogon::orm::Result{nullptr});
  state->fail(std::make_exception_ptr(std::runtime_error("duplicate error")));
  CHECK(resumed == 1);
  state->revoke();
  probe.destroy();
  accepted.reset();
  CHECK_FALSE(registry.drain(0ms));
  state.reset();
  CHECK(registry.drain(100ms));
}

TEST_CASE("partial SQL binder setup cannot auto-dispatch on unwinding",
          "[js][async][owned-completion]") {
  auto database = std::make_shared<CallbackDatabase>();
  const auto begin_partial_setup = [&] {
    auto binder = *database << "SELECT $1";
    plinth::js::SqlBinderSetupGuard guard{binder};
    binder << 1;
    binder >> [](const drogon::orm::Result&) {};
    // Native setup fails AFTER the success callback but BEFORE the exception
    // callback/explicit exec. This invokes the production guard and the actual
    // pinned SqlBinder destructor, not a replacement binder implementation.
    throw std::runtime_error("injected partial callback setup failure");
  };
  CHECK_THROWS_AS(begin_partial_setup(), std::runtime_error);
  CHECK(database->dispatched == 0);
  CHECK(database->closes == 0);
}

TEST_CASE(
    "retained transaction timeout callback survives failed acquisition setup",
    "[js][async][owned-completion]") {
  plinth::lifecycle::AsyncTaskRegistry registry;
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto rollback = std::make_shared<plinth::js::NativeRollbackOwner>(accepted);
  auto database = std::make_shared<CallbackDatabase>();
  database->throw_after_acquisition_capture = true;
  std::size_t resumed = 0;
  auto probe = resume_probe(resumed);
  {
    plinth::js::OwnedTransactionAwaiter awaiter{database, accepted, rollback,
                                                nullptr};
    CHECK_THROWS_AS(awaiter.await_suspend(probe.get()), std::runtime_error);
    REQUIRE(database->acquisition != nullptr);
  }
  probe.destroy();
  accepted.reset();
  rollback.reset();
  registry.close_admission();
  CHECK_FALSE(registry.drain(0ms));
  database->acquisition(nullptr);
  CHECK(resumed == 0);
  CHECK(registry.active_for_test() == 1);
  database->acquisition = {};
  CHECK(registry.drain(100ms));
  CHECK(database->closes == 0);
}

TEST_CASE("revoked acquisition rolls back a late transaction without resuming",
          "[js][async][owned-completion]") {
  plinth::lifecycle::AsyncTaskRegistry registry;
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto rollback = std::make_shared<plinth::js::NativeRollbackOwner>(accepted);
  auto database = std::make_shared<CallbackDatabase>();
  auto transaction = std::make_shared<CallbackDatabase>();
  database->throw_after_acquisition_capture = true;
  std::size_t resumed = 0;
  auto probe = resume_probe(resumed);
  {
    plinth::js::OwnedTransactionAwaiter awaiter{database, accepted, rollback,
                                                nullptr};
    CHECK_THROWS_AS(awaiter.await_suspend(probe.get()), std::runtime_error);
    REQUIRE(database->acquisition != nullptr);
  }
  probe.destroy();
  accepted.reset();
  rollback.reset();
  registry.close_admission();
  database->acquisition(transaction);
  CHECK(resumed == 0);
  CHECK(transaction->rollbacks == 1);
  CHECK(transaction->commit != nullptr);
  CHECK(transaction->closes == 0);
  database->acquisition = {};
  // A successful rollback REQUEST is not native completion: the actual
  // transaction callback still owns the native cleanup owner and its lease.
  CHECK_FALSE(registry.drain(0ms));
  transaction->commit = {};
  CHECK(registry.drain(100ms));
}

TEST_CASE("native rollback failure remains retained through retry and callback "
          "release",
          "[js][async][owned-completion][cleanup-retention]") {
  auto& registry = plinth::lifecycle::async_tasks();
  REQUIRE(registry.active_for_test() == 0);
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto owner = std::make_shared<plinth::js::NativeRollbackOwner>(accepted);
  auto transaction = std::make_shared<CallbackDatabase>();
  RollbackCleanupOnUnwind cleanup{transaction};
  transaction->throw_rollback = true;
  const bool requested = owner->request(transaction, nullptr);
  const bool owner_failed = owner->has_failure();
  std::weak_ptr<plinth::js::NativeRollbackOwner> weak = owner;
  owner.reset();
  accepted.reset();
  const bool first_drained = registry.drain(2ms);
  const bool retained = !weak.expired();
  const auto active_after_failure = registry.active_for_test();
  transaction->throw_rollback = false;
  const bool drain_before_callback_release = registry.drain(2ms);
  const auto rollback_requests = transaction->rollbacks;
  bool owner_present_after_retry = false;
  bool owner_failed_after_retry = true;
  if (const auto after_retry = weak.lock()) {
    owner_present_after_retry = true;
    owner_failed_after_retry = after_retry->has_failure();
  }
  transaction->commit = {};
  const bool final_drained = registry.drain(100ms);

  CHECK_FALSE(requested);
  CHECK(owner_failed);
  CHECK_FALSE(first_drained);
  CHECK(retained);
  CHECK(active_after_failure == 1);
  CHECK_FALSE(drain_before_callback_release);
  CHECK(rollback_requests == 3);
  CHECK(owner_present_after_retry);
  CHECK_FALSE(owner_failed_after_retry);
  CHECK(final_drained);
  CHECK(weak.expired());
  CHECK(transaction->closes == 0);
}
