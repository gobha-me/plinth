// SPDX-License-Identifier: MIT

#include "async_bridge_fixture.hpp"
#include "kernel/db/connection_info.hpp"

#include "postgresql_impl/PgConnection.h"

#include <catch2/catch_test_macros.hpp>
#include <drogon/config.h>
#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>
#include <trantor/net/EventLoopThread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;
using Client = drogon::orm::DbClient;
using Connection = drogon::orm::PgConnection;
using Command = drogon::orm::SqlCmd;
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto demand(bool condition, const char* message) -> void {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct ErrorDetails {
  bool sql_error = false;
  bool exact_failure = false;
  std::string state;
  std::string query;
  std::string message;
};

auto inspect_error(const std::exception_ptr& value) -> ErrorDetails {
  ErrorDetails details;
  if (value) {
    try {
      std::rethrow_exception(value);
    } catch (const drogon::orm::SqlError& error) {
      details.sql_error = true;
      details.state = error.sqlState();
      details.query = error.query();
      details.message = error.what();
    } catch (const drogon::orm::Failure& error) {
      details.exact_failure = typeid(error) == typeid(drogon::orm::Failure);
      details.message = error.what();
    } catch (const std::exception& error) {
      details.message = error.what();
    } catch (...) {
      details.message = "non-standard exception";
    }
  }
  return details;
}

struct LiteralRow {
  std::string name;
  std::string state;
  std::string sql;
  std::string parameter;
};

class Fixture {
 public:
  Fixture() {
    static std::atomic<unsigned> sequence{0};
    schema = "plinth_sqlstate_" + std::to_string(getpid()) + "_" +
             std::to_string(sequence.fetch_add(1));
    info = plinth::db::connection_info(
               plinth::async_bridge_test::test_config().db) +
           " connect_timeout=3 options='-c statement_timeout=3000 -c "
           "lock_timeout=2500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand(connection && PQstatus(connection.get()) == CONNECTION_OK,
           "SQLSTATE control connection unavailable");
    auto absent = query("SELECT to_regnamespace($1)", &schema);
    demand(PQresultStatus(absent.get()) == PGRES_TUPLES_OK &&
               PQntuples(absent.get()) == 1 && PQgetisnull(absent.get(), 0, 0),
           "refuse a pre-existing SQLSTATE fixture schema");
    // An attempted CREATE remains owned even if its acknowledgement is lost.
    owned = true;
    try {
      command("CREATE SCHEMA " + schema);
      command("CREATE TABLE " + schema +
              ".rows (id integer PRIMARY KEY, payload text NOT NULL)");
      command("INSERT INTO " + schema + ".rows VALUES (1, 'seed')");
      command("CREATE FUNCTION " + schema +
              ".raise_state(text) RETURNS integer LANGUAGE plpgsql VOLATILE AS "
              "$$ BEGIN RAISE EXCEPTION USING ERRCODE='P1234', "
              "MESSAGE='syntax error relation missing SQLSTATE 42601'; END $$");
    } catch (...) {
      if (!finish()) {
        std::terminate();
      }
      throw;
    }
  }
  ~Fixture() {
    if (!finish()) {
      std::terminate();
    }
  }
  Fixture(const Fixture&) = delete;
  auto operator=(const Fixture&) -> Fixture& = delete;

  auto rows() const -> std::array<LiteralRow, 6> {
    return {
        {{"syntax", "42601", "SELEC $1::integer AS answer", "1"},
         {"relation", "42P01",
          "SELECT $1::integer FROM " + schema + ".absent_relation", "1"},
         {"column", "42703",
          "SELECT absent_column FROM " + schema + ".rows WHERE id=$1::integer",
          "1"},
         {"division", "22012", "SELECT 10/$1::integer AS answer", "0"},
         {"constraint", "23505",
          "INSERT INTO " + schema +
              ".rows VALUES ($1::integer, 'duplicate') RETURNING id",
          "1"},
         {"unknown_misleading", "P1234",
          "SELECT " + schema + ".raise_state($1::text)", "1"}}};
  }
  auto query(const std::string& sql, const std::string* parameter = nullptr)
      -> PgResult {
    const char* values[] = {parameter ? parameter->c_str() : nullptr};
    PgResult result{
        PQexecParams(connection.get(), sql.c_str(), parameter ? 1 : 0, nullptr,
                     parameter ? values : nullptr, nullptr, nullptr, 0),
        PQclear};
    demand(result != nullptr, "control query returned no PGresult");
    return result;
  }
  auto baseline(const LiteralRow& row) -> ErrorDetails {
    auto result = query(row.sql, &row.parameter);
    demand(PQresultStatus(result.get()) == PGRES_FATAL_ERROR,
           "literal control query did not fail");
    const auto* state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
    const auto* message =
        PQresultErrorField(result.get(), PG_DIAG_MESSAGE_PRIMARY);
    ErrorDetails details;
    details.state = state ? state : "";
    details.message = message ? message : "";
    auto recovery = query("SELECT 42");
    demand(PQresultStatus(recovery.get()) == PGRES_TUPLES_OK &&
               PQntuples(recovery.get()) == 1 &&
               PQnfields(recovery.get()) == 1 &&
               std::string_view{PQgetvalue(recovery.get(), 0, 0)} == "42",
           "independent control recovery failed");
    return details;
  }
  auto finish() noexcept -> bool {
    if (!owned) {
      return true;
    }
    try {
      command("DROP FUNCTION IF EXISTS " + schema + ".raise_state(text)");
      command("DROP TABLE IF EXISTS " + schema + ".rows");
      command("DROP SCHEMA IF EXISTS " +
              schema); // Never CASCADE unknown contents.
      owned = false;
      return true;
    } catch (...) {
      return false;
    }
  }
  auto backend() const -> int { return PQbackendPID(connection.get()); }

  std::string info;

 private:
  auto command(const std::string& sql) -> void {
    auto result = query(sql);
    demand(PQresultStatus(result.get()) == PGRES_COMMAND_OK,
           "owned SQLSTATE fixture command failed");
  }
  std::string schema;
  bool owned = false;
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
};

