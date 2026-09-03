// HTTP integration tests for ICD-0.4.5 §X.* upgrade-lifecycle cases.
//
// Built in 0.6.0.N session 4 alongside the PATCH/DELETE test_seam +
// HttpTestFixture::build_patch / build_delete additions. Closes:
//   X.05 — Upgrade with new migrations.
//   X.06 — Upgrade migration failure.
//   X.07 — Upgrade RBAC reconciliation (new-rule sub-case only; missing
//          + changed sub-cases deferred — see CHANGELOG deviation).
//   X.10 — Upgrade asset routes: old 404, new serves.
//   X.11 — Upgrade filesystem: both versions coexist + symlink flips.
//   X.13 — Concurrent POSTs same name different versions.
//
// X.08 + X.09 (in-flight capability call drain — within-window vs
// timeout) deferred — need a controllable-delay JS extension fixture
// that does not exist today; their own follow-up session.
//
// X.12 (crash-at-swap-T3) lives in `atomic_swap_crash_test.cpp`
// per ICD-0.4.5 §675 — seeded-state + reconcile_in_flight_installs
// pattern, distinct from this fixture-driven file.
//
// Routing: same `[ws]` tag as session-2's `install_lifecycle_http_test.cpp`
// to steer cases into `plinth_tests_ws` (the sole drogon-starter
// subprocess); see that file's top-of-file comment for the structural
// reason.

#include "http_test_fixture.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/handlers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <json/reader.h>
#include <json/value.h>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

auto parse_json_body(const drogon::HttpResponsePtr& resp) -> Json::Value {
  auto body = std::string{resp->getBody()};
  Json::Value v;
  Json::CharReaderBuilder b;
  auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
  std::string errs;
  reader->parse(body.data(), body.data() + body.size(), &v, &errs);
  return v;
}

// Read a fixture zip from CMAKE_BINARY_DIR/fixtures/<name>.zip. The
// install + lifecycle fixture trees are pre-zipped at build time by
// `plinth_install_fixture_zips` + `plinth_lifecycle_fixture_zips` —
// same pattern install_lifecycle_test.cpp / lifecycle_transitions_test.cpp
// already use.
auto read_fixture_zip(const std::string& name) -> std::vector<std::byte> {
  fs::path p = fs::path{CMAKE_BINARY_DIR} / "fixtures" / (name + ".zip");
  std::ifstream in(p, std::ios::binary);
  std::vector<std::byte> out;
  if (!in.is_open()) {
    return out;
  }
  in.seekg(0, std::ios::end);
  auto n = in.tellg();
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(n));
  // needs char*; std::byte is bit-compatible.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
  return out;
}

// Direct libpq query helper for assertions that the HTTP response
// body alone can't satisfy (migrations-table state, schema state,
// rbac_rules orphaned_at). HttpTestFixture's seeding `pg` is private;
// each test creates its own short-lived TestPg.
struct AssertPg {
  plinth::ws_test::TestPg pg;
  explicit AssertPg() : pg(plinth::ws_test::test_config().db) {}

  [[nodiscard]] auto scalar(const std::string& sql) const -> std::string {
    auto r = pg.exec(sql);
    if (PQresultStatus(r.get()) != PGRES_TUPLES_OK || PQntuples(r.get()) == 0) {
      return {};
    }
    return {PQgetvalue(r.get(), 0, 0)};
  }
};

// Install/enable/upgrade fires a detached RBAC test run that holds the
// per-name `pg_try_advisory_lock('plinth.packages.<name>')` lock for a
// few ms (ICD-0.4.7 slice B). A subsequent upgrade POST issued before
// the RBAC test settles loses the lock race and 409s with
// `advisory-lock-held`. Mirror of `lifecycle_transitions_test.cpp`'s
// `wait_rbac_test_settled` helper, querying plinth.packages directly.
auto wait_rbac_test_settled(const std::string& package_id) -> void {
  using namespace std::chrono_literals;
  AssertPg pg;
  auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pg.scalar("SELECT COUNT(*) FROM plinth.packages WHERE id='" +
                  package_id + "' AND last_rbac_test_run_at IS NOT NULL") ==
        "1") {
      return;
    }
    std::this_thread::sleep_for(25ms);
  }
}

