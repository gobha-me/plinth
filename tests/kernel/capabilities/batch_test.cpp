#include <catch2/catch_test_macros.hpp>

#include "kernel/capabilities/batch.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp" // error_code()

#include <atomic>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Unit tests for plinth::capabilities::batch_call_capability — pure,
// no PG required. Each test re-uses the resolver fixture pattern from
// resolution_test.cpp (clear_resolver_for_test + register_tier1_handler
// + default_ctx helpers). Coverage maps 1:1 to ICD-0.2.2 §cap.batch()
// Behavioral Contract:
//   * Empty → success with empty vector (Promise.all([]) parity)
//   * Order-preserving fan-out
//   * Mixed-tier fan-out
//   * Fail-fast on parse / RBAC / disabled errors; priors discarded
//   * Shared call_depth across the batch
//   * Thread-safety smoke

using plinth::capabilities::batch_call_capability;
using plinth::capabilities::BatchResult;
using plinth::capabilities::CachedCapability;
using plinth::capabilities::CapabilityCall;
using plinth::capabilities::CapabilityError;
using plinth::capabilities::CapabilityHandler;
using plinth::capabilities::CapabilityResult;
using plinth::capabilities::HandlerOutcome;
using plinth::capabilities::MAX_CALL_DEPTH;
using plinth::capabilities::UserContext;

namespace {

auto fresh_resolver() -> void {
  plinth::capabilities::clear_resolver_for_test();
}

// Counting-echo handler: captures how many times it was invoked plus
// the last observed call_depth, so fail-fast tests can assert that
// post-error elements were *not* dispatched.
struct CallCounter {
  std::atomic<int> invocations{0};
  std::atomic<int> last_depth{-1};
};

auto make_counting_handler(std::string tag,
                           std::shared_ptr<CallCounter> counter)
    -> CapabilityHandler {
  return [tag = std::move(tag), counter = std::move(counter)](
             const Json::Value& args, const UserContext& /*ctx*/,
             int call_depth) -> HandlerOutcome {
    counter->invocations.fetch_add(1);
    counter->last_depth.store(call_depth);
    Json::Value payload(Json::objectValue);
    payload["tag"] = tag;
    payload["args"] = args;
    payload["call_depth"] = call_depth;
    return payload;
  };
}

auto default_ctx() -> UserContext {
  return UserContext{.user_id = "11111111-1111-1111-1111-111111111111",
                     .username = "alice",
                     .auth_type = "session",
                     .effective_rules = {"kernel.admin"},
                     .session_id = {},
                     .ip_address = {}};
}

auto make_instance_entry(std::string signature, std::string provider_type,
                         bool enabled = true) -> CachedCapability {
  bool is_kernel = provider_type == "kernel";
  return CachedCapability{
      .signature = std::move(signature),
      .provider_type = std::move(provider_type),
      .extension_name = is_kernel ? "" : "ext_example",
      .scope = "instance",
      .user_id = {},
      .rbac_rule = "kernel.example",
      .enabled = enabled,
  };
}

} // namespace

TEST_CASE("Empty batch resolves to an empty result vector",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto out = batch_call_capability({}, default_ctx());
  REQUIRE(out.values.has_value());
  REQUIRE_FALSE(out.error.has_value());
  REQUIRE(out.values->empty());
}

TEST_CASE("Batch preserves input order across a successful fan-out",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto c_a = std::make_shared<CallCounter>();
  auto c_b = std::make_shared<CallCounter>();
  auto c_c = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:alpha", "kernel.example", make_counting_handler("A", c_a));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:beta", "kernel.example", make_counting_handler("B", c_b));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:gamma", "kernel.example", make_counting_handler("C", c_c));

  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:alpha"},
      CapabilityCall{.signature = "kernel:1:beta"},
      CapabilityCall{.signature = "kernel:1:gamma"},
  };

  auto out = batch_call_capability(std::move(calls), default_ctx());
  REQUIRE(out.values.has_value());
  REQUIRE(out.values->size() == 3);
  REQUIRE((*out.values)[0].data["tag"].asString() == "A");
  REQUIRE((*out.values)[1].data["tag"].asString() == "B");
  REQUIRE((*out.values)[2].data["tag"].asString() == "C");
  REQUIRE(c_a->invocations.load() == 1);
  REQUIRE(c_b->invocations.load() == 1);
  REQUIRE(c_c->invocations.load() == 1);
}

TEST_CASE("Batch walks Tier 1 and Tier 2 entries in the same pipeline",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto ctr = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:alpha", "kernel.example", make_counting_handler("A", ctr));
  // Sidecar provider → tier3_not_available; this test cares about the
  // *error mode being consistent with the standard resolver*, not
  // about successful Tier 2 dispatch (which needs the JS bridge).
  // So we assert that a mixed Tier 1 + Tier 2 batch fails exactly
  // where the Tier 2 entry is, with the expected tier3 error.
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("llm:1:complete", "sidecar"));

  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:alpha"},
      CapabilityCall{.signature = "llm:1:complete"},
  };

  auto out = batch_call_capability(std::move(calls), default_ctx());
  REQUIRE_FALSE(out.values.has_value());
  REQUIRE(out.error.has_value());
  REQUIRE(plinth::capabilities::error_code(*out.error) ==
          "tier3_not_available");
  REQUIRE(out.failed_index == 1);
  // The Tier 1 element ran (as a prior) before the batch aborted.
  REQUIRE(ctr->invocations.load() == 1);
}

