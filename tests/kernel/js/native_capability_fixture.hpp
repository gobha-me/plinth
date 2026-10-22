// SPDX-License-Identifier: MIT
#pragma once

#include "async_bridge_fixture.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/extension_identity.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/migrations.hpp"

#include <drogon/utils/coroutine.h>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace native_capability_corpus::fixture {

using namespace std::chrono_literals;

inline auto demand(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

class Control {
 public:
  explicit Control(const plinth::Config::Database& database) {
    const auto info = plinth::db::connection_info(database) +
                      " connect_timeout=3 options='-c statement_timeout=3000 "
                      "-c lock_timeout=2500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand(connection && PQstatus(connection.get()) == CONNECTION_OK,
           "native capability control database unavailable");
  }
  auto query(const std::string& sql)
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    ++operations;
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    demand(result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK ||
                      PQresultStatus(result.get()) == PGRES_COMMAND_OK),
           "native capability control SQL failed");
    return result;
  }
  auto scalar(const std::string& sql) -> std::string {
    const auto result = query(sql);
    demand(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1 &&
               PQgetisnull(result.get(), 0, 0) == 0,
           "native capability control scalar cardinality");
    return PQgetvalue(result.get(), 0, 0);
  }
  auto raw() -> PGconn& { return *connection; }
  unsigned operations = 0;

 private:
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
};

inline auto user() -> plinth::capabilities::UserContext {
  return {.user_id = "11111111-1111-1111-1111-111111111187",
          .username = "native-capability-fixture",
          .auth_type = "session",
          .effective_rules = {"capnative.call"},
          .session_id = {},
          .ip_address = {}};
}

// These serial fixture cases own the process registry. Bootstrap its stopped
// state before construction, rather than treating init_registry's rejection
// of somebody else's live registry as permission to take that registry over.
inline auto initialize() -> void {
  demand(plinth::async_bridge_test::pg_available(),
         "native capability corpus requires PostgreSQL");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  demand(plinth::lifecycle::async_tasks().drain(5s) &&
             plinth::log::wait_for_audits(5s) &&
             plinth::extensions::shutdown_registry(5s),
         "native capability serial registry bootstrap did not drain");
  plinth::async_bridge_test::reset_schema(
      plinth::async_bridge_test::test_config().db);
}

