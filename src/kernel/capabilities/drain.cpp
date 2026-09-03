#include "kernel/capabilities/drain.hpp"

#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace plinth::capabilities::drain {

namespace {

// protecting the drain map below; cannot be const (lock() mutates).
std::shared_mutex g_mu;
// per-extension drain state is inherently process-wide; begin_drain/end_drain
// mutate, and every call_capability's DispatchGuard queries via shared_lock.
std::unordered_map<std::string, std::shared_ptr<DrainState>> g_drains;
// early-out counter; incremented on begin_drain, decremented on end_drain, read
// by every DispatchGuard ctor without taking g_mu.
std::atomic<std::size_t> g_active_count{0};

auto find_state(std::string_view name) -> std::shared_ptr<DrainState> {
  std::shared_lock lock(g_mu);
  auto it = g_drains.find(std::string{name});
  if (it == g_drains.end()) {
    return nullptr;
  }
  return it->second;
}

} // namespace

auto begin_drain(std::string_view name) -> std::shared_ptr<DrainState> {
  std::unique_lock lock(g_mu);
  auto it = g_drains.find(std::string{name});
  if (it != g_drains.end()) {
    return it->second;
  }
  auto state = std::make_shared<DrainState>();
  g_drains.emplace(std::string{name}, state);
  g_active_count.fetch_add(1, std::memory_order_release);
  return state;
}

auto end_drain(std::string_view name) -> void {
  std::unique_lock lock(g_mu);
  auto it = g_drains.find(std::string{name});
  if (it == g_drains.end()) {
    return;
  }
  g_drains.erase(it);
  g_active_count.fetch_sub(1, std::memory_order_release);
}

auto wait_for_zero(const std::shared_ptr<DrainState>& state,
                   std::chrono::milliseconds timeout)
    -> std::pair<bool, std::size_t> {
  std::unique_lock lock(state->mu);
  bool ok = state->cv.wait_for(lock, timeout, [&] {
    return state->in_flight.load(std::memory_order_acquire) == 0;
  });
  auto outstanding = state->in_flight.load(std::memory_order_acquire);
  return {ok, outstanding};
}

auto active_drain_count() -> std::size_t {
  return g_active_count.load(std::memory_order_acquire);
}

DispatchGuard::DispatchGuard(std::string_view name) {
  if (g_active_count.load(std::memory_order_acquire) == 0) {
    return;
  }
  state = find_state(name);
  if (state) {
    state->in_flight.fetch_add(1, std::memory_order_release);
  }
}

DispatchGuard::~DispatchGuard() {
  if (!state) {
    return;
  }
  auto prev = state->in_flight.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 1) {
    std::lock_guard lock(state->mu);
    state->cv.notify_all();
  }
}

} // namespace plinth::capabilities::drain
