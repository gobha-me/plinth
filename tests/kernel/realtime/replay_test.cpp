// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan — Y.* replay engine cases (9 of 9).

#include "kernel/realtime/replay.hpp"

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/rbac/subscribe_rule.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/ws/conn_state.hpp"

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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rp = plinth::realtime::replay;

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

// Direct libpq seed of plinth.events — bypasses the writer so tests
// can populate large datasets fast (Y.04 wants 15000 rows).
struct PgSeeder {
  PGconn* conn{nullptr};
  explicit PgSeeder(const plinth::Config::Database& db) {
    conn = PQconnectdb(build_conninfo(db).c_str());
  }
  ~PgSeeder() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  PgSeeder(const PgSeeder&) = delete;
  auto operator=(const PgSeeder&) -> PgSeeder& = delete;
  PgSeeder(PgSeeder&&) = delete;
  auto operator=(PgSeeder&&) -> PgSeeder& = delete;

  auto seed(std::string_view channel, std::size_t count) const -> void {
    const std::string CHANNEL_STR{channel};
    for (std::size_t i = 0; i < count; ++i) {
      std::string sql =
          "INSERT INTO plinth.events (channel, payload) VALUES ($1, $2::jsonb)";
      std::string payload = R"({"layer":"data","channel":")" + CHANNEL_STR +
                            R"(","i":)" + std::to_string(i) + "}";
      const std::array<const char*, 2> VALS = {CHANNEL_STR.c_str(),
                                               payload.c_str()};
      const std::array<int, 2> LENS = {static_cast<int>(CHANNEL_STR.size()),
                                       static_cast<int>(payload.size())};
      const std::array<int, 2> FMTS = {0, 0};
      std::unique_ptr<PGresult, decltype(&PQclear)> r{
          PQexecParams(conn, sql.c_str(), 2, nullptr, VALS.data(), LENS.data(),
                       FMTS.data(), 0),
          PQclear};
      REQUIRE(PQresultStatus(r.get()) == PGRES_COMMAND_OK);
    }
  }

  auto exec(std::string_view sql) const -> void {
    std::unique_ptr<PGresult, decltype(&PQclear)> r{
        PQexec(conn, std::string{sql}.c_str()), PQclear};
    REQUIRE(PQresultStatus(r.get()) == PGRES_COMMAND_OK);
  }
};

struct CapturedFrames {
  std::mutex mu;
  std::vector<std::string> frames;
  auto sink() {
    return [this](std::string s) {
      std::lock_guard lock(mu);
      frames.push_back(std::move(s));
    };
  }
  auto size() {
    std::lock_guard lock(mu);
    return frames.size();
  }
  auto count_type(std::string_view t) {
    std::lock_guard lock(mu);
    std::size_t n = 0;
    for (const auto& f : frames) {
      if (f.find(std::string{R"("type":")"} + std::string{t} + '"') !=
          std::string::npos) {
        n += 1;
      }
    }
    return n;
  }
  auto last_frame() -> std::string {
    std::lock_guard lock(mu);
    return frames.empty() ? std::string{} : frames.back();
  }
};

// Build an admin-equivalent conn state — replay engine treats
// `is_admin == true` as "every channel allowed".
auto admin_state() -> plinth::ws::ConnState {
  plinth::ws::ConnState s;
  s.authenticated = true;
  s.is_admin = true;
  s.auth.user_id = "00000000-0000-0000-0000-000000000001";
  return s;
}

struct Harness {
  drogon::orm::DbClientPtr db;
  PgSeeder seeder;
  explicit Harness() : seeder{pg_config()} {
    plinth::Config::Realtime::Broker bcfg;
    bcfg.enabled = true;
    bcfg.rbac_enforce = true;
    plinth::realtime::broker::start(bcfg);
    rp::reset_audit_state_for_test();
    // 0.6.3.N — shared process-lifetime client; per-test create+
    // destroy of `newPgClient` reproducibly trips
    // `EventLoopThreadPool::~ + Resource deadlock avoided`. Member-
    // init not viable: setup work above must run first.
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    rp::set_db_client_for_test(db);
  }
  ~Harness() {
    rp::set_db_client_for_test(nullptr);
    plinth::realtime::broker::stop();
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

} // namespace

// ── Y.01 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.01: empty replay — replay_done with row_count=0",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  auto state = admin_state();

  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(state, cf.sink(), /*since_seq=*/0,
                                            {"plinth:data:ext_y01.t"}, ecfg));

  CHECK(r.emitted == 0);
  CHECK_FALSE(r.resync.has_value());
  CHECK(cf.count_type("replay_done") == 1);
  CHECK(cf.count_type("replay") == 0); // no replay frames for an empty result
}

// ── Y.02 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.02: single-chunk replay — 250 rows, one chunk, replay_done",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y02.t", 250);

  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_rows_per_chunk = 500;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(admin_state(), cf.sink(),
                                            /*since_seq=*/0,
                                            {"plinth:data:ext_y02.t"}, ecfg));

  CHECK(r.emitted == 250);
  CHECK_FALSE(r.truncated);
  CHECK(cf.count_type("replay") == 250);
  CHECK(cf.count_type("replay_done") == 1);
}

