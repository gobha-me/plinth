// PG-gated integration tests for RBAC test end-to-end (ICD-0.4.7 cases
// PB.01–PB.06, PB.08, PB.09). Slice A drives `run_rbac_test` directly
// against a pre-seeded `ACTIVE` package — trigger sites are wired in
// Slice B, so these tests bypass `install_package` and seed the DB +
// Tier 2 cache rows that an install would otherwise produce. Slice B
// adds install-driven PB.* cases once the trigger edits land.

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/rbac_test_runner.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto pg_config() -> plinth::Config::Database {
  plinth::Config::Database db;
  if (auto* v = std::getenv("PLINTH_PG_HOST")) {
    db.host = v;
  }
  if (auto* v = std::getenv("PLINTH_PG_PORT")) {
    db.port = static_cast<std::uint16_t>(std::stoi(v));
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

auto conninfo_of(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password + " connect_timeout=3";
}

auto pg_available() -> bool {
  if (std::getenv("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  PGconn* conn = PQconnectdb(conninfo_of(pg_config()).c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

namespace fs = std::filesystem;

auto drop_all_schemas(const plinth::Config::Database& db) -> void {
  PGconn* conn = PQconnectdb(conninfo_of(db).c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    PQfinish(conn);
    return;
  }
  // Drop per-extension schemas created by install_package, and their
  // roles. Mirrors install_lifecycle_test::drop_all_ext_schemas.
  PGresult* res =
      PQexec(conn, "SELECT schema_name FROM information_schema.schemata "
                   "WHERE schema_name LIKE 'ext\\_%' ESCAPE '\\'");
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int n = PQntuples(res);
    for (int i = 0; i < n; ++i) {
      std::string s = PQgetvalue(res, i, 0);
      PQclear(
          PQexec(conn, ("DROP SCHEMA IF EXISTS " + s + " CASCADE").c_str()));
      PQclear(PQexec(conn, ("DROP ROLE IF EXISTS " + s + "_role").c_str()));
    }
  }
  PQclear(res);
  PQclear(PQexec(conn, "DROP SCHEMA IF EXISTS plinth CASCADE"));
  PQfinish(conn);
}

std::atomic<std::uint64_t> g_scratch_counter{0};

struct Scratch {
  plinth::Config::Database db;
  PGconn* conn = nullptr;
  plinth::packages::InstallerContext ctx;
  std::string package_id;
  fs::path base; // per-test tmp dir (install-driven tests)

  Scratch() : db(pg_config()) {
    drop_all_schemas(db);
    plinth::db::bootstrap_schema(
        db, std::string{CMAKE_SOURCE_DIR} + "/migrations", true);
    plinth::groups::bootstrap_groups(db);
    conn = PQconnectdb(conninfo_of(db).c_str());
    plinth::capabilities::clear_resolver_for_test();

    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(g_scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_rbac_test_" + id);
    fs::create_directories(base / "data");
    fs::create_directories(base / "staging");

    ctx.db = db;
    ctx.data_dir = base / "data";
    ctx.staging_dir = base / "staging";
    ctx.max_package_size_bytes = 50ULL * 1024ULL * 1024ULL;
  }
  ~Scratch() {
    plinth::capabilities::clear_resolver_for_test();
    if (conn != nullptr) {
      PQfinish(conn);
    }
    drop_all_schemas(db);
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  Scratch(const Scratch&) = delete;
  auto operator=(const Scratch&) -> Scratch& = delete;
  Scratch(Scratch&&) = delete;
  auto operator=(Scratch&&) -> Scratch& = delete;
};

auto exec_params(PGconn* conn, const char* sql,
                 const std::vector<std::string>& vs) -> PgResultPtr {
  std::vector<const char*> values;
  values.reserve(vs.size());
  for (const auto& s : vs) {
    values.push_back(s.c_str());
  }
  return {PQexecParams(conn, sql, static_cast<int>(values.size()), nullptr,
                       values.data(), nullptr, nullptr, 0),
          PQclear};
}

// Seed a package row in ACTIVE state with synthetic manifest metadata.
auto seed_package(PGconn* conn, std::string_view name, std::string_view version)
    -> std::string {
  auto res = exec_params(
      conn,
      "INSERT INTO plinth.packages "
      "(name, version, state, provenance, manifest_json, entry_point, "
      " manifest_checksum) "
      "VALUES ($1, $2, 'ACTIVE', 'user', '{}'::jsonb, "
      "        'server/main.js', 'sha256-placeholder') "
      "RETURNING id::text",
      {std::string{name}, std::string{version}});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  return PQgetvalue(res.get(), 0, 0);
}

// Seed an rbac_rule with optional test_contract JSONB.
auto seed_rule(PGconn* conn, std::string_view rule, std::string_view namespace_,
               std::string_view extension_name,
               const std::optional<nlohmann::json>& test_contract) -> void {
  std::vector<std::string> vs = {
      std::string{rule},
      std::string{namespace_},
      std::string{extension_name},
  };
  const char* sql = nullptr;
  if (test_contract.has_value()) {
    vs.push_back(test_contract->dump());
    sql = "INSERT INTO plinth.rbac_rules "
          "(rule, namespace, description, extension_name, test_contract) "
          "VALUES ($1, $2, 'seeded rule', $3, $4::jsonb)";
  } else {
    sql = "INSERT INTO plinth.rbac_rules "
          "(rule, namespace, description, extension_name) "
          "VALUES ($1, $2, 'seeded rule', $3)";
  }
  auto res = exec_params(conn, sql, vs);
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
}

// Register the extension capability in Tier 2 cache with the specified
// rbac_rule (so dispatch returns TIER3_NOT_AVAILABLE past the RBAC
// gate).
auto register_ext_cap(std::string_view signature, std::string_view rbac_rule,
                      std::string_view extension_name) -> void {
  plinth::capabilities::upsert_tier2_entry({
      .signature = std::string{signature},
      .provider_type = "extension",
      .extension_name = std::string{extension_name},
      .scope = "instance",
      .user_id = "",
      .rbac_rule = std::string{rbac_rule},
      .enabled = true,
  });
}

auto grant_everyone(PGconn* conn, std::string_view rule) -> void {
  auto res =
      exec_params(conn,
                  "INSERT INTO plinth.group_rules (group_id, rule_id) "
                  "SELECT g.id, r.id FROM plinth.groups g, plinth.rbac_rules r "
                  "WHERE g.name = 'everyone' AND r.rule = $1",
                  {std::string{rule}});
  REQUIRE(PQresultStatus(res.get()) == PGRES_COMMAND_OK);
}

// Read back the current state for a package.
auto pkg_state(PGconn* conn, std::string_view id) -> std::string {
  auto res =
      exec_params(conn, "SELECT state FROM plinth.packages WHERE id = $1::uuid",
                  {std::string{id}});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  return PQgetvalue(res.get(), 0, 0);
}

auto pkg_result(PGconn* conn, std::string_view id) -> nlohmann::json {
  auto res = exec_params(
      conn,
      "SELECT last_rbac_test_result FROM plinth.packages WHERE id = $1::uuid",
      {std::string{id}});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  const char* val = PQgetvalue(res.get(), 0, 0);
  if (PQgetisnull(res.get(), 0, 0) != 0) {
    return nlohmann::json{};
  }
  return nlohmann::json::parse(val);
}

auto count(PGconn* conn, const char* sql) -> long {
  PgResultPtr res(PQexec(conn, sql), PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return -1;
  }
  return std::stol(PQgetvalue(res.get(), 0, 0));
}

auto test_contract_for(std::string_view call_sig) -> nlohmann::json {
  return {
      {"assert_deny", {{"call", std::string{call_sig}}}},
      {"assert_allow", {{"call", std::string{call_sig}}}},
  };
}

struct BlockingHandlerState {
  std::mutex mu;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;

  auto release() -> void {
    {
      std::lock_guard lock(mu);
      released = true;
    }
    cv.notify_all();
  }
};

struct AsyncWorkersRestore {
  std::shared_ptr<BlockingHandlerState> handler;

  ~AsyncWorkersRestore() {
    handler->release();
    (void)plinth::packages::rbac_test::shutdown_async_workers(
        std::chrono::seconds{2});
    (void)plinth::packages::rbac_test::start_async_workers();
  }
};

} // namespace

TEST_CASE("PB.01 happy — all rules pass, skipped counted",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.read", "notes", "notes",
            test_contract_for("notes:1:read"));
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  seed_rule(s.conn, "notes.delete", "notes", "notes", std::nullopt);
  register_ext_cap("notes:1:read", "notes.read", "notes");
  register_ext_cap("notes:1:edit", "notes.edit", "notes");
  register_ext_cap("notes:1:delete", "notes.delete", "notes");

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE(out->overall_passed());
  REQUIRE(out->passed.size() == 4); // 2 rules × 2 clauses
  REQUIRE(out->failed.empty());
  REQUIRE(out->skipped.size() == 1);
  REQUIRE(out->skipped[0] == "notes.delete");

  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE");
  auto j = pkg_result(s.conn, pid);
  REQUIRE(j["failed"].empty());
  REQUIRE(j["skipped"].size() == 1);
}

TEST_CASE(
    "PB.02 broken assert_deny — rule granted to everyone flips to FLAGGED",
    "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");

  // Poison: grant notes.edit to the `everyone` group. The denied
  // ephemeral user inherits the grant, so RBAC no longer fails the
  // test call — `actual != permission_denied` → assert_deny FAILS.
  grant_everyone(s.conn, "notes.edit");

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE_FALSE(out->overall_passed());
  REQUIRE(out->failed.size() == 1);
  REQUIRE(out->failed[0].clause == "assert_deny");

  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE_FLAGGED");
}

TEST_CASE("PB.03 broken assert_allow — capability rbac_rule mismatch",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  // rbac.json declares `notes.edit`; the capability actually requires
  // `notes.other_rule` — granting `notes.edit` to the allowed user
  // does not unlock the capability. The allowed call returns
  // PERMISSION_DENIED → assert_allow FAILS.
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  seed_rule(s.conn, "notes.other_rule", "notes", "notes", std::nullopt);
  register_ext_cap("notes:1:edit", "notes.other_rule", "notes");

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE_FALSE(out->overall_passed());
  REQUIRE(out->failed.size() == 1);
  REQUIRE(out->failed[0].clause == "assert_allow");
  REQUIRE(out->failed[0].actual == "permission_denied");

  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE_FLAGGED");
}

TEST_CASE("PB.04 mixed pass/fail — deny and allow each break one rule",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.ok", "notes", "notes",
            test_contract_for("notes:1:ok"));
  seed_rule(s.conn, "notes.leaky", "notes", "notes",
            test_contract_for("notes:1:leaky"));
  seed_rule(s.conn, "notes.locked", "notes", "notes",
            test_contract_for("notes:1:locked"));
  seed_rule(s.conn, "notes.other", "notes", "notes", std::nullopt);
  register_ext_cap("notes:1:ok", "notes.ok", "notes");
  register_ext_cap("notes:1:leaky", "notes.leaky", "notes");
  register_ext_cap("notes:1:locked", "notes.other", "notes"); // mismatch

  grant_everyone(s.conn, "notes.leaky");

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE_FALSE(out->overall_passed());
  REQUIRE(out->failed.size() == 2);
  // notes.ok passes on both clauses; notes.leaky fails assert_deny;
  // notes.locked fails assert_allow.
  bool saw_deny_fail = false;
  bool saw_allow_fail = false;
  for (const auto& f : out->failed) {
    if (f.rule == "notes.leaky" && f.clause == "assert_deny") {
      saw_deny_fail = true;
    }
    if (f.rule == "notes.locked" && f.clause == "assert_allow") {
      saw_allow_fail = true;
    }
  }
  REQUIRE(saw_deny_fail);
  REQUIRE(saw_allow_fail);

  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE_FLAGGED");
}

TEST_CASE("PB.05 no test_contract — all rules skipped, state stays ACTIVE",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.a", "notes", "notes", std::nullopt);
  seed_rule(s.conn, "notes.b", "notes", "notes", std::nullopt);
  seed_rule(s.conn, "notes.c", "notes", "notes", std::nullopt);

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE(out->overall_passed());
  REQUIRE(out->passed.empty());
  REQUIRE(out->failed.empty());
  REQUIRE(out->skipped.size() == 3);

  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE");
}

TEST_CASE("PB.06 assert_allow with non-permission error passes",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE(out->overall_passed());
  // Extension-registered caps surface `async_required` on the sync
  // `call_capability` path the rbac_test runner uses (ICD-0.5.0.3
  // §Sync vs async, since 0.5.0.4). That's a non-permission error —
  // per DESIGN §0.4.7 it passes `assert_allow`.
  bool saw_allow_pass_non_success = false;
  for (const auto& p : out->passed) {
    if (p.clause == "assert_allow" && p.actual == "async_required") {
      saw_allow_pass_non_success = true;
    }
  }
  REQUIRE(saw_allow_pass_non_success);
  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE");
}

TEST_CASE("PB.08 ephemeral users cleaned up on RBAC test success",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");

  auto before = count(
      s.conn, "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true");
  REQUIRE(before == 0);

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());

  auto after = count(
      s.conn, "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true");
  REQUIRE(after == 0);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE starts_with(name, '__rbac_test_')") == 0);
}

TEST_CASE("PB.09 ephemeral users cleaned up on RBAC test failure",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");
  grant_everyone(s.conn, "notes.edit"); // poisons assert_deny

  auto out = plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(out.has_value());
  REQUIRE_FALSE(out->overall_passed());

  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      0);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE starts_with(name, '__rbac_test_')") == 0);
}

