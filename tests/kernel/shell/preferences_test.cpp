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

#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/shell/firstboot.hpp"

#include "../js/async_bridge_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <libpq-fe.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

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

    REQUIRE(
        plinth::shell::ensure_bundled_shell_installed(cfg, ctx).has_value());
  }
  ~PrefScratch() {
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

  // Wire the runtime registry to the same data dir PrefScratch's
  // installer used. Without this `init_registry` would scan the
  // default `./data` and find nothing.
  s.cfg.packages_data_dir = (s.base / "data").string();

  plinth::capabilities::init_resolver(s.cfg.db);
  plinth::extensions::init_registry(s.cfg);

  // Per-test scope teardown — sidesteps the 0.6.3.N Bug 2 atexit
  // failure mode (init_registry paired with `drogon::app().quit()`
  // from atexit). RAII guard runs even on REQUIRE-fail.
  struct RegistryGuard {
    RegistryGuard() = default;
    ~RegistryGuard() {
      (void)plinth::extensions::shutdown_registry();
      plinth::capabilities::clear_resolver_for_test();
    }
    RegistryGuard(const RegistryGuard&) = delete;
    auto operator=(const RegistryGuard&) -> RegistryGuard& = delete;
    RegistryGuard(RegistryGuard&&) = delete;
    auto operator=(RegistryGuard&&) -> RegistryGuard& = delete;
  } guard;

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
