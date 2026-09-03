#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kernel/capabilities/listener.hpp"
#include "kernel/capabilities/registration.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/logging.hpp"

// Tests for the ICD-0.2.3 NOTIFY-driven cache invalidation. Divided into
// three layers:
//
//   * Parse + apply unit tests (no PG — drive apply_notification_for_test
//     against pre-seeded cache entries).
//   * Register-apply integration test (PG required; exercises the
//     fetch_row path after a real INSERT).
//   * Full-loop integration test (PG required; starts the listener
//     thread and asserts the cache converges for each of the four
//     mutation verbs). Also covers the ICD-0.2.2 §Multi-Node claim by
//     opening two bare LISTEN connections and confirming both drain the
//     same NOTIFY.

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

struct TestPg {
  PGconn* conn = nullptr;

  explicit TestPg(const plinth::Config::Database& db) {
    auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                    " dbname=" + db.database + " user=" + db.user +
                    " password=" + db.password;
    conn = PQconnectdb(conninfo.c_str());
  }
  ~TestPg() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  TestPg(const TestPg&) = delete;
  auto operator=(const TestPg&) -> TestPg& = delete;
  TestPg(TestPg&&) = delete;
  auto operator=(TestPg&&) -> TestPg& = delete;

  [[nodiscard]] auto exec(const std::string& sql) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    return {PQexec(conn, sql.c_str()), PQclear};
  }
  [[nodiscard]] auto exec_params(const std::string& sql,
                                 const std::vector<std::string>& params) const
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
      values.push_back(p.c_str());
    }
    return {PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()),
                         nullptr, values.data(), nullptr, nullptr, 0),
            PQclear};
  }
};

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, true);
  plinth::log::set_node_id("test-node");
  plinth::groups::bootstrap_groups(db);
}

auto seed_rule(TestPg& pg, const std::string& rule, const std::string& ns,
               const std::string& ext = "test_ext") -> void {
  auto res = pg.exec_params(
      "INSERT INTO plinth.rbac_rules (rule, namespace, description, "
      "extension_name) "
      "VALUES ($1, $2, 'test rule', $3) ON CONFLICT (rule) DO NOTHING",
      {rule, ns, ext});
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
}

auto valid_ext_reg() -> plinth::capabilities::CapabilityRegistration {
  return plinth::capabilities::CapabilityRegistration{
      .namespace_ = "terminal",
      .version = 1,
      .function = "shell",
      .provider_type = "extension",
      .extension_name = std::string{"terminal"},
      .scope = "instance",
      .description = "Execute a shell command",
      .rbac_rule = "terminal.shell.execute",
  };
}

auto make_cached(std::string signature, std::string extension_name,
                 bool enabled = true)
    -> plinth::capabilities::CachedCapability {
  return plinth::capabilities::CachedCapability{
      .signature = std::move(signature),
      .provider_type = "extension",
      .extension_name = std::move(extension_name),
      .scope = "instance",
      .user_id = {},
      .rbac_rule = "terminal.shell.execute",
      .enabled = enabled,
  };
}

auto default_ctx() -> plinth::capabilities::UserContext {
  // kernel.admin so the 0.2.4 step-3 check is transparent to the
  // listener-focused integration cases below. RBAC itself is unit-
  // tested in resolution_test.cpp.
  return plinth::capabilities::UserContext{
      .user_id = "11111111-1111-1111-1111-111111111111",
      .username = "alice",
      .auth_type = "session",
      .effective_rules = {"kernel.admin"},
      .session_id = {},
      .ip_address = {}};
}

