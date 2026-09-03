#pragma once

// plinth::shell::firstboot — bundled-shell first-boot pre-flight.
//
// ICD-0.6.1 §3.1 normative entry point. Replaces the
// `plinth::packages::install_shell_if_needed` linker-embedded blob path
// from ICD-0.4.4 slice B with the on-disk byte source pinned by the
// 0.6.0 OQ1 architect override.
//
// On every kernel boot, after PG bootstrap and crash-recovery and
// before the HTTP listener accepts traffic:
//
//   1. SELECT plinth.packages WHERE provenance='bundled'
//      AND frontend_mount IS NOT NULL
//      AND state IN ('ACTIVE','ACTIVE_FLAGGED') LIMIT 2.
//   2. Zero rows: read `<bundle_path>/shell.zip` from disk and hand
//      bytes to `install_lifecycle::install_package` with
//      `Provenance::BUNDLED`. Emits `shell.firstboot.bundled_install_*`
//      audits at the boundaries.
//   3. One row: short-circuit (no install).
//   4. Two or more rows: ERR_MULTIPLE_ACTIVE_FRONTENDS.
//
// Failure aborts boot via the `FirstBootFailure::exit_code()` value.
// The audit row is persisted (synchronous via `audit_sync`) before
// the error return so operators see the failure cause without
// chasing PG state.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace plinth {
struct Config;
namespace packages {
struct InstallerContext;
} // namespace packages
} // namespace plinth

namespace plinth::shell {

enum class FirstBootError : std::uint8_t {
  BUNDLE_MISSING,            // ERR_BUNDLE_MISSING — exit 1
  BUNDLE_INSTALL_FAILED,     // ERR_BUNDLE_INSTALL_FAILED — exit 2
  MULTIPLE_ACTIVE_FRONTENDS, // ERR_MULTIPLE_ACTIVE_FRONTENDS — exit 3
  SCHEMA_RESERVED,           // ERR_BUNDLE_SCHEMA_RESERVED — exit 3
  DETECTION_FAILED,          // ERR_BUNDLE_DETECTION_FAILED — exit 4
};

struct FirstBootFailure {
  FirstBootError kind;
  std::string message;

  [[nodiscard]] auto exit_code() const noexcept -> int;
  [[nodiscard]] auto kind_string() const noexcept -> std::string_view;
};

// Resolve `cfg.shell.bundle_path` to an absolute filesystem path per
// ICD-0.6.1 §9.2:
//   - non-empty absolute  → used verbatim
//   - non-empty relative  → relative to CWD
//   - empty               → /proc/self/exe-derived; try
//                           `<bin>/share/plinth/bundled` (dev layout)
//                           then `<bin>/../share/plinth/bundled` (FHS).
[[nodiscard]] auto resolve_bundle_path(const std::string& configured)
    -> std::filesystem::path;

// Boot pre-flight per ICD-0.6.1 §3.1. The InstallerContext supplied is
// the same `bootstrap_ctx` main.cpp constructs for
// `reconcile_in_flight_installs` — passed by reference to avoid re-constructing
// the package paths.
auto ensure_bundled_shell_installed(
    const Config& cfg, const packages::InstallerContext& bootstrap_ctx)
    -> std::expected<void, FirstBootFailure>;

} // namespace plinth::shell
