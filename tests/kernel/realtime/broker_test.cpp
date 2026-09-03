// SPDX-License-Identifier: MIT
//
// ICD-0.5.2 §Test Cases — B.* broker subsystem.
//
// v0.5.2 ship landed 10 B.* cases (B.01–04, B.07, B.09, B.11, B.11b,
// B.13, B.14). The 0.5.2.N broker-test-backfill session adds the
// remaining 5: B.05, B.06, B.08, B.10, B.12 — closing the B.* matrix.
//
// B.05 / B.06 require a real WS subscriber conn to exercise the
// `publish_dispatched` → `ConnectionRegistry::for_each` path. The ICD
// marks them PG-gated=No, but without a fake-conn seam on publish.hpp
// they need `ws_test_fixture`'s drogon + WsTestClient scaffolding.
// Documented deviation: these two tests are PG-gated in practice and
// skip cleanly when PG is unavailable. Coverage equivalence with the
// I.* end-to-end suite is intentional — B.05 / B.06 isolate the
// broker's dispatch-arm routing from the listener/coalescer source.
//
// Tag: `[realtime][broker]` (ICD-0.5.2 §Test Cases — Test-seam notes).

#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/listener.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <json/value.h>
#include <thread>

using plinth::realtime::DispatchedEvent;
namespace broker = plinth::realtime::broker;

namespace {

auto make_ev(const std::string& layer, const std::string& channel)
    -> DispatchedEvent {
  DispatchedEvent ev;
  ev.layer = layer;
  ev.channel = channel;
  Json::Value env(Json::objectValue);
  env["layer"] = layer;
  env["channel"] = channel;
  ev.envelope = std::move(env);
  return ev;
}

// Config helper — each TEST_CASE gets a fresh scaffold + metrics.
auto reset() -> void {
  broker::stop(); // idempotent; no-op if never started
  broker::reset_metrics_for_test();
  plinth::realtime::clear_handlers_for_test();
}

} // namespace

TEST_CASE("B.01 start() is idempotent", "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{}; // defaults: enabled=true
  broker::start(cfg);
  broker::start(cfg); // second call — no duplicate registration
  // Counters still monotonic; handler registered exactly once.
  REQUIRE(broker::dispatch_count() == 0);
  broker::dispatch_for_test(make_ev("extension", "plinth:ext:x:t"));
  REQUIRE(broker::dispatch_count() == 1);
  broker::stop();
}

TEST_CASE("B.02 start(enabled=false) no-ops", "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  cfg.enabled = false;
  broker::start(cfg);
  // Handler not registered -> simulated dispatch is a no-op; the
  // test-seam path short-circuits on g_enabled=false.
  auto n = broker::dispatch_for_test(make_ev("extension", "plinth:ext:x:t"));
  REQUIRE(n == 0);
  REQUIRE(broker::dispatch_count() == 0);
  broker::stop();
}

TEST_CASE("B.03 stop() before start does not crash",
          "[realtime][broker][unit]") {
  reset();
  broker::stop(); // never started
  broker::stop(); // idempotent
  SUCCEED("stop() before start is a no-op");
}

TEST_CASE("B.04 dispatch with zero subscribers", "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);
  auto n =
      broker::dispatch_for_test(make_ev("extension", "plinth:ext:unseen:ch"));
  // No WS or JS subscribers — match count is zero; counter bumps.
  REQUIRE(n == 0);
  REQUIRE(broker::dispatch_count() == 1);
  REQUIRE(broker::js_subscriber_count() == 0);
  broker::stop();
}

TEST_CASE("B.11 metrics counters monotonic + reset seam",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);
  for (int i = 0; i < 5; ++i) {
    broker::dispatch_for_test(make_ev("extension", "plinth:ext:x:t"));
  }
  REQUIRE(broker::dispatch_count() >= 5);
  broker::reset_metrics_for_test();
  REQUIRE(broker::dispatch_count() == 0);
  REQUIRE(broker::rbac_denial_count() == 0);
  broker::stop();
}