// Poll a predicate up to `max_wait` with 20 ms granularity. Returns
// true once the predicate holds; false on timeout. The predicate is
// taken by value so the loop may invoke it repeatedly without worrying
// about forwarding semantics on each tick.
template <class Pred>
auto wait_for(Pred p, std::chrono::milliseconds max_wait) -> bool {
  using clock = std::chrono::steady_clock;
  auto deadline = clock::now() + max_wait;
  while (clock::now() < deadline) {
    if (p()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  return p();
}

// RAII guard so every integration case stops the listener on exit even
// when a REQUIRE fails mid-case. The global resolver state is otherwise
// shared across Catch2 cases and would leak the thread into later runs.
struct ListenerGuard {
  explicit ListenerGuard(const plinth::Config::Database& db) {
    plinth::capabilities::start_notify_listener(db);
  }
  ~ListenerGuard() { plinth::capabilities::stop_notify_listener(); }
  ListenerGuard(const ListenerGuard&) = delete;
  auto operator=(const ListenerGuard&) -> ListenerGuard& = delete;
  ListenerGuard(ListenerGuard&&) = delete;
  auto operator=(ListenerGuard&&) -> ListenerGuard& = delete;
};

} // namespace

using plinth::capabilities::call_capability;
using plinth::capabilities::CapabilityCall;
using plinth::capabilities::deregister_capability;
using plinth::capabilities::disable_by_extension;
using plinth::capabilities::enable_by_extension;
using plinth::capabilities::register_capability;

// ── Parse / apply unit tests (no PG) ─────────────────────────────

TEST_CASE("apply_notification_for_test rejects malformed JSON",
          "[capabilities][listener][unit]") {
  plinth::capabilities::clear_resolver_for_test();
  auto db = pg_config(); // unused on this path
  REQUIRE_FALSE(
      plinth::capabilities::apply_notification_for_test(db, "not json at all"));
}

TEST_CASE("apply_notification_for_test rejects unknown action",
          "[capabilities][listener][unit]") {
  plinth::capabilities::clear_resolver_for_test();
  auto db = pg_config();
  REQUIRE_FALSE(plinth::capabilities::apply_notification_for_test(
      db, R"({"action":"bogus","signature":"a:1:b","scope":"instance",
              "extension_name":null})"));
}

TEST_CASE("apply_notification_for_test: deregister removes a cached entry",
          "[capabilities][listener][unit]") {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_cached("terminal:1:shell", "terminal"));
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  auto db = pg_config();
  REQUIRE(plinth::capabilities::apply_notification_for_test(
      db,
      R"({"action":"deregister","signature":"terminal:1:shell",
            "scope":"instance","extension_name":null})"));

  REQUIRE(plinth::capabilities::tier2_cache_size() == 0);
}

TEST_CASE("apply_notification_for_test: bulk disable and enable by extension",
          "[capabilities][listener][unit]") {
  plinth::capabilities::clear_resolver_for_test();
  plinth::capabilities::seed_tier2_cache_for_test(
      make_cached("terminal:1:shell", "terminal"));
  plinth::capabilities::seed_tier2_cache_for_test(
      make_cached("terminal:1:pty", "terminal"));
  plinth::capabilities::seed_tier2_cache_for_test(
      make_cached("fs:1:read", "ext_other"));

  auto db = pg_config();
  REQUIRE(plinth::capabilities::apply_notification_for_test(
      db,
      R"({"action":"disable","signature":"","scope":"",
            "extension_name":"terminal"})"));

  auto after_disable = call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, default_ctx());
  REQUIRE_FALSE(after_disable.has_value());
  REQUIRE(plinth::capabilities::error_code(after_disable.error()) ==
          "capability_disabled");

  // Other extension untouched — sync path surfaces async_required
  // for extension entries (ICD-0.5.0.3 §Sync vs async).
  auto other =
      call_capability(CapabilityCall{.signature = "fs:1:read"}, default_ctx());
  REQUIRE(plinth::capabilities::error_code(other.error()) == "async_required");

  REQUIRE(plinth::capabilities::apply_notification_for_test(
      db,
      R"({"action":"enable","signature":"","scope":"",
            "extension_name":"terminal"})"));

  // Sync path surfaces async_required for extension entries
  // (ICD-0.5.0.3 §Sync vs async); proves the enable unlocked the
  // cache row.
  auto after_enable = call_capability(
      CapabilityCall{.signature = "terminal:1:shell"}, default_ctx());
  REQUIRE(plinth::capabilities::error_code(after_enable.error()) ==
          "async_required");
}

// ── Register fetch path (PG-backed, targeted) ────────────────────

TEST_CASE("apply_notification_for_test: register fetches the row and upserts",
          "[capabilities][listener][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  plinth::capabilities::clear_resolver_for_test();

  // Insert the row directly so the fetch_row SELECT sees it, then
  // deliver the payload the producer would have emitted.
  auto insert = register_capability(db, valid_ext_reg(), {});
  REQUIRE(insert.has_value());
  plinth::capabilities::clear_resolver_for_test(); // listener hasn't run

  REQUIRE(plinth::capabilities::apply_notification_for_test(
      db,
      R"({"action":"register","signature":"terminal:1:shell",
            "scope":"instance","extension_name":"terminal"})"));

  auto out = call_capability(CapabilityCall{.signature = "terminal:1:shell"},
                             default_ctx());
  // Sync path surfaces async_required for extension entries
  // (ICD-0.5.0.3 §Sync vs async); the observable proof that the row
  // was cached.
  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "async_required");
}

// ── Full loop (PG + thread) ──────────────────────────────────────