struct Event {
  std::string label;
  bool success = false;
  int value = 0;
  int backend = 0;
  std::exception_ptr error;
};

struct Observations {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Event> events;
  std::atomic<bool> fault{false};
  bool connected = false;
  bool closed = false;
  int pipeline = -1;
  int backend = 0;
  unsigned idles = 0;
  bool idle_clean = true;
  // Access this owner only on its loop, and destroy it there before joining.
  std::shared_ptr<Connection> connection;
};

struct Operation {
  const std::string label;
  const std::string sql;
  const std::string parameter;
};

auto result_callback(const std::shared_ptr<Observations>& state,
                     const std::shared_ptr<const Operation>& operation)
    -> drogon::orm::ResultCallback {
  return [state, operation](const drogon::orm::Result& result) noexcept {
    try {
      Event event;
      event.label = operation->label;
      event.success = true;
      demand(result.size() == 1 && result[0].size() >= 1,
             "unexpected native result shape");
      event.value = result[0][0].as<int>();
      if (result[0].size() == 2) {
        event.backend = result[0][1].as<int>();
      }
      std::lock_guard lock(state->mutex);
      state->events.push_back(std::move(event));
    } catch (...) {
      state->fault.store(true);
    }
    state->changed.notify_all();
  };
}

auto error_callback(const std::shared_ptr<Observations>& state,
                    const std::shared_ptr<const Operation>& operation)
    -> std::function<void(const std::exception_ptr&)> {
  return [state, operation](const std::exception_ptr& error) noexcept {
    try {
      std::lock_guard lock(state->mutex);
      state->events.push_back({operation->label, false, 0, 0, error});
    } catch (...) {
      state->fault.store(true);
    }
    state->changed.notify_all();
  };
}

