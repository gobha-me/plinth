// SPDX-License-Identifier: MIT
//
// ICD-0.5.2 §Subscription RBAC — `pubsub.subscribe` cross-extension
// gate. Covers the new cross-ext Layer-3 + Layer-1 paths opened by
// honouring `bc.user.effective_rules` in `classify_pubsub_subscribe`
// per §Security Constraint 6 (shipped with LH-2; closes the v0.5.2
// implementation / ICD deviation).
//
// Same fixture pattern as `pubsub_test.cpp` — denial cases call
// `broker::note_subscribe_denied` → `plinth::log::audit`, which needs
// a Drogon DbClient reachable (audit() aborts inside drogon's
// `DbClientManager::getDbClient` otherwise). Gated by `pg_available`.
//
// Scope is intentionally narrow: ONLY the cross-ext paths this PR
// adds. Same-ext regression + broader U.* backfill (12 cases) stays
// for `0.5.2.N broker test matrix backfill`.

#include <catch2/catch_test_macros.hpp>

#include "async_bridge_fixture.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/realtime/broker.hpp"

#include <drogon/utils/coroutine.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::test_config;
using plinth::capabilities::UserContext;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::EvalResult;
using plinth::js::run_on_context;
using plinth::js::RuntimePool;

namespace {

auto drive(BridgeContext& bc, std::string_view source) -> EvalResult {
  return drogon::sync_wait(run_on_context(bc, source));
}

auto make_pool() -> RuntimePool {
  auto cfg = test_config();
  return {/*ext=*/nullptr, default_runtime_limits(), cfg, 1};
}

// Acquire a BC, set its extension identity + effective rules, run the
// script, return the EvalResult. Mirrors `pubsub_test.cpp::eval_with_extension`
// but also populates `bc.user.effective_rules` — the new signal the
// widened classify gate reads.
auto eval_as(RuntimePool& pool, const std::string& ext_name,
             std::vector<std::string> rules, std::string_view source)
    -> EvalResult {
  auto* bc = pool.acquire();
  bc->extension_name = ext_name;
  bc->user = UserContext{.user_id = "11111111-1111-1111-1111-111111111111",
                         .username = "test",
                         .auth_type = "session",
                         .effective_rules = std::move(rules),
                         .session_id = {},
                         .ip_address = {}};
  auto r = drive(*bc, source);
  pool.destroy(bc);
  return r;
}

// Re-enable the default rbac_enforce = true posture between test cases.
// Catch2 runs each TEST_CASE in its own subprocess today, but
// double-guarding keeps the contract clear.
struct RbacEnforceGuard {
  RbacEnforceGuard() = default;
  ~RbacEnforceGuard() {
    plinth::realtime::broker::set_rbac_enforce_for_test(true);
  }
  RbacEnforceGuard(const RbacEnforceGuard&) = delete;
  auto operator=(const RbacEnforceGuard&) -> RbacEnforceGuard& = delete;
  RbacEnforceGuard(RbacEnforceGuard&&) = delete;
  auto operator=(RbacEnforceGuard&&) -> RbacEnforceGuard& = delete;
};

} // namespace

