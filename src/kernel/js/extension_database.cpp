// SPDX-License-Identifier: MIT
#include "kernel/js/extension_database.hpp"

#include "kernel/db/connection_info.hpp"
#include "kernel/db/extension_identity.hpp"
#include "kernel/js/db_search_path.hpp"

#include <drogon/drogon.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace plinth::js {

ExtensionDatabaseClients::ExtensionDatabaseClients(const Config::Database& cfg)
    : config(cfg) {
  config.password.clear();
}

auto ExtensionDatabaseClients::get(std::string extension_name)
    -> drogon::Task<std::shared_ptr<drogon::orm::DbClient>> {
  if (!db::is_valid_extension_name(extension_name) ||
      extension_name.size() > 63) {
    throw std::runtime_error("invalid extension database identity");
  }
  {
    std::lock_guard lock(mutex);
    if (auto found = clients.find(extension_name); found != clients.end()) {
      co_return found->second;
    }
  }
  auto kernel = drogon::app().getDbClient();
  if (!kernel) {
    throw std::runtime_error("no kernel database client configured");
  }
  auto credentials = co_await kernel->execSqlCoro(
      "SELECT role_name, password FROM plinth.extension_database_credentials "
      "WHERE extension_name = $1",
      extension_name);
  if (credentials.empty()) {
    // Trusted host-created contexts may precede package installation. Use the
    // same fail-closed provisioning as installation, never a kernel fallback.
    co_await kernel->execSqlCoro(
        "SELECT plinth.provision_extension_database($1)", extension_name);
    credentials = co_await kernel->execSqlCoro(
        "SELECT role_name, password FROM plinth.extension_database_credentials "
        "WHERE extension_name = $1",
        extension_name);
  }
  if (credentials.size() != 1) {
    throw std::runtime_error("extension database identity is unavailable");
  }
  auto isolated_config = config;
  isolated_config.user = credentials[0]["role_name"].as<std::string>();
  isolated_config.password = credentials[0]["password"].as<std::string>();
  if (isolated_config.user !=
          plinth::db::extension_role_name(config.database, extension_name) ||
      isolated_config.user == config.user || isolated_config.password.empty()) {
    throw std::runtime_error("extension database identity is invalid");
  }
  std::lock_guard lock(mutex);
  auto [entry, inserted] = clients.try_emplace(extension_name);
  if (inserted) {
    entry->second = drogon::orm::DbClient::newPgClient(
        plinth::db::connection_info(isolated_config),
        static_cast<std::size_t>(std::clamp(config.pool_size, 1, 4)));
    entry->second->setTimeout(5.0);
  }
  co_return entry->second;
}

} // namespace plinth::js
