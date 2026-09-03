// SPDX-License-Identifier: MIT
//
// Milestone tests for ICD-0.3.1-runtime-lifecycle §Tests. Each [0.3.1]
// tag below maps to one of the five required exit tests.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using plinth::Config;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::eval_on_context;
using plinth::js::EvalErrorKind;
using plinth::js::RuntimeLimits;
using plinth::js::RuntimePool;

#ifdef PLINTH_JS_TEST_SHIMS
// Registered by tests/kernel/js/test_host_sleep.cpp. Installs
// globalThis.__host_sleep_ms__ on the supplied context.
auto register_host_sleep(plinth::js::BridgeContext& bc) -> void;
#endif

namespace {

// A generous limit profile suitable for tests that should NOT trip any
// guard. Derived from the kernel defaults; individual tests override
// targeted fields (cpu, memory) to exercise the limit they're testing.
auto test_limits() -> RuntimeLimits {
  return default_runtime_limits();
}

// Default-constructed Config carries sane test values (dev_mode=false,
// node_id="node-1", listen_host="0.0.0.0", listen_port=8080, etc. —
// see src/kernel/config.hpp). 0.3.2 adds a Config parameter to
// RuntimePool for the `config.get()` projection, but 0.3.1 tests don't
// care about the projection — any Config will do.
auto test_config() -> Config {
  return Config{};
}

// Minimal capturing spdlog sink — mirrors the pattern in stdlib_test.cpp.
// Kept local so this file has no dependency on a shared test-support
// header. SC4's proof path is "the warn line fires"; this captures it.
class CapturingSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  [[nodiscard]] auto contains(std::string_view needle) -> bool {
    std::lock_guard<std::mutex> g(this->mutex_);
    return std::ranges::any_of(lines, [&](const std::string& s) {
      return s.find(needle) != std::string::npos;
    });
  }

 protected:
  auto sink_it_(const spdlog::details::log_msg& m) -> void override {
    spdlog::memory_buf_t formatted;
    formatter_->format(m, formatted);
    lines.emplace_back(formatted.data(), formatted.size());
  }
  auto flush_() -> void override {}

 private:
  std::vector<std::string> lines;
};

class ScopedDefaultLogger {
 public:
  explicit ScopedDefaultLogger(std::shared_ptr<CapturingSink> s)
      : sink(std::move(s)), previous(spdlog::default_logger()) {
    auto logger = std::make_shared<spdlog::logger>("pool_test", sink);
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
  }
  ~ScopedDefaultLogger() { spdlog::set_default_logger(previous); }
  ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
  auto operator=(const ScopedDefaultLogger&) -> ScopedDefaultLogger& = delete;
  ScopedDefaultLogger(ScopedDefaultLogger&&) = delete;
  auto operator=(ScopedDefaultLogger&&) -> ScopedDefaultLogger& = delete;

 private:
  std::shared_ptr<CapturingSink> sink;
  std::shared_ptr<spdlog::logger> previous;
};

} // namespace

// [0.3.1] Test 1 — Pool acquire / release / reuse.
// Acquire N contexts, release all, acquire again; the second round
// returns contexts from the free list (pointer identity proves reuse).
TEST_CASE("RuntimePool reuses released contexts", "[js][pool]") {
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, test_limits(), cfg, /*pool_size=*/3);
  REQUIRE(pool.pool_size() == 3);
  REQUIRE(pool.free_count() == 3);
  REQUIRE(pool.active_count() == 0);

  BridgeContext* a = pool.acquire();
  BridgeContext* b = pool.acquire();
  BridgeContext* c = pool.acquire();
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  REQUIRE(c != nullptr);
  REQUIRE(pool.active_count() == 3);
  REQUIRE(pool.free_count() == 0);

  pool.release(a);
  pool.release(b);
  pool.release(c);
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 3);

  // Reuse: pointers match one of {a,b,c} — no allocation.
  BridgeContext* r1 = pool.acquire();
  BridgeContext* r2 = pool.acquire();
  BridgeContext* r3 = pool.acquire();
  const bool ALL_REUSED = (r1 == a || r1 == b || r1 == c) &&
                          (r2 == a || r2 == b || r2 == c) &&
                          (r3 == a || r3 == b || r3 == c);
  REQUIRE(ALL_REUSED);

  pool.release(r1);
  pool.release(r2);
  pool.release(r3);
  REQUIRE(pool.free_count() == 3);
}

