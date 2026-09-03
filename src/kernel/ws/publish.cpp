#include "kernel/ws/publish.hpp"

#include "kernel/logging.hpp"
#include "kernel/rbac/subscribe_rule.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/messages.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/drogon.h>
#include <functional>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace plinth::ws {

namespace {

auto build_event_frame(std::string_view channel, const Json::Value& payload)
    -> std::string {
  Json::Value v;
  v["type"] = msg::EVENT;
  v["channel"] = std::string{channel};
  v["payload"] = payload;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, v);
}

// Channel belongs to extension `name` when it carries the Layer-1
// schema prefix `plinth:data:ext_<name>.` or the Layer-3 extension
// namespace `plinth:ext:<name>:`. Kernel-schema Layer-1 channels (e.g.
// `plinth:data:plinth.users`) do not belong to any extension.
auto channel_belongs_to_extension(std::string_view channel,
                                  std::string_view name) -> bool {
  const std::string DATA_PFX = "plinth:data:ext_" + std::string{name} + ".";
  const std::string EXT_PFX = "plinth:ext:" + std::string{name} + ":";
  return channel.starts_with(DATA_PFX) || channel.starts_with(EXT_PFX);
}

// process-wide metric counter, only mutated via the atomic note_* helpers
std::atomic<std::size_t> g_total_subscriptions{0};

// ICD-0.5.5 §11 — sliding-window dedup state for
// `realtime.seq.gap_detected`. Mirrors the
// `claim_debounce_audit_slot` shape in subscriptions.cpp:473-486
// (different map + mutex; same window-reset semantics). Keyed on
// `user_id\0channel` per ICD §11 line 1206-1214 — bursty gap
// detection on one channel doesn't drown out a different gap
// elsewhere.
struct GapAuditWindow {
  std::chrono::steady_clock::time_point first_ts;
  std::uint64_t count{0};
};

// module-local audit-window state mirrors the debounce dedup pattern
std::mutex g_gap_audit_mu;
std::unordered_map<std::string, GapAuditWindow> g_gap_windows;
std::atomic<std::uint64_t> g_gap_emit_count{0};

auto gap_window_key(std::string_view user_id, std::string_view channel)
    -> std::string {
  std::string k;
  k.reserve(user_id.size() + channel.size() + 1);
  k.append(user_id);
  k.push_back('\0');
  k.append(channel);
  return k;
}

auto claim_gap_audit_slot(const std::string& key,
                          std::chrono::steady_clock::time_point now,
                          std::chrono::milliseconds window)
    -> std::pair<bool, std::uint64_t> {
  std::lock_guard lock(g_gap_audit_mu);
  auto& win = g_gap_windows[key];
  if (win.count == 0 || now - win.first_ts > window) {
    win.first_ts = now;
    win.count = 1;
    return {true, 1};
  }
  win.count += 1;
  return {false, win.count};
}

auto peer_ip(const drogon::WebSocketConnectionPtr& conn) -> std::string {
  return conn->peerAddr().toIp();
}

// ICD-0.5.5 §11 — emit `realtime.seq.gap_detected` audit. Caller has
// already established that prev_seq > 0 and next_seq > prev_seq + 1.
// Rate-limited via `claim_gap_audit_slot` keyed on (user_id, channel)
// using `events.seq.gap_audit_window_ms`. Empty user_id short-circuits
// (no row would attribute correctly anyway).
auto emit_gap_detected(const drogon::WebSocketConnectionPtr& conn,
                       const ConnState& state, const std::string& channel,
                       std::int64_t prev_seq, std::int64_t next_seq) -> void {
  if (state.auth.user_id.empty()) {
    return;
  }
  const auto CFG = plinth::realtime::events_writer::current_config();
  const auto WINDOW = std::chrono::milliseconds(CFG.seq.gap_audit_window_ms);
  const auto KEY = gap_window_key(state.auth.user_id, channel);
  auto [emit, cnt] =
      claim_gap_audit_slot(KEY, std::chrono::steady_clock::now(), WINDOW);
  if (!emit) {
    return;
  }
  g_gap_emit_count.fetch_add(1, std::memory_order_relaxed);
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  Json::Value payload(Json::objectValue);
  payload["user_id"] = state.auth.user_id;
  payload["channel"] = channel;
  payload["prev_seq"] = static_cast<Json::Int64>(prev_seq);
  payload["next_seq"] = static_cast<Json::Int64>(next_seq);
  payload["gap_size"] = static_cast<Json::Int64>(next_seq - prev_seq - 1);
  payload["count_in_window"] = static_cast<Json::UInt64>(cnt);
  payload["window_ms"] = static_cast<Json::UInt64>(CFG.seq.gap_audit_window_ms);
  plinth::log::audit("realtime.seq.gap_detected", payload,
                     {.user_id = state.auth.user_id,
                      .session_id = state.auth.session_id,
                      .ip_address = peer_ip(conn)});
}