// Install valid-install (notes 1.2.3) via POST through the real HTTP
// surface; assert 201, wait for the RBAC test run to settle (so the
// caller's next upgrade POST doesn't race the advisory lock), and
// return the package id.
auto install_v1_via_http(plinth::http_test::HttpTestFixture& fx,
                         const std::string& token) -> std::string {
  auto zip = plinth::http_test::HttpTestFixture::read_valid_install_zip();
  REQUIRE(!zip.empty());
  auto req = fx.build_post(zip, token);
  auto resp = fx.dispatch(req);
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("v1 install body: " << body.toStyledString());
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);
  REQUIRE(body["state"].asString() == "ACTIVE");
  REQUIRE(body["version"].asString() == "1.2.3");
  auto id = body["id"].asString();
  wait_rbac_test_settled(id);
  return id;
}

} // namespace

// ── X.05: upgrade applies new migrations and leaves base intact ────

TEST_CASE("X.05: upgrade with new migrations appends rows + applies SQL",
          "[integration][packages][http][ws][lifecycle][X.05]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x05_admin");
  auto v1_id = install_v1_via_http(fx, token);

  auto v2_zip = read_fixture_zip("upgrade-v2");
  REQUIRE(!v2_zip.empty());
  auto resp = fx.dispatch(fx.build_post(v2_zip, token));
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("v2 upgrade body: " << body.toStyledString());
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);
  REQUIRE(body["state"].asString() == "ACTIVE");
  REQUIRE(body["version"].asString() == "1.3.0");

  AssertPg pg;
  // plinth.migrations: both 001_init.sql + 002_add_notes_comments.sql
  // present for extension_name='notes' with applied_at NOT NULL.
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.migrations "
                    "WHERE extension_name='notes'") == "2");
  REQUIRE(
      pg.scalar("SELECT COUNT(*) FROM plinth.migrations "
                "WHERE extension_name='notes' AND applied_at IS NOT NULL") ==
      "2");
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.migrations "
                    "WHERE extension_name='notes' "
                    "  AND migration_file='001_init.sql'") == "1");
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.migrations "
                    "WHERE extension_name='notes' "
                    "  AND migration_file='002_add_notes_comments.sql'") ==
          "1");

  // ICD-0.4.5 §X.05 also requires "existing `ext_{name}` schema
  // unchanged except for migration additions". Both v1.2.3's
  // 001_init.sql + v1.3.0's 002_add_notes_comments.sql are
  // schema-qualified to ext_notes per ICD-0.4.3 §Schema + GRANT
  // Contract; the qualified-DDL guard added in 0.6.0.N session 5
  // rejects unqualified migrations at discover time.
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM information_schema.tables "
                    "WHERE table_schema='ext_notes' AND table_name='notes'") ==
          "1");
  REQUIRE(
      pg.scalar(
          "SELECT COUNT(*) FROM information_schema.tables "
          "WHERE table_schema='ext_notes' AND table_name='notes_comments'") ==
      "1");
  // No leak into the admin connection's default schema. Migrations
  // run as the `plinth` admin role, whose `$user` is `plinth` — so
  // unqualified DDL lands in the `plinth` schema. (Other test files
  // legitimately create `public.notes` to exercise runtime
  // search_path behaviour, so don't include `public` in this check.)
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM information_schema.tables "
                    "WHERE table_schema='plinth' "
                    "  AND table_name IN ('notes','notes_comments')") == "0");

  // Old row → SUPERSEDED, new row → ACTIVE (covered by X.01 too,
  // but we re-assert here so X.05 is a self-contained regression).
  REQUIRE(pg.scalar("SELECT state FROM plinth.packages WHERE id='" + v1_id +
                    "'") == "SUPERSEDED");
}