TEST_CASE("Batch aborts on first parse error; later elements never dispatch",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto c_pre = std::make_shared<CallCounter>();
  auto c_post = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:pre", "kernel.example", make_counting_handler("pre", c_pre));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:post", "kernel.example", make_counting_handler("post", c_post));

  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:pre"},
      CapabilityCall{.signature = "not-a-valid-sig"},
      CapabilityCall{.signature = "kernel:1:post"},
  };

  auto out = batch_call_capability(std::move(calls), default_ctx());
  REQUIRE_FALSE(out.values.has_value());
  REQUIRE(out.error.has_value());
  REQUIRE(plinth::capabilities::error_code(*out.error) == "invalid_capability");
  REQUIRE(out.failed_index == 1);
  // Prior element ran; post-error element did not.
  REQUIRE(c_pre->invocations.load() == 1);
  REQUIRE(c_post->invocations.load() == 0);
}

TEST_CASE("Batch aborts on RBAC denial; later elements never dispatch",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto c_ok = std::make_shared<CallCounter>();
  auto c_after = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:ok", "kernel.example", make_counting_handler("ok", c_ok));
  // Handler guarded by a rule the context does not hold.
  plinth::capabilities::register_tier1_handler(
      "kernel:1:locked", "kernel.secret",
      make_counting_handler("locked", std::make_shared<CallCounter>()));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:after", "kernel.example",
      make_counting_handler("after", c_after));

  // Context holds kernel.example but NOT kernel.secret or kernel.admin.
  auto ctx = default_ctx();
  ctx.effective_rules = {"kernel.example"};

  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:ok"},
      CapabilityCall{.signature = "kernel:1:locked"},
      CapabilityCall{.signature = "kernel:1:after"},
  };

  auto out = batch_call_capability(std::move(calls), ctx);
  REQUIRE_FALSE(out.values.has_value());
  REQUIRE(out.error.has_value());
  REQUIRE(plinth::capabilities::error_code(*out.error) == "permission_denied");
  REQUIRE(out.failed_index == 1);
  REQUIRE(c_ok->invocations.load() == 1);
  REQUIRE(c_after->invocations.load() == 0);
}

TEST_CASE("Batch aborts on a disabled capability",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto ctr = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:ok", "kernel.example", make_counting_handler("ok", ctr));
  plinth::capabilities::seed_tier2_cache_for_test(
      make_instance_entry("terminal:1:shell", "extension",
                          /*enabled=*/false));

  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:ok"},
      CapabilityCall{.signature = "terminal:1:shell"},
  };

  auto out = batch_call_capability(std::move(calls), default_ctx());
  REQUIRE_FALSE(out.values.has_value());
  REQUIRE(out.error.has_value());
  REQUIRE(plinth::capabilities::error_code(*out.error) ==
          "capability_disabled");
  REQUIRE(out.failed_index == 1);
}

TEST_CASE("Batch normalizes call_depth across elements",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto c_a = std::make_shared<CallCounter>();
  auto c_b = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:alpha", "kernel.example", make_counting_handler("A", c_a));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:beta", "kernel.example", make_counting_handler("B", c_b));

  // Caller sets calls[0].call_depth = 5; calls[1] is left at 0 to
  // prove that batch normalizes *both* elements to the same shared
  // depth (matching the ICD's "one hop from the caller" semantics).
  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:alpha", .call_depth = 5},
      CapabilityCall{.signature = "kernel:1:beta", .call_depth = 0},
  };

  auto out = batch_call_capability(std::move(calls), default_ctx());
  REQUIRE(out.values.has_value());
  // dispatch_tier1 increments depth once at the handler boundary, so
  // both handlers should observe shared_depth + 1 = 6.
  REQUIRE(c_a->last_depth.load() == 6);
  REQUIRE(c_b->last_depth.load() == 6);
}

TEST_CASE("Batch propagates call_depth_exceeded from the shared depth",
          "[capabilities][batch][unit]") {
  fresh_resolver();
  auto ctr = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:deep", "kernel.example", make_counting_handler("deep", ctr));

  std::vector<CapabilityCall> calls = {
      CapabilityCall{.signature = "kernel:1:deep",
                     .call_depth = MAX_CALL_DEPTH},
  };

  auto out = batch_call_capability(std::move(calls), default_ctx());
  REQUIRE_FALSE(out.values.has_value());
  REQUIRE(out.error.has_value());
  REQUIRE(plinth::capabilities::error_code(*out.error) ==
          "call_depth_exceeded");
  REQUIRE(out.failed_index == 0);
  REQUIRE(ctr->invocations.load() == 0);
}

