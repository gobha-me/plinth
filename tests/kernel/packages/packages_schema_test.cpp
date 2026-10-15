// Schema-level tests for the 0.4.4 additions: plinth.packages and
// plinth.panels. Mirrors tests/kernel/db/bootstrap_test.cpp patterns —
// unit checks hit the on-disk schema.sql file, integration checks run
// against a live PG under PLINTH_PG_* env vars (skipped otherwise).

#include <catch2/catch_test_macros.hpp>

#include "../ws/ws_test_fixture.hpp"
#include "kernel/packages/panels.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <libpq-fe.h>

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"

namespace fs = std::filesystem;

namespace {

auto read_schema_sql() -> std::string {
  auto schema_path = fs::path{CMAKE_SOURCE_DIR} / "migrations" / "schema.sql";
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  // single-threaded.
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

auto conninfo_of(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password + " connect_timeout=3";
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  PGconn* conn = PQconnectdb(conninfo_of(pg_config()).c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  // is the only alternative and less readable here.
  PQfinish(conn);
  return ok;
}

auto drop_plinth_schema(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) == CONNECTION_OK) {
    PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  }
  PQfinish(conn);
}

// Execute `sql` and return true iff PGRES_TUPLES_OK or PGRES_COMMAND_OK.
auto exec_ok(PGconn* conn, const std::string& sql) -> bool {
  PGresult* res = PQexec(conn, sql.c_str());
  auto status = PQresultStatus(res);
  PQclear(res);
  return status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
}

} // namespace

namespace {

// These mandatory foundation cases use real schema/bootstrap and allocator
// code. The shared fixture barrier settles producers before any destructive
// DDL.
struct TypedSchemaFixture {
  plinth::Config::Database db{pg_config()};
  std::unique_ptr<plinth::ws_test::TestPg> pg;

  TypedSchemaFixture() {
    plinth::ws_test::reset_schema(db);
    pg = std::make_unique<plinth::ws_test::TestPg>(db);
    REQUIRE(PQstatus(pg->conn) == CONNECTION_OK);
  }
  ~TypedSchemaFixture() { plinth::ws_test::settle_fixture_work_or_exit(); }
  TypedSchemaFixture(const TypedSchemaFixture&) = delete;
  auto operator=(const TypedSchemaFixture&) -> TypedSchemaFixture& = delete;

  [[nodiscard]] auto scalar(const std::string& sql) const -> std::string {
    auto result = pg->exec(sql);
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    REQUIRE(PQgetisnull(result.get(), 0, 0) == 0);
    return PQgetvalue(result.get(), 0, 0);
  }

  auto command(const std::string& sql) const -> void {
    auto result = pg->exec(sql);
    INFO(PQresultErrorMessage(result.get()));
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }

  auto package(const std::string& name, const std::string& version = "1.0.0",
               const std::string& predecessor = "") const -> std::string {
    auto result = pg->exec_params(
        "INSERT INTO plinth.packages(name,version,state,provenance,"
        "manifest_json,entry_point,manifest_checksum,supersedes_id) "
        "VALUES ($1,$2,'ACTIVATING','user','{}','server/main.js','test',"
        "NULLIF($3,'')::uuid) RETURNING id::text",
        {name, version, predecessor});
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    return PQgetvalue(result.get(), 0, 0);
  }

  auto panel(
      const std::string& package_id, const std::string& panel_id,
      plinth::packages::PanelType type = plinth::packages::PanelType::FLOAT,
      std::uint32_t marker = 2) const -> void {
    auto registered = plinth::packages::register_panel(
        *pg->conn, {.package_id = package_id,
                    .panel_id = panel_id,
                    .panel_type = type,
                    .slot_type = std::nullopt,
                    .declaration = {{"id", panel_id}, {"type", "float"}},
                    .validated_format_version = marker,
                    .logical_install_order = std::nullopt});
    INFO((registered ? "registered" : registered.error()));
    REQUIRE(registered.has_value());
  }

  [[nodiscard]] auto rank(const std::string& package_id,
                          const std::string& panel_id) const -> std::string {
    return scalar("SELECT COALESCE(logical_install_order::text,'NULL') FROM "
                  "plinth.panels WHERE package_id='" +
                  package_id + "' AND panel_id='" + panel_id + "'");
  }

