// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/extension_http/codec.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::extension_http {

inline constexpr std::size_t EXTRA_BYTES_PER_LEASE = 96ULL * 1024 * 1024;
inline constexpr std::size_t OWNER_BOOKKEEPING_BYTES = 4096;
inline constexpr std::size_t PACKAGE_TRANSPORT_BYTES = 256ULL * 1024 * 1024;
inline constexpr std::size_t FEATURE_TRANSPORT_BYTES = 512ULL * 1024 * 1024;

struct CreditState;
struct HttpLeaseState;

// A move-only allocation reservation. Holding even just this token retains
// the feature admission AND process lease. Reserve before allocating/copying;
// the token accounts its own storage as well as the requested payload.
class ExtraMemoryCharge {
 public:
  ~ExtraMemoryCharge();
  ExtraMemoryCharge(ExtraMemoryCharge&& other) noexcept;
  auto operator=(ExtraMemoryCharge&& other) noexcept -> ExtraMemoryCharge&;
  ExtraMemoryCharge(const ExtraMemoryCharge&) = delete;
  auto operator=(const ExtraMemoryCharge&) -> ExtraMemoryCharge& = delete;
  [[nodiscard]] auto bytes() const noexcept -> std::size_t;

 private:
  friend class AdmissionLease;
  ExtraMemoryCharge(std::shared_ptr<HttpLeaseState> state, std::size_t bytes);
  auto release() noexcept -> void;
  std::shared_ptr<HttpLeaseState> state;
  std::size_t charged = 0;
};

class AdmissionLease {
 public:
  AdmissionLease(const AdmissionLease&) = delete;
  auto operator=(const AdmissionLease&) -> AdmissionLease& = delete;
  [[nodiscard]] auto reserve_extra(std::size_t payload_bytes)
      -> std::optional<ExtraMemoryCharge>;
  [[nodiscard]] auto extra_used() const -> std::size_t;

 private:
  friend class CreditPool;
  explicit AdmissionLease(std::shared_ptr<HttpLeaseState> state);
  std::shared_ptr<HttpLeaseState> state;
};

struct CreditSnapshot {
  std::size_t tasks = 0;
  std::size_t borrowed_transport_bytes = 0;
  std::size_t reserved_extra_bytes = 0;
  auto operator==(const CreditSnapshot&) const -> bool = default;
};

// Stable captured package NAME is the quota key, including across upgrades.
// No request body is read to discover its trusted transport length. These
// are feature credits, not a global pre-handler networking/heap guarantee.
class CreditPool {
 public:
  CreditPool();
  ~CreditPool();
  CreditPool(const CreditPool&) = delete;
  auto operator=(const CreditPool&) -> CreditPool& = delete;
  [[nodiscard]] auto try_acquire(
      std::string_view package_name, std::size_t transport_bytes,
      std::shared_ptr<lifecycle::AsyncTaskLease> process_lease)
      -> std::shared_ptr<AdmissionLease>;
  auto close_admission() -> void;
  [[nodiscard]] auto drain(std::chrono::steady_clock::time_point deadline)
      -> bool;
  [[nodiscard]] auto snapshot() const -> CreditSnapshot;

 private:
  std::shared_ptr<CreditState> state;
};

struct DeadlineBounds {
  std::uint64_t admitted_at_ns = 0;
  std::uint64_t execution_cutoff_ns = 0;
  std::uint64_t wire_expiry_ns = 0;
  // An expired enclosing bound rejects admission, never an invalid context
  // with cutoff < admitted_at. Additions saturate rather than wrapping.
  [[nodiscard]] static auto make(
      std::uint64_t admitted_at_ns,
      std::optional<std::uint64_t> enclosing_deadline_ns = std::nullopt)
      -> std::optional<DeadlineBounds>;
};

[[nodiscard]] auto http_drain_deadline(
    std::uint64_t now_ns, std::uint32_t configured_ms = 5000,
    std::optional<std::uint64_t> package_deadline_ns = std::nullopt,
    std::optional<std::uint64_t> coordinator_deadline_ns = std::nullopt)
    -> std::optional<std::uint64_t>;

