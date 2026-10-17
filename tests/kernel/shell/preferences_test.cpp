// ICD-0.6.1 §12.4 — `ext_shell.user_preferences` schema + default-
// grant coverage (P.* test family, library-level subset).
//
// Scope of this file: schema integrity (CHECK + FK + UPSERT semantics)
// and default-grant application after the bundled-shell first-boot
// install. Full JS-handler / capability-dispatch round-trip cases
// (P.01 .. P.14 as enumerated in ICD §12.4) require the async-bridge
// fixture and the WS test client; that integration suite is deferred
// to a 0.6.1.N follow-up per the §17 OQ amendment block. The cases
// in this file cover the schema-level and RBAC-grant-level contracts
// the full suite assumes hold.

#include "../ws/ws_test_fixture.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/frontend/float_layout_service.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/generation_binding.hpp"
#include "kernel/packages/generation_capture.hpp"
#include "kernel/packages/generation_receipts.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/installed_generation_registry.hpp"
#include "kernel/packages/migrations_internal.hpp"
#include "kernel/packages/name_operation.hpp"
#include "kernel/packages/panels_manifest.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/shell/active_frontend.hpp"
#include "kernel/shell/firstboot.hpp"
#include "kernel/shell/platform_graph.hpp"

#include "../js/async_bridge_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <libpq-fe.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fs = std::filesystem;
namespace capabilities = plinth::capabilities;
namespace extensions = plinth::extensions;

static_assert(!std::is_constructible_v<
              plinth::extensions::ValidatedFloatCommit,
              plinth::extensions::PoolIdentity, Json::Value,
              std::unique_ptr<capabilities::drain::DispatchGuard>>);
static_assert(!std::is_constructible_v<
              plinth::extensions::FixedStorageCall,
              plinth::extensions::FixedStorageOperation,
              plinth::extensions::PoolIdentity, Json::Value,
              std::unique_ptr<capabilities::drain::DispatchGuard>>);

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

std::atomic<uint64_t> g_pf_scratch_counter{0};

// Fixture: bootstraps PG + groups + bundled-shell first-boot. Each
// test starts from an idempotently-installed bundled shell so the
// `ext_shell.user_preferences` table exists and the default rules
// are granted to the `users` group.
struct PrefScratch {
  plinth::Config::Database db;
  plinth::Config cfg;
  fs::path base;
  plinth::packages::InstallerContext ctx;

  PrefScratch() {
    db = pg_config();
    plinth::ws_test::settle_fixture_work();
    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_pf_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_pf_" + id);
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
    cfg.packages_data_dir = ctx.data_dir.string();
    cfg.packages_staging_dir = ctx.staging_dir.string();
    static_cast<void>(plinth::extensions::shutdown_registry());
    auto resolver = plinth::capabilities::init_resolver(db);
    REQUIRE(resolver.has_value());
    plinth::extensions::init_registry(cfg);
    REQUIRE(plinth::packages::rbac_test::start_async_workers());

    REQUIRE(
        plinth::shell::ensure_bundled_shell_installed(cfg, ctx).has_value());
    plinth::ws_test::settle_fixture_work();
    REQUIRE(plinth::packages::rbac_test::start_async_workers());
  }
  ~PrefScratch() {
    plinth::ws_test::settle_fixture_work_or_exit();
    plinth::packages::asset_server::cancel_all_registrations();
    static_cast<void>(plinth::extensions::shutdown_registry());
    plinth::capabilities::clear_resolver_for_test();
    std::error_code ec;
    fs::remove_all(base, ec);
    drop_all_ext_schemas(db);
  }
  PrefScratch(const PrefScratch&) = delete;
  auto operator=(const PrefScratch&) -> PrefScratch& = delete;
  PrefScratch(PrefScratch&&) = delete;
  auto operator=(PrefScratch&&) -> PrefScratch& = delete;
};

// Insert a user with a stable UUID so tests can reference rows by id.
[[nodiscard]] auto seed_user(const plinth::Config::Database& db,
                             const std::string& username) -> std::string {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  std::array<const char*, 1> values = {username.c_str()};
  PGresult* r =
      PQexecParams(conn,
                   "INSERT INTO plinth.users (username, password_hash) "
                   "VALUES ($1, 'unused-for-this-test') "
                   "RETURNING id::text",
                   1, nullptr, values.data(), nullptr, nullptr, 0);
  REQUIRE(PQresultStatus(r) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(r) == 1);
  std::string id = PQgetvalue(r, 0, 0);
  PQclear(r);
  PQfinish(conn);
  return id;
}

auto pg_run(const plinth::Config::Database& db, const std::string& sql)
    -> std::pair<int /*status*/, std::string /*err*/> {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string e = PQerrorMessage(conn);
    PQfinish(conn);
    return {-1, e};
  }
  PGresult* r = PQexec(conn, sql.c_str());
  int status = PQresultStatus(r);
  std::string err = PQresultErrorMessage(r);
  PQclear(r);
  PQfinish(conn);
  return {status, err};
}

} // namespace

// ── P.s.01 — Schema exists with PK + FK + CHECKs ────────────────────

TEST_CASE("P.s.01: ext_shell.user_preferences ships with PK, FK, and CHECK "
          "constraints",
          "[shell][preferences][integration][P.s.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* res =
      PQexec(conn, "SELECT conname FROM pg_constraint "
                   "WHERE conrelid = 'ext_shell.user_preferences'::regclass "
                   "ORDER BY conname");
  REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
  std::string names;
  int n = PQntuples(res);
  for (int i = 0; i < n; ++i) {
    if (i > 0) {
      names += ",";
    }
    names += PQgetvalue(res, i, 0);
  }
  PQclear(res);
  PQfinish(conn);

  // ICD §6.1: PK on (user_id, key), FK on user_id, key + value CHECKs.
  REQUIRE(names.find("user_preferences_pkey") != std::string::npos);
  REQUIRE(names.find("fk_user_preferences_user_id") != std::string::npos);
  REQUIRE(names.find("chk_key_nonempty") != std::string::npos);
  REQUIRE(names.find("chk_value_size") != std::string::npos);
}

