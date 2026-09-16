#pragma once

// plinth::capabilities::drain — lifecycle dispatch fence and drain counter.
//
// ICD-0.4.5 §Atomic Swap T1/T2. On upgrade, after the new version's
// REGISTERING commits, the old version's in-flight capability calls
// must be allowed to complete before the atomic swap performs state
// transitions and route cutover. `begin_drain(name)` activates a
// per-extension fence. Calls admitted before the fence remain counted by a
// RAII `DispatchGuard`; calls arriving after it are rejected. `wait_for_zero`
// blocks until every pre-fence call completes or the timeout expires.
//
// Semantics:
//   - Calls started BEFORE begin_drain are counted and allowed to complete.
//   - Calls started AFTER begin_drain are not admitted.
//   - State entries persist after a fence is released so beginning a later
//     drain cannot miss calls already admitted for that name.

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
  std::atomic<bool> fenced{false};
  std::atomic<bool> application_blocked{false};
  std::mutex mu;
  std::condition_variable cv;
};

// Atomically fence new dispatches for `name` and return the state tracking
// calls that were already admitted. Idempotent per name.
auto begin_drain(std::string_view name) -> std::shared_ptr<DrainState>;

// Remove `name` from the active-drain map. Outstanding DispatchGuards
// keep their captured shared_ptr; further decrements remain safe and
// wake any waiter on the state's condvar. Idempotent.
auto end_drain(std::string_view name) -> void;

// Return whether new dispatch is currently fenced for `name`.
[[nodiscard]] auto is_fenced(std::string_view name) -> bool;

// Keep discovery fail-closed after a lifecycle operation reaches an uncertain
// or partially committed state. Cleared by end_drain after successful repair.
auto block_application(std::string_view name) -> void;
[[nodiscard]] auto is_application_blocked(std::string_view name) -> bool;

// Block until `state->in_flight` reaches 0 or `timeout` elapses.
// Returns `{reached_zero, outstanding_at_return}`. Does not remove
// the drain — caller invokes end_drain explicitly.
auto wait_for_zero(const std::shared_ptr<DrainState>& state,
                   std::chrono::milliseconds timeout)
    -> std::pair<bool, std::size_t>;

// Test-visible: active drain count (number of names with registered
// drains). Hot-path uses the internal atomic for early-out.
[[nodiscard]] auto active_drain_count() -> std::size_t;
[[nodiscard]] auto tracked_state_count_for_test() -> std::size_t;

// RAII guard. Ctor admits and counts the call unless the name is fenced; dtor
// decrements only an admitted call. Copy + move disabled.
class DispatchGuard {
 public:
  explicit DispatchGuard(std::string_view name);
  ~DispatchGuard();
  DispatchGuard(const DispatchGuard&) = delete;
  auto operator=(const DispatchGuard&) -> DispatchGuard& = delete;
  DispatchGuard(DispatchGuard&&) = delete;
  auto operator=(DispatchGuard&&) -> DispatchGuard& = delete;

  [[nodiscard]] auto admitted() const -> bool { return was_admitted; }

 private:
  std::string name;
  std::shared_ptr<DrainState> state;
  bool was_admitted{false};
};

} // namespace plinth::capabilities::drain
