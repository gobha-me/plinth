// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/js/native_rollback_owner.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"

#include <coroutine>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/coroutine.h>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace plinth::js {

// SQL callbacks own their completion state, not the suspended awaiter frame.
// A setup/launch exception revokes the handle before later driver callbacks
// can touch that frame. The accepted lease survives the callback resume stack.
class OwnedSqlCompletion {
 public:
  explicit OwnedSqlCompletion(
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted_in)
      : accepted(std::move(accepted_in)) {}

  auto arm(std::coroutine_handle<> handle) -> void {
    std::lock_guard lock(mutex);
    suspended = handle;
  }
  auto revoke() -> void {
    std::lock_guard lock(mutex);
    active = false;
    suspended = {};
  }
  auto succeed(const drogon::orm::Result& value) -> void {
    std::lock_guard lock(mutex);
    if (!active || !suspended) {
      return;
    }
    result = value;
    auto handle = std::exchange(suspended, {});
    handle.resume();
    // No frame access after resume; destruction recursively revokes state.
  }
  auto fail(const std::exception_ptr& value) -> void {
    std::lock_guard lock(mutex);
    if (!active || !suspended) {
      return;
    }
    error = value;
    auto handle = std::exchange(suspended, {});
    handle.resume();
  }
  auto take_result() -> drogon::orm::Result {
    std::lock_guard lock(mutex);
    if (error) {
      std::rethrow_exception(error);
    }
    return std::move(result.value());
  }

 private:
  // Declared first, destroyed last after callback state.
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  std::recursive_mutex mutex;
  std::coroutine_handle<> suspended{};
  std::optional<drogon::orm::Result> result;
  std::exception_ptr error;
  bool active = true;
};

// Declared after the binder so it cancels destructor auto-exec BEFORE binder
// unwinding. Explicit exec is permitted only after every parameter/callback
// has been installed. cancel never cancels already-dispatched native SQL.
class SqlBinderSetupGuard {
 public:
  explicit SqlBinderSetupGuard(
      drogon::orm::internal::SqlBinder& binder_in) noexcept
      : binder(binder_in) {}
  ~SqlBinderSetupGuard() { binder.cancel(); }

 private:
  drogon::orm::internal::SqlBinder& binder;
};

class OwnedTransactionCompletion {
 public:
  OwnedTransactionCompletion(
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted_in,
      std::shared_ptr<NativeRollbackOwner> rollback_owner_in,
      std::shared_ptr<ExtensionDatabaseClients> domain_in)
      : accepted(std::move(accepted_in)),
        rollback_owner(std::move(rollback_owner_in)),
        database_owner(std::move(domain_in)) {}

  auto arm(std::coroutine_handle<> handle) -> void {
    std::lock_guard lock(mutex);
    suspended = handle;
  }
  auto revoke() -> void {
    std::lock_guard lock(mutex);
    active = false;
    suspended = {};
    if (transaction) {
      rollback_owner->request(transaction, database_owner);
      transaction.reset();
    }
  }
  auto complete(const std::shared_ptr<drogon::orm::Transaction>& value)
      -> void {
    std::lock_guard lock(mutex);
    if (!active || !suspended) {
      if (value) {
        rollback_owner->request(value, database_owner);
      }
      return;
    }
    if (value) {
      transaction = value;
    } else {
      try {
        error = std::make_exception_ptr(drogon::orm::TimeoutError(
            "Timeout, no connection available for transaction"));
      } catch (...) {
        error = std::current_exception();
      }
    }
    auto handle = std::exchange(suspended, {});
    handle.resume();
  }
  auto take_result() -> std::shared_ptr<drogon::orm::Transaction> {
    std::lock_guard lock(mutex);
    if (error) {
      std::rethrow_exception(error);
    }
    return std::exchange(transaction, {});
  }

 private:
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  std::shared_ptr<NativeRollbackOwner> rollback_owner;
  std::shared_ptr<ExtensionDatabaseClients> database_owner;
  std::recursive_mutex mutex;
  std::coroutine_handle<> suspended{};
  std::shared_ptr<drogon::orm::Transaction> transaction;
  std::exception_ptr error;
  bool active = true;
};

// Independently owned acquisition completion also fences timeout callbacks
// retained when newTransactionAsync throws during driver setup. A late actual
// transaction is explicitly rollback-owned, never dropped into auto-COMMIT.
class OwnedTransactionAwaiter {
 public:
  OwnedTransactionAwaiter(
      std::shared_ptr<drogon::orm::DbClient> client_in,
      std::shared_ptr<lifecycle::AsyncTaskLease> accepted_in,
      std::shared_ptr<NativeRollbackOwner> rollback_owner_in,
      std::shared_ptr<ExtensionDatabaseClients> domain_in)
      : completion(std::make_shared<OwnedTransactionCompletion>(
            std::move(accepted_in), std::move(rollback_owner_in),
            std::move(domain_in))),
        client(std::move(client_in)) {}

  ~OwnedTransactionAwaiter() { completion->revoke(); }
  [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }
  auto await_resume() -> std::shared_ptr<drogon::orm::Transaction> {
    return completion->take_result();
  }

  auto await_suspend(std::coroutine_handle<> handle) -> void {
    auto state = completion;
    auto database = client;
    state->arm(handle);
    try {
      database->newTransactionAsync(
          [state](const auto& transaction) { state->complete(transaction); });
    } catch (...) {
      state->revoke();
      throw;
    }
  }

 private:
  std::shared_ptr<OwnedTransactionCompletion> completion;
  std::shared_ptr<drogon::orm::DbClient> client;
};

template <typename... Parameters> class OwnedSqlAwaiter {
 public:
  OwnedSqlAwaiter(std::shared_ptr<drogon::orm::DbClient> client_in,
                  std::shared_ptr<lifecycle::AsyncTaskLease> accepted_in,
                  std::string sql_in, Parameters... parameters_in)
      : completion(
            std::make_shared<OwnedSqlCompletion>(std::move(accepted_in))),
        client(std::move(client_in)), sql(std::move(sql_in)),
        parameters(std::move(parameters_in)...) {}

  ~OwnedSqlAwaiter() { completion->revoke(); }
  [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }
  auto await_resume() -> drogon::orm::Result {
    return completion->take_result();
  }

  auto await_suspend(std::coroutine_handle<> handle) -> void {
    auto state = completion;
    // Keep the referenced client alive across a synchronous completion which
    // can destroy this awaiter before exec() returns.
    auto database = client;
    state->arm(handle);
    try {
      auto binder = *database << sql;
      SqlBinderSetupGuard setup{binder};
      std::apply([&](const auto&... values) { (binder << ... << values); },
                 parameters);
      binder >> [state](const drogon::orm::Result& result) {
        state->succeed(result);
      };
      binder >>
          [state](const std::exception_ptr& error) { state->fail(error); };
      binder.exec();
    } catch (...) {
      state->revoke();
      throw;
    }
  }

 private:
  std::shared_ptr<OwnedSqlCompletion> completion;
  std::shared_ptr<drogon::orm::DbClient> client;
  std::string sql;
  std::tuple<Parameters...> parameters;
};

} // namespace plinth::js
