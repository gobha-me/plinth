#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <vector>

#include "kernel/capabilities/bootstrap.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/logging.hpp"

// Integration tests for bootstrap_kernel_capabilities — verifies the
// minimum kernel capability set from ICD-0.2.0 §Bootstrap is seeded
// and is idempotent across restarts.

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

constexpr std::array<const char*, 5> EXPECTED_SIGNATURES = {
    "kernel:1:db.query", "kernel:1:db.exec",    "kernel:1:log",
    "kernel:1:audit",    "kernel:1:config.get",
};

constexpr std::array<const char*, 5> EXPECTED_RULES = {
    "kernel.db.query", "kernel.db.exec",    "kernel.log",
    "kernel.audit",    "kernel.config.get",
};

} // namespace

TEST_CASE("bootstrap_kernel_capabilities seeds ICD minimum set",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  plinth::capabilities::bootstrap_kernel_capabilities(db);

  TestPg pg(db);

  // Rules present with kernel namespace + kernel extension.
  for (const char* rule : EXPECTED_RULES) {
    auto r = pg.exec_params(
        "SELECT namespace, extension_name FROM plinth.rbac_rules "
        "WHERE rule = $1",
        {rule});
    REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(r.get()) == 1);
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "kernel");
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 1)} == "kernel");
  }

  // Capabilities present as kernel-provided, instance-scope, enabled.
  for (const char* sig : EXPECTED_SIGNATURES) {
    auto r =
        pg.exec_params("SELECT provider_type, extension_name, scope, enabled "
                       "FROM plinth.capabilities WHERE signature = $1",
                       {sig});
    REQUIRE(PQresultStatus(r.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(r.get()) == 1);
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "kernel");
    REQUIRE(PQgetisnull(r.get(), 0, 1) == 1); // extension_name IS NULL
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 2)} == "instance");
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 3)} == "t");
  }
}

TEST_CASE("bootstrap_kernel_capabilities emits audit events for new rows",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  plinth::capabilities::bootstrap_kernel_capabilities(db);

  TestPg pg(db);

  // Each new rule → rbac.rule_registered audit.
  for (const char* rule : EXPECTED_RULES) {
    auto r = pg.exec_params("SELECT COUNT(*) FROM plinth.audit_log "
                            "WHERE action = 'rbac.rule_registered' "
                            "  AND detail->>'rule' = $1",
                            {rule});
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "1");
  }

  // Each new capability → capability.registered audit.
  for (const char* sig : EXPECTED_SIGNATURES) {
    auto r = pg.exec_params("SELECT COUNT(*) FROM plinth.audit_log "
                            "WHERE action = 'capability.registered' "
                            "  AND detail->>'signature' = $1",
                            {sig});
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "1");
  }
}

TEST_CASE("bootstrap_kernel_capabilities is idempotent",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  plinth::capabilities::bootstrap_kernel_capabilities(db);
  // Simulate a kernel restart.
  plinth::capabilities::bootstrap_kernel_capabilities(db);

  TestPg pg(db);

  // No duplicate rule rows.
  for (const char* rule : EXPECTED_RULES) {
    auto r = pg.exec_params(
        "SELECT COUNT(*) FROM plinth.rbac_rules WHERE rule = $1", {rule});
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "1");
  }

  // No duplicate capability rows.
  for (const char* sig : EXPECTED_SIGNATURES) {
    auto r = pg.exec_params(
        "SELECT COUNT(*) FROM plinth.capabilities WHERE signature = $1", {sig});
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "1");
  }

  // No duplicate audit events.
  for (const char* sig : EXPECTED_SIGNATURES) {
    auto r = pg.exec_params("SELECT COUNT(*) FROM plinth.audit_log "
                            "WHERE action = 'capability.registered' "
                            "  AND detail->>'signature' = $1",
                            {sig});
    REQUIRE(std::string{PQgetvalue(r.get(), 0, 0)} == "1");
  }
}
