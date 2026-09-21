// X.12 crash injection for the real non-bundled package-upgrade path.
//
// The first child installs v1, upgrades to v2, and is SIGKILLed immediately
// after T3's database COMMIT but before the active symlink is touched. A fresh
// second child then executes the production startup reconciliation sequence.

#include "advisory_lock_harness.hpp"

#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/install_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* value = std::getenv("PLINTH_PG_HOST")) {
    db.host = value;
  }
  if (auto* value = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<std::uint16_t>(std::stoi(value));
  }
  if (auto* value = std::getenv("PLINTH_PG_USER")) {
    db.user = value;
  }
  if (auto* value = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = value;
  }
  if (auto* value = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = value;
  }
  return db;
}

auto pg_available(const plinth::Config::Database& db) -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  PGconn* conn = PQconnectdb(plinth::lock_test::build_conninfo(db).c_str());
  const bool available = conn != nullptr && PQstatus(conn) == CONNECTION_OK;
  if (conn != nullptr) {
    PQfinish(conn);
  }
  return available;
}

auto exec_ignoring_result(PGconn* conn, const std::string& sql) -> void {
  if (auto* result = PQexec(conn, sql.c_str()); result != nullptr) {
    PQclear(result);
  }
}

auto drop_test_schema(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(plinth::lock_test::build_conninfo(db).c_str());
  if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    return;
  }

  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);
  std::unique_ptr<PGresult, decltype(&PQclear)> schemas(
      PQexec(conn, "SELECT schema_name FROM information_schema.schemata "
                   "WHERE schema_name LIKE 'ext\\_%' ESCAPE '\\'"),
      PQclear);
  if (PQresultStatus(schemas.get()) == PGRES_TUPLES_OK) {
    for (int row = 0; row < PQntuples(schemas.get()); ++row) {
      const std::string schema = PQgetvalue(schemas.get(), row, 0);
      exec_ignoring_result(conn,
                           "DROP SCHEMA IF EXISTS " + schema + " CASCADE");
      exec_ignoring_result(conn, "DROP ROLE IF EXISTS " + schema + "_role");
    }
  }
  exec_ignoring_result(conn, "DROP SCHEMA IF EXISTS plinth CASCADE");
}

auto database_artifacts_are_absent(const plinth::Config::Database& db) -> bool {
  PGconn* conn = PQconnectdb(plinth::lock_test::build_conninfo(db).c_str());
  if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    return false;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);
  std::unique_ptr<PGresult, decltype(&PQclear)> result(
      PQexec(conn, "SELECT to_regnamespace('plinth') IS NULL "
                   "AND to_regnamespace('ext_notes') IS NULL "
                   "AND NOT EXISTS (SELECT 1 FROM pg_roles "
                   "WHERE rolname='ext_notes_role')"),
      PQclear);
  return PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
         PQntuples(result.get()) == 1 &&
         std::string_view{PQgetvalue(result.get(), 0, 0)} == "t";
}

auto read_file_bytes(const fs::path& path) -> std::vector<std::byte> {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(size));
  return bytes;
}

auto read_fixture_zip(std::string_view name) -> std::vector<std::byte> {
  return read_file_bytes(fs::path{CMAKE_BINARY_DIR} / "fixtures" /
                         (std::string{name} + ".zip"));
}

auto child_error(std::string_view message, int code) -> int {
  std::cout << "CHILD_FAIL: " << message << '\n';
  std::cout.flush();
  return code;
}

auto configure_child(const plinth::packages::InstallerContext& ctx,
                     plinth::Config& cfg) -> bool {
  cfg.db = ctx.db;
  cfg.packages_data_dir = ctx.data_dir.string();
  cfg.packages_staging_dir = ctx.staging_dir.string();
  auto resolver = plinth::capabilities::init_resolver(ctx.db);
  if (!resolver.has_value()) {
    return false;
  }
  plinth::extensions::init_registry(cfg);
  return true;
}