  auto finalize(const std::string& package_id) const -> void {
    auto result =
        plinth::packages::finalize_panel_install_order(*pg->conn, package_id);
    INFO((result ? "finalized" : result.error()));
    REQUIRE(result.has_value());
  }

  auto activate(const std::string& package_id) const -> void {
    command("BEGIN");
    finalize(package_id);
    command("UPDATE plinth.packages SET state='ACTIVE' WHERE id='" +
            package_id + "'");
    command("COMMIT");
  }
};

// On any fatal assertion the held transaction is rolled back BEFORE joining
// the worker. The worker captures its connection owner, never a borrowed
// PGconn.
struct BlockedOrderWorker {
  plinth::ws_test::TestPg& first;
  std::future<std::string> result;
  std::thread thread;

  BlockedOrderWorker(plinth::ws_test::TestPg& held,
                     std::shared_ptr<plinth::ws_test::TestPg> second,
                     std::string package_id, std::string lock_timeout = {},
                     std::string statement_timeout = "5s")
      : first(held) {
    std::promise<std::string> promise;
    result = promise.get_future();
    thread = std::thread([second = std::move(second),
                          package_id = std::move(package_id),
                          lock_timeout = std::move(lock_timeout),
                          statement_timeout = std::move(statement_timeout),
                          promise = std::move(promise)]() mutable {
      try {
        auto begin = second->exec("BEGIN");
        if (PQresultStatus(begin.get()) != PGRES_COMMAND_OK) {
          promise.set_value("worker BEGIN failed");
          return;
        }
        auto statement = second->exec_params(
            "SELECT set_config('statement_timeout',$1,true)",
            {statement_timeout});
        if (PQresultStatus(statement.get()) != PGRES_TUPLES_OK) {
          auto rollback = second->exec("ROLLBACK");
          promise.set_value("worker statement timeout setup failed");
          return;
        }
        if (!lock_timeout.empty()) {
          auto timeout = second->exec_params(
              "SELECT set_config('lock_timeout',$1,true)", {lock_timeout});
          if (PQresultStatus(timeout.get()) != PGRES_TUPLES_OK) {
            auto rollback = second->exec("ROLLBACK");
            promise.set_value("worker lock timeout setup failed");
            return;
          }
        }
        auto assigned = plinth::packages::finalize_panel_install_order(
            *second->conn, package_id);
        if (!assigned) {
          const auto error =
              assigned.error() + ": " + PQerrorMessage(second->conn);
          const auto aborted =
              PQtransactionStatus(second->conn) == PQTRANS_INERROR;
          auto rollback = second->exec("ROLLBACK");
          promise.set_value(
              aborted && PQresultStatus(rollback.get()) == PGRES_COMMAND_OK &&
                      PQtransactionStatus(second->conn) == PQTRANS_IDLE
                  ? "owner rolled back allocator refusal: " + error
                  : "worker rollback failed after allocator refusal: " + error);
          return;
        }
        auto active = second->exec_params(
            "UPDATE plinth.packages SET state='ACTIVE' WHERE id=$1::uuid",
            {package_id});
        if (PQresultStatus(active.get()) != PGRES_COMMAND_OK) {
          auto rollback = second->exec("ROLLBACK");
          promise.set_value("worker ACTIVE failed");
          return;
        }
        auto commit = second->exec("COMMIT");
        promise.set_value(PQresultStatus(commit.get()) == PGRES_COMMAND_OK
                              ? std::string{}
                              : "worker COMMIT failed");
      } catch (const std::exception& error) {
        promise.set_value(error.what());
      }
    });
  }
  ~BlockedOrderWorker() {
    if (PQtransactionStatus(first.conn) == PQTRANS_INTRANS ||
        PQtransactionStatus(first.conn) == PQTRANS_INERROR) {
      auto rollback = first.exec("ROLLBACK");
    }
    if (thread.joinable()) {
      thread.join();
    }
  }
  BlockedOrderWorker(const BlockedOrderWorker&) = delete;
  auto operator=(const BlockedOrderWorker&) -> BlockedOrderWorker& = delete;
};

} // namespace

