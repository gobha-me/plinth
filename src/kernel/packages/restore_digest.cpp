#include "kernel/packages/restore_digest.hpp"

#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/packages/detail/restore_json.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::packages::restore {
namespace {

constexpr std::string_view ARTIFACT_DOMAIN = "plinth.restore.artifact";
constexpr std::string_view SCHEMA_DOMAIN = "plinth.restore.schema";

auto unsigned_less(std::string_view left, std::string_view right) -> bool {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
      });
}

class Sha256 {
 public:
  Sha256() : context(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    initialized = context != nullptr &&
                  EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1;
  }

  auto update(std::string_view bytes) -> bool {
    return initialized &&
           (bytes.empty() ||
            EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) == 1);
  }

  template <typename Integer> auto integer(Integer value) -> bool {
    std::array<char, sizeof(Integer)> bytes{};
    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
      bytes.at(bytes.size() - offset - 1) =
          static_cast<char>(value & static_cast<Integer>(0xffU));
      value >>= 8U;
    }
    return update({bytes.data(), bytes.size()});
  }

  auto domain(std::string_view value) -> bool {
    constexpr std::array<char, 1> NUL{'\0'};
    return update(value) && update({NUL.data(), NUL.size()}) &&
           integer(std::uint32_t{1});
  }

  auto finish() -> std::expected<std::string, CodecError> {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (!initialized ||
        EVP_DigestFinal_ex(context.get(), bytes.data(), &length) != 1 ||
        length != 32) {
      return std::unexpected(CodecError::HASHING_FAILED);
    }
    constexpr std::string_view HEX = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (std::size_t index = 0; index < length; ++index) {
      const auto byte = bytes.at(index);
      digest.push_back(HEX[byte >> 4U]);
      digest.push_back(HEX[byte & 0x0fU]);
    }
    return digest;
  }

 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context;
  bool initialized = false;
};

auto add_bounded(std::size_t& total, std::size_t amount, std::size_t cap)
    -> bool {
  if (amount > cap - total) return false;
  total += amount;
  return true;
}

auto validate_path(std::string_view path) -> std::expected<void, CodecError> {
  if (path.size() > INVENTORY_PATH_MAX_BYTES) {
    return std::unexpected(CodecError::LIMIT_EXCEEDED);
  }
  if (path.empty() || path.front() == '/' || path.back() == '/' ||
      !plinth::packages::detail::unicode_scalar_count(path)) {
    return std::unexpected(CodecError::INVALID_INVENTORY);
  }
  for (char ch : path) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U || byte == 0x7fU || byte == '\\') {
      return std::unexpected(CodecError::INVALID_INVENTORY);
    }
  }
  std::size_t components = 0;
  std::size_t start = 0;
  while (start < path.size()) {
    const auto slash = path.find('/', start);
    const auto component = path.substr(
        start, slash == std::string_view::npos ? slash : slash - start);
    if (component.empty() || component == "." || component == "..") {
      return std::unexpected(CodecError::INVALID_INVENTORY);
    }
    if (++components > INVENTORY_MAX_COMPONENTS) {
      return std::unexpected(CodecError::LIMIT_EXCEEDED);
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return {};
}

auto validate_records(std::span<const ArtifactRecord> records,
                      std::size_t content_cap)
    -> std::expected<void, CodecError> {
  if (records.size() > INVENTORY_MAX_RECORDS) {
    return std::unexpected(CodecError::LIMIT_EXCEEDED);
  }
  std::size_t metadata = ARTIFACT_DOMAIN.size() + 1 + 4 + 4;
  std::size_t content = 0;
  for (const auto& record : records) {
    if (auto path = validate_path(record.path); !path) return path;
    if (record.mode > 0777U) {
      return std::unexpected(CodecError::INVALID_INVENTORY);
    }
    if (record.kind == ArtifactKind::DIRECTORY) {
      if (!record.content.empty()) {
        return std::unexpected(CodecError::INVALID_INVENTORY);
      }
    } else if (record.kind == ArtifactKind::REGULAR) {
      if (!add_bounded(content, record.content.size(), content_cap)) {
        return std::unexpected(CodecError::LIMIT_EXCEEDED);
      }
    } else {
      return std::unexpected(CodecError::INVALID_INVENTORY);
    }
    if (!add_bounded(metadata, 1 + 4 + 4, INVENTORY_METADATA_MAX_BYTES) ||
        !add_bounded(metadata, record.path.size(),
                     INVENTORY_METADATA_MAX_BYTES) ||
        (record.kind == ArtifactKind::REGULAR &&
         !add_bounded(metadata, 8, INVENTORY_METADATA_MAX_BYTES))) {
      return std::unexpected(CodecError::LIMIT_EXCEEDED);
    }
  }
  return {};
}

auto valid_context_key(std::string_view value) -> bool {
  return !value.empty() && value.size() <= 64 &&
         value.find('\0') == std::string_view::npos && value != "__proto__" &&
         value != "prototype" && value != "constructor" &&
         plinth::packages::detail::unicode_scalar_count(value).has_value();
}

auto valid_schema(const PanelContextSchema& schema) -> bool {
  if (schema.version != 1 || schema.fields.size() > 16 ||
      schema.persist_keys.size() > 16) {
    return false;
  }
  for (const auto& [key, field] : schema.fields) {
    if (!valid_context_key(key) || field.max_bytes > 512 ||
        field.meaning.empty() || field.meaning.size() > 256 ||
        !plinth::packages::detail::unicode_scalar_count(field.meaning)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < schema.persist_keys.size(); ++index) {
    const auto& key = schema.persist_keys.at(index);
    if (!valid_context_key(key) || !schema.fields.contains(key)) return false;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (schema.persist_keys.at(earlier) == key) return false;
    }
  }
  return true;
}

} // namespace

