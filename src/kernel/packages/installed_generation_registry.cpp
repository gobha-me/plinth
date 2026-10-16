#include "kernel/packages/installed_generation_registry.hpp"

#include "kernel/db/operations.hpp"
#include "kernel/packages/generation_binding.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace plinth::packages::generation {
namespace {
using Error = InstalledGenerationError;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto interrupted(InstalledGenerationRegistry::Deadline deadline,
                 std::stop_token cancellation) -> std::optional<Error> {
  if (cancellation.stop_requested()) return Error::CANCELLED;
  if (std::chrono::steady_clock::now() >= deadline) return Error::DEADLINE;
  return std::nullopt;
}

auto receipt_error(ReceiptError error) -> Error {
  switch (error) {
    case ReceiptError::INVALID: return Error::INVALID;
    case ReceiptError::TRANSACTION_REQUIRED: return Error::TRANSACTION_REQUIRED;
    case ReceiptError::CONFLICT:
    case ReceiptError::GENERATION_MISMATCH: return Error::MISMATCH;
    case ReceiptError::LOCK_NOT_OWNED:
    case ReceiptError::UNAVAILABLE: return Error::UNAVAILABLE;
    case ReceiptError::LIMIT_EXCEEDED: return Error::BUSY;
  }
  return Error::UNAVAILABLE;
}

// Selectors here are kernel metadata, not filesystem paths supplied by HTTP.
// Still reject separators/dot-components BEFORE constructing an exact root.
auto selectors_valid(const GenerationKey& key) -> bool {
  const auto lower = [](char byte) { return byte >= 'a' && byte <= 'z'; };
  const auto digit = [](char byte) { return byte >= '0' && byte <= '9'; };
  return !key.name.empty() && key.name.size() <= 64 &&
         lower(key.name.front()) &&
         std::ranges::all_of(key.name,
                             [&](char byte) {
                               return lower(byte) || digit(byte) || byte == '-';
                             }) &&
         !key.version.empty() && key.version.size() <= 256 &&
         digit(key.version.front()) &&
         std::ranges::all_of(key.version,
                             [&](char byte) {
                               return lower(byte) || digit(byte) ||
                                      (byte >= 'A' && byte <= 'Z') ||
                                      byte == '-' || byte == '+' || byte == '.';
                             }) &&
         key.profile == 1;
}

auto check_state(name_operation::NameOperation& operation,
                 const GenerationKey& key, bool staging)
    -> std::expected<void, Error> {
  // read_receipt establishes actual name ownership and exact UUID/name/version
  // syntax before this UUID cast. It neither changes state nor fills absence.
  const auto receipt = read_receipt(operation, key);
  if (!receipt) return std::unexpected(receipt_error(receipt.error()));
  const char* id = key.package_id.c_str();
  const Result result{
      db::exec_params(operation.connection(),
                      "SELECT state FROM plinth.packages WHERE id=$1::uuid", 1,
                      nullptr, &id, nullptr, nullptr, 0),
      PQclear};
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 1 || PQgetisnull(result.get(), 0, 0))
    return std::unexpected(Error::UNAVAILABLE);
  const std::string_view state{PQgetvalue(result.get(), 0, 0)};
  if (state != "ACTIVE" && state != "ACTIVE_FLAGGED" &&
      !(staging && state == "ACTIVATING"))
    return std::unexpected(Error::MISMATCH);
  return {};
}

auto derive_receipt(name_operation::NameOperation& operation,
                    const CapturedArtifact& artifact, Budget& budget)
    -> std::expected<GenerationReceipt, Error> {
  const auto binding = bind_registered_generation(operation, artifact, budget);
  if (!binding) {
    switch (binding.error()) {
      case BindingError::LIMIT_EXCEEDED: return std::unexpected(Error::BUSY);
      case BindingError::UNAVAILABLE:
      case BindingError::LOCK_NOT_OWNED:
        return std::unexpected(Error::UNAVAILABLE);
      case BindingError::INVALID_CAPTURE:
        return std::unexpected(Error::INVALID);
      case BindingError::GENERATION_MISMATCH:
      case BindingError::DECLARATION_MISMATCH:
        return std::unexpected(Error::MISMATCH);
    }
    return std::unexpected(Error::UNAVAILABLE);
  }
  auto receipt = receipt_from_artifact(artifact, *binding);
  if (!receipt) return std::unexpected(receipt_error(receipt.error()));
  return std::move(*receipt);
}
} // namespace

