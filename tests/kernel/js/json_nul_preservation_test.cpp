// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <chrono>
#include <json/value.h>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace {

constexpr std::string_view SOURCE =
    R"(({['A\u0000B']: ['V\u0000W'], A: 'other'}))";

auto expected_value() -> Json::Value {
  Json::Value expected(Json::objectValue);
  Json::Value nested(Json::arrayValue);
  nested.append(std::string{"V\0W", 3});
  expected[std::string{"A\0B", 3}] = std::move(nested);
  expected["A"] = "other";
  return expected;
}

} // namespace

TEST_CASE("QuickJS JSON conversion preserves embedded NUL value and key bytes",
          "[js][conversion][nul-regression]") {
  const auto expected = expected_value();

  SECTION("one-shot conversion") {
    auto result = plinth::js::eval(SOURCE);
    REQUIRE(result.has_value());
    REQUIRE(*result == expected);
  }

  SECTION("pooled synchronous conversion") {
    plinth::Config config{};
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    auto* context = pool.acquire();
    REQUIRE(context != nullptr);
    auto result = plinth::js::eval_on_context(*context, SOURCE);
    pool.destroy(context);
    REQUIRE(pool.shutdown(2s));
    REQUIRE(result.has_value());
    REQUIRE(*result == expected);
  }

  SECTION("shared conversion") {
    plinth::Config config{};
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    auto* context = pool.acquire();
    REQUIRE(context != nullptr);
    JSValue value = JS_Eval(context->ctx, SOURCE.data(), SOURCE.size(),
                            "<json-nul-regression>", JS_EVAL_TYPE_GLOBAL);
    const bool evaluated = !JS_IsException(value);
    auto result =
        evaluated ? plinth::js::detail::js_to_json(context->ctx, value)
                  : plinth::js::detail::js_to_json(context->ctx, JS_UNDEFINED);
    JS_FreeValue(context->ctx, value);
    pool.destroy(context);
    REQUIRE(pool.shutdown(2s));
    REQUIRE(evaluated);
    REQUIRE(result.has_value());
    REQUIRE(*result == expected);
  }
}
