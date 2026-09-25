// SPDX-License-Identifier: MIT
//
// Bounded, deterministic coverage of the shared QuickJS conversion helpers.
// The Json::Value oracle is constructed from the generated tree, never by
// parsing or stringifying the JavaScript result under test.

#include <catch2/catch_test_macros.hpp>

#include "async_bridge_fixture.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace {

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1240'0000'0000'0001ULL, 0x1240'0000'0000'0025ULL,
    0x1240'0000'0000'00C3ULL, 0x1240'0000'0000'BEEFULL};
constexpr int CASES_PER_SEED = 32;
constexpr int MAX_DEPTH = 4;
constexpr int MAX_NODES = 31;
constexpr std::size_t MAX_STRING_BYTES = 16;
constexpr int MAX_SHRINK_ATTEMPTS = 16;
constexpr auto SHUTDOWN_BOUND = 2s;
static_assert(MAX_STRING_BYTES >= 9); // longest generated UTF-8 object key

struct Tree {
  std::string source;
  Json::Value expected;
};

struct Lease {
  plinth::js::RuntimePool& pool;
  plinth::js::BridgeContext* context;

  Lease(plinth::js::RuntimePool& owner, plinth::js::BridgeContext* value)
      : pool(owner), context(value) {}

  Lease(const Lease&) = delete;
  auto operator=(const Lease&) -> Lease& = delete;
  Lease(Lease&&) = delete;
  auto operator=(Lease&&) -> Lease& = delete;

  ~Lease() {
    if (context != nullptr) {
      pool.destroy(context);
    }
  }

  auto release() -> void {
    pool.release(context);
    context = nullptr;
  }

  auto destroy() -> void {
    pool.destroy(context);
    context = nullptr;
  }
};

struct ValueOwner {
  JSContext* ctx;
  JSValue value;

  ValueOwner(JSContext* context, JSValue input) : ctx(context), value(input) {}
  ValueOwner(const ValueOwner&) = delete;
  auto operator=(const ValueOwner&) -> ValueOwner& = delete;
  ValueOwner(ValueOwner&&) = delete;
  auto operator=(ValueOwner&&) -> ValueOwner& = delete;

  ~ValueOwner() { JS_FreeValue(ctx, value); }
};