PreparedGeneration::PreparedGeneration(
    std::shared_ptr<const CapturedArtifact> artifact, GenerationReceipt receipt,
    BudgetTicket charge)
    : metadata(std::move(charge)), captured(std::move(artifact)),
      proof(std::move(receipt)) {
}

InstalledGenerationRegistry::InstalledGenerationRegistry(
    std::filesystem::path root, Budget source_budget)
    : data_dir(std::filesystem::absolute(root)),
      budget(std::move(source_budget)), cache(budget) {
}

auto PreparedGeneration::operator=(PreparedGeneration&& other) noexcept
    -> PreparedGeneration& {
  // The retired receipt keeps its metadata charge until its strings disappear.
  using std::swap;
  swap(metadata, other.metadata);
  swap(captured, other.captured);
  swap(proof, other.proof);
  return *this;
}

auto InstalledGenerationRegistry::capture(const GenerationKey& key,
                                          Deadline deadline,
                                          std::stop_token cancellation)
    -> std::expected<std::shared_ptr<const CapturedArtifact>, Error> {
  if (!selectors_valid(key)) return std::unexpected(Error::INVALID);
  if (const auto error = interrupted(deadline, cancellation))
    return std::unexpected(*error);
  if (capturing.test_and_set()) return std::unexpected(Error::BUSY);
  struct CaptureLane {
    std::atomic_flag& capturing;
    ~CaptureLane() { capturing.clear(); }
  } lane{capturing};
  auto ticket = cache.reserve_capture();
  if (!ticket) return std::unexpected(Error::BUSY);
  auto captured = capture_artifact(
      {.key = key,
       .root = data_dir / "extensions" / key.name / key.version,
       .deadline = deadline,
       .cancellation = cancellation},
      std::move(*ticket));
  if (!captured) {
    switch (captured.error()) {
      case CaptureError::CANCELLED: return std::unexpected(Error::CANCELLED);
      case CaptureError::DEADLINE: return std::unexpected(Error::DEADLINE);
      case CaptureError::BUSY: return std::unexpected(Error::BUSY);
      case CaptureError::CHANGED: return std::unexpected(Error::MISMATCH);
      case CaptureError::INVALID:
      case CaptureError::TOO_LARGE:
        return std::unexpected(Error::PROFILE_UNAVAILABLE);
      default: return std::unexpected(Error::UNAVAILABLE);
    }
  }
  return std::move(*captured);
}

auto InstalledGenerationRegistry::stage_locked(
    name_operation::NameOperation& operation, const GenerationKey& key,
    Deadline deadline, std::stop_token cancellation)
    -> std::expected<PreparedGeneration, Error> try {
  db::OperationScope scope{cancellation, deadline};
  if (const auto error = interrupted(deadline, cancellation))
    return std::unexpected(*error);
  if (!selectors_valid(key)) return std::unexpected(Error::INVALID);
  if (PQtransactionStatus(operation.connection()) != PQTRANS_INTRANS)
    return std::unexpected(Error::TRANSACTION_REQUIRED);
  if (auto current = check_state(operation, key, true); !current)
    return std::unexpected(current.error());
  // Finite control-copy envelope retained with the staged receipt. Source
  // capture and the binding adapter reserve their separate bounded envelopes.
  auto metadata = budget.try_reserve(std::size_t{4} * 1024);
  if (!metadata) return std::unexpected(Error::BUSY);
  auto captured = capture(key, deadline, cancellation);
  if (!captured) return std::unexpected(captured.error());
  auto receipt = derive_receipt(operation, **captured, budget);
  if (!receipt) return std::unexpected(receipt.error());
  if (const auto error = interrupted(deadline, cancellation))
    return std::unexpected(*error);
  if (auto inserted = insert_matching_receipt(operation, *receipt); !inserted)
    return std::unexpected(receipt_error(inserted.error()));
  scope.checkpoint();
  return PreparedGeneration{std::move(*captured), std::move(*receipt),
                            std::move(*metadata)};
} catch (...) {
  return std::unexpected(Error::UNAVAILABLE);
}

