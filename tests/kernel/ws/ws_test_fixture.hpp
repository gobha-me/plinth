#pragma once

// WebSocket integration-test harness.
//
// A single Drogon instance is started lazily on first use in a background
// thread and reused across all WS tests (Drogon's app() is a singleton and
// does not support restart). Tests reset the plinth schema between runs
// via the destructive dev_mode bootstrap path.
//
// Tests that require PG use pg_available() with SKIP() (same pattern as
// auth_integration_test.cpp).

#include "kernel/config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <drogon/WebSocketClient.h>
#include <filesystem>
#include <functional>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace plinth::ws_test {

auto pg_available() -> bool;
auto test_config() -> plinth::Config;
auto reset_schema(const plinth::Config::Database& db) -> void;

// Synchronous seeding helpers (libpq directly).
struct TestPg {
  PGconn* conn{nullptr};

  explicit TestPg(const plinth::Config::Database& db);
  ~TestPg();
  TestPg(const TestPg&) = delete;
  auto operator=(const TestPg&) -> TestPg& = delete;
  TestPg(TestPg&&) = delete;
  auto operator=(TestPg&&) -> TestPg& = delete;

  [[nodiscard]] auto exec(const std::string& sql) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)>;
  [[nodiscard]] auto exec_params(const std::string& sql,
                                 const std::vector<std::string>& params) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)>;
};

auto insert_user(TestPg& pg, const std::string& username,
                 const std::string& password) -> std::string;
auto insert_session(TestPg& pg, const std::string& user_id,
                    const std::string& raw_token) -> std::string;
auto insert_pat(TestPg& pg, const std::string& user_id,
                const std::string& raw_random) -> std::string;

// Add user to the admin group (creates the group if missing). Returns
// the user_id for chaining.
auto make_admin(TestPg& pg, const std::string& user_id) -> void;

// Port the shared Drogon test server listens on. Throws if startup fails.
// Starts the server lazily on first call.
auto test_server_port() -> uint16_t;

// Per-process package data + staging directories. Created on first call;
// removed at process exit. Stable across calls so the routes registered
// at server startup keep matching what tests touch on disk. Used by
// `tests/kernel/packages/http_test_fixture.{hpp,cpp}` (0.6.0.N HTTP fixture).
auto packages_data_dir() -> const std::filesystem::path&;
auto packages_staging_dir() -> const std::filesystem::path&;

// ICD-0.5.5 §8 — thin wrappers around
// `plinth::ws::test_seam::{set,clear}_live_buffer_cap_override`. Tests
// that drive L.05 (`live_buffer_overflow forces resync`) use these so
// `WsTestClient` setup + cap override + WS connect live in one
// namespace. Idempotent; safe to clear without a prior set.
auto set_live_buffer_cap_override(std::size_t cap) -> void;
auto clear_live_buffer_cap_override() -> void;

// Synchronous wrapper around drogon::WebSocketClient. One instance per test.
class WsTestClient {
 public:
  WsTestClient();
  ~WsTestClient();
  WsTestClient(const WsTestClient&) = delete;
  auto operator=(const WsTestClient&) -> WsTestClient& = delete;
  WsTestClient(WsTestClient&&) = delete;
  auto operator=(WsTestClient&&) -> WsTestClient& = delete;

  // Connect to ws://127.0.0.1:<test_server_port>/ws/events. Returns true
  // on success, false on timeout or error.
  auto connect(std::chrono::milliseconds timeout) -> bool;

  // Send a JSON frame. No-op if not connected.
  auto send_json(const Json::Value& v) -> void;

  // Block up to `timeout` for the next JSON frame. Returns nullopt on
  // timeout or if the connection closed without another frame.
  auto receive_json(std::chrono::milliseconds timeout)
      -> std::optional<Json::Value>;

  // Block up to `timeout` for the connection to close. Returns true if
  // it closed within the timeout, false otherwise.
  auto wait_for_close(std::chrono::milliseconds timeout) -> bool;

  [[nodiscard]] auto is_closed() const -> bool { return closed; }

  // ICD-0.5.5 §8 — pause / resume the inbox drain. While paused, raw
  // text frames arriving at the message handler accumulate in a side
  // `paused_raw` deque; `receive_json` returns nullopt regardless of
  // arrival count. `resume_drain` re-parses each pending raw frame in
  // arrival order and flushes to `inbox` under `mu`. L.03 / L.04 use
  // pause to wedge the broker into the live-buffer arm of
  // `publish.cpp` while a replay is in-flight.
  auto pause_drain() -> void;
  auto resume_drain() -> void;
  [[nodiscard]] auto is_drain_paused() const -> bool;

  // L.04 — frame inspector callback. Invoked under `mu` after each
  // `inbox.push_back` (both on the live arrival path and on resume-
  // flush). Install BEFORE connect(); installation post-connect is
  // permitted but races with in-flight pushes. Pass {} to clear.
  using FrameInspector = std::function<void(const Json::Value&)>;
  auto set_frame_inspector(FrameInspector cb) -> void;

 private:
  drogon::WebSocketClientPtr client;
  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<Json::Value> inbox;
  std::deque<std::string> paused_raw; // protected by mu
  FrameInspector inspector;           // protected by mu
  std::atomic<bool> drain_paused{false};
  bool connected{false};
  bool closed{false};
};

} // namespace plinth::ws_test
