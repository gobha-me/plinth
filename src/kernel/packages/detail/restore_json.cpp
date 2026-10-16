#include "kernel/packages/detail/restore_json.hpp"

#include "kernel/packages/detail/launcher_validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace plinth::packages::restore::detail {
namespace {

auto unsigned_less(std::string_view left, std::string_view right) -> bool {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
      });
}

auto short_escape(unsigned char byte) -> char {
  switch (byte) {
    case '\b': return 'b';
    case '\t': return 't';
    case '\n': return 'n';
    case '\f': return 'f';
    case '\r': return 'r';
    default: return '\0';
  }
}

template <typename Integer>
auto integer_text(Integer value, std::array<char, 32>& buffer)
    -> std::string_view {
  auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    return {};
  }
  return {buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())};
}

struct Budget {
  std::size_t cap;
  std::size_t bytes = 0;
  std::size_t nodes = 0;

  auto add(std::size_t amount) -> bool {
    if (amount > cap - bytes) {
      return false;
    }
    bytes += amount;
    return true;
  }
};

auto measure_string(std::string_view value, Budget& budget)
    -> std::expected<void, CodecError> {
  // Raw UTF-8 bytes are a lower bound on their JSON encoding size. Check it
  // before scanning a potentially oversized caller-constructed JSON string.
  if (value.size() > budget.cap - budget.bytes || !budget.add(2)) {
    return std::unexpected(CodecError::TOO_LARGE);
  }
  if (!plinth::packages::detail::unicode_scalar_count(value)) {
    return std::unexpected(CodecError::INVALID_JSON);
  }
  for (char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    std::size_t width = 1;
    if (byte == '"' || byte == '\\' || short_escape(byte) != '\0') {
      width = 2;
    } else if (byte < 0x20U) {
      width = 6;
    }
    if (!budget.add(width)) {
      return std::unexpected(CodecError::TOO_LARGE);
    }
  }
  return {};
}

auto measure_value(const nlohmann::json& value, std::size_t depth,
                   Budget& budget) -> std::expected<void, CodecError> {
  if (depth > JSON_MAX_DEPTH || budget.nodes == JSON_MAX_NODES) {
    return std::unexpected(CodecError::LIMIT_EXCEEDED);
  }
  ++budget.nodes;
  if (value.is_string()) {
    return measure_string(value.get_ref<const std::string&>(), budget);
  }
  if (value.is_array() || value.is_object()) {
    if (value.size() > JSON_MAX_NODES - budget.nodes) {
      return std::unexpected(CodecError::LIMIT_EXCEEDED);
    }
    if (!budget.add(2) || (!value.empty() && !budget.add(value.size() - 1))) {
      return std::unexpected(CodecError::TOO_LARGE);
    }
    if (value.is_object()) {
      for (const auto& member : value.items()) {
        if (auto key = measure_string(member.key(), budget); !key) {
          return key;
        }
        if (!budget.add(1)) {
          return std::unexpected(CodecError::TOO_LARGE);
        }
        if (auto child = measure_value(member.value(), depth + 1, budget);
            !child) {
          return child;
        }
      }
    } else {
      for (const auto& child : value) {
        if (auto measured = measure_value(child, depth + 1, budget);
            !measured) {
          return measured;
        }
      }
    }
    return {};
  }
  std::array<char, 32> buffer{};
  std::string_view literal;
  if (value.is_number_unsigned()) {
    literal = integer_text(value.get<std::uint64_t>(), buffer);
  } else if (value.is_number_integer()) {
    literal = integer_text(value.get<std::int64_t>(), buffer);
  } else if (value.is_boolean()) {
    literal = value.get<bool>() ? "true" : "false";
  } else if (value.is_null()) {
    literal = "null";
  } else {
    // Floating, binary and discarded values are not in this codec's shape.
    return std::unexpected(CodecError::INVALID_JSON);
  }
  if (literal.empty()) {
    return std::unexpected(CodecError::INVALID_JSON);
  }
  if (!budget.add(literal.size())) {
    return std::unexpected(CodecError::TOO_LARGE);
  }
  return {};
}

class Encoder {
 public:
  explicit Encoder(std::size_t byte_cap, std::size_t encoded_size)
      : cap(byte_cap) {
    output.reserve(encoded_size);
  }

  auto encode(const nlohmann::json& value) -> bool {
    if (value.is_string()) {
      return string(value.get_ref<const std::string&>());
    }
    if (value.is_object()) {
      struct Member {
        std::string_view key;
        const nlohmann::json* value;
      };
      std::vector<Member> members;
      members.reserve(value.size());
      for (const auto& member : value.items()) {
        members.push_back({member.key(), std::addressof(member.value())});
      }
      std::ranges::sort(members, [](const Member& left, const Member& right) {
        return unsigned_less(left.key, right.key);
      });
      if (!append('{')) return false;
      bool first = true;
      for (const auto& member : members) {
        if (!first && !append(',')) return false;
        first = false;
        if (!string(member.key) || !append(':') || !encode(*member.value)) {
          return false;
        }
      }
      return append('}');
    }
    if (value.is_array()) {
      if (!append('[')) return false;
      bool first = true;
      for (const auto& child : value) {
        if (!first && !append(',')) return false;
        first = false;
        if (!encode(child)) return false;
      }
      return append(']');
    }
    std::array<char, 32> buffer{};
    if (value.is_number_unsigned()) {
      return append(integer_text(value.get<std::uint64_t>(), buffer));
    }
    if (value.is_number_integer()) {
      return append(integer_text(value.get<std::int64_t>(), buffer));
    }
    if (value.is_boolean()) {
      return append(value.get<bool>() ? "true" : "false");
    }
    return append("null"); // Preflight has refused every unsupported type.
  }

  auto take() -> std::string { return std::move(output); }

 private:
  auto append(char value) -> bool {
    if (output.size() == cap) return false;
    output.push_back(value);
    return true;
  }

  auto append(std::string_view value) -> bool {
    if (value.size() > cap - output.size()) return false;
    output.append(value);
    return true;
  }

  auto string(std::string_view value) -> bool {
    constexpr std::string_view HEX = "0123456789abcdef";
    if (!append('"')) return false;
    for (char ch : value) {
      const auto byte = static_cast<unsigned char>(ch);
      if (byte == '"' || byte == '\\') {
        if (!append('\\') || !append(ch)) return false;
      } else if (const char escaped = short_escape(byte); escaped != '\0') {
        if (!append('\\') || !append(escaped)) return false;
      } else if (byte < 0x20U) {
        if (!append("\\u00") || !append(HEX[byte >> 4U]) ||
            !append(HEX[byte & 0x0fU])) {
          return false;
        }
      } else if (!append(ch)) {
        return false;
      }
    }
    return append('"');
  }

  std::size_t cap;
  std::string output;
};

} // namespace

auto canonical_json(const nlohmann::json& value, std::size_t byte_cap)
    -> std::expected<std::string, CodecError> try {
  Budget budget{.cap = byte_cap};
  if (auto measured = measure_value(value, 1, budget); !measured) {
    return std::unexpected(measured.error());
  }
  // Preflight bounds the entire input before key-index allocation or output
  // construction. Appends independently retain the exact caller output cap.
  Encoder encoder(byte_cap, budget.bytes);
  if (!encoder.encode(value)) {
    return std::unexpected(CodecError::TOO_LARGE);
  }
  return encoder.take();
} catch (const nlohmann::json::exception&) {
  return std::unexpected(CodecError::INVALID_JSON);
} catch (const std::bad_alloc&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
}

} // namespace plinth::packages::restore::detail
