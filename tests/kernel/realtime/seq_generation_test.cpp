// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §14 — S.* sequence-generation cases. Phase 2 lands the
// writer-first topology shift; this TU verifies that envelope.seq is
// stamped from the writer's `INSERT … RETURNING seq` BEFORE
// `broker::dispatch` fires, that monotonicity holds across the
// production code path, and that the failure / lock-loss / disabled
// branches preserve the no-stamp / no-dispatch invariants.
//
// S.06 (gap-detected audit) + S.07 (cursor catch-up across writer
// crash) ride Phase 5 (broker-side gap detection) and the multi-node
// integration harness respectively; both `SKIP()` here with a
// pointer to where they will land.

#include "kernel/realtime/events_writer.hpp"

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/listener.hpp"

#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <expected>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ew = plinth::realtime::events_writer;

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
};

auto build_dispatched(std::string_view layer, std::string_view channel)
    -> plinth::realtime::DispatchedEvent {
  plinth::realtime::DispatchedEvent ev;
  ev.layer = std::string{layer};
  ev.channel = std::string{channel};
  ev.envelope = Json::Value(Json::objectValue);
  ev.envelope["layer"] = std::string{layer};
  ev.envelope["channel"] = std::string{channel};
  ev.envelope["emitted_at"] = "1970-01-01T00:00:00.000Z";
  return ev;
}

// 0.6.3.N — local helper deferred to the shared header; the original
// pattern (bad_weak_ptr at PG-client IO-thread teardown,
// `project_ws_flaky_segfault.md`) is the same family of failure that
// surfaced as `EventLoopThreadPool::~ + Resource deadlock avoided` in
// the v0.6.3 PR-CI L.06 sweep. The shared header generalises this TU's
// historical pattern across every realtime test TU.

// RAII fixture — start writer with a pinned DbClient + start the
// broker so the writer-downstream `broker::dispatch` path is live.
struct Harness {
  explicit Harness(plinth::Config::Realtime::Events cfg = {}) {
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::clear_pre_broker_hook_for_test();
    ew::reset_counters_for_test();
    plinth::realtime::clear_handlers_for_test();
    plinth::Config::Realtime::Broker bcfg{};
    plinth::realtime::broker::start(bcfg);
    ew::set_db_client_for_test(
        plinth::realtime_test::shared_pg_client(/*connNum=*/1));
    ew::start(cfg);
  }
  ~Harness() {
    ew::stop();
    ew::set_db_client_for_test(nullptr);
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::clear_pre_broker_hook_for_test();
    plinth::realtime::clear_handlers_for_test();
    plinth::realtime::broker::stop();
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

} // namespace

// ── S.01 ─────────────────────────────────────────────────────────────

TEST_CASE("S.01: writer stamps envelope.seq before broker dispatch",
          "[realtime][events][writer][seq][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  // The pre-broker hook captures the envelope at the precise
  // instant after the writer has stamped seq from the RETURNING
  // result and BEFORE `broker::dispatch` fires. After Phase 2 the
  // envelope MUST already carry a numeric `seq` field at that
  // point — that is the writer-first invariant L.08 pins.
  std::int64_t observed_seq = -1;
  bool seq_present = false;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent& ev) {
        if (ev.envelope.isMember("seq")) {
          seq_present = true;
          observed_seq = ev.envelope["seq"].asInt64();
        }
      });

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_s01.t")));
  ew::apply_drain_for_test();

  CHECK(seq_present);
  CHECK(observed_seq > 0);

  // Cross-check: envelope.seq matches the BIGSERIAL plinth.events.seq
  // for the same row.
  auto res = pg.exec("SELECT seq FROM plinth.events "
                     "WHERE channel = 'plinth:data:ext_s01.t' LIMIT 1");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  auto table_seq = std::stoll(PQgetvalue(res.get(), 0, 0));
  CHECK(table_seq == observed_seq);
}

// ── S.02 ─────────────────────────────────────────────────────────────

TEST_CASE("S.02: INSERT failure leaves envelope.seq absent + skips broker",
          "[realtime][events][writer][seq][unit]") {
  plinth::Config::Realtime::Events cfg; // enabled=true; no DbClient
  cfg.enabled = false;                  // suppress the production arm
  Harness h{cfg};

  // Force the writer to use the test arm via an INSERT hook that
  // synthesizes a PG-side failure. The pre-broker hook is wired but
  // MUST not fire on the failure path (S.02 acceptance).
  bool pre_broker_fired = false;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent&) {
        pre_broker_fired = true;
      });
  ew::set_insert_hook_for_test(
      [](const std::string&,
         const Json::Value&) -> std::expected<std::int64_t, std::string> {
        return std::unexpected("synthetic_pg_error");
      });

  // The writer has to be running to drain — start it manually with
  // the disabled config + restore enabled for this test only.
  plinth::Config::Realtime::Events live;
  live.enabled = true;
  ew::stop();
  ew::start(live);

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_s02.t")));
  ew::apply_drain_for_test();

  CHECK_FALSE(pre_broker_fired);
  CHECK(ew::writes_persisted_for_test() == 0);
}

// ── S.03 ─────────────────────────────────────────────────────────────

