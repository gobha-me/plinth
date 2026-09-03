// SPDX-License-Identifier: MIT
//
// ICD-0.5.1 §Test Cases — I.* end-to-end integration.
//
// Each case wires a live Drogon DbClient + `realtime::start_listener`
// + a test-registered `EventHandler`. The test exercises the full
// production path: classify_sql() → record_write() → window timer →
// flush → emit_notify_async → PG NOTIFY → listener → handler.
// DbClient is injected via `set_db_client_for_test` so the coalescer
// doesn't require the full Drogon app to be running.

#include "kernel/config.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/realtime/sql_classify.hpp"
#include "shared_pg_client.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using plinth::realtime::classify_sql;
using plinth::realtime::CoalescerRegistry;
using plinth::realtime::DispatchedEvent;
using plinth::realtime::OpKind;

namespace {

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

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

// Handler-received envelope capture — shared across a test's scope.
struct Received {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<DispatchedEvent> events;

  auto on_event(const DispatchedEvent& ev) -> void {
    {
      std::lock_guard lock(mu);
      events.push_back(ev);
    }
    cv.notify_all();
  }

  auto wait_for_n(std::size_t n, std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, timeout,
                       [this, n]() { return events.size() >= n; });
  }

  auto size() -> std::size_t {
    std::lock_guard lock(mu);
    return events.size();
  }
};

// RAII harness — start listener + coalescer against a live PG + test
// DbClient; register a capturing handler; tear everything down in
// ~Harness so each TEST_CASE starts from a clean baseline.
struct Harness {
  std::shared_ptr<Received> received;
  drogon::orm::DbClientPtr db;
  plinth::Config::Realtime::Coalescer cfg;

  explicit Harness(std::size_t window_ms = 80) {
    received = std::make_shared<Received>();

    plinth::realtime::clear_handlers_for_test();
    plinth::realtime::register_handler(
        [rcv = received](const DispatchedEvent& ev) { rcv->on_event(ev); });

    auto db_cfg = pg_config();
    plinth::Config::Realtime::Listener lcfg;
    lcfg.reconnect_backoff_ms = 200;
    plinth::realtime::start_listener(db_cfg, lcfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 0.6.3.N — shared process-lifetime client; per-test create+
    // destroy of `newPgClient` reproducibly trips
    // `EventLoopThreadPool::~ + Resource deadlock avoided`. Member-
    // init not viable: setup work above (`start_listener` + sleep)
    // must precede the assignment.
    db = plinth::realtime_test::shared_pg_client(/*connNum=*/1);
    REQUIRE(db);

    auto& reg = CoalescerRegistry::instance();
    reg.clear_windows_for_test();
    reg.shutdown();
    reg.clear_emit_hook_for_test();
    reg.set_db_client_for_test(db);

    cfg.enabled = true;
    cfg.window_ms = window_ms;
    reg.start(cfg);
  }

  ~Harness() {
    auto& reg = CoalescerRegistry::instance();
    reg.clear_windows_for_test();
    reg.shutdown();
    reg.set_db_client_for_test(nullptr);
    plinth::realtime::stop_listener();
    plinth::realtime::clear_handlers_for_test();
  }
  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
  Harness(Harness&&) = delete;
  auto operator=(Harness&&) -> Harness& = delete;
};

// Simulate the `run_db_exec_outcome` hook path: classify then record.
auto simulated_exec(std::string_view sql, std::string_view ext_name,
                    std::size_t row_count) -> void {
  if (auto c = classify_sql(sql, ext_name); c.has_value()) {
    CoalescerRegistry::instance().record_write(c->schema, c->table, c->op_kind,
                                               row_count, ext_name);
  }
}

} // namespace

TEST_CASE("I.01 one simulated write → one envelope",
          "[realtime][coalescer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  Harness h{80};

  simulated_exec("INSERT INTO ext_it01.t (id) VALUES ('x')", "it01", 1);

  REQUIRE(h.received->wait_for_n(1, std::chrono::milliseconds(500)));
  const auto& ev = h.received->events.at(0);
  CHECK(ev.layer == "data");
  CHECK(ev.channel == "plinth:data:ext_it01.t");
  CHECK(ev.envelope["schema"].asString() == "ext_it01");
  CHECK(ev.envelope["table"].asString() == "t");
  CHECK(ev.envelope["ops"][0]["count"].asUInt64() == 1);
}

TEST_CASE("I.02 three simulated writes within window → one envelope",
          "[realtime][coalescer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  Harness h{200};

  simulated_exec("INSERT INTO ext_it02.t (id) VALUES ('a')", "it02", 1);
  simulated_exec("INSERT INTO ext_it02.t (id) VALUES ('b')", "it02", 1);
  simulated_exec("INSERT INTO ext_it02.t (id) VALUES ('c')", "it02", 1);

  REQUIRE(h.received->wait_for_n(1, std::chrono::milliseconds(600)));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK(h.received->size() == 1);
  CHECK(h.received->events.at(0).envelope["ops"][0]["count"].asUInt64() == 3);
}

TEST_CASE("I.03 writes to two tables → two envelopes",
          "[realtime][coalescer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  Harness h{80};

  simulated_exec("INSERT INTO ext_it03.a (id) VALUES ('1')", "it03", 1);
  simulated_exec("INSERT INTO ext_it03.b (id) VALUES ('2')", "it03", 1);

  REQUIRE(h.received->wait_for_n(2, std::chrono::milliseconds(500)));
  CHECK(h.received->size() == 2);
}

TEST_CASE("I.04 mixed INSERT/UPDATE/DELETE → one envelope with all counts",
          "[realtime][coalescer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  Harness h{200};

  simulated_exec("INSERT INTO ext_it04.t (id) VALUES ('x')", "it04", 1);
  simulated_exec("UPDATE ext_it04.t SET x = 1 WHERE id = 'x'", "it04", 1);
  simulated_exec("DELETE FROM ext_it04.t WHERE id = 'x'", "it04", 1);

  REQUIRE(h.received->wait_for_n(1, std::chrono::milliseconds(500)));
  const auto& env = h.received->events.at(0).envelope;
  CHECK(env["ops"][0]["count"].asUInt64() == 1);
  CHECK(env["ops"][1]["count"].asUInt64() == 1);
  CHECK(env["ops"][2]["count"].asUInt64() == 1);
}

TEST_CASE("I.05 unparseable SQL (SELECT) via DB_EXEC → no envelope",
          "[realtime][coalescer][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — set PLINTH_PG_HOST + friends to run");
  }
  Harness h{80};

  // Classifier returns std::nullopt → record_write is never called.
  simulated_exec("SELECT 1", "it05", 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(h.received->size() == 0);
}
