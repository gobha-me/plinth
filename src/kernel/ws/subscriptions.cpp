#include "kernel/ws/subscriptions.hpp"

#include "kernel/config.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/rbac/subscribe_rule.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/channel.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/replay.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/messages.hpp"
#include "kernel/ws/publish.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/drogon.h>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace plinth::ws {

namespace {

// ICD-0.5.5 §8 — process-static atomic backing
// `test_seam::live_buffer_cap_override`. Sentinel `SIZE_MAX` means
// "no override set"; any other value is the override. Mirrors the
// existing `set_db_client_for_test` storage pattern used by
// realtime/replay.cpp, cursor_store.cpp, events_writer.cpp etc.
constexpr std::size_t LIVE_CAP_SENTINEL_NONE = static_cast<std::size_t>(-1);
// process-static atomic backing the test seam, only mutated via
// `set_live_buffer_cap_override`.
std::atomic<std::size_t> g_live_cap_override{LIVE_CAP_SENTINEL_NONE};

auto peer_ip(const drogon::WebSocketConnectionPtr& conn) -> std::string {
  return conn->peerAddr().toIp();
}

// Extract the "channels" string array from a subscribe/unsubscribe message.
auto extract_channels(const Json::Value& msg) -> std::vector<std::string> {
  std::vector<std::string> out;
  const auto& arr = msg["channels"];
  if (!arr.isArray()) {
    return out;
  }
  out.reserve(arr.size());
  for (const auto& v : arr) {
    if (v.isString()) {
      out.push_back(v.asString());
    }
  }
  return out;
}

// Build {"type":"subscribed"|"unsubscribed", "channels":[...]}.
// ICD-0.5.5 §7 — when type is "subscribed", append the
// `recommended_debounce_ms` + `recommended_jitter_ms` advisory pair.
// Per OQ5 the advisory ships once per subscribe_ack; SDKs cache it
// for the subscription lifetime. Mid-session config reloads do not
// republish (matches the 0.5.4 norm for retention_seconds).
auto make_ack(std::string_view type_str,
              const std::vector<std::string>& channels) -> Json::Value {
  Json::Value v;
  v["type"] = std::string{type_str};
  Json::Value arr(Json::arrayValue);
  for (const auto& c : channels) {
    arr.append(c);
  }
  v["channels"] = arr;
  if (type_str == plinth::ws::msg::SUBSCRIBED) {
    const auto CFG = plinth::realtime::events_writer::current_config();
    v["recommended_debounce_ms"] =
        static_cast<Json::UInt64>(CFG.debounce.recommend_ms);
    v["recommended_jitter_ms"] =
        static_cast<Json::UInt64>(CFG.debounce.jitter_max_ms);
  }
  return v;
}

// Per-channel RBAC gate per ICD-0.5.2 §Subscription RBAC (S.09 pins
// the rbac_enforce=false semantics: fall back to 0.1.6 admin-only,
// i.e. non-admins are denied regardless of grants).
//
// Ordering:
//   1. Admin short-circuit per §Admin bypass — preserves the 0.1.6
//      verbatim behaviour where an admin gets every requested
//      channel, including legacy pre-0.5.0 channel strings that do
//      not match today's `validate_channel` shape.
//   2. `rbac_enforce=false` test seam — non-admins denied (0.1.6
//      fallback). Admin already returned above.
//   3. Syntactic validation — malformed channels never enter a
//      non-admin's subscription set since the broker can never match
//      them against any envelope.
//   4. Rule-derivation + effective-rules set lookup.
auto subscribe_allowed(const ConnState& state, std::string_view channel)
    -> bool {
  if (state.is_admin) {
    return true;
  }
  if (!plinth::realtime::broker::is_rbac_enforced()) {
    return false; // 0.1.6 fallback: non-admin never granted
  }
  if (!plinth::realtime::validate_channel(channel)) {
    return false;
  }
  auto rule = plinth::rbac::derive_subscribe_rule(channel);
  if (rule.empty()) {
    return false;
  }
  return state.effective_rules.contains(rule);
}

// ICD-0.5.5 §8 — set up the per-conn live-buffer machinery on the
// conn's owning loop BEFORE the replay coroutine starts. Pinned out
// of `fire_replay` to keep its cognitive complexity under the tidy
// threshold; runs the same setup the inline lambda used to.
auto post_replay_setup(
    const drogon::WebSocketConnectionPtr& conn, trantor::EventLoop* loop,
    const std::vector<std::string>& granted, std::size_t live_cap,
    const std::shared_ptr<std::atomic<bool>>& abort_flag,
    const std::shared_ptr<std::atomic<std::size_t>>& buffered_live_count)
    -> void {
  if (loop == nullptr) {
    return;
  }
  loop->queueInLoop(
      [conn, granted, live_cap, abort_flag, buffered_live_count]() {
        auto* s = conn->getContext<ConnState>().get();
        if (s == nullptr) {
          return;
        }
        s->live_buffer_cap = live_cap;
        s->replay_abort_flag = abort_flag;
        s->buffered_live_count = buffered_live_count;
        for (const auto& ch : granted) {
          s->replay_in_flight[ch] = true;
          s->live_buffer[ch];
        }
      });
}

// ICD-0.5.5 §8 — post-replay cleanup. Runs on the conn's owning
// loop after `run_replay` returns. On a clean replay_done exit it
// flushes the buffered live frames in arrival (= writer-first seq)
// order; on any resync exit it discards them (the client re-queries
// authoritatively). Clears the per-channel `replay_in_flight` flag
// and releases the shared atomics so subsequent live frames pass
// straight through.
auto post_replay_cleanup(const drogon::WebSocketConnectionPtr& conn,
                         trantor::EventLoop* loop,
                         const std::vector<std::string>& granted,
                         bool resync_fired) -> void {
  if (loop == nullptr) {
    return;
  }
  loop->queueInLoop([conn, granted, resync_fired]() {
    auto* s = conn->getContext<ConnState>().get();
    if (s == nullptr) {
      return;
    }
    for (const auto& ch : granted) {
      auto it = s->live_buffer.find(ch);
      if (it == s->live_buffer.end()) {
        continue;
      }
      if (!resync_fired) {
        for (auto& fp : it->second) {
          if (conn->connected()) {
            conn->send(*fp);
          }
        }
      }
      it->second.clear();
    }
    for (const auto& ch : granted) {
      s->replay_in_flight[ch] = false;
    }
    s->replay_abort_flag.reset();
    s->buffered_live_count.reset();
  });
}

auto build_send_fn(const drogon::WebSocketConnectionPtr& conn,
                   trantor::EventLoop* loop)
    -> std::function<void(std::string)> {
  return [conn, loop](std::string frame) {
    if (loop == nullptr) {
      conn->send(frame);
      return;
    }
    auto frame_shared = std::make_shared<std::string>(std::move(frame));
    loop->queueInLoop([conn, frame_shared]() {
      if (!conn->connected()) {
        return;
      }
      conn->send(*frame_shared);
    });
  };
}

// ICD-0.5.4 §Reconnect Handshake — kick off a replay coroutine for a
// subscribe frame that carried `since_seq`. Snapshots the conn-state
// slice the replay engine needs (so the async coroutine doesn't hold
// a pointer into the conn's owning ConnState past suspension) and
// posts a queue-in-loop send-callback that hops every emitted frame
// onto the conn's owning loop.
auto fire_replay(const drogon::WebSocketConnectionPtr& conn,
                 const ConnState& state, std::int64_t since_seq,
                 std::vector<std::string> granted) -> void {
  const auto CFG = plinth::realtime::events_writer::current_config();
  if (!CFG.enabled) {
    Json::Value resync(Json::objectValue);
    resync["type"] = std::string{plinth::ws::msg::RESYNC};
    resync["reason"] = "events_disabled";
    resync["retention_seconds"] =
        static_cast<Json::UInt64>(CFG.retention_seconds);
    conn->sendJson(resync);
    return;
  }
  plinth::ws::ConnState state_copy;
  state_copy.is_admin = state.is_admin;
  state_copy.authenticated = true;
  state_copy.auth.user_id = state.auth.user_id;
  state_copy.auth.session_id = state.auth.session_id;
  state_copy.effective_rules = state.effective_rules;
  state_copy.channels = state.channels;

  auto* loop = state.loop;
  auto async_task = plinth::lifecycle::async_tasks().try_acquire();
  if (async_task == nullptr) {
    Json::Value resync(Json::objectValue);
    resync["type"] = std::string{plinth::ws::msg::RESYNC};
    resync["reason"] = "server_shutting_down";
    conn->sendJson(resync);
    return;
  }
  auto abort_flag = std::make_shared<std::atomic<bool>>(false);
  auto buffered_live_count = std::make_shared<std::atomic<std::size_t>>(0);
  const auto LIVE_CAP = test_seam::live_buffer_cap_override().value_or(
      CFG.live_buffer_cap_per_subscription);
  post_replay_setup(conn, loop, granted, LIVE_CAP, abort_flag,
                    buffered_live_count);
  auto send_fn = build_send_fn(conn, loop);
  auto cleanup_channels =
      std::make_shared<const std::vector<std::string>>(granted);

  try {
    drogon::async_run(
        // Every capture is owned. `async_task` publishes frame destruction to
        // the process coordinator on success, failure, and cancellation.
        [conn, loop, state_copy = std::move(state_copy),
         send_fn = std::move(send_fn), since_seq,
         granted_copy = std::move(granted), CFG, abort_flag,
         buffered_live_count, cleanup_channels,
         async_task]() mutable -> drogon::Task<> {
          bool resync_fired = true;
          try {
            auto result = co_await plinth::realtime::replay::run_replay(
                state_copy, send_fn, since_seq, std::move(granted_copy), CFG,
                abort_flag, buffered_live_count);
            resync_fired = result.aborted || result.resync.has_value();
          } catch (...) {
          }
          post_replay_cleanup(conn, loop, *cleanup_channels, resync_fired);
        });
  } catch (...) {
    post_replay_cleanup(conn, loop, *cleanup_channels, true);
  }
}

// ICD-0.5.4 §Reconnect Handshake — extract the optional `since_seq`
// claim from a subscribe frame. Returns:
//   - nullopt + ok=true  : no since_seq present (fresh subscribe path)
//   - value  + ok=true   : reconnect path with the given cursor
//   - error code on ok=false : malformed since_seq (must be a non-
//     negative integer per ICD §Error Model)
struct SinceSeqParse {
  std::optional<std::int64_t> value;
  std::string_view error; // empty = no error
};

auto extract_since_seq(const Json::Value& msg) -> SinceSeqParse {
  if (!msg.isMember("since_seq")) {
    return {};
  }
  const auto& v = msg["since_seq"];
  if (!v.isIntegral()) {
    return {.value = std::nullopt, .error = "resubscribe.invalid_since_seq"};
  }
  auto i = v.asInt64();
  if (i < 0) {
    return {.value = std::nullopt, .error = "resubscribe.invalid_since_seq"};
  }
  return {.value = i, .error = {}};
}

// Map `channel_layer` to the stable string used in audit details.
auto layer_token(std::string_view channel) -> std::string_view {
  using plinth::realtime::ChannelLayer;
  if (!plinth::realtime::validate_channel(channel)) {
    return "invalid";
  }
  switch (plinth::realtime::channel_layer(channel)) {
    case ChannelLayer::DATA: return "data";
    case ChannelLayer::SYSTEM: return "system";
    case ChannelLayer::EXTENSION: return "extension";
  }
  return "invalid";
}

} // namespace

