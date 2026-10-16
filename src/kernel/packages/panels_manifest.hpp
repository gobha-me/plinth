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
#include "kernel/packages/panels.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages {

struct PanelContextField {
  bool required = false;
  std::uint32_t max_bytes = 0;
  std::string meaning;

  auto operator==(const PanelContextField&) const -> bool = default;
};

struct PanelContextSchema {
  std::uint32_t version = 1;
  std::map<std::string, PanelContextField> fields;
  std::vector<std::string> persist_keys;

  auto operator==(const PanelContextSchema&) const -> bool = default;
};

struct PanelEntry {
  std::string id;
  std::string client_path;
  std::optional<std::string> title;
  std::optional<std::string> icon;
  std::string rbac_rule;
  std::uint32_t order = 0;
  nlohmann::json unknown_fields = nlohmann::json::object();
  // This is parser provenance for serialization, not installed admission or
  // persistence approval. Only explicit format_version:2 sets these fields.
  bool typed_declaration = false;
  PanelType type = PanelType::PRIMARY;
  std::optional<std::string> capability;
  std::vector<std::string> content_types;
  std::optional<PanelContextSchema> context_schema;
  bool accepts_navigation_intent = false;
  std::map<std::string, std::string> tray_states;
  std::optional<std::string> tray_initial_state;
  std::optional<std::uint32_t> preferred_width;
  bool chrome_essential = false;

  [[nodiscard]] auto declaration() const -> nlohmann::json;

  auto operator==(const PanelEntry&) const -> bool = default;
};

struct PanelsManifestParseResult;

struct PanelsManifest {
  std::vector<PanelEntry> panels;
  nlohmann::json unknown_fields = nlohmann::json::object();
  // Absent means the historical six-field parser; metadata cannot opt in.
  std::optional<std::uint32_t> format_version;

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