// Per-conn delivery-time RBAC re-check (defense in depth per
// ICD-0.5.2 §Security Constraints item 5). The subscribe-time gate
// already filtered denied channels out of `ConnState::channels`, so
// the happy path hits the contains(channel_str) guard first; this
// re-check only matters when group membership was revoked between
// subscribe and emit. Mirrors `ws::subscriptions::subscribe_allowed`
// ordering — admin short-circuits, rbac_enforce=false falls back to
// 0.1.6 admin-only semantics (S.09), else rule lookup.
auto delivery_rbac_allows(const ConnState& state, std::string_view channel)
    -> bool {
  if (state.is_admin) {
    return true;
  }
  if (!plinth::realtime::broker::is_rbac_enforced()) {
    return false; // 0.1.6 fallback: non-admin never delivered
  }
  auto rule = plinth::rbac::derive_subscribe_rule(channel);
  if (rule.empty()) {
    return false;
  }
  return state.effective_rules.contains(rule);
}

// ICD-0.5.5 §8 — buffer overflow path. Pulled out of the per-conn
// lambda in `publish_dispatched` so the lambda's cognitive
// complexity stays under the tidy threshold. Runs on the conn's
// owning loop; flips the shared abort flag, clears the buffer, and
// emits the `live_buffer_overflow` resync frame inline so the
// client sees the resync without waiting for the replay coroutine
// to advance.
auto handle_buffer_overflow(const drogon::WebSocketConnectionPtr& conn,
                            ConnState& state, const std::string& channel)
    -> void {
  if (state.replay_abort_flag != nullptr) {
    state.replay_abort_flag->store(true, std::memory_order_release);
  }
  auto& buf = state.live_buffer[channel];
  buf.clear();
  if (state.buffered_live_count != nullptr) {
    state.buffered_live_count->store(0, std::memory_order_release);
  }
  Json::Value resync(Json::objectValue);
  resync["type"] = msg::RESYNC;
  resync["reason"] = "live_buffer_overflow";
  resync["retention_seconds"] = static_cast<Json::UInt64>(0);
  conn->sendJson(resync);
  auto it = state.replay_in_flight.find(channel);
  if (it != state.replay_in_flight.end()) {
    it->second = false;
  }
}