auto on_subscribe(const drogon::WebSocketConnectionPtr& conn,
                  const Json::Value& msg) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !state->authenticated) {
    return;
  }

  // ICD-0.5.4 §Reconnect Handshake — parse since_seq up-front so a
  // malformed claim short-circuits BEFORE channel registration.
  auto since_parse = extract_since_seq(msg);
  if (!since_parse.error.empty()) {
    conn->sendJson(plinth::ws::msg::make_error(
        since_parse.error, "since_seq must be a non-negative integer"));
    return;
  }

  auto requested = extract_channels(msg);
  auto max_subs = plinth::realtime::broker::max_subscriptions_per_conn();

  std::vector<std::string> granted;
  granted.reserve(requested.size());
  std::size_t added = 0;

  for (const auto& ch : requested) {
    plinth::log::AuditCtx audit_ctx{.user_id = state->auth.user_id,
                                    .session_id = state->auth.session_id,
                                    .ip_address = peer_ip(conn)};
    bool already = false;
    bool over_quota = false;
    {
      std::lock_guard lk(*state->channels_mu);
      // Idempotent: a channel already in the set is silently
      // re-included in the ack. No counter bump, no RBAC replay.
      if (state->channels.contains(ch)) {
        already = true;
      } else if (state->channels.size() >= max_subs) {
        over_quota = true;
      }
    }
    if (already) {
      granted.push_back(ch);
      continue;
    }
    if (over_quota) {
      plinth::realtime::broker::note_subscribe_denied(
          audit_ctx, ch, layer_token(ch), "quota_exceeded", "ws");
      continue;
    }
    if (!subscribe_allowed(*state, ch)) {
      bool invalid = !plinth::realtime::validate_channel(ch);
      plinth::realtime::broker::note_subscribe_denied(
          audit_ctx, ch, layer_token(ch),
          invalid ? "channel_invalid" : "rbac_denied", "ws");
      continue;
    }
    {
      std::lock_guard lk(*state->channels_mu);
      state->channels.insert(ch);
    }
    // ICD-0.5.5 §11 — reset the gap-detect baseline per subscribe
    // so a re-subscribe after unsubscribe does NOT carry the prior
    // sequence state forward; the first live frame after subscribe
    // establishes a new baseline (no false-positive gap fires
    // across the subscribe boundary).
    state->last_live_seen_seq[ch] = 0;
    granted.push_back(ch);
    added += 1;
  }
  if (added > 0) {
    note_subscriptions_added(added);
  }

  conn->sendJson(make_ack("subscribed", granted));

  Json::Value detail;
  Json::Value req_arr(Json::arrayValue);
  for (const auto& c : requested) {
    req_arr.append(c);
  }
  Json::Value granted_arr(Json::arrayValue);
  for (const auto& c : granted) {
    granted_arr.append(c);
  }
  detail["requested"] = req_arr;
  detail["granted"] = granted_arr;
  plinth::log::audit("ws.subscribed", detail,
                     {.user_id = state->auth.user_id,
                      .session_id = state->auth.session_id,
                      .ip_address = peer_ip(conn)});

  // ICD-0.5.4 §Reconnect Handshake — replay branch fires AFTER the
  // `subscribed` ack so live frames queue behind replay frames in
  // the conn's FIFO send queue. Per ICD §Why register live subs
  // BEFORE replay: live subscriptions are already inserted into
  // `state->channels` above; live envelopes arriving while replay
  // is in flight will be queued by `publish_dispatched` after the
  // replay frames already enqueued via the SendFn closure.
  if (since_parse.value.has_value()) {
    fire_replay(conn, *state, *since_parse.value, granted);
  }
}

