// SPDX-License-Identifier: MIT
//
// ICD-0.5.4 §Test Plan — I.* end-to-end integration cases.
//
// Phase plan ships I.01 only; I.02 (multi-process advisory-lock harness)
// and I.03 (live+replay race) defer to 0.5.4.1 — both need a fault-
// injection seam not pinned in the ICD.

#include "../kernel/realtime/shared_pg_client.hpp"
#include "../kernel/ws/ws_test_fixture.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/ws/conn_state.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>
#include <memory>
#include <string>

using namespace std::chrono_literals;
using plinth::ws_test::pg_available;
using plinth::ws_test::WsTestClient;

namespace {

namespace broker = plinth::realtime::broker;
namespace cs = plinth::realtime::cursor_store;
namespace ew = plinth::realtime::events_writer;

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

auto count_events_for_channel(plinth::ws_test::TestPg& pg,
                              const std::string& channel) -> int {
  auto res = pg.exec_params(
      "SELECT COUNT(*) FROM plinth.events WHERE channel = $1", {channel});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return -1;
  }
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

auto pg_cursor(plinth::ws_test::TestPg& pg, const std::string& user_id)
    -> std::int64_t {
  auto res = pg.exec_params("SELECT last_seq FROM plinth.user_event_cursors "
                            "WHERE user_id = $1::uuid",
                            {user_id});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return -1;
  }
  return std::stoll(PQgetvalue(res.get(), 0, 0));
}

} // namespace

// ── I.01 ─────────────────────────────────────────────────────────────

TEST_CASE("I.01: full chain — listener -> writer -> cursor -> WS client",
          "[realtime][events][integration][ws]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  // Wire up broker, writer, cursor store with a dedicated DbClient so
  // we can assert on PG rows without the WS server's connection pool.
  broker::stop();
  ew::stop();
  plinth::realtime::clear_handlers_for_test();
  broker::reset_metrics_for_test();
  broker::reset_audit_windows_for_test();
  broker::set_rbac_enforce_for_test(true);
  cs::clear_cache_for_test();
  ew::reset_counters_for_test();

  plinth::Config::Realtime::Broker bcfg;
  bcfg.enabled = true;
  bcfg.rbac_enforce = true;
  broker::start(bcfg);

  // 0.6.3.N — shared process-lifetime client; per-test create+destroy
  // of `newPgClient` reproducibly trips `EventLoopThreadPool::~ +
  // Resource deadlock avoided`.
  auto db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
  REQUIRE(db);
  cs::set_db_client_for_test(db);
  ew::set_db_client_for_test(db);

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1; // every advance writes through
  ew::start(ecfg);

  plinth::ws_test::TestPg pg{cfg.db};

  // Connect a WS client + admin auth so the broker's RBAC re-check
  // passes and `delivered_to_users` gets the user_id.
  WsTestClient client;
  auto user_id = plinth::ws_test::insert_user(pg, "i01-admin", "pw-x");
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
  arr.append("plinth:data:ext_i01.t");
  sub["channels"] = arr;
  client.send_json(sub);
  REQUIRE(client.receive_json(3s).has_value()); // subscribed ack

  // Drive ONE envelope through the listener's dispatch chain. The
  // listener iterates registered handlers in registration order:
  // broker first (publishes the WS frame + fills delivered_to_users),
  // then writer (INSERTs into plinth.events and advances the cursor
  // via cursor_store::record_delivered).
  const std::string PAYLOAD =
      R"({"layer":"data","channel":"plinth:data:ext_i01.t",)"
      R"("schema":"ext_i01","table":"t","ops":[{"op":"insert","count":1}]})";
  REQUIRE(plinth::realtime::apply_notification_for_test("plinth:realtime",
                                                        PAYLOAD));

  // 1. WS client receives the live event frame.
  Json::Value live;
  for (int i = 0; i < 5; ++i) {
    auto f = client.receive_json(2s);
    REQUIRE(f.has_value());
    if ((*f)["type"].asString() == "event") {
      live = *f;
      break;
    }
    // skip pings / other interleaved frames
  }
  CHECK(live["channel"].asString() == "plinth:data:ext_i01.t");

  // 2. Writer persisted ONE row to plinth.events.
  // Allow up to 1 s for the listener-thread → writer-loop dispatch
  // chain to land the INSERT before the test reads.
  int row_count = 0;
  for (int i = 0; i < 20; ++i) {
    ew::apply_drain_for_test();
    row_count = count_events_for_channel(pg, "plinth:data:ext_i01.t");
    if (row_count == 1) {
      break;
    }
    std::this_thread::sleep_for(50ms);
  }
  CHECK(row_count == 1);

  // 3. Cursor advanced for the user.
  auto cursor = pg_cursor(pg, user_id);
  CHECK(cursor > 0);

  // Cleanup.
  ew::stop();
  ew::set_db_client_for_test(nullptr);
  cs::set_db_client_for_test(nullptr);
  cs::clear_cache_for_test();
  plinth::realtime::clear_handlers_for_test();
  broker::stop();
}

// ── I.05 (ICD-0.5.5 §14) ─────────────────────────────────────────────

namespace {

// Wait for a frame of the given type, draining heartbeat pings and
// other interleaved frames. Returns the matching frame on success.
auto receive_of_type(WsTestClient& client, const std::string& type_str,
                     std::chrono::milliseconds timeout = 3s) -> Json::Value {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto f = client.receive_json(500ms);
    if (!f.has_value()) {
      continue;
    }
    if ((*f)["type"].asString() == type_str) {
      return *f;
    }
  }
  return Json::Value{};
}

} // namespace

