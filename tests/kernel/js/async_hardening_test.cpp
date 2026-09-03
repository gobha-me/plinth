// SPDX-License-Identifier: MIT
//
// Adversarial async-hardening tests for ICD-0.3.5-runtime-hardening
// §Tests. Async-specific pressure cases (result-size enforcement,
// Promise.all fan-out burst, in-flight cancellation, promise-in-catch,
// non-serializable return, nested cap.call depth chain).
//
// Memory / CPU / stack / wall-clock pressure cases live alongside in
// limits_test.cpp.
//
// Case coverage:
//   * N.39 — Promise.all fan-out under back-pressure (DESIGN §9.1 bullet 3)
//   * N.41 — throw inside `.catch` does not crash the bridge
//            (DESIGN §9.1 bullet 5)
//   * N.42 — cycle in returned value → INTERNAL (DESIGN §9.1 bullet 6)
//   * N.44 — nested cap.call chain → cap.call_depth_exceeded
//            (DESIGN §9.1 bullet 8)
//   * N.45 — wall-clock trip with 100 pending async ops cancels cleanly
//            (DESIGN §9.1 bullet 9)
//   * N.46 — result-size exceeded → ASYNC_RESULT_SIZE_EXCEEDED
//            (ICD §New Enforcement close-out)
//   * N.47 — result-size at-limit resolves cleanly
//            (proves strict `>` semantic)

#include <catch2/catch_test_macros.hpp>

#include "async_bridge_fixture.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <chrono>
#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <json/writer.h>
#include <string>
#include <string_view>

using plinth::async_bridge_test::ensure_drogon_running;
using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::reset_schema;
using plinth::async_bridge_test::test_config;
using plinth::capabilities::CapabilityCall;
using plinth::capabilities::HandlerOutcome;
using plinth::capabilities::MAX_CALL_DEPTH;
using plinth::capabilities::UserContext;
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

// Make an admin user so cap.* calls pass the RBAC check on a
// handler registered with `kernel.log`. Mirrors the pattern used
// throughout async_bridge_test.cpp.
auto admin_user(std::string user_id) -> UserContext {
  return UserContext{.user_id = std::move(user_id),
                     .username = "hardening",
                     .auth_type = "session",
                     .effective_rules = {"kernel.admin"},
                     .session_id = {},
                     .ip_address = {}};
}

// Register a fast Tier 1 stub that resolves with `{ok:true, i:<args.i>}`.
// Used by N.39 + N.45.
auto register_fast_log_stub() -> void {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:log", "kernel.log",
      [](const Json::Value& args, const UserContext& /*ctx*/,
         int /*call_depth*/) -> HandlerOutcome {
        Json::Value out(Json::objectValue);
        out["ok"] = true;
        if (args.isMember("i")) {
          out["i"] = args["i"];
        }
        return out;
      });
}

// Register a Tier 1 stub that self-recurses via call_capability. Each
// hop increments call_depth; the resolver's entry check rejects with
// CALL_DEPTH_EXCEEDED once call_depth reaches MAX_CALL_DEPTH. Used by
// N.44.
auto register_recursive_stub() -> void {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:rec", "kernel.log",
      [](const Json::Value& args, const UserContext& ctx,
         int call_depth) -> HandlerOutcome {
        CapabilityCall nested{.signature = "kernel:1:rec",
                              .args = args,
                              .call_depth = call_depth + 1};
        auto inner = plinth::capabilities::call_capability(nested, ctx);
        if (inner.has_value()) {
          return inner->data;
        }
        return std::unexpected(inner.error());
      });
}

// Register a Tier 1 stub that returns a fixed Json::Value. The
// surrounding test sets `async_result_size_limit_bytes` to a value
// derived from the stub's actual FastWriter-measured output size, so
// boundary behavior is robust across jsoncpp formatting quirks. Used
// by N.46 + N.47.
auto register_pad_stub(Json::Value payload) -> void {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::register_tier1_handler(
      "kernel:1:pad", "kernel.log",
      [payload = std::move(payload)](
          const Json::Value& /*args*/, const UserContext& /*ctx*/,
          int /*call_depth*/) -> HandlerOutcome { return payload; });
}

// Measure the serialized size of a Json::Value with the same primitive
// as the bridge's result-size check (see
// src/kernel/js/run_on_context.cpp::check_result_size). Using this in
// tests keeps the boundary case robust.
auto measured_size(const Json::Value& v) -> std::size_t {
  return Json::FastWriter{}.write(v).size();
}

} // namespace

