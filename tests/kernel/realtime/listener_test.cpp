// SPDX-License-Identifier: MIT
//
// ICD-0.5.0 §Test Cases — R.* listener lifecycle tests.
//
// R.02 / R.06 / R.07 — no-PG lifecycle (no spawn, idempotent stop,
//     double-start). [realtime][unit].
// R.04           — bad-port PG at start; reconnect-loop + stop-interrupt.
//                  [realtime][unit] (simplified from ICD's "live toggle"
//                  — flagged in PR body).
// R.08 / R.09 / R.10 — parse-path via apply_notification_for_test seam.
//                      [realtime][unit].
// R.01 / R.03 / R.05 — end-to-end via real PG side-conn + pg_notify.
//                      [realtime][integration].

#include "kernel/config.hpp"
#include "kernel/realtime/listener.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Small backoff so any fallthrough into the reconnect loop (R.06/R.07
// where the PG connect will fail and the thread spins) doesn't block
// stop() for 1 s. The wait is interruptible via the wake-fd but this
// keeps test runtime tight.
auto make_test_listener_cfg(bool enabled = true)
    -> plinth::Config::Realtime::Listener {
  plinth::Config::Realtime::Listener cfg;
  cfg.enabled = enabled;
  cfg.reconnect_backoff_ms = 100;
  return cfg;
}

// Bad-port db_cfg so the listener's connect attempt fails immediately
// and the test does not require a live PG. Thread enters the reconnect
// loop; stop_listener() interrupts via the wake-fd before the 100 ms
// backoff elapses.
auto unreachable_db_cfg() -> plinth::Config::Database {
  plinth::Config::Database db;
  db.host = "127.0.0.1";
  db.port = 1; // unassigned low port — connect fails fast
  db.user = "plinth";
  db.password = "plinth";
  db.database = "plinth";
  return db;
}

// ── PG integration helpers (R.01/R.03/R.05) ────────────────────────

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

struct SidePg {
  PGconn* conn = nullptr;
  explicit SidePg(const plinth::Config::Database& db) {
    auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                    " dbname=" + db.database + " user=" + db.user +
                    " password=" + db.password;
    conn = PQconnectdb(conninfo.c_str());
  }
  ~SidePg() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  SidePg(const SidePg&) = delete;
  auto operator=(const SidePg&) -> SidePg& = delete;
  SidePg(SidePg&&) = delete;
  auto operator=(SidePg&&) -> SidePg& = delete;
};

// Raw pg_notify via a side connection — decouples R.01/R.03/R.05 from
// emit_notify correctness (which E.01 covers in slice 5).
auto side_notify(SidePg& side, const std::string& envelope_json) -> bool {
  std::array<const char*, 2> values = {"plinth:realtime",
                                       envelope_json.c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> res{
      PQexecParams(side.conn, "SELECT pg_notify($1, $2)", 2, nullptr,
                   values.data(), nullptr, nullptr, 0),
      PQclear};
  return PQresultStatus(res.get()) == PGRES_TUPLES_OK;
}

// Wait briefly for the listener's LISTEN to take effect. The LISTEN
// happens inside the listener thread on first-iteration connect; a
// short settle window avoids a race where side_notify fires before
// LISTEN has been issued.
auto settle_listen() -> void {
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

} // namespace

TEST_CASE("R.02 start_listener with enabled=false does not spawn thread",
          "[realtime][unit]") {
  plinth::realtime::clear_handlers_for_test();
  auto db = unreachable_db_cfg();
  auto cfg = make_test_listener_cfg(/*enabled=*/false);

  plinth::realtime::start_listener(db, cfg);
  // With no thread spawned, stop should be an immediate no-op.
  plinth::realtime::stop_listener();

  // Reaching here without hang/assert is the verification.
  SUCCEED("start_listener with enabled=false returned promptly");
}

TEST_CASE("R.06 stop_listener is idempotent", "[realtime][unit]") {
  plinth::realtime::clear_handlers_for_test();
  auto db = unreachable_db_cfg();
  auto cfg = make_test_listener_cfg();

  plinth::realtime::start_listener(db, cfg);
  plinth::realtime::stop_listener();
  // Second stop must be a clean no-op.
  plinth::realtime::stop_listener();
  // And a third for good measure.
  plinth::realtime::stop_listener();

  SUCCEED("repeated stop_listener calls did not hang or crash");
}

TEST_CASE("R.07 double-start is a no-op", "[realtime][unit]") {
  plinth::realtime::clear_handlers_for_test();
  auto db = unreachable_db_cfg();
  auto cfg = make_test_listener_cfg();

  plinth::realtime::start_listener(db, cfg);
  // Second start must not spawn a second thread or crash.
  plinth::realtime::start_listener(db, cfg);

  plinth::realtime::stop_listener();

  SUCCEED("double start_listener did not crash; stop joined cleanly");
}

TEST_CASE("R.04 bad-port PG at start — reconnect loop + stop interrupt",
          "[realtime][unit]") {
  // Simplified per plan gotcha 8: instead of a live PG toggle we
  // drive the thread into the reconnect loop via an unreachable
  // host and verify stop_listener() interrupts the backoff sleep
  // via the wake-fd rather than waiting for the full interval.
  plinth::realtime::clear_handlers_for_test();
  auto db = unreachable_db_cfg();
  plinth::Config::Realtime::Listener cfg;
  cfg.enabled = true;
  cfg.reconnect_backoff_ms = 5000; // long — would hang stop() without wake-fd

  plinth::realtime::start_listener(db, cfg);

  // Let the thread enter the backoff sleep.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto t0 = std::chrono::steady_clock::now();
  plinth::realtime::stop_listener();
  auto dur = std::chrono::steady_clock::now() - t0;

  // Stop should return well under the 5 s backoff — wake-fd
  // interrupts the poll immediately.
  REQUIRE(dur < std::chrono::seconds(2));
}

TEST_CASE("R.08 parse-reject on malformed JSON NOTIFY", "[realtime][unit]") {
  plinth::realtime::clear_handlers_for_test();
  std::atomic<int> calls{0};
  plinth::realtime::register_handler([&calls](const auto&) { ++calls; });

  auto ok = plinth::realtime::apply_notification_for_test("plinth:realtime",
                                                          "{not-valid-json");
  REQUIRE_FALSE(ok);
  REQUIRE(calls.load() == 0);
}

TEST_CASE("R.09 parse-reject on missing-layer NOTIFY", "[realtime][unit]") {
  plinth::realtime::clear_handlers_for_test();
  std::atomic<int> calls{0};
  plinth::realtime::register_handler([&calls](const auto&) { ++calls; });

  auto ok = plinth::realtime::apply_notification_for_test(
      "plinth:realtime", R"({"channel":"plinth:system:foo"})");
  REQUIRE_FALSE(ok);
  REQUIRE(calls.load() == 0);
}

TEST_CASE("R.10 multi-handler — all handlers invoked in registration order",
          "[realtime][unit]") {
  plinth::realtime::clear_handlers_for_test();

  std::mutex mu;
  std::vector<int> order;
  plinth::realtime::register_handler([&mu, &order](const auto&) {
    std::lock_guard lock(mu);
    order.push_back(1);
  });
  plinth::realtime::register_handler([&mu, &order](const auto&) {
    std::lock_guard lock(mu);
    order.push_back(2);
  });
  plinth::realtime::register_handler([&mu, &order](const auto&) {
    std::lock_guard lock(mu);
    order.push_back(3);
  });

  auto ok = plinth::realtime::apply_notification_for_test(
      "plinth:realtime",
      R"({"layer":"system","channel":"plinth:system:multi.handler"})");
  REQUIRE(ok);
  std::lock_guard lock(mu);
  REQUIRE(order == std::vector<int>{1, 2, 3});
}

// ── PG-gated integration tests (R.01 / R.03 / R.05) ────────────────

TEST_CASE("R.01 listener delivers valid NOTIFY to handler",
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
  settle_listen();

  SidePg side{db};
  REQUIRE(PQstatus(side.conn) == CONNECTION_OK);
  REQUIRE(side_notify(
      side,
      R"({"layer":"system","channel":"plinth:system:test.r01","payload":{"x":1}})"));

  {
    std::unique_lock lock(mu);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5),
                        [&received] { return !received.empty(); }));
  }

  plinth::realtime::stop_listener();

  REQUIRE(received.size() == 1);
  REQUIRE(received[0].layer == "system");
  REQUIRE(received[0].channel == "plinth:system:test.r01");
}

