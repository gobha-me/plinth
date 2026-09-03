#pragma once

// plinth::packages — ConfigManifest typed struct + parser.
//
// Contract: ICD-0.4.2 §Library Surface — typed parser for config.json.
// Validates the `schema` object shape: each key is a setting name
// mapped to a scalar default (bool/number/string). Arrays and objects
// are rejected with `config.<key>.non_scalar_default` — richer config
// types land in a later milestone. Follows the ParseResult<T> shape
// (same accepted deviation rationale as 0.4.1's manifest/capabilities
// parsers).

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace plinth::packages {

using ConfigValue =
    std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct ConfigEntry {
  std::string key;
  ConfigValue default_value;

  auto operator==(const ConfigEntry&) const -> bool = default;
};

struct ConfigManifestParseResult;

struct ConfigManifest {
  std::vector<ConfigEntry> entries;
  nlohmann::json unknown_fields = nlohmann::json::object();

  static auto parse(std::string_view json_text, std::string_view source_path)
      -> ConfigManifestParseResult;

  [[nodiscard]] auto serialize() const -> std::string;

  auto operator==(const ConfigManifest&) const -> bool = default;
};

struct ConfigManifestParseResult {
  std::optional<ConfigManifest> value;
  std::vector<ManifestParseError> messages;
};

} // namespace plinth::packages
