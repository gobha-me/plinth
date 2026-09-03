// PG-gated integration tests for 0.4.5 lifecycle transitions beyond
// first install. Slice A covers disable (D.*) and uninstall (U.*) +
// enable (D.04/D.05/D.08 per ICD-0.4.5 §Test Cases). Slice B adds X.*
// upgrade + G.* garbage-collection.
//
// Mirrors install_lifecycle_test.cpp's `Scratch` pattern: drop + recreate
// plinth schema per test, create an in-memory data_dir + staging_dir,
// install a fixture package as the transition precondition, then
// drive disable/enable/uninstall directly.

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/install_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

std::atomic<std::uint64_t> g_scratch_counter{0};

struct Scratch {
  plinth::Config::Database db;
  fs::path base;
  plinth::packages::InstallerContext ctx;
  PGconn* conn = nullptr;

  Scratch() {
    db = pg_config();
    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_lifecycle_" + id);
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

    conn = PQconnectdb(conninfo_of(db).c_str());
  }
  ~Scratch() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    // Scrub in-memory asset_server state so subsequent Scratches
    // don't leak routes from a prior install.
    plinth::packages::asset_server::cancel_all_registrations();
    std::error_code ec;
    fs::remove_all(base, ec);
    drop_all_ext_schemas(db);
  }
  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  Scratch(Scratch&&) = delete;
  auto operator=(Scratch&&) -> Scratch& = delete;

  [[nodiscard]] auto count(const std::string& sql) const -> std::string {
    std::unique_ptr<PGresult, decltype(&PQclear)> res(PQexec(conn, sql.c_str()),
                                                      PQclear);
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
        PQntuples(res.get()) == 0) {
      return {};
    }
    return {PQgetvalue(res.get(), 0, 0)};
  }

  [[nodiscard]] auto scalar(const std::string& sql) const -> std::string {
    return count(sql);
  }
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
  // needs char*; std::byte is bit-compatible.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
  return out;
}

auto read_fixture_zip(const std::string& name) -> std::vector<std::byte> {
  fs::path p = fs::path{CMAKE_BINARY_DIR} / "fixtures" / (name + ".zip");
  return read_file_bytes(p);
}

// 0.4.7 Slice B: every install/enable/upgrade fires a detached RBAC
// test run that holds the per-name advisory lock for a few ms.
// Disable/enable/uninstall/upgrade/GC all use pg_try_advisory_lock on
// the same key — without waiting, subsequent transitions race with the
// RBAC test and fail with advisory-lock-held. valid-install has no
// test_contracts, so the RBAC test runs fast and writes
// last_rbac_test_run_at + an empty report.
auto wait_rbac_test_settled(Scratch& s, std::string_view pid) -> void {
  using namespace std::chrono_literals;
  auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" +
                 std::string{pid} +
                 "' AND last_rbac_test_run_at IS NOT NULL") == "1") {
      return;
    }
    std::this_thread::sleep_for(25ms);
  }
}

auto install_valid_notes(Scratch& s) -> plinth::packages::PackageRecord {
  auto blob = read_fixture_zip("valid-install");
  REQUIRE(!blob.empty());
  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);
  if (!r.has_value()) {
    UNSCOPED_INFO("precondition install failed: " << r.error().message);
  }
  REQUIRE(r.has_value());
  REQUIRE(r->state == plinth::packages::InstallStage::ACTIVE);
  wait_rbac_test_settled(s, r->id);
  return *r;
}

} // namespace

// ── D.01: disable ACTIVE package ─────────────────────────────────

