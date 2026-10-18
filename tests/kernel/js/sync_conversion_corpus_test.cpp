// SPDX-License-Identifier: MIT
//
// Finite host-API corpus. These are test generation caps, not product quotas.
// In particular, synthetic resource-word throws test classification only; they
// do not demonstrate actual OOM/stack guards or production cancellation.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using plinth::js::EvalErrorKind;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1760'0000'0000'0000ULL, 0x1760'0000'0000'0025ULL,
    0x1760'0000'0000'00C2ULL, 0x1760'0000'0000'BEEFULL};
constexpr std::size_t FAMILY_COUNT = 32;
constexpr std::size_t REPLAYS = 2;
using Coverage =
    std::array<std::array<std::array<int, REPLAYS>, FAMILY_COUNT>, 2>;
constexpr std::size_t MAX_SOURCE_BYTES = 16384;
constexpr std::size_t MAX_TEXT_BYTES = 4096;
constexpr int MAX_SHRINK_ATTEMPTS = 16;
constexpr std::string_view MINIMAL_NUL_SOURCE = "'A\\u0000B'";
constexpr std::array<std::string_view, FAMILY_COUNT> FAMILY_NAMES{
    "null-undefined",
    "booleans",
    "int32",
    "finite-fractions",
    "safe-integer-edges",
    "negative-zero",
    "bounded-strings",
    "value-bytes",
    "unicode-values",
    "arrays",
    "array-holes",
    "objects",
    "nul-keys",
    "unicode-keys",
    "mixed-trees",
    "own-enumerable-keys",
    "depth-accepted",
    "depth-rejected",
    "object-cycle",
    "array-cycle",
    "symbol-rejected",
    "bigint-rejected",
    "object-getter-throws",
    "array-getter-throws",
    "proxy-ownkeys-throws",
    "proxy-descriptor-throws",
    "proxy-get-throws",
    "parse-error",
    "error-throws",
    "primitive-throws",
    "syntaxerror-throws",
    "synthetic-resource-word-classification"};

enum class Entry : std::size_t { standalone, pooled };

auto entry_name(Entry entry) -> std::string_view {
  return entry == Entry::standalone ? "eval" : "pooled";
}

auto coverage_matches(const Coverage& counts) -> bool {
  int total = 0;
  for (const auto& entry : counts) {
    int entry_total = 0;
    for (const auto& family : entry) {
      int family_total = 0;
      for (const auto replay : family) {
        if (replay != 4) {
          return false;
        }
        family_total += replay;
      }
      if (family_total != 8) {
        return false;
      }
      entry_total += family_total;
    }
    if (entry_total != 256) {
      return false;
    }
    total += entry_total;
  }
  return total == 512;
}

struct Budget {
  int nodes = 1;
  int depth = 0;
  int width = 0;
  int object_entries = 0;
  int array_slots = 0;
  std::size_t text_bytes = 0;
  bool depth_boundary = false;
};

struct Item {
  std::size_t family = 0;
  std::string source;
  Json::Value expected;
  std::optional<EvalErrorKind> standalone_error;
  std::optional<EvalErrorKind> pooled_error;
  std::string marker;
  Budget budget;
};

auto success(std::size_t family, std::string source, Json::Value expected,
             Budget budget = {}) -> Item {
  return {.family = family,
          .source = std::move(source),
          .expected = std::move(expected),
          .standalone_error = std::nullopt,
          .pooled_error = std::nullopt,
          .marker = {},
          .budget = budget};
}

auto rejected(std::size_t family, std::string source, EvalErrorKind kind,
              std::string marker = {}, Budget budget = {}) -> Item {
  auto item = success(family, std::move(source), Json::Value{}, budget);
  item.standalone_error = kind;
  item.pooled_error = kind;
  item.marker = std::move(marker);
  return item;
}

auto array(std::initializer_list<Json::Value> values) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const auto& value : values) {
    result.append(value);
  }
  return result;
}

auto depth_item(std::size_t family, int depth, bool accepted) -> Item {
  std::string source(static_cast<std::size_t>(depth), '[');
  source += '1';
  source.append(static_cast<std::size_t>(depth), ']');
  Json::Value expected{1};
  for (int edge = 0; edge < depth; ++edge) {
    expected = array({expected});
  }
  const Budget budget{.nodes = depth + 1,
                      .depth = depth,
                      .width = 1,
                      .object_entries = 0,
                      .array_slots = 1,
                      .text_bytes = 0,
                      .depth_boundary = true};
  if (accepted) {
    return success(family, std::move(source), std::move(expected), budget);
  }
  return rejected(family, std::move(source), EvalErrorKind::INTERNAL, {},
                  budget);
}