template <typename Predicate>
auto await(const std::shared_ptr<Observations>& state, Predicate predicate,
           Clock::time_point deadline) -> bool {
  std::unique_lock lock(state->mutex);
  return state->changed.wait_until(
             lock, std::min(deadline, Clock::now() + 5s),
             [&] { return state->fault.load() || predicate(*state); }) &&
         !state->fault.load();
}

class OwnedClient {
 public:
  OwnedClient(const std::string& info, bool automatic)
      : client(Client::newPgClient(info, 1, automatic)) {
    client->setTimeout(3.0);
  }
  ~OwnedClient() {
    if (!finish()) {
      std::terminate(); // A failed finite drain is not permission to release.
    }
  }
  OwnedClient(const OwnedClient&) = delete;
  auto operator=(const OwnedClient&) -> OwnedClient& = delete;
  auto finish() -> bool {
    if (!client) {
      return true;
    }
    if (!client->closeAllFor(5s)) {
      return false;
    }
    client.reset();
    return true;
  }
  std::shared_ptr<Client> client;
};

class OwnedConnection {
 public:
  explicit OwnedConnection(const std::string& info)
      : state(std::make_shared<Observations>()), thread("sqlstate-native") {
    thread.run();
    loop = thread.getLoop();
    auto observed = state;
    loop->queueInLoop([observed, info, loop_in = loop] {
      try {
        observed->connection =
            std::make_shared<Connection>(loop_in, info, true);
        const std::weak_ptr<Observations> weak = observed;
        observed->connection->setOkCallback(
            [weak](const drogon::orm::DbConnectionPtr& base) {
              auto locked = weak.lock();
              if (!locked) {
                return;
              }
              try {
                const auto pg = std::static_pointer_cast<Connection>(base);
                std::lock_guard lock(locked->mutex);
                locked->pipeline =
                    static_cast<int>(PQpipelineStatus(pg->pgConn().get()));
                locked->backend = PQbackendPID(pg->pgConn().get());
                locked->connected = true;
              } catch (...) {
                locked->fault.store(true);
              }
              locked->changed.notify_all();
            });
        observed->connection->setCloseCallback(
            [weak](const drogon::orm::DbConnectionPtr&) {
              auto locked = weak.lock();
              if (!locked) {
                return;
              }
              try {
                std::lock_guard lock(locked->mutex);
                locked->closed = true;
              } catch (...) {
                locked->fault.store(true);
              }
              locked->changed.notify_all();
            });
        observed->connection->setIdleCallback([weak] {
          auto locked = weak.lock();
          if (!locked) {
            return;
          }
          try {
            std::lock_guard lock(locked->mutex);
            const auto& pg = locked->connection;
            ++locked->idles;
            locked->idle_clean =
                locked->idle_clean && !pg->isWorking() &&
                PQisBusy(pg->pgConn().get()) == 0 &&
                PQtransactionStatus(pg->pgConn().get()) == PQTRANS_IDLE &&
                PQbackendPID(pg->pgConn().get()) == locked->backend &&
                static_cast<int>(PQpipelineStatus(pg->pgConn().get())) ==
                    locked->pipeline;
          } catch (...) {
            locked->fault.store(true);
          }
          locked->changed.notify_all();
        });
        observed->connection->init();
      } catch (...) {
        observed->fault.store(true);
        observed->changed.notify_all();
      }
    });
  }
  ~OwnedConnection() {
    if (!finish()) {
      std::terminate();
    }
  }
  OwnedConnection(const OwnedConnection&) = delete;
  auto operator=(const OwnedConnection&) -> OwnedConnection& = delete;