auto on_unsubscribe(const drogon::WebSocketConnectionPtr& conn,
                    const Json::Value& msg) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !state->authenticated) {
    return;
  }

  auto requested = extract_channels(msg);
  std::vector<std::string> removed;
  removed.reserve(requested.size());
  {
    std::lock_guard lk(*state->channels_mu);
    for (const auto& ch : requested) {
      if (state->channels.erase(ch) > 0) {
        removed.push_back(ch);
      }
    }
  }
  // ICD-0.5.5 §11 — drop the gap-detect baseline on unsubscribe so
  // the map does not grow unboundedly across long-lived connections
  // that subscribe/unsubscribe many channels.
  for (const auto& ch : removed) {
    state->last_live_seen_seq.erase(ch);
  }
  if (!removed.empty()) {
    note_subscriptions_removed(removed.size());
  }

  conn->sendJson(make_ack("unsubscribed", removed));

  Json::Value detail;
  Json::Value arr(Json::arrayValue);
  for (const auto& c : removed) {
    arr.append(c);
  }
  detail["channels"] = arr;
  plinth::log::audit("ws.unsubscribed", detail,
                     {.user_id = state->auth.user_id,
                      .session_id = state->auth.session_id,
                      .ip_address = peer_ip(conn)});
}

// ── ICD-0.5.5 §7 — `debounce_renegotiate` audit pipeline ─────────────
//
// Sliding-window dedup keyed on (user_id, channel). The first
// renegotiate frame in a window emits the audit; subsequent frames
// inside the same window bump an in-memory counter; when the next
// frame after window-close fires, `count_in_window` reflects the
// aggregated total. Mirrors the 0.5.2 broker's `claim_audit_slot`
// shape with the operator-tunable window from
// `events.audit_window_ms` (0.5.4 knob, reused per ICD §11).

