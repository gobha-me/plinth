// SPDX-License-Identifier: MIT
//
// Deterministic, sanitizer-friendly corpus for the shipped QuickJS crypto
// callbacks. The seed and case index reproduce every input; no entropy from
// crypto.randomBytes is used as an oracle.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

// 4 × 48 = 192 cases. Generated payloads are at most 1024 bytes; a view
// backing adds at most six guard bytes. The sole larger result is the
// documented 4096-byte randomBytes boundary.
constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1190'0000'0000'0001ULL, 0x1190'0000'0000'0025ULL,
    0x1190'0000'0000'00C3ULL, 0x1190'0000'0000'BEEFULL};
constexpr int CASES_PER_SEED = 48;
constexpr std::size_t MAX_INPUT_BYTES = 1024;
constexpr int MAX_SHRINK_ATTEMPTS = 32;
constexpr auto SHUTDOWN_BOUND = 250ms;

enum class Kind {
  hash_string,
  hash_view,
  hash_bad_algorithm,
  hash_missing_algorithm,
  hash_missing_data,
  hash_wrong_algorithm,
  hash_wrong_data,
  random_valid,
  random_range,
  random_wrong,
  random_missing,
  equal_same,
  equal_different,
  equal_length,
  equal_wrong,
  equal_missing,
};

