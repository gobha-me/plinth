// SPDX-License-Identifier: MIT

#include "async_bridge_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <json/json.h>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using plinth::js::BridgeContext;
using plinth::js::RuntimePool;
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto demand_projection(bool condition, const char* message) -> void {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct ProjectionRow {
  std::string name;
  std::string state;
  std::string code;
  std::string sql;
  std::string parameter;
};

class ProjectionFixture {
 public:
  explicit ProjectionFixture(const plinth::Config::Database& config) {
    const auto info = plinth::db::connection_info(config) +
                      " connect_timeout=3 options='-c statement_timeout=3000 "
                      "-c lock_timeout=2500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand_projection(connection && PQstatus(connection.get()) == CONNECTION_OK,
                      "SQLSTATE projection control connection unavailable");
    const auto suffix = std::to_string(getpid());
    table = "plinth.sqlstate_projection_" + suffix;
    function = "plinth.sqlstate_projection_error_" + suffix;
    auto absent_table = query("SELECT to_regclass($1)", &table);
    const auto signature = function + "(text)";
    auto absent_function = query("SELECT to_regprocedure($1)", &signature);
    demand_projection(PQresultStatus(absent_table.get()) == PGRES_TUPLES_OK &&
                          PQntuples(absent_table.get()) == 1 &&
                          PQgetisnull(absent_table.get(), 0, 0) &&
                          PQresultStatus(absent_function.get()) ==
                              PGRES_TUPLES_OK &&
                          PQntuples(absent_function.get()) == 1 &&
                          PQgetisnull(absent_function.get(), 0, 0),
                      "refuse pre-existing SQLSTATE projection objects");
    try {
      // Absence was checked before admission. A lost CREATE acknowledgement
      // still leaves an attempted object owned by this exact fixture.
      table_owned = true;
      command("CREATE TABLE " + table + " (id integer PRIMARY KEY)");
      command("INSERT INTO " + table + " VALUES (1)");
      function_owned = true;
      command("CREATE FUNCTION " + function +
              "(text) RETURNS integer LANGUAGE plpgsql VOLATILE AS $$ BEGIN "
              "RAISE EXCEPTION USING ERRCODE='P1234', "
              "MESSAGE='syntax error relation missing SQLSTATE 42601'; END $$");
    } catch (...) {
      if (!finish()) {
        std::terminate();
      }
      throw;
    }
  }
  ~ProjectionFixture() {
    if (!finish()) {
      std::terminate(); // Never pretend unacknowledged teardown succeeded.
    }
  }
  ProjectionFixture(const ProjectionFixture&) = delete;
  auto operator=(const ProjectionFixture&) -> ProjectionFixture& = delete;

  auto rows() const -> std::array<ProjectionRow, 6> {
    return {
        {{"syntax", "42601", "db.syntax_error", "SELEC $1::integer AS answer",
          "1"},
         {"relation", "42P01", "db.undefined_table",
          "SELECT $1::integer FROM plinth.sqlstate_absent_relation", "1"},
         {"column", "42703", "db.undefined_column",
          "SELECT absent_column FROM " + table + " WHERE id=$1::integer", "1"},
         {"division", "22012", "db.internal", "SELECT 10/$1::integer AS answer",
          "0"},
         {"constraint", "23505", "db.constraint_violation",
          "INSERT INTO " + table + " VALUES ($1::integer) RETURNING id", "1"},
         {"unknown_misleading", "P1234", "db.internal",
          "SELECT " + function + "($1::text)", "1"}}};
  }

  auto count(int id) -> int {
    const auto parameter = std::to_string(id);
    auto result = query(
        "SELECT count(*) FROM " + table + " WHERE id=$1::integer", &parameter);
    demand_projection(PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
                          PQntuples(result.get()) == 1 &&
                          PQnfields(result.get()) == 1,
                      "independent SQLSTATE rollback readback failed");
    return std::stoi(PQgetvalue(result.get(), 0, 0));
  }

  auto finish() noexcept -> bool {
    if (!table_owned && !function_owned) {
      return true;
    }
    if (!plinth::lifecycle::async_tasks().drain(5s) ||
        !plinth::log::wait_for_audits(5s)) {
      return false;
    }
    try {
      if (function_owned) {
        command("DROP FUNCTION IF EXISTS " + function + "(text)");
        function_owned = false;
      }
      if (table_owned) {
        command("DROP TABLE IF EXISTS " + table);
        table_owned = false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  std::string table;

 private:
  auto query(const std::string& sql, const std::string* parameter = nullptr)
      -> PgResult {
    const char* values[] = {parameter ? parameter->c_str() : nullptr};
    PgResult result{
        PQexecParams(connection.get(), sql.c_str(), parameter ? 1 : 0, nullptr,
                     parameter ? values : nullptr, nullptr, nullptr, 0),
        PQclear};
    demand_projection(result != nullptr,
                      "SQLSTATE projection control returned no result");
    return result;
  }
  auto command(const std::string& sql) -> void {
    auto result = query(sql);
    demand_projection(PQresultStatus(result.get()) == PGRES_COMMAND_OK,
                      "owned SQLSTATE projection fixture command failed");
  }

  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
  std::string function;
  bool table_owned = false;
  bool function_owned = false;
};

struct ProjectionRun {
  plinth::js::EvalResult result;
  bool clean;
};

auto evaluate_projection(RuntimePool& pool, const std::string& source)
    -> ProjectionRun {
  demand_projection(source.size() < 4096, "SQLSTATE program bound exceeded");
  const auto destroy = [&pool](BridgeContext* context) {
    pool.destroy(context);
  };
  std::unique_ptr<BridgeContext, decltype(destroy)> context{pool.acquire(),
                                                            destroy};
  demand_projection(context != nullptr,
                    "SQLSTATE projection context unavailable");
  auto result = drogon::sync_wait(plinth::js::run_on_context(*context, source));
  const bool retired =
      !context->execution_lifetime.load(std::memory_order_acquire);
  const bool queues =
      context->pending_ops.empty() && context->retired_pending_ops.empty();
  const bool callbacks =
      context->callbacks.empty() && context->persistent_callbacks.empty();
  const bool workers =
      context->concurrent_async_ops == 0 &&
      context->inflight_detached.load(std::memory_order_acquire) == 0;
  const auto& batch = context->batch_state;
  const bool batch_empty = batch.depth == 0 && batch.scope_id == 0 &&
                           !batch.pinned_conn && !batch.rollback_owner &&
                           batch.timer_id == trantor::InvalidTimerId &&
                           batch.timer_loop == nullptr;
  const bool drained = plinth::lifecycle::async_tasks().drain(5s);
  const bool audits = plinth::log::wait_for_audits(5s);
  const bool inactive = plinth::lifecycle::async_tasks().active_for_test() == 0;
  context
      .reset(); // Every assertion below uses only independently owned C++ data.
  return {std::move(result), retired && queues && callbacks && workers &&
                                 batch_empty && drained && audits && inactive};
}

auto projection_program(const ProjectionRow& row, std::string_view route,
                        const std::string& table, int insert_id)
    -> std::string {
  Json::Value spec{Json::objectValue};
  spec["sql"] = row.sql;
  spec["parameter"] = row.parameter;
  spec["route"] = std::string{route};
  spec["batch"] = insert_id != 0;
  spec["insert"] = "INSERT INTO " + table + " VALUES ($1::integer)";
  // Match the other textual parameters: integer-width binding belongs to the
  // separate value corpus, not this diagnostic-projection regression.
  spec["id"] = std::to_string(insert_id);
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return "const spec=" + Json::writeString(writer, spec) + R"JS(;
(async () => {
  let error=null;
  let inserted=!spec.batch;
  try {
    if (spec.batch) {
      await db.batch(async () => {
        const write=await db.exec(spec.insert,[spec.id]);
        inserted=write.row_count===1;
        await db.query(spec.sql,[spec.parameter]);
      });
    } else {
      await db[spec.route](spec.sql,[spec.parameter]);
    }
  } catch(e) {
    const own=(key)=>Object.prototype.hasOwnProperty.call(e,key);
    error={code:e.code,sqlstate:e.sqlstate,message:e.message,
      ownCode:own('code'),ownState:own('sqlstate'),ownMessage:own('message'),
      codeType:typeof e.code,stateType:typeof e.sqlstate,messageType:typeof e.message};
  }
  const recovery=await db.query('SELECT 42 AS answer');
  return {error,inserted,recovered:recovery.row_count===1 && recovery.rows.length===1,
          answer:recovery.rows[0].answer};
})()
)JS";
}

struct ProjectionObservation {
  std::string name;
  std::string code;
  std::string state;
  ProjectionRun run;
  bool rollback;
};

auto check_projection(const ProjectionObservation& observation) -> void {
  INFO("SQLSTATE projection=" << observation.name);
  CHECK(observation.run.clean);
  CHECK(observation.rollback);
  CHECK(observation.run.result.value.has_value());
  if (!observation.run.result.value) {
    return;
  }
  const auto& actual = *observation.run.result.value;
  CHECK(actual.isObject());
  if (!actual.isObject()) {
    return;
  }
  CHECK(actual["recovered"].asBool());
  CHECK(actual["inserted"].asBool());
  CHECK(actual["answer"].asInt() == 42);
  const auto& error = actual["error"];
  CHECK(error.isObject());
  if (!error.isObject()) {
    return;
  }
  CHECK(error["ownCode"].asBool());
  CHECK(error["ownState"].asBool());
  CHECK(error["ownMessage"].asBool());
  CHECK(error["codeType"].asString() == "string");
  CHECK(error["stateType"].asString() == "string");
  CHECK(error["messageType"].asString() == "string");
  CHECK(error["code"].isString());
  CHECK(error["sqlstate"].isString());
  CHECK(error["message"].isString());
  if (error["code"].isString()) {
    CHECK(error["code"].asString() == observation.code);
  }
  if (error["sqlstate"].isString()) {
    CHECK(error["sqlstate"].asString() == observation.state);
  }
  if (error["message"].isString()) {
    CHECK_FALSE(error["message"].asString().empty());
  }
}

} // namespace

TEST_CASE("Registered database bindings preserve authoritative SQLSTATE",
          "[js][db][sqlstate][projection][batch-transaction]") {
  // Admission deadline includes startup/setup; the strict subprocess watchdog
  // is the hard bound, including any uncooperative native drain or teardown.
  const auto deadline = Clock::now() + 45s;
  REQUIRE(plinth::async_bridge_test::pg_available());
  const auto config = plinth::async_bridge_test::test_config();
  REQUIRE(config.db.pool_size == 1);
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  plinth::async_bridge_test::reset_schema(config.db);
  ProjectionFixture fixture{config.db};
  const auto rows = fixture.rows();
  std::vector<ProjectionObservation> observations;
  std::array<std::array<std::array<unsigned, 2>, 6>, 2> coverage{};
  std::array<std::array<unsigned, 2>, 2> batches{};
  struct Shutdown {
    bool stopped;
    bool idempotent;
    int active;
    int free;
  };
  std::array<Shutdown, 2> shutdowns{};
  constexpr std::array<std::string_view, 2> routes{"query", "exec"};
  for (std::size_t epoch = 0; epoch < 2; ++epoch) {
    auto limits = plinth::js::default_runtime_limits();
    // This is a JS wall-clock bound, not a server statement/connect timeout.
    // Only the independent control has explicit 3s libpq/server limits. Keep
    // shared fixture client/GUC defaults unchanged and fail closed on a drain
    // failure; the strict process watchdog remains authoritative.
    limits.wall_clock_limit = 3s;
    RuntimePool pool{nullptr, limits, config, 1};
    for (std::size_t row = 0; row < rows.size(); ++row) {
      for (std::size_t route = 0; route < routes.size(); ++route) {
        demand_projection(Clock::now() < deadline,
                          "SQLSTATE projection deadline");
        auto run = evaluate_projection(
            pool,
            projection_program(rows[row], routes[route], fixture.table, 0));
        observations.push_back({std::to_string(epoch) + "/" + rows[row].name +
                                    "/" + std::string{routes[route]},
                                rows[row].code, rows[row].state, std::move(run),
                                true});
        ++coverage[epoch][row][route];
      }
    }
    constexpr std::array<std::size_t, 2> batch_rows{4, 3};
    for (std::size_t scenario = 0; scenario < batch_rows.size(); ++scenario) {
      demand_projection(Clock::now() < deadline,
                        "SQLSTATE batch projection deadline");
      const auto& row = rows[batch_rows[scenario]];
      const auto id = static_cast<int>(10 + epoch * 2 + scenario);
      auto run = evaluate_projection(
          pool, projection_program(row, "query", fixture.table, id));
      const bool rollback = fixture.count(id) == 0 && fixture.count(1) == 1;
      observations.push_back({std::to_string(epoch) + "/batch/" + row.name,
                              row.code, row.state, std::move(run), rollback});
      ++batches[epoch][scenario];
    }
    const bool stopped = pool.shutdown(5s);
    const bool idempotent = pool.shutdown(0ms);
    shutdowns[epoch] = {stopped, idempotent, pool.active_count(),
                        pool.free_count()};
  }
  const bool removed = fixture.finish();
  // Complete collection and owned teardown precede literal-oracle assertions.
  REQUIRE(observations.size() == 28);
  CHECK(removed);
  for (const auto& shutdown : shutdowns) {
    CHECK(shutdown.stopped);
    CHECK(shutdown.idempotent);
    CHECK(shutdown.active == 0);
    CHECK(shutdown.free == 0);
  }
  for (const auto& epoch : coverage) {
    for (const auto& row : epoch) {
      for (const auto count : row) {
        CHECK(count == 1);
      }
    }
  }
  for (const auto& epoch : batches) {
    for (const auto count : epoch) {
      CHECK(count == 1);
    }
  }
  for (const auto& observation : observations) {
    check_projection(observation);
  }
  CHECK(Clock::now() < deadline);
}
