// ICD-0.6.2 §12.1 — `/api/frontend/tokens.css` indirection (B.* family).
//
// Library-level coverage of the `plinth::frontend::api_frontend` 302
// indirection handler. Tests dispatch through `test_seam::dispatch_tokens_css`
// directly, bypassing Drogon's listener (mirrors the
// `tests/kernel/shell/active_frontend_test.cpp` pattern that avoids the
// trantor TimerQueue teardown race documented in
// `project_ws_flaky_segfault.md`). PG access is required for the
// LIMIT 2 active-frontend lookup.
//
// Implementation deviation from ICD §12.1 B.02: the ICD enumerates
// "follow the 302 → expect 200 + text/css + body contains :root {".
// Following the redirect requires the asset server's per-(name, version)
// route registration which is set up by the install_lifecycle path —
// expensive setup for these tests. The byte-serving path is covered
// by `tests/kernel/packages/asset_server_test.cpp`. B.02 here verifies
// URL construction with a synthetic name+version pair, which is the
// only kernel-side contribution of the api_frontend handler. Recorded
// in §17 amendment block.

#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/frontend/api_frontend.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/shell/firstboot.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

[[nodiscard]] auto pg_available() -> bool {
  return std::getenv("PLINTH_PG_HOST") != nullptr;
}

[[nodiscard]] auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* h = std::getenv("PLINTH_PG_HOST")) {
    db.host = h;
  }
  if (auto* p = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<uint16_t>(std::stoi(p));
  }
  if (auto* u = std::getenv("PLINTH_PG_USER")) {
    db.user = u;
  }
  if (auto* w = std::getenv("PLINTH_PG_PASSWORD")) {
    db.password = w;
  }
  if (auto* d = std::getenv("PLINTH_PG_DATABASE")) {
    db.database = d;
  }
  return db;
}

[[nodiscard]] auto conninfo_of(const plinth::Config::Database& db)
    -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

auto drop_all_ext_schemas(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return;
  }
  PGresult* res = PQexec(
      conn, "SELECT nspname FROM pg_namespace WHERE nspname LIKE 'ext_%'");
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int n = PQntuples(res);
    for (int i = 0; i < n; ++i) {
      std::string s{PQgetvalue(res, i, 0)};
      std::string sql = "DROP SCHEMA IF EXISTS " + s + " CASCADE";
      PQclear(PQexec(conn, sql.c_str()));
    }
  }
  PQclear(res);
  PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  PQfinish(conn);
}

auto pg_exec(const plinth::Config::Database& db, const std::string& sql)
    -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* res = PQexec(conn, sql.c_str());
  REQUIRE(PQresultStatus(res) == PGRES_COMMAND_OK);
  PQclear(res);
  PQfinish(conn);
}

std::atomic<uint64_t> g_af_test_counter{0};

// Fixture: bootstraps PG + groups + bundled-shell first-boot. Each
// test starts from an idempotently-installed bundled shell so a single
// ACTIVE row exists. Tests that need 0-row or >1-row scenarios mutate
// the row(s) explicitly.
struct ApiFrontendScratch {
  plinth::Config::Database db;
  plinth::Config cfg;
  fs::path base;
  plinth::packages::InstallerContext ctx;

  ApiFrontendScratch() {
    db = pg_config();
    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_af_test_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_apif_" + id);
    fs::create_directories(base / "data");
    fs::create_directories(base / "staging");

    drop_all_ext_schemas(db);
    auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
    plinth::db::bootstrap_schema(db, migrations_dir, /*dev_mode=*/true);
    plinth::groups::bootstrap_groups(db);

    ctx.db = db;
    ctx.caller_user_id = "";
    ctx.data_dir = base / "data";
    ctx.staging_dir = base / "staging";
    ctx.max_package_size_bytes = 50ULL * 1024ULL * 1024ULL;

    cfg.db = db;
    cfg.shell.bundle_path =
        std::string{CMAKE_BINARY_DIR} + "/share/plinth/bundled";

    REQUIRE(
        plinth::shell::ensure_bundled_shell_installed(cfg, ctx).has_value());
  }
  ~ApiFrontendScratch() {
    std::error_code ec;
    fs::remove_all(base, ec);
    drop_all_ext_schemas(db);
  }
  ApiFrontendScratch(const ApiFrontendScratch&) = delete;
  auto operator=(const ApiFrontendScratch&) -> ApiFrontendScratch& = delete;
  ApiFrontendScratch(ApiFrontendScratch&&) = delete;
  auto operator=(ApiFrontendScratch&&) -> ApiFrontendScratch& = delete;
};