class Provider {
 public:
  Provider()
      : config(plinth::async_bridge_test::test_config()), control(config.db) {
    static std::atomic<unsigned> sequence{0};
    config.db.pool_size = 1;
    extension =
        "caprt" + std::to_string(getpid()) + "r" +
        std::to_string(sequence.fetch_add(1)) + "t" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    schema = "ext_" + extension;
    // This trusted host helper chooses the exact database-scoped identity to
    // preflight/clean. It is not a value/converter oracle or a runtime grant.
    role = plinth::db::extension_role_name(config.db.database, extension);
    legacy_role = schema + "_role";
    demand(
        extension.size() <= 54 &&
            control.scalar(
                "SELECT count(*) FROM plinth.extension_database_credentials "
                "WHERE extension_name='" +
                extension + "'") == "0" &&
            control.scalar("SELECT to_regnamespace('" + schema +
                           "') IS NULL") == "t" &&
            control.scalar("SELECT count(*) FROM pg_roles WHERE rolname IN ('" +
                           role + "','" + legacy_role + "')") == "0",
        "native capability refuses pre-existing fixture identity");
    base = std::filesystem::temp_directory_path() / ("plinth_" + extension);
    demand(std::filesystem::create_directory(base),
           "native capability scratch path already exists");
    try {
      config.packages_data_dir = (base / "data").string();
      directory =
          base / "data/extensions" / extension / "1.0.0/server/handlers";
      std::filesystem::create_directories(directory);
      {
        std::ofstream main_file(directory.parent_path() / "main.js");
        main_file << "// owned native capability fixture\n";
        demand(main_file.good(), "native capability main source write failed");
      }
      std::filesystem::create_directory_symlink(
          "1.0.0", base / "data/extensions" / extension / "active");
      registry_owned = true;
      plinth::extensions::init_registry(config);
      demand(plinth::extensions::create_pool(extension),
             "native capability provider pool unavailable");
      identity_owned = true; // Before issuing DDL: uncertain ACK still unwinds.
      static_cast<void>(control.query(
          "SELECT plinth.provision_extension_database('" + extension + "')"));
      demand(control.scalar(
                 "SELECT role_name FROM plinth.extension_database_credentials "
                 "WHERE extension_name='" +
                 extension + "'") == role &&
                 role != config.db.user,
             "native capability restricted identity mismatch");
      static_cast<void>(control.query("CREATE TABLE " + schema +
                                      ".receipt (marker integer PRIMARY KEY, "
                                      "pid integer NOT NULL, login text NOT "
                                      "NULL, session_login text NOT NULL)"));
      static_cast<void>(control.query("ALTER TABLE " + schema +
                                      ".receipt OWNER TO \"" + role + "\""));
    } catch (...) {
      if (!finish()) std::terminate();
      throw;
    }
  }
  ~Provider() {
    if (!closed) {
      try {
        if (!finish()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Provider(const Provider&) = delete;
  auto operator=(const Provider&) -> Provider& = delete;
  auto stage(std::string_view function, const std::string& source) const
      -> void {
    demand(
        function.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") ==
                std::string_view::npos &&
            !function.empty() && source.size() <= 16384,
        "native capability fixture source/name bound");
    std::ofstream file(directory / (std::string{function} + ".js"));
    file << source;
    demand(file.good(), "native capability handler source write failed");
  }
  auto seed(std::string_view function, std::string rule = "capnative.call",
            bool enabled = true) const -> void {
    plinth::capabilities::CachedCapability entry{
        .signature = "capnative:1:" + std::string{function},
        .provider_type = "extension",
        .extension_name = extension,
        .scope = "instance",
        .user_id = {},
        .rbac_rule = std::move(rule),
        .enabled = enabled};
    plinth::capabilities::seed_tier2_cache_for_test(entry);
  }
  auto entry_statement(unsigned marker) const -> std::string {
    demand(marker <= 100000, "native capability receipt identity bound");
    return "const receipt=await db.exec('INSERT INTO " + schema +
           ".receipt VALUES (" + std::to_string(marker) +
           ",pg_backend_pid(),current_user,session_user)',[],{silent:true});"
           "if(receipt.row_count!==1)throw new Error('native receipt "
           "acknowledgement');";
  }
  auto receipt_count(unsigned marker) -> unsigned {
    return static_cast<unsigned>(std::stoul(
        control.scalar("SELECT count(*) FROM " + schema +
                       ".receipt WHERE marker=" + std::to_string(marker) +
                       " AND login='" + role + "' AND session_login=login")));
  }
  auto total_receipts() -> unsigned {
    return static_cast<unsigned>(std::stoul(
        control.scalar("SELECT count(*) FROM " + schema + ".receipt")));
  }
  auto finish() -> bool {
    if (closed) return true;
    if (!plinth::lifecycle::async_tasks().drain(5s) ||
        !plinth::log::wait_for_audits(5s))
      return false;
    if (registry_owned) {
      if (!plinth::extensions::shutdown_registry(5s)) return false;
      registry_owned = false;
      plinth::capabilities::clear_resolver_for_test();
    }
    if (identity_owned) {
      if (!plinth::packages::drop_schema_and_migrations(extension,
                                                        control.raw()))
        return false;
      demand(
          control.scalar("SELECT to_regnamespace('" + schema + "') IS NULL") ==
                  "t" &&
              control.scalar(
                  "SELECT count(*) FROM plinth.extension_database_credentials "
                  "WHERE extension_name='" +
                  extension + "'") == "0" &&
              control.scalar(
                  "SELECT count(*) FROM pg_roles WHERE rolname IN ('" + role +
                  "','" + legacy_role + "')") == "0",
          "native capability fixture identity cleanup incomplete");
      identity_owned = false;
    }
    std::filesystem::remove_all(base);
    closed = !std::filesystem::exists(base);
    return closed;
  }
  plinth::Config config;
  std::string extension;
  std::string schema;
  std::string role;
  Control control;

 private:
  std::string legacy_role;
  std::filesystem::path base;
  std::filesystem::path directory;
  bool registry_owned = false;
  bool identity_owned = false;
  bool closed = false;
};

struct Cleanup {
  bool retired;
  std::size_t pending;
  std::size_t retired_pending;
  std::size_t callbacks;
  std::size_t persistent;
  int concurrent;
  int detached;
  int depth;
  std::uint64_t scope;
  bool pinned;
  bool rollback;
  bool timer;
  bool timer_loop;
  bool drained;
  bool audits;
  std::size_t native_active;
  int pool_active;
  auto operator==(const Cleanup&) const -> bool = default;
};
inline auto clean(const Cleanup& actual) -> bool {
  const Cleanup expected{true,  0,     0,     0,     0,    0,    0, 0, 0,
                         false, false, false, false, true, true, 0, 0};
  return actual == expected;
}
struct Execution {
  plinth::js::EvalResult result;
  Cleanup cleanup;
  int admitted = 0;
};
struct Shutdown {
  bool stopped;
  bool idempotent;
  int active;
  int free;
};
inline auto stopped(const Shutdown& state) -> bool {
  return state.stopped && state.idempotent && state.active == 0 &&
         state.free == 0;
}
inline auto limits() -> plinth::js::RuntimeLimits {
  auto result = plinth::js::default_runtime_limits();
  result.wall_clock_limit = 3s;
  return result;
}

class Caller {
 private:
  plinth::Config config;
  bool closed = false;

 public:
  Caller(const plinth::Config& config_in,
         const plinth::capabilities::UserContext& identity = user(),
         plinth::js::RuntimeLimits runtime_limits = limits())
      : config(config_in), pool(nullptr, runtime_limits, config, 1, &identity) {
  }
  ~Caller() {
    if (!closed) {
      try {
        if (!stopped(finish())) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Caller(const Caller&) = delete;
  auto operator=(const Caller&) -> Caller& = delete;
  auto run(const std::string& source, int call_depth = 0) -> Execution {
    demand(source.size() <= 16384, "native capability caller source bound");
    const auto destroy = [this](plinth::js::BridgeContext* context) {
      pool.destroy(context);
    };
    std::unique_ptr<plinth::js::BridgeContext, decltype(destroy)> context{
        pool.acquire(), destroy};
    demand(context != nullptr, "native capability caller acquire failed");
    context->call_depth = call_depth;
    auto result =
        drogon::sync_wait(plinth::js::run_on_context(*context, source));
    const auto& batch = context->batch_state;
    const int admitted = context->next_callback_id;
    Cleanup cleanup{
        !context->execution_lifetime.load(std::memory_order_acquire),
        context->pending_ops.size(),
        context->retired_pending_ops.size(),
        context->callbacks.size(),
        context->persistent_callbacks.size(),
        context->concurrent_async_ops,
        context->inflight_detached.load(std::memory_order_acquire),
        batch.depth,
        batch.scope_id,
        static_cast<bool>(batch.pinned_conn),
        static_cast<bool>(batch.rollback_owner),
        batch.timer_id != trantor::InvalidTimerId,
        batch.timer_loop != nullptr,
        plinth::lifecycle::async_tasks().drain(5s),
        plinth::log::wait_for_audits(5s),
        plinth::lifecycle::async_tasks().active_for_test(),
        0};
    context.reset();
    cleanup.pool_active = pool.active_count();
    return {std::move(result), cleanup, admitted};
  }
  auto finish() -> Shutdown {
    const bool first = pool.shutdown(5s);
    const bool second = pool.shutdown(0ms);
    const Shutdown result{first, second, pool.active_count(),
                          pool.free_count()};
    closed = stopped(result);
    return result;
  }
  plinth::js::RuntimePool pool;
};

} // namespace native_capability_corpus::fixture