auto stop_after_t3_commit() noexcept -> void {
  constexpr std::string_view READY = "READY\n";
  const char* cursor = READY.data();
  std::size_t remaining = READY.size();
  while (remaining > 0) {
    const auto written = ::write(STDOUT_FILENO, cursor, remaining);
    if (written > 0) {
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  while (true) {
    (void)::pause();
  }
}

struct PackageRow {
  std::string id;
  std::string version;
  std::string state;
  std::string supersedes_id;
  bool application_ready = false;
  bool retired = false;
};

auto package_rows(const plinth::Config::Database& db)
    -> std::vector<PackageRow> {
  PGconn* conn = PQconnectdb(plinth::lock_test::build_conninfo(db).c_str());
  if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    return {};
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);
  std::unique_ptr<PGresult, decltype(&PQclear)> result(
      PQexec(conn, "SELECT id::text, version, state, supersedes_id::text, "
                   "application_ready::text, (retired_at IS NOT NULL)::text "
                   "FROM plinth.packages "
                   "WHERE name='notes' ORDER BY version"),
      PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
    return {};
  }

  std::vector<PackageRow> rows;
  rows.reserve(static_cast<std::size_t>(PQntuples(result.get())));
  for (int row = 0; row < PQntuples(result.get()); ++row) {
    const std::string_view ready_text{PQgetvalue(result.get(), row, 4)};
    const std::string_view retired_text{PQgetvalue(result.get(), row, 5)};
    rows.push_back({
        .id = PQgetvalue(result.get(), row, 0),
        .version = PQgetvalue(result.get(), row, 1),
        .state = PQgetvalue(result.get(), row, 2),
        .supersedes_id = PQgetisnull(result.get(), row, 3) != 0
                             ? std::string{}
                             : std::string{PQgetvalue(result.get(), row, 3)},
        .application_ready = ready_text == "t" || ready_text == "true",
        .retired = retired_text == "t" || retired_text == "true",
    });
  }
  return rows;
}

auto child_was_reaped(pid_t pid) -> bool {
  int status = 0;
  errno = 0;
  return ::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD;
}

auto wait_for_package_lock_release(const plinth::Config::Database& db,
                                   std::string_view package_name,
                                   std::chrono::milliseconds timeout) -> bool {
  PGconn* conn = PQconnectdb(plinth::lock_test::build_conninfo(db).c_str());
  if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
    if (conn != nullptr) {
      PQfinish(conn);
    }
    return false;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);
  const std::string seed = "plinth.packages." + std::string{package_name};
  const std::array<const char*, 1> params{seed.c_str()};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::unique_ptr<PGresult, decltype(&PQclear)> locked(
        PQexecParams(conn,
                     "SELECT pg_try_advisory_lock(hashtextextended($1, 0))", 1,
                     nullptr, params.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(locked.get()) != PGRES_TUPLES_OK) {
      return false;
    }
    if (PQntuples(locked.get()) == 1 &&
        std::string_view{PQgetvalue(locked.get(), 0, 0)} == "t") {
      std::unique_ptr<PGresult, decltype(&PQclear)> unlocked(
          PQexecParams(conn,
                       "SELECT pg_advisory_unlock(hashtextextended($1, 0))", 1,
                       nullptr, params.data(), nullptr, nullptr, 0),
          PQclear);
      return PQresultStatus(unlocked.get()) == PGRES_TUPLES_OK &&
             PQntuples(unlocked.get()) == 1 &&
             std::string_view{PQgetvalue(unlocked.get(), 0, 0)} == "t";
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

std::atomic<std::uint64_t> g_scratch_counter{0};

struct Scratch {
  plinth::Config::Database db = pg_config();
  fs::path base;
  plinth::packages::InstallerContext ctx;
  bool cleaned = false;

  Scratch() {
    const auto suffix = std::to_string(::getpid()) + "_" +
                        std::to_string(g_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_x12_" + suffix);
    fs::create_directories(base / "data");
    fs::create_directories(base / "staging");

    drop_test_schema(db);
    plinth::db::bootstrap_schema(
        db, std::string{CMAKE_SOURCE_DIR} + "/migrations", /*dev_mode=*/true);
    plinth::groups::bootstrap_groups(db);

    ctx.db = db;
    ctx.data_dir = base / "data";
    ctx.staging_dir = base / "staging";
    ctx.schedule_rbac_tests = false;
  }

  ~Scratch() { cleanup(); }

  auto cleanup() -> void {
    if (cleaned) {
      return;
    }
    plinth::packages::test_seam::clear_upgrade_swap_committed_hook();
    std::error_code error;
    fs::remove_all(base, error);
    drop_test_schema(db);
    cleaned = true;
  }

  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  Scratch(Scratch&&) = delete;
  auto operator=(Scratch&&) -> Scratch& = delete;
};

} // namespace

TEST_CASE("X.12: restart repairs upgrade killed after T3 commit",
          "[integration][subprocess][packages][lifecycle][X.12]") {
  const auto db = pg_config();
  if (!pg_available(db)) {
    SKIP("PG not available");
  }

  Scratch scratch;
  plinth::lock_test::AdvisoryLockHarness harness(scratch.db);

  const auto crashed = harness.run_until_ready_and_kill(
      60s, [&scratch](PGconn*, int, int) -> int {
        plinth::Config cfg;
        if (!configure_child(scratch.ctx, cfg)) {
          return child_error("resolver initialization failed", 10);
        }

        const auto v1_blob = read_fixture_zip("valid-install");
        if (v1_blob.empty()) {
          return child_error("valid-install fixture missing", 11);
        }
        auto v1 = plinth::packages::install_package(
            v1_blob, plinth::packages::Provenance::USER, scratch.ctx);
        if (!v1.has_value()) {
          return child_error("v1 install failed: " + v1.error().message, 12);
        }

        const auto v2_blob = read_fixture_zip("upgrade-v2");
        if (v2_blob.empty()) {
          return child_error("upgrade-v2 fixture missing", 13);
        }
        plinth::packages::test_seam::set_upgrade_swap_committed_hook(
            &stop_after_t3_commit);
        auto upgraded = plinth::packages::upgrade_package(
            v2_blob, v1->id, scratch.ctx, plinth::packages::Provenance::USER);
        plinth::packages::test_seam::clear_upgrade_swap_committed_hook();
        if (!upgraded.has_value()) {
          return child_error("v2 upgrade failed: " + upgraded.error().message,
                             14);
        }
        return child_error("T3 crash hook did not stop the child", 15);
      });

  INFO(crashed.stdout_text);
  REQUIRE(crashed.checkpoint_reached);
  REQUIRE(crashed.kill_requested);
  REQUIRE_FALSE(crashed.timed_out);
  REQUIRE(crashed.exit_code == -SIGKILL);
  REQUIRE(child_was_reaped(crashed.pid));
  REQUIRE(wait_for_package_lock_release(scratch.db, "notes", 10s));

  auto rows = package_rows(scratch.db);
  REQUIRE(rows.size() == 2);
  const auto& old_row = rows.at(0);
  const auto& new_row = rows.at(1);
  REQUIRE(old_row.version == "1.2.3");
  REQUIRE(old_row.state == "SUPERSEDED");
  REQUIRE(old_row.retired);
  REQUIRE(new_row.version == "1.3.0");
  REQUIRE(new_row.state == "ACTIVE");
  REQUIRE(new_row.supersedes_id == old_row.id);
  REQUIRE_FALSE(new_row.application_ready);

  const auto extension_root = scratch.ctx.data_dir / "extensions" / "notes";
  REQUIRE(fs::is_directory(extension_root / old_row.version));
  REQUIRE(fs::is_directory(extension_root / new_row.version));
  REQUIRE(fs::read_symlink(extension_root / "active").filename() ==
          old_row.version);
  REQUIRE_FALSE(fs::exists(extension_root / "active.tmp"));

  auto restarted = harness.run(1, 60s, [&scratch](PGconn*, int, int) -> int {
    plinth::Config cfg;
    if (!configure_child(scratch.ctx, cfg)) {
      return child_error("restart resolver initialization failed", 20);
    }
    auto reconciled =
        plinth::packages::reconcile_in_flight_installs(scratch.ctx);
    if (!reconciled.has_value()) {
      return child_error("package reconciliation failed: " + reconciled.error(),
                         21);
    }
    plinth::packages::asset_server::restore_routes(scratch.db,
                                                   scratch.ctx.data_dir);
    auto readiness =
        plinth::packages::reconcile_application_readiness(scratch.ctx);
    if (!readiness.has_value()) {
      return child_error("application readiness failed: " + readiness.error(),
                         22);
    }
    if (!plinth::packages::asset_server::has_registered_route("notes",
                                                              "1.3.0")) {
      return child_error("v2 route was not restored", 23);
    }
    if (!plinth::extensions::has_pool("notes")) {
      return child_error("v2 runtime pool was not restored", 24);
    }
    if (!plinth::extensions::shutdown_registry()) {
      return child_error("runtime registry shutdown timed out", 25);
    }
    return 0;
  });

  REQUIRE(restarted.size() == 1);
  INFO(restarted.front().stdout_text);
  REQUIRE_FALSE(restarted.front().timed_out);
  REQUIRE(restarted.front().exit_code == 0);
  REQUIRE(child_was_reaped(restarted.front().pid));

  rows = package_rows(scratch.db);
  REQUIRE(rows.size() == 2);
  REQUIRE(rows.at(0).state == "SUPERSEDED");
  REQUIRE(rows.at(0).retired);
  REQUIRE(rows.at(1).state == "ACTIVE");
  REQUIRE(rows.at(1).supersedes_id == rows.at(0).id);
  REQUIRE(rows.at(1).application_ready);
  REQUIRE(fs::is_directory(extension_root / rows.at(0).version));
  REQUIRE(fs::is_directory(extension_root / rows.at(1).version));
  REQUIRE(fs::read_symlink(extension_root / "active").filename() ==
          rows.at(1).version);
  REQUIRE_FALSE(fs::exists(extension_root / "active.tmp"));

  scratch.cleanup();
  REQUIRE_FALSE(fs::exists(scratch.base));
  REQUIRE(database_artifacts_are_absent(scratch.db));
}
