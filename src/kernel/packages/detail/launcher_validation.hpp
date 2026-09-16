#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace plinth::packages::detail {

// Return the number of Unicode scalar values in a UTF-8 string. Invalid UTF-8,
// overlong encodings, surrogate code points, and values above U+10FFFF fail
// closed. JSON parsing normally guarantees this invariant; keeping the check
// here makes the manifest-field contract explicit and independently testable.
inline auto unicode_scalar_count(std::string_view text)
    -> std::optional<std::size_t> {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size();) {
    auto lead = static_cast<std::uint8_t>(text[i]);
    std::size_t width = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (lead < 0x80) {
      width = 1;
      codepoint = lead;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      width = 2;
      codepoint = lead & 0x1FU;
      minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      width = 3;
      codepoint = lead & 0x0FU;
      minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      width = 4;
      codepoint = lead & 0x07U;
      minimum = 0x10000;
    } else {
      return std::nullopt;
    }
    if (i + width > text.size()) {
      return std::nullopt;
    }
    for (std::size_t offset = 1; offset < width; ++offset) {
      auto byte = static_cast<std::uint8_t>(text[i + offset]);
      if ((byte & 0xC0U) != 0x80U) {
        return std::nullopt;
      }
      codepoint = (codepoint << 6U) | (byte & 0x3FU);
    }
    if ((width > 1 && codepoint < minimum) ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
      return std::nullopt;
    }
    ++count;
    i += width;
  }
  return count;
}

inline auto is_icon_token_valid(std::string_view token) -> bool {
  if (token.empty() || token.size() > 64 || token.front() < 'a' ||
      token.front() > 'z') {
    return false;
  }
  for (char c : token.substr(1)) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
  }
  return true;
}

// RBAC rule-name grammar from rule_validator.cpp (Rule A.1).
inline auto is_rbac_rule_name_valid(std::string_view rule) -> bool {
  std::size_t index = 0;
  std::size_t segments = 0;
  while (index < rule.size()) {
    if (rule[index] < 'a' || rule[index] > 'z') {
      return false;
    }
    ++index;
    while (index < rule.size() && rule[index] != '.') {
      char c = rule[index];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
        return false;
      }
      ++index;
    }
    ++segments;
    if (index < rule.size()) {
      ++index;
      if (index == rule.size()) {
        return false;
      }
    }
  }
  return segments >= 2 && segments <= 5;
}

} // namespace plinth::packages::detail
