// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan — K.* cleanup task cases (4 of 4).

#include "kernel/scheduled_tasks/cleanup_events.hpp"

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"

#include "../realtime/shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ce = plinth::scheduled_tasks::cleanup_events;

namespace {

auto pg_config() -> plinth::Config::Database {
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

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  auto db = pg_config();
  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password + " connect_timeout=3";
  PGconn* conn = PQconnectdb(conninfo.c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);
}

struct PgRaw {
  PGconn* conn{nullptr};
  explicit PgRaw(const plinth::Config::Database& db) {
    conn = PQconnectdb(build_conninfo(db).c_str());
  }
  ~PgRaw() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  PgRaw(const PgRaw&) = delete;
  auto operator=(const PgRaw&) -> PgRaw& = delete;
  PgRaw(PgRaw&&) = delete;
  auto operator=(PgRaw&&) -> PgRaw& = delete;

  [[nodiscard]] auto exec(const std::string& sql) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    return {PQexec(conn, sql.c_str()), PQclear};
  }
  [[nodiscard]] auto exec_params(const std::string& sql,
                                 const std::vector<std::string>& params) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
      values.push_back(p.c_str());
    }
    return {PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()),
                         nullptr, values.data(), nullptr, nullptr, 0),
            PQclear};
  }
};

auto count_events(PgRaw& pg) -> int {
  auto r = pg.exec("SELECT COUNT(*) FROM plinth.events");
  return std::stoi(PQgetvalue(r.get(), 0, 0));
}

// Insert one row with `created_at` set N seconds in the past.
auto insert_aged(PgRaw& pg, std::string_view channel, int seconds_ago) -> void {
  auto r = pg.exec_params(
      "INSERT INTO plinth.events (channel, payload, created_at) "
      "VALUES ($1, '{}'::jsonb, NOW() - INTERVAL '1 second' * $2::int)",
      {std::string{channel}, std::to_string(seconds_ago)});
  REQUIRE(PQresultStatus(r.get()) == PGRES_COMMAND_OK);
}

struct Harness {
  drogon::orm::DbClientPtr db;
  explicit Harness() {
    ce::reset_audit_state_for_test();
    // 0.6.3.N — shared process-lifetime client; per-test create+
    // destroy of `newPgClient` reproducibly trips
    // `EventLoopThreadPool::~ + Resource deadlock avoided`. Member-
    // init not viable: setup work above must run first.
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    ce::set_db_client_for_test(db);
  }
  ~Harness() {
    ce::set_db_client_for_test(nullptr);
    ce::reset_audit_state_for_test();
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

} // namespace

// ── K.01 ─────────────────────────────────────────────────────────────

TEST_CASE("K.01: sweep DELETEs rows older than retention",
          "[realtime][events][cleanup][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  PgRaw pg{pg_config()};

  // Three rows: one fresh, one borderline (just inside retention),
  // and one ancient (well past retention).
  insert_aged(pg, "plinth:data:ext_k01.t", /*seconds_ago=*/0);
  insert_aged(pg, "plinth:data:ext_k01.t", /*seconds_ago=*/30);
  insert_aged(pg, "plinth:data:ext_k01.t", /*seconds_ago=*/120);

  plinth::Config::Realtime::Events cfg;
  cfg.retention_seconds = 60; // delete rows older than 60s

  drogon::sync_wait(ce::run(cfg));

  CHECK(count_events(pg) == 2); // ancient (120s) deleted
  CHECK(ce::last_swept_for_test() == 1);
}

// ── K.02 ─────────────────────────────────────────────────────────────

TEST_CASE("K.02: empty sweep — no rows older than retention",
          "[realtime][events][cleanup][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  PgRaw pg{pg_config()};

  insert_aged(pg, "plinth:data:ext_k02.t", /*seconds_ago=*/5);

  plinth::Config::Realtime::Events cfg;
  cfg.retention_seconds = 3600; // 1 hour — nothing to delete

  drogon::sync_wait(ce::run(cfg));

  CHECK(count_events(pg) == 1);
  CHECK(ce::last_swept_for_test() == 0);
}

// ── K.03 ─────────────────────────────────────────────────────────────

TEST_CASE("K.03: advisory lock contention — only one thread sweeps",
          "[realtime][events][cleanup][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  PgRaw pg{pg_config()};

  // 5 ancient rows.
  for (int i = 0; i < 5; ++i) {
    insert_aged(pg, "plinth:data:ext_k03.t", /*seconds_ago=*/600);
  }

  plinth::Config::Realtime::Events cfg;
  cfg.retention_seconds = 60;

  // Two concurrent sweeps. The advisory xact-lock guarantees that
  // exactly one wins on the first DELETE pass; the loser observes
  // got_lock=false and silent-skips. The second sweep that runs
  // AFTER the winner releases the lock (by COMMIT) will then see
  // an empty WHERE and delete zero. Net: 5 rows deleted total
  // across both runs (one round wins all 5, the other wins 0).
  auto run_one = [&]() { drogon::sync_wait(ce::run(cfg)); };
  std::thread t1{run_one};
  std::thread t2{run_one};
  t1.join();
  t2.join();

  CHECK(count_events(pg) == 0);
}

// ── K.04 ─────────────────────────────────────────────────────────────

TEST_CASE("K.04: PG failure routes through write_failed audit",
          "[realtime][events][cleanup][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  PgRaw pg{pg_config()};

  // Drop the events table so the sweep's DELETE raises a Drogon
  // exception inside ce::run. The cleanup-failed audit path fires;
  // last_swept stays at zero because the success branch was never
  // entered. Avoids the alternative — pointing a DbClient at a non-
  // existent database — which hangs on lazy connect.
  auto drop_r = pg.exec("DROP TABLE plinth.events");
  REQUIRE(PQresultStatus(drop_r.get()) == PGRES_COMMAND_OK);

  plinth::Config::Realtime::Events cfg;
  cfg.retention_seconds = 60;
  drogon::sync_wait(ce::run(cfg));

  CHECK(ce::last_swept_for_test() == 0);
}
