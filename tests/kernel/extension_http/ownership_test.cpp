// SPDX-License-Identifier: MIT
#include "kernel/extension_http/ownership.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace plinth::extension_http;
using namespace std::chrono_literals;
constexpr std::uint64_t MS = 1000000;
constexpr auto ID = "11111111-1111-1111-1111-111111111111";
constexpr auto EPOCH = "22222222-2222-2222-2222-222222222222";
constexpr auto CHOICE = "33333333-3333-3333-3333-333333333333";
constexpr auto OTHER = "44444444-4444-4444-4444-444444444444";

struct Fixture {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  std::uint64_t now = 100 * MS;
  std::shared_ptr<AdmissionLease> lease =
      pool.try_acquire("sample", 0, process.try_acquire());
  std::unique_ptr<ResponseArbiter> owner = std::make_unique<ResponseArbiter>(
      ChoiceIdentity{ID, EPOCH, 1, CHOICE}, *DeadlineBounds::make(now),
      [this] { return now; }, lease);
};
} // namespace

TEST_CASE("HTTP credits require a process owner and bound stable package names",
          "[extension-http][ownership]") {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  REQUIRE(pool.try_acquire("sample", 0, nullptr) == nullptr);
  REQUIRE(pool.try_acquire("Bad", 0, process.try_acquire()) == nullptr);
  REQUIRE(pool.try_acquire("sample", std::numeric_limits<std::size_t>::max(),
                           process.try_acquire()) == nullptr);
  REQUIRE(pool.snapshot() == CreditSnapshot{});
  std::vector<std::shared_ptr<AdmissionLease>> held;
  for (int index = 0; index < 4; ++index) {
    held.push_back(pool.try_acquire("sample", 0, process.try_acquire()));
    REQUIRE(held.back() != nullptr);
  }
  REQUIRE(pool.try_acquire("sample", 0, process.try_acquire()) == nullptr);
  for (int index = 0; index < 4; ++index) {
    held.push_back(pool.try_acquire("second", 0, process.try_acquire()));
    REQUIRE(held.back() != nullptr);
  }
  REQUIRE(pool.try_acquire("third", 0, process.try_acquire()) == nullptr);
  REQUIRE(pool.snapshot().tasks == 8);
  REQUIRE(pool.snapshot().reserved_extra_bytes == 768ULL * 1024 * 1024);
  REQUIRE(process.active_for_test() == 8);
  held.clear();
  REQUIRE(pool.snapshot() == CreditSnapshot{});
  REQUIRE(process.active_for_test() == 0);
}

TEST_CASE("HTTP transport credits reject excess without partially admitting",
          "[extension-http][ownership]") {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  const auto bytes = 128ULL * 1024 * 1024;
  auto a = pool.try_acquire("sample", bytes, process.try_acquire());
  auto b = pool.try_acquire("sample", bytes, process.try_acquire());
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  const auto before = pool.snapshot();
  REQUIRE(pool.try_acquire("sample", 1, process.try_acquire()) == nullptr);
  REQUIRE(pool.snapshot() == before);
  auto c = pool.try_acquire("second", bytes, process.try_acquire());
  auto d = pool.try_acquire("second", bytes, process.try_acquire());
  REQUIRE(c != nullptr);
  REQUIRE(d != nullptr);
  REQUIRE(pool.snapshot().borrowed_transport_bytes == FEATURE_TRANSPORT_BYTES);
  REQUIRE(pool.try_acquire("third", 1, process.try_acquire()) == nullptr);
  auto empty = pool.try_acquire("third", 0, process.try_acquire());
  REQUIRE(empty != nullptr);
  REQUIRE(pool.snapshot().tasks == 5);
}

