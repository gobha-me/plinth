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

} // namespace

auto begin_drain(std::string_view name) -> std::shared_ptr<DrainState> {
  std::unique_lock lock(g_mu);
  auto it = g_drains.find(std::string{name});
  if (it != g_drains.end()) {
    if (!it->second->fenced.exchange(true, std::memory_order_acq_rel)) {
      g_active_count.fetch_add(1, std::memory_order_release);
    }
    return it->second;
  }
  auto state = std::make_shared<DrainState>();
  state->fenced.store(true, std::memory_order_release);
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
  if (it->second->fenced.exchange(false, std::memory_order_acq_rel)) {
    g_active_count.fetch_sub(1, std::memory_order_release);
  }
  it->second->application_blocked.store(false, std::memory_order_release);
  if (it->second->in_flight.load(std::memory_order_acquire) == 0) {
    g_drains.erase(it);
  }
}

auto is_fenced(std::string_view name) -> bool {
  std::shared_lock lock(g_mu);
  auto it = g_drains.find(std::string{name});
  return it != g_drains.end() &&
         it->second->fenced.load(std::memory_order_acquire);
}

auto block_application(std::string_view name) -> void {
  std::unique_lock lock(g_mu);
  auto [it, inserted] =
      g_drains.try_emplace(std::string{name}, std::make_shared<DrainState>());
  (void)inserted;
  it->second->application_blocked.store(true, std::memory_order_release);
}

auto is_application_blocked(std::string_view name) -> bool {
  std::shared_lock lock(g_mu);
  auto it = g_drains.find(std::string{name});
  return it != g_drains.end() &&
         it->second->application_blocked.load(std::memory_order_acquire);
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

auto tracked_state_count_for_test() -> std::size_t {
  std::shared_lock lock(g_mu);
  return g_drains.size();
}

DispatchGuard::DispatchGuard(std::string_view name) {
  std::unique_lock lock(g_mu);
  auto [it, inserted] =
      g_drains.try_emplace(std::string{name}, std::make_shared<DrainState>());
  (void)inserted;
  if (it->second->fenced.load(std::memory_order_acquire)) {
    return;
  }
  this->name = std::string{name};
  state = it->second;
  state->in_flight.fetch_add(1, std::memory_order_release);
  was_admitted = true;
}

DispatchGuard::~DispatchGuard() {
  if (!state) {
    return;
  }
  std::size_t prev = 0;
  {
    std::unique_lock lock(g_mu);
    prev = state->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1 && !state->fenced.load(std::memory_order_acquire) &&
        !state->application_blocked.load(std::memory_order_acquire)) {
      auto it = g_drains.find(name);
      if (it != g_drains.end() && it->second == state) {
        g_drains.erase(it);
      }
    }
  }
  if (prev == 1) {
    std::lock_guard lock(state->mu);
    state->cv.notify_all();
  }
}

} // namespace plinth::capabilities::drain
