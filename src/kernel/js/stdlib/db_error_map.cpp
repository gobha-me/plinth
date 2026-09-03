// SPDX-License-Identifier: MIT
//
// See db_error_map.hpp. Mapping is per ICD-0.3.3 §Promise Rejection
// Shape (db.*). Adding entries is a per-PR source change — no ICD
// patch required.

#include "kernel/js/stdlib/db_error_map.hpp"

#include <string_view>

namespace plinth::js::db_error_map {

auto map_sqlstate(std::string_view sqlstate) noexcept -> std::string_view {
  // Per-row mappings first; class-prefix fallback for the integrity-
  // constraint family second; default last.
  if (sqlstate == "42601") {
    return "db.syntax_error";
  }
  if (sqlstate == "42P01") {
    return "db.undefined_table";
  }
  if (sqlstate == "42703") {
    return "db.undefined_column";
  }
  if (sqlstate == "42501") {
    return "db.permission_denied";
  }
  if (sqlstate == "40001") {
    return "db.serialization_failure";
  }
  if (sqlstate == "40P01") {
    return "db.deadlock_detected";
  }
  // Class 23 — Integrity constraint violation. Per §Promise Rejection
  // Shape: every 23xx SQLSTATE collapses to db.constraint_violation.
  if (sqlstate.starts_with("23")) {
    return "db.constraint_violation";
  }
  // Class 08 — Connection exception. The driver typically surfaces
  // these as a separate exception type; mapping is here for cases
  // where SQLSTATE alone reaches us.
  if (sqlstate.starts_with("08")) {
    return "db.connection_error";
  }
  return "db.internal";
}

} // namespace plinth::js::db_error_map
