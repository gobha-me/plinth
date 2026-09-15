// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/config.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace plinth::js {

// Owned by a RuntimePool, then retained by its contexts and in-flight ops.
// The normal runtime lease drain therefore also drains these database clients
// before destruction. Credentials never enter the JS config projection.
class ExtensionDatabaseClients {
 public:
  explicit ExtensionDatabaseClients(const Config::Database& config);
  auto get(std::string extension_name)
      -> drogon::Task<std::shared_ptr<drogon::orm::DbClient>>;
  // Stop admission, drain Drogon's private database loops, and release the
  // durable client owners on the calling (lifecycle-owner) thread. Idempotent.
  [[nodiscard]] auto shutdown(std::chrono::milliseconds timeout) -> bool;

 private:
  Config::Database config;
  std::timed_mutex shutdown_mutex;
  std::mutex mutex;
  enum class State { running, stopping, stopped };
  State state = State::running;
  std::unordered_map<std::string, std::shared_ptr<drogon::orm::DbClient>>
      clients;
};

} // namespace plinth::js