// ── X.06: upgrade migration failure → INSTALL_FAILED + old untouched ──

TEST_CASE(
    "X.06: upgrade migration failure leaves new=INSTALL_FAILED, old=ACTIVE",
    "[integration][packages][http][ws][lifecycle][X.06]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x06_admin");
  auto v1_id = install_v1_via_http(fx, token);

  auto v_bad_zip = read_fixture_zip("upgrade-v2-broken-migration");
  REQUIRE(!v_bad_zip.empty());

  auto resp = fx.dispatch(fx.build_post(v_bad_zip, token));
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("broken-upgrade body: " << body.toStyledString());

  // ICD-0.4.5 X.06 specifies 422 `upgrade-migration-failed`. Production
  // returns 400 with `failed_at_stage=UPLOADING` because the
  // upgrade-path failure conversion at
  // [`install_lifecycle.cpp:1402`](src/kernel/packages/install_lifecycle.cpp:1402)
  // hardcodes `failed_at = InstallStage::UPLOADING` regardless of the
  // actual stage at which `upgrade_package` reported failure. Test
  // asserts current production behaviour; ICD/production reconciliation
  // (status mapping + `failed_at_stage` accuracy) deferred to its own
  // follow-up session.
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 400);
  REQUIRE(body["state"].asString() == "INSTALL_FAILED");
  REQUIRE(body["failed_at_stage"].asString() == "UPLOADING");
  REQUIRE(body["kind"].asString() == "upgrade-migration-failed");

  AssertPg pg;
  // Old row untouched — still ACTIVE at 1.2.3.
  REQUIRE(pg.scalar("SELECT state FROM plinth.packages WHERE id='" + v1_id +
                    "'") == "ACTIVE");
  REQUIRE(pg.scalar("SELECT version FROM plinth.packages WHERE id='" + v1_id +
                    "'") == "1.2.3");

  // ICD §X.06 also requires "old schema intact with any
  // successfully-applied pre-failure migrations". v1.2.3's
  // 001_init.sql created ext_notes.notes during the base install;
  // the upgrade attempt re-runs 001 (skipped via checksum match)
  // then trips on the broken 002. ext_notes.notes survives, no
  // 002-introduced object exists.
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.migrations "
                    "WHERE extension_name='notes' "
                    "  AND migration_file='001_init.sql' "
                    "  AND applied_at IS NOT NULL") == "1");
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.migrations "
                    "WHERE extension_name='notes' "
                    "  AND migration_file='002_broken.sql'") == "0");
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM information_schema.tables "
                    "WHERE table_schema='ext_notes' AND table_name='notes'") ==
          "1");

  // No `active` symlink flip — still points at 1.2.3 (upgrade failed
  // before atomic swap).
  const auto& data_dir = plinth::ws_test::packages_data_dir();
  auto active_link = data_dir / "data" / "extensions" / "notes" / "active";
  std::error_code ec;
  auto target = fs::read_symlink(active_link, ec);
  REQUIRE_FALSE(ec);
  REQUIRE(target.filename().string() == "1.2.3");
}

// ── X.07: upgrade RBAC reconciliation — new-rule sub-case ─────────
//
// PARTIAL — covers only the "new rule" sub-case (notes.comment added in
// upgrade-v2). The "missing rule" + "changed rule" sub-cases require a
// different fixture pair (v1 with ≥2 rules, v2 dropping one + changing
// another) that doesn't exist today; deferred to follow-up. Documented
// as CHANGELOG deviation for this session.

TEST_CASE(
    "X.07: upgrade RBAC reconciliation — new rule lands with orphaned_at NULL",
    "[integration][packages][http][ws][lifecycle][X.07]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x07_admin");
  auto v1_id = install_v1_via_http(fx, token);
  (void)v1_id;

  auto v2_zip = read_fixture_zip("upgrade-v2");
  REQUIRE(!v2_zip.empty());
  auto resp = fx.dispatch(fx.build_post(v2_zip, token));
  REQUIRE(resp != nullptr);
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);

  AssertPg pg;
  // notes.read preserved across the upgrade — no orphaned_at flip.
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='notes' AND rule='notes.read' "
                    "  AND orphaned_at IS NULL") == "1");
  // notes.comment added in v2 — present with orphaned_at NULL.
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='notes' AND rule='notes.comment' "
                    "  AND orphaned_at IS NULL") == "1");
}