// ── Slice B: trigger / reconciler / fixture helpers ────────────────

namespace {

using namespace std::chrono_literals;

auto read_file_bytes(const fs::path& p) -> std::vector<std::byte> {
  std::ifstream in(p, std::ios::binary);
  std::vector<std::byte> out;
  if (!in.is_open()) {
    return out;
  }
  in.seekg(0, std::ios::end);
  auto n = in.tellg();
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(n));
  // char*; std::byte is bit-compatible.
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
  return out;
}

auto read_fixture(const std::string& relpath) -> std::vector<std::byte> {
  fs::path p = fs::path{CMAKE_BINARY_DIR} / "fixtures" / (relpath + ".zip");
  return read_file_bytes(p);
}

// Poll until last_rbac_test_run_at is populated (or budget elapses).
// Returns true on populated; false on timeout.
auto wait_rbac_test(PGconn* conn, std::string_view id,
                    std::chrono::milliseconds budget = 10s) -> bool {
  auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    auto res = exec_params(
        conn,
        "SELECT last_rbac_test_run_at IS NOT NULL FROM plinth.packages "
        "WHERE id = $1::uuid",
        {std::string{id}});
    if (PQresultStatus(res.get()) == PGRES_TUPLES_OK &&
        PQntuples(res.get()) == 1 && *PQgetvalue(res.get(), 0, 0) == 't') {
      return true;
    }
    std::this_thread::sleep_for(50ms);
  }
  return false;
}