struct MatchResult {
  bool matched = false;
  std::optional<plinth::js::EvalErrorKind> error_kind;
  bool pending_exception = false;
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

auto string_atom(std::uint64_t choice) -> Tree {
  switch (choice % 5U) {
    case 0: return {R"('')", Json::Value{std::string{}}};
    case 1: return {R"('A\u0000B')", Json::Value{std::string{"A\0B", 3}}};
    case 2: return {R"('\u00e9')", Json::Value{std::string{"\xC3\xA9", 2}}};
    case 3: return {R"('\u4e2d')", Json::Value{std::string{"\xE4\xB8\xAD", 3}}};
    default:
      return {R"('\ud83d\ude00')",
              Json::Value{std::string{"\xF0\x9F\x98\x80", 4}}};
  }
}

auto tree(std::uint64_t& state, int depth, int& nodes, int node_limit) -> Tree {
  ++nodes;
  const auto choice = next(state);
  const bool terminal = depth >= MAX_DEPTH || nodes >= node_limit - 3;
  switch (choice % (terminal ? 5U : 7U)) {
    case 0: return {"null", Json::Value{Json::nullValue}};
    case 1: return {"true", Json::Value{true}};
    case 2: return {"false", Json::Value{false}};
    case 3: {
      const auto number = static_cast<int>(next(state) % 2001U) - 1000;
      if ((choice & 0x100U) == 0) {
        return {std::to_string(number), Json::Value{number}};
      }
      const auto fraction = static_cast<double>(number) / 4.0 + 0.125;
      return {std::to_string(fraction), Json::Value{fraction}};
    }
    case 4: return string_atom(choice >> 8U);
    case 5: {
      Json::Value expected{Json::arrayValue};
      std::string source{"["};
      const auto count = static_cast<int>(next(state) % 4U);
      for (int index = 0; index < count && nodes < node_limit; ++index) {
        auto child = tree(state, depth + 1, nodes, node_limit);
        if (index != 0) {
          source += ',';
        }
        source += child.source;
        expected.append(std::move(child.expected));
      }
      return {source + ']', std::move(expected)};
    }
    default: {
      Json::Value expected{Json::objectValue};
      std::string source{"({"};
      constexpr std::array<std::string_view, 3> keys{"plain", "nul\\u0000key",
                                                     "unicode\\u00e9"};
      constexpr std::array<std::string_view, 3> expected_keys{
          "plain", std::string_view{"nul\0key", 7}, "unicode\xC3\xA9"};
      const auto count = static_cast<int>(next(state) % 4U);
      for (int index = 0; index < count && nodes < node_limit; ++index) {
        auto child = tree(state, depth + 1, nodes, node_limit);
        if (index != 0) {
          source += ',';
        }
        const auto slot = static_cast<std::size_t>(index);
        source += "['" + std::string{keys[slot]} + "']:" + child.source;
        expected[std::string{expected_keys[slot]}] = std::move(child.expected);
      }
      return {source + "})", std::move(expected)};
    }
  }
}

auto evaluate(JSContext* ctx, std::string_view source) -> JSValue {
  return JS_Eval(ctx, source.data(), source.size(), "<conversion-corpus>",
                 JS_EVAL_TYPE_GLOBAL);
}

auto matches(JSContext* ctx, const Tree& item) -> MatchResult {
  JSValue value = evaluate(ctx, item.source);
  if (JS_IsException(value)) {
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, JS_GetException(ctx));
    return {.matched = false,
            .error_kind = plinth::js::EvalErrorKind::SYNTAX_ERROR,
            .pending_exception = true};
  }
  auto converted = plinth::js::detail::js_to_json(ctx, value);
  JS_FreeValue(ctx, value);
  const bool pending = JS_HasException(ctx);
  if (pending) {
    JS_FreeValue(ctx, JS_GetException(ctx));
  }
  return {.matched =
              converted.has_value() && *converted == item.expected && !pending,
          .error_kind = converted.has_value()
                            ? std::nullopt
                            : std::optional{converted.error().kind},
          .pending_exception = pending};
}

auto minimal_failure(JSContext* ctx, std::uint64_t seed, int index)
    -> std::string {
  // Reduce the generation budget on a mismatch. This diagnostic path is
  // bounded and does not alter the primary fixed-seed oracle.
  std::string smallest;
  for (int attempt = 0; attempt < MAX_SHRINK_ATTEMPTS; ++attempt) {
    auto state = seed ^ static_cast<std::uint64_t>(index);
    int nodes = 0;
    auto item = tree(state, 0, nodes, MAX_NODES - attempt);
    if (!matches(ctx, item).matched &&
        (smallest.empty() || item.source.size() < smallest.size())) {
      smallest = std::move(item.source);
    }
  }
  return smallest;
}

auto fresh_pool() -> plinth::js::RuntimePool {
  return {nullptr, plinth::js::default_runtime_limits(),
          plinth::async_bridge_test::test_config(), 1};
}

} // namespace

