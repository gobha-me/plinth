// SPDX-License-Identifier: MIT
//
// See async_bridge_fixture.hpp.

#include "async_bridge_fixture.hpp"

#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/lifecycle/shutdown_coordinator.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/realtime/listener.hpp"

#include "../test_process.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <drogon/drogon.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace plinth::async_bridge_test {

namespace {

auto env(const char* name) -> const char* {
  return std::getenv(name);
}

} // namespace

auto pg_available() -> bool {
  if (env("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  auto cfg = test_config();
  auto conninfo = "host=" + cfg.db.host +
                  " port=" + std::to_string(cfg.db.port) +
                  " dbname=" + cfg.db.database + " user=" + cfg.db.user +
                  " password=" + cfg.db.password + " connect_timeout=3";
  PGconn* conn = PQconnectdb(conninfo.c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

auto test_config() -> plinth::Config {
  plinth::Config cfg;
  cfg.dev_mode = true;
  cfg.node_id = "test-node";
  cfg.migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  // Inherit production default pool_size=32. 0.4.4.1 dropped an
  // earlier 80-connection override that had been sized for D.17's
  // 10 × 5 fan-out; with subprocess-per-test × PG max_connections=100,
  // leaked 80-connection pools cascade into "sorry, too many clients"
  // failures on the next subprocess (project_ws_flaky_segfault.md
  // §Fourth occurrence). D.17 now queues ~18 of its 50 pg_sleep(0.05)
  // queries behind the 32 free slots — each slot runs two sleeps back
  // to back (~100 ms), still well under the test's implicit tolerance.

  if (const auto* v = env("PLINTH_PG_HOST")) {
    cfg.db.host = v;
  }
  if (const auto* v = env("PLINTH_PG_PORT")) {
    cfg.db.port = static_cast<uint16_t>(std::stoi(v));
  }
  if (const auto* v = env("PLINTH_PG_USER")) {
    cfg.db.user = v;
  }
  if (const auto* v = env("PLINTH_PG_PASSWORD")) {
    cfg.db.password = v;
  }
  if (const auto* v = env("PLINTH_PG_DATABASE")) {
    cfg.db.database = v;
  }
  return cfg;
}

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, /*dev_mode=*/true);
  plinth::groups::bootstrap_groups(db);
}

namespace {

// Drogon cannot be started twice in the same process. Mirrors
// ws_test_fixture's call_once + background thread pattern. We do NOT
// add HTTP listeners — the async bridge only needs the event loop
// (+ optional DbClient).
std::once_flag g_once;
std::thread g_thread;
std::atomic<bool> g_ready{false};

auto start_drogon(bool with_db) -> void {
  auto cfg = test_config();
  plinth::log::init(cfg);
  plinth::log::set_node_id(cfg.node_id);
  // DbClient is optional — 0.3.3.1 parallel dispatch still needs a
  // running Drogon loop to service queueInLoop callbacks from
  // detached AsyncOps, even for tests whose AsyncOps reject before
  // touching PG (e.g. audit.not_ready). Only create the client when
  // the caller explicitly asked for it (cap.* / pure-JS tests opt
  // out via `ensure_drogon_running()` to avoid DbClient's own
  // heartbeat ticks racing weak_ptr teardown — see
  // project_ws_flaky_segfault.md fifth-occurrence notes). PG-gated
  // tests still guard with pg_available() before driving the
  // client.
  if (with_db && pg_available()) {
    drogon::app().createDbClient("postgresql", cfg.db.host, cfg.db.port,
                                 cfg.db.database, cfg.db.user, cfg.db.password,
                                 cfg.db.pool_size);
  }
  auto shutdown = std::make_shared<plinth::lifecycle::ShutdownCoordinator>();
  plinth::test_process::register_shutdown([shutdown] {
    if (!g_ready.load()) {
      return;
    }
    auto result = shutdown->quiesce();
    if (!result.clean) {
      throw std::runtime_error("async bridge shutdown stopped at " +
                               result.failed_step);
    }
    if (g_thread.joinable()) {
      g_thread.join();
    }
    shutdown->finish_after_drogon();
  });
  if (!plinth::packages::rbac_test::start_async_workers()) {
    throw std::runtime_error("RBAC worker registry could not start");
  }
  shutdown->install_ingress_gate();
  drogon::app()
      .setLogPath("")
      .setLogLevel(trantor::Logger::kWarn)
      .disableSigtermHandling();

  drogon::app().getLoop()->queueInLoop([]() { g_ready.store(true); });
  g_thread = std::thread([]() { drogon::app().run(); });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!g_ready.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!g_ready.load()) {
    throw std::runtime_error("Drogon failed to start in async-bridge fixture");
  }
}

} // namespace

auto ensure_drogon_running() -> void {
  std::call_once(g_once, []() { start_drogon(/*with_db=*/false); });
}

auto ensure_drogon_with_db_running() -> void {
  std::call_once(g_once, []() { start_drogon(/*with_db=*/true); });
}

} // namespace plinth::async_bridge_test