// Expected strings/numbers/keys are literal C++ values, never obtained by
// executing another JS entry, parsing JS source, or using a JS JSON codec.
auto make_item(std::size_t family, std::uint64_t seed) -> Item {
  const auto variant = static_cast<std::size_t>(seed % 4U);
  switch (family) {
    case 0:
      return success(family, variant % 2 == 0 ? "null" : "void 0",
                     Json::Value{});
    case 1:
      return success(family, variant % 2 == 0 ? "true" : "false",
                     Json::Value{variant % 2 == 0});
    case 2: {
      constexpr std::array<std::string_view, 4> source{"(-2147483647 - 1)",
                                                       "2147483647", "-1", "0"};
      constexpr std::array<Json::Int, 4> expected{
          std::numeric_limits<Json::Int>::min(),
          std::numeric_limits<Json::Int>::max(), -1, 0};
      return success(family, std::string{source[variant]},
                     Json::Value{expected[variant]});
    }
    case 3: {
      constexpr std::array<std::string_view, 4> source{"0.125", "-0.5",
                                                       "2 ** -900", "2 ** 900"};
      constexpr std::array<double, 4> expected{0.125, -0.5, 0x1p-900, 0x1p900};
      return success(family, std::string{source[variant]},
                     Json::Value{expected[variant]});
    }
    case 4: {
      constexpr std::array<std::string_view, 4> source{
          "2147483648", "-2147483649", "9007199254740991", "-9007199254740991"};
      constexpr std::array<double, 4> expected{
          2147483648.0, -2147483649.0, 9007199254740991.0, -9007199254740991.0};
      return success(family, std::string{source[variant]},
                     Json::Value{expected[variant]});
    }
    case 5: {
      constexpr std::array<std::string_view, 4> source{"-0", "-0.0", "-(0)",
                                                       "-0 / 1"};
      return success(family, std::string{source[variant]}, Json::Value{-0.0});
    }
    case 6: {
      constexpr std::array<std::size_t, 4> lengths{0, 31, 127, 1024};
      const std::string value(lengths[variant], 'x');
      return success(family, "'" + value + "'", Json::Value{value},
                     {.text_bytes = value.size()});
    }
    case 7: {
      // Include an adjacent-prefix value in the corpus (A\0B versus A\0C).
      constexpr std::array<std::string_view, 4> source{
          "'A\\u0000B'", "'A\\u0000C'", "'\\u0000A'",
          "'A\\u0000B\\n\\t\\r\\b\\fC'"};
      const std::array<std::string, 4> expected{
          std::string{"A\0B", 3}, std::string{"A\0C", 3}, std::string{"\0A", 2},
          std::string{"A\0B\n\t\r\b\fC", 9}};
      return success(family, std::string{source[variant]},
                     Json::Value{expected[variant]},
                     {.text_bytes = expected[variant].size()});
    }
    case 8: {
      constexpr std::array<std::string_view, 4> source{
          "'\\u00e9'", "'\\u4e2d'", "'\\ud83d\\ude00'",
          "'\\u00e9\\u4e2d\\ud83d\\ude00'"};
      const std::array<std::string, 4> expected{
          "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80",
          "\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80"};
      return success(family, std::string{source[variant]},
                     Json::Value{expected[variant]},
                     {.text_bytes = expected[variant].size()});
    }
    case 9: {
      constexpr std::array<int, 4> widths{0, 1, 4, 8};
      std::string source{"["};
      Json::Value expected{Json::arrayValue};
      for (int index = 0; index < widths[variant]; ++index) {
        source += (index == 0 ? "" : ",") + std::to_string(index + 1);
        expected.append(index + 1);
      }
      return success(family, source + ']', std::move(expected),
                     {.nodes = widths[variant] + 1,
                      .depth = 1,
                      .width = widths[variant],
                      .array_slots = widths[variant]});
    }
    case 10:
      return success(family, "[,'x',undefined,null]",
                     array({Json::Value{}, Json::Value{"x"}, Json::Value{},
                            Json::Value{}}),
                     {.nodes = 5,
                      .depth = 1,
                      .width = 4,
                      .array_slots = 4,
                      .text_bytes = 1});
    case 11: {
      constexpr std::array<int, 4> widths{0, 1, 4, 8};
      std::string source{"({"};
      Json::Value expected{Json::objectValue};
      for (int index = 0; index < widths[variant]; ++index) {
        const auto key = "k" + std::to_string(index);
        source +=
            (index == 0 ? "" : ",") + key + ':' + std::to_string(index + 1);
        expected[key] = index + 1;
      }
      return success(
          family, source + "})", std::move(expected),
          {.nodes = widths[variant] + 1,
           .depth = 1,
           .width = widths[variant],
           .object_entries = widths[variant],
           .text_bytes = static_cast<std::size_t>(widths[variant]) * 2});
    }
    case 12: {
      Json::Value expected{Json::objectValue};
      expected["A"] = 1;
      expected[std::string{"A\0B", 3}] = 2;
      expected[std::string{"A\0C", 3}] = 3;
      return success(family, "({'A':1,['A\\u0000B']:2,['A\\u0000C']:3})",
                     std::move(expected),
                     {.nodes = 4,
                      .depth = 1,
                      .width = 3,
                      .object_entries = 3,
                      .text_bytes = 7});
    }
    case 13: {
      Json::Value expected{Json::objectValue};
      expected["\xC3\xA9"] = 1;
      expected["\xE4\xB8\xAD"] = 2;
      expected["\xF0\x9F\x98\x80"] = 3;
      return success(family,
                     "({['\\u00e9']:1,['\\u4e2d']:2,"
                     "['\\ud83d\\ude00']:3})",
                     std::move(expected),
                     {.nodes = 4,
                      .depth = 1,
                      .width = 3,
                      .object_entries = 3,
                      .text_bytes = 9});
    }
    case 14: {
      Json::Value leaf{Json::objectValue};
      leaf["flag"] = true;
      Json::Value expected{Json::objectValue};
      expected["a"] = array({Json::Value{1}, array({leaf})});
      expected["b"] = Json::Value{"tail"};
      return success(family, "({a:[1,[{flag:true}]],b:'tail'})",
                     std::move(expected),
                     {.nodes = 7,
                      .depth = 4,
                      .width = 2,
                      .object_entries = 2,
                      .array_slots = 2,
                      .text_bytes = 10});
    }
    case 15: {
      Json::Value expected{Json::objectValue};
      expected["visible"] = 7;
      return success(family,
                     "(()=>{const o=Object.create({inherited:9});"
                     "o.visible=7;Object.defineProperty(o,'hidden',{value:8});"
                     "o[Symbol('excluded')]=10;return o})()",
                     std::move(expected),
                     {.nodes = 5,
                      .depth = 1,
                      .width = 4,
                      .object_entries = 4,
                      .text_bytes = 32});
    }
    case 16: return depth_item(family, variant % 2 == 0 ? 63 : 64, true);
    case 17: return depth_item(family, variant % 2 == 0 ? 65 : 66, false);
    case 18:
      return rejected(family, "(()=>{const o={};o.self=o;return o})()",
                      EvalErrorKind::INTERNAL);
    case 19:
      return rejected(family, "(()=>{const a=[];a[0]=a;return a})()",
                      EvalErrorKind::INTERNAL);
    case 20:
      return rejected(family, "Symbol('finite')", EvalErrorKind::INTERNAL);
    case 21: return rejected(family, "1n", EvalErrorKind::INTERNAL);
    case 22:
      return rejected(family,
                      "({get value(){throw new Error('getter-marker')}})",
                      EvalErrorKind::INTERNAL);
    case 23:
      return rejected(family,
                      "(()=>{const a=[0];Object.defineProperty(a,'0',"
                      "{get(){throw new Error('array-marker')}});return a})()",
                      EvalErrorKind::INTERNAL);
    case 24:
      return rejected(
          family, "new Proxy({},{ownKeys(){throw new Error('keys-marker')}})",
          EvalErrorKind::INTERNAL);
    case 25:
      return rejected(family,
                      "new Proxy({x:1},{getOwnPropertyDescriptor(){"
                      "throw new Error('descriptor-marker')}})",
                      EvalErrorKind::INTERNAL);
    case 26:
      return rejected(family,
                      "new Proxy({x:1},{get(){throw new Error('get-marker')}})",
                      EvalErrorKind::INTERNAL);
    case 27: {
      constexpr std::array<std::string_view, 4> source{"const = ;", "let x = ;",
                                                       "function (", "({x:})"};
      return rejected(family, std::string{source[variant]},
                      EvalErrorKind::SYNTAX_ERROR);
    }
    case 28:
      return rejected(family,
                      variant % 2 == 0
                          ? "throw new Error('runtime-marker')"
                          : "throw new TypeError('runtime-marker')",
                      EvalErrorKind::RUNTIME_ERROR, "runtime-marker");
    case 29: {
      constexpr std::array<std::string_view, 4> source{
          "throw 'primitive-marker'", "throw 17", "throw null",
          "throw undefined"};
      return rejected(family, std::string{source[variant]},
                      EvalErrorKind::RUNTIME_ERROR);
    }
    case 30:
      return rejected(family, "throw new SyntaxError('syntax-marker')",
                      EvalErrorKind::SYNTAX_ERROR, "syntax-marker");
    default: {
      REQUIRE(family == 31);
      if (variant < 2) {
        return rejected(family, "throw new InternalError('out of memory')",
                        EvalErrorKind::MEMORY_LIMIT, "out of memory");
      }
      auto item = rejected(
          family,
          variant == 2
              ? "throw new RangeError('Maximum call stack size exceeded')"
              : "throw new InternalError('stack overflow')",
          EvalErrorKind::RUNTIME_ERROR, "stack");
      // Distinct public classifiers are intentionally asserted independently.
      item.pooled_error = EvalErrorKind::STACK_OVERFLOW;
      return item;
    }
  }
}

