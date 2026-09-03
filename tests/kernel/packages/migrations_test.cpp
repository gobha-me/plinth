// ICD-0.4.3 M.01–M.15 PG-gated integration tests for
// plinth::packages::run_migrations + drop_schema_and_migrations.
// Every TEST_CASE skips with `SKIP("PG not available")` if the
// PLINTH_PG_* env vars are not wired up (matching listener_integration_test
// and audit_test conventions).

#include "kernel/packages/migrations.hpp"
#include "kernel/packages/migrations_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <libpq-fe.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef CMAKE_SOURCE_DIR
#error "CMAKE_SOURCE_DIR must be defined by the build system"
#endif

namespace fs = std::filesystem;
using plinth::packages::drop_schema_and_migrations;
using plinth::packages::MigrationError;
using plinth::packages::MigrationReport;
using plinth::packages::run_migrations;

namespace {

auto env_or(const char* name, const std::string& fallback) -> std::string {
  const char* v = std::getenv(name);
  return (v == nullptr) ? fallback : std::string{v};
}

struct PgEnv {
  std::string host;
  std::string port;
  std::string user;
  std::string password;
  std::string database;
};

auto pg_env() -> PgEnv {
  return PgEnv{
      .host = env_or("PLINTH_PG_HOST", ""),
      .port = env_or("PLINTH_PG_PORT", "5432"),
      .user = env_or("PLINTH_PG_USER", ""),
      .password = env_or("PLINTH_PG_PASSWORD", ""),
      .database = env_or("PLINTH_PG_DATABASE", ""),
  };
}

auto conninfo_of(const PgEnv& env) -> std::string {
  return "host=" + env.host + " port=" + env.port + " dbname=" + env.database +
         " user=" + env.user + " password=" + env.password +
         " connect_timeout=3";
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  auto info = conninfo_of(pg_env());
  PGconn* conn = PQconnectdb(info.c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

struct Pg {
  PGconn* conn = nullptr;

  explicit Pg(const std::string& info) : conn(PQconnectdb(info.c_str())) {}
  ~Pg() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  Pg(const Pg&) = delete;
  auto operator=(const Pg&) -> Pg& = delete;
  Pg(Pg&&) = delete;
  auto operator=(Pg&&) -> Pg& = delete;

  [[nodiscard]] auto exec(const std::string& sql) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    return {PQexec(conn, sql.c_str()), PQclear};
  }
};

auto ensure_plinth_schema(Pg& pg) -> void {
  // Ensure plinth.migrations exists; if a previous test run dropped the
  // schema, re-apply migrations/schema.sql via a direct execution.
  auto r = pg.exec(
      "SELECT 1 FROM information_schema.schemata WHERE schema_name='plinth'");
  if (PQntuples(r.get()) == 0) {
    std::ifstream in{std::string{CMAKE_SOURCE_DIR} + "/migrations/schema.sql"};
    std::ostringstream buf;
    buf << in.rdbuf();
    auto r2 = pg.exec(buf.str());
    REQUIRE(PQresultStatus(r2.get()) == PGRES_COMMAND_OK);
  }
}

auto next_ext_name() -> std::string {
  static std::atomic<std::uint64_t> counter{0};
  auto n = counter.fetch_add(1, std::memory_order_relaxed);
  auto pid = static_cast<std::uint64_t>(::getpid());
  return "test_" + std::to_string(pid) + "_" + std::to_string(n);
}

auto fixtures_root() -> fs::path {
  return fs::path{CMAKE_SOURCE_DIR} / "tests" / "fixtures" /
         "migration_packages";
}

auto fixture(std::string_view name) -> fs::path {
  return fixtures_root() / name;
}

auto replace_all(std::string s, std::string_view from, std::string_view to)
    -> std::string {
  std::string::size_type pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

// Copy a fixture to a unique temp path and substitute {EXT_NAME} in every
// `.sql` file under migrations/. Returns the staged root path. Auto-cleaned
// via StagedFixture RAII.
struct StagedFixture {
  fs::path root;
  StagedFixture() = default;
  explicit StagedFixture(fs::path p) : root(std::move(p)) {}
  ~StagedFixture() {
    if (!root.empty()) {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  }
  StagedFixture(const StagedFixture&) = delete;
  auto operator=(const StagedFixture&) -> StagedFixture& = delete;
  StagedFixture(StagedFixture&&) = delete;
  auto operator=(StagedFixture&&) -> StagedFixture& = delete;
};

auto stage_fixture(std::string_view fixture_name, std::string_view ext_name)
    -> StagedFixture {
  static std::atomic<std::uint64_t> staged_counter{0};
  auto n = staged_counter.fetch_add(1, std::memory_order_relaxed);
  auto dst = fs::temp_directory_path() /
             ("plinth_mig_stage_" + std::to_string(::getpid()) + "_" +
              std::to_string(n));
  std::error_code ec;
  fs::remove_all(dst, ec);
  fs::create_directories(dst);
  fs::copy(fixture(fixture_name), dst, fs::copy_options::recursive, ec);
  REQUIRE_FALSE(ec);
  auto migrations_dir = dst / "migrations";
  if (fs::is_directory(migrations_dir, ec)) {
    for (const auto& entry : fs::directory_iterator{migrations_dir}) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (entry.path().extension() != ".sql") {
        continue;
      }
      std::ifstream in{entry.path(), std::ios::binary};
      std::ostringstream buf;
      buf << in.rdbuf();
      in.close();
      auto body = replace_all(buf.str(), "{EXT_NAME}", ext_name);
      std::ofstream out{entry.path(), std::ios::binary | std::ios::trunc};
      out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
  }
  return StagedFixture{dst};
}

auto has_warning(const MigrationReport& r, std::string_view kind) -> bool {
  return std::ranges::any_of(r.warnings,
                             [&](const auto& w) { return w.kind == kind; });
}

struct ExtensionScope {
  Pg* pg = nullptr;
  std::string name;

  ExtensionScope(Pg& pg_in, std::string name_in)
      : pg(&pg_in), name(std::move(name_in)) {}

  ~ExtensionScope() {
    if (pg != nullptr && pg->conn != nullptr) {
      auto r = drop_schema_and_migrations(name, *pg->conn);
      (void)r;
    }
  }

  ExtensionScope(const ExtensionScope&) = delete;
  auto operator=(const ExtensionScope&) -> ExtensionScope& = delete;
  ExtensionScope(ExtensionScope&&) = delete;
  auto operator=(ExtensionScope&&) -> ExtensionScope& = delete;
};

auto row_count(Pg& pg, const std::string& extension_name) -> int {
  auto r = pg.exec("SELECT COUNT(*) FROM plinth.migrations "
                   "WHERE extension_name = '" +
                   extension_name + "'");
  REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  const char* v = PQgetvalue(r.get(), 0, 0);
  return std::stoi(std::string{v == nullptr ? "0" : v});
}

} // namespace

TEST_CASE("migrations: M.01 empty fixture — schema only",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("empty", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.empty());
  REQUIRE(r->skipped.empty());
  REQUIRE(r->warnings.empty());

  auto s =
      pg.exec("SELECT 1 FROM pg_namespace WHERE nspname = 'ext_" + name + "'");
  REQUIRE(PQntuples(s.get()) == 1);
  auto role =
      pg.exec("SELECT 1 FROM pg_roles WHERE rolname = 'ext_" + name + "_role'");
  REQUIRE(PQntuples(role.get()) == 1);
}

TEST_CASE("migrations: M.02 single migration applies",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("single-migration", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied == std::vector<std::string>{"001_create_foo.sql"});
  REQUIRE(r->skipped.empty());
  REQUIRE(row_count(pg, name) == 1);
}

TEST_CASE("migrations: M.03 three migrations in order",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("three-migrations-in-order", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.size() == 3);
  REQUIRE(r->applied[0] == "001_a.sql");
  REQUIRE(r->applied[1] == "002_b.sql");
  REQUIRE(r->applied[2] == "003_c.sql");
  REQUIRE(row_count(pg, name) == 3);
}

TEST_CASE("migrations: M.04 numeric not lexicographic (010 sorts after 002)",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("three-migrations-010-vs-002", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.size() == 3);
  REQUIRE(r->applied[0] == "001_a.sql");
  REQUIRE(r->applied[1] == "002_b.sql");
  REQUIRE(r->applied[2] == "010_c.sql");
}

TEST_CASE("migrations: M.05 gap emits warning",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("gap-001-004", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.size() == 2);
  REQUIRE(has_warning(*r, "migration-gap"));
}

TEST_CASE("migrations: M.06 duplicate sequence fails",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("duplicate-sequence", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == MigrationError::DUPLICATE_SEQUENCE);
}

TEST_CASE("migrations: M.07 invalid filename fails",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("invalid-filename", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == MigrationError::INVALID_FILENAME);
}

TEST_CASE("migrations: M.08 non-sql extension silently ignored",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("non-sql-extension", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.empty());
  REQUIRE(r->warnings.empty());
}

TEST_CASE("migrations: M.09 no migrations dir is legal",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("no-migrations-dir", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.empty());
  auto s =
      pg.exec("SELECT 1 FROM pg_namespace WHERE nspname = 'ext_" + name + "'");
  REQUIRE(PQntuples(s.get()) == 1);
}

TEST_CASE("migrations: M.10 bad SQL surfaces pg_sqlstate",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("bad-sql", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == MigrationError::MIGRATION_APPLY_FAILED);
  REQUIRE(r.error().pg_sqlstate.has_value());
  REQUIRE(row_count(pg, name) == 0);
}

TEST_CASE("migrations: M.11 checksum mismatch after modification",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("checksum-mismatch", name);

  auto r1 = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r1.has_value());
  REQUIRE(r1->applied.size() == 1);

  // Modify the applied migration file on disk.
  {
    std::ofstream out{staged.root / "migrations" / "001_create.sql",
                      std::ios::binary | std::ios::trunc};
    out << "-- modified after application --\n"
        << "CREATE TABLE ext_" << name << ".tampered (id INT);\n";
  }

  auto r2 = run_migrations(name, staged.root, *pg.conn);
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(r2.error().kind == MigrationError::CHECKSUM_MISMATCH);
}

TEST_CASE("migrations: M.12 idempotent rerun skips applied",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("idempotent-rerun", name);

  auto r1 = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r1.has_value());
  REQUIRE(r1->applied.size() == 2);
  REQUIRE(r1->skipped.empty());

  auto r2 = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r2.has_value());
  REQUIRE(r2->applied.empty());
  REQUIRE(r2->skipped.size() == 2);
}