TEST_CASE("B.13 handler short-circuit post-stop", "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);
  broker::dispatch_for_test(make_ev("extension", "plinth:ext:x:t"));
  REQUIRE(broker::dispatch_count() == 1);
  broker::stop();
  // Post-stop the test seam honors g_enabled=false and counter
  // stays at 1 (no increment on short-circuit).
  auto n = broker::dispatch_for_test(make_ev("extension", "plinth:ext:x:t"));
  REQUIRE(n == 0);
  REQUIRE(broker::dispatch_count() == 1);
}

TEST_CASE("drain_extension is idempotent with zero subscriptions",
          "[realtime][broker][unit]") {
  // ICD B.* matrix item covering the drain hook's zero-match path.
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);
  broker::drain_extension("notes"); // no subs -> no audit, no-op
  broker::drain_extension("notes"); // idempotent
  REQUIRE(broker::js_subscriber_count() == 0);
  broker::stop();
}

TEST_CASE("B.07 drain_extension evicts JS subs matching the extension",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);

  // Stand-in BridgeContext pointers — the broker registry is keyed
  // on the pointer, not on JS state, so a non-null sentinel is enough
  // for the registry-only path exercised here. The full
  // dispatch-to-callbacks path is covered by the integration suite.
  // Real stack BridgeContexts so drain_extension's
  // `bc->extension_name` check is safe to dereference. `extension_name`
  // defaults to empty, so drain_extension's "drop bc with matching
  // extension" branch is inert — only the channel-prefix-match arm
  // runs, which is what this test targets.
  plinth::js::BridgeContext bc_1;
  plinth::js::BridgeContext bc_2;
  auto* dummy_bc_1 = &bc_1;
  auto* dummy_bc_2 = &bc_2;

  REQUIRE(broker::register_js_subscription(dummy_bc_1,
                                           "plinth:ext:notes:chat_typing", 1));
  REQUIRE(broker::register_js_subscription(dummy_bc_1,
                                           "plinth:data:ext_notes.notes", 2));
  REQUIRE(broker::register_js_subscription(dummy_bc_2,
                                           "plinth:ext:terminal:typing", 3));
  REQUIRE(broker::js_subscriber_count() == 3);

  broker::drain_extension("notes", "upgrading");
  // Both `notes` subscriptions on bc_1 drop; `terminal` on bc_2 stays.
  REQUIRE(broker::js_subscriber_count() == 1);

  broker::drain_extension("terminal", "uninstall");
  REQUIRE(broker::js_subscriber_count() == 0);

  // Release the stand-ins so stop() sees a clean registry.
  broker::drop_bc_subscriptions(dummy_bc_1);
  broker::drop_bc_subscriptions(dummy_bc_2);
  broker::stop();
}

TEST_CASE("B.09 unregister_js_subscription removes single channel",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);
  plinth::js::BridgeContext bc_var;
  auto* dummy_bc = &bc_var;

  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:a", 10));
  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:b", 11));
  REQUIRE(broker::js_subscriber_count() == 2);

  broker::unregister_js_subscription(dummy_bc, "plinth:ext:notes:a");
  REQUIRE(broker::js_subscriber_count() == 1);

  // Unregister of unknown channel is a no-op.
  broker::unregister_js_subscription(dummy_bc, "plinth:ext:unseen:x");
  REQUIRE(broker::js_subscriber_count() == 1);

  broker::unregister_js_subscription(dummy_bc, "plinth:ext:notes:b");
  REQUIRE(broker::js_subscriber_count() == 0);
  broker::stop();
}

TEST_CASE("B.11b register_js_subscription honours quota",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  cfg.max_subscriptions_per_conn = 2;
  broker::start(cfg);
  plinth::js::BridgeContext bc_var;
  auto* dummy_bc = &bc_var;

  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:a", 1));
  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:b", 2));
  REQUIRE_FALSE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:c",
                                                 3)); // quota tripped

  // Re-subscribing an existing channel bypasses the quota check
  // (overwrite semantics per §OQ3 / U.12).
  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:a", 4));
  REQUIRE(broker::js_subscriber_count() == 2);

  broker::drop_bc_subscriptions(dummy_bc);
  broker::stop();
}

