#pragma once

#include "kernel/packages/manifest_error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages {

enum class HttpHandlerMode : std::uint8_t { QUICKJS, SIDECAR, BUNDLED_NATIVE };

struct HttpDeclarationParseResult;

// Submission data only, never a claim receipt, committed reservation, adapter
// readiness/provenance proof or permission grant. Neutral PackageManifest reads
// deliberately continue preserving unknown legacy fields without activating
// them.
struct HttpDeclaration {
  std::vector<std::string> http_prefixes;
  std::vector<std::string> unauthenticated_prefixes;
  std::optional<HttpHandlerMode> handler_mode;

  // Parse ORIGINAL manifest bytes: decoded duplicate object keys are rejected
  // before DOM overwrite, including keys outside the HTTP fields. Successful
  // arrays are byte-sorted. Current fixed kernel mounts are checked here;
  // configured frontend/installed claims and adapter readiness are not.
  static auto parse(std::string_view manifest_bytes,
                    std::string_view source_path = "manifest.json")
      -> HttpDeclarationParseResult;

  auto operator==(const HttpDeclaration&) const -> bool = default;
};

struct HttpDeclarationParseResult {
  std::optional<HttpDeclaration> value;
  std::vector<ManifestParseError> messages;
};

[[nodiscard]] auto is_valid_http_claim(std::string_view claim) -> bool;

// Paths must already have passed the HTTP target validator. This function
// neither splits queries nor decodes/normalizes paths. Invalid claims fail
// closed; a terminal subtree includes its base, trailing slash and descendants.
[[nodiscard]] auto http_claim_matches(std::string_view claim,
                                      std::string_view path) -> bool;
[[nodiscard]] auto http_claims_intersect(std::string_view left,
                                         std::string_view right) -> bool;

} // namespace plinth::packages
