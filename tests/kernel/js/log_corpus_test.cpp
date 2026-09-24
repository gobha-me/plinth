// SPDX-License-Identifier: MIT
//
// Bounded, reproducible coverage for the synchronous QuickJS log callbacks.
// The independent oracle compares raw spdlog payload bytes and levels, not
// the binding's own string conversion helpers.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1210'0000'0000'0001ULL, 0x1210'0000'0000'0025ULL,
    0x1210'0000'0000'00C3ULL, 0x1210'0000'0000'BEEFULL};
constexpr int CASES_PER_SEED = 48;
constexpr int MAX_SHRINK_ATTEMPTS = 16;
constexpr std::size_t MAX_MESSAGE_CODEPOINTS = 256;
constexpr std::size_t MAX_CONTEXT_DEPTH = 3;
constexpr auto SHUTDOWN_BOUND = 2s;

// The nested templates span object depth one through three; other ordinary
// templates contain at most one object and one array level. The cyclic error
// case constructs one object with a self-edge, not an unbounded input tree.
constexpr std::array<std::string_view, MAX_CONTEXT_DEPTH> NESTED_CONTEXT_ARGS{
    ", {n:true, a:[0,false]}",
    ", {n:{v:true}, a:[0,false]}",
    ", {n:{v:{v:true}}, a:[0,false]}",
};
constexpr std::array<std::string_view, MAX_CONTEXT_DEPTH>
    NESTED_CONTEXT_SUFFIXES{
        " ctx={\"n\":true,\"a\":[0,false]}",
        " ctx={\"n\":{\"v\":true},\"a\":[0,false]}",
        " ctx={\"n\":{\"v\":{\"v\":true}},\"a\":[0,false]}",
    };

struct Record {
  spdlog::level::level_enum level;
  std::string payload;
};

class RecordSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  [[nodiscard]] auto snapshot() -> std::vector<Record> {
    std::lock_guard<std::mutex> guard(this->mutex_);
    return records_;
  }

 protected:
  auto sink_it_(const spdlog::details::log_msg& message) -> void override {
    records_.push_back({message.level, std::string{message.payload.data(),
                                                   message.payload.size()}});
  }
  auto flush_() -> void override {}

 private:
  std::vector<Record> records_;
};

class ScopedLogger final {
 public:
  explicit ScopedLogger(std::shared_ptr<RecordSink> sink)
      : previous_(spdlog::default_logger()) {
    auto logger = std::make_shared<spdlog::logger>("log_corpus", sink);
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(std::move(logger));
  }
  ~ScopedLogger() { spdlog::set_default_logger(previous_); }
  ScopedLogger(const ScopedLogger&) = delete;
  auto operator=(const ScopedLogger&) -> ScopedLogger& = delete;
  ScopedLogger(ScopedLogger&&) = delete;
  auto operator=(ScopedLogger&&) -> ScopedLogger& = delete;

 private:
  std::shared_ptr<spdlog::logger> previous_;
};

enum class Kind {
  omitted,
  null_context,
  undefined_context,
  object_context,
  array_context,
  nested_context,
  empty_context,
  extra_argument,
  wrong_context,
  cyclic_context,
  throwing_context,
  missing_message,
  wrong_message,
};

