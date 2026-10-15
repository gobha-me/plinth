// SPDX-License-Identifier: MIT

#include "../js/async_bridge_fixture.hpp"
#include "kernel/audit/detail_encoding.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/lifecycle/shutdown_coordinator.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace std::chrono_literals;

namespace {

class AuditPg {
 public:
  explicit AuditPg(const plinth::Config::Database& db)
      : connection(PQconnectdb(
                       ("host=" + db.host + " port=" + std::to_string(db.port) +
                        " dbname=" + db.database + " user=" + db.user +
                        " password=" + db.password + " connect_timeout=3")
                           .c_str()),
                   PQfinish) {
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  }

  auto command(const char* sql) -> void {
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }

  [[nodiscard]] auto audit_count(const char* action) -> int {
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(connection.get(),
                     "SELECT count(*) FROM plinth.audit_log WHERE action=$1", 1,
                     nullptr, &action, nullptr, nullptr, 0),
        PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    return std::stoi(PQgetvalue(result.get(), 0, 0));
  }

  [[nodiscard]] auto read_detail(const char* action)
      -> std::pair<Json::Value, int> {
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(connection.get(),
                     "SELECT detail::text, detail_encoding "
                     "FROM plinth.audit_log WHERE action=$1",
                     1, nullptr, &action, nullptr, nullptr, 0),
        PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    const auto encoding = std::stoi(PQgetvalue(result.get(), 0, 1));
    const auto detail =
        plinth::audit::decode_detail(PQgetvalue(result.get(), 0, 0), encoding);
    REQUIRE(detail.has_value());
    return {*detail, encoding};
  }

 private:
  // Closing this connection also releases a held transaction lock if any
  // assertion unwinds the test, allowing the fixture coordinator to finish.
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection;
};

} // namespace

TEST_CASE("audit writers persist NUL details and preserve legacy payloads",
          "[js][audit][storage]") {
  using namespace plinth::async_bridge_test;
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  const auto cfg = test_config();
  reset_schema(cfg.db);
  AuditPg pg{cfg.db};
  Json::Value detail{Json::objectValue};
  detail["channel"] = std::string{"before\0after", 12};
  detail[std::string{"key\0end", 7}][0] = std::string{"\0", 1};
  detail["literal"] = R"(\u0000)";
  plinth::log::audit("test.audit.storage.async", detail,
                     plinth::log::AuditCtx{});
  plinth::log::audit_sync(cfg.db, "test.audit.storage.sync", detail);
  REQUIRE(plinth::log::wait_for_audits(2s));
  const auto async_row = pg.read_detail("test.audit.storage.async");
  const auto sync_row = pg.read_detail("test.audit.storage.sync");
  REQUIRE(async_row.first == detail);
  REQUIRE(sync_row.first == detail);
  REQUIRE(async_row.second == 1);
  REQUIRE(sync_row.second == 1);

  // Emulate a retained pre-upgrade table and caller-controlled marker-shaped
  // payload. The upgrade must neither reinterpret nor rewrite this old row.
  pg.command("DELETE FROM plinth.audit_log");
  pg.command("ALTER TABLE plinth.audit_log DROP COLUMN detail_encoding");
  pg.command(
      "INSERT INTO plinth.audit_log(action,detail,node_id) "
      "VALUES ('test.audit.storage.legacy', "
      "'{\"detail_encoding\":1,\"json\":\"not encoded\"}', 'test-node')");
  plinth::db::bootstrap_schema(
      cfg.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false);
  plinth::db::bootstrap_schema(
      cfg.db, std::string{CMAKE_SOURCE_DIR} + "/migrations", false);
  const auto legacy = pg.read_detail("test.audit.storage.legacy");
  REQUIRE(legacy.second == 0);
  REQUIRE(legacy.first["detail_encoding"].asInt() == 1);
  REQUIRE(legacy.first["json"].asString() == "not encoded");
  Json::Value ordinary{Json::objectValue};
  ordinary["literal"] = R"(\u0000)";
  plinth::log::audit("test.audit.storage.ordinary", ordinary,
                     plinth::log::AuditCtx{});
  REQUIRE(plinth::log::wait_for_audits(2s));
  const auto plain = pg.read_detail("test.audit.storage.ordinary");
  REQUIRE(plain.second == 0);
  REQUIRE(plain.first == ordinary);

  const auto migrations = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "TYPE INTEGER");
  REQUIRE_THROWS_AS(plinth::db::bootstrap_schema(cfg.db, migrations, false),
                    std::runtime_error);
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "TYPE SMALLINT");
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "SET DEFAULT 1");
  REQUIRE_THROWS_AS(plinth::db::bootstrap_schema(cfg.db, migrations, false),
                    std::runtime_error);
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "SET DEFAULT 0");
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "DROP NOT NULL");
  REQUIRE_THROWS_AS(plinth::db::bootstrap_schema(cfg.db, migrations, false),
                    std::runtime_error);
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "SET NOT NULL");
  pg.command("ALTER TABLE plinth.audit_log DROP CONSTRAINT "
             "chk_audit_log_detail_encoding");
  REQUIRE_THROWS_AS(plinth::db::bootstrap_schema(cfg.db, migrations, false),
                    std::runtime_error);
  pg.command("ALTER TABLE plinth.audit_log ADD CONSTRAINT "
             "chk_audit_log_detail_encoding CHECK (detail_encoding IN (0, 1))");
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(cfg.db, migrations, false));

  // pg_attrdef holds generation expressions as well as defaults. A generated
  // constant zero is not a writable encoding column, despite matching the
  // type, expression, nullability and check constraint of the real default.
  pg.command("ALTER TABLE plinth.audit_log DROP COLUMN detail_encoding");
  pg.command("ALTER TABLE plinth.audit_log ADD COLUMN detail_encoding "
             "SMALLINT GENERATED ALWAYS AS (0) STORED NOT NULL "
             "CONSTRAINT chk_audit_log_detail_encoding "
             "CHECK (detail_encoding IN (0, 1))");
  REQUIRE_THROWS_AS(plinth::db::bootstrap_schema(cfg.db, migrations, false),
                    std::runtime_error);
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "DROP EXPRESSION");
  pg.command("ALTER TABLE plinth.audit_log ALTER COLUMN detail_encoding "
             "SET DEFAULT 0");
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(cfg.db, migrations, false));
}