// ── P.s.02 — Empty key rejected by chk_key_nonempty ─────────────────

TEST_CASE("P.s.02: empty key rejected at PG layer (chk_key_nonempty)",
          "[shell][preferences][integration][P.s.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;
  auto uid = seed_user(s.db, "p_s02_user");
  auto [status, err] = pg_run(
      s.db, "INSERT INTO ext_shell.user_preferences (user_id, key, value) "
            "VALUES ('" +
                uid + "', '', '\"x\"'::jsonb)");
  REQUIRE(status != PGRES_COMMAND_OK);
  REQUIRE(err.find("chk_key_nonempty") != std::string::npos);
}

// ── P.s.03 — Over-long key rejected by chk_key_nonempty (length cap) ─

TEST_CASE("P.s.03: key length > 255 rejected at PG layer",
          "[shell][preferences][integration][P.s.03]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;
  auto uid = seed_user(s.db, "p_s03_user");
  std::string long_key(256, 'k');
  auto [status, err] = pg_run(
      s.db, "INSERT INTO ext_shell.user_preferences (user_id, key, value) "
            "VALUES ('" +
                uid + "', '" + long_key + "', '\"x\"'::jsonb)");
  REQUIRE(status != PGRES_COMMAND_OK);
  REQUIRE(err.find("chk_key_nonempty") != std::string::npos);
}

// ── P.s.04 — 64 KiB cap at PG layer (chk_value_size) ────────────────

TEST_CASE("P.s.04: value > 64 KiB serialised rejected at PG layer",
          "[shell][preferences][integration][P.s.04]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;
  auto uid = seed_user(s.db, "p_s04_user");
  // 65537 bytes inside a JSON string (with quote chars + escapes the
  // serialised octet_length exceeds 65536).
  std::string huge(70000, 'x');
  auto [status, err] = pg_run(
      s.db, "INSERT INTO ext_shell.user_preferences (user_id, key, value) "
            "VALUES ('" +
                uid +
                "', 'big', "
                "        to_jsonb('" +
                huge + "'::text))");
  REQUIRE(status != PGRES_COMMAND_OK);
  REQUIRE(err.find("chk_value_size") != std::string::npos);
}

// ── P.s.05 — UPSERT round-trip preserves JSONB byte-identity ────────

TEST_CASE("P.s.05: UPSERT round-trips object/array values byte-identically",
          "[shell][preferences][integration][P.s.05]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;
  auto uid = seed_user(s.db, "p_s05_user");

  auto [s1, e1] = pg_run(
      s.db, "INSERT INTO ext_shell.user_preferences (user_id, key, value) "
            "VALUES ('" +
                uid +
                "', 'tabs', "
                "        '[\"notes\", \"files\", \"kb\"]'::jsonb) "
                "ON CONFLICT (user_id, key) DO UPDATE SET "
                "  value = EXCLUDED.value, updated_at = now()");
  REQUIRE(s1 == PGRES_COMMAND_OK);

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  std::string sql = "SELECT value::text FROM ext_shell.user_preferences "
                    "WHERE user_id = '" +
                    uid + "' AND key = 'tabs'";
  PGresult* r = PQexec(conn, sql.c_str());
  REQUIRE(PQresultStatus(r) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(r) == 1);
  REQUIRE(std::string{PQgetvalue(r, 0, 0)} == "[\"notes\", \"files\", \"kb\"]");
  PQclear(r);
  PQfinish(conn);
}

// ── P.s.06 — User deletion cascades to user_preferences ─────────────

TEST_CASE("P.s.06: ON DELETE CASCADE drops user_preferences when user removed",
          "[shell][preferences][integration][P.s.06]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;
  auto uid = seed_user(s.db, "p_s06_user");
  REQUIRE(pg_run(s.db,
                 "INSERT INTO ext_shell.user_preferences (user_id, key, value) "
                 "VALUES ('" +
                     uid +
                     "', 'a', '1'::jsonb), "
                     "       ('" +
                     uid + "', 'b', '\"two\"'::jsonb)")
              .first == PGRES_COMMAND_OK);

  REQUIRE(
      pg_run(s.db, "DELETE FROM plinth.users WHERE id = '" + uid + "'").first ==
      PGRES_COMMAND_OK);

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  std::string sql = "SELECT COUNT(*) FROM ext_shell.user_preferences "
                    "WHERE user_id = '" +
                    uid + "'";
  PGresult* r = PQexec(conn, sql.c_str());
  REQUIRE(PQresultStatus(r) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(r, 0, 0)} == "0");
  PQclear(r);
  PQfinish(conn);
}

// ── P.r.01 — Default grants applied to users group at first-boot ────

TEST_CASE(
    "P.r.01: shell.preferences.read|write granted to users group at install",
    "[shell][preferences][rbac][integration][P.r.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);

  // All three rules MUST be in plinth.rbac_rules under
  // extension_name='shell'. v0.6.3 added `shell.audit.emit` for the
  // boundary-audit emission path.
  PGresult* r = PQexec(conn, "SELECT rule FROM plinth.rbac_rules "
                             "WHERE extension_name = 'shell' "
                             "ORDER BY rule");
  REQUIRE(PQresultStatus(r) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(r) == 3);
  REQUIRE(std::string{PQgetvalue(r, 0, 0)} == "shell.audit.emit");
  REQUIRE(std::string{PQgetvalue(r, 1, 0)} == "shell.preferences.read");
  REQUIRE(std::string{PQgetvalue(r, 2, 0)} == "shell.preferences.write");
  PQclear(r);

  // The `everyone` group must have all three rules granted via group_rules.
  PGresult* g =
      PQexec(conn, "SELECT rl.rule FROM plinth.group_rules gr "
                   "JOIN plinth.groups gp ON gp.id = gr.group_id "
                   "JOIN plinth.rbac_rules rl ON rl.id = gr.rule_id "
                   "WHERE gp.name = 'everyone' AND rl.extension_name = 'shell' "
                   "ORDER BY rl.rule");
  REQUIRE(PQresultStatus(g) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(g) == 3);
  REQUIRE(std::string{PQgetvalue(g, 0, 0)} == "shell.audit.emit");
  REQUIRE(std::string{PQgetvalue(g, 1, 0)} == "shell.preferences.read");
  REQUIRE(std::string{PQgetvalue(g, 2, 0)} == "shell.preferences.write");
  PQclear(g);

  PQfinish(conn);
}

