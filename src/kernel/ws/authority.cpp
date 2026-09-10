// SPDX-License-Identifier: MIT
#include "kernel/ws/authority.hpp"

#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/ws/close_codes.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/messages.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <drogon/drogon.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace plinth::ws {
namespace {

using Clock = std::chrono::steady_clock;

// Identity and rule snapshots are immutable after authentication. Closing on
// any change keeps replay snapshots and queued work from adopting new rights.
auto reject_authority(const drogon::WebSocketConnectionPtr& conn) -> void {
  auto state = conn->getContext<ConnState>();
  if (!state) {
    return;
  }
  state->authority_until_ms->store(0, std::memory_order_release);
  state->auth_failed = true;
  state->authority_stopped = true;
  if (state->replay_abort_flag) {
    state->replay_abort_flag->store(true, std::memory_order_release);
  }
  state->live_buffer.clear();
  if (state->authority_timer_id != trantor::InvalidTimerId) {
    state->loop->invalidateTimer(state->authority_timer_id);
    state->authority_timer_id = trantor::InvalidTimerId;
  }
  if (conn->connected()) {
    conn->sendJson(
        msg::make_error("auth_failed", "Authentication must be renewed"));
    conn->shutdown(to_drogon(WsCloseCode::AUTH_FAILED), "auth_failed");
  }
}

struct Snapshot {
  bool valid{false};
  std::string username;
  std::unordered_set<std::string> rules;
  Clock::time_point deadline;
};

auto read_snapshot(const drogon::orm::Result& result, Clock::time_point started)
    -> Snapshot {
  Snapshot snapshot;
  if (result.empty()) {
    return snapshot;
  }
  const auto ttl = result[0]["ttl"].as<double>();
  if (!std::isfinite(ttl) || ttl <= 0) {
    return snapshot;
  }
  snapshot.deadline =
      started + std::chrono::milliseconds{
                    static_cast<std::int64_t>(std::min(ttl, 2.0) * 1000.0)};
  snapshot.username = result[0]["username"].as<std::string>();
  for (const auto& row : result) {
    if (!row["rule"].isNull()) {
      snapshot.rules.insert(row["rule"].as<std::string>());
    }
  }
  snapshot.valid = true;
  return snapshot;
}

auto apply_snapshot(const drogon::WebSocketConnectionPtr& conn,
                    Snapshot snapshot, std::function<void()> ready) -> void {
  auto state = conn->getContext<ConnState>();
  if (!state || state->authority_stopped || state->auth_failed ||
      !conn->connected()) {
    return;
  }
  state->authority_refresh_pending = false;
  if (!snapshot.valid || Clock::now() >= snapshot.deadline ||
      snapshot.username != state->auth.username ||
      (state->authenticated && (!authority_is_current(*state) ||
                                snapshot.rules != state->effective_rules))) {
    reject_authority(conn);
    return;
  }
  if (!state->authenticated) {
    state->effective_rules = std::move(snapshot.rules);
    state->is_admin = state->effective_rules.contains("kernel.admin");
  }
  state->authority_until_ms->store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          snapshot.deadline.time_since_epoch())
          .count(),
      std::memory_order_release);
  if (ready) {
    ready();
  }
}

auto refresh_authority(const drogon::WebSocketConnectionPtr& conn,
                       std::function<void()> ready = {}) -> void {
  auto state = conn->getContext<ConnState>();
  if (!state || state->authority_stopped || state->auth_failed ||
      !conn->connected()) {
    return;
  }
  if (state->authenticated && !authority_is_current(*state)) {
    reject_authority(conn);
    return;
  }
  if (state->authority_refresh_pending) {
    return;
  }
  auto task = plinth::lifecycle::async_tasks().try_acquire();
  if (!task || state->loop == nullptr) {
    reject_authority(conn);
    return;
  }
  state->authority_refresh_pending = true;
  const auto started = Clock::now();
  auto* loop = state->loop;
  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  // Drogon's timeout can finish the caller before releasing the server-side
  // result closure. Only the winning completion owns the lifecycle lease;
  // a late/suppressed database callback cannot pin shutdown or touch the loop.
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto task_owner =
      std::make_shared<std::shared_ptr<plinth::lifecycle::AsyncTaskLease>>(
          std::move(task));
  auto complete = [weak, loop, task_owner, completed,
                   ready = std::move(ready)](Snapshot snapshot) {
    if (completed->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    auto completion_task = std::exchange(*task_owner, {});
    loop->queueInLoop([weak, completion_task, ready,
                       snapshot = std::move(snapshot)]() mutable {
      if (auto strong = weak.lock()) {
        apply_snapshot(strong, std::move(snapshot), std::move(ready));
      }
    });
  };
  const bool session = state->auth.auth_type == "session";
  const auto query =
      std::string{"SELECT u.username, COALESCE(EXTRACT(EPOCH FROM "
                  "(c.expires_at - statement_timestamp())), 2)::double "
                  "precision AS ttl, r.rule "
                  "FROM plinth."} +
      (session ? "sessions" : "pats") +
      " c JOIN plinth.users u ON u.id = c.user_id "
      "LEFT JOIN plinth.group_members gm ON gm.user_id = u.id "
      "LEFT JOIN plinth.group_rules gr ON gr.group_id = gm.group_id "
      "LEFT JOIN plinth.rbac_rules r ON r.id = gr.rule_id "
      "WHERE c.id = $1::uuid AND c.user_id = $2::uuid AND c.token_hash = $3 "
      "AND c.revoked_at IS NULL AND u.disabled_at IS NULL "
      "AND (c.expires_at IS NULL OR c.expires_at > statement_timestamp())";
  try {
    drogon::app()
        .getDbClient("ws_authority")
        ->execSqlAsync(
            query,
            [complete, started](const drogon::orm::Result& result) {
              try {
                complete(read_snapshot(result, started));
              } catch (const std::exception&) {
                complete({});
              }
            },
            [complete](const drogon::orm::DrogonDbException&) { complete({}); },
            session ? state->auth.session_id : state->auth.pat_id,
            state->auth.user_id, state->auth.token_hash);
  } catch (const std::exception&) {
    complete({});
  }
}

} // namespace

auto establish_authority(const drogon::WebSocketConnectionPtr& conn,
                         std::function<void()> ready) -> void {
  refresh_authority(conn, std::move(ready));
}

auto start_authority_monitor(const drogon::WebSocketConnectionPtr& conn)
    -> void {
  auto state = conn->getContext<ConnState>();
  if (!state || state->loop == nullptr || state->authority_stopped) {
    return;
  }
  std::weak_ptr<drogon::WebSocketConnection> weak{conn};
  state->authority_timer_id = state->loop->runEvery(1.0, [weak]() {
    if (auto strong = weak.lock()) {
      refresh_authority(strong);
    }
  });
}

auto stop_authority_monitor(const drogon::WebSocketConnectionPtr& conn)
    -> void {
  auto state = conn->getContext<ConnState>();
  if (!state) {
    return;
  }
  state->authority_stopped = true;
  state->authority_until_ms->store(0, std::memory_order_release);
  if (state->replay_abort_flag) {
    state->replay_abort_flag->store(true, std::memory_order_release);
  }
  if (state->loop && state->authority_timer_id != trantor::InvalidTimerId) {
    state->loop->invalidateTimer(state->authority_timer_id);
    state->authority_timer_id = trantor::InvalidTimerId;
  }
}

} // namespace plinth::ws
