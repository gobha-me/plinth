#pragma once

// Multi-process advisory-lock harness for tests that need to verify
// PG advisory-lock behavior across separate OS processes (real
// connections, real backends, real lock manager) — single-process
// in-Drogon contention is a different test surface and lives elsewhere.
//
// Built in 0.6.0.N session 3 (advisory-lock harness + G.03 + I.02).
// Closes ICD-0.4.5 G.03 (GC under advisory contention) and ICD-0.5.4
// I.02 (events_writer multi-process advisory-lock single-winner).
//
// Pattern: fork() per child, each opens its own libpq `PGconn*`
// (libpq conns are NOT fork-safe so we never inherit one), runs the
// caller-supplied lambda, then `_exit(rc)` to skip Catch2 destructors
// and reporter flush. Parent reads each child's stdout via a pipe
// (capped at 4 KiB), reaps with `waitpid`, returns outcomes.
//
// Two shapes:
//   * `run(N, ...)` — N children all run in parallel; harness waits
//     for all to exit. Used for I.02-style "racing writers" tests.
//   * `run_with_contention(...)` — 1 child runs, signals "READY\n"
//     after acquiring its lock, then sleeps until either parent_fn
//     returns or `child_timeout` elapses. Parent runs `parent_fn`
//     synchronously while the child holds its lock. Used for G.03-style
//     "child holds, parent acts under contention" tests.

#include "kernel/config.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <libpq-fe.h>
#include <string>
#include <sys/types.h>
#include <vector>

namespace plinth::lock_test {

struct ChildOutcome {
  pid_t pid = -1;
  int exit_code = -1;      // _exit value; 0 = success
  std::string stdout_text; // pipe-captured, capped 4 KiB
  std::chrono::milliseconds wall{0};
  bool timed_out = false; // true → harness sent SIGKILL
};

// Child lambda contract: receives an open libpq conn (caller does NOT
// own — harness closes on child exit), the child's index, and the
// total child count. Returns the desired _exit code (0 for success,
// non-zero for diagnostic). May write to stdout (captured into
// `ChildOutcome::stdout_text`).
using ChildFn = std::function<int(PGconn* conn, int idx, int total)>;

class AdvisoryLockHarness {
 public:
  explicit AdvisoryLockHarness(plinth::Config::Database db);
  ~AdvisoryLockHarness() = default;

  AdvisoryLockHarness(const AdvisoryLockHarness&) = delete;
  auto operator=(const AdvisoryLockHarness&) -> AdvisoryLockHarness& = delete;
  AdvisoryLockHarness(AdvisoryLockHarness&&) = delete;
  auto operator=(AdvisoryLockHarness&&) -> AdvisoryLockHarness& = delete;

  // Fork `n` children, each running `fn(conn, idx, n)`. Block in the
  // parent until every child exits or `child_timeout` elapses (in
  // which case stragglers get SIGKILL and `timed_out=true`). Outcomes
  // are returned in input order (`idx` 0..n-1).
  [[nodiscard]] auto run(int n, std::chrono::milliseconds child_timeout,
                         const ChildFn& fn) -> std::vector<ChildOutcome>;

  // Fork 1 child running `child_fn`; the child must write the literal
  // line "READY\n" (and flush) once it has acquired whatever resource
  // it is contending on. The harness reads the child's pipe until it
  // sees "READY\n", then synchronously invokes `parent_fn` (child is
  // alive, lock still held). After `parent_fn` returns, the harness
  // waits for the child to exit naturally or sends SIGKILL once
  // `child_timeout` elapses.
  [[nodiscard]] auto run_with_contention(
      std::chrono::milliseconds child_timeout, const ChildFn& child_fn,
      const std::function<void()>& parent_fn) -> ChildOutcome;

 private:
  plinth::Config::Database db;
};

// Build a libpq conninfo string from `Config::Database`. Exposed so
// child lambdas can reuse the same connection format the harness
// itself uses (e.g. when a test wants to open a second PGconn inside
// the child for a setup/cleanup query).
[[nodiscard]] auto build_conninfo(const plinth::Config::Database& db)
    -> std::string;

} // namespace plinth::lock_test
