// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §Security Constraint 3 — Cross-extension batch
// prohibited. A `db.exec` / `db.query` inside a `db.batch` whose
// SQL references an `ext_<other>` schema (where `<other>` does not
// match `bc.extension_name`) rejects synchronously with
// `db.batch.cross_extension_not_allowed` and fires a rate-limited
// `db.batch.cross_extension_rejected` audit event.
//
// The per-op `SET LOCAL search_path` wrapper already isolates
// unqualified writes; this check makes the explicit-cross-schema
// attempt visible at classify time rather than letting it surface
// as a generic PG `permission_denied`.
//
// Trade-off: a string literal containing `ext_<word>` (e.g.
// `INSERT INTO log VALUES ('parsed by ext_foo')`) will trigger a
// false positive. The classifier is deliberately strict —
// extension authors who need to log `ext_*` substrings can use
// `db.exec` outside the batch or qualify with `plinth.*` table
// names. Same posture as the coalescer's regex-based table
// classifier in `src/kernel/realtime/sql_classifier.cpp`.

#pragma once

#include <string>
#include <string_view>

namespace plinth::js::db {

// Returns true when `sql` references at least one `ext_<X>` schema
// where `<X>` is non-empty and not equal to `expected_ext`. Returns
// false when `expected_ext` is empty (kernel-scope bcs may touch
// any schema by design) or when no `ext_<X>` reference is present.
[[nodiscard]] auto classify_cross_extension(std::string_view sql,
                                            std::string_view expected_ext)
    -> bool;

// Rate-limited audit event for SC3 rejections. 64-entry LRU keyed
// on `extension_name`, 60s window — same envelope as
// `db.search_path.set_failed`.
auto audit_batch_cross_extension_rejected(const std::string& extension_name,
                                          const std::string& sql) -> void;

// Test seam — drain the rate-limiter state.
auto reset_cross_extension_audit_for_test() -> void;

} // namespace plinth::js::db
