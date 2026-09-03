#pragma once

// plinth::packages — CapabilityManifest typed struct + parser.
//
// Contract: ICD-0.4.1 §CapabilityManifest — Shape and Rules. Reuses
// 0.2.0's per-field validators (validate_namespace/function/version)
// from kernel/capabilities/validation.hpp and 0.2.1's
// parse_signature from kernel/capabilities/parser.hpp for
// requires[] entries.
//
// The error type follows the same accepted-deviation rationale as
// manifest.hpp: vector<ManifestParseError> instead of single error.

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages {

struct CapabilityParam {
  std::string name;
  std::string type;

  auto operator==(const CapabilityParam&) const -> bool = default;
};

struct ProvidedCapability {
  // mandatory; `namespace` is a reserved keyword and the rest of this struct's
  // members are lower_case per project convention.
  std::string namespace_;
  int version = 0;
  std::string function;
  std::vector<CapabilityParam> params;
  std::string returns;
  std::string scope;
  std::string description;
  std::optional<std::string> rbac_rule;

  auto operator==(const ProvidedCapability&) const -> bool = default;
};

struct CapabilityManifestParseResult;

struct CapabilityManifest {
  std::vector<ProvidedCapability> provides;
  // the manifest field `requires` (C++20 identifier-with-special-meaning
  // avoidance; kernel convention per ConnectionRegistry precedent).
  std::vector<std::string> requires_;
  nlohmann::json unknown_fields = nlohmann::json::object();

  static auto parse(std::string_view json_text, std::string_view source_path)
      -> CapabilityManifestParseResult;

  [[nodiscard]] auto serialize() const -> std::string;

  auto operator==(const CapabilityManifest&) const -> bool = default;
};

struct CapabilityManifestParseResult {
  std::optional<CapabilityManifest> value;
  std::vector<ManifestParseError> messages;
};

} // namespace plinth::packages