  auto finish() -> bool {
    if (finished) {
      return true;
    }
    auto acknowledgement = std::make_shared<std::promise<bool>>();
    auto done = acknowledgement->get_future();
    auto observed = state;
    loop->queueInLoop([observed, acknowledgement] {
      try {
        if (observed->connection) {
          observed->connection->disconnect();
          observed->connection.reset();
        }
        acknowledgement->set_value(true);
      } catch (...) {
        acknowledgement->set_value(false);
      }
    });
    if (done.wait_for(5s) != std::future_status::ready || !done.get()) {
      return false;
    }
    thread.requestStop();
    finished = thread.waitFor(5s);
    return finished;
  }
  auto single(const std::shared_ptr<const Operation>& operation) -> void {
    auto observed = state;
    loop->queueInLoop([observed, operation] {
      try {
        demand(!observed->connection->isWorking(),
               "single query requires idle");
        auto command = make_command(observed, operation);
        observed->connection->execSql(
            std::string_view{command->sql_}, command->parametersNumber_,
            std::move(command->parameters_), std::move(command->lengths_),
            std::move(command->formats_), std::move(command->callback_),
            std::move(command->exceptionCallback_));
      } catch (...) {
        observed->fault.store(true);
        observed->changed.notify_all();
      }
    });
  }
  auto pair(const std::shared_ptr<const Operation>& cause,
            const std::shared_ptr<const Operation>& follower) -> void {
    auto observed = state;
    loop->queueInLoop([observed, cause, follower] {
      try {
        demand(!observed->connection->isWorking(), "batch requires idle");
        std::deque<std::shared_ptr<Command>> commands;
        commands.push_back(make_command(observed, cause));
        commands.push_back(make_command(observed, follower));
        observed->connection->batchSql(
            std::move(commands)); // One atomic submission.
      } catch (...) {
        observed->fault.store(true);
        observed->changed.notify_all();
      }
    });
  }

  std::shared_ptr<Observations> state;

 private:
  static auto make_command(const std::shared_ptr<Observations>& observed,
                           const std::shared_ptr<const Operation>& operation)
      -> std::shared_ptr<Command> {
    const bool parameterized = !operation->parameter.empty();
    std::vector<const char*> parameters;
    std::vector<int> lengths;
    std::vector<int> formats;
    if (parameterized) {
      parameters.push_back(operation->parameter.c_str());
      lengths.push_back(static_cast<int>(operation->parameter.size()));
      formats.push_back(0);
    }
    const auto parameter_count = parameters.size();
    return std::make_shared<Command>(std::string_view(operation->sql),
                                     parameter_count, std::move(parameters),
                                     std::move(lengths), std::move(formats),
                                     result_callback(observed, operation),
                                     error_callback(observed, operation));
  }
  trantor::EventLoopThread thread;
  trantor::EventLoop* loop = nullptr;
  bool finished = false;
};

auto check_error(const Event& event, const LiteralRow& row) -> void {
  CHECK_FALSE(event.success);
  const auto details = inspect_error(event.error);
  CHECK(details.sql_error);
  CHECK(details.state == row.state);
  CHECK(details.query == row.sql);
  CHECK_FALSE(details.message.empty());
}

} // namespace

