// SPDX-License-Identifier: MIT
// 0.6.3.N — see shared_pg_client.hpp for rationale.

#include "shared_pg_client.hpp"

#include "kernel/config.hpp"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace plinth::realtime_test {

namespace {

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
  static std::mutex mu;
  static std::map<int, drogon::orm::DbClientPtr> by_conn_num;
  std::scoped_lock lock{mu};
  auto& slot = by_conn_num[connNum];
  if (!slot) {
    // The `connNum` argument keys the pool slot but the pool size
    // is always POOL_SIZE — see comment above.
    slot = drogon::orm::DbClient::newPgClient(
        build_conninfo(pg_config_from_env()), POOL_SIZE);
  }
  return slot;
}

} // namespace plinth::realtime_test
