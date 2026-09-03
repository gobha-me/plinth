// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §14 — D.* + J.* debounce + jitter advisory cases. Phase 4
// lands the `subscribed` ack advisory pair (`recommended_debounce_ms`
// + `recommended_jitter_ms`) and the `debounce_renegotiate` parser
// + `realtime.debounce.advisory_overridden` audit pipeline. Tests
// drive the production code through synthetic seams
// (`subscribed_ack_for_test`, `run_debounce_renegotiate_for_test`)
// rather than a full Drogon WS round-trip — the round-trip surface
// is exercised by the integration suite in Phase 6.

#include "kernel/ws/subscriptions.hpp"

#include "kernel/config.hpp"
#include "kernel/realtime/events_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace ew = plinth::realtime::events_writer;

namespace {

// Restart the events writer with the supplied config so the
// subscribed-ack advisory and audit-window helpers read the new
// values via `current_config()`.
auto restart_writer(plinth::Config::Realtime::Events cfg = {}) -> void {
  ew::stop();
  ew::set_db_client_for_test(nullptr);
  ew::start(cfg);
}

} // namespace

// ── D.01 ─────────────────────────────────────────────────────────────

TEST_CASE("D.01: subscribed ack carries recommended_debounce_ms default",
          "[realtime][events][debounce][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false; // no PG required for the advisory shape
  restart_writer(cfg);

  auto ack = plinth::ws::subscribed_ack_for_test({"plinth:data:ext_d01.t"});
  REQUIRE(ack.isMember("recommended_debounce_ms"));
  REQUIRE(ack.isMember("recommended_jitter_ms"));
  CHECK(ack["recommended_debounce_ms"].asUInt64() == 100);
  CHECK(ack["recommended_jitter_ms"].asUInt64() == 50);

  ew::stop();
}

// ── D.02 ─────────────────────────────────────────────────────────────

TEST_CASE("D.02: operator config override propagates to ack",
          "[realtime][events][debounce][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  cfg.debounce.recommend_ms = 250;
  cfg.debounce.jitter_max_ms = 50; // unchanged default
  restart_writer(cfg);

  auto ack = plinth::ws::subscribed_ack_for_test({"plinth:data:ext_d02.t"});
  CHECK(ack["recommended_debounce_ms"].asUInt64() == 250);
  CHECK(ack["recommended_jitter_ms"].asUInt64() == 50);

  ew::stop();
}

// ── D.03 ─────────────────────────────────────────────────────────────

TEST_CASE("D.03: debounce_renegotiate fires advisory_overridden audit",
          "[realtime][events][debounce][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  restart_writer(cfg);
  plinth::ws::reset_debounce_audit_state_for_test();

  auto [fired, cnt] = plinth::ws::run_debounce_renegotiate_for_test(
      "user-d03", "plinth:data:ext_d03.t", 250);
  CHECK(fired);
  CHECK(cnt == 1);
  CHECK(plinth::ws::debounce_audit_emit_count_for_test() == 1);

  ew::stop();
}

// ── D.04 ─────────────────────────────────────────────────────────────

TEST_CASE("D.04: debounce_renegotiate audit rate-limited per (user, channel)",
          "[realtime][events][debounce][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  cfg.audit_window_ms = 60000; // explicit; matches default.
  restart_writer(cfg);
  plinth::ws::reset_debounce_audit_state_for_test();

  constexpr std::size_t N = 100;
  std::uint64_t last_cnt = 0;
  bool first_fired = false;
  bool any_subsequent_fired = false;
  for (std::size_t i = 0; i < N; ++i) {
    auto [fired, cnt] = plinth::ws::run_debounce_renegotiate_for_test(
        "user-d04", "plinth:data:ext_d04.t", 250);
    if (i == 0) {
      first_fired = fired;
    } else if (fired) {
      any_subsequent_fired = true;
    }
    last_cnt = cnt;
  }
  CHECK(first_fired);
  CHECK_FALSE(any_subsequent_fired);
  CHECK(plinth::ws::debounce_audit_emit_count_for_test() == 1);
  CHECK(last_cnt == N);

  ew::stop();
}

// ── D.05 ─────────────────────────────────────────────────────────────

TEST_CASE("D.05: zero debounce honored — recommended_debounce_ms == 0",
          "[realtime][events][debounce][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  cfg.debounce.recommend_ms = 0;
  restart_writer(cfg);

  auto ack = plinth::ws::subscribed_ack_for_test({"plinth:data:ext_d05.t"});
  CHECK(ack["recommended_debounce_ms"].asUInt64() == 0);
  CHECK(ack["recommended_jitter_ms"].asUInt64() == 50);

  ew::stop();
}

// ── J.01 ─────────────────────────────────────────────────────────────

TEST_CASE("J.01: default jitter populated on subscribed ack",
          "[realtime][events][debounce][jitter][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  restart_writer(cfg);

  auto ack = plinth::ws::subscribed_ack_for_test({"plinth:data:ext_j01.t"});
  CHECK(ack["recommended_jitter_ms"].asUInt64() == 50);

  ew::stop();
}

// ── J.02 ─────────────────────────────────────────────────────────────

TEST_CASE("J.02: operator zero jitter honored",
          "[realtime][events][debounce][jitter][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  cfg.debounce.jitter_max_ms = 0;
  restart_writer(cfg);

  auto ack = plinth::ws::subscribed_ack_for_test({"plinth:data:ext_j02.t"});
  CHECK(ack["recommended_jitter_ms"].asUInt64() == 0);

  ew::stop();
}

// ── J.03 ─────────────────────────────────────────────────────────────

TEST_CASE("J.03: operator-tuned max jitter honored",
          "[realtime][events][debounce][jitter][unit]") {
  plinth::Config::Realtime::Events cfg;
  cfg.enabled = false;
  cfg.debounce.jitter_max_ms = 5000;
  restart_writer(cfg);

  auto ack = plinth::ws::subscribed_ack_for_test({"plinth:data:ext_j03.t"});
  CHECK(ack["recommended_jitter_ms"].asUInt64() == 5000);

  ew::stop();
}