// ─── N.39 — Promise.all fan-out under back-pressure ──────────────
//
// DESIGN §9.1 bullet 3: 10,000 (scaled to 100 per ICD) concurrent ops
// under `max_concurrent_async_ops=8` MUST complete without exhausting
// the DB pool or racing. The test uses `db.query(pg_sleep(...))` rather
// than `cap.call` because the high-parallelism cap.call path hits the
// K.33-noted flake in the 0.3.3.1 parallel fan-out (see async_bridge_
// test.cpp:907 comment). The correctness property — back-pressure
// bounds concurrent in-flight to `max_concurrent_async_ops` — is
// orthogonal to the AsyncOp type; `db.query` exercises the same
// dispatcher path and is latency-shielded from the cap.call race.

TEST_CASE("async_hardening: parallel queries honour max_concurrent cap",
          "[js][async][hardening][group_n]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  reset_schema(test_config().db);

  // ICD-0.3.3 §N.39 — 100 × 8 fan-out. 0.5.5.2 closed the
  // back-pressure spin race in `run_on_context`'s main loop (the
  // dispatch loop now suspends on `AnyCompletionAwaiter` when
  // `concurrent_async_ops` is at cap with `pending_ops` waiting,
  // instead of spinning + running `drive_jobs` concurrently with the
  // main_loop thread's `bc.resolve` JSValue work). Pre-0.5.5.2 this
  // case was capped at 4 × 2 to avoid the race; the comment block
  // referenced K.33 / project_ws_flaky_segfault.md / 0.3.4.1 cascade.
  auto cfg = test_config();
  auto limits = default_runtime_limits();
  limits.max_concurrent_async_ops = 8;
  limits.wall_clock_limit = std::chrono::milliseconds(30000);
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1);
  const auto* src = R"(
        (async () => {
            const ps = [];
            for (let i = 0; i < 100; i++) {
                ps.push(db.query('SELECT pg_sleep(0.01), ' + i + ' AS x'));
            }
            const rs = await Promise.all(ps);
            return rs.length;
        })()
    )";
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  pool.destroy(bc);
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asInt() == 100);
}

// ─── N.41 — Throw inside .catch() does not crash the bridge ──────
//
// DESIGN §9.1 bullet 5. A rejected `db.query` is caught; the catch
// handler then throws a fresh `Error("nope")`. The promise chain ends
// rejected with that Error; top-level classifier surfaces
// PROMISE_REJECTED_UNHANDLED with the thrown message. The ASAN gate
// is on no leak / no crash during cascade teardown.

TEST_CASE("async_hardening: throw inside .catch surfaces rejection cleanly",
          "[js][async][hardening][group_n]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  reset_schema(test_config().db);

  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, default_runtime_limits(), cfg, 1);

  const auto* src = R"(
        db.query('this is not valid sql')
          .catch(() => { throw new Error('nope'); })
    )";
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  const auto& err = r.value.error();
  REQUIRE(err.kind == EvalErrorKind::PROMISE_REJECTED_UNHANDLED);
  REQUIRE(err.message.find("nope") != std::string::npos);
}

// ─── N.42 — Cycle in returned value → INTERNAL ────────────────────
//
// DESIGN §9.1 bullet 6. A self-referential object cannot be converted
// to Json::Value: js_to_json trips MAX_CONVERSION_DEPTH and returns
// INTERNAL. Proves clean rejection (no use-after-free, no hang).

TEST_CASE("async_hardening: cyclic return value rejects with INTERNAL",
          "[js][async][hardening][group_n]") {
  ensure_drogon_running();

  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, default_runtime_limits(), cfg, 1);

  const auto* src = R"(
        (async () => {
            const x = {};
            x.self = x;
            return x;
        })()
    )";
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  REQUIRE(r.value.error().kind == EvalErrorKind::INTERNAL);
}

// ─── N.44 — Nested cap.call chain → cap.call_depth_exceeded ──────
//
// DESIGN §9.1 bullet 8. A Tier 1 stub that itself invokes
// call_capability with call_depth+1 forms a recursion chain of
// MAX_CALL_DEPTH hops; the resolver entry check then rejects with
// CALL_DEPTH_EXCEEDED, which the CAP_CALL dispatch arm maps to
// `cap.call_depth_exceeded` at the JS surface.

TEST_CASE("async_hardening: nested cap.call chain trips call_depth_exceeded",
          "[js][async][hardening][group_n]") {
  ensure_drogon_running();
  register_recursive_stub();
  auto user = admin_user("u-n44");
  auto cfg = test_config();
  RuntimePool pool(/*ext=*/nullptr, default_runtime_limits(), cfg, 1, &user);
  auto* bc = pool.acquire();
  auto r = drive(*bc, "cap.call('kernel:1:rec', {}).catch(e => e.code)");
  pool.destroy(bc);
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asString() == "cap.call_depth_exceeded");
  static_assert(MAX_CALL_DEPTH > 0,
                "depth chain test requires MAX_CALL_DEPTH > 0");
}