TEST_CASE("typed surface schema preserves legacy provenance during retained "
          "bootstrap",
          "[typed-surface-foundation][packages_schema][integration]") {
  REQUIRE(pg_available());
  TypedSchemaFixture s;
  REQUIRE(s.scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator WHERE id=1") == "0");
  REQUIRE(
      s.scalar(
          "SELECT numeric_precision::text || ':' || numeric_scale::text "
          "FROM information_schema.columns WHERE table_schema='plinth' "
          "AND table_name='panels' AND column_name='logical_install_order'") ==
      "20:0");
  const auto old = s.package("retained");
  s.panel(old, "legacy", plinth::packages::PanelType::PRIMARY, 0);
  s.command("UPDATE plinth.panels SET declaration='"
            "{\"format_version\":2,\"type\":\"float\",\"capability\":"
            "\"retained:1:read\"}' "
            "WHERE package_id='" +
            old + "'");
  const auto declaration =
      s.scalar("SELECT declaration::text FROM plinth.panels");
  // Manufacture the exact pre-foundation table shape, not a JSON-only model.
  s.command("ALTER TABLE plinth.panels DROP COLUMN logical_install_order, "
            "DROP COLUMN validated_format_version");
  s.command("DROP TABLE plinth.handler_install_order_allocator");
  plinth::ws_test::settle_fixture_work();
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(
      s.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false));
  REQUIRE(
      s.scalar("SELECT validated_format_version::text FROM plinth.panels") ==
      "0");
  REQUIRE(s.rank(old, "legacy") == "NULL");
  REQUIRE(s.scalar("SELECT panel_type FROM plinth.panels") == "primary");
  REQUIRE(s.scalar("SELECT declaration::text FROM plinth.panels") ==
          declaration);
  s.command("UPDATE plinth.handler_install_order_allocator SET "
            "last_value=9007199254740993 WHERE id=1");
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(
      s.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false));
  REQUIRE(s.scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator WHERE id=1") ==
          "9007199254740993");
  s.command("BEGIN");
  s.finalize(old);
  s.command("COMMIT");
  REQUIRE(s.rank(old, "legacy") == "NULL");

  for (const auto* value : {"-1", "18446744073709551616"}) {
    auto rejected = s.pg->exec("UPDATE plinth.handler_install_order_allocator "
                               "SET last_value=" +
                               std::string{value});
    REQUIRE(PQresultStatus(rejected.get()) == PGRES_FATAL_ERROR);
  }
  auto marker =
      s.pg->exec("UPDATE plinth.panels SET validated_format_version=1");
  REQUIRE(PQresultStatus(marker.get()) == PGRES_FATAL_ERROR);
  auto legacy_rank =
      s.pg->exec("UPDATE plinth.panels SET logical_install_order=1");
  REQUIRE(PQresultStatus(legacy_rank.get()) == PGRES_FATAL_ERROR);
  SECTION("a missing retained allocator row is not reseeded") {
    s.command("DELETE FROM plinth.handler_install_order_allocator");
    REQUIRE_THROWS(plinth::db::bootstrap_schema(
        s.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false));
    REQUIRE(
        s.scalar(
            "SELECT count(*) FROM plinth.handler_install_order_allocator") ==
        "0");
  }
  SECTION("a missing retained allocator table is not reconstructed") {
    s.command("DROP TABLE plinth.handler_install_order_allocator");
    REQUIRE_THROWS(plinth::db::bootstrap_schema(
        s.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false));
    REQUIRE(
        s.scalar("SELECT to_regclass('plinth.handler_install_order_allocator') "
                 "IS NULL") == "t");
  }
  SECTION("a matching constraint name with weaker semantics is refused") {
    s.command("ALTER TABLE plinth.panels DROP CONSTRAINT "
              "chk_panels_logical_install_order, ADD CONSTRAINT "
              "chk_panels_logical_install_order CHECK(TRUE)");
    REQUIRE_THROWS(plinth::db::bootstrap_schema(
        s.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false));
    REQUIRE(
        s.scalar("SELECT validated_format_version::text FROM plinth.panels") ==
        "0");
  }
}

