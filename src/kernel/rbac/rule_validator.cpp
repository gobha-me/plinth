#include "kernel/rbac/rule_validator.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"
#include "kernel/packages/reserved_names.hpp"
#include "kernel/rbac/rbac_manifest.hpp"

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::rbac {

using plinth::packages::is_reserved_kernel_namespace;
using plinth::packages::ManifestParseError;
using plinth::packages::Severity;

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto is_valid_rule_segment_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Rule A.1 regex: ^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$
auto is_valid_rule_name(std::string_view s) -> bool {
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
      if (!is_valid_rule_segment_char(s[i])) {
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

auto make_path(std::size_t idx, std::string_view suffix) -> std::string {
  std::string p = "/rules/" + std::to_string(idx);
  if (!suffix.empty()) {
    p.append("/");
    p.append(suffix);
  }
  return p;
}

auto push_error(std::vector<ManifestParseError>& out, std::string rule,
                std::string message, std::string field_path,
                std::optional<std::string> remediation = std::nullopt) -> void {
  out.push_back(ManifestParseError{
      .file = "rbac.json",
      .line = std::nullopt,
      .column = std::nullopt,
      .field_path = std::move(field_path),
      .rule = std::move(rule),
      .message = std::move(message),
      .remediation = std::move(remediation),
      .severity = Severity::ERROR,
  });
}

auto check_rule_a1(const RbacRule& r, std::size_t idx,
                   std::vector<ManifestParseError>& out) -> void {
  if (!is_valid_rule_name(r.rule)) {
    push_error(out, "rbac.rule.invalid_name",
               "rbac.json rules[" + std::to_string(idx) + "].rule '" + r.rule +
                   "' does not match the required pattern",
               make_path(idx, "rule"),
               "use lowercase letters, digits, and dots; e.g. 'notes.edit' or "
               "'terminal.shell.execute'");
  }
}

auto check_rule_a2(const RbacRule& r, std::size_t idx,
                   std::string_view package_name,
                   std::vector<ManifestParseError>& out) -> void {
  if (r.namespace_ == package_name) {
    return;
  }
  if (is_reserved_kernel_namespace(r.namespace_)) {
    return;
  }
  push_error(out, "rbac.rule.namespace_mismatch",
             "rbac.json rules[" + std::to_string(idx) + "].namespace '" +
                 r.namespace_ + "' does not match package name '" +
                 std::string{package_name} + "' or any reserved namespace",
             make_path(idx, "namespace"),
             "rename the rule's namespace to '" + std::string{package_name} +
                 "' or one of: kernel, plinth, system");
}

// Helper for A.3 + A.4: for a present `{call, expect}` object, parse
// the call string and emit A.3 / A.4 errors as appropriate. `which` is
// "assert_deny" or "assert_allow".
auto check_assert_call(const nlohmann::json& assert_node, std::size_t idx,
                       std::string_view which,
                       const plinth::packages::CapabilityManifest& caps,
                       std::vector<ManifestParseError>& out) -> void {
  if (!assert_node.is_object() || !assert_node.contains("call") ||
      !assert_node["call"].is_string()) {
    // Shape errors already reported by parse_rbac_manifest; a rule
    // with malformed assert shape was dropped from value->rules.
    return;
  }
  auto call = assert_node["call"].get<std::string>();
  auto parsed = plinth::capabilities::parse_signature(call);
  if (std::holds_alternative<plinth::capabilities::CapabilityError>(parsed)) {
    auto err_code = std::get<plinth::capabilities::CapabilityError>(parsed);
    push_error(out, "rbac.test.call.parse_error",
               "rbac.json rules[" + std::to_string(idx) + "].test." +
                   std::string{which} + ".call '" + call +
                   "' is not a valid capability signature (parser error: " +
                   std::to_string(static_cast<int>(err_code)) + ")",
               make_path(idx, "test/") + std::string{which} + "/call",
               "format: <namespace>:<version>:<function> (e.g. notes:1:create "
               "or notes:1:edit('foo'))");
    return;
  }
  const auto& sig = std::get<plinth::capabilities::ParsedSignature>(parsed);
  bool hit = std::ranges::any_of(caps.provides, [&](const auto& pc) {
    return pc.namespace_ == sig.namespace_ && pc.version == sig.version &&
           pc.function == sig.function;
  });
  if (!hit) {
    push_error(out, "rbac.test.call.unresolved",
               "rbac.json rules[" + std::to_string(idx) + "].test." +
                   std::string{which} + ".call '" + call +
                   "' does not reference any provided capability",
               make_path(idx, "test/") + std::string{which} + "/call",
               "add the capability to capabilities.json or fix the signature "
               "to match an existing provides[] entry");
  }
}

auto check_rules_a3_a4(const RbacRule& r, std::size_t idx,
                       const plinth::packages::CapabilityManifest& caps,
                       std::vector<ManifestParseError>& out) -> void {
  if (!r.test.has_value()) {
    return;
  }
  const auto& test = *r.test;
  if (test.contains("assert_deny")) {
    check_assert_call(test["assert_deny"], idx, "assert_deny", caps, out);
  }
  if (test.contains("assert_allow")) {
    check_assert_call(test["assert_allow"], idx, "assert_allow", caps, out);
  }
}

auto check_rule_a5(const RbacRule& r, std::size_t idx,
                   std::string_view package_name, PGconn& conn,
                   std::vector<ManifestParseError>& out) -> void {
  std::string rule_s{r.rule};
  std::array<const char*, 1> values = {rule_s.c_str()};
  PgResultPtr res(
      PQexecParams(
          &conn, "SELECT extension_name FROM plinth.rbac_rules WHERE rule = $1",
          1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    push_error(out, "rbac.rule.name_collision",
               "rbac.json rules[" + std::to_string(idx) +
                   "] collision check failed: " +
                   std::string{PQresultErrorMessage(res.get())},
               make_path(idx, "rule"));
    return;
  }
  if (PQntuples(res.get()) == 0) {
    return; // rule not registered yet — no collision
  }
  std::string existing{PQgetvalue(res.get(), 0, 0)};
  if (existing == package_name) {
    return; // same-extension re-register is silent (upgrade case)
  }
  push_error(out, "rbac.rule.name_collision",
             "rbac.json rules[" + std::to_string(idx) + "].rule '" + r.rule +
                 "' is already registered by extension '" + existing + "'",
             make_path(idx, "rule"),
             "rename this rule; rule names are a global identity space across "
             "extensions (ICD-0.1.4 §Rule Registration)");
}

} // namespace

auto validate_rules(const RbacManifest& rbac,
                    const plinth::packages::CapabilityManifest& caps,
                    std::string_view package_name, PGconn& conn)
    -> std::vector<ManifestParseError> {
  std::vector<ManifestParseError> out;
  for (std::size_t i = 0; i < rbac.rules.size(); ++i) {
    const auto& r = rbac.rules[i];
    check_rule_a1(r, i, out);
    check_rule_a2(r, i, package_name, out);
    check_rules_a3_a4(r, i, caps, out);
    check_rule_a5(r, i, package_name, conn, out);
  }
  return out;
}

} // namespace plinth::rbac
