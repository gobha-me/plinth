// SPDX-License-Identifier: MIT
//
// ICD-0.4.5 §Test Plan G.03 — `garbage_collect_superseded_versions`
// skips a SUPERSEDED row whose per-name advisory lock is held by
// another process. The test forks a child that takes
// `pg_try_advisory_lock(hashtextextended('plinth.packages.<name>', 0))`
// and signals "READY\n" once held. The parent then runs the production
// GC function — which itself opens a libpq conn from
// `InstallerContext::db` and uses `try_acquire_name_lock` (session-
// scope) — and asserts the row is reported in `skipped_ids`. Phase 2
// re-runs GC after the child exits to confirm the lock-release path
// works and the row collects.
//
// The parent runs the real production
// `plinth::packages::garbage_collect_superseded_versions` (no stub, no
// SQL-equivalent) per `feedback_real_code_paths.md`. The child only
// needs to hold a session-scope advisory lock — minimal libpq usage,
// no Drogon dependency.

#include "../ws/ws_test_fixture.hpp"
#include "advisory_lock_harness.hpp"

#include "kernel/packages/install_lifecycle.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

auto unique_data_dir() -> fs::path {
  // counter
  static std::atomic<std::uint64_t> counter{0};
  auto seq = counter.fetch_add(1, std::memory_order_relaxed);
  auto p = fs::temp_directory_path() /
           ("plinth_gc_advisory_" + std::to_string(::getpid()) + "_" +
            std::to_string(seq));
  fs::create_directories(p / "extensions");
  return p;
}

// Insert a SUPERSEDED package row whose retired_at is far enough in
// the past to be GC-eligible against a 0h retention. Returns the row's
// id (UUID as string).
auto seed_superseded_row(plinth::ws_test::TestPg& pg, const std::string& name,
                         const std::string& version) -> std::string {
  const std::string MANIFEST_JSON =
      R"({"name":")" + name + R"(","version":")" + version + R"("})";
  auto r = pg.exec_params(
      "INSERT INTO plinth.packages (name, version, state, provenance, "
      "                              manifest_json, entry_point, "
      "                              manifest_checksum, retired_at) "
      "VALUES ($1, $2, 'SUPERSEDED', 'user', "
      "        $3::jsonb, $4, $5, NOW() - INTERVAL '24 hours') "
      "RETURNING id",
      {name, version, MANIFEST_JSON, "src/index.js", "deadbeef"});
  REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(r.get()) == 1);
  return PQgetvalue(r.get(), 0, 0);
}

} // namespace

