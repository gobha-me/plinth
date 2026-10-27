#pragma once

#include "kernel/packages/http_declarations.hpp"
#include "kernel/packages/name_operation.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages {
class AuthorizedHttpSubmission;
namespace generation {
struct GenerationKey;
}
} // namespace plinth::packages

namespace plinth::packages::http_claims {

enum class Error {
  INVALID,
  UNAVAILABLE,
  BUSY,
  DEADLINE,
  CANCELLED,
  LOCK_NOT_OWNED,
  TRANSACTION_REQUIRED,
  STALE_TRANSACTION,
  GENERATION_MISMATCH,
  AUTHORIZATION_MISMATCH,
  CONFLICT,
  SCHEMA_MISMATCH
};
template <typename T> using Result = std::expected<T, Error>;

// Distinct PostgreSQL two-int advisory-lock namespace; existing package-name
// advisory locks use the bigint namespace. Reserved solely for HTTP claims and
// frontend allocation/cutover cooperation, not a permission or route lock.
inline constexpr std::int32_t GLOBAL_LOCK_CLASS = 0x504c4854;
inline constexpr std::int32_t GLOBAL_LOCK_OBJECT = 0x434c4149;

struct Receipt {
  std::string generation_receipt_id;
  std::string package_id;
  std::string name;
  std::string version;
  std::string provenance;
  std::string artifact_digest;
  std::string registered_declarations_digest;
  std::string http_declarations_digest;
  std::string adapter_descriptor_digest;
  HttpDeclaration declaration;
  std::string adapter_binding_key;
  std::optional<std::string> module_entry;
  auto operator==(const Receipt&) const -> bool = default;
};
enum class ReservationPhase { HELD, RETIRING };
struct Reservation {
  std::string package_id;
  std::vector<std::string> http_prefixes;
  std::string http_declarations_digest;
  std::optional<std::string> generation_receipt_id;
  ReservationPhase phase = ReservationPhase::HELD;
  auto operator==(const Reservation&) const -> bool = default;
};
struct AdapterBinding {
  std::string adapter_epoch;
  std::string generation_receipt_id;
  std::string owner_epoch;
  std::string clock_domain_id;
  std::string adapter_descriptor_digest;
  bool retired = false; // Read observation; inserting a retired tuple refuses.
  auto operator==(const AdapterBinding&) const -> bool = default;
};

// Trusted kernel projection of actual registrations, NOT caller manifest data.
// Fixed/configured registrations (including the boot frontend) MUST have null
// package_id and are never predecessor-exempt. Nonnull owner is permitted only
// for an actually owned mutable trampoline with supported
// withdrawal/retirement. Do not insert the exact '/' redirect as a whole-tree
// frontend root. A genuine frontend '/' or legacy root not provably literal
// fails new HTTP reservation closed; this does not change legacy frontend
// installation/routing semantics.
struct MountedFrontendRoot {
  std::string mount;
  std::optional<std::string> package_id;
};

// All methods are synchronous/thread-confined. Caller owns NameOperation, PG
// session, BEGIN/COMMIT/ROLLBACK, deadline and acknowledgement. This object is
// not an async lifetime or permission. Every use freshly checks name ownership,
// backend/transaction identity, READ COMMITTED and actual global xact lock.
class Transaction {
 public:
  using Deadline = std::chrono::steady_clock::time_point;
  [[nodiscard]] static auto acquire(name_operation::NameOperation& operation,
                                    Deadline deadline,
                                    std::stop_token stop = {})
      -> Result<Transaction>;
  Transaction(const Transaction&) = delete;
  auto operator=(const Transaction&) -> Transaction& = delete;
  Transaction(Transaction&&) noexcept = default;
  auto operator=(Transaction&&) noexcept -> Transaction& = default;
  [[nodiscard]] auto assert_current() const -> Result<void>;
  [[nodiscard]] auto connection() const noexcept -> PGconn*;
  [[nodiscard]] auto name() const noexcept -> std::string_view;

