// SPDX-License-Identifier: MIT

#include "async_bridge_fixture.hpp"

#include "kernel/js/db_batch_audit.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/extension_database.hpp"
#include "kernel/js/native_setup_failure.hpp"
#include "kernel/js/owned_database_awaiters.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/realtime/coalescer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/drogon.h>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace {

using plinth::js::NativeSetupFailurePoint;

class SetupPg {
 public:
  explicit SetupPg(const plinth::Config::Database& db) {
    const auto conninfo = "host=" + db.host +
                          " port=" + std::to_string(db.port) +
                          " dbname=" + db.database + " user=" + db.user +
                          " password=" + db.password + " connect_timeout=3";
    connection.reset(PQconnectdb(conninfo.c_str()));
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  }

  auto exec(const char* sql) -> void {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }

  auto scalar(const char* sql) -> int {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    REQUIRE(PQnfields(result.get()) == 1);
    return std::stoi(PQgetvalue(result.get(), 0, 0));
  }

 private:
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
};

class SetupFailureScope {
 public:
  explicit SetupFailureScope(NativeSetupFailurePoint point) {
    plinth::js::set_native_setup_failure_for_test(point);
  }
  ~SetupFailureScope() { plinth::js::clear_native_setup_failure_for_test(); }
};

class CheckedOutContext {
 public:
  explicit CheckedOutContext(plinth::js::RuntimePool& pool_in)
      : pool(pool_in), context(pool.acquire()) {
    REQUIRE(context != nullptr);
  }
  ~CheckedOutContext() {
    if (context != nullptr) {
      pool.destroy(context);
    }
  }
  auto get() -> plinth::js::BridgeContext& { return *context; }
  auto destroy() -> void { pool.destroy(std::exchange(context, nullptr)); }

 private:
  plinth::js::RuntimePool& pool;
  plinth::js::BridgeContext* context;
};

class EmitScope {
 public:
  explicit EmitScope(std::atomic<std::size_t>& emitted) {
    plinth::realtime::CoalescerRegistry::instance().set_emit_hook_for_test(
        [&emitted](const Json::Value&)
            -> std::expected<void, plinth::realtime::NotifyError> {
          emitted.fetch_add(1, std::memory_order_relaxed);
          return {};
        });
  }
  ~EmitScope() {
    plinth::realtime::CoalescerRegistry::instance().clear_emit_hook_for_test();
  }
};

auto drive(plinth::js::BridgeContext& context, std::string_view source)
    -> plinth::js::EvalResult {
  return drogon::sync_wait(plinth::js::run_on_context(context, source));
}

auto acquire_owned_transaction(
    std::shared_ptr<drogon::orm::DbClient> client,
    std::shared_ptr<plinth::lifecycle::AsyncTaskLease> accepted,
    std::shared_ptr<plinth::js::NativeRollbackOwner> owner,
    std::shared_ptr<plinth::js::ExtensionDatabaseClients> domain)
    -> drogon::Task<std::shared_ptr<drogon::orm::Transaction>> {
  co_return co_await plinth::js::OwnedTransactionAwaiter{
      std::move(client), std::move(accepted), std::move(owner),
      std::move(domain)};
}

constexpr const char* BATCH_INSERT = R"JS(
  db.batch(async () => {
    await db.exec("INSERT INTO notes(body) VALUES($1)", ["setup"]);
    return "ok";
  }).then(value => ({value}), error => ({code: error.code, message: error.message}))
)JS";

constexpr const char* WRAPPER_INSERT = R"JS(
  db.exec("INSERT INTO notes(body) VALUES($1)", ["setup"])
    .then(() => ({value: "ok"}), error => ({code: error.code, message: error.message}))
)JS";

constexpr const char* COMMITTED_AUDIT =
    "SELECT COUNT(*) FROM plinth.audit_log WHERE action = 'db.batch.committed' "
    "AND detail->>'extension' = 'setupfault'";

