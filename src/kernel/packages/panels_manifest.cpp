#include "kernel/packages/panels_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::packages {

namespace {

constexpr std::size_t ID_MAX_LEN = 64;
constexpr std::size_t TITLE_MAX_LEN = 256;

constexpr std::array<std::string_view, 4> KNOWN_PANEL_FIELDS{
    "id",
    "client_path",
    "title",
    "icon",
};

constexpr std::array<std::string_view, 1> KNOWN_TOP_LEVEL{
    "panels",
};

auto is_known_panel_field(std::string_view key) -> bool {
  return std::ranges::any_of(KNOWN_PANEL_FIELDS,
                             [&](std::string_view k) { return k == key; });
}

auto is_known_top_level(std::string_view key) -> bool {
  return std::ranges::any_of(KNOWN_TOP_LEVEL,
                             [&](std::string_view k) { return k == key; });
}

auto is_id_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_';
}

auto is_id_valid(std::string_view s) -> bool {
  if (s.empty() || s.size() > ID_MAX_LEN) {
    return false;
  }
  if (s[0] < 'a' || s[0] > 'z') {
    return false;
  }
  return std::ranges::all_of(s.substr(1), is_id_char);
}

auto is_relative_path_valid(std::string_view s) -> bool {
  if (s.empty() || s[0] == '/') {
    return false;
  }
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '/') {
      if (s.substr(start, i - start) == "..") {
        return false;
      }
      start = i + 1;
    }
  }
  return true;
}

auto line_column_from_byte(std::string_view text, std::size_t byte_offset)
    -> std::pair<std::optional<std::size_t>, std::optional<std::size_t>> {
  if (byte_offset > text.size()) {
    return {std::nullopt, std::nullopt};
  }
  std::size_t line = 1;
  std::size_t column = 1;
  for (std::size_t i = 0; i < byte_offset; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  return {line, column};
}

struct ErrorSink {
  std::string file;
  std::vector<ManifestParseError>* out = nullptr;

  auto push(std::string rule, std::string message,
            std::optional<std::string> field_path = std::nullopt,
            std::optional<std::string> remediation = std::nullopt,
            Severity severity = Severity::ERROR) -> void {
    out->push_back(ManifestParseError{
        .file = file,
        .line = std::nullopt,
        .column = std::nullopt,
        .field_path = std::move(field_path),
        .rule = std::move(rule),
        .message = std::move(message),
        .remediation = std::move(remediation),
        .severity = severity,
    });
  }
};

auto make_path(std::size_t idx, std::string_view field) -> std::string {
  std::string p = "/panels/" + std::to_string(idx) + "/";
  p.append(field);
  return p;
}

auto make_rule(std::size_t idx, std::string_view field, std::string_view fail)
    -> std::string {
  std::string r = "panels.panels[" + std::to_string(idx) + "].";
  r.append(field);
  r.append(".");
  r.append(fail);
  return r;
}

auto parse_id(const nlohmann::json& entry, std::size_t idx, ErrorSink& sink,
              PanelEntry& out) -> bool {
  if (!entry.contains("id") || !entry["id"].is_string()) {
    sink.push(make_rule(idx, "id", "missing"),
              "'id' is required and must be a string", make_path(idx, "id"));
    return false;
  }
  out.id = entry["id"].get<std::string>();
  if (!is_id_valid(out.id)) {
    sink.push(make_rule(idx, "id", "invalid"),
              "'id' must match ^[a-z][a-z0-9_-]*$ (≤ 64 chars)",
              make_path(idx, "id"));
    return false;
  }
  return true;
}

auto parse_client_path(const nlohmann::json& entry, std::size_t idx,
                       ErrorSink& sink, PanelEntry& out) -> bool {
  if (!entry.contains("client_path") || !entry["client_path"].is_string()) {
    sink.push(make_rule(idx, "client_path", "missing"),
              "'client_path' is required and must be a string",
              make_path(idx, "client_path"));
    return false;
  }
  out.client_path = entry["client_path"].get<std::string>();
  if (!is_relative_path_valid(out.client_path)) {
    sink.push(make_rule(idx, "client_path", "invalid_path"),
              "'client_path' must be a non-empty relative path "
              "(no leading '/', no '..' components)",
              make_path(idx, "client_path"));
    return false;
  }
  return true;
}

auto parse_optional_string(const nlohmann::json& entry, std::size_t idx,
                           std::string_view field, std::size_t max_len,
                           ErrorSink& sink, std::optional<std::string>& out)
    -> void {
  std::string key{field};
  if (!entry.contains(key)) {
    return;
  }
  if (!entry[key].is_string()) {
    sink.push(make_rule(idx, field, "invalid"),
              "'" + key + "' must be a string", make_path(idx, field));
    return;
  }
  auto val = entry[key].get<std::string>();
  if (val.size() > max_len) {
    sink.push(make_rule(idx, field, "too_long"),
              "'" + key + "' exceeds " + std::to_string(max_len) +
                  " characters",
              make_path(idx, field));
    return;
  }
  out = std::move(val);
}

auto parse_one_panel(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, PanelEntry& out) -> bool {
  if (!entry.is_object()) {
    std::string rule = "panels.panels[" + std::to_string(idx) + "].not_object";
    sink.push(std::move(rule), "'panels' entry must be an object",
              "/panels/" + std::to_string(idx));
    return false;
  }
  bool ok = true;
  ok = parse_id(entry, idx, sink, out) && ok;
  ok = parse_client_path(entry, idx, sink, out) && ok;
  parse_optional_string(entry, idx, "title", TITLE_MAX_LEN, sink, out.title);
  parse_optional_string(entry, idx, "icon", TITLE_MAX_LEN, sink, out.icon);
  for (auto it = entry.begin(); it != entry.end(); ++it) {
    if (!is_known_panel_field(it.key())) {
      out.unknown_fields[it.key()] = it.value();
    }
  }
  return ok;
}

auto has_error(const std::vector<ManifestParseError>& msgs) -> bool {
  return std::ranges::any_of(
      msgs, [](const auto& e) { return e.severity == Severity::ERROR; });
}

} // namespace

