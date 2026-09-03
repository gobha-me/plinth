#pragma once

// plinth::packages — PackageManifest typed struct + parser.
//
// Contract: ICD-0.4.1 §PackageManifest — Shape and Rules. The parser
// accepts already-loaded bytes (no filesystem access) and returns a
// ParseResult carrying an optional typed struct plus a vector of
// ManifestParseError entries (errors + warnings). Accepted deviation
// from the ICD's `std::expected<T, ManifestParseError>` signature:
// ParseResult lets 0.4.0 surface every finding (including warnings)
// from a single parse pass without a second `parse_collect` entry
// point — the ICD's §Appendix: Error Example explicitly permits the
// implementing session to pick the collected-errors shape.

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages {

struct RuntimeOverrides {
  std::optional<std::uint64_t> memory_limit_mb;
  std::optional<std::uint64_t> cpu_time_limit_ms;
  std::optional<std::uint64_t> max_stack_depth;

  auto operator==(const RuntimeOverrides&) const -> bool = default;
};

struct FrontendMount {
  std::string mount;
  std::string entry;

  auto operator==(const FrontendMount&) const -> bool = default;
};

struct PackageManifestParseResult;

struct PackageManifest {
  std::string name;
  std::string version;
  std::string description;
  std::string author;
  std::string license;
  std::string entry_point;
  std::optional<FrontendMount> frontend;
  std::optional<RuntimeOverrides> runtime;
  std::vector<nlohmann::json> shareable;

  // Reserved for future CF7 relaxation (ICD-0.4.2 §Open Questions #2):
  // when true, CF7 tolerates `provides[].namespace != name`. Unconditional
  // in 0.4.2 — the field is parsed but never consulted. Activation lands
  // in a later milestone (earliest 0.4.7).
  bool provider_extension = false;

  // Unknown top-level fields preserved verbatim for forward
  // compatibility (DESIGN-packages-v04x.md §7.1).
  nlohmann::json unknown_fields = nlohmann::json::object();

  // ICD-0.6.1 §5.5 / OQ4: when `is_bundled=false` (default; user-uploaded
  // package) the parser rejects `name == "shell"` with rule
  // `manifest.name.reserved`. Bundled-shell first-boot install passes
  // `is_bundled=true` to bypass the reservation for its own canonical
  // manifest. Threaded from `Provenance` at the install_lifecycle layer.
  static auto parse(std::string_view json_text, std::string_view source_path,
                    bool is_bundled = false) -> PackageManifestParseResult;

  [[nodiscard]] auto serialize() const -> std::string;

  auto operator==(const PackageManifest&) const -> bool = default;
};

struct PackageManifestParseResult {
  std::optional<PackageManifest> value;
  std::vector<ManifestParseError> messages;
};

// Compare two SemVer 2.0.0 version strings. Returns -1 if `a < b`,
// 0 if equal, 1 if `a > b`. Build metadata (`+...`) is stripped before
// comparison. Pre-release precedence: a pre-release is lower than the
// same MMP without a pre-release tag. Pre-release vs pre-release is
// compared identifier-by-identifier per the SemVer 2.0.0 rules
// (numeric idents compare numerically, alphanumeric lexically, a
// smaller set of identifiers is lower when all shared identifiers
// are equal). Invalid inputs compare as equal (callers should validate
// with is_semver_valid first — see install_lifecycle's UPLOADING
// stage, which gates on the 0.4.1 manifest parser).
auto compare_semver(std::string_view a, std::string_view b) -> int;

} // namespace plinth::packages