// ── P.r.02 — Default-grant idempotency on second firstboot pass ────

TEST_CASE("P.r.02: re-running firstboot keeps users-group grants idempotent",
          "[shell][preferences][rbac][integration][P.r.02]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;

  // The PrefScratch ctor already ran firstboot once. A second call
  // hits the EXACTLY_ONE short-circuit path; group_rules row count
  // for shell rules must stay at 3 (v0.6.3 added shell.audit.emit).
  auto fb = plinth::shell::ensure_bundled_shell_installed(s.cfg, s.ctx);
  REQUIRE(fb.has_value());

  PGconn* conn = PQconnectdb(conninfo_of(s.db).c_str());
  REQUIRE(PQstatus(conn) == CONNECTION_OK);
  PGresult* g = PQexec(conn, "SELECT COUNT(*) FROM plinth.group_rules gr "
                             "JOIN plinth.rbac_rules rl ON rl.id = gr.rule_id "
                             "WHERE rl.extension_name = 'shell'");
  REQUIRE(PQresultStatus(g) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(g, 0, 0)} == "3");
  PQclear(g);
  PQfinish(conn);
}

// ── P.h.01 — Handler files ship + are deployed to data_dir ──────────

TEST_CASE("P.h.01: bundled-shell deploys preferences.{get,set,get_all}.js "
          "handlers to data_dir",
          "[shell][preferences][integration][P.h.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  PrefScratch s;

  // Bundled-shell version tracks the manifest's "version" field; bumps
  // each milestone (0.6.1 → 0.6.2 → …). Resolve dynamically from the
  // installed extension dir so this test does not need a manual edit
  // on every shell version bump.
  auto extensions_root = s.ctx.data_dir / "extensions" / "shell";
  REQUIRE(fs::is_directory(extensions_root));
  fs::path version_dir;
  for (const auto& entry : fs::directory_iterator(extensions_root)) {
    if (entry.is_directory() && entry.path().filename() != "active") {
      version_dir = entry.path();
      break;
    }
  }
  REQUIRE(!version_dir.empty());
  auto handlers_dir = version_dir / "server" / "handlers";
  REQUIRE(fs::is_regular_file(handlers_dir / "preferences.get.js"));
  REQUIRE(fs::is_regular_file(handlers_dir / "preferences.set.js"));
  REQUIRE(fs::is_regular_file(handlers_dir / "preferences.get_all.js"));
}

// ── P.dispatch.01 — Bundled-shell preferences round-trip via dispatch ─
//
// 0.6.3.N — End-to-end proof that the runtime_registry wrapper now
// passes a populated `ctx` to the bundled shell handlers. Pre-fix,
// `preferences.get`/`set`/`get_all` rejected with `cap.handler_threw`
// (TypeError reading `ctx.user.id`) because the wrapper called
// `__mod.default(__handler_args)` with one positional. This test
// drives a complete GET-missing → SET → GET-back round-trip via
// `extensions::dispatch` against the bundled shell installed by
// `PrefScratch`, which only succeeds if the handler reads
// `ctx.user.id` correctly to address the `ext_shell.user_preferences`
// row.

TEST_CASE("P.dispatch.01: bundled-shell preferences round-trip via "
          "extensions::dispatch",
          "[shell][preferences][integration][P.dispatch.01]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }

  // Need Drogon + DbClient running so the JS db.* binding has a real
  // client to drive `db.query` against. Same precondition as
  // tests/kernel/capabilities/dispatch_extension_test.cpp's
  // ExtScratch.
  plinth::async_bridge_test::ensure_drogon_with_db_running();

  PrefScratch s;
  auto uid = seed_user(s.db, "p_dispatch_01_alice");

  // session_id is UUID-typed in plinth.audit_log — leave empty so
  // the audit insert binds NULL rather than a bogus string. ip_address
  // empty for the same reason (audit row column is `inet` typed).
  plinth::capabilities::UserContext caller{
      .user_id = uid,
      .username = "p_dispatch_01_alice",
      .auth_type = "session",
      .effective_rules = {"shell.preferences.read", "shell.preferences.write"},
      .session_id = "",
      .ip_address = "",
  };

  // 1. GET on a key that's never been set omits `value`, preserving the
  //    distinction between absence and a stored JSON null.
  Json::Value get_args(Json::objectValue);
  get_args["key"] = "shell.theme";
  auto r1 = drogon::sync_wait(plinth::extensions::dispatch(
      "shell", "preferences.get", get_args, caller, 0));
  REQUIRE(r1.has_value());
  REQUIRE(r1->isObject());
  REQUIRE_FALSE(r1->isMember("value"));

  // 2. SET shell.theme = "dark".
  Json::Value set_args(Json::objectValue);
  set_args["key"] = "shell.theme";
  set_args["value"] = "dark";
  auto r2 = drogon::sync_wait(plinth::extensions::dispatch(
      "shell", "preferences.set", set_args, caller, 0));
  REQUIRE(r2.has_value());

  // 3. GET → handler now finds the row scoped to this user.
  auto r3 = drogon::sync_wait(plinth::extensions::dispatch(
      "shell", "preferences.get", get_args, caller, 0));
  REQUIRE(r3.has_value());
  REQUIRE(r3->isMember("value"));
  REQUIRE(r3->operator[]("value").asString() == "dark");
}