TEST_CASE(
    "HTTP allocation tokens retain physical ownership after response owner",
    "[extension-http][ownership]") {
  plinth::lifecycle::AsyncTaskRegistry process;
  std::optional<ExtraMemoryCharge> descendant;
  {
    CreditPool pool;
    auto lease = pool.try_acquire("sample", 31, process.try_acquire());
    REQUIRE(lease != nullptr);
    REQUIRE(lease->extra_used() == OWNER_BOOKKEEPING_BYTES);
    REQUIRE_FALSE(lease->reserve_extra(0).has_value());
    REQUIRE_FALSE(lease->reserve_extra(std::numeric_limits<std::size_t>::max())
                      .has_value());
    const auto payload = EXTRA_BYTES_PER_LEASE - OWNER_BOOKKEEPING_BYTES -
                         sizeof(ExtraMemoryCharge);
    descendant = lease->reserve_extra(payload);
    REQUIRE(descendant.has_value());
    REQUIRE(lease->extra_used() == EXTRA_BYTES_PER_LEASE);
    REQUIRE_FALSE(lease->reserve_extra(1).has_value());
    auto moved = std::move(*descendant);
    descendant.reset();
    descendant.emplace(std::move(moved));
    REQUIRE(lease->extra_used() == EXTRA_BYTES_PER_LEASE);
    lease.reset();
    REQUIRE(pool.snapshot().tasks == 1);
    REQUIRE_FALSE(pool.drain(std::chrono::steady_clock::now()));
    REQUIRE(pool.snapshot().borrowed_transport_bytes == 31);
    REQUIRE(process.active_for_test() == 1);
  }
  process.close_admission();
  REQUIRE_FALSE(process.drain(0ms));
  descendant.reset();
  REQUIRE(process.drain(0ms));
}

TEST_CASE(
    "HTTP allocation move assignment returns only its own previous credit",
    "[extension-http][ownership]") {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  auto lease = pool.try_acquire("sample", 0, process.try_acquire());
  auto first = lease->reserve_extra(10);
  auto second = lease->reserve_extra(20);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  const auto second_bytes = second->bytes();
  *first = std::move(*second);
  REQUIRE(lease->extra_used() == OWNER_BOOKKEEPING_BYTES + second_bytes);
  second.reset();
  first.reset();
  REQUIRE(lease->extra_used() == OWNER_BOOKKEEPING_BYTES);
  pool.close_admission();
  REQUIRE(pool.try_acquire("sample", 0, process.try_acquire()) == nullptr);
  REQUIRE_FALSE(pool.drain(std::chrono::steady_clock::time_point::min()));
  lease.reset();
  REQUIRE(pool.drain(std::chrono::steady_clock::time_point::min()));
}

TEST_CASE(
    "HTTP clocks distinguish execution cutoff and wire expiry without wrap",
    "[extension-http][ownership]") {
  const auto bounds = DeadlineBounds::make(100 * MS);
  REQUIRE(bounds.has_value());
  REQUIRE(bounds->execution_cutoff_ns == 4600 * MS);
  REQUIRE(bounds->wire_expiry_ns == 5100 * MS);
  const auto tight = DeadlineBounds::make(100 * MS, 800 * MS);
  REQUIRE(tight.has_value());
  REQUIRE(tight->execution_cutoff_ns == 800 * MS);
  REQUIRE(tight->wire_expiry_ns == 800 * MS);
  REQUIRE_FALSE(DeadlineBounds::make(100 * MS, 100 * MS).has_value());
  REQUIRE_FALSE(DeadlineBounds::make(100 * MS, 0).has_value());
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto edge = DeadlineBounds::make(maximum - 1);
  REQUIRE(edge.has_value());
  REQUIRE(edge->execution_cutoff_ns == maximum);
  REQUIRE(edge->wire_expiry_ns == maximum);
  REQUIRE_FALSE(DeadlineBounds::make(maximum).has_value());
}