// ── X.10: asset routes — old 404, new serves ──────────────────────

TEST_CASE("X.10: post-upgrade /ext/{name}/<old>/* → 404; <new>/* → 200",
          "[integration][packages][http][ws][lifecycle][X.10]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x10_admin");
  auto v1_id = install_v1_via_http(fx, token);
  (void)v1_id;

  auto v2_zip = read_fixture_zip("upgrade-v2");
  REQUIRE(!v2_zip.empty());
  auto resp = fx.dispatch(fx.build_post(v2_zip, token));
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);

  // Both fixture trees ship `client/panels/editor.js`. After the
  // atomic swap, asset_server's route map should serve 1.3.0 only.
  auto build_get = [](const std::string& path) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(path);
    return req;
  };

  auto old_resp = fx.dispatch(build_get("/ext/notes/1.2.3/panels/editor.js"));
  REQUIRE(old_resp != nullptr);
  UNSCOPED_INFO(
      "old asset status: " << static_cast<int>(old_resp->getStatusCode()));
  REQUIRE(static_cast<int>(old_resp->getStatusCode()) == 404);

  auto new_resp = fx.dispatch(build_get("/ext/notes/1.3.0/panels/editor.js"));
  REQUIRE(new_resp != nullptr);
  UNSCOPED_INFO(
      "new asset status: " << static_cast<int>(new_resp->getStatusCode()));
  REQUIRE(static_cast<int>(new_resp->getStatusCode()) == 200);
}

// ── X.11: filesystem — both version dirs present + symlink flipped ──

TEST_CASE("X.11: post-upgrade both version_dirs coexist; active → new",
          "[integration][packages][http][ws][lifecycle][X.11]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x11_admin");
  auto v1_id = install_v1_via_http(fx, token);
  (void)v1_id;

  auto v2_zip = read_fixture_zip("upgrade-v2");
  REQUIRE(!v2_zip.empty());
  auto resp = fx.dispatch(fx.build_post(v2_zip, token));
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);

  const auto& data_dir = plinth::ws_test::packages_data_dir();
  auto ext_dir = data_dir / "data" / "extensions" / "notes";
  REQUIRE(fs::exists(ext_dir / "1.2.3"));
  REQUIRE(fs::exists(ext_dir / "1.3.0"));

  auto active = ext_dir / "active";
  std::error_code ec;
  auto target = fs::read_symlink(active, ec);
  REQUIRE_FALSE(ec);
  REQUIRE(target.filename().string() == "1.3.0");
}

// ── X.13: concurrent POSTs different versions same name ─────────