TEST_CASE("PostgreSQL driver preserves authoritative error diagnostics",
          "[js][db][sqlstate][native]") {
  REQUIRE(plinth::async_bridge_test::pg_available());
  Fixture fixture;
  const auto rows = fixture.rows();
  std::array<ErrorDetails, 6> baselines;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    baselines[index] = fixture.baseline(rows[index]);
  }
  const auto deadline = Clock::now() + 40s;
  int compiled_mode = -1;
  bool mode_clean = false;
  {
    OwnedConnection mode{fixture.info};
    demand(await(
               mode.state, [](const auto& state) { return state.connected; },
               deadline),
           "compiled-driver mode connection unavailable");
    compiled_mode = mode.state->pipeline;
    mode_clean = mode.finish();
  }
  std::array<std::shared_ptr<Observations>, 2> results;
  std::array<bool, 2> closed{};
  std::string infrastructure;
  // Immutable command buffers stay owned through callbacks AND finite client
  // join.
  std::vector<std::shared_ptr<const Operation>> owners;
  for (std::size_t batch = 0; batch < results.size(); ++batch) {
    auto state = std::make_shared<Observations>();
    results[batch] = state;
    OwnedClient client{fixture.info, batch != 0};
    try {
      auto dispatch = [&](std::string label, std::string sql,
                          std::string parameter) {
        demand(Clock::now() < deadline, "native SQLSTATE admission deadline");
        auto operation = std::make_shared<const Operation>(
            Operation{std::move(label), std::move(sql), std::move(parameter)});
        owners.push_back(operation);
        if (operation->parameter.empty()) {
          client.client->execSqlAsync(operation->sql,
                                      result_callback(state, operation),
                                      error_callback(state, operation));
        } else {
          client.client->execSqlAsync(
              operation->sql, result_callback(state, operation),
              error_callback(state, operation), operation->parameter);
        }
        demand(await(
                   state,
                   [&](const auto& observed) {
                     return std::ranges::any_of(
                         observed.events, [&](const auto& event) {
                           return event.label == operation->label;
                         });
                   },
                   deadline),
               "native SQLSTATE callback deadline");
      };
      dispatch("initial", "SELECT 42, pg_backend_pid()", "");
      for (const auto& row : rows) {
        dispatch(row.name, row.sql, row.parameter);
        dispatch(row.name + "_recovery", "SELECT 42, pg_backend_pid()", "");
      }
    } catch (const std::exception& error) {
      infrastructure = error.what();
    }
    closed[batch] = client.finish();
  }
  const bool fixture_removed = fixture.finish();
  // All Catch checks, exception inspection and cardinality checks follow
  // cleanup.
  CHECK(infrastructure.empty());
  CHECK(mode_clean);
#if LIBPQ_SUPPORTS_BATCH_MODE
  CHECK(compiled_mode == static_cast<int>(PQ_PIPELINE_ON));
#else
  CHECK(compiled_mode == static_cast<int>(PQ_PIPELINE_OFF));
#endif
  CHECK(fixture_removed);
  for (std::size_t index = 0; index < rows.size(); ++index) {
    CHECK(baselines[index].state == rows[index].state);
    CHECK_FALSE(baselines[index].message.empty());
  }
  for (std::size_t batch = 0; batch < results.size(); ++batch) {
    INFO("autoBatch=" << batch);
    CHECK(closed[batch]);
    const auto& state = results[batch];
    CHECK_FALSE(state->fault.load());
    REQUIRE(state->events.size() == 13);
    const auto backend = state->events[0].backend;
    CHECK(backend > 0);
    CHECK(backend != fixture.backend());
    CHECK(state->events[0].label == "initial");
    CHECK(state->events[0].success);
    CHECK(state->events[0].value == 42);
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto& error = state->events[1 + index * 2];
      CHECK(error.label == rows[index].name);
      check_error(error, rows[index]);
      const auto& recovery = state->events[2 + index * 2];
      CHECK(recovery.label == rows[index].name + "_recovery");
      CHECK(recovery.success);
      CHECK(recovery.value == 42);
      CHECK(recovery.backend == backend);
    }
  }
}

