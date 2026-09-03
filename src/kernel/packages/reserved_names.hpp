#pragma once

// Kernel-reserved identifier set — shared between:
//   - ICD-0.4.0 CF1 / CF6 (cross-file validator): rejects package names
//     in this set (CF6) and accepts rule namespaces in this set (CF1).
//   - ICD-0.4.6 rule validator Rule A.2: the set of namespaces a
//     package's rbac.json rule is permitted to declare *other* than
//     the package name itself.
//
// Extending this list requires a kernel-bootstrap change plus an
// architecture-doc amendment; keep deliberately short.

#include <algorithm>
#include <array>
#include <string_view>

namespace plinth::packages {

constexpr std::array<std::string_view, 3> RESERVED_NAMES{
    "kernel",
    "plinth",
    "system",
};

[[nodiscard]] constexpr auto is_reserved_kernel_namespace(std::string_view s)
    -> bool {
  return std::ranges::any_of(RESERVED_NAMES,
                             [&](std::string_view r) { return r == s; });
}

} // namespace plinth::packages