namespace {

struct DebounceAuditWindow {
  std::chrono::steady_clock::time_point first_ts;
  std::uint64_t count{0};
};

// module-local audit-window state mirrors the broker's pattern
std::mutex g_debounce_mu;
std::unordered_map<std::string, DebounceAuditWindow> g_debounce_windows;
std::atomic<std::uint64_t> g_debounce_emit_count{0};

auto debounce_window_key(std::string_view user_id, std::string_view channel)
    -> std::string {
  std::string k;
  k.reserve(user_id.size() + channel.size() + 1);
  k.append(user_id);
  k.push_back('\0');
  k.append(channel);
  return k;
}

// Returns (emit_now, count_in_window). Honours `audit_window_ms`
// from the events config (current_config snapshot).
auto claim_debounce_audit_slot(const std::string& key,
                               std::chrono::steady_clock::time_point now,
                               std::chrono::milliseconds window)
    -> std::pair<bool, std::uint64_t> {
  std::lock_guard lock(g_debounce_mu);
  auto& win = g_debounce_windows[key];
  if (win.count == 0 || now - win.first_ts > window) {
    win.first_ts = now;
    win.count = 1;
    return {true, 1};
  }
  win.count += 1;
  return {false, win.count};
}

} // namespace

auto on_debounce_renegotiate(const drogon::WebSocketConnectionPtr& conn,
                             const Json::Value& msg) -> void {
  auto* state = conn->getContext<ConnState>().get();
  if (state == nullptr || !state->authenticated) {
    return;
  }
  // Channel is required for the audit's window key. Frames missing
  // it are silently dropped — the kernel never enforces, so a
  // malformed renegotiate has no effect either way; logging at
  // debug keeps the audit pipeline focused on real overrides.
  if (!msg.isMember("channel") || !msg["channel"].isString()) {
    return;
  }
  const auto CHANNEL = msg["channel"].asString();
  const auto OVERRIDE =
      msg.isMember("debounce_ms") ? msg["debounce_ms"].asInt64() : -1;
  const auto CFG = plinth::realtime::events_writer::current_config();
  const auto KEY = debounce_window_key(state->auth.user_id, CHANNEL);
  const auto WINDOW = std::chrono::milliseconds(CFG.audit_window_ms);
  auto [emit, cnt] =
      claim_debounce_audit_slot(KEY, std::chrono::steady_clock::now(), WINDOW);
  if (!emit) {
    return;
  }
  g_debounce_emit_count.fetch_add(1, std::memory_order_relaxed);
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  Json::Value payload(Json::objectValue);
  payload["user_id"] = state->auth.user_id;
  payload["channel"] = CHANNEL;
  payload["advisory_ms"] = static_cast<Json::UInt64>(CFG.debounce.recommend_ms);
  payload["override_ms"] = static_cast<Json::Int64>(OVERRIDE);
  payload["count_in_window"] = static_cast<Json::UInt64>(cnt);
  payload["window_ms"] = static_cast<Json::UInt64>(CFG.audit_window_ms);
  plinth::log::audit("realtime.debounce.advisory_overridden", payload,
                     {.user_id = state->auth.user_id,
                      .session_id = state->auth.session_id,
                      .ip_address = peer_ip(conn)});
}

