#include "kernel/packages/capabilities_manifest.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::packages {

namespace {

constexpr std::size_t DESCRIPTION_MAX_LEN = 1024;

// ICD-0.6.1 §7.1 — `"any"` accepts any JSONB-serialisable value
// (used by shell.preferences.set's `value` param). The handler is
// responsible for runtime shape checks; the manifest declares
// "anything goes here, validate server-side".
constexpr std::array<std::string_view, 7> PARAM_TYPE_LITERALS{
    "string", "number", "boolean", "object", "array", "buffer", "any",
};

constexpr std::array<std::string_view, 2> KNOWN_TOP_LEVEL{
    "provides",
    "requires",
};

auto is_known_top_level(std::string_view key) -> bool {
  return std::ranges::any_of(KNOWN_TOP_LEVEL,
                             [&](std::string_view k) { return k == key; });
}

auto is_valid_param_first_char(char first) -> bool {
  return (first >= 'a' && first <= 'z') || first == '_';
}

auto is_valid_param_body_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

auto is_valid_param_name(std::string_view s) -> bool {
  if (s.empty() || !is_valid_param_first_char(s[0])) {
    return false;
  }
  return std::ranges::all_of(s.substr(1), is_valid_param_body_char);
}

auto is_valid_param_type(std::string_view s) -> bool {
  return std::ranges::any_of(PARAM_TYPE_LITERALS,
                             [&](std::string_view t) { return t == s; });
}

auto is_valid_rbac_segment_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// rbac_rule: ^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$ — 2..5 dot-separated
// segments, each segment starts with a lowercase letter followed by optional
// lowercase alnum.
auto is_valid_rbac_rule(std::string_view s) -> bool {
  if (s.empty()) {
    return false;
  }
  std::size_t i = 0;
  std::size_t segments = 0;
  while (i < s.size()) {
    if (s[i] < 'a' || s[i] > 'z') {
      return false;
    }
    ++i;
    while (i < s.size() && s[i] != '.') {
      if (!is_valid_rbac_segment_char(s[i])) {
        return false;
      }
      ++i;
    }
    ++segments;
    if (i < s.size() && s[i] == '.') {
      if (i + 1 == s.size()) {
        return false; // trailing dot
      }
      ++i;
    }
  }
  return segments >= 2 && segments <= 5;
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
  std::string p = "/provides/" + std::to_string(idx) + "/";
  p.append(field);
  return p;
}

auto make_rule(std::size_t idx, std::string_view field, std::string_view fail)
    -> std::string {
  std::string r = "capabilities.provides[" + std::to_string(idx) + "].";
  r.append(field);
  r.append(".");
  r.append(fail);
  return r;
}

auto parse_namespace(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.contains("namespace") || !entry["namespace"].is_string()) {
    sink.push(make_rule(idx, "namespace", "missing"),
              "'namespace' is required and must be a string",
              make_path(idx, "namespace"));
    return false;
  }
  out.namespace_ = entry["namespace"].get<std::string>();
  if (plinth::capabilities::validate_namespace(out.namespace_)) {
    sink.push(make_rule(idx, "namespace", "invalid"),
              "'namespace' fails capability-registry validation",
              make_path(idx, "namespace"));
    return false;
  }
  return true;
}

auto parse_version(const nlohmann::json& entry, std::size_t idx,
                   ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.contains("version")) {
    sink.push(make_rule(idx, "version", "missing"), "'version' is required",
              make_path(idx, "version"));
    return false;
  }
  if (!entry["version"].is_number_integer()) {
    sink.push(make_rule(idx, "version", "invalid"),
              "'version' must be an integer", make_path(idx, "version"));
    return false;
  }
  out.version = entry["version"].get<int>();
  if (plinth::capabilities::validate_version(out.version)) {
    sink.push(make_rule(idx, "version", "invalid"), "'version' must be >= 1",
              make_path(idx, "version"));
    return false;
  }
  return true;
}

auto parse_function(const nlohmann::json& entry, std::size_t idx,
                    ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.contains("function") || !entry["function"].is_string()) {
    sink.push(make_rule(idx, "function", "missing"),
              "'function' is required and must be a string",
              make_path(idx, "function"));
    return false;
  }
  out.function = entry["function"].get<std::string>();
  if (plinth::capabilities::validate_function(out.function)) {
    sink.push(make_rule(idx, "function", "invalid"),
              "'function' fails capability-registry validation",
              make_path(idx, "function"));
    return false;
  }
  return true;
}

