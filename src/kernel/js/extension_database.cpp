// SPDX-License-Identifier: MIT
#include "kernel/js/extension_database.hpp"

#include "kernel/db/connection_info.hpp"
#include "kernel/db/extension_identity.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/owned_database_awaiters.hpp"

#include <drogon/drogon.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace plinth::js {

ExtensionDatabaseClients::ExtensionDatabaseClients(const Config::Database& cfg)
    : config(cfg) {
  config.password.clear();
}

auto ExtensionDatabaseClients::get(
    std::string extension_name,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> drogon::Task<std::shared_ptr<drogon::orm::DbClient>> {
  if (!db::is_valid_extension_name(extension_name) ||
      extension_name.size() > 63) {
    throw std::runtime_error("invalid extension database identity");
  }
  {
    std::lock_guard lock(mutex);
    if (state != State::running) {
      throw std::runtime_error("extension database clients are stopping");
    }
    if (auto found = clients.find(extension_name); found != clients.end()) {
      co_return found->second;
    }
  }
  auto kernel = drogon::app().getDbClient();
  if (!kernel) {
    throw std::runtime_error("no kernel database client configured");
  }
  auto credentials = co_await OwnedSqlAwaiter<std::string>{
      kernel, accepted,
      "SELECT role_name, password FROM plinth.extension_database_credentials "
      "WHERE extension_name = $1",
      extension_name};
  if (credentials.empty()) {
    // Trusted host-created contexts may precede package installation. Use the
    // same fail-closed provisioning as installation, never a kernel fallback.
    co_await OwnedSqlAwaiter<std::string>{
        kernel, accepted, "SELECT plinth.provision_extension_database($1)",
        extension_name};
    credentials = co_await OwnedSqlAwaiter<std::string>{
        kernel, accepted,
        "SELECT role_name, password FROM plinth.extension_database_credentials "
        "WHERE extension_name = $1",
        extension_name};
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
  if (state != State::running) {
    throw std::runtime_error("extension database clients are stopping");
  }
  auto [entry, inserted] = clients.try_emplace(extension_name);
  if (inserted) {
    entry->second = drogon::orm::DbClient::newPgClient(
        plinth::db::connection_info(isolated_config),
        static_cast<std::size_t>(std::clamp(config.pool_size, 1, 4)));
    entry->second->setTimeout(5.0);
  }
  co_return entry->second;
}

auto ExtensionDatabaseClients::request_owned_rollback(
    const std::shared_ptr<drogon::orm::Transaction>& transaction) -> bool {
  std::unique_lock shutdown_lock(shutdown_mutex, std::try_to_lock);
  if (!shutdown_lock.owns_lock()) {
    return false;
  }
  {
    std::lock_guard lock(mutex);
    if (state == State::stopped) {
      return true; // Actual pool-owner closeAllFor already verified cleanup.
    }
    if (state != State::running) {
      return false;
    }
  }
  // Do not hold the map mutex during driver callbacks. try_lock above also
  // avoids blocking recursive native callbacks behind the shutdown owner.
  transaction->rollback();
  return true;
}

auto ExtensionDatabaseClients::shutdown(std::chrono::milliseconds timeout)
    -> bool {
  const bool unbounded = timeout == std::chrono::milliseconds::max();
  const auto deadline = unbounded ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() + timeout;
  std::unique_lock shutdown_lock(shutdown_mutex, std::defer_lock);
  if (unbounded) {
    shutdown_lock.lock();
  } else if (!shutdown_lock.try_lock_for(deadline -
                                         std::chrono::steady_clock::now())) {
    return false;
  }

  decltype(clients) local;
  {
    std::lock_guard lock(mutex);
    if (state == State::stopped) {
      return true;
    }
    state = State::stopping;
    local.swap(clients);
  }
  bool drained = true;
  for (const auto& [name, client] : local) {
    (void)name;
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        unbounded ? std::chrono::milliseconds::max()
                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now);
    if ((!unbounded && now >= deadline) || !client->closeAllFor(remaining)) {
      drained = false;
      break;
    }
  }
  if (!drained) {
    {
      std::lock_guard lock(mutex);
      clients.merge(local);
    }
    // closeAllFor has sealed driver admission. A rollback request can no
    // longer succeed until the actual close is verified. Preserve this
    // lifecycle owner in the process drain too, avoiding the dependency cycle
    // where native drain waits for node 6's failed-pool retry.
    if (auto durable_owner = weak_from_this().lock()) {
      lifecycle::async_tasks().retain_cleanup(std::move(durable_owner));
    }
    // Existing stack-owned test/native domains cannot enter the shared
    // registry. Their caller retains this domain and explicitly retries the
    // same shutdown(false) contract; do not throw bad_weak_ptr on failure.
    return false;
  }
  // Keep every client alive through every closeAll() call. The patched Drogon
  // implementation drains callbacks which may have transient shared owners;
  // only this lifecycle-owner thread may release the durable owners.
  local.clear();
  {
    std::lock_guard lock(mutex);
    state = State::stopped;
  }
  return true;
}

auto ExtensionDatabaseClients::retry_cleanup(
    std::chrono::steady_clock::time_point deadline) noexcept -> bool {
  {
    std::lock_guard lock(mutex);
    if (state == State::stopped) {
      return true;
    }
    if (state != State::stopping) {
      return false;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return false;
  }
  try {
    const auto remaining =
        deadline == std::chrono::steady_clock::time_point::max()
            ? std::chrono::milliseconds::max()
            : std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                    now);
    return shutdown(remaining);
  } catch (...) {
    // The durable client map and registered domain owner remain available to
    // a later explicit drain. No resource release or cleanup success is
    // claimed.
    return false;
  }
}

} // namespace plinth::js