TEST_CASE("pubsub.subscribe cross-ext Layer 3 — rule granted resolves",
          "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh2sidecar", {"lh1storm.realtime.subscribe.storm_event"},
              R"(pubsub.subscribe("plinth:ext:lh1storm:storm_event", () => {})
             .then(() => "ok", e => "err:" + e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

TEST_CASE(
    "pubsub.subscribe cross-ext Layer 3 — rule absent rejects rbac_denied",
    "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh2sidecar", {}, // no rules
              R"(pubsub.subscribe("plinth:ext:lh1storm:storm_event", () => {})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.rbac_denied");
}

TEST_CASE("pubsub.subscribe cross-ext Layer 3 — kernel.admin bypass resolves",
          "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh2sidecar", {"kernel.admin"},
              R"(pubsub.subscribe("plinth:ext:lh1storm:storm_event", () => {})
             .then(() => "ok", e => "err:" + e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

TEST_CASE("pubsub.subscribe cross-ext Layer 1 — rule granted resolves",
          "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh2sidecar", {"lh1storm.realtime.subscribe"},
              R"(pubsub.subscribe("plinth:data:ext_lh1storm.notes", () => {})
             .then(() => "ok", e => "err:" + e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

TEST_CASE(
    "pubsub.subscribe cross-ext Layer 1 — rule absent rejects rbac_denied",
    "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh2sidecar", {},
              R"(pubsub.subscribe("plinth:data:ext_lh1storm.notes", () => {})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.rbac_denied");
}

TEST_CASE("pubsub.subscribe cross-ext — rbac_enforce=false denies non-admin",
          "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  RbacEnforceGuard guard;
  plinth::realtime::broker::set_rbac_enforce_for_test(false);

  auto pool = make_pool();
  auto result = eval_as(
      pool, "lh2sidecar",
      {"lh1storm.realtime.subscribe.storm_event"}, // rule held but ignored
      R"(pubsub.subscribe("plinth:ext:lh1storm:storm_event", () => {})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.rbac_denied");
}

TEST_CASE("pubsub.subscribe cross-ext — rbac_enforce=false allows admin",
          "[js][realtime][broker][rbac]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  RbacEnforceGuard guard;
  plinth::realtime::broker::set_rbac_enforce_for_test(false);

  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh2sidecar", {"kernel.admin"},
              R"(pubsub.subscribe("plinth:ext:lh1storm:storm_event", () => {})
             .then(() => "ok", e => "err:" + e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

TEST_CASE("pubsub.subscribe own-ext Layer 3 — identity-only (no RBAC required)",
          "[js][realtime][broker][rbac]") {
  // Regression: own-extension subscribe must still resolve with an
  // empty effective_rules set (identity gate, not RBAC).
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "lh1storm", {}, // no RBAC rules
              R"(pubsub.subscribe("plinth:ext:lh1storm:storm_event", () => {})
             .then(() => "ok", e => "err:" + e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

// ── U.* backfill — ICD-0.5.2 §Test Cases U.01–U.12 ───────────────
//
// The 8 LH-2 cases above cover SC6 cross-ext widening (rule granted /
// absent / kernel.admin bypass / rbac_enforce=false). The U.* matrix
// covers the distinct JS-binding surface: happy own-ext paths, error
// taxonomy (extension_mismatch / layer_unsupported / channel_invalid /
// cancelled / quota_exceeded), and lifecycle (unsubscribe token, bc
// teardown, extension UPGRADE drain, multi-subscribe overwrite).

namespace {

// Seed a bc without destroying it on return. Caller owns pool.destroy.
// Used by U.09 / U.10 / U.11 / U.12 where the scenario spans multiple
// scripts and the bc must persist across evals so `globalThis` state
// (and registered subscriptions) survive between calls.
auto setup_bc(RuntimePool& pool, const std::string& ext_name,
              std::vector<std::string> rules) -> BridgeContext* {
  auto* bc = pool.acquire();
  bc->extension_name = ext_name;
  bc->user = UserContext{.user_id = "11111111-1111-1111-1111-111111111111",
                         .username = "test",
                         .auth_type = "session",
                         .effective_rules = std::move(rules),
                         .session_id = {},
                         .ip_address = {}};
  return bc;
}

} // namespace

TEST_CASE("U.01 happy Layer 3 own-ext subscribe resolves",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  plinth::realtime::broker::reset_metrics_for_test();

  auto pool = make_pool();
  auto result =
      eval_as(pool, "notes", {},
              R"(pubsub.subscribe("plinth:ext:notes:chat_typing", () => {})
             .then(() => "ok", e => "err:" + e.code))");
  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

TEST_CASE("U.02 empty extension_name → pubsub.extension_mismatch",
          "[js][realtime][pubsub][subscribe]") {
  // Post-SC6 widening, a `notes` bc subscribing `plinth:ext:terminal:x`
  // without a rule rejects `pubsub.rbac_denied` (covered by LH-2
  // cross-ext deny). The literal `pubsub.extension_mismatch` arm
  // now only fires when bc.extension_name is empty (unknown-extension
  // bc), which this case exercises.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "", {}, // empty extension_name
              R"(pubsub.subscribe("plinth:ext:notes:chat_typing", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.extension_mismatch");
}

TEST_CASE("U.03 Layer 1 own-ext with RBAC resolves",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "notes", {"notes.realtime.subscribe"},
              R"(pubsub.subscribe("plinth:data:ext_notes.notes", () => {})
             .then(() => "ok", e => "err:" + e.code))");
  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");
}

TEST_CASE("U.04 Layer 1 cross-ext without rule rejects pubsub.rbac_denied",
          "[js][realtime][pubsub][subscribe]") {
  // Full matrix case; LH-2 already covers the same shape at line 153.
  // Keep as U.04 so grep for ICD IDs hits a TEST_CASE.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "notes", {},
              R"(pubsub.subscribe("plinth:data:ext_terminal.foo", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.rbac_denied");
}

TEST_CASE("U.05 Layer 2 subscribe rejects pubsub.layer_unsupported",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result =
      eval_as(pool, "notes", {"kernel.admin"}, // even admin can't Layer-2
              R"(pubsub.subscribe("plinth:system:packages.installed", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.layer_unsupported");
}

TEST_CASE("U.06 malformed channel rejects pubsub.channel_invalid",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  auto pool = make_pool();
  auto result = eval_as(pool, "notes", {},
                        R"(pubsub.subscribe("not:a:channel", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.channel_invalid");
}

TEST_CASE("U.07 cancelled bc does not register a subscription",
          "[js][realtime][pubsub][subscribe]") {
  // Deviation: the literal ICD U.07 expects the `.then` onReject
  // callback to observe `e.code === "pubsub.cancelled"`. In practice
  // run_on_context's cancellation cascade (run_on_context.cpp:1036)
  // preempts the JS job queue's `.then` dispatch once bc.cancelled
  // is true — the binding's reject_inline fires, but the cascade
  // short-circuits evaluation before the onReject runs. We test the
  // observable side effect: no subscription is ever registered when
  // the bc is cancelled before the subscribe call. The specific
  // `pubsub.cancelled` reject code is covered by source review
  // (pubsub_bindings.cpp:321).
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  plinth::realtime::broker::stop();
  plinth::realtime::broker::reset_metrics_for_test();
  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  auto pool = make_pool();
  auto* bc = setup_bc(pool, "notes", {});
  bc->cancelled.store(true, std::memory_order_release);
  (void)drive(*bc,
              R"(pubsub.subscribe("plinth:ext:notes:chat_typing", () => {}))");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);

  pool.destroy(bc);
  plinth::realtime::broker::stop();
}

TEST_CASE("U.08 quota exceeded on last subscribe rejects",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  plinth::Config::Realtime::Broker bcfg{};
  bcfg.max_subscriptions_per_conn = 2;
  plinth::realtime::broker::stop();
  plinth::realtime::broker::reset_metrics_for_test();
  plinth::realtime::broker::start(bcfg);

  auto pool = make_pool();
  auto* bc = setup_bc(pool, "notes", {});
  // Subscribe up to the quota.
  auto r1 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:a", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r1.value.has_value());
  REQUIRE(r1.value->asString() == "ok");
  auto r2 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:b", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r2.value.has_value());
  REQUIRE(r2.value->asString() == "ok");
  // Third exceeds quota.
  auto r3 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:c", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r3.value.has_value());
  REQUIRE(r3.value->asString() == "pubsub.quota_exceeded");

  pool.destroy(bc);
  plinth::realtime::broker::stop();
}

TEST_CASE("U.09 unsubscribe token drops the subscription",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::stop();
  plinth::realtime::broker::reset_metrics_for_test();
  plinth::realtime::broker::start(bcfg);

  auto pool = make_pool();
  auto* bc = setup_bc(pool, "notes", {});

  // Step 1: subscribe and stash the unsub token on globalThis.
  auto r1 = drive(*bc, R"(
        globalThis.__u = null;
        pubsub.subscribe("plinth:ext:notes:chat_typing", () => {})
            .then(u => { globalThis.__u = u; return "ok"; },
                  e => e.code))");
  REQUIRE(r1.value.has_value());
  REQUIRE(r1.value->asString() == "ok");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 1);

  // Step 2: call the token and await the unsubscribe op.
  auto r2 = drive(*bc, R"(
        globalThis.__u().then(() => "unsubbed", e => "err:" + e.code))");
  REQUIRE(r2.value.has_value());
  REQUIRE(r2.value->asString() == "unsubbed");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);

  pool.destroy(bc);
  plinth::realtime::broker::stop();
}

TEST_CASE("U.10 bc teardown drops every subscription the bc held",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::stop();
  plinth::realtime::broker::reset_metrics_for_test();
  plinth::realtime::broker::start(bcfg);

  auto pool = make_pool();
  auto* bc = setup_bc(pool, "notes", {});
  auto r = drive(*bc,
                 R"(pubsub.subscribe("plinth:ext:notes:chat_typing", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "ok");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 1);

  // Destroying the bc runs RuntimePool::destroy → drops broker subs
  // per project_plinth_state v0.5.2.N fix.
  pool.destroy(bc);
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);

  plinth::realtime::broker::stop();
}

TEST_CASE("U.11 extension UPGRADE drain drops every JS subscription",
          "[js][realtime][pubsub][subscribe]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::stop();
  plinth::realtime::broker::reset_metrics_for_test();
  plinth::realtime::broker::start(bcfg);

  auto pool = make_pool();
  auto* bc = setup_bc(pool, "notes", {});
  auto r1 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:a", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r1.value->asString() == "ok");
  auto r2 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:b", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r2.value->asString() == "ok");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 2);

  // UPGRADE trigger: broker.drain_extension evicts both channels.
  plinth::realtime::broker::drain_extension("notes", "upgrading");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);

  pool.destroy(bc);
  plinth::realtime::broker::stop();
}

TEST_CASE("U.12 re-subscribe same channel is last-writer-wins (no quota bump)",
          "[js][realtime][pubsub][subscribe]") {
  // Per §OQ3 resolution + B.11b: a second subscribe on the same
  // (bc, channel) pair re-registers and bypasses the quota check.
  // The registry size stays at 1 so overflow can't be tripped by a
  // repeat call. This case covers the JS-binding path (binding
  // free+dup overwrite); B.11b covers the broker-registry path
  // with C++ sentinel BCs. Strict quota=1 + a successful first
  // subscribe means a quota failure on the second call would be
  // observable as `pubsub.quota_exceeded`; the assertion that the
  // second call resolves "ok" is the direct test that overwrite
  // semantics short-circuit the quota check.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  plinth::Config::Realtime::Broker bcfg{};
  bcfg.max_subscriptions_per_conn = 1; // strict quota
  plinth::realtime::broker::stop();
  plinth::realtime::broker::reset_metrics_for_test();
  plinth::realtime::broker::start(bcfg);

  auto pool = make_pool();
  auto* bc = setup_bc(pool, "notes", {});
  auto r1 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:chat_typing", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r1.value->asString() == "ok");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 1);

  auto r2 = drive(*bc,
                  R"(pubsub.subscribe("plinth:ext:notes:chat_typing", () => {})
             .then(() => "ok", e => e.code))");
  REQUIRE(r2.value->asString() == "ok");
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 1);

  pool.destroy(bc);
  plinth::realtime::broker::stop();
}
