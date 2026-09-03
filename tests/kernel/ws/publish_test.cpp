#include "ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"
#include "kernel/ws/publish.hpp"

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

auto authenticate_admin(WsTestClient& client, plinth::ws_test::TestPg& pg,
                        const std::string& username) -> void {
  auto user_id = plinth::ws_test::insert_user(pg, username, "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(raw_token));
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
}

// Drain up to `tries` frames looking for one of type `type_str`.
// Returns the first match or nullopt.
auto drain_for(WsTestClient& client, const std::string& type_str,
               std::chrono::milliseconds per_frame = 1500ms, int tries = 10)
    -> std::optional<Json::Value> {
  for (int i = 0; i < tries; ++i) {
    auto msg = client.receive_json(per_frame);
    if (!msg.has_value()) {
      return std::nullopt;
    }
    if ((*msg)["type"].asString() == type_str) {
      return msg;
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("WS publish reaches only subscribed connections",
          "[ws][integration][publish]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  WsTestClient c1;
  authenticate_admin(c1, pg, "pub-a");
  c1.send_json(sub_msg({"kernel:test"}));
  REQUIRE(drain_for(c1, "subscribed").has_value());

  WsTestClient c2;
  authenticate_admin(c2, pg, "pub-b");
  c2.send_json(sub_msg({"kernel:other"}));
  REQUIRE(drain_for(c2, "subscribed").has_value());

  Json::Value payload;
  payload["hello"] = "world";
  plinth::ws::publish("kernel:test", payload);

  auto evt = drain_for(c1, "event", 2s);
  REQUIRE(evt.has_value());
  REQUIRE((*evt)["channel"].asString() == "kernel:test");
  REQUIRE((*evt)["payload"]["hello"].asString() == "world");

  // c2 must NOT see this event within a short window.
  auto missed = drain_for(c2, "event", 300ms, 2);
  REQUIRE_FALSE(missed.has_value());
}

TEST_CASE("WS publish is safe when no subscribers exist",
          "[ws][integration][publish]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  // No connections at all — publish must be a no-op, not a crash.
  Json::Value payload;
  payload["foo"] = 42;
  plinth::ws::publish("kernel:nobody", payload);
  SUCCEED("publish with zero subscribers did not crash");
}

TEST_CASE("WS unsubscribe stops delivery", "[ws][integration][publish]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  WsTestClient c;
  authenticate_admin(c, pg, "pub-unsub");
  c.send_json(sub_msg({"kernel:x"}));
  REQUIRE(drain_for(c, "subscribed").has_value());

  // Subscribe + publish → receives event.
  Json::Value payload;
  payload["v"] = 1;
  plinth::ws::publish("kernel:x", payload);
  auto evt = drain_for(c, "event", 2s);
  REQUIRE(evt.has_value());
  REQUIRE((*evt)["payload"]["v"].asInt() == 1);

  // Unsubscribe.
  Json::Value unsub;
  unsub["type"] = "unsubscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("kernel:x");
  unsub["channels"] = arr;
  c.send_json(unsub);
  REQUIRE(drain_for(c, "unsubscribed").has_value());

  // Publish again → no event frame delivered.
  payload["v"] = 2;
  plinth::ws::publish("kernel:x", payload);
  auto missed = drain_for(c, "event", 300ms, 2);
  REQUIRE_FALSE(missed.has_value());
}
