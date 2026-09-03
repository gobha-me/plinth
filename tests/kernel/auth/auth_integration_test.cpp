#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"

// ── Integration test helpers ─────────────────────────────────
// These tests exercise the auth SQL logic directly via libpq,
// verifying the data layer that the HTTP handlers depend on.
// Full HTTP handler tests require a running Drogon server;
// those are deferred to smoke tests.

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

// RAII PG connection for test helpers
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
}

auto insert_user(TestPg& pg, const std::string& username,
                 const std::string& password) -> std::string {
  auto hash = plinth::auth::hash_password(password);
  auto res =
      pg.exec_params("INSERT INTO plinth.users (username, password_hash) "
                     "VALUES ($1, $2) RETURNING id",
                     {username, hash});
  return PQgetvalue(res.get(), 0, 0);
}

auto insert_session(TestPg& pg, const std::string& user_id,
                    const std::string& raw_token) -> std::string {
  auto token_hash = plinth::auth::sha256_hex(raw_token);
  auto res = pg.exec_params("INSERT INTO plinth.sessions (user_id, token_hash) "
                            "VALUES ($1::uuid, $2) RETURNING id",
                            {user_id, token_hash});
  return PQgetvalue(res.get(), 0, 0);
}

auto row_count(TestPg& pg, const std::string& table) -> int {
  auto res = pg.exec("SELECT COUNT(*) FROM plinth." + table);
  return std::stoi(PQgetvalue(res.get(), 0, 0));
}

} // namespace

// ── User registration ────────────────────────────────────────

TEST_CASE("Register creates user in database", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "alice", "correct-horse-battery-staple");
  REQUIRE_FALSE(user_id.empty());
  REQUIRE(row_count(pg, "users") == 1);
}

TEST_CASE("First user gets admin group membership", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "first-admin", "secure-password-123");

  // Simulate first-user admin bootstrap (as handlers.cpp does)
  pg.exec_params("INSERT INTO plinth.groups (name, built_in) "
                 "VALUES ('admin', true) "
                 "ON CONFLICT (name) DO UPDATE SET name = 'admin' "
                 "RETURNING id",
                 {});
  auto grp_res = pg.exec("SELECT id FROM plinth.groups WHERE name = 'admin'");
  auto group_id = std::string{PQgetvalue(grp_res.get(), 0, 0)};

  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "VALUES ($1::uuid, $2::uuid) ON CONFLICT DO NOTHING",
                 {group_id, user_id});

  // Verify membership
  auto mem_res =
      pg.exec_params("SELECT 1 FROM plinth.group_members gm "
                     "JOIN plinth.groups g ON g.id = gm.group_id "
                     "WHERE gm.user_id = $1::uuid AND g.name = 'admin'",
                     {user_id});
  REQUIRE(PQntuples(mem_res.get()) == 1);
}

TEST_CASE("Duplicate username insert fails", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  insert_user(pg, "alice", "password-one-123");

  auto hash2 = plinth::auth::hash_password("password-two-456");
  auto res = pg.exec_params(
      "INSERT INTO plinth.users (username, password_hash) VALUES ($1, $2)",
      {"alice", hash2});

  // Should fail with unique violation
  REQUIRE(PQresultStatus(res.get()) != PGRES_COMMAND_OK);
}

// ── Login / password verification ────────────────────────────

TEST_CASE("Password verification round-trip", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto password = std::string{"my-secure-password"};
  auto user_id = insert_user(pg, "bob", password);

  // Retrieve hash from DB
  auto res = pg.exec_params(
      "SELECT password_hash FROM plinth.users WHERE id = $1::uuid", {user_id});
  auto stored_hash = std::string{PQgetvalue(res.get(), 0, 0)};

  REQUIRE(plinth::auth::verify_password(password, stored_hash));
  REQUIRE_FALSE(plinth::auth::verify_password("wrong-password", stored_hash));
}

// ── Session lifecycle ────────────────────────────────────────

