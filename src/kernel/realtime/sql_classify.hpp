// SPDX-License-Identifier: MIT
//
// ICD-0.5.1-pg-auto-event-coalescer §SQL Classification.
//
// Hand-rolled, narrow classifier for the single-statement write shapes
// the coalescer cares about: INSERT / UPDATE / DELETE against a single
// qualified or unqualified table. CTE writes, multi-table writes, DDL,
// SELECT, and anything else return std::nullopt (skip) with a debug
// log at the call site. See ICD §Supported shapes + §Skip cases.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::realtime {

enum class OpKind : std::uint8_t { INSERT, UPDATE, DELETE };

struct SqlClass {
  std::string schema;
  std::string table;
  OpKind op_kind{OpKind::INSERT};
};

// Classify `sql` into one of the three supported shapes. `ext_name` is
// the `AsyncOp::bc_extension_name` snapshot and drives the unqualified
// fallback (`INSERT INTO notes ...` resolves to
// `(ext_<ext_name>, notes, INSERT)` when ext_name is non-empty; kernel-
// scope callers with unqualified writes get std::nullopt). A mismatch
// between the extracted schema and `ext_` + ext_name is the §Defense-
// in-depth signal: the classifier emits a warn log and still returns
// the extracted tuple — the security boundary is ICD-0.4.3's
// search_path + role + capability gate, not this check.
auto classify_sql(std::string_view sql, std::string_view ext_name)
    -> std::optional<SqlClass>;

} // namespace plinth::realtime
