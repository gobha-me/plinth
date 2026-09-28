// SPDX-License-Identifier: MIT
//
// ICD-0.5.5 §11 / §14 row S.06 — broker-side `realtime.seq.gap_detected`
// audit pipeline. Four TEST_CASEs:
//
//   S.06a — happy gap fires audit. Subscribe live (no since_seq → no
//           replay), dispatch seq=1 then seq=3; assert audit row with
//           prev_seq=1, next_seq=3, gap_size=1, count_in_window=1.
//   S.06b — same-window dedup suppresses. Dispatch seq=1, seq=3 (audit
//           fires), seq=5 (gap of 1 again, same window); assert exactly
//           ONE audit row exists.
//   S.06c — first live frame after subscribe doesn't false-fire.
//           Dispatch seq=42 with no prior baseline → no audit; then
//           seq=44 → audit fires with prev_seq=42, next_seq=44.
//   S.06d — missing/rolled-back/duplicate commits cannot fool the observer;
//           filtered and libpq-buffered notifications retain their contracts.
//
// Pure live-path tests (replay unused) — `subscribe` is sent WITHOUT
// `since_seq` so `fire_replay` does not run and `deliver_to_conn`
// always takes the immediate-send arm where gap detection lives.
//
// Reuses the session 6 `LiveReplayWsHarness` + `WsTestClient` pattern
// from `live_replay_ordering_test.cpp` per the
// `project_test_fixture_inflight.md` Session 8 plan.

#include "kernel/realtime/events_writer.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/cursor_store.hpp"
#include "kernel/realtime/replay.hpp"
#include "kernel/ws/conn_state.hpp"
#include "kernel/ws/publish.hpp"

#include "shared_pg_client.hpp"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <libpq-fe.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace ew = plinth::realtime::events_writer;
namespace rp = plinth::realtime::replay;

namespace {

auto build_dispatched(std::string_view layer, std::string_view channel)
    -> plinth::realtime::DispatchedEvent {
  plinth::realtime::DispatchedEvent ev;
  ev.layer = std::string{layer};
  ev.channel = std::string{channel};
  ev.envelope = Json::Value(Json::objectValue);
  ev.envelope["layer"] = std::string{layer};
  ev.envelope["channel"] = std::string{channel};
  ev.envelope["emitted_at"] = "1970-01-01T00:00:00.000Z";
  return ev;
}

// Mirrors LiveReplayWsHarness from live_replay_ordering_test.cpp —
// broker + writer + replay all wired against a connNum=2 pool. Writer
// is started but no events are seeded; tests inject live envelopes
// directly via `broker::dispatch_for_test` (bypasses writer's PG
// INSERT and the BIGSERIAL — seq stamps come from each test's
// `ev.envelope["seq"] = ...`). Cap override is cleared in the
// destructor so cross-test leakage is impossible.
struct GapDetectHarness {
  drogon::orm::DbClientPtr db;
  explicit GapDetectHarness() {
    namespace br = plinth::realtime::broker;
    namespace cs = plinth::realtime::cursor_store;
    br::stop();
    ew::stop();
    plinth::realtime::clear_handlers_for_test();
    br::reset_metrics_for_test();
    br::reset_audit_windows_for_test();
    plinth::ws::reset_gap_audit_windows_for_test();
    br::set_rbac_enforce_for_test(true);
    plinth::Config::Realtime::Broker bcfg;
    bcfg.enabled = true;
    bcfg.rbac_enforce = true;
    br::start(bcfg);

    cs::clear_cache_for_test();
    ew::reset_counters_for_test();
    ew::clear_insert_hook_for_test();
    ew::clear_advisory_lock_hook_for_test();
    ew::clear_pre_broker_hook_for_test();
    // 0.6.3.N — shared process-lifetime client; per-test create+
    // destroy of `newPgClient` reproducibly trips
    // `EventLoopThreadPool::~ + Resource deadlock avoided`. Member-
    // init not viable: setup work above must run first.
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/2);
    REQUIRE(db);
    cs::set_db_client_for_test(db);
    rp::set_db_client_for_test(db);
    ew::set_db_client_for_test(db);
    plinth::Config::Realtime::Events ecfg;
    ecfg.enabled = true;
    ew::start(ecfg);
  }
  ~GapDetectHarness() {
    namespace br = plinth::realtime::broker;
    namespace cs = plinth::realtime::cursor_store;
    plinth::ws_test::clear_live_buffer_cap_override();
    ew::stop();
    ew::set_db_client_for_test(nullptr);
    ew::clear_pre_broker_hook_for_test();
    rp::set_db_client_for_test(nullptr);
    cs::set_db_client_for_test(nullptr);
    cs::clear_cache_for_test();
    plinth::realtime::clear_handlers_for_test();
    br::stop();
  }
  GapDetectHarness(const GapDetectHarness&) = delete;
  auto operator=(const GapDetectHarness&) -> GapDetectHarness& = delete;
  GapDetectHarness(GapDetectHarness&&) = delete;
  auto operator=(GapDetectHarness&&) -> GapDetectHarness& = delete;
};

