#pragma once

// plinth::packages::install_lifecycle — end-to-end install orchestrator.
//
// ICD-0.4.4. `install_package` drives the state machine from UPLOADING
// through ACTIVE (or INSTALL_FAILED). Every state transition persists
// to `plinth.packages.state` so a crash is recoverable via
// `reconcile_in_flight_installs`. Per-name PG advisory lock serialises
// concurrent installs of the same package; different packages proceed
// in parallel.
//
// Terminal audit events only (`packages.installed` on ACTIVE,
// `packages.install_failed{stage, kind}` on INSTALL_FAILED) — per-
// stage state transitions are observable in `plinth.packages.state`
// and `last_install_report`. See ICD OQ #5.

#include "kernel/config.hpp"
#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::packages {

enum class Provenance : std::uint8_t { USER, BUNDLED };

enum class InstallStage : std::uint8_t {
  UPLOADING,
  VALIDATING,
  MIGRATING,
  REGISTERING,
  EXTRACTING,
  ACTIVATING,
  ACTIVE,
  ACTIVE_FLAGGED, // 0.4.7 RBAC test will write this; enum carries it for
                  // decode.
  DISABLED,       // 0.4.5 disable_package.
  INSTALL_FAILED,
  UNINSTALLING, // 0.4.5 uninstall_package.
  SUPERSEDED,   // 0.4.5 upgrade_package (old row after atomic swap).
};

// Transition taxonomy for the 0.4.5 entry points. `install_package`
// retains its own `InstallFailure` shape because UPLOADING-before-
// INSERT failures don't fit the post-INSERT `TransitionFailure`
// contract.
enum class TransitionKind : std::uint8_t {
  DISABLE,
  ENABLE,
  UNINSTALL,
  UPGRADE,
};

[[nodiscard]] auto provenance_to_string(Provenance p) -> std::string_view;
[[nodiscard]] auto stage_to_string(InstallStage s) -> std::string_view;

struct PackageRecord {
  std::string id;
  std::string name;
  std::string version;
  InstallStage state = InstallStage::UPLOADING;
  Provenance provenance = Provenance::USER;
  std::optional<std::string> frontend_mount;
  std::optional<std::string> frontend_entry; // ICD-0.6.1 §4.3
  nlohmann::json manifest_json;
  std::chrono::system_clock::time_point installed_at;
};

struct InstallFailure {
  InstallStage failed_at;
  std::string package_id; // empty if UPLOADING failed before INSERT
  std::string kind;       // e.g. "not-a-zip", "validation-errors"
  std::string message;
  nlohmann::json report; // validation report / migration error / fs error
};

struct TransitionFailure {
  TransitionKind kind;
  std::string package_id; // empty only when upgrade fails pre-INSERT
  std::string message;
  nlohmann::json report; // structured error detail; top-level `kind` per ICD
                         // error taxonomy
};

struct RbacReconciliation {
  std::vector<std::string> added;    // rules new in v2
  std::vector<std::string> updated;  // present in both; fields updated in place
  std::vector<std::string> orphaned; // in v1, absent in v2; orphaned_at set
};

struct UpgradeReport {
  PackageRecord new_record;
  std::string superseded_id; // old row, now SUPERSEDED
  std::chrono::system_clock::time_point retired_at;
  RbacReconciliation rbac;
  std::vector<std::string> warnings;
  std::chrono::milliseconds drain_waited{0};
};

struct GcReport {
  std::vector<std::string> collected_ids; // removed SUPERSEDED rows
  std::vector<std::string> skipped_ids;   // eligible but locked/in-flight
  std::size_t bytes_freed = 0;
  std::vector<std::string> warnings; // filesystem residuals etc.
};

struct GcFailure {
  std::string message;
  std::vector<std::string> partial_collected_ids;
};

struct InstallerContext {
  Config::Database db;               // for register_capability, audit, etc.
  std::string caller_user_id;        // empty → NULL (kernel / bundled)
  std::filesystem::path data_dir;    // {data_dir}/extensions/ is the target
  std::filesystem::path staging_dir; // scratch space
  std::size_t max_package_size_bytes = 50ULL * 1024ULL * 1024ULL;
  std::chrono::milliseconds upgrade_drain_timeout_ms{5000};
};

// Primary entry point. Drives UPLOADING through ACTIVE or
// INSTALL_FAILED. On success: PackageRecord; on failure:
// InstallFailure (plus a `plinth.packages` row in INSTALL_FAILED
// state, unless the failure was pre-INSERT in UPLOADING).
//
// Dry-run mode (ICD-0.4.4 §HTTP Surface line 173, I.19): when
// `dry_run=true`, runs UPLOADING + VALIDATING and returns early
// before MIGRATING. No `plinth.packages` row persists; no schema
// is created; no audit fires. The synthesised `PackageRecord` has
// `id == ""`, `state == VALIDATING`, and the rest of the fields
// populated from the staged manifest. Caller passes a non-null
// `dry_run_report` to receive the validation report JSON; the
// out-param is left untouched on failure (caller reads
// `InstallFailure::report` instead via the regular failure path).
auto install_package(std::span<const std::byte> zip_blob, Provenance provenance,
                     const InstallerContext& ctx, bool dry_run = false,
                     nlohmann::json* dry_run_report = nullptr)
    -> std::expected<PackageRecord, InstallFailure>;

