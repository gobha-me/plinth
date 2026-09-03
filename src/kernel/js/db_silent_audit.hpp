// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §silent Flag §Rate-limited `db.silent.used` audit.
//
// Each `db.exec(..., {silent:true})` call increments an in-memory
// counter keyed by `bc.extension_name`. At first-hit-in-window and
// every `db.silent.audit_window_ms` afterward (default 60 s) the
// `db.silent.used` audit fires with `{extension, count_in_window,
// window_ms}`. Lets operators detect extensions that silence every
// write (suggesting misconfiguration) without drowning the audit log.

#pragma once

#include <cstddef>
#include <string>

namespace plinth::js {

// Called from the DB_EXEC dispatch arm whenever `op.silent == true`.
// Fires the rate-limited `db.silent.used` audit (see header intro).
// `extension_name` matches `AsyncOp::bc_extension_name` — empty string
// for kernel-scope bcs; the audit still emits with an empty
// `extension` field in that case (no suppression).
auto record_silent_use(const std::string& extension_name) -> void;

// Set the audit aggregation window. Called from main.cpp after
// `load_config`, and from tests. Thread-safe (atomic).
auto set_silent_audit_window_ms(std::size_t window_ms) -> void;

// Reset the rate-limiter state. Test-only — clears all entries.
// Callers MUST also `set_silent_audit_window_ms` afterward if they
// need a non-default window.
auto reset_silent_audit_for_test() -> void;

// Snapshot of the current window (test introspection).
[[nodiscard]] auto silent_audit_window_ms() -> std::size_t;

} // namespace plinth::js