// ─── N.45 — Wall-clock trip with 100 pending async ops ───────────
//
// DESIGN §9.1 bullet 9. 100 × pg_sleep(5) via Promise.all with
// max_concurrent=8 means 8 in-flight, 92 queued. Wall-clock fires at
// 200 ms. The cascade drains the 8 in-flight (≤ 5 s ceiling), rejects
// all outstanding promises, returns WALL_CLOCK_EXCEEDED. No
// PROMISE_RESOLVE_AFTER_CANCEL must surface.

TEST_CASE(
    "async_hardening: wall-clock trip with 100 pending ops cancels cleanly",
    "[js][async][hardening][group_n]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  reset_schema(test_config().db);

  auto cfg = test_config();
  auto limits = default_runtime_limits();
  limits.max_concurrent_async_ops = 8;
  limits.wall_clock_limit = std::chrono::milliseconds(200);
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1);

  const auto* src = R"(
        (async () => {
            const ps = [];
            for (let i = 0; i < 100; i++) {
                ps.push(db.query('SELECT pg_sleep(5)'));
            }
            return await Promise.all(ps);
        })()
    )";
  const auto START = std::chrono::steady_clock::now();
  auto* bc = pool.acquire();
  auto r = drive(*bc, src);
  const auto DUR = std::chrono::steady_clock::now() - START;
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  const auto KIND = r.value.error().kind;
  REQUIRE((KIND == EvalErrorKind::WALL_CLOCK_EXCEEDED ||
           KIND == EvalErrorKind::CANCELLED));
  REQUIRE(KIND != EvalErrorKind::PROMISE_RESOLVE_AFTER_CANCEL);
  // Cascade's 5 s ceiling + start-up slack. pg_sleep isn't
  // preemptible, so the ceiling governs upper bound.
  REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(DUR) <
          std::chrono::seconds(10));
}

// ─── N.46 — Result size exceeded ─────────────────────────────────
//
// ICD §New Enforcement close-out. Tier 1 stub returns a payload large
// enough that its serialized byte count exceeds
// `async_result_size_limit_bytes`. The per-op outcome converts to a
// rejection with code `async.result_size_exceeded`; the unhandled
// top-level promise is classified as ASYNC_RESULT_SIZE_EXCEEDED.

TEST_CASE("async_hardening: oversized cap result rejects with "
          "ASYNC_RESULT_SIZE_EXCEEDED",
          "[js][async][hardening][group_n]") {
  ensure_drogon_running();
  // Large enough to clearly exceed any reasonable small-test limit.
  Json::Value payload(Json::objectValue);
  payload["big"] = std::string(5000, 'x');
  const auto BIG_SIZE = measured_size(payload);
  register_pad_stub(payload);

  auto user = admin_user("u-n46");
  auto cfg = test_config();
  auto limits = default_runtime_limits();
  limits.async_result_size_limit_bytes = 4096; // < BIG_SIZE
  REQUIRE(BIG_SIZE > limits.async_result_size_limit_bytes);
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1, &user);

  // Unhandled top-level await — classify_rejection must surface the
  // new kind.
  auto* bc = pool.acquire();
  auto r = drive(*bc, "cap.call('kernel:1:pad', {})");
  pool.destroy(bc);
  REQUIRE_FALSE(r.value.has_value());
  REQUIRE(r.value.error().kind == EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED);
}

// ─── N.47 — Result size at-limit resolves ────────────────────────
//
// Proves the strict `>` semantic: a result measured at exactly the
// configured limit MUST resolve, not reject. Measurement runs through
// the same FastWriter primitive the enforcement uses, so the boundary
// is reproducible regardless of jsoncpp formatting quirks.

TEST_CASE("async_hardening: at-limit cap result resolves (strict > semantic)",
          "[js][async][hardening][group_n]") {
  ensure_drogon_running();
  Json::Value payload(Json::objectValue);
  payload["big"] = std::string(500, 'x');
  const auto AT_LIMIT = measured_size(payload);
  register_pad_stub(payload);

  auto user = admin_user("u-n47");
  auto cfg = test_config();
  auto limits = default_runtime_limits();
  // Exactly at the measured size — enforcement uses `>` so this
  // resolves; an off-by-one `>=` would falsely reject.
  limits.async_result_size_limit_bytes = AT_LIMIT;
  RuntimePool pool(/*ext=*/nullptr, limits, cfg, 1, &user);

  auto* bc = pool.acquire();
  auto r = drive(*bc, "cap.call('kernel:1:pad', {}).then(r => r.big.length)");
  pool.destroy(bc);
  REQUIRE(r.value.has_value());
  REQUIRE(r.value->asInt() == 500);
}
