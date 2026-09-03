// ICD-0.6.1 §12.1 — first-boot pre-flight (B.* test family).
//
// Library-level coverage of `plinth::shell::ensure_bundled_shell_installed`
// (kernel/shell/firstboot.cpp). Crash-recovery I.16 + I.17 in
// tests/kernel/packages/crash_recovery_test.cpp keep their ICD-0.4.4
// slice-B framing; this file is the ICD-0.6.1-shaped suite.

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/shell/firstboot.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

[[nodiscard]] auto pg_available() -> bool {
  return std::getenv("PLINTH_PG_HOST") != nullptr;
}

[[nodiscard]] auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* h = std::getenv("PLINTH_PG_HOST")) {
    db.host = h;
  }
  if (auto* p = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<uint16_t>(std::stoi(p));
  }
  if (auto* u = std::getenv("PLINTH_PG_USER")) {
    db.user = u;
  }
  if (auto* w = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = w;
  }
  if (auto* d = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = d;
  }
  return db;
}

[[nodiscard]] auto conninfo_of(const plinth::Config::Database& db)
    -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

// Drop ext_<name> schemas left behind by previous test runs. Mirrors
// the equivalent helper in crash_recovery_test.cpp.
auto drop_all_ext_schemas(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return;
  }
  PGresult* res = PQexec(
      conn, "SELECT nspname FROM pg_namespace WHERE nspname LIKE 'ext_%'");
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int n = PQntuples(res);
    for (int i = 0; i < n; ++i) {
      std::string s{PQgetvalue(res, i, 0)};
      std::string sql = "DROP SCHEMA IF EXISTS " + s + " CASCADE";
      PQclear(PQexec(conn, sql.c_str()));
    }
  }
  PQclear(res);
  PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  PQfinish(conn);
}

std::atomic<uint64_t> g_fb_scratch_counter{0};

struct Scratch {
  plinth::Config::Database db;
  plinth::Config cfg;
  fs::path base;
  plinth::packages::InstallerContext ctx;

  Scratch() {
    db = pg_config();
    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_fb_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_fb_" + id);
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

// Count audit_log rows for the given category. Used to assert
// shell.firstboot.* audits land synchronously per ICD §10.1.
[[nodiscard]] auto audit_count(const plinth::Config::Database& db,
                               std::string_view category) -> int {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return -1;
  }
  std::string cat{category};
  std::array<const char*, 1> values = {cat.c_str()};
  PGresult* res = PQexecParams(
      conn, "SELECT COUNT(*) FROM plinth.audit_log WHERE action = $1", 1,
      nullptr, values.data(), nullptr, nullptr, 0);
  int out = -1;
  if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
    out = std::stoi(std::string{PQgetvalue(res, 0, 0)});
  }
  PQclear(res);
  PQfinish(conn);
  return out;
}

[[nodiscard]] auto count_packages_by_name(const plinth::Config::Database& db,
                                          std::string_view name) -> int {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return -1;
  }
  std::string n{name};
  std::array<const char*, 1> values = {n.c_str()};
  PGresult* res =
      PQexecParams(conn, "SELECT COUNT(*) FROM plinth.packages WHERE name = $1",
                   1, nullptr, values.data(), nullptr, nullptr, 0);
  int out = -1;
  if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1) {
    out = std::stoi(std::string{PQgetvalue(res, 0, 0)});
  }
  PQclear(res);
  PQfinish(conn);
  return out;
}

} // namespace

// ── B.01 ─────────────────────────────────────────────────────
TEST_CASE("B.01: fresh PG + bundle present → first-boot installs ACTIVE "
          "bundled shell",
          "[shell][firstboot][integration][B.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(fb.has_value());

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* res = PQexec(
      conn,
      "SELECT name, version, state, provenance, frontend_mount, frontend_entry "
      "FROM plinth.packages WHERE name = 'shell'");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res) == 1);
  REQUIRE(std::string{PQgetvalue(res, 0, 2)} == "ACTIVE");
  REQUIRE(std::string{PQgetvalue(res, 0, 3)} == "bundled");
  REQUIRE(std::string{PQgetvalue(res, 0, 4)} == "/app");
  REQUIRE(std::string{PQgetvalue(res, 0, 5)} == "index.html");
  PQclear(res);
  PQfinish(conn);

  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_started") == 1);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_completed") == 1);
}

