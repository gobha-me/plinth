// PG-gated integration tests for the install lifecycle state machine
// (ICD-0.4.4 cases I.01-I.10). Each test provisions a fresh plinth
// schema + data directory, then invokes `install_package()` directly
// and asserts the resulting state and DB rows. Fixtures are pre-zipped
// by the `plinth_install_fixture_zips` CMake target into
// ${CMAKE_BINARY_DIR}/fixtures/<name>.zip so this driver reads static
// bytes rather than zipping at runtime.
//
// Deferred to follow-up work: I.11/I.12 (crash-recovery SIGKILL harness),
// I.18-I.20 (concurrent POST / dry-run / RBAC denial — need the HTTP
// surface, not the library entry point).

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>
#include <zip.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ── PG plumbing ────────────────────────────────────────────────

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
  // plinth.packages is dropped in the CASCADE below; scrub any per-
  // extension schemas and their roles that prior tests may have left.
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

// ── Per-test scratch + installer context ─────────────────────────

std::atomic<uint64_t> g_scratch_counter{0};

struct Scratch {
  plinth::Config::Database db;
  fs::path base;
  plinth::packages::InstallerContext ctx;

  Scratch() {
    db = pg_config();
    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_install_" + id);
    fs::create_directories(base / "data");
    fs::create_directories(base / "staging");

    // Fresh schema every test — parallel ctest would race on a shared PG
    // but tests/kernel/packages/migrations_test.cpp has the same posture.
    drop_all_ext_schemas(db);
    auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
    plinth::db::bootstrap_schema(db, migrations_dir, /*dev_mode=*/true);
    plinth::groups::bootstrap_groups(db);

    ctx.db = db;
    ctx.caller_user_id = "";
    ctx.data_dir = base / "data";
    ctx.staging_dir = base / "staging";
    ctx.max_package_size_bytes = 50ULL * 1024ULL * 1024ULL;
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

auto read_file_bytes(const fs::path& p) -> std::vector<std::byte> {
  std::ifstream in(p, std::ios::binary);
  std::vector<std::byte> out;
  if (!in.is_open()) {
    return out;
  }
  in.seekg(0, std::ios::end);
  auto n = in.tellg();
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(n));
  // needs char*; std::byte backing store is bit-compatible.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
  return out;
}

auto read_fixture_zip(const std::string& name) -> std::vector<std::byte> {
  fs::path p = fs::path{CMAKE_BINARY_DIR} / "fixtures" / (name + ".zip");
  return read_file_bytes(p);
}

// Generate a zip hand-crafted to contain a `../evil.txt` entry (I.07).
// Uses libzip's in-memory source API; no filesystem side effects.
auto make_path_traversal_zip() -> std::vector<std::byte> {
  // Minimal well-formed zip with one entry whose name contains "..".
  // libzip + zip_source_buffer_create flow.
  zip_error_t err{};
  zip_error_init(&err);
  zip_source_t* src = zip_source_buffer_create(nullptr, 0, 0, &err);
  REQUIRE(src != nullptr);
  zip_source_keep(src); // keep alive after zip_close
  zip_t* archive = zip_open_from_source(src, ZIP_TRUNCATE, &err);
  REQUIRE(archive != nullptr);

  std::string payload = "pwned";
  zip_source_t* body =
      zip_source_buffer(archive, payload.data(), payload.size(), 0);
  REQUIRE(body != nullptr);
  zip_int64_t idx =
      zip_file_add(archive, "../evil.txt", body, ZIP_FL_OVERWRITE);
  REQUIRE(idx >= 0);
  REQUIRE(zip_close(archive) == 0);

  // Flush buffer source.
  REQUIRE(zip_source_open(src) == 0);
  zip_source_seek(src, 0, SEEK_END);
  zip_int64_t sz = zip_source_tell(src);
  zip_source_seek(src, 0, SEEK_SET);
  std::vector<std::byte> out(static_cast<std::size_t>(sz));
  zip_source_read(src, out.data(), static_cast<zip_uint64_t>(sz));
  zip_source_close(src);
  zip_source_free(src);
  return out;
}

} // namespace

// ── Test cases ────────────────────────────────────────────────────

TEST_CASE("I.01: valid install -> ACTIVE with DB + disk artefacts",
          "[install_lifecycle][integration][I.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("valid-install");
  REQUIRE(!blob.empty());

  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);

  if (!r.has_value()) {
    UNSCOPED_INFO("install failed at stage "
                  << plinth::packages::stage_to_string(r.error().failed_at)
                  << " kind=" << r.error().kind << " msg=" << r.error().message
                  << " report=" << r.error().report.dump());
  }
  REQUIRE(r.has_value());
  REQUIRE(r->state == plinth::packages::InstallStage::ACTIVE);
  REQUIRE(r->name == "notes");
  REQUIRE(r->version == "1.2.3");
  REQUIRE(fs::exists(s.ctx.data_dir / "extensions" / "notes" / "1.2.3" /
                     "manifest.json"));
}