auto count_audit(PGconn* conn, std::string_view action, std::string_view pkg_id)
    -> long {
  auto res = exec_params(conn,
                         "SELECT COUNT(*) FROM plinth.audit_log "
                         "WHERE action = $1 AND detail->>'package_id' = $2",
                         {std::string{action}, std::string{pkg_id}});
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return -1;
  }
  return std::stol(PQgetvalue(res.get(), 0, 0));
}

// Poll until the sum of rbac_test_passed + rbac_test_failed audit rows reaches
// `target`. This remains useful when the caller is specifically waiting on
// multiple detached runs rather than one package completion marker.
auto wait_rbac_test_audit_count(PGconn* conn, std::string_view pid, long target,
                                std::chrono::milliseconds budget = 5s) -> long {
  auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    long total = count_audit(conn, "packages.rbac_test_passed", pid) +
                 count_audit(conn, "packages.rbac_test_failed", pid);
    if (total >= target) {
      return total;
    }
    std::this_thread::sleep_for(25ms);
  }
  return count_audit(conn, "packages.rbac_test_passed", pid) +
         count_audit(conn, "packages.rbac_test_failed", pid);
}

// Seed a bare ACTIVE package row (no rules) with a specific installed_at.
auto seed_package_at(PGconn* conn, std::string_view name,
                     std::string_view version, std::string_view installed_expr)
    -> std::string {
  std::string sql =
      "INSERT INTO plinth.packages "
      "(name, version, state, provenance, manifest_json, entry_point, "
      " manifest_checksum, installed_at) "
      "VALUES ($1, $2, 'ACTIVE', 'user', '{}'::jsonb, "
      "        'server/main.js', 'sha256-placeholder', " +
      std::string{installed_expr} +
      ") "
      "RETURNING id::text";
  auto res =
      exec_params(conn, sql.c_str(), {std::string{name}, std::string{version}});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  return PQgetvalue(res.get(), 0, 0);
}

