// SPDX-License-Identifier: MIT

#include "kernel/lifecycle/shutdown_coordinator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

auto recording_hooks(std::vector<std::string>& calls,
                     bool fail_first_extension_drain)
    -> plinth::lifecycle::ShutdownHooks {
  auto record = [&calls](std::string name) {
    return [&calls, name = std::move(name)](std::chrono::milliseconds timeout) {
      REQUIRE(timeout >= 0ms);
      calls.push_back(name);
      return true;
    };
  };
  auto record_bounded = [&calls](std::string name) {
    return [&calls, name = std::move(name)](std::chrono::milliseconds timeout) {
      REQUIRE(timeout >= 0ms);
      calls.push_back(name);
      return true;
    };
  };
  auto record_void = [&calls](std::string name) {
    return [&calls, name = std::move(name)] { calls.push_back(name); };
  };
  return plinth::lifecycle::ShutdownHooks{
      .close_ingress = record("close_ingress"),
      .stop_listeners = record("stop_listeners"),
      .drain_async_tasks = record_bounded("drain_async_tasks"),
      .drain_rbac_workers = record_bounded("drain_rbac_workers"),
      .drain_extension_dispatches =
          [&calls,
           fail_first_extension_drain](std::chrono::milliseconds timeout) {
            REQUIRE(timeout >= 0ms);
            calls.emplace_back("drain_extension_dispatches");
            return !fail_first_extension_drain ||
                   std::ranges::count(calls, "drain_extension_dispatches") > 1;
          },
      .drain_js_stress_dispatches =
          record_bounded("drain_js_stress_dispatches"),
      .flush_database_state = record("flush_database_state"),
      .close_audit_gate = record("close_audit_gate"),
      .stop_drogon = record("stop_drogon"),
      .close_log_sinks = record_void("close_log_sinks"),
  };
}

} // namespace

TEST_CASE("shutdown coordinator implements the dependency graph once",
          "[lifecycle][shutdown]") {
  std::vector<std::string> calls;
  plinth::lifecycle::ShutdownCoordinator coordinator{
      recording_hooks(calls, false), 1s};

  auto result = coordinator.quiesce();
  REQUIRE(result.clean);
  REQUIRE_FALSE(coordinator.accepting_ingress());
  REQUIRE(result.completed_steps.at(1) == "drain_http_requests");
  REQUIRE(calls == std::vector<std::string>{
                       "close_ingress",
                       "stop_listeners",
                       "drain_async_tasks",
                       "drain_rbac_workers",
                       "drain_extension_dispatches",
                       "drain_js_stress_dispatches",
                       "flush_database_state",
                       "close_audit_gate",
                       "stop_drogon",
                   });

  REQUIRE(coordinator.quiesce().clean);
  coordinator.finish_after_drogon();
  coordinator.finish_after_drogon();
  REQUIRE(calls.back() == "close_log_sinks");
  REQUIRE(calls.size() == 10);
}

TEST_CASE("shutdown coordinator stops before a live dependency",
          "[lifecycle][shutdown]") {
  std::vector<std::string> calls;
  plinth::lifecycle::ShutdownCoordinator coordinator{
      recording_hooks(calls, true), 1s};

  auto failed = coordinator.quiesce();
  REQUIRE_FALSE(failed.clean);
  REQUIRE(failed.failed_step == "drain_extension_dispatches");
  REQUIRE(calls.back() == "drain_extension_dispatches");
  REQUIRE(calls.size() == 5);

  auto retried = coordinator.quiesce();
  REQUIRE(retried.clean);
  coordinator.finish_after_drogon();
  REQUIRE(calls.back() == "close_log_sinks");
}