auto audit_reaches(SetupPg& pg, int count) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pg.scalar(COMMITTED_AUDIT) >= count) {
      return true;
    }
    std::this_thread::yield();
  }
  return pg.scalar(COMMITTED_AUDIT) >= count;
}

auto setup_fault_case(NativeSetupFailurePoint point, bool wrapper = false,
                      bool commit_acknowledged = false) -> void {
  REQUIRE(plinth::async_bridge_test::pg_available());
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  auto cfg = plinth::async_bridge_test::test_config();
  plinth::async_bridge_test::reset_schema(cfg.db);
  plinth::js::db::set_search_path_enforce(true);
  plinth::js::set_batch_timeout_ms(30000);
  plinth::js::reset_batch_audit_for_test();
  plinth::realtime::CoalescerRegistry::instance().clear_windows_for_test();
  SetupPg pg(cfg.db);
  pg.exec("DROP SCHEMA IF EXISTS ext_setupfault CASCADE");
  pg.exec("CREATE SCHEMA ext_setupfault");
  pg.exec("CREATE TABLE ext_setupfault.notes (body text)");
  plinth::js::RuntimePool pool{nullptr, plinth::js::default_runtime_limits(),
                               cfg,     1,
                               nullptr, "setupfault"};
  std::atomic<std::size_t> emitted{0};
  EmitScope emit_scope{emitted};
  const auto previous_audit = pg.scalar(COMMITTED_AUDIT);

  // Prove the same real extension/database path commits a durable row before
  // injecting anything. A connection/search-path failure cannot stand in for
  // the intended pre-COMMIT setup failure.
  {
    CheckedOutContext healthy{pool};
    const auto control = drive(healthy.get(), BATCH_INSERT);
    healthy.destroy();
    REQUIRE(control.value.has_value());
    REQUIRE((*control.value)["value"].asString() == "ok");
  }
  REQUIRE(plinth::lifecycle::async_tasks().drain(5s));
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM ext_setupfault.notes") == 1);
  REQUIRE(emitted.load(std::memory_order_relaxed) == 1);
  REQUIRE(audit_reaches(pg, previous_audit + 1));
  const auto before_audit = pg.scalar(COMMITTED_AUDIT);
  pg.exec("TRUNCATE ext_setupfault.notes");
  plinth::js::reset_batch_audit_for_test();
  emitted.store(0, std::memory_order_relaxed);

  std::optional<plinth::js::EvalResult> outcome;
  bool retired = false;
  int concurrent = -1;
  int detached = -1;
  bool pin_released = false;
  {
    CheckedOutContext context{pool};
    SetupFailureScope fault{point};
    outcome = drive(context.get(), wrapper ? WRAPPER_INSERT : BATCH_INSERT);
    retired = context.get().execution_lifetime.load(
                  std::memory_order_acquire) == nullptr;
    concurrent = context.get().concurrent_async_ops;
    detached = context.get().inflight_detached.load(std::memory_order_acquire);
    pin_released = !context.get().batch_state.pinned_conn;
    context.destroy();
  }
  REQUIRE(plinth::lifecycle::async_tasks().drain(5s));
  REQUIRE(plinth::js::native_setup_failure_observed_for_test() == point);
  CHECK(retired);
  CHECK(concurrent == 0);
  CHECK(detached == 0);
  CHECK(pin_released);
  CHECK(plinth::lifecycle::async_tasks().active_for_test() == 0);
  CHECK(plinth::js::discard_batches_for_extension("setupfault") == 0);
  CHECK(plinth::realtime::CoalescerRegistry::instance()
            .open_window_count_for_test() == 0);
  CHECK(plinth::realtime::CoalescerRegistry::instance()
            .batch_scope_count_for_test() == 0);
  CHECK(pg.scalar("SELECT COUNT(*) FROM ext_setupfault.notes") ==
        (commit_acknowledged ? 1 : 0));
  CHECK(emitted.load(std::memory_order_relaxed) ==
        (commit_acknowledged ? 1 : 0));

  const bool host_failure =
      point == NativeSetupFailurePoint::dispatcher_commit_task ||
      point == NativeSetupFailurePoint::launcher_commit_task;
  REQUIRE(outcome.has_value());
  if (host_failure) {
    REQUIRE_FALSE(outcome->value.has_value());
    CHECK(outcome->value.error().kind ==
          plinth::js::EvalErrorKind::RUNTIME_ERROR);
    CHECK(outcome->value.error().message == std::bad_alloc{}.what());
  } else {
    REQUIRE(outcome->value.has_value());
    CHECK((*outcome->value)["code"].asString() == "db.internal");
    CHECK((*outcome->value)["message"].asString() == std::bad_alloc{}.what());
  }
  if (commit_acknowledged) {
    REQUIRE(audit_reaches(pg, before_audit + 1));
  } else {
    CHECK(pg.scalar(COMMITTED_AUDIT) == before_audit);
  }

  // The same private DB domain remains usable; per-execution failure must not
  // initiate shared-client shutdown or leave a poisoned transaction behind.
  plinth::js::reset_batch_audit_for_test();
  const auto before_recovery_audit = pg.scalar(COMMITTED_AUDIT);
  {
    CheckedOutContext recovery{pool};
    const auto result = drive(recovery.get(), BATCH_INSERT);
    recovery.destroy();
    REQUIRE(result.value.has_value());
    CHECK((*result.value)["value"].asString() == "ok");
  }
  REQUIRE(plinth::lifecycle::async_tasks().drain(5s));
  CHECK(pg.scalar("SELECT COUNT(*) FROM ext_setupfault.notes") ==
        (commit_acknowledged ? 2 : 1));
  REQUIRE(audit_reaches(pg, before_recovery_audit + 1));
  CHECK(pool.shutdown(5s));
}

