#pragma once

// plinth::rbac — RbacManifest typed struct + parser.
//
// Contract: ICD-0.4.6-rbac-rule-registration §RbacManifest — C++
// surface. Mirrors the `plinth::packages::CapabilityManifest::parse`
// pattern from ICD-0.4.1. Errors reuse
// `plinth::packages::ManifestParseError` with the `rbac.*` code
// prefix; no parallel error type per ICD-0.4.6 OQ4.
//
// The `test` object is stored verbatim as `nlohmann::json` so the
// 0.4.7 RBAC test consumer (via the `test_contract JSONB` column) has
// full access to any forward-compatible fields the shape parser does
// not know about. Missing `test` is valid (stored as std::nullopt →
// SQL NULL).

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::rbac {

struct RbacRule {
  std::string rule;
  // keyword; trailing underscore is the project-wide convention (matches
  // ProvidedCapability::namespace_).
  std::string namespace_;
  std::string description;
  std::optional<nlohmann::json> test;

  auto operator==(const RbacRule&) const -> bool = default;
};

// ICD-0.6.1 §7.2 — `default_grants` array entries. Each entry binds
// a freshly-registered rule to a kernel group at install time, so
// extensions don't need a separate post-install admin step to make
// their capabilities reachable. Applied during REGISTERING after the
// rules themselves are upserted.
struct DefaultGrant {
  std::string group;
  std::string rule;

  auto operator==(const DefaultGrant&) const -> bool = default;
};

struct RbacManifest {
  std::vector<RbacRule> rules;
  std::vector<DefaultGrant> default_grants;
  nlohmann::json unknown_fields = nlohmann::json::object();

  [[nodiscard]] auto serialize() const -> std::string;

  auto operator==(const RbacManifest&) const -> bool = default;
};

struct RbacManifestParseResult {
  std::optional<RbacManifest> value;
  std::vector<plinth::packages::ManifestParseError> messages;
};

// Pure: no PG, no filesystem. Takes already-loaded bytes and a source
// path (for error reporting only; the path is not opened). Collects
// all structural errors and warnings in one pass. Returns
// value = nullopt only when the JSON itself fails to parse, when the
// root is not an object, or when `rules` is present but not an array.
// Per-rule structural errors accumulate in `messages`; a rule with a
// structural error is NOT included in `value->rules` (ICD-0.4.0 §R3
// convention — prevents downstream consumers from operating on
// partially-valid data).
auto parse_rbac_manifest(std::string_view json_text,
                         std::string_view source_path)
    -> RbacManifestParseResult;

} // namespace plinth::rbac
