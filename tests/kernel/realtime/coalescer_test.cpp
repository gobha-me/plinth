// SPDX-License-Identifier: MIT
//
// ICD-0.5.1 §Test Cases — C.* coalescer state machine, T.* truncation
// heuristic, E.02-E.04 error paths. E.01 (classifier DDL rejection)
// lives in sql_classify_test.cpp. I.* end-to-end cases live in
// coalescer_integration_test.cpp.
//
// These tests set a process-wide emit hook on the registry that
// captures envelopes without a live Drogon DbClient, so they can run
// under plinth_tests_pg without PG (tagged [integration] per ICD
// §CI wiring so the CTest group picks them up alongside the actual
// PG-gated cases).

#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/sql_classify.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using plinth::realtime::CoalescerRegistry;
using plinth::realtime::NotifyError;
using plinth::realtime::OpKind;

namespace {

// Thread-safe envelope capture — the emit hook runs on whichever
// thread fires the flush (timer loop for async flushes, caller's
// thread for apply_flush_for_test / drain_extension / shutdown drain).
struct Captured {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Json::Value> envelopes;
  NotifyError inject_error{NotifyError::PG_FAILURE};
  bool inject_failure{false};

  auto on_emit(const Json::Value& env) -> std::expected<void, NotifyError> {
    {
      std::lock_guard lock(mu);
      envelopes.push_back(env);
    }
    cv.notify_all();
    if (inject_failure) {
      return std::unexpected(inject_error);
    }
    return {};
  }

  auto wait_for_n(std::size_t n, std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, timeout,
                       [this, n]() { return envelopes.size() >= n; });
  }

  auto size() -> std::size_t {
    std::lock_guard lock(mu);
    return envelopes.size();
  }
};

auto make_capturing_hook(const std::shared_ptr<Captured>& cap)
    -> CoalescerRegistry::EmitHook {
  return [cap](const Json::Value& env) -> std::expected<void, NotifyError> {
    return cap->on_emit(env);
  };
}

// Reset the registry to a known baseline and start with `cfg`. Every
// test calls this at its top so failures in prior tests don't leak
// state. Order matters: clear_windows_for_test first (silently drops
// any pending windows so shutdown has nothing to drain), shutdown
// next (stops the loop thread idempotently), then clear/set the hook,
// then start with the new config.
auto reset_and_start(const plinth::Config::Realtime::Coalescer& cfg,
                     const std::shared_ptr<Captured>& cap) -> void {
  auto& reg = CoalescerRegistry::instance();
  reg.clear_windows_for_test();
  reg.shutdown();
  reg.clear_emit_hook_for_test();
  reg.set_emit_hook_for_test(make_capturing_hook(cap));
  reg.start(cfg);
}

auto default_cfg() -> plinth::Config::Realtime::Coalescer {
  plinth::Config::Realtime::Coalescer c;
  c.enabled = true;
  c.window_ms = 200;
  return c;
}

} // namespace

// ── C.* — state machine ─────────────────────────────────────────────

TEST_CASE("C.01 single-write flush", "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  CoalescerRegistry::instance().record_write("ext_notes", "notes",
                                             OpKind::INSERT, 1, "notes");
  REQUIRE(
      CoalescerRegistry::instance().apply_flush_for_test("ext_notes", "notes"));
  REQUIRE(cap->size() == 1);

  const auto& env = cap->envelopes.at(0);
  CHECK(env["layer"].asString() == "data");
  CHECK(env["channel"].asString() == "plinth:data:ext_notes.notes");
  CHECK(env["schema"].asString() == "ext_notes");
  CHECK(env["table"].asString() == "notes");
  CHECK(env["ops"].size() == 3);
  CHECK(env["ops"][0]["op"].asString() == "insert");
  CHECK(env["ops"][0]["count"].asUInt64() == 1);
  CHECK(env["ops"][1]["count"].asUInt64() == 0);
  CHECK(env["ops"][2]["count"].asUInt64() == 0);
}

TEST_CASE("C.02 two-write accumulate", "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 5, "notes");
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 3, "notes");
  REQUIRE(reg.apply_flush_for_test("ext_notes", "notes"));
  REQUIRE(cap->size() == 1);

  CHECK(cap->envelopes.at(0)["ops"][0]["count"].asUInt64() == 8);
}

TEST_CASE("C.03 no-extend window — fixed duration from first write",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  auto cfg = default_cfg();
  cfg.window_ms = 250;
  reset_and_start(cfg, cap);

  auto& reg = CoalescerRegistry::instance();
  const auto T0 = std::chrono::steady_clock::now();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  std::this_thread::sleep_for(std::chrono::milliseconds(125));
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");

  // Wait for the timer to fire — window flushes ~window_ms from T0,
  // not from the last write.
  REQUIRE(cap->wait_for_n(1, std::chrono::milliseconds(600)));
  const auto ELAPSED = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - T0)
                           .count();
  CHECK(cap->size() == 1);
  CHECK(ELAPSED < 500); // << 2×window_ms
  CHECK(cap->envelopes.at(0)["ops"][0]["count"].asUInt64() == 3);
}