TEST_CASE("migrations: M.13 role already exists — idempotent reuse",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  // Pre-create the role.
  auto r0 = pg.exec("CREATE ROLE ext_" + name + "_role NOLOGIN");
  REQUIRE(PQresultStatus(r0.get()) == PGRES_COMMAND_OK);

  auto staged = stage_fixture("role-already-exists", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.size() == 1);
}

TEST_CASE("migrations: M.14 advisory lock serializes concurrent installs",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto info = conninfo_of(pg_env());
  Pg setup_pg{info};
  ensure_plinth_schema(setup_pg);
  auto name = next_ext_name();
  ExtensionScope scope{setup_pg, name};

  Pg pg_a{info};
  Pg pg_b{info};
  REQUIRE(PQstatus(pg_a.conn) == CONNECTION_OK);
  REQUIRE(PQstatus(pg_b.conn) == CONNECTION_OK);

  std::promise<void> a_has_lock;
  std::promise<void> release_a;
  auto f_a = std::async(std::launch::async, [&] {
    // Acquire the advisory lock on pg_a and hold it until release_a fires.
    // Uses the same key as run_migrations so pg_b sees contention.
    auto key = std::string{"plinth.migrations."} + name;
    auto* escaped = PQescapeLiteral(pg_a.conn, key.data(), key.size());
    std::string sql = "SELECT pg_advisory_lock(hashtextextended(" +
                      std::string{escaped} + ", 0))";
    PQfreemem(escaped);
    auto r = pg_a.exec(sql);
    REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
    a_has_lock.set_value();
    release_a.get_future().wait();
    auto r2 = pg_a.exec("SELECT pg_advisory_unlock_all()");
    (void)r2;
  });

  a_has_lock.get_future().wait();

  auto staged = stage_fixture("concurrent-same-extension", name);
  auto r = run_migrations(name, staged.root, *pg_b.conn);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == MigrationError::ADVISORY_LOCK_FAILED);

  release_a.set_value();
  f_a.get();
}

