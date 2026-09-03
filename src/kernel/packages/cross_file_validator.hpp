#pragma once

// plinth::packages — cross-file manifest validation (ICD-0.4.2).
//
// Runs after 0.4.0's R1..R6 and 0.4.1's GlassWorm L1. Answers the
// question "given the already-parsed manifests, are they internally
// consistent?" — namespaces match, file references resolve, RBAC rules
// reference real capabilities, package does not collide with reserved
// kernel names / URL prefixes. Rules CF1..CF7 (errors) and CFW1..CFW4
// (warnings) are deterministic from disk; RT1..RT3 additionally
// consult a running kernel.
//
// The existing public `validate()` entry point in validator.hpp calls
// `run_cross_file_validation` when `cfg.cross_file == true` (the new
// default) and `run_runtime_state_validation` when
// `cfg.against_running_kernel == true`. The two helpers are exposed
// here so the 0.4.4 install lifecycle can drive them directly (without
// the CLI roundtrip) once that milestone lands.

#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/config_manifest.hpp"
#include "kernel/packages/detail/reporter.hpp"
#include "kernel/packages/manifest.hpp"
#include "kernel/packages/panels_manifest.hpp"
#include "kernel/packages/validator.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>

namespace plinth::packages {

// ParseResult-of-all-manifests bundle threaded through both passes.
// Typed fields are only populated if the 0.4.1 parser succeeded. rbac
// stays as raw JSON until 0.4.6 (per ICD-0.4.2 §Out-of-Scope).
struct ParsedPackage {
  std::optional<PackageManifest> manifest;
  std::optional<CapabilityManifest> capabilities;
  std::optional<PanelsManifest> panels;
  std::optional<ConfigManifest> config;
  std::optional<nlohmann::json> rbac_raw;
};

auto run_cross_file_validation(const ParsedPackage& pkg,
                               const std::filesystem::path& package_root,
                               const ValidationConfig& cfg, detail::Reporter& r)
    -> void;

auto run_runtime_state_validation(const ParsedPackage& pkg,
                                  const ValidationConfig& cfg,
                                  detail::Reporter& r) -> void;

} // namespace plinth::packages
