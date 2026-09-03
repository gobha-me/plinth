#pragma once

// plinth::packages — on-disk package structural validator.
//
// Contract: ICD-0.4.0-package-structure-validation — six rules R1..R6
// applied to a directory. Rule-set is per-file / per-entry; cross-file
// semantic validation (ICD-0.4.2) extends `validate()` with a post-parse
// pass (CF1..CF7, CFW1..CFW4) and an optional runtime-state pass
// (RT1..RT3, requires a reachable kernel).

#include "kernel/packages/manifest_error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace plinth::packages {

// Tag on every ValidationMessage so `--json` consumers can filter by
// layer: R1..R6 emit `structure`; CF*/CFW* emit `cross_file`; RT* emit
// `runtime_state`. Additive in 0.4.2; default STRUCTURE preserves the
// 0.4.0 message shape.
enum class Phase : std::uint8_t { STRUCTURE, CROSS_FILE, RUNTIME_STATE };

struct ValidationMessage {
  Severity severity = Severity::ERROR;
  Phase phase = Phase::STRUCTURE;
  std::string rule;
  std::optional<std::string> path;
  std::string message;
  std::optional<std::string> remediation;
};

struct ValidationReport {
  std::vector<ValidationMessage> messages;
  std::size_t files_scanned = 0;
  std::size_t total_bytes = 0;

  [[nodiscard]] auto error_count() const noexcept -> std::size_t;
  [[nodiscard]] auto warning_count() const noexcept -> std::size_t;
  // 0 = pass-no-warnings, 1 = errors present, 2 = warnings only.
  [[nodiscard]] auto disposition() const noexcept -> int;
};

struct ValidationConfig {
  std::size_t max_size_bytes = 50ULL * 1024ULL * 1024ULL; // 50 MB default

  // GlassWorm Layer 1 (ICD-0.4.1 §Layer 1 — Package-Install Gate).
  // Defaults match plinth::Config; the CLI uses these defaults
  // unconditionally (no `plinth validate` flag). Kernel-internal
  // callers (future installer, 0.4.4) override from loaded Config.
  bool unicode_scanner_enabled = true;
  std::size_t unicode_scanner_threshold = 50;
  bool unicode_scanner_log_findings = true;

  // ICD-0.4.2 §Library Surface — cross-file is the new default
  // disposition of `plinth validate`. --structure-only flips this
  // off; --against-running-kernel additionally enables the RT pass.
  bool cross_file = true;
  bool against_running_kernel = false;
  std::optional<std::string> kernel_url;

  // ICD-0.4.4 OQ #4 — additive flag for in-process RT validation.
  // When true, runtime-state probes (RT1/RT2/RT3) use the in-process
  // kernel surfaces (PG + capability cache) rather than the HTTP
  // loopback path the CLI uses. Only the 0.4.4 install lifecycle
  // sets this. Default preserves CLI behavior (HTTP loopback, which
  // is itself stubbed in 0.4.2; see cross_file_validator.cpp).
  bool in_process_registry = false;

  // ICD-0.4.5 §VALIDATING — set to the predecessor `plinth.packages.id`
  // when the validation run is part of an upgrade. RT1 (name collision)
  // whitelists the existing row whose id matches; without it, an upgrade
  // run would fail RT1 against its own upgrade target. Unset on first
  // install and on CLI runs.
  std::optional<std::string> upgrade_from_id;

  // ICD-0.6.1 §5.5 / OQ4 — when true, the manifest parser bypasses
  // the `name == "shell"` reservation. Set by the bundled-shell
  // first-boot install path; left false for all other installs and
  // CLI runs so user-uploaded manifests are rejected at parse time.
  bool is_bundled = false;
};

auto validate(const std::filesystem::path& package_root,
              const ValidationConfig& cfg = {}) -> ValidationReport;

struct RenderOptions {
  bool quiet = false;
  bool colour = false;
};

// Emit human-readable text to `out`. No trailing newline beyond what
// the finding lines produce. Respects `quiet` (no output) and
// `colour` (ANSI escapes around severity prefix).
auto render_text(const ValidationReport& report,
                 const std::filesystem::path& package_root, std::ostream& out,
                 const RenderOptions& opts) -> void;

// Emit the structured JSON shape from ICD-0.4.0 §CLI Contract to
// `out`. Single object, newline-terminated.
auto render_json(const ValidationReport& report,
                 const std::filesystem::path& package_root, std::ostream& out)
    -> void;

} // namespace plinth::packages
