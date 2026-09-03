// SPDX-License-Identifier: MIT
//
// ICD-0.5.2 §Test Cases — S.* subscribe RBAC.
//
// Per-channel RBAC gate on `on_subscribe` (subscriptions.cpp). Covers
// the 4-layer check order (admin bypass → rbac_enforce=false fallback
// → syntactic validate → rule-derivation lookup) plus the broker's
// per-delivery re-check, quota enforcement, and extension drain.
//
// Tag `[ws][realtime][rbac][integration]` routes to `plinth_tests_ws`.
// Fixture: `ws_test_fixture` drogon + `WsTestClient` + TestPg libpq
// seeding. Each case resets the schema and rebuilds its own user +
// group + rules state before connecting.

#include "../ws/ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"
#include "kernel/realtime/broker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using plinth::ws_test::WsTestClient;

namespace {

// ── Seed helpers (libpq) ───────────────────────────────────────────
//
// ws_test_fixture exposes user/session/PAT/make_admin. S.* cases also
// need group + rule + grant + member plumbing to assemble non-admin
// effective_rules sets. Local helpers mirror the shape used by
// tests/kernel/groups/rbac_integration_test.cpp.

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
  (void)pg.exec_params("INSERT INTO plinth.group_rules (group_id, rule_id) "
                       "VALUES ($1, $2)",
                       {group_id, rule_id});
}

auto add_to_group(plinth::ws_test::TestPg& pg, const std::string& user_id,
                  const std::string& group_id) -> void {
  (void)pg.exec_params("INSERT INTO plinth.group_members (user_id, group_id) "
                       "VALUES ($1, $2)",
                       {user_id, group_id});
}

// Create a non-admin user, create a group, attach rules, add user.
// Returns the session token the caller uses to authenticate a
// WsTestClient.
struct SeededUser {
  std::string user_id;
  std::string token;
  std::string group_id;
};

auto seed_user_with_rules(
    plinth::ws_test::TestPg& pg, const std::string& username,
    const std::string& group_name,
    const std::vector<std::pair<std::string, std::string>>& rules)
    -> SeededUser {
  auto user_id = plinth::ws_test::insert_user(pg, username, "pw-123");
  auto group_id = insert_group(pg, group_name);
  add_to_group(pg, user_id, group_id);
  for (const auto& [rule, ns] : rules) {
    auto rid = insert_rule(pg, rule, ns);
    grant_rule(pg, group_id, rid);
  }
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);
  return SeededUser{.user_id = user_id, .token = token, .group_id = group_id};
}

// ── WS helpers ─────────────────────────────────────────────────────

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

auto authenticate(WsTestClient& client, const std::string& token) -> void {
  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(token));
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
}

auto granted_channels(WsTestClient& client) -> std::vector<std::string> {
  auto ack = client.receive_json(3s);
  REQUIRE(ack.has_value());
  REQUIRE((*ack)["type"].asString() == "subscribed");
  std::vector<std::string> out;
  for (const auto& v : (*ack)["channels"]) {
    out.push_back(v.asString());
  }
  return out;
}

// Guard — drop broker state + metrics + audit windows between cases
// so a stale subscription count from an earlier TEST_CASE can't
// pollute the quota check in S.10.
struct BrokerGuard {
  BrokerGuard() {
    plinth::realtime::broker::stop();
    plinth::realtime::broker::reset_metrics_for_test();
    plinth::realtime::broker::reset_audit_windows_for_test();
    plinth::realtime::broker::set_rbac_enforce_for_test(true);
  }
  ~BrokerGuard() {
    plinth::realtime::broker::stop();
    plinth::realtime::broker::set_rbac_enforce_for_test(true);
  }
  BrokerGuard(const BrokerGuard&) = delete;
  auto operator=(const BrokerGuard&) -> BrokerGuard& = delete;
  BrokerGuard(BrokerGuard&&) = delete;
  auto operator=(BrokerGuard&&) -> BrokerGuard& = delete;
};

} // namespace

