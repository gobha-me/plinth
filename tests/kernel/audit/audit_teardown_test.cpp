#include <catch2/catch_test_macros.hpp>

#include <json/value.h>

#include "kernel/logging.hpp"

// Lock-in tests for `plinth::log::shutdown()` and the `g_audit_ready`
// gate that `audit()` consults at `logging.cpp:124` before reaching into
// `drogon::app().getDbClient()`. Regression for the 0.6.0.1 atexit
// ordering fix (5/5 SEGV repro on 2026-04-27 manual smoke):
// `broker::stop()` ran before `log::shutdown()` in the atexit chain,
// so its `audit("realtime.broker.stopped", …)` reached into a partially
// torn-down Drogon and SEGV'd at `DbClientManager::find (this=0x0)`.
//
// These cases pin the *gate* contract — they do not (and cannot, from a
// pure unit test) reproduce the partial-Drogon-teardown state. The
// no-core runtime repro under `./build/plinth serve` is the full
// regression verification.
//
// Pure-tag (no `[integration]`/`[ws]`/`[js]`) so this routes to
// `plinth_tests_pure`.

TEST_CASE("audit gate is closed before init", "[audit][teardown]") {
  // Default state: `g_audit_ready` defaults `false` per
  // logging.cpp:45. Catch2 cases run in the same TU process; if a
  // prior case ever flips the gate true, this assertion locks in
  // the documented contract from `logging.hpp:69` ("True after
  // `init()` sets up the async audit sink").
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}

TEST_CASE("audit() is a safe no-op when the gate is closed",
          "[audit][teardown]") {
  // The gate-closed branch must not reach `drogon::app().getDbClient()`
  // (logging.cpp:129) — the framework state is partial during atexit
  // teardown. This case calls audit() with the gate closed and
  // requires it to return without crashing or asserting.
  REQUIRE_FALSE(plinth::log::is_audit_ready());

  Json::Value payload(Json::objectValue);
  payload["dispatch_count"] = static_cast<Json::UInt64>(42);
  payload["rbac_denial_count"] = static_cast<Json::UInt64>(7);

  plinth::log::audit("realtime.broker.stopped", payload,
                     plinth::log::AuditCtx{});

  // No-op contract: the gate is still closed, no Drogon state was
  // materialized as a side effect.
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}

TEST_CASE("shutdown() leaves the gate closed and is idempotent",
          "[audit][teardown]") {
  // Per logging.hpp:73-77 — "Safe to call multiple times". `shutdown`
  // is the function that the 0.6.0.1 atexit handler calls FIRST so
  // every subsequent `stop()` audit no-ops cleanly.
  plinth::log::shutdown();
  REQUIRE_FALSE(plinth::log::is_audit_ready());

  plinth::log::shutdown();
  REQUIRE_FALSE(plinth::log::is_audit_ready());

  // audit() after shutdown() must remain safe.
  plinth::log::audit("test.shutdown.audit_after_shutdown",
                     Json::Value{Json::objectValue}, plinth::log::AuditCtx{});
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}
