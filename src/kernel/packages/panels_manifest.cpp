#include "kernel/packages/panels_manifest.hpp"
#include "kernel/capabilities/parser.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::packages {

namespace {

constexpr std::size_t ID_MAX_LEN = 64;
constexpr std::size_t TITLE_MAX_LEN = 256;
constexpr std::size_t TYPED_MANIFEST_MAX_BYTES = 256 * 1024;
constexpr std::size_t TYPED_DECLARATION_MAX_BYTES = 16 * 1024;

constexpr std::array<std::string_view, 6> KNOWN_PANEL_FIELDS{
    "id", "client_path", "title", "icon", "rbac_rule", "order",
};

constexpr std::array<std::string_view, 1> KNOWN_TOP_LEVEL{
    "panels",
};

constexpr std::array<std::string_view, 9> TYPED_PANEL_FIELDS{
    "type",
    "capability",
    "content_types",
    "context_schema",
    "accepts_navigation_intent",
    "tray_states",
    "tray_initial_state",
    "preferred_width",
    "chrome_essential",
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
  if (s.empty() || s.front() == '/' || s.back() == '/' ||
      s.find('\\') != std::string_view::npos ||
      s.find('\0') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '/') {
      auto component = s.substr(start, i - start);
      if (component.empty() || component == "." || component == "..") {
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

auto valid_text(std::string_view value, std::size_t minimum,
                std::size_t maximum) -> bool {
  return value.size() >= minimum && value.size() <= maximum &&
         detail::unicode_scalar_count(value).has_value();
}

auto context_field_name(std::string_view value) -> bool {
  return valid_text(value, 1, 64) &&
         value.find('\0') == std::string_view::npos && value != "__proto__" &&
         value != "prototype" && value != "constructor";
}

auto bounded_integer(const nlohmann::json& value, std::uint32_t minimum,
                     std::uint32_t maximum) -> std::optional<std::uint32_t> {
  if (value.is_number_unsigned()) {
    auto number = value.get<std::uint64_t>();
    if (number >= minimum && number <= maximum) {
      return static_cast<std::uint32_t>(number);
    }
  } else if (value.is_number_integer()) {
    auto number = value.get<std::int64_t>();
    if (number >= minimum && number <= maximum) {
      return static_cast<std::uint32_t>(number);
    }
  }
  return std::nullopt;
}

auto exact_keys(const nlohmann::json& value,
                std::initializer_list<std::string_view> names) -> bool {
  return value.is_object() && value.size() == names.size() &&
         std::ranges::all_of(names, [&](auto name) {
           return value.contains(std::string{name});
         });
}

auto schema_field(const nlohmann::json& value)
    -> std::optional<PanelContextField> {
  if (!exact_keys(value, {"required", "max_bytes", "meaning"}) ||
      !value["required"].is_boolean() || !value["meaning"].is_string()) {
    return std::nullopt;
  }
  auto maximum = bounded_integer(value["max_bytes"], 0, 512);
  const auto& meaning = value["meaning"].get_ref<const std::string&>();
  if (!maximum || !valid_text(meaning, 1, 256)) {
    return std::nullopt;
  }
  return PanelContextField{.required = value["required"].get<bool>(),
                           .max_bytes = *maximum,
                           .meaning = meaning};
}

auto context_schema(const nlohmann::json& value)
    -> std::optional<PanelContextSchema> {
  if (!exact_keys(value, {"version", "fields", "persist_keys"}) ||
      bounded_integer(value["version"], 1, 1) != 1 ||
      !value["fields"].is_object() || value["fields"].size() > 16 ||
      !value["persist_keys"].is_array() || value["persist_keys"].size() > 16) {
    return std::nullopt;
  }
  PanelContextSchema schema;
  for (const auto& [key, field] : value["fields"].items()) {
    auto parsed = schema_field(field);
    if (!context_field_name(key) || !parsed) {
      return std::nullopt;
    }
    schema.fields.emplace(key, std::move(*parsed));
  }
  std::set<std::string> persisted;
  for (const auto& key : value["persist_keys"]) {
    if (!key.is_string()) {
      return std::nullopt;
    }
    const auto& name = key.get_ref<const std::string&>();
    if (!schema.fields.contains(name) || !persisted.insert(name).second) {
      return std::nullopt;
    }
    schema.persist_keys.push_back(name);
  }
  return schema;
}

auto literal_content_type(std::string_view input)
    -> std::optional<std::string> {
  // RFC 6838 section 4.2 restricted names, with the ICD's tighter combined
  // 128-byte bound. ASCII case folds only; parameters/wildcards never match.
  if (input.empty() || input.size() > 128) {
    return std::nullopt;
  }
  std::string normalized;
  normalized.reserve(input.size());
  std::size_t separators = 0;
  for (char byte : input) {
    char lower =
        byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
    if (lower == '/') {
      if (normalized.empty() || ++separators != 1) {
        return std::nullopt;
      }
    } else if ((normalized.empty() || normalized.back() == '/') &&
               !((lower >= 'a' && lower <= 'z') ||
                 (lower >= '0' && lower <= '9'))) {
      return std::nullopt;
    } else if (!((lower >= 'a' && lower <= 'z') ||
                 (lower >= '0' && lower <= '9') ||
                 std::string_view{"!#$&^_.+-"}.contains(lower))) {
      return std::nullopt;
    }
    normalized.push_back(lower);
  }
  if (separators != 1 || normalized.back() == '/') {
    return std::nullopt;
  }
  return normalized;
}

auto typed_error(std::size_t idx, std::string_view field, ErrorSink& sink)
    -> void {
  sink.push(make_rule(idx, field, "invalid"),
            "invalid typed panel field '" + std::string{field} + "'",
            make_path(idx, field));
}

auto parse_typed_content(const nlohmann::json& entry, std::size_t idx,
                         ErrorSink& sink, PanelEntry& out) -> void {
  if (!entry.contains("capability") || !entry["capability"].is_string()) {
    typed_error(idx, "capability", sink);
  } else {
    const auto& text = entry["capability"].get_ref<const std::string&>();
    auto signature = plinth::capabilities::parse_signature(text);
    const auto* parsed =
        std::get_if<plinth::capabilities::ParsedSignature>(&signature);
    if (text.size() > 256 || parsed == nullptr) {
      typed_error(idx, "capability", sink);
    } else {
      out.capability = parsed->namespace_ + ":" +
                       std::to_string(parsed->version) + ":" + parsed->function;
    }
  }
  if (!entry.contains("content_types") || !entry["content_types"].is_array() ||
      entry["content_types"].size() > 32) {
    typed_error(idx, "content_types", sink);
    return;
  }
  std::set<std::string> seen;
  // The approved bound is at most 32, not a requirement to advertise a type.
  // An empty list is valid metadata but cannot match any resolver lookup.
  for (const auto& type : entry["content_types"]) {
    auto parsed = type.is_string()
                      ? literal_content_type(type.get_ref<const std::string&>())
                      : std::nullopt;
    if (!parsed || !seen.insert(*parsed).second) {
      typed_error(idx, "content_types", sink);
      return;
    }
    out.content_types.push_back(std::move(*parsed));
  }
}

auto parse_typed_tray(const nlohmann::json& entry, std::size_t idx,
                      ErrorSink& sink, PanelEntry& out) -> void {
  if (!entry.contains("tray_states") || !entry["tray_states"].is_object() ||
      entry["tray_states"].empty() || entry["tray_states"].size() > 16) {
    typed_error(idx, "tray_states", sink);
  } else {
    for (const auto& [name, icon] : entry["tray_states"].items()) {
      if (!detail::is_icon_token_valid(name) || !icon.is_string() ||
          !detail::is_icon_token_valid(icon.get_ref<const std::string&>())) {
        typed_error(idx, "tray_states", sink);
      } else {
        out.tray_states.emplace(name, icon.get<std::string>());
      }
    }
  }
  if (!entry.contains("tray_initial_state") ||
      !entry["tray_initial_state"].is_string() ||
      !out.tray_states.contains(
          entry["tray_initial_state"].get<std::string>())) {
    typed_error(idx, "tray_initial_state", sink);
  } else {
    out.tray_initial_state = entry["tray_initial_state"].get<std::string>();
  }
  if (entry.contains("preferred_width")) {
    out.preferred_width = bounded_integer(entry["preferred_width"], 280, 480);
  }
  if (!out.preferred_width) {
    typed_error(idx, "preferred_width", sink);
  }
  if (entry.contains("chrome_essential")) {
    if (!entry["chrome_essential"].is_boolean()) {
      typed_error(idx, "chrome_essential", sink);
    } else {
      out.chrome_essential = entry["chrome_essential"].get<bool>();
    }
  }
}

auto parse_typed_fields(const nlohmann::json& entry, std::size_t idx,
                        ErrorSink& sink, PanelEntry& out) -> void {
  out.typed_declaration = true;
  if (!entry.contains("type") || !entry["type"].is_string()) {
    typed_error(idx, "type", sink);
    return;
  }
  auto type = entry["type"].get<std::string>();
  if (type == "primary")
    out.type = PanelType::PRIMARY;
  else if (type == "float")
    out.type = PanelType::FLOAT;
  else if (type == "tray")
    out.type = PanelType::TRAY;
  else {
    typed_error(idx, "type", sink);
    return;
  }
  // Keep the existing common title grammar (256 Unicode scalars, including
  // valid escaped NUL), adding only the reviewed v2 1024-byte ceiling.
  // PostgreSQL JSONB remains a separate storage boundary, not parser approval.
  if (out.client_path.size() > 2048 ||
      (out.title && !valid_text(*out.title, 1, 1024))) {
    typed_error(idx, out.client_path.size() > 2048 ? "client_path" : "title",
                sink);
  }
  if (entry.contains("accepts_navigation_intent")) {
    if (!entry["accepts_navigation_intent"].is_boolean()) {
      typed_error(idx, "accepts_navigation_intent", sink);
    } else {
      out.accepts_navigation_intent =
          entry["accepts_navigation_intent"].get<bool>();
    }
  }
  if (out.accepts_navigation_intent && out.type != PanelType::PRIMARY) {
    typed_error(idx, "accepts_navigation_intent", sink);
  }
  if (entry.contains("context_schema")) {
    out.context_schema = context_schema(entry["context_schema"]);
    if (!out.context_schema) typed_error(idx, "context_schema", sink);
  }
  if ((out.type == PanelType::FLOAT || out.accepts_navigation_intent) &&
      !out.context_schema) {
    typed_error(idx, "context_schema", sink);
  }
  if (out.type == PanelType::FLOAT) {
    parse_typed_content(entry, idx, sink, out);
  } else {
    for (auto field : {"capability", "content_types"}) {
      if (entry.contains(field)) typed_error(idx, field, sink);
    }
  }
  if (out.type == PanelType::TRAY) {
    parse_typed_tray(entry, idx, sink, out);
  } else {
    for (auto field : {"tray_states", "tray_initial_state", "preferred_width",
                       "chrome_essential"}) {
      if (entry.contains(field)) typed_error(idx, field, sink);
    }
  }
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
              "with non-empty components other than '.' or '..' and no "
              "backslash or NUL",
              make_path(idx, "client_path"));
    return false;
  }
  return true;
}

auto parse_optional_title(const nlohmann::json& entry, std::size_t idx,
                          ErrorSink& sink, std::optional<std::string>& out)
    -> void {
  constexpr std::string_view field = "title";
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
  auto scalars = detail::unicode_scalar_count(val);
  if (val.empty() || !scalars.has_value()) {
    sink.push(make_rule(idx, field, "invalid"),
              "'title' must be non-empty valid UTF-8", make_path(idx, field));
    return;
  }
  if (*scalars > TITLE_MAX_LEN) {
    sink.push(make_rule(idx, field, "too_long"),
              "'title' exceeds 256 Unicode scalar values",
              make_path(idx, field));
    return;
  }
  out = std::move(val);
}

auto parse_icon(const nlohmann::json& entry, std::size_t idx, ErrorSink& sink,
                PanelEntry& out) -> bool {
  if (!entry.contains("icon")) {
    return true;
  }
  if (!entry["icon"].is_string()) {
    sink.push(make_rule(idx, "icon", "invalid"), "'icon' must be a string",
              make_path(idx, "icon"));
    return false;
  }
  auto value = entry["icon"].get<std::string>();
  if (!detail::is_icon_token_valid(value)) {
    sink.push(make_rule(idx, "icon", "invalid"),
              "'icon' must match ^[a-z][a-z0-9-]{0,63}$",
              make_path(idx, "icon"));
    return false;
  }
  out.icon = std::move(value);
  return true;
}

auto parse_rbac_rule(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, PanelEntry& out) -> bool {
  if (!entry.contains("rbac_rule") || !entry["rbac_rule"].is_string()) {
    sink.push(make_rule(idx, "rbac_rule", "missing"),
              "'rbac_rule' is required and must be a string",
              make_path(idx, "rbac_rule"));
    return false;
  }
  out.rbac_rule = entry["rbac_rule"].get<std::string>();
  if (!detail::is_rbac_rule_name_valid(out.rbac_rule)) {
    sink.push(make_rule(idx, "rbac_rule", "invalid"),
              "'rbac_rule' must use the RBAC rule-name grammar",
              make_path(idx, "rbac_rule"));
    return false;
  }
  return true;
}

auto parse_order(const nlohmann::json& entry, std::size_t idx, ErrorSink& sink,
                 PanelEntry& out) -> bool {
  if (!entry.contains("order")) {
    return true;
  }
  const auto& value = entry["order"];
  bool in_range = false;
  if (value.is_number_unsigned()) {
    in_range = value.get<std::uint64_t>() <= 10000;
  } else if (value.is_number_integer()) {
    auto signed_value = value.get<std::int64_t>();
    in_range = signed_value >= 0 && signed_value <= 10000;
  }
  if (!in_range) {
    sink.push(make_rule(idx, "order", "invalid"),
              "'order' must be an integer in [0, 10000]",
              make_path(idx, "order"));
    return false;
  }
  out.order = value.get<std::uint32_t>();
  return true;
}

auto parse_one_panel(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, PanelEntry& out, bool typed) -> bool {
  if (!entry.is_object()) {
    std::string rule = "panels.panels[" + std::to_string(idx) + "].not_object";
    sink.push(std::move(rule), "'panels' entry must be an object",
              "/panels/" + std::to_string(idx));
    return false;
  }
  bool ok = true;
  ok = parse_id(entry, idx, sink, out) && ok;
  ok = parse_client_path(entry, idx, sink, out) && ok;
  parse_optional_title(entry, idx, sink, out.title);
  ok = parse_icon(entry, idx, sink, out) && ok;
  ok = parse_rbac_rule(entry, idx, sink, out) && ok;
  ok = parse_order(entry, idx, sink, out) && ok;
  if (typed) parse_typed_fields(entry, idx, sink, out);
  for (auto it = entry.begin(); it != entry.end(); ++it) {
    const bool known_typed =
        typed && std::ranges::any_of(TYPED_PANEL_FIELDS, [&](auto field) {
          return field == it.key();
        });
    if (!is_known_panel_field(it.key()) && !known_typed) {
      out.unknown_fields[it.key()] = it.value();
    }
  }
  return ok;
}

auto has_error(const std::vector<ManifestParseError>& msgs) -> bool {
  return std::ranges::any_of(
      msgs, [](const auto& e) { return e.severity == Severity::ERROR; });
}

auto manifest_document(const PanelsManifest& manifest) -> nlohmann::json {
  auto document = manifest.unknown_fields;
  document["panels"] = nlohmann::json::array();
  for (const auto& panel : manifest.panels) {
    document["panels"].push_back(panel.declaration());
  }
  if (manifest.format_version)
    document["format_version"] = *manifest.format_version;
  return document;
}

} // namespace

auto PanelsManifest::parse(std::string_view json_text,
                           std::string_view source_path)
    -> PanelsManifestParseResult {
  PanelsManifestParseResult result;
  ErrorSink sink{.file = std::string{source_path}, .out = &result.messages};

  nlohmann::json j;
  bool duplicate_key = false;
  std::vector<std::set<std::string>> object_keys;
  auto observe_keys = [&](int /* depth */, nlohmann::json::parse_event_t event,
                          nlohmann::json& value) {
    if (event == nlohmann::json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == nlohmann::json::parse_event_t::key) {
      if (!object_keys.back().insert(value.get<std::string>()).second) {
        duplicate_key = true;
      }
    } else if (event == nlohmann::json::parse_event_t::object_end) {
      object_keys.pop_back();
    }
    return true; // Preserve legacy DOM semantics; rejection is v2-only below.
  };
  try {
    j = nlohmann::json::parse(json_text, observe_keys);
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
  const bool typed = j.is_object() && j.contains("format_version");
  if (typed) {
    if (duplicate_key) {
      sink.push("panels.json.duplicate_key",
                "typed panels.json must not contain repeated raw object keys");
      return result;
    }
    if (bounded_integer(j["format_version"], 2, 2) != 2) {
      sink.push("panels.format_version.unsupported",
                "explicit format_version must be integer 2", "/format_version");
      return result;
    }
    m.format_version = 2;
  }
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
      if (!is_known_top_level(it.key()) &&
          !(typed && it.key() == "format_version")) {
        m.unknown_fields[it.key()] = it.value();
      }
    }
  } else {
    sink.push("panels.root.invalid",
              "panels.json top-level value must be an object with a "
              "'panels' array or an array of panel entries");
    return result;
  }