TEST_CASE("S.01 admin subscribes across Layer 1 + 2 + 3",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "s01-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, token);
  client.send_json(
      sub_msg({"plinth:data:ext_x.t", "plinth:system:packages.installed",
               "plinth:ext:x:custom"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 3);
}

TEST_CASE("S.02 non-admin Layer 1 grant", "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded = seed_user_with_rules(pg, "s02-user", "s02-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:data:ext_notes.notes"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 1);
  REQUIRE(granted[0] == "plinth:data:ext_notes.notes");
}

TEST_CASE("S.03 non-admin Layer 1 deny silent-omits + audits",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded = seed_user_with_rules(pg, "s03-user", "s03-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:data:ext_terminal.sessions"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.empty());
  REQUIRE(plinth::realtime::broker::rbac_denial_count() >= 1);
}

TEST_CASE("S.04 non-admin Layer 3 grant", "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded =
      seed_user_with_rules(pg, "s04-user", "s04-group",
                           {{"notes.realtime.subscribe.chat_typing", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:ext:notes:chat_typing"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 1);
  REQUIRE(granted[0] == "plinth:ext:notes:chat_typing");
}

TEST_CASE("S.05 non-admin Layer 3 deny silent-omits + audits",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded =
      seed_user_with_rules(pg, "s05-user", "s05-group",
                           {{"notes.realtime.subscribe.chat_typing", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:ext:terminal:typing"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.empty());
  REQUIRE(plinth::realtime::broker::rbac_denial_count() >= 1);
}

TEST_CASE("S.06 non-admin Layer 2 denied with rbac_denied reason",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  // User in some group with unrelated rule so auth completes.
  auto seeded = seed_user_with_rules(pg, "s06-user", "s06-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:system:packages.installed"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.empty());
  REQUIRE(plinth::realtime::broker::rbac_denial_count() >= 1);
}

TEST_CASE("S.07 mixed-batch partial grant",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded = seed_user_with_rules(pg, "s07-user", "s07-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  // Batch: (L1 granted, L1 cross-ext denied, L3 under-specific denied).
  client.send_json(sub_msg({
      "plinth:data:ext_notes.notes",
      "plinth:data:ext_terminal.foo",
      "plinth:ext:notes:chat_typing",
  }));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 1);
  REQUIRE(granted[0] == "plinth:data:ext_notes.notes");
  REQUIRE(plinth::realtime::broker::rbac_denial_count() >= 2);
}

TEST_CASE("S.08 cross-extension Layer-1 attempt denied",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  // `notes` user holds notes.realtime.subscribe but not
  // terminal.realtime.subscribe.
  auto seeded = seed_user_with_rules(pg, "s08-user", "s08-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:data:ext_terminal.foo"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.empty());
  REQUIRE(plinth::realtime::broker::rbac_denial_count() >= 1);
}

TEST_CASE("S.09 rbac_enforce=false denies non-admin (0.1.6 fallback)",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded = seed_user_with_rules(pg, "s09-user", "s09-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  bcfg.rbac_enforce = false; // honoured by broker::start + set_for_test
  plinth::realtime::broker::start(bcfg);
  plinth::realtime::broker::set_rbac_enforce_for_test(false);

  WsTestClient client;
  authenticate(client, seeded.token);
  // User holds the rule, but rbac_enforce=false falls back to
  // admin-only — the non-admin is denied.
  client.send_json(sub_msg({"plinth:data:ext_notes.notes"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.empty());
}

TEST_CASE("S.10 subscription quota overflow",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "s10-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::Config::Realtime::Broker bcfg{};
  bcfg.max_subscriptions_per_conn = 3;
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, token);
  // Admin tries to subscribe 4 channels; quota cuts the fourth.
  client.send_json(sub_msg({
      "plinth:ext:x:a",
      "plinth:ext:x:b",
      "plinth:ext:x:c",
      "plinth:ext:x:d",
  }));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 3);
  REQUIRE(plinth::realtime::broker::rbac_denial_count() >= 1);
}

TEST_CASE("S.11 RBAC re-check on delivery (rbac_enforce flip)",
          "[ws][realtime][rbac][integration]") {
  // ICD S.11 originally targeted mutating `state.effective_rules`
  // between subscribe and dispatch. That state is per-conn in-memory
  // and not externally reachable from the test without a new seam.
  // The rbac_enforce flip exercises the same delivery-time re-check
  // code path (publish.cpp:delivery_rbac_allows): a non-admin with
  // the rule held subscribes cleanly, then the flip demotes every
  // subsequent delivery through the 0.1.6 admin-only fallback arm.
  // Documented deviation from the literal ICD scenario.
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded = seed_user_with_rules(pg, "s11-user", "s11-group",
                                     {{"notes.realtime.subscribe", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:data:ext_notes.notes"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 1);

  // Flip the delivery-time gate to admin-only. Next dispatch denies.
  plinth::realtime::broker::set_rbac_enforce_for_test(false);

  Json::Value envelope(Json::objectValue);
  envelope["layer"] = "data";
  envelope["channel"] = "plinth:data:ext_notes.notes";
  plinth::realtime::DispatchedEvent ev;
  ev.layer = "data";
  ev.channel = "plinth:data:ext_notes.notes";
  ev.envelope = envelope;
  (void)plinth::realtime::broker::dispatch_for_test(ev);

  auto frame = client.receive_json(300ms);
  REQUIRE_FALSE(frame.has_value());
}

TEST_CASE("S.12 drain-on-disable evicts WS subscriptions",
          "[ws][realtime][rbac][integration]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  BrokerGuard g;
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto seeded =
      seed_user_with_rules(pg, "s12-user", "s12-group",
                           {{"notes.realtime.subscribe.chat_typing", "notes"}});

  plinth::Config::Realtime::Broker bcfg{};
  plinth::realtime::broker::start(bcfg);

  WsTestClient client;
  authenticate(client, seeded.token);
  client.send_json(sub_msg({"plinth:ext:notes:chat_typing"}));
  auto granted = granted_channels(client);
  REQUIRE(granted.size() == 1);

  // Drain evicts per-ConnState.channels and the per-bc JS registry.
  plinth::realtime::broker::drain_extension("notes", "disabled");

  // Dispatch matching envelope — nothing reaches the client.
  Json::Value envelope(Json::objectValue);
  envelope["layer"] = "extension";
  envelope["channel"] = "plinth:ext:notes:chat_typing";
  plinth::realtime::DispatchedEvent ev;
  ev.layer = "extension";
  ev.channel = "plinth:ext:notes:chat_typing";
  ev.envelope = envelope;
  (void)plinth::realtime::broker::dispatch_for_test(ev);

  auto frame = client.receive_json(300ms);
  REQUIRE_FALSE(frame.has_value());
}
