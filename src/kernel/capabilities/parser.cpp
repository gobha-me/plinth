#include "kernel/capabilities/parser.hpp"

#include "kernel/capabilities/validation.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace plinth::capabilities {

namespace {

auto is_digit(char ch) -> bool {
  return ch >= '0' && ch <= '9';
}

} // namespace

auto parse_signature(std::string_view signature)
    -> std::variant<ParsedSignature, CapabilityError> {
  // Step 1: split on the two ':' separators. Exactly two colons are
  // required; a missing or extra colon means the signature is malformed
  // at the shape level, which the ICD names "invalid_capability".
  auto first = signature.find(':');
  if (first == std::string_view::npos) {
    return CapabilityError::INVALID_CAPABILITY;
  }
  auto second = signature.find(':', first + 1);
  if (second == std::string_view::npos) {
    return CapabilityError::INVALID_CAPABILITY;
  }
  // Anything beyond a third colon is extraneous; reject as malformed.
  if (signature.find(':', second + 1) != std::string_view::npos) {
    return CapabilityError::INVALID_CAPABILITY;
  }

  auto ns_view = signature.substr(0, first);
  auto ver_view = signature.substr(first + 1, second - first - 1);
  auto fn_view = signature.substr(second + 1);

  // Empty segments are a shape error, independent of per-field rules
  // (the validators also reject empty strings, but "too few colons" and
  // "empty segment" collapse to the same meaning for a caller).
  if (ns_view.empty() || ver_view.empty() || fn_view.empty()) {
    return CapabilityError::INVALID_CAPABILITY;
  }

  // Step 2: version must be a plain unsigned decimal integer. std::from_chars
  // refuses leading whitespace and non-digits in the parsed prefix, but it
  // *does* accept a leading '-' for signed targets — so guard the first byte
  // explicitly. A partial parse (not all bytes consumed) is also rejected —
  // this catches "1.0", "1abc", "1 ", etc.
  if (!is_digit(ver_view.front())) {
    return CapabilityError::INVALID_CAPABILITY;
  }
  int version = 0;
  const auto* ver_begin = ver_view.data();
  // std::from_chars is a pointer-range API; offsetting data() by size() is the
  // documented usage.
  const auto* ver_end = ver_view.data() + ver_view.size();
  auto conv = std::from_chars(ver_begin, ver_end, version);
  if (conv.ec != std::errc{} || conv.ptr != ver_end) {
    return CapabilityError::INVALID_CAPABILITY;
  }

  // Step 3: run per-field validators. These are the single source of
  // truth for the namespace / version / function rules (ICD-0.2.0
  // §Validation Rules), so reusing them keeps the parser aligned with
  // registration by construction.
  if (auto e = validate_namespace(ns_view)) {
    return *e;
  }
  if (auto e = validate_version(version)) {
    return *e;
  }
  if (auto e = validate_function(fn_view)) {
    return *e;
  }

  return ParsedSignature{
      .namespace_ = std::string{ns_view},
      .version = version,
      .function = std::string{fn_view},
  };
}

} // namespace plinth::capabilities
