#pragma once

// plinth::security — Unicode invisible-character scanner primitive.
//
// Contract: ICD-0.4.1 §Scanner Primitive. Pure function over UTF-8 source
// bytes; no kernel state, no allocations on the clean path beyond a single
// reserved findings vector. Re-used by Layer 1 (package-install gate) and
// Layer 2 (QuickJS source-load gate); the primitive does not know who
// called it and policy lives at the gate.
//
// Scanned ranges (ICD §Scanned Ranges): variation selectors, bidi
// overrides, zero-width space/joiner family, BOM, language tags. The list
// is comprehensive; the exceeds_threshold verdict is by count, not by
// codepoint identity.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::security {

struct UnicodeFinding {
  std::size_t byte_offset; // offset in UTF-8 source
  std::uint32_t codepoint; // decoded codepoint
  std::string_view
      range_name; // points into a static table; lifetime is program-static

  auto operator==(const UnicodeFinding&) const -> bool = default;
};

struct UnicodeScanResult {
  std::vector<UnicodeFinding> first_findings; // size <= cfg.record_first_n
  std::size_t total_count = 0;                // authoritative count
  bool exceeds_threshold = false;
  std::optional<std::string> decode_error; // populated on malformed UTF-8
};

struct UnicodeScanConfig {
  std::size_t threshold = 50;     // findings at or above this = rejection
  std::size_t record_first_n = 5; // upper bound on first_findings.size()
  bool strict_utf8 = true;        // malformed UTF-8 = reject, not skip
};

// Scan `source` for invisible-Unicode codepoints in the ICD-enumerated
// ranges. Single-pass UTF-8 decoder. Pure function: deterministic,
// thread-safe, no I/O.
//
// On malformed UTF-8 with strict_utf8=true: decode_error is populated
// with a byte-offset + short description and exceeds_threshold is set;
// scanning stops at the malformed byte (total_count may be partial).
// Callers MUST treat decode_error.has_value() as rejection regardless
// of total_count (ICD §Security Constraint 3).
//
// Implementation latitude: early-exit on threshold crossing is permitted
// as an optimization but must continue counting until total_count reaches
// at least threshold * 2 so callers have audit-grade evidence.
[[nodiscard]] auto scan_for_invisible_unicode(std::string_view source,
                                              const UnicodeScanConfig& cfg = {})
    -> UnicodeScanResult;

} // namespace plinth::security