// Per-conn delivery body. Runs on the conn's owning loop; reads
// state safely. Returns without acting if the conn is no longer
// authenticated, the channel was unsubscribed, or the per-channel
// RBAC re-check fails. Otherwise either buffers the frame (when a
// replay for the channel is in flight) or sends it.
//
// `seq` is the envelope's writer-stamped sequence number (0 if the
// caller could not extract a usable seq — e.g. the older non-
// dispatched `publish()` primitive). On the immediate-send path
// (no replay in flight) a non-zero seq is compared against
// `state.last_live_seen_seq[channel]` and a forward gap (K > 1)
// fires the rate-limited `realtime.seq.gap_detected` audit per
// ICD-0.5.5 §11. Buffered live events deliberately bypass the
// gap-detect update — per ICD §11 "no replay/reconnect intervened"
// suppresses gap detection across recovery.
auto deliver_to_conn(const drogon::WebSocketConnectionPtr& conn,
                     const std::shared_ptr<std::string>& frame,
                     const std::string& channel, std::int64_t seq) -> void {
  auto* s = conn->getContext<ConnState>().get();
  if (s == nullptr || !s->authenticated || !conn->connected()) {
    return;
  }
  {
    std::lock_guard lk(*s->channels_mu);
    if (!s->channels.contains(channel)) {
      return;
    }
  }
  if (!delivery_rbac_allows(*s, channel)) {
    return;
  }
  auto it = s->replay_in_flight.find(channel);
  if (it == s->replay_in_flight.end() || !it->second) {
    if (seq > 0) {
      auto& last = s->last_live_seen_seq[channel];
      // ICD-0.5.5 §11 — emit audit only on a forward jump of
      // K > 1 against an established baseline (last > 0).
      // First-frame case (last == 0) and in-order case
      // (seq == last + 1) both fall through to the unified
      // baseline-advance below — collapsed into one branch to
      // avoid a `bugprone-branch-clone` clang-tidy diagnostic.
      if (last > 0 && seq > last + 1) {
        emit_gap_detected(conn, *s, channel, last, seq);
      }
      // Duplicate / reorder (seq <= last with last > 0) leaves
      // `last` unchanged: §11 gap detection is forward-only.
      // `std::max` collapses the (last == 0) and (seq > last)
      // cases without a branch — first-frame establishes the
      // baseline, in-order / gap advance it, duplicate /
      // reorder is a no-op.
      last = std::max(last, seq);
    }
    conn->send(*frame);
    return;
  }
  auto& buf = s->live_buffer[channel];
  if (buf.size() >= s->live_buffer_cap) {
    handle_buffer_overflow(conn, *s, channel);
    return;
  }
  buf.push_back(frame);
  if (s->buffered_live_count != nullptr) {
    s->buffered_live_count->fetch_add(1, std::memory_order_release);
  }
}

} // namespace

auto publish(std::string_view channel, const Json::Value& payload) -> void {
  auto channel_str = std::string{channel};
  auto frame =
      std::make_shared<std::string>(build_event_frame(channel, payload));

  // Snapshot current connections under the registry lock.
  std::vector<drogon::WebSocketConnectionPtr> snapshot;
  ConnectionRegistry::instance().for_each(
      [&snapshot](const drogon::WebSocketConnectionPtr& conn) {
        snapshot.push_back(conn);
      });

  // Dispatch to each connection on its owning loop. We deliberately do
  // not check ConnState.channels here (it's mutated on the owning loop
  // and reading it from an arbitrary thread is a race) — the per-loop
  // lambda below does that check where it's safe.
  for (const auto& conn : snapshot) {
    auto* state = conn->getContext<ConnState>().get();
    if (state == nullptr || state->loop == nullptr) {
      continue;
    }
    state->loop->queueInLoop([conn, frame, channel_str]() {
      auto* s = conn->getContext<ConnState>().get();
      if (s == nullptr || !s->authenticated || !conn->connected()) {
        return;
      }
      bool subscribed = false;
      {
        std::lock_guard lk(*s->channels_mu);
        subscribed = s->channels.contains(channel_str);
      }
      if (subscribed) {
        conn->send(*frame);
      }
    });
  }
}

