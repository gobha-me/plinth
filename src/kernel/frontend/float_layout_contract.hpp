#pragma once

#include "kernel/packages/restore_codec.hpp"
#include "kernel/packages/restore_policy.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::frontend::layout_contract {

// INVALID/TOO_LARGE/STRUCTURAL_LIMIT are semantic input refusals. Operational
// allocation/length/hash failures retain LIMIT_EXCEEDED, never a dropped entry
// or a permanent invalid-input/byte-size conclusion.
enum class Error { INVALID, TOO_LARGE, STRUCTURAL_LIMIT, LIMIT_EXCEEDED };

namespace detail {
// Pure error classification, not a fault injector. Every canonical caller
// validates the small flat layout profile before encoding, so the shared
// encoder's own depth/node profile cannot be reached by accepted layout data.
[[nodiscard]] constexpr auto classify_canonical_error(
    packages::restore::CodecError error) noexcept -> Error {
  using packages::restore::CodecError;
  switch (error) {
    case CodecError::TOO_LARGE: return Error::TOO_LARGE;
    case CodecError::LIMIT_EXCEEDED:
    case CodecError::HASHING_FAILED: return Error::LIMIT_EXCEEDED;
    case CodecError::INVALID_JSON:
    case CodecError::DUPLICATE_KEY:
    case CodecError::INVALID_POLICY:
    case CodecError::CONFLICTING_BINDING:
    case CodecError::INVALID_INVENTORY:
    case CodecError::INVALID_SCHEMA: return Error::INVALID;
  }
  return Error::LIMIT_EXCEEDED;
}
} // namespace detail

struct FloatCommitBinding {
  std::size_t index = 0;
  std::string generation;
  std::string artifact_digest;
  std::string schema_digest;
  packages::restore::PolicyProjection projection_contract;
  std::string policy_revision;
};

// Pure normalized syntax, NEVER authorization/policy/receipt/admission or a
// write token. Revalidate every binding against fresh actual protected state.
struct ParsedFloatCommit {
  nlohmann::json layout;
  std::vector<FloatCommitBinding> bindings; // Complete, sorted by index.
};

// Raw <=64KiB; decoded duplicate/depth/node checks precede DOM. Layout
// version1,
// <=5 entries, <=32KiB snapshot/6KiB entry/4KiB context, exact closed shapes.
// Matches browser Number.isInteger/SafeInteger: integral finite numeric values
// are accepted without string/bool coercion, then normalized to native
// integers. Browser canonicalCapability similarly normalizes accepted version
// zeros.
// Raw depth/node excess is STRUCTURAL_LIMIT; operational failure is
// LIMIT_EXCEEDED and must remain unavailable rather than invalid/too-large.
[[nodiscard]] auto parse_float_commit(std::string_view raw)
    -> std::expected<ParsedFloatCommit, Error>;

// SAME savedEntry grammar as the browser, usable for original-index restore
// filtering. Does not consult schema/permissions or drop/repair stored entries.
// Boolean convenience only: use normalize_saved_float_entry where operational
// failure must be distinguished from semantic invalidity.
[[nodiscard]] auto validate_saved_float_entry(const nlohmann::json& value)
    -> bool;

// Canonical ephemeral view for restore proof only. Caller retains/returns the
// ORIGINAL raw stored layout unchanged; this never repairs or writes it.
[[nodiscard]] auto normalize_saved_float_entry(const nlohmann::json& value)
    -> std::expected<nlohmann::json, Error>;

// Exact browser identity JSON:
// [app,panel,canonical_cap,canonical_context_JSON]. Context keys sort by
// unsigned UTF8 bytes; values are flat bounded strings. This is an identity for
// deduplication, never a hash/proof or authority token.
[[nodiscard]] auto saved_float_identity(const nlohmann::json& value)
    -> std::expected<std::string, Error>;

} // namespace plinth::frontend::layout_contract