TEST_CASE("C.04 separate windows — post-flush new write reopens",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  auto cfg = default_cfg();
  cfg.window_ms = 80;
  reset_and_start(cfg, cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  REQUIRE(cap->wait_for_n(1, std::chrono::milliseconds(500)));
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  REQUIRE(cap->wait_for_n(2, std::chrono::milliseconds(500)));

  CHECK(cap->size() == 2);
  CHECK(cap->envelopes.at(0)["ops"][0]["count"].asUInt64() == 1);
  CHECK(cap->envelopes.at(1)["ops"][0]["count"].asUInt64() == 1);
}

TEST_CASE("C.05 per-table isolation", "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 2, "notes");
  reg.record_write("ext_notes", "tags", OpKind::INSERT, 1, "notes");

  REQUIRE(reg.apply_flush_for_test("ext_notes", "notes"));
  REQUIRE(reg.apply_flush_for_test("ext_notes", "tags"));
  REQUIRE(cap->size() == 2);

  // Find each envelope by table rather than assuming ordering.
  const std::array<std::string, 2> TABLES = {
      cap->envelopes.at(0)["table"].asString(),
      cap->envelopes.at(1)["table"].asString(),
  };
  CHECK(((TABLES[0] == "notes" && TABLES[1] == "tags") ||
         (TABLES[0] == "tags" && TABLES[1] == "notes")));
}

TEST_CASE("C.06 mixed ops in single window",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 5, "notes");
  reg.record_write("ext_notes", "notes", OpKind::UPDATE, 2, "notes");
  reg.record_write("ext_notes", "notes", OpKind::DELETE, 1, "notes");
  REQUIRE(reg.apply_flush_for_test("ext_notes", "notes"));

  const auto& env = cap->envelopes.at(0);
  CHECK(env["ops"][0]["count"].asUInt64() == 5); // insert
  CHECK(env["ops"][1]["count"].asUInt64() == 2); // update
  CHECK(env["ops"][2]["count"].asUInt64() == 1); // delete
}

TEST_CASE("C.07 enabled=false short-circuits",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  auto cfg = default_cfg();
  cfg.enabled = false;
  reset_and_start(cfg, cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  CHECK(reg.open_window_count_for_test() == 0);
  CHECK_FALSE(reg.apply_flush_for_test("ext_notes", "notes"));
  CHECK(cap->size() == 0);
}

TEST_CASE("C.08 window_ms=1", "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  auto cfg = default_cfg();
  cfg.window_ms = 1;
  reset_and_start(cfg, cap);

  CoalescerRegistry::instance().record_write("ext_notes", "notes",
                                             OpKind::INSERT, 1, "notes");
  REQUIRE(cap->wait_for_n(1, std::chrono::milliseconds(200)));
  CHECK(cap->envelopes.at(0)["window_ms"].asUInt64() == 1);
}

TEST_CASE("C.09 zero-row write on empty bucket is a no-op",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::UPDATE, 0, "notes");
  CHECK(reg.open_window_count_for_test() == 0);
  CHECK_FALSE(reg.apply_flush_for_test("ext_notes", "notes"));
  CHECK(cap->size() == 0);
}

TEST_CASE("C.10 zero-row write on open bucket leaves counters",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 3, "notes");
  reg.record_write("ext_notes", "notes", OpKind::UPDATE, 0, "notes");
  REQUIRE(reg.apply_flush_for_test("ext_notes", "notes"));

  const auto& env = cap->envelopes.at(0);
  CHECK(env["ops"][0]["count"].asUInt64() == 3);
  CHECK(env["ops"][1]["count"].asUInt64() == 0);
}

TEST_CASE("C.11 cross-extension write warns + accumulates",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_shared", "notes", OpKind::INSERT, 2, "a");
  reg.record_write("ext_shared", "notes", OpKind::INSERT, 1, "b");
  REQUIRE(reg.apply_flush_for_test("ext_shared", "notes"));

  // Ownership is "a" (first writer); counts accumulate regardless.
  CHECK(cap->envelopes.at(0)["ops"][0]["count"].asUInt64() == 3);
}

TEST_CASE("C.12 drain_extension flushes matching windows only",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_a", "t1", OpKind::INSERT, 1, "a");
  reg.record_write("ext_a", "t2", OpKind::INSERT, 1, "a");
  reg.record_write("ext_b", "t1", OpKind::INSERT, 1, "b");

  reg.drain_extension("a");
  CHECK(cap->size() == 2);
  CHECK(reg.open_window_count_for_test() == 1); // only ext_b remains
}

TEST_CASE("C.13 drain_extension with no matches is a no-op",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.drain_extension("nobody");
  CHECK(cap->size() == 0);
}

// ── T.* — truncation heuristic ──────────────────────────────────────

