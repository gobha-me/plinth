// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan I.02 — multi-process events_writer advisory-lock
// single-winner. Forks 4 children that each open a libpq conn and run
// the production writer's lock-and-insert SQL transaction targeting the
// same (channel, emitted_at) tuple. The advisory-xact-lock invariant
// (events_writer.cpp:322-345) is that exactly one transaction commits
// the INSERT and the others observe `got=false` and silently skip
// (ICD-0.5.4 §HA, mirrors the multi-node single-writer contract).
//
// The child path issues the SQL literal-string-equal to the production
// writer's `LOCK_KEY_SQL` macro and INSERT statement. Spinning up a
// Drogon DbClient inside a forked child is impractical — the pool is a
// process-singleton initialised once per Drogon app — so the test
// exercises the SQL invariant directly. Recorded under §Implementation
// deviation in the 0.6.0.N session 3 CHANGELOG entry.

#include "../packages/advisory_lock_harness.hpp"
#include "../ws/ws_test_fixture.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

// Mirror of `events_writer.cpp:131-137` LOCK_KEY_SQL macro. The
// literal-string-equality with production is the load-bearing invariant
// here — if the writer's lock-key SQL changes, this test must move with
// it (and the deviation note in CHANGELOG flags that link). NOTE: no
// `E` prefix on the separator literal — under
// `standard_conforming_strings=on` (PG default since 9.1) `'\u0000'`
// is a six-character literal `\u0000`, not a NUL byte. Production
// relies on this — adding `E'...'` would tell PG to parse the escape
// and reject the embedded NUL.
constexpr const char* LOCK_KEY_SQL =
    "hashtextextended($1::text || '\\u0000' || $2::text, 0)";

} // namespace