TEST_CASE("migrations: M.15 UTF-8 BOM stripped before checksum",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();
  ExtensionScope scope{pg, name};

  auto staged = stage_fixture("non-utf8-bom", name);
  auto r = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r.has_value());
  REQUIRE(r->applied.size() == 1);
  REQUIRE(row_count(pg, name) == 1);
}

TEST_CASE("migrations: drop_schema_and_migrations is idempotent",
          "[packages][migrations][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  Pg pg{conninfo_of(pg_env())};
  ensure_plinth_schema(pg);
  auto name = next_ext_name();

  auto staged = stage_fixture("single-migration", name);
  auto r1 = run_migrations(name, staged.root, *pg.conn);
  REQUIRE(r1.has_value());

  auto d1 = drop_schema_and_migrations(name, *pg.conn);
  REQUIRE(d1.has_value());

  auto s =
      pg.exec("SELECT 1 FROM pg_namespace WHERE nspname = 'ext_" + name + "'");
  REQUIRE(PQntuples(s.get()) == 0);
  auto role =
      pg.exec("SELECT 1 FROM pg_roles WHERE rolname = 'ext_" + name + "_role'");
  REQUIRE(PQntuples(role.get()) == 0);
  REQUIRE(row_count(pg, name) == 0);

  // Second drop is a no-op.
  auto d2 = drop_schema_and_migrations(name, *pg.conn);
  REQUIRE(d2.has_value());
}
