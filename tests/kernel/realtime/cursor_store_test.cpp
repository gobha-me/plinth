// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan — C.* cursor store cases (7 of 7).

#include "kernel/realtime/cursor_store.hpp"

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"

#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace cs = plinth::realtime::cursor_store;

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

struct TestPg {
  PGconn* conn = nullptr;
  explicit TestPg(const plinth::Config::Database& db) {
    conn = PQconnectdb(build_conninfo(db).c_str());
  }
  ~TestPg() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  TestPg(const TestPg&) = delete;
  auto operator=(const TestPg&) -> TestPg& = delete;
  TestPg(TestPg&&) = delete;
  auto operator=(TestPg&&) -> TestPg& = delete;

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

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);
}

// Generate a unique-per-test username so concurrent runs do not
// collide on the `plinth.users.username` UNIQUE.
auto unique_username(std::string_view prefix) -> std::string {
  static std::atomic<std::uint64_t> seq{0};
  return std::string{prefix} + "_" + std::to_string(seq.fetch_add(1)) + "_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count() &
             0xffff);
}

auto insert_user(TestPg& pg, std::string_view username) -> std::string {
  auto hash = plinth::auth::hash_password("test");
  auto res =
      pg.exec_params("INSERT INTO plinth.users (username, password_hash) "
                     "VALUES ($1, $2) RETURNING id",
                     {std::string{username}, hash});
  REQUIRE(res != nullptr);
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  return PQgetvalue(res.get(), 0, 0);
}

auto cursor_in_pg(TestPg& pg, const std::string& user_id) -> std::int64_t {
  auto res = pg.exec_params("SELECT last_seq FROM plinth.user_event_cursors "
                            "WHERE user_id = $1::uuid",
                            {user_id});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return -1; // no row
  }
  return std::stoll(PQgetvalue(res.get(), 0, 0));
}

// RAII harness — set DbClient + reset cache + register events config.
struct Harness {
  drogon::orm::DbClientPtr db;
  explicit Harness() {
    cs::clear_cache_for_test();
    cs::set_clock_for_test(nullptr);
    plinth::Config::Realtime::Events ecfg;
    cs::configure(ecfg);
    // 0.6.3.N — shared process-lifetime client; per-test create+
    // destroy of `newPgClient` reproducibly trips
    // `EventLoopThreadPool::~ + Resource deadlock avoided`. Member-
    // init not viable: setup work above must run first.
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    cs::set_db_client_for_test(db);
  }
  ~Harness() {
    cs::set_db_client_for_test(nullptr);
    cs::clear_cache_for_test();
    cs::set_clock_for_test(nullptr);
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

} // namespace

// ── C.01 ─────────────────────────────────────────────────────────────

TEST_CASE("C.01: first record_delivered creates cursor row",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c01"));

  // Default cfg has cursor_flush_threshold=50 → set to 1 so the first
  // record_delivered flushes synchronously without TTL juggling.
  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1;
  cs::configure(ecfg);

  drogon::sync_wait(cs::record_delivered(uid, 100));

  CHECK(cursor_in_pg(pg, uid) == 100);
  CHECK(drogon::sync_wait(cs::read_cursor(uid)) == 100);
}

// ── C.02 ─────────────────────────────────────────────────────────────

TEST_CASE("C.02: monotonic advance — out-of-order seq is no-op",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c02"));

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1; // flush every advance
  cs::configure(ecfg);

  drogon::sync_wait(cs::record_delivered(uid, 50));
  drogon::sync_wait(cs::record_delivered(uid, 100));
  drogon::sync_wait(cs::record_delivered(uid, 75)); // out-of-order

  CHECK(cursor_in_pg(pg, uid) == 100);
  CHECK(drogon::sync_wait(cs::read_cursor(uid)) == 100);
}

// ── C.03 ─────────────────────────────────────────────────────────────

TEST_CASE("C.03: cache TTL gates writes; one PG hit per window",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c03"));

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_cache_ttl_ms = 1000;
  ecfg.cursor_flush_threshold = 100; // out of reach for this test
  cs::configure(ecfg);

  auto t = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::time_point{});
  cs::set_clock_for_test([t]() { return *t; });

