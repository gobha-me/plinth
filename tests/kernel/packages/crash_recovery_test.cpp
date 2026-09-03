// Crash-recovery + bundled-shell first-boot tests (ICD-0.4.4 I.11, I.12,
// I.16, I.17).
//
// Deviation from the plan's SIGKILL subprocess harness: these cases test
// the reconciler's disposition logic directly against manufactured DB
// states instead of fork()/exec()/SIGKILL-ing a real kernel. Rationale:
// the reconciler's correctness is a function of (plinth.packages.state,
// on-disk tree presence), and both inputs are easily constructed without
// a subprocess. A true fork/exec/SIGKILL harness still adds value —
// particularly for exercising the bootstrap path end-to-end — but it
// introduces child-process lifecycle complexity and interacts with the
// parked trantor teardown flake (project_ws_flaky_segfault.md). Deferred
// to follow-up work; tracked via the DEFERRED.md entry for that flake.
//
// I.16 / I.17 are boot-only (no kill required) and exercise
// `plinth::shell::ensure_bundled_shell_installed` (ICD-0.6.1 §3.1; the
// successor of `install_shell_if_needed` from ICD-0.4.4 slice B).

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/shell/firstboot.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* v = std::getenv("PLINTH_PG_HOST")) {
    db.host = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<std::uint16_t>(std::stoi(v));
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
  PQfinish(conn);
  return ok;
}

auto drop_all_ext_schemas(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return;
  }
  PGresult* res =
      PQexec(conn, "SELECT schema_name FROM information_schema.schemata "
                   "WHERE schema_name LIKE 'ext\\_%' ESCAPE '\\'");
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int n = PQntuples(res);
    for (int i = 0; i < n; ++i) {
      std::string s = PQgetvalue(res, i, 0);
      PQclear(
          PQexec(conn, ("DROP SCHEMA IF EXISTS " + s + " CASCADE").c_str()));
      PQclear(PQexec(conn, ("DROP ROLE IF EXISTS " + s + "_role").c_str()));
    }
  }
  PQclear(res);
  PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  PQfinish(conn);
}

std::atomic<uint64_t> g_cr_scratch_counter{0};

struct Scratch {
  plinth::Config::Database db;
  plinth::Config cfg; // ICD-0.6.1 §3.1 firstboot
  fs::path base;
  plinth::packages::InstallerContext ctx;

  Scratch() {
    db = pg_config();
    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_cr_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_crash_" + id);
    fs::create_directories(base / "data");
    fs::create_directories(base / "staging");

    drop_all_ext_schemas(db);
    auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
    plinth::db::bootstrap_schema(db, migrations_dir, /*dev_mode=*/true);
    plinth::groups::bootstrap_groups(db);

    ctx.db = db;
    ctx.caller_user_id = "";
    ctx.data_dir = base / "data";
    ctx.staging_dir = base / "staging";
    ctx.max_package_size_bytes = 50ULL * 1024ULL * 1024ULL;

    // ICD-0.6.1 §9 — point firstboot at the staged shell.zip in
    // the build tree. cmake-staged at
    // ${CMAKE_BINARY_DIR}/share/plinth/bundled/shell.zip per
    // `plinth_shell_bundle_staged`.
    cfg.db = db;
    cfg.shell.bundle_path =
        std::string{CMAKE_BINARY_DIR} + "/share/plinth/bundled";
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(base, ec);
    drop_all_ext_schemas(db);
  }
  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  Scratch(Scratch&&) = delete;
  auto operator=(Scratch&&) -> Scratch& = delete;
};

// Seed a plinth.packages row in `state` with the given (name, version).
// Mimics what a crashed install would leave behind so the reconciler can
// be driven against it.
auto seed_row(const plinth::Config::Database& db, const std::string& name,
              const std::string& version, const std::string& state,
              const std::string& frontend_mount = "") -> std::string {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  std::string mount_value;
  std::string fentry_value;
  if (frontend_mount.empty()) {
    mount_value = "NULL";
    fentry_value = "NULL";
  } else {
    mount_value = "'" + frontend_mount + "'";
    // ICD-0.6.1 §4.3 chk_frontend_pair: mount and entry are NULL
    // together or non-NULL together.
    fentry_value = "'index.html'";
  }
  std::string sql =
      "INSERT INTO plinth.packages "
      "(name, version, state, provenance, manifest_json, frontend_mount,"
      " frontend_entry, entry_point, manifest_checksum) "
      "VALUES ('" +
      name + "', '" + version + "', '" + state + "', 'user', '{}'::jsonb, " +
      mount_value + ", " + fentry_value +
      ", 'server/main.js', 'fake-checksum') "
      "RETURNING id::text";
  PGresult* res = PQexec(conn, sql.c_str());
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  std::string id = PQgetvalue(res, 0, 0);
  PQclear(res);
  PQfinish(conn);
  return id;
}

auto state_of(const plinth::Config::Database& db, const std::string& id)
    -> std::string {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  auto sql = "SELECT state FROM plinth.packages WHERE id = '" + id + "'::uuid";
  PGresult* res = PQexec(conn, sql.c_str());
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 1);
  std::string out = PQgetvalue(res, 0, 0);
  PQclear(res);
  PQfinish(conn);
  return out;
}

