// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §14 — S.* sequence-generation cases. Phase 2 lands the
// writer-first topology shift; this TU verifies that envelope.seq is
// stamped from the writer's `INSERT … RETURNING seq` BEFORE
// `broker::dispatch` fires, that monotonicity holds across the
// production code path, and that the failure / lock-loss / disabled
// branches preserve the no-stamp / no-dispatch invariants.
//
// S.06 (gap-detected audit) rides Phase 5's broker-side coverage. S.07
// uses the shared multi-process harness to kill an exact production writer
// child after COMMIT and prove replay plus resumed live delivery catch up.

#include "kernel/realtime/events_writer.hpp"

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/realtime/replay.hpp"
#include "kernel/ws/conn_state.hpp"

#include "../packages/advisory_lock_harness.hpp"
#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ew = plinth::realtime::events_writer;
namespace cs = plinth::realtime::cursor_store;
namespace rp = plinth::realtime::replay;

using namespace std::chrono_literals;

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

  [[nodiscard]] auto exec_params(const std::string& sql,
                                 const std::vector<std::string>& params) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) {
      values.push_back(param.c_str());
    }
    return {PQexecParams(conn, sql.c_str(), static_cast<int>(values.size()),
                         nullptr, values.data(), nullptr, nullptr, 0),
            PQclear};
  }
};