auto check_budget(const Item& item) -> void {
  REQUIRE(item.source.size() <= MAX_SOURCE_BYTES);
  REQUIRE(item.budget.text_bytes <= MAX_TEXT_BYTES);
  REQUIRE(item.budget.width <= 8);
  REQUIRE(item.budget.object_entries <= 16);
  REQUIRE(item.budget.array_slots <= 128);
  REQUIRE(item.budget.nodes <= (item.budget.depth_boundary ? 67 : 63));
  REQUIRE(item.budget.depth <= (item.budget.depth_boundary ? 66 : 4));
}

struct Observation {
  bool succeeded = false;
  Json::Value value;
  plinth::js::EvalError error{
      .kind = EvalErrorKind::INTERNAL, .message = {}, .line = 0, .column = 0};
};

auto capture(std::expected<Json::Value, plinth::js::EvalError> result)
    -> Observation {
  if (result) {
    return {.succeeded = true, .value = std::move(*result), .error = {}};
  }
  return {.succeeded = false, .value = {}, .error = std::move(result.error())};
}

enum class Mismatch {
  none,
  polarity,
  type,
  integer,
  real,
  sign,
  string_length,
  string_bytes,
  array_length,
  keys,
  error_kind,
  diagnostic,
  marker
};

auto compare_json(const Json::Value& observed, const Json::Value& expected)
    -> Mismatch {
  if (observed.type() != expected.type()) {
    return Mismatch::type;
  }
  switch (expected.type()) {
    case Json::nullValue: return Mismatch::none;
    case Json::intValue:
      return observed.asLargestInt() == expected.asLargestInt()
                 ? Mismatch::none
                 : Mismatch::integer;
    case Json::uintValue:
      return observed.asLargestUInt() == expected.asLargestUInt()
                 ? Mismatch::none
                 : Mismatch::integer;
    case Json::realValue: {
      const double actual = observed.asDouble();
      const double wanted = expected.asDouble();
      // Finite corpus policy: NaN/Infinity outputs are not silently accepted.
      // This predicate does not ratify their current conversion policy.
      if (!std::isfinite(actual) || actual != wanted) {
        return Mismatch::real;
      }
      return actual == 0.0 && std::signbit(actual) != std::signbit(wanted)
                 ? Mismatch::sign
                 : Mismatch::none;
    }
    case Json::stringValue: {
      const auto actual = observed.asString();
      const auto wanted = expected.asString();
      if (actual.size() != wanted.size()) {
        return Mismatch::string_length;
      }
      return actual == wanted ? Mismatch::none : Mismatch::string_bytes;
    }
    case Json::booleanValue:
      return observed.asBool() == expected.asBool() ? Mismatch::none
                                                    : Mismatch::integer;
    case Json::arrayValue:
      if (observed.size() != expected.size()) {
        return Mismatch::array_length;
      }
      for (Json::ArrayIndex index = 0; index < expected.size(); ++index) {
        const auto mismatch = compare_json(observed[index], expected[index]);
        if (mismatch != Mismatch::none) {
          return mismatch;
        }
      }
      return Mismatch::none;
    case Json::objectValue: {
      const auto actual_keys = observed.getMemberNames();
      const auto wanted_keys = expected.getMemberNames();
      if (actual_keys.size() != wanted_keys.size() ||
          actual_keys != wanted_keys) {
        return Mismatch::keys;
      }
      // JsonCpp member names are length-bearing strings, including embedded
      // NUL.
      for (const auto& key : wanted_keys) {
        const auto mismatch = compare_json(observed[key], expected[key]);
        if (mismatch != Mismatch::none) {
          return mismatch;
        }
      }
      return Mismatch::none;
    }
  }
  return Mismatch::type;
}

