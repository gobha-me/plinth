#include "kernel/realtime/emit.hpp"
#include "kernel/db/operations.hpp"

#include "kernel/realtime/channel.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <json/writer.h>
#include <libpq-fe.h>
#include <spdlog/spdlog.h>
#include <string>

namespace plinth::realtime {

namespace {

// Atomic so tests can flip it without serialized access. Default is
// the ICD hard ceiling (§Config Surface); config-load lowers it per
// realtime.notify.max_payload_bytes. Guaranteed in (0, 8000] by
// config validation (see config.cpp apply_realtime).
// configuration state
std::atomic<std::size_t> g_max_payload_bytes{8000};

auto json_write_compact(const Json::Value& v) -> std::string {
  // Matches registration.cpp:61-65 json_write — compact, no indent.
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

} // namespace

// ICD §Validation pipeline steps 1-5 (pre-PG). Returns the compact
// serialized envelope on success; validation error on failure.
// Public — the sync and async emit helpers reuse it, and tests drive
// every rejection path directly without a PGconn.
auto validate_envelope(const Json::Value& envelope)
    -> std::expected<std::string, NotifyError> {
  // Step 1: envelope["layer"] required.
  if (!envelope.isObject() || !envelope.isMember("layer") ||
      !envelope["layer"].isString()) {
    return std::unexpected(NotifyError::MISSING_LAYER);
  }
  auto layer = envelope["layer"].asString();

  // Step 2: envelope["channel"] required + regex check.
  if (!envelope.isMember("channel") || !envelope["channel"].isString()) {
    return std::unexpected(NotifyError::INVALID_CHANNEL);
  }
  auto channel = envelope["channel"].asString();
  if (!validate_channel(channel)) {
    return std::unexpected(NotifyError::INVALID_CHANNEL);
  }

  // Step 3: layer↔channel consistency.
  auto ch_layer = channel_layer(channel);
  bool consistent =
      (ch_layer == ChannelLayer::DATA && layer == "data") ||
      (ch_layer == ChannelLayer::SYSTEM && layer == "system") ||
      (ch_layer == ChannelLayer::EXTENSION && layer == "extension");
  if (!consistent) {
    return std::unexpected(NotifyError::LAYER_MISMATCH);
  }

  // Step 4: serialize compact.
  auto serialized = json_write_compact(envelope);

  // Step 5: size check.
  if (serialized.size() > g_max_payload_bytes.load()) {
    return std::unexpected(NotifyError::PAYLOAD_TOO_LARGE);
  }
  return serialized;
}

auto set_max_payload_bytes(std::size_t bytes) -> void {
  g_max_payload_bytes.store(bytes);
}

auto get_max_payload_bytes() -> std::size_t {
  return g_max_payload_bytes.load();
}

auto emit_notify(PGconn& conn, const Json::Value& envelope)
    -> std::expected<void, NotifyError> {
  auto serialized = validate_envelope(envelope);
  if (!serialized.has_value()) {
    return std::unexpected(serialized.error());
  }

  // Steps 6-7: persist the authoritative envelope through the protected
  // kernel function. PostgreSQL NOTIFY carries only the resulting row id as an
  // untrusted wake hint; listeners never accept envelope data from NOTIFY.
  std::array<const char*, 1> values = {serialized->c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> res{
      plinth::db::exec_params(&conn,
                              "SELECT plinth.enqueue_realtime_event($1::jsonb)",
                              1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear};
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::error("realtime emit: outbox enqueue failed: {}",
                  PQresultErrorMessage(res.get()));
    return std::unexpected(NotifyError::PG_FAILURE);
  }
  return {};
}

auto emit_notify_async(drogon::orm::DbClientPtr db, Json::Value envelope)
    -> drogon::Task<std::expected<void, NotifyError>> {
  auto serialized = validate_envelope(envelope);
  if (!serialized.has_value()) {
    co_return std::unexpected(serialized.error());
  }
  try {
    auto r = co_await db->execSqlCoro(
        "SELECT plinth.enqueue_realtime_event($1::jsonb)", *serialized);
    (void)r;
  } catch (const drogon::orm::DrogonDbException& e) {
    spdlog::error("realtime emit: outbox enqueue async failed: {}",
                  e.base().what());
    co_return std::unexpected(NotifyError::PG_FAILURE);
  }
  co_return {};
}

} // namespace plinth::realtime
