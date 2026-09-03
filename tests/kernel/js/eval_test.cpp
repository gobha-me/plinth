// SPDX-License-Identifier: MIT
//
// Milestone tests for ICD-0.3.0-quickjs-vendoring §Milestone Criteria.
// Each [0.3.0] tag below maps to one of the four required exit tests.

#include <catch2/catch_test_macros.hpp>

#include <json/value.h>

#include "kernel/js/eval.hpp"

using plinth::js::eval;
using plinth::js::EvalErrorKind;

// [0.3.0] Test 1 — simple eval returns a number.
TEST_CASE("eval returns integer result of 1 + 1", "[js][eval]") {
  auto r = eval("1 + 1");
  REQUIRE(r.has_value());
  REQUIRE(r->isIntegral());
  REQUIRE(r->asInt() == 2);
}

// Extra smoke coverage for basic JSON shapes the converter supports.
TEST_CASE("eval converts strings, bools, arrays, objects", "[js][eval]") {
  SECTION("string literal") {
    auto r = eval(R"("hi")");
    REQUIRE(r.has_value());
    REQUIRE(r->asString() == "hi");
  }
  SECTION("boolean") {
    auto r = eval("true");
    REQUIRE(r.has_value());
    REQUIRE(r->asBool() == true);
  }
  SECTION("array") {
    auto r = eval("[1, 2, 3]");
    REQUIRE(r.has_value());
    REQUIRE(r->isArray());
    REQUIRE(r->size() == 3);
    REQUIRE((*r)[0].asInt() == 1);
    REQUIRE((*r)[2].asInt() == 3);
  }
  SECTION("object") {
    auto r = eval(R"(({a: 1, b: "two"}))");
    REQUIRE(r.has_value());
    REQUIRE(r->isObject());
    REQUIRE((*r)["a"].asInt() == 1);
    REQUIRE((*r)["b"].asString() == "two");
  }
}

// [0.3.0] Test 2 — syntax error surfaces cleanly; no crash, no leak.
TEST_CASE("eval reports SYNTAX_ERROR on parse failure", "[js][eval]") {
  auto r = eval("function (");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == EvalErrorKind::SYNTAX_ERROR);
  REQUIRE_FALSE(r.error().message.empty());
}

// [0.3.0] Test 3 — 16 MiB memory ceiling is enforced within bounded time.
// QuickJS's out-of-memory path throws InternalError("out of memory"); the
// eval classifier maps that to MEMORY_LIMIT. The allocation loop is
// unbounded in JS source terms but terminates quickly once the heap is
// exhausted — there is no interrupt handler in 0.3.0, only the
// allocation-refused escape hatch (ICD §Milestone Criteria Test 3).
TEST_CASE("eval enforces hard-coded 16 MiB memory limit", "[js][eval]") {
  const char* src =
      R"js(let a=[]; while(true) a.push(new Array(100000).fill(0));)js";
  auto r = eval(src);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind == EvalErrorKind::MEMORY_LIMIT);
}

// [0.3.0] Test 4 — 1,000 sequential eval() calls, no leaks. Under
// -DPLINTH_SANITIZERS=ON this provides the ASAN/UBSan evidence required by
// the ICD; without sanitizers the loop still exercises the create/destroy
// path for fast-fail on gross correctness regressions.
TEST_CASE("eval create/destroy leak check — 1000 iterations", "[js][eval]") {
  for (int i = 0; i < 1000; ++i) {
    auto r = eval("1 + 1");
    REQUIRE(r.has_value());
    REQUIRE(r->asInt() == 2);
  }
}