auto matches(const Observation& observed, const Item& item, Entry entry)
    -> Mismatch {
  const auto kind =
      entry == Entry::standalone ? item.standalone_error : item.pooled_error;
  if (observed.succeeded == kind.has_value()) {
    return Mismatch::polarity;
  }
  if (!kind) {
    return compare_json(observed.value, item.expected);
  }
  if (observed.error.kind != *kind) {
    return Mismatch::error_kind;
  }
  if (observed.error.message.empty()) {
    return Mismatch::diagnostic;
  }
  if (!item.marker.empty() &&
      observed.error.message.find(item.marker) == std::string::npos) {
    return Mismatch::marker;
  }
  return Mismatch::none;
}

class Lease {
 public:
  explicit Lease(plinth::js::RuntimePool& owner)
      : pool(owner), context(owner.acquire()) {}
  Lease(const Lease&) = delete;
  auto operator=(const Lease&) -> Lease& = delete;
  Lease(Lease&&) = delete;
  auto operator=(Lease&&) -> Lease& = delete;
  ~Lease() { destroy(); }

  auto release() -> void {
    pool.release(context);
    context = nullptr;
  }
  auto destroy() -> void {
    if (context != nullptr) {
      pool.destroy(context);
      context = nullptr;
    }
  }
  auto get() const -> plinth::js::BridgeContext* { return context; }

 private:
  plinth::js::RuntimePool& pool;
  plinth::js::BridgeContext* context;
};

auto limits() -> plinth::js::RuntimeLimits {
  auto result = plinth::js::default_runtime_limits();
  result.cpu_time_limit = 5s;
  result.wall_clock_limit = 10s;
  return result;
}

struct LifecycleObservation {
  Json::Value reset_value;
  bool counters_reset = false;
  int released_active = -1;
  int released_free = -1;
  int acquired_active = -1;
  int acquired_free = -1;
};

auto reset_expected() -> Json::Value {
  Json::Value value{Json::objectValue};
  value["marker"] = false;
  value["number"] = 42;
  value["builtin"] = true;
  return value;
}

auto lifecycle_matches(const LifecycleObservation& value) -> bool {
  return compare_json(value.reset_value, reset_expected()) == Mismatch::none &&
         value.counters_reset && value.released_active == 0 &&
         value.released_free == 1 && value.acquired_active == 1 &&
         value.acquired_free == 0;
}

struct Harness {
  Clock::time_point deadline = Clock::now() + 60s;
  int cleanup_checks = 0;
  int controls = 0;
  int shrink_attempts = 0;
  int shutdowns = 0;
  int minimized = 0;
  int standalone_calls = 0;
  int pooled_calls = 0;
  int representation_observations = 0;
  std::optional<LifecycleObservation> lifecycle_sample;

  auto check_time() const -> void { REQUIRE(Clock::now() < deadline); }

  auto standalone(std::string_view source) -> Observation {
    check_time();
    ++standalone_calls;
    return capture(plinth::js::eval(source));
  }

  auto pooled(plinth::js::BridgeContext& context, std::string_view source)
      -> Observation {
    check_time();
    ++pooled_calls;
    return capture(plinth::js::eval_on_context(context, source));
  }

  auto observe(Entry entry, plinth::js::RuntimePool& pool, const Item& item)
      -> Observation {
    check_budget(item);
    if (entry == Entry::standalone) {
      auto observed = standalone(item.source);
      if (!observed.succeeded) {
        const auto clean = standalone("42");
        REQUIRE(clean.succeeded);
        REQUIRE(compare_json(clean.value, Json::Value{42}) == Mismatch::none);
        ++cleanup_checks;
      }
      return observed;
    }

    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    REQUIRE(pool.active_count() == 1);
    REQUIRE(pool.free_count() == 0);
    // The source stays finite even when conversion invokes a getter/trap.
    const std::string marked =
        "globalThis.corpus_marker='owned';\n" + item.source;
    REQUIRE(marked.size() <= MAX_SOURCE_BYTES);
    auto observed = pooled(*lease.get(), marked);
    if (!observed.succeeded) {
      lease.destroy(); // Never clear an exception or recycle a failed context.
      REQUIRE(pool.active_count() == 0);
      Lease fresh{pool};
      REQUIRE(fresh.get() != nullptr);
      const auto clean = pooled(*fresh.get(), "42");
      REQUIRE(clean.succeeded);
      REQUIRE(compare_json(clean.value, Json::Value{42}) == Mismatch::none);
      fresh.destroy();
      REQUIRE(pool.active_count() == 0);
      REQUIRE(pool.free_count() == 0);
      ++cleanup_checks;
      return observed;
    }

    // Non-default counters prove release's actual reset rather than only
    // proving that untouched default values stayed at zero.
    lease.get()->call_depth = 3;
    lease.get()->next_callback_id = 9;
    lease.get()->concurrent_async_ops = 2;
    lease.release();
    LifecycleObservation life;
    life.released_active = pool.active_count();
    life.released_free = pool.free_count();
    Lease reused{pool};
    REQUIRE(reused.get() != nullptr);
    life.acquired_active = pool.active_count();
    life.acquired_free = pool.free_count();
    const auto& context = *reused.get();
    life.counters_reset =
        context.call_depth == 0 && context.next_callback_id == 0 &&
        context.concurrent_async_ops == 0 && context.callbacks.empty() &&
        context.pending_ops.empty() &&
        !context.cancelled.load(std::memory_order_acquire) &&
        context.execution_start == Clock::time_point{} &&
        context.cpu_timer_start == Clock::time_point{} &&
        context.cpu_time_accumulated == 0ns;
    const auto reset = pooled(
        *reused.get(),
        "({marker:Object.prototype.hasOwnProperty.call(globalThis,'corpus_"
        "marker'),"
        "number:6*7,builtin:typeof Object==='function'&&Array.isArray([])})");
    REQUIRE(reset.succeeded);
    life.reset_value = reset.value;
    REQUIRE(lifecycle_matches(life));
    lifecycle_sample = life;
    reused.destroy(); // Both replays start with genuinely fresh JS owners.
    REQUIRE(pool.active_count() == 0);
    REQUIRE(pool.free_count() == 0);
    ++cleanup_checks;
    return observed;
  }
};

auto complexity(const Item& item) -> std::tuple<int, std::size_t, int> {
  return {item.budget.nodes, item.source.size(), item.budget.depth};
}

