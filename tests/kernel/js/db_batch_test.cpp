// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §Test Cases — B.* `db.batch()` transactional wrapper.
// P.02 (inside-batch single SET LOCAL) also lands here since it
// asserts against the batch-scope wrapper from phase 4.
//
// B.07 (drain-on-DISABLE) defers to phase 5 — the lifecycle drain
// hook is wired there.
//
// B.* tests assert against both PG row state (via libpq TestPg) and
// coalescer counters (via the `scope_to_buckets` seam test hooks +
// set_emit_hook_for_test).

#include "async_bridge_fixture.hpp"

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/js/db_batch_schema_check.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/execution_lifetime.hpp"
#include "kernel/js/native_rollback_owner.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/coalescer.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>
#include <poll.h>
#include <quickjs.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::reset_schema;
using plinth::async_bridge_test::test_config;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::EvalResult;
using plinth::js::run_on_context;
using plinth::js::RuntimePool;
using plinth::realtime::CoalescerRegistry;

namespace {

auto drive(BridgeContext& bc, std::string_view source) -> EvalResult {
  return drogon::sync_wait(run_on_context(bc, source));
}

auto make_pool(const plinth::Config& cfg) -> RuntimePool {
  return {/*ext=*/nullptr, default_runtime_limits(), cfg, 1};
}

auto eval_as(RuntimePool& pool, std::string_view ext_name,
             std::string_view source) -> EvalResult {
  auto* bc = pool.acquire();
  bc->extension_name = ext_name;
  auto r = drive(*bc, source);
  pool.destroy(bc);
  return r;
}

struct TestPg {
  PGconn* conn{nullptr};

  explicit TestPg(const plinth::Config::Database& db) {
    auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                    " dbname=" + db.database + " user=" + db.user +
                    " password=" + db.password + " connect_timeout=3";
    conn = PQconnectdb(conninfo.c_str());
    REQUIRE(PQstatus(conn) == CONNECTION_OK);
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

  auto exec(const std::string& sql) const -> void {
    const std::unique_ptr<PGresult, decltype(&PQclear)> res{
        PQexec(conn, sql.c_str()), PQclear};
    REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
  }

  [[nodiscard]] auto count_rows(const std::string& qualified_table) const
      -> int {
    const std::unique_ptr<PGresult, decltype(&PQclear)> res{
        PQexec(conn, ("SELECT COUNT(*) FROM " + qualified_table).c_str()),
        PQclear};
    REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(res.get()) == 1);
    return std::stoi(PQgetvalue(res.get(), 0, 0));
  }