struct PrivateCallbackGate {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<bool> entered_once{false};
  std::atomic<bool> released_once{false};
  std::atomic<bool> succeeded{false};

  auto hold(bool success) -> void {
    succeeded.store(success, std::memory_order_release);
    if (!entered_once.exchange(true, std::memory_order_acq_rel)) {
      entered.set_value();
    }
    released.wait();
  }
  auto unblock() -> void {
    if (!released_once.exchange(true, std::memory_order_acq_rel)) {
      release.set_value();
    }
  }
};

class UnblockPrivateCallbackOnUnwind {
 public:
  explicit UnblockPrivateCallbackOnUnwind(
      std::shared_ptr<PrivateCallbackGate> gate_in)
      : gate(std::move(gate_in)) {}
  ~UnblockPrivateCallbackOnUnwind() {
    gate->unblock();
    static_cast<void>(plinth::lifecycle::async_tasks().drain(5s));
  }

 private:
  std::shared_ptr<PrivateCallbackGate> gate;
};

} // namespace

TEST_CASE("batch dispatcher Task setup failure rolls back completed writes",
          "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::dispatcher_commit_task);
}
TEST_CASE("batch launcher Task setup failure rolls back completed writes",
          "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::launcher_commit_task);
}
TEST_CASE("batch child Task setup failure rolls back completed writes",
          "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::batch_commit_task);
}
TEST_CASE(
    "batch commit completion allocation failure rolls back completed writes",
    "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::commit_completion_allocation);
}
TEST_CASE(
    "batch commit callback installation failure rolls back completed writes",
    "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::commit_callback_install);
}
TEST_CASE("extension wrapper commit Task failure rolls back completed writes",
          "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::wrapper_commit_task, true);
}
TEST_CASE("batch BEGIN metadata allocation failure retires its transaction",
          "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::batch_begin_metadata);
}
TEST_CASE("batch BEGIN queue allocation failure retires its transaction",
          "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::batch_begin_queue);
}
TEST_CASE(
    "batch post-ACK metadata failure preserves durable commit bookkeeping",
    "[js][async][db][native-setup-failure]") {
  setup_fault_case(NativeSetupFailurePoint::batch_commit_metadata, false, true);
}

