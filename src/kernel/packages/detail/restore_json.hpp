#pragma once

#include "kernel/packages/restore_codec.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <expected>
#include <string>

namespace plinth::packages::restore::detail {

// Exact codec-only JSON encoding: unsigned-byte-sorted object keys; minimal
// integer literals; short JSON control escapes, other C0 as lowercase u00xx;
// quote/backslash escaped, other valid UTF-8 unchanged. No floating values.
// Enforces the policy value-node/depth limits and output cap while encoding.
[[nodiscard]] auto canonical_json(const nlohmann::json& value,
                                  std::size_t byte_cap)
    -> std::expected<std::string, CodecError>;

} // namespace plinth::packages::restore::detail