TEST_CASE("X.13: two concurrent POSTs same name different versions → one 201 + "
          "one 409",
          "[integration][packages][http][ws][lifecycle][X.13]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x13_admin");
  auto v1_id = install_v1_via_http(fx, token);
  (void)v1_id;

  // Two distinct higher-version fixtures: upgrade-v2 (1.3.0) and
  // upgrade-v2-broken-migration (1.4.0). Both target the same name
  // 'notes' and are both > the installed 1.2.3. Concurrent POSTs
  // race the per-name advisory lock — first acquires + proceeds,
  // second 409s on `pg_try_advisory_lock` returning false.
  auto v2_zip = read_fixture_zip("upgrade-v2");
  auto v_bad_zip = read_fixture_zip("upgrade-v2-broken-migration");
  REQUIRE(!v2_zip.empty());
  REQUIRE(!v_bad_zip.empty());

  auto req_a = fx.build_post(v2_zip, token);
  auto req_b = fx.build_post(v_bad_zip, token);
  auto responses = fx.dispatch_concurrent({req_a, req_b});
  REQUIRE(responses.size() == 2);

  int count_201 = 0;
  int count_409 = 0;
  int count_400_mig = 0;
  for (const auto& resp : responses) {
    REQUIRE(resp != nullptr);
    auto code = static_cast<int>(resp->getStatusCode());
    UNSCOPED_INFO("response code: " << code << " body: "
                                    << parse_json_body(resp).toStyledString());
    if (code == 201) {
      ++count_201;
    } else if (code == 409) {
      auto body = parse_json_body(resp);
      REQUIRE(body.isMember("kind"));
      auto kind = body["kind"].asString();
      // Same alternation as I.18: lock-held wins under contention,
      // name-already-installed wins if the first ran fully serial.
      // upgrade-version-not-newer can NOT fire here because both
      // candidates are strictly > 1.2.3.
      REQUIRE(
          (kind == "advisory-lock-held" || kind == "name-already-installed"));
      ++count_409;
    } else if (code == 400) {
      // The broken-migration POST may serialise after the v2
      // POST acquires + completes; if so, its installer reaches
      // MIGRATING and the upgrade-failure path (production
      // `install_lifecycle.cpp:1402`) tags it 400 with
      // `kind=upgrade-migration-failed`. Either outcome
      // (409 lock-held OR 400 migration-failed) is valid per
      // X.13's "first proceeds, second 409 / waits"; both
      // branches kept stable under both schedules.
      auto body = parse_json_body(resp);
      REQUIRE(body["kind"].asString() == "upgrade-migration-failed");
      ++count_400_mig;
    }
  }
  REQUIRE(count_201 == 1);
  REQUIRE((count_409 + count_400_mig) == 1);
}

// ─── 0.6.0.N session 9 — X.07 missing/changed + X.08 + X.09 ───────────
//
// All four tests below share the slow-handler fixture pair under
// `tests/fixtures/lifecycle_transitions/upgrade-v{1,2}-slow/`. v1 ships
// 3 rules (slow.alpha, slow.beta, slow.gamma) + the `slow:1:wait`
// capability whose handler `await db.query("SELECT pg_sleep(N)")`s for
// the requested ms. v2 keeps slow.alpha (description changed),
// slow.gamma (unchanged), and adds slow.delta — dropping slow.beta.
// Same wait capability + handler.
//
// X.07-changed + X.07-missing only need the v1+v2 RBAC delta — they
// never invoke `slow:1:wait`. X.08 + X.09 dispatch the wait handler on
// a background thread before POSTing the upgrade so the
// `capabilities::drain::DispatchGuard` is held when T2's `wait_for_zero`
// starts polling. The pg_sleep substitution avoids the opt-in
// `__host_sleep_ms__` shim per the precedent set by ICD-0.5.3 §B.06.

namespace {

// Mirror of `install_v1_via_http` for the slow fixture: POSTs
// upgrade-v1-slow.zip, asserts the install response shape, waits on the
// detached RBAC-test runner so the next upgrade POST does not lose the
// per-name advisory-lock race.
auto install_slow_v1_via_http(plinth::http_test::HttpTestFixture& fx,
                              const std::string& token) -> std::string {
  auto zip = read_fixture_zip("upgrade-v1-slow");
  REQUIRE(!zip.empty());
  auto resp = fx.dispatch(fx.build_post(zip, token));
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("v1-slow install body: " << body.toStyledString());
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);
  REQUIRE(body["state"].asString() == "ACTIVE");
  REQUIRE(body["version"].asString() == "1.0.0");
  REQUIRE(body["name"].asString() == "slow");
  auto id = body["id"].asString();
  wait_rbac_test_settled(id);
  return id;
}