TEST_CASE("HTTP configured drain lower bound never extends enclosing time",
          "[extension-http][ownership]") {
  constexpr auto NOW = 10000 * MS;
  REQUIRE(http_drain_deadline(NOW) == NOW + 5000 * MS);
  REQUIRE(http_drain_deadline(NOW, 1000) == NOW + 1000 * MS);
  REQUIRE_FALSE(http_drain_deadline(NOW, 999).has_value());
  REQUIRE_FALSE(http_drain_deadline(NOW, 5001).has_value());
  for (const auto remaining : {0ULL, 100ULL, 700ULL}) {
    REQUIRE(http_drain_deadline(NOW, 5000, NOW + remaining * MS) ==
            NOW + remaining * MS);
    REQUIRE(http_drain_deadline(NOW, 1000, std::nullopt,
                                NOW + remaining * MS) == NOW + remaining * MS);
  }
  REQUIRE(http_drain_deadline(NOW, 1000, NOW + 6000 * MS) == NOW + 1000 * MS);
  REQUIRE(http_drain_deadline(NOW, 5000, NOW - 1) == NOW - 1);
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  REQUIRE(http_drain_deadline(maximum - 1) == maximum);
}

TEST_CASE("HTTP physical queued-entry cutoff is checked using current clock",
          "[extension-http][ownership]") {
  Fixture f;
  REQUIRE_FALSE(f.owner->entry_permitted());
  REQUIRE(f.owner->authorize_dispatch());
  REQUIRE_FALSE(f.owner->authorize_dispatch());
  REQUIRE(f.owner->remaining_execution_ms() == 4500);
  f.now += 4400 * MS;
  REQUIRE(f.owner->entry_permitted());
  f.now += 100 * MS;
  REQUIRE_FALSE(f.owner->entry_permitted());
  REQUIRE(f.owner->remaining_execution_ms() == 0);
  // No timer turn has run. A late result still cannot select 200.
  REQUIRE(f.owner->submit_response(200) == CandidateDisposition::RETIRED);
  REQUIRE(f.owner->choice()->record.status == 504);
  REQUIRE(f.owner->choice()->record.reason == ChoiceReason::DEADLINE);
  REQUIRE(f.pool.snapshot().tasks == 1);
}

TEST_CASE("HTTP already-expired admission never authorizes a dispatch",
          "[extension-http][ownership]") {
  Fixture f;
  f.now += 5000 * MS;
  REQUIRE_FALSE(f.owner->authorize_dispatch());
  REQUIRE_FALSE(f.owner->entry_permitted());
  REQUIRE(f.owner->choice() == nullptr);
  REQUIRE(f.owner->wire_closed());
  REQUIRE(f.owner->wire_observation() == WireObservation::UNKNOWN);
  REQUIRE(f.pool.snapshot().tasks == 1);
}

TEST_CASE("HTTP one timely choice survives deadline and exact durable readback",
          "[extension-http][ownership]") {
  Fixture f;
  REQUIRE(f.owner->authorize_dispatch());
  REQUIRE(f.owner->submit_response(200) == CandidateDisposition::ACCEPTED);
  const auto choice = f.owner->choice();
  REQUIRE(choice != nullptr);
  REQUIRE(f.owner->submit_response(201) == CandidateDisposition::DUPLICATE);
  f.now += 4500 * MS;
  f.owner->expire();
  REQUIRE(f.owner->choice() == choice);
  REQUIRE(choice->record.status == 200);
  REQUIRE(f.owner->cancellation() == CancelReason::DEADLINE);
  bool started = false;
  const auto writer = [&](const FrozenChoice& frozen) {
    started = true;
    REQUIRE(frozen.record == choice->record);
    return WireObservation::ATTEMPT_STARTED;
  };
  REQUIRE_FALSE(f.owner->dispatch_wire(writer));
  auto wrong = choice->record;
  wrong.identity.owner_epoch = OTHER;
  REQUIRE_FALSE(f.owner->confirm_choice(wrong));
  wrong = choice->record;
  ++wrong.identity.version;
  REQUIRE_FALSE(f.owner->confirm_choice(wrong));
  wrong = choice->record;
  wrong.identity.dispatch_id = OTHER;
  REQUIRE_FALSE(f.owner->confirm_choice(wrong));
  wrong = choice->record;
  wrong.identity.choice_id = OTHER;
  REQUIRE_FALSE(f.owner->confirm_choice(wrong));
  wrong = choice->record;
  wrong.status = 504;
  REQUIRE_FALSE(f.owner->confirm_choice(wrong));
  wrong = choice->record;
  wrong.reason = ChoiceReason::DEADLINE;
  REQUIRE_FALSE(f.owner->confirm_choice(wrong));
  REQUIRE_FALSE(started);
  REQUIRE(f.owner->confirm_choice(choice->record));
  REQUIRE(f.owner->dispatch_wire(writer));
  REQUIRE(started);
  REQUIRE_FALSE(f.owner->dispatch_wire(writer));
  REQUIRE(f.owner->wire_observation() == WireObservation::ATTEMPT_STARTED);
  REQUIRE(
      f.owner->observe_wire(choice->record, WireObservation::WRITE_COMPLETED));
  REQUIRE_FALSE(
      f.owner->observe_wire(choice->record, WireObservation::WRITE_FAILED));
  REQUIRE(f.owner->wire_observation() == WireObservation::WRITE_COMPLETED);
  REQUIRE(choice->record.status == 200);
}