TEST_CASE("P.platform.01: shell storage migration and generic layout mutation "
          "guard preserve existing data",
          "[shell][preferences][integration][platform-storage]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  PrefScratch scratch;
  const auto uid = seed_user(scratch.db, "platform_guard_owner");
  auto [marker_status, marker_error] = pg_run(
      scratch.db, "SELECT storage_version FROM ext_shell.platform_contract "
                  "WHERE singleton = true AND storage_version = 1");
  INFO(marker_error);
  REQUIRE(marker_status == PGRES_TUPLES_OK);
  auto [seed_status, seed_error] = pg_run(
      scratch.db, "INSERT INTO ext_shell.user_preferences (user_id,key,value) "
                  "VALUES ('" +
                      uid + "','shell.floats','null'::jsonb)");
  INFO(seed_error);
  REQUIRE(seed_status == PGRES_COMMAND_OK);
  capabilities::UserContext caller{
      .user_id = uid,
      .username = "platform_guard_owner",
      .auth_type = "session",
      .effective_rules = {"shell.preferences.read", "shell.preferences.write"},
      .session_id = "",
      .ip_address = "",
  };
  Json::Value args(Json::objectValue);
  args["key"] = "shell.floats";
  args["bypass"] = true;
  args["validated"] = true;
  // Missing value is the generic delete gesture: it must be refused too.
  auto deleted = drogon::sync_wait(
      extensions::dispatch("shell", "preferences.set", args, caller, 0));
  REQUIRE_FALSE(deleted.has_value());
  REQUIRE(deleted.error().code == "invalid_argument");
  args["value"] = Json::Value(Json::objectValue);
  auto written = drogon::sync_wait(
      extensions::dispatch("shell", "preferences.set", args, caller, 0));
  REQUIRE_FALSE(written.has_value());
  REQUIRE(written.error().code == "invalid_argument");
  args.removeMember("value");
  auto retained = drogon::sync_wait(
      extensions::dispatch("shell", "preferences.get", args, caller, 0));
  REQUIRE(retained.has_value());
  REQUIRE(retained->isMember("value"));
  REQUIRE((*retained)["value"].isNull());

  Json::Value defaults(Json::objectValue);
  defaults["version"] = 1;
  defaults["choices"] = Json::Value(Json::arrayValue);
  args["key"] = "shell.handler_defaults";
  args["value"] = defaults;
  auto valid = drogon::sync_wait(
      extensions::dispatch("shell", "preferences.set", args, caller, 0));
  REQUIRE(valid.has_value());
  args["value"]["version"] = Json::UInt64{18446744073709551615ULL};
  auto invalid = drogon::sync_wait(
      extensions::dispatch("shell", "preferences.set", args, caller, 0));
  REQUIRE_FALSE(invalid.has_value());
  REQUIRE(invalid.error().code == "invalid_argument");

  auto before = extensions::pool_identity("shell");
  REQUIRE(before.has_value());
  REQUIRE(extensions::create_pool("shell", "test-package-id", "test-version"));
  auto after = extensions::pool_identity("shell");
  REQUIRE(after.has_value());
  REQUIRE(after->serial != before->serial);
  REQUIRE(after->package_id == "test-package-id");
  REQUIRE(after->version == "test-version");
  REQUIRE_FALSE(extensions::fixed_shell_storage_identity().has_value());

  plinth::packages::generation::Budget budget;
  const plinth::packages::generation::CaptureRequest capture_request{
      .key = {.package_id = after->package_id,
              .name = after->name,
              .version = after->version,
              .profile = 1},
      .root = fs::canonical(scratch.ctx.data_dir / "extensions" / "shell" /
                            "active"),
      .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10},
      .cancellation = {},
  };
  auto captured =
      plinth::packages::generation::capture_artifact(capture_request, budget);
  REQUIRE(captured.has_value());
  const auto* module = (*captured)->find("server/platform-storage-v1.js");
  const auto* contract = (*captured)->find("server/plinth-platform.json");
  const auto* migration =
      (*captured)->find("migrations/002_platform_storage_v1.sql");
  REQUIRE(module != nullptr);
  REQUIRE(contract != nullptr);
  REQUIRE(migration != nullptr);
  extensions::FixedShellStorageDigests pins{
      .module = plinth::packages::detail::sha256_hex(module->content),
      .contract = plinth::packages::detail::sha256_hex(contract->content),
      .migration = plinth::packages::detail::sha256_hex(migration->content),
      .applied_migration =
          plinth::packages::detail::sha256_hex(migration->content),
  };
  auto wrong_receipt = pins;
  wrong_receipt.applied_migration[0] = pins.migration[0] == '0' ? '1' : '0';
  REQUIRE_FALSE(extensions::prepare_fixed_shell_storage(**captured, *after,
                                                        wrong_receipt, budget)
                    .has_value());
  plinth::packages::generation::Budget insufficient_budget{0};
  REQUIRE_FALSE(extensions::prepare_fixed_shell_storage(
                    **captured, *after, pins, insufficient_budget)
                    .has_value());
  plinth::packages::generation::Budget module_only_budget{
      module->content.size()};
  REQUIRE_FALSE(extensions::prepare_fixed_shell_storage(
                    **captured, *after, pins, module_only_budget)
                    .has_value());
  const auto captured_bytes = budget.used();
  auto prepared =
      extensions::prepare_fixed_shell_storage(**captured, *after, pins, budget);
  REQUIRE(prepared.has_value());
  const auto retained_bytes = budget.used() - captured_bytes;
  REQUIRE(retained_bytes > module->content.size());
  // Keep a second prepared owner to prove a stale pool stamp cannot publish
  // after replacement, even though UUID/version and physical artifact match.
  auto stale = *prepared;
  REQUIRE(extensions::publish_fixed_shell_storage(std::move(*prepared)));
  REQUIRE(extensions::fixed_storage_retained_bytes_for_test() ==
          retained_bytes);
  REQUIRE(extensions::fixed_shell_storage_identity() == after);
  REQUIRE(extensions::create_pool("shell", after->package_id, after->version));
  auto replacement = extensions::pool_identity("shell");
  REQUIRE(replacement.has_value());
  REQUIRE(replacement->serial != after->serial);
  const bool stale_published =
      extensions::publish_fixed_shell_storage(std::move(stale));
  REQUIRE_FALSE(stale_published);
  REQUIRE_FALSE(extensions::fixed_shell_storage_identity().has_value());
  // All prepared/published small-source owners were dropped by stale rejection
  // and pool replacement; capture itself remains physically leased separately.
  REQUIRE(budget.used() == captured_bytes);
  extensions::destroy_pool("shell");
  REQUIRE_FALSE(extensions::pool_identity("shell").has_value());
}