  // Only submitted canonical claim strings are returned, never foreign owner
  // names/IDs. Includes all HELD/RETIRING rows irrespective of package state,
  // exact retained frontend states and supplied actual/configured roots.
  [[nodiscard]] auto conflicts(const AuthorizedHttpSubmission& authorization,
                               std::string_view package_id,
                               std::span<const MountedFrontendRoot> roots) const
      -> Result<std::vector<std::string>>;
  [[nodiscard]] auto reserve(const AuthorizedHttpSubmission& authorization,
                             std::string_view package_id,
                             std::span<const MountedFrontendRoot> roots)
      -> Result<Reservation>;
  [[nodiscard]] auto read_reservation(std::string_view package_id) const
      -> Result<std::optional<Reservation>>;
  [[nodiscard]] auto read_receipt(std::string_view package_id) const
      -> Result<std::optional<Receipt>>;
  // Empty proposed UUID is generated once on the initial INSERT. Exact match
  // returns the EXISTING stored UUID; no UPDATE or authority backfill on
  // ACTIVE. Requires the exact existing profile1 artifact/declaration receipt
  // and an authorized provisional reservation. Captured bytes/physical adapter
  // proof still belong to http_generation/registry, never this value-only
  // record.
  [[nodiscard]] auto insert_or_match_receipt(
      const AuthorizedHttpSubmission& authorization, const Receipt& proposed)
      -> Result<Receipt>;
  [[nodiscard]] auto attach_receipt(
      const AuthorizedHttpSubmission& authorization,
      std::string_view package_id, std::string_view generation_receipt_id)
      -> Result<Reservation>;
  [[nodiscard]] auto read_binding(std::string_view adapter_epoch) const
      -> Result<std::optional<AdapterBinding>>;
  [[nodiscard]] auto insert_or_match_binding(
      const AuthorizedHttpSubmission& authorization,
      const AdapterBinding& proposed) -> Result<AdapterBinding>;

  // Deliberately NO successful release/GC/retirement/restart-publication API.
  // Those require the actual opaque registry physical/durable-reference proof;
  // a bool, row tuple, timeout or absent optional service cannot stand in for
  // it.

 private:
  Transaction(name_operation::NameOperation& operation, Deadline deadline,
              std::stop_token stop, int backend, std::string transaction);
  name_operation::NameOperation* operation;
  Deadline deadline;
  std::stop_token stop;
  int backend;
  std::string transaction;
};

// Fresh committed metadata only, NOT an observed activation COMMIT ACK,
// captured-code owner, physical readiness or publication permission. Requires
// the exact same-name session to be IDLE and actually own its name lock before
// and after one bounded autocommit SELECT. No BEGIN/COMMIT/capture/backfill,
// global-lock acquisition, fallback connection or retry. ACTIVE/ACTIVE_FLAGGED
// must match the exact key and existing artifact/HTTP receipt/HELD attachment.
// A matching legacy row with NEITHER HTTP receipt nor reservation is inert;
// partial/mismatched modern records are unavailable, never legacy absence.
[[nodiscard]] auto read_committed_receipt(
    name_operation::NameOperation& operation,
    const generation::GenerationKey& expected,
    std::chrono::steady_clock::time_point deadline, std::stop_token stop = {})
    -> Result<std::optional<Receipt>>;

// Three independent kernel-owned table definitions. Root bootstrap/schema may
// mirror these exact strings. ensure_schema requires an existing caller-owned
// transaction and checks actual ownership/columns/constraints/FKs against fresh
// independent temporary templates; it never BEGINs/COMMITs/ROLLBACKs/retries.
[[nodiscard]] auto schema_sql() -> std::string;
[[nodiscard]] auto ensure_schema(PGconn& connection) -> Result<void>;

} // namespace plinth::packages::http_claims