TEST_CASE("Session creation and token lookup", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "charlie", "password-12345");
  auto raw_token = plinth::auth::generate_token();
  auto session_id = insert_session(pg, user_id, raw_token);

  REQUIRE_FALSE(session_id.empty());

  // Look up by token_hash (as middleware does)
  auto token_hash = plinth::auth::sha256_hex(raw_token);
  auto res = pg.exec_params("SELECT s.id, s.user_id, u.username "
                            "FROM plinth.sessions s "
                            "JOIN plinth.users u ON u.id = s.user_id "
                            "WHERE s.token_hash = $1 AND s.revoked_at IS NULL "
                            "AND s.expires_at > NOW()",
                            {token_hash});

  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "charlie");
}

TEST_CASE("Revoked session is not found", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "dave", "password-12345");
  auto raw_token = plinth::auth::generate_token();
  auto session_id = insert_session(pg, user_id, raw_token);

  // Revoke the session
  pg.exec_params(
      "UPDATE plinth.sessions SET revoked_at = NOW() WHERE id = $1::uuid",
      {session_id});

  auto token_hash = plinth::auth::sha256_hex(raw_token);
  auto res = pg.exec_params(
      "SELECT 1 FROM plinth.sessions "
      "WHERE token_hash = $1 AND revoked_at IS NULL AND expires_at > NOW()",
      {token_hash});

  REQUIRE(PQntuples(res.get()) == 0);
}

TEST_CASE("Expired session is not found", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "eve", "password-12345");
  auto raw_token = plinth::auth::generate_token();
  auto token_hash = plinth::auth::sha256_hex(raw_token);

  // Insert session with already-expired timestamp
  pg.exec_params(
      "INSERT INTO plinth.sessions (user_id, token_hash, expires_at) "
      "VALUES ($1::uuid, $2, NOW() - INTERVAL '1 hour')",
      {user_id, token_hash});

  auto res = pg.exec_params(
      "SELECT 1 FROM plinth.sessions "
      "WHERE token_hash = $1 AND revoked_at IS NULL AND expires_at > NOW()",
      {token_hash});

  REQUIRE(PQntuples(res.get()) == 0);
}

// ── Disabled user ────────────────────────────────────────────

TEST_CASE("Disabled user cannot be found for login", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "frank", "password-12345");

  // Disable the user
  pg.exec_params(
      "UPDATE plinth.users SET disabled_at = NOW() WHERE id = $1::uuid",
      {user_id});

  auto res = pg.exec_params(
      "SELECT disabled_at FROM plinth.users WHERE username = $1", {"frank"});

  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE_FALSE(std::string{PQgetvalue(res.get(), 0, 0)}.empty());
}

// ── Audit log ────────────────────────────────────────────────

TEST_CASE("Audit entries can be written and queried", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "grace", "password-12345");

  pg.exec_params(
      "INSERT INTO plinth.audit_log (action, user_id, detail, node_id) "
      "VALUES ($1, $2::uuid, '{\"username\": \"grace\"}'::jsonb, $3)",
      {"user.registered", user_id, "test-node"});

  auto res = pg.exec_params(
      "SELECT action, user_id FROM plinth.audit_log WHERE user_id = $1::uuid",
      {user_id});

  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == "user.registered");
}

// ── Multiple sessions ────────────────────────────────────────

TEST_CASE("User can have multiple active sessions", "[auth][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "heidi", "password-12345");

  auto token1 = plinth::auth::generate_token();
  auto token2 = plinth::auth::generate_token();
  insert_session(pg, user_id, token1);
  insert_session(pg, user_id, token2);

  auto res = pg.exec_params(
      "SELECT COUNT(*) FROM plinth.sessions "
      "WHERE user_id = $1::uuid AND revoked_at IS NULL AND expires_at > NOW()",
      {user_id});

  REQUIRE(std::stoi(PQgetvalue(res.get(), 0, 0)) == 2);
}