struct CorpusCase {
  Kind kind = Kind::hash_string;
  std::string algorithm = "sha256";
  std::vector<std::uint32_t> codepoints;
  std::vector<std::uint8_t> bytes;
  int prefix = 0;
  int suffix = 0;
  int length = 1;
  int variant = 0;
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

auto generated_case(std::uint64_t seed, int index) -> CorpusCase {
  std::uint64_t state =
      seed ^ (static_cast<std::uint64_t>(index) * 0x9e37'79b9'7f4a'7c15ULL);
  CorpusCase out;
  out.kind = static_cast<Kind>(index % 16);
  out.algorithm = next(state) % 2 == 0 ? "sha256" : "sha512";
  out.prefix = static_cast<int>(next(state) % 4);
  out.suffix = static_cast<int>(next(state) % 4);
  out.variant = (index / 16 + static_cast<int>(seed % 4)) % 4;
  constexpr std::array<std::uint32_t, 6> chars{'a',    'Z',    0,
                                               0x00E9, 0x4E2D, 0x1F600};
  const auto char_count = static_cast<std::size_t>(next(state) % 25);
  for (std::size_t i = 0; i < char_count; ++i) {
    out.codepoints.push_back(chars[next(state) % chars.size()]);
  }
  if (index == 0) {
    out.codepoints.clear(); // empty string
    out.algorithm = "sha256";
  } else if (index == 16) {
    out.codepoints = {'A', 0, 0x00E9, 0x4E2D}; // NUL + UTF-8
    out.algorithm = "sha512";
  } else if (index == 32) {
    out.codepoints.assign(MAX_INPUT_BYTES, 'x'); // bounded long string
    out.algorithm = "sha256";
  }

  const auto byte_count = static_cast<std::size_t>(next(state) % 65);
  for (std::size_t i = 0; i < byte_count; ++i) {
    out.bytes.push_back(static_cast<std::uint8_t>(next(state) & 0xffU));
  }
  if (index == 1) {
    out.bytes.clear(); // empty Uint8Array
  } else if (index == 17) {
    out.bytes = {0, 0x61, 0xff, 0x80, 0};
    out.prefix = 2;
    out.suffix = 1;
  } else if (index == 33) {
    out.bytes.resize(MAX_INPUT_BYTES);
    for (auto& byte : out.bytes) {
      byte = static_cast<std::uint8_t>(next(state) & 0xffU);
    }
    out.prefix = 3;
    out.suffix = 2;
  }
  if (out.kind == Kind::equal_different) {
    out.variant = index / 16; // first, middle, last across three cases
    if (out.bytes.size() < 3) {
      out.bytes = {1, 2, 3};
    }
  }
  constexpr std::array<int, 8> lengths{1, 2, 16, 127, 4096, 1, 64, 4096};
  constexpr std::array<int, 4> outside{-1, 0, 4097, 5000};
  // Fractional, non-finite, and int32-wrapping JS numbers are deliberately
  // excluded: the ICD and JS_ToInt32 callback differ on their contract.
  // Those inputs need a separate API decision, not an assumed test oracle.
  out.length = out.kind == Kind::random_range
                   ? outside[static_cast<std::size_t>(out.variant)]
                   : lengths[next(state) % lengths.size()];
  if (index == 7) {
    out.length = 1;
  } else if (index == 23) {
    out.length = 4096;
  }
  return out;
}

auto js_number_list(std::span<const std::uint8_t> bytes) -> std::string {
  std::string out;
  for (const auto byte : bytes) {
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(byte);
  }
  return out;
}

auto js_view(std::span<const std::uint8_t> bytes, int prefix, int suffix)
    -> std::string {
  std::vector<std::uint8_t> backing(static_cast<std::size_t>(prefix), 0xA5);
  backing.insert(backing.end(), bytes.begin(), bytes.end());
  backing.insert(backing.end(), static_cast<std::size_t>(suffix), 0x5A);
  return "new Uint8Array([" + js_number_list(backing) + "]).subarray(" +
         std::to_string(prefix) + "," +
         std::to_string(prefix + static_cast<int>(bytes.size())) + ")";
}

auto js_string(std::span<const std::uint32_t> codepoints) -> std::string {
  std::string out = "String.fromCodePoint(";
  for (std::size_t i = 0; i < codepoints.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += std::to_string(codepoints[i]);
  }
  out += ')';
  return out;
}

auto utf8(std::span<const std::uint32_t> codepoints)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  for (const auto cp : codepoints) {
    if (cp <= 0x7fU) {
      out.push_back(static_cast<std::uint8_t>(cp));
    } else if (cp <= 0x7ffU) {
      out.push_back(static_cast<std::uint8_t>(0xc0U | (cp >> 6U)));
      out.push_back(static_cast<std::uint8_t>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0xffffU) {
      out.push_back(static_cast<std::uint8_t>(0xe0U | (cp >> 12U)));
      out.push_back(static_cast<std::uint8_t>(0x80U | ((cp >> 6U) & 0x3fU)));
      out.push_back(static_cast<std::uint8_t>(0x80U | (cp & 0x3fU)));
    } else {
      out.push_back(static_cast<std::uint8_t>(0xf0U | (cp >> 18U)));
      out.push_back(static_cast<std::uint8_t>(0x80U | ((cp >> 12U) & 0x3fU)));
      out.push_back(static_cast<std::uint8_t>(0x80U | ((cp >> 6U) & 0x3fU)));
      out.push_back(static_cast<std::uint8_t>(0x80U | (cp & 0x3fU)));
    }
  }
  return out;
}

auto digest_hex(std::span<const std::uint8_t> bytes, std::string_view algorithm)
    -> std::string {
  const EVP_MD* md = algorithm == "sha256" ? EVP_sha256() : EVP_sha512();
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, md,
                 nullptr) != 1) {
    throw std::runtime_error("OpenSSL oracle failed");
  }
  constexpr std::string_view hex = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<std::size_t>(size) * 2);
  for (unsigned int i = 0; i < size; ++i) {
    out += hex[digest[i] >> 4U];
    out += hex[digest[i] & 0x0fU];
  }
  return out;
}

auto wrong_value(int variant) -> std::string_view {
  constexpr std::array<std::string_view, 4> values{"null", "true", "({})",
                                                   "([])"};
  return values[static_cast<std::size_t>(variant)];
}