TEST_CASE("I.02: valid install without panels.json -> ACTIVE, no panels rows",
          "[install_lifecycle][integration][I.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("valid-install-no-panels");
  REQUIRE(!blob.empty());

  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);

  REQUIRE(r.has_value());
  REQUIRE(r->state == plinth::packages::InstallStage::ACTIVE);

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* pr =
      PQexec(conn, ("SELECT COUNT(*) FROM plinth.panels WHERE package_id = '" +
                    r->id + "'")
                       .c_str());
  REQUIRE(PQresultStatus(pr) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(pr, 0, 0)} == "0");
  PQclear(pr);
  PQfinish(conn);
}

TEST_CASE("I.03: valid install with frontend.mount -> frontend_mount set",
          "[install_lifecycle][integration][I.03]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("valid-install-frontend");
  REQUIRE(!blob.empty());

  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);

  if (!r.has_value()) {
    UNSCOPED_INFO("install failed at stage "
                  << plinth::packages::stage_to_string(r.error().failed_at)
                  << " kind=" << r.error().kind << " msg=" << r.error().message
                  << " report=" << r.error().report.dump());
  }
  REQUIRE(r.has_value());
  REQUIRE(r->state == plinth::packages::InstallStage::ACTIVE);
  REQUIRE(r->frontend_mount.has_value());
  REQUIRE(*r->frontend_mount == "/notes");
}

TEST_CASE("I.04: second install of same name while first ACTIVE is rejected",
          "[install_lifecycle][integration][I.04]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("valid-install");
  REQUIRE(!blob.empty());

  auto first = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE(first.has_value());
  REQUIRE(first->state == plinth::packages::InstallStage::ACTIVE);

  auto second = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE_FALSE(second.has_value());
  // Original must still be ACTIVE.
  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  PGresult* pr = PQexec(
      conn, ("SELECT state FROM plinth.packages WHERE id = '" + first->id + "'")
                .c_str());
  REQUIRE(PQresultStatus(pr) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(pr, 0, 0)} == "ACTIVE");
  PQclear(pr);
  PQfinish(conn);
}

TEST_CASE("I.05: zip exceeding max_package_size_bytes is rejected",
          "[install_lifecycle][integration][I.05]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  s.ctx.max_package_size_bytes = 1024; // tiny cap for this test
  std::vector<std::byte> big(std::size_t{2} * 1024, std::byte{0});
  auto r = plinth::packages::install_package(
      big, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().failed_at == plinth::packages::InstallStage::UPLOADING);
}

TEST_CASE("I.06: non-zip bytes rejected at UPLOADING",
          "[install_lifecycle][integration][I.06]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto bytes =
      read_file_bytes(fs::path{CMAKE_SOURCE_DIR} /
                      "tests/fixtures/install_lifecycle/not-a-zip.bin");
  REQUIRE(!bytes.empty());

  auto r = plinth::packages::install_package(
      bytes, plinth::packages::Provenance::USER, s.ctx);

  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().failed_at == plinth::packages::InstallStage::UPLOADING);
}

TEST_CASE("I.07: path-traversal zip entry rejected at UPLOADING",
          "[install_lifecycle][integration][I.07]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto zip_bytes = make_path_traversal_zip();
  REQUIRE(!zip_bytes.empty());

  auto r = plinth::packages::install_package(
      zip_bytes, plinth::packages::Provenance::USER, s.ctx);

  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().failed_at == plinth::packages::InstallStage::UPLOADING);
}

TEST_CASE("I.08: missing manifest.json -> UPLOADING failure",
          "[install_lifecycle][integration][I.08]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("missing-manifest");
  REQUIRE(!blob.empty());

  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);

  REQUIRE_FALSE(r.has_value());
  // Must fail before MIGRATING so no schema is created.
  auto stage = r.error().failed_at;
  REQUIRE((stage == plinth::packages::InstallStage::UPLOADING ||
           stage == plinth::packages::InstallStage::VALIDATING));
}

TEST_CASE("I.09: validator CF1 failure -> INSTALL_FAILED at VALIDATING",
          "[install_lifecycle][integration][I.09]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("fail-validator");
  REQUIRE(!blob.empty());

  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);

  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().failed_at == plinth::packages::InstallStage::VALIDATING);
}

TEST_CASE("I.10: migration syntax error -> INSTALL_FAILED at MIGRATING, schema "
          "dropped",
          "[install_lifecycle][integration][I.10]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto blob = read_fixture_zip("fail-migration");
  REQUIRE(!blob.empty());

  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);

  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().failed_at == plinth::packages::InstallStage::MIGRATING);

  // Extension schema must have been cleaned up by drop_schema_and_migrations.
  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  PGresult* pr =
      PQexec(conn, "SELECT schema_name FROM information_schema.schemata "
                   "WHERE schema_name = 'ext_notes'");
  REQUIRE(PQresultStatus(pr) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(pr) == 0);
  PQclear(pr);
  PQfinish(conn);
}