struct Captured {
  bool received{false};
  drogon::HttpStatusCode status{drogon::k200OK};
  std::string body;
  std::string location;
  std::string cache_control;
  drogon::ContentType content_type{drogon::CT_NONE};
};

auto capture(const plinth::Config::Database& db) -> Captured {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath("/api/frontend/tokens.css");
  Captured out;
  plinth::frontend::test_seam::dispatch_tokens_css(
      req,
      [&out](const drogon::HttpResponsePtr& resp) {
        out.received = true;
        if (!resp) {
          return;
        }
        out.status = resp->statusCode();
        out.body = std::string{resp->body()};
        out.location = resp->getHeader("Location");
        out.cache_control = resp->getHeader("Cache-Control");
        out.content_type = resp->getContentType();
      },
      db);
  return out;
}

} // namespace

// ── B.01 — happy path: single ACTIVE shell → 302 with Location ───────

TEST_CASE("B.01: GET /api/frontend/tokens.css returns 302 to "
          "/ext/{name}/{version}/css/tokens.css with Cache-Control: no-cache",
          "[pg][api_frontend][B.01]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  ApiFrontendScratch s;

  auto r = capture(s.db);
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k302Found);
  REQUIRE(r.location == "/ext/shell/0.6.3/css/tokens.css");
  REQUIRE(r.cache_control == "no-cache");
  REQUIRE(r.body.empty());
}

// ── B.02 — URL construction with custom name + version ───────────────
//
// Replaces the bundled shell's row with a synthetic ACTIVE row using a
// different name/version pair, verifying the redirect URL is built
// from the row's name + version (not hardcoded "shell" / "0.6.2").

TEST_CASE("B.02: redirect URL is constructed from active row's name + version",
          "[pg][api_frontend][B.02]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  ApiFrontendScratch s;

  // Replace the bundled-shell row with a synthetic ACTIVE row at
  // name=demo, version=1.2.3, mount=/console (different from /app
  // to dodge `uniq_packages_mount_active` if any retired bundled
  // rows linger).
  pg_exec(s.db,
          "UPDATE plinth.packages SET state='SUPERSEDED' WHERE name='shell'");
  pg_exec(s.db,
          "INSERT INTO plinth.packages "
          "(name, version, state, provenance, manifest_json, "
          " frontend_mount, frontend_entry, entry_point, manifest_checksum) "
          "VALUES ('demo', '1.2.3', 'ACTIVE', 'user', "
          " '{\"name\":\"demo\",\"version\":\"1.2.3\"}'::jsonb, "
          " '/console', 'index.html', 'server/main.js', 'sha256-fake')");

  auto r = capture(s.db);
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k302Found);
  REQUIRE(r.location == "/ext/demo/1.2.3/css/tokens.css");
  REQUIRE(r.cache_control == "no-cache");
}

// ── B.03 — no_active_frontend 503 ────────────────────────────────────

TEST_CASE("B.03: no ACTIVE frontend → 503 with no_active_frontend body",
          "[pg][api_frontend][B.03]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  ApiFrontendScratch s;

  // Take the bundled shell out of the resolver's WHERE clause by
  // moving it to SUPERSEDED state. The unique partial index
  // `uniq_packages_mount_active` only constrains ACTIVE/ACTIVE_FLAGGED,
  // so this is safe.
  pg_exec(s.db,
          "UPDATE plinth.packages SET state='SUPERSEDED' WHERE name='shell'");

  auto r = capture(s.db);
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k503ServiceUnavailable);
  REQUIRE(r.cache_control == "no-cache");
  REQUIRE(r.content_type == drogon::CT_APPLICATION_JSON);
  auto body = nlohmann::json::parse(r.body);
  REQUIRE(body["error"] == "no_active_frontend");
  REQUIRE(body["code"] == 503);
  REQUIRE(body["message"].is_string());
}