auto publish_dispatched(const plinth::realtime::DispatchedEvent& ev) -> void {
  // Envelope-aware fan-out. ICD-0.5.2 §Subscription Matching →
  // frame delivery path (WS). Frame carries the FULL envelope as
  // `payload` per §OQ6 pin so Layer-1/2/3 unify under one parser.
  auto channel_str = ev.channel;
  auto frame =
      std::make_shared<std::string>(build_event_frame(ev.channel, ev.envelope));
  // ICD-0.5.5 §11 — extract the writer-stamped seq once for the
  // per-conn gap-detect path. 0 sentinel means "no usable seq"
  // (events_writer always stamps a positive seq via INSERT ...
  // RETURNING; the older non-dispatched `publish()` path does not
  // and routes through `deliver_to_conn` with seq=0 to skip
  // gap detection).
  std::int64_t seq = 0;
  if (ev.envelope.isMember("seq") && ev.envelope["seq"].isInt64()) {
    seq = ev.envelope["seq"].asInt64();
  }

  std::vector<drogon::WebSocketConnectionPtr> snapshot;
  ConnectionRegistry::instance().for_each(
      [&snapshot](const drogon::WebSocketConnectionPtr& conn) {
        snapshot.push_back(conn);
      });

  // ICD-0.5.4 §When `record_delivered` fires — synchronous pre-pass
  // populates `ev.delivered_to_users` BEFORE the per-conn loop hops.
  // The actual `queueInLoop` lambdas below run async on the conn's
  // owning loop; if we deferred the user-id collection to those
  // lambdas, the events writer's handler (which runs immediately
  // after `publish_dispatched` returns on the listener thread) would
  // see an empty list and skip every cursor advance. The cost is one
  // extra synchronous walk over the snapshot — same asymptotic shape
  // as the existing fan-out — and the trade-off is that per-conn
  // liveness checks deferred to the lambda may differ from this
  // pre-pass (e.g. conn dropped between the two). That's acceptable
  // per ICD §SC8 (cursor advance is fire-and-forget; on rare drops
  // the duplicate-tolerance contract handles re-delivery).
  for (const auto& conn : snapshot) {
    auto* state = conn->getContext<ConnState>().get();
    if (state == nullptr || !state->authenticated) {
      continue;
    }
    {
      std::lock_guard lk(*state->channels_mu);
      if (!state->channels.contains(channel_str)) {
        continue;
      }
    }
    if (!delivery_rbac_allows(*state, channel_str)) {
      continue;
    }
    if (!state->auth.user_id.empty()) {
      ev.delivered_to_users.push_back(state->auth.user_id);
    }
  }

  for (const auto& conn : snapshot) {
    auto* state = conn->getContext<ConnState>().get();
    if (state == nullptr || state->loop == nullptr) {
      continue;
    }
    state->loop->queueInLoop([conn, frame, channel_str, seq]() {
      deliver_to_conn(conn, frame, channel_str, seq);
    });
  }
}

auto drain_ws_subscriptions_for_extension(std::string_view name)
    -> std::size_t {
  // Iterate the connection snapshot and, on each conn's loop, erase
  // every `channels` entry that belongs to `name`. Accumulate the
  // eviction count via an atomic that the lambdas bump — we cannot
  // return an accurate count until every queueInLoop lambda has
  // finished, so we post the work and return the running total at
  // the time of call (conservative for the audit payload). For the
  // synchronous path the test seam relies on, tests call
  // `drain_ws_subscriptions_for_extension` on the same loop they
  // subscribed from so the lambda runs inline.
  std::vector<drogon::WebSocketConnectionPtr> snapshot;
  ConnectionRegistry::instance().for_each(
      [&snapshot](const drogon::WebSocketConnectionPtr& conn) {
        snapshot.push_back(conn);
      });

  auto removed = std::make_shared<std::atomic<std::size_t>>(0);
  const std::string NAME{name};

  for (const auto& conn : snapshot) {
    auto* state = conn->getContext<ConnState>().get();
    if (state == nullptr || state->loop == nullptr) {
      continue;
    }
    state->loop->queueInLoop([conn, removed, NAME]() {
      auto* s = conn->getContext<ConnState>().get();
      if (s == nullptr) {
        return;
      }
      std::size_t local = 0;
      {
        std::lock_guard lk(*s->channels_mu);
        for (auto it = s->channels.begin(); it != s->channels.end();) {
          if (channel_belongs_to_extension(*it, NAME)) {
            it = s->channels.erase(it);
            local += 1;
          } else {
            ++it;
          }
        }
      }
      if (local > 0) {
        removed->fetch_add(local, std::memory_order_relaxed);
        note_subscriptions_removed(local);
      }
    });
  }
  return removed->load(std::memory_order_relaxed);
}

auto total_subscription_count() -> std::size_t {
  return g_total_subscriptions.load(std::memory_order_relaxed);
}

auto note_subscriptions_added(std::size_t n) -> void {
  g_total_subscriptions.fetch_add(n, std::memory_order_relaxed);
}

auto note_subscriptions_removed(std::size_t n) -> void {
  g_total_subscriptions.fetch_sub(n, std::memory_order_relaxed);
}

auto reset_subscription_count_for_test() -> void {
  g_total_subscriptions.store(0, std::memory_order_relaxed);
}

auto reset_gap_audit_windows_for_test() -> void {
  std::lock_guard lock(g_gap_audit_mu);
  g_gap_windows.clear();
  g_gap_emit_count.store(0, std::memory_order_relaxed);
}

auto gap_audit_emit_count_for_test() -> std::uint64_t {
  return g_gap_emit_count.load(std::memory_order_relaxed);
}

} // namespace plinth::ws
