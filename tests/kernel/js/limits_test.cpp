// SPDX-License-Identifier: MIT
//
// Adversarial limits tests for ICD-0.3.5-runtime-hardening §Tests.
// Memory / CPU / stack / wall-clock pressure cases. Async-specific
// pressure cases live alongside in async_hardening_test.cpp.
//
// Case coverage:
//   * N.37 — memory limit across an await (DESIGN §9.1 bullet 1)
//   * N.38 — CPU limit inside a .then() callback (DESIGN §9.1 bullet 2)
//   * N.40 — promise-allocation loop trips memory limit (DESIGN §9.1 bullet 4)
//   * N.43 — eval / Function disabled at stdlib injection (DESIGN §9.1 bullet
//   7)
//
// N.39, N.41, N.42, N.44, N.45, N.46, N.47 are async-hardening cases
// and live in async_hardening_test.cpp.

#include <catch2/catch_test_macros.hpp>

#include "async_bridge_fixture.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <chrono>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <string_view>

using plinth::async_bridge_test::ensure_drogon_running;
using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::reset_schema;
using plinth::async_bridge_test::test_config;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::EvalErrorKind;
using plinth::js::EvalResult;
using plinth::js::run_on_context;
using plinth::js::RuntimePool;

namespace {

auto drive(BridgeContext& bc, std::string_view source) -> EvalResult {
  return drogon::sync_wait(run_on_context(bc, source));
}

} // namespace

// ─── N.37 — Memory limit across await ─────────────────────────────
// DESIGN §9.1 bullet 1: "Allocate until memory limit, then await, then
// allocate more → limit still enforced." Proves the 0.3.3.3 classifier
// routes a post-await OOM through MEMORY_LIMIT even when the OOM fires
// deep in an async frame (and the async unwind reclaims memory before
// classify_rejection can read it off the runtime).

TEST_CASE("limits: memory cap survives an intervening await",
          "[js][async][limits][group_n]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  reset_schema(test_config().db);

  auto cfg = test_config();
  auto limits = default_runtime_limits();
  limits.memory_limit_bytes = 4UL * 1024UL * 1024UL; // 4 MiB
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1);

  // Pre-allocate ~3 MiB, then a single await, then continue allocating
  // until the cap trips. The post-await allocation loop is the N.37
  // property: a frame-crossing OOM must still classify as MEMORY_LIMIT.
  const auto* src = R"(
        (async () => {
            const preallocated = [];
            for (let i = 0; i < 75; i++) {
                preallocated.push(new Array(5000).fill(0));
            }
            await db.query('SELECT 1');
            const tail = [];
            for (let i = 0; i < 10000; i++) {
                tail.push(new Array(5000).fill(0));
            }
            return 'should-not-reach';
        })()
    )";
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  REQUIRE(r.value.error().kind == EvalErrorKind::MEMORY_LIMIT);
}

// ─── N.38 — CPU limit fires inside a .then() callback ─────────────
// DESIGN §9.1 bullet 2. Proves the interrupt handler does not lose its
// CPU budget while a promise callback is executing.

TEST_CASE("limits: CPU cap trips inside .then() callback",
          "[js][async][limits][group_n]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  reset_schema(test_config().db);

  auto cfg = test_config();
  auto limits = default_runtime_limits();
  limits.cpu_time_limit = std::chrono::milliseconds(50);
  limits.wall_clock_limit = std::chrono::milliseconds(5000);
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1);

  // db.query resolves on the main loop, then .then's body runs the
  // tight loop. CPU interrupt must fire — not wall-clock.
  const auto* src = R"(
        db.query('SELECT 1').then(() => { for(;;){} });
    )";
  const auto START = std::chrono::steady_clock::now();
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  const auto DUR = std::chrono::steady_clock::now() - START;
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  REQUIRE(r.value.error().kind == EvalErrorKind::CPU_TIME_EXCEEDED);
  REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(DUR) <
          std::chrono::milliseconds(500));
}

// ─── N.40 — Promise-allocation loop trips memory limit ───────────
// DESIGN §9.1 bullet 4. The promise allocation path must count against
// the JS heap. Each `new Promise(() => {})` allocates a JSObject with
// internal state — summed across 200000 iterations this vastly exceeds
// the 4 MiB cap. No await here: the OOM fires synchronously from the
// tight loop.

TEST_CASE("limits: promise-allocation loop trips MEMORY_LIMIT",
          "[js][async][limits][group_n]") {
  ensure_drogon_running();

  auto cfg = test_config();
  auto limits = default_runtime_limits();
  limits.memory_limit_bytes = 4UL * 1024UL * 1024UL; // 4 MiB
  // The tight loop is CPU-bound; give it enough budget to reach the
  // memory cap before the CPU cap (default 100 ms).
  limits.cpu_time_limit = std::chrono::milliseconds(5000);
  limits.wall_clock_limit = std::chrono::milliseconds(10000);
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1);

  const auto* src = R"(
        (() => {
            const keep = [];
            for (let i = 0; i < 200000; i++) {
                keep.push(new Promise(() => {}));
            }
            return 'should-not-reach';
        })()
    )";
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  REQUIRE(r.value.error().kind == EvalErrorKind::MEMORY_LIMIT);
}

// ─── N.43 — eval / Function disabled by default ──────────────────
// DESIGN §9.1 bullet 7. ICD-0.3.5 §Security Constraint 5 +
// §Open Question 1: `inject_kernel_stdlib` deletes `globalThis.eval`
// and `globalThis.Function` so a script source cannot construct a
// dynamic code path outside the kernel surface. Architect ratification
// of the deletion posture is flagged in the PR description.

TEST_CASE("limits: eval and Function are undefined on globalThis",
          "[js][async][limits][group_n][security]") {
  ensure_drogon_running();

  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, default_runtime_limits(), cfg, 1);
  auto* bc = pool.acquire();
  // Probe surface + call-site behavior in one expression. Expected
  // string assembly: "undefined|undefined|ReferenceError|ReferenceError".
  const auto* src = R"(
        (() => {
            const evalKind = typeof globalThis.eval;
            const fnKind   = typeof globalThis.Function;
            let evalErr = 'nothrow';
            try { eval('1+1'); }
            catch (e) { evalErr = e.name; }
            let fnErr = 'nothrow';
            try { new Function('return 1')(); }
            catch (e) { fnErr = e.name; }
            return `${evalKind}|${fnKind}|${evalErr}|${fnErr}`;
        })()
    )";
  auto r = drive(*bc, src);
  pool.destroy(bc);
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() ==
          "undefined|undefined|ReferenceError|ReferenceError");
}