TEST_CASE("typed float order is transactional exact and lifetime-stable",
          "[typed-surface-foundation][packages_schema][integration]") {
  REQUIRE(pg_available());
  TypedSchemaFixture s;
  const auto policy = s.package("allocator-timeout-policy");
  s.panel(policy, "handler");
  for (const auto* timeout : {"0", "25ms", "20s"}) {
    INFO("original allocator lock timeout: " << timeout);
    s.command("BEGIN");
    s.command("SET LOCAL lock_timeout='" + std::string{timeout} + "'");
    s.command("SET LOCAL statement_timeout='3s'");
    const auto original_lock = s.scalar("SHOW lock_timeout");
    const auto original_statement = s.scalar("SHOW statement_timeout");
    s.finalize(policy);
    REQUIRE(s.scalar("SHOW lock_timeout") == original_lock);
    REQUIRE(s.scalar("SHOW statement_timeout") == original_statement);
    REQUIRE(s.rank(policy, "handler") == "1");
    s.command("ROLLBACK");
    REQUIRE(s.rank(policy, "handler") == "NULL");
    REQUIRE(s.scalar("SELECT last_value::text FROM "
                     "plinth.handler_install_order_allocator") == "0");
  }
  const auto invalid = s.package("invalid-rank-input");
  for (const auto* value :
       {"1.5", "01", "1e1", "+1", " 1", "-1", "0", "18446744073709551616"}) {
    const auto result = plinth::packages::register_panel(
        *s.pg->conn, {.package_id = invalid,
                      .panel_id = "invalid",
                      .panel_type = plinth::packages::PanelType::FLOAT,
                      .slot_type = std::nullopt,
                      .declaration = nlohmann::json::object(),
                      .validated_format_version = 2,
                      .logical_install_order = std::string{value}});
    INFO("noncanonical order: " << value);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(PQtransactionStatus(s.pg->conn) == PQTRANS_IDLE);
  }
  REQUIRE(s.scalar("SELECT count(*) FROM plinth.panels WHERE package_id='" +
                   invalid + "'") == "0");
  const auto first = s.package("ordered");
  s.panel(first, "zeta");
  s.panel(first, "alpha");
  s.panel(first, "primary", plinth::packages::PanelType::PRIMARY);
  s.panel(first, "tray", plinth::packages::PanelType::TRAY);
  REQUIRE_FALSE(
      plinth::packages::finalize_panel_install_order(*s.pg->conn, first));
  s.command("UPDATE plinth.handler_install_order_allocator SET "
            "last_value=9007199254740992 WHERE id=1");
  s.command("BEGIN");
  s.finalize(first);
  REQUIRE(s.rank(first, "alpha") == "9007199254740993");
  REQUIRE(s.rank(first, "zeta") == "9007199254740994");
  REQUIRE(s.rank(first, "primary") == "NULL");
  REQUIRE(s.rank(first, "tray") == "NULL");
  s.command("ROLLBACK");
  REQUIRE(s.rank(first, "alpha") == "NULL");
  REQUIRE(s.scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator") ==
          "9007199254740992");
  s.activate(first);
  const auto alpha_rank = s.rank(first, "alpha");
  s.command("BEGIN");
  s.finalize(first); // Fully committed retries are idempotent.
  s.command("COMMIT");
  REQUIRE(s.rank(first, "alpha") == alpha_rank);

  const auto second = s.package("ordered", "2.0.0", first);
  s.panel(second, "alpha");
  s.panel(second, "primary"); // A type change allocates a new identity.
  s.panel(second, "new");
  s.command("BEGIN");
  s.finalize(second);
  s.command("UPDATE plinth.packages SET state='SUPERSEDED' WHERE id='" + first +
            "'");
  s.command("UPDATE plinth.packages SET state='ACTIVE' WHERE id='" + second +
            "'");
  s.command("COMMIT");
  REQUIRE(s.rank(second, "alpha") == alpha_rank);
  REQUIRE(s.rank(second, "new") == "9007199254740995");
  REQUIRE(s.rank(second, "primary") == "9007199254740996");
  s.command("DELETE FROM plinth.packages WHERE id='" + first + "'");
  REQUIRE(s.rank(second, "alpha") ==
          alpha_rank); // Predecessor GC retains copy.
  s.command("UPDATE plinth.packages SET state='DISABLED' WHERE id='" + second +
            "'");
  s.command("BEGIN");
  s.finalize(second);
  s.command("COMMIT");
  REQUIRE(s.rank(second, "alpha") == alpha_rank);
  const auto third = s.package("ordered", "3.0.0", second);
  s.panel(third, "alpha");
  s.panel(third, "zeta"); // Removed in v2: never resurrect the v1 order.
  s.command("BEGIN");
  s.finalize(third);
  s.command("UPDATE plinth.packages SET state='SUPERSEDED' WHERE id='" +
            second + "'");
  s.command("UPDATE plinth.packages SET state='ACTIVE' WHERE id='" + third +
            "'");
  s.command("COMMIT");
  REQUIRE(s.rank(third, "alpha") == alpha_rank);
  REQUIRE(s.rank(third, "zeta") == "9007199254740997");
  s.command("DELETE FROM plinth.packages WHERE id='" + second + "'");
  REQUIRE(s.rank(third, "alpha") == alpha_rank);
  s.command("DELETE FROM plinth.packages WHERE id='" + third + "'");
  const auto reinstall = s.package("ordered", "4.0.0");
  s.panel(reinstall, "alpha");
  s.activate(reinstall);
  REQUIRE(s.rank(reinstall, "alpha") == "9007199254740998");

  const auto missing = s.package("missing-rank");
  s.panel(missing, "lost");
  s.command("UPDATE plinth.packages SET state='ACTIVE' WHERE id='" + missing +
            "'");
  s.command("BEGIN");
  REQUIRE_FALSE(
      plinth::packages::finalize_panel_install_order(*s.pg->conn, missing));
  s.command("ROLLBACK");
  REQUIRE(s.rank(missing, "lost") == "NULL");

  const auto exhausted = s.package("exhausted");
  s.panel(exhausted, "a");
  s.panel(exhausted, "b");
  s.command("UPDATE plinth.handler_install_order_allocator SET "
            "last_value=18446744073709551614");
  s.command("BEGIN");
  REQUIRE_FALSE(
      plinth::packages::finalize_panel_install_order(*s.pg->conn, exhausted));
  REQUIRE(s.rank(exhausted, "a") == "NULL");
  REQUIRE(s.rank(exhausted, "b") == "NULL");
  REQUIRE(s.scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator") ==
          "18446744073709551614");
  s.command("ROLLBACK");
  s.command("DELETE FROM plinth.panels WHERE package_id='" + exhausted +
            "' AND panel_id='b'");
  s.activate(exhausted);
  REQUIRE(s.rank(exhausted, "a") == "18446744073709551615");
  const auto overflow = s.package("overflow");
  s.panel(overflow, "a");
  s.command("BEGIN");
  REQUIRE_FALSE(
      plinth::packages::finalize_panel_install_order(*s.pg->conn, overflow));
  s.command("ROLLBACK");
  REQUIRE(s.rank(overflow, "a") == "NULL");
  REQUIRE(s.scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator") ==
          "18446744073709551615");
}