  if (typed && arr->size() > 256) {
    sink.push("panels.panels.too_many", "typed panels.json exceeds 256 entries",
              "/panels");
    return result;
  }

  std::set<std::string> panel_ids;
  for (std::size_t i = 0; i < arr->size(); ++i) {
    PanelEntry pe;
    parse_one_panel((*arr)[i], i, sink, pe, typed);
    if (is_id_valid(pe.id) && !panel_ids.insert(pe.id).second) {
      sink.push(make_rule(i, "id", "duplicate"),
                "duplicate panel id '" + pe.id + "'", make_path(i, "id"));
    }
    // Bounds concern compact normalized UTF-8 JSON, including the defaults
    // declaration() inserts, not raw whitespace or the pre-normalized DOM.
    if (typed && pe.declaration().dump().size() > TYPED_DECLARATION_MAX_BYTES) {
      sink.push(make_rule(i, "declaration", "too_large"),
                "typed panel declaration exceeds 16 KiB",
                "/panels/" + std::to_string(i));
    }
    m.panels.push_back(std::move(pe));
  }

  if (typed && manifest_document(m).dump().size() > TYPED_MANIFEST_MAX_BYTES) {
    sink.push("panels.manifest.too_large",
              "typed panels.json exceeds 256 KiB compact normalized JSON");
  }