// [0.3.1] Test 2 — Pool on-demand creation + transient destroy.
// With pool_size = 2, acquire 3 contexts; the third is created on
// demand and marked transient. On release the first two return to the
// free list and the third is destroyed.
TEST_CASE("RuntimePool on-demand grows and destroys transient", "[js][pool]") {
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, test_limits(), cfg, /*pool_size=*/2);
  REQUIRE(pool.free_count() == 2);

  BridgeContext* a = pool.acquire();
  BridgeContext* b = pool.acquire();
  REQUIRE(pool.free_count() == 0);
  REQUIRE(pool.active_count() == 2);

  // Pool is exhausted; this one is transient.
  BridgeContext* c = pool.acquire();
  REQUIRE(c != nullptr);
  REQUIRE(pool.active_count() == 3);

  // release(c) must destroy, not pool.
  pool.release(c);
  REQUIRE(pool.active_count() == 2);
  REQUIRE(pool.free_count() == 0);

  pool.release(a);
  pool.release(b);
  REQUIRE(pool.free_count() == 2);
  REQUIRE(pool.active_count() == 0);
}

// [0.3.1] Test 3 — Memory limit enforced.
// Eval JS that allocates past the memory limit. Result is
// MEMORY_LIMIT; the context is destroyed; active_count is unchanged
// on the next acquire/release cycle. The 1000-iteration construct/
// destroy check in eval_test.cpp already covers ASAN leak evidence
// for the one-shot path; this test adds pool-path coverage with a
// 50-iteration loop (the pool setup cost is larger so we keep the
// count lower to hit CI budget).
TEST_CASE("RuntimePool enforces memory limit and stays clean", "[js][pool]") {
  auto limits = test_limits();
  limits.memory_limit_bytes = 1UL * 1024UL * 1024UL; // 1 MiB
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, /*pool_size=*/2);

  for (int i = 0; i < 50; ++i) {
    BridgeContext* bc = pool.acquire();
    REQUIRE(bc != nullptr);
    auto r = eval_on_context(
        *bc, "let a=[]; while(true) a.push(new Array(100000).fill(0));");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == EvalErrorKind::MEMORY_LIMIT);
    pool.destroy(bc);
  }
  REQUIRE(pool.active_count() == 0);
}

// [0.3.1] Test 4 — CPU-time limit enforced, with independence from
// wall-clock when a shim is available. The first sub-test always runs:
// a tight infinite JS loop must terminate within the CPU budget. The
// second sub-test runs only under PLINTH_JS_TEST_SHIMS=ON — it calls a
// host shim that sleeps with the CPU timer paused, proving wall-clock
// time does NOT trip the CPU guard.
TEST_CASE("RuntimePool enforces CPU-time limit", "[js][pool]") {
  auto limits = test_limits();
  limits.cpu_time_limit = std::chrono::milliseconds{50};
  limits.wall_clock_limit = std::chrono::milliseconds{5000};
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, /*pool_size=*/1);

  SECTION("tight JS loop trips CPU limit within a bounded margin") {
    BridgeContext* bc = pool.acquire();
    const auto START = std::chrono::steady_clock::now();
    auto r = eval_on_context(*bc, "while(true){}");
    const auto DUR = std::chrono::steady_clock::now() - START;
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == EvalErrorKind::CPU_TIME_EXCEEDED);
    // 50 ms budget + interrupt latency + error-classification work.
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(DUR) <
            std::chrono::milliseconds{500});
    pool.destroy(bc);
  }

#ifdef PLINTH_JS_TEST_SHIMS
  SECTION("wall-clock-only work does not trip the CPU limit") {
    BridgeContext* bc = pool.acquire();
    register_host_sleep(*bc);
    // Sleep 120 ms — well past cpu_time_limit (50 ms), well inside
    // wall_clock_limit (5000 ms). Host shim pauses the CPU timer
    // around sleep_for.
    auto r = eval_on_context(*bc, "__host_sleep_ms__(120); 42");
    REQUIRE(r.has_value());
    REQUIRE(r->asInt() == 42);
    pool.release(bc);
  }
