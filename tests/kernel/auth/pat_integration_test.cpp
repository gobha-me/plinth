#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"

// ── Integration test helpers ─────────────────────────────────
// These tests exercise the PAT SQL logic directly via libpq,
// verifying the data layer that the HTTP handlers and middleware
// depend on. Follows the same pattern as auth_integration_test.cpp.

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

// Insert a PAT row and return {pat_id, full_token, token_hash, token_prefix}
struct PatInfo {
  std::string pat_id;
  std::string full_token;
  std::string token_hash;
  std::string token_prefix;
};

auto insert_pat(TestPg& pg, const std::string& user_id, const std::string& name)
    -> PatInfo {
  auto raw_random = plinth::auth::generate_token();
  auto full_token = "plinth_" + raw_random;
  auto token_hash = plinth::auth::sha256_hex(raw_random);
  auto token_prefix = raw_random.substr(0, 8);

  auto res = pg.exec_params(
      "INSERT INTO plinth.pats (user_id, name, token_hash, token_prefix) "
      "VALUES ($1::uuid, $2, $3, $4) RETURNING id",
      {user_id, name, token_hash, token_prefix});

  return {.pat_id = PQgetvalue(res.get(), 0, 0),
          .full_token = full_token,
          .token_hash = token_hash,
          .token_prefix = token_prefix};
}

} // namespace

// ── Token format ─────────────────────────────────────────────

TEST_CASE("PAT token format: plinth_ prefix + 43 chars = 50 total",
          "[pat][unit]") {
  auto raw_random = plinth::auth::generate_token();
  auto full_token = "plinth_" + raw_random;

  REQUIRE(raw_random.size() == 43);
  REQUIRE(full_token.size() == 50);
  REQUIRE(full_token.substr(0, 7) == "plinth_");
}

TEST_CASE("PAT token prefix is first 8 chars of random portion",
          "[pat][unit]") {
  auto raw_random = plinth::auth::generate_token();
  auto token_prefix = raw_random.substr(0, 8);

  REQUIRE(token_prefix.size() == 8);
  REQUIRE(raw_random.substr(0, 8) == token_prefix);
}

TEST_CASE("PAT token hash is SHA-256 of random portion only", "[pat][unit]") {
  auto raw_random = plinth::auth::generate_token();
  auto full_token = "plinth_" + raw_random;
  auto hash_of_random = plinth::auth::sha256_hex(raw_random);
  auto hash_of_full = plinth::auth::sha256_hex(full_token);

  // Hash is of the random part, NOT the full token
  REQUIRE(hash_of_random.size() == 64);
  REQUIRE(hash_of_random != hash_of_full);
}

// ── PAT creation and lookup ──────────────────────────────────

TEST_CASE("PAT creation stores correct hash and prefix", "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "alice", "correct-horse-battery-staple");
  auto pat = insert_pat(pg, user_id, "CI token");

  REQUIRE_FALSE(pat.pat_id.empty());

  // Verify stored values
  auto res =
      pg.exec_params("SELECT token_hash, token_prefix, name FROM plinth.pats "
                     "WHERE id = $1::uuid",
                     {pat.pat_id});

  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 0)} == pat.token_hash);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 1)} == pat.token_prefix);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "CI token");
}

TEST_CASE("PAT lookup by token_hash succeeds for valid PAT",
          "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "bob", "secure-password-123");
  auto pat = insert_pat(pg, user_id, "API token");

  // Simulate the middleware lookup query
  auto res =
      pg.exec_params("SELECT p.id, p.user_id, u.username "
                     "FROM plinth.pats p "
                     "JOIN plinth.users u ON u.id = p.user_id "
                     "WHERE p.token_hash = $1 "
                     "  AND p.revoked_at IS NULL "
                     "  AND (p.expires_at IS NULL OR p.expires_at > NOW()) "
                     "  AND u.disabled_at IS NULL",
                     {pat.token_hash});

  REQUIRE(PQntuples(res.get()) == 1);
  REQUIRE(std::string{PQgetvalue(res.get(), 0, 2)} == "bob");
}

// ── Revoked PAT ──────────────────────────────────────────────

TEST_CASE("Revoked PAT is not found by middleware query",
          "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "charlie", "password-12345");
  auto pat = insert_pat(pg, user_id, "temp token");

  // Revoke it
  pg.exec_params(
      "UPDATE plinth.pats SET revoked_at = NOW() WHERE id = $1::uuid",
      {pat.pat_id});

  auto res = pg.exec_params("SELECT 1 FROM plinth.pats "
                            "WHERE token_hash = $1 AND revoked_at IS NULL",
                            {pat.token_hash});

  REQUIRE(PQntuples(res.get()) == 0);
}

// ── Expired PAT ──────────────────────────────────────────────