TEST_CASE("typed float order allocation follows committed installation order",
          "[typed-surface-foundation][packages_schema][integration]") {
  REQUIRE(pg_available());
  TypedSchemaFixture s;
  const auto first = s.package("first-order");
  const auto second = s.package("second-order");
  s.panel(first, "handler");
  s.panel(second, "handler");
  struct TimeoutControl {
    const char* lock_timeout;
    const char* statement_timeout;
    std::chrono::seconds observation_bound;
  };
  for (const auto& control :
       {TimeoutControl{"25ms", "5s", std::chrono::seconds{3}},
        TimeoutControl{"0", "15s", std::chrono::seconds{20}}}) {
    INFO("held allocator with original lock_timeout=" << control.lock_timeout);
    // A real held singleton must respect a shorter configured limit and
    // impose the default ceiling when locking was originally unlimited.
    // The 15s statement deadline deliberately exceeds the 10s lock ceiling:
    // without the cap that control fails with a statement timeout instead.
    // Both failures must roll back without rank/high-water writes.
    auto bounded_connection = std::make_shared<plinth::ws_test::TestPg>(s.db);
    REQUIRE(PQstatus(bounded_connection->conn) == CONNECTION_OK);
    s.command("BEGIN");
    auto held = s.pg->exec("SELECT last_value FROM "
                           "plinth.handler_install_order_allocator "
                           "WHERE id=1 FOR UPDATE");
    REQUIRE(PQresultStatus(held.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(held.get()) == 1);
    BlockedOrderWorker timed_out{*s.pg, std::move(bounded_connection), second,
                                 control.lock_timeout,
                                 control.statement_timeout};
    REQUIRE(timed_out.result.wait_for(control.observation_bound) ==
            std::future_status::ready);
    const auto refusal = timed_out.result.get();
    timed_out.thread.join();
    INFO(refusal);
    REQUIRE(refusal.starts_with("owner rolled back allocator refusal: "));
    REQUIRE(refusal.find("lock timeout") != std::string::npos);
    REQUIRE(refusal.find("statement timeout") == std::string::npos);
    REQUIRE(s.rank(first, "handler") == "NULL");
    REQUIRE(s.rank(second, "handler") == "NULL");
    REQUIRE(s.scalar("SELECT last_value::text FROM "
                     "plinth.handler_install_order_allocator") == "0");
    s.command("ROLLBACK");
    REQUIRE(PQtransactionStatus(s.pg->conn) == PQTRANS_IDLE);
  }
  auto worker_connection = std::make_shared<plinth::ws_test::TestPg>(s.db);
  REQUIRE(PQstatus(worker_connection->conn) == CONNECTION_OK);
  const auto worker_pid = std::to_string(PQbackendPID(worker_connection->conn));
  const auto owner_pid = std::to_string(PQbackendPID(s.pg->conn));
  std::string blocking_query{"SELECT "};
  blocking_query += owner_pid;
  blocking_query += "=ANY(pg_blocking_pids(";
  blocking_query += worker_pid;
  blocking_query += "))";
  plinth::ws_test::TestPg observer{s.db};
  REQUIRE(PQstatus(observer.conn) == CONNECTION_OK);
  s.command("BEGIN");
  s.finalize(first);
  BlockedOrderWorker worker{*s.pg, std::move(worker_connection), second};
  bool blocked = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (std::chrono::steady_clock::now() < deadline) {
    auto lock = observer.exec(blocking_query);
    REQUIRE(PQresultStatus(lock.get()) == PGRES_TUPLES_OK);
    if (std::string_view{PQgetvalue(lock.get(), 0, 0)} == "t") {
      blocked = true;
      break;
    }
    if (worker.result.wait_for(std::chrono::milliseconds{0}) ==
        std::future_status::ready) {
      break;
    }
  }
  REQUIRE(blocked); // Actual PostgreSQL row-lock dependency, not timing luck.
  REQUIRE(worker.result.wait_for(std::chrono::milliseconds{0}) ==
          std::future_status::timeout);
  s.command("UPDATE plinth.packages SET state='ACTIVE' WHERE id='" + first +
            "'");
  s.command("COMMIT");
  REQUIRE(worker.result.wait_for(std::chrono::seconds{5}) ==
          std::future_status::ready);
  const auto worker_error = worker.result.get();
  INFO(worker_error);
  REQUIRE(worker_error.empty());
  REQUIRE(s.rank(first, "handler") == "1");
  REQUIRE(s.rank(second, "handler") == "2");
  REQUIRE(s.scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator") == "2");
}

// ── Unit: schema.sql source inspection ─────────────────────────

TEST_CASE("schema.sql defines plinth.packages with all 12 states",
          "[packages_schema][unit]") {
  auto sql = read_schema_sql();
  REQUIRE(sql.find("CREATE TABLE plinth.packages") != std::string::npos);
  for (auto const* state :
       {"UPLOADING", "VALIDATING", "MIGRATING", "REGISTERING", "EXTRACTING",
        "ACTIVATING", "ACTIVE", "ACTIVE_FLAGGED", "DISABLED", "INSTALL_FAILED",
        "UNINSTALLING", "SUPERSEDED"}) {
    INFO("state token not found: " << state);
    REQUIRE(sql.find(std::string{"'"} + state + "'") != std::string::npos);
  }
}

TEST_CASE("schema.sql declares retired_at + supersedes_id columns for 0.4.5",
          "[packages_schema][unit]") {
  auto sql = read_schema_sql();
  REQUIRE(sql.find("retired_at") != std::string::npos);
  REQUIRE(sql.find("supersedes_id") != std::string::npos);
  REQUIRE(sql.find("REFERENCES plinth.packages(id) ON DELETE SET NULL") !=
          std::string::npos);
}

TEST_CASE("schema.sql defines plinth.panels with panel_type + slot_type CHECK",
          "[packages_schema][unit]") {
  auto sql = read_schema_sql();
  REQUIRE(sql.find("CREATE TABLE plinth.panels") != std::string::npos);
  REQUIRE(sql.find("'primary'") != std::string::npos);
  REQUIRE(sql.find("'float'") != std::string::npos);
  REQUIRE(sql.find("'settings'") != std::string::npos);
  REQUIRE(sql.find("'tray'") != std::string::npos);
  REQUIRE(sql.find("'home'") != std::string::npos);
}

TEST_CASE("schema.sql declares the partial unique indexes + state index",
          "[packages_schema][unit]") {
  auto sql = read_schema_sql();
  REQUIRE(sql.find("uniq_packages_name_active") != std::string::npos);
  REQUIRE(sql.find("uniq_packages_mount_active") != std::string::npos);
  REQUIRE(sql.find("idx_packages_state") != std::string::npos);
  REQUIRE(sql.find("idx_packages_supersedes") != std::string::npos);
  REQUIRE(sql.find("uniq_packages_name_application_ready") !=
          std::string::npos);
  REQUIRE(sql.find("chk_packages_application_ready_state") !=
          std::string::npos);
  REQUIRE(sql.find("NOT NULL DEFAULT FALSE") != std::string::npos);
}

// ── Integration: bootstrap_schema creates tables + constraints hold ──

TEST_CASE("bootstrap_schema creates plinth.packages and plinth.panels",
          "[packages_schema][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto db = pg_config();
  drop_plinth_schema(db);
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, true));

  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);

  PGresult* res = PQexec(
      conn,
      "SELECT table_name FROM information_schema.tables "
      "WHERE table_schema='plinth' AND table_name IN ('packages','panels') "
      "ORDER BY table_name");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 2);
  PQclear(res);

  // state CHECK constraint is operational.
  REQUIRE_FALSE(exec_ok(
      conn,
      "INSERT INTO plinth.packages "
      "(name, version, state, provenance, manifest_json, entry_point, "
      "manifest_checksum) "
      "VALUES ('bad', '0.0.1', 'NOT_A_STATE', 'user', '{}'::jsonb, 'x', 'y')"));

  PQfinish(conn);
  drop_plinth_schema(db);
}