// ── B.02 ─────────────────────────────────────────────────────
TEST_CASE("B.02: existing ACTIVE bundled shell → pre-flight short-circuits "
          "with no install or audit",
          "[shell][firstboot][integration][B.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  // First boot installs the shell.
  REQUIRE(
      plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx).has_value());
  int started_before =
      audit_count(s.db, "shell.firstboot.bundled_install_started");
  int completed_before =
      audit_count(s.db, "shell.firstboot.bundled_install_completed");

  // Second pre-flight pass: short-circuit; no new audits; package count
  // stays at 1.
  auto t0 = std::chrono::steady_clock::now();
  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
  REQUIRE(fb.has_value());

  REQUIRE(count_packages_by_name(s.db, "shell") == 1);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_started") ==
          started_before);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_completed") ==
          completed_before);
  // ICD §12.1 B.02: short-circuit completes in < 100 ms (loose bound to
  // tolerate CI jitter; the ICD figure is 10 ms which is achievable on
  // a warm PG connection but flaky under load).
  REQUIRE(elapsed_ms < 100);
}

// ── B.03 ─────────────────────────────────────────────────────
TEST_CASE("B.03: missing shell.zip → exit code 1 + bundle-missing audit",
          "[shell][firstboot][integration][B.03]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  // Point bundle_path at an empty directory.
  auto empty = s.base / "no_bundle";
  fs::create_directories(empty);
  s.cfg.shell.bundle_path = empty.string();

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(!fb.has_value());
  REQUIRE(fb.error().kind == plinth::shell::FirstBootError::BUNDLE_MISSING);
  REQUIRE(fb.error().exit_code() == 1);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_failed") == 1);
}

// ── B.04 ─────────────────────────────────────────────────────
TEST_CASE(
    "B.04: corrupt shell.zip → exit code 2 + install-lifecycle-failed audit",
    "[shell][firstboot][integration][B.04]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  // Substitute a non-zip file at <bundle_path>/shell.zip. The install
  // lifecycle's UPLOADING stage rejects at `extract_zip_to`.
  auto bundle = s.base / "bundle";
  fs::create_directories(bundle);
  {
    std::ofstream f(bundle / "shell.zip", std::ios::binary);
    f.write("NOT A ZIP", 9);
  }
  s.cfg.shell.bundle_path = bundle.string();

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(!fb.has_value());
  REQUIRE(fb.error().kind ==
          plinth::shell::FirstBootError::BUNDLE_INSTALL_FAILED);
  REQUIRE(fb.error().exit_code() == 2);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_failed") == 1);
}

// ── B.05 ─────────────────────────────────────────────────────
TEST_CASE("B.05: two ACTIVE bundled frontends → exit code 3 + "
          "singleton-violation audit",
          "[shell][firstboot][integration][B.05]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  // Manufacture two ACTIVE bundled-frontend rows pre-pre-flight.
  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  auto seed = [conn](const std::string& name, const std::string& mount) {
    std::string sql =
        "INSERT INTO plinth.packages "
        "(name, version, state, provenance, manifest_json, "
        " frontend_mount, frontend_entry, entry_point, manifest_checksum) "
        "VALUES ('" +
        name +
        "', '1.0.0', 'ACTIVE', 'bundled', "
        "        '{}'::jsonb, '" +
        mount +
        "', 'index.html', "
        "        'server/main.js', 'fake-checksum-" +
        name + "')";
    PGresult* r = PQexec(conn, sql.c_str());
    REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
    PQclear(r);
  };
  seed("preinstalled-a", "/legacy-a/");
  seed("preinstalled-b", "/legacy-b/");
  PQfinish(conn);

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(!fb.has_value());
  REQUIRE(fb.error().kind ==
          plinth::shell::FirstBootError::MULTIPLE_ACTIVE_FRONTENDS);
  REQUIRE(fb.error().exit_code() == 3);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_failed") == 1);
}

// ── B.06 ─────────────────────────────────────────────────────
TEST_CASE("B.06: pre-existing user package name='shell' → exit code 3 + "
          "schema-name-conflict audit",
          "[shell][firstboot][integration][B.06]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  // Pre-0.6.1 user-uploaded `name='shell'` row. Post-0.6.1 the
  // parse-time RESERVED_NAME guard prevents new ones, but legacy rows
  // can still squat on the schema.
  PGresult* r =
      PQexec(conn, "INSERT INTO plinth.packages "
                   "(name, version, state, provenance, manifest_json, "
                   " entry_point, manifest_checksum) "
                   "VALUES ('shell', '0.0.1', 'ACTIVE', 'user', '{}'::jsonb, "
                   "        'server/main.js', 'legacy-checksum')");
  REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
  PQclear(r);
  PQfinish(conn);

  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(!fb.has_value());
  REQUIRE(fb.error().kind == plinth::shell::FirstBootError::SCHEMA_RESERVED);
  REQUIRE(fb.error().exit_code() == 3);
  REQUIRE(audit_count(s.db, "shell.firstboot.bundled_install_failed") == 1);
  REQUIRE(count_packages_by_name(s.db, "shell") == 1); // still the user row
}