// Subscribe with no `since_seq` so replay does NOT fire (per
// subscriptions.cpp:277-279 → fire_replay branch at :399 only runs
// when `since_seq` is present). Returns once the `subscribed` ack
// arrives.
auto auth_and_subscribe(plinth::ws_test::WsTestClient& client,
                        const std::string& token, const std::string& channel)
    -> void {
  using namespace std::chrono_literals;
  Json::Value auth;
  auth["type"] = "auth";
  auth["token"] = token;
  client.send_json(auth);
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");

  Json::Value sub;
  sub["type"] = "subscribe";
  Json::Value arr(Json::arrayValue);
  arr.append(channel);
  sub["channels"] = arr;
  client.send_json(sub);
  auto sub_ack = client.receive_json(3s);
  REQUIRE(sub_ack.has_value());
  REQUIRE((*sub_ack)["type"].asString() == "subscribed");
  REQUIRE(plinth::log::is_audit_ready());
}

// Dispatch queues owner-loop delivery; receipt of the frame is not an audit
// commit acknowledgement. The separate fixture observer below supplies that.
auto dispatch_seq(const std::string& channel, std::int64_t seq) -> void {
  auto ev = build_dispatched("data", channel);
  ev.envelope["seq"] = static_cast<Json::Int64>(seq);
  (void)plinth::realtime::broker::dispatch_for_test(ev);
}

// Drain the next `event` frame off the inbox. Returns the parsed seq
// or -1 if no event arrives within the timeout.
auto next_event_seq(plinth::ws_test::WsTestClient& client) -> std::int64_t {
  using namespace std::chrono_literals;
  for (int i = 0; i < 20; ++i) {
    auto f = client.receive_json(2s);
    if (!f.has_value()) {
      return -1;
    }
    if ((*f)["type"].asString() == "event" && (*f).isMember("payload") &&
        (*f)["payload"].isMember("seq")) {
      return (*f)["payload"]["seq"].asInt64();
    }
  }
  return -1;
}

// Count `realtime.seq.gap_detected` audit rows for the given user.
auto gap_audit_count(plinth::ws_test::TestPg& pg, const std::string& user_id)
    -> int {
  auto r = pg.exec_params("SELECT count(*) FROM plinth.audit_log "
                          "WHERE action = 'realtime.seq.gap_detected' "
                          "AND user_id = $1::uuid",
                          {user_id});
  REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(r.get()) == 1);
  return std::stoi(PQgetvalue(r.get(), 0, 0));
}

// Fetch the most recent `realtime.seq.gap_detected` detail JSON for
// the user. Returns an empty string if no row exists.
auto latest_gap_detail(plinth::ws_test::TestPg& pg, const std::string& user_id)
    -> std::string {
  auto r = pg.exec_params("SELECT detail FROM plinth.audit_log "
                          "WHERE action = 'realtime.seq.gap_detected' "
                          "AND user_id = $1::uuid "
                          "ORDER BY timestamp DESC LIMIT 1",
                          {user_id});
  REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  if (PQntuples(r.get()) == 0) {
    return {};
  }
  return PQgetvalue(r.get(), 0, 0);
}