#if LIBPQ_SUPPORTS_BATCH_MODE
TEST_CASE("PostgreSQL pipeline failures preserve only their causal diagnostics",
          "[js][db][sqlstate][pipeline]") {
  REQUIRE(plinth::async_bridge_test::pg_available());
  Fixture fixture;
  const auto deadline = Clock::now() + 40s;
  struct Outcome {
    bool execution;
    unsigned epoch;
    std::shared_ptr<Observations> state;
    bool clean;
    std::string infrastructure;
    LiteralRow expected;
  };
  std::vector<Outcome> outcomes;
  for (bool execution : {false, true}) {
    for (unsigned epoch = 0; epoch < 2; ++epoch) {
      // Every owner is immutable and survives callback, idle, disconnect and
      // join.
      const auto warm_cause = std::make_shared<const Operation>(
          Operation{"warm_cause", "SELECT 10/$1::integer AS answer", "1"});
      const auto warm_follower = std::make_shared<const Operation>(
          Operation{"warm_follower", "SELECT $1::integer + 41 AS answer", "1"});
      const auto cause = std::make_shared<const Operation>(Operation{
          "cause", execution ? warm_cause->sql : "SELEC $1::integer AS answer",
          execution ? "0" : "1"});
      const auto follower = std::make_shared<const Operation>(
          Operation{"follower", warm_follower->sql, warm_follower->parameter});
      const auto recovery = std::make_shared<const Operation>(
          Operation{"recovery", warm_follower->sql, warm_follower->parameter});
      auto state = std::shared_ptr<Observations>{};
      bool clean = false;
      std::string infrastructure;
      {
        OwnedConnection connection{fixture.info};
        state = connection.state;
        try {
          demand(await(
                     state,
                     [](const auto& observed) { return observed.connected; },
                     deadline),
                 "pipeline connection unavailable");
          demand(state->pipeline == static_cast<int>(PQ_PIPELINE_ON),
                 "actual connection did not enter compiled pipeline mode");
          unsigned expected_idles = 0;
          std::size_t expected_events = 0;
          const auto wait_idle = [&] {
            ++expected_idles;
            demand(await(
                       state,
                       [&](const auto& observed) {
                         return observed.idles >= expected_idles &&
                                observed.events.size() >= expected_events;
                       },
                       deadline),
                   "pipeline callback/sync-idle deadline");
          };
          if (execution) {
            connection.single(warm_cause);
            ++expected_events;
            wait_idle();
          }
          connection.single(
              warm_follower); // The follower is always cached first.
          ++expected_events;
          wait_idle();
          connection.pair(cause,
                          follower); // Exactly one atomic two-command deque.
          expected_events += 2;
          wait_idle(); // Unchanged driver invokes idle only after consumed
                       // Sync.
          connection.single(recovery);
          ++expected_events;
          wait_idle();
        } catch (const std::exception& error) {
          infrastructure = error.what();
        }
        clean = connection.finish();
      }
      outcomes.push_back({execution,
                          epoch,
                          state,
                          clean,
                          infrastructure,
                          {"cause", execution ? "22012" : "42601", cause->sql,
                           cause->parameter}});
    }
  }
  const bool fixture_removed = fixture.finish();
  // No early RED assertion can omit a later epoch or bypass owner cleanup.
  CHECK(fixture_removed);
  REQUIRE(outcomes.size() == 4);
  for (const auto& outcome : outcomes) {
    INFO("execution=" << outcome.execution
                      << " fresh-cache epoch=" << outcome.epoch);
    const auto& state = outcome.state;
    CHECK(outcome.infrastructure.empty());
    CHECK(outcome.clean);
    CHECK_FALSE(state->fault.load());
    CHECK(state->connected);
    CHECK(state->pipeline == static_cast<int>(PQ_PIPELINE_ON));
    CHECK(state->backend > 0);
    CHECK(state->backend != fixture.backend());
    CHECK(state->idle_clean);
    CHECK(state->idles == (outcome.execution ? 4 : 3));
    REQUIRE(state->events.size() == (outcome.execution ? 5 : 4));
    std::size_t offset = 0;
    if (outcome.execution) {
      CHECK(state->events[offset].label == "warm_cause");
      CHECK(state->events[offset].success);
      CHECK(state->events[offset++].value == 10);
    }
    CHECK(state->events[offset].label == "warm_follower");
    CHECK(state->events[offset].success);
    CHECK(state->events[offset++].value == 42);
    CHECK(state->events[offset].label == "cause");
    check_error(state->events[offset++], outcome.expected);
    const auto& skipped = state->events[offset++];
    CHECK(skipped.label == "follower");
    CHECK_FALSE(skipped.success);
    const auto details = inspect_error(skipped.error);
    CHECK(details.exact_failure);
    CHECK_FALSE(details.sql_error);
    CHECK(details.state.empty());
    CHECK(details.query.empty());
    CHECK_FALSE(details.message.empty());
    CHECK(state->events[offset].label == "recovery");
    CHECK(state->events[offset].success);
    CHECK(state->events[offset].value == 42);
  }
}
#endif