auto parse_param_entry(const nlohmann::json& pe, std::size_t idx,
                       std::size_t pi, ErrorSink& sink,
                       std::vector<CapabilityParam>& out) -> bool {
  auto ppath = [idx, pi](std::string_view field) {
    return make_path(idx, "params") + "/" + std::to_string(pi) + "/" +
           std::string(field);
  };
  auto prule = [idx, pi](std::string_view field, std::string_view fail) {
    return "capabilities.provides[" + std::to_string(idx) + "].params[" +
           std::to_string(pi) + "]." + std::string(field) + "." +
           std::string(fail);
  };
  if (!pe.is_object()) {
    sink.push(prule("", "not_object"), "'params' entry must be an object",
              ppath(""));
    return false;
  }
  CapabilityParam cp;
  bool ok = true;
  if (!pe.contains("name") || !pe["name"].is_string() ||
      !is_valid_param_name(pe["name"].get<std::string>())) {
    sink.push(prule("name", "invalid"),
              "'params[].name' must match ^[a-z_][a-zA-Z0-9_]*$",
              ppath("name"));
    ok = false;
  } else {
    cp.name = pe["name"].get<std::string>();
  }
  if (!pe.contains("type") || !pe["type"].is_string() ||
      !is_valid_param_type(pe["type"].get<std::string>())) {
    sink.push(prule("type", "invalid"),
              "'params[].type' must be one of "
              "string/number/boolean/object/array/buffer",
              ppath("type"));
    ok = false;
  } else {
    cp.type = pe["type"].get<std::string>();
  }
  out.push_back(std::move(cp));
  return ok;
}

auto parse_params(const nlohmann::json& entry, std::size_t idx, ErrorSink& sink,
                  ProvidedCapability& out) -> bool {
  if (!entry.contains("params")) {
    return true;
  }
  const auto& p = entry["params"];
  if (!p.is_array()) {
    sink.push(make_rule(idx, "params", "not_array"),
              "'params' must be an array", make_path(idx, "params"));
    return false;
  }
  bool ok = true;
  for (std::size_t pi = 0; pi < p.size(); ++pi) {
    if (!parse_param_entry(p[pi], idx, pi, sink, out.params)) {
      ok = false;
    }
  }
  return ok;
}

auto parse_returns(const nlohmann::json& entry, std::size_t idx,
                   ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.contains("returns") || !entry["returns"].is_string()) {
    sink.push(make_rule(idx, "returns", "missing"),
              "'returns' is required and must be a string",
              make_path(idx, "returns"));
    return false;
  }
  out.returns = entry["returns"].get<std::string>();
  if (out.returns.empty()) {
    sink.push(make_rule(idx, "returns", "missing"),
              "'returns' must be non-empty", make_path(idx, "returns"));
    return false;
  }
  return true;
}

auto parse_scope(const nlohmann::json& entry, std::size_t idx, ErrorSink& sink,
                 ProvidedCapability& out) -> bool {
  if (!entry.contains("scope") || !entry["scope"].is_string()) {
    sink.push(make_rule(idx, "scope", "missing"),
              "'scope' is required and must be a string",
              make_path(idx, "scope"));
    return false;
  }
  out.scope = entry["scope"].get<std::string>();
  if (out.scope != "instance" && out.scope != "user") {
    sink.push(make_rule(idx, "scope", "invalid"),
              "'scope' must be 'instance' or 'user'", make_path(idx, "scope"));
    return false;
  }
  return true;
}

auto parse_description(const nlohmann::json& entry, std::size_t idx,
                       ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.contains("description") || !entry["description"].is_string()) {
    sink.push(make_rule(idx, "description", "missing"),
              "'description' is required and must be a string",
              make_path(idx, "description"));
    return false;
  }
  out.description = entry["description"].get<std::string>();
  if (out.description.empty()) {
    sink.push(make_rule(idx, "description", "missing"),
              "'description' must be non-empty", make_path(idx, "description"));
    return false;
  }
  if (out.description.size() > DESCRIPTION_MAX_LEN) {
    sink.push(make_rule(idx, "description", "too_long"),
              "'description' exceeds 1024 characters",
              make_path(idx, "description"));
    return false;
  }
  return true;
}

auto parse_rbac_rule(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.contains("rbac_rule")) {
    return true;
  }
  if (!entry["rbac_rule"].is_string()) {
    sink.push(make_rule(idx, "rbac_rule", "invalid"),
              "'rbac_rule' must be a string", make_path(idx, "rbac_rule"));
    return false;
  }
  auto rr = entry["rbac_rule"].get<std::string>();
  if (!is_valid_rbac_rule(rr)) {
    sink.push(make_rule(idx, "rbac_rule", "invalid"),
              "'rbac_rule' must match ^[a-z][a-z0-9]*(\\.[a-z][a-z0-9]*){1,4}$",
              make_path(idx, "rbac_rule"));
    return false;
  }
  out.rbac_rule = std::move(rr);
  return true;
}

