#include "ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/ws/js_stress.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <json/value.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using plinth::capabilities::CapabilityHandler;
using plinth::capabilities::HandlerOutcome;
using plinth::capabilities::UserContext;
using plinth::ws_test::WsTestClient;

namespace {

auto auth_msg(const std::string& token) -> Json::Value {
  Json::Value v;
  v["type"] = "auth";
  v["token"] = token;
  return v;
}

auto call_msg(const std::string& id, const std::string& signature,
              const Json::Value& args) -> Json::Value {
  Json::Value v;
  v["type"] = "call";
  v["id"] = id;
  v["signature"] = signature;
  v["args"] = args;
  return v;
}

auto authenticate_as(WsTestClient& client, const std::string& raw_token)
    -> void {
  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(raw_token));
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
}

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

auto register_echo_handler(const std::string& signature,
                           const std::string& rbac_rule) -> void {
  plinth::capabilities::register_tier1_handler(
      signature, rbac_rule,
      [](const Json::Value& args, const UserContext& ctx,
         int call_depth) -> HandlerOutcome {
        Json::Value out(Json::objectValue);
        out["echo"] = args;
        out["username"] = ctx.username;
        out["call_depth"] = call_depth;
        return out;
      });
}

struct JsStressPoolRestore {
  const plinth::Config& cfg;

  ~JsStressPoolRestore() {
    (void)plinth::ws::shutdown_js_stress_pool(2s);
    plinth::ws::init_js_stress_pool(cfg);
  }
};

} // namespace

TEST_CASE("WS call returns call_result for Tier 1 hit",
          "[ws][integration][call]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "call-admin", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  register_echo_handler("lh0:1:echo", "lh0.echo");

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value args(Json::arrayValue);
  args.append("hello");
  client.send_json(call_msg("req-1", "lh0:1:echo", args));

  auto result = receive_of_type(client, "call_result");
  REQUIRE(result["id"].asString() == "req-1");
  REQUIRE(result["resolved_tier"].asString() == "tier1");
  REQUIRE(result["provider_type"].asString() == "kernel");
  REQUIRE(result["value"]["username"].asString() == "call-admin");
  REQUIRE(result["value"]["echo"].isArray());
  REQUIRE(result["value"]["echo"][0].asString() == "hello");
  REQUIRE(result["value"]["call_depth"].asInt() == 1);
}

TEST_CASE("WS call returns call_error for unknown signature",
          "[ws][integration][call]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "call-unknown", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  client.send_json(
      call_msg("req-2", "ghost:1:missing", Json::Value{Json::arrayValue}));

  auto err = receive_of_type(client, "call_error");
  REQUIRE(err["id"].asString() == "req-2");
  REQUIRE(err["code"].asString() == "capability_not_found");
}

TEST_CASE("WS call rejects missing signature with invalid_call",
          "[ws][integration][call]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "call-malformed", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value frame;
  frame["type"] = "call";
  frame["id"] = "req-3";
  // Deliberately omit "signature".
  client.send_json(frame);

  auto err = receive_of_type(client, "call_error");
  REQUIRE(err["id"].asString() == "req-3");
  REQUIRE(err["code"].asString() == "invalid_call");
}

TEST_CASE("WS call denies non-admin for RBAC-gated capability",
          "[ws][integration][call]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  // Plain (non-admin) user — no make_admin.
  auto user_id = plinth::ws_test::insert_user(pg, "call-plain", "pw-123");
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  register_echo_handler("lh0:1:gated", "lh0.gated");

  WsTestClient client;
  authenticate_as(client, raw_token);

  client.send_json(
      call_msg("req-4", "lh0:1:gated", Json::Value{Json::arrayValue}));

  auto err = receive_of_type(client, "call_error");
  REQUIRE(err["id"].asString() == "req-4");
  REQUIRE(err["code"].asString() == "permission_denied");
}

// ── LH-0.1 — async-bridge stress fork ─────────────────────────────
//
// See docs/icd/ICD-LH-0.1-async-bridge-stress.md §4. These cases
// exercise the `lh0:1:js_stress` dispatch fork in `on_call`, not the
// sync resolver path. The process-lifetime RuntimePool is initialized
// by ws_test_fixture.cpp's `start_test_server`.

TEST_CASE("WS call js_stress evaluates simple script",
          "[ws][integration][call][js_stress]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "js-admin", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value args(Json::arrayValue);
  args.append("(() => 1 + 1)()");
  client.send_json(call_msg("js-1", "lh0:1:js_stress", args));

  auto result = receive_of_type(client, "call_result");
  REQUIRE(result["id"].asString() == "js-1");
  REQUIRE(result["resolved_tier"].asString() == "tier1");
  REQUIRE(result["provider_type"].asString() == "kernel");
  REQUIRE(result["value"].asInt() == 2);
}

TEST_CASE("WS call js_stress reports js_eval_error on throw",
          "[ws][integration][call][js_stress]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "js-throw", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value args(Json::arrayValue);
  args.append("throw new Error('boom')");
  client.send_json(call_msg("js-2", "lh0:1:js_stress", args));

  auto err = receive_of_type(client, "call_error");
  REQUIRE(err["id"].asString() == "js-2");
  REQUIRE(err["code"].asString() == "js_eval_error");
  REQUIRE(err["message"].asString().find("boom") != std::string::npos);
}

TEST_CASE("WS call js_stress denies non-admin",
          "[ws][integration][call][js_stress]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "js-plain", "pw-123");
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value args(Json::arrayValue);
  args.append("1");
  client.send_json(call_msg("js-3", "lh0:1:js_stress", args));

  auto err = receive_of_type(client, "call_error");
  REQUIRE(err["id"].asString() == "js-3");
  REQUIRE(err["code"].asString() == "permission_denied");
}

TEST_CASE("WS call js_stress rejects non-string arg",
          "[ws][integration][call][js_stress]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "js-bad-arg", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value args(Json::arrayValue);
  args.append(42);
  client.send_json(call_msg("js-4", "lh0:1:js_stress", args));

  auto err = receive_of_type(client, "call_error");
  REQUIRE(err["id"].asString() == "js-4");
  REQUIRE(err["code"].asString() == "invalid_call");
}

TEST_CASE("WS call js_stress shutdown drains accepted work",
          "[ws][integration][call][js_stress][lifecycle]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  JsStressPoolRestore restore{cfg};
  plinth::ws_test::reset_schema(cfg.db);
  plinth::capabilities::clear_resolver_for_test();

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "js-shutdown", "pw-123");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  authenticate_as(client, raw_token);

  Json::Value args(Json::arrayValue);
  args.append("(async () => { await db.query('SELECT pg_sleep(0.2)'); return "
              "7; })()");
  client.send_json(call_msg("js-shutdown-1", "lh0:1:js_stress", args));

  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (plinth::ws::js_stress_inflight_count_for_test() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(plinth::ws::js_stress_inflight_count_for_test() == 1);
  REQUIRE_FALSE(plinth::ws::shutdown_js_stress_pool(0ms));

  auto result = receive_of_type(client, "call_result");
  REQUIRE(result["id"].asString() == "js-shutdown-1");
  REQUIRE(result["value"].asInt() == 7);
  REQUIRE(plinth::ws::shutdown_js_stress_pool(1s));
}