TEST_CASE("R.03 listener reconnects after PG connection killed",
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
  settle_listen();

  // Kill every backend application_name matching the listener's
  // conninfo user/database. libpq default application_name is the
  // program name ("plinth_tests"). Kill all "plinth" user backends
  // EXCEPT our side connection.
  SidePg side{db};
  REQUIRE(PQstatus(side.conn) == CONNECTION_OK);
  auto my_pid_res = std::unique_ptr<PGresult, decltype(&PQclear)>{
      PQexec(side.conn, "SELECT pg_backend_pid()"), PQclear};
  REQUIRE(PQresultStatus(my_pid_res.get()) == PGRES_TUPLES_OK);
  std::string my_pid = PQgetvalue(my_pid_res.get(), 0, 0);
  auto kill_sql =
      std::string{"SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                  "WHERE pid <> "} +
      my_pid +
      " AND usename = current_user "
      " AND datname = current_database() "
      " AND query LIKE '%LISTEN%' ";
  auto kill_res = std::unique_ptr<PGresult, decltype(&PQclear)>{
      PQexec(side.conn, kill_sql.c_str()), PQclear};
  REQUIRE(PQresultStatus(kill_res.get()) == PGRES_TUPLES_OK);

  // Wait for reconnect (backoff + LISTEN settle).
  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  REQUIRE(side_notify(
      side, R"({"layer":"system","channel":"plinth:system:test.r03"})"));

  {
    std::unique_lock lock(mu);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5),
                        [&received] { return !received.empty(); }));
  }

  plinth::realtime::stop_listener();

  REQUIRE(!received.empty());
  REQUIRE(received[0].channel == "plinth:system:test.r03");
}

TEST_CASE("R.05 stop_listener barriers on in-flight handler dispatch",
          "[realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  plinth::realtime::clear_handlers_for_test();

  std::atomic<bool> handler_started{false};
  std::atomic<bool> handler_finished{false};
  plinth::realtime::register_handler(
      [&handler_started, &handler_finished](const auto&) {
        handler_started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        handler_finished.store(true);
      });

  auto db = pg_config();
  plinth::Config::Realtime::Listener lcfg;
  lcfg.reconnect_backoff_ms = 200;
  plinth::realtime::start_listener(db, lcfg);
  settle_listen();

  SidePg side{db};
  REQUIRE(PQstatus(side.conn) == CONNECTION_OK);
  REQUIRE(side_notify(
      side, R"({"layer":"system","channel":"plinth:system:test.r05"})"));

  // Wait for the handler to enter (but not finish).
  for (int i = 0; i < 100; ++i) {
    if (handler_started.load()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(handler_started.load());
  REQUIRE_FALSE(handler_finished.load());

  // stop_listener must BLOCK until the handler completes.
  plinth::realtime::stop_listener();
  REQUIRE(handler_finished.load());
}