TEST_CASE("plinth.panels CASCADE-deletes with its owning package",
          "[packages_schema][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto db = pg_config();
  drop_plinth_schema(db);
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);

  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);

  REQUIRE(exec_ok(
      conn, "INSERT INTO plinth.packages "
            "(id, name, version, state, provenance, manifest_json, "
            "entry_point, manifest_checksum) "
            "VALUES ('11111111-1111-1111-1111-111111111111', "
            "        'p1', '0.0.1', 'ACTIVE', 'user', '{}'::jsonb, 'x', 'y')"));
  REQUIRE(exec_ok(conn, "INSERT INTO plinth.panels "
                        "(package_id, panel_id, panel_type, declaration) "
                        "VALUES ('11111111-1111-1111-1111-111111111111', "
                        "        'main', 'primary', '{}'::jsonb)"));

  REQUIRE(exec_ok(conn, "DELETE FROM plinth.packages "
                        "WHERE id = '11111111-1111-1111-1111-111111111111'"));

  PGresult* res =
      PQexec(conn, "SELECT COUNT(*) FROM plinth.panels "
                   "WHERE package_id = '11111111-1111-1111-1111-111111111111'");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(res, 0, 0)} == "0");
  PQclear(res);

  PQfinish(conn);
  drop_plinth_schema(db);
}