auto truncate_nul(Observation value) -> Observation {
  REQUIRE(value.succeeded);
  REQUIRE(value.value.type() == Json::stringValue);
  const auto text = value.value.asString();
  const auto nul = text.find('\0');
  REQUIRE(nul != std::string::npos);
  value.value = text.substr(0, nul);
  return value;
}

auto smaller_items(const Item& item) -> std::vector<Item> {
  std::vector<Item> candidates;
  if (item.family == 9 && item.expected.size() > 1) {
    auto candidate = item;
    const auto retained = item.expected.size() / 2;
    candidate.source = "[";
    candidate.expected = Json::Value{Json::arrayValue};
    for (Json::ArrayIndex index = 0; index < retained; ++index) {
      if (index != 0) {
        candidate.source += ',';
      }
      candidate.source += std::to_string(item.expected[index].asInt());
      candidate.expected.append(item.expected[index]);
    }
    candidate.source += ']';
    candidate.budget.nodes = static_cast<int>(retained) + 1;
    candidate.budget.width = static_cast<int>(retained);
    candidate.budget.array_slots = static_cast<int>(retained);
    candidates.push_back(std::move(candidate));
  }
  if (item.family == 11 && item.expected.size() > 1) {
    auto candidate = item;
    const auto retained = item.expected.size() / 2;
    const auto keys = item.expected.getMemberNames();
    candidate.source = "({";
    candidate.expected = Json::Value{Json::objectValue};
    for (Json::ArrayIndex index = 0; index < retained; ++index) {
      if (index != 0) {
        candidate.source += ',';
      }
      const auto& key = keys[index];
      candidate.source +=
          key + ':' + std::to_string(item.expected[key].asInt());
      candidate.expected[key] = item.expected[key];
    }
    candidate.source += "})";
    candidate.budget.nodes = static_cast<int>(retained) + 1;
    candidate.budget.width = static_cast<int>(retained);
    candidate.budget.object_entries = static_cast<int>(retained);
    candidate.budget.text_bytes = static_cast<std::size_t>(retained) * 2;
    candidates.push_back(std::move(candidate));
  }
  if (item.family == 14 && item.expected.isMember("b")) {
    auto candidate = item;
    candidate.source = "({a:[1,[{flag:true}]]})";
    candidate.expected.removeMember("b");
    --candidate.budget.nodes;
    candidate.budget.object_entries = 1;
    candidate.budget.text_bytes -= 5;
    candidates.push_back(std::move(candidate));
  }
  if ((item.family == 16 && item.budget.depth == 64) ||
      (item.family == 17 && item.budget.depth == 66)) {
    candidates.push_back(
        depth_item(item.family, item.budget.depth - 1, item.family == 16));
  }
  if (item.family == 7 && item.source != MINIMAL_NUL_SOURCE) {
    const auto text = item.expected.asString();
    if (text == std::string{"unrelated-prefix-A\0B-unrelated-tail", 35}) {
      // Delete the original prefix; do not regenerate an unrelated string.
      const std::string middle{"A\0B-unrelated-tail", 18};
      candidates.push_back(success(7, "'A\\u0000B-unrelated-tail'",
                                   Json::Value{middle},
                                   {.text_bytes = middle.size()}));
    }
    if (text.find(std::string{"A\0B", 3}) != std::string::npos) {
      auto candidate =
          success(7, std::string{MINIMAL_NUL_SOURCE},
                  Json::Value{std::string{"A\0B", 3}}, {.text_bytes = 3});
      if (complexity(candidate) < complexity(item)) {
        candidates.push_back(std::move(candidate));
      }
    }
  }
  return candidates;
}

auto shrink(Harness& harness, Entry entry, plinth::js::RuntimePool& pool,
            Item current, Mismatch signature, bool corrupt) -> Item {
  int attempts = 0;
  while (attempts < MAX_SHRINK_ATTEMPTS) {
    bool improved = false;
    for (auto candidate : smaller_items(current)) {
      if (attempts >= MAX_SHRINK_ATTEMPTS) {
        break;
      }
      REQUIRE(candidate.family == current.family);
      REQUIRE(complexity(candidate) < complexity(current));
      ++attempts;
      ++harness.shrink_attempts;
      auto observed = harness.observe(entry, pool, candidate);
      if (corrupt) {
        // Corruption is applied to a separately validated actual observation.
        REQUIRE(matches(observed, candidate, entry) == Mismatch::none);
        observed = truncate_nul(std::move(observed));
      }
      if (matches(observed, candidate, entry) == signature) {
        current = std::move(candidate);
        improved = true;
        break;
      }
    }
    if (!improved) {
      break;
    }
  }
  return current;
}

template <class Mutate>
auto negative(Harness& harness, const Observation& original, const Item& item,
              Entry entry, Mutate mutate) -> void {
  REQUIRE(matches(original, item, entry) == Mismatch::none);
  auto corrupted = original;
  mutate(corrupted);
  REQUIRE(matches(corrupted, item, entry) != Mismatch::none);
  ++harness.controls;
}

