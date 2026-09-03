#pragma once

// plinth::capabilities — pure validation helpers.
//
// All functions in this header are pure (no PG, no Drogon, no globals)
// so they are exercised entirely by [unit] tests without a database.
// Rules mirror ICD-0.2.0 §Validation Rules exactly.

#include "kernel/capabilities/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace plinth::capabilities {

// Namespace: [a-z][a-z0-9_]{0,63}
auto validate_namespace(std::string_view ns) -> std::optional<CapabilityError>;

// Version: positive integer (>= 1)
auto validate_version(int version) -> std::optional<CapabilityError>;

// Function: [a-z][a-z0-9_.]{0,127}, no leading / trailing / consecutive dots.
auto validate_function(std::string_view fn) -> std::optional<CapabilityError>;

// Scope: "instance" accepted; "user" returns user_scope_not_supported
// in 0.2.x per ICD §Data Model "User-scope deferral"; anything else
// returns invalid_scope.
auto validate_scope(std::string_view scope) -> std::optional<CapabilityError>;

// Provider type: "kernel" | "extension" | "sidecar"
auto validate_provider_type(std::string_view provider_type)
    -> std::optional<CapabilityError>;

// Description: 0–256 chars (ICD §Data Model).
auto validate_description(std::string_view description)
    -> std::optional<CapabilityError>;

// Compose all per-field checks plus cross-field rules:
//   - extension/sidecar requires non-empty extension_name
//   - only provider_type=kernel may use namespace "kernel"
//   - rbac_rule must start with "<namespace>." (namespace alignment per
//     DESIGN-rbac-philosophy / ICD-0.2.4)
auto validate_registration(const CapabilityRegistration& reg)
    -> std::optional<CapabilityError>;

// Canonical identifier: "namespace:version:function".
auto make_signature(const CapabilityRegistration& reg) -> std::string;

} // namespace plinth::capabilities
