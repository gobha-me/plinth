// SPDX-License-Identifier: MIT
// 0.6.3.N — see shared_pg_client.hpp for rationale.

#include "shared_pg_client.hpp"

#include "kernel/config.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace plinth::realtime_test {

namespace {

std::mutex clients_mutex;
std::map<int, drogon::orm::DbClientPtr> clients_by_conn_num;
std::vector<drogon::orm::DbClientPtr> retained_clients;

auto pg_config_from_env() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* v = std::getenv("PLINTH_PG_HOST")) {
    db.host = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<uint16_t>(std::stoi(v));
  }
  if (auto* v = std::getenv("PLINTH_PG_USER")) {
    db.user = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = v;
  }
  return db;
}

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

} // namespace

// Per-pool connection count. Pre-0.6.3.N each test created its own
// `newPgClient(..., 1 or 2)` and never collided with other tests.
// Sharing the client across tests means connection contention now
// matters — multiple subsystems on the same pool can serialize
// on the underlying PG connections (observed in
// `live_replay_ordering_test::L.08` where the events_writer's
// drain coroutine and `cursor_store::record_delivered` shared a
// 1-connection pool and deadlocked on `apply_drain_for_test`).
// 8 mirrors the production `pool_size` default (kernel/config.hpp
// `Database::pool_size = 8`).
constexpr int POOL_SIZE = 8;

auto shared_pg_client(int connNum) -> drogon::orm::DbClientPtr& {
  // std::map (node-based) keeps reference stability across inserts
  // — the returned reference stays valid for the life of the static
  // map, i.e. until process exit.
  // function-local statics, see header
  std::scoped_lock lock{clients_mutex};
  auto& slot = clients_by_conn_num[connNum];
  if (!slot) {
    // The `connNum` argument keys the pool slot but the pool size
    // is always POOL_SIZE — see comment above.
    slot = drogon::orm::DbClient::newPgClient(
        build_conninfo(pg_config_from_env()), POOL_SIZE);
  }
  return slot;
}

auto shutdown_shared_pg_clients(std::chrono::milliseconds timeout) -> bool {
  const bool unbounded = timeout == std::chrono::milliseconds::max();
  const auto deadline = unbounded ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() + timeout;
  std::vector<drogon::orm::DbClientPtr> local;
  {
    std::scoped_lock lock{clients_mutex};
    local.reserve(clients_by_conn_num.size() + retained_clients.size());
    for (auto& [conn_num, client] : clients_by_conn_num) {
      (void)conn_num;
      local.push_back(std::move(client));
    }
    clients_by_conn_num.clear();
    local.insert(local.end(), std::make_move_iterator(retained_clients.begin()),
                 std::make_move_iterator(retained_clients.end()));
    retained_clients.clear();
  }
  for (const auto& client : local) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        unbounded ? std::chrono::milliseconds::max()
                  : std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now);
    if ((!unbounded && now >= deadline) || !client->closeAllFor(remaining)) {
      std::scoped_lock lock{clients_mutex};
      retained_clients.insert(retained_clients.end(),
                              std::make_move_iterator(local.begin()),
                              std::make_move_iterator(local.end()));
      return false;
    }
  }
  return true;
}

} // namespace plinth::realtime_test
