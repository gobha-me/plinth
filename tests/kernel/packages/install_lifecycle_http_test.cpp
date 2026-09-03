// HTTP integration tests for `POST /api/packages` (ICD-0.4.4).
//
// Built in 0.6.0.N session 2 alongside `http_test_fixture`. Closes:
//   I.18  — Concurrent POSTs for the same package name produce one 201
//           and one 409 advisory-lock-held.
//   I.19  — `?dry_run=1` validation-only path (added in 0.6.0.N session 7).
//           Three sub-cases:
//             I.19a happy: valid zip + dry_run → 200 + ValidationReport,
//                          no row in `plinth.packages`, no `ext_<n>` schema.
//             I.19b fail:  fail-validator zip + dry_run → 422 INSTALL_FAILED.
//             I.19c coll:  dry_run after a real install of the same name →
//                          409 (advisory-lock-held / name-already-installed /
//                          upgrade-version-not-newer, depending on
//                          disposition).
//   I.20  — Authenticated user without `packages.install` rule gets 403.
//
// **Test routing:** these cases are tagged `[ws]` to route to
// `plinth_tests_ws` rather than `plinth_tests_pg`. Same reason
// documented at `tests/kernel/realtime/broker_test.cpp:382-384` —
// `plinth_tests_pg` runs
// `tests/kernel/capabilities/dispatch_extension_test.cpp` which starts drogon
// via `async_bridge_fixture::ensure_drogon_with_db_running()` (no listener);
// routing the HTTP fixture's `start_test_server()` (which adds a listener and
// calls `createDbClient`) into the same subprocess fires drogon's `!running_`
// assertion when the second fixture's `createDbClient` runs after the first's
// `app().run()`. The `[ws]` tag steers our fixture into `plinth_tests_ws` where
// `ws_test_fixture` is the sole drogon starter. The `[ws]` tag here is
// routing-only — these are HTTP tests, not WebSocket tests.

#include "../ws/ws_test_fixture.hpp"
#include "http_test_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <json/reader.h>
#include <json/value.h>
#include <libpq-fe.h>

#include <string>
#include <string_view>
#include <utility>

namespace {

auto parse_json_body(const drogon::HttpResponsePtr& resp) -> Json::Value {
  auto body = std::string{resp->getBody()};
  Json::Value v;
  Json::CharReaderBuilder b;
  auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
  std::string errs;
  // Json::CharReader::parse requires (begin, end) char pointers — the
  // one-past-end is well-defined per std::string's contiguous-storage
  // guarantee.
  reader->parse(body.data(), body.data() + body.size(), &v, &errs);
  return v;
}

} // namespace

TEST_CASE("I.18: concurrent POSTs for same package name produce 201 + 409 "
          "advisory-lock-held",
          "[integration][packages][http][ws][I.18]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin();
  auto zip = plinth::http_test::HttpTestFixture::read_valid_install_zip();
  REQUIRE(!zip.empty());

  auto req_a = fx.build_post(zip, token);
  auto req_b = fx.build_post(zip, token);
  auto responses = fx.dispatch_concurrent({req_a, req_b});
  REQUIRE(responses.size() == 2);

  int count_201 = 0;
  int count_409_lock = 0;
  for (const auto& resp : responses) {
    REQUIRE(resp != nullptr);
    auto code = static_cast<int>(resp->getStatusCode());
    if (code == 201) {
      ++count_201;
    } else if (code == 409) {
      auto body = parse_json_body(resp);
      // The 409 is keyed by `kind` in the install_failed body
      // (see handlers.cpp:166-176 — INSTALL_FAILED → kind /
      // failed_at_stage payload). The first-arrival-wins path
      // returns kind=advisory-lock-held when the second POST
      // reaches handle_post_packages while the first holds the
      // pg_try_advisory_lock.
      //
      // The OTHER 409 codepaths that can fire here are
      // `name-already-installed` (first install completed before
      // the second's POST started; serial outcome) and
      // `upgrade-version-not-newer` (first install completed AND
      // the disposition classifier sees the ACTIVE row at the
      // same version, so the second is treated as an upgrade
      // candidate that fails the strictly-newer check). All
      // three are valid same-name-collision outcomes; the
      // barrier in dispatch_concurrent makes the lock-held
      // path the dominant outcome under load. Widened in
      // 0.6.0.N session 7 after I.19 verification surfaced the
      // upgrade-version-not-newer race window in ~10% of runs.
      UNSCOPED_INFO("409 body: " << body.toStyledString());
      REQUIRE(body.isMember("kind"));
      auto kind = body["kind"].asString();
      REQUIRE((kind == "advisory-lock-held" ||
               kind == "name-already-installed" ||
               kind == "upgrade-version-not-newer"));
      ++count_409_lock;
    }
  }
  REQUIRE(count_201 == 1);
  REQUIRE(count_409_lock == 1);
}

namespace {

// Count of `plinth.packages` rows with the given name. Used by I.19 to
// assert no row persists on dry-run.
auto packages_row_count(plinth::ws_test::TestPg& pg, std::string_view name)
    -> int {
  auto res =
      pg.exec_params("SELECT count(*) FROM plinth.packages WHERE name = $1",
                     {std::string{name}});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return -1;
  }
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

auto schema_exists(plinth::ws_test::TestPg& pg, std::string_view schema_name)
    -> bool {
  auto res = pg.exec_params("SELECT count(*) FROM information_schema.schemata "
                            "WHERE schema_name = $1",
                            {std::string{schema_name}});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return false;
  }
  return std::stoi(PQgetvalue(res.get(), 0, 0)) > 0;
}

} // namespace