// Manually insert a stranded ephemeral user + group — the state a prior
// RBAC test run would have left if it crashed before destroy_run_users.
auto seed_stranded_test_user(PGconn* conn, std::string_view run_id,
                             std::string_view age_expr) -> void {
  std::string user = std::string{"__test_denied_"} + std::string{run_id};
  std::string grp = std::string{"__rbac_test_"} + std::string{run_id};
  // User row — placeholder hash; is_test_user=true excludes from any
  // COUNT the first-user bootstrap would see.
  auto u = exec_params(
      conn,
      std::string{"INSERT INTO plinth.users "
                  "(username, password_hash, is_test_user, created_at) "
                  "VALUES ($1, 'stub', true, "}
          .append(age_expr)
          .append(")")
          .c_str(),
      {user});
  REQUIRE(PQresultStatus(u.get()) == PGRES_COMMAND_OK);
  // Group row (age-checked by reconciler — groups.created_at matters).
  auto g = exec_params(conn,
                       std::string{"INSERT INTO plinth.groups "
                                   "(name, description, created_at) "
                                   "VALUES ($1, 'stranded test group', "}
                           .append(age_expr)
                           .append(")")
                           .c_str(),
                       {grp});
  REQUIRE(PQresultStatus(g.get()) == PGRES_COMMAND_OK);
}

} // namespace

