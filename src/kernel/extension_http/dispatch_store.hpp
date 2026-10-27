// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/config.hpp"
#include "kernel/extension_http/ownership.hpp"

#include <libpq-fe.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace plinth::extension_http {

enum class StoreError {
  INVALID,
  BUSY,
  UNAVAILABLE,
  UNKNOWN,
  CONFLICT,
  FENCED,
  EXISTING_STATE,
  PROOF_REQUIRED
};
enum class AuthorizationState { PREPARED, ARMED, DISPATCHED, UNKNOWN, ABORTED };
enum class PhysicalEntry { UNKNOWN, KNOWN_NOT_ENTERED, KNOWN_ENTERED };
enum class StorePhase {
  PREPARE,
  ARM,
  AUTHORIZATION,
  CHOICE,
  OBSERVATION,
  TERMINAL
};
enum class HttpAdapterMode { QUICKJS, SIDECAR, BUNDLED_NATIVE };

// Captured kernel data, not authority or an adapter-readiness assertion.
// No resource path, query, headers, body, credentials or exception text.
struct DispatchBinding {
  ContextData context;
  std::uint64_t wire_expiry_ns = 0;
  HttpAdapterMode mode = HttpAdapterMode::QUICKJS;
  std::string adapter_binding_id;
  std::string method;
  std::string session_id;
  std::string ip_address;
  std::string node_id;
  auto operator==(const DispatchBinding&) const -> bool = default;
};
struct StoreFence {
  std::string owner_epoch;
  std::uint64_t version = 1;
  auto operator==(const StoreFence&) const -> bool = default;
};
struct DispatchRecord {
  DispatchBinding binding;
  StoreFence fence;
  std::uint64_t revision = 1;
  AuthorizationState authorization = AuthorizationState::PREPARED;
  std::optional<ChoiceRecord> choice;
  WireObservation wire = WireObservation::NOT_ATTEMPTED;
  PhysicalEntry entry = PhysicalEntry::UNKNOWN;
  std::optional<std::uint64_t> entered_at_ns;
  // Restrictive live FK; immutable binding keeps the historical UUID even
  // after a genuine settled-terminal authority may eventually release it.
  std::optional<std::string> live_generation_receipt_id;
  bool projection_confirmed = false;
  bool pending = true;
  bool retained = false;
  bool settlement_resolved = false;
  auto operator==(const DispatchRecord&) const -> bool = default;
};
struct ExpectedMutation {
  StorePhase phase = StorePhase::PREPARE;
  std::uint64_t pre_revision = 0;
  DispatchRecord post;
  auto operator==(const ExpectedMutation&) const -> bool = default;
};

[[nodiscard]] auto valid_dispatch_binding(const DispatchBinding&) -> bool;
[[nodiscard]] auto valid_dispatch_record(const DispatchRecord&) -> bool;
[[nodiscard]] auto exact_postcondition(const DispatchRecord&,
                                       const ExpectedMutation&) -> bool;
// An absent/sub-millisecond budget never becomes PostgreSQL timeout zero.
[[nodiscard]] auto store_timeout_ms(std::uint64_t now_ns,
                                    std::uint64_t absolute_deadline_ns)
    -> std::optional<std::uint32_t>;
[[nodiscard]] auto schema_sql() -> std::string_view;
// Caller owns an ordinary startup transaction and its OperationScope deadline.
[[nodiscard]] auto ensure_schema(PGconn&) -> std::expected<void, StoreError>;

class DispatchStoreSession;
class DispatchStoreRoot;
class HttpDispatchCoordinator;
class HttpDispatchRecoveryCoordinator;

// Only the future actual common owner can supply these observations. Public
// metadata, row readback and a caller boolean cannot construct them. Until
// that owner is integrated the corresponding mutation APIs fail closed.
class HandoffObservation {
 private:
  friend class HttpDispatchCoordinator;
  friend class DispatchStoreSession;
  HandoffObservation() = default;
};
class ActualSettlementWitness {
 private:
  friend class HttpDispatchCoordinator;
  friend class DispatchStoreSession;
  ActualSettlementWitness() = default;
};
class RecoveryAuthority {
 private:
  friend class HttpDispatchRecoveryCoordinator;
  RecoveryAuthority() = default;
};

template <StorePhase Phase> class DispatchStoreAck {
 public:
  [[nodiscard]] auto record() const -> const DispatchRecord& { return value; }

 private:
  friend class DispatchStoreSession;
  DispatchStoreAck(DispatchRecord value_in,
                   std::weak_ptr<DispatchStoreSession> owner_in)
      : value(std::move(value_in)), owner(std::move(owner_in)) {}
  DispatchRecord value;
  std::weak_ptr<DispatchStoreSession> owner;
};
using PrepareAck = DispatchStoreAck<StorePhase::PREPARE>;
using ArmAck = DispatchStoreAck<StorePhase::ARM>;
using AuthorizationAck = DispatchStoreAck<StorePhase::AUTHORIZATION>;
using ChoiceAck = DispatchStoreAck<StorePhase::CHOICE>;
using ObservationAck = DispatchStoreAck<StorePhase::OBSERVATION>;
using TerminalAck = DispatchStoreAck<StorePhase::TERMINAL>;
template <typename T> using StoreResult = std::expected<T, StoreError>;
template <typename T> using StoreCallback = std::function<void(StoreResult<T>)>;

