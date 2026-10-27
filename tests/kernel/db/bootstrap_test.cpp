#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <libpq-fe.h>
#include <memory>

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/db/connection_info.hpp"

// ── Unit tests (no PG required) ─────────────────────────────

TEST_CASE("load_schema_sql reads file contents", "[bootstrap][unit]") {
  namespace fs = std::filesystem;
  fs::path dir = "test_migrations_tmp";
  fs::path path = dir / "schema.sql";

  fs::create_directories(dir);
  {
    std::ofstream f(path);
    f << "CREATE SCHEMA IF NOT EXISTS plinth;\n"
      << "CREATE TABLE plinth.test (id int);\n";
  }

  auto sql = plinth::db::load_schema_sql(dir.string());

  fs::remove_all(dir);

  REQUIRE(sql.find("CREATE SCHEMA") != std::string::npos);
  REQUIRE(sql.find("plinth.test") != std::string::npos);
}

TEST_CASE("load_schema_sql throws on missing file", "[bootstrap][unit]") {
  REQUIRE_THROWS_AS(plinth::db::load_schema_sql("__no_such_dir__"),
                    std::runtime_error);
}

TEST_CASE("load_schema_sql throws on empty file", "[bootstrap][unit]") {
  namespace fs = std::filesystem;
  fs::path dir = "test_migrations_empty";
  fs::path path = dir / "schema.sql";

  fs::create_directories(dir);
  {
    std::ofstream f(path);
  } // create empty file

  REQUIRE_THROWS_AS(plinth::db::load_schema_sql(dir.string()),
                    std::runtime_error);

  fs::remove_all(dir);
}

// ── Integration tests (require PG) ──────────────────────────
// These tests connect to a real PostgreSQL instance.
// CI provides PG via a service container with env vars:
//   PLINTH_PG_HOST, PLINTH_PG_PORT, PLINTH_PG_USER,
//   PLINTH_PG_PASSWORD, PLINTH_PG_DATABASE

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

// Probe PG connectivity — skip integration tests if unreachable
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

// Helper: check if a table exists in plinth schema
auto table_exists(const plinth::Config::Database& db,
                  const std::string& table_name) -> bool {
  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password;

  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return false;
  }

  auto sql = "SELECT table_name FROM information_schema.tables "
             "WHERE table_schema = 'plinth' AND table_name = '" +
             table_name + "'";
  PGresult* res = PQexec(conn, sql.c_str());
  bool found = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
  PQclear(res);
  PQfinish(conn);
  return found;
}

// Helper: drop plinth schema for test isolation
auto drop_plinth_schema(const plinth::Config::Database& db) -> void {
  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password;

  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) == CONNECTION_OK) {
    PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  }
  PQfinish(conn);
}

struct OwnedBootstrapSchema {
  plinth::Config::Database db = pg_config();
  OwnedBootstrapSchema() { drop_plinth_schema(db); }
  ~OwnedBootstrapSchema() { drop_plinth_schema(db); }
  OwnedBootstrapSchema(const OwnedBootstrapSchema&) = delete;
  auto operator=(const OwnedBootstrapSchema&) -> OwnedBootstrapSchema& = delete;
};

using BootstrapResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
auto http_schema_query(PGconn& connection, const std::string& sql)
    -> BootstrapResult {
  BootstrapResult result{PQexec(&connection, sql.c_str()), PQclear};
  REQUIRE(result != nullptr);
  REQUIRE((PQresultStatus(result.get()) == PGRES_COMMAND_OK ||
           PQresultStatus(result.get()) == PGRES_TUPLES_OK));
  return result;
}

} // namespace

