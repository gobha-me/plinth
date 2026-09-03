// Benchmarks for Tier 2 capability dispatch.
//
// Validates ICD-0.2.2 §Performance Targets:
//   * Tier 2 cache hit: < 1ms (including scope precedence check)
//
// Feeds the metrics story in architecture/04-services-ha.md §3.1.
//
// Four cases — each exercises one branch of the §Resolution Algorithm
// step 3–5 walk after Tier 1 miss:
//
//   BM_Tier2_InstanceScope_Hit  — the primary < 1ms target.
//                                 Dispatch lands in dispatch_tier2;
//                                 a 0.2.x extension entry resolves
//                                 to TIER3_NOT_AVAILABLE — that is
//                                 the correct outcome and does not
//                                 distort the dispatch-cost measurement.
//   BM_Tier2_UserScope_Override — both instance and user entries present;
//                                 user_scope_key built and looked up first.
//   BM_Tier2_Disabled           — instance entry with enabled=false;
//                                 fast-reject path in dispatch_tier2.
//   BM_Tier2_NotFound           — no entries at all; worst-case lookup
//                                 path (Tier 1 miss + both Tier 2 probes).
//
// Benchmarks measure dispatch cost in isolation:
//   * effective_rules = {"kernel.admin"} keeps the ICD-0.2.4 step-3 RBAC
//     check a pass-through.
//   * seed_tier2_cache_for_test populates the cache without PG.
//   * The no-op handler avoids allocation-dominated measurements (see
//     tier1_benchmark.cpp for the equivalent Tier 1 shape).

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
using plinth::capabilities::UserContext;

auto default_ctx() -> UserContext {
  return UserContext{.user_id = "11111111-1111-1111-1111-111111111111",
                     .username = "alice",
                     .auth_type = "session",
                     .effective_rules = {"kernel.admin"},
                     .session_id = {},
                     .ip_address = {}};
}

auto make_instance_entry(std::string signature, bool enabled = true)
    -> CachedCapability {
  return CachedCapability{
      .signature = std::move(signature),
      .provider_type = "extension",
      .extension_name = "ext_example",
      .scope = "instance",
      .user_id = {},
      .rbac_rule = "kernel.admin",
      .enabled = enabled,
  };
}

auto make_user_entry(std::string signature, std::string user_id)
    -> CachedCapability {
  return CachedCapability{
      .signature = std::move(signature),
      .provider_type = "extension",
      .extension_name = "ext_example",
      .scope = "user",
      .user_id = std::move(user_id),
      .rbac_rule = "kernel.admin",
      .enabled = true,
  };
}

} // namespace

// ── BM_Tier2_InstanceScope_Hit ────────────────────────────────────────
// The primary < 1ms target. Tier 1 miss → user-scope probe miss (no user
// entry) → instance-scope hit → RBAC pass → dispatch_tier2 returns
// TIER3_NOT_AVAILABLE (extensions route through Tier 3 until 0.3.x).
// Measurement is the full dispatch-pipeline cost.
static void BM_Tier2_InstanceScope_Hit(benchmark::State& state) {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("ext_example:1:bench.noop"));

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
BENCHMARK(BM_Tier2_InstanceScope_Hit);

// ── BM_Tier2_UserScope_Override ───────────────────────────────────────
// Both instance and user entries present. User context carries user_id,
// so the user_scope_key probe hits first and wins precedence over the
// instance entry. Measures the additional cost of the user-scope-key
// string build + first map lookup.
static void BM_Tier2_UserScope_Override(benchmark::State& state) {
  plinth::capabilities::clear_resolver_for_test();
  const std::string signature = "ext_example:1:bench.scoped";
  const std::string user_id = "11111111-1111-1111-1111-111111111111";
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry(signature));
  plinth::capabilities::seed_tier2_cache_for_test(
      make_user_entry(signature, user_id));

  const auto ctx = default_ctx(); // user_id matches
  const CapabilityCall call{
      .signature = signature,
      .args = Json::Value(Json::objectValue),
      .call_depth = 0,
  };

  for (auto _ : state) {
    auto out = plinth::capabilities::call_capability(call, ctx);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_Tier2_UserScope_Override);

// ── BM_Tier2_Disabled ─────────────────────────────────────────────────
// Instance entry with enabled=false. Dispatch reaches dispatch_tier2,
// which short-circuits on !entry.enabled and returns CAPABILITY_DISABLED.
// The fast-reject path — expected to be indistinguishable from the hit
// path within measurement noise (same lookup cost, different branch).
static void BM_Tier2_Disabled(benchmark::State& state) {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("ext_example:1:bench.disabled",
                          /*enabled=*/false));

  const auto ctx = default_ctx();
  const CapabilityCall call{
      .signature = "ext_example:1:bench.disabled",
      .args = Json::Value(Json::objectValue),
      .call_depth = 0,
  };

  for (auto _ : state) {
    auto out = plinth::capabilities::call_capability(call, ctx);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_Tier2_Disabled);

// ── BM_Tier2_NotFound ─────────────────────────────────────────────────
// No entries seeded. Dispatch walks Tier 1 miss → user-scope probe miss
// → instance-scope probe miss → CAPABILITY_NOT_FOUND. The worst-case
// lookup path — two Tier 2 probes before the deny.
static void BM_Tier2_NotFound(benchmark::State& state) {
  plinth::capabilities::clear_resolver_for_test();

  const auto ctx = default_ctx();
  const CapabilityCall call{
      .signature = "ext_example:1:bench.missing",
      .args = Json::Value(Json::objectValue),
      .call_depth = 0,
  };

  for (auto _ : state) {
    auto out = plinth::capabilities::call_capability(call, ctx);
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_Tier2_NotFound);

// benchmark::benchmark_main provides main() — see CMakeLists.txt.
