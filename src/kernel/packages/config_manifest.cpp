#include "kernel/packages/config_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::packages {

namespace {

constexpr std::size_t KEY_MAX_LEN = 128;

constexpr std::array<std::string_view, 1> KNOWN_TOP_LEVEL{
    "schema",
};

auto is_known_top_level(std::string_view key) -> bool {
  return std::ranges::any_of(KNOWN_TOP_LEVEL,
                             [&](std::string_view k) { return k == key; });
}

auto is_key_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.';
}

auto is_key_valid(std::string_view s) -> bool {
  if (s.empty() || s.size() > KEY_MAX_LEN) {
    return false;
  }
  if (s[0] < 'a' || s[0] > 'z') {
    return false;
  }
  return std::ranges::all_of(s.substr(1), is_key_char);
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

auto convert_scalar(const nlohmann::json& v) -> ConfigValue {
  if (v.is_null()) {
    return std::monostate{};
  }
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_number_integer()) {
    return v.get<std::int64_t>();
  }
  if (v.is_number_float()) {
    return v.get<double>();
  }
  if (v.is_number_unsigned()) {
    return static_cast<std::int64_t>(v.get<std::uint64_t>());
  }
  if (v.is_string()) {
    return v.get<std::string>();
  }
  return std::monostate{};
}

auto parse_schema_entry(std::string_view key, const nlohmann::json& v,
                        ErrorSink& sink, ConfigManifest& m) -> void {
  if (!is_key_valid(key)) {
    std::string rule = "config.";
    rule.append(key);
    rule.append(".invalid_key");
    std::string field_path = "/schema/";
    field_path.append(key);
    sink.push(std::move(rule),
              "config key '" + std::string{key} +
                  "' must match ^[a-z][a-z0-9_.]*$ (≤ 128 chars)",
              std::move(field_path));
    return;
  }
  if (v.is_array() || v.is_object()) {
    std::string rule = "config.";
    rule.append(key);
    rule.append(".non_scalar_default");
    std::string field_path = "/schema/";
    field_path.append(key);
    sink.push(std::move(rule),
              "config default for '" + std::string{key} +
                  "' must be a scalar (bool/number/string); "
                  "arrays and objects are reserved",
              std::move(field_path));
    return;
  }
  m.entries.push_back(ConfigEntry{
      .key = std::string{key},
      .default_value = convert_scalar(v),
  });
}

auto has_error(const std::vector<ManifestParseError>& msgs) -> bool {
  return std::ranges::any_of(
      msgs, [](const auto& e) { return e.severity == Severity::ERROR; });
}

} // namespace

auto ConfigManifest::parse(std::string_view json_text,
                           std::string_view source_path)
    -> ConfigManifestParseResult {
  ConfigManifestParseResult result;
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
        .rule = "config.json.parse_error",
        .message = e.what(),
        .remediation = "ensure the file is syntactically valid JSON",
        .severity = Severity::ERROR,
    });
    return result;
  }

  if (!j.is_object()) {
    sink.push("config.root.not_object",
              "config.json top-level value must be an object");
    return result;
  }

  ConfigManifest m;

  if (j.contains("schema")) {
    const auto& s = j["schema"];
    if (!s.is_object()) {
      sink.push("config.schema.not_object",
                "'schema' must be an object keyed by setting name", "/schema");
    } else {
      for (auto it = s.begin(); it != s.end(); ++it) {
        parse_schema_entry(it.key(), it.value(), sink, m);
      }
    }
  }

  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!is_known_top_level(it.key())) {
      m.unknown_fields[it.key()] = it.value();
    }
  }

  if (!has_error(result.messages)) {
    result.value = std::move(m);
  }
  return result;
}

auto ConfigManifest::serialize() const -> std::string {
  auto out = nlohmann::json::object();
  for (auto it = unknown_fields.begin(); it != unknown_fields.end(); ++it) {
    out[it.key()] = it.value();
  }
  auto schema = nlohmann::json::object();
  for (const auto& e : entries) {
    std::visit(
        [&](const auto& v) {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, std::monostate>) {
            schema[e.key] = nullptr;
          } else {
            schema[e.key] = v;
          }
        },
        e.default_value);
  }
  out["schema"] = schema;
  return out.dump(2);
}

} // namespace plinth::packages