TEST_CASE("bootstrap_schema dev_mode creates tables",
          "[bootstrap][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }

  auto db = pg_config();
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";

  // Clean slate
  drop_plinth_schema(db);

  // Bootstrap in dev_mode
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, true));

  // Verify core tables exist
  REQUIRE(table_exists(db, "migrations"));
  REQUIRE(table_exists(db, "users"));
  REQUIRE(table_exists(db, "registration_invites"));
  REQUIRE(table_exists(db, "sessions"));
  REQUIRE(table_exists(db, "audit_log"));
  REQUIRE(table_exists(db, "realtime_outbox"));

  // Clean up
  drop_plinth_schema(db);
}

TEST_CASE("bootstrap_schema dev_mode is idempotent",
          "[bootstrap][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }

  auto db = pg_config();
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";

  drop_plinth_schema(db);

  // Run twice — second run should not fail
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, true));
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, true));

  REQUIRE(table_exists(db, "users"));

  drop_plinth_schema(db);
}

TEST_CASE("bootstrap_schema dev_mode removes extension schemas and roles",
          "[bootstrap][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }

  auto db = pg_config();
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  drop_plinth_schema(db);
  plinth::db::bootstrap_schema(db, migrations_dir, true);

  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password;
  PGconn* conn = PQconnectdb(conninfo.c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PQclear(PQexec(conn, "CREATE SCHEMA ext_dev_reset_probe"));
  PQclear(PQexec(conn, "CREATE ROLE ext_dev_reset_probe_role NOLOGIN"));
  PQfinish(conn);

  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, true));

  conn = PQconnectdb(conninfo.c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  auto* schema_result =
      PQexec(conn, "SELECT 1 FROM information_schema.schemata "
                   "WHERE schema_name = 'ext_dev_reset_probe'");
  REQUIRE(PQresultStatus(schema_result) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(schema_result) == 0);
  PQclear(schema_result);
  auto* role_result =
      PQexec(conn, "SELECT 1 FROM pg_roles "
                   "WHERE rolname = 'ext_dev_reset_probe_role'");
  REQUIRE(PQresultStatus(role_result) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(role_result) == 0);
  PQclear(role_result);
  PQfinish(conn);

  drop_plinth_schema(db);
}

TEST_CASE("bootstrap_schema non-dev creates schema on fresh DB",
          "[bootstrap][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }

  auto db = pg_config();
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";

  // Ensure clean state
  drop_plinth_schema(db);

  // Bootstrap without dev_mode — should detect fresh install
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, false));
  REQUIRE(table_exists(db, "users"));

  drop_plinth_schema(db);
}

TEST_CASE("bootstrap_schema non-dev skips existing schema",
          "[bootstrap][integration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }

  auto db = pg_config();
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";

  drop_plinth_schema(db);

  // First run: creates schema
  plinth::db::bootstrap_schema(db, migrations_dir, false);
  REQUIRE(table_exists(db, "users"));

  // Second run: should skip (no error)
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, false));
  REQUIRE(table_exists(db, "users"));
  REQUIRE(table_exists(db, "registration_invites"));

  drop_plinth_schema(db);
}

TEST_CASE("bootstrap_schema upgrades an existing install with invite storage",
          "[bootstrap][integration][registration]") {
  if (!pg_available()) {
    SKIP("PG not available (set PLINTH_PG_HOST to enable)");
  }

  auto db = pg_config();
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  drop_plinth_schema(db);
  plinth::db::bootstrap_schema(db, migrations_dir, false);

  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password;
  PGconn* conn = PQconnectdb(conninfo.c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  auto* dropped = PQexec(conn, "DROP TABLE plinth.registration_invites");
  REQUIRE(PQresultStatus(dropped) == PGRES_COMMAND_OK);
  PQclear(dropped);
  PQfinish(conn);
  REQUIRE_FALSE(table_exists(db, "registration_invites"));

  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, false));
  REQUIRE(table_exists(db, "registration_invites"));
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(db, migrations_dir, false));

  drop_plinth_schema(db);
}