// ── Y.03 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.03: multi-chunk replay — 1500 rows, three chunks",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y03.t", 1500);

  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_rows_per_chunk = 500;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(admin_state(), cf.sink(),
                                            /*since_seq=*/0,
                                            {"plinth:data:ext_y03.t"}, ecfg));

  CHECK(r.emitted == 1500);
  CHECK(cf.count_type("replay") == 1500);
  CHECK(cf.count_type("replay_done") == 1);
}

// ── Y.04 ─────────────────────────────────────────────────────────────

TEST_CASE(
    "Y.04: row-cap truncation — 15000 rows, replay_truncated + resync row_cap",
    "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y04.t", 15000);

  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_rows_per_chunk = 500;
  ecfg.replay_max_total_rows = 10000;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(admin_state(), cf.sink(),
                                            /*since_seq=*/0,
                                            {"plinth:data:ext_y04.t"}, ecfg));

  CHECK(r.truncated);
  REQUIRE(r.resync.has_value());
  CHECK(*r.resync == rp::ResyncReason::ROW_CAP);
  CHECK(cf.count_type("resync") == 1);
  CHECK(cf.last_frame().find("row_cap") != std::string::npos);
}

// ── Y.05 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.05: per-channel filter — only granted channel rows replay",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y05.a", 100);
  h.seeder.seed("plinth:data:ext_y05.b", 100);

  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(admin_state(), cf.sink(),
                                            /*since_seq=*/0,
                                            {"plinth:data:ext_y05.a"}, ecfg));

  CHECK(r.emitted == 100);
  CHECK(cf.count_type("replay") == 100);
}

// ── Y.06 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.06: RBAC denial during replay — denied rows skip the wire",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  // Two distinct extensions — RBAC rule is per-extension, not per-
  // table, so two tables under the same ext_X share one rule (cf.
  // ICD §Subscription RBAC layer mapping).
  h.seeder.seed("plinth:data:ext_y06a.t", 50);
  h.seeder.seed("plinth:data:ext_y06b.t", 50);

  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;

  // Non-admin state with effective_rules covering only ext_y06a.
  // The y06b.realtime.subscribe rule is missing so RBAC re-check
  // denies all 50 ext_y06b rows.
  plinth::ws::ConnState s;
  s.authenticated = true;
  s.is_admin = false;
  s.auth.user_id = "00000000-0000-0000-0000-000000000002";
  s.effective_rules.insert(
      plinth::rbac::derive_subscribe_rule("plinth:data:ext_y06a.t"));

  auto r = drogon::sync_wait(rp::run_replay(
      s, cf.sink(), /*since_seq=*/0,
      {"plinth:data:ext_y06a.t", "plinth:data:ext_y06b.t"}, ecfg));

  CHECK(r.emitted == 50);
  CHECK(cf.count_type("replay") == 50);
}

// ── Y.07 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.07: server-cursor mismatch — since_seq far ahead emits resync",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y07.t", 200);

  plinth::Config::Realtime::Events ecfg;
  ecfg.replay_max_total_rows = 10000;
  CapturedFrames cf;

  // Client claims a since_seq 100k beyond the largest persisted row.
  auto r = drogon::sync_wait(rp::run_replay(admin_state(), cf.sink(),
                                            /*since_seq=*/100000,
                                            {"plinth:data:ext_y07.t"}, ecfg));

  REQUIRE(r.resync.has_value());
  CHECK(*r.resync == rp::ResyncReason::MISMATCH);
  CHECK(cf.count_type("resync") == 1);
  CHECK(cf.last_frame().find("mismatch") != std::string::npos);
}

// ── Y.08 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.08: concurrent replays — five users each see their full slice",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y08.t", 100);

  plinth::Config::Realtime::Events ecfg;

  constexpr int N = 5;
  std::vector<std::thread> workers;
  std::vector<CapturedFrames> captures(N);
  std::vector<rp::ReplayResult> results(N);
  workers.reserve(N);
  for (int i = 0; i < N; ++i) {
    workers.emplace_back([&, i]() {
      results[i] = drogon::sync_wait(
          rp::run_replay(admin_state(), captures[i].sink(), /*since_seq=*/0,
                         {"plinth:data:ext_y08.t"}, ecfg));
    });
  }
  for (auto& w : workers) {
    w.join();
  }

  for (int i = 0; i < N; ++i) {
    CHECK(results[i].emitted == 100);
    CHECK(captures[i].count_type("replay") == 100);
  }
}

// ── Y.09 ─────────────────────────────────────────────────────────────

TEST_CASE("Y.09: cursor-expired precondition — since_seq below MIN(seq)",
          "[realtime][events][replay][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  h.seeder.seed("plinth:data:ext_y09.t", 50);

  // Force MIN(seq) higher than the client's since_seq by deleting the
  // first 30 rows — leaving seq 31..50.
  h.seeder.exec("DELETE FROM plinth.events WHERE seq <= 30");

  plinth::Config::Realtime::Events ecfg;
  CapturedFrames cf;
  auto r = drogon::sync_wait(rp::run_replay(admin_state(), cf.sink(),
                                            /*since_seq=*/10,
                                            {"plinth:data:ext_y09.t"}, ecfg));

  REQUIRE(r.resync.has_value());
  CHECK(*r.resync == rp::ResyncReason::CURSOR_EXPIRED);
  CHECK(cf.count_type("resync") == 1);
  CHECK(cf.last_frame().find("cursor_expired") != std::string::npos);
}