auto artifact_digest(std::span<const ArtifactRecord> records,
                     std::size_t content_byte_cap)
    -> std::expected<std::string, CodecError> try {
  if (auto validated = validate_records(
          records, std::min(content_byte_cap, INVENTORY_CONTENT_MAX_BYTES));
      !validated) {
    return std::unexpected(validated.error());
  }
  // Every scalar/count/byte bound has passed before allocating the sort index.
  // Only borrowed pointers are indexed; no file payload is copied or retained.
  std::vector<const ArtifactRecord*> sorted;
  sorted.reserve(records.size());
  for (const auto& record : records)
    sorted.push_back(std::addressof(record));
  std::ranges::sort(sorted, [](const auto* left, const auto* right) {
    return unsigned_less(left->path, right->path);
  });
  std::string_view previous;
  for (const auto* record : sorted) {
    if (record->path == previous) {
      return std::unexpected(CodecError::INVALID_INVENTORY);
    }
    previous = record->path;
    auto slash = record->path.rfind('/');
    while (slash != std::string_view::npos) {
      const auto parent = record->path.substr(0, slash);
      const auto found = std::lower_bound(
          sorted.begin(), sorted.end(), parent,
          [](const ArtifactRecord* candidate, std::string_view path) {
            return unsigned_less(candidate->path, path);
          });
      if (found == sorted.end() || (*found)->path != parent ||
          (*found)->kind != ArtifactKind::DIRECTORY) {
        return std::unexpected(CodecError::INVALID_INVENTORY);
      }
      slash = parent.rfind('/');
    }
  }

  Sha256 hash;
  if (!hash.domain(ARTIFACT_DOMAIN) ||
      !hash.integer(static_cast<std::uint32_t>(records.size()))) {
    return std::unexpected(CodecError::HASHING_FAILED);
  }
  for (const auto* record : sorted) {
    const std::array<char, 1> type{static_cast<char>(record->kind)};
    if (!hash.update({type.data(), type.size()}) ||
        !hash.integer(static_cast<std::uint32_t>(record->path.size())) ||
        !hash.update(record->path) || !hash.integer(record->mode)) {
      return std::unexpected(CodecError::HASHING_FAILED);
    }
    if (record->kind == ArtifactKind::REGULAR &&
        (!hash.integer(static_cast<std::uint64_t>(record->content.size())) ||
         !hash.update(record->content))) {
      return std::unexpected(CodecError::HASHING_FAILED);
    }
  }
  return hash.finish();
} catch (const std::bad_alloc&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
}

auto canonical_schema(const PanelContextSchema& schema)
    -> std::expected<std::string, CodecError> try {
  if (!valid_schema(schema)) {
    return std::unexpected(CodecError::INVALID_SCHEMA);
  }
  nlohmann::json fields = nlohmann::json::object();
  for (const auto& [key, field] : schema.fields) {
    fields[key] = {{"required", field.required},
                   {"max_bytes", field.max_bytes},
                   {"meaning", field.meaning}};
  }
  std::vector<std::string_view> keys;
  keys.reserve(schema.persist_keys.size());
  for (const auto& key : schema.persist_keys)
    keys.push_back(key);
  std::ranges::sort(keys, unsigned_less);
  nlohmann::json persisted = nlohmann::json::array();
  for (const auto key : keys)
    persisted.push_back(std::string{key});
  const nlohmann::json document = {{"version", schema.version},
                                   {"fields", std::move(fields)},
                                   {"persist_keys", std::move(persisted)}};
  return detail::canonical_json(document, POLICY_MAX_BYTES);
} catch (const nlohmann::json::exception&) {
  return std::unexpected(CodecError::INVALID_SCHEMA);
} catch (const std::bad_alloc&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
}

auto schema_digest(const PanelContextSchema& schema)
    -> std::expected<std::string, CodecError> try {
  auto canonical = canonical_schema(schema);
  if (!canonical) return std::unexpected(canonical.error());
  Sha256 hash;
  if (!hash.domain(SCHEMA_DOMAIN) || !hash.update(*canonical)) {
    return std::unexpected(CodecError::HASHING_FAILED);
  }
  return hash.finish();
} catch (const nlohmann::json::exception&) {
  return std::unexpected(CodecError::INVALID_SCHEMA);
} catch (const std::bad_alloc&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
} catch (const std::length_error&) {
  return std::unexpected(CodecError::LIMIT_EXCEEDED);
}

} // namespace plinth::packages::restore
