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

#include "../ws/ws_test_fixture.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/shell/firstboot.hpp"
#include "typed_surface_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

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
    plinth::ws_test::settle_fixture_work();
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
    cfg.packages_data_dir = ctx.data_dir.string();
    cfg.packages_staging_dir = ctx.staging_dir.string();
    static_cast<void>(plinth::extensions::shutdown_registry());
    auto resolver = plinth::capabilities::init_resolver(db);
    REQUIRE(resolver.has_value());
    plinth::extensions::init_registry(cfg);
    REQUIRE(plinth::packages::rbac_test::start_async_workers());
  }
  ~Scratch() {
    plinth::ws_test::settle_fixture_work_or_exit();
    plinth::packages::asset_server::cancel_all_registrations();
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::capabilities::clear_resolver_for_test();
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

TEST_CASE("typed surface recovery preserves committed order and provenance",
          "[typed-surface-foundation][crash_recovery][integration][packages]") {
  REQUIRE(pg_available());
  namespace fixture = plinth::typed_surface_test;
  Scratch s;
  fixture::NotesFenceCleanup fence_cleanup;
  plinth::ws_test::TestPg pg{s.db};
  const auto scalar = [&](const std::string& sql) -> std::string {
    const auto result = pg.exec(sql);
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    REQUIRE(PQgetisnull(result.get(), 0, 0) == 0);
    return PQgetvalue(result.get(), 0, 0);
  };
  const auto command = [&](const std::string& sql) {
    const auto result = pg.exec(sql);
    INFO(PQresultErrorMessage(result.get()));
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  };
  auto declarations = fixture::panels({fixture::floating()});
  bool legacy = false;
  bool first_active = false;
  SECTION("committed typed upgrade repairs pointer without reallocating") {
  }
  SECTION("first ACTIVE recovery allocates once inside its transition") {
    first_active = true;
  }
  SECTION("legacy root and entry collisions do not acquire provenance") {
    declarations = {{"panels", {fixture::floating()}}};
    legacy = true;
  }
  const auto first = plinth::packages::install_package(
      fixture::package(declarations), plinth::packages::Provenance::USER,
      s.ctx);
  INFO(first ? "installed" : first.error().message);
  REQUIRE(first.has_value());
  plinth::ws_test::settle_fixture_work();
  REQUIRE(plinth::packages::rbac_test::start_async_workers());
  std::string current_id = first->id;
  std::string current_version = first->version;
  if (!first_active) {
    const auto next = plinth::packages::install_package(
        fixture::package(declarations, "1.2.4"),
        plinth::packages::Provenance::USER, s.ctx);
    INFO(next ? "upgraded" : next.error().message);
    REQUIRE(next.has_value());
    plinth::ws_test::settle_fixture_work();
    REQUIRE(plinth::packages::rbac_test::start_async_workers());
    current_id = next->id;
    current_version = next->version;
    const auto active = s.ctx.data_dir / "extensions" / "notes" / "active";
    REQUIRE(fs::remove(active));
    fs::create_symlink(first->version, active);
  } else {
    // Manufactured pre-COMMIT state, as in the existing reconciler tests.
    // This is disposition coverage, not a SIGKILL/uncertain-COMMIT claim.
    command("BEGIN");
    command(
        "UPDATE plinth.packages SET state='ACTIVATING',application_ready=FALSE "
        "WHERE id='" +
        first->id + "'");
    command("UPDATE plinth.panels SET logical_install_order=NULL WHERE "
            "package_id='" +
            first->id + "'");
    command("UPDATE plinth.handler_install_order_allocator SET last_value=0");
    command("COMMIT");
  }
  if (legacy) {
    const auto path = s.ctx.data_dir / "extensions" / "notes" /
                      current_version / "panels.json";
    std::ofstream output(path);
    REQUIRE(output.is_open());
    output << fixture::panels({fixture::floating()}).dump();
    output.close();
    REQUIRE(output.good());
    command("UPDATE plinth.panels SET declaration=declaration||"
            "'{\"format_version\":2}'::jsonb WHERE package_id='" +
            current_id + "'");
  }
  command("UPDATE plinth.packages SET application_ready=FALSE WHERE id='" +
          current_id + "'");
  plinth::extensions::destroy_pool("notes");
  plinth::packages::asset_server::cancel_all_registrations();
  for (int pass = 0; pass < 2; ++pass) {
    INFO("recovery pass " << pass);
    const auto reconciled =
        plinth::packages::reconcile_in_flight_installs(s.ctx);
    INFO(reconciled ? "reconciled" : reconciled.error());
    REQUIRE(reconciled.has_value());
    plinth::packages::asset_server::restore_routes(s.db, s.ctx.data_dir);
    REQUIRE(
        plinth::packages::reconcile_application_readiness(s.ctx).has_value());
    REQUIRE(state_of(s.db, current_id) == "ACTIVE");
    REQUIRE(plinth::extensions::has_pool("notes"));
    REQUIRE(fs::read_symlink(s.ctx.data_dir / "extensions" / "notes" /
                             "active") == current_version);
    REQUIRE(
        scalar(
            "SELECT application_ready::text FROM plinth.packages WHERE id='" +
            current_id + "'") == "true");
    REQUIRE(scalar("SELECT validated_format_version::text FROM plinth.panels "
                   "WHERE package_id='" +
                   current_id + "'") == (legacy ? "0" : "2"));
    REQUIRE(scalar("SELECT panel_type FROM plinth.panels WHERE package_id='" +
                   current_id + "'") == (legacy ? "primary" : "float"));
    REQUIRE(scalar("SELECT COALESCE(logical_install_order::text,'NULL') FROM "
                   "plinth.panels WHERE package_id='" +
                   current_id + "'") == (legacy ? "NULL" : "1"));
    REQUIRE(scalar("SELECT last_value::text FROM "
                   "plinth.handler_install_order_allocator") ==
            (legacy ? "0" : "1"));
  }
}

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
  write_tree(tree / "server", "main.js", "export default {};\n");
  fs::create_symlink("2.0.0", tree.parent_path() / "active");

  auto reconciled = plinth::packages::reconcile_in_flight_installs(s.ctx);
  REQUIRE(reconciled.has_value());

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

  auto reconciled = plinth::packages::reconcile_in_flight_installs(s.ctx);
  REQUIRE(reconciled.has_value());

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

  auto reconciled = plinth::packages::reconcile_in_flight_installs(s.ctx);
  REQUIRE(reconciled.has_value());

  REQUIRE(state_of(s.db, up) == "INSTALL_FAILED");
  REQUIRE(state_of(s.db, val) == "INSTALL_FAILED");
}