auto observation_controls(Harness& harness, Entry entry,
                          plinth::js::RuntimePool& pool) -> void {
  const auto checked = [&](const Item& item) {
    auto observed = harness.observe(entry, pool, item);
    REQUIRE(matches(observed, item, entry) == Mismatch::none);
    return observed;
  };
  const auto integer = make_item(2, SEEDS[0]);
  const auto actual_integer = checked(integer);
  negative(harness, actual_integer, integer, entry,
           [](Observation& value) { value.succeeded = false; });
  negative(harness, actual_integer, integer, entry,
           [](Observation& value) { value.value = "wrong-type"; });
  negative(harness, actual_integer, integer, entry,
           [](Observation& value) { value.value = -2147483647; });

  const auto fraction = make_item(3, SEEDS[0]);
  const auto actual_fraction = checked(fraction);
  negative(harness, actual_fraction, fraction, entry,
           [](Observation& value) { value.value = 0.25; });
  negative(harness, actual_fraction, fraction, entry, [](Observation& value) {
    value.value = std::numeric_limits<double>::quiet_NaN();
  });
  negative(harness, actual_fraction, fraction, entry, [](Observation& value) {
    value.value = std::numeric_limits<double>::infinity();
  });
  const auto zero = make_item(5, SEEDS[0]);
  const auto actual_zero = checked(zero);
  negative(harness, actual_zero, zero, entry,
           [](Observation& value) { value.value = 0.0; });

  const auto bytes = make_item(7, SEEDS[0]);
  const auto actual_bytes = checked(bytes);
  negative(harness, actual_bytes, bytes, entry, [](Observation& value) {
    auto text = value.value.asString();
    text.back() = 'C';
    value.value = text;
  });
  negative(harness, actual_bytes, bytes, entry,
           [](Observation& value) { value = truncate_nul(std::move(value)); });

  const auto keys = make_item(12, SEEDS[0]);
  const auto actual_keys = checked(keys);
  negative(harness, actual_keys, keys, entry, [](Observation& value) {
    const std::string key{"A\0B", 3};
    const auto member = value.value[key];
    value.value.removeMember(key);
    value.value[std::string{"A\0D", 3}] = member;
  });
  negative(harness, actual_keys, keys, entry, [](Observation& value) {
    // Simulate truncation colliding with the existing prefix sibling.
    value.value["A"] = value.value[std::string{"A\0B", 3}];
    value.value.removeMember(std::string{"A\0B", 3});
  });
  negative(harness, actual_keys, keys, entry,
           [](Observation& value) { value.value.removeMember("A"); });
  negative(harness, actual_keys, keys, entry,
           [](Observation& value) { value.value["extra"] = 4; });

  const auto sequence = make_item(9, SEEDS[3]);
  const auto actual_sequence = checked(sequence);
  negative(harness, actual_sequence, sequence, entry,
           [](Observation& value) { value.value.append(9); });
  negative(harness, actual_sequence, sequence, entry, [](Observation& value) {
    const auto first = value.value[0];
    value.value[0] = value.value[1];
    value.value[1] = first;
  });
  const auto error = make_item(28, SEEDS[0]);
  const auto actual_error = checked(error);
  negative(harness, actual_error, error, entry,
           [](Observation& value) { value.succeeded = true; });
  negative(harness, actual_error, error, entry, [](Observation& value) {
    value.error.kind = EvalErrorKind::INTERNAL;
  });
  negative(harness, actual_error, error, entry,
           [](Observation& value) { value.error.message = "marker removed"; });
}

auto numeric_representation_controls(Harness& harness, Entry entry,
                                     plinth::js::RuntimePool& pool) -> void {
  // Pinned QuickJS represents the operand 2147483648 as a float and preserves
  // that representation through unary negation. Integer subtraction follows
  // its int32 path. Characterize both concrete inputs without promising a
  // general normalization policy for all mathematically integral JS numbers.
  const auto literal = success(2, "-2147483648", Json::Value{-2147483648.0});
  const auto arithmetic =
      success(2, "(-2147483647 - 1)",
              Json::Value{std::numeric_limits<Json::Int>::min()});
  for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
    INFO("numeric representation characterization entry="
         << entry_name(entry) << " replay=" << replay);
    const auto actual_literal = harness.observe(entry, pool, literal);
    REQUIRE(matches(actual_literal, literal, entry) == Mismatch::none);
    ++harness.representation_observations;
    negative(harness, actual_literal, literal, entry, [](Observation& value) {
      // Preserve the numeric magnitude: only the JsonCpp kind changes.
      value.value = Json::Value{std::numeric_limits<Json::Int>::min()};
    });

    const auto actual_arithmetic = harness.observe(entry, pool, arithmetic);
    REQUIRE(matches(actual_arithmetic, arithmetic, entry) == Mismatch::none);
    ++harness.representation_observations;
    negative(
        harness, actual_arithmetic, arithmetic, entry,
        [](Observation& value) { value.value = Json::Value{-2147483648.0}; });
  }
}

auto shrinking_controls(Harness& harness, Entry entry,
                        plinth::js::RuntimePool& pool) -> void {
  const std::string text{"unrelated-prefix-A\0B-unrelated-tail", 35};
  const auto original =
      success(7, "'unrelated-prefix-A\\u0000B-unrelated-tail'",
              Json::Value{text}, {.text_bytes = text.size()});
  const auto observed = harness.observe(entry, pool, original);
  REQUIRE(matches(observed, original, entry) == Mismatch::none);
  const auto signature = matches(truncate_nul(observed), original, entry);
  REQUIRE(signature == Mismatch::string_length);
  const auto minimized =
      shrink(harness, entry, pool, original, signature, true);
  REQUIRE(complexity(minimized) < complexity(original));
  REQUIRE(minimized.family == original.family);
  REQUIRE(minimized.source == MINIMAL_NUL_SOURCE);
  const auto clean = harness.observe(entry, pool, minimized);
  REQUIRE(matches(clean, minimized, entry) == Mismatch::none);
  REQUIRE(matches(truncate_nul(clean), minimized, entry) == signature);
  ++harness.controls;
  // This literal is retained above as MINIMAL_NUL_SOURCE, independent of the
  // shrinker's candidate generation. Disable corruption and re-evaluate it.
  const auto regression =
      success(7, std::string{MINIMAL_NUL_SOURCE},
              Json::Value{std::string{"A\0B", 3}}, {.text_bytes = 3});
  REQUIRE(matches(harness.observe(entry, pool, regression), regression,
                  entry) == Mismatch::none);
  ++harness.minimized;
  std::cout << "SYNC_CONVERSION_CORPUS_SHRINK seed=" << SEEDS[0]
            << " entry=" << entry_name(entry)
            << " family=7 replay=0 signature=string_length original="
            << original.source << " minimized=" << minimized.source
            << " observed_type=" << static_cast<int>(clean.value.type())
            << " observed_kind=success clean=1 corrupted=1 cleanup_active="
            << pool.active_count() << '\n';
}

