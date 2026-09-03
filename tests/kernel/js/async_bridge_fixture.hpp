// SPDX-License-Identifier: MIT
//
// Test fixture for the 0.3.3 async-bridge integration suite.
//
// The async bridge dispatches via Drogon coroutines (`execSqlCoro`,
// `sleepCoro`). Those awaitables resume on a Drogon event loop, so any
// test that drives `run_on_context` must have a running loop available.
// This fixture spins one up on first use and leaves it running until
// the test binary exits — same pattern as
// tests/kernel/ws/ws_test_fixture.cpp.

#pragma once

#include "kernel/config.hpp"

#include <cstdint>

namespace plinth::async_bridge_test {

// True iff the PG env vars (PLINTH_PG_HOST etc.) are set AND a libpq
// connect succeeds. Mirrors tests/kernel/audit/audit_test.cpp's
// pg_available — gating pattern for ICD §Tests Group A–E PG-backed
// cases.
[[nodiscard]] auto pg_available() -> bool;

// Build a test Config with PG env overrides applied. dev_mode=true so
// schema bootstrap runs unconditionally.
[[nodiscard]] auto test_config() -> plinth::Config;

// Reset and re-bootstrap the plinth schema. Each test that needs a
// known DB state calls this in its prologue.
auto reset_schema(const plinth::Config::Database& db) -> void;

// Lazy-initialize Drogon's event loop on a background thread WITHOUT
// creating a PG DbClient. Idempotent. Calls plinth::log::init() so
// the audit binding's is_audit_ready() gate returns true (audit()
// itself no-ops when no DbClient is present — see logging.cpp).
// Cap.* / pure-JS tests that don't touch db.* or audit.log call this
// so their subprocess never spins up drogon's DbClient pool —
// closes the teardown sub-path where DbClient's own heartbeat ticks
// race weak_ptr teardown and trip `bad_weak_ptr`. See
// project_ws_flaky_segfault.md fifth-occurrence notes.
auto ensure_drogon_running() -> void;

// Same as `ensure_drogon_running()` plus `drogon::app().createDbClient`
// against the test PG instance. Tests that actually use `db.query` /
// `audit.log` (and want the audit write to land in the table) call
// this. Shares the same call_once as `ensure_drogon_running()`; only
// one of the two should be called per subprocess.
auto ensure_drogon_with_db_running() -> void;

} // namespace plinth::async_bridge_test
