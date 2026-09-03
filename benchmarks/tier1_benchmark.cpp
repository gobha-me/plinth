// Benchmarks for Tier 1 capability dispatch.
//
// Validates ICD-0.2.2 §Performance Targets:
//   * Tier 1 resolution: < 1μs (direct map lookup + function pointer call)
//
// Feeds the metrics story in architecture/04-services-ha.md §3.1 — the
// real dispatch latency is what the kernel will eventually export as
// "Capability resolution latency per tier".
//
// Two cases:
//   BM_Tier1_Hit                 — the primary < 1μs target.
//   BM_Tier1_Miss_FallsToTier2   — signature absent from Tier 1; exercises
//                                  the miss-path lock + second map lookup
//                                  (Tier 2 instance-scope fallback).
//
// Benchmark fixtures mirror tests/kernel/capabilities/resolution_test.cpp —
// register_tier1_handler / seed_tier2_cache_for_test populate the resolver
// without a live PG instance, and effective_rules = {"kernel.admin"} makes
// the ICD-0.2.4 step-3 RBAC check a pass-through so we measure dispatch
// cost in isolation.

#include <benchmark/benchmark.h>

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"

#include <json/value.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using plinth::capabilities::CachedCapability;
using plinth::capabilities::CapabilityCall;
using plinth::capabilities::CapabilityHandler;
using plinth::capabilities::HandlerOutcome;
using plinth::capabilities::UserContext;

// Minimal no-op handler: returns a fixed empty JSON object. Allocations
// inside the handler would dominate the measurement, so we keep it to
// the cost of constructing one empty Json::Value.
auto make_noop_handler() -> CapabilityHandler {
  return [](const Json::Value& /*args*/, const UserContext& /*ctx*/,
            int /*call_depth*/) -> HandlerOutcome {
    return Json::Value(Json::objectValue);
  };
}

// effective_rules = {"kernel.admin"} keeps the ICD-0.2.4 step-3 RBAC
// check a pass-through — we are not measuring permission logic here.
auto default_ctx() -> UserContext {
  return UserContext{.user_id = "11111111-1111-1111-1111-111111111111",
                     .username = "alice",
                     .auth_type = "session",
                     .effective_rules = {"kernel.admin"},
                     .session_id = {},
                     .ip_address = {}};
}

auto make_instance_entry(std::string signature, std::string provider_type)
    -> CachedCapability {
  return CachedCapability{
      .signature = std::move(signature),
      .provider_type = std::move(provider_type),
      .extension_name = "ext_example",
      .scope = "instance",
      .user_id = {},
      .rbac_rule = "kernel.admin",
      .enabled = true,
  };
}

} // namespace

// ── BM_Tier1_Hit ──────────────────────────────────────────────────────
// Warm Tier 1 map, signature present. This is the < 1μs ICD target.
static void BM_Tier1_Hit(benchmark::State& state) {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:bench.noop", "kernel.admin", make_noop_handler());

  const auto ctx = default_ctx();
  const CapabilityCall call{
      .signature = "kernel:1:bench.noop",
      .args = Json::Value(Json::objectValue),
      .call_depth = 0,
  };

  for (auto _ : state) {
    auto out = plinth::capabilities::call_capability(call, ctx);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_Tier1_Hit);

// ── BM_Tier1_Miss_FallsToTier2 ────────────────────────────────────────
// Signature absent from Tier 1 but present as an instance-scope Tier 2
// entry. Exercises the miss-path lock + second map lookup. Expected to
// be slower than BM_Tier1_Hit but comfortably below the < 1ms Tier 2
// target since the Tier 2 handler itself is a synthetic "not_implemented"
// echo (extension provider_type without a real handler).
static void BM_Tier1_Miss_FallsToTier2(benchmark::State& state) {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("ext_example:1:bench.noop", "extension"));

  const auto ctx = default_ctx();
  const CapabilityCall call{
      .signature = "ext_example:1:bench.noop",
      .args = Json::Value(Json::objectValue),
      .call_depth = 0,
  };

  for (auto _ : state) {
    auto out = plinth::capabilities::call_capability(call, ctx);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_Tier1_Miss_FallsToTier2);

// benchmark::benchmark_main provides main() — see CMakeLists.txt.