TEST_CASE("Expired PAT is not found by middleware query",
          "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "dave", "password-12345");

  auto raw_random = plinth::auth::generate_token();
  auto token_hash = plinth::auth::sha256_hex(raw_random);
  auto token_prefix = raw_random.substr(0, 8);

  // Insert PAT with already-expired timestamp
  pg.exec_params("INSERT INTO plinth.pats "
                 "(user_id, name, token_hash, token_prefix, expires_at) "
                 "VALUES ($1::uuid, $2, $3, $4, NOW() - INTERVAL '1 hour')",
                 {user_id, "expired token", token_hash, token_prefix});

  auto res = pg.exec_params("SELECT 1 FROM plinth.pats "
                            "WHERE token_hash = $1 AND revoked_at IS NULL "
                            "AND (expires_at IS NULL OR expires_at > NOW())",
                            {token_hash});

  REQUIRE(PQntuples(res.get()) == 0);
}

// ── NULL expires_at (never expires) ──────────────────────────

TEST_CASE("PAT with NULL expires_at is valid (never expires)",
          "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "eve", "password-12345");
  auto pat = insert_pat(pg, user_id, "permanent token");

  // Verify expires_at is NULL
  auto check = pg.exec_params(
      "SELECT expires_at FROM plinth.pats WHERE id = $1::uuid", {pat.pat_id});
  REQUIRE(PQgetisnull(check.get(), 0, 0) == 1);

  // Verify middleware query still finds it
  auto res = pg.exec_params("SELECT 1 FROM plinth.pats "
                            "WHERE token_hash = $1 AND revoked_at IS NULL "
                            "AND (expires_at IS NULL OR expires_at > NOW())",
                            {pat.token_hash});

  REQUIRE(PQntuples(res.get()) == 1);
}

// ── Disabled user ────────────────────────────────────────────

TEST_CASE("Disabled user's PAT is rejected by middleware query",
          "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "frank", "password-12345");
  auto pat = insert_pat(pg, user_id, "my token");

  // Disable the user
  pg.exec_params(
      "UPDATE plinth.users SET disabled_at = NOW() WHERE id = $1::uuid",
      {user_id});

  auto res =
      pg.exec_params("SELECT 1 FROM plinth.pats p "
                     "JOIN plinth.users u ON u.id = p.user_id "
                     "WHERE p.token_hash = $1 AND p.revoked_at IS NULL "
                     "AND (p.expires_at IS NULL OR p.expires_at > NOW()) "
                     "AND u.disabled_at IS NULL",
                     {pat.token_hash});

  REQUIRE(PQntuples(res.get()) == 0);
}

// ── last_used_at ─────────────────────────────────────────────

TEST_CASE("PAT last_used_at can be updated", "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "grace", "password-12345");
  auto pat = insert_pat(pg, user_id, "usage tracker");

  // Initially NULL
  auto before = pg.exec_params(
      "SELECT last_used_at FROM plinth.pats WHERE id = $1::uuid", {pat.pat_id});
  REQUIRE(PQgetisnull(before.get(), 0, 0) == 1);

  // Update last_used_at
  pg.exec_params(
      "UPDATE plinth.pats SET last_used_at = NOW() WHERE id = $1::uuid",
      {pat.pat_id});

  auto after = pg.exec_params(
      "SELECT last_used_at FROM plinth.pats WHERE id = $1::uuid", {pat.pat_id});
  REQUIRE(PQgetisnull(after.get(), 0, 0) == 0);
}

// ── Multiple PATs per user ───────────────────────────────────

TEST_CASE("User can have multiple active PATs", "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "heidi", "password-12345");

  insert_pat(pg, user_id, "token one");
  insert_pat(pg, user_id, "token two");
  insert_pat(pg, user_id, "token three");

  auto res = pg.exec_params("SELECT COUNT(*) FROM plinth.pats "
                            "WHERE user_id = $1::uuid AND revoked_at IS NULL",
                            {user_id});

  REQUIRE(std::stoi(PQgetvalue(res.get(), 0, 0)) == 3);
}

// ── Unique tokens ────────────────────────────────────────────

TEST_CASE("Each generated PAT token is unique", "[pat][unit]") {
  auto token1 = "plinth_" + plinth::auth::generate_token();
  auto token2 = "plinth_" + plinth::auth::generate_token();
  auto token3 = "plinth_" + plinth::auth::generate_token();

  REQUIRE(token1 != token2);
  REQUIRE(token2 != token3);
  REQUIRE(token1 != token3);
}

// ── Audit log for PAT operations ─────────────────────────────

TEST_CASE("PAT audit entries can be written", "[pat][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  auto user_id = insert_user(pg, "ivan", "password-12345");
  auto pat = insert_pat(pg, user_id, "audited token");

  pg.exec_params(
      "INSERT INTO plinth.audit_log (action, user_id, detail, node_id) "
      "VALUES ($1, $2::uuid, $3::jsonb, $4)",
      {"pat.created", user_id,
       R"({"pat_name": "audited token", "pat_id": ")" + pat.pat_id + R"("})",
       "test-node"});

  auto res =
      pg.exec_params("SELECT action FROM plinth.audit_log "
                     "WHERE user_id = $1::uuid AND action = 'pat.created'",
                     {user_id});

  REQUIRE(PQntuples(res.get()) == 1);
}
