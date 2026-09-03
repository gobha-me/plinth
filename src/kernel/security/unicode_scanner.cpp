#include "kernel/security/unicode_scanner.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace plinth::security {

namespace {

// Static range names — UnicodeFinding::range_name string_views point
// into these literals. Program-lifetime; safe to expose at any call site.
constexpr std::string_view RN_VARIATION_SELECTOR = "variation-selector";
constexpr std::string_view RN_VARIATION_SELECTOR_SUPP =
    "variation-selector-supplement";
constexpr std::string_view RN_ZWS = "zero-width-space";
constexpr std::string_view RN_ZWNJ = "zero-width-non-joiner";
constexpr std::string_view RN_ZWJ = "zero-width-joiner";
constexpr std::string_view RN_LTR_MARK = "ltr-mark";
constexpr std::string_view RN_RTL_MARK = "rtl-mark";
constexpr std::string_view RN_BIDI_OVERRIDE = "bidi-override";
constexpr std::string_view RN_WORD_JOINER = "word-joiner";
constexpr std::string_view RN_INVISIBLE_MATH = "invisible-math-operator";
constexpr std::string_view RN_BOM = "bom";
constexpr std::string_view RN_LANGUAGE_TAG = "language-tag";
constexpr std::string_view RN_TAG_CHARACTER = "tag-character";

// classify_codepoint: return the static range name if cp is in any
// scanned range, else empty string_view.
auto classify_codepoint(std::uint32_t cp) -> std::string_view {
  if (cp >= 0xFE00 && cp <= 0xFE0F) {
    return RN_VARIATION_SELECTOR;
  }
  if (cp >= 0xE0100 && cp <= 0xE01EF) {
    return RN_VARIATION_SELECTOR_SUPP;
  }
  if (cp == 0x200B) {
    return RN_ZWS;
  }
  if (cp == 0x200C) {
    return RN_ZWNJ;
  }
  if (cp == 0x200D) {
    return RN_ZWJ;
  }
  if (cp == 0x200E) {
    return RN_LTR_MARK;
  }
  if (cp == 0x200F) {
    return RN_RTL_MARK;
  }
  if (cp >= 0x202A && cp <= 0x202E) {
    return RN_BIDI_OVERRIDE;
  }
  if (cp == 0x2060) {
    return RN_WORD_JOINER;
  }
  if (cp >= 0x2061 && cp <= 0x2064) {
    return RN_INVISIBLE_MATH;
  }
  if (cp == 0xFEFF) {
    return RN_BOM;
  }
  if (cp == 0xE0001) {
    return RN_LANGUAGE_TAG;
  }
  if (cp >= 0xE0020 && cp <= 0xE007F) {
    return RN_TAG_CHARACTER;
  }
  return {};
}

struct DecodeStep {
  std::uint32_t codepoint;
  std::size_t bytes_consumed;
  bool ok;
  const char* error_kind; // nullptr on ok; static literal on error
};

// Read one continuation byte (10xxxxxx) at src[off]. Returns the
// 6 payload bits via `out`, or false if the byte is missing or
// not a valid continuation.
auto read_continuation(std::string_view src, std::size_t off, std::uint8_t& out)
    -> bool {
  if (off >= src.size()) {
    return false;
  }
  auto b = static_cast<std::uint8_t>(src[off]);
  if ((b & 0xC0) != 0x80) {
    return false;
  }
  out = b & 0x3F;
  return true;
}

auto make_decode_error(const char* kind) -> DecodeStep {
  return {.codepoint = 0, .bytes_consumed = 1, .ok = false, .error_kind = kind};
}

auto make_decode_ok(std::uint32_t cp, std::size_t bytes) -> DecodeStep {
  return {.codepoint = cp,
          .bytes_consumed = bytes,
          .ok = true,
          .error_kind = nullptr};
}

// Decode a 2-byte UTF-8 sequence starting at src[i] with leading byte b0.
auto decode_two(std::string_view src, std::size_t i, std::uint8_t b0)
    -> DecodeStep {
  std::uint8_t c1 = 0;
  if (!read_continuation(src, i + 1, c1)) {
    return make_decode_error("invalid continuation byte");
  }
  auto cp = static_cast<std::uint32_t>(((b0 & 0x1F) << 6) | c1);
  if (cp < 0x80) {
    return {.codepoint = 0,
            .bytes_consumed = 2,
            .ok = false,
            .error_kind = "overlong encoding"};
  }
  return make_decode_ok(cp, 2);
}

// Decode a 3-byte UTF-8 sequence starting at src[i] with leading byte b0.
auto decode_three(std::string_view src, std::size_t i, std::uint8_t b0)
    -> DecodeStep {
  std::uint8_t c1 = 0;
  std::uint8_t c2 = 0;
  if (!read_continuation(src, i + 1, c1) ||
      !read_continuation(src, i + 2, c2)) {
    return make_decode_error("invalid continuation byte");
  }
  auto cp = static_cast<std::uint32_t>(((b0 & 0x0F) << 12) | (c1 << 6) | c2);
  if (cp < 0x800) {
    return {.codepoint = 0,
            .bytes_consumed = 3,
            .ok = false,
            .error_kind = "overlong encoding"};
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return {.codepoint = 0,
            .bytes_consumed = 3,
            .ok = false,
            .error_kind = "surrogate codepoint"};
  }
  return make_decode_ok(cp, 3);
}

// Decode a 4-byte UTF-8 sequence starting at src[i] with leading byte b0.
auto decode_four(std::string_view src, std::size_t i, std::uint8_t b0)
    -> DecodeStep {
  std::uint8_t c1 = 0;
  std::uint8_t c2 = 0;
  std::uint8_t c3 = 0;
  if (!read_continuation(src, i + 1, c1) ||
      !read_continuation(src, i + 2, c2) ||
      !read_continuation(src, i + 3, c3)) {
    return make_decode_error("invalid continuation byte");
  }
  auto cp = static_cast<std::uint32_t>(((b0 & 0x07) << 18) | (c1 << 12) |
                                       (c2 << 6) | c3);
  if (cp < 0x10000) {
    return {.codepoint = 0,
            .bytes_consumed = 4,
            .ok = false,
            .error_kind = "overlong encoding"};
  }
  if (cp > 0x10FFFF) {
    return {.codepoint = 0,
            .bytes_consumed = 4,
            .ok = false,
            .error_kind = "codepoint out of range"};
  }
  return make_decode_ok(cp, 4);
}

// Decode one UTF-8 codepoint at src[i]. Validates: continuation-byte
// shape, no overlong encodings, no surrogates, no codepoints > U+10FFFF.
auto decode_one(std::string_view src, std::size_t i) -> DecodeStep {
  auto b0 = static_cast<std::uint8_t>(src[i]);
  if (b0 < 0x80) {
    return make_decode_ok(b0, 1);
  }
  if (b0 < 0xC2) {
    return make_decode_error("invalid lead byte");
  }
  if (b0 < 0xE0) {
    return decode_two(src, i, b0);
  }
  if (b0 < 0xF0) {
    return decode_three(src, i, b0);
  }
  if (b0 < 0xF5) {
    return decode_four(src, i, b0);
  }
  return make_decode_error("invalid lead byte");
}

auto format_decode_error(const char* kind, std::size_t offset) -> std::string {
  auto s = std::string{kind};
  s += " at offset ";
  s += std::to_string(offset);
  return s;
}

// soft_cap: counting ceiling. Per ICD §Implementation Latitude, the
// scanner may early-exit at 2× threshold so the audit trail still has
// useful evidence without scanning an entire 50 MB file. Threshold 0
// disables the cap (useful for benchmarks).
auto compute_soft_cap(std::size_t threshold) -> std::size_t {
  if (threshold == 0) {
    return 0;
  }
  if (threshold > (SIZE_MAX / 2)) {
    return SIZE_MAX;
  }
  return threshold * 2;
}

} // namespace

auto scan_for_invisible_unicode(std::string_view source,
                                const UnicodeScanConfig& cfg)
    -> UnicodeScanResult {
  UnicodeScanResult out;
  out.first_findings.reserve(cfg.record_first_n);
  auto soft_cap = compute_soft_cap(cfg.threshold);

  std::size_t i = 0;
  while (i < source.size()) {
    auto step = decode_one(source, i);
    if (!step.ok) {
      if (cfg.strict_utf8) {
        out.decode_error = format_decode_error(step.error_kind, i);
        out.exceeds_threshold = true;
        return out;
      }
      i += step.bytes_consumed;
      continue;
    }

    if (auto rn = classify_codepoint(step.codepoint); !rn.empty()) {
      if (out.first_findings.size() < cfg.record_first_n) {
        out.first_findings.push_back(
            {.byte_offset = i, .codepoint = step.codepoint, .range_name = rn});
      }
      ++out.total_count;
    }

    i += step.bytes_consumed;

    if (cfg.threshold > 0 && out.total_count >= soft_cap) {
      break;
    }
  }

  if (cfg.threshold > 0 && out.total_count >= cfg.threshold) {
    out.exceeds_threshold = true;
  }
  return out;
}

} // namespace plinth::security
