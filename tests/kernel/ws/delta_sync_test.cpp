// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan — D.* delta-sync handshake cases (7 of 8;
// D.08 deferred to 0.5.4.1 per the phase plan: requires a "replay
// mid-flight" injection seam not pinned in the ICD).

#include "kernel/auth/crypto.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/replay.hpp"
#include "kernel/ws/conn_state.hpp"
#include "ws_test_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using plinth::ws_test::pg_available;
using plinth::ws_test::WsTestClient;

namespace {

namespace broker = plinth::realtime::broker;
namespace cs = plinth::realtime::cursor_store;
namespace ew = plinth::realtime::events_writer;

// ── RBAC seed helpers (mirror broker_integration_test.cpp) ──────────

auto insert_group(plinth::ws_test::TestPg& pg, const std::string& name)
    -> std::string {
  auto res = pg.exec_params(
      "INSERT INTO plinth.groups (name) VALUES ($1) RETURNING id", {name});
  return PQgetvalue(res.get(), 0, 0);
}

auto insert_rule(plinth::ws_test::TestPg& pg, const std::string& rule,
                 const std::string& ns) -> std::string {
  auto res = pg.exec_params("INSERT INTO plinth.rbac_rules "
                            "(rule, namespace, description, extension_name) "
                            "VALUES ($1, $2, '', $2) RETURNING id",
                            {rule, ns});
  return PQgetvalue(res.get(), 0, 0);
}

auto grant_rule(plinth::ws_test::TestPg& pg, const std::string& group_id,
                const std::string& rule_id) -> void {
  (void)pg.exec_params(
      "INSERT INTO plinth.group_rules (group_id, rule_id) VALUES ($1, $2)",
      {group_id, rule_id});
}

auto add_to_group(plinth::ws_test::TestPg& pg, const std::string& user_id,
                  const std::string& group_id) -> void {
  (void)pg.exec_params(
      "INSERT INTO plinth.group_members (user_id, group_id) VALUES ($1, $2)",
      {user_id, group_id});
}

// Direct libpq seed of plinth.events — bypasses the writer.
auto seed_events(plinth::ws_test::TestPg& pg, std::string_view channel,
                 std::size_t count) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> seqs;
  for (std::size_t i = 0; i < count; ++i) {
    std::string payload = R"({"layer":"data","channel":")" +
                          std::string{channel} + R"(","i":)" +
                          std::to_string(i) + "}";
    auto res = pg.exec_params("INSERT INTO plinth.events (channel, payload) "
                              "VALUES ($1, $2::jsonb) RETURNING seq",
                              {std::string{channel}, payload});
    REQUIRE(res != nullptr);
    REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
    seqs.push_back(std::stoll(PQgetvalue(res.get(), 0, 0)));
  }
  return seqs;
}