auto lifecycle_controls(Harness& harness) -> void {
  REQUIRE(harness.lifecycle_sample.has_value());
  const auto original = *harness.lifecycle_sample;
  REQUIRE(lifecycle_matches(original));
  auto corrupted = original;
  corrupted.reset_value["marker"] = true;
  REQUIRE_FALSE(lifecycle_matches(corrupted));
  ++harness.controls;
  corrupted = original;
  corrupted.counters_reset = false;
  REQUIRE_FALSE(lifecycle_matches(corrupted));
  ++harness.controls;
  corrupted = original;
  corrupted.released_free = 0;
  REQUIRE_FALSE(lifecycle_matches(corrupted));
  ++harness.controls;
  corrupted = original;
  corrupted.acquired_active = 0;
  REQUIRE_FALSE(lifecycle_matches(corrupted));
  ++harness.controls;

  plinth::Config config;
  plinth::js::RuntimePool pool{nullptr, limits(), config, 2};
  Lease held{pool};
  REQUIRE(held.get() != nullptr);
  const auto marked =
      harness.pooled(*held.get(), "globalThis.corpus_marker='held-owner'; 42");
  REQUIRE(marked.succeeded);
  REQUIRE(compare_json(marked.value, Json::Value{42}) == Mismatch::none);
  REQUIRE(pool.active_count() == 1);
  REQUIRE(pool.free_count() == 1);
  {
    Lease free_candidate{pool};
    REQUIRE(free_candidate.get() != nullptr);
    const auto poisoned = harness.pooled(
        *free_candidate.get(), "Object.corpus_rebuild_poison='free-owner';"
                               "Object.corpus_rebuild_poison");
    REQUIRE(poisoned.succeeded);
    REQUIRE(compare_json(poisoned.value, Json::Value{"free-owner"}) ==
            Mismatch::none);
    free_candidate.release();
  }
  REQUIRE(pool.active_count() == 1);
  REQUIRE(pool.free_count() == 1);
  {
    Lease reused_free{pool};
    REQUIRE(reused_free.get() != nullptr);
    // Builtin heap state intentionally survives release. Establish that the
    // free owner really is dirty before requiring rebuild to replace it.
    const auto persisted =
        harness.pooled(*reused_free.get(), "Object.corpus_rebuild_poison");
    REQUIRE(persisted.succeeded);
    REQUIRE(compare_json(persisted.value, Json::Value{"free-owner"}) ==
            Mismatch::none);
    reused_free.release();
  }
  REQUIRE(pool.active_count() == 1);
  REQUIRE(pool.free_count() == 1);
  pool.rebuild();
  REQUIRE(pool.active_count() == 1);
  REQUIRE(pool.free_count() == 2);
  const auto survived = harness.pooled(*held.get(), "globalThis.corpus_marker");
  REQUIRE(survived.succeeded);
  REQUIRE(compare_json(survived.value, Json::Value{"held-owner"}) ==
          Mismatch::none);
  Lease refreshed{pool};
  REQUIRE(refreshed.get() != nullptr);
  constexpr std::string_view rebuilt_source =
      "({marker:Object.prototype.hasOwnProperty.call(globalThis,'corpus_marker'"
      "),"
      "number:6*7,builtin:typeof Object==='function'&&Array.isArray([]),"
      "poison:Object.prototype.hasOwnProperty.call(Object,"
      "'corpus_rebuild_poison')})";
  const auto clean = harness.pooled(*refreshed.get(), rebuilt_source);
  REQUIRE(clean.succeeded);
  auto rebuilt_expected = reset_expected();
  rebuilt_expected["poison"] = false;
  REQUIRE(compare_json(clean.value, rebuilt_expected) == Mismatch::none);
  // Keep the first owner checked out so a retained dirty free entry cannot
  // hide behind one fresh entry at the top of the free list.
  Lease second_refreshed{pool};
  REQUIRE(second_refreshed.get() != nullptr);
  REQUIRE(pool.free_count() == 0);
  REQUIRE(pool.active_count() == 3);
  const auto second_clean =
      harness.pooled(*second_refreshed.get(), rebuilt_source);
  REQUIRE(second_clean.succeeded);
  REQUIRE(compare_json(second_clean.value, rebuilt_expected) == Mismatch::none);
  second_refreshed.destroy();
  refreshed.destroy();
  held.destroy();
  REQUIRE(pool.active_count() == 0);
  pool.rebuild();
  REQUIRE(pool.free_count() == 2);
  {
    Lease fresh{pool};
    REQUIRE(fresh.get() != nullptr);
    const auto literal = harness.pooled(*fresh.get(), "42");
    REQUIRE(literal.succeeded);
    REQUIRE(compare_json(literal.value, Json::Value{42}) == Mismatch::none);
  }
  REQUIRE(pool.active_count() == 0);

  Lease shutdown_owner{pool};
  REQUIRE(shutdown_owner.get() != nullptr);
  const auto start = Clock::now();
  REQUIRE_FALSE(pool.shutdown(100ms));
  REQUIRE(Clock::now() - start <= 1s);
  REQUIRE(pool.active_count() == 1);
  REQUIRE(pool.acquire() == nullptr);
  // shutdown(false) retains this owner's validity. Do not evaluate after the
  // pool starts stopping; destruction is the final operation on this lease.
  shutdown_owner.destroy();
  const auto retry = Clock::now();
  REQUIRE(pool.shutdown(2s));
  REQUIRE(Clock::now() - retry <= 2s);
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 0);
  REQUIRE(pool.acquire() == nullptr);
  REQUIRE(pool.shutdown(0ms));
  ++harness.controls;
}

auto cancellation_control(Harness& harness) -> void {
  plinth::Config config;
  plinth::js::RuntimePool pool{nullptr, limits(), config, 1};
  // Fixed 100000-iteration work: enough interrupt polls, no unbounded loop,
  // allocations, jobs, thread, or sleep. This is only synchronous pre-cancel.
  constexpr std::string_view source =
      "(()=>{let n=0;for(let i=0;i<100000;++i){n+=1}return n})()";
  {
    Lease normal{pool};
    REQUIRE(normal.get() != nullptr);
    const auto observed = harness.pooled(*normal.get(), source);
    REQUIRE(observed.succeeded);
    REQUIRE(compare_json(observed.value, Json::Value{100000}) ==
            Mismatch::none);
  }
  {
    Lease cancelled{pool};
    REQUIRE(cancelled.get() != nullptr);
    cancelled.get()->cancelled.store(true, std::memory_order_release);
    const auto observed = harness.pooled(*cancelled.get(), source);
    REQUIRE_FALSE(observed.succeeded);
    REQUIRE(observed.error.kind == EvalErrorKind::CANCELLED);
    REQUIRE_FALSE(observed.error.message.empty());
    cancelled.destroy();
  }
  {
    Lease fresh{pool};
    REQUIRE(fresh.get() != nullptr);
    const auto observed = harness.pooled(*fresh.get(), "42");
    REQUIRE(observed.succeeded);
    REQUIRE(compare_json(observed.value, Json::Value{42}) == Mismatch::none);
  }
  REQUIRE(pool.shutdown(2s));
  ++harness.controls;
}