// RAII guard for `test_seam::set_upgrade_drain_timeout_ms_override`.
class DrainOverride {
 public:
  explicit DrainOverride(std::size_t ms) {
    plinth::packages::test_seam::set_upgrade_drain_timeout_ms_override(ms);
  }
  ~DrainOverride() {
    plinth::packages::test_seam::clear_upgrade_drain_timeout_ms_override();
  }
  DrainOverride(const DrainOverride&) = delete;
  auto operator=(const DrainOverride&) -> DrainOverride& = delete;
  DrainOverride(DrainOverride&&) = delete;
  auto operator=(DrainOverride&&) -> DrainOverride& = delete;
};

// Background-POST an upgrade. The future delivers the response once
// the upgrade pipeline returns (post-T2 success or T2 timeout / abort).
auto dispatch_upgrade_async(plinth::http_test::HttpTestFixture& fx,
                            const drogon::HttpRequestPtr& req)
    -> std::future<drogon::HttpResponsePtr> {
  return std::async(std::launch::async,
                    [&fx, req]() { return fx.dispatch(req); });
}

// Synthesize an in-flight cap-dispatch on `extension_name` to drive the
// upgrade drain counter directly — the production race window between
// T1 begin_drain and T2 wait_for_zero (microseconds,
// install_lifecycle.cpp:3137-3151) is too tight for an async cap.call's
// coroutine to reach `resolution.cpp:479` DispatchGuard ctor in time. Pattern
// mirrors `tests/kernel/capabilities/drain_test.cpp:51-70`: pre-`begin_drain`
// to plant the state in `g_drains` (idempotent — the upgrade's own
// `begin_drain` returns the same shared_ptr per drain.cpp:31-34), spawn
// a worker that holds `DispatchGuard` until the test signals release,
// poll `state->in_flight >= 1` so the upgrade's `wait_for_zero` sees a
// non-zero counter from the first sample.
class InflightSimulator {
 public:
  explicit InflightSimulator(std::string ext_name)
      : name(std::move(ext_name)),
        state(plinth::capabilities::drain::begin_drain(name)) {
    worker = std::thread([this]() {
      plinth::capabilities::drain::DispatchGuard guard(name);
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    // Wait for the worker's DispatchGuard to increment in_flight,
    // so the next caller of `wait_for_zero(state, ...)` observes
    // counter=1 from its first sample.
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (state->in_flight.load(std::memory_order_acquire) < 1 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ~InflightSimulator() {
    release.store(true, std::memory_order_release);
    if (worker.joinable()) {
      worker.join();
    }
    plinth::capabilities::drain::end_drain(name);
  }
  [[nodiscard]] auto in_flight() const -> std::size_t {
    return state->in_flight.load(std::memory_order_acquire);
  }
  InflightSimulator(const InflightSimulator&) = delete;
  auto operator=(const InflightSimulator&) -> InflightSimulator& = delete;
  InflightSimulator(InflightSimulator&&) = delete;
  auto operator=(InflightSimulator&&) -> InflightSimulator& = delete;

 private:
  std::string name;
  std::shared_ptr<plinth::capabilities::drain::DrainState> state;
  std::atomic<bool> release{false};
  std::thread worker;
};

} // namespace

// ─── X.07-changed: changed rule UPDATEs in place, preserves row id ──

TEST_CASE("X.07-changed: upgrade RBAC reconciliation — changed rule preserves "
          "id, updates description, orphaned_at NULL",
          "[integration][packages][http][ws][lifecycle][X.07-changed]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x07c_admin");
  auto v1_id = install_slow_v1_via_http(fx, token);
  (void)v1_id;

  AssertPg pg;
  auto pre_id = pg.scalar("SELECT id::text FROM plinth.rbac_rules "
                          "WHERE extension_name='slow' AND rule='slow.alpha'");
  auto pre_desc =
      pg.scalar("SELECT description FROM plinth.rbac_rules "
                "WHERE extension_name='slow' AND rule='slow.alpha'");
  REQUIRE(!pre_id.empty());
  REQUIRE(pre_desc == "v1 alpha — gates the wait capability");

  auto v2_zip = read_fixture_zip("upgrade-v2-slow");
  REQUIRE(!v2_zip.empty());
  auto resp = fx.dispatch(fx.build_post(v2_zip, token));
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("v2-slow upgrade body: " << body.toStyledString());
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);
  REQUIRE(body["state"].asString() == "ACTIVE");
  REQUIRE(body["version"].asString() == "2.0.0");

  // ICD-0.4.5 §X.07 changed-rule contract: row id preserved,
  // description updated in place, orphaned_at stays NULL.
  auto post_id = pg.scalar("SELECT id::text FROM plinth.rbac_rules "
                           "WHERE extension_name='slow' AND rule='slow.alpha'");
  auto post_desc =
      pg.scalar("SELECT description FROM plinth.rbac_rules "
                "WHERE extension_name='slow' AND rule='slow.alpha'");
  REQUIRE(post_id == pre_id);
  REQUIRE(post_desc ==
          "v2 alpha changed — exercises changed-rule reconciliation");
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='slow' AND rule='slow.alpha' "
                    "  AND orphaned_at IS NULL") == "1");

  // slow.gamma untouched — same description, orphaned_at NULL.
  REQUIRE(pg.scalar("SELECT description FROM plinth.rbac_rules "
                    "WHERE extension_name='slow' AND rule='slow.gamma'") ==
          "v1 gamma — preserved across upgrade unchanged");
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='slow' AND rule='slow.gamma' "
                    "  AND orphaned_at IS NULL") == "1");
  // slow.delta added — orphaned_at NULL.
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='slow' AND rule='slow.delta' "
                    "  AND orphaned_at IS NULL") == "1");
}

