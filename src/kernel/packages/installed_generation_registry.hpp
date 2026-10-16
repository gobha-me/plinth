#pragma once

#include "kernel/packages/generation_receipts.hpp"

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace plinth::packages::generation {

enum class InstalledGenerationError {
  INVALID,
  UNAVAILABLE,
  BUSY,
  CANCELLED,
  DEADLINE,
  MISMATCH,
  TRANSACTION_REQUIRED,
  RECEIPT_REQUIRED,
  PROFILE_UNAVAILABLE,
};

// Staged immutable bytes, NOT an activation/COMMIT acknowledgement. Only the
// registry constructs this value after actual capture/registration binding.
class PreparedGeneration {
 public:
  PreparedGeneration(PreparedGeneration&&) noexcept = default;
  auto operator=(PreparedGeneration&& other) noexcept -> PreparedGeneration&;
  PreparedGeneration(const PreparedGeneration&) = delete;
  auto operator=(const PreparedGeneration&) -> PreparedGeneration& = delete;
  [[nodiscard]] auto artifact() const
      -> const std::shared_ptr<const CapturedArtifact>& {
    return captured;
  }
  [[nodiscard]] auto receipt() const -> const GenerationReceipt& {
    return proof;
  }

 private:
  friend class InstalledGenerationRegistry;
  PreparedGeneration(std::shared_ptr<const CapturedArtifact> artifact,
                     GenerationReceipt receipt, BudgetTicket metadata);
  BudgetTicket metadata;
  std::shared_ptr<const CapturedArtifact> captured;
  GenerationReceipt proof;
};

// One process-owned charged cache. It creates NO worker/thread/queue and does
// NO eager all-package scan. Call on owned startup/lifecycle/metadata workers,
// with the caller's SAME thread-confined NameOperation. No callback retains a
// PG session, borrowed transaction, or this registry. Cache eviction does not
// revoke an independently admitted stream/runtime owner.
class InstalledGenerationRegistry {
 public:
  using Deadline = std::chrono::steady_clock::time_point;
  explicit InstalledGenerationRegistry(std::filesystem::path data_dir,
                                       Budget budget = Budget{});
  // Caller holds a transaction and its owning lifecycle/bootstrap protocol:
  // name fence, or name-lock-then-counted-guard for existing v2. Captures the
  // EXACT version (never active symlink), binds registered declarations, and
  // inserts/matches a write-once receipt IN THAT transaction. No COMMIT/cache
  // publication, permission/readiness grant, retry or rollback is performed.
  [[nodiscard]] auto stage_locked(name_operation::NameOperation& operation,
                                  const GenerationKey& key, Deadline deadline,
                                  std::stop_token cancellation = {})
      -> std::expected<PreparedGeneration, InstalledGenerationError>;
  // Only after the caller observed COMMIT: re-read matching receipt/current
  // ACTIVE generation on an IDLE same-name session before evictable
  // publication. A lost COMMIT acknowledgement is not permission to call this
  // implicitly.
  [[nodiscard]] auto publish_committed_locked(
      name_operation::NameOperation& operation,
      const PreparedGeneration& prepared, Deadline deadline,
      std::stop_token cancellation = {})
      -> std::expected<void, InstalledGenerationError>;
  // Missing receipt is unavailable, not absence/automatic mutation. A caller
  // implementing explicit existing-v2 bootstrap owns stage/COMMIT/publication.
  [[nodiscard]] auto current_locked(name_operation::NameOperation& operation,
                                    const GenerationKey& key, Deadline deadline,
                                    std::stop_token cancellation = {})
      -> std::expected<std::shared_ptr<const CapturedArtifact>,
                       InstalledGenerationError>;
  auto retire(std::string_view package_id) -> void;
  auto clear() -> void;
  [[nodiscard]] auto source_budget() const noexcept -> Budget { return budget; }

 private:
  [[nodiscard]] auto capture(const GenerationKey& key, Deadline deadline,
                             std::stop_token cancellation)
      -> std::expected<std::shared_ptr<const CapturedArtifact>,
                       InstalledGenerationError>;
  std::filesystem::path data_dir;
  Budget budget;
  GenerationCache cache;
  // Nonwaiting physical capture admission, not a registry/cache lock held
  // over I/O. Its caller's owned worker/lifecycle lease must join before this
  // registry is destroyed; the registry itself schedules no deferred work.
  std::atomic_flag capturing = ATOMIC_FLAG_INIT;
};

} // namespace plinth::packages::generation