auto InstalledGenerationRegistry::publish_committed_locked(
    name_operation::NameOperation& operation,
    const PreparedGeneration& prepared, Deadline deadline,
    std::stop_token cancellation) -> std::expected<void, Error> try {
  db::OperationScope scope{cancellation, deadline};
  if (const auto error = interrupted(deadline, cancellation))
    return std::unexpected(*error);
  if (PQtransactionStatus(operation.connection()) != PQTRANS_IDLE)
    return std::unexpected(Error::TRANSACTION_REQUIRED);
  if (!prepared.artifact() || !prepared.artifact()->belongs_to(budget))
    return std::unexpected(Error::INVALID);
  const auto& key = prepared.receipt().key;
  if (auto current = check_state(operation, key, false); !current)
    return std::unexpected(current.error());
  const auto stored = read_receipt(operation, key);
  if (!stored) return std::unexpected(receipt_error(stored.error()));
  if (!stored->has_value()) return std::unexpected(Error::RECEIPT_REQUIRED);
  const auto actual = derive_receipt(operation, *prepared.artifact(), budget);
  if (!actual) return std::unexpected(actual.error());
  if (**stored != prepared.receipt() || *actual != prepared.receipt())
    return std::unexpected(Error::MISMATCH);
  scope.checkpoint();
  if (!cache.publish(prepared.artifact())) return std::unexpected(Error::BUSY);
  return {};
} catch (...) {
  return std::unexpected(Error::UNAVAILABLE);
}

auto InstalledGenerationRegistry::current_locked(
    name_operation::NameOperation& operation, const GenerationKey& key,
    Deadline deadline, std::stop_token cancellation)
    -> std::expected<std::shared_ptr<const CapturedArtifact>, Error> try {
  db::OperationScope scope{cancellation, deadline};
  if (const auto error = interrupted(deadline, cancellation))
    return std::unexpected(*error);
  // READ COMMITTED/IDLE ensures a caller cannot offer a retained
  // repeatable-read snapshot as fresh generation authority. It also rules out
  // uncommitted stage.
  if (PQtransactionStatus(operation.connection()) != PQTRANS_IDLE)
    return std::unexpected(Error::TRANSACTION_REQUIRED);
  if (!selectors_valid(key)) return std::unexpected(Error::INVALID);
  if (auto current = check_state(operation, key, false); !current)
    return std::unexpected(current.error());
  const auto stored = read_receipt(operation, key);
  if (!stored) return std::unexpected(receipt_error(stored.error()));
  if (!stored->has_value()) return std::unexpected(Error::RECEIPT_REQUIRED);
  auto captured = cache.find(key, (**stored).artifact_digest);
  if (!captured) {
    auto cold = capture(key, deadline, cancellation);
    if (!cold) return std::unexpected(cold.error());
    captured = std::move(*cold);
  }
  const auto actual = derive_receipt(operation, *captured, budget);
  if (!actual) return std::unexpected(actual.error());
  if (*actual != **stored) return std::unexpected(Error::MISMATCH);
  scope.checkpoint();
  if (!cache.publish(captured)) return std::unexpected(Error::BUSY);
  return captured;
} catch (...) {
  return std::unexpected(Error::UNAVAILABLE);
}

auto InstalledGenerationRegistry::retire(std::string_view package_id) -> void {
  cache.retire(package_id);
}
auto InstalledGenerationRegistry::clear() -> void {
  cache.clear();
}

} // namespace plinth::packages::generation