TEST_CASE("I.11c: reconciler lock conflict leaves every candidate untouched",
          "[crash_recovery][integration][I.11][locking]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto first = seed_row(s.db, "a-before-lock", "1.0.0", "UPLOADING");
  auto blocked = seed_row(s.db, "z-held-lock", "1.0.0", "VALIDATING");

  std::unique_ptr<PGconn, decltype(&PQfinish)> lock_conn(
      PQconnectdb(conninfo_of(s.db).c_str()), PQfinish);
  REQUIRE(PQstatus(lock_conn.get()) == CONNECTION_OK);
  std::unique_ptr<PGresult, decltype(&PQclear)> locked(
      PQexec(lock_conn.get(), "SELECT pg_advisory_lock(hashtextextended("
                              "'plinth.packages.z-held-lock', 0))"),
      PQclear);
  REQUIRE(PQresultStatus(locked.get()) == PGRES_TUPLES_OK);

  auto reconciled = plinth::packages::reconcile_in_flight_installs(s.ctx);
  REQUIRE_FALSE(reconciled.has_value());
  REQUIRE(reconciled.error().find("advisory-lock-held") != std::string::npos);
  // Acquiring every name lock must precede every disposition. In particular,
  // the lexically earlier row cannot be consumed before the later conflict is
  // discovered, and the conflicting row remains untouched as well.
  REQUIRE(state_of(s.db, first) == "UPLOADING");
  REQUIRE(state_of(s.db, blocked) == "VALIDATING");

  std::unique_ptr<PGresult, decltype(&PQclear)> first_released(
      PQexec(lock_conn.get(), "SELECT pg_try_advisory_lock(hashtextextended("
                              "'plinth.packages.a-before-lock', 0))"),
      PQclear);
  REQUIRE(PQresultStatus(first_released.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::string_view{PQgetvalue(first_released.get(), 0, 0)} == "t");

  PQclear(PQexec(lock_conn.get(), "SELECT pg_advisory_unlock(hashtextextended("
                                  "'plinth.packages.a-before-lock', 0))"));
  PQclear(PQexec(lock_conn.get(), "SELECT pg_advisory_unlock(hashtextextended("
                                  "'plinth.packages.z-held-lock', 0))"));
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

  // A retained installation includes the active pointer matching its
  // committed package row; an absent/mismatched pointer requires recovery.
  auto installed = s.ctx.data_dir / "extensions" / "preinstalled-fe";
  fs::create_directories(installed / "1.0.0");
  fs::create_symlink("1.0.0", installed / "active");

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
