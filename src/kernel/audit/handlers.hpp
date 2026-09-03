#pragma once

// plinth::audit — kernel audit-log HTTP API and retention support.
//
// Per ICD-0.1.7:
// - GET /api/audit query endpoint (requires kernel.admin rule)
// - Retention purge helper (callable; scheduler wiring is 0.7)
//
// The audit *write* path (`plinth::log::audit`, `plinth::log::audit_sync`)
// lives in kernel/logging.{hpp,cpp}; this module is the read/cleanup side.

#include "kernel/config.hpp"

#include <cstdint>

namespace plinth::audit {

// Register the audit query endpoint (GET /api/audit) with Drogon.
// Idempotent per process: the first call registers the route and its
// `kernel.admin` rule requirement; subsequent calls are no-ops. Production
// calls this once from main() before `app().run()`; the grouped Catch2
// subprocess model (0.4.5.1) calls it from multiple TEST_CASEs, where the
// idempotency guard prevents drogon's `!routersInit_` assertion from
// firing after the kernel has begun listening.
auto register_audit_routes() -> void;

// Delete audit_log rows whose timestamp is older than `retention_days`.
// Returns the number of rows deleted. Uses a synchronous libpq connection;
// intended to be invoked by the 0.7 scheduler (not wired here).
// `retention_days <= 0` is a no-op and returns 0.
auto purge_older_than(const Config::Database& db, int retention_days)
    -> int64_t;

} // namespace plinth::audit