TEST_CASE(
    "D.01: disable ACTIVE package lands DISABLED + orphans rules + drops caps",
    "[lifecycle_transitions][integration][D.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);

  // Precondition: capabilities + rbac rows present for the extension.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='notes'") != "0");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                   "WHERE extension_name='notes' AND orphaned_at IS NULL") !=
          "0");

  auto d = plinth::packages::disable_package(rec.id, s.ctx);
  if (!d.has_value()) {
    UNSCOPED_INFO("disable failed: " << d.error().message);
  }
  REQUIRE(d.has_value());
  REQUIRE(d->state == plinth::packages::InstallStage::DISABLED);
  REQUIRE(d->name == "notes");

  // State flipped, disabled_at set.
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "DISABLED");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages "
                   "WHERE id='" +
                   rec.id + "' AND disabled_at IS NOT NULL") == "1");

  // RBAC rules orphaned (not deleted — group grants survive re-enable).
  REQUIRE(
      s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
               "WHERE extension_name='notes' AND orphaned_at IS NOT NULL") !=
      "0");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                   "WHERE extension_name='notes' AND orphaned_at IS NULL") ==
          "0");

  // Capabilities removed entirely (not just disabled — bulk DELETE).
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='notes'") == "0");
}

// ── D.06: disable already-DISABLED returns already-disabled ───────

TEST_CASE("D.06: disable DISABLED package returns already-disabled (409)",
          "[lifecycle_transitions][integration][D.06]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  REQUIRE(plinth::packages::disable_package(rec.id, s.ctx).has_value());

  auto d = plinth::packages::disable_package(rec.id, s.ctx);
  REQUIRE_FALSE(d.has_value());
  REQUIRE(d.error().kind == plinth::packages::TransitionKind::DISABLE);
  REQUIRE(d.error().report["kind"] == "already-disabled");
}

// ── D.07: disable unknown id returns not-found ────────────────────

TEST_CASE("D.07: disable unknown id returns not-found (404)",
          "[lifecycle_transitions][integration][D.07]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto d = plinth::packages::disable_package(
      "00000000-0000-0000-0000-000000000000", s.ctx);
  REQUIRE_FALSE(d.has_value());
  REQUIRE(d.error().report["kind"] == "not-found");
}

// ── D.04: enable DISABLED package restores ACTIVE ─────────────────

TEST_CASE("D.04: enable DISABLED package flips ACTIVE + clears orphans + "
          "re-registers caps",
          "[lifecycle_transitions][integration][D.04]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  REQUIRE(plinth::packages::disable_package(rec.id, s.ctx).has_value());
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "DISABLED");

  auto e = plinth::packages::enable_package(rec.id, s.ctx);
  if (!e.has_value()) {
    UNSCOPED_INFO("enable failed: " << e.error().message);
  }
  REQUIRE(e.has_value());
  REQUIRE(e->state == plinth::packages::InstallStage::ACTIVE);

  // State, disabled_at cleared.
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "ACTIVE");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages "
                   "WHERE id='" +
                   rec.id + "' AND disabled_at IS NULL") == "1");

  // RBAC un-orphaned (all rows back to orphaned_at NULL).
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                   "WHERE extension_name='notes' AND orphaned_at IS NULL") !=
          "0");
  REQUIRE(
      s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
               "WHERE extension_name='notes' AND orphaned_at IS NOT NULL") ==
      "0");

  // Capabilities rematerialised.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='notes'") != "0");
}

// ── D.05: enable ACTIVE package returns already-enabled ────────────

TEST_CASE("D.05: enable ACTIVE package returns already-enabled (409)",
          "[lifecycle_transitions][integration][D.05]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);

  auto e = plinth::packages::enable_package(rec.id, s.ctx);
  REQUIRE_FALSE(e.has_value());
  REQUIRE(e.error().report["kind"] == "already-enabled");
}

// ── D.08: enable with on-disk manifest drift returns checksum-mismatch ──

TEST_CASE("D.08: enable with edited manifest returns "
          "checksum-mismatch-on-enable (422)",
          "[lifecycle_transitions][integration][D.08]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  REQUIRE(plinth::packages::disable_package(rec.id, s.ctx).has_value());

  // Tamper with manifest.json on disk.
  fs::path mf =
      s.ctx.data_dir / "extensions" / rec.name / rec.version / "manifest.json";
  REQUIRE(fs::exists(mf));
  {
    std::ofstream append(mf, std::ios::app);
    append << "\n// tampered\n";
  }

  auto e = plinth::packages::enable_package(rec.id, s.ctx);
  REQUIRE_FALSE(e.has_value());
  REQUIRE(e.error().report["kind"] == "checksum-mismatch-on-enable");

  // State still DISABLED; no partial rematerialisation.
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "DISABLED");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='notes'") == "0");
}

