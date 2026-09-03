// Schema-level tests for the 0.4.4 additions: plinth.packages and
// plinth.panels. Mirrors tests/kernel/db/bootstrap_test.cpp patterns —
// unit checks hit the on-disk schema.sql file, integration checks run
// against a live PG under PLINTH_PG_* env vars (skipped otherwise).

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
