// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/config.hpp"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

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

 private:
  Config::Database config;
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<drogon::orm::DbClient>>
      clients;
};

} // namespace plinth::js
