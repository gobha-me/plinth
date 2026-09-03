#pragma once

// plinth::js::runtime_config — build-time JS runtime limits exposed to
// non-JS callers.
//
// Extracted in 0.4.2 so the package validator (CFW4 memory-over-max)
// can read the QuickJS per-runtime memory cap without pulling in
// <quickjs.h>. ICD-0.3.1 §Memory Limit + ICD-0.4.2 §Library Surface
// (PackageManifest::runtime.memory_limit_mb → warn if larger).

#include <cstddef>

namespace plinth::js::runtime_config {

// 16 MiB — the hard-coded per-runtime memory ceiling passed to
// `JS_SetMemoryLimit`. Matches eval.cpp and runtime_pool.cpp.
inline constexpr std::size_t MAX_MEMORY_BYTES = 16UL * 1024UL * 1024UL;

[[nodiscard]] constexpr auto get_max_memory_mb() noexcept -> std::size_t {
  return MAX_MEMORY_BYTES / (1024UL * 1024UL);
}

} // namespace plinth::js::runtime_config