struct CorpusCase {
  std::string_view level;
  spdlog::level::level_enum expected_level;
  Kind kind;
  int variant;
  std::vector<std::uint32_t> codepoints;
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

auto generated_case(std::uint64_t seed, int seed_index, int index)
    -> CorpusCase {
  constexpr std::array<std::string_view, 4> names{"debug", "info", "warn",
                                                  "error"};
  constexpr std::array<spdlog::level::level_enum, 4> levels{
      spdlog::level::debug, spdlog::level::info, spdlog::level::warn,
      spdlog::level::err};
  std::uint64_t state = seed ^ static_cast<std::uint64_t>(index);
  CorpusCase result{.level = names[static_cast<std::size_t>(seed_index)],
                    .expected_level =
                        levels[static_cast<std::size_t>(seed_index)],
                    .kind = static_cast<Kind>(index % 13),
                    .variant = (index / 13 + seed_index) % 4,
                    .codepoints = {}};
  constexpr std::array<std::uint32_t, 7> alphabet{'a',  'Z',    '0',    '\n',
                                                  '\t', 0x00E9, 0x1F600};
  const auto size = static_cast<std::size_t>(next(state) % 24);
  for (std::size_t pos = 0; pos < size; ++pos) {
    result.codepoints.push_back(alphabet[next(state) % alphabet.size()]);
  }
  if (index == 0) {
    result.codepoints.clear();
  } else if (index == 13) {
    result.codepoints = {'A', '\n', 0x00E9, 0x1F600};
  } else if (index == 26) {
    result.codepoints.assign(MAX_MESSAGE_CODEPOINTS, 'x');
  } else if (index == 39) {
    // One NUL-bearing message per level; compare the full raw sink payload.
    result.codepoints = {'A', 0, 'B'};
  }
  return result;
}

auto message_expression(std::span<const std::uint32_t> codepoints)
    -> std::string {
  std::string result = "String.fromCodePoint(";
  for (const auto codepoint : codepoints) {
    if (result.back() != '(') {
      result += ',';
    }
    result += std::to_string(codepoint);
  }
  return result + ')';
}

auto utf8(std::span<const std::uint32_t> codepoints) -> std::string {
  std::string result;
  for (const auto cp : codepoints) {
    if (cp <= 0x7fU) {
      result += static_cast<char>(cp);
    } else if (cp <= 0x7ffU) {
      result += static_cast<char>(0xc0U | (cp >> 6U));
      result += static_cast<char>(0x80U | (cp & 0x3fU));
    } else if (cp <= 0xffffU) {
      result += static_cast<char>(0xe0U | (cp >> 12U));
      result += static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU));
      result += static_cast<char>(0x80U | (cp & 0x3fU));
    } else {
      result += static_cast<char>(0xf0U | (cp >> 18U));
      result += static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU));
      result += static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU));
      result += static_cast<char>(0x80U | (cp & 0x3fU));
    }
  }
  return result;
}

auto context_argument(const CorpusCase& item) -> std::string_view {
  switch (item.kind) {
    case Kind::omitted:
    case Kind::missing_message:
    case Kind::wrong_message: return "";
    case Kind::null_context: return ", null";
    case Kind::undefined_context: return ", undefined";
    case Kind::object_context: return ", {a:1, b:'x'}";
    case Kind::array_context: return ", [1, 'x', null]";
    case Kind::nested_context:
      return NESTED_CONTEXT_ARGS[static_cast<std::size_t>(item.variant) %
                                 MAX_CONTEXT_DEPTH];
    case Kind::empty_context: return ", {}";
    case Kind::extra_argument: return ", {a:1}, 123";
    case Kind::wrong_context: {
      constexpr std::array<std::string_view, 4> WRONG{
          ", 42", ", 'text'", ", true", ", Symbol('ctx')"};
      return WRONG[static_cast<std::size_t>(item.variant)];
    }
    case Kind::cyclic_context:
      return ", (() => {const x = {}; x.self = x; return x})()";
    case Kind::throwing_context:
      return ", {toJSON() {throw new Error('sentinel')}}";
  }
  return "";
}

auto expected_suffix(Kind kind, int variant) -> std::string_view {
  switch (kind) {
    case Kind::object_context: return " ctx={\"a\":1,\"b\":\"x\"}";
    case Kind::extra_argument: return " ctx={\"a\":1}";
    case Kind::array_context: return " ctx=[1,\"x\",null]";
    case Kind::nested_context:
      return NESTED_CONTEXT_SUFFIXES[static_cast<std::size_t>(variant) %
                                     MAX_CONTEXT_DEPTH];
    case Kind::empty_context: return " ctx={}";
    default: return "";
  }
}

auto expected_result(Kind kind) -> std::string_view {
  switch (kind) {
    case Kind::wrong_context:
    case Kind::cyclic_context:
    case Kind::missing_message:
    case Kind::wrong_message: return "error:TypeError";
    case Kind::throwing_context: return "error:Error";
    default: return "ok";
  }
}

