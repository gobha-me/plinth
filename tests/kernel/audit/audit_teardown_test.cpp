#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <json/value.h>

#include "kernel/logging.hpp"

// These cases pin the pre-init/after-seal no-op contract without materializing
// Drogon. The real PostgreSQL blocked-write case separately tests the common
// coordinator's drain and retry before database destruction.
//
// Pure-tag (no `[integration]`/`[ws]`/`[js]`) so this routes to
// `plinth_tests_pure`.

TEST_CASE("audit gate is closed before init", "[audit][teardown]") {
  // This pure process never initializes the audit sink.
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}

TEST_CASE("audit() is a safe no-op when the gate is closed",
          "[audit][teardown]") {
  // A rejected audit must not touch Drogon's database manager or logger.
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
  // The coordinator's permanent admission seal is idempotent and safe even
  // when this process never initialized logging or a database manager.
  plinth::log::shutdown();
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  REQUIRE(plinth::log::wait_for_audits(std::chrono::milliseconds{0}));

  plinth::log::shutdown();
  REQUIRE_FALSE(plinth::log::is_audit_ready());

  // audit() after shutdown() must remain safe.
  plinth::log::audit("test.shutdown.audit_after_shutdown",
                     Json::Value{Json::objectValue}, plinth::log::AuditCtx{});
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}