// ── U.01: uninstall ACTIVE drives full cleanup ───────────────────

TEST_CASE("U.01: uninstall ACTIVE deletes row + rbac + caps + schema + files",
          "[lifecycle_transitions][integration][U.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);

  fs::path ext_dir = s.ctx.data_dir / "extensions" / rec.name;
  REQUIRE(fs::exists(ext_dir));
  REQUIRE(s.scalar("SELECT COUNT(*) FROM information_schema.schemata "
                   "WHERE schema_name = 'ext_" +
                   rec.name + "'") == "1");

  auto u =
      plinth::packages::uninstall_package(rec.id, /*confirmed=*/true, s.ctx);
  if (!u.has_value()) {
    UNSCOPED_INFO("uninstall failed: " << u.error().message);
  }
  REQUIRE(u.has_value());

  // Row gone.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "0");
  // RBAC + caps + panels all cleared.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                   "WHERE extension_name='" +
                   rec.name + "'") == "0");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='" +
                   rec.name + "'") == "0");
  // Extension schema dropped.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM information_schema.schemata "
                   "WHERE schema_name = 'ext_" +
                   rec.name + "'") == "0");
  // Filesystem tree gone.
  REQUIRE_FALSE(fs::exists(ext_dir));
}

// ── U.02: uninstall DISABLED package succeeds ─────────────────────

TEST_CASE("U.02: uninstall DISABLED package drives full cleanup",
          "[lifecycle_transitions][integration][U.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  REQUIRE(plinth::packages::disable_package(rec.id, s.ctx).has_value());

  auto u =
      plinth::packages::uninstall_package(rec.id, /*confirmed=*/true, s.ctx);
  REQUIRE(u.has_value());

  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "0");
}

// ── U.03: uninstall without ?confirm returns confirmation-required ──

TEST_CASE("U.03: uninstall without confirm returns confirmation-required (409)",
          "[lifecycle_transitions][integration][U.03]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);

  auto u =
      plinth::packages::uninstall_package(rec.id, /*confirmed=*/false, s.ctx);
  REQUIRE_FALSE(u.has_value());
  REQUIRE(u.error().report["kind"] == "confirmation-required");

  // Row UNCHANGED (ACTIVE) — confirmation check runs before any state write.
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "ACTIVE");
}

// ── U.04: uninstall unknown id returns not-found ──────────────────

TEST_CASE("U.04: uninstall unknown id returns not-found (404)",
          "[lifecycle_transitions][integration][U.04]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto u = plinth::packages::uninstall_package(
      "00000000-0000-0000-0000-000000000000", /*confirmed=*/true, s.ctx);
  REQUIRE_FALSE(u.has_value());
  REQUIRE(u.error().report["kind"] == "not-found");
}

// ── U.05: uninstall already-UNINSTALLING returns already-uninstalling ──

TEST_CASE("U.05: already-UNINSTALLING row returns already-uninstalling (409)",
          "[lifecycle_transitions][integration][U.05]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  // Manually seed UNINSTALLING state (mid-uninstall crash simulator).
  PGresult* r =
      PQexec(s.conn, ("UPDATE plinth.packages SET state='UNINSTALLING', "
                      "uninstalling_at = NOW() WHERE id='" +
                      rec.id + "'")
                         .c_str());
  REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
  PQclear(r);

  auto u =
      plinth::packages::uninstall_package(rec.id, /*confirmed=*/true, s.ctx);
  REQUIRE_FALSE(u.has_value());
  REQUIRE(u.error().report["kind"] == "already-uninstalling");
}

// ── U.06: reconciler recovers UNINSTALLING row with rules still present ──