auto ext_schema_exists(const plinth::Config::Database& db,
                       const std::string& schema) -> bool {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return false;
  }
  auto sql = "SELECT 1 FROM information_schema.schemata "
             "WHERE schema_name = '" +
             schema + "'";
  PGresult* res = PQexec(conn, sql.c_str());
  bool found = PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0;
  PQclear(res);
  PQfinish(conn);
  return found;
}

// Manually create ext_<name> schema (simulating what a half-run migration
// leaves on disk during MIGRATING).
auto create_ext_schema(const plinth::Config::Database& db,
                       const std::string& schema) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PQclear(PQexec(conn, ("CREATE SCHEMA " + schema).c_str()));
  PQfinish(conn);
}

auto write_tree(const fs::path& root, const std::string& file,
                const std::string& body) -> void {
  fs::create_directories(root);
  std::ofstream out(root / file);
  out << body;
}

} // namespace

// ── I.11 variant: MIGRATING row + ext schema present → INSTALL_FAILED, drop ──

TEST_CASE("I.11: reconciler drops ext schema for MIGRATING row",
          "[crash_recovery][integration][I.11]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto id = seed_row(s.db, "notescrash", "1.0.0", "MIGRATING");
  create_ext_schema(s.db, "ext_notescrash");
  REQUIRE(ext_schema_exists(s.db, "ext_notescrash"));

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  REQUIRE(state_of(s.db, id) == "INSTALL_FAILED");
  REQUIRE_FALSE(ext_schema_exists(s.db, "ext_notescrash"));
}

// ── I.12a: ACTIVATING + tree present → ACTIVE ────────────────────

TEST_CASE("I.12a: reconciler advances ACTIVATING with on-disk tree to ACTIVE",
          "[crash_recovery][integration][I.12]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto id = seed_row(s.db, "notesactivating", "2.0.0", "ACTIVATING");
  auto tree = s.ctx.data_dir / "extensions" / "notesactivating" / "2.0.0";
  write_tree(tree, "manifest.json", "{}\n");

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  REQUIRE(state_of(s.db, id) == "ACTIVE");
}

// ── I.12b: ACTIVATING without tree → INSTALL_FAILED ──────────────

TEST_CASE("I.12b: reconciler fails ACTIVATING with missing tree",
          "[crash_recovery][integration][I.12]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto id = seed_row(s.db, "notesmissingtree", "2.0.0", "ACTIVATING");
  // Deliberately no write_tree() — simulates EXTRACTING that never finished.

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  REQUIRE(state_of(s.db, id) == "INSTALL_FAILED");
}

// ── I.11b: UPLOADING/VALIDATING -> INSTALL_FAILED, no schema drop ──

TEST_CASE("I.11b: reconciler marks UPLOADING / VALIDATING rows INSTALL_FAILED",
          "[crash_recovery][integration][I.11]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto up = seed_row(s.db, "up-1", "1.0.0", "UPLOADING");
  auto val = seed_row(s.db, "val-1", "1.0.0", "VALIDATING");

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  REQUIRE(state_of(s.db, up) == "INSTALL_FAILED");
  REQUIRE(state_of(s.db, val) == "INSTALL_FAILED");
}

// ── I.16: bundled-shell first boot when no frontend is installed ──

TEST_CASE(
    "I.16: ensure_bundled_shell_installed installs bundled shell on fresh DB",
    "[crash_recovery][integration][I.16]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(fb.has_value());

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* res =
      PQexec(conn, "SELECT name, provenance, state, installed_by_user_id, "
                   "       frontend_mount, frontend_entry "
                   "FROM plinth.packages WHERE name = 'shell'");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 1);
  REQUIRE(std::string{PQgetvalue(res, 0, 1)} == "bundled");
  REQUIRE(std::string{PQgetvalue(res, 0, 2)} == "ACTIVE");
  REQUIRE(PQgetisnull(res, 0, 3) == 1); // installed_by_user_id IS NULL
  REQUIRE(std::string{PQgetvalue(res, 0, 4)} == "/app");
  REQUIRE(std::string{PQgetvalue(res, 0, 5)} == "index.html");
  PQclear(res);
  PQfinish(conn);
}

// ── I.17: first-boot is a no-op when a frontend is already installed ──

TEST_CASE(
    "I.17: ensure_bundled_shell_installed skips when frontend already ACTIVE",
    "[crash_recovery][integration][I.17]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  // Seed an active bundled-frontend row to trigger the short-circuit.
  {
    PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
    REQUIRE(PQstatus(conn) == CONNECTION_OK);
    std::string sql =
        "INSERT INTO plinth.packages "
        "(name, version, state, provenance, manifest_json, "
        " frontend_mount, frontend_entry, entry_point, manifest_checksum) "
        "VALUES ('preinstalled-fe', '1.0.0', 'ACTIVE', 'bundled', "
        "        '{}'::jsonb, '/legacy/', 'index.html', "
        "        'server/main.js', 'fake-checksum')";
    PGresult* r = PQexec(conn, sql.c_str());
    REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
    PQclear(r);
    PQfinish(conn);
  }

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(fb.has_value());

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* res =
      PQexec(conn, "SELECT COUNT(*) FROM plinth.packages WHERE name = 'shell'");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(res, 0, 0)} == "0");
  PQclear(res);
  PQfinish(conn);
}
