#pragma once

// Singleton registry of authenticated WebSocket connections, keyed by
// (auth_type, session_or_pat_id). Used to enforce single-connection per
// session/PAT and to iterate subscribers on publish.

#include "kernel/ws/conn_state.hpp"

#include <chrono>
#include <drogon/WebSocketConnection.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace plinth::ws {

struct RegistryKey {
  std::string auth_type; // "session" or "pat"
  std::string id;        // session_id (sessions) or pat_id (PATs)

  auto operator==(const RegistryKey& other) const -> bool {
    return auth_type == other.auth_type && id == other.id;
  }
};

struct RegistryKeyHash {
  auto operator()(const RegistryKey& k) const noexcept -> std::size_t {
    return std::hash<std::string>{}(k.auth_type) ^
           (std::hash<std::string>{}(k.id) << 1U);
  }
};

// Per-entry payload: the conn (for publish fan-out + displacement) and
// a separately-owned shared_ptr<ConnState> (for teardown-time timer
// cancellation that must never touch the conn's own control block —
// see `cancel_all_timers` rationale).
struct RegistryEntry {
  drogon::WebSocketConnectionPtr conn;
  std::shared_ptr<ConnState> state;
  // Captured on the owning loop before publication and immutable thereafter.
  // Displacement never reads another connection's mutable Drogon context.
  // The coordinator keeps this loop alive through registry owner release.
  trantor::EventLoop* loop{nullptr};
};

class ConnectionRegistry {
 public:
  static auto instance() -> ConnectionRegistry&;

  // Atomically install `conn` + `state` for `key`, returning the
  // previously installed entry (empty conn if absent). Its owned state and
  // captured loop survive concurrent connection-context clearing. Caller is
  // responsible for closing the displaced connection on its own event loop.
  // `state` must be the same `shared_ptr<ConnState>` attached to
  // `conn` via `setContext` — the registry holds its own copy so
  // `cancel_all_timers` can invalidate timers without ever touching
  // the connection's control block during shutdown.
  auto register_connection(const RegistryKey& key,
                           const drogon::WebSocketConnectionPtr& conn,
                           std::shared_ptr<ConnState> state) -> RegistryEntry;

  // Remove `conn` for `key`, but only if `conn` matches the currently
  // installed pointer. Avoids racing with a displacement that already
  // installed a new connection.
  auto unregister_connection(const RegistryKey& key,
                             const drogon::WebSocketConnectionPtr& conn)
      -> void;

  // Snapshot all current connections under the lock, then call `fn`
  // on each pointer (lock released first, so `fn` may safely use
  // queueInLoop on the connection's owning loop).
  auto for_each(
      const std::function<void(const drogon::WebSocketConnectionPtr&)>& fn)
      const -> void;

  // For tests.
  [[nodiscard]] auto size() const -> std::size_t;

  // Seal singleton admission under the registry lock and gate late callbacks.
  // Owning references still require release_connections() before loops stop.
  static auto initiate_shutdown() noexcept -> void;

  // Seal admission, release every registered owner on its original IO loop,
  // and wait for acknowledgments within one shared deadline. Pending releases
  // survive timeout and repeated calls wait for the same acknowledgments.
  // Call from the coordinator after producers drain, while all IO loops live.
  [[nodiscard]] auto release_connections(std::chrono::milliseconds timeout)
      -> bool;

  // Cancel every live connection's auth + heartbeat timers on their
  // owning event loops, synchronously. All connections share one deadline;
  // false means at least one loop did not acknowledge before it expired.
  // The coordinator must keep Drogon alive and retry or fail closed. Without
  // this barrier, a timer tick already queued inside trantor's internal
  // TimerQueue fires during `quit()`'s drain, its lambda's
  // `weak_ptr<WebSocketConnection>` lock() throws `bad_weak_ptr` (the
  // connection shared_ptr has been released by the client-side close),
  // the throw lands inside trantor's noexcept boundary, and the
  // subprocess aborts. See `project_ws_flaky_segfault.md` — this is
  // the WS-internal sub-path that the 0.3.4.1 bundle didn't close.
  //
  // 0.4.4.1 rework: the implementation iterates `entry.state`
  // (shared_ptr<ConnState>, our control block, allocated via
  // make_shared in events_controller) rather than `entry.conn`
  // (shared_ptr<WebSocketConnection>, drogon-owned control block).
  // Prior revisions SEGV'd inside the snapshot loop when copying
  // the conn shared_ptr found a stale control block — drogon's
  // WS connection shared_ptrs can land in the registry with control
  // blocks that later become dangling under specific teardown
  // orderings. Our ConnState control block is unaffected.
  [[nodiscard]] auto cancel_all_timers(
      std::chrono::milliseconds timeout = std::chrono::seconds{5}) -> bool;

  // Initiate a normal close on every currently registered WebSocket before
  // Drogon stops its listeners. Must run before initiate_shutdown(), which
  // deliberately disables registry iteration.
  auto close_all_connections() -> void;

  // Prefer ConnectionRegistry::instance() in production code. The
  // default constructor is public to let unit tests exercise the map
  // semantics without touching the global singleton.
  ConnectionRegistry() = default;

 private:
  mutable std::mutex mu;
  bool sealed{false};
  std::vector<std::shared_future<void>> releases;
  std::unordered_map<RegistryKey, RegistryEntry, RegistryKeyHash> conns;
};

} // namespace plinth::ws
