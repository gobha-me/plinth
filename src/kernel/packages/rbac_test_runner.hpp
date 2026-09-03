#pragma once

// plinth::packages::rbac_test — RBAC integration test execution
// (ICD-0.4.7).
//
// After a package reaches ACTIVE, the kernel schedules a post-install
// RBAC test run: ephemeral test users are created, each rule's
// `assert_deny` / `assert_allow` is invoked through the normal
// capability dispatch pipeline, the outcome is compared to the declared
// expectation, and the aggregate result is persisted in
// `plinth.packages.last_rbac_test_result`. All-pass leaves the package
// ACTIVE; any-fail flips to ACTIVE_FLAGGED (advisory — extension keeps
// running). The flag clears on any successful re-run.
//
// The RBAC test runner is NOT a sandbox, NOT a new dispatch path, and
// NOT a new authentication surface. It synthesises a `UserContext`,
// calls the real dispatcher, and reads the `std::expected<
// CapabilityResult, CapabilityError>` back. The RBAC check fires inside
// the dispatcher unchanged; the value of the RBAC test is that it
// asserts that check fires the way the extension author said it would.
//
// `assert_allow` side effects are the extension's responsibility —
// the runner does not sandbox capability calls. An extension whose
// `assert_allow` sends real emails during every run has authored an
// extension that spams its users.

#include "kernel/packages/install_lifecycle.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages::rbac_test {

// Per-rule outcome — observable in `RbacTestReport`.
struct RuleOutcome {
  std::string rule;     // "notes.edit"
  std::string clause;   // "assert_deny" | "assert_allow"
  std::string expected; // "permission_denied" | "success"
  std::string actual;   // "permission_denied" | "success" |
                        // "<other CapabilityError variant>" |
                        // "timeout"
  bool passed = false;
  nlohmann::json detail = nlohmann::json::object();
};

// Aggregate RBAC test report. Persisted as JSONB in
// `plinth.packages.last_rbac_test_result`. `overall_passed()` is true
// iff `failed` is empty (an empty-run with only skipped rules is a
// pass).
struct RbacTestReport {
  std::string run_id;
  std::string package_id;
  std::string package_name;
  std::string package_version;
  std::chrono::system_clock::time_point started_at;
  std::chrono::milliseconds duration{0};
  std::vector<RuleOutcome> passed;
  std::vector<RuleOutcome> failed;
  std::vector<std::string> skipped; // rules with NULL test_contract

  [[nodiscard]] auto overall_passed() const -> bool { return failed.empty(); }
};

// Operational-error shape — distinct from per-rule failure. An
// `RbacTestFailure` means the run could not complete (DB unavailable,
// package not found, lock-acquisition failure, etc.). Per-rule
// failures live inside `RbacTestReport.failed`.
struct RbacTestFailure {
  std::string kind; // "package_not_found" | "invalid_state" |
                    // "db_error" | "setup_failed" |
                    // "cleanup_failed" | "lock_failed" | "cancelled"
  std::string message;
  nlohmann::json detail = nlohmann::json::object();
};

// JSON round-trip (matches the `last_rbac_test_result` JSONB shape).
auto to_json(const RuleOutcome& o) -> nlohmann::json;
auto rule_outcome_from_json(const nlohmann::json& j) -> RuleOutcome;
auto to_json(const RbacTestReport& r) -> nlohmann::json;
auto rbac_test_report_from_json(const nlohmann::json& j) -> RbacTestReport;

// Primary synchronous entry. Acquires the per-name advisory lock,
// creates the ephemeral users, runs every rule, tears down, writes
// `last_rbac_test_*`, emits audit. Returns the report on success — both
// "all passed" and "any failed" come back as Ok; distinguish via
// `report.overall_passed()`. Returns `RbacTestFailure` only for
// operational failures that prevent completion.
//
// `triggered_by` threads into the audit event detail (one of
// "install" | "enable" | "upgrade" | "cli" | "reconcile").
//
// `run_id_override` is empty by default — the implementation synthesises
// a fresh UUIDv4 per execution (ICD OQ2). A non-empty value is used
// verbatim; this exists for test harnesses that need deterministic
// ephemeral-user names (`__test_denied_<run_id>` etc.) and for the
// `plinth test rbac --run-id <uuid>` CLI flag.
auto run_rbac_test(std::string_view package_id,
                   const plinth::packages::InstallerContext& ctx,
                   std::string_view triggered_by = "install",
                   std::string_view run_id_override = {})
    -> std::expected<RbacTestReport, RbacTestFailure>;

// Open the owned worker registry for startup. Idempotent when no previous
// workers remain. Returns false if a prior timed-out shutdown still has live
// work and therefore cannot safely reopen admission.
[[nodiscard]] auto start_async_workers() -> bool;

// Close admission, request cancellation for every RBAC worker (including
// nested timed capability calls), and wait up to `timeout` for all of them to
// finish. Returns false on timeout; callers must not tear down dependencies
// used by the remaining workers.
[[nodiscard]] auto shutdown_async_workers(
    std::chrono::milliseconds timeout = std::chrono::seconds{35}) -> bool;

// Test-visible ownership diagnostic.
[[nodiscard]] auto active_async_worker_count_for_test() -> std::size_t;

// Post-install / post-enable / post-upgrade dispatch. Schedules an owned
// cancellable worker that calls `run_rbac_test`; operational failures are
// logged and the reconciler's fresh-install window remains the recovery path.
// Returns immediately. The worker holds its own libpq connection; it does NOT
// share the caller's conn.
//
// `ctx` is value-copied into the worker so it never references the caller's
// stack frame. Scheduling after shutdown is rejected and logged.
auto schedule_rbac_test(std::string_view package_id,
                        const plinth::packages::InstallerContext& ctx,
                        std::string_view triggered_by) -> void;

// CLI options for the `plinth test rbac` subcommand. Populated by
// main.cpp's argparse block; consumed by `run_cli_test_rbac`.
struct CliTestRbacOptions {
  std::string extension_name; // positional — package.name
  bool json_output = false;   // --json
  std::string run_id;         // --run-id — empty = fresh UUIDv4
};

// CLI entry for `plinth test rbac <extension>`. Looks up the active
// package row by name (state IN ('ACTIVE', 'ACTIVE_FLAGGED')), invokes
// `run_rbac_test` with `triggered_by = "cli"`, renders the outcome to
// `out` (JSON if `opts.json_output`, human-readable otherwise), and
// returns the ICD §CLI Surface exit code:
//
//   0 — all rules passed; if the package was ACTIVE_FLAGGED on entry,
//       it is now ACTIVE.
//   1 — any rule failed; package is ACTIVE_FLAGGED.
//   2 — operational error (package not found, not in active state,
//       DB unreachable, lock acquisition failure). Error message is
//       written to `err`.
//
// Shares `run_rbac_test` with the post-install / post-enable / post-
// upgrade triggers — one code path, two entry points.
auto run_cli_test_rbac(const CliTestRbacOptions& opts,
                       const plinth::packages::InstallerContext& ctx,
                       std::ostream& out, std::ostream& err) -> int;

} // namespace plinth::packages::rbac_test
