#pragma once

#include <cstddef>

namespace plinth::packages::restore {

// Pure syntax/encoding errors only. Never carry raw policy data, reviewer
// identity, paths or parser exception messages across this boundary.
enum class CodecError {
  TOO_LARGE,
  INVALID_JSON,
  DUPLICATE_KEY,
  LIMIT_EXCEEDED,
  INVALID_POLICY,
  CONFLICTING_BINDING,
  INVALID_INVENTORY,
  INVALID_SCHEMA,
  HASHING_FAILED,
};

inline constexpr std::size_t POLICY_MAX_BYTES = std::size_t{1024} * 1024;
inline constexpr std::size_t POLICY_MAX_ENTRIES = 256;
inline constexpr std::size_t POLICY_ENTRY_MAX_BYTES = std::size_t{16} * 1024;
inline constexpr std::size_t JSON_MAX_DEPTH = 6;
inline constexpr std::size_t JSON_MAX_NODES = 16384;

// NEW codec eligibility profile, not installer admission limits. A future
// filesystem capture must independently establish completeness and ownership.
inline constexpr std::size_t INVENTORY_MAX_RECORDS = 4096;
inline constexpr std::size_t INVENTORY_MAX_COMPONENTS = 32;
inline constexpr std::size_t INVENTORY_PATH_MAX_BYTES = 1024;
inline constexpr std::size_t INVENTORY_METADATA_MAX_BYTES =
    std::size_t{1024} * 1024;
inline constexpr std::size_t INVENTORY_CONTENT_MAX_BYTES =
    std::size_t{50} * 1024 * 1024;

} // namespace plinth::packages::restore