  // Three advances inside the TTL window: cache absorbs all of them,
  // PG sees none. (New entry's last_flush == now() at creation, so
  // the TTL window starts at the first call.)
  drogon::sync_wait(cs::record_delivered(uid, 100));
  *t += std::chrono::milliseconds(500);
  drogon::sync_wait(cs::record_delivered(uid, 200));
  *t += std::chrono::milliseconds(400);
  drogon::sync_wait(cs::record_delivered(uid, 300));

  CHECK(cursor_in_pg(pg, uid) == -1); // nothing persisted yet

  // Advance past TTL → next call flushes the latest seq.
  *t += std::chrono::milliseconds(200); // total 1.1 s since creation
  drogon::sync_wait(cs::record_delivered(uid, 400));

  CHECK(cursor_in_pg(pg, uid) == 400);
}

// ── C.04 ─────────────────────────────────────────────────────────────

TEST_CASE("C.04: flush threshold — Nth advance flushes",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c04"));

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_cache_ttl_ms = 60000; // out of reach
  ecfg.cursor_flush_threshold = 5;
  cs::configure(ecfg);

  // Freeze the clock so TTL never fires during the test.
  auto now = std::chrono::steady_clock::now();
  cs::set_clock_for_test([now]() { return now; });

  // First call: no entry exists yet → a fresh entry is created with
  // pending_delta=1; ttl_expired evaluates true (last_flush == now()
  // is < ttl, but we just created it) — actually with ttl=60s we're
  // not at TTL. Confirm by setting an even larger ttl below threshold:
  // we want ONLY the 5th delta to flush.
  drogon::sync_wait(cs::record_delivered(uid, 10));
  drogon::sync_wait(cs::record_delivered(uid, 20));
  drogon::sync_wait(cs::record_delivered(uid, 30));
  drogon::sync_wait(cs::record_delivered(uid, 40));

  // First call seeded a new entry with last_flush=now and
  // pending_delta=1 BEFORE the threshold check, so threshold_hit=
  // (1 >= 5) is false. None of the four calls flush.
  CHECK(cursor_in_pg(pg, uid) == -1);

  drogon::sync_wait(cs::record_delivered(uid, 50)); // pending=5 → flush

  CHECK(cursor_in_pg(pg, uid) == 50);
}

// ── C.05 ─────────────────────────────────────────────────────────────

TEST_CASE("C.05: ON DELETE CASCADE drops the cursor row with the user",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c05"));

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1;
  cs::configure(ecfg);

  drogon::sync_wait(cs::record_delivered(uid, 100));
  REQUIRE(cursor_in_pg(pg, uid) == 100);

  (void)pg.exec_params("DELETE FROM plinth.users WHERE id = $1::uuid", {uid});

  CHECK(cursor_in_pg(pg, uid) == -1); // CASCADE removed the row
}

// ── C.06 ─────────────────────────────────────────────────────────────

TEST_CASE("C.06: reset_cursor lowers the persisted seq",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c06"));

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1;
  cs::configure(ecfg);

  drogon::sync_wait(cs::record_delivered(uid, 100));
  REQUIRE(cursor_in_pg(pg, uid) == 100);

  drogon::sync_wait(cs::reset_cursor(uid, 50));
  CHECK(cursor_in_pg(pg, uid) == 50);
  CHECK(drogon::sync_wait(cs::read_cursor(uid)) == 50);
}

// ── C.07 ─────────────────────────────────────────────────────────────

TEST_CASE("C.07: concurrent record_delivered — final seq is the max",
          "[realtime][events][cursor][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};
  auto uid = insert_user(pg, unique_username("c07"));

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1;
  cs::configure(ecfg);

  constexpr std::size_t THREAD_COUNT = 8;
  constexpr std::size_t PER_THREAD = 50;

  std::vector<std::thread> workers;
  workers.reserve(THREAD_COUNT);
  for (std::size_t t = 0; t < THREAD_COUNT; ++t) {
    workers.emplace_back([uid, t]() {
      for (std::size_t i = 0; i < PER_THREAD; ++i) {
        auto seq = static_cast<std::int64_t>((t * PER_THREAD) + i + 1);
        drogon::sync_wait(cs::record_delivered(uid, seq));
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }

  constexpr auto EXPECTED_MAX =
      static_cast<std::int64_t>(THREAD_COUNT * PER_THREAD);
  CHECK(cursor_in_pg(pg, uid) == EXPECTED_MAX);
}