TEST_CASE("PB.10 reconciler cleans orphaned ephemeral users (>1h)",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  // Two stale runs (2 h old) and one fresh (just now) — only the
  // stale pair should be collected.
  seed_stranded_test_user(s.conn, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                          "NOW() - interval '2 hours'");
  seed_stranded_test_user(s.conn, "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
                          "NOW() - interval '2 hours'");
  seed_stranded_test_user(s.conn, "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                          "NOW()");

  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      3);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE starts_with(name, '__rbac_test_')") == 3);

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  // Only the fresh one (c...) remains; the two 2h-old are gone.
  REQUIRE(
      count(s.conn,
            "SELECT COUNT(*) FROM plinth.users WHERE is_test_user = true") ==
      1);
  REQUIRE(count(s.conn, "SELECT COUNT(*) FROM plinth.groups "
                        "WHERE starts_with(name, '__rbac_test_')") == 1);
}

TEST_CASE("PB.11 reconciler schedules RBAC test for fresh NULL row",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  // Fresh ACTIVE row (installed 10 m ago) with last_rbac_test_run_at NULL.
  auto pid = seed_package_at(s.conn, "notests", "1.0.0",
                             "NOW() - interval '10 minutes'");
  // No rules — all skipped — but RBAC test still runs and writes
  // last_rbac_test_run_at + an empty report.

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  REQUIRE(wait_rbac_test(s.conn, pid));
  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE");
}

TEST_CASE("PB.12 reconciler skips stale NULL row (>1h)",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid =
      seed_package_at(s.conn, "notests", "1.0.0", "NOW() - interval '2 hours'");

  plinth::packages::reconcile_in_flight_installs(s.ctx);

  // Give any erroneous detached thread a chance to write.
  std::this_thread::sleep_for(250ms);

  auto res =
      exec_params(s.conn,
                  "SELECT last_rbac_test_run_at IS NULL FROM plinth.packages "
                  "WHERE id = $1::uuid",
                  {pid});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(*PQgetvalue(res.get(), 0, 0) == 't'); // stays NULL
}

