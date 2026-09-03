// SPDX-License-Identifier: MIT
//
// ICD-0.5.0 §Test Cases — E.* emitter tests.
//
// Slice 3 (this file at its initial commit) implements the no-PG
// rejection cases: E.02 (missing table), E.03 (spaces in channel),
// E.04 (oversized), E.05 (layer mismatch). Slice 5 extends with E.01
// (happy-path end-to-end) and E.06 (async overload) under
// [realtime][integration] + PG gate.
//
// Tests exercise `validate_envelope` directly. The sync + async
// emit_notify helpers compose validate_envelope + a PG call; the
// validation behavior is thus fully covered without a live PGconn.

#include "kernel/config.hpp"
#include "kernel/realtime/channel.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/listener.hpp"
#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// RAII guard to scope max_payload_bytes changes so they don't leak
// between TEST_CASEs.
struct ScopedMaxPayload {
  std::size_t prev;
  explicit ScopedMaxPayload(std::size_t bytes)
      : prev(plinth::realtime::get_max_payload_bytes()) {
    plinth::realtime::set_max_payload_bytes(bytes);
  }
  ~ScopedMaxPayload() { plinth::realtime::set_max_payload_bytes(prev); }
  ScopedMaxPayload(const ScopedMaxPayload&) = delete;
  auto operator=(const ScopedMaxPayload&) -> ScopedMaxPayload& = delete;
  ScopedMaxPayload(ScopedMaxPayload&&) = delete;
  auto operator=(ScopedMaxPayload&&) -> ScopedMaxPayload& = delete;
};

auto envelope_of(const std::string& layer, const std::string& channel)
    -> Json::Value {
  Json::Value env(Json::objectValue);
  env["layer"] = layer;
  env["channel"] = channel;
  return env;
}

// ── PG integration helpers (E.01 / E.06) ────────────────────────────

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* v = std::getenv("PLINTH_PG_HOST")) {
    db.host = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<uint16_t>(std::stoi(v));
  }
  if (auto* v = std::getenv("PLINTH_PG_USER")) {
    db.user = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = v;
  }
  return db;
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  auto db = pg_config();
  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password + " connect_timeout=3";
  PGconn* conn = PQconnectdb(conninfo.c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

} // namespace

TEST_CASE("E.02 invalid channel — missing table", "[realtime][unit]") {
  auto env = envelope_of("data", "plinth:data:");
  auto res = plinth::realtime::validate_envelope(env);
  REQUIRE(!res.has_value());
  REQUIRE(res.error() == plinth::realtime::NotifyError::INVALID_CHANNEL);
}

TEST_CASE("E.03 invalid channel — spaces", "[realtime][unit]") {
  auto env = envelope_of("extension", "plinth:ext:notes:has spaces");
  auto res = plinth::realtime::validate_envelope(env);
  REQUIRE(!res.has_value());
  REQUIRE(res.error() == plinth::realtime::NotifyError::INVALID_CHANNEL);
}

TEST_CASE("E.04 oversized envelope", "[realtime][unit]") {
  ScopedMaxPayload guard{200};

  auto env = envelope_of("system", "plinth:system:test.oversized");
  env["payload"] = std::string(300, 'x');

  auto res = plinth::realtime::validate_envelope(env);
  REQUIRE(!res.has_value());
  REQUIRE(res.error() == plinth::realtime::NotifyError::PAYLOAD_TOO_LARGE);
}

TEST_CASE("E.05 layer mismatch", "[realtime][unit]") {
  auto env = envelope_of("data", "plinth:system:foo");
  auto res = plinth::realtime::validate_envelope(env);
  REQUIRE(!res.has_value());
  REQUIRE(res.error() == plinth::realtime::NotifyError::LAYER_MISMATCH);
}

TEST_CASE("validate_envelope rejects missing layer field", "[realtime][unit]") {
  // Defensive coverage for validation pipeline step 1 — MISSING_LAYER
  // precedence over INVALID_CHANNEL.
  Json::Value env(Json::objectValue);
  env["channel"] = "plinth:system:foo";
  auto res = plinth::realtime::validate_envelope(env);
  REQUIRE(!res.has_value());
  REQUIRE(res.error() == plinth::realtime::NotifyError::MISSING_LAYER);
}

TEST_CASE("validate_envelope accepts valid layer-2 envelope",
          "[realtime][unit]") {
  Json::Value env(Json::objectValue);
  env["layer"] = "system";
  env["channel"] = "plinth:system:packages.installed";
  env["payload"]["name"] = "notes";
  env["payload"]["version"] = "1.0.0";

  auto res = plinth::realtime::validate_envelope(env);
  REQUIRE(res.has_value());
  REQUIRE(res->find("plinth:system:packages.installed") != std::string::npos);
}

// ── Channel-validator coverage ──────────────────────────────────────

TEST_CASE("validate_channel accepts all three layers",
          "[realtime][unit][channel]") {
  using plinth::realtime::validate_channel;
  REQUIRE(validate_channel("plinth:data:ext_notes.notes"));
  REQUIRE(validate_channel("plinth:system:packages.installed"));
  REQUIRE(validate_channel("plinth:system:node.join"));
  REQUIRE(validate_channel("plinth:ext:notes:chat_typing"));
  REQUIRE(validate_channel("plinth:ext:notes:events.typing"));
}