TEST_CASE("U.06: reconciler recovers UNINSTALLING crashed pre-Tx-B",
          "[lifecycle_transitions][integration][U.06]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  // Crash simulator: flip state to UNINSTALLING without touching
  // rbac / caps / panels / schema / files. This matches the "Tx A
  // committed, Tx B never ran" mid-state.
  PGresult* r =
      PQexec(s.conn, ("UPDATE plinth.packages SET state='UNINSTALLING', "
                      "uninstalling_at = NOW() WHERE id='" +
                      rec.id + "'")
                         .c_str());
  REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
  PQclear(r);

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  // All of Tx B + DROP SCHEMA + fs cleanup + row delete must have run.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "0");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM information_schema.schemata "
                   "WHERE schema_name = 'ext_" +
                   rec.name + "'") == "0");
  REQUIRE_FALSE(fs::exists(s.ctx.data_dir / "extensions" / rec.name));
}

// ── U.07: reconciler recovers UNINSTALLING when row is the only leftover ──

TEST_CASE("U.07: reconciler recovers UNINSTALLING crashed pre-row-delete",
          "[lifecycle_transitions][integration][U.07]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto rec = install_valid_notes(s);
  // Advance to a mid-uninstall state where everything except the
  // final DELETE has been cleaned up: strip rbac/caps/panels,
  // drop schema, remove files — then flip state UNINSTALLING.
  PGresult* r = PQexec(
      s.conn,
      (std::string{} +
       "DELETE FROM plinth.group_rules WHERE rule_id IN (SELECT id FROM "
       "plinth.rbac_rules WHERE extension_name='" +
       rec.name + "'); " +
       "DELETE FROM plinth.rbac_rules WHERE extension_name='" + rec.name +
       "'; " + "DELETE FROM plinth.panels WHERE package_id='" + rec.id + "'; " +
       "DELETE FROM plinth.capabilities WHERE extension_name='" + rec.name +
       "'; " + "DROP SCHEMA IF EXISTS ext_" + rec.name + " CASCADE; " +
       "UPDATE plinth.packages SET state='UNINSTALLING', uninstalling_at = "
       "NOW() WHERE id='" +
       rec.id + "'")
          .c_str());
  REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
  PQclear(r);
  std::error_code ec;
  fs::remove_all(s.ctx.data_dir / "extensions" / rec.name, ec);

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" + rec.id +
                   "'") == "0");
}

// ── X.01: upgrade happy path (v1.2.3 → v1.3.0) ──────────────────────

TEST_CASE("X.01: upgrade v1.3.0 over ACTIVE v1.2.3 lands ACTIVE + SUPERSEDED",
          "[lifecycle_transitions][integration][X.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto v1 = install_valid_notes(s);
  REQUIRE(v1.version == "1.2.3");

  auto v2_blob = read_fixture_zip("upgrade-v2");
  REQUIRE(!v2_blob.empty());

  // Dispatch through install_package so the UPLOADING collision
  // classifier routes into upgrade_package (B5 + B6/B7 happy path).
  auto r = plinth::packages::install_package(
      v2_blob, plinth::packages::Provenance::USER, s.ctx);
  if (!r.has_value()) {
    UNSCOPED_INFO("upgrade failed at stage="
                  << plinth::packages::stage_to_string(r.error().failed_at)
                  << " kind=" << r.error().kind
                  << " message=" << r.error().message);
  }
  REQUIRE(r.has_value());

  // New row is ACTIVE at v1.3.0; old row is SUPERSEDED + retired_at set.
  REQUIRE(r->version == "1.3.0");
  REQUIRE(r->state == plinth::packages::InstallStage::ACTIVE);

  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + v1.id +
                   "'") == "SUPERSEDED");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages "
                   "WHERE id='" +
                   v1.id + "' AND retired_at IS NOT NULL") == "1");
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + r->id +
                   "'") == "ACTIVE");
  REQUIRE(
      s.scalar("SELECT supersedes_id::text FROM plinth.packages WHERE id='" +
               r->id + "'") == v1.id);

  // Active symlink now points at v1.3.0; both version dirs on disk.
  fs::path active_link = s.ctx.data_dir / "extensions" / "notes" / "active";
  std::error_code ec;
  auto target = fs::read_symlink(active_link, ec);
  REQUIRE_FALSE(ec);
  REQUIRE(target.filename().string() == "1.3.0");
  REQUIRE(fs::exists(s.ctx.data_dir / "extensions" / "notes" / "1.2.3"));
  REQUIRE(fs::exists(s.ctx.data_dir / "extensions" / "notes" / "1.3.0"));

  // Capability set updated: notes.comment should be present post-upgrade.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='notes' AND function='comment'") ==
          "1");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.capabilities "
                   "WHERE extension_name='notes' AND function='read'") == "1");

  // RBAC reconciliation: notes.comment added, notes.read updated in-place.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                   "WHERE extension_name='notes' AND rule='notes.comment' "
                   "  AND orphaned_at IS NULL") == "1");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                   "WHERE extension_name='notes' AND rule='notes.read' "
                   "  AND orphaned_at IS NULL") == "1");
}