TEST_CASE(
    "audit drain retains blocked writes before Drogon shutdown and retries",
    "[js][audit][isolated-audit-drain]") {
  using namespace plinth::async_bridge_test;
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  const auto cfg = test_config();
  reset_schema(cfg.db);
  AuditPg pg{cfg.db};
  pg.command("BEGIN");
  pg.command("LOCK TABLE plinth.audit_log IN ACCESS EXCLUSIVE MODE");

  Json::Value detail{Json::objectValue};
  detail["boundary"] = "accepted-before-seal";
  plinth::log::audit("test.audit.drain.accepted", detail,
                     plinth::log::AuditCtx{});
  REQUIRE_FALSE(plinth::log::wait_for_audits(0ms));

  bool stopped_drogon = false;
  auto hooks = plinth::lifecycle::production_shutdown_hooks();
  auto original_stop = std::move(hooks.stop_drogon);
  hooks.stop_drogon = [&stopped_drogon, original_stop](auto timeout) {
    stopped_drogon = true;
    return original_stop(timeout);
  };
  plinth::lifecycle::ShutdownCoordinator coordinator{std::move(hooks), 1s};
  const auto started = std::chrono::steady_clock::now();
  const auto failed = coordinator.quiesce();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK_FALSE(failed.clean);
  CHECK(failed.failed_step == "close_audit_gate");
  CHECK_FALSE(stopped_drogon);
  CHECK(elapsed < 3s);
  CHECK_FALSE(plinth::log::is_audit_ready());
  CHECK_FALSE(plinth::log::wait_for_audits(0ms));

  // This call cannot enter the default DB after the admission seal.
  plinth::log::audit("test.audit.drain.rejected", detail,
                     plinth::log::AuditCtx{});
  pg.command("COMMIT");
  REQUIRE(plinth::log::wait_for_audits(2s));
  REQUIRE(pg.audit_count("test.audit.drain.accepted") == 1);
  REQUIRE(pg.audit_count("test.audit.drain.rejected") == 0);
  REQUIRE(coordinator.quiesce().clean);
  REQUIRE(stopped_drogon);
  REQUIRE(coordinator.quiesce().clean);
  REQUIRE(plinth::log::wait_for_audits(0ms));
  // The existing process fixture joins Drogon and closes sinks after Catch.
}
