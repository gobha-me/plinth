// SPDX-License-Identifier: MIT
//
// Process-lifetime PG client shared across realtime test TUs.
//
// 0.6.3.N — generalises the seq_generation_test.cpp:118-126 pattern
// across the realtime test TUs that previously each constructed their
// own `drogon::orm::DbClient::newPgClient(...)`. Per-test create+
// destroy reproducibly trips a Drogon `EventLoopThreadPool::~`
// `Resource deadlock avoided` (`pthread_self_join` → EDEADLK) when the
// last `shared_ptr<DbClient>` drops on a coroutine running on that
// pool's IO thread.
//
// Sharing the client at function-local-static lifetime moves the
// destructor outside any coroutine context: per Itanium ABI, function-
// local statics' destructors run AFTER `std::atexit` chains have
// completed (Drogon's loop has already stopped), on the main thread,
// after all test threads have exited. The race is unreachable from the
// harness.
//
// Tests that need `db.* set_db_client_for_test(shared_pg_client())`
// keep using the same hook surface (events_writer.hpp) — only the
// client construction site moves.
//
// Production behaviour is unchanged. The g_inflight gate inside
// `events_writer.cpp:600-615` stays as defense-in-depth; this header
// fixes the failure mode at the test-harness layer where it actually
// fires.

#pragma once

#include <drogon/orm/DbClient.h>

namespace plinth::realtime_test {

// Returns a process-lifetime shared `DbClient`. Multiple distinct
// `connNum` values get independent pools. Lazy on first use; PG must
// be available (caller's responsibility — check `PLINTH_PG_HOST` /
// the TU's local `pg_available()` first or the underlying connect
// will throw).
//
// `connNum` mirrors the existing test convention: most realtime TUs
// use 1 (single conn pool), some use 2 (one writer pool + one reader
// pool to avoid serialising fixture-side reads behind in-flight
// writes). Map-keyed so both keep their own static slot.
auto shared_pg_client(int connNum = 1) -> drogon::orm::DbClientPtr&;

} // namespace plinth::realtime_test