TEST_CASE("I.19a: dry_run with valid package returns 200 + validation report; "
          "no row persists",
          "[integration][packages][http][ws][I.19]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin();

  // ws_test_fixture's reset_schema only drops `plinth`; an `ext_notes`
  // schema lingering from a previous test (e.g. I.19c's real install)
  // would defeat the "no schema created" assertion below. Drop it
  // explicitly so the post-dry-run check is meaningful.
  plinth::ws_test::TestPg pg{plinth::ws_test::test_config().db};
  (void)pg.exec("DROP SCHEMA IF EXISTS ext_notes CASCADE");
  REQUIRE_FALSE(schema_exists(pg, "ext_notes"));

  auto zip =
      plinth::http_test::HttpTestFixture::read_install_zip("valid-install");
  REQUIRE(!zip.empty());

  auto req = fx.build_post(zip, token, "dry_run=1");
  auto resp = fx.dispatch(req);
  REQUIRE(resp != nullptr);
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 200);

  auto body = parse_json_body(resp);
  UNSCOPED_INFO("dry-run 200 body: " << body.toStyledString());
  REQUIRE(body["state"].asString() == "VALIDATING");
  REQUIRE(body["name"].asString() == "notes");
  REQUIRE(body["version"].asString() == "1.2.3");
  REQUIRE(body.isMember("validation_report"));
  REQUIRE(body["validation_report"].isObject());
  REQUIRE(body["validation_report"].isMember("disposition"));

  // No row in `plinth.packages`; no `ext_notes` schema (the dry-run
  // did not run MIGRATING, so 001_init.sql never executed).
  REQUIRE(packages_row_count(pg, "notes") == 0);
  REQUIRE_FALSE(schema_exists(pg, "ext_notes"));
}

TEST_CASE("I.19b: dry_run with fail-validator fixture returns 422 "
          "INSTALL_FAILED; no row persists",
          "[integration][packages][http][ws][I.19]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin();
  // fail-validator's manifest parses cleanly (so VALIDATING is reached);
  // its rbac.json carries an orphan namespace per the fixture README's
  // CF1 case, which the validator flags.
  auto zip =
      plinth::http_test::HttpTestFixture::read_install_zip("fail-validator");
  REQUIRE(!zip.empty());

  auto req = fx.build_post(zip, token, "dry_run=1");
  auto resp = fx.dispatch(req);
  REQUIRE(resp != nullptr);
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 422);

  auto body = parse_json_body(resp);
  UNSCOPED_INFO("dry-run 422 body: " << body.toStyledString());
  REQUIRE(body["state"].asString() == "INSTALL_FAILED");
  REQUIRE(body["failed_at_stage"].asString() == "VALIDATING");
  REQUIRE(body["kind"].asString() == "validation-errors");

  // Even on validation failure, dry-run leaves no row behind.
  plinth::ws_test::TestPg pg{plinth::ws_test::test_config().db};
  auto fixture_name =
      body["name"].isString() ? body["name"].asString() : std::string{};
  if (!fixture_name.empty()) {
    REQUIRE(packages_row_count(pg, fixture_name) == 0);
  }
}

TEST_CASE(
    "I.19c: dry_run after real install of same name returns 409 collision",
    "[integration][packages][http][ws][I.19]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_admin();
  auto zip =
      plinth::http_test::HttpTestFixture::read_install_zip("valid-install");
  REQUIRE(!zip.empty());

  // 1) Real install: assert 201.
  auto first_req = fx.build_post(zip, token);
  auto first_resp = fx.dispatch(first_req);
  REQUIRE(first_resp != nullptr);
  REQUIRE(static_cast<int>(first_resp->getStatusCode()) == 201);

  // 2) Dry-run install with same zip + same name: must collide before
  //    INSERT (advisory-lock-held / name-already-installed / upgrade-
  //    version-not-newer per the disposition the existing UPLOADING
  //    branch produces). All three are 409.
  plinth::ws_test::TestPg pg{plinth::ws_test::test_config().db};
  auto rows_before = packages_row_count(pg, "notes");
  REQUIRE(rows_before == 1); // only the row from step 1

  auto dr_req = fx.build_post(zip, token, "dry_run=1");
  auto dr_resp = fx.dispatch(dr_req);
  REQUIRE(dr_resp != nullptr);
  REQUIRE(static_cast<int>(dr_resp->getStatusCode()) == 409);

  auto body = parse_json_body(dr_resp);
  UNSCOPED_INFO("dry-run 409 body: " << body.toStyledString());
  REQUIRE(body.isMember("kind"));
  auto kind = body["kind"].asString();
  REQUIRE((kind == "advisory-lock-held" || kind == "name-already-installed" ||
           kind == "upgrade-version-not-newer"));

  // Dry-run did not create or remove any row.
  REQUIRE(packages_row_count(pg, "notes") == rows_before);
}

TEST_CASE("I.20: authenticated user without packages.install rule receives 403",
          "[integration][packages][http][ws][I.20]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  plinth::http_test::HttpTestFixture fx;
  auto token = fx.seed_non_admin();
  auto zip = plinth::http_test::HttpTestFixture::read_valid_install_zip();
  REQUIRE(!zip.empty());

  auto req = fx.build_post(zip, token);
  auto resp = fx.dispatch(req);
  REQUIRE(resp != nullptr);
  REQUIRE(static_cast<int>(resp->getStatusCode()) == 403);

  auto body = parse_json_body(resp);
  UNSCOPED_INFO("403 body: " << body.toStyledString());
  // RbacFilter emits {error: "permission_denied", rule: "packages.install",
  // message: "..."}
  REQUIRE(body["error"].asString() == "permission_denied");
  REQUIRE(body["rule"].asString() == "packages.install");
}
