// SPDX-License-Identifier: MIT
//
// ICD-0.5.0-pg-listen-notify-bridge §NOTIFY Emission Helper.
//
// Generalizes registration.cpp:67-95 send_notify into a typed,
// validated, reusable emit surface. The PG wire channel is always the
// literal string "plinth:realtime"; the logical channel rides inside
// the envelope's `channel` field.

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <libpq-fe.h>

namespace plinth::realtime {

enum class NotifyError : std::uint8_t {
  MISSING_LAYER,     // envelope lacks 'layer' field
  INVALID_CHANNEL,   // channel fails §Channel Naming regex
  LAYER_MISMATCH,    // envelope.layer conflicts with channel's layer prefix
  PAYLOAD_TOO_LARGE, // serialized envelope > max_payload_bytes
  PG_FAILURE,        // pg_notify returned non-OK / DrogonDbException
};

// Validate an envelope per ICD §Validation pipeline steps 1-5 and, on
// success, return the compact-serialized payload ready for pg_notify.
// The sync and async emit helpers compose this — exposing it lets
// unit tests exercise every rejection path without a live PGconn.
auto validate_envelope(const Json::Value& envelope)
    -> std::expected<std::string, NotifyError>;

// Configure the serialized-envelope size ceiling. Default 8000.
// Driven by config.realtime.notify.max_payload_bytes at startup;
// tests may lower it to keep E.04/P.04 payloads readable. Clamped to
// (0, 8000] at config-load time — emit_notify does not re-validate.
auto set_max_payload_bytes(std::size_t bytes) -> void;

// Read the current limit. Primarily for tests' RAII restore guards.
auto get_max_payload_bytes() -> std::size_t;

// Sync emit via a caller-owned PGconn. Validates the envelope, then
// runs `SELECT pg_notify('plinth:realtime', <serialized>)`. Callers
// SHOULD run inside BEGIN/COMMIT so NOTIFY fires only on post-write
// success (PG buffers NOTIFYs until COMMIT).
auto emit_notify(PGconn& conn, const Json::Value& envelope)
    -> std::expected<void, NotifyError>;

// Async emit via Drogon's PG client pool. Same validation, same
// error taxonomy. Used by the 0.5.0 pubsub.publish JS binding and
// the 0.5.1 coalescer.
auto emit_notify_async(drogon::orm::DbClientPtr db, Json::Value envelope)
    -> drogon::Task<std::expected<void, NotifyError>>;

} // namespace plinth::realtime