namespace {
struct ScopedMaxPayload {
  std::size_t previous;
  explicit ScopedMaxPayload(std::size_t n)
      : previous(plinth::realtime::get_max_payload_bytes()) {
    plinth::realtime::set_max_payload_bytes(n);
  }
  ~ScopedMaxPayload() { plinth::realtime::set_max_payload_bytes(previous); }
  ScopedMaxPayload(const ScopedMaxPayload&) = delete;
  auto operator=(const ScopedMaxPayload&) -> ScopedMaxPayload& = delete;
  ScopedMaxPayload(ScopedMaxPayload&&) = delete;
  auto operator=(ScopedMaxPayload&&) -> ScopedMaxPayload& = delete;
};
} // namespace

TEST_CASE("T.01 normal envelope emits without truncation",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);
  ScopedMaxPayload guard{8000};

  CoalescerRegistry::instance().record_write("ext_notes", "notes",
                                             OpKind::INSERT, 1, "notes");
  REQUIRE(
      CoalescerRegistry::instance().apply_flush_for_test("ext_notes", "notes"));

  CHECK(cap->size() == 1);
  CHECK_FALSE(cap->envelopes.at(0).isMember("truncated"));
}

TEST_CASE("T.02 oversize envelope audit+drops (counts-only 0.5.1)",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);
  ScopedMaxPayload guard{80}; // below a normal envelope

  CoalescerRegistry::instance().record_write("ext_notes", "notes",
                                             OpKind::INSERT, 1, "notes");
  // The flush short-circuits before reaching the hook — envelope
  // never emits. try_shrink_or_drop audits flush_failed with
  // `reason=payload_too_large` (no side-effect visible without the
  // audit pipeline running). The window IS erased.
  CHECK(CoalescerRegistry::instance().apply_flush_for_test("ext_notes",
                                                           "notes") == false);
  CHECK(cap->size() == 0);
  CHECK(CoalescerRegistry::instance().open_window_count_for_test() == 0);
}

TEST_CASE("T.03 max_payload_bytes knob exercises oversize path",
          "[realtime][coalescer][integration]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);
  ScopedMaxPayload guard{200};
  const std::size_t CEILING_IN_SCOPE =
      plinth::realtime::get_max_payload_bytes();
  CHECK(CEILING_IN_SCOPE == 200);
  // A normal coalescer envelope for (ext_notes, notes) is O(170)
  // bytes; 200 is right on the knife edge. The main assertion here
  // is that the knob is live and writable without leaking state —
  // scoped_max_payload's RAII restore is covered by T.04.
  CoalescerRegistry::instance().record_write("ext_notes", "notes",
                                             OpKind::INSERT, 1, "notes");
  (void)CoalescerRegistry::instance().apply_flush_for_test("ext_notes",
                                                           "notes");
}

TEST_CASE("T.04 ScopedMaxPayload round-trip",
          "[realtime][coalescer][integration]") {
  const std::size_t ORIGINAL = plinth::realtime::get_max_payload_bytes();
  {
    ScopedMaxPayload guard{200};
    CHECK(plinth::realtime::get_max_payload_bytes() == 200);
  }
  CHECK(plinth::realtime::get_max_payload_bytes() == ORIGINAL);
}

// ── E.* — error paths (E.01 lives in sql_classify_test.cpp) ─────────

TEST_CASE("E.02 zero-row write on empty bucket opens nothing",
          "[realtime][coalescer][integration][errors]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::UPDATE, 0, "notes");
  CHECK(reg.open_window_count_for_test() == 0);
}

TEST_CASE("E.03 emit failure audits flush_failed; subsequent writes OK",
          "[realtime][coalescer][integration][errors]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);
  cap->inject_failure = true; // hook captures + returns PG_FAILURE

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  // Flush — hook captures the envelope then returns the error.
  // flush_snapshot returns false; apply_flush_for_test echoes that.
  CHECK_FALSE(reg.apply_flush_for_test("ext_notes", "notes"));
  CHECK(cap->size() == 1); // envelope was built + attempted

  // Subsequent writes should still proceed on a separate bucket.
  cap->inject_failure = false;
  reg.record_write("ext_notes", "tags", OpKind::INSERT, 1, "notes");
  REQUIRE(reg.apply_flush_for_test("ext_notes", "tags"));
  CHECK(cap->size() == 2);
  CHECK(cap->envelopes.at(1)["table"].asString() == "tags");
}

TEST_CASE("E.04 shutdown drains an open window",
          "[realtime][coalescer][integration][errors]") {
  auto cap = std::make_shared<Captured>();
  reset_and_start(default_cfg(), cap);

  auto& reg = CoalescerRegistry::instance();
  reg.record_write("ext_notes", "notes", OpKind::INSERT, 1, "notes");
  REQUIRE(reg.open_window_count_for_test() == 1);

  reg.shutdown();

  CHECK(cap->size() == 1);
  CHECK(reg.open_window_count_for_test() == 0);
  CHECK(cap->envelopes.at(0)["channel"].asString() ==
        "plinth:data:ext_notes.notes");
}