TEST_CASE("PB.13 install_package triggers RBAC test end-to-end",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto blob = read_fixture("rbac_test_runner/happy-all-pass");
  REQUIRE(!blob.empty());
  auto r = plinth::packages::install_package(
      blob, plinth::packages::Provenance::USER, s.ctx);
  if (!r.has_value()) {
    UNSCOPED_INFO("install failed at stage "
                  << plinth::packages::stage_to_string(r.error().failed_at)
                  << " kind=" << r.error().kind << " msg=" << r.error().message
                  << " report=" << r.error().report.dump(2));
  }
  REQUIRE(r.has_value());

  // Populate the Tier 2 cache from the freshly-registered DB rows.
  // Production gets this via LISTEN/NOTIFY; the test binary has no
  // listener thread, so the detached RBAC test worker would otherwise
  // see capability_not_found. The reload is small (single SELECT);
  // the worker's startup (PG connect + advisory lock + ephemeral user
  // INSERTs) comfortably exceeds the reload, so this wins the race
  // in practice — if CI surfaces a flake, promote this into the
  // kernel trigger sites.
  plinth::capabilities::reload_tier2_cache(s.ctx.db);

  REQUIRE(wait_rbac_test(s.conn, r->id));
  // happy-all-pass has 2 rules with test_contract + 1 without — all pass.
  auto j = pkg_result(s.conn, r->id);
  INFO("rbac_test result: " << j.dump(2));
  REQUIRE(j["failed"].empty());
  REQUIRE(pkg_state(s.conn, r->id) == "ACTIVE");
  REQUIRE(j["skipped"].size() == 1);

  REQUIRE(wait_rbac_test_audit_count(s.conn, r->id, 1) == 1);
  REQUIRE(count_audit(s.conn, "packages.rbac_test_passed", r->id) == 1);
  REQUIRE(count_audit(s.conn, "packages.rbac_test_failed", r->id) == 0);

  // Trigger provenance lives in the audit detail, not last_rbac_test_result.
  auto trig =
      exec_params(s.conn,
                  "SELECT detail->>'triggered_by' FROM plinth.audit_log "
                  "WHERE action = 'packages.rbac_test_passed' "
                  "  AND detail->>'package_id' = $1",
                  {r->id});
  REQUIRE(PQresultStatus(trig.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(trig.get()) == 1);
  REQUIRE(std::string{PQgetvalue(trig.get(), 0, 0)} == "install");
}

TEST_CASE("PB.14 upgrade fires RBAC test on new row; old SUPERSEDED untouched",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto v1_blob = read_fixture("valid-install"); // notes 1.2.3
  REQUIRE(!v1_blob.empty());
  auto v1 = plinth::packages::install_package(
      v1_blob, plinth::packages::Provenance::USER, s.ctx);
  REQUIRE(v1.has_value());
  REQUIRE(wait_rbac_test(s.conn, v1->id));

  // Capture v1's run_at + result so we can assert they don't change.
  auto v1_result_before = pkg_result(s.conn, v1->id);
  auto run_at_before =
      exec_params(s.conn,
                  "SELECT last_rbac_test_run_at::text FROM plinth.packages "
                  "WHERE id = $1::uuid",
                  {v1->id});
  REQUIRE(PQntuples(run_at_before.get()) == 1);
  std::string v1_run_at = PQgetvalue(run_at_before.get(), 0, 0);

  auto v2_blob = read_fixture("upgrade-v2"); // notes 2.0.0
  REQUIRE(!v2_blob.empty());
  auto v2 = plinth::packages::upgrade_package(v2_blob, v1->id, s.ctx);
  REQUIRE(v2.has_value());

  REQUIRE(wait_rbac_test(s.conn, v2->new_record.id));
  REQUIRE(pkg_state(s.conn, v2->new_record.id) == "ACTIVE");

  // Old row is SUPERSEDED, and its RBAC test trail is frozen.
  REQUIRE(pkg_state(s.conn, v1->id) == "SUPERSEDED");
  auto v1_result_after = pkg_result(s.conn, v1->id);
  REQUIRE(v1_result_after == v1_result_before);
  auto run_at_after =
      exec_params(s.conn,
                  "SELECT last_rbac_test_run_at::text FROM plinth.packages "
                  "WHERE id = $1::uuid",
                  {v1->id});
  REQUIRE(PQntuples(run_at_after.get()) == 1);
  REQUIRE(std::string{PQgetvalue(run_at_after.get(), 0, 0)} == v1_run_at);
}

TEST_CASE("PB.15 sequential re-runs each acquire fresh advisory lock",
          "[rbac_test][integration]") {
  // RBAC test's lock is `pg_try_advisory_lock` (non-blocking) — concurrent
  // contention returns `lock_failed` rather than queuing. This test
  // exercises the lifecycle invariant Slice B cares about: back-to-back
  // runs (each fully drained before the next fires) both succeed and
  // emit distinct audit events. CLI-driven contention is Slice C's
  // ICD-0.4.7 PB.15 proper; that needs the synchronous entry point.
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");

  plinth::packages::rbac_test::schedule_rbac_test(pid, s.ctx, "install");
  REQUIRE(wait_rbac_test_audit_count(s.conn, pid, 1) == 1);

  // Clear last_rbac_test_run_at so wait_rbac_test can tell when run 2 lands.
  auto reset =
      exec_params(s.conn,
                  "UPDATE plinth.packages SET last_rbac_test_run_at = NULL "
                  "WHERE id = $1::uuid",
                  {pid});
  REQUIRE(PQresultStatus(reset.get()) == PGRES_COMMAND_OK);

  plinth::packages::rbac_test::schedule_rbac_test(pid, s.ctx, "cli");
  REQUIRE(wait_rbac_test_audit_count(s.conn, pid, 2) == 2);

  auto res = exec_params(
      s.conn,
      "SELECT COUNT(DISTINCT detail->>'run_id') FROM plinth.audit_log "
      "WHERE detail->>'package_id' = $1 "
      "  AND action IN "
      "('packages.rbac_test_passed','packages.rbac_test_failed')",
      {pid});
  REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::stol(PQgetvalue(res.get(), 0, 0)) == 2);
}

