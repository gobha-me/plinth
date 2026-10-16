#include "kernel/packages/generation_receipts.hpp"

#include "kernel/db/operations.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>

namespace plinth::packages::generation {
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
constexpr std::size_t HEADER_BYTES =
    std::string_view{"plinth.restore.artifact"}.size() + 1 + 4 + 4;

auto hexadecimal(char value) -> bool {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}
auto digest_valid(std::string_view value) -> bool {
  return value.size() == 64 && std::ranges::all_of(value, hexadecimal);
}
auto uuid_valid(std::string_view value) -> bool {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (!hexadecimal(value[index]))
      return false;
  }
  return true;
}
auto key_valid(const GenerationKey& key) -> bool {
  return uuid_valid(key.package_id) && key.profile == 1 && !key.name.empty() &&
         key.name.size() <= 64 && key.name.find('\0') == std::string::npos &&
         !key.version.empty() && key.version.size() <= 256 &&
         key.version.find('\0') == std::string::npos &&
         detail::unicode_scalar_count(key.name).has_value() &&
         detail::unicode_scalar_count(key.version).has_value();
}
auto receipt_valid(const GenerationReceipt& receipt) -> bool {
  return key_valid(receipt.key) && digest_valid(receipt.artifact_digest) &&
         digest_valid(receipt.declarations_digest) &&
         receipt.record_count <= restore::INVENTORY_MAX_RECORDS &&
         receipt.content_bytes <= restore::INVENTORY_CONTENT_MAX_BYTES &&
         receipt.metadata_bytes >= HEADER_BYTES &&
         receipt.metadata_bytes <= restore::INVENTORY_METADATA_MAX_BYTES;
}
template <typename Integer>
auto decimal(std::string_view text, Integer& value) -> bool {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
auto validate_operation(name_operation::NameOperation& operation,
                        const GenerationKey& key)
    -> std::expected<void, ReceiptError> {
  if (!key_valid(key) || key.name != operation.name())
    return std::unexpected(ReceiptError::INVALID);
  if (auto owned = operation.assert_owned(); !owned)
    return std::unexpected(owned.error() == name_operation::NameError::NOT_OWNED
                               ? ReceiptError::LOCK_NOT_OWNED
                               : ReceiptError::UNAVAILABLE);
  return {};
}

auto read_impl(name_operation::NameOperation& operation,
               const GenerationKey& key)
    -> std::expected<std::optional<GenerationReceipt>, ReceiptError> {
  if (auto valid = validate_operation(operation, key); !valid)
    return std::unexpected(valid.error());
  const std::string profile = std::to_string(key.profile);
  const std::array<const char*, 2> values{key.package_id.c_str(),
                                          profile.c_str()};
  Result result(
      plinth::db::exec_params(
          operation.connection(),
          "SELECT p.name,p.version,r.profile::text,r.artifact_digest,"
          "r.declarations_digest,r.record_count::text,r.content_bytes::text,"
          "r.metadata_bytes::text FROM plinth.packages p "
          "LEFT JOIN plinth.package_artifact_receipts r "
          "ON r.package_id=p.id AND r.profile=$2::integer "
          "WHERE p.id=$1::uuid",
          2, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQnfields(result.get()) != 8)
    return std::unexpected(ReceiptError::UNAVAILABLE);
  if (PQntuples(result.get()) != 1 || PQgetisnull(result.get(), 0, 0) != 0 ||
      PQgetisnull(result.get(), 0, 1) != 0 ||
      PQgetlength(result.get(), 0, 0) != static_cast<int>(key.name.size()) ||
      PQgetlength(result.get(), 0, 1) != static_cast<int>(key.version.size()) ||
      key.name != PQgetvalue(result.get(), 0, 0) ||
      key.version != PQgetvalue(result.get(), 0, 1))
    return std::unexpected(ReceiptError::GENERATION_MISMATCH);
  if (PQgetisnull(result.get(), 0, 2) != 0) return std::nullopt;
  for (int column = 3; column < 8; ++column)
    if (PQgetisnull(result.get(), 0, column) != 0)
      return std::unexpected(ReceiptError::INVALID);
  if (PQgetlength(result.get(), 0, 3) != 64 ||
      PQgetlength(result.get(), 0, 4) != 64 ||
      PQgetlength(result.get(), 0, 2) > 10 ||
      PQgetlength(result.get(), 0, 5) > 10 ||
      PQgetlength(result.get(), 0, 6) > 20 ||
      PQgetlength(result.get(), 0, 7) > 20)
    return std::unexpected(ReceiptError::INVALID);
  GenerationReceipt receipt{.key = key,
                            .artifact_digest = PQgetvalue(result.get(), 0, 3),
                            .declarations_digest =
                                PQgetvalue(result.get(), 0, 4)};
  std::uint32_t stored_profile = 0;
  if (!decimal(PQgetvalue(result.get(), 0, 2), stored_profile) ||
      stored_profile != key.profile ||
      !decimal(PQgetvalue(result.get(), 0, 5), receipt.record_count) ||
      !decimal(PQgetvalue(result.get(), 0, 6), receipt.content_bytes) ||
      !decimal(PQgetvalue(result.get(), 0, 7), receipt.metadata_bytes) ||
      !receipt_valid(receipt))
    return std::unexpected(ReceiptError::INVALID);
  return std::optional<GenerationReceipt>{std::move(receipt)};
}

auto insert_impl(name_operation::NameOperation& operation,
                 const GenerationReceipt& receipt)
    -> std::expected<void, ReceiptError> {
  if (!receipt_valid(receipt)) return std::unexpected(ReceiptError::INVALID);
  if (operation.connection() == nullptr ||
      PQtransactionStatus(operation.connection()) != PQTRANS_INTRANS)
    return std::unexpected(ReceiptError::TRANSACTION_REQUIRED);
  const auto existing = read_impl(operation, receipt.key);
  if (!existing) return std::unexpected(existing.error());
  if (existing->has_value()) {
    if (**existing == receipt) return {};
    return std::unexpected(ReceiptError::CONFLICT);
  }
  const std::array<std::string, 9> parameters{
      receipt.key.package_id,
      receipt.key.name,
      receipt.key.version,
      std::to_string(receipt.key.profile),
      receipt.artifact_digest,
      receipt.declarations_digest,
      std::to_string(receipt.record_count),
      std::to_string(receipt.content_bytes),
      std::to_string(receipt.metadata_bytes)};
  std::array<const char*, 9> values{};
  for (std::size_t index = 0; index < values.size(); ++index)
    values[index] = parameters[index].c_str();
  Result result(
      plinth::db::exec_params(
          operation.connection(),
          "INSERT INTO plinth.package_artifact_receipts "
          "(package_id,profile,artifact_digest,declarations_digest,record_"
          "count,"
          "content_bytes,metadata_bytes) SELECT "
          "p.id,$4::integer,$5,$6,$7::integer,"
          "$8::bigint,$9::integer FROM plinth.packages p "
          "WHERE p.id=$1::uuid AND p.name=$2 AND p.version=$3 "
          "ON CONFLICT(package_id,profile) DO NOTHING RETURNING package_id",
          9, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
    return std::unexpected(ReceiptError::UNAVAILABLE);
  const auto stored = read_impl(operation, receipt.key);
  if (!stored) return std::unexpected(stored.error());
  if (!stored->has_value() || **stored != receipt)
    return std::unexpected(ReceiptError::CONFLICT);
  return {};
}
} // namespace

auto receipt_from_artifact(const CapturedArtifact& artifact,
                           std::string_view declarations_digest)
    -> std::expected<GenerationReceipt, ReceiptError> {
  if (!digest_valid(declarations_digest) || !key_valid(artifact.key()))
    return std::unexpected(ReceiptError::INVALID);
  try {
    GenerationReceipt receipt{
        .key = artifact.key(),
        .artifact_digest = artifact.digest(),
        .declarations_digest = std::string{declarations_digest},
        .record_count = static_cast<std::uint32_t>(artifact.records().size()),
        .content_bytes = 0,
        .metadata_bytes = HEADER_BYTES};
    for (const auto& record : artifact.records()) {
      receipt.metadata_bytes += 1 + 4 + record.path.size() + 4;
      if (record.kind == restore::ArtifactKind::REGULAR) {
        receipt.metadata_bytes += 8;
        receipt.content_bytes += record.content.size();
      }
    }
    if (!receipt_valid(receipt)) return std::unexpected(ReceiptError::INVALID);
    return receipt;
  } catch (const std::bad_alloc&) {
    return std::unexpected(ReceiptError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(ReceiptError::LIMIT_EXCEEDED);
  }
}
auto read_receipt(name_operation::NameOperation& operation,
                  const GenerationKey& key)
    -> std::expected<std::optional<GenerationReceipt>, ReceiptError> {
  try {
    return read_impl(operation, key);
  } catch (const std::bad_alloc&) {
    return std::unexpected(ReceiptError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(ReceiptError::LIMIT_EXCEEDED);
  }
}
auto insert_matching_receipt(name_operation::NameOperation& operation,
                             const GenerationReceipt& receipt)
    -> std::expected<void, ReceiptError> {
  try {
    return insert_impl(operation, receipt);
  } catch (const std::bad_alloc&) {
    return std::unexpected(ReceiptError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(ReceiptError::LIMIT_EXCEEDED);
  }
}

} // namespace plinth::packages::generation