namespace {
class ConstructionGate {
 public:
  auto arrive() -> void {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds{20},
                          [this] { return released; })) {
      throw std::runtime_error("construction test gate expired");
    }
  }
  auto wait() -> bool {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds{10},
                            [this] { return entered; });
  }
  auto release() -> void {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }

 private:
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
};
struct ResetConstructionHook {
  ~ResetConstructionHook() { extensions::set_pool_creation_hook_for_test({}); }
};

class HeldStorageRead {
 public:
  explicit HeldStorageRead(const plinth::Config::Database& db) : owner(db) {
    REQUIRE(PQstatus(owner.conn) == CONNECTION_OK);
    auto started = owner.exec("BEGIN");
    REQUIRE(PQresultStatus(started.get()) == PGRES_COMMAND_OK);
    auto locked = owner.exec(
        "LOCK TABLE ext_shell.platform_contract IN ACCESS EXCLUSIVE MODE");
    REQUIRE(PQresultStatus(locked.get()) == PGRES_COMMAND_OK);
  }
  ~HeldStorageRead() {
    if (held) static_cast<void>(owner.exec("ROLLBACK"));
  }
  auto release() -> void {
    auto result = owner.exec("ROLLBACK");
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
    held = false;
  }
  [[nodiscard]] auto backend_pid() const -> int {
    return PQbackendPID(owner.conn);
  }

 private:
  plinth::ws_test::TestPg owner;
  bool held = true;
};

struct ResetPlatformReadState {
  std::string package_id;
  ~ResetPlatformReadState() {
    capabilities::drain::end_drain("shell");
    plinth::shell::retire_platform_frontend(package_id);
  }
};

class MainLoopBarrier {
 public:
  auto hold() -> void {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    expired = !changed.wait_for(lock, std::chrono::seconds{20},
                                [this] { return released; });
  }
  auto wait_entered() -> bool {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds{5},
                            [this] { return entered; });
  }
  auto release() -> void {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
  [[nodiscard]] auto timed_out() const -> bool {
    std::lock_guard lock(mutex);
    return expired;
  }

 private:
  mutable std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  bool expired = false;
};

struct ReleaseMainLoopBarrier {
  std::shared_ptr<MainLoopBarrier> barrier;
  ~ReleaseMainLoopBarrier() { barrier->release(); }
};

auto storage_read_is_blocked(plinth::ws_test::TestPg& observer,
                             const std::string& blocker_pid) -> bool {
  auto result = observer.exec_params(
      "SELECT EXISTS (SELECT 1 FROM pg_stat_activity "
      "WHERE datname = current_database() AND wait_event_type = 'Lock' "
      "AND query LIKE 'SELECT storage_version FROM "
      "ext_shell.platform_contract%' "
      "AND $1::int = ANY(pg_blocking_pids(pid)))",
      {blocker_pid});
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(result.get()) == 1);
  return std::string_view{PQgetvalue(result.get(), 0, 0)} == "t";
}
} // namespace

TEST_CASE(
    "P.platform.02: pool construction has per-name reservations and "
    "exception-safe bounded cutover",
    "[shell][preferences][integration][platform-storage][runtime-ownership]") {
  if (!pg_available()) SKIP("PG not available");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  PrefScratch scratch;
  for (const auto name : {"build-a", "build-b"}) {
    fs::create_directories(scratch.ctx.data_dir / "extensions" / name /
                           "active" / "server");
  }
  ConstructionGate gate;
  ResetConstructionHook reset;
  SECTION("unrelated names both publish despite overlapping completed "
          "construction") {
    extensions::set_pool_creation_hook_for_test(
        [&](std::string_view name, extensions::PoolCreationStage stage) {
          if (name == "build-a" &&
              stage == extensions::PoolCreationStage::CONSTRUCTED)
            gate.arrive();
        });
    auto first = std::async(std::launch::async, [] {
      return extensions::create_pool("build-a", "id-a", "1");
    });
    const bool entered = gate.wait();
    const bool second =
        entered && extensions::create_pool("build-b", "id-b", "1");
    gate.release();
    const bool first_result = first.get();
    REQUIRE(entered);
    REQUIRE(second);
    REQUIRE(first_result);
    REQUIRE(extensions::has_pool("build-a"));
    REQUIRE(extensions::has_pool("build-b"));
    REQUIRE(extensions::pending_pool_constructions_for_test() == 0);
  }
  SECTION(
      "last-started same-name attempt supersedes only its earlier builder") {
    std::atomic<unsigned int> constructed = 0;
    extensions::set_pool_creation_hook_for_test(
        [&](std::string_view name, extensions::PoolCreationStage stage) {
          if (name == "build-a" &&
              stage == extensions::PoolCreationStage::CONSTRUCTED &&
              constructed.fetch_add(1) == 0)
            gate.arrive();
        });
    auto first = std::async(std::launch::async, [] {
      return extensions::create_pool("build-a", "id-old", "1");
    });
    const bool entered = gate.wait();
    const bool second =
        entered && extensions::create_pool("build-a", "id-new", "2");
    gate.release();
    const bool first_result = first.get();
    REQUIRE(entered);
    REQUIRE(second);
    REQUIRE_FALSE(first_result);
    auto current = extensions::pool_identity("build-a");
    REQUIRE(current);
    REQUIRE(current->package_id == "id-new");
    REQUIRE(current->version == "2");
    REQUIRE(extensions::pending_pool_constructions_for_test() == 0);
  }
  SECTION("allocation fault after construction preserves old route and retires "
          "unpublished pool") {
    REQUIRE(extensions::create_pool("build-a", "id-old", "1"));
    const auto original = extensions::pool_identity("build-a");
    extensions::set_pool_creation_hook_for_test(
        [](std::string_view name, extensions::PoolCreationStage stage) {
          if (name == "build-a" &&
              stage == extensions::PoolCreationStage::CONSTRUCTED)
            throw std::bad_alloc{};
        });
    REQUIRE_FALSE(extensions::create_pool("build-a", "id-new", "2"));
    REQUIRE(extensions::pool_identity("build-a") == original);
    REQUIRE(extensions::pending_pool_constructions_for_test() == 0);
    REQUIRE(extensions::failed_pool_count_for_test() == 0);
  }
  SECTION(
      "shutdown seals pending construction without pretending it has joined") {
    extensions::set_pool_creation_hook_for_test(
        [&](std::string_view name, extensions::PoolCreationStage stage) {
          if (name == "build-a" &&
              stage == extensions::PoolCreationStage::CONSTRUCTED)
            gate.arrive();
        });
    auto first = std::async(std::launch::async, [] {
      return extensions::create_pool("build-a", "id-a", "1");
    });
    const bool entered = gate.wait();
    const bool first_shutdown =
        extensions::shutdown_registry(std::chrono::milliseconds{25});
    gate.release();
    const bool first_result = first.get();
    REQUIRE(entered);
    REQUIRE_FALSE(first_shutdown);
    REQUIRE_FALSE(first_result);
    REQUIRE_FALSE(extensions::has_pool("build-a"));
    REQUIRE(extensions::pending_pool_constructions_for_test() == 0);
    REQUIRE(extensions::shutdown_registry(std::chrono::seconds{5}));
  }
}