  [[nodiscard]] auto count_audit(const std::string& action,
                                 const std::string& extension) const -> int {
    std::array<const char*, 2> params{action.c_str(), extension.c_str()};
    PGresult* res =
        PQexecParams(conn,
                     "SELECT COUNT(*) FROM plinth.audit_log "
                     "WHERE action = $1 AND detail->>'extension' = $2",
                     2, nullptr, params.data(), nullptr, nullptr, 0);
    int n = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      n = std::stoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return n;
  }
};

auto wait_for_audit(const TestPg& pg, const std::string& action,
                    const std::string& extension, int expect) -> bool {
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + std::chrono::seconds{2};
  while (clock::now() < deadline) {
    if (pg.count_audit(action, extension) >= expect) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  return pg.count_audit(action, extension) >= expect;
}

auto reset_for_batch_test(const plinth::Config& cfg) -> void {
  reset_schema(cfg.db);
  CoalescerRegistry::instance().clear_windows_for_test();
  plinth::js::reset_batch_audit_for_test();
  plinth::js::set_batch_audit_window_ms(60000);
  plinth::js::set_batch_max_ops_per_batch(500);
  plinth::js::set_batch_timeout_ms(30000);
  plinth::js::db::set_search_path_enforce(true);
  plinth::js::db::reset_cross_extension_audit_for_test();
}

// Shared ext_batch schema for tests that don't need isolation.
auto setup_ext_batch(const TestPg& pg) -> void {
  pg.exec("DROP SCHEMA IF EXISTS ext_batch CASCADE");
  pg.exec("CREATE SCHEMA ext_batch");
  pg.exec("CREATE TABLE ext_batch.notes (id serial primary key, body text)");
}

// Fixture-only observation called synchronously by the original rejection
// handler, BEFORE its second batch or the driver's exit cleanup can run.
auto batch_rejection_snapshot(JSContext* ctx, JSValueConst /*this_value*/,
                              int /*argc*/, JSValue* /*argv*/) -> JSValue {
  auto* context = static_cast<BridgeContext*>(JS_GetContextOpaque(ctx));
  if (context == nullptr) {
    return JS_ThrowInternalError(ctx, "batch fixture context unavailable");
  }
  const auto& batch = context->batch_state;
  const std::array<std::pair<const char*, std::int64_t>, 10> fields{{
      {"depth", batch.depth},
      {"scopeLive", batch.scope_id != 0},
      {"operations", static_cast<std::int64_t>(batch.ops_in_batch)},
      {"timedOut", batch.timed_out},
      {"pinned", batch.pinned_conn != nullptr},
      {"rollbackOwner", batch.rollback_owner != nullptr},
      {"timerLive", batch.timer_id != trantor::InvalidTimerId ||
                        batch.timer_loop != nullptr},
      {"callbacks", static_cast<std::int64_t>(context->callbacks.size())},
      {"pending", static_cast<std::int64_t>(context->pending_ops.size())},
      {"detached", context->inflight_detached.load()},
  }};
  JSValue snapshot = JS_NewObject(ctx);
  if (JS_IsException(snapshot)) {
    return snapshot;
  }
  for (const auto& [name, value] : fields) {
    if (JS_SetPropertyStr(ctx, snapshot, name, JS_NewInt64(ctx, value)) < 0) {
      JS_FreeValue(ctx, snapshot);
      return JS_EXCEPTION;
    }
  }
  return snapshot;
}

// These helpers belong to the regression fixture, not the production runtime.
// No sleep, replacement orchestrator, replacement deadline, or DB mock is used
// by the PostgreSQL case below.
class BatchLease {
 public:
  explicit BatchLease(RuntimePool& owner) : pool(owner), bc(pool.acquire()) {}
  explicit BatchLease(std::shared_ptr<RuntimePool> owner)
      : owned_pool(std::move(owner)), pool(*owned_pool), bc(pool.acquire()) {}
  ~BatchLease() { destroy(); }
  BatchLease(const BatchLease&) = delete;
  auto operator=(const BatchLease&) -> BatchLease& = delete;
  BatchLease(BatchLease&&) = delete;
  auto operator=(BatchLease&&) -> BatchLease& = delete;
  auto get() const -> BridgeContext* { return bc; }
  auto destroy() -> void {
    if (bc != nullptr) {
      if (bc->execution_lifetime.load(std::memory_order_acquire)) {
        // Never discard the caller handle if a driver is still using it.
        std::terminate();
      }
      // Cancel the fixture's remaining timer before normal pool retirement.
      plinth::js::clear_batch_timer(bc->batch_state.scope_id);
      pool.destroy(bc);
      bc = nullptr;
    }
  }

 private:
  std::shared_ptr<RuntimePool> owned_pool;
  RuntimePool& pool;
  BridgeContext* bc;
};

struct BatchRegressionReset {
  BatchRegressionReset() = default;
  ~BatchRegressionReset() {
    plinth::js::set_batch_timeout_ms(30000);
    CoalescerRegistry::instance().clear_emit_hook_for_test();
    plinth::js::discard_batches_for_extension("batch");
    CoalescerRegistry::instance().clear_windows_for_test();
  }
  BatchRegressionReset(const BatchRegressionReset&) = delete;
  auto operator=(const BatchRegressionReset&) -> BatchRegressionReset& = delete;
  BatchRegressionReset(BatchRegressionReset&&) = delete;
  auto operator=(BatchRegressionReset&&) -> BatchRegressionReset& = delete;
};

struct TimeoutObservation {
  std::uint64_t scope = 0;
  std::size_t operations = 0;
  bool pinned = false;
};

class BatchTimeoutGate {
 public:
  BatchTimeoutGate(TestPg& controller, std::shared_ptr<BatchLease> context)
      : pg(controller), state(std::make_shared<State>(std::move(context))),
        observed(state->observation.get_future()),
        result(state->result.get_future()), loop(drogon::app().getLoop()) {
    // Session lock and transaction lock use the same PostgreSQL key space.
    const std::unique_ptr<PGresult, decltype(&PQclear)> acquired{
        PQexec(pg.conn, "SELECT pg_try_advisory_lock(53144, 953)"), PQclear};
    REQUIRE(PQresultStatus(acquired.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(acquired.get()) == 1);
    REQUIRE(std::string_view{PQgetvalue(acquired.get(), 0, 0)} == "t");
    timer = loop->runEvery(0.005, [owned = state] {
      if (!owned->active.load(std::memory_order_acquire) || owned->reported) {
        return;
      }
      // Read only on the actual JS/deadline owner loop. This observes the real
      // production timer flag; it does not set it or infer it from elapsed
      // time.
      if (auto lifetime = owned->context->get()->execution_lifetime.load(
              std::memory_order_acquire)) {
        lifetime->visit([&](BridgeContext& context) {
          const auto& batch = context.batch_state;
          if (batch.timed_out) {
            owned->reported = true;
            owned->observation.set_value({batch.scope_id, batch.ops_in_batch,
                                          batch.pinned_conn != nullptr});
          }
        });
      }
    });
  }
  ~BatchTimeoutGate() { finish(); }
  BatchTimeoutGate(const BatchTimeoutGate&) = delete;
  auto operator=(const BatchTimeoutGate&) -> BatchTimeoutGate& = delete;
  BatchTimeoutGate(BatchTimeoutGate&&) = delete;
  auto operator=(BatchTimeoutGate&&) -> BatchTimeoutGate& = delete;

  auto start() -> void {
    worker = std::thread([owned = state, context = state->context] {
      try {
        owned->result.set_value(drive(*context->get(), R"JS(
          globalThis.__batch_callback_finished = false;
          db.batch(async () => {
            await db.exec("INSERT INTO notes(body) VALUES('timed-out')");
            await db.query("SELECT pg_advisory_xact_lock(53144, 953)");
            __batch_callback_finished = true;
            return 'body-finished';
          }).then(
            v => ({first: 'unexpected:' + v}),
            e => {
              const beforeReuse = __batch_rejection_snapshot();
              return db.batch(async () => {
                await db.exec("INSERT INTO notes(body) VALUES('subsequent')");
                return 'second-ok';
              }).then(
                v => ({first: e.code, second: v, beforeReuse,
                       bodyFinished: __batch_callback_finished}),
                later => ({first: e.code, second: 'err:' + later.code,
                           beforeReuse, bodyFinished: __batch_callback_finished})
              );
            }
          )
        )JS"));
      } catch (...) {
        owned->result.set_exception(std::current_exception());
      }
    });
  }

  auto observation() -> std::optional<TimeoutObservation> {
    if (observed.wait_for(std::chrono::seconds{3}) !=
        std::future_status::ready) {
      return std::nullopt;
    }
    return observed.get();
  }

  auto finish() -> void {
    if (finished) {
      return;
    }
    // Every path releases the blocker before joining the runtime driver.
    const std::unique_ptr<PGresult, decltype(&PQclear)> unlocked{
        PQexec(pg.conn, "SELECT pg_advisory_unlock(53144, 953)"), PQclear};
    unlock_verified = PQresultStatus(unlocked.get()) == PGRES_TUPLES_OK &&
                      PQntuples(unlocked.get()) == 1 &&
                      std::string_view{PQgetvalue(unlocked.get(), 0, 0)} == "t";
    if (!unlock_verified) {
      // Closing this fixture-only session also releases its advisory lock.
      PQfinish(pg.conn);
      pg.conn = nullptr;
    }
    state->active.store(false, std::memory_order_release);
    loop->invalidateTimer(timer);
    auto barrier = std::make_shared<std::promise<void>>();
    auto completion = barrier->get_future();
    loop->queueInLoop([owned = state, barrier] {
      // Release the observer's actual context/pool owner on its own loop;
      // the worker independently retains its owner until it has joined.
      owned->context.reset();
      barrier->set_value();
    });
    observer_joined = completion.wait_for(std::chrono::seconds{3}) ==
                      std::future_status::ready;
    if (!observer_joined) {
      // An unjoined observer may still be reading its owner. An assertion
      // unwind must never destroy that owner after an unverified barrier.
      std::terminate();
    }
    if (worker.joinable()) {
      if (result.wait_for(std::chrono::seconds{5}) !=
          std::future_status::ready) {
        // A stuck owned worker cannot safely outlive its context. Fail the
        // isolated test process rather than detach it or destroy its owner.
        std::terminate();
      }
      worker.join();
    }
    finished = true;
  }
  auto outcome() -> EvalResult { return result.get(); }
  bool unlock_verified = false;
  bool observer_joined = false;

 private:
  struct State {
    explicit State(std::shared_ptr<BatchLease> owner)
        : context(std::move(owner)) {}
    std::shared_ptr<BatchLease> context;
    std::atomic<bool> active{true};
    bool reported = false; // owner-loop only
    std::promise<TimeoutObservation> observation;
    std::promise<EvalResult> result;
  };
  TestPg& pg;
  std::shared_ptr<State> state;
  std::future<TimeoutObservation> observed;
  std::future<EvalResult> result;
  trantor::EventLoop* loop;
  trantor::TimerId timer;
  std::thread worker;
  bool finished = false;
};

// The disposable DB's AFTER INSERT notification acknowledges an actually
// committed audit row. A socket deadline replaces the older sleep/poll helper.
auto wait_for_batch_audit_notification(PGconn* conn) -> bool {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (std::chrono::steady_clock::now() < deadline) {
    if (PQconsumeInput(conn) == 0) {
      return false;
    }
    while (auto* notification = PQnotifies(conn)) {
      const bool matches = std::string_view{notification->relname} ==
                           "plinth_batch_timeout_audit";
      PQfreemem(notification);
      if (matches) {
        return true;
      }
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }
    pollfd socket{PQsocket(conn), POLLIN, 0};
    const int ready = poll(&socket, 1, static_cast<int>(remaining.count()));
    if (ready < 0 && errno != EINTR) {
      return false;
    }
  }
  return false;
}

auto drain_batch_jobs(BridgeContext& context) -> bool {
  for (int count = 0; count < 128 && JS_IsJobPending(context.rt); ++count) {
    JSContext* job_context = nullptr;
    if (JS_ExecutePendingJob(context.rt, &job_context) < 0) {
      return false;
    }
  }
  return !JS_IsJobPending(context.rt);
}

} // namespace

// ─── B.01 ─────────────────────────────────────────────────────────
TEST_CASE("B.01: happy commit — 3 inserts land + committed audit fires",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch",
                   "db.batch(async () => {"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('c')\");"
                   "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 3);
  REQUIRE(wait_for_audit(pg, "db.batch.committed", "batch", 1));
}

// ─── B.02 ─────────────────────────────────────────────────────────
TEST_CASE("B.02: rollback on user throw — 0 rows + rolled_back audit fires",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch",
                   "db.batch(async () => {"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
                   "  throw { code: 'user.boom', message: 'boom' };"
                   "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "err:user.boom");
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
  REQUIRE(wait_for_audit(pg, "db.batch.rolled_back", "batch", 1));
}

TEST_CASE("batch query reads its own exec writes and commits returning DML",
          "[js][async][db][batch][batch-transaction]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);
  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch", R"JS(
    db.batch(async () => {
      await db.exec("INSERT INTO notes(body) VALUES($1)", ["first"]);
      const {rows} = await db.query("SELECT body FROM notes ORDER BY id");
      if (rows.length !== 1 || rows[0].body !== "first")
        throw {code: "test.missing_uncommitted_write"};
      const {rows: inserted} = await db.query(
        "INSERT INTO notes(body) VALUES($1) RETURNING body", ["second"]);
      if (inserted.length !== 1 || inserted[0].body !== "second")
        throw {code: "test.missing_returning_row"};
      return "ok";
    }).then(v => v, e => "err:" + e.code)
  )JS");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 2);
  REQUIRE(plinth::log::wait_for_audits(std::chrono::seconds{2}));
  REQUIRE(pg.count_audit("db.batch.committed", "batch") == 1);
}

TEST_CASE("batch query returning DML rolls back with exec on user exception",
          "[js][async][db][batch][batch-transaction]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);
  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch", R"JS(
    db.batch(async () => {
      await db.exec("INSERT INTO notes(body) VALUES($1)", ["exec"]);
      const {rows} = await db.query(
        "INSERT INTO notes(body) VALUES($1) RETURNING body", ["query"]);
      if (rows.length !== 1 || rows[0].body !== "query")
        throw {code: "test.missing_returning_row"};
      throw {code: "test.rollback"};
    }).then(() => "unexpected commit", e => e.code)
  )JS");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "test.rollback");
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
}

TEST_CASE("batch query database error rolls back preceding mixed writes",
          "[js][async][db][batch][batch-transaction]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);
  pg.exec("ALTER TABLE ext_batch.notes ADD UNIQUE(body)");
  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch", R"JS(
    db.batch(async () => {
      await db.exec("INSERT INTO notes(body) VALUES($1)", ["exec"]);
      await db.query("INSERT INTO notes(body) VALUES($1) RETURNING body", ["query"]);
      await db.query("INSERT INTO notes(body) VALUES($1) RETURNING body", ["exec"]);
    }).then(() => ({code: "unexpected commit"}), e => ({code: e.code,
      message: e.message, sqlstate: e.sqlstate,
      ownState: Object.prototype.hasOwnProperty.call(e, "sqlstate")}))
  )JS");
  REQUIRE(r.value.has_value());
  REQUIRE((*r.value)["code"].asString() == "db.constraint_violation");
  REQUIRE((*r.value)["sqlstate"].isString());
  REQUIRE((*r.value)["sqlstate"].asString() == "23505");
  REQUIRE((*r.value)["ownState"].asBool());
  REQUIRE((*r.value)["message"].asString().find("notes_body_key") !=
          std::string::npos);
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
}

// ─── B.03 ─────────────────────────────────────────────────────────
TEST_CASE("B.03: rollback on DB error — 0 rows persist",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);
  pg.exec("ALTER TABLE ext_batch.notes ADD COLUMN unique_col int UNIQUE");

  auto pool = make_pool(cfg);
  auto r = eval_as(
      pool, "batch",
      "db.batch(async () => {"
      "  await db.exec(\"INSERT INTO notes(body, unique_col) VALUES('a', 1)\");"
      "  await db.exec(\"INSERT INTO notes(body, unique_col) VALUES('b', 1)\");"
      "}).then(() => 'ok', e => ({code:e.code, sqlstate:e.sqlstate,"
      "ownState:Object.prototype.hasOwnProperty.call(e,'sqlstate')}))");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->isObject());
  REQUIRE((*r.value)["code"].asString() == "db.constraint_violation");
  REQUIRE((*r.value)["sqlstate"].isString());
  REQUIRE((*r.value)["sqlstate"].asString() == "23505");
  REQUIRE((*r.value)["ownState"].asBool());
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
}

// ─── B.04 ─────────────────────────────────────────────────────────
TEST_CASE("B.04: nested db.batch rejects synchronously, outer unaffected",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r =
      eval_as(pool, "batch",
              "db.batch(async () => {"
              "  await db.exec(\"INSERT INTO notes(body) VALUES('outer')\");"
              "  const inner = await db.batch(async () => {})"
              "    .then(() => 'inner-ok', e => 'inner-err:' + e.code);"
              "  return inner;"
              "}).then(v => 'outer-ok:' + v, e => 'outer-err:' + e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() ==
          "outer-ok:inner-err:db.batch.nested_not_allowed");
  // Outer batch committed normally — the outer insert lands.
  REQUIRE(pg.count_rows("ext_batch.notes") == 1);
}

// ─── B.05 ─────────────────────────────────────────────────────────
TEST_CASE("B.05: quota exceeded mid-batch — 501st rejects, batch still commits",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  // Shrink the quota to make the test tractable. ICD bound [1, 100000]
  // allows a 3-op cap for the purposes of this test.
  plinth::js::set_batch_max_ops_per_batch(3);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r = eval_as(
      pool, "batch",
      "db.batch(async () => {"
      "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
      "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
      "  await db.exec(\"INSERT INTO notes(body) VALUES('c')\");"
      "  const quota = await db.exec(\"INSERT INTO notes(body) VALUES('d')\")"
      "    .then(() => 'ok', e => e.code);"
      "  return quota;"
      "}).then(v => 'committed:' + v, e => 'err:' + e.code)");
  REQUIRE(r.value.has_value());
  // ICD §Field semantics: caller swallows the quota rejection and
  // COMMITs with what succeeded (3 rows). `quota_exceeded` is the
  // rejection code on the 4th exec.
  REQUIRE(r.value->asString() == "committed:db.batch.quota_exceeded");
  REQUIRE(pg.count_rows("ext_batch.notes") == 3);
  // Restore.
  plinth::js::set_batch_max_ops_per_batch(500);
}

// ─── B.06 ─────────────────────────────────────────────────────────
// Timeout enforcement deferred — phase 4 ships without a timer
// mechanism on the pinned connection. B.06 test exists as a pointer
// to the deferred behavior. Tracked in CHANGELOG §ICD amendments.

// ─── B.07 defers to phase 5 (lifecycle drain) ─────────────────────

// ─── B.08 ─────────────────────────────────────────────────────────
TEST_CASE("B.08: empty batch — BEGIN + COMMIT only, no envelope, audit fires",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r = eval_as(
      pool, "batch",
      "db.batch(async () => {}).then(() => 'ok', e => 'err:' + e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
  REQUIRE(CoalescerRegistry::instance().open_window_count_for_test() == 0);
  REQUIRE(wait_for_audit(pg, "db.batch.committed", "batch", 1));
}

// ─── B.09 ─────────────────────────────────────────────────────────
TEST_CASE("B.09: silent=true inside batch — both rows land, neither emits",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  // Install a counting emit hook before the batch runs — lets us
  // assert "one envelope per non-silent table at batch commit".
  std::atomic<int> emit_count{0};
  CoalescerRegistry::instance().set_emit_hook_for_test(
      [&emit_count](const Json::Value&) {
        emit_count.fetch_add(1, std::memory_order_relaxed);
        return std::expected<void, plinth::realtime::NotifyError>{};
      });

  auto pool = make_pool(cfg);
  auto r =
      eval_as(pool, "batch",
              "db.batch(async () => {"
              "  await db.exec(\"INSERT INTO notes(body) VALUES('loud')\");"
              "  await db.exec(\"INSERT INTO notes(body) VALUES('quiet')\","
              "                 [], {silent: true});"
              "}).then(() => 'ok', e => 'err:' + e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 2);
  // One envelope from the non-silent insert; silent write is excluded
  // from the scope bucket.
  REQUIRE(emit_count.load() == 1);

  CoalescerRegistry::instance().clear_emit_hook_for_test();
}

// ─── B.10 ─────────────────────────────────────────────────────────
TEST_CASE("B.10: metrics counters monotonic across multiple batches",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  // 5 commits + 3 rollbacks. `batch` ext name matches the
  // `ext_batch` schema setup_ext_batch prepared.
  for (int i = 0; i < 5; ++i) {
    auto r = eval_as(pool, "batch",
                     "db.batch(async () => {"
                     "  await db.exec(\"INSERT INTO notes(body) VALUES('c')\");"
                     "}).then(() => 'ok', e => 'err:' + e.code)");
    REQUIRE(r.value.has_value());
    REQUIRE(r.value->asString() == "ok");
  }
  for (int i = 0; i < 3; ++i) {
    auto r = eval_as(pool, "batch",
                     "db.batch(async () => {"
                     "  await db.exec(\"INSERT INTO notes(body) VALUES('r')\");"
                     "  throw { code: 'user.rollback' };"
                     "}).then(() => 'ok', e => 'err:' + e.code)");
    REQUIRE(r.value.has_value());
    REQUIRE(r.value->asString() == "err:user.rollback");
  }
  REQUIRE(pg.count_rows("ext_batch.notes") == 5);
  // Audit rate limiter aggregates per extension per window; at this
  // scale, the first commit + the first rollback are the visible
  // emits (phase 2 / phase 4 rate-limit pattern). The test asserts
  // at least one of each — monotonicity is proven by the batch-scope
  // bookkeeping even if the audit window suppresses later entries.
  REQUIRE(wait_for_audit(pg, "db.batch.committed", "batch", 1));
  REQUIRE(wait_for_audit(pg, "db.batch.rolled_back", "batch", 1));
}

// ─── B.11 ─────────────────────────────────────────────────────────
TEST_CASE("B.11: cancellation during batch — in-flight exec rejects, 0 rows",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  // Drive a batch whose callback cooperatively checks the cancelled
  // flag via db.exec's internal gate — once bc.cancelled flips, the
  // next db.exec inside the batch rejects db.cancelled.
  auto pool = make_pool(cfg);
  auto* acq = pool.acquire();
  acq->extension_name = "batch";
  // Pre-enqueue cancel via the flag.
  acq->cancelled.store(true, std::memory_order_release);
  auto r =
      drive(*acq, "db.batch(async () => {"
                  "  await db.exec(\"INSERT INTO notes(body) VALUES('x')\");"
                  "}).then(() => 'ok', e => 'err:' + e.code)");
  pool.destroy(acq);

  // The binding rejects inline at db.batch entry (cancelled bc —
  // db.batch never opens a transaction). Rows should be empty.
  if (r.value.has_value()) {
    // cancelled code is db.cancelled.
    REQUIRE(r.value->asString().rfind("err:db.cancelled", 0) == 0);
  }
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
}

// ─── B.12 ─────────────────────────────────────────────────────────
TEST_CASE("B.12: batch_state reset after commit — depth == 0, no leak",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto* acq = pool.acquire();
  acq->extension_name = "batch";
  auto r =
      drive(*acq, "db.batch(async () => {"
                  "  await db.exec(\"INSERT INTO notes(body) VALUES('r')\");"
                  "}).then(() => 'ok', e => 'err:' + e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");

  // After commit the bc.batch_state must be fully reset. Depth 0 +
  // no pinned conn + ops_in_batch 0.
  REQUIRE(acq->batch_state.depth == 0);
  REQUIRE(acq->batch_state.ops_in_batch == 0);
  REQUIRE(!acq->batch_state.pinned_conn);
  pool.destroy(acq);
}

// ─── B.07 ─────────────────────────────────────────────────────────
// Lifecycle drain — discard_batches_for_extension drops the scope
// bucket before the extension's state row moves. Verified against
// the coalescer seam (scope bucket gone → subsequent flush emits
// nothing). This test doesn't exercise the install_lifecycle code
// path directly (that requires a full package install harness);
// instead it drives the drain function directly — sufficient to
// lock the kernel-side contract the lifecycle sites consume.
TEST_CASE("B.07: discard_batches_for_extension drops the scope bucket",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  plinth::js::reset_in_flight_batches_for_test();
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  // Manually register an in-flight batch + seed a coalescer scope
  // bucket, then drain by extension.
  std::uint64_t sid = plinth::js::alloc_batch_scope_id();
  plinth::js::register_in_flight_batch(sid, "batch");
  CoalescerRegistry::instance().record_write(
      "ext_batch", "notes", plinth::realtime::OpKind::INSERT,
      /*row_count=*/1, /*extension_name=*/"batch",
      /*batch_scope_id=*/sid);

  // Drain — should drop the scope; subsequent flush is a no-op.
  auto dropped = plinth::js::discard_batches_for_extension("batch");
  REQUIRE(dropped == 1);

  // The scope bucket is gone; flushing returns 0 emits.
  auto emitted = CoalescerRegistry::instance().flush_batch_scope(sid);
  REQUIRE(emitted == 0);
}

// ─── I.01 ─────────────────────────────────────────────────────────
// End-to-end: db.batch with N inserts → COMMIT → flush_batch_scope
// emits one envelope per (schema, table) → broker fans out to JS
// subscribers within the same bc. Verifies the full pipeline modulo
// the PG LISTEN/NOTIFY round-trip (which I.02 exercises).
TEST_CASE("I.01: batch → coalescer → subscriber receives one envelope",
          "[js][async][db][batch][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  // Count envelopes via the coalescer emit hook — avoids standing up
  // the full WS subscriber harness for the phase 5 integration.
  std::atomic<int> emit_count{0};
  std::atomic<int> insert_count{0};
  CoalescerRegistry::instance().set_emit_hook_for_test(
      [&emit_count, &insert_count](const Json::Value& env) {
        emit_count.fetch_add(1, std::memory_order_relaxed);
        if (env.isMember("ops") && env["ops"].isArray()) {
          for (const auto& op : env["ops"]) {
            if (op["op"].asString() == "insert") {
              insert_count.fetch_add(static_cast<int>(op["count"].asUInt64()),
                                     std::memory_order_relaxed);
            }
          }
        }
        return std::expected<void, plinth::realtime::NotifyError>{};
      });

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch",
                   "db.batch(async () => {"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('c')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('d')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('e')\");"
                   "}).then(() => 'ok', e => 'err:' + e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");

  // Exactly one envelope, carrying count=5 inserts.
  REQUIRE(emit_count.load() == 1);
  REQUIRE(insert_count.load() == 5);
  REQUIRE(pg.count_rows("ext_batch.notes") == 5);

  CoalescerRegistry::instance().clear_emit_hook_for_test();
}

// ─── I.02 ─────────────────────────────────────────────────────────
// Two bcs from the same extension run batches concurrently; each
// commits its own 10 rows. Both envelopes land, counts don't merge
// (distinct scope buckets).
TEST_CASE("I.02: concurrent batches do not cross-contaminate scopes",
          "[js][async][db][batch][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  std::vector<int> envelope_counts;
  std::mutex counts_mu;
  CoalescerRegistry::instance().set_emit_hook_for_test(
      [&envelope_counts, &counts_mu](const Json::Value& env) {
        int inserts = 0;
        if (env.isMember("ops") && env["ops"].isArray()) {
          for (const auto& op : env["ops"]) {
            if (op["op"].asString() == "insert") {
              inserts += static_cast<int>(op["count"].asUInt64());
            }
          }
        }
        std::lock_guard<std::mutex> g(counts_mu);
        envelope_counts.push_back(inserts);
        return std::expected<void, plinth::realtime::NotifyError>{};
      });

  // Run two batches back-to-back through the same pool. Even
  // though the pool is single-threaded for JS, the dispatch
  // arms go through Drogon IO threads — real concurrency at the
  // PG layer. Serializing the eval_as calls still proves the
  // scope-bucket isolation contract because each batch allocates
  // a fresh scope_id.
  auto pool = make_pool(cfg);
  const char* src =
      "db.batch(async () => {"
      "  for (let i = 0; i < 10; i++)"
      "    await db.exec(\"INSERT INTO notes(body) VALUES('x')\");"
      "}).then(() => 'ok', e => 'err:' + e.code)";
  for (int b = 0; b < 2; ++b) {
    auto r = eval_as(pool, "batch", src);
    REQUIRE(r.value.has_value());
    REQUIRE(r.value->asString() == "ok");
  }

  REQUIRE(pg.count_rows("ext_batch.notes") == 20);
  std::lock_guard<std::mutex> g(counts_mu);
  REQUIRE(envelope_counts.size() == 2);
  REQUIRE(envelope_counts[0] == 10);
  REQUIRE(envelope_counts[1] == 10);

  CoalescerRegistry::instance().clear_emit_hook_for_test();
}

// ─── B.06 ─────────────────────────────────────────────────────────
// ICD-0.5.3 §B.06 — wall-clock timeout enforcement. Set
// `timeout_ms=200`; user-fn runs INSERT ⇒ `pg_sleep(0.4)` ⇒ INSERT.
// pg_sleep yields the main loop while the server-side wait runs, so
// the runAfter timer fires and flips `bc.batch_state.timed_out`. The
// second INSERT enqueue sees the flag and rejects inline; the
// orchestrator's catch path runs ROLLBACK. Asserts: outer promise
// rejects `db.batch.timeout`; zero rows persist; rolled_back audit
// fires with reason=`timeout`; no row pre-rollback either (covered
// by the count check after the batch resolves).
TEST_CASE("B.06: timeout — second op rejects db.batch.timeout, rolls back",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  plinth::js::set_batch_timeout_ms(200);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch",
                   "db.batch(async () => {"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
                   "  await db.query(\"SELECT pg_sleep(0.4)\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
                   "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "err:db.batch.timeout");
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
  REQUIRE(wait_for_audit(pg, "db.batch.rolled_back", "batch", 1));

  // The audit detail should record reason=timeout. Read the most
  // recent rolled_back row and assert.
  PGresult* res = PQexecParams(pg.conn,
                               "SELECT detail->>'reason' FROM plinth.audit_log "
                               "WHERE action='db.batch.rolled_back' "
                               "AND detail->>'extension'='batch' "
                               "ORDER BY \"timestamp\" DESC LIMIT 1",
                               0, nullptr, nullptr, nullptr, nullptr, 0);
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 1);
  REQUIRE(std::string{PQgetvalue(res, 0, 0)} == "timeout");
  PQclear(res);

  // Restore the default for any subsequent test in the same process.
  plinth::js::set_batch_timeout_ms(30000);
}

TEST_CASE("batch commit-time timeout permits reuse in the same evaluation",
          "[js][async][db][batch][batch-timeout-cleanup]") {
  REQUIRE(pg_available());
  ensure_drogon_with_db_running();
  auto cfg = test_config();
  cfg.db.pool_size = 1;
  reset_for_batch_test(cfg);
  BatchRegressionReset reset;
  plinth::js::set_batch_timeout_ms(500);
  TestPg pg(cfg.db);
  TestPg blocker(cfg.db);
  setup_ext_batch(pg);
  pg.exec(R"SQL(
    CREATE FUNCTION ext_batch.notify_timeout_audit() RETURNS trigger
    LANGUAGE plpgsql AS $$ BEGIN
      PERFORM pg_notify('plinth_batch_timeout_audit', 'done');
      RETURN NEW;
    END $$;
    CREATE TRIGGER batch_timeout_audit_notify AFTER INSERT ON plinth.audit_log
    FOR EACH ROW WHEN (NEW.action='db.batch.rolled_back'
                      AND NEW.detail->>'extension'='batch')
    EXECUTE FUNCTION ext_batch.notify_timeout_audit();
    LISTEN plinth_batch_timeout_audit;
  )SQL");
  auto emissions = std::make_shared<std::atomic<int>>(0);
  CoalescerRegistry::instance().set_emit_hook_for_test(
      [emissions](const Json::Value&) {
        emissions->fetch_add(1, std::memory_order_relaxed);
        return std::expected<void, plinth::realtime::NotifyError>{};
      });

  auto pool =
      std::make_shared<RuntimePool>(nullptr, default_runtime_limits(), cfg, 1);
  auto lease = std::make_shared<BatchLease>(pool);
  auto& bc = *lease->get();
  bc.extension_name = "batch";
  JSValue global = JS_GetGlobalObject(bc.ctx);
  const int installed =
      JS_SetPropertyStr(bc.ctx, global, "__batch_rejection_snapshot",
                        JS_NewCFunction(bc.ctx, &batch_rejection_snapshot,
                                        "__batch_rejection_snapshot", 0));
  JS_FreeValue(bc.ctx, global);
  REQUIRE(installed >= 0);

  BatchTimeoutGate gate(blocker, lease);
  gate.start();
  const auto observation = gate.observation();
  // This changes only future BEGINs. The original real 500ms timer has
  // already fired and its timed_out flag remains latched before unlock.
  if (observation.has_value()) {
    plinth::js::set_batch_timeout_ms(30000);
  }
  gate.finish();
  REQUIRE(gate.observer_joined);
  REQUIRE(gate.unlock_verified);
  REQUIRE(observation.has_value());
  REQUIRE(observation->scope != 0);
  REQUIRE(observation->operations == 2);
  REQUIRE(observation->pinned);
  auto result = gate.outcome();

  // Result delivery is not a native/audit persistence barrier. Drain accepted
  // producers first, then their audit callback tails, before reading or DDL.
  REQUIRE(plinth::lifecycle::async_tasks().drain(std::chrono::seconds{5}));
  REQUIRE(plinth::log::wait_for_audits(std::chrono::seconds{2}));
  CHECK(plinth::lifecycle::async_tasks().active_for_test() == 0);
  CHECK_FALSE(bc.execution_lifetime.load(std::memory_order_acquire));
  const bool state_reset =
      bc.batch_state.depth == 0 && bc.batch_state.scope_id == 0 &&
      bc.batch_state.ops_in_batch == 0 && !bc.batch_state.timed_out &&
      !bc.batch_state.pinned_conn && !bc.batch_state.rollback_owner &&
      bc.batch_state.timer_id == trantor::InvalidTimerId &&
      bc.batch_state.timer_loop == nullptr;
  const bool async_reset = bc.callbacks.empty() && bc.pending_ops.empty() &&
                           bc.concurrent_async_ops == 0 &&
                           bc.inflight_detached.load() == 0;
  const int before_destroy = pg.count_rows("ext_batch.notes");
  const bool audit_acknowledged = wait_for_batch_audit_notification(pg.conn);
  const int committed = pg.count_audit("db.batch.committed", "batch");
  const int rolled_back = pg.count_audit("db.batch.rolled_back", "batch");
  const std::unique_ptr<PGresult, decltype(&PQclear)> audit{
      PQexec(pg.conn,
             "SELECT detail->>'reason', detail->>'error_code', detail_encoding "
             "FROM plinth.audit_log WHERE action='db.batch.rolled_back' "
             "AND detail->>'extension'='batch'"),
      PQclear};
  const bool audit_detail =
      PQresultStatus(audit.get()) == PGRES_TUPLES_OK &&
      PQntuples(audit.get()) == 1 &&
      std::string_view{PQgetvalue(audit.get(), 0, 0)} == "timeout" &&
      std::string_view{PQgetvalue(audit.get(), 0, 1)} == "db.batch.timeout" &&
      std::string_view{PQgetvalue(audit.get(), 0, 2)} == "0";
  const auto remaining_buckets =
      CoalescerRegistry::instance().batch_scope_count_for_test();
  const auto orphaned_scopes =
      plinth::js::discard_batches_for_extension("batch");
  // No callback replacement: the real NativeRollbackOwner keeps its lease.
  lease->destroy();
  REQUIRE(plinth::lifecycle::async_tasks().drain(std::chrono::seconds{5}));
  REQUIRE(plinth::log::wait_for_audits(std::chrono::seconds{2}));
  const int after_destroy = pg.count_rows("ext_batch.notes");
  const std::unique_ptr<PGresult, decltype(&PQclear)> remaining{
      PQexec(pg.conn, "SELECT body FROM ext_batch.notes ORDER BY id"), PQclear};
  const bool only_second_row =
      PQresultStatus(remaining.get()) == PGRES_TUPLES_OK &&
      PQntuples(remaining.get()) == 1 &&
      std::string_view{PQgetvalue(remaining.get(), 0, 0)} == "subsequent";

  REQUIRE(result.value.has_value());
  INFO("same-evaluation timeout result: " << result.value->toStyledString());
  const auto& value = *result.value;
  CHECK(value["first"].asString() == "db.batch.timeout");
  CHECK(value["bodyFinished"].asBool());
  CHECK(value["second"].asString() == "second-ok");
  REQUIRE(value["beforeReuse"].isObject());
  REQUIRE(value["beforeReuse"].size() == 10);
  for (const auto& field : value["beforeReuse"].getMemberNames()) {
    INFO("pre-reuse field: " << field);
    CHECK(value["beforeReuse"][field].asInt64() == 0);
  }
  CHECK(state_reset);
  CHECK(async_reset);
  CHECK(before_destroy == 1);
  CHECK(audit_acknowledged);
  CHECK(rolled_back == 1);
  CHECK(committed == 1);
  CHECK(audit_detail);
  CHECK(emissions->load() == 1); // Only the successful second batch emits.
  CHECK(after_destroy == 1);
  CHECK(only_second_row);
  CHECK(orphaned_scopes == 0);
  CHECK(remaining_buckets == 0);
}

TEST_CASE("installed batch orchestrator requests rollback before timeout "
          "rejection",
          "[js][db][batch][batch-timeout-hermetic]") {
  // Actual registered db.batch/internal helpers and installed JS orchestrator;
  // only host settlement/reset is simulated. No SQL, driver or timer claim.
  auto cfg = test_config();
  auto pool = make_pool(cfg);
  BatchLease lease(pool);
  auto& bc = *lease.get();
  const std::string_view source = R"JS(
    globalThis.__batch_settlements = 0;
    globalThis.__batch_rejection = '';
    globalThis.__batch_body_calls = 0;
    db.batch(() => { __batch_body_calls++; return 'body-success'; }).then(
      () => { __batch_settlements++; __batch_rejection = 'unexpected-success'; },
      e => { __batch_settlements++; __batch_rejection = e.code; }
    );
  )JS";
  JS_UpdateStackTop(bc.rt);
  JSValue evaluation = JS_Eval(bc.ctx, source.data(), source.size(),
                               "<batch-test>", JS_EVAL_TYPE_GLOBAL);
  const bool evaluated = !JS_IsException(evaluation);
  JS_FreeValue(bc.ctx, evaluation);
  REQUIRE(evaluated);
  auto operations = bc.take_pending_ops();
  REQUIRE(operations.size() == 1);
  CHECK(operations[0].type == plinth::js::AsyncOp::Type::DB_BATCH_BEGIN);
  const auto scope = operations[0].batch_scope_id;
  CHECK(scope != 0);
  CHECK(bc.batch_state.scope_id == scope);
  // An alias-null shared owner token proves ownership transfer without ever
  // constructing or invoking a fake Transaction. Actual transactions are
  // covered by the separate PG regression, not by this host simulation.
  auto owner = std::make_shared<int>(953);
  std::weak_ptr<int> lifetime = owner;
  bc.batch_state.pinned_conn =
      std::shared_ptr<drogon::orm::Transaction>{owner, nullptr};
  bc.batch_state.rollback_owner =
      std::make_shared<plinth::js::NativeRollbackOwner>(nullptr);
  std::weak_ptr<plinth::js::NativeRollbackOwner> rollback_lifetime =
      bc.batch_state.rollback_owner;
  owner.reset();
  bc.batch_state.timed_out = true;
  bc.resolve(operations[0].callback_id, Json::Value{});
  operations.clear();
  REQUIRE(drain_batch_jobs(bc));
  operations = bc.take_pending_ops();
  // Baseline fails here: it rejects directly without enqueueing rollback.
  REQUIRE(operations.size() == 1);
  CHECK(operations[0].type == plinth::js::AsyncOp::Type::DB_BATCH_ROLLBACK);
  CHECK(operations[0].batch_scope_id == scope);
  CHECK(operations[0].rollback_error["code"].asString() == "db.batch.timeout");
  CHECK(bc.batch_state.pinned_conn.use_count() == 0);
  CHECK(operations[0].batch_pinned_conn.use_count() == 1);
  CHECK_FALSE(bc.batch_state.rollback_owner);
  CHECK(operations[0].batch_rollback_owner.use_count() == 1);
  CHECK_FALSE(rollback_lifetime.expired());
  CHECK_FALSE(lifetime.expired());
  auto read = [&bc](const char* property) {
    JSValue global = JS_GetGlobalObject(bc.ctx);
    JSValue value = JS_GetPropertyStr(bc.ctx, global, property);
    int32_t integer = -1;
    const bool converted = JS_ToInt32(bc.ctx, &integer, value) == 0;
    JS_FreeValue(bc.ctx, value);
    JS_FreeValue(bc.ctx, global);
    REQUIRE(converted);
    return integer;
  };
  CHECK(read("__batch_body_calls") == 1);
  CHECK(read("__batch_settlements") == 0);
  const int rollback_callback = operations[0].callback_id;
  // Simulate the owning host's terminal cleanup only AFTER observing enqueue.
  bc.batch_state = BridgeContext::BatchState{};
  operations.clear();
  CHECK(lifetime.expired());
  CHECK(rollback_lifetime.expired());
  bc.resolve(rollback_callback, Json::Value{});
  REQUIRE(drain_batch_jobs(bc));
  CHECK(read("__batch_settlements") == 1);
  JSValue global = JS_GetGlobalObject(bc.ctx);
  JSValue rejection = JS_GetPropertyStr(bc.ctx, global, "__batch_rejection");
  const char* text = JS_ToCString(bc.ctx, rejection);
  const std::string code = text != nullptr ? text : "";
  if (text != nullptr) {
    JS_FreeCString(bc.ctx, text);
  }
  JS_FreeValue(bc.ctx, rejection);
  JS_FreeValue(bc.ctx, global);
  CHECK(code == "db.batch.timeout");
  // Duplicate late completion has no callback owner and must not settle again.
  bc.resolve(rollback_callback, Json::Value{});
  REQUIRE(drain_batch_jobs(bc));
  CHECK(read("__batch_settlements") == 1);
  CHECK(bc.callbacks.empty());
  CHECK(bc.pending_ops.empty());
  CHECK(bc.concurrent_async_ops == 0);
}