// Owns private PostgreSQL loops, shared operation owners and accepted leases.
// create/close are lifecycle/off-DbLoop operations. Real closeAllFor success,
// not a timeout/user callback, permits release of durable database owners.
class StoreDatabaseOwner final
    : public lifecycle::AsyncCleanupOwner,
      public std::enable_shared_from_this<StoreDatabaseOwner> {
 public:
  ~StoreDatabaseOwner() override;
  StoreDatabaseOwner(const StoreDatabaseOwner&) = delete;
  auto operator=(const StoreDatabaseOwner&) -> StoreDatabaseOwner& = delete;
  [[nodiscard]] auto close(std::chrono::steady_clock::time_point deadline)
      -> bool;
  auto retry_cleanup(std::chrono::steady_clock::time_point deadline) noexcept
      -> bool override;

 private:
  friend class DispatchStoreRoot;
  friend class DispatchStoreSession;
  struct State;
  explicit StoreDatabaseOwner(std::shared_ptr<State>);
  std::shared_ptr<State> state;
};

// Register ONE root before any database resources/acquisition. Fixed slots
// retain at most eight request domains and one startup domain independently
// of caller/callback ownership. Each domain has one writer connection/loop
// and at most one separately owned readback connection/loop. Normal off-loop
// close removes a slot ONLY after actual closeAllFor success. Failed close
// leaves this preallocated cleanup root reachable by the coordinator.
class DispatchStoreRoot final
    : public lifecycle::AsyncCleanupOwner,
      public std::enable_shared_from_this<DispatchStoreRoot> {
 public:
  [[nodiscard]] static auto create() -> std::shared_ptr<DispatchStoreRoot>;
  // Explicit enclosing owner for component fixtures/services. It must outlive
  // the root: drain this component first, then that same registry. No borrowed
  // registry pointer is retained by native callbacks.
  [[nodiscard]] static auto create(lifecycle::AsyncTaskRegistry& registry)
      -> std::shared_ptr<DispatchStoreRoot>;
  ~DispatchStoreRoot() override;
  [[nodiscard]] auto open(const Config::Database&,
                          std::shared_ptr<AdmissionLease>,
                          std::shared_ptr<lifecycle::AsyncTaskLease>,
                          bool with_readback = true)
      -> std::shared_ptr<StoreDatabaseOwner>;
  [[nodiscard]] auto open_startup(const Config::Database&,
                                  std::shared_ptr<lifecycle::AsyncTaskLease>)
      -> std::shared_ptr<StoreDatabaseOwner>;
  auto close_admission() -> void;
  // Same coordinator deadline across every retained domain, never 5s/domain.
  [[nodiscard]] auto drain(std::chrono::steady_clock::time_point deadline)
      -> bool;
  auto retry_cleanup(std::chrono::steady_clock::time_point deadline) noexcept
      -> bool override;
  [[nodiscard]] auto active_domains() const -> std::size_t;

 private:
  friend class StoreDatabaseOwner;
  struct State;
  DispatchStoreRoot();
  std::shared_ptr<State> state;
  auto open_impl(const Config::Database&, std::shared_ptr<AdmissionLease>,
                 std::shared_ptr<lifecycle::AsyncTaskLease>, bool startup,
                 bool with_readback) -> std::shared_ptr<StoreDatabaseOwner>;
};

// One captured dispatch/live owner; no method mints an Invocation/ticket.
// At most one mutation plus its ONE explicitly requested read-only readback.
class DispatchStoreSession
    : public std::enable_shared_from_this<DispatchStoreSession> {
 public:
  using Clock = ResponseArbiter::Clock;
  [[nodiscard]] static auto create(std::shared_ptr<StoreDatabaseOwner>,
                                   DispatchBinding, Clock)
      -> std::shared_ptr<DispatchStoreSession>;
  ~DispatchStoreSession();
  // Common owner may only shorten an enclosing bound. This bars later SQL;
  // it does not cancel/join a native command or release accepted leases.
  auto shorten_deadline(std::uint64_t absolute_deadline_ns) -> void;
  auto prepare(StoreCallback<PrepareAck>) -> void;
  auto arm(const PrepareAck&, StoreCallback<ArmAck>) -> void;
  auto acknowledge_handoff(const ArmAck&, const HandoffObservation&,
                           StoreCallback<AuthorizationAck>) -> void;
  auto commit_choice(const AuthorizationAck&, const ChoiceRecord&,
                     StoreCallback<ChoiceAck>) -> void;
  auto observe(const ChoiceAck&, WireObservation, PhysicalEntry,
               std::optional<std::uint64_t>, StoreCallback<ObservationAck>)
      -> void;
  auto retain_terminal(const ObservationAck&, const ActualSettlementWitness&,
                       StoreCallback<TerminalAck>) -> void;
  // Only the original operation's captured exact postcondition can ACK.
  // Missing/unchanged rows never start another writer or replay an operation.
  auto readback(StoreCallback<DispatchRecord>) -> void;
  auto recover(const RecoveryAuthority&, StoreCallback<DispatchRecord>) -> void;
  [[nodiscard]] auto expected_mutation() const
      -> std::optional<ExpectedMutation>;

 private:
  struct State;
  explicit DispatchStoreSession(std::shared_ptr<State>);
  std::shared_ptr<State> state;
  auto mutate(ExpectedMutation,
              std::function<void(StoreResult<DispatchRecord>)>) -> void;
};

} // namespace plinth::extension_http
