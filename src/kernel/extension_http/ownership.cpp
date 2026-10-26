// SPDX-License-Identifier: MIT
#include "kernel/extension_http/ownership.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace plinth::extension_http {
namespace {
constexpr std::size_t PACKAGE_TASKS = 4;
constexpr std::size_t FEATURE_TASKS = 8;
constexpr std::uint64_t NS_PER_MS = 1000000;

auto saturated_add(std::uint64_t left, std::uint64_t right) -> std::uint64_t {
  return left > std::numeric_limits<std::uint64_t>::max() - right
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}
} // namespace

struct CreditState {
  struct Package {
    std::string name;
    std::size_t tasks = 0;
    std::size_t transport = 0;
  };
  std::mutex mutex;
  std::condition_variable changed;
  bool accepting = true;
  CreditSnapshot totals;
  std::array<Package, FEATURE_TASKS> packages;
};

struct HttpLeaseState {
  std::shared_ptr<CreditState> pool;
  std::shared_ptr<lifecycle::AsyncTaskLease> process;
  std::size_t slot = 0;
  std::size_t transport = 0;
  std::size_t extra = OWNER_BOOKKEEPING_BYTES;
  bool admitted = false;

  ~HttpLeaseState() {
    if (!admitted) {
      return;
    }
    {
      const std::lock_guard lock(pool->mutex);
      auto& package = pool->packages[slot];
      --package.tasks;
      package.transport -= transport;
      --pool->totals.tasks;
      pool->totals.borrowed_transport_bytes -= transport;
      pool->totals.reserved_extra_bytes -= EXTRA_BYTES_PER_LEASE;
      if (package.tasks == 0) {
        package.name.clear();
      }
    }
    pool->changed.notify_all();
    // The process lease is released AFTER the feature counters and mutex.
  }
};

ExtraMemoryCharge::ExtraMemoryCharge(std::shared_ptr<HttpLeaseState> state_in,
                                     std::size_t bytes_in)
    : state(std::move(state_in)), charged(bytes_in) {
}
ExtraMemoryCharge::~ExtraMemoryCharge() {
  release();
}
ExtraMemoryCharge::ExtraMemoryCharge(ExtraMemoryCharge&& other) noexcept
    : state(std::move(other.state)), charged(std::exchange(other.charged, 0)) {
}
auto ExtraMemoryCharge::operator=(ExtraMemoryCharge&& other) noexcept
    -> ExtraMemoryCharge& {
  if (this != &other) {
    release();
    state = std::move(other.state);
    charged = std::exchange(other.charged, 0);
  }
  return *this;
}
auto ExtraMemoryCharge::bytes() const noexcept -> std::size_t {
  return charged;
}
auto ExtraMemoryCharge::release() noexcept -> void {
  if (state) {
    auto owner = std::move(state);
    {
      const std::lock_guard lock(owner->pool->mutex);
      owner->extra -= charged;
      charged = 0;
    }
    // owner may be the final physical lease; destroy it outside the lock.
  }
}

AdmissionLease::AdmissionLease(std::shared_ptr<HttpLeaseState> state_in)
    : state(std::move(state_in)) {
}
auto AdmissionLease::reserve_extra(std::size_t payload_bytes)
    -> std::optional<ExtraMemoryCharge> {
  const std::lock_guard lock(state->pool->mutex);
  constexpr auto OVERHEAD = sizeof(ExtraMemoryCharge);
  if (payload_bytes == 0 || payload_bytes > EXTRA_BYTES_PER_LEASE - OVERHEAD) {
    return std::nullopt;
  }
  const auto bytes = payload_bytes + OVERHEAD;
  if (bytes > EXTRA_BYTES_PER_LEASE - state->extra) {
    return std::nullopt;
  }
  state->extra += bytes;
  return ExtraMemoryCharge(state, bytes);
}
auto AdmissionLease::extra_used() const -> std::size_t {
  const std::lock_guard lock(state->pool->mutex);
  return state->extra;
}