auto PanelsManifest::parse(std::string_view json_text,
                           std::string_view source_path)
    -> PanelsManifestParseResult {
  PanelsManifestParseResult result;
  ErrorSink sink{.file = std::string{source_path}, .out = &result.messages};

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::parse_error& e) {
    auto [ln, col] = line_column_from_byte(json_text, e.byte);
    result.messages.push_back(ManifestParseError{
        .file = std::string{source_path},
        .line = ln,
        .column = col,
        .field_path = std::nullopt,
        .rule = "panels.json.parse_error",
        .message = e.what(),
        .remediation = "ensure the file is syntactically valid JSON",
        .severity = Severity::ERROR,
    });
    return result;
  }

  PanelsManifest m;
  const nlohmann::json* arr = nullptr;

  if (j.is_array()) {
    sink.push("panels.shape.array_at_root",
              "panels.json top-level value is an array; prefer "
              "an object with a 'panels' key",
              std::nullopt, "wrap the array as {\"panels\": [...]}",
              Severity::WARNING);
    arr = &j;
  } else if (j.is_object() && j.contains("panels") && j["panels"].is_array()) {
    arr = &j["panels"];
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (!is_known_top_level(it.key())) {
        m.unknown_fields[it.key()] = it.value();
      }
    }
  } else {
    sink.push("panels.root.invalid",
              "panels.json top-level value must be an object with a "
              "'panels' array or an array of panel entries");
    return result;
  }

  for (std::size_t i = 0; i < arr->size(); ++i) {
    PanelEntry pe;
    parse_one_panel((*arr)[i], i, sink, pe);
    m.panels.push_back(std::move(pe));
  }

  if (!has_error(result.messages)) {
    result.value = std::move(m);
  }
  return result;
}

auto PanelsManifest::serialize() const -> std::string {
  auto out = nlohmann::json::object();
  for (auto it = unknown_fields.begin(); it != unknown_fields.end(); ++it) {
    out[it.key()] = it.value();
  }
  auto arr = nlohmann::json::array();
  for (const auto& pe : panels) {
    auto entry = nlohmann::json::object();
    for (auto it = pe.unknown_fields.begin(); it != pe.unknown_fields.end();
         ++it) {
      entry[it.key()] = it.value();
    }
    entry["id"] = pe.id;
    entry["client_path"] = pe.client_path;
    if (pe.title) {
      entry["title"] = *pe.title;
    }
    if (pe.icon) {
      entry["icon"] = *pe.icon;
    }
    arr.push_back(std::move(entry));
  }
  out["panels"] = arr;
  return out.dump(2);
}

} // namespace plinth::packages
