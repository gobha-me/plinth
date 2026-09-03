#pragma once

// Per-connection state stored in WebSocketConnection::setContext.
// Most fields are touched only on the connection's owning event loop.
// `channels` is the exception — `publish_dispatched` reads it from the
// listener / writer thread for the synchronous `delivered_to_users`
// pre-pass (ICD-0.5.4 §When `record_delivered` fires) while subscribe /
// unsubscribe / drain_extension mutate it on the conn's loop. A
// per-connection mutex serializes that one cross-thread access.

#include "kernel/auth/middleware.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <unordered_set>

namespace plinth::ws {

struct ConnState {
  plinth::auth::AuthContext auth;
  bool authenticated{false};
  bool is_admin{false};

  // Effective RBAC rules loaded once at WS auth completion (the same
  // union-across-groups query the HTTP `RbacFilter` uses). The
  // per-channel subscribe gate (`on_subscribe`) and the defense-in-
  // depth delivery re-check (`publish_dispatched`) both read from
  // this set on the conn's owning loop; no mutex needed because all
  // reads and the one write live on the same loop. Empty for an
  // admin (admin short-circuits RBAC).
  std::unordered_set<std::string> effective_rules;

  // Subscribed channels. Mutated on the conn's owning loop (subscribe,
  // unsubscribe, drain_extension), read on the conn's loop AND from
  // the listener/writer thread via `publish_dispatched`. Every access
  // takes `*channels_mu`; without it the writer's pre-pass races
  // against drain_extension's per-conn erase loop and crashes inside
  // `unordered_set::contains` (I.06 broker_integration_test repro).
  // Held via unique_ptr because std::mutex is non-movable and
  // `subscriptions.cpp` copies a ConnState into the replay coroutine
  // frame; the unique_ptr move-construction is what keeps ConnState
  // move-able while still owning a unique mutex.
  mutable std::unique_ptr<std::mutex> channels_mu{
      std::make_unique<std::mutex>()};
  std::unordered_set<std::string> channels;

  // The IO loop the connection is bound to. Captured in handleNewConnection
  // (where getEventLoopOfCurrentThread() is guaranteed to return the
  // correct loop) so that DB callbacks — which run on pool threads — can
  // hop back onto the right loop before touching per-conn state.
  trantor::EventLoop* loop{nullptr};

  // Timer ids belong to `loop`. InvalidTimerId means none scheduled.
  trantor::TimerId auth_timer_id{trantor::InvalidTimerId};
  trantor::TimerId heartbeat_timer_id{trantor::InvalidTimerId};

  // Outstanding ping timestamp for heartbeat tracking. 0 = no pending ping.
  int64_t pending_ping_ts{0};

  // Heartbeat timings copied from Config into ConnState at connection time
  // so the auth flow can start the heartbeat without reaching back into
  // the controller.
  double heartbeat_interval_s{30.0};
  double heartbeat_timeout_s{10.0};

  // ICD-0.5.5 §8 — mid-replay live-frame buffering. Each entry is
  // keyed on the channel string; the bool is true while a replay
  // for that channel is in flight, the deque holds the live frames
  // that arrived during the replay (kept in arrival order =
  // writer-first seq order), and `live_buffer_cap` is the per-
  // subscription overflow cap (a snapshot of
  // `events.live_buffer_cap_per_subscription` taken when the
  // replay started). Touched on the conn's owning loop only.
  std::unordered_map<std::string, bool> replay_in_flight;
  std::unordered_map<std::string, std::deque<std::shared_ptr<std::string>>>
      live_buffer;
  std::size_t live_buffer_cap{256};

  // ICD-0.5.5 §8 — abort flag + buffered_live_count atomic shared
  // with the in-flight replay coroutine. The publish_dispatched
  // overflow site flips `replay_abort_flag` so the next chunk
  // boundary in `run_replay` returns early; `buffered_live_count`
  // is bumped on every buffered envelope so `replay_done` reports
  // the count atomically without crossing the conn-loop boundary.
  // Both shared_ptrs are nullptr unless a replay is in flight.
  std::shared_ptr<std::atomic<bool>> replay_abort_flag;
  std::shared_ptr<std::atomic<std::size_t>> buffered_live_count;

  // ICD-0.5.5 §11 — per-channel last-seen-seq for live-path gap
  // detection. Reset to 0 on each subscribe so the baseline starts
  // fresh; only updated on `deliver_to_conn`'s immediate-send path
  // (replay_in_flight=false). Buffered live events during replay
  // bypass the update — per ICD §11 "no replay/reconnect intervened"
  // suppresses gap detection across recovery. Sentinel 0 means "no
  // baseline yet"; the first live frame sets it without firing.
  // Touched only on the conn's owning loop, no mutex needed.
  std::unordered_map<std::string, std::int64_t> last_live_seen_seq;
};

} // namespace plinth::ws
