// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/js/extension_database.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"

#include <drogon/orm/DbClient.h>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

namespace plinth::js {

class ExtensionDatabaseClients;

// Allocated before transaction acquisition. Healthy ownership contains no tx
// or DB domain, avoiding a callback/domain cycle. A failed request becomes an
// explicit registry-owned cleanup job; retry clears the failed tx pin, while
// its callback still retains this owner through actual native cleanup.
class NativeRollbackOwner
    : public lifecycle::AsyncCleanupOwner,
      public std::enable_shared_from_this<NativeRollbackOwner> {
 public:
  explicit NativeRollbackOwner(std::shared_ptr<lifecycle::AsyncTaskLease> lease)
      : accepted(std::move(lease)) {}

  auto attach(const std::shared_ptr<drogon::orm::Transaction>& transaction)
      -> void {
    std::lock_guard lock(mutex);
    transaction->setCommitCallback(
        [owner = shared_from_this()](bool) { (void)owner; });
    attached = true;
  }

  auto request(const std::shared_ptr<drogon::orm::Transaction>& transaction,
               const std::shared_ptr<ExtensionDatabaseClients>& domain)
      -> bool {
    if (!transaction) {
      return true;
    }
    std::lock_guard lock(mutex);
    try {
      if (!attached) {
        attach(transaction);
      }
      if (domain) {
        if (domain->request_owned_rollback(transaction)) {
          return true;
        }
      } else {
        transaction->rollback();
        return true;
      }
    } catch (...) {
      failure = std::current_exception();
    }
    failed_transaction = transaction;
    database_owner = domain;
    lifecycle::async_tasks().retain_cleanup(shared_from_this());
    plinth::log::error(
        "native rollback request incomplete; cleanup owner retained");
    return false;
  }

  auto retry_cleanup(std::chrono::steady_clock::time_point deadline) noexcept
      -> bool override {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::lock_guard lock(mutex);
    if (!failed_transaction) {
      return true;
    }
    try {
      if (!attached) {
        attach(failed_transaction);
      }
      if (database_owner) {
        if (!database_owner->request_owned_rollback(failed_transaction)) {
          return false;
        }
      } else {
        failed_transaction->rollback();
      }
      // The queued native rollback holds TransactionImpl until completion;
      // its attached callback keeps this owner and accepted lease alive.
      failed_transaction.reset();
      failure = nullptr;
      return true;
    } catch (...) {
      failure = std::current_exception();
      return false;
    }
  }

  [[nodiscard]] auto has_failure() const -> bool {
    std::lock_guard lock(mutex);
    return static_cast<bool>(failed_transaction);
  }

 private:
  // Declared first: process admission is released after dependency owners.
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  std::shared_ptr<ExtensionDatabaseClients> database_owner;
  std::shared_ptr<drogon::orm::Transaction> failed_transaction;
  std::exception_ptr failure;
  bool attached = false;
  mutable std::recursive_mutex mutex;
};

} // namespace plinth::js
