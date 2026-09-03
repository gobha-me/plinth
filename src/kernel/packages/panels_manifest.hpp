#pragma once

// plinth::packages — PanelsManifest typed struct + parser.
//
// Contract: ICD-0.4.2 §Library Surface — typed parser for panels.json,
// consumed by the cross-file validator's CF3 (panel-missing-client-file).
// Shape mirrors capabilities_manifest.hpp (ParseResult<T> with optional
// value + vector<ManifestParseError> messages). DESIGN-packages-v04x.md
// §4.3 establishes `id` as the stable panel identifier (stored in
// plinth.panels.panel_id at install time).

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages {

struct PanelEntry {
  std::string id;
  std::string client_path;
  std::optional<std::string> title;
  std::optional<std::string> icon;
  nlohmann::json unknown_fields = nlohmann::json::object();

  auto operator==(const PanelEntry&) const -> bool = default;
};

struct PanelsManifestParseResult;

struct PanelsManifest {
  std::vector<PanelEntry> panels;
  nlohmann::json unknown_fields = nlohmann::json::object();

  static auto parse(std::string_view json_text, std::string_view source_path)
      -> PanelsManifestParseResult;

  [[nodiscard]] auto serialize() const -> std::string;

  auto operator==(const PanelsManifest&) const -> bool = default;
};

struct PanelsManifestParseResult {
  std::optional<PanelsManifest> value;
  std::vector<ManifestParseError> messages;
};

} // namespace plinth::packages