TEST_CASE("private database domain failed close is retained until actual retry",
          "[js][async][db][native-setup-failure][lifecycle]") {
  REQUIRE(plinth::async_bridge_test::pg_available());
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  auto cfg = plinth::async_bridge_test::test_config();
  cfg.db.pool_size = 1;
  plinth::async_bridge_test::reset_schema(cfg.db);
  auto domain = std::make_shared<plinth::js::ExtensionDatabaseClients>(cfg.db);
  std::weak_ptr<plinth::js::ExtensionDatabaseClients> weak = domain;
  auto& registry = plinth::lifecycle::async_tasks();
  REQUIRE(registry.drain(5s));
  auto accepted = registry.try_acquire();
  REQUIRE(accepted != nullptr);
  auto client = drogon::sync_wait(domain->get("faultdomain", accepted));
  REQUIRE(client != nullptr);
  auto owner = std::make_shared<plinth::js::NativeRollbackOwner>(accepted);
  std::weak_ptr<plinth::js::NativeRollbackOwner> weak_owner = owner;
  auto transaction = drogon::sync_wait(
      acquire_owned_transaction(client, accepted, owner, domain));
  REQUIRE(transaction != nullptr);
  owner->attach(transaction);
  auto gate = std::make_shared<PrivateCallbackGate>();
  auto entered = gate->entered.get_future();
  UnblockPrivateCallbackOnUnwind cleanup{gate};
  transaction->execSqlAsync(
      "SELECT 1 /* owned domain cleanup retry */",
      [gate, accepted](const drogon::orm::Result&) {
        static_cast<void>(accepted);
        gate->hold(true);
      },
      [gate, accepted](const std::exception_ptr&) {
        static_cast<void>(accepted);
        gate->hold(false);
      });
  accepted.reset();
  REQUIRE(entered.wait_for(2s) == std::future_status::ready);
  REQUIRE(gate->succeeded.load(std::memory_order_acquire));

  // This is an actual private client's occupied native event-loop callback,
  // not a fake closeAllFor(false). Its finite lifecycle close cannot join yet.
  const bool initially_closed = domain->shutdown(25ms);
  // The domain is ALREADY stopping: cancellation cannot initiate another
  // close or claim that this request was accepted by a sealed private client.
  // This actual native cleanup record is inserted after the domain record,
  // so the dependent is retried FIRST by the registry's intrusive LIFO list.
  const bool rollback_requested = owner->request(transaction, domain);
  const bool rollback_failed = owner->has_failure();
  transaction.reset();
  owner.reset();
  domain.reset();
  const bool retained = !weak.expired();
  const bool native_retained = !weak_owner.expired();
  const bool blocked_drain = registry.drain(25ms);
  // Failed records are reinserted at the head, reversing the first pass.
  // A second real failed pass while the callback is STILL held restores the
  // native-dependent-first order for the succeeding explicit retry. That
  // pass must retry the native record again immediately after domain progress,
  // rather than waiting for a callback from an already-stopped private loop.
  const bool second_blocked_drain = registry.drain(25ms);
  const auto active_while_blocked = registry.active_for_test();
  gate->unblock();
  const bool retried = registry.drain(5s);
  const bool owner_released = weak.expired();
  const bool native_released = weak_owner.expired();

  CHECK_FALSE(initially_closed);
  CHECK_FALSE(rollback_requested);
  CHECK(rollback_failed);
  CHECK(retained);
  CHECK(native_retained);
  CHECK_FALSE(blocked_drain);
  CHECK_FALSE(second_blocked_drain);
  CHECK(active_while_blocked == 1);
  CHECK(retried);
  CHECK(owner_released);
  CHECK(native_released);
  CHECK(registry.active_for_test() == 0);
  // The owned domain already verified closeAllFor; this idempotent actual
  // client check is not used as a replacement for the registry retry above.
  CHECK(client->closeAllFor(0ms));
  client.reset();
  const auto kernel = drogon::app().getDbClient();
  REQUIRE(kernel != nullptr);
  const auto result = kernel->execSqlSync("SELECT 42 AS answer");
  REQUIRE(result.size() == 1);
  CHECK(result[0]["answer"].as<int>() == 42);
}
