#include "kernel/realtime/broker.hpp"

#include "kernel/js/bridge_context.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/channel.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/ws/publish.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <drogon/drogon.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace plinth::realtime::broker {

namespace {

// ── State ───────────────────────────────────────────────────────────

// JS-side subscription registry: one entry per (bc, channel) pair
// with the JS-binding's callback_id. The actual JSValue handler is
// owned by the bc's callbacks map — the broker only holds the id.
// Distinct from `ConnState::channels` per §OQ4 (different identity +
// lifetime + reachability — see ICD §JS-side subscription registry).
using BcSubscriptions =
    std::unordered_map<std::string, int>; // channel → callback_id
using BcRegistry =
    std::unordered_map<plinth::js::BridgeContext*, BcSubscriptions>;

// ICD-0.5.2 §Audit Events — sliding-window rate limiter. A single
// audit per (user_id, channel) per minute on the deny path, and a
// single audit per channel per minute on the dispatch-skipped path.
// Subsequent denials within the window are counted but not audited
// (the count rides on the emitted audit so operators can see how
// many suppressed signals there were).
constexpr auto AUDIT_WINDOW = std::chrono::seconds(60);

struct SlidingWindow {
  std::chrono::steady_clock::time_point first_ts;
  std::uint64_t count = 0;
};

struct DeniedKey {
  std::string user_id;
  std::string channel;
  auto operator==(const DeniedKey&) const -> bool = default;
};
struct DeniedKeyHash {
  auto operator()(const DeniedKey& k) const -> std::size_t {
    return std::hash<std::string>{}(k.user_id) ^
           (std::hash<std::string>{}(k.channel) << 1U);
  }
};

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_started{false};
std::atomic<bool> g_rbac_enforce{true};
std::atomic<std::size_t> g_max_subs_per_conn{64};

std::atomic<std::uint64_t> g_dispatch_count{0};
std::atomic<std::uint64_t> g_rbac_denial_count{0};

std::shared_mutex g_bc_mu;
BcRegistry g_bc_registry;

std::mutex g_lifecycle_mu;

std::mutex g_audit_mu;
std::unordered_map<DeniedKey, SlidingWindow, DeniedKeyHash> g_denied_windows;
std::unordered_map<std::string, SlidingWindow> g_skipped_windows;

// Returns (should_emit, count_in_window_after_hit). Caller holds
// g_audit_mu. Atomically transitions a window: a fresh/expired key
// resets the timestamp + sets count=1 and returns emit=true; an
// in-window key bumps count and returns emit=false.
template <typename Map, typename Key>
auto claim_audit_slot(Map& map, const Key& key,
                      std::chrono::steady_clock::time_point now)
    -> std::pair<bool, std::uint64_t> {
  auto& win = map[key];
  if (win.count == 0 || now - win.first_ts > AUDIT_WINDOW) {
    win.first_ts = now;
    win.count = 1;
    return {true, 1};
  }
  win.count += 1;
  return {false, win.count};
}

// ── Writer-downstream dispatch entry point (ICD-0.5.5 §5) ───────────

// Forward for the dispatch body. Defined later in this TU.
auto dispatch_to_js_subscribers(const DispatchedEvent& ev) -> std::size_t;

// ── JS-side dispatch ────────────────────────────────────────────────

auto dispatch_to_js_subscribers(const DispatchedEvent& ev) -> std::size_t {
  // Snapshot matching subscriptions under the shared lock, then
  // dispatch outside the lock (so the bc's invoke_callback call
  // chain — which may reach back into the runtime's loop — does not
  // hold the broker's registry mutex).
  std::vector<plinth::js::BridgeContext*> matches;
  {
    std::shared_lock lock(g_bc_mu);
    for (const auto& [bc, subs] : g_bc_registry) {
      if (subs.contains(ev.channel)) {
        matches.push_back(bc);
      }
    }
  }
  if (matches.empty()) {
    return 0;
  }
  // Hop onto drogon's main loop — the bc's JSContext is only safe
  // to touch there. The envelope is shared_ptr'd across lambdas so
  // each subscriber gets a read-only copy without re-serializing.
  auto envelope_shared = std::make_shared<Json::Value>(ev.envelope);
  std::string channel_str{ev.channel};
  auto* main_loop = drogon::app().getLoop();
  if (main_loop == nullptr) {
    return matches.size();
  }
  for (auto* bc : matches) {
    main_loop->queueInLoop([bc, envelope_shared, channel_str]() {
      if (bc == nullptr) {
        return;
      }
      // Liveness check — between the listener-thread snapshot
      // and this loop-hop a pool destroy could have run
      // `drop_bc_subscriptions`, in which case the bc is
      // either destroyed or reset and invoking it would
      // reach into freed JS state. The registry presence is
      // the canonical "is this subscription still live"
      // signal.
      {
        std::shared_lock reg_lock(g_bc_mu);
        if (!g_bc_registry.contains(bc)) {
          return;
        }
      }
      if (bc->cancelled.load(std::memory_order_acquire)) {
        return;
      }
      bc->invoke_callback(channel_str, *envelope_shared);
    });
  }
  return matches.size();
}

// ── Drain-helpers ───────────────────────────────────────────────────

// True when `channel` belongs to extension `name`. Layer-1 prefix is
// `plinth:data:ext_<name>.` (matching the ext_-stripped extension
// name per ICD-0.5.0 §Channel Naming Scheme). Layer-3 prefix is
// `plinth:ext:<name>:`. Kernel-schema Layer-1 channels (e.g.
// `plinth:data:plinth.users`) are unaffected by extension drain.
auto channel_belongs_to_extension(std::string_view channel,
                                  std::string_view name) -> bool {
  const std::string DATA_PFX = "plinth:data:ext_" + std::string{name} + ".";
  const std::string EXT_PFX = "plinth:ext:" + std::string{name} + ":";
  return channel.starts_with(DATA_PFX) || channel.starts_with(EXT_PFX);
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

auto start(const Config::Realtime::Broker& broker_cfg) -> void {
  std::lock_guard lock(g_lifecycle_mu);
  if (g_started.load()) {
    return; // idempotent
  }
  if (!broker_cfg.enabled) {
    spdlog::info("realtime broker: disabled by config");
    g_started.store(true);
    // g_enabled stays false — handler short-circuits if still
    // registered from a prior start/stop cycle (testing seam).
    return;
  }
  g_max_subs_per_conn.store(broker_cfg.max_subscriptions_per_conn,
                            std::memory_order_release);
  g_rbac_enforce.store(broker_cfg.rbac_enforce, std::memory_order_release);
  g_enabled.store(true, std::memory_order_release);
  g_started.store(true);

  // ICD-0.5.5 §5 — broker is no longer a peer listener handler. The
  // events writer is the sole listener consumer; it invokes
  // `broker::dispatch` after the `INSERT … RETURNING seq` stamp so
  // every fanned envelope carries the canonical seq by construction.
  spdlog::info(
      "realtime broker: started (max_subs_per_conn={}, rbac_enforce={})",
      broker_cfg.max_subscriptions_per_conn, broker_cfg.rbac_enforce);
}

auto stop() -> void {
  std::lock_guard lock(g_lifecycle_mu);
  if (!g_started.load()) {
    return;
  }
  // Flip the flag first so any in-flight handler invocation on the
  // listener thread short-circuits on its next entry.
  g_enabled.store(false, std::memory_order_release);

  // Clear JS-side registry under unique_lock. WS-side ConnState is
  // not touched here — ConnectionRegistry::initiate_shutdown (which
  // runs earlier in the coordinator graph) has already signalled the
  // per-conn loops to drain.
  {
    std::unique_lock reg_lock(g_bc_mu);
    g_bc_registry.clear();
  }
  g_started.store(false);

  // Final metric counts use spdlog only. The historical atexit path raced a
  // torn-down DbClient (5/5 SEGV repro on 2026-04-27); the coordinator now
  // keeps both the database and spdlog alive through this step. spdlog is
  // safe across the entire teardown chain — the file/console sinks
  // are owned by spdlog's static state and outlive every plinth
  // subsystem's `stop()`.
  spdlog::info(
      "realtime broker: stopped (dispatch_count={}, rbac_denial_count={})",
      g_dispatch_count.load(std::memory_order_relaxed),
      g_rbac_denial_count.load(std::memory_order_relaxed));
}

auto drain_extension(std::string_view name) -> void {
  drain_extension(name, "manual");
}

auto drain_extension(std::string_view name, std::string_view trigger) -> void {
  if (!g_started.load()) {
    return;
  }
  std::size_t js_removed = 0;

  // JS-side drain — iterate every bc's subscriptions, erase matches.
  // Also drop any bc whose extension_name matches `name` outright
  // (extension-owned bcs lose their slot on UPGRADE/UNINSTALL per
  // RuntimePool lifecycle, but we defensively clear here so drain is
  // idempotent + covers the edge case of kernel-scope bcs holding
  // subscriptions into the extension's namespace).
  {
    std::unique_lock reg_lock(g_bc_mu);
    for (auto bc_it = g_bc_registry.begin(); bc_it != g_bc_registry.end();) {
      auto& subs = bc_it->second;
      for (auto sub_it = subs.begin(); sub_it != subs.end();) {
        if (channel_belongs_to_extension(sub_it->first, name)) {
          sub_it = subs.erase(sub_it);
          js_removed += 1;
        } else {
          ++sub_it;
        }
      }
      // Drop the bc entirely if the bc itself belongs to this
      // extension (pooled slot is going away on UPGRADE/UNINSTALL).
      if (bc_it->first != nullptr && bc_it->first->extension_name == name) {
        js_removed += bc_it->second.size();
        bc_it = g_bc_registry.erase(bc_it);
      } else if (subs.empty()) {
        bc_it = g_bc_registry.erase(bc_it);
      } else {
        ++bc_it;
      }
    }
  }

  // WS-side drain — ConnState::channels eviction. Handled by
  // iterating ConnectionRegistry snapshot + posting queueInLoop
  // lambdas that erase matching channels. The ws module owns the
  // snapshot primitive; broker calls a helper there.
  std::size_t ws_removed =
      plinth::ws::drain_ws_subscriptions_for_extension(name);

  if ((js_removed > 0 || ws_removed > 0) && plinth::log::is_audit_ready()) {
    Json::Value payload(Json::objectValue);
    payload["extension_name"] = std::string{name};
    payload["ws_subscriptions_removed"] = static_cast<Json::UInt64>(ws_removed);
    payload["js_subscriptions_removed"] = static_cast<Json::UInt64>(js_removed);
    payload["trigger"] = std::string{trigger};
    plinth::log::audit("realtime.broker.extension_drained", payload,
                       plinth::log::AuditCtx{});
  }
}

auto register_js_subscription(plinth::js::BridgeContext* bc,
                              std::string_view channel, int callback_id)
    -> bool {
  std::unique_lock lock(g_bc_mu);
  auto& subs = g_bc_registry[bc];
  if (subs.size() >= g_max_subs_per_conn.load(std::memory_order_acquire) &&
      !subs.contains(std::string{channel})) {
    if (subs.empty()) {
      g_bc_registry.erase(bc); // don't leave an empty entry
    }
    return false;
  }
  subs[std::string{channel}] = callback_id;
  return true;
}

auto unregister_js_subscription(plinth::js::BridgeContext* bc,
                                std::string_view channel) -> void {
  std::unique_lock lock(g_bc_mu);
  auto bc_it = g_bc_registry.find(bc);
  if (bc_it == g_bc_registry.end()) {
    return;
  }
  bc_it->second.erase(std::string{channel});
  if (bc_it->second.empty()) {
    g_bc_registry.erase(bc_it);
  }
}

auto drop_bc_subscriptions(plinth::js::BridgeContext* bc) -> void {
  std::unique_lock lock(g_bc_mu);
  g_bc_registry.erase(bc);
}

auto dispatch(const DispatchedEvent& ev) -> std::size_t {
  // ICD-0.5.5 §5 — writer-downstream entry point. Called from
  // `events_writer::insert_envelope` on the production path after
  // the INSERT-and-stamp; called from `dispatch_for_test` on the
  // synthetic path. Honours the `broker_enabled` flag (post-stop
  // invocations short-circuit + audit). Returns the JS-side
  // dispatch count; WS-side fan-out is reported via
  // `ws_subscriber_count` because each delivery is queued onto a
  // per-conn loop.
  if (!g_enabled.load(std::memory_order_acquire)) {
    // Writer keeps invoking us after broker::stop() during
    // shutdown — the listener can still drain into the writer
    // queue between stop_listener() and events_writer::stop().
    // Record the skipped dispatch once per minute per channel
    // per ICD §Audit Events.
    plinth::realtime::broker::note_dispatch_skipped(ev.channel,
                                                    "broker_disabled");
    return 0;
  }
  // WS arm + JS arm fan out in parallel off one writer-side call.
  plinth::ws::publish_dispatched(ev);
  auto js_n = dispatch_to_js_subscribers(ev);
  g_dispatch_count.fetch_add(1, std::memory_order_relaxed);
  return js_n;
}

auto dispatch_for_test(const DispatchedEvent& ev) -> std::size_t {
  // Test seam — same body as the production `dispatch`; kept under
  // its own name for the existing ICD-0.5.2 B.* tests so they
  // continue to read as "dispatch through the broker as the writer
  // would". Returns the JS-match count.
  return dispatch(ev);
}

auto set_rbac_enforce_for_test(bool on) -> void {
  g_rbac_enforce.store(on, std::memory_order_release);
}

auto is_rbac_enforced() -> bool {
  return g_rbac_enforce.load(std::memory_order_acquire);
}

auto max_subscriptions_per_conn() -> std::size_t {
  return g_max_subs_per_conn.load(std::memory_order_acquire);
}

auto note_rbac_denial() -> void {
  g_rbac_denial_count.fetch_add(1, std::memory_order_relaxed);
}

auto reset_audit_windows_for_test() -> void {
  std::lock_guard lock(g_audit_mu);
  g_denied_windows.clear();
  g_skipped_windows.clear();
}

auto note_subscribe_denied(const plinth::log::AuditCtx& ctx,
                           std::string_view channel, std::string_view layer,
                           std::string_view reason, std::string_view source)
    -> void {
  // Metric bump is unconditional per ICD §Metrics — even when the
  // audit itself is suppressed by the rate limiter, the denial
  // counter moves.
  g_rbac_denial_count.fetch_add(1, std::memory_order_relaxed);

  bool emit = false;
  std::uint64_t cnt = 0;
  {
    std::lock_guard lock(g_audit_mu);
    std::tie(emit, cnt) = claim_audit_slot(
        g_denied_windows,
        DeniedKey{.user_id = ctx.user_id, .channel = std::string{channel}},
        std::chrono::steady_clock::now());
  }
  if (!emit) {
    return;
  }
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  Json::Value detail;
  detail["channel"] = std::string{channel};
  detail["layer"] = std::string{layer};
  detail["reason"] = std::string{reason};
  detail["source"] = std::string{source};
  detail["denied_in_window"] = static_cast<Json::UInt64>(cnt);
  plinth::log::audit("realtime.broker.subscribe_denied", detail, ctx);
}

auto note_dispatch_skipped(std::string_view channel, std::string_view reason)
    -> void {
  bool emit = false;
  std::uint64_t cnt = 0;
  {
    std::lock_guard lock(g_audit_mu);
    std::tie(emit, cnt) =
        claim_audit_slot(g_skipped_windows, std::string{channel},
                         std::chrono::steady_clock::now());
  }
  if (!emit) {
    return;
  }
  if (!plinth::log::is_audit_ready()) {
    return;
  }
  Json::Value detail;
  detail["channel"] = std::string{channel};
  detail["reason"] = std::string{reason};
  detail["skipped_in_window"] = static_cast<Json::UInt64>(cnt);
  plinth::log::audit("realtime.broker.dispatch_skipped", detail,
                     plinth::log::AuditCtx{});
}

auto dispatch_count() -> std::uint64_t {
  return g_dispatch_count.load(std::memory_order_relaxed);
}

auto rbac_denial_count() -> std::uint64_t {
  return g_rbac_denial_count.load(std::memory_order_relaxed);
}

auto js_subscriber_count() -> std::size_t {
  std::shared_lock lock(g_bc_mu);
  std::size_t total = 0;
  for (const auto& [_bc, subs] : g_bc_registry) {
    total += subs.size();
  }
  return total;
}

auto ws_subscriber_count() -> std::size_t {
  return plinth::ws::total_subscription_count();
}

auto reset_metrics_for_test() -> void {
  g_dispatch_count.store(0, std::memory_order_relaxed);
  g_rbac_denial_count.store(0, std::memory_order_relaxed);
}

} // namespace plinth::realtime::broker