TEST_CASE("B.14 note_subscribe_denied bumps metric + rate-limits audit",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);
  broker::reset_audit_windows_for_test();

  // Three denials on the same (user, channel) key — the metric bumps
  // every time; only the first falls in a fresh window and is audited.
  // Direct audit emission is hard to assert without a log spy, so we
  // verify the observable-from-tests part: metric monotonicity and
  // absence of regressions in adjacent counters.
  plinth::log::AuditCtx ctx{
      .user_id = "user-a", .session_id = {}, .ip_address = {}};
  broker::note_subscribe_denied(ctx, "plinth:ext:notes:x", "extension",
                                "rbac_denied", "ws");
  broker::note_subscribe_denied(ctx, "plinth:ext:notes:x", "extension",
                                "rbac_denied", "ws");
  broker::note_subscribe_denied(ctx, "plinth:ext:notes:x", "extension",
                                "rbac_denied", "ws");
  REQUIRE(broker::rbac_denial_count() == 3);
  REQUIRE(broker::dispatch_count() == 0);

  // Different key gets its own window.
  broker::note_subscribe_denied(ctx, "plinth:ext:notes:y", "extension",
                                "rbac_denied", "ws");
  REQUIRE(broker::rbac_denial_count() == 4);

  // Reset clears windows so the next denial on the original key
  // opens a fresh window (observable only via audit emission).
  broker::reset_audit_windows_for_test();
  broker::note_subscribe_denied(ctx, "plinth:ext:notes:x", "extension",
                                "rbac_denied", "ws");
  REQUIRE(broker::rbac_denial_count() == 5);

  broker::stop();
}

TEST_CASE("B.08 dispatch one JS subscriber, match returns 1",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);

  // Sentinel bc — dispatch_to_js_subscribers snapshots matches off the
  // shared registry mutex BEFORE hopping onto drogon's main loop, and
  // returns the snapshot size regardless of whether the loop is
  // available (broker.cpp:dispatch_to_js_subscribers). That's what B.08
  // asserts: the registry-match arm routes correctly.
  plinth::js::BridgeContext bc_var;
  auto* dummy_bc = &bc_var;
  REQUIRE(broker::register_js_subscription(dummy_bc,
                                           "plinth:ext:notes:chat_typing", 7));
  REQUIRE(broker::js_subscriber_count() == 1);

  auto n = broker::dispatch_for_test(
      make_ev("extension", "plinth:ext:notes:chat_typing"));
  REQUIRE(n == 1);
  REQUIRE(broker::dispatch_count() == 1);

  // Non-matching channel does not route to the same bc.
  auto n2 =
      broker::dispatch_for_test(make_ev("extension", "plinth:ext:notes:other"));
  REQUIRE(n2 == 0);

  broker::drop_bc_subscriptions(dummy_bc);
  broker::stop();
}

TEST_CASE("B.10 dispatch_for_test completes under the latency budget "
          "from a listener-thread simulator",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);

  // Seed a small registry so the dispatch has real work to do
  // (registry lookup + snapshot under shared lock).
  plinth::js::BridgeContext bc_a;
  plinth::js::BridgeContext bc_b;
  REQUIRE(broker::register_js_subscription(&bc_a,
                                           "plinth:ext:notes:chat_typing", 1));
  REQUIRE(broker::register_js_subscription(&bc_b, "plinth:ext:notes:other", 2));

  // Simulate the listener calling broker_event_handler from its own
  // thread. ICD §Threading model budget: mutex-held time ≤ 1 ms per
  // dispatch. A 50 ms envelope absorbs test-host scheduling jitter;
  // a regression into blocking I/O on the listener path would blow
  // this out by orders of magnitude. Results are captured via a
  // promise + collected on the test thread to keep Catch2 assertions
  // off the spawned thread (thread-safety requirement).
  std::promise<std::pair<std::size_t, std::chrono::nanoseconds>> done;
  auto fut = done.get_future();
  std::thread listener_sim([&done]() {
    auto start = std::chrono::steady_clock::now();
    auto n = broker::dispatch_for_test(
        make_ev("extension", "plinth:ext:notes:chat_typing"));
    auto elapsed = std::chrono::steady_clock::now() - start;
    done.set_value({n, elapsed});
  });
  listener_sim.join();
  auto [matches, elapsed] = fut.get();
  REQUIRE(matches == 1);
  REQUIRE(elapsed < std::chrono::milliseconds(50));

  broker::drop_bc_subscriptions(&bc_a);
  broker::drop_bc_subscriptions(&bc_b);
  broker::stop();
}