// Test-only observation of the existing fire-and-forget audit path. The
// trigger notifies with the inserted row ID, but PostgreSQL publishes NOTIFY
// only at commit. Neither production logging nor its caller gets a new API or
// persistence promise. The function lives in this connection's temporary
// schema; this owner removes its table trigger before closing that connection.
class GapAuditCommitObserver {
 public:
  GapAuditCommitObserver(const plinth::Config::Database& db,
                         const std::string& user_id)
      : listener(db),
        channel("plinth_gap_commit_" + std::to_string(::getpid()) + "_" +
                std::to_string(next_id.fetch_add(1))) {
    REQUIRE(listener.conn != nullptr);
    REQUIRE(PQstatus(listener.conn) == CONNECTION_OK);
    REQUIRE(user_id.find_first_not_of("0123456789abcdef-") ==
            std::string::npos);
    require_command("SET statement_timeout = 2000");
    require_command("LISTEN " + channel);
    require_command("CREATE FUNCTION pg_temp." + channel +
                    "() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
                    "IF NEW.action = 'realtime.seq.gap_detected' "
                    "AND NEW.user_id = TG_ARGV[1]::uuid THEN "
                    "PERFORM pg_catalog.pg_notify(TG_ARGV[0], NEW.id::text); "
                    "END IF; RETURN NEW; END; $$");
    require_command("CREATE TRIGGER " + channel +
                    " AFTER INSERT ON plinth.audit_log FOR EACH ROW "
                    "EXECUTE FUNCTION pg_temp." +
                    channel + "('" + channel + "', '" + user_id + "')");
  }

  ~GapAuditCommitObserver() {
    auto result = listener.exec("DROP TRIGGER IF EXISTS " + channel +
                                " ON plinth.audit_log");
    if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
      std::fputs("gap audit fixture could not release its owned trigger\n",
                 stderr);
      std::_Exit(EXIT_FAILURE);
    }
  }
  GapAuditCommitObserver(const GapAuditCommitObserver&) = delete;
  auto operator=(const GapAuditCommitObserver&)
      -> GapAuditCommitObserver& = delete;
  GapAuditCommitObserver(GapAuditCommitObserver&&) = delete;
  auto operator=(GapAuditCommitObserver&&) -> GapAuditCommitObserver& = delete;

  [[nodiscard]] auto wait_for_commits(std::size_t count,
                                      std::chrono::milliseconds timeout)
      -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      // PQexec can already have absorbed a notification while reading
      // ReadyForQuery. Drain libpq first, even with a zero budget/idle socket.
      if (PQconsumeInput(listener.conn) == 0) {
        throw std::runtime_error("gap audit notification connection failed");
      }
      drain_notifications();
      if (committed_ids.size() >= count) {
        return true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return false;
      }
      const auto remaining =
          std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
      const auto poll_timeout = static_cast<int>(
          std::min(remaining, static_cast<std::chrono::milliseconds::rep>(
                                  std::numeric_limits<int>::max())));
      pollfd socket{
          .fd = PQsocket(listener.conn), .events = POLLIN, .revents = 0};
      const auto ready = ::poll(&socket, 1, poll_timeout);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready < 0 || (socket.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("gap audit notification socket failed");
      }
    }
  }

  [[nodiscard]] auto committed_count() const -> std::size_t {
    return committed_ids.size();
  }

  // A self-NOTIFY from a real committed INSERT reaches this connection before
  // its ReadyForQuery. This control seam tests the libpq-buffered arm
  // without sleeps or an assumed ordering between different pool connections.
  [[nodiscard]] auto connection_for_control() -> plinth::ws_test::TestPg& {
    return listener;
  }

 private:
  auto require_command(const std::string& sql) -> void {
    auto result = listener.exec(sql);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }

  auto drain_notifications() -> void {
    for (;;) {
      std::unique_ptr<PGnotify, decltype(&PQfreemem)> notification{
          PQnotifies(listener.conn), PQfreemem};
      if (!notification) {
        return;
      }
      if (channel == notification->relname) {
        committed_ids.emplace_back(notification->extra);
      }
    }
  }

  static inline std::atomic<std::uint64_t> next_id{0};
  plinth::ws_test::TestPg listener;
  std::string channel;
  std::vector<std::string> committed_ids;
};