auto script(const CorpusCase& item) -> std::string {
  std::string argument = message_expression(item.codepoints);
  if (item.kind == Kind::missing_message) {
    argument.clear();
  } else if (item.kind == Kind::wrong_message) {
    constexpr std::array<std::string_view, 4> WRONG{"42", "null", "true",
                                                    "({})"};
    argument = WRONG[static_cast<std::size_t>(item.variant)];
  }
  return "(() => {try {const value = log." + std::string{item.level} + "(" +
         argument + std::string{context_argument(item)} +
         "); return value === undefined ? 'ok' : 'wrong-return';"
         "} catch (error) {return 'error:' + error.name}})()";
}

struct Failure {
  std::string message;
  std::string signature;
};

auto discrepancy(plinth::js::BridgeContext& context, RecordSink& sink,
                 const CorpusCase& item) -> std::optional<Failure> {
  const auto before = sink.snapshot().size();
  const auto result = plinth::js::eval_on_context(context, script(item));
  if (!result.has_value() || !result->isString()) {
    return Failure{"host evaluation did not return a status string",
                   "evaluation"};
  }
  const auto actual = result->asString();
  const auto wanted = expected_result(item.kind);
  if (actual != wanted) {
    return Failure{"expected status " + std::string{wanted} + ", got " + actual,
                   "status"};
  }
  const auto records = sink.snapshot();
  if (wanted != "ok") {
    if (records.size() != before) {
      return Failure{"rejected call emitted a log record", "rejected-log"};
    }
    return std::nullopt;
  }
  if (records.size() != before + 1) {
    return Failure{"accepted call did not emit exactly one record", "count"};
  }
  const std::string payload =
      utf8(item.codepoints) +
      std::string{expected_suffix(item.kind, item.variant)};
  if (records.back().level != item.expected_level ||
      records.back().payload != payload) {
    return Failure{"record level or raw payload mismatch", "payload"};
  }
  return std::nullopt;
}

auto shrink(CorpusCase item, std::string_view signature, RecordSink& sink)
    -> std::pair<CorpusCase, int> {
  int attempts = 0;
  while (!item.codepoints.empty() && attempts < MAX_SHRINK_ATTEMPTS) {
    auto candidate = item;
    candidate.codepoints.resize(item.codepoints.size() / 2);
    ++attempts;
    plinth::Config config{};
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    auto* context = pool.acquire();
    if (context == nullptr) {
      break;
    }
    auto failure = discrepancy(*context, sink, candidate);
    pool.destroy(context);
    if (!pool.shutdown(SHUTDOWN_BOUND)) {
      break;
    }
    if (!failure.has_value() || failure->signature != signature) {
      break;
    }
    item = std::move(candidate);
  }
  return {std::move(item), attempts};
}

} // namespace

TEST_CASE("QuickJS log callbacks have a bounded deterministic corpus",
          "[js][stdlib][log][corpus]") {
  INFO("seed_count=" << SEEDS.size() << " cases_per_seed=" << CASES_PER_SEED
                     << " max_codepoints=" << MAX_MESSAGE_CODEPOINTS
                     << " max_context_depth=" << MAX_CONTEXT_DEPTH
                     << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS);
  auto sink = std::make_shared<RecordSink>();
  ScopedLogger guard(sink);
  for (std::size_t seed_index = 0; seed_index < SEEDS.size(); ++seed_index) {
    const auto seed = SEEDS[seed_index];
    plinth::Config config{};
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    for (int index = 0; index < CASES_PER_SEED; ++index) {
      INFO("seed=" << seed << " case=" << index);
      auto item = generated_case(seed, static_cast<int>(seed_index), index);
      auto* context = pool.acquire();
      REQUIRE(context != nullptr);
      auto failure = discrepancy(*context, *sink, item);
      if (index % 3 == 0) {
        pool.destroy(context);
      } else {
        pool.release(context);
      }
      if (index % 8 == 7) {
        pool.rebuild();
      }
      if (failure.has_value()) {
        auto [minimal, attempts] = shrink(item, failure->signature, *sink);
        INFO("signature=" << failure->signature << " attempts=" << attempts
                          << " minimal=" << script(minimal));
        FAIL(failure->message);
      }
    }
    REQUIRE(pool.active_count() == 0);
    REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  }
}
