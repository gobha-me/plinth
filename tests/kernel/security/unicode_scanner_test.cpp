#include "kernel/security/unicode_scanner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using plinth::security::scan_for_invisible_unicode;
using plinth::security::UnicodeScanConfig;

namespace {

// Encode a single codepoint into UTF-8 bytes. Used to build test
// inputs without relying on \uXXXX escapes in source-encoded
// strings (which only work for u8/u16/u32 string literals in C++).
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

auto repeat_codepoint(std::uint32_t cp, std::size_t n) -> std::string {
  auto one = encode_utf8(cp);
  std::string out;
  out.reserve(one.size() * n);
  for (std::size_t i = 0; i < n; ++i) {
    out.append(one);
  }
  return out;
}

} // namespace

// G.01 — Clean ASCII: zero findings, not above threshold.
TEST_CASE("scanner: clean ASCII produces zero findings", "[unicode][scanner]") {
  auto src = std::string(1024, 'a');
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.total_count == 0);
  REQUIRE_FALSE(r.exceeds_threshold);
  REQUIRE(r.first_findings.empty());
  REQUIRE_FALSE(r.decode_error.has_value());
}

// G.02 — Legitimate emoji (thumbs-up + VS-16): one finding, below threshold.
TEST_CASE("scanner: legitimate emoji with one variation selector",
          "[unicode][scanner]") {
  auto src = encode_utf8(0x1F44D) + encode_utf8(0xFE0F); // 👍 + VS-16
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.total_count == 1);
  REQUIRE_FALSE(r.exceeds_threshold);
  REQUIRE(r.first_findings.size() == 1);
  REQUIRE(r.first_findings[0].codepoint == 0xFE0F);
  REQUIRE(r.first_findings[0].range_name == "variation-selector");
}

// G.03 — Threshold boundary: 49 variation selectors do NOT trip.
TEST_CASE("scanner: 49 variation selectors stays below default threshold",
          "[unicode][scanner]") {
  auto src = repeat_codepoint(0xFE0F, 49);
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.total_count == 49);
  REQUIRE_FALSE(r.exceeds_threshold);
}

// G.04 — Threshold boundary: 50 variation selectors trips.
TEST_CASE("scanner: 50 variation selectors trips default threshold",
          "[unicode][scanner]") {
  auto src = repeat_codepoint(0xFE0F, 50);
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.total_count == 50);
  REQUIRE(r.exceeds_threshold);
  REQUIRE(r.first_findings.size() == 5);
  REQUIRE(r.first_findings[0].range_name == "variation-selector");
}

// G.05 — Bidi override burst: classified as bidi-override, trips.
TEST_CASE("scanner: bidi override burst trips threshold",
          "[unicode][scanner]") {
  auto src = repeat_codepoint(0x202E, 55); // RIGHT-TO-LEFT OVERRIDE
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.exceeds_threshold);
  REQUIRE(r.first_findings.size() == 5);
  REQUIRE(r.first_findings[0].range_name == "bidi-override");
}

// G.06 — Mixed-range burst: zero-width + BOM + tag character cumulative
// count crosses threshold; first_findings sample across multiple ranges.
TEST_CASE("scanner: mixed-range burst tallies across families",
          "[unicode][scanner]") {
  std::string src;
  src += repeat_codepoint(0x200B, 20);  // zero-width-space
  src += repeat_codepoint(0xFEFF, 20);  // BOM (counted anywhere)
  src += repeat_codepoint(0xE0020, 20); // tag-character
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.total_count == 60);
  REQUIRE(r.exceeds_threshold);
  REQUIRE_FALSE(r.first_findings.empty());
  // The first 5 findings all come from the leading zero-width-space run.
  REQUIRE(r.first_findings[0].range_name == "zero-width-space");
}

// G.07 — Malformed UTF-8 (overlong NUL) rejects via decode_error.
TEST_CASE("scanner: malformed UTF-8 sets decode_error and rejects",
          "[unicode][scanner]") {
  // 0xC0 0x80 is the overlong (invalid) two-byte encoding of U+0000.
  // Build it byte-by-byte to avoid raw-string-literal NUL-termination
  // and clang-tidy embedded-NUL warnings.
  auto src = std::string{'A', 'B', 'C'};
  src.push_back(static_cast<char>(0xC0));
  src.push_back(static_cast<char>(0x80));
  src += "rest";
  auto r = scan_for_invisible_unicode(src);
  REQUIRE(r.decode_error.has_value());
  REQUIRE(r.exceeds_threshold);
}

// G.08 — first_findings bound: 100 findings cap at record_first_n=5.
TEST_CASE("scanner: first_findings bounded by record_first_n",
          "[unicode][scanner]") {
  auto src = repeat_codepoint(0xFE0F, 100);
  UnicodeScanConfig cfg; // defaults: threshold=50, record_first_n=5
  auto r = scan_for_invisible_unicode(src, cfg);
  // Early-exit at 2× threshold (=100) is allowed; total_count is the
  // authoritative count up to the soft cap. With 100 findings and a
  // cap of 100, total_count equals exactly 100.
  REQUIRE(r.total_count == 100);
  REQUIRE(r.first_findings.size() == 5);
  REQUIRE(r.exceeds_threshold);
}
