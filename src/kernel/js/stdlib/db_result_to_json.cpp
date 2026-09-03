// SPDX-License-Identifier: MIT
//
// See header.

#include "kernel/js/stdlib/db_result_to_json.hpp"

#include <drogon/orm/ResultIterator.h>
#include <drogon/orm/Row.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

namespace plinth::js::db {

namespace {

// ICD-0.5.3 §OID switch table. PG built-in type OIDs. Stable since
// PG 8.0; Plinth's supported PG version is ≥ 14. Defining locally
// rather than pulling in `postgres.h` avoids a server-header
// dependency for a kernel-space client TU.
constexpr int BOOL_OID = 16;
constexpr int BYTEA_OID = 17;
constexpr int CHAR_OID = 18;
constexpr int NAME_OID = 19;
constexpr int INT8_OID = 20;
constexpr int INT2_OID = 21;
constexpr int INT4_OID = 23;
constexpr int TEXT_OID = 25;
constexpr int JSON_OID = 114;
constexpr int FLOAT4_OID = 700;
constexpr int FLOAT8_OID = 701;
constexpr int DATE_OID = 1082;
constexpr int TIME_OID = 1083;
constexpr int TIMESTAMP_OID = 1114;
constexpr int TIMESTAMPTZ_OID = 1184;
constexpr int VARCHAR_OID = 1043;
constexpr int NUMERIC_OID = 1700;
constexpr int UUID_OID = 2950;
constexpr int JSONB_OID = 3802;

// JS Number safe-integer range per ECMA-262 §Number.MAX_SAFE_INTEGER.
constexpr std::int64_t JS_SAFE_INT_MIN = -((std::int64_t{1} << 53) - 1);
constexpr std::int64_t JS_SAFE_INT_MAX = (std::int64_t{1} << 53) - 1;

// feature-flag state
std::atomic<bool> g_oid_mapping_enabled{true};

// ── Heuristic fallback (0.3.3 behavior) ────────────────────────────
//
// Retained verbatim behind `db.oid_mapping.enabled=false`. Misreads
// `SELECT 'true'::text` → bool (`true`) etc; see ICD-0.5.3 §OID-Driven
// PG-Type → JS-Type Mapping §Problem with the 0.3.3 heuristic.
auto heuristic_text_to_json(const std::string& s) -> Json::Value {
  if (s == "t") {
    return Json::Value{true};
  }
  if (s == "f") {
    return Json::Value{false};
  }
  if (s.empty()) {
    return Json::Value{std::string{}};
  }
  std::int64_t ival = 0;
  const auto* data_ptr = s.data();
  const auto* end_ptr = s.data() + s.size();
  auto ires = std::from_chars(data_ptr, end_ptr, ival);
  if (ires.ec == std::errc{} && ires.ptr == end_ptr) {
    if (ival >= JS_SAFE_INT_MIN && ival <= JS_SAFE_INT_MAX) {
      return Json::Value{static_cast<Json::Int64>(ival)};
    }
    return Json::Value{s};
  }
  char* endptr = nullptr;
  double d = std::strtod(s.c_str(), &endptr);
  if (endptr != s.c_str() && endptr == s.c_str() + s.size()) {
    return Json::Value{d};
  }
  return Json::Value{s};
}

// ── OID-driven conversion (0.5.3) ───────────────────────────────────
//
// Per ICD §OID switch table. Unrecognized OIDs fail closed to string
// (rate-limited warn); this matches the ICD posture "any other type
// → string (fail-closed)". Array OIDs (TEXT[] / INT4[] / UUID[]) stay
// in the default arm — array promotion is a future milestone
// (ICD §Array types still deferred).
auto oid_switch_to_json(const drogon::orm::Field& f, int oid) -> Json::Value {
  auto text = f.as<std::string>();

  switch (oid) {
    case BOOL_OID: {
      // Drogon's as<bool>() maps 't'/'1' → true else false. PG
      // text repr for BOOLOID is literally "t" or "f".
      return Json::Value{text == "t"};
    }
    case INT2_OID: {
      std::int16_t v = 0;
      auto r = std::from_chars(text.data(), text.data() + text.size(), v);
      if (r.ec != std::errc{}) {
        return Json::Value{text};
      }
      return Json::Value{static_cast<Json::Int>(v)};
    }
    case INT4_OID: {
      std::int32_t v = 0;
      auto r = std::from_chars(text.data(), text.data() + text.size(), v);
      if (r.ec != std::errc{}) {
        return Json::Value{text};
      }
      return Json::Value{static_cast<Json::Int>(v)};
    }
    case INT8_OID: {
      std::int64_t v = 0;
      auto r = std::from_chars(text.data(), text.data() + text.size(), v);
      if (r.ec != std::errc{}) {
        return Json::Value{text};
      }
      if (v < JS_SAFE_INT_MIN || v > JS_SAFE_INT_MAX) {
        // Outside JS Number safe range → string (lossless).
        return Json::Value{text};
      }
      return Json::Value{static_cast<Json::Int64>(v)};
    }
    case FLOAT4_OID:
    case FLOAT8_OID:
    case NUMERIC_OID: {
      // NUMERIC is technically arbitrary-precision; JS Number is
      // double. ICD §OID switch table documents the lossy
      // NUMERIC → double conversion explicitly.
      char* endptr = nullptr;
      double v = std::strtod(text.c_str(), &endptr);
      if (endptr == text.c_str() || endptr != text.c_str() + text.size()) {
        return Json::Value{text};
      }
      return Json::Value{v};
    }
    case TEXT_OID:
    case VARCHAR_OID:
    case NAME_OID:
    case CHAR_OID:
    case UUID_OID:
    case DATE_OID:
    case TIME_OID:
    case TIMESTAMP_OID:
    case TIMESTAMPTZ_OID:
      // Strings pass through verbatim. String `"true"` stays
      // string — this is the pinned regression vs the 0.3.3
      // heuristic that mis-classified it as bool.
      return Json::Value{text};
    case BYTEA_OID: {
      // Drogon's Field::as<std::string>() already auto-decodes
      // BYTEA when OID==17 (see Drogon Field.cc — hexToBinaryString
      // path). `text` is the RAW bytes, not the `\x...` text
      // repr. Hex-encode those bytes into a tagged Json object;
      // the json_to_js unpacker (bridge_context.cpp) decodes the
      // tag into a JS Uint8Array. This mirrors the write-direction
      // `__bytea__` tag symmetrically.
      std::string hex;
      hex.reserve(text.size() * 2);
      constexpr std::string_view DIGITS = "0123456789abcdef";
      for (unsigned char byte : text) {
        hex.push_back(DIGITS[byte >> 4U]);
        hex.push_back(DIGITS[byte & 0xFU]);
      }
      Json::Value tagged(Json::objectValue);
      // jsoncpp's operator[] takes const-ref; move would be a no-op.
      tagged["__bytea_hex__"] = hex;
      return tagged;
    }
    case JSON_OID:
    case JSONB_OID: {
      // The SQL-level JSON value is already a JSON string; pass
      // through verbatim and let the JS-side `JSON.parse` in the
      // caller handle structure. A future milestone may promote
      // this to a structural Json::Value in the kernel; for now
      // the raw-string posture matches ICD-0.3.3's "unknown
      // structural types stay string" convention, but see the
      // note at ICD-0.5.3 §OID switch table for the planned
      // promotion when array-type support lands.
      return Json::Value{text};
    }
    default: {
      // Unknown OID: fail closed to string + rate-limited warn.
      // Rate limiting TODO — phase 2 introduces the audit
      // rate-limit plumbing. For 0.5.3 phase 1 a plain debug log
      // here is acceptable because any production encounter
      // with an unknown OID is a development-time signal.
      spdlog::debug("db.oid_mapping.unknown_oid={} text_len={}", oid,
                    text.size());
      return Json::Value{text};
    }
  }
}

} // namespace

auto set_oid_mapping_enabled(bool enabled) -> void {
  g_oid_mapping_enabled.store(enabled, std::memory_order_release);
}

auto oid_mapping_enabled() -> bool {
  return g_oid_mapping_enabled.load(std::memory_order_acquire);
}

auto field_to_json(const drogon::orm::Field& field) -> Json::Value {
  if (field.isNull()) {
    return Json::Value{Json::nullValue};
  }
  if (g_oid_mapping_enabled.load(std::memory_order_acquire)) {
    return oid_switch_to_json(field, field.oid());
  }
  return heuristic_text_to_json(field.as<std::string>());
}

auto result_to_json(const drogon::orm::Result& result) -> Json::Value {
  Json::Value rows{Json::arrayValue};
  for (const auto& row : result) {
    Json::Value row_obj{Json::objectValue};
    for (std::size_t i = 0; i < row.size(); ++i) {
      const auto& field = row[i];
      const char* name = field.name();
      std::string col =
          (name != nullptr) ? std::string{name} : std::to_string(i);
      row_obj[col] = field_to_json(field);
    }
    rows.append(std::move(row_obj));
  }
  Json::Value out{Json::objectValue};
  out["rows"] = std::move(rows);
  out["row_count"] = static_cast<Json::Int>(result.size());
  return out;
}

} // namespace plinth::js::db