TEST_CASE("B.12 stop() clears the JS subscription registry",
          "[realtime][broker][unit]") {
  reset();
  plinth::Config::Realtime::Broker cfg{};
  broker::start(cfg);

  plinth::js::BridgeContext bc_var;
  auto* dummy_bc = &bc_var;
  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:a", 1));
  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:b", 2));
  REQUIRE(broker::register_js_subscription(dummy_bc, "plinth:ext:notes:c", 3));
  REQUIRE(broker::js_subscriber_count() == 3);

  broker::stop();
  REQUIRE(broker::js_subscriber_count() == 0);
}

// B.05 / B.06 need a real WS subscriber conn because `publish_dispatched`
// iterates `ConnectionRegistry::instance()`. Without a
// `register_fake_conn_for_test` seam on publish.hpp, the cleanest path
// is the existing ws_test_fixture drogon + WsTestClient scaffolding.
// ICD marks them PG-gated=No, but in practice they skip cleanly when
// PG is unavailable; I.01–I.04 cover equivalent end-to-end behaviour.
// Tag `[ws][integration]` routes them to `plinth_tests_ws` where the
// ws_test_fixture drogon lifecycle is the only one running — avoids
// the `!running_` collision with `plinth_tests_pg`'s async_bridge_fixture
// drogon (e.g. dispatch_extension_test.cpp).

namespace {

using namespace std::chrono_literals;

auto authenticate_admin_ws(plinth::ws_test::WsTestClient& client,
                           plinth::ws_test::TestPg& pg,
                           const std::string& username) -> void {
  auto user_id = plinth::ws_test::insert_user(pg, username, "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  REQUIRE(client.connect(2s));
  Json::Value auth_msg;
  auth_msg["type"] = "auth";
  auth_msg["token"] = raw_token;
  client.send_json(auth_msg);
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
}

auto send_subscribe(plinth::ws_test::WsTestClient& client,
                    const std::string& channel) -> void {
  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append(channel);
  sub["channels"] = arr;
  client.send_json(sub);
  auto ack = client.receive_json(3s);
  REQUIRE(ack.has_value());
  REQUIRE((*ack)["type"].asString() == "subscribed");
}

} // namespace

TEST_CASE("B.05 dispatch one WS subscriber, match delivers one frame",
          "[realtime][broker][ws][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  reset();
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  plinth::ws_test::WsTestClient client;
  authenticate_admin_ws(client, pg, "b05-admin");
  send_subscribe(client, "plinth:data:ext_x.t");

  plinth::Config::Realtime::Broker bcfg{};
  broker::start(bcfg);

  // ICD B.05 says `returns 1`, but `dispatch_for_test` counts the
  // JS-arm only (broker.cpp:dispatch_for_test) — the WS arm's match
  // count is observable only as a delivered frame. Primary assertion
  // is the frame on the wire; JS-arm count is zero since no bc subs.
  (void)broker::dispatch_for_test(make_ev("data", "plinth:data:ext_x.t"));

  auto frame = client.receive_json(3s);
  REQUIRE(frame.has_value());
  REQUIRE((*frame)["type"].asString() == "event");
  REQUIRE((*frame)["channel"].asString() == "plinth:data:ext_x.t");

  broker::stop();
}

TEST_CASE("B.06 dispatch one WS subscriber, no match delivers no frame",
          "[realtime][broker][ws][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  reset();
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  plinth::ws_test::WsTestClient client;
  authenticate_admin_ws(client, pg, "b06-admin");
  send_subscribe(client, "plinth:data:ext_x.t");

  plinth::Config::Realtime::Broker bcfg{};
  broker::start(bcfg);

  // Dispatch a channel the subscriber did NOT request. Both arms
  // (WS filter by ConnState.channels, JS bc registry lookup) should
  // miss. Return value 0 covers the JS arm; frame absence covers WS.
  auto n = broker::dispatch_for_test(make_ev("data", "plinth:data:ext_y.t"));
  REQUIRE(n == 0);

  auto frame = client.receive_json(300ms);
  REQUIRE_FALSE(frame.has_value());

  broker::stop();
}