auto expression(const CorpusCase& item) -> std::string {
  const auto view = js_view(item.bytes, item.prefix, item.suffix);
  const auto hash = [&](std::string_view data) {
    return "crypto.hash('" + item.algorithm + "'," + std::string{data} + ")";
  };
  switch (item.kind) {
    case Kind::hash_string: return hash(js_string(item.codepoints));
    case Kind::hash_view: return hash(view);
    case Kind::hash_bad_algorithm: {
      constexpr std::array<std::string_view, 4> rejected{"md5", "sha1",
                                                         "SHA256", "unknown"};
      return "crypto.hash('" +
             std::string{rejected[static_cast<std::size_t>(item.variant)]} +
             "'," + view + ")";
    }
    case Kind::hash_missing_algorithm: return "crypto.hash()";
    case Kind::hash_missing_data: return "crypto.hash('sha256')";
    case Kind::hash_wrong_algorithm:
      return "crypto.hash(" + std::string{wrong_value(item.variant)} + "," +
             view + ")";
    case Kind::hash_wrong_data: return hash(wrong_value(item.variant));
    case Kind::random_valid:
    case Kind::random_range:
      return "crypto.randomBytes(" + std::to_string(item.length) + ")";
    case Kind::random_wrong:
      return "crypto.randomBytes(" + std::string{wrong_value(item.variant)} +
             ")";
    case Kind::random_missing: return "crypto.randomBytes()";
    case Kind::equal_same:
      return "crypto.timingSafeEqual(" + view + "," +
             js_view(item.bytes, item.suffix, item.prefix) + ")";
    case Kind::equal_different: {
      auto original = item.bytes;
      if (original.empty()) {
        original.push_back(0);
      }
      auto changed = original;
      const auto position = item.variant == 0   ? 0U
                            : item.variant == 1 ? changed.size() / 2
                                                : changed.size() - 1;
      changed[position] ^= 1;
      return "crypto.timingSafeEqual(" +
             js_view(original, item.prefix, item.suffix) + "," +
             js_view(changed, item.suffix, item.prefix) + ")";
    }
    case Kind::equal_length: {
      auto longer = item.bytes;
      longer.push_back(0);
      return "crypto.timingSafeEqual(" + view + "," +
             js_view(longer, item.suffix, item.prefix) + ")";
    }
    case Kind::equal_wrong:
      return "crypto.timingSafeEqual(" +
             std::string{wrong_value(item.variant)} + "," + view + ")";
    case Kind::equal_missing: return "crypto.timingSafeEqual(" + view + ")";
  }
  throw std::runtime_error("unknown crypto corpus kind");
}

auto expected(const CorpusCase& item) -> std::string {
  switch (item.kind) {
    case Kind::hash_string:
      return "string:" + digest_hex(utf8(item.codepoints), item.algorithm);
    case Kind::hash_view:
      return "string:" + digest_hex(item.bytes, item.algorithm);
    case Kind::hash_bad_algorithm:
    case Kind::random_range: return "error:RangeError";
    case Kind::hash_missing_algorithm:
    case Kind::hash_missing_data:
    case Kind::hash_wrong_algorithm:
    case Kind::hash_wrong_data:
    case Kind::random_wrong:
    case Kind::random_missing:
    case Kind::equal_wrong:
    case Kind::equal_missing: return "error:TypeError";
    case Kind::random_valid: return "Uint8Array:" + std::to_string(item.length);
    case Kind::equal_same: return "boolean:true";
    case Kind::equal_different:
    case Kind::equal_length: return "boolean:false";
  }
  throw std::runtime_error("unknown crypto corpus kind");
}

auto script(const CorpusCase& item) -> std::string {
  return "(() => { try { const result = " + expression(item) +
         "; if (typeof result === 'string') return 'string:' + result;"
         " if (result instanceof Uint8Array) return 'Uint8Array:' +"
         " result.length;"
         " if (typeof result === 'boolean') return 'boolean:' + result;"
         " return 'other';"
         " } catch (error) { return 'error:' + error.name; } })()";
}

struct Failure {
  std::string message;
  std::string signature;
};

auto result_class(std::string_view value) -> std::string {
  if (value.starts_with("error:")) {
    return std::string{value}; // preserve the JS exception class
  }
  return std::string{value.substr(0, value.find(':'))};
}

auto discrepancy(plinth::js::BridgeContext& context, const CorpusCase& item)
    -> std::optional<Failure> {
  auto result = plinth::js::eval_on_context(context, script(item));
  const auto want = expected(item);
  if (!result.has_value()) {
    return Failure{.message = "host eval failed: " + result.error().message +
                              "; expected " + want,
                   .signature = "host_eval:" + std::to_string(static_cast<int>(
                                                   result.error().kind))};
  }
  if (!result->isString()) {
    return Failure{.message =
                       "host eval returned a non-string; expected " + want,
                   .signature = "non_string"};
  }
  const auto got = result->asString();
  if (got != want) {
    return Failure{.message = "expected " + want + ", got " + got,
                   .signature = "mismatch:" + result_class(want) + "/" +
                                result_class(got)};
  }
  return std::nullopt;
}