// Crash-recovery reconciler. Iterates every row in an in-flight
// state and advances to ACTIVE (if evidence supports it) or
// INSTALL_FAILED. First-install MIGRATING/REGISTERING/EXTRACTING
// rows also call `drop_schema_and_migrations` to clean up extension
// schema when safe (prior ACTIVE rows with earlier `applied_at`
// inhibit the drop).
//
// Stubbed in Slice A — body ships in Slice B (0.4.4.1).
auto reconcile_in_flight_installs(const InstallerContext& ctx) -> void;

// `install_shell_if_needed` (ICD-0.4.4 slice B) was removed in
// ICD-0.6.1 §3.1; the bundled-shell first-boot install now lives at
// `plinth::shell::ensure_bundled_shell_installed` in
// `kernel/shell/firstboot.{hpp,cpp}` and reads `<bundle_path>/shell.zip`
// from disk per the 0.6.0 OQ1 architect override (replacing the
// linker-embedded blob path).

// 0.4.5 lifecycle transitions beyond first install. Each acquires the
// per-name advisory lock, writes `plinth.packages.state` as its first
// durable action, and emits a terminal audit event on success/failure.
// `InstallerContext` passed by const reference for consistency with
// `install_package`; the ICD sketches non-const but no caller mutates.

// ACTIVE / ACTIVE_FLAGGED → DISABLED. Orphans RBAC rules; unregisters
// capabilities + asset routes. Keeps schema + files for enable.
auto disable_package(std::string_view package_id, const InstallerContext& ctx)
    -> std::expected<PackageRecord, TransitionFailure>;

// DISABLED → ACTIVE. Checksum-verifies on-disk manifest before state
// flip; rematerialises capabilities + asset routes; clears
// `orphaned_at` on every rbac_rules row for the extension.
auto enable_package(std::string_view package_id, const InstallerContext& ctx)
    -> std::expected<PackageRecord, TransitionFailure>;

// Any state → row removed. Multi-tx per ICD §UNINSTALLING: Tx A marks
// UNINSTALLING + uninstalling_at; step-4 unregisters routes/caps; Tx B
// deletes rules + group_rules + panels + capabilities; DROP SCHEMA
// CASCADE (out of tx); fs::remove_all; Tx C deletes row.
// `?confirm=true` → `confirmed=true`; false → `confirmation-required`
// before any state change.
auto uninstall_package(std::string_view package_id, bool confirmed,
                       const InstallerContext& ctx)
    -> std::expected<void, TransitionFailure>;

// ACTIVE + strictly-newer version → atomic swap. Reuses
// UPLOADING/VALIDATING/MIGRATING/REGISTERING/EXTRACTING from
// install_package, then T0–T5 swap: drain old version's in-flight
// capability calls (≤ ctx.upgrade_drain_timeout_ms), commit
// old→SUPERSEDED+retired_at + new→ACTIVE, symlink rename, route +
// capability cutover. Old row retained for GC.
auto upgrade_package(std::span<const std::byte> zip_blob,
                     std::string_view existing_package_id,
                     const InstallerContext& ctx)
    -> std::expected<UpgradeReport, TransitionFailure>;

// GC contract body. Invocation is owned by a 0.7.x scheduler; 0.4.5
// ships the function so that 0.7.x lands as pure wiring. Deletes
// SUPERSEDED rows whose `retired_at < NOW() - retention`; tries a
// non-blocking advisory lock per row (skips contended).
auto garbage_collect_superseded_versions(std::chrono::hours retention,
                                         const InstallerContext& ctx)
    -> std::expected<GcReport, GcFailure>;

// Pure eligibility predicate for a SUPERSEDED row. Eligible iff
// `retired_at + retention <= now`; boundary equality counts as eligible.
// Exposed for unit testing (G.01) independent of the PG-dependent GC
// driver.
[[nodiscard]]
auto is_gc_eligible(std::chrono::system_clock::time_point retired_at,
                    std::chrono::system_clock::time_point now,
                    std::chrono::hours retention) -> bool;

// ICD-0.4.5 §Security Constraint 4 — `rename(2)` is atomic only within
// a single filesystem. The atomic swap depends on
// `{data_dir}/extensions/{name}/*` rename staying on one mountpoint.
// Bootstrap calls this; an `st_dev` mismatch aborts startup with a
// diagnostic. Missing paths are tolerated (bootstrap creates them).
[[nodiscard]]
auto check_single_mountpoint(const std::filesystem::path& data_dir,
                             const std::filesystem::path& staging_dir)
    -> std::expected<void, std::string>;

} // namespace plinth::packages
