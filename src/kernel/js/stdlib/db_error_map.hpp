// SPDX-License-Identifier: MIT
//
// db_error_map — SQLSTATE → ICD-0.3.3 db.* error code mapping.
// See ICD-0.3.3-async-bridge.md §Promise Rejection Shape (db.*).

#pragma once

#include <string_view>

namespace plinth::js::db_error_map {

// Returns the canonical db.* `code` string for the given PG SQLSTATE.
// Always returns a non-empty value — unmapped SQLSTATEs surface as
// "db.internal" so the JS caller still gets a structured rejection.
[[nodiscard]] auto map_sqlstate(std::string_view sqlstate) noexcept
    -> std::string_view;

} // namespace plinth::js::db_error_map