TEST_CASE("Concurrent batches do not race on resolver state",
          "[capabilities][batch][unit][thread]") {
  // Thread-safety smoke. Under -DPLINTH_SANITIZERS=ON (TSan/ASan)
  // this case proves that the shared_lock in call_capability holds
  // up under concurrent batch dispatch. The assertion is simply
  // "every call resolves to a successful batch"; TSan would flag
  // any data race during execution.
  fresh_resolver();
  auto ctr = std::make_shared<CallCounter>();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:alpha", "kernel.example", make_counting_handler("A", ctr));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:beta", "kernel.example", make_counting_handler("B", ctr));

  // Catch2 REQUIRE is not thread-safe, so workers accumulate
  // successes atomically and we assert from the main thread.
  constexpr int ITERATIONS = 50;
  std::atomic<int> successes{0};
  auto worker = [&] {
    for (int i = 0; i < ITERATIONS; ++i) {
      std::vector<CapabilityCall> calls = {
          CapabilityCall{.signature = "kernel:1:alpha"},
          CapabilityCall{.signature = "kernel:1:beta"},
      };
      auto out = batch_call_capability(std::move(calls), default_ctx());
      if (out.values.has_value() && out.values->size() == 2) {
        successes.fetch_add(1);
      }
    }
  };

  std::thread t1(worker);
  std::thread t2(worker);
  t1.join();
  t2.join();

  REQUIRE(successes.load() == 2 * ITERATIONS);
  REQUIRE(ctr->invocations.load() == 2 * 2 * ITERATIONS);
}

// ── ICD-0.2.6 async wrapper parity ───────────────────────────────────
//
// Three sub-cases (success / fail-fast / empty input) exercising
// batch_call_capability_async. Each runs the sync form and the async
// form against the same fixture and asserts byte-identical
// BatchResult. The async wrapper is a `co_return` pass-through (no
// suspension point), so drogon::sync_wait spinning a worker thread is
// sufficient — no Drogon event loop required.

TEST_CASE("batch_call_capability_async matches sync result",
          "[capabilities][batch][async][unit]") {
  fresh_resolver();
  auto ctr_alpha = std::make_shared<CallCounter>();
  auto ctr_beta = std::make_shared<CallCounter>();
  auto ctr_gamma = std::make_shared<CallCounter>();

  plinth::capabilities::register_tier1_handler(
      "kernel:1:alpha", "kernel.example",
      make_counting_handler("A", ctr_alpha));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:beta", "kernel.example", make_counting_handler("B", ctr_beta));
  plinth::capabilities::register_tier1_handler(
      "kernel:1:gamma", "kernel.example",
      make_counting_handler("C", ctr_gamma));

  auto compare = [](const BatchResult& s, const BatchResult& a) {
    REQUIRE(s.values.has_value() == a.values.has_value());
    REQUIRE(s.error.has_value() == a.error.has_value());
    REQUIRE(s.failed_index == a.failed_index);
    if (s.values.has_value()) {
      REQUIRE(s.values->size() == a.values->size());
      for (std::size_t i = 0; i < s.values->size(); ++i) {
        REQUIRE((*s.values)[i].data == (*a.values)[i].data);
      }
    }
    if (s.error.has_value()) {
      REQUIRE(plinth::capabilities::error_code(*s.error) ==
              plinth::capabilities::error_code(*a.error));
    }
  };

  SECTION("All three resolve") {
    std::vector<CapabilityCall> calls = {
        CapabilityCall{.signature = "kernel:1:alpha"},
        CapabilityCall{.signature = "kernel:1:beta"},
        CapabilityCall{.signature = "kernel:1:gamma"},
    };
    auto sync_r = batch_call_capability(calls, default_ctx());
    auto async_r =
        drogon::sync_wait(plinth::capabilities::batch_call_capability_async(
            calls, default_ctx()));
    compare(sync_r, async_r);
    REQUIRE(sync_r.values.has_value());
    REQUIRE(sync_r.values->size() == 3);
  }

  SECTION("Middle call fails — fail-fast with failed_index == 1") {
    std::vector<CapabilityCall> calls = {
        CapabilityCall{.signature = "kernel:1:alpha"},
        CapabilityCall{.signature = "nowhere:1:fn"}, // fail-fast trigger
        CapabilityCall{.signature = "kernel:1:gamma"},
    };
    auto sync_r = batch_call_capability(calls, default_ctx());
    auto async_r =
        drogon::sync_wait(plinth::capabilities::batch_call_capability_async(
            calls, default_ctx()));
    compare(sync_r, async_r);
    REQUIRE_FALSE(sync_r.values.has_value());
    REQUIRE(sync_r.error.has_value());
    REQUIRE(sync_r.failed_index == 1);
    REQUIRE(plinth::capabilities::error_code(*sync_r.error) ==
            "capability_not_found");
  }

  SECTION("Empty batch resolves to empty vector") {
    std::vector<CapabilityCall> calls;
    auto sync_r = batch_call_capability(calls, default_ctx());
    auto async_r =
        drogon::sync_wait(plinth::capabilities::batch_call_capability_async(
            calls, default_ctx()));
    compare(sync_r, async_r);
    REQUIRE(sync_r.values.has_value());
    REQUIRE(sync_r.values->empty());
  }
}