// All three independent observations are required. In particular, a delivery
// or emission counter cannot stand in for a committed row, and duplicate
// persistence cannot disappear behind a first-notification success.
auto exactly_one_gap(std::uint64_t emitted, std::size_t acknowledged,
                     int persisted) -> bool {
  return emitted == 1 && acknowledged == 1 && persisted == 1;
}

class AuditShareLock {
 public:
  explicit AuditShareLock(const plinth::Config::Database& db) : owner(db) {
    REQUIRE(owner.conn != nullptr);
    REQUIRE(PQstatus(owner.conn) == CONNECTION_OK);
    require_command("SET statement_timeout = 2000");
    require_command("BEGIN");
    require_command("LOCK TABLE plinth.audit_log IN SHARE MODE");
  }
  ~AuditShareLock() {
    if (held) {
      auto result = owner.exec("ROLLBACK");
      if (PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        std::fputs("gap audit fixture could not release its SHARE lock\n",
                   stderr);
        std::_Exit(EXIT_FAILURE);
      }
    }
  }
  AuditShareLock(const AuditShareLock&) = delete;
  auto operator=(const AuditShareLock&) -> AuditShareLock& = delete;
  AuditShareLock(AuditShareLock&&) = delete;
  auto operator=(AuditShareLock&&) -> AuditShareLock& = delete;

  auto release() -> void {
    require_command("COMMIT");
    held = false;
  }
  [[nodiscard]] auto backend_pid() const -> int {
    return PQbackendPID(owner.conn);
  }