auto describe(const CorpusCase& item) -> std::string {
  std::ostringstream stream;
  stream << "kind=" << static_cast<int>(item.kind)
         << " algorithm=" << item.algorithm
         << " codepoints=" << item.codepoints.size()
         << " bytes=" << item.bytes.size() << " prefix=" << item.prefix
         << " suffix=" << item.suffix << " n=" << item.length
         << " variant=" << item.variant << " expression=" << expression(item);
  return stream.str();
}

auto shrink_candidates(const CorpusCase& item) -> std::vector<CorpusCase> {
  std::vector<CorpusCase> result;
  if (!item.codepoints.empty()) {
    auto candidate = item;
    candidate.codepoints.resize(item.codepoints.size() / 2);
    result.push_back(std::move(candidate));
  }
  if (!item.bytes.empty()) {
    auto candidate = item;
    candidate.bytes.resize(item.bytes.size() / 2);
    result.push_back(std::move(candidate));
  }
  if (item.prefix != 0 || item.suffix != 0) {
    auto candidate = item;
    candidate.prefix = 0;
    candidate.suffix = 0;
    result.push_back(std::move(candidate));
  }
  if (item.variant != 0) {
    auto candidate = item;
    candidate.variant = 0;
    result.push_back(std::move(candidate));
  }
  if (item.length != 1) {
    auto candidate = item;
    candidate.length = item.kind == Kind::random_range ? 0 : 1;
    result.push_back(std::move(candidate));
  }
  return result;
}

auto shrink(CorpusCase item, std::string_view signature)
    -> std::pair<CorpusCase, int> {
  int attempts = 0;
  while (attempts < MAX_SHRINK_ATTEMPTS) {
    bool improved = false;
    for (auto& candidate : shrink_candidates(item)) {
      if (attempts >= MAX_SHRINK_ATTEMPTS) {
        break;
      }
      ++attempts;
      plinth::Config config{};
      plinth::js::RuntimePool pool(
          nullptr, plinth::js::default_runtime_limits(), config, 1);
      auto* context = pool.acquire();
      if (context == nullptr) {
        break;
      }
      auto failure = discrepancy(*context, candidate);
      pool.destroy(context);
      if (!pool.shutdown(SHUTDOWN_BOUND)) {
        break;
      }
      if (failure.has_value() && failure->signature == signature) {
        item = std::move(candidate);
        improved = true;
        break;
      }
    }
    if (!improved) {
      break;
    }
  }
  return {std::move(item), attempts};
}

} // namespace

TEST_CASE("QuickJS crypto host callbacks have a bounded deterministic corpus",
          "[js][stdlib][crypto][corpus]") {
  INFO("seed_count=" << SEEDS.size() << " cases_per_seed=" << CASES_PER_SEED
                     << " max_input_bytes=" << MAX_INPUT_BYTES
                     << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS
                     << " shutdown_ms=" << SHUTDOWN_BOUND.count());
  for (const auto seed : SEEDS) {
    INFO("seed=" << seed);
    plinth::Config config{};
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    for (int index = 0; index < CASES_PER_SEED; ++index) {
      INFO("case=" << index);
      auto item = generated_case(seed, index);
      auto* context = pool.acquire();
      REQUIRE(context != nullptr);
      auto failed = discrepancy(*context, item);
      if (index % 3 == 0) {
        pool.destroy(context);
      } else {
        pool.release(context);
      }
      if (index % 8 == 7) {
        pool.rebuild();
      }
      if (failed.has_value()) {
        auto [minimal, attempts] = shrink(item, failed->signature);
        INFO("seed=" << seed << " case=" << index << " signature="
                     << failed->signature << " shrink_attempts=" << attempts
                     << " minimal=" << describe(minimal));
        FAIL(failed->message);
      }
    }
    REQUIRE(pool.active_count() == 0);
    auto* held = pool.acquire();
    REQUIRE(held != nullptr);
    REQUIRE_FALSE(pool.shutdown(25ms));
    pool.destroy(held);
    REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
    REQUIRE(pool.acquire() == nullptr);
  }
}