TEST_CASE("uniq_packages_name_active blocks a second ACTIVE row with same name",
          "[packages_schema][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto db = pg_config();
  drop_plinth_schema(db);
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);

  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);

  auto insert = [&](const std::string& id, const std::string& version,
                    const std::string& state) {
    return exec_ok(conn, "INSERT INTO plinth.packages "
                         "(id, name, version, state, provenance, "
                         "manifest_json, entry_point, manifest_checksum) "
                         "VALUES ('" +
                             id + "', 'notes', '" + version + "', '" + state +
                             "', 'user', '{}'::jsonb, 'x', 'y')");
  };

  REQUIRE(insert("22222222-2222-2222-2222-222222222222", "1.0.0", "ACTIVE"));
  // Second ACTIVE row for the same name must fail.
  REQUIRE_FALSE(
      insert("33333333-3333-3333-3333-333333333333", "1.0.1", "ACTIVE"));
  // But a concurrent INSTALL_FAILED row is fine — the partial index only
  // covers {ACTIVE, ACTIVE_FLAGGED, DISABLED}.
  REQUIRE(insert("44444444-4444-4444-4444-444444444444", "1.0.2",
                 "INSTALL_FAILED"));

  PQfinish(conn);
  drop_plinth_schema(db);
}

TEST_CASE(
    "application_ready is false by default and constrained to active states",
    "[packages_schema][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto db = pg_config();
  drop_plinth_schema(db);
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);

  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  REQUIRE(exec_ok(conn,
                  "INSERT INTO plinth.packages "
                  "(id, name, version, state, provenance, manifest_json, "
                  "entry_point, manifest_checksum) VALUES "
                  "('77777777-7777-7777-7777-777777777777', 'ready-test', "
                  "'1.0.0', 'VALIDATING', 'user', '{}'::jsonb, 'x', 'y')"));

  PGresult* default_value =
      PQexec(conn, "SELECT application_ready FROM plinth.packages WHERE "
                   "id='77777777-7777-7777-7777-777777777777'");
  REQUIRE(PQresultStatus(default_value) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(default_value, 0, 0)} == "f");
  PQclear(default_value);

  REQUIRE_FALSE(
      exec_ok(conn, "UPDATE plinth.packages SET application_ready=true WHERE "
                    "id='77777777-7777-7777-7777-777777777777'"));
  REQUIRE(exec_ok(conn, "UPDATE plinth.packages SET state='ACTIVE', "
                        "application_ready=true WHERE "
                        "id='77777777-7777-7777-7777-777777777777'"));

  PQfinish(conn);
  drop_plinth_schema(db);
}

