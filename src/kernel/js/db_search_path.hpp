// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §Per-Op `SET search_path` Isolation.
//
// Every `db.exec` / `db.query` from an extension-scope bc runs
// inside a short transaction with
// `SET LOCAL search_path TO ext_<extension_name>, plinth;` pinned
// to the transaction. Kernel-scope bcs (empty `extension_name`) run
// raw. The feature flag `db.search_path.enforce` disables the
// wrapper process-wide as a deployment-ramp escape hatch.
//
// This module owns the feature flag + identity-regex check + the
// `db.search_path.set_failed` rate-limited audit event. The wrapper
// itself lives in `run_on_context.cpp` alongside the existing db
// dispatch arms.

#pragma once

#include <string>
#include <string_view>

namespace plinth::js::db {

// ICD §Config override. Default true; set false via config to disable
// the wrapper globally. Warn-logged at load time when false.
auto set_search_path_enforce(bool enforce) -> void;
[[nodiscard]] auto search_path_enforced() -> bool;

// ICD §Identity source. `extension_name` must match `[a-z][a-z0-9_]*`
// per ICD-0.4.1 install-time validation. This function defensively
// re-validates at dispatch time before substituting the identity into
// a SET LOCAL statement. Returns false for any non-matching input
// including empty strings.
[[nodiscard]] auto is_valid_extension_name(std::string_view name) -> bool;

// ICD §Audit events — `db.search_path.set_failed`. Rate-limited to
// avoid flooding the audit log on a stuck ext_<id> (e.g. schema was
// dropped out-of-band and every dispatch trips the wrapper). Same
// 64-entry LRU + per-key window pattern as `db.silent.used`.
//
// Called from the dispatch arm's search_path-wrapper catch block.
auto audit_search_path_set_failed(const std::string& extension_name,
                                  const std::string& sqlstate) -> void;

// Test seam — drain the rate-limiter state.
auto reset_search_path_audit_for_test() -> void;

} // namespace plinth::js::db
