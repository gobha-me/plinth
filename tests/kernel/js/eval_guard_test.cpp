// SPDX-License-Identifier: MIT
//
// ICD-0.4.1 Layer 2 — pre-`JS_Eval` GlassWorm gate. Cases G.14–G.17 from
// ICD §Test Battery cover all three JS_Eval entry points (one-shot eval,
// pooled eval_on_context, async run_on_context coroutine) and the
// happy-path "clean source still evaluates" regression check.

#include <catch2/catch_test_macros.hpp>

#include "async_bridge_fixture.hpp"
#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/eval_guard.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <string>

using plinth::Config;
using plinth::async_bridge_test::ensure_drogon_running;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::eval;
using plinth::js::eval_on_context;
using plinth::js::EvalErrorKind;
using plinth::js::EvalResult;
using plinth::js::run_on_context;
using plinth::js::RuntimePool;

namespace {

// Static initializer: disable audit emission for the eval_guard test
// suite. The gate behavior (rejection + EvalError shape) is what we're
// covering here. The audit path crosses Drogon's DbClient layer, which
// `ensure_drogon_running()` deliberately does not bring up — letting
// audit() fall through assert(dbClientsMap_.find(name) != end()) in
// debug builds. Production callers always run with log_findings=true.
const bool DISABLE_AUDIT_FOR_TESTS = [] {
  plinth::js::set_unicode_scanner_policy(/*enabled=*/true,
                                         /*threshold=*/50,
                                         /*log_findings=*/false);
  return true;
}();

// Encode one codepoint into UTF-8. Same helper as the scanner-unit
// tests; duplicated here so the two test TUs stay independent (each
// test file is compiled in isolation by Catch2's test discovery).
auto encode_utf8(std::uint32_t cp) -> std::string {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

// Build a JS source that LOOKS clean but carries 60 invisible
// variation selectors inside a comment — well over the default
// threshold of 50.
auto smuggled_source() -> std::string {
  std::string s = "// payload: ";
  for (int i = 0; i < 60; ++i) {
    s += encode_utf8(0xFE0F);
  }
  s += "\n42;\n";
  return s;
}

} // namespace

// ─── G.14 — One-shot eval rejects smuggled source ─────────────────────
TEST_CASE("eval rejects source carrying invisible Unicode payload",
          "[js][eval][unicode][group_g]") {
  auto r = eval(smuggled_source());
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == EvalErrorKind::UNICODE_SMUGGLE_DETECTED);
  // Message must cite the rule name + total count + threshold.
  REQUIRE(r.error().message.find("unicode-smuggle") != std::string::npos);
  REQUIRE(r.error().message.find("60") != std::string::npos);
  REQUIRE(r.error().message.find("threshold 50") != std::string::npos);
}

// ─── G.15 — Pooled eval_on_context rejects smuggled source ────────────
TEST_CASE("eval_on_context rejects source carrying invisible Unicode payload",
          "[js][pool][unicode][group_g]") {
  auto cfg = Config{};
  RuntimePool pool(/*ext=*/nullptr, default_runtime_limits(), cfg,
                   /*pool_size=*/1);
  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);
  auto r = eval_on_context(*bc, smuggled_source());
  pool.release(bc);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == EvalErrorKind::UNICODE_SMUGGLE_DETECTED);
  REQUIRE(r.error().message.find("<pool>") != std::string::npos);
}

// ─── G.16 — Async run_on_context rejects smuggled source ──────────────
TEST_CASE("run_on_context rejects source carrying invisible Unicode payload",
          "[js][async][unicode][group_g]") {
  ensure_drogon_running();
  auto cfg = Config{};
  RuntimePool pool(/*ext=*/nullptr, default_runtime_limits(), cfg,
                   /*pool_size=*/1);
  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);
  auto result = drogon::sync_wait(run_on_context(*bc, smuggled_source()));
  pool.release(bc);
  REQUIRE_FALSE(result.value.has_value());
  REQUIRE(result.value.error().kind == EvalErrorKind::UNICODE_SMUGGLE_DETECTED);
  REQUIRE(result.value.error().message.find("<async>") != std::string::npos);
}

// ─── G.17 — Clean source still evaluates ──────────────────────────────
TEST_CASE("eval still works on clean source after gate is wired",
          "[js][eval][unicode][group_g]") {
  auto r = eval("1 + 1");
  REQUIRE(r.has_value());
  REQUIRE(r->isIntegral());
  REQUIRE(r->asInt() == 2);
}