  if (!has_error(result.messages)) {
    result.value = std::move(m);
  }
  return result;
}

auto PanelEntry::declaration() const -> nlohmann::json {
  auto entry = nlohmann::json::object();
  for (auto it = unknown_fields.begin(); it != unknown_fields.end(); ++it) {
    entry[it.key()] = it.value();
  }
  entry["id"] = id;
  entry["client_path"] = client_path;
  if (title) {
    entry["title"] = *title;
  }
  if (icon) {
    entry["icon"] = *icon;
  }
  entry["rbac_rule"] = rbac_rule;
  entry["order"] = order;
  if (typed_declaration) {
    entry["type"] = type == PanelType::FLOAT  ? "float"
                    : type == PanelType::TRAY ? "tray"
                                              : "primary";
    entry["accepts_navigation_intent"] = accepts_navigation_intent;
    if (context_schema) {
      auto fields = nlohmann::json::object();
      for (const auto& [name, field] : context_schema->fields) {
        fields[name] = {{"required", field.required},
                        {"max_bytes", field.max_bytes},
                        {"meaning", field.meaning}};
      }
      entry["context_schema"] = {
          {"version", context_schema->version},
          {"fields", std::move(fields)},
          {"persist_keys", context_schema->persist_keys}};
    }
    if (type == PanelType::FLOAT) {
      if (capability) entry["capability"] = *capability;
      entry["content_types"] = content_types;
    }
    if (type == PanelType::TRAY) {
      entry["tray_states"] = tray_states;
      if (tray_initial_state) entry["tray_initial_state"] = *tray_initial_state;
      if (preferred_width) entry["preferred_width"] = *preferred_width;
      entry["chrome_essential"] = chrome_essential;
    }
  }
  return entry;
}

auto PanelsManifest::serialize() const -> std::string {
  return manifest_document(*this).dump(2);
}

} // namespace plinth::packages
