#include "ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <set>
#include <string>

using namespace std::chrono_literals;
using plinth::ws_test::WsTestClient;

namespace {

auto auth_msg(const std::string& token) -> Json::Value {
  Json::Value v;
  v["type"] = "auth";
  v["token"] = token;
  return v;
}

auto sub_msg(const std::vector<std::string>& channels) -> Json::Value {
  Json::Value v;
  v["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  for (const auto& c : channels) {
    arr.append(c);
  }
  v["channels"] = arr;
  return v;
}

auto unsub_msg(const std::vector<std::string>& channels) -> Json::Value {
  Json::Value v;
  v["type"] = "unsubscribe";
  Json::Value arr(Json::arrayValue);
  for (const auto& c : channels) {
    arr.append(c);
  }
  v["channels"] = arr;
  return v;
}

// Connect + auth as the given user, return once we've seen "connected".
auto authenticate_as(WsTestClient& client, const std::string& raw_token)
    -> void {
  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(raw_token));
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
}

// Drain any non-ack frames (e.g., heartbeat pings) until we see a frame
// with the given type.
auto receive_of_type(WsTestClient& client, const std::string& type_str)
    -> Json::Value {
  for (int i = 0; i < 10; ++i) {
    auto msg = client.receive_json(2s);
    REQUIRE(msg.has_value());
    if ((*msg)["type"].asString() == type_str) {
      return *msg;
    }
  }
  FAIL("timed out waiting for frame type " + type_str);
  return {};
}

auto json_array_to_set(const Json::Value& arr) -> std::set<std::string> {
  std::set<std::string> s;
  for (const auto& v : arr) {
    s.insert(v.asString());
  }
  return s;
}

} // namespace

TEST_CASE("WS admin user subscribes to requested channels",
          "[ws][integration][subscribe]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "admin-sub", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  client.send_json(sub_msg({"kernel:audit", "db:plinth:users"}));
  auto ack = receive_of_type(client, "subscribed");
  REQUIRE(json_array_to_set(ack["channels"]) ==
          std::set<std::string>{"kernel:audit", "db:plinth:users"});
}

TEST_CASE("WS non-admin user receives empty subscribed list",
          "[ws][integration][subscribe]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "plain-user", "pw-123");
  // Deliberately do NOT call make_admin.
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  client.send_json(sub_msg({"kernel:audit"}));
  auto ack = receive_of_type(client, "subscribed");
  REQUIRE(ack["channels"].isArray());
  REQUIRE(ack["channels"].empty());
}

TEST_CASE("WS unsubscribe removes previously subscribed channels",
          "[ws][integration][subscribe]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "admin-unsub", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  client.send_json(sub_msg({"kernel:a", "kernel:b"}));
  auto sub_ack = receive_of_type(client, "subscribed");
  REQUIRE(sub_ack["channels"].size() == 2);

  client.send_json(unsub_msg({"kernel:a"}));
  auto unsub_ack = receive_of_type(client, "unsubscribed");
  REQUIRE(json_array_to_set(unsub_ack["channels"]) ==
          std::set<std::string>{"kernel:a"});
}