TEST_CASE("I.05 (0.5.5): subscribe → live → drop → reconnect → replay → live",
          "[realtime][events][integration][ws][seq]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  broker::stop();
  ew::stop();
  plinth::realtime::clear_handlers_for_test();
  broker::reset_metrics_for_test();
  broker::reset_audit_windows_for_test();
  broker::set_rbac_enforce_for_test(true);
  cs::clear_cache_for_test();
  ew::reset_counters_for_test();

  plinth::Config::Realtime::Broker bcfg;
  bcfg.enabled = true;
  bcfg.rbac_enforce = true;
  broker::start(bcfg);

  // 0.6.3.N — shared process-lifetime client; per-test create+destroy
  // of `newPgClient` reproducibly trips `EventLoopThreadPool::~ +
  // Resource deadlock avoided`.
  auto db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
  REQUIRE(db);
  cs::set_db_client_for_test(db);
  ew::set_db_client_for_test(db);

  plinth::Config::Realtime::Events ecfg;
  ecfg.cursor_flush_threshold = 1; // every advance writes through
  ecfg.cursor_cache_ttl_ms = 0;
  ew::start(ecfg);

  plinth::ws_test::TestPg pg{cfg.db};

  auto user_id = plinth::ws_test::insert_user(pg, "i05-admin", "pw-x");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  constexpr auto CHANNEL = "plinth:data:ext_i05.t";
  const std::string PAYLOAD =
      R"({"layer":"data","channel":")" + std::string{CHANNEL} +
      R"(",)"
      R"("schema":"ext_i05","table":"t","ops":[{"op":"insert","count":1}]})";

  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  Json::Value arr(Json::arrayValue);
  arr.append(CHANNEL);

  std::int64_t seq_1 = 0;

  // ── Phase 1: subscribe + receive live envelope #1 ────────────────
  {
    WsTestClient client_a;
    REQUIRE(client_a.connect(2s));
    client_a.send_json(auth);
    REQUIRE(client_a.receive_json(3s).has_value());

    Json::Value sub;
    sub["type"] = "subscribe";
    sub["channels"] = arr;
    client_a.send_json(sub);
    auto ack = receive_of_type(client_a, "subscribed");
    REQUIRE(ack.isObject());
    // ICD-0.5.5 §7 — subscribed ack carries the advisory pair.
    CHECK(ack.isMember("recommended_debounce_ms"));
    CHECK(ack.isMember("recommended_jitter_ms"));

    REQUIRE(plinth::realtime::apply_notification_for_test("plinth:realtime",
                                                          PAYLOAD));

    auto frame_1 = receive_of_type(client_a, "event");
    REQUIRE(frame_1.isObject());
    REQUIRE(frame_1["payload"].isMember("seq"));
    seq_1 = frame_1["payload"]["seq"].asInt64();
    CHECK(seq_1 > 0);
  }
  // ── Phase 2: client_a closed by destructor; emit envelope #2 ──────
  std::this_thread::sleep_for(150ms);
  REQUIRE(plinth::realtime::apply_notification_for_test("plinth:realtime",
                                                        PAYLOAD));
  // Drain the writer so the row lands and the cursor flushes.
  for (int i = 0; i < 20; ++i) {
    ew::apply_drain_for_test();
    if (count_events_for_channel(pg, CHANNEL) == 2) {
      break;
    }
    std::this_thread::sleep_for(50ms);
  }
  CHECK(count_events_for_channel(pg, CHANNEL) == 2);

  // ── Phase 3: reconnect with since_seq=seq_1, expect replay ───────
  WsTestClient client_b;
  REQUIRE(client_b.connect(2s));
  client_b.send_json(auth);
  REQUIRE(client_b.receive_json(3s).has_value());

  Json::Value resub;
  resub["type"] = "subscribe";
  resub["channels"] = arr;
  resub["since_seq"] = static_cast<Json::Int64>(seq_1);
  client_b.send_json(resub);
  REQUIRE(receive_of_type(client_b, "subscribed").isObject());

  auto replay_frame = receive_of_type(client_b, "replay");
  REQUIRE(replay_frame.isObject());
  REQUIRE(replay_frame["envelope"].isMember("seq"));
  auto seq_2 = replay_frame["envelope"]["seq"].asInt64();
  CHECK(seq_2 > seq_1);

  auto done = receive_of_type(client_b, "replay_done");
  REQUIRE(done.isObject());
  // ICD-0.5.5 §8 — replay_done carries buffered_live_count.
  CHECK(done.isMember("buffered_live_count"));
  CHECK(done.isMember("up_to_seq"));
  CHECK(done["up_to_seq"].asInt64() == seq_2);

  // ── Phase 4: emit envelope #3 live after replay_done ─────────────
  REQUIRE(plinth::realtime::apply_notification_for_test("plinth:realtime",
                                                        PAYLOAD));
  auto frame_3 = receive_of_type(client_b, "event");
  REQUIRE(frame_3.isObject());
  REQUIRE(frame_3["payload"].isMember("seq"));
  auto seq_3 = frame_3["payload"]["seq"].asInt64();
  // ICD-0.5.5 §8 ordering invariant — first live > last replay.
  CHECK(seq_3 > seq_2);

  // Cleanup.
  ew::stop();
  ew::set_db_client_for_test(nullptr);
  cs::set_db_client_for_test(nullptr);
  cs::clear_cache_for_test();
  plinth::realtime::clear_handlers_for_test();
  broker::stop();
}
