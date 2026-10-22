// SPDX-License-Identifier: MIT

#include "async_bridge_fixture.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/js/execution_lifetime.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Generation = std::weak_ptr<plinth::js::ExecutionLifetime>;

auto demand(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

auto same_generation(const Generation& left, const Generation& right) -> bool {
  return !left.owner_before(right) && !right.owner_before(left);
}

class Control {
 public:
  explicit Control(const plinth::Config::Database& db) {
    const auto info = plinth::db::connection_info(db) +
                      " connect_timeout=3 options='-c statement_timeout=3000 "
                      "-c lock_timeout=2500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand(connection && PQstatus(connection.get()) == CONNECTION_OK,
           "capability control database unavailable");
  }
  auto query(const std::string& sql)
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    demand(result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK ||
                      PQresultStatus(result.get()) == PGRES_COMMAND_OK),
           "capability fixture SQL failed");
    return result;
  }
  auto text(const std::string& sql) -> std::string {
    const auto result = query(sql);
    demand(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1 &&
               PQgetisnull(result.get(), 0, 0) == 0,
           "capability control expected exactly one scalar");
    return PQgetvalue(result.get(), 0, 0);
  }

 private:
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
};

class SqlGate {
 public:
  SqlGate(Control& control_in, unsigned key_in)
      : key(key_in), control(control_in) {
    const auto acknowledged = control.query("SELECT pg_advisory_lock(53187," +
                                            std::to_string(key) + ")");
    demand(PQntuples(acknowledged.get()) == 1,
           "capability advisory lock acknowledgement missing");
    locked = true;
  }
  ~SqlGate() {
    if (locked) {
      try {
        if (!unlock()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  SqlGate(const SqlGate&) = delete;
  auto operator=(const SqlGate&) -> SqlGate& = delete;
  auto unlock() -> bool {
    if (!locked) return true;
    const bool acknowledged = control.text("SELECT pg_advisory_unlock(53187," +
                                           std::to_string(key) + ")") == "t";
    if (acknowledged) locked = false;
    return acknowledged;
  }
  auto waiter(const std::string& role, const std::string& pid) -> bool {
    return control.text(
               "SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a "
               "ON a.pid=l.pid WHERE l.locktype='advisory' "
               "AND l.classid=53187 AND l.objid=" +
               std::to_string(key) +
               " AND l.objsubid=2 AND NOT l.granted AND a.usename='" + role +
               "' AND a.pid=" + pid + " AND a.state='active'") == "1";
  }
  unsigned key;

 private:
  Control& control;
  bool locked = false;
};

struct Snapshot {
  bool visited = false;
  bool cancelled = false;
  std::size_t callbacks = 0;
  int inflight = 0;
};

// Never publish completion while a strong generation still holds a native
// task lease: that would contaminate the ownership assertion being measured.
auto snapshot(Generation generation) -> Snapshot {
  auto promise = std::make_shared<std::promise<Snapshot>>();
  auto ready = promise->get_future();
  drogon::app().getLoop()->queueInLoop([generation, promise] {
    Snapshot result;
    if (auto lifetime = generation.lock()) {
      result.visited = lifetime->visit([&result](const auto& context) {
        result.cancelled = context.cancelled.load(std::memory_order_acquire);
        result.callbacks = context.callbacks.size();
        result.inflight =
            context.inflight_detached.load(std::memory_order_acquire);
      });
    }
    promise->set_value(result);
  });
  demand(ready.wait_for(2s) == std::future_status::ready,
         "capability observer owner-loop deadline");
  return ready.get();
}

struct Observations {
  std::mutex mutex;
  std::vector<plinth::js::ExecutionObservation> executions;
  std::vector<plinth::extensions::DispatchResultObservation> results;

  auto entered(const std::string& extension) -> Generation {
    std::lock_guard lock(mutex);
    for (const auto& record : executions) {
      if (record.stage == plinth::js::ExecutionObservationStage::ENTER &&
          record.extension_name == extension)
        return record.execution;
    }
    return {};
  }
  auto finished(const Generation& generation)
      -> std::optional<plinth::js::EvalErrorKind> {
    std::lock_guard lock(mutex);
    for (const auto& record : executions) {
      if (record.stage == plinth::js::ExecutionObservationStage::FINISH &&
          same_generation(record.execution, generation))
        return record.error_kind;
    }
    return std::nullopt;
  }
};

class Observe {
 public:
  Observe() : state(std::make_shared<Observations>()) {
    plinth::js::set_execution_observer_for_test(
        [owned = state](const auto& record) {
          std::lock_guard lock(owned->mutex);
          owned->executions.push_back(record);
        });
    plinth::extensions::set_dispatch_result_observer_for_test(
        [owned = state](const auto& record) {
          std::lock_guard lock(owned->mutex);
          owned->results.push_back(record);
        });
  }
  ~Observe() {
    plinth::js::set_execution_observer_for_test({});
    plinth::extensions::set_dispatch_result_observer_for_test({});
    // Every actual driver has joined before observer removal. A queued fence
    // drains already copied callbacks; captures also independently own state.
    try {
      static_cast<void>(snapshot({}));
    } catch (...) {
      std::terminate();
    }
  }
  Observe(const Observe&) = delete;
  auto operator=(const Observe&) -> Observe& = delete;
  std::shared_ptr<Observations> state;
};

class Provider {
 public:
  Provider() : config(plinth::async_bridge_test::test_config()) {
    static std::atomic<unsigned> sequence{0};
    config.db.pool_size = 1;
    base = std::filesystem::temp_directory_path() /
           ("plinth_cap_native_" + std::to_string(getpid()) + "_" +
            std::to_string(sequence.fetch_add(1)));
    demand(std::filesystem::create_directory(base),
           "capability fixture scratch path already exists");
    try {
      config.packages_data_dir = (base / "data").string();
      const auto directory =
          base / "data/extensions/capnative/1.0.0/server/handlers";
      std::filesystem::create_directories(directory);
      {
        std::ofstream main_file(directory.parent_path() / "main.js");
        main_file << "// owned native capability fixture\n";
        demand(main_file.good(), "capability fixture main write failed");
      }
      std::filesystem::create_directory_symlink(
          "1.0.0", base / "data/extensions/capnative/active");
      plinth::extensions::init_registry(config);
      demand(plinth::extensions::create_pool("capnative"),
             "capability fixture provider pool unavailable");
      plinth::capabilities::CachedCapability entry{
          .signature = "capnative:1:held",
          .provider_type = "extension",
          .extension_name = "capnative",
          .scope = "instance",
          .user_id = {},
          .rbac_rule = "capnative.call",
          .enabled = true};
      plinth::capabilities::seed_tier2_cache_for_test(entry);
      // Pool construction does not provision a database client. Use the same
      // trusted host provisioning operation as installation before any fixture
      // DDL; actual handlers still authenticate with their restricted login.
      Control control(config.db);
      static_cast<void>(control.query(
          "SELECT plinth.provision_extension_database('capnative')"));
      role = control.text(
          "SELECT role_name FROM plinth.extension_database_credentials "
          "WHERE extension_name='capnative'");
      demand(role.size() == 63 && role.starts_with("px_") &&
                 role != config.db.user,
             "capability provider restricted identity malformed");
      demand(control.text(
                 "SELECT to_regclass('ext_capnative.cap_receipt') IS NULL") ==
                 "t",
             "capability receipt relation already exists");
      static_cast<void>(control.query(
          "CREATE TABLE ext_capnative.cap_receipt (marker integer PRIMARY "
          "KEY,pid integer NOT NULL,login text NOT NULL,session_login text "
          "NOT NULL)"));
      relation_owned = true;
      static_cast<void>(control.query(
          "ALTER TABLE ext_capnative.cap_receipt OWNER TO \"" + role + "\""));
    } catch (...) {
      cleanup();
      throw;
    }
  }
  ~Provider() { cleanup(); }
  Provider(const Provider&) = delete;
  auto operator=(const Provider&) -> Provider& = delete;
  auto stage(const std::string& source) const -> void {
    std::ofstream file(
        base / "data/extensions/capnative/1.0.0/server/handlers/held.js");
    file << source;
    demand(file.good(), "capability handler source write failed");
  }
  plinth::Config config;
  std::string role;

 private:
  auto cleanup() noexcept -> void {
    try {
      if (!plinth::extensions::shutdown_registry(5s)) std::terminate();
      plinth::capabilities::clear_resolver_for_test();
      if (relation_owned) {
        Control control(config.db);
        static_cast<void>(
            control.query("DROP TABLE ext_capnative.cap_receipt"));
        relation_owned = false;
        demand(control.text(
                   "SELECT to_regclass('ext_capnative.cap_receipt') IS NULL") ==
                   "t",
               "capability receipt relation cleanup not acknowledged");
      }
      std::filesystem::remove_all(base);
    } catch (...) {
      std::terminate();
    }
  }
  std::filesystem::path base;
  bool relation_owned = false;
};

struct NestedObservation {
  bool waiter = false;
  bool caller_active = false;
  bool callee_active = false;
  bool distinct = false;
  bool requested = false;
  bool propagated = false;
  bool still_blocked = false;
  bool unlocked = false;
  bool caller_cancelled = false;
  bool callee_cancelled = false;
  bool actual_rejection = false;
  bool exact_records = false;
  std::string target_outcome;
  bool native_drained = false;
  bool audits_drained = false;
  bool caller_closed = false;
};

auto nested_cell(Provider& provider, unsigned ordinal, bool batch)
    -> NestedObservation {
  Control control(provider.config.db);
  const auto& role = provider.role;
  const auto key = static_cast<unsigned>(getpid()) * 16U + ordinal + 1U;
  SqlGate gate(control, key);
  provider.stage(
      "export default async function held() { await db.exec("
      "'INSERT INTO ext_capnative.cap_receipt VALUES (" +
      std::to_string(ordinal) +
      ",pg_backend_pid(),current_user,session_user)',[],{silent:true});"
      "await db.query('SELECT pg_advisory_xact_lock($1::int8::int4,"
      "$2::int8::int4)',[53187," +
      std::to_string(key) + "]); return 42; }");
  Observe observe;
  const plinth::capabilities::UserContext user{
      .user_id = "11111111-1111-1111-1111-111111111187",
      .username = "native-capability-fixture",
      .auth_type = "session",
      .effective_rules = {"capnative.call"},
      .session_id = {},
      .ip_address = {}};
  auto limits = plinth::js::default_runtime_limits();
  limits.wall_clock_limit = 20s;
  plinth::js::RuntimePool pool(nullptr, limits, provider.config, 1, &user);
  auto* caller = pool.acquire();
  demand(caller != nullptr, "capability caller context unavailable");
  struct CallerLease {
    plinth::js::RuntimePool& pool;
    plinth::js::BridgeContext* context;
    ~CallerLease() {
      if (context != nullptr) pool.destroy(context);
    }
  } caller_lease{pool, caller};
  const std::string source = batch ? "cap.batch([['capnative:1:held',null]])"
                                   : "cap.call('capnative:1:held',null)";
  std::promise<plinth::js::EvalResult> promise;
  auto done = promise.get_future();
  std::jthread worker([caller, &promise, &source] {
    try {
      promise.set_value(
          drogon::sync_wait(plinth::js::run_on_context(*caller, source)));
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
  });
  struct UnlockBeforeJoin {
    SqlGate& gate;
    ~UnlockBeforeJoin() {
      try {
        if (!gate.unlock()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  } unlock_before_join{gate};
  NestedObservation result;
  std::string pid;
  const auto admission_deadline = Clock::now() + 2s;
  do {
    const auto receipt = control.query(
        "SELECT pid FROM ext_capnative.cap_receipt WHERE marker=" +
        std::to_string(ordinal) + " AND login='" + role +
        "' AND session_login=login");
    if (PQntuples(receipt.get()) == 1) pid = PQgetvalue(receipt.get(), 0, 0);
    result.waiter = !pid.empty() && gate.waiter(role, pid);
    if (!result.waiter) std::this_thread::yield();
  } while (!result.waiter && Clock::now() < admission_deadline);
  const auto caller_generation = observe.state->entered("");
  const auto callee_generation = observe.state->entered("capnative");
  const auto caller_snapshot = snapshot(caller_generation);
  const auto callee_snapshot = snapshot(callee_generation);
  result.caller_active = caller_snapshot.visited && !caller_snapshot.cancelled;
  result.callee_active = callee_snapshot.visited && !callee_snapshot.cancelled;
  result.distinct = !same_generation(caller_generation, callee_generation);
  if (result.waiter && result.caller_active && result.callee_active) {
    if (auto generation = caller_generation.lock())
      result.requested = generation->request_cancel();
    const auto propagation_deadline = Clock::now() + 1500ms;
    do {
      const auto state = snapshot(callee_generation);
      result.propagated = state.visited && state.cancelled;
      if (!result.propagated) std::this_thread::yield();
    } while (!result.propagated && Clock::now() < propagation_deadline);
    result.still_blocked = gate.waiter(role, pid);
  }
  result.unlocked = gate.unlock();
  if (done.wait_for(8s) != std::future_status::ready) std::terminate();
  worker.join();
  const auto actual = done.get();
  result.caller_cancelled =
      !actual.value.has_value() &&
      actual.value.error().kind == plinth::js::EvalErrorKind::CANCELLED;
  result.callee_cancelled = observe.state->finished(callee_generation) ==
                            plinth::js::EvalErrorKind::CANCELLED;
  {
    std::lock_guard lock(observe.state->mutex);
    unsigned caller_enters = 0, caller_finishes = 0;
    unsigned callee_enters = 0, callee_finishes = 0;
    for (const auto& record : observe.state->executions) {
      const bool enter =
          record.stage == plinth::js::ExecutionObservationStage::ENTER;
      if (same_generation(record.execution, caller_generation)) {
        caller_enters += enter ? 1U : 0U;
        caller_finishes += enter ? 0U : 1U;
      }
      if (same_generation(record.execution, callee_generation)) {
        callee_enters += enter ? 1U : 0U;
        callee_finishes += enter ? 0U : 1U;
      }
    }
    result.exact_records = observe.state->executions.size() == 4 &&
                           caller_enters == 1 && caller_finishes == 1 &&
                           callee_enters == 1 && callee_finishes == 1 &&
                           observe.state->results.size() == 1;
    if (observe.state->results.size() == 1) {
      const auto& record = observe.state->results.front();
      result.actual_rejection = record.extension_name == "capnative" &&
                                record.function == "held" &&
                                record.call_depth == 1 && record.rejection &&
                                record.rejection->code == "cap.cancelled";
      result.target_outcome =
          record.rejection ? record.rejection->code : "success";
    }
  }
  pool.destroy(caller);
  caller_lease.context = nullptr;
  result.caller_closed = pool.shutdown(3s) && pool.shutdown(0ms) &&
                         pool.active_count() == 0 && pool.free_count() == 0;
  result.native_drained = plinth::lifecycle::async_tasks().drain(3s);
  result.audits_drained = plinth::log::wait_for_audits(3s);
  return result;
}

} // namespace

TEST_CASE("Native capability cancellation reaches the owned callee generation",
          "[js][async][cap][native-capability][nested-cancellation]") {
  REQUIRE(plinth::async_bridge_test::pg_available());
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  const auto config = plinth::async_bridge_test::test_config();
  plinth::async_bridge_test::reset_schema(config.db);
  std::vector<NestedObservation> observations;
  for (unsigned replay = 0; replay < 2; ++replay) {
    Provider provider;
    for (unsigned route = 0; route < 2; ++route)
      observations.push_back(
          nested_cell(provider, replay * 2 + route, route != 0));
  }
  // All native workers and both provider owners have cleaned up before an
  // oracle assertion can abort. Neither caller retirement nor a direct child
  // cancellation request can substitute for the separately observed flag.
  REQUIRE(observations.size() == 4);
  for (const auto& state : observations) {
    INFO("waiter=" << state.waiter << " propagated=" << state.propagated
                   << " blocked=" << state.still_blocked
                   << " callerCancelled=" << state.caller_cancelled
                   << " calleeCancelled=" << state.callee_cancelled
                   << " target=" << state.target_outcome
                   << " exactRecords=" << state.exact_records
                   << " nativeDrained=" << state.native_drained
                   << " auditDrained=" << state.audits_drained
                   << " callerClosed=" << state.caller_closed);
    REQUIRE(state.waiter);
    REQUIRE(state.caller_active);
    REQUIRE(state.callee_active);
    REQUIRE(state.distinct);
    REQUIRE(state.requested);
    REQUIRE(state.propagated);
    REQUIRE(state.still_blocked);
    REQUIRE(state.unlocked);
    REQUIRE(state.caller_cancelled);
    REQUIRE(state.callee_cancelled);
    REQUIRE(state.actual_rejection);
    REQUIRE(state.exact_records);
    REQUIRE(state.native_drained);
    REQUIRE(state.audits_drained);
    REQUIRE(state.caller_closed);
  }
}