TEST_CASE("HTTP wire gate rechecks clock and late ACK never reopens response",
          "[extension-http][ownership]") {
  Fixture f;
  REQUIRE(f.owner->authorize_dispatch());
  REQUIRE(f.owner->submit_response(200) == CandidateDisposition::ACCEPTED);
  const auto choice = f.owner->choice();
  REQUIRE(f.owner->confirm_choice(choice->record));
  f.now += 5000 * MS;
  bool called = false;
  REQUIRE_FALSE(f.owner->dispatch_wire([&](const FrozenChoice&) {
    called = true;
    return WireObservation::ATTEMPT_STARTED;
  }));
  REQUIRE_FALSE(called);
  REQUIRE(f.owner->confirm_choice(choice->record));
  REQUIRE(f.owner->wire_closed());
  REQUIRE(f.owner->wire_observation() == WireObservation::UNKNOWN);
  REQUIRE(choice->record.status == 200);
  REQUIRE(f.pool.snapshot().tasks == 1);
}

TEST_CASE("HTTP disconnect and retirement never rewrite a frozen choice",
          "[extension-http][ownership]") {
  SECTION("disconnect before choice is no response without invented 499") {
    Fixture f;
    REQUIRE(f.owner->authorize_dispatch());
    f.owner->cancel(CancelReason::CLIENT_GONE);
    REQUIRE_FALSE(f.owner->choice()->record.status.has_value());
    REQUIRE(f.owner->choice()->record.reason == ChoiceReason::CLIENT_GONE);
    f.owner->cancel(CancelReason::SHUTDOWN);
    REQUIRE(f.owner->cancellation() == CancelReason::CLIENT_GONE);
    REQUIRE_FALSE(f.owner->entry_permitted());
  }
  SECTION("retired epoch bars old ACK and writer but preserves 200") {
    Fixture f;
    REQUIRE(f.owner->authorize_dispatch());
    REQUIRE(f.owner->submit_response(200) == CandidateDisposition::ACCEPTED);
    const auto choice = f.owner->choice();
    f.owner->cancel(CancelReason::GENERATION_RETIRED);
    REQUIRE_FALSE(f.owner->confirm_choice(choice->record));
    REQUIRE_FALSE(f.owner->dispatch_wire(
        [](const FrozenChoice&) { return WireObservation::ATTEMPT_STARTED; }));
    REQUIRE(choice->record.status == 200);
    REQUIRE_FALSE(f.owner->entry_permitted());
    REQUIRE(f.pool.snapshot().tasks == 1);
  }
  SECTION("shutdown cancellation is unavailable rather than deadline") {
    Fixture f;
    REQUIRE(f.owner->authorize_dispatch());
    f.owner->cancel(CancelReason::SHUTDOWN);
    REQUIRE(f.owner->choice()->record.status == 503);
    f.now += 5000 * MS;
    f.owner->expire();
    REQUIRE(f.owner->choice()->record.reason == ChoiceReason::SHUTDOWN);
  }
}

