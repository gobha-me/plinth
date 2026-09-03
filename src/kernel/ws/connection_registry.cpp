#include "kernel/ws/connection_registry.hpp"

#include "kernel/ws/conn_state.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <vector>

namespace plinth::ws {

namespace {

// Shutdown gate: during process teardown, an IO loop may dispatch a
// `handleConnectionClosed` event for a connection whose TCP-level close is
// still pending. The coordinator flips this gate before stopping Drogon and
// joins the application thread before static destruction begins, so no late
// callback can touch the registry while its members are being destroyed.
//
// The test fixture / main.cpp flip this flag **before**
// `drogon::app().quit()`; every public method checks the flag on
// entry and no-ops if set. The flag lives at file scope (anonymous
// namespace) so its zero-initialized storage outlives the singleton's
// dynamic-init-ordered destruction.
// gate across threads
std::atomic<bool> g_shutdown_pending{false};

} // namespace

auto ConnectionRegistry::instance() -> ConnectionRegistry& {
  // The coordinator closes the gate and joins Drogon's application thread
  // before main returns, so ordinary static storage is both safe and owned.
  static ConnectionRegistry inst;
  return inst;
}

auto ConnectionRegistry::register_connection(
    const RegistryKey& key, const drogon::WebSocketConnectionPtr& conn,
    std::shared_ptr<ConnState> state) -> drogon::WebSocketConnectionPtr {
  if (g_shutdown_pending.load(std::memory_order_acquire)) {
    return {};
  }
  std::lock_guard lock(mu);
  drogon::WebSocketConnectionPtr displaced;
  auto it = conns.find(key);
  if (it != conns.end()) {
    displaced = it->second.conn;
    it->second = RegistryEntry{.conn = conn, .state = std::move(state)};
  } else {
    conns.emplace(key, RegistryEntry{.conn = conn, .state = std::move(state)});
  }
  return displaced;
}

auto ConnectionRegistry::unregister_connection(
    const RegistryKey& key, const drogon::WebSocketConnectionPtr& conn)
    -> void {
  if (g_shutdown_pending.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard lock(mu);
  auto it = conns.find(key);
  if (it != conns.end() && it->second.conn == conn) {
    conns.erase(it);
  }
}

auto ConnectionRegistry::for_each(
    const std::function<void(const drogon::WebSocketConnectionPtr&)>& fn) const
    -> void {
  if (g_shutdown_pending.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<drogon::WebSocketConnectionPtr> snapshot;
  {
    std::lock_guard lock(mu);
    snapshot.reserve(conns.size());
    for (const auto& [_, entry] : conns) {
      snapshot.push_back(entry.conn);
    }
  }
  // Lock released before invoking callbacks — fn may call back into
  // event loops that could in turn touch the registry.
  for (const auto& conn : snapshot) {
    fn(conn);
  }
}

auto ConnectionRegistry::size() const -> std::size_t {
  if (g_shutdown_pending.load(std::memory_order_acquire)) {
    return 0;
  }
  std::lock_guard lock(mu);
  return conns.size();
}

auto ConnectionRegistry::initiate_shutdown() noexcept -> void {
  g_shutdown_pending.store(true, std::memory_order_release);
}

auto ConnectionRegistry::cancel_all_timers(std::chrono::milliseconds timeout)
    -> bool {
  // Snapshot ConnState shared_ptrs under lock — NOT the conn
  // shared_ptrs. ConnState is allocated in events_controller via
  // `std::make_shared<ConnState>` so its control block is ours.
  // The conn shared_ptr's control block is drogon-managed and has
  // been observed to be stale during legacy teardown (CI #12174 and
  // project_ws_flaky_segfault.md §REOPENED 2026-04-19 —
  // `cancel_all_timers` SEGV'd in `shared_ptr<WSConn>` copy ctor
  // inside this very snapshot loop).
  //
  // Callers must invoke this BEFORE `initiate_shutdown()` — the
  // shutdown-pending gate short-circuits the map read.
  std::vector<std::shared_ptr<ConnState>> snapshot;
  {
    std::lock_guard lock(mu);
    snapshot.reserve(conns.size());
    for (const auto& [_, entry] : conns) {
      if (entry.state) {
        snapshot.push_back(entry.state);
      }
    }
  }

  // For each connection, queue a timer-invalidate onto its owning
  // loop and wait for it to complete. The loop callback runs
  // synchronously with the TimerQueue; once it returns, the timer is
  // guaranteed not to fire again. Blocking on the future ensures we
  // don't race ahead to `drogon::app().quit()` with stale timers
  // still armed.
  //
  // One shared deadline prevents connection count from multiplying the
  // shutdown bound.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (const auto& state : snapshot) {
    if (!state || state->loop == nullptr) {
      continue;
    }
    auto* loop = state->loop;
    auto done = std::make_shared<std::promise<void>>();
    auto done_fut = done->get_future();
    // Capture `state` by value — extends the ConnState lifetime
    // into the callback so the lambda body's state-> accesses are
    // safe even if the map entry gets erased concurrently.
    // The callback can run after the deadline. Keep its completion state
    // heap-owned instead of capturing a stack promise that the timeout path
    // has already destroyed.
    loop->queueInLoop([state, done]() {
      if (state->auth_timer_id != trantor::InvalidTimerId &&
          state->loop != nullptr) {
        state->loop->invalidateTimer(state->auth_timer_id);
        state->auth_timer_id = trantor::InvalidTimerId;
      }
      if (state->heartbeat_timer_id != trantor::InvalidTimerId &&
          state->loop != nullptr) {
        state->loop->invalidateTimer(state->heartbeat_timer_id);
        state->heartbeat_timer_id = trantor::InvalidTimerId;
      }
      done->set_value();
    });
    if (done_fut.wait_until(deadline) != std::future_status::ready) {
      return false;
    }
  }
  return true;
}

auto ConnectionRegistry::close_all_connections() -> void {
  for_each([](const drogon::WebSocketConnectionPtr& conn) {
    if (conn != nullptr && conn->connected()) {
      conn->shutdown(drogon::CloseCode::kEndpointGone, "server shutdown");
    }
  });
}

} // namespace plinth::ws
