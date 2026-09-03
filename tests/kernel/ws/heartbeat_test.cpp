#include "ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>

using namespace std::chrono_literals;
using plinth::ws_test::WsTestClient;

namespace {

auto auth_msg(const std::string& token) -> Json::Value {
  Json::Value v;
  v["type"] = "auth";
  v["token"] = token;
  return v;
}

auto pong_msg(int64_t timestamp) -> Json::Value {
  Json::Value v;
  v["type"] = "pong";
  v["timestamp"] = static_cast<Json::Int64>(timestamp);
  return v;
}

// Run the auth handshake and return the raw session token used.
auto authenticate(WsTestClient& client, plinth::ws_test::TestPg& pg,
                  const std::string& username) -> std::string {
  auto user_id = plinth::ws_test::insert_user(pg, username, "password-123");
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(raw_token));
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
  return raw_token;
}

} // namespace

TEST_CASE("WS pong keeps the connection alive past the interval",
          "[ws][integration][heartbeat]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  WsTestClient client;
  authenticate(client, pg, "heartbeat-ok");

  // Test config: interval=0.5, timeout=0.5.
  // Expect a ping within ~0.6s; reply with matching timestamp.
  auto ping = client.receive_json(2s);
  REQUIRE(ping.has_value());
  REQUIRE((*ping)["type"].asString() == "ping");
  auto ts = (*ping)["timestamp"].asInt64();
  client.send_json(pong_msg(ts));

  // Second ping should arrive because we responded; connection stays open.
  auto ping2 = client.receive_json(2s);
  REQUIRE(ping2.has_value());
  REQUIRE((*ping2)["type"].asString() == "ping");
  client.send_json(pong_msg((*ping2)["timestamp"].asInt64()));

  // Still connected after two round-trips.
  REQUIRE_FALSE(client.is_closed());
}

TEST_CASE("WS connection closes on heartbeat timeout",
          "[ws][integration][heartbeat]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  WsTestClient client;
  authenticate(client, pg, "heartbeat-timeout");

  // Expect a ping; ignore it.
  auto ping = client.receive_json(2s);
  REQUIRE(ping.has_value());
  REQUIRE((*ping)["type"].asString() == "ping");

  // Wait for server to notice — interval=0.5, timeout=0.5, so ~1s max.
  // The server may emit another ping before closing; drain until we see
  // the heartbeat_timeout error frame or timeout.
  bool saw_timeout = false;
  for (int i = 0; i < 5 && !saw_timeout; ++i) {
    auto msg = client.receive_json(1s);
    if (!msg.has_value()) {
      break;
    }
    if ((*msg)["type"].asString() == "error" &&
        (*msg)["error"].asString() == "heartbeat_timeout") {
      saw_timeout = true;
    }
  }
  REQUIRE(saw_timeout);
  REQUIRE(client.wait_for_close(2s));
}