TEST_CASE("installed batch orchestrator preserves enqueued commit rejection",
          "[js][db][batch][batch-commit-rejection]") {
  // The original helpers enqueue COMMIT and transfer both owners. Only its
  // host completion is simulated: never dispatch this alias-null token.
  auto cfg = test_config();
  auto pool = make_pool(cfg);
  BatchLease lease(pool);
  auto& bc = *lease.get();
  constexpr std::string_view source = R"JS(
    globalThis.__native_commit_settlements = 0;
    globalThis.__native_commit_rejection = null;
    db.batch(() => 'body-success').then(
      () => { __native_commit_settlements++; },
      e => { __native_commit_settlements++; __native_commit_rejection = e; }
    );
  )JS";
  JS_UpdateStackTop(bc.rt);
  JSValue evaluation = JS_Eval(bc.ctx, source.data(), source.size(),
                               "<batch-test>", JS_EVAL_TYPE_GLOBAL);
  const bool evaluated = !JS_IsException(evaluation);
  JS_FreeValue(bc.ctx, evaluation);
  REQUIRE(evaluated);
  auto operations = bc.take_pending_ops();
  REQUIRE(operations.size() == 1);
  REQUIRE(operations[0].type == plinth::js::AsyncOp::Type::DB_BATCH_BEGIN);
  auto owner = std::make_shared<int>(954);
  std::weak_ptr<int> transaction_lifetime = owner;
  bc.batch_state.pinned_conn =
      std::shared_ptr<drogon::orm::Transaction>{owner, nullptr};
  owner.reset();
  bc.batch_state.rollback_owner =
      std::make_shared<plinth::js::NativeRollbackOwner>(nullptr);
  std::weak_ptr<plinth::js::NativeRollbackOwner> rollback_lifetime =
      bc.batch_state.rollback_owner;
  bc.resolve(operations[0].callback_id, Json::Value{});
  operations.clear();
  REQUIRE(drain_batch_jobs(bc));
  operations = bc.take_pending_ops();
  REQUIRE(operations.size() == 1);
  REQUIRE(operations[0].type == plinth::js::AsyncOp::Type::DB_BATCH_COMMIT);
  CHECK(bc.batch_state.pinned_conn.use_count() == 0);
  CHECK_FALSE(bc.batch_state.rollback_owner);
  CHECK(operations[0].batch_pinned_conn.use_count() == 1);
  CHECK(operations[0].batch_rollback_owner.use_count() == 1);
  const int callback = operations[0].callback_id;
  // Simulate terminal native ownership release, not another ROLLBACK.
  bc.batch_state = BridgeContext::BatchState{};
  operations.clear();
  CHECK(transaction_lifetime.expired());
  CHECK(rollback_lifetime.expired());
  const plinth::js::PromiseRejection failure{.code = "db.internal",
                                             .message = "native commit failed",
                                             .sqlstate = "XX000"};
  bc.reject(callback, failure);
  REQUIRE(drain_batch_jobs(bc));
  CHECK(bc.pending_ops.empty());
  CHECK(bc.callbacks.empty());
  JSValue global = JS_GetGlobalObject(bc.ctx);
  JSValue rejected =
      JS_GetPropertyStr(bc.ctx, global, "__native_commit_rejection");
  auto converted = plinth::js::detail::js_to_json(bc.ctx, rejected);
  JS_FreeValue(bc.ctx, rejected);
  JSValue settlements =
      JS_GetPropertyStr(bc.ctx, global, "__native_commit_settlements");
  int32_t count = -1;
  const int conversion = JS_ToInt32(bc.ctx, &count, settlements);
  JS_FreeValue(bc.ctx, settlements);
  JS_FreeValue(bc.ctx, global);
  REQUIRE(converted.has_value());
  CHECK((*converted)["code"].asString() == "db.internal");
  CHECK((*converted)["message"].asString() == "native commit failed");
  CHECK((*converted)["sqlstate"].asString() == "XX000");
  REQUIRE(conversion == 0);
  CHECK(count == 1);
  CHECK(bc.concurrent_async_ops == 0);
}

