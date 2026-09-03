#include "kernel/capabilities/validation.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace plinth::capabilities {

namespace {

constexpr std::size_t NAMESPACE_MAX_LEN = 64;
constexpr std::size_t FUNCTION_MAX_LEN = 128;
constexpr std::size_t DESCRIPTION_MAX_LEN = 256;

auto is_lower(char ch) -> bool {
  return ch >= 'a' && ch <= 'z';
}
auto is_digit(char ch) -> bool {
  return ch >= '0' && ch <= '9';
}
auto is_namespace_tail(char ch) -> bool {
  return is_lower(ch) || is_digit(ch) || ch == '_';
}
auto is_function_tail(char ch) -> bool {
  return is_lower(ch) || is_digit(ch) || ch == '_' || ch == '.';
}

auto error_string(CapabilityError e) -> std::string_view {
  switch (e) {
    case CapabilityError::INVALID_NAMESPACE: return "invalid_namespace";
    case CapabilityError::INVALID_VERSION: return "invalid_version";
    case CapabilityError::INVALID_FUNCTION: return "invalid_function";
    case CapabilityError::INVALID_SCOPE: return "invalid_scope";
    case CapabilityError::INVALID_PROVIDER_TYPE: return "invalid_provider_type";
    case CapabilityError::INVALID_DESCRIPTION: return "invalid_description";
    case CapabilityError::INVALID_CAPABILITY: return "invalid_capability";
    case CapabilityError::MISSING_EXTENSION_NAME:
      return "missing_extension_name";
    case CapabilityError::RESERVED_NAMESPACE: return "reserved_namespace";
    case CapabilityError::NAMESPACE_MISMATCH: return "namespace_mismatch";
    case CapabilityError::CAPABILITY_EXISTS: return "capability_exists";
    case CapabilityError::CAPABILITY_NOT_FOUND: return "capability_not_found";
    case CapabilityError::RBAC_RULE_NOT_FOUND: return "rbac_rule_not_found";
    case CapabilityError::USER_SCOPE_NOT_SUPPORTED:
      return "user_scope_not_supported";
    case CapabilityError::DB_ERROR: return "db_error";
    case CapabilityError::CAPABILITY_DISABLED: return "capability_disabled";
    case CapabilityError::TIER3_NOT_AVAILABLE: return "tier3_not_available";
    case CapabilityError::CALL_DEPTH_EXCEEDED: return "call_depth_exceeded";
    case CapabilityError::PERMISSION_DENIED: return "permission_denied";
    case CapabilityError::ASYNC_REQUIRED: return "async_required";
    case CapabilityError::EXTENSION_DISPATCH_FAILED:
      return "extension_dispatch_failed";
  }
  return "unknown_error";
}

} // namespace

auto error_code(CapabilityError e) -> std::string_view {
  return error_string(e);
}

auto validate_namespace(std::string_view ns) -> std::optional<CapabilityError> {
  if (ns.empty() || ns.size() > NAMESPACE_MAX_LEN) {
    return CapabilityError::INVALID_NAMESPACE;
  }
  if (!is_lower(ns.front())) {
    return CapabilityError::INVALID_NAMESPACE;
  }
  if (!std::all_of(ns.begin() + 1, ns.end(), is_namespace_tail)) {
    return CapabilityError::INVALID_NAMESPACE;
  }
  return std::nullopt;
}

auto validate_version(int version) -> std::optional<CapabilityError> {
  if (version < 1) {
    return CapabilityError::INVALID_VERSION;
  }
  return std::nullopt;
}

auto validate_function(std::string_view fn) -> std::optional<CapabilityError> {
  if (fn.empty() || fn.size() > FUNCTION_MAX_LEN) {
    return CapabilityError::INVALID_FUNCTION;
  }
  if (!is_lower(fn.front())) {
    return CapabilityError::INVALID_FUNCTION;
  }
  if (fn.back() == '.') {
    return CapabilityError::INVALID_FUNCTION;
  }
  if (!std::all_of(fn.begin() + 1, fn.end(), is_function_tail)) {
    return CapabilityError::INVALID_FUNCTION;
  }
  // No consecutive dots.
  for (std::size_t i = 1; i < fn.size(); ++i) {
    if (fn[i] == '.' && fn[i - 1] == '.') {
      return CapabilityError::INVALID_FUNCTION;
    }
  }
  return std::nullopt;
}

auto validate_scope(std::string_view scope) -> std::optional<CapabilityError> {
  if (scope == "instance") {
    return std::nullopt;
  }
  if (scope == "user") {
    return CapabilityError::USER_SCOPE_NOT_SUPPORTED;
  }
  return CapabilityError::INVALID_SCOPE;
}

auto validate_provider_type(std::string_view provider_type)
    -> std::optional<CapabilityError> {
  if (provider_type == "kernel" || provider_type == "extension" ||
      provider_type == "sidecar") {
    return std::nullopt;
  }
  return CapabilityError::INVALID_PROVIDER_TYPE;
}

auto validate_description(std::string_view description)
    -> std::optional<CapabilityError> {
  if (description.size() > DESCRIPTION_MAX_LEN) {
    return CapabilityError::INVALID_DESCRIPTION;
  }
  return std::nullopt;
}

auto validate_registration(const CapabilityRegistration& reg)
    -> std::optional<CapabilityError> {
  if (auto e = validate_namespace(reg.namespace_)) {
    return e;
  }
  if (auto e = validate_version(reg.version)) {
    return e;
  }
  if (auto e = validate_function(reg.function)) {
    return e;
  }
  if (auto e = validate_provider_type(reg.provider_type)) {
    return e;
  }
  if (auto e = validate_scope(reg.scope)) {
    return e;
  }
  if (auto e = validate_description(reg.description)) {
    return e;
  }

  // extension_name required for extension + sidecar providers.
  auto needs_extension_name =
      (reg.provider_type == "extension" || reg.provider_type == "sidecar");
  if (needs_extension_name) {
    if (!reg.extension_name.has_value() || reg.extension_name->empty()) {
      return CapabilityError::MISSING_EXTENSION_NAME;
    }
  }

  // The kernel namespace is reserved for provider_type=kernel.
  if (reg.namespace_ == "kernel" && reg.provider_type != "kernel") {
    return CapabilityError::RESERVED_NAMESPACE;
  }

  // RBAC rule namespace must match the capability namespace.
  // Rules follow "<namespace>.<action>" or "<namespace>.<resource>.<action>"
  // per DESIGN-rbac-philosophy.md and ICD-0.2.4 §Rule Naming Convention.
  auto prefix = reg.namespace_ + ".";
  if (!reg.rbac_rule.starts_with(prefix) ||
      reg.rbac_rule.size() == prefix.size()) {
    return CapabilityError::NAMESPACE_MISMATCH;
  }

  return std::nullopt;
}

auto make_signature(const CapabilityRegistration& reg) -> std::string {
  std::ostringstream ss;
  ss << reg.namespace_ << ':' << reg.version << ':' << reg.function;
  return ss.str();
}

} // namespace plinth::capabilities