auto reset_debounce_audit_state_for_test() -> void {
  std::lock_guard lock(g_debounce_mu);
  g_debounce_windows.clear();
  g_debounce_emit_count.store(0, std::memory_order_relaxed);
}

auto debounce_audit_emit_count_for_test() -> std::uint64_t {
  return g_debounce_emit_count.load(std::memory_order_relaxed);
}

auto run_debounce_renegotiate_for_test(std::string_view user_id,
                                       std::string_view channel,
                                       std::int64_t override_ms)
    -> std::pair<bool, std::uint64_t> {
  const auto CFG = plinth::realtime::events_writer::current_config();
  const auto KEY = debounce_window_key(user_id, channel);
  const auto WINDOW = std::chrono::milliseconds(CFG.audit_window_ms);
  auto [emit, cnt] =
      claim_debounce_audit_slot(KEY, std::chrono::steady_clock::now(), WINDOW);
  if (emit) {
    g_debounce_emit_count.fetch_add(1, std::memory_order_relaxed);
  }
  (void)override_ms; // Audit-write is gated by is_audit_ready() in
                     // the production path; tests don't bring up
                     // the audit pipeline so we report claim-state
                     // only and skip the actual log call here.
  return {emit, cnt};
}

auto subscribed_ack_for_test(const std::vector<std::string>& channels)
    -> Json::Value {
  return make_ack(plinth::ws::msg::SUBSCRIBED, channels);
}

namespace test_seam {

auto live_buffer_cap_override() -> std::optional<std::size_t> {
  const auto OVERRIDE_VAL = g_live_cap_override.load(std::memory_order_acquire);
  if (OVERRIDE_VAL == LIVE_CAP_SENTINEL_NONE) {
    return std::nullopt;
  }
  return OVERRIDE_VAL;
}

auto set_live_buffer_cap_override(std::size_t cap) -> void {
  if (cap == LIVE_CAP_SENTINEL_NONE) {
    cap = LIVE_CAP_SENTINEL_NONE - 1;
  }
  g_live_cap_override.store(cap, std::memory_order_release);
}

auto clear_live_buffer_cap_override() -> void {
  g_live_cap_override.store(LIVE_CAP_SENTINEL_NONE, std::memory_order_release);
}

} // namespace test_seam

} // namespace plinth::ws