TEST_CASE("PB.16 shutdown owns and drains a timed capability worker",
          "[rbac_test][integration][lifecycle]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  REQUIRE(plinth::packages::rbac_test::shutdown_async_workers(
      std::chrono::seconds{2}));
  REQUIRE(plinth::packages::rbac_test::start_async_workers());

  Scratch s;
  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.block", "notes", "notes",
            test_contract_for("notes:1:block"));

  auto handler = std::make_shared<BlockingHandlerState>();
  AsyncWorkersRestore restore{handler};
  plinth::capabilities::register_tier1_handler(
      "notes:1:block", "notes.block",
      [handler](const Json::Value&, const plinth::capabilities::UserContext&,
                int) -> plinth::capabilities::HandlerOutcome {
        std::unique_lock lock(handler->mu);
        handler->entered = true;
        handler->cv.notify_all();
        handler->cv.wait(lock, [&] { return handler->released; });
        return Json::Value{Json::objectValue};
      });

  plinth::packages::rbac_test::schedule_rbac_test(pid, s.ctx, "install");
  {
    std::unique_lock lock(handler->mu);
    REQUIRE(handler->cv.wait_for(lock, std::chrono::seconds{2},
                                 [&] { return handler->entered; }));
  }
  REQUIRE(plinth::packages::rbac_test::active_async_worker_count_for_test() >=
          2);
  REQUIRE_FALSE(plinth::packages::rbac_test::shutdown_async_workers(
      std::chrono::milliseconds{0}));

  handler->release();
  REQUIRE(plinth::packages::rbac_test::shutdown_async_workers(
      std::chrono::seconds{2}));
  REQUIRE(plinth::packages::rbac_test::active_async_worker_count_for_test() ==
          0);
  REQUIRE(plinth::packages::rbac_test::start_async_workers());
}