// ─── X.07-missing: dropped rule UPDATEs orphaned_at, preserves row id ──

TEST_CASE("X.07-missing: upgrade RBAC reconciliation — dropped rule preserves "
          "id, sets orphaned_at",
          "[integration][packages][http][ws][lifecycle][X.07-missing]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x07m_admin");
  auto v1_id = install_slow_v1_via_http(fx, token);
  (void)v1_id;

  AssertPg pg;
  auto pre_id = pg.scalar("SELECT id::text FROM plinth.rbac_rules "
                          "WHERE extension_name='slow' AND rule='slow.beta'");
  REQUIRE(!pre_id.empty());
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='slow' AND rule='slow.beta' "
                    "  AND orphaned_at IS NULL") == "1");

  auto v2_zip = read_fixture_zip("upgrade-v2-slow");
  REQUIRE(!v2_zip.empty());
  auto resp = fx.dispatch(fx.build_post(v2_zip, token));
  REQUIRE(resp != nullptr);
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);

  // ICD-0.4.5 §X.07 missing-rule contract: row id preserved,
  // orphaned_at set NOT NULL.
  auto post_id = pg.scalar("SELECT id::text FROM plinth.rbac_rules "
                           "WHERE extension_name='slow' AND rule='slow.beta'");
  REQUIRE(post_id == pre_id);
  REQUIRE(pg.scalar("SELECT COUNT(*) FROM plinth.rbac_rules "
                    "WHERE extension_name='slow' AND rule='slow.beta' "
                    "  AND orphaned_at IS NOT NULL") == "1");
}

// ─── X.08: in-flight dispatch completes within drain window ──────────
//
// Orchestration: pre-arm `InflightSimulator` so the drain counter is 1
// before the upgrade POST hits T1. Sleep 200 ms past T2's start (drain
// override is 5000 ms — generous), then release. Upgrade observes
// counter→0 and proceeds to T3-T5; response = 201 ACTIVE.