TEST_CASE("shared QuickJS conversion fixed-seed JSON corpus",
          "[js][conversion][corpus]") {
  auto pool = fresh_pool();
  for (const auto seed : SEEDS) {
    for (int index = 0; index < CASES_PER_SEED; ++index) {
      auto state = seed ^ static_cast<std::uint64_t>(index);
      int nodes = 0;
      auto item = tree(state, 0, nodes, MAX_NODES);
      REQUIRE(nodes <= MAX_NODES);
      REQUIRE(item.source.size() <= 1024);
      Lease lease{pool, pool.acquire()};
      auto* bc = lease.context;
      REQUIRE(bc != nullptr);
      const auto result = matches(bc->ctx, item);
      const auto minimized = result.matched
                                 ? std::string{}
                                 : minimal_failure(bc->ctx, seed, index);
      lease.release();
      INFO("seed=" << seed << " case=" << index << " source=" << item.source
                   << " error_kind="
                   << (result.error_kind ? static_cast<int>(*result.error_kind)
                                         : -1)
                   << " pending=" << result.pending_exception
                   << " minimized=" << minimized);
      REQUIRE(result.matched);
    }
    pool.rebuild();
  }
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

TEST_CASE("shared QuickJS conversion boundary and failure ownership",
          "[js][conversion][corpus]") {
  auto pool = fresh_pool();
  Lease lease{pool, pool.acquire()};
  auto* bc = lease.context;
  REQUIRE(bc != nullptr);

  // Root is depth zero: a leaf at depth 64 is accepted, at 65 rejected.
  for (const int depth : {64, 65}) {
    std::string source(static_cast<std::size_t>(depth), '[');
    source += '1';
    source.append(static_cast<std::size_t>(depth), ']');
    ValueOwner value{bc->ctx, evaluate(bc->ctx, source)};
    REQUIRE_FALSE(JS_IsException(value.value));
    auto converted = plinth::js::detail::js_to_json(bc->ctx, value.value);
    REQUIRE(converted.has_value() == (depth == 64));
    if (!converted) {
      REQUIRE(converted.error().kind == plinth::js::EvalErrorKind::INTERNAL);
    }
  }

  {
    ValueOwner sparse{bc->ctx, evaluate(bc->ctx, "[undefined,,null]")};
    REQUIRE_FALSE(JS_IsException(sparse.value));
    auto sparse_json = plinth::js::detail::js_to_json(bc->ctx, sparse.value);
    Json::Value expected_sparse{Json::arrayValue};
    expected_sparse.append(Json::Value{Json::nullValue});
    expected_sparse.append(Json::Value{Json::nullValue});
    expected_sparse.append(Json::Value{Json::nullValue});
    REQUIRE(sparse_json.has_value());
    REQUIRE(*sparse_json == expected_sparse);
  }

  constexpr std::array<std::string_view, 5> failures{
      "(() => { const a=[]; a.push(a); return a; })()",
      "({get x(){throw Error('getter')}})",
      "new Proxy({}, {ownKeys(){throw Error('keys')}})",
      "Symbol('unsupported')", "1n"};
  for (const auto source : failures) {
    ValueOwner value{bc->ctx, evaluate(bc->ctx, source)};
    REQUIRE_FALSE(JS_IsException(value.value));
    ValueOwner duplicate{bc->ctx, JS_DupValue(bc->ctx, value.value)};
    auto converted = plinth::js::detail::js_to_json(bc->ctx, value.value);
    REQUIRE_FALSE(converted.has_value());
    REQUIRE(converted.error().kind == plinth::js::EvalErrorKind::INTERNAL);
    REQUIRE_FALSE(JS_IsException(duplicate.value));
    if (JS_HasException(bc->ctx)) {
      JS_FreeValue(bc->ctx, JS_GetException(bc->ctx));
    }
  }
  lease.destroy();
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

TEST_CASE("shared QuickJS pending and rejection classification table",
          "[js][conversion][corpus]") {
  using plinth::js::EvalErrorKind;
  struct Row {
    std::string_view source;
    EvalErrorKind expected;
  };
  constexpr std::array<Row, 5> pending{{
      {"(()", EvalErrorKind::SYNTAX_ERROR},
      {"throw new SyntaxError('bad')", EvalErrorKind::SYNTAX_ERROR},
      {"throw new Error('bad')", EvalErrorKind::RUNTIME_ERROR},
      {"throw new RangeError('call stack size exceeded')",
       EvalErrorKind::STACK_OVERFLOW},
      {"throw new InternalError('out of memory')", EvalErrorKind::MEMORY_LIMIT},
  }};
  constexpr std::array<Row, 5> rejected{{
      {"new Error('bad')", EvalErrorKind::PROMISE_REJECTED_UNHANDLED},
      {"new RangeError('call stack size exceeded')",
       EvalErrorKind::STACK_OVERFLOW},
      {"new InternalError('out of memory')", EvalErrorKind::MEMORY_LIMIT},
      {"({code:'async.result_size_exceeded',message:'large'})",
       EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED},
      {"'ordinary reason'", EvalErrorKind::PROMISE_REJECTED_UNHANDLED},
  }};
  auto pool = fresh_pool();
  Lease lease{pool, pool.acquire()};
  auto* bc = lease.context;
  REQUIRE(bc != nullptr);
  for (const auto& row : pending) {
    ValueOwner value{bc->ctx, evaluate(bc->ctx, row.source)};
    REQUIRE(JS_IsException(value.value));
    const auto error = plinth::js::detail::extract_error(bc->ctx, *bc);
    INFO("pending source=" << row.source);
    REQUIRE(error.kind == row.expected);
    REQUIRE_FALSE(JS_HasException(bc->ctx));
  }
  for (const auto& row : rejected) {
    ValueOwner reason{bc->ctx, evaluate(bc->ctx, row.source)};
    REQUIRE_FALSE(JS_IsException(reason.value));
    ValueOwner duplicate{bc->ctx, JS_DupValue(bc->ctx, reason.value)};
    const auto error =
        plinth::js::detail::classify_rejection(bc->ctx, reason.value, *bc);
    INFO("rejection source=" << row.source);
    REQUIRE(error.kind == row.expected);
    REQUIRE_FALSE(JS_IsException(duplicate.value));
  }
  // The latch and interrupt precedence are deliberately independent of
  // exception names. Set time points in the past; no timing sleeps.
  bc->execution_start = std::chrono::steady_clock::now() - 2s;
  bc->wall_clock_limit = 1s;
  bc->cpu_time_accumulated = 2s;
  bc->cpu_time_limit = 1s;
  {
    ValueOwner reason{bc->ctx, evaluate(bc->ctx, "new Error('bad')")};
    REQUIRE_FALSE(JS_IsException(reason.value));
    const auto pending_kind = [&] {
      JSValue thrown = evaluate(bc->ctx, "throw new Error('bad')");
      const bool is_exception = JS_IsException(thrown);
      JS_FreeValue(bc->ctx, thrown);
      if (!is_exception) {
        return EvalErrorKind::INTERNAL;
      }
      return plinth::js::detail::extract_error(bc->ctx, *bc).kind;
    };
    REQUIRE(plinth::js::detail::classify_rejection(bc->ctx, reason.value, *bc)
                .kind == EvalErrorKind::CPU_TIME_EXCEEDED);
    REQUIRE(pending_kind() == EvalErrorKind::CPU_TIME_EXCEEDED);
    bc->cancelled.store(true);
    REQUIRE(plinth::js::detail::classify_rejection(bc->ctx, reason.value, *bc)
                .kind == EvalErrorKind::CANCELLED);
    REQUIRE(pending_kind() == EvalErrorKind::CANCELLED);
    bc->cancelled.store(false);
    bc->cpu_time_accumulated = 0ns;
    REQUIRE(plinth::js::detail::classify_rejection(bc->ctx, reason.value, *bc)
                .kind == EvalErrorKind::WALL_CLOCK_EXCEEDED);
    REQUIRE(pending_kind() == EvalErrorKind::WALL_CLOCK_EXCEEDED);
  }
  bc->execution_start = {};
  bc->memory_limit_hit.store(true);
  {
    ValueOwner other{bc->ctx, evaluate(bc->ctx, "'unclassified'")};
    REQUIRE_FALSE(JS_IsException(other.value));
    REQUIRE(plinth::js::detail::classify_rejection(bc->ctx, other.value, *bc)
                .kind == EvalErrorKind::MEMORY_LIMIT);
  }
  bc->memory_limit_hit.store(false);
  plinth::js::detail::sample_memory_peak(*bc);
  REQUIRE_FALSE(bc->memory_limit_hit.load());
  JSMemoryUsage stats{};
  JS_ComputeMemoryUsage(bc->rt, &stats);
  const auto original_limit = static_cast<std::size_t>(stats.malloc_limit);
  JS_SetMemoryLimit(bc->rt, static_cast<std::size_t>(stats.malloc_size) +
                                512ULL * 1024ULL);
  plinth::js::detail::sample_memory_peak(*bc);
  JS_SetMemoryLimit(bc->rt, original_limit);
  REQUIRE(bc->memory_limit_hit.load());
  lease.destroy();
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  plinth::js::BridgeContext no_runtime{};
  plinth::js::detail::sample_memory_peak(no_runtime);
  REQUIRE_FALSE(no_runtime.memory_limit_hit.load());
}

TEST_CASE("shared QuickJS conversion survives pool and async boundaries",
          "[js][conversion][corpus]") {
  auto pool = fresh_pool();
  // The named corpus is DB-free. In the grouped [js] CTest with PostgreSQL
  // available, start the same DB-capable singleton fixture used by the later
  // PG-backed async cases so this test cannot freeze it in no-DB mode first.
  if (plinth::async_bridge_test::pg_available()) {
    plinth::async_bridge_test::ensure_drogon_with_db_running();
  } else {
    plinth::async_bridge_test::ensure_drogon_running();
  }
  for (int repetition = 0; repetition < 8; ++repetition) {
    Lease lease{pool, pool.acquire()};
    auto* bc = lease.context;
    REQUIRE(bc != nullptr);
    auto result = drogon::sync_wait(
        plinth::js::run_on_context(*bc, R"(({a:[null,true,'A\u0000B']}))"));
    Json::Value expected{Json::objectValue};
    Json::Value values{Json::arrayValue};
    values.append(Json::Value{Json::nullValue});
    values.append(true);
    values.append(std::string{"A\0B", 3});
    expected["a"] = std::move(values);
    REQUIRE(result.value.has_value());
    REQUIRE(*result.value == expected);
    lease.release();
    if (repetition == 3) {
      pool.rebuild();
    }
  }
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

TEST_CASE("shared QuickJS failures survive release rebuild and destroy",
          "[js][conversion][corpus]") {
  auto pool = fresh_pool();
  for (int repetition = 0; repetition < 8; ++repetition) {
    Lease lease{pool, pool.acquire()};
    auto* bc = lease.context;
    REQUIRE(bc != nullptr);
    bool converted_failed = false;
    bool pending_cleared = false;
    plinth::js::EvalErrorKind rejection_kind =
        plinth::js::EvalErrorKind::INTERNAL;
    {
      ValueOwner value{bc->ctx,
                       evaluate(bc->ctx, "({get x(){throw Error('getter')}})")};
      REQUIRE_FALSE(JS_IsException(value.value));
      auto converted = plinth::js::detail::js_to_json(bc->ctx, value.value);
      converted_failed = !converted && converted.error().kind ==
                                           plinth::js::EvalErrorKind::INTERNAL;
      if (JS_HasException(bc->ctx)) {
        JS_FreeValue(bc->ctx, JS_GetException(bc->ctx));
      }
      pending_cleared = !JS_HasException(bc->ctx);
      ValueOwner reason{bc->ctx, evaluate(bc->ctx, "new Error('ordinary')")};
      REQUIRE_FALSE(JS_IsException(reason.value));
      rejection_kind =
          plinth::js::detail::classify_rejection(bc->ctx, reason.value, *bc)
              .kind;
    }
    if (repetition == 7) {
      lease.destroy();
    } else {
      lease.release();
    }
    REQUIRE(converted_failed);
    REQUIRE(pending_cleared);
    REQUIRE(rejection_kind ==
            plinth::js::EvalErrorKind::PROMISE_REJECTED_UNHANDLED);
    if (repetition == 3) {
      pool.rebuild();
    }
  }
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}
