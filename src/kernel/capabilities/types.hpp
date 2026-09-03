#pragma once

// plinth::capabilities — core types shared by registration + bootstrap.
//
// See ICD-0.2.0-capability-registry.md for the authoritative schema and
// error catalogue. The 0.2.0 milestone covers storage + registration only;
// resolution, dispatch RBAC, LISTEN/NOTIFY listener, and batching are
// implemented in 0.2.2 – 0.2.5.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::capabilities {

// A registration request. All string fields are validated per
// ICD-0.2.0 §Validation Rules before the row is inserted.
struct CapabilityRegistration {
  // mandatory; `namespace` is a reserved keyword and the rest of this struct's
  // members are lower_case per project convention.
  std::string namespace_;
  int version = 0;
  std::string function;
  std::string provider_type; // "kernel" | "extension" | "sidecar"
  std::optional<std::string> extension_name; // required for extension/sidecar
  std::string scope;       // "instance" | "user" (user deferred)
  std::string description; // 0–256 chars
  std::string rbac_rule;   // must exist in plinth.rbac_rules
};

// Error codes returned by validation / registration. Stringified via
// error_code() into the snake_case form mandated by
// ICD-0.2.0 §Standardized Error Shape.
enum class CapabilityError : std::uint8_t {
  INVALID_NAMESPACE,
  INVALID_VERSION,
  INVALID_FUNCTION,
  INVALID_SCOPE,
  INVALID_PROVIDER_TYPE,
  INVALID_DESCRIPTION,
  INVALID_CAPABILITY, // signature malformed as a whole — see parser.hpp
  MISSING_EXTENSION_NAME,
  RESERVED_NAMESPACE, // only provider_type=kernel may use "kernel"
  NAMESPACE_MISMATCH, // rbac_rule must start with "<namespace>."
  CAPABILITY_EXISTS,
  CAPABILITY_NOT_FOUND,
  RBAC_RULE_NOT_FOUND,
  USER_SCOPE_NOT_SUPPORTED, // scope="user" deferred to 0.4.x per ICD
  DB_ERROR,
  // Resolver-only errors (ICD-0.2.2 §Error Codes). Added for 0.2.2;
  // register_capability never returns these.
  CAPABILITY_DISABLED,
  TIER3_NOT_AVAILABLE,
  CALL_DEPTH_EXCEEDED,
  // Added for 0.2.4 per ICD-0.2.4 §Standardized Error Shape.
  PERMISSION_DENIED,
  // Added for 0.5.0.4 per ICD-0.5.0.3 §Error taxonomy. The sync
  // `call_capability` path cannot dispatch extension capabilities —
  // callers must migrate to `call_capability_async`. Surfaces as
  // `cap.async_required`. Also used by `run_cap_call_outcome`-style
  // callers to detect that the extension arm requires async plumbing.
  ASYNC_REQUIRED,
  // Added for 0.5.0.4 per ICD-0.5.0.3 §Resolver integration. Transport
  // variant used when the RuntimeRegistry's extension dispatch returns
  // a `PromiseRejection`. The concrete `cap.*` rejection code and
  // capped message travel alongside the enum via the out-parameter
  // channel on `call_capability_async`, not as an enum-keyed constant.
  EXTENSION_DISPATCH_FAILED,
};

// Snake-case string code, suitable for API / audit payloads.
auto error_code(CapabilityError e) -> std::string_view;

// The registration API's result type (RegisterResult) lives in
// registration.hpp, not here. Parser / validation / fuzz-harness TUs
// only need CapabilityError and are kept independent of `<expected>`.

} // namespace plinth::capabilities
