#pragma once

#include "kernel/packages/generation_capture.hpp"
#include "kernel/packages/name_operation.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::packages::generation {

struct GenerationReceipt {
  GenerationKey key;
  std::string artifact_digest;
  std::string declarations_digest;
  std::uint32_t record_count = 0;
  std::uint64_t content_bytes = 0;
  std::uint64_t metadata_bytes = 0;
  auto operator==(const GenerationReceipt&) const -> bool = default;
};

enum class ReceiptError {
  INVALID,
  LOCK_NOT_OWNED,
  TRANSACTION_REQUIRED,
  GENERATION_MISMATCH,
  CONFLICT,
  UNAVAILABLE,
  LIMIT_EXCEEDED,
};

// Derives counters and artifact identity from real captured bytes. The caller
// must derive declarations_digest from its validated captured/registered
// declaration binding; this helper confers no provenance or authority.
[[nodiscard]] auto receipt_from_artifact(const CapturedArtifact& artifact,
                                         std::string_view declarations_digest)
    -> std::expected<GenerationReceipt, ReceiptError>;

// All calls are synchronous/thread-confined to the NameOperation's SAME PG
// session. A borrowed token is valid only within its caller-owned lock/session
// scope; ownership is freshly asserted, never recursively reacquired here.
// Reads allow IDLE or INTRANS. Nullopt means no receipt for an existing exact
// UUID/name/version, not a nonexistent/mismatched package.
[[nodiscard]] auto read_receipt(name_operation::NameOperation& operation,
                                const GenerationKey& key)
    -> std::expected<std::optional<GenerationReceipt>, ReceiptError>;

// Requires an existing PQTRANS_INTRANS transaction. No BEGIN, COMMIT, rollback,
// retry, UPDATE, caller-selected role or connection fallback. An existing exact
// receipt is idempotent; any differing field refuses CONFLICT. Only callers
// own activation state/commit acknowledgement/publication/recovery policy.
[[nodiscard]] auto insert_matching_receipt(
    name_operation::NameOperation& operation, const GenerationReceipt& receipt)
    -> std::expected<void, ReceiptError>;

} // namespace plinth::packages::generation