CreditPool::CreditPool() : state(std::make_shared<CreditState>()) {
}
CreditPool::~CreditPool() {
  close_admission();
}
auto CreditPool::try_acquire(
    std::string_view package_name, std::size_t transport_bytes,
    std::shared_ptr<lifecycle::AsyncTaskLease> process_lease)
    -> std::shared_ptr<AdmissionLease> {
  if (!process_lease || !valid_name(package_name) ||
      transport_bytes > PACKAGE_TRANSPORT_BYTES) {
    return nullptr;
  }
  const std::lock_guard lock(state->mutex);
  if (!state->accepting || state->totals.tasks == FEATURE_TASKS ||
      transport_bytes >
          FEATURE_TRANSPORT_BYTES - state->totals.borrowed_transport_bytes) {
    return nullptr;
  }
  auto slot = state->packages.size();
  for (std::size_t index = 0; index < state->packages.size(); ++index) {
    const auto& package = state->packages[index];
    if (package.tasks != 0 && package.name == package_name) {
      slot = index;
      break;
    }
    if (package.tasks == 0 && slot == state->packages.size()) {
      slot = index;
    }
  }
  if (slot == state->packages.size()) {
    return nullptr;
  }
  auto& package = state->packages[slot];
  if (package.tasks == PACKAGE_TASKS ||
      transport_bytes > PACKAGE_TRANSPORT_BYTES - package.transport) {
    return nullptr;
  }
  // Allocate every potentially throwing object BEFORE changing counters.
  // A rejected/throwing construction has not admitted a physical task.
  auto owner = std::make_shared<HttpLeaseState>();
  owner->pool = state;
  owner->process = std::move(process_lease);
  owner->slot = slot;
  owner->transport = transport_bytes;
  auto lease = std::shared_ptr<AdmissionLease>(new AdmissionLease(owner));
  if (package.tasks == 0) {
    package.name = package_name;
  }
  ++package.tasks;
  package.transport += transport_bytes;
  ++state->totals.tasks;
  state->totals.borrowed_transport_bytes += transport_bytes;
  state->totals.reserved_extra_bytes += EXTRA_BYTES_PER_LEASE;
  owner->admitted = true;
  return lease;
}
auto CreditPool::close_admission() -> void {
  const std::lock_guard lock(state->mutex);
  state->accepting = false;
}
auto CreditPool::drain(std::chrono::steady_clock::time_point deadline) -> bool {
  std::unique_lock lock(state->mutex);
  state->accepting = false;
  return state->changed.wait_until(lock, deadline,
                                   [&] { return state->totals.tasks == 0; });
}
auto CreditPool::snapshot() const -> CreditSnapshot {
  const std::lock_guard lock(state->mutex);
  return state->totals;
}

auto DeadlineBounds::make(std::uint64_t admitted,
                          std::optional<std::uint64_t> enclosing)
    -> std::optional<DeadlineBounds> {
  auto wire = saturated_add(admitted, 5000 * NS_PER_MS);
  if (enclosing) {
    wire = std::min(wire, *enclosing);
  }
  if (wire <= admitted) {
    return std::nullopt;
  }
  return DeadlineBounds{
      admitted, std::min(saturated_add(admitted, 4500 * NS_PER_MS), wire),
      wire};
}
auto http_drain_deadline(std::uint64_t now, std::uint32_t configured_ms,
                         std::optional<std::uint64_t> package,
                         std::optional<std::uint64_t> coordinator)
    -> std::optional<std::uint64_t> {
  if (configured_ms < 1000 || configured_ms > 5000) {
    return std::nullopt;
  }
  auto deadline = saturated_add(now, configured_ms * NS_PER_MS);
  if (package) {
    deadline = std::min(deadline, *package);
  }
  if (coordinator) {
    deadline = std::min(deadline, *coordinator);
  }
  return deadline;
}

struct ResponseArbiter::State {
  mutable std::mutex mutex;
  ChoiceIdentity identity;
  DeadlineBounds bounds;
  Clock clock;
  std::shared_ptr<AdmissionLease> admission;
  ExtraMemoryCharge bookkeeping;
  std::shared_ptr<FrozenChoice> choice_slot;
  std::shared_ptr<const FrozenChoice> frozen;
  std::optional<CancelReason> cancelled;
  bool authorized = false;
  bool owner_live = true;
  bool durable = false;
  bool handed_to_writer = false;
  bool closed = false;
  WireObservation wire = WireObservation::NOT_ATTEMPTED;

  State(ChoiceIdentity identity_in, DeadlineBounds bounds_in, Clock clock_in,
        std::shared_ptr<AdmissionLease> admission_in,
        ExtraMemoryCharge bookkeeping_in,
        std::shared_ptr<FrozenChoice> choice_slot_in)
      : identity(std::move(identity_in)), bounds(bounds_in),
        clock(std::move(clock_in)), admission(std::move(admission_in)),
        bookkeeping(std::move(bookkeeping_in)),
        choice_slot(std::move(choice_slot_in)) {}