auto coverage_controls(Harness& harness, const Coverage& actual) -> void {
  REQUIRE(coverage_matches(actual));
  for (std::size_t entry = 0; entry < actual.size(); ++entry) {
    auto dropped_entry = actual;
    dropped_entry[entry] = {};
    REQUIRE_FALSE(coverage_matches(dropped_entry));
    ++harness.controls;
    for (std::size_t family = 0; family < FAMILY_COUNT; ++family) {
      auto dropped_cell = actual;
      dropped_cell[entry][family] = {};
      REQUIRE_FALSE(coverage_matches(dropped_cell));
      ++harness.controls;
      for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
        auto dropped_replay = actual;
        --dropped_replay[entry][family][replay];
        REQUIRE_FALSE(coverage_matches(dropped_replay));
        ++harness.controls;
      }
    }
  }
}

} // namespace

TEST_CASE("QuickJS synchronous evaluation has bounded deterministic conversion "
          "corpora",
          "[js][conversion][corpus][isolated-sync-conversion]") {
  Harness harness;
  Coverage counts{};
  constexpr std::array<Entry, 2> entries{Entry::standalone, Entry::pooled};
  for (const auto seed : SEEDS) {
    plinth::Config config;
    plinth::js::RuntimePool pool{nullptr, limits(), config, 1};
    for (const auto entry : entries) {
      for (std::size_t family = 0; family < FAMILY_COUNT; ++family) {
        for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
          const auto item = make_item(family, seed);
          INFO("seed=" << seed << " entry=" << entry_name(entry)
                       << " family=" << family << ':' << FAMILY_NAMES[family]
                       << " replay=" << replay << " source=" << item.source);
          const auto observed = harness.observe(entry, pool, item);
          const auto mismatch = matches(observed, item, entry);
          if (mismatch != Mismatch::none) {
            const auto minimized =
                shrink(harness, entry, pool, item, mismatch, false);
            INFO("signature="
                 << static_cast<int>(mismatch)
                 << " succeeded=" << observed.succeeded
                 << " type=" << static_cast<int>(observed.value.type())
                 << " kind=" << static_cast<int>(observed.error.kind)
                 << " diagnostic=" << observed.error.message
                 << " minimized=" << minimized.source
                 << " cleanup_active=" << pool.active_count());
            REQUIRE(mismatch == Mismatch::none);
          }
          ++counts[static_cast<std::size_t>(entry)][family][replay];
        }
      }
    }
    REQUIRE(pool.active_count() == 0);
    const auto shutdown_start = Clock::now();
    REQUIRE(pool.shutdown(2s));
    REQUIRE(Clock::now() - shutdown_start <= 2s);
    REQUIRE(pool.active_count() == 0);
    REQUIRE(pool.free_count() == 0);
    REQUIRE(pool.shutdown(0ms));
    ++harness.shutdowns;
  }

  {
    plinth::Config config;
    plinth::js::RuntimePool pool{nullptr, limits(), config, 1};
    for (const auto entry : entries) {
      observation_controls(harness, entry, pool);
      numeric_representation_controls(harness, entry, pool);
      shrinking_controls(harness, entry, pool);
    }
    REQUIRE(pool.shutdown(2s));
  }
  lifecycle_controls(harness);
  cancellation_control(harness);
  coverage_controls(harness, counts);

  // Alternating standalone outcomes exercise real per-call create/destroy.
  for (const std::size_t family : {14U, 22U, 27U, 28U, 14U}) {
    const auto item = make_item(family, SEEDS[0]);
    const auto observed = harness.standalone(item.source);
    REQUIRE(matches(observed, item, Entry::standalone) == Mismatch::none);
    if (!observed.succeeded) {
      const auto fresh = harness.standalone("42");
      REQUIRE(fresh.succeeded);
      REQUIRE(compare_json(fresh.value, Json::Value{42}) == Mismatch::none);
    }
  }

  int total = 0;
  for (const auto entry : entries) {
    for (std::size_t family = 0; family < FAMILY_COUNT; ++family) {
      const auto& replays = counts[static_cast<std::size_t>(entry)][family];
      const auto count = replays[0] + replays[1];
      REQUIRE(count == 8);
      total += count;
      std::cout << "SYNC_CONVERSION_CORPUS_CELL entry=" << entry_name(entry)
                << " family=" << family << " observations=" << count << '\n';
    }
  }
  REQUIRE(total == 512);
  REQUIRE(harness.shutdowns == 4);
  REQUIRE(harness.controls == 246);
  REQUIRE(harness.shrink_attempts == 4);
  REQUIRE(harness.minimized == 2);
  REQUIRE(harness.representation_observations == 8);
  REQUIRE(harness.standalone_calls == 401);
  REQUIRE(harness.pooled_calls == 554);
  harness.check_time();
  std::cout << "SYNC_CONVERSION_CORPUS_SUMMARY primary=" << total
            << " eval=256 pooled=256 cells=64 per_cell=8 seeds=4 families=32"
               " replays=2 controls="
            << harness.controls
            << " shrink_attempts=" << harness.shrink_attempts
            << " minimized=" << harness.minimized
            << " shutdowns=" << harness.shutdowns
            << " cleanup_checks=" << harness.cleanup_checks
            << " representation_observations="
            << harness.representation_observations
            << " actual_eval_calls=" << harness.standalone_calls
            << " actual_pooled_calls=" << harness.pooled_calls << '\n';
}