#endif
}

// [0.3.1] Test 5 — Stack-depth limit enforced.
// Deep recursion must terminate with STACK_OVERFLOW, without crashing
// the process, and the pool must be in a usable state afterward.
TEST_CASE("RuntimePool enforces stack-depth limit", "[js][pool]") {
  auto limits = test_limits();
  limits.max_stack_depth = 64; // 256 KiB C-stack for a tight fail
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, /*pool_size=*/1);

  BridgeContext* bc = pool.acquire();
  auto r = eval_on_context(*bc, "function f(){f()} f()");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == EvalErrorKind::STACK_OVERFLOW);
  pool.destroy(bc);

  // Pool still usable after a stack overflow.
  REQUIRE(pool.active_count() == 0);
  BridgeContext* bc2 = pool.acquire();
  REQUIRE(bc2 != nullptr);
  auto r2 = eval_on_context(*bc2, "1 + 1");
  REQUIRE(r2.has_value());
  REQUIRE(r2->asInt() == 2);
  pool.release(bc2);
}

// Ancillary — rebuild() after a hot-reload swaps out all pooled
// contexts but leaves anyone already acquired untouched.
TEST_CASE("RuntimePool::rebuild replaces pooled contexts", "[js][pool]") {
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, test_limits(), cfg, /*pool_size=*/2);
  BridgeContext* a = pool.acquire();
  pool.release(a);
  REQUIRE(pool.free_count() == 2);
  pool.rebuild();
  REQUIRE(pool.free_count() == 2);
  REQUIRE(pool.active_count() == 0);
}

// [0.3.3.3] ICD-0.3.1 §Security Constraint 4 — `release()` on a
// context whose `cancelled` flag is set routes internally to
// `destroy()`. The defensive-destroy branch at runtime_pool.cpp emits a
// warn line naming the ICD section; capturing that proves the route
// taken. Side effects (slot not returned to free_list; active_count
// drops) are asserted as the secondary proof.
TEST_CASE("RuntimePool release on cancelled context routes to destroy",
          "[js][pool][security]") {
  auto sink = std::make_shared<CapturingSink>();
  ScopedDefaultLogger guard(sink);

  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, test_limits(), cfg, /*pool_size=*/1);

  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);
  REQUIRE(pool.active_count() == 1);
  REQUIRE(pool.free_count() == 0);

  // Simulate post-execution cancellation (e.g. an external driver
  // flipped the flag). The defensive check reads cancelled BEFORE the
  // in-release reset, per runtime_pool.cpp:528–534.
  bc->cancelled.store(true, std::memory_order_release);

  pool.release(bc);

  // Primary proof: warn line naming the ICD section fired.
  REQUIRE(sink->contains("ICD-0.3.1 §Security Constraint 4"));
  REQUIRE(sink->contains("cancelled=true"));

  // Secondary proof: slot was destroyed, not returned to free list.
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 0);

  // Post-destroy the pool is still usable (next acquire re-creates
  // the slot on demand up to pool_size).
  BridgeContext* bc2 = pool.acquire();
  REQUIRE(bc2 != nullptr);
  pool.destroy(bc2);
}

// Ancillary — state reset clears globalThis own props across uses.
TEST_CASE("RuntimePool release clears globalThis own props", "[js][pool]") {
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, test_limits(), cfg, /*pool_size=*/1);

  BridgeContext* a = pool.acquire();
  auto r = eval_on_context(*a, "globalThis.x = 42; x");
  REQUIRE(r.has_value());
  REQUIRE(r->asInt() == 42);
  pool.release(a);

  BridgeContext* b = pool.acquire();
  // x was cleared on release. Accessing undefined prop throws a
  // ReferenceError → RUNTIME_ERROR (not SYNTAX_ERROR).
  auto r2 = eval_on_context(*b, "x");
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(r2.error().kind == EvalErrorKind::RUNTIME_ERROR);
  pool.release(b);
}