TEST_CASE("start_notify_listener: cache converges across all four actions",
          "[capabilities][listener][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  plinth::capabilities::clear_resolver_for_test();
  ListenerGuard guard(db);

  const auto WAIT = std::chrono::seconds{5};

  // ── register ────────────────────────────────────────────
  auto r = register_capability(db, valid_ext_reg(), {});
  REQUIRE(r.has_value());
  REQUIRE(wait_for([] { return plinth::capabilities::tier2_cache_size() == 1; },
                   WAIT));

  auto call_shell = [] {
    return call_capability(CapabilityCall{.signature = "terminal:1:shell"},
                           default_ctx());
  };
  auto out_reg = call_shell();
  REQUIRE_FALSE(out_reg.has_value());
  REQUIRE(plinth::capabilities::error_code(out_reg.error()) ==
          "async_required");

  // ── disable ─────────────────────────────────────────────
  auto d = disable_by_extension(db, "terminal", {});
  REQUIRE(d.has_value());
  REQUIRE(wait_for(
      [&] {
        auto o = call_shell();
        return !o.has_value() && plinth::capabilities::error_code(o.error()) ==
                                     "capability_disabled";
      },
      WAIT));

  // ── enable ──────────────────────────────────────────────
  auto e = enable_by_extension(db, "terminal", {});
  REQUIRE(e.has_value());
  REQUIRE(wait_for(
      [&] {
        auto o = call_shell();
        return !o.has_value() &&
               plinth::capabilities::error_code(o.error()) == "async_required";
      },
      WAIT));

  // ── deregister ──────────────────────────────────────────
  auto dr = deregister_capability(db, "terminal:1:shell", "instance", {});
  REQUIRE(dr.has_value());
  REQUIRE(wait_for(
      [&] {
        auto o = call_shell();
        return !o.has_value() && plinth::capabilities::error_code(o.error()) ==
                                     "capability_not_found";
      },
      WAIT));
}

// ── Multi-node smoke ─────────────────────────────────────────────

TEST_CASE("NOTIFY reaches multiple LISTEN connections",
          "[capabilities][listener][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  TestPg sub_a(db);
  TestPg sub_b(db);
  REQUIRE(
      PQresultStatus(sub_a.exec("LISTEN plinth_capability_changed").get()) ==
      PGRES_COMMAND_OK);
  REQUIRE(
      PQresultStatus(sub_b.exec("LISTEN plinth_capability_changed").get()) ==
      PGRES_COMMAND_OK);

  auto r = register_capability(db, valid_ext_reg(), {});
  REQUIRE(r.has_value());

  auto drain_one = [](TestPg& sub) -> std::string {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::seconds{3};
    while (clock::now() < deadline) {
      REQUIRE(PQconsumeInput(sub.conn) == 1);
      std::unique_ptr<PGnotify, decltype(&PQfreemem)> n(PQnotifies(sub.conn),
                                                        PQfreemem);
      if (n != nullptr) {
        return n->extra == nullptr ? "" : std::string{n->extra};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return {};
  };

  auto a = drain_one(sub_a);
  auto b = drain_one(sub_b);
  REQUIRE(a.find("\"signature\":\"terminal:1:shell\"") != std::string::npos);
  REQUIRE(b.find("\"signature\":\"terminal:1:shell\"") != std::string::npos);
}

// ── reload_tier2_cache (ICD-0.2.4 amendment) ─────────────────────
//
// Simulates a missed NOTIFY by inserting a row while no listener is
// running, then asserts that reload_tier2_cache() reconciles the cache
// to match the authoritative table. The listener open path calls this
// helper on every LISTEN open, so this case is a targeted unit for the
// same code exercised every reconnect.

TEST_CASE("reload_tier2_cache syncs the cache from plinth.capabilities",
          "[capabilities][listener][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  plinth::capabilities::clear_resolver_for_test();

  // Seed a stale cache entry that does NOT exist in the DB — it must
  // be evicted by the resync.
  plinth::capabilities::upsert_tier2_entry(
      make_cached("ghost:1:stale", "ghost"));
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  // Insert a real row directly (no listener, no NOTIFY emitted by
  // the consumer side — mirrors the missed-NOTIFY case).
  auto r = register_capability(db, valid_ext_reg(), {});
  REQUIRE(r.has_value());

  // Cache still only has the stale entry until the resync runs.
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  auto loaded = plinth::capabilities::reload_tier2_cache(db);
  REQUIRE(loaded == 1);
  REQUIRE(plinth::capabilities::tier2_cache_size() == 1);

  // Stale entry is gone; real entry resolves through dispatch (sync
  // path surfaces async_required for extension entries per
  // ICD-0.5.0.3 §Sync vs async).
  auto out = call_capability(CapabilityCall{.signature = "terminal:1:shell"},
                             default_ctx());
  REQUIRE_FALSE(out.has_value());
  REQUIRE(plinth::capabilities::error_code(out.error()) == "async_required");

  auto ghost = call_capability(CapabilityCall{.signature = "ghost:1:stale"},
                               default_ctx());
  REQUIRE_FALSE(ghost.has_value());
  REQUIRE(plinth::capabilities::error_code(ghost.error()) ==
          "capability_not_found");
}