TEST_CASE("P.platform.04: rejected request delivery is attempted once",
          "[frontend][unit]") {
  plinth::Config config;
  auto registry = std::make_shared<
      plinth::packages::generation::InstalledGenerationRegistry>(
      config.packages_data_dir,
      plinth::packages::generation::Budget{std::size_t{64} * 1024});
  plinth::frontend::FloatLayoutService service{config, registry};
  unsigned calls = 0;
  service.read_layout_storage({}, "127.0.0.1", 0, [&](auto result) {
    ++calls;
    REQUIRE_FALSE(result);
    throw std::runtime_error("fixture completion failure");
  });
  REQUIRE(calls == 1);
  REQUIRE(service.active_count() == 0);
  REQUIRE_FALSE(service.drain(std::chrono::steady_clock::now() +
                              std::chrono::seconds{2}));
}

TEST_CASE(
    "P.platform.03: fixed storage keeps the shell fence and service lane "
    "until actual native SQL settlement",
    "[shell][preferences][integration][platform-storage][runtime-ownership]") {
  if (!pg_available()) SKIP("PG not available");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  PrefScratch scratch;
  const auto user = seed_user(scratch.db, "platform_physical_owner");
  plinth::ws_test::TestPg observer{scratch.db};
  REQUIRE(PQstatus(observer.conn) == CONNECTION_OK);
  const std::string token_hash(64, 'a');
  auto session =
      observer.exec_params("INSERT INTO plinth.sessions(user_id,token_hash) "
                           "VALUES($1::uuid,$2) RETURNING id::text",
                           {user, token_hash});
  REQUIRE(PQresultStatus(session.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(session.get()) == 1);
  const plinth::auth::AuthContext auth{
      .user_id = user,
      .username = "platform_physical_owner",
      .auth_type = "session",
      .session_id = PQgetvalue(session.get(), 0, 0),
      .pat_id = {},
      .token_hash = token_hash,
      .credential_source = plinth::auth::CredentialSource::COOKIE};

  // Use the actual installed bundled identity, full trusted client graph and
  // durable applied migration receipt. There is no test token/friend bypass.
  auto active =
      plinth::shell::resolve_active_frontend(scratch.db, scratch.ctx.data_dir);
  REQUIRE(active);
  REQUIRE(active->name == "shell");
  REQUIRE(active->bundled);
  REQUIRE(extensions::create_pool("shell", active->id, active->version));
  const auto identity = extensions::pool_identity("shell");
  REQUIRE(identity);
  plinth::packages::generation::Budget budget;
  const plinth::packages::generation::CaptureRequest request{
      .key = {.package_id = active->id,
              .name = "shell",
              .version = active->version,
              .profile = 1},
      .root = fs::canonical(scratch.ctx.data_dir / "extensions" / "shell" /
                            "active"),
      .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10},
      .cancellation = {}};
  auto captured =
      plinth::packages::generation::capture_artifact(request, budget);
  REQUIRE(captured);
  auto graph = plinth::shell::prepare_platform(**captured, budget);
  REQUIRE(graph);
  const auto* module = (*captured)->find("server/platform-storage-v1.js");
  const auto* contract = (*captured)->find("server/plinth-platform.json");
  const auto* migration =
      (*captured)->find("migrations/002_platform_storage_v1.sql");
  REQUIRE(module != nullptr);
  REQUIRE(contract != nullptr);
  REQUIRE(migration != nullptr);
  auto receipt = observer.exec(
      "SELECT checksum FROM plinth.migrations WHERE extension_name='shell' "
      "AND migration_file='002_platform_storage_v1.sql'");
  REQUIRE(PQresultStatus(receipt.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(receipt.get()) == 1);
  const extensions::FixedShellStorageDigests pins{
      .module = plinth::packages::detail::sha256_hex(module->content),
      .contract = plinth::packages::detail::sha256_hex(contract->content),
      .migration = plinth::packages::detail::sha256_hex(migration->content),
      .applied_migration = PQgetvalue(receipt.get(), 0, 0)};
  const auto* declarations = (*captured)->find("panels.json");
  REQUIRE(declarations != nullptr);
  const auto parsed = plinth::packages::PanelsManifest::parse(
      declarations->content, "panels.json");
  REQUIRE(parsed.value);
  REQUIRE(parsed.value->panels.empty());
  REQUIRE_FALSE(parsed.value->format_version.has_value());
  const auto registered = observer.exec_params(
      "SELECT count(*)::text FROM plinth.panels WHERE package_id=$1::uuid",
      {active->id});
  REQUIRE(PQresultStatus(registered.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(registered.get()) == 1);
  REQUIRE(std::string_view{PQgetvalue(registered.get(), 0, 0)} == "0");
  auto publish_storage = [&](const auto& artifact) {
    const auto current_identity = extensions::pool_identity("shell");
    REQUIRE(current_identity);
    auto storage = extensions::prepare_fixed_shell_storage(
        *artifact, *current_identity, pins, budget);
    REQUIRE(storage);
    REQUIRE(extensions::publish_fixed_shell_storage(std::move(*storage)));
  };
  auto receipt_connection = std::shared_ptr<PGconn>{
      PQconnectdb(conninfo_of(scratch.db).c_str()), PQfinish};
  REQUIRE(receipt_connection);
  REQUIRE(PQstatus(receipt_connection.get()) == CONNECTION_OK);
  auto binding_operation =
      plinth::packages::name_operation::NameOperation::acquire(
          receipt_connection, "shell");
  REQUIRE(binding_operation);
  const auto declarations_digest =
      plinth::packages::generation::bind_registered_generation(
          *binding_operation, **captured, budget);
  REQUIRE(declarations_digest);
  auto actual_receipt = plinth::packages::generation::receipt_from_artifact(
      **captured, *declarations_digest);
  REQUIRE(actual_receipt);
  REQUIRE(binding_operation->release_checked());
  auto command = [&](const char* sql) {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(receipt_connection.get(), sql), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  };
  ResetPlatformReadState reset{active->id};
  auto require_refused = [&] {
    plinth::frontend::FloatLayoutService refused_service{scratch.cfg};
    HeldStorageRead held{scratch.db};
    const auto blocker = std::to_string(held.backend_pid());
    auto completion = std::make_shared<
        std::promise<plinth::frontend::FloatLayoutService::Result>>();
    auto result = completion->get_future();
    refused_service.read_layout_storage(
        auth, "127.0.0.1", 0,
        [completion](auto value) { completion->set_value(std::move(value)); });
    REQUIRE(result.wait_for(std::chrono::seconds{5}) ==
            std::future_status::ready);
    const auto failed = result.get();
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().code == "storage_unavailable");
    REQUIRE_FALSE(storage_read_is_blocked(observer, blocker));
    REQUIRE(refused_service.drain(std::chrono::steady_clock::now() +
                                  std::chrono::seconds{5}));
    REQUIRE(refused_service.active_count() == 0);
  };

  SECTION("missing committed generation receipt is unavailable") {
    {
      auto name = plinth::packages::name_operation::NameOperation::acquire(
          receipt_connection, "shell");
      REQUIRE(name);
      const auto missing =
          plinth::packages::generation::read_receipt(*name, request.key);
      REQUIRE(missing);
      REQUIRE_FALSE(missing->has_value());
      REQUIRE(name->release_checked());
    }
    publish_storage(*captured);
    REQUIRE(plinth::shell::publish_platform_frontend(*active, *graph));
    require_refused();
  }

  SECTION("matching immutable receipt is committed before any publication") {
    {
      auto name = plinth::packages::name_operation::NameOperation::acquire(
          receipt_connection, "shell");
      REQUIRE(name);
      command("BEGIN");
      REQUIRE(plinth::packages::generation::insert_matching_receipt(
          *name, *actual_receipt));
      command("COMMIT");
      const auto committed =
          plinth::packages::generation::read_receipt(*name, request.key);
      REQUIRE(committed);
      REQUIRE(committed->has_value());
      REQUIRE(**committed == *actual_receipt);
      REQUIRE(name->release_checked());
    }
    publish_storage(*captured);
    REQUIRE(plinth::shell::publish_platform_frontend(*active, *graph));

    SECTION(
        "same UUID and version cannot combine different captured artifacts") {
      const auto variant_path = request.root / "physical-capture-variant.txt";
      REQUIRE_FALSE(fs::exists(variant_path));
      {
        std::ofstream variant{variant_path, std::ios::binary};
        REQUIRE(variant.good());
        variant
            << "another real package member changes full artifact identity\n";
        REQUIRE(variant.good());
      }
      auto changed_request = request;
      changed_request.deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{10};
      auto changed = plinth::packages::generation::capture_artifact(
          changed_request, budget);
      REQUIRE(changed);
      REQUIRE((*changed)->key() == (*captured)->key());
      REQUIRE((*changed)->digest() != (*captured)->digest());
      for (const auto* path :
           {"server/platform-storage-v1.js", "server/plinth-platform.json",
            "migrations/002_platform_storage_v1.sql"}) {
        const auto* original = (*captured)->find(path);
        const auto* replacement = (*changed)->find(path);
        REQUIRE(original != nullptr);
        REQUIRE(replacement != nullptr);
        REQUIRE(original->content == replacement->content);
      }
      auto changed_graph = plinth::shell::prepare_platform(**changed, budget);
      REQUIRE(changed_graph);
      SECTION("changed graph cannot use original fixed storage") {
        REQUIRE(
            plinth::shell::publish_platform_frontend(*active, *changed_graph));
      }
      SECTION("changed storage cannot use original graph") {
        REQUIRE(extensions::create_pool("shell", active->id, active->version));
        const auto replacement_identity = extensions::pool_identity("shell");
        REQUIRE(replacement_identity);
        REQUIRE(replacement_identity->serial != identity->serial);
        publish_storage(*changed);
      }
      SECTION(
          "matching changed graph and storage cannot reuse original receipt") {
        REQUIRE(extensions::create_pool("shell", active->id, active->version));
        const auto replacement_identity = extensions::pool_identity("shell");
        REQUIRE(replacement_identity);
        REQUIRE(replacement_identity->serial != identity->serial);
        publish_storage(*changed);
        REQUIRE(
            plinth::shell::publish_platform_frontend(*active, *changed_graph));
      }
      require_refused();
    }

    SECTION(
        "admitted metadata guard survives a fence during main-loop handoff") {
      plinth::frontend::FloatLayoutService service{scratch.cfg};
      HeldStorageRead blocked{scratch.db};
      const auto blocker_pid = std::to_string(blocked.backend_pid());
      auto barrier = std::make_shared<MainLoopBarrier>();
      // This RAII release precedes both PG lock and service destruction even
      // on failed assertions. The queued callback borrows no stack state.
      ReleaseMainLoopBarrier release{barrier};
      // A real short-lived fixture guard retains the observable state across
      // begin/end. It is removed BEFORE testing the request's exact count one.
      auto observation_guard =
          std::make_unique<capabilities::drain::DispatchGuard>("shell");
      REQUIRE(observation_guard->admitted());
      auto state = capabilities::drain::begin_drain("shell");
      capabilities::drain::end_drain("shell");
      drogon::app().getLoop()->queueInLoop([barrier] { barrier->hold(); });
      REQUIRE(barrier->wait_entered());
      auto completion = std::make_shared<
          std::promise<plinth::frontend::FloatLayoutService::Result>>();
      auto result = completion->get_future();
      auto calls = std::make_shared<std::atomic<unsigned>>(0);
      service.read_layout_storage(auth, "127.0.0.1", 0,
                                  [completion, calls](auto value) {
                                    if (calls->fetch_add(1) == 0)
                                      completion->set_value(std::move(value));
                                  });
      const auto admission_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{4};
      bool admitted = false;
      do {
        if (state->in_flight.load() == 2) {
          admitted = true;
          break;
        }
      } while (std::chrono::steady_clock::now() < admission_deadline &&
               result.wait_for(std::chrono::milliseconds{10}) !=
                   std::future_status::ready);
      REQUIRE(admitted);
      observation_guard.reset();
      REQUIRE(state->in_flight.load() == 1);
      REQUIRE_FALSE(storage_read_is_blocked(observer, blocker_pid));
      REQUIRE(result.wait_for(std::chrono::milliseconds{0}) ==
              std::future_status::timeout);
      auto fenced = capabilities::drain::begin_drain("shell");
      REQUIRE(fenced == state);
      const auto [zero, outstanding] = capabilities::drain::wait_for_zero(
          fenced, std::chrono::milliseconds{25});
      REQUIRE_FALSE(zero);
      REQUIRE(outstanding == 1);
      SECTION("admitted owner completes after a later fence") {
        blocked.release();
        barrier->release();
        REQUIRE_FALSE(barrier->timed_out());
        REQUIRE(result.wait_for(std::chrono::seconds{5}) ==
                std::future_status::ready);
        const auto completed = result.get();
        REQUIRE(completed);
        REQUIRE((*completed)["present"].isBool());
        REQUIRE_FALSE((*completed)["present"].asBool());
      }
      SECTION(
          "stopped metadata consumer cannot resume on late native delivery") {
        // Stop wakes the owned bridge, not the blocked loop or accepted native
        // request. Logical failure must not release that request's guard/slot.
        REQUIRE_FALSE(service.drain(std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds{25}));
        REQUIRE(result.wait_for(std::chrono::seconds{2}) ==
                std::future_status::ready);
        const auto completed = result.get();
        REQUIRE_FALSE(completed);
        REQUIRE(service.active_count() == 1);
        REQUIRE(fenced->in_flight.load() == 1);
        REQUIRE_FALSE(storage_read_is_blocked(observer, blocker_pid));
        barrier->release();
        REQUIRE_FALSE(barrier->timed_out());
        // Keep the SQL lock held: a mistakenly launched late native call
        // would prevent this physical drain from succeeding.
        REQUIRE(service.drain(std::chrono::steady_clock::now() +
                              std::chrono::seconds{5}));
        REQUIRE_FALSE(storage_read_is_blocked(observer, blocker_pid));
        blocked.release();
      }
      REQUIRE(service.drain(std::chrono::steady_clock::now() +
                            std::chrono::seconds{5}));
      const auto [settled, remaining] =
          capabilities::drain::wait_for_zero(fenced, std::chrono::seconds{5});
      REQUIRE(settled);
      REQUIRE(remaining == 0);
      REQUIRE(service.active_count() == 0);
      REQUIRE(calls->load() == 1);
    }

    SECTION("genuine native SQL settlement retains name and service owners") {

      plinth::frontend::FloatLayoutService service{scratch.cfg};
      // Destruction order releases this genuine PG lock BEFORE destroying the
      // service, including assertion unwinding. No native owner is abandoned.
      HeldStorageRead blocked{scratch.db};
      const auto blocker_pid = std::to_string(blocked.backend_pid());
      auto completion = std::make_shared<
          std::promise<plinth::frontend::FloatLayoutService::Result>>();
      auto result = completion->get_future();
      service.read_layout_storage(auth, "127.0.0.1", 0,
                                  [completion](auto value) {
                                    completion->set_value(std::move(value));
                                  });
      bool observed = false;
      const auto observation_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{4};
      do {
        if (storage_read_is_blocked(observer, blocker_pid)) {
          observed = true;
          break;
        }
        // This is bounded observation of a real lock dependency, not a sleep
        // standing in for native settlement or a fabricated callback result.
      } while (std::chrono::steady_clock::now() < observation_deadline &&
               result.wait_for(std::chrono::milliseconds{10}) !=
                   std::future_status::ready);
      REQUIRE(observed);
      auto fenced = capabilities::drain::begin_drain("shell");
      REQUIRE(fenced->in_flight.load() == 1);

      // The isolated driver has a real five-second SQL deadline; its timeout
      // may finish JS BEFORE the still-issued SQL and its callback owner are
      // released. This deliberately does not claim a 30s wall-clock
      // cancellation occurred.
      REQUIRE(result.wait_for(std::chrono::seconds{45}) ==
              std::future_status::ready);
      const auto failed = result.get();
      REQUIRE_FALSE(failed);
      REQUIRE(failed.error().code == "cap.handler_threw");
      REQUIRE(storage_read_is_blocked(observer, blocker_pid));
      const auto [prematurely_zero, outstanding] =
          capabilities::drain::wait_for_zero(fenced,
                                             std::chrono::milliseconds{25});
      REQUIRE_FALSE(prematurely_zero);
      REQUIRE(outstanding == 1);
      REQUIRE(service.active_count() == 1);
      REQUIRE_FALSE(service.drain(std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds{25}));

      blocked.release();
      const auto [settled, remaining] =
          capabilities::drain::wait_for_zero(fenced, std::chrono::seconds{5});
      REQUIRE(settled);
      REQUIRE(remaining == 0);
      REQUIRE(service.drain(std::chrono::steady_clock::now() +
                            std::chrono::seconds{5}));
      REQUIRE(service.active_count() == 0);
      REQUIRE_FALSE(storage_read_is_blocked(observer, blocker_pid));
    }
  }
}
