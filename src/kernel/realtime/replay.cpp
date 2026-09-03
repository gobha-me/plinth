#include "kernel/realtime/replay.hpp"

#include "kernel/logging.hpp"
#include "kernel/rbac/subscribe_rule.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/ws/messages.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility>

namespace plinth::realtime::replay {

namespace {

struct SlidingWindow {
  std::chrono::steady_clock::time_point first_ts;
  std::uint64_t count{0};
};

// module-local replay audit + test-seam state
std::mutex g_test_mu;
drogon::orm::DbClientPtr g_test_db_client;

std::mutex g_audit_mu;
std::unordered_map<std::string, SlidingWindow> g_audit_windows;

auto db_client() -> drogon::orm::DbClientPtr {
  {
    std::lock_guard lock(g_test_mu);
    if (g_test_db_client) {
      return g_test_db_client;
    }
  }
  return drogon::app().getDbClient();
}

// switch over a stable enum, complexity inflated by clang-tidy's branch
// counter.
auto resync_reason_str(ResyncReason r) -> const char* {
  switch (r) {
    case ResyncReason::CURSOR_EXPIRED: return "cursor_expired";
    case ResyncReason::ROW_CAP: return "row_cap";
    case ResyncReason::MISMATCH: return "mismatch";
    case ResyncReason::LIVE_BUFFER_OVERFLOW: return "live_buffer_overflow";
  }
  return "unknown"; // unreachable; switch is exhaustive
}

auto serialize(const Json::Value& v) -> std::string {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

auto rbac_allows(const ws::ConnState& state, std::string_view channel) -> bool {
  if (state.is_admin) {
    return true;
  }
  if (!plinth::realtime::broker::is_rbac_enforced()) {
    return false;
  }
  auto rule = plinth::rbac::derive_subscribe_rule(channel);
  if (rule.empty()) {
    return false;
  }
  return state.effective_rules.contains(rule);
}

// Audit emitter — rate-limited per (audit_name, user_id) sliding window.
auto audit_event(std::string_view action, std::string_view user_id,
                 const Json::Value& detail, std::size_t audit_window_ms)
    -> void {
  bool emit = false;
  std::uint64_t cnt = 0;
  {
    std::lock_guard lock(g_audit_mu);
    const std::string KEY = std::string{action} + '\0' + std::string{user_id};
    auto& win = g_audit_windows[KEY];
    const auto NOW = std::chrono::steady_clock::now();
    if (win.count == 0 ||
        NOW - win.first_ts > std::chrono::milliseconds(audit_window_ms)) {
      win.first_ts = NOW;
      win.count = 1;
      emit = true;
      cnt = 1;
    } else {
      win.count += 1;
      cnt = win.count;
    }
  }
  if (!emit || !plinth::log::is_audit_ready()) {
    return;
  }
  auto payload = detail;
  payload["count_in_window"] = static_cast<Json::UInt64>(cnt);
  payload["window_ms"] = static_cast<Json::UInt64>(audit_window_ms);
  plinth::log::audit(action, payload, plinth::log::AuditCtx{});
}

// Emit a `{type:"resync", reason, retention_seconds}` frame and audit
// `realtime.events.resync_required`. Extracted so each precondition
// branch in `run_replay` is a one-liner.
auto emit_resync_with_audit(const SendFn& send, ResyncReason reason,
                            const ws::ConnState& state, std::int64_t since_seq,
                            const Config::Realtime::Events& cfg) -> void {
  send(build_resync_frame(reason, cfg.retention_seconds));
  Json::Value detail(Json::objectValue);
  detail["user_id"] = state.auth.user_id;
  detail["since_seq"] = static_cast<Json::Int64>(since_seq);
  detail["reason"] = std::string{resync_reason_str(reason)};
  detail["retention_seconds"] =
      static_cast<Json::UInt64>(cfg.retention_seconds);
  audit_event("realtime.events.resync_required", state.auth.user_id, detail,
              cfg.audit_window_ms);
}

// Build the PG `text[]` array literal — `{"ch1","ch2",...}`. Channel
// strings come from rbac-vetted granted_channels and never contain
// quotes or backslashes (validated upstream by `realtime::validate_channel`),
// so the simple quoting suffices. Drogon's SqlBinder doesn't bind
// std::vector<std::string> to a text[] param, so we format the literal
// here and pass it as a single text param.
auto build_text_array_literal(const std::vector<std::string>& xs)
    -> std::string {
  std::string out = "{";
  bool first = true;
  for (const auto& x : xs) {
    if (!first) {
      out += ',';
    }
    out += '"';
    out += x;
    out += '"';
    first = false;
  }
  out += '}';
  return out;
}

// Parse the JSONB payload column into a `Json::Value`. Returns nullopt
// on malformed JSON; caller logs + skips.
auto parse_payload(const std::string& payload_str)
    -> std::optional<Json::Value> {
  Json::CharReaderBuilder rb;
  Json::Value envelope;
  std::string err;
  const auto READER = std::unique_ptr<Json::CharReader>(rb.newCharReader());
  // CharReader::parse mandates a (begin, end) pair into a contiguous buffer;
  // std::string::data() + size() is the canonical pattern.
  const auto* end = payload_str.data() + payload_str.size();
  if (!READER->parse(payload_str.data(), end, &envelope, &err)) {
    return std::nullopt;
  }
  return envelope;
}

} // namespace

// ── Frame builders ──────────────────────────────────────────────────

auto build_replay_frame(const Json::Value& envelope) -> std::string {
  Json::Value v;
  v["type"] = ws::msg::REPLAY;
  v["envelope"] = envelope;
  return serialize(v);
}

auto build_replay_done_frame(std::int64_t up_to_seq, std::size_t row_count,
                             std::size_t buffered_live_count) -> std::string {
  Json::Value v;
  v["type"] = ws::msg::REPLAY_DONE;
  v["up_to_seq"] = static_cast<Json::Int64>(up_to_seq);
  v["row_count"] = static_cast<Json::UInt64>(row_count);
  // ICD-0.5.5 §8 — `buffered_live_count` reports how many live
  // frames the broker buffered during this replay and is about to
  // flush after `replay_done`. Always present (0 when nothing was
  // buffered) so SDK code has a uniform shape.
  v["buffered_live_count"] = static_cast<Json::UInt64>(buffered_live_count);
  return serialize(v);
}

auto build_resync_frame(ResyncReason reason, std::size_t retention_seconds)
    -> std::string {
  Json::Value v;
  v["type"] = ws::msg::RESYNC;
  v["reason"] = std::string{resync_reason_str(reason)};
  v["retention_seconds"] = static_cast<Json::UInt64>(retention_seconds);
  return serialize(v);
}

// ── Replay engine ───────────────────────────────────────────────────

// — (1) sequential pagination loop with linear precondition / per-row / abort
// branches; ICD-0.5.5 §8 added two of those branches without otherwise changing
// the shape, so splitting now would scatter the chunk-loop invariant for a
// 4-point complexity reduction. (2) `const ConnState&` rather than by-value —
// 0.5.5.1 added `unique_ptr<mutex> channels_mu` for the publish_dispatched race
// fix which makes ConnState non-copyable. The reference is lifetime-safe
// because every call site (production: the lambda capture in
// `subscriptions.cpp::fire_replay`; tests: the local `state` outliving
// `sync_wait`) keeps the source alive for the duration of the coroutine.
auto run_replay(const ws::ConnState& state, SendFn send, std::int64_t since_seq,
                std::vector<std::string> granted_channels,
                Config::Realtime::Events cfg,
                std::shared_ptr<std::atomic<bool>> abort_flag,
                std::shared_ptr<std::atomic<std::size_t>> buffered_live_count)
    -> drogon::Task<ReplayResult> {
  ReplayResult result;
  result.up_to_seq = since_seq;
  auto aborted = [&]() {
    return abort_flag != nullptr && abort_flag->load(std::memory_order_acquire);
  };
  auto blc_at_done = [&]() -> std::size_t {
    return buffered_live_count != nullptr
               ? buffered_live_count->load(std::memory_order_acquire)
               : 0;
  };

  if (granted_channels.empty()) {
    send(build_replay_done_frame(since_seq, 0));
    co_return result;
  }

  auto db = db_client();
  if (!db) {
    send(build_replay_done_frame(since_seq, 0));
    co_return result;
  }

  // ── Precondition checks ─────────────────────────────────────────
  try {
    auto min_r = co_await db->execSqlCoro(
        "SELECT COALESCE(MIN(seq), 0) AS min_seq FROM plinth.events");
    // Cursor-expired: client `since_seq` falls before retention.
    // Skip the check when since_seq == 0 — that signals "no prior
    // cursor — from the start of available history" rather than
    // an aged-out cursor (per ICD §New-user behaviour).
    const auto MIN_SEQ = min_r[0]["min_seq"].as<std::int64_t>();
    if (since_seq > 0 && MIN_SEQ > 0 && since_seq < MIN_SEQ) {
      result.resync = ResyncReason::CURSOR_EXPIRED;
      emit_resync_with_audit(send, *result.resync, state, since_seq, cfg);
      co_return result;
    }
    auto max_r = co_await db->execSqlCoro(
        "SELECT COALESCE(MAX(seq), 0) AS max_seq FROM plinth.events");
    // Mismatch: client claim is MORE than `replay_max_total_rows`
    // beyond the largest persisted row.
    const auto MAX_SEQ = max_r[0]["max_seq"].as<std::int64_t>();
    if (since_seq >
        MAX_SEQ + static_cast<std::int64_t>(cfg.replay_max_total_rows)) {
      result.resync = ResyncReason::MISMATCH;
      emit_resync_with_audit(send, *result.resync, state, since_seq, cfg);
      co_return result;
    }
  } catch (const drogon::orm::DrogonDbException& e) {
    spdlog::warn("realtime replay: precondition query failed: {}",
                 e.base().what());
    send(build_replay_done_frame(since_seq, 0));
    co_return result;
  }

  // ── replay_started audit ────────────────────────────────────────
  {
    Json::Value started(Json::objectValue);
    started["user_id"] = state.auth.user_id;
    started["since_seq"] = static_cast<Json::Int64>(since_seq);
    started["channels_count"] =
        static_cast<Json::UInt64>(granted_channels.size());
    audit_event("realtime.events.replay_started", state.auth.user_id, started,
                cfg.audit_window_ms);
  }

  // ── Pagination loop ─────────────────────────────────────────────
  const auto CHANNEL_ARRAY = build_text_array_literal(granted_channels);
  const auto STARTED_AT = std::chrono::steady_clock::now();
  std::int64_t cursor = since_seq;
  while (true) {
    std::optional<drogon::orm::Result> rows_opt;
    try {
      rows_opt = co_await db->execSqlCoro(
          "SELECT seq, channel, payload "
          "FROM plinth.events "
          "WHERE seq > $1 AND channel = ANY($2::text[]) "
          "ORDER BY seq ASC LIMIT $3",
          cursor, CHANNEL_ARRAY,
          static_cast<std::int64_t>(cfg.replay_max_rows_per_chunk));
    } catch (const drogon::orm::DrogonDbException& e) {
      spdlog::warn("realtime replay: chunk query failed: {}", e.base().what());
      break;
    }
    const auto& rows = *rows_opt;
    if (rows.empty()) {
      break;
    }
    // ICD-0.5.5 §8 — abort flag is checked at every chunk
    // boundary. Set by the broker overflow site when the
    // per-(connection, subscription) live buffer fills up; the
    // overflow site emits the resync frame inline so the client
    // sees `live_buffer_overflow` without waiting for the next
    // chunk to land. We just stop emitting replay frames here.
    if (aborted()) {
      result.aborted = true;
      result.resync = ResyncReason::LIVE_BUFFER_OVERFLOW;
      co_return result;
    }
    for (const auto& row : rows) {
      cursor = row["seq"].as<std::int64_t>();
      const auto CHANNEL = row["channel"].as<std::string>();
      if (!rbac_allows(state, CHANNEL)) {
        Json::Value denied(Json::objectValue);
        denied["channel"] = CHANNEL;
        denied["reason"] = "replay";
        audit_event("realtime.broker.subscribe_rule_denied", state.auth.user_id,
                    denied, cfg.audit_window_ms);
        continue;
      }
      auto envelope_opt = parse_payload(row["payload"].as<std::string>());
      if (!envelope_opt.has_value()) {
        spdlog::warn("realtime replay: skipped unparseable row seq={}", cursor);
        continue;
      }
      // ICD-0.5.5 §4 — the canonical envelope-seq lives in the
      // `plinth.events.seq` BIGSERIAL column; the JSONB
      // `payload` was serialised by the writer pre-INSERT and
      // intentionally lacks the seq slot. Stamp it onto the
      // envelope here so replay frames and live frames carry
      // the same field shape (every persisted envelope leaving
      // the kernel has REQUIRED `seq` post-0.5.5).
      (*envelope_opt)["seq"] = static_cast<Json::Int64>(cursor);
      send(build_replay_frame(*envelope_opt));
      result.emitted += 1;
      result.up_to_seq = cursor;
      if (result.emitted > cfg.replay_max_total_rows) {
        result.truncated = true;
        result.resync = ResyncReason::ROW_CAP;
        Json::Value det(Json::objectValue);
        det["user_id"] = state.auth.user_id;
        det["since_seq"] = static_cast<Json::Int64>(since_seq);
        det["up_to_seq"] = static_cast<Json::Int64>(result.up_to_seq);
        det["row_cap"] = static_cast<Json::UInt64>(cfg.replay_max_total_rows);
        audit_event("realtime.events.replay_truncated", state.auth.user_id, det,
                    cfg.audit_window_ms);
        emit_resync_with_audit(send, ResyncReason::ROW_CAP, state, since_seq,
                               cfg);
        co_return result;
      }
    }
    if (rows.size() < cfg.replay_max_rows_per_chunk) {
      break;
    }
  }

  send(
      build_replay_done_frame(result.up_to_seq, result.emitted, blc_at_done()));
  {
    const auto WALL = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - STARTED_AT)
                          .count();
    Json::Value done(Json::objectValue);
    done["user_id"] = state.auth.user_id;
    done["since_seq"] = static_cast<Json::Int64>(since_seq);
    done["up_to_seq"] = static_cast<Json::Int64>(result.up_to_seq);
    done["row_count"] = static_cast<Json::UInt64>(result.emitted);
    done["wall_clock_ms"] = static_cast<Json::Int64>(WALL);
    audit_event("realtime.events.replay_completed", state.auth.user_id, done,
                cfg.audit_window_ms);
  }
  co_return result;
}

// ── Test seams ──────────────────────────────────────────────────────

auto set_db_client_for_test(drogon::orm::DbClientPtr db) -> void {
  std::lock_guard lock(g_test_mu);
  g_test_db_client = std::move(db);
}

auto reset_audit_state_for_test() -> void {
  std::lock_guard lock(g_audit_mu);
  g_audit_windows.clear();
}

} // namespace plinth::realtime::replay