// ── X.02: upgrade rejected when version is the same ──────────────────

TEST_CASE("X.02: upload same version → 409 upgrade-version-not-newer",
          "[lifecycle_transitions][integration][X.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto v1 = install_valid_notes(s);

  auto same_blob = read_fixture_zip("valid-install"); // v1.2.3 again
  auto r = plinth::packages::install_package(
      same_blob, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == "upgrade-version-not-newer");

  // Existing row is untouched.
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + v1.id +
                   "'") == "ACTIVE");
  // No new row for name=notes.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE name='notes'") ==
          "1");
}

// ── X.04: upgrade rejected against DISABLED ──────────────────────────

TEST_CASE("X.04: upload against DISABLED name → 409 disabled-version-present",
          "[lifecycle_transitions][integration][X.04]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto v1 = install_valid_notes(s);
  auto dis = plinth::packages::disable_package(v1.id, s.ctx);
  REQUIRE(dis.has_value());

  auto v2_blob = read_fixture_zip("upgrade-v2");
  auto r = plinth::packages::install_package(
      v2_blob, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == "disabled-version-present");
  // No new row inserted.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE name='notes'") ==
          "1");
}

// ── G.02: garbage_collect deletes eligible SUPERSEDED row + tree ────

TEST_CASE("G.02: GC deletes eligible SUPERSEDED row + tree + NULLs FK",
          "[lifecycle_transitions][integration][G.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Scratch s;
  auto v1 = install_valid_notes(s);
  auto v2_blob = read_fixture_zip("upgrade-v2");
  auto v2 = plinth::packages::install_package(
      v2_blob, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE(v2.has_value());
  wait_rbac_test_settled(s, v2->id);

  // Force the old SUPERSEDED row's retired_at to be past the retention
  // window. (The upgrade set retired_at=NOW() a few ms ago.)
  PGresult* r = PQexec(s.conn, ("UPDATE plinth.packages "
                                "SET retired_at = NOW() - INTERVAL '25 hours' "
                                "WHERE id='" +
                                v1.id + "'")
                                   .c_str());
  REQUIRE(PQresultStatus(r) == PGRES_COMMAND_OK);
  PQclear(r);

  auto gc = plinth::packages::garbage_collect_superseded_versions(
      std::chrono::hours{24}, s.ctx);
  REQUIRE(gc.has_value());
  REQUIRE(std::ranges::find(gc->collected_ids, v1.id) !=
          gc->collected_ids.end());

  // Old row gone; old version tree gone; ACTIVE row untouched; FK nulled.
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" + v1.id +
                   "'") == "0");
  REQUIRE_FALSE(fs::exists(s.ctx.data_dir / "extensions" / "notes" / "1.2.3"));
  REQUIRE(fs::exists(s.ctx.data_dir / "extensions" / "notes" / "1.3.0"));
  REQUIRE(s.scalar("SELECT state FROM plinth.packages WHERE id='" + v2->id +
                   "'") == "ACTIVE");
  REQUIRE(s.scalar("SELECT COUNT(*) FROM plinth.packages "
                   "WHERE id='" +
                   v2->id + "' AND supersedes_id IS NULL") == "1");
}