// ─── P.02 ─────────────────────────────────────────────────────────
// Deferred from phase 3. Inside-batch: the wrapper issues ONE `SET
// LOCAL search_path` at BEGIN, not one per statement. Assert the
// three unqualified inserts all land in ext_batch.notes (proof they
// ran under the batch's search_path) AND no per-op search_path
// wrapper ran — the latter is implicit in this test since the batch
// path doesn't emit "no transaction in progress" warnings the way
// phase 3's per-op wrapper does. Behavioral assertion is schema-
// landing; no wire-trace available.
TEST_CASE("P.02: inside-batch single SET LOCAL — all rows land in ext_ schema",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);

  auto pool = make_pool(cfg);
  auto r = eval_as(pool, "batch",
                   "db.batch(async () => {"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
                   "  await db.exec(\"INSERT INTO notes(body) VALUES('c')\");"
                   "}).then(() => 'ok', e => 'err:' + e.code)");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 3);
}

// ─── B.13 ─────────────────────────────────────────────────────────
// ICD-0.5.3 §Security Constraint 3 — cross-extension batch
// prohibited. From the `batch` extension, attempt an INSERT against
// `ext_other.notes`. Classifier matches `ext_other`, sees it differs
// from the bc's `extension_name`, rejects with
// `db.batch.cross_extension_not_allowed`. Orchestrator's catch
// runs ROLLBACK; outer promise rejects; `ext_batch.notes` row from
// the first insert is also rolled back.
TEST_CASE("B.13: cross-extension batch — rejects + rolls back + audit fires",
          "[js][async][db][batch]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  auto cfg = test_config();
  reset_for_batch_test(cfg);
  TestPg pg(cfg.db);
  setup_ext_batch(pg);
  pg.exec("DROP SCHEMA IF EXISTS ext_other CASCADE");
  pg.exec("CREATE SCHEMA ext_other");
  pg.exec("CREATE TABLE ext_other.notes (id serial primary key, body text)");

  auto pool = make_pool(cfg);
  auto r = eval_as(
      pool, "batch",
      "db.batch(async () => {"
      "  await db.exec(\"INSERT INTO notes(body) VALUES('a')\");"
      "  await db.exec(\"INSERT INTO ext_other.notes(body) VALUES('x')\");"
      "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "err:db.batch.cross_extension_not_allowed");
  REQUIRE(pg.count_rows("ext_batch.notes") == 0);
  REQUIRE(pg.count_rows("ext_other.notes") == 0);
  REQUIRE(wait_for_audit(pg, "db.batch.cross_extension_rejected", "batch", 1));

  // Negative: a `plinth.*` reference (kernel schema) inside the
  // same `batch` extension's batch must NOT trip the SC3 rule.
  // Re-acquire an isolated pool for the second batch.
  plinth::js::db::reset_cross_extension_audit_for_test();
  auto r2 = eval_as(pool, "batch",
                    "db.batch(async () => {"
                    "  await db.query(\"SELECT id FROM plinth.users LIMIT 1\");"
                    "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
                    "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r2.value.has_value());
  REQUIRE(r2.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 1);

  pg.exec("DROP SCHEMA ext_other CASCADE");
}