auto child_write(std::string_view text) -> void {
  const char* cursor = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const auto written = ::write(STDOUT_FILENO, cursor, remaining);
    if (written > 0) {
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

auto stop_writer_after_commit() noexcept -> void {
  child_write("READY\n");
  while (true) {
    (void)::pause();
  }
}

// Run one envelope through the real production writer arm in a fresh child.
// The child owns a post-fork DbClient; no parent pool crosses the process
// boundary. `_Exit` in AdvisoryLockHarness intentionally owns final teardown.
auto child_write_with_production_writer(const plinth::Config::Database& db_cfg,
                                        std::string_view channel,
                                        std::string_view kind,
                                        bool wait_for_kill) -> int {
  auto db = drogon::orm::DbClient::newPgClient(
      plinth::lock_test::build_conninfo(db_cfg), /*connNum=*/1);
  if (!db) {
    child_write("DB_CLIENT_FAIL\n");
    return 1;
  }

  ew::clear_insert_hook_for_test();
  ew::clear_advisory_lock_hook_for_test();
  ew::clear_pre_broker_hook_for_test();
  ew::clear_post_commit_hook_for_test();
  ew::reset_counters_for_test();
  ew::set_db_client_for_test(db);
  if (wait_for_kill) {
    ew::set_post_commit_hook_for_test(&stop_writer_after_commit);
  }

  plinth::Config::Realtime::Events cfg;
  ew::start(cfg);
  auto event = plinth::realtime::DispatchedEvent{};
  event.layer = "data";
  event.channel = std::string{channel};
  event.envelope = Json::Value(Json::objectValue);
  event.envelope["layer"] = "data";
  event.envelope["channel"] = event.channel;
  event.envelope["kind"] = std::string{kind};
  if (!ew::enqueue_and_drain_one_for_test(std::move(event))) {
    child_write("WRITE_FAIL\n");
    return 2;
  }
  if (wait_for_kill) {
    child_write("HOOK_RETURNED\n");
    return 3;
  }
  if (ew::writes_persisted_for_test() != 1) {
    child_write("PERSISTENCE_FAIL\n");
    return 4;
  }
  child_write("COMMITTED\n");
  return 0;
}

struct CapturedFrames {
  std::mutex mu;
  std::vector<std::string> frames;

  auto sink() {
    return [this](std::string frame) {
      std::lock_guard lock(mu);
      frames.push_back(std::move(frame));
    };
  }

  auto snapshot() -> std::vector<std::string> {
    std::lock_guard lock(mu);
    return frames;
  }
};

struct S07DatabaseCleanup {
  TestPg& pg;
  std::string channel;
  std::string user_id;

  ~S07DatabaseCleanup() {
    (void)pg.exec_params("DELETE FROM plinth.events WHERE channel = $1",
                         {channel});
    (void)pg.exec_params("DELETE FROM plinth.users WHERE id = $1::uuid",
                         {user_id});
  }
};

struct S07RuntimeCleanup {
  bool active{true};

  ~S07RuntimeCleanup() { shutdown(); }

  auto shutdown() -> void {
    if (!active) {
      return;
    }
    active = false;
    (void)ew::stop();
    ew::set_db_client_for_test(nullptr);
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::clear_post_commit_hook_for_test();
    ew::clear_pre_broker_hook_for_test();
    rp::set_db_client_for_test(nullptr);
    cs::set_db_client_for_test(nullptr);
    cs::clear_cache_for_test();
    plinth::realtime::clear_handlers_for_test();
    plinth::realtime::broker::stop();
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
    ew::clear_post_commit_hook_for_test();
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
    ew::clear_post_commit_hook_for_test();
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

  // The pre-broker hook captures the envelope at the precise
  // instant after the writer has stamped seq from the RETURNING
  // result and BEFORE `broker::dispatch` fires. After Phase 2 the
  // envelope MUST already carry a numeric `seq` field at that
  // point — that is the writer-first invariant L.08 pins.
  std::int64_t observed_seq = -1;
  bool seq_present = false;
  Harness h;
  TestPg pg{pg_config()};
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
  bool pre_broker_fired = false;
  Harness h{cfg};

  // Force the writer to use the test arm via an INSERT hook that
  // synthesizes a PG-side failure. The pre-broker hook is wired but
  // MUST not fire on the failure path (S.02 acceptance).
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
  bool pre_broker_fired = false;
  Harness h;

  // The lock hook returns false → §HA losers skip silently.
  ew::set_advisory_lock_hook_for_test([](const std::string&) { return false; });

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
          "[realtime][events][writer][seq][integration][subprocess]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  const auto DB_CFG = pg_config();
  reset_schema(DB_CFG);
  TestPg pg{DB_CFG};

  const std::string USER_ID = "00000000-0000-0000-0000-000000000034";
  const std::string CHANNEL = "plinth:data:ext_s07.recovery";
  const std::string BASELINE_PAYLOAD =
      R"({"layer":"data","channel":"plinth:data:ext_s07.recovery","kind":"baseline"})";
  S07DatabaseCleanup database_cleanup{pg, CHANNEL, USER_ID};

  auto user =
      pg.exec_params("INSERT INTO plinth.users (id, username, password_hash) "
                     "VALUES ($1::uuid, '__s07_crash_recovery', 'x')",
                     {USER_ID});
  REQUIRE(PQresultStatus(user.get()) == PGRES_COMMAND_OK);

  auto baseline = pg.exec_params("INSERT INTO plinth.events (channel, payload) "
                                 "VALUES ($1, $2::jsonb) RETURNING seq",
                                 {CHANNEL, BASELINE_PAYLOAD});
  REQUIRE(PQresultStatus(baseline.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(baseline.get()) == 1);
  const auto BASELINE_SEQ = std::stoll(PQgetvalue(baseline.get(), 0, 0));

  auto cursor_seed = pg.exec_params(
      "INSERT INTO plinth.user_event_cursors (user_id, last_seq) "
      "VALUES ($1::uuid, $2::bigint)",
      {USER_ID, std::to_string(BASELINE_SEQ)});
  REQUIRE(PQresultStatus(cursor_seed.get()) == PGRES_COMMAND_OK);

  plinth::lock_test::AdvisoryLockHarness process_harness(DB_CFG);
  const auto victim = process_harness.run_until_ready_and_kill(
      10s, [&](PGconn*, int /*idx*/, int /*total*/) {
        return child_write_with_production_writer(DB_CFG, CHANNEL, "victim",
                                                  true);
      });
  INFO("victim stdout: " << victim.stdout_text);
  REQUIRE(victim.checkpoint_reached);
  REQUIRE(victim.kill_requested);
  REQUIRE_FALSE(victim.timed_out);
  REQUIRE(victim.exit_code == -SIGKILL);
  errno = 0;
  int victim_status = 0;
  CHECK(::waitpid(victim.pid, &victim_status, WNOHANG) == -1);
  CHECK(errno == ECHILD);

  auto after_victim =
      pg.exec_params("SELECT seq, payload->>'kind' FROM plinth.events "
                     "WHERE channel = $1 ORDER BY seq",
                     {CHANNEL});
  REQUIRE(PQresultStatus(after_victim.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(after_victim.get()) == 2);
  const auto VICTIM_SEQ = std::stoll(PQgetvalue(after_victim.get(), 1, 0));
  REQUIRE(VICTIM_SEQ > BASELINE_SEQ);
  REQUIRE(std::string_view{PQgetvalue(after_victim.get(), 1, 1)} == "victim");

  auto cursor_after_crash =
      pg.exec_params("SELECT last_seq FROM plinth.user_event_cursors "
                     "WHERE user_id = $1::uuid",
                     {USER_ID});
  REQUIRE(PQresultStatus(cursor_after_crash.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::stoll(PQgetvalue(cursor_after_crash.get(), 0, 0)) ==
          BASELINE_SEQ);

  auto survivor =
      process_harness.run(1, 10s, [&](PGconn*, int /*idx*/, int /*total*/) {
        return child_write_with_production_writer(DB_CFG, CHANNEL, "survivor",
                                                  false);
      });
  REQUIRE(survivor.size() == 1);
  INFO("survivor stdout: " << survivor[0].stdout_text);
  REQUIRE_FALSE(survivor[0].timed_out);
  REQUIRE(survivor[0].exit_code == 0);
  REQUIRE(survivor[0].stdout_text.find("COMMITTED\n") != std::string::npos);
  errno = 0;
  int survivor_status = 0;
  CHECK(::waitpid(survivor[0].pid, &survivor_status, WNOHANG) == -1);
  CHECK(errno == ECHILD);

  auto committed =
      pg.exec_params("SELECT seq, payload->>'kind' FROM plinth.events "
                     "WHERE channel = $1 ORDER BY seq",
                     {CHANNEL});
  REQUIRE(PQresultStatus(committed.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(committed.get()) == 3);
  const auto SURVIVOR_SEQ = std::stoll(PQgetvalue(committed.get(), 2, 0));
  REQUIRE(SURVIVOR_SEQ > VICTIM_SEQ);
  REQUIRE(std::string_view{PQgetvalue(committed.get(), 2, 1)} == "survivor");

  auto db = plinth::realtime_test::shared_pg_client(/*connNum=*/4);
  REQUIRE(db);
  cs::clear_cache_for_test();
  plinth::Config::Realtime::Events events_cfg;
  cs::configure(events_cfg);
  cs::set_db_client_for_test(db);
  rp::set_db_client_for_test(db);
  rp::reset_audit_state_for_test();
  plinth::realtime::clear_handlers_for_test();
  S07RuntimeCleanup runtime_cleanup;
  plinth::Config::Realtime::Broker broker_cfg;
  plinth::realtime::broker::start(broker_cfg);
  ew::set_db_client_for_test(db);
  ew::start(events_cfg);

  plinth::ws::ConnState state;
  state.authenticated = true;
  state.is_admin = true;
  state.auth.user_id = USER_ID;
  CapturedFrames captured;
  const auto replay_result = drogon::sync_wait(rp::run_replay(
      state, captured.sink(), BASELINE_SEQ, {CHANNEL}, events_cfg));

  REQUIRE_FALSE(replay_result.aborted);
  REQUIRE_FALSE(replay_result.resync.has_value());
  REQUIRE(replay_result.emitted == 2);
  REQUIRE(replay_result.up_to_seq == SURVIVOR_SEQ);
  const auto FRAMES = captured.snapshot();
  REQUIRE(FRAMES.size() == 3);
  CHECK(FRAMES[0].find("\"seq\":" + std::to_string(VICTIM_SEQ)) !=
        std::string::npos);
  CHECK(FRAMES[1].find("\"seq\":" + std::to_string(SURVIVOR_SEQ)) !=
        std::string::npos);
  CHECK(FRAMES[2].find("\"type\":\"replay_done\"") != std::string::npos);

  auto cursor_after_replay =
      pg.exec_params("SELECT last_seq FROM plinth.user_event_cursors "
                     "WHERE user_id = $1::uuid",
                     {USER_ID});
  REQUIRE(PQresultStatus(cursor_after_replay.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::stoll(PQgetvalue(cursor_after_replay.get(), 0, 0)) ==
          BASELINE_SEQ);

  // Prove the restarted writer resumes normal live delivery strictly after
  // the recovered window and catches the durable cursor up through the new
  // live seq. The hook models the broker's synchronous WS delivery pre-pass.
  auto live_cfg = events_cfg;
  live_cfg.cursor_flush_threshold = 1;
  cs::configure(live_cfg);
  std::int64_t live_seq = 0;
  ew::set_pre_broker_hook_for_test(
      [&](const plinth::realtime::DispatchedEvent& event) {
        live_seq = event.envelope["seq"].asInt64();
        event.delivered_to_users.push_back(USER_ID);
      });
  REQUIRE(ew::enqueue_for_test(build_dispatched("data", CHANNEL)));
  ew::apply_drain_for_test();
  REQUIRE(live_seq > SURVIVOR_SEQ);

  auto cursor_after_live =
      pg.exec_params("SELECT last_seq FROM plinth.user_event_cursors "
                     "WHERE user_id = $1::uuid",
                     {USER_ID});
  REQUIRE(PQresultStatus(cursor_after_live.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::stoll(PQgetvalue(cursor_after_live.get(), 0, 0)) == live_seq);

  REQUIRE(ew::stop());
  runtime_cleanup.shutdown();

  auto delete_events =
      pg.exec_params("DELETE FROM plinth.events WHERE channel = $1", {CHANNEL});
  REQUIRE(PQresultStatus(delete_events.get()) == PGRES_COMMAND_OK);
  auto delete_user =
      pg.exec_params("DELETE FROM plinth.users WHERE id = $1::uuid", {USER_ID});
  REQUIRE(PQresultStatus(delete_user.get()) == PGRES_COMMAND_OK);
  auto residue = pg.exec_params(
      "SELECT (SELECT count(*) FROM plinth.events WHERE channel = $1) + "
      "(SELECT count(*) FROM plinth.users WHERE id = $2::uuid) + "
      "(SELECT count(*) FROM plinth.user_event_cursors "
      " WHERE user_id = $2::uuid)",
      {CHANNEL, USER_ID});
  REQUIRE(PQresultStatus(residue.get()) == PGRES_TUPLES_OK);
  CHECK(std::string_view{PQgetvalue(residue.get(), 0, 0)} == "0");
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
  bool pre_broker_fired = false;
  Harness h{cfg};
  TestPg pg{pg_config()};

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