TEST_CASE("ICD-0.4.5 G.03: GC skips advisory-locked superseded row",
          "[integration][packages][advisory][G.03]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG unavailable");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  const std::string CANONICAL_NAME = "ext_test_gc_advisory";
  const std::string CANONICAL_VERSION = "0.0.1";

  // Seed a SUPERSEDED row + its on-disk version_dir so GC has both
  // a row to DELETE and a tree to fs::remove_all.
  plinth::ws_test::TestPg pg(cfg.db);
  auto package_id = seed_superseded_row(pg, CANONICAL_NAME, CANONICAL_VERSION);

  auto data_dir = unique_data_dir();
  auto version_dir =
      data_dir / "extensions" / CANONICAL_NAME / CANONICAL_VERSION;
  fs::create_directories(version_dir);
  {
    std::ofstream marker(version_dir / "marker.txt");
    marker << "G.03 GC fixture\n";
  }
  REQUIRE(fs::exists(version_dir));

  plinth::packages::InstallerContext ctx{
      .db = cfg.db,
      .caller_user_id = "",
      .data_dir = data_dir,
      .staging_dir = data_dir / "staging",
  };

  plinth::lock_test::AdvisoryLockHarness harness(cfg.db);

  // ── Phase 1: child holds the lock; parent runs GC; assert skipped. ──
  auto child = harness.run_with_contention(
      /*child_timeout=*/8s,
      [name = CANONICAL_NAME](PGconn* conn, int /*idx*/, int /*total*/) -> int {
        // Mirror install_lifecycle.cpp:177-197 try_acquire_name_lock:
        // SELECT
        // pg_try_advisory_lock(hashtextextended('plinth.packages.<name>', 0))
        const std::string SEED = std::string{"plinth.packages."} + name;
        const std::array<const char*, 1> PARAMS{SEED.c_str()};
        std::unique_ptr<PGresult, decltype(&PQclear)> r{
            PQexecParams(conn,
                         "SELECT pg_try_advisory_lock(hashtextextended($1, 0))",
                         /*nParams=*/1, /*paramTypes=*/nullptr, PARAMS.data(),
                         /*paramLengths=*/nullptr,
                         /*paramFormats=*/nullptr, /*resultFormat=*/0),
            PQclear};
        if (PQresultStatus(r.get()) != PGRES_TUPLES_OK) {
          std::cout << "LOCK_FAIL: " << PQresultErrorMessage(r.get()) << '\n';
          return 1;
        }
        const char* got = PQgetvalue(r.get(), 0, 0);
        if (got == nullptr || std::string{got} != "t") {
          std::cout << "LOCK_NOT_HELD\n";
          return 2;
        }
        // Signal parent: lock is held.
        std::cout << "READY\n";
        std::cout.flush();
        // Hold the lock long enough for the parent's GC + the second
        // assertion. Sleep dominates the harness's 8s timeout so we
        // never release before the parent runs its assertions.
        std::this_thread::sleep_for(3s);
        // Release — pg_advisory_unlock returns true if released.
        std::unique_ptr<PGresult, decltype(&PQclear)> u{
            PQexecParams(conn,
                         "SELECT pg_advisory_unlock(hashtextextended($1, 0))",
                         /*nParams=*/1, /*paramTypes=*/nullptr, PARAMS.data(),
                         /*paramLengths=*/nullptr,
                         /*paramFormats=*/nullptr, /*resultFormat=*/0),
            PQclear};
        (void)u;
        return 0;
      },
      /*parent_fn=*/
      [&] {
        // Run the real production GC under contention.
        auto report = plinth::packages::garbage_collect_superseded_versions(
            /*retention=*/std::chrono::hours{0}, ctx);
        REQUIRE(report.has_value());
        REQUIRE(report->collected_ids.empty());
        REQUIRE(report->skipped_ids.size() == 1);
        REQUIRE(report->skipped_ids[0] == package_id);
        // Row + on-disk dir must still exist (skipped, not collected).
        auto row_r = pg.exec_params(
            "SELECT count(*)::int FROM plinth.packages WHERE id = $1::uuid",
            {package_id});
        REQUIRE(PQresultStatus(row_r.get()) == PGRES_TUPLES_OK);
        REQUIRE(std::string{PQgetvalue(row_r.get(), 0, 0)} == "1");
        REQUIRE(fs::exists(version_dir));
      });
  REQUIRE_FALSE(child.timed_out);
  REQUIRE(child.exit_code == 0);
  REQUIRE(child.stdout_text.find("READY\n") != std::string::npos);

  // ── Phase 2: lock released; parent runs GC; assert collected. ──
  auto report2 = plinth::packages::garbage_collect_superseded_versions(
      /*retention=*/std::chrono::hours{0}, ctx);
  REQUIRE(report2.has_value());
  REQUIRE(report2->collected_ids.size() == 1);
  REQUIRE(report2->collected_ids[0] == package_id);
  REQUIRE(report2->skipped_ids.empty());

  // Row + on-disk dir must be gone.
  auto row_r2 = pg.exec_params(
      "SELECT count(*)::int FROM plinth.packages WHERE id = $1::uuid",
      {package_id});
  REQUIRE(PQresultStatus(row_r2.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(row_r2.get(), 0, 0)} == "0");
  REQUIRE_FALSE(fs::exists(version_dir));
}
