// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/extension_http/caller_resolution.hpp"

#include <cstddef>
#include <expected>

struct pg_result;
using PGresult = pg_result;

namespace plinth::extension_http::detail {

inline constexpr std::size_t MAX_CALLER_RULES = 1024;
inline constexpr std::size_t MAX_CALLER_TEXT_BYTES = 256;

// Native PostgreSQL result boundary, returning DATA only. It cannot fabricate
// the sealed ResolvedHttpCaller. Named columns and exact text/null shapes are
// checked before copying any field. The SQL also bounds fields before libpq
// materializes them and returns at most MAX_CALLER_RULES + 1 rows.
[[nodiscard]] auto decode_http_caller(const PGresult& result,
                                      const Principal& principal)
    -> std::expected<capabilities::UserContext, CallerResolutionFailure>;

} // namespace plinth::extension_http::detail