  auto freeze(std::optional<int> status, ChoiceReason reason) -> void {
    if (!frozen && authorized) {
      // Storage/identity were charged and allocated before authorization.
      // Choosing at a cancellation boundary cannot allocate or replace a
      // choice.
      choice_slot->record.status = status;
      choice_slot->record.reason = reason;
      frozen = choice_slot;
    }
  }
  auto close_wire() -> void {
    closed = true;
    if (handed_to_writer && wire != WireObservation::WRITE_COMPLETED &&
        wire != WireObservation::WRITE_FAILED) {
      wire = WireObservation::UNKNOWN;
    }
  }
  auto update(std::uint64_t now) -> void {
    if (now < bounds.admitted_at_ns) {
      owner_live = false;
      close_wire();
      return;
    }
    if (now >= bounds.execution_cutoff_ns) {
      if (!cancelled) {
        cancelled = CancelReason::DEADLINE;
      }
      freeze(504, ChoiceReason::DEADLINE);
    }
    if (now >= bounds.wire_expiry_ns) {
      close_wire();
      if (!handed_to_writer) {
        wire = WireObservation::UNKNOWN;
      }
    }
  }
};

ResponseArbiter::ResponseArbiter(ChoiceIdentity identity, DeadlineBounds bounds,
                                 Clock clock,
                                 std::shared_ptr<AdmissionLease> admission) {
  if (!admission || !clock || !valid_uuid(identity.dispatch_id) ||
      !valid_uuid(identity.owner_epoch) || !valid_uuid(identity.choice_id) ||
      identity.version == 0 ||
      bounds.admitted_at_ns >= bounds.execution_cutoff_ns ||
      bounds.execution_cutoff_ns > bounds.wire_expiry_ns ||
      bounds.wire_expiry_ns >
          saturated_add(bounds.admitted_at_ns, 5000 * NS_PER_MS) ||
      bounds.execution_cutoff_ns >
          saturated_add(bounds.admitted_at_ns, 4500 * NS_PER_MS)) {
    throw std::invalid_argument("invalid extension HTTP owner");
  }
  static_assert(sizeof(State) + sizeof(FrozenChoice) + 1024 < 4096);
  auto charge = admission->reserve_extra(4096);
  if (!charge) {
    throw std::bad_alloc();
  }
  auto choice_charge = admission->reserve_extra(1024);
  if (!choice_charge) {
    throw std::bad_alloc();
  }
  auto slot = std::make_shared<FrozenChoice>(FrozenChoice{
      ChoiceRecord{identity, std::nullopt, ChoiceReason::HANDLER_RESPONSE},
      admission, std::move(*choice_charge)});
  state = std::make_unique<State>(std::move(identity), bounds, std::move(clock),
                                  std::move(admission), std::move(*charge),
                                  std::move(slot));
}
ResponseArbiter::~ResponseArbiter() = default;
auto ResponseArbiter::authorize_dispatch() -> bool {
  const std::lock_guard lock(state->mutex);
  state->update(state->clock());
  if (state->authorized || !state->owner_live || state->cancelled ||
      state->closed) {
    return false;
  }
  state->authorized = true;
  return true;
}
auto ResponseArbiter::entry_permitted() const -> bool {
  const std::lock_guard lock(state->mutex);
  const auto now = state->clock();
  return state->authorized && state->owner_live && !state->cancelled &&
         now >= state->bounds.admitted_at_ns &&
         now < state->bounds.execution_cutoff_ns;
}
auto ResponseArbiter::remaining_execution_ms() const -> std::uint32_t {
  const std::lock_guard lock(state->mutex);
  const auto now = state->clock();
  if (!state->owner_live || state->cancelled ||
      now < state->bounds.admitted_at_ns ||
      now >= state->bounds.execution_cutoff_ns) {
    return 0;
  }
  return static_cast<std::uint32_t>((state->bounds.execution_cutoff_ns - now) /
                                    NS_PER_MS);
}
auto ResponseArbiter::submit_response(int status) -> CandidateDisposition {
  const std::lock_guard lock(state->mutex);
  const bool had_choice = static_cast<bool>(state->frozen);
  state->update(state->clock());
  if (had_choice) {
    return CandidateDisposition::DUPLICATE;
  }
  if (!state->authorized || !state->owner_live || state->frozen) {
    return CandidateDisposition::RETIRED;
  }
  state->freeze(status >= 200 && status <= 599 ? status : 500,
                status >= 200 && status <= 599
                    ? ChoiceReason::HANDLER_RESPONSE
                    : ChoiceReason::RESPONSE_INVALID);
  return CandidateDisposition::ACCEPTED;
}
auto ResponseArbiter::submit_failure(AdapterFailure failure)
    -> CandidateDisposition {
  const std::lock_guard lock(state->mutex);
  const bool had_choice = static_cast<bool>(state->frozen);
  state->update(state->clock());
  if (had_choice) {
    return CandidateDisposition::DUPLICATE;
  }
  if (!state->authorized || !state->owner_live || state->frozen) {
    return CandidateDisposition::RETIRED;
  }
  switch (failure) {
    case AdapterFailure::HANDLER_FAILED:
      state->freeze(500, ChoiceReason::HANDLER_FAILED);
      break;
    case AdapterFailure::RESPONSE_INVALID:
    case AdapterFailure::EXECUTION_CANCELLED:
      // Legitimate cancellation already froze the owner's cancellation choice.
      state->freeze(500, ChoiceReason::RESPONSE_INVALID);
      break;
    case AdapterFailure::ADAPTER_UNAVAILABLE:
      state->freeze(503, ChoiceReason::ADAPTER_UNAVAILABLE);
      break;
    case AdapterFailure::RESOURCE_EXHAUSTED:
      state->freeze(500, ChoiceReason::RESOURCE_EXHAUSTED);
      break;
  }
  return CandidateDisposition::ACCEPTED;
}
auto ResponseArbiter::cancel(CancelReason reason) -> void {
  const std::lock_guard lock(state->mutex);
  state->update(state->clock());
  if (!state->cancelled) {
    state->cancelled = reason;
    switch (reason) {
      case CancelReason::DEADLINE:
        state->freeze(504, ChoiceReason::DEADLINE);
        break;
      case CancelReason::CLIENT_GONE:
        state->freeze(std::nullopt, ChoiceReason::CLIENT_GONE);
        break;
      case CancelReason::SHUTDOWN:
        state->freeze(503, ChoiceReason::SHUTDOWN);
        break;
      case CancelReason::GENERATION_RETIRED:
        state->freeze(503, ChoiceReason::GENERATION_RETIRED);
        break;
    }
  }
  if (reason != CancelReason::DEADLINE) {
    state->close_wire();
    if (reason != CancelReason::CLIENT_GONE) {
      state->owner_live = false;
    }
  }
}
auto ResponseArbiter::expire() -> void {
  const std::lock_guard lock(state->mutex);
  state->update(state->clock());
}
auto ResponseArbiter::choice() const -> std::shared_ptr<const FrozenChoice> {
  const std::lock_guard lock(state->mutex);
  return state->frozen;
}
auto ResponseArbiter::cancellation() const -> std::optional<CancelReason> {
  const std::lock_guard lock(state->mutex);
  return state->cancelled;
}
auto ResponseArbiter::confirm_choice(const ChoiceRecord& acknowledged) -> bool {
  const std::lock_guard lock(state->mutex);
  state->update(state->clock());
  if (!state->owner_live || !state->frozen ||
      state->frozen->record != acknowledged) {
    return false;
  }
  state->durable = true;
  return true;
}
auto ResponseArbiter::dispatch_wire(const Writer& writer) -> bool {
  const std::lock_guard lock(state->mutex);
  state->update(state->clock());
  if (!writer || !state->owner_live || state->closed ||
      state->handed_to_writer || !state->durable || !state->frozen ||
      !state->frozen->record.status) {
    return false;
  }
  state->handed_to_writer = true;
  try {
    const auto observed = writer(*state->frozen);
    state->wire = observed == WireObservation::NOT_ATTEMPTED ||
                          observed == WireObservation::ATTEMPT_STARTED
                      ? observed
                      : WireObservation::UNKNOWN;
  } catch (...) {
    // Start may have happened: retain UNKNOWN, never infer no side effects.
    state->wire = WireObservation::UNKNOWN;
  }
  state->closed = true; // Single handoff, even if outcome is UNKNOWN.
  return true;
}
auto ResponseArbiter::observe_wire(const ChoiceRecord& identity,
                                   WireObservation observation) -> bool {
  const std::lock_guard lock(state->mutex);
  if (!state->owner_live || !state->handed_to_writer || !state->frozen ||
      state->frozen->record != identity ||
      (state->wire != WireObservation::ATTEMPT_STARTED &&
       state->wire != WireObservation::UNKNOWN) ||
      (observation != WireObservation::WRITE_COMPLETED &&
       observation != WireObservation::WRITE_FAILED)) {
    return false;
  }
  state->wire = observation;
  return true;
}
auto ResponseArbiter::wire_observation() const -> WireObservation {
  const std::lock_guard lock(state->mutex);
  return state->wire;
}
auto ResponseArbiter::wire_closed() const -> bool {
  const std::lock_guard lock(state->mutex);
  return state->closed;
}

} // namespace plinth::extension_http