// ── B.04 — multiple_active_frontends 503 ─────────────────────────────

TEST_CASE(
    "B.04: two ACTIVE frontends → 503 with multiple_active_frontends body",
    "[pg][api_frontend][B.04]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  ApiFrontendScratch s;

  // Insert a second ACTIVE row with a different name + mount; the
  // partial unique indexes constrain (name) and (frontend_mount)
  // independently, so this is permitted by schema. The resolver's
  // LIMIT 2 returns both rows → multiple_active_frontends.
  pg_exec(s.db,
          "INSERT INTO plinth.packages "
          "(name, version, state, provenance, manifest_json, "
          " frontend_mount, frontend_entry, entry_point, manifest_checksum) "
          "VALUES ('admin', '0.0.1', 'ACTIVE', 'user', "
          " '{\"name\":\"admin\",\"version\":\"0.0.1\"}'::jsonb, "
          " '/admin', 'index.html', 'server/main.js', 'sha256-fake')");

  auto r = capture(s.db);
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k503ServiceUnavailable);
  REQUIRE(r.cache_control == "no-cache");
  REQUIRE(r.content_type == drogon::CT_APPLICATION_JSON);
  auto body = nlohmann::json::parse(r.body);
  REQUIRE(body["error"] == "multiple_active_frontends");
  REQUIRE(body["code"] == 503);
  REQUIRE(body["message"].is_string());
}

// ── ICD-0.6.3 §12.1 B.04 — `/api/frontend/sdk.js` 302 round-trip ────
//
// Sibling indirection to `/api/frontend/tokens.css`; same handler shape
// (per ICD-0.6.3 §5.2), only the redirect target suffix differs. The
// test inserts a synthetic ACTIVE row to exercise URL construction
// against a known name + version (mirrors the §12.1 deviation noted at
// the top of this file — covers URL construction, not byte-serving).

namespace {

auto capture_sdk_js(const plinth::Config::Database& db) -> Captured {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath("/api/frontend/sdk.js");
  Captured out;
  plinth::frontend::test_seam::dispatch_sdk_js(
      req,
      [&out](const drogon::HttpResponsePtr& resp) {
        out.received = true;
        if (!resp) {
          return;
        }
        out.status = resp->statusCode();
        out.body = std::string{resp->body()};
        out.location = resp->getHeader("Location");
        out.cache_control = resp->getHeader("Cache-Control");
        out.content_type = resp->getContentType();
      },
      db);
  return out;
}

} // namespace

TEST_CASE("B.04 (sdk.js): GET /api/frontend/sdk.js returns 302 to "
          "/ext/{name}/{version}/client/sdk.js with Cache-Control: no-cache",
          "[pg][api_frontend][B.04-sdk.js]") {
  if (!pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  ApiFrontendScratch s;

  // Replace the bundled-shell row with a synthetic ACTIVE row at
  // name=demo, version=1.2.3 (different from the bundled shell's
  // /app mount to dodge `uniq_packages_mount_active`).
  pg_exec(s.db,
          "UPDATE plinth.packages SET state='SUPERSEDED' WHERE name='shell'");
  pg_exec(s.db,
          "INSERT INTO plinth.packages "
          "(name, version, state, provenance, manifest_json, "
          " frontend_mount, frontend_entry, entry_point, manifest_checksum) "
          "VALUES ('demo', '1.2.3', 'ACTIVE', 'user', "
          " '{\"name\":\"demo\",\"version\":\"1.2.3\"}'::jsonb, "
          " '/console', 'index.html', 'server/main.js', 'sha256-fake')");

  auto r = capture_sdk_js(s.db);
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k302Found);
  REQUIRE(r.location == "/ext/demo/1.2.3/sdk.js");
  REQUIRE(r.cache_control == "no-cache");
  REQUIRE(r.body.empty());
}
