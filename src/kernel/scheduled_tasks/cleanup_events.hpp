// SPDX-License-Identifier: MIT
//
// ICD-0.5.4-events-table-delta-sync §Cleanup Task.
//
// `plinth.events_cleanup` — periodic retention sweep over plinth.events.
// In 0.5.4 the call site rides events_writer's dedicated EventLoopThread
// via `runEvery(cleanup_interval_ms)`. Per
// `architecture/04-services-ha.md §2`, the proper scheduled-tasks
// subsystem ships in 0.7.2; this TU's surface is stable enough that
// the migration is a one-line wiring change at the future site.
//
// Multi-node single-sweeper coordination uses a transaction-scoped
// PG advisory lock — `pg_try_advisory_xact_lock(KEY)` — so the lock
// releases at COMMIT/ROLLBACK regardless of which pooled connection
// the next coroutine picks up. ICD §Cleanup pseudocode shows the
// session-scoped variant; that is wrong under Drogon's connection
// pool (a follow-up `co_await execSqlCoro` may run on a different
// connection, dropping the lock). Documented deviation in CHANGELOG.

#pragma once

#include "kernel/config.hpp"

#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

namespace plinth::scheduled_tasks::cleanup_events {

// Run a single sweep tick. Acquires the advisory lock; if a peer
// already holds it, returns silently. Otherwise DELETEs every row
// whose `created_at` is older than `cfg.retention_seconds`. Audits
// `realtime.events.cleanup_swept` on success and
// `realtime.events.write_failed reason=cleanup_failed` on PG error.
auto run(Config::Realtime::Events cfg) -> drogon::Task<void>;

// Test seam — pin a specific DbClient. When set, every PG operation
// routes through this client instead of `drogon::app().getDbClient()`.
// Pass nullptr to clear.
auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void;

// Test seam — reset audit-window state between TEST_CASEs.
auto reset_audit_state_for_test() -> void;

// Test seam — last-sweep row-deletion count. Useful for assertions
// without grovelling through the audit log.
[[nodiscard]] auto last_swept_for_test() -> std::int64_t;

} // namespace plinth::scheduled_tasks::cleanup_events
