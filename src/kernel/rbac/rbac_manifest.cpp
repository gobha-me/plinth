#include "kernel/rbac/rbac_manifest.hpp"

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::rbac {

using plinth::packages::ManifestParseError;
using plinth::packages::Severity;

namespace {

constexpr std::size_t DESCRIPTION_MAX_LEN = 1024;

constexpr std::array<std::string_view, 2> KNOWN_TOP_LEVEL{
    "rules",
    "default_grants",
};

auto is_known_top_level(std::string_view key) -> bool {
  return std::ranges::any_of(KNOWN_TOP_LEVEL,
                             [&](std::string_view k) { return k == key; });
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
  std::string p = "/rules/" + std::to_string(idx);
  if (!field.empty()) {
    p.append("/");
    p.append(field);
  }
  return p;
}

auto parse_rule_name(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, RbacRule& out) -> bool {
  if (!entry.contains("rule")) {
    sink.push("rbac.rule.missing",
              "rbac.json rules[" + std::to_string(idx) + "] is missing 'rule'",
              make_path(idx, "rule"));
    return false;
  }
  if (!entry["rule"].is_string()) {
    sink.push("rbac.rule.not_string",
              "rbac.json rules[" + std::to_string(idx) +
                  "].rule must be a string",
              make_path(idx, "rule"));
    return false;
  }
  out.rule = entry["rule"].get<std::string>();
  if (out.rule.empty()) {
    sink.push("rbac.rule.missing",
              "rbac.json rules[" + std::to_string(idx) +
                  "].rule must be non-empty",
              make_path(idx, "rule"));
    return false;
  }
  return true;
}

auto parse_rule_namespace(const nlohmann::json& entry, std::size_t idx,
                          ErrorSink& sink, RbacRule& out) -> bool {
  if (!entry.contains("namespace") || !entry["namespace"].is_string()) {
    sink.push("rbac.namespace.missing",
              "rbac.json rules[" + std::to_string(idx) +
                  "].namespace is required and must be a string",
              make_path(idx, "namespace"));
    return false;
  }
  out.namespace_ = entry["namespace"].get<std::string>();
  if (out.namespace_.empty()) {
    sink.push("rbac.namespace.missing",
              "rbac.json rules[" + std::to_string(idx) +
                  "].namespace must be non-empty",
              make_path(idx, "namespace"));
    return false;
  }
  return true;
}

auto parse_rule_description(const nlohmann::json& entry, std::size_t idx,
                            ErrorSink& sink, RbacRule& out) -> bool {
  if (!entry.contains("description") || !entry["description"].is_string()) {
    sink.push("rbac.description.missing",
              "rbac.json rules[" + std::to_string(idx) +
                  "].description is required and must be a string",
              make_path(idx, "description"));
    return false;
  }
  out.description = entry["description"].get<std::string>();
  if (out.description.empty()) {
    sink.push("rbac.description.missing",
              "rbac.json rules[" + std::to_string(idx) +
                  "].description must be non-empty",
              make_path(idx, "description"));
    return false;
  }
  if (out.description.size() > DESCRIPTION_MAX_LEN) {
    sink.push("rbac.description.too_long",
              "rbac.json rules[" + std::to_string(idx) +
                  "].description exceeds 1024 characters",
              make_path(idx, "description"));
    return false;
  }
  return true;
}

// Validate a single `{call, expect}` assertion. `which` is the key
// name ("assert_deny" or "assert_allow"); `expected_literal` is the
// string the caller requires `.expect` to equal exactly.
auto parse_assert_entry(const nlohmann::json& assert_node, std::size_t idx,
                        std::string_view which,
                        std::string_view expected_literal, ErrorSink& sink)
    -> bool {
  auto path_root = make_path(idx, "test") + "/" + std::string(which);
  if (!assert_node.is_object()) {
    sink.push("rbac.test." + std::string(which) + ".shape",
              "rbac.json rules[" + std::to_string(idx) + "].test." +
                  std::string(which) + " must be an object",
              path_root);
    return false;
  }
  bool ok = true;
  if (!assert_node.contains("call") || !assert_node["call"].is_string() ||
      assert_node["call"].get<std::string>().empty()) {
    sink.push("rbac.test.call.missing",
              "rbac.json rules[" + std::to_string(idx) + "].test." +
                  std::string(which) +
                  ".call is required and must be a non-empty string",
              path_root + "/call");
    ok = false;
  }
  bool expect_ok = assert_node.contains("expect") &&
                   assert_node["expect"].is_string() &&
                   assert_node["expect"].get<std::string>() == expected_literal;
  if (!expect_ok) {
    sink.push("rbac.test.expect.invalid",
              "rbac.json rules[" + std::to_string(idx) + "].test." +
                  std::string(which) + ".expect must be the literal '" +
                  std::string(expected_literal) + "'",
              path_root + "/expect");
    ok = false;
  }
  return ok;
}

auto parse_rule_test(const nlohmann::json& entry, std::size_t idx,
                     ErrorSink& sink, RbacRule& out) -> bool {
  if (!entry.contains("test")) {
    return true;
  }
  const auto& test = entry["test"];
  if (!test.is_object()) {
    sink.push("rbac.test.not_object",
              "rbac.json rules[" + std::to_string(idx) +
                  "].test must be an object",
              make_path(idx, "test"));
    return false;
  }
  bool ok = true;
  if (test.contains("assert_deny")) {
    ok = parse_assert_entry(test["assert_deny"], idx, "assert_deny",
                            "permission_denied", sink) &&
         ok;
  }
  if (test.contains("assert_allow")) {
    ok = parse_assert_entry(test["assert_allow"], idx, "assert_allow",
                            "success", sink) &&
         ok;
  }
  if (ok) {
    out.test = test;
  }
  return ok;
}

auto parse_one_default_grant(const nlohmann::json& entry, std::size_t idx,
                             ErrorSink& sink, DefaultGrant& out) -> bool {
  std::string p = "/default_grants/" + std::to_string(idx);
  if (!entry.is_object()) {
    sink.push("rbac.default_grants[" + std::to_string(idx) + "].not_object",
              "default_grants entry must be an object", p);
    return false;
  }
  if (!entry.contains("group") || !entry["group"].is_string() ||
      entry["group"].get<std::string>().empty()) {
    sink.push("rbac.default_grants[" + std::to_string(idx) + "].group.invalid",
              "default_grants[].group must be a non-empty string",
              p + "/group");
    return false;
  }
  if (!entry.contains("rule") || !entry["rule"].is_string() ||
      entry["rule"].get<std::string>().empty()) {
    sink.push("rbac.default_grants[" + std::to_string(idx) + "].rule.invalid",
              "default_grants[].rule must be a non-empty string", p + "/rule");
    return false;
  }
  out.group = entry["group"].get<std::string>();
  out.rule = entry["rule"].get<std::string>();
  return true;
}

auto parse_one_rule(const nlohmann::json& entry, std::size_t idx,
                    ErrorSink& sink, RbacRule& out) -> bool {
  if (!entry.is_object()) {
    sink.push("rbac.rules[" + std::to_string(idx) + "].not_object",
              "rbac.json rules[" + std::to_string(idx) + "] must be an object",
              make_path(idx, ""));
    return false;
  }
  bool ok = true;
  ok = parse_rule_name(entry, idx, sink, out) && ok;
  ok = parse_rule_namespace(entry, idx, sink, out) && ok;
  ok = parse_rule_description(entry, idx, sink, out) && ok;
  ok = parse_rule_test(entry, idx, sink, out) && ok;
  return ok;
}

} // namespace

auto parse_rbac_manifest(std::string_view json_text,
                         std::string_view source_path)
    -> RbacManifestParseResult {
  RbacManifestParseResult result;
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
        .rule = "rbac.json.parse_error",
        .message = e.what(),
        .remediation = "ensure the file is syntactically valid JSON",
        .severity = Severity::ERROR,
    });
    return result;
  }

  if (!j.is_object()) {
    sink.push("rbac.root.not_object",
              "rbac.json top-level value must be an object");
    return result;
  }

  RbacManifest m;

  if (j.contains("rules")) {
    const auto& r = j["rules"];
    if (!r.is_array()) {
      sink.push("rbac.rules.not_array", "'rules' must be an array", "/rules");
      return result;
    }
    for (std::size_t i = 0; i < r.size(); ++i) {
      RbacRule rr;
      if (parse_one_rule(r[i], i, sink, rr)) {
        m.rules.push_back(std::move(rr));
      }
    }
  }

  // ICD-0.6.1 §7.2 — `default_grants[]` (optional). Each entry must
  // be `{ "group": "<group_name>", "rule": "<rule_name>" }`. The
  // rule must appear in the `rules[]` array of the same manifest;
  // cross-rule references are validated at install_lifecycle's
  // REGISTERING stage. Missing field is fine — extension declares
  // no default grants.
  if (j.contains("default_grants")) {
    const auto& dg = j["default_grants"];
    if (!dg.is_array()) {
      sink.push("rbac.default_grants.not_array",
                "'default_grants' must be an array", "/default_grants");
    } else {
      for (std::size_t i = 0; i < dg.size(); ++i) {
        DefaultGrant g;
        if (parse_one_default_grant(dg[i], i, sink, g)) {
          m.default_grants.push_back(std::move(g));
        }
      }
    }
  }

  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!is_known_top_level(it.key())) {
      m.unknown_fields[it.key()] = it.value();
    }
  }

  result.value = std::move(m);
  return result;
}

auto RbacManifest::serialize() const -> std::string {
  auto out = nlohmann::json::object();
  for (auto it = unknown_fields.begin(); it != unknown_fields.end(); ++it) {
    out[it.key()] = it.value();
  }
  auto rules_arr = nlohmann::json::array();
  for (const auto& r : rules) {
    auto entry = nlohmann::json::object();
    entry["rule"] = r.rule;
    entry["namespace"] = r.namespace_;
    entry["description"] = r.description;
    if (r.test.has_value()) {
      entry["test"] = *r.test;
    }
    rules_arr.push_back(std::move(entry));
  }
  out["rules"] = rules_arr;
  if (!default_grants.empty()) {
    auto grants_arr = nlohmann::json::array();
    for (const auto& g : default_grants) {
      auto e = nlohmann::json::object();
      e["group"] = g.group;
      e["rule"] = g.rule;
      grants_arr.push_back(std::move(e));
    }
    out["default_grants"] = grants_arr;
  }
  return out.dump(2);
}

} // namespace plinth::rbac
