#pragma once

// plinth::capabilities::drain — upgrade-time dispatch-drain counter.
//
// ICD-0.4.5 §Atomic Swap T1/T2. On upgrade, after the new version's
// REGISTERING commits, the old version's in-flight capability calls
// must be allowed to complete before the atomic swap performs state
// transitions and route cutover. `begin_drain(name)` activates a
// per-extension counter; subsequent `call_capability` dispatches
// whose signature namespace matches `name` increment the counter via
// a RAII `DispatchGuard`. `wait_for_zero` blocks until the counter
// reaches 0 or the timeout expires.
//
// Semantics:
//   - Calls started BEFORE begin_drain are not counted (their
//     DispatchGuard saw no active drain at ctor time). These complete
//     harmlessly against the old handler under the
//     resolution.cpp state_mutex shared_lock; B7's T4 unregister
//     naturally waits for them via the same shared_mutex.
//   - Calls started AFTER begin_drain increment/decrement the counter.
//     The timeout triggers when load pins the counter above zero.
//   - When no drain is active, DispatchGuard is a pair of
//     relaxed-atomic reads (check g_active_count == 0 early-out);
//     hot-path cost is a single non-contended atomic load.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace plinth::capabilities::drain {

struct DrainState {
  std::atomic<std::size_t> in_flight{0};
  std::mutex mu;
  std::condition_variable cv;
};

// Register a drain on `name` (extension name, i.e. the capability
// namespace prefix). Idempotent per (name): a second call returns the
// existing state. Matching dispatches that enter AFTER this call
// increment/decrement the returned state's counter.
auto begin_drain(std::string_view name) -> std::shared_ptr<DrainState>;

// Remove `name` from the active-drain map. Outstanding DispatchGuards
// keep their captured shared_ptr; further decrements remain safe and
// wake any waiter on the state's condvar. Idempotent.
auto end_drain(std::string_view name) -> void;

// Block until `state->in_flight` reaches 0 or `timeout` elapses.
// Returns `{reached_zero, outstanding_at_return}`. Does not remove
// the drain — caller invokes end_drain explicitly.
auto wait_for_zero(const std::shared_ptr<DrainState>& state,
                   std::chrono::milliseconds timeout)
    -> std::pair<bool, std::size_t>;

// Test-visible: active drain count (number of names with registered
// drains). Hot-path uses the internal atomic for early-out.
[[nodiscard]] auto active_drain_count() -> std::size_t;

// RAII guard. Ctor increments `in_flight` iff a drain is active for
// `name`; dtor decrements only if ctor incremented. Copy + move
// disabled — guards are scope-bound to one dispatch.
class DispatchGuard {
 public:
  explicit DispatchGuard(std::string_view name);
  ~DispatchGuard();
  DispatchGuard(const DispatchGuard&) = delete;
  auto operator=(const DispatchGuard&) -> DispatchGuard& = delete;
  DispatchGuard(DispatchGuard&&) = delete;
  auto operator=(DispatchGuard&&) -> DispatchGuard& = delete;

 private:
  std::shared_ptr<DrainState> state;
};

} // namespace plinth::capabilities::drain