TEST_CASE("application_ready unique index prevents split-brain package names",
          "[packages_schema][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto db = pg_config();
  drop_plinth_schema(db);
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);

  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  REQUIRE(exec_ok(conn, "DROP INDEX plinth.uniq_packages_name_active"));
  auto insert = [&](std::string_view id, std::string_view version, bool ready) {
    return exec_ok(conn, "INSERT INTO plinth.packages "
                         "(id, name, version, state, provenance, "
                         "manifest_json, entry_point, manifest_checksum, "
                         "application_ready) VALUES ('" +
                             std::string{id} + "', 'split-test', '" +
                             std::string{version} +
                             "', 'ACTIVE', 'user', '{}'::jsonb, 'x', 'y', " +
                             (ready ? "true" : "false") + ")");
  };
  REQUIRE(insert("88888888-8888-8888-8888-888888888888", "1.0.0", true));
  REQUIRE(insert("99999999-9999-9999-9999-999999999999", "1.1.0", false));
  REQUIRE_FALSE(
      exec_ok(conn, "UPDATE plinth.packages SET application_ready=true WHERE "
                    "id='99999999-9999-9999-9999-999999999999'"));

  PQfinish(conn);
  drop_plinth_schema(db);
}

TEST_CASE("SUPERSEDED coexists with a new ACTIVE row for the same name",
          "[packages_schema][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }
  auto db = pg_config();
  drop_plinth_schema(db);
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);

  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);

  // Old row lands at SUPERSEDED with retired_at + NULL supersedes_id.
  REQUIRE(exec_ok(
      conn,
      "INSERT INTO plinth.packages "
      "(id, name, version, state, provenance, manifest_json, entry_point, "
      " manifest_checksum, retired_at) "
      "VALUES ('55555555-5555-5555-5555-555555555555', "
      "        'notes', '1.0.0', 'SUPERSEDED', 'user', '{}'::jsonb, 'x', 'y', "
      "        NOW())"));
  // New ACTIVE row for the same name points at the old via supersedes_id.
  REQUIRE(exec_ok(
      conn,
      "INSERT INTO plinth.packages "
      "(id, name, version, state, provenance, manifest_json, entry_point, "
      " manifest_checksum, supersedes_id) "
      "VALUES ('66666666-6666-6666-6666-666666666666', "
      "        'notes', '1.1.0', 'ACTIVE', 'user', '{}'::jsonb, 'x', 'y', "
      "        '55555555-5555-5555-5555-555555555555')"));

  // ON DELETE SET NULL: deleting the SUPERSEDED row nulls the FK on the
  // survivor rather than blocking the delete.
  REQUIRE(exec_ok(conn, "DELETE FROM plinth.packages "
                        "WHERE id = '55555555-5555-5555-5555-555555555555'"));
  PGresult* res =
      PQexec(conn, "SELECT supersedes_id FROM plinth.packages "
                   "WHERE id = '66666666-6666-6666-6666-666666666666'");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 1);
  REQUIRE(PQgetisnull(res, 0, 0) == 1);
  PQclear(res);

  PQfinish(conn);
  drop_plinth_schema(db);
}