 private:
  auto require_command(const std::string& sql) -> void {
    auto result = owner.exec(sql);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  plinth::ws_test::TestPg owner;
  bool held{true};
};

auto audit_insert_is_blocked(plinth::ws_test::TestPg& pg, int blocker_pid)
    -> bool {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  do {
    auto result = pg.exec_params(
        "SELECT EXISTS (SELECT 1 FROM pg_stat_activity "
        "WHERE datname = current_database() AND wait_event_type = 'Lock' "
        "AND query LIKE 'INSERT INTO plinth.audit_log%' "
        "AND $1::int = ANY(pg_blocking_pids(pid)))",
        {std::to_string(blocker_pid)});
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    if (std::string_view{PQgetvalue(result.get(), 0, 0)} == "t") {
      return true;
    }
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

} // namespace

// ── S.06a ────────────────────────────────────────────────────────────

TEST_CASE("S.06a: live-path gap fires realtime.seq.gap_detected audit",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws::reset_gap_audit_windows_for_test();
  GapDetectHarness h;
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_s06a.t";

  auto user_id = plinth::ws_test::insert_user(pg, "s06a-admin", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
  auth_and_subscribe(client, token, CHANNEL);
  GapAuditCommitObserver committed{cfg.db, user_id};

  // First live frame establishes the baseline (no gap fires).
  dispatch_seq(CHANNEL, 1);
  REQUIRE(next_event_seq(client) == 1);
  CHECK(plinth::ws::gap_audit_emit_count_for_test() == 0);

  // Reproduce the missing observation edge without a timing assumption:
  // SHARE permits our reads but blocks the audit INSERT's ROW EXCLUSIVE lock.
  // Event delivery and emission still succeed while no row can commit. This
  // control does not claim delayed commit was the original CI failure's cause.
  AuditShareLock delayed{cfg.db};
  dispatch_seq(CHANNEL, 3);
  REQUIRE(next_event_seq(client) == 3);
  REQUIRE(plinth::ws::gap_audit_emit_count_for_test() == 1);
  REQUIRE(audit_insert_is_blocked(pg, delayed.backend_pid()));
  CHECK(gap_audit_count(pg, user_id) == 0);
  CHECK(latest_gap_detail(pg, user_id).empty());
  CHECK_FALSE(committed.wait_for_commits(1, 0ms));
  delayed.release();
  REQUIRE(committed.wait_for_commits(1, 2s));
  CHECK(exactly_one_gap(plinth::ws::gap_audit_emit_count_for_test(),
                        committed.committed_count(),
                        gap_audit_count(pg, user_id)));

  CHECK(gap_audit_count(pg, user_id) == 1);
  auto detail = latest_gap_detail(pg, user_id);
  REQUIRE(!detail.empty());
  CHECK(detail.find("\"prev_seq\": 1") != std::string::npos);
  CHECK(detail.find("\"next_seq\": 3") != std::string::npos);
  CHECK(detail.find("\"gap_size\": 1") != std::string::npos);
  CHECK(detail.find("\"count_in_window\": 1") != std::string::npos);
  CHECK(detail.find("\"channel\": \"plinth:data:ext_s06a.t\"") !=
        std::string::npos);
  CHECK(detail.find("\"window_ms\": 60000") != std::string::npos);
}

// ── S.06b ────────────────────────────────────────────────────────────

TEST_CASE("S.06b: same-window dedup suppresses second gap audit",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws::reset_gap_audit_windows_for_test();
  GapDetectHarness h;
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_s06b.t";

  auto user_id = plinth::ws_test::insert_user(pg, "s06b-admin", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
  auth_and_subscribe(client, token, CHANNEL);
  GapAuditCommitObserver committed{cfg.db, user_id};

  // Establish baseline at seq=1.
  dispatch_seq(CHANNEL, 1);
  REQUIRE(next_event_seq(client) == 1);

  // First gap (1 → 3) fires audit (count_in_window=1).
  dispatch_seq(CHANNEL, 3);
  REQUIRE(next_event_seq(client) == 3);

  // Second gap (3 → 5) within the same 60s window → suppressed.
  dispatch_seq(CHANNEL, 5);
  REQUIRE(next_event_seq(client) == 5);

  REQUIRE(committed.wait_for_commits(1, 2s));
  CHECK(exactly_one_gap(plinth::ws::gap_audit_emit_count_for_test(),
                        committed.committed_count(),
                        gap_audit_count(pg, user_id)));

  CHECK(gap_audit_count(pg, user_id) == 1);
  // The single audit row reflects the FIRST gap (1 → 3); the second
  // gap was suppressed by the sliding-window dedup.
  auto detail = latest_gap_detail(pg, user_id);
  CHECK(detail.find("\"prev_seq\": 1") != std::string::npos);
  CHECK(detail.find("\"next_seq\": 3") != std::string::npos);

  // The emit-count test seam confirms exactly one audit was fired
  // from publish.cpp's deliver_to_conn path (not just one INSERT
  // landed in PG — proves the suppression really happened upstream).
  CHECK(plinth::ws::gap_audit_emit_count_for_test() == 1);
}

// ── S.06c ────────────────────────────────────────────────────────────

TEST_CASE("S.06c: first live frame after subscribe does not false-fire",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws::reset_gap_audit_windows_for_test();
  GapDetectHarness h;
  plinth::ws_test::TestPg pg{cfg.db};

  constexpr auto CHANNEL = "plinth:data:ext_s06c.t";

  auto user_id = plinth::ws_test::insert_user(pg, "s06c-admin", "pw");
  plinth::ws_test::make_admin(pg, user_id);
  auto token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, token);

  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
  auth_and_subscribe(client, token, CHANNEL);
  GapAuditCommitObserver committed{cfg.db, user_id};

  // First live frame on this channel since subscribe — establishes
  // baseline at 42, no gap audit even though seq jumped from "no
  // baseline" to 42.
  dispatch_seq(CHANNEL, 42);
  REQUIRE(next_event_seq(client) == 42);
  CHECK(gap_audit_count(pg, user_id) == 0);
  CHECK(plinth::ws::gap_audit_emit_count_for_test() == 0);
  CHECK_FALSE(committed.wait_for_commits(1, 0ms));

  // Second live frame at 44 → gap of K=2 against the baseline.
  dispatch_seq(CHANNEL, 44);
  REQUIRE(next_event_seq(client) == 44);
  REQUIRE(committed.wait_for_commits(1, 2s));
  CHECK(exactly_one_gap(plinth::ws::gap_audit_emit_count_for_test(),
                        committed.committed_count(),
                        gap_audit_count(pg, user_id)));

  CHECK(gap_audit_count(pg, user_id) == 1);
  auto detail = latest_gap_detail(pg, user_id);
  CHECK(detail.find("\"prev_seq\": 42") != std::string::npos);
  CHECK(detail.find("\"next_seq\": 44") != std::string::npos);
  CHECK(detail.find("\"gap_size\": 1") != std::string::npos);
}

TEST_CASE("S.06d: gap audit commit oracle rejects missing and duplicate writes",
          "[realtime][broker][audit][seq][gap][integration][ws]") {
  using namespace std::chrono_literals;
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  const auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws_test::TestPg pg{cfg.db};
  auto user_id = plinth::ws_test::insert_user(pg, "s06d-control", "pw");
  GapAuditCommitObserver committed{cfg.db, user_id};
  auto& writer = committed.connection_for_control();
  const auto insert = [&] {
    auto result = writer.exec_params(
        "INSERT INTO plinth.audit_log(action, user_id, detail, node_id) "
        "VALUES ('realtime.seq.gap_detected', $1::uuid, '{}'::jsonb, "
        "'test-node')",
        {user_id});
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  };

  SECTION("missing emission cannot authorize a persistence assertion") {
    CHECK_FALSE(committed.wait_for_commits(1, 0ms));
    CHECK_FALSE(exactly_one_gap(0, committed.committed_count(),
                                gap_audit_count(pg, user_id)));
    CHECK_FALSE(exactly_one_gap(0, 1, 1));
  }
  SECTION("an INSERT rolled back after its trigger is not acknowledged") {
    auto began = writer.exec("BEGIN");
    REQUIRE(PQresultStatus(began.get()) == PGRES_COMMAND_OK);
    insert();
    CHECK_FALSE(committed.wait_for_commits(1, 0ms));
    CHECK(gap_audit_count(pg, user_id) == 0);
    auto rolled_back = writer.exec("ROLLBACK");
    REQUIRE(PQresultStatus(rolled_back.get()) == PGRES_COMMAND_OK);
    CHECK_FALSE(committed.wait_for_commits(1, 0ms));
    CHECK_FALSE(exactly_one_gap(1, committed.committed_count(),
                                gap_audit_count(pg, user_id)));
  }
  SECTION("other users actions and notification channels are not "
          "acknowledgements") {
    auto listened = writer.exec("LISTEN unrelated_gap_control");
    REQUIRE(PQresultStatus(listened.get()) == PGRES_COMMAND_OK);
    auto notified =
        writer.exec("SELECT pg_notify('unrelated_gap_control', 'ignored')");
    REQUIRE(PQresultStatus(notified.get()) == PGRES_TUPLES_OK);
    auto unrelated = writer.exec_params(
        "INSERT INTO plinth.audit_log(action, user_id, detail, node_id) VALUES "
        "('gap_control_unrelated', $1::uuid, '{}'::jsonb, 'test-node'), "
        "('realtime.seq.gap_detected', gen_random_uuid(), '{}'::jsonb, "
        "'test-node')",
        {user_id});
    REQUIRE(PQresultStatus(unrelated.get()) == PGRES_COMMAND_OK);
    CHECK_FALSE(committed.wait_for_commits(1, 0ms));
    CHECK(committed.committed_count() == 0);
    CHECK(gap_audit_count(pg, user_id) == 0);
    CHECK_FALSE(exactly_one_gap(1, committed.committed_count(),
                                gap_audit_count(pg, user_id)));
  }
  SECTION("two committed rows cannot pass the exactly-one oracle") {
    insert();
    insert();
    REQUIRE(committed.wait_for_commits(2, 2s));
    CHECK(committed.committed_count() == 2);
    CHECK(gap_audit_count(pg, user_id) == 2);
    CHECK_FALSE(exactly_one_gap(1, committed.committed_count(),
                                gap_audit_count(pg, user_id)));
    CHECK_FALSE(exactly_one_gap(2, 1, 1));
  }
  SECTION("a committed self-NOTIFY buffered by PQexec needs no socket wake") {
    insert();
    pollfd socket{.fd = PQsocket(writer.conn), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&socket, 1, 0) == 0);
    REQUIRE(committed.wait_for_commits(1, 0ms));
    CHECK(exactly_one_gap(1, committed.committed_count(),
                          gap_audit_count(pg, user_id)));
  }
}