auto build_conninfo_local(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

// Drain inbound frames and collect every {"type":"replay"} envelope
// followed by the terminating {"type":"replay_done"}. Returns nullopt
// if the terminator never arrives.
struct ReplayDrain {
  std::vector<Json::Value> replays;
  std::optional<Json::Value> done;
  std::optional<Json::Value> resync;
  std::optional<Json::Value> error;
};

auto drain_replay(WsTestClient& client, std::chrono::milliseconds timeout = 3s)
    -> ReplayDrain {
  ReplayDrain out;
  constexpr int MAX_FRAMES = 200;
  for (int i = 0; i < MAX_FRAMES; ++i) {
    auto f = client.receive_json(timeout);
    if (!f.has_value()) {
      break;
    }
    const auto& t = (*f)["type"].asString();
    if (t == "replay") {
      out.replays.push_back(*f);
    } else if (t == "replay_done") {
      out.done = *f;
      break;
    } else if (t == "resync") {
      out.resync = *f;
      break;
    } else if (t == "error") {
      out.error = *f;
      break;
    }
    // ignore other frame types (subscribed, ping, connected, etc.)
  }
  return out;
}

struct Harness {
  plinth::ws::ConnState unused_state;
  plinth::Config::Realtime::Events cfg;
  drogon::orm::DbClientPtr db;
  explicit Harness(plinth::Config::Realtime::Events ecfg = {}) : cfg(ecfg) {
    broker::stop();
    ew::stop();
    plinth::realtime::clear_handlers_for_test();
    broker::reset_metrics_for_test();
    broker::reset_audit_windows_for_test();
    broker::set_rbac_enforce_for_test(true);
    plinth::Config::Realtime::Broker bcfg;
    bcfg.enabled = true;
    bcfg.rbac_enforce = true;
    broker::start(bcfg);

    // Wire writer + cursor store + replay's DB client to a freshly-
    // created connNum=2 pool so we don't fight the WS server's pool.
    cs::clear_cache_for_test();
    ew::reset_counters_for_test();
    db = drogon::orm::DbClient::newPgClient(
        build_conninfo_local(plinth::ws_test::test_config().db),
        /*connNum=*/2);
    REQUIRE(db);
    cs::set_db_client_for_test(db);
    plinth::realtime::replay::set_db_client_for_test(db);
    ew::set_db_client_for_test(db);
    ew::start(cfg);
  }
  ~Harness() {
    ew::stop();
    ew::set_db_client_for_test(nullptr);
    plinth::realtime::replay::set_db_client_for_test(nullptr);
    cs::set_db_client_for_test(nullptr);
    cs::clear_cache_for_test();
    broker::stop();
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

} // namespace

// ── D.01 ─────────────────────────────────────────────────────────────

TEST_CASE("D.01: fresh subscribe (no since_seq) — 0.5.2 path unchanged",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  Harness h;
  plinth::ws_test::TestPg pg{cfg.db};
  WsTestClient client;

  auto user_id = plinth::ws_test::insert_user(pg, "d01-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value()); // connected

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d01.t");
  sub["channels"] = arr;
  client.send_json(sub);

  auto ack = client.receive_json(3s);
  REQUIRE(ack.has_value());
  CHECK((*ack)["type"].asString() == "subscribed");
  // No replay/replay_done/resync — fresh subscribe doesn't trigger
  // replay. Drain a few frames to skip heartbeat pings; fail if any
  // delta-sync frame arrives.
  for (int i = 0; i < 3; ++i) {
    auto next = client.receive_json(300ms);
    if (!next.has_value()) {
      break;
    }
    const auto& t = (*next)["type"].asString();
    if (t == "ping") {
      continue;
    }
    FAIL("Unexpected post-subscribe frame: type=" + t);
  }
}

// ── D.02 ─────────────────────────────────────────────────────────────

TEST_CASE("D.02: reconnect with valid since_seq — replay frames + replay_done",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  Harness h;
  plinth::ws_test::TestPg pg{cfg.db};

  seed_events(pg, "plinth:data:ext_d02.t", 5);

  WsTestClient client;
  auto user_id = plinth::ws_test::insert_user(pg, "d02-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);
  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d02.t");
  sub["channels"] = arr;
  sub["since_seq"] = 0; // claim everything from start
  client.send_json(sub);

  REQUIRE(client.receive_json(3s).has_value()); // subscribed ack
  auto drain = drain_replay(client);
  CHECK(drain.replays.size() == 5);
  REQUIRE(drain.done.has_value());
  CHECK((*drain.done)["row_count"].asUInt64() == 5);
}

// ── D.03 ─────────────────────────────────────────────────────────────

TEST_CASE("D.03: since_seq older than retention — resync cursor_expired",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  Harness h;
  plinth::ws_test::TestPg pg{cfg.db};

  auto seqs = seed_events(pg, "plinth:data:ext_d03.t", 10);
  // Force MIN(seq) high — delete the first 5 rows.
  REQUIRE(seqs.size() == 10);
  auto del = pg.exec("DELETE FROM plinth.events WHERE seq <= " +
                     std::to_string(seqs[4]));
  REQUIRE(PQresultStatus(del.get()) == PGRES_COMMAND_OK);

  WsTestClient client;
  auto user_id = plinth::ws_test::insert_user(pg, "d03-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);
  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d03.t");
  sub["channels"] = arr;
  sub["since_seq"] = static_cast<Json::Int64>(seqs[0]); // < new MIN(seq)
  client.send_json(sub);

  REQUIRE(client.receive_json(3s).has_value()); // subscribed
  auto drain = drain_replay(client);
  REQUIRE(drain.resync.has_value());
  CHECK((*drain.resync)["reason"].asString() == "cursor_expired");
}

// ── D.04 ─────────────────────────────────────────────────────────────

TEST_CASE("D.04: negative since_seq — error frame, no subscribe registers",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  Harness h;
  plinth::ws_test::TestPg pg{cfg.db};

  WsTestClient client;
  auto user_id = plinth::ws_test::insert_user(pg, "d04-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);
  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d04.t");
  sub["channels"] = arr;
  sub["since_seq"] = -1;
  client.send_json(sub);

  auto err = client.receive_json(2s);
  REQUIRE(err.has_value());
  CHECK((*err)["type"].asString() == "error");
  CHECK((*err)["error"].asString() == "resubscribe.invalid_since_seq");
}

// ── D.05 ─────────────────────────────────────────────────────────────

TEST_CASE("D.05: events.enabled=false + since_seq — resync events_disabled",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::Config::Realtime::Events ecfg;
  ecfg.enabled = false;
  Harness h{ecfg};
  plinth::ws_test::TestPg pg{cfg.db};

  WsTestClient client;
  auto user_id = plinth::ws_test::insert_user(pg, "d05-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);
  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d05.t");
  sub["channels"] = arr;
  sub["since_seq"] = 0;
  client.send_json(sub);

  REQUIRE(client.receive_json(3s).has_value()); // subscribed
  auto drain = drain_replay(client, 2s);
  REQUIRE(drain.resync.has_value());
  CHECK((*drain.resync)["reason"].asString() == "events_disabled");
}

// ── D.06 ─────────────────────────────────────────────────────────────

TEST_CASE("D.06: multi-channel reconnect — only seeded channels emit replays",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  Harness h;
  plinth::ws_test::TestPg pg{cfg.db};

  seed_events(pg, "plinth:data:ext_d06a.t", 3);
  seed_events(pg, "plinth:data:ext_d06b.t", 4);

  WsTestClient client;
  auto user_id = plinth::ws_test::insert_user(pg, "d06-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);
  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d06a.t");
  arr.append("plinth:data:ext_d06b.t");
  arr.append("plinth:data:ext_d06c.t"); // unseeded
  sub["channels"] = arr;
  sub["since_seq"] = 0;
  client.send_json(sub);

  REQUIRE(client.receive_json(3s).has_value()); // subscribed
  auto drain = drain_replay(client);
  CHECK(drain.replays.size() == 7); // 3 + 4 + 0
  REQUIRE(drain.done.has_value());
  CHECK((*drain.done)["row_count"].asUInt64() == 7);
}

// ── D.07 ─────────────────────────────────────────────────────────────

TEST_CASE("D.07: RBAC denied per channel — replay skips denied channel rows",
          "[realtime][events][delta-sync][ws][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  Harness h;
  plinth::ws_test::TestPg pg{cfg.db};

  seed_events(pg, "plinth:data:ext_d07a.t", 5);
  seed_events(pg, "plinth:data:ext_d07b.t", 5);

  // Non-admin with rule for ext_d07a only.
  auto user_id = plinth::ws_test::insert_user(pg, "d07-rbac", "pw-x");
  auto group_id = insert_group(pg, "d07-grp");
  add_to_group(pg, user_id, group_id);
  auto rid = insert_rule(pg, "d07a.realtime.subscribe", "extension");
  grant_rule(pg, group_id, rid);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  WsTestClient client;
  REQUIRE(client.connect(2s));
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  REQUIRE(client.receive_json(3s).has_value());

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append("plinth:data:ext_d07a.t"); // allowed
  arr.append("plinth:data:ext_d07b.t"); // denied
  sub["channels"] = arr;
  sub["since_seq"] = 0;
  client.send_json(sub);

  auto ack = client.receive_json(3s);
  REQUIRE(ack.has_value());
  CHECK((*ack)["type"].asString() == "subscribed");
  // The "subscribed" ack only lists granted channels (one).
  CHECK((*ack)["channels"].size() == 1);

  auto drain = drain_replay(client);
  CHECK(drain.replays.size() == 5); // only ext_d07a rows
  REQUIRE(drain.done.has_value());
  CHECK((*drain.done)["row_count"].asUInt64() == 5);
}