TEST_CASE("PB.07 re-run via CLI clears flag — poisoned deny, fix, re-run",
          "[rbac_test][integration]") {
  // Reproduces ICD-0.4.7 §Entry/Exit exit criterion verbatim: install a
  // package with a broken deny (mirrors PB.02's setup), observe
  // ACTIVE_FLAGGED + `packages.rbac_test_failed`, fix the `group_rules`
  // grant, invoke the CLI entry, observe exit 0 + state ACTIVE + a
  // second `packages.rbac_test_passed` audit row.
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  // Phase 1 — broken deny (mirrors PB.02).
  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");
  grant_everyone(s.conn, "notes.edit"); // poison

  auto first =
      plinth::packages::rbac_test::run_rbac_test(pid, s.ctx, "install");
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first->overall_passed());
  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE_FLAGGED");

  // Phase 2 — admin fixes the grant: remove the poisoned
  // `everyone → notes.edit` row so the denied user's RBAC check fails
  // as intended.
  auto fix = exec_params(s.conn,
                         "DELETE FROM plinth.group_rules gr "
                         "USING plinth.groups g, plinth.rbac_rules r "
                         "WHERE gr.group_id = g.id AND gr.rule_id = r.id "
                         "  AND g.name = 'everyone' AND r.rule = 'notes.edit'",
                         {});
  REQUIRE(PQresultStatus(fix.get()) == PGRES_COMMAND_OK);

  // Phase 3 — invoke the CLI entry. Exit 0 + flag cleared.
  plinth::packages::rbac_test::CliTestRbacOptions opts{
      .extension_name = "notes",
      .json_output = false,
      .run_id = "",
  };
  std::ostringstream out;
  std::ostringstream err;
  int rc =
      plinth::packages::rbac_test::run_cli_test_rbac(opts, s.ctx, out, err);
  REQUIRE(rc == 0);
  REQUIRE(pkg_state(s.conn, pid) == "ACTIVE");

  // Audit stream — exactly one passed row post-fix, one failed row
  // pre-fix.
  auto audit_passed = exec_params(s.conn,
                                  "SELECT COUNT(*) FROM plinth.audit_log "
                                  "WHERE detail->>'package_id' = $1 "
                                  "  AND action = 'packages.rbac_test_passed'",
                                  {pid});
  REQUIRE(PQresultStatus(audit_passed.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::stol(PQgetvalue(audit_passed.get(), 0, 0)) == 1);
  auto audit_failed = exec_params(s.conn,
                                  "SELECT COUNT(*) FROM plinth.audit_log "
                                  "WHERE detail->>'package_id' = $1 "
                                  "  AND action = 'packages.rbac_test_failed'",
                                  {pid});
  REQUIRE(PQresultStatus(audit_failed.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::stol(PQgetvalue(audit_failed.get(), 0, 0)) == 1);
}

TEST_CASE("PB.07b CLI rejects unknown extension with exit 2",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  plinth::packages::rbac_test::CliTestRbacOptions opts{
      .extension_name = "no-such-extension",
      .json_output = false,
      .run_id = "",
  };
  std::ostringstream out;
  std::ostringstream err;
  int rc =
      plinth::packages::rbac_test::run_cli_test_rbac(opts, s.ctx, out, err);
  REQUIRE(rc == 2);
  REQUIRE_FALSE(err.str().empty());
}

TEST_CASE("PB.07c CLI --json emits parseable RbacTestReport",
          "[rbac_test][integration]") {
  if (!pg_available()) {
    SKIP("PG unavailable");
  }
  Scratch s;

  auto pid = seed_package(s.conn, "notes", "1.0.0");
  seed_rule(s.conn, "notes.edit", "notes", "notes",
            test_contract_for("notes:1:edit"));
  register_ext_cap("notes:1:edit", "notes.edit", "notes");

  plinth::packages::rbac_test::CliTestRbacOptions opts{
      .extension_name = "notes",
      .json_output = true,
      .run_id = "",
  };
  std::ostringstream out;
  std::ostringstream err;
  int rc =
      plinth::packages::rbac_test::run_cli_test_rbac(opts, s.ctx, out, err);
  REQUIRE(rc == 0);

  auto j = nlohmann::json::parse(out.str());
  REQUIRE(j["package_name"] == "notes");
  REQUIRE(j["package_version"] == "1.0.0");
  REQUIRE(j["failed"].empty());
  REQUIRE(j.contains("run_id"));
  REQUIRE_FALSE(j["run_id"].get<std::string>().empty());
  REQUIRE(pid == j["package_id"].get<std::string>());
}