TEST_CASE("validate_channel rejects malformed channels",
          "[realtime][unit][channel]") {
  using plinth::realtime::validate_channel;
  REQUIRE_FALSE(validate_channel(""));
  REQUIRE_FALSE(validate_channel("plinth:"));
  REQUIRE_FALSE(validate_channel("plinth:data:"));
  REQUIRE_FALSE(validate_channel("plinth:data:notable"));
  REQUIRE_FALSE(validate_channel("plinth:data:.table"));
  REQUIRE_FALSE(validate_channel("plinth:data:schema."));
  REQUIRE_FALSE(validate_channel("plinth:system:"));
  REQUIRE_FALSE(validate_channel("plinth:ext:"));
  REQUIRE_FALSE(validate_channel("plinth:ext:notes"));
  REQUIRE_FALSE(validate_channel("plinth:ext:notes:"));
  REQUIRE_FALSE(validate_channel("plinth:ext:Notes:foo"));
  REQUIRE_FALSE(validate_channel("plinth:ext:notes_ext:foo"));
  REQUIRE_FALSE(validate_channel("plinth:bogus:foo"));
  REQUIRE_FALSE(validate_channel("plinth:ext:notes:chat typing"));
  REQUIRE_FALSE(validate_channel("notplinth:system:foo"));
  REQUIRE_FALSE(validate_channel("plinth:system:" + std::string(80, 'a')));
}

TEST_CASE("channel_extension extracts the extension segment",
          "[realtime][unit][channel]") {
  using plinth::realtime::channel_extension;
  REQUIRE(channel_extension("plinth:ext:notes:chat_typing") == "notes");
  REQUIRE(channel_extension("plinth:data:ext_notes.notes").empty());
  REQUIRE(channel_extension("plinth:system:foo.bar").empty());
  REQUIRE(channel_extension("").empty());
}

// ── PG-gated integration tests (E.01 / E.06) ────────────────────────

TEST_CASE("E.01 emit_notify sync happy path — listener receives",
          "[realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  plinth::realtime::clear_handlers_for_test();

  std::mutex mu;
  std::condition_variable cv;
  std::vector<plinth::realtime::DispatchedEvent> received;
  plinth::realtime::register_handler(
      [&mu, &cv, &received](const plinth::realtime::DispatchedEvent& ev) {
        std::lock_guard lock(mu);
        received.push_back(ev);
        cv.notify_all();
      });

  auto db = pg_config();
  plinth::Config::Realtime::Listener lcfg;
  lcfg.reconnect_backoff_ms = 200;
  plinth::realtime::start_listener(db, lcfg);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Open a dedicated PGconn for the sync emit_notify caller.
  auto conninfo = build_conninfo(db);
  auto* conn = PQconnectdb(conninfo.c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);

  Json::Value env(Json::objectValue);
  env["layer"] = "system";
  env["channel"] = "plinth:system:test.e01";
  env["payload"]["k"] = "v";

  auto res = plinth::realtime::emit_notify(*conn, env);
  PQfinish(conn);
  REQUIRE(res.has_value());

  {
    std::unique_lock lock(mu);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5),
                        [&received] { return !received.empty(); }));
  }
  plinth::realtime::stop_listener();

  REQUIRE(received.size() == 1);
  REQUIRE(received[0].layer == "system");
  REQUIRE(received[0].channel == "plinth:system:test.e01");
  REQUIRE(received[0].envelope["payload"]["k"].asString() == "v");
}

TEST_CASE("E.06 emit_notify_async via Drogon DbClient — listener receives",
          "[realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  plinth::realtime::clear_handlers_for_test();

  std::mutex mu;
  std::condition_variable cv;
  std::vector<plinth::realtime::DispatchedEvent> received;
  plinth::realtime::register_handler(
      [&mu, &cv, &received](const plinth::realtime::DispatchedEvent& ev) {
        std::lock_guard lock(mu);
        received.push_back(ev);
        cv.notify_all();
      });

  auto db_cfg = pg_config();
  plinth::Config::Realtime::Listener lcfg;
  lcfg.reconnect_backoff_ms = 200;
  plinth::realtime::start_listener(db_cfg, lcfg);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 0.6.3.N — shared process-lifetime client; per-test create+destroy
  // of `newPgClient` reproducibly trips `EventLoopThreadPool::~ +
  // Resource deadlock avoided`.
  auto db = plinth::realtime_test::shared_pg_client(/*connNum=*/1);
  REQUIRE(db);

  Json::Value env(Json::objectValue);
  env["layer"] = "system";
  env["channel"] = "plinth:system:test.e06";
  env["payload"]["async"] = true;

  auto result = drogon::sync_wait(plinth::realtime::emit_notify_async(db, env));
  REQUIRE(result.has_value());

  {
    std::unique_lock lock(mu);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5),
                        [&received] { return !received.empty(); }));
  }
  plinth::realtime::stop_listener();

  REQUIRE(received.size() == 1);
  REQUIRE(received[0].channel == "plinth:system:test.e06");
  REQUIRE(received[0].envelope["payload"]["async"].asBool() == true);
}
