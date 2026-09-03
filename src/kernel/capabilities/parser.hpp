#pragma once

// plinth::capabilities — canonical-signature parser.
//
// The inverse of `make_signature()` in validation.hpp. Parses strings of the
// form "namespace:version:function" into components and validates each field
// with the existing validators. Pure (no PG, no Drogon, no globals) so it
// is exercised entirely by [unit] tests.
//
// Contract is defined by:
//   - ICD-0.2.0 §Capability Identifier Format + §Validation Rules
//   - ICD-0.2.2 §Resolution Algorithm step 1 ("invalid_capability")
//
// Error semantics:
//   - Malformed overall shape (wrong colon count, empty segments, non-numeric
//     or overflowing version) → CapabilityError::INVALID_CAPABILITY.
//   - Components that parse as segments but fail field validation →
//     CapabilityError::INVALID_NAMESPACE / INVALID_VERSION / INVALID_FUNCTION.
//
// Callers at the 0.2.2 resolver boundary are expected to map any of the above
// to the user-visible "invalid_capability" error per ICD-0.2.2 §Error Codes.

#include "kernel/capabilities/types.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace plinth::capabilities {

// Parsed components of a canonical "namespace:version:function" signature.
struct ParsedSignature {
  // mandatory; `namespace` is a reserved keyword and the rest of this struct's
  // members are lower_case per project convention.
  std::string namespace_;
  int version = 0;
  std::string function;
};

// Parse a canonical capability signature. Returns the parsed components on
// success, or a CapabilityError on failure. See header comment for error
// semantics.
auto parse_signature(std::string_view signature)
    -> std::variant<ParsedSignature, CapabilityError>;

} // namespace plinth::capabilities