enum class CancelReason { DEADLINE, CLIENT_GONE, SHUTDOWN, GENERATION_RETIRED };
enum class ChoiceReason {
  HANDLER_RESPONSE,
  HANDLER_FAILED,
  RESPONSE_INVALID,
  ADAPTER_UNAVAILABLE,
  RESOURCE_EXHAUSTED,
  DEADLINE,
  CLIENT_GONE,
  SHUTDOWN,
  GENERATION_RETIRED
};
enum class AdapterFailure {
  HANDLER_FAILED,
  RESPONSE_INVALID,
  ADAPTER_UNAVAILABLE,
  RESOURCE_EXHAUSTED,
  EXECUTION_CANCELLED
};
enum class CandidateDisposition { ACCEPTED, DUPLICATE, RETIRED };
enum class WireObservation {
  NOT_ATTEMPTED,
  ATTEMPT_STARTED,
  WRITE_COMPLETED,
  WRITE_FAILED,
  UNKNOWN
};

struct ChoiceIdentity {
  std::string dispatch_id;
  std::string owner_epoch;
  std::uint64_t version = 1;
  std::string choice_id;
  auto operator==(const ChoiceIdentity&) const -> bool = default;
};

// The durable record excludes response bytes/headers. They remain a captured
// immutable local value and are NEVER reconstructed/replayed after a crash.
struct ChoiceRecord {
  ChoiceIdentity identity;
  std::optional<int> status;
  ChoiceReason reason = ChoiceReason::HANDLER_RESPONSE;
  auto operator==(const ChoiceRecord&) const -> bool = default;
};
struct FrozenChoice {
  ChoiceRecord record;
  // A persistence/transport reader retaining this value also retains physical
  // ownership. These are internal C++ owners, not serialized record fields.
  std::shared_ptr<AdmissionLease> admission;
  ExtraMemoryCharge allocation;
};

// Pure live-owner protocol: it neither implements a durable store nor proves
// adapter/transport settlement. The common dispatcher must obtain actual
// prepare acknowledgement BEFORE authorize_dispatch; adapters must recheck
// entry_permitted IMMEDIATELY on their owned queue before physical entry.
class ResponseArbiter {
 public:
  using Clock = std::function<std::uint64_t()>;
  // Invoked synchronously at the actual kernel wire handoff, under the
  // serialized owner lock, never as an earlier queued-writer permit. This is
  // trusted transport code only; it must not re-enter this owner. Completion
  // callbacks post their observations to an owned queue. Return only actual
  // NOT_ATTEMPTED / ATTEMPT_STARTED / UNKNOWN transport observations.
  using Writer = std::function<WireObservation(const FrozenChoice&)>;

  ResponseArbiter(ChoiceIdentity identity, DeadlineBounds bounds, Clock clock,
                  std::shared_ptr<AdmissionLease> admission);
  ~ResponseArbiter();
  ResponseArbiter(const ResponseArbiter&) = delete;
  auto operator=(const ResponseArbiter&) -> ResponseArbiter& = delete;
  [[nodiscard]] auto authorize_dispatch() -> bool;
  [[nodiscard]] auto entry_permitted() const -> bool;
  [[nodiscard]] auto remaining_execution_ms() const -> std::uint32_t;
  // Decision metadata only. The common owner must separately capture the
  // validated immutable response and its allocation charges before wire use.
  // This primitive neither snapshots response bytes nor validates a DTO.
  [[nodiscard]] auto submit_response(int validated_status)
      -> CandidateDisposition;
  [[nodiscard]] auto submit_failure(AdapterFailure failure)
      -> CandidateDisposition;
  auto cancel(CancelReason reason) -> void;
  auto expire() -> void;
  [[nodiscard]] auto choice() const -> std::shared_ptr<const FrozenChoice>;
  [[nodiscard]] auto cancellation() const -> std::optional<CancelReason>;
  // Call only with an ACTUAL durable commit ACK / authoritative same-record
  // readback. Exact matching is necessary, not storage acknowledgement proof.
  [[nodiscard]] auto confirm_choice(const ChoiceRecord& acknowledged) -> bool;
  [[nodiscard]] auto dispatch_wire(const Writer& writer) -> bool;
  [[nodiscard]] auto observe_wire(const ChoiceRecord& identity,
                                  WireObservation observation) -> bool;
  [[nodiscard]] auto wire_observation() const -> WireObservation;
  [[nodiscard]] auto wire_closed() const -> bool;

 private:
  struct State;
  std::unique_ptr<State> state;
};

} // namespace plinth::extension_http