TEST_CASE("S.03: advisory-lock loss silently skips broker + cursor",
          "[realtime][events][writer][seq][unit]") {
  Harness h;

  // The lock hook returns false → §HA losers skip silently.
  ew::set_advisory_lock_hook_for_test([](const std::string&) { return false; });

  bool pre_broker_fired = false;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent&) {
        pre_broker_fired = true;
      });

  // Pair the lock-loss with an INSERT hook so the writer routes
  // through the test arm (otherwise it tries the production PG
  // arm and the harness's nullptr db_client trips a different
  // audit path).
  ew::set_insert_hook_for_test(
      [](const std::string&,
         const Json::Value&) -> std::expected<std::int64_t, std::string> {
        return std::expected<std::int64_t, std::string>{42};
      });

  REQUIRE(
      ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_s03.t")));
  ew::apply_drain_for_test();

  CHECK_FALSE(pre_broker_fired);
  CHECK(ew::writes_persisted_for_test() == 0);
}

// ── S.04 ─────────────────────────────────────────────────────────────

TEST_CASE("S.04: concurrent enqueue → distinct monotonic seqs",
          "[realtime][events][writer][seq][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  constexpr std::size_t PER_THREAD = 25;
  std::atomic<std::size_t> dropped{0};
  auto enq = [&](std::string_view chan) {
    for (std::size_t i = 0; i < PER_THREAD; ++i) {
      if (!ew::enqueue_for_test(build_dispatched("data", chan))) {
        dropped.fetch_add(1);
      }
    }
  };
  std::thread t1{enq, "plinth:data:ext_s04.a"};
  std::thread t2{enq, "plinth:data:ext_s04.b"};
  t1.join();
  t2.join();
  ew::apply_drain_for_test();

  CHECK(dropped.load() == 0);
  CHECK(ew::writes_persisted_for_test() == PER_THREAD * 2);

  auto res =
      pg.exec("SELECT MIN(seq), MAX(seq), COUNT(*), COUNT(DISTINCT seq) "
              "FROM plinth.events WHERE channel LIKE 'plinth:data:ext_s04.%'");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  auto count = std::stoll(PQgetvalue(res.get(), 0, 2));
  auto distinct_count = std::stoll(PQgetvalue(res.get(), 0, 3));
  CHECK(static_cast<std::size_t>(count) == (PER_THREAD * 2));
  CHECK(distinct_count == count); // every seq distinct
}

// ── S.05 ─────────────────────────────────────────────────────────────

TEST_CASE("S.05: 100-envelope sequence is gapless when no failures",
          "[realtime][events][writer][seq][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());
  Harness h;
  TestPg pg{pg_config()};

  constexpr std::size_t N = 100;
  for (std::size_t i = 0; i < N; ++i) {
    REQUIRE(ew::enqueue_for_test(
        build_dispatched("data", "plinth:data:ext_s05.t")));
  }
  ew::apply_drain_for_test();
  CHECK(ew::writes_persisted_for_test() == N);

  auto res =
      pg.exec("SELECT MIN(seq), MAX(seq) "
              "FROM plinth.events WHERE channel = 'plinth:data:ext_s05.t'");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  auto min_seq = std::stoll(PQgetvalue(res.get(), 0, 0));
  auto max_seq = std::stoll(PQgetvalue(res.get(), 0, 1));
  CHECK((max_seq - min_seq) == static_cast<long long>(N - 1));
}

// ── S.06 ─────────────────────────────────────────────────────────────

TEST_CASE("S.06: gap-detected audit fires on broker-side observation",
          "[realtime][events][writer][seq][integration][.skip]") {
  // Phase 5 lands the broker-side per-(connection, channel)
  // last-seen cache + `realtime.seq.gap_detected` audit pipeline.
  // Pre-broker hook is in place to inject the gap; the audit
  // assertion lives with the rest of the §11 audit wiring in
  // Phase 5's `live_replay_ordering_test.cpp`.
  SKIP("Deferred to Phase 5 — broker-side gap-detection audit");
}

// ── S.07 ─────────────────────────────────────────────────────────────

TEST_CASE("S.07: cursor catches up after writer crash mid-window",
          "[realtime][events][writer][seq][integration][.skip]") {
  // Phase 5 / 6 — needs the multi-process advisory-lock harness
  // (ICD-0.5.4 I.02 sibling) to simulate a crashed writer + a
  // surviving node taking over. Out of scope for Phase 2's
  // single-process topology shift.
  SKIP("Deferred to Phase 5/6 — multi-process failover harness");
}

// ── S.08 ─────────────────────────────────────────────────────────────

TEST_CASE("S.08: events.enabled=false emits no seq + no INSERT",
          "[realtime][events][writer][seq][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  reset_schema(pg_config());

  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  Harness h{cfg};
  TestPg pg{pg_config()};

  bool pre_broker_fired = false;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent&) {
        pre_broker_fired = true;
      });

  // Disabled writer drops the enqueue silently per ICD-0.5.4 §Config
  // — no INSERT, no broker call, no cursor advance.
  (void)ew::enqueue_for_test(build_dispatched("data", "plinth:data:ext_s08.t"));
  ew::apply_drain_for_test();

  CHECK_FALSE(pre_broker_fired);
  auto res = pg.exec("SELECT COUNT(*) FROM plinth.events "
                     "WHERE channel = 'plinth:data:ext_s08.t'");
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  CHECK(std::stoi(PQgetvalue(res.get(), 0, 0)) == 0);
}