TEST_CASE("HTTP claims bootstrap creates checked empty storage without legacy "
          "backfill",
          "[bootstrap][integration][http-claims-bootstrap]") {
  if (!pg_available()) SKIP("PG not available");
  OwnedBootstrapSchema fixture;
  const auto migrations = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(fixture.db, migrations, false));
  auto connection = std::unique_ptr<PGconn, decltype(&PQfinish)>(
      PQconnectdb(plinth::db::connection_info(fixture.db).c_str()), PQfinish);
  REQUIRE(connection);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  for (const auto* name :
       {"package_http_claim_receipts", "package_http_claim_reservations",
        "package_http_adapter_bindings"})
    REQUIRE(table_exists(fixture.db, name));
  http_schema_query(*connection, R"sql(
INSERT INTO plinth.packages(name,version,state,provenance,manifest_json,
entry_point,manifest_checksum) VALUES('legacy-http','1.0.0','ACTIVE','user',
'{"http_prefixes":["/legacy/**"],"handler_mode":"historical-unknown"}',
'server/main.js','unchanged');
DROP TABLE plinth.extension_http_dispatch_obligations;
DROP TABLE plinth.extension_http_pending_gate;
DROP TABLE plinth.package_http_adapter_bindings;
DROP TABLE plinth.package_http_claim_reservations;
DROP TABLE plinth.package_http_claim_receipts;
)sql");
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(fixture.db, migrations, false));
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(fixture.db, migrations, false));
  const auto rows = http_schema_query(*connection, R"sql(
SELECT state,manifest_checksum,manifest_json->>'handler_mode',
(SELECT count(*) FROM plinth.package_http_claim_receipts),
(SELECT count(*) FROM plinth.package_http_claim_reservations),
(SELECT count(*) FROM plinth.package_http_adapter_bindings),
(SELECT count(*) FROM plinth.package_artifact_receipts)
FROM plinth.packages WHERE name='legacy-http'
)sql");
  REQUIRE(PQntuples(rows.get()) == 1);
  REQUIRE(std::string{PQgetvalue(rows.get(), 0, 0)} == "ACTIVE");
  REQUIRE(std::string{PQgetvalue(rows.get(), 0, 1)} == "unchanged");
  REQUIRE(std::string{PQgetvalue(rows.get(), 0, 2)} == "historical-unknown");
  for (int column = 3; column < 7; ++column)
    REQUIRE(std::string{PQgetvalue(rows.get(), 0, column)} == "0");
}

TEST_CASE(
    "HTTP claims bootstrap refuses incompatible retained authority tables",
    "[bootstrap][integration][http-claims-bootstrap]") {
  if (!pg_available()) SKIP("PG not available");
  OwnedBootstrapSchema fixture;
  const auto migrations = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  REQUIRE_NOTHROW(plinth::db::bootstrap_schema(fixture.db, migrations, false));
  auto connection = std::unique_ptr<PGconn, decltype(&PQfinish)>(
      PQconnectdb(plinth::db::connection_info(fixture.db).c_str()), PQfinish);
  REQUIRE(connection);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  SECTION("receipt column") {
    http_schema_query(*connection,
                      "ALTER TABLE plinth.package_http_claim_receipts "
                      "ALTER COLUMN http_prefixes DROP NOT NULL");
  }
  SECTION("reservation column") {
    http_schema_query(*connection,
                      "ALTER TABLE plinth.package_http_claim_reservations "
                      "ALTER COLUMN phase DROP NOT NULL");
  }
  SECTION("binding column") {
    http_schema_query(*connection,
                      "ALTER TABLE plinth.package_http_adapter_bindings "
                      "ALTER COLUMN owner_epoch DROP NOT NULL");
  }
  SECTION("nonkernel grants") {
    http_schema_query(
        *connection,
        "GRANT SELECT ON plinth.package_http_claim_receipts TO PUBLIC");
  }
  REQUIRE_THROWS_WITH(
      plinth::db::bootstrap_schema(fixture.db, migrations, false),
      "incompatible HTTP claims schema");
}