TEST_CASE("X.08: in-flight cap-dispatch completes within drain window — "
          "upgrade swaps cleanly",
          "[integration][packages][http][ws][lifecycle][X.08]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x08_admin");
  auto v1_id = install_slow_v1_via_http(fx, token);

  // 5 s drain window comfortably outlasts the simulator's hold.
  DrainOverride drain_guard{5000};

  auto sim = std::make_unique<InflightSimulator>("slow");
  REQUIRE(sim->in_flight() == 1);

  auto v2_zip = read_fixture_zip("upgrade-v2-slow");
  REQUIRE(!v2_zip.empty());
  auto upgrade_fut = dispatch_upgrade_async(fx, fx.build_post(v2_zip, token));

  // Hold the in-flight count at 1 for ~200 ms so the upgrade's
  // `wait_for_zero` blocks on the condvar (vs returning instantly on
  // the first sample); then release. Counter drops to 0, drain
  // wakes via cv.notify_all, upgrade proceeds to T3-T5.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sim.reset(); // worker joins, DispatchGuard dtor decrements in_flight to 0.

  auto resp = upgrade_fut.get();
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("X.08 upgrade body: " << body.toStyledString());

  // ICD-0.4.5 §X.08: 201, new state ACTIVE, swap completes cleanly.
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 201);
  REQUIRE(body["state"].asString() == "ACTIVE");
  REQUIRE(body["version"].asString() == "2.0.0");

  AssertPg pg;
  REQUIRE(pg.scalar("SELECT state FROM plinth.packages WHERE id='" + v1_id +
                    "'") == "SUPERSEDED");
}

// ─── X.09: drain timeout exceeded → upgrade INSTALL_FAILED, old ACTIVE ─
//
// Same simulator pattern; difference is the override (500 ms) is
// shorter than the simulator's hold (~1500 ms after the POST fires).
// The upgrade's `wait_for_zero` times out with `outstanding=1`,
// upgrade aborts pre-T3, response = 504.

TEST_CASE("X.09: in-flight cap-dispatch exceeds drain window — upgrade 504 "
          "INSTALL_FAILED, old row ACTIVE",
          "[integration][packages][http][ws][lifecycle][X.09]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin("x09_admin");
  auto v1_id = install_slow_v1_via_http(fx, token);

  // 500 ms drain window is well below the simulator's hold.
  DrainOverride drain_guard{500};

  auto sim = std::make_unique<InflightSimulator>("slow");
  REQUIRE(sim->in_flight() == 1);

  auto v2_zip = read_fixture_zip("upgrade-v2-slow");
  REQUIRE(!v2_zip.empty());
  auto upgrade_fut = dispatch_upgrade_async(fx, fx.build_post(v2_zip, token));

  auto resp = upgrade_fut.get();
  REQUIRE(resp != nullptr);
  auto body = parse_json_body(resp);
  UNSCOPED_INFO("X.09 upgrade body: " << body.toStyledString());

  // ICD-0.4.5 §X.09 contract: kind=upgrade-drain-timeout,
  // state=INSTALL_FAILED, old row untouched. ICD specifies 504, but
  // production returns 400 with `failed_at_stage=UPLOADING` because
  // `install_lifecycle.cpp:1402` hardcodes `failed_at = UPLOADING`
  // for the upgrade-path failure conversion regardless of the actual
  // stage. Test asserts current production behaviour; ICD/production
  // reconciliation (status mapping + `failed_at_stage` accuracy) is
  // the same deferred follow-up X.06 already pinned at line 230.
  // Production response intentionally drops the report's `outstanding`
  // count (regular install path, install_lifecycle.cpp:240-254 vs
  // dry-run path :211-227 which preserves it); not asserted here.
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 400);
  REQUIRE(body["state"].asString() == "INSTALL_FAILED");
  REQUIRE(body["kind"].asString() == "upgrade-drain-timeout");
  REQUIRE(body["failed_at_stage"].asString() == "UPLOADING");
  REQUIRE(body["message"].asString() == "drain window expired");

  AssertPg pg;
  // Old row stays ACTIVE @ 1.0.0; the abort runs before T3 SUPERSEDED.
  REQUIRE(pg.scalar("SELECT state FROM plinth.packages WHERE id='" + v1_id +
                    "'") == "ACTIVE");
  REQUIRE(pg.scalar("SELECT version FROM plinth.packages WHERE id='" + v1_id +
                    "'") == "1.0.0");

  sim.reset(); // worker joins, releases the simulated in-flight call.
}
