#pragma once

// plinth::packages — structured manifest-parse error type.
//
// Shared by manifest.cpp, capabilities_manifest.cpp, and (later) the
// 0.4.6 rbac.json parser. The `rule` field is a stable, grep-friendly
// string of the form "<file-stem>.<field-path>.<failure-mode>" — see
// ICD-0.4.1 §ManifestParseError. CI scripts grep on these; breaking
// them is a contract break.

#include <cstdint>
#include <optional>
#include <string>

namespace plinth::packages {

enum class Severity : std::uint8_t { ERROR, WARNING };

struct ManifestParseError {
  std::string file;
  std::optional<std::size_t> line;
  std::optional<std::size_t> column;
  std::optional<std::string> field_path;
  std::string rule;
  std::string message;
  std::optional<std::string> remediation;
  Severity severity = Severity::ERROR;
};

} // namespace plinth::packages