TEST_CASE(
    "ICD-0.5.4 I.02: multi-process events_writer advisory-lock single-winner",
    "[integration][realtime][advisory][I.02]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG unavailable");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  // Fixed (channel, emitted_at) — every child contends the same key.
  // Children must compute the same iso string, so we pin it in the
  // parent and pass it through closure-capture into the lambda.
  const std::string CHANNEL = "ext_test:advisory_i02";
  const std::string ISO = "2026-04-27T12:00:00.000Z";
  const std::string PAYLOAD = R"({"msg":"i02"})";

  // Synchronized start: pin a wall-clock instant 500 ms in the future
  // and have every child sleep until that instant before calling
  // `pg_try_advisory_xact_lock`. Production's invariant is "of the
  // writers whose transactions overlap, exactly one acquires the
  // lock"; without synchronized start, fork-ordered children run
  // serially and each "wins" their own non-overlapping window.
  // 500 ms is comfortably longer than fork+conn-open for 4 children
  // and well under the harness's 10 s timeout.
  const auto START_AT_US =
      std::chrono::duration_cast<std::chrono::microseconds>(
          (std::chrono::system_clock::now() + std::chrono::milliseconds{500})
              .time_since_epoch())
          .count();

  plinth::lock_test::AdvisoryLockHarness harness(cfg.db);

  auto outcomes = harness.run(
      /*n=*/4,
      /*child_timeout=*/10s,
      [CHANNEL, ISO, PAYLOAD, START_AT_US](PGconn* conn, int /*idx*/,
                                           int /*total*/) -> int {
        // Mirror events_writer.cpp:322-345 production arm exactly:
        // BEGIN; SELECT pg_try_advisory_xact_lock(<key>); INSERT ...;
        // COMMIT. Drogon's transaction wrapper is hand-rolled here as
        // BEGIN/COMMIT around `PQexecParams` since each child uses
        // raw libpq (not a Drogon DbClient).

        std::unique_ptr<PGresult, decltype(&PQclear)> begin_r{
            PQexec(conn, "BEGIN"), PQclear};
        if (PQresultStatus(begin_r.get()) != PGRES_COMMAND_OK) {
          std::cout << "BEGIN_FAIL: " << PQresultErrorMessage(begin_r.get())
                    << '\n';
          return 1;
        }

        // Synchronized start — sleep until the shared `START_AT_US`
        // wall-clock instant so all 4 children's lock attempts
        // overlap inside PG's lock manager.
        const auto TARGET = std::chrono::system_clock::time_point(
            std::chrono::microseconds{START_AT_US});
        const auto NOW = std::chrono::system_clock::now();
        if (NOW < TARGET) {
          std::this_thread::sleep_for(TARGET - NOW);
        }

        const std::string LOCK_Q =
            std::string{"SELECT pg_try_advisory_xact_lock("} + LOCK_KEY_SQL +
            ") AS got";
        const std::array<const char*, 2> LOCK_PARAMS{CHANNEL.c_str(),
                                                     ISO.c_str()};
        std::unique_ptr<PGresult, decltype(&PQclear)> lock_r{
            PQexecParams(conn, LOCK_Q.c_str(),
                         /*nParams=*/2, /*paramTypes=*/nullptr,
                         LOCK_PARAMS.data(), /*paramLengths=*/nullptr,
                         /*paramFormats=*/nullptr, /*resultFormat=*/0),
            PQclear};
        if (PQresultStatus(lock_r.get()) != PGRES_TUPLES_OK) {
          std::cout << "LOCK_FAIL: " << PQresultErrorMessage(lock_r.get())
                    << '\n';
          std::unique_ptr<PGresult, decltype(&PQclear)> rb_r{
              PQexec(conn, "ROLLBACK"), PQclear};
          return 1;
        }
        const char* got_v = PQgetvalue(lock_r.get(), 0, 0);
        const bool GOT = (got_v != nullptr && std::string{got_v} == "t");
        if (!GOT) {
          // Loser: emit the invariant marker, ROLLBACK, return 0.
          // Production writer also rolls back here (no audit, no
          // error per ICD-0.5.4 §HA).
          std::unique_ptr<PGresult, decltype(&PQclear)> rb_r{
              PQexec(conn, "ROLLBACK"), PQclear};
          std::cout << "SKIPPED\n";
          return 0;
        }

        // Winner: INSERT mirrors events_writer.cpp:337-341 exactly:
        //   INSERT INTO plinth.events (channel, payload)
        //   VALUES ($1, $2::jsonb) RETURNING seq
        // The schema's `created_at` column is set server-side via
        // its NOW() default; the lock key uses (channel, iso) as
        // the contention-point string but `iso` is not stored.
        //
        // Hold the transaction open for 200 ms before the INSERT.
        // pg_try_advisory_xact_lock is non-blocking — losers
        // observe "lock held" and roll back immediately. To make
        // sure all 3 losers have already tried the lock by the
        // time the winner commits (which would release the lock
        // and let a late-comer succeed), sleep here while the
        // lock is held.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});

        const std::array<const char*, 2> INS_PARAMS{CHANNEL.c_str(),
                                                    PAYLOAD.c_str()};
        std::unique_ptr<PGresult, decltype(&PQclear)> ins_r{
            PQexecParams(conn,
                         "INSERT INTO plinth.events (channel, payload) "
                         "VALUES ($1, $2::jsonb) RETURNING seq",
                         /*nParams=*/2, /*paramTypes=*/nullptr,
                         INS_PARAMS.data(), /*paramLengths=*/nullptr,
                         /*paramFormats=*/nullptr, /*resultFormat=*/0),
            PQclear};
        if (PQresultStatus(ins_r.get()) != PGRES_TUPLES_OK) {
          std::cout << "INSERT_FAIL: " << PQresultErrorMessage(ins_r.get())
                    << '\n';
          std::unique_ptr<PGresult, decltype(&PQclear)> rb_r{
              PQexec(conn, "ROLLBACK"), PQclear};
          return 1;
        }

        std::unique_ptr<PGresult, decltype(&PQclear)> commit_r{
            PQexec(conn, "COMMIT"), PQclear};
        if (PQresultStatus(commit_r.get()) != PGRES_COMMAND_OK) {
          std::cout << "COMMIT_FAIL: " << PQresultErrorMessage(commit_r.get())
                    << '\n';
          return 1;
        }
        std::cout << "WON\n";
        return 0;
      });

  // Outcome aggregation.
  REQUIRE(outcomes.size() == 4);
  int won = 0;
  int skipped = 0;
  for (const auto& oc : outcomes) {
    // On any failure path, surface the child's pipe-captured stdout
    // via stderr so CI logs carry the diagnostic. Catch2's INFO/
    // UNSCOPED_INFO machinery is unreliable when an enclosing
    // assertion fails, and fork+race tests are first-of-their-kind
    // in plinth_tests — better to have the marker visible.
    if (oc.timed_out || oc.exit_code != 0) {
      std::cerr << "[I.02] child pid=" << oc.pid << " exit=" << oc.exit_code
                << " timed_out=" << oc.timed_out << " stdout=["
                << oc.stdout_text << "]\n";
    }
    REQUIRE_FALSE(oc.timed_out);
    REQUIRE(oc.exit_code == 0);
    if (oc.stdout_text.find("WON\n") != std::string::npos) {
      ++won;
    }
    if (oc.stdout_text.find("SKIPPED\n") != std::string::npos) {
      ++skipped;
    }
  }
  REQUIRE(won == 1);
  REQUIRE(skipped == 3);

  // Confirm the SQL invariant held end-to-end: exactly one row
  // committed for our channel. `reset_schema` left the table empty
  // and only the lock-winner committed.
  plinth::ws_test::TestPg pg(cfg.db);
  auto row_r = pg.exec_params(
      "SELECT count(*)::int FROM plinth.events WHERE channel = $1", {CHANNEL});
  REQUIRE(PQresultStatus(row_r.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(row_r.get(), 0, 0)} == "1");
}