TEST_CASE("HTTP wire uncertainty permits truthful refinement without reopening",
          "[extension-http][ownership]") {
  Fixture f;
  REQUIRE(f.owner->authorize_dispatch());
  REQUIRE(f.owner->submit_response(200) == CandidateDisposition::ACCEPTED);
  const auto choice = f.owner->choice();
  REQUIRE(f.owner->confirm_choice(choice->record));
  REQUIRE(f.owner->dispatch_wire([](const FrozenChoice&) -> WireObservation {
    throw std::runtime_error("unknown transport handoff");
  }));
  REQUIRE(f.owner->wire_observation() == WireObservation::UNKNOWN);
  auto stale = choice->record;
  stale.identity.owner_epoch = OTHER;
  REQUIRE_FALSE(f.owner->observe_wire(stale, WireObservation::WRITE_COMPLETED));
  REQUIRE_FALSE(
      f.owner->observe_wire(choice->record, WireObservation::NOT_ATTEMPTED));
  REQUIRE(f.owner->observe_wire(choice->record, WireObservation::WRITE_FAILED));
  REQUIRE(f.owner->wire_closed());
  REQUIRE_FALSE(f.owner->dispatch_wire(
      [](const FrozenChoice&) { return WireObservation::ATTEMPT_STARTED; }));
  REQUIRE(choice->record.status == 200);
}

TEST_CASE("HTTP immutable choice readers retain process and feature credits",
          "[extension-http][ownership]") {
  Fixture f;
  REQUIRE(f.owner->authorize_dispatch());
  REQUIRE(f.owner->submit_response(200) == CandidateDisposition::ACCEPTED);
  auto pending_store_reader = f.owner->choice();
  f.owner.reset();
  f.lease.reset();
  REQUIRE(f.pool.snapshot().tasks == 1);
  REQUIRE_FALSE(f.pool.drain(std::chrono::steady_clock::now()));
  REQUIRE(f.process.active_for_test() == 1);
  pending_store_reader.reset();
  REQUIRE(f.pool.snapshot() == CreditSnapshot{});
  REQUIRE(f.process.active_for_test() == 0);
}

TEST_CASE("HTTP unsolicited adapter cancellation is an invalid response",
          "[extension-http][ownership]") {
  Fixture f;
  REQUIRE(f.owner->authorize_dispatch());
  REQUIRE(f.owner->submit_failure(AdapterFailure::EXECUTION_CANCELLED) ==
          CandidateDisposition::ACCEPTED);
  REQUIRE(f.owner->choice()->record.status == 500);
  REQUIRE(f.owner->choice()->record.reason == ChoiceReason::RESPONSE_INVALID);
  REQUIRE_FALSE(f.owner->cancellation().has_value());
}

TEST_CASE("HTTP owner construction rolls back a partial allocation reservation",
          "[extension-http][ownership]") {
  Fixture f;
  constexpr auto FIRST_CHARGE = 4096 + sizeof(ExtraMemoryCharge);
  const auto free = EXTRA_BYTES_PER_LEASE - f.lease->extra_used();
  auto held = f.lease->reserve_extra(free - FIRST_CHARGE - 512 -
                                     sizeof(ExtraMemoryCharge));
  REQUIRE(held.has_value());
  const auto used_before = f.lease->extra_used();
  const auto totals_before = f.pool.snapshot();
  auto construct = [&] {
    return std::make_unique<ResponseArbiter>(
        ChoiceIdentity{ID, EPOCH, 1, OTHER}, *DeadlineBounds::make(f.now),
        [&] { return f.now; }, f.lease);
  };
  // The 4096-byte reservation succeeds; the following 1024-byte choice
  // reservation fails. All earlier construction ownership must unwind.
  REQUIRE_THROWS_AS(construct(), std::bad_alloc);
  REQUIRE(f.lease->extra_used() == used_before);
  REQUIRE(f.pool.snapshot() == totals_before);
  REQUIRE(f.process.active_for_test() == 1);
}
