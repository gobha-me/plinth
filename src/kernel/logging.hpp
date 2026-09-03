#pragma once

// plinth::log — kernel logging facade.
//
// Wraps spdlog for general logging and adds a single canonical audit path
// (`log::audit`) that writes to the plinth.audit_log table. Replaces the
// per-module inline audit_log helpers that previously lived in auth, groups,
// and rbac code.
//
// Design context: DESIGN-logging-subsystem.md (officially 0.1.7; the audit
// API is pulled forward into 0.1.6 because new WS code emits audit events
// and the existing helper was already duplicated in four files).

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <json/value.h>
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>

namespace plinth::log {

// Set up the async spdlog logger (console + rotating file sinks) based on
// dev_mode. Initializes the shared thread pool per
// DESIGN-logging-subsystem.md §Configuration (queue 8192, block overflow).
// Must be called once at startup before any logging.
auto init(const Config& cfg) -> void;

// Set the module-level node_id used by audit() inserts. Should be called
// once at startup, after init().
auto set_node_id(std::string node_id) -> void;

// Audit context — fields written into plinth.audit_log alongside the action.
// Empty strings are treated as SQL NULL (via NULLIF), so callers don't need
// to special-case session-only or PAT-only paths.
struct AuditCtx {
  std::string user_id;    // empty → NULL
  std::string session_id; // empty → NULL (typical for PAT-authed actions)
  std::string ip_address; // empty → NULL
};

// Write an audit entry. Async fire-and-forget via Drogon's DbClient.
// Failures are logged but do not propagate to the caller.
auto audit(std::string_view action, const Json::Value& detail,
           const AuditCtx& ctx) -> void;

// Convenience overload: pulls user_id / session_id from the request's auth
// attributes and ip from peerAddr(). Equivalent to constructing an AuditCtx
// manually from the post-SessionFilter request.
auto audit(std::string_view action, const Json::Value& detail,
           const drogon::HttpRequestPtr& req) -> void;

// Synchronous libpq-path audit writer, for callers that run BEFORE the
// Drogon DbClient is created (currently only bootstrap_groups). Opens a
// short-lived libpq connection, inserts one row unconditionally, and
// closes. Idempotency is the caller's concern. On error, logs via
// spdlog::error and returns — never throws.
auto audit_sync(const Config::Database& db, std::string_view action,
                const Json::Value& detail) -> void;

// True after `init()` sets up the async audit sink. Consumed by the
// 0.3.3 JS `audit.log` binding to decide whether to enqueue the
// AUDIT_WRITE op or reject inline with `audit.not_ready`.
[[nodiscard]] auto is_audit_ready() noexcept -> bool;

// Flip the audit-ready flag back to false so subsequent audit() calls
// short-circuit before touching any Drogon state. The coordinator invokes this
// before drogon::app().quit() to avoid the bad_weak_ptr teardown race
// documented in project_ws_flaky_segfault.md. No-op before init; idempotent.
auto shutdown() noexcept -> void;

#ifdef PLINTH_JS_TEST_SHIMS
// Test-only: clear the g_audit_ready flag so the 0.3.3 JS binding
// surfaces audit.not_ready cleanly. Mirrors the shim convention in
// tests/kernel/js/test_host_sleep.cpp. Compiled in only when
// -DPLINTH_JS_TEST_SHIMS=ON.
auto test_reset_ready() noexcept -> void;
#endif

// Thin re-exports of spdlog levels — provided so new code can write
// `plinth::log::info(...)` without sprinkling spdlog includes everywhere.
// Existing spdlog::*() call sites are not migrated by this commit.
template <typename... Args>
auto trace(spdlog::format_string_t<Args...> fmt, Args&&... args) -> void {
  spdlog::trace(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
auto debug(spdlog::format_string_t<Args...> fmt, Args&&... args) -> void {
  spdlog::debug(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
auto info(spdlog::format_string_t<Args...> fmt, Args&&... args) -> void {
  spdlog::info(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
auto warn(spdlog::format_string_t<Args...> fmt, Args&&... args) -> void {
  spdlog::warn(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
auto error(spdlog::format_string_t<Args...> fmt, Args&&... args) -> void {
  spdlog::error(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
auto critical(spdlog::format_string_t<Args...> fmt, Args&&... args) -> void {
  spdlog::critical(fmt, std::forward<Args>(args)...);
}

} // namespace plinth::log