auto parse_one_provided(const nlohmann::json& entry, std::size_t idx,
                        ErrorSink& sink, ProvidedCapability& out) -> bool {
  if (!entry.is_object()) {
    std::string rule =
        "capabilities.provides[" + std::to_string(idx) + "].not_object";
    sink.push(std::move(rule), "'provides' entry must be an object",
              "/provides/" + std::to_string(idx));
    return false;
  }
  bool ok = true;
  ok = parse_namespace(entry, idx, sink, out) && ok;
  ok = parse_version(entry, idx, sink, out) && ok;
  ok = parse_function(entry, idx, sink, out) && ok;
  ok = parse_params(entry, idx, sink, out) && ok;
  ok = parse_returns(entry, idx, sink, out) && ok;
  ok = parse_scope(entry, idx, sink, out) && ok;
  ok = parse_description(entry, idx, sink, out) && ok;
  ok = parse_rbac_rule(entry, idx, sink, out) && ok;
  return ok;
}

auto parse_requires(const nlohmann::json& j, ErrorSink& sink,
                    CapabilityManifest& m) -> void {
  if (!j.contains("requires")) {
    return;
  }
  const auto& r = j["requires"];
  if (!r.is_array()) {
    sink.push("capabilities.requires.not_array",
              "'requires' must be an array of signature strings", "/requires");
    return;
  }
  for (std::size_t i = 0; i < r.size(); ++i) {
    const auto& elem = r[i];
    if (!elem.is_string()) {
      sink.push("capabilities.requires[" + std::to_string(i) + "].not_string",
                "'requires' entries must be signature strings",
                "/requires/" + std::to_string(i));
      continue;
    }
    auto sig = elem.get<std::string>();
    auto parsed = plinth::capabilities::parse_signature(sig);
    if (std::holds_alternative<plinth::capabilities::CapabilityError>(parsed)) {
      sink.push(
          "capabilities.requires[" + std::to_string(i) + "].invalid_signature",
          "signature '" + sig + "' is not a valid capability identifier",
          "/requires/" + std::to_string(i),
          "format: <namespace>:<version>:<function> (e.g. notes:1:create)");
      continue;
    }
    m.requires_.push_back(std::move(sig));
  }
}

auto has_error(const std::vector<ManifestParseError>& msgs) -> bool {
  return std::ranges::any_of(
      msgs, [](const auto& e) { return e.severity == Severity::ERROR; });
}

} // namespace

auto CapabilityManifest::parse(std::string_view json_text,
                               std::string_view source_path)
    -> CapabilityManifestParseResult {
  CapabilityManifestParseResult result;
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
        .rule = "capabilities.json.parse_error",
        .message = e.what(),
        .remediation = "ensure the file is syntactically valid JSON",
        .severity = Severity::ERROR,
    });
    return result;
  }

  if (!j.is_object()) {
    sink.push("capabilities.root.not_object",
              "capabilities.json top-level value must be an object");
    return result;
  }

  CapabilityManifest m;

  if (j.contains("provides")) {
    const auto& p = j["provides"];
    if (!p.is_array()) {
      sink.push("capabilities.provides.not_array",
                "'provides' must be an array", "/provides");
    } else {
      for (std::size_t i = 0; i < p.size(); ++i) {
        ProvidedCapability pc;
        parse_one_provided(p[i], i, sink, pc);
        m.provides.push_back(std::move(pc));
      }
    }
  }

  parse_requires(j, sink, m);

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

auto CapabilityManifest::serialize() const -> std::string {
  auto out = nlohmann::json::object();
  for (auto it = unknown_fields.begin(); it != unknown_fields.end(); ++it) {
    out[it.key()] = it.value();
  }
  auto provides_arr = nlohmann::json::array();
  for (const auto& pc : provides) {
    auto entry = nlohmann::json::object();
    entry["namespace"] = pc.namespace_;
    entry["version"] = pc.version;
    entry["function"] = pc.function;
    auto params_arr = nlohmann::json::array();
    for (const auto& p : pc.params) {
      params_arr.push_back(nlohmann::json::object({
          {"name", p.name},
          {"type", p.type},
      }));
    }
    entry["params"] = params_arr;
    entry["returns"] = pc.returns;
    entry["scope"] = pc.scope;
    entry["description"] = pc.description;
    if (pc.rbac_rule) {
      entry["rbac_rule"] = *pc.rbac_rule;
    }
    provides_arr.push_back(std::move(entry));
  }
  out["provides"] = provides_arr;
  auto requires_arr = nlohmann::json::array();
  for (const auto& s : requires_) {
    requires_arr.push_back(s);
  }
  out["requires"] = requires_arr;
  return out.dump(2);
}

} // namespace plinth::packages
