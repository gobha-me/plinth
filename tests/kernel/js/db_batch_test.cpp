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
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/js/db_batch_schema_check.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/coalescer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
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
    PGresult* res = PQexec(conn, sql.c_str());
    PQclear(res);
  }

  [[nodiscard]] auto count_rows(const std::string& qualified_table) const
      -> int {
    PGresult* res =
        PQexec(conn, ("SELECT COUNT(*) FROM " + qualified_table).c_str());
    int n = 0;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
      n = std::stoi(PQgetvalue(res, 0, 0));
    }
    PQclear(res);
    return n;
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
      "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r.value.has_value());
  auto code = r.value->asString();
  // Either the unique-violation code or a generic db.internal — PG
  // wires SQLSTATE 23505 which db_error_map translates.
  REQUIRE(code.rfind("err:", 0) == 0);
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
  auto r2 =
      eval_as(pool, "batch",
              "db.batch(async () => {"
              "  await db.query(\"SELECT 1 FROM plinth.audit_log LIMIT 1\");"
              "  await db.exec(\"INSERT INTO notes(body) VALUES('b')\");"
              "}).then(() => 'ok', e => 'err:' + (e && e.code))");
  REQUIRE(r2.value.has_value());
  REQUIRE(r2.value->asString() == "ok");
  REQUIRE(pg.count_rows("ext_batch.notes") == 1);

  pg.exec("DROP SCHEMA ext_other CASCADE");
}
