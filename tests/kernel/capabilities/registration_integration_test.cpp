#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <libpq-fe.h>
#include <memory>
#include <string>
#include <vector>

#include "kernel/capabilities/registration.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/logging.hpp"

// Integration tests for plinth::capabilities registration API — exercises
// the four mutation functions against a live PG. Mirrors the patterns in
// tests/kernel/audit/audit_test.cpp for PG connection + schema reset.

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

// Insert a standalone RBAC rule so we have one to point a registration at.
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

} // namespace

using plinth::capabilities::CapabilityError;
using plinth::capabilities::deregister_capability;
using plinth::capabilities::disable_by_extension;
using plinth::capabilities::enable_by_extension;
using plinth::capabilities::register_capability;

// ── register_capability happy path ───────────────────────────────

TEST_CASE("register_capability inserts row, emits audit, emits NOTIFY",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  // Subscribe on a dedicated connection BEFORE the mutation so we catch
  // the NOTIFY. This mirrors how the 0.2.3 listener will behave.
  TestPg sub(db);
  auto listen_res = sub.exec("LISTEN plinth_capability_changed");
  REQUIRE(PQresultStatus(listen_res.get()) == PGRES_COMMAND_OK);

  auto result = register_capability(db, valid_ext_reg(), {});
  REQUIRE(result.has_value());
  REQUIRE(*result == "terminal:1:shell");

  // Row present.
  auto row = pg.exec_params(
      "SELECT provider_type, extension_name, scope, rbac_rule, enabled "
      "FROM plinth.capabilities WHERE signature = $1",
      {"terminal:1:shell"});
  REQUIRE(PQresultStatus(row.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(row.get()) == 1);
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 0)} == "extension");
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 1)} == "terminal");
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 2)} == "instance");
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 3)} == "terminal.shell.execute");
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 4)} == "t");

  // Audit event present.
  auto audit =
      pg.exec_params("SELECT detail->>'signature' FROM plinth.audit_log "
                     "WHERE action = 'capability.registered' "
                     "  AND detail->>'signature' = $1",
                     {"terminal:1:shell"});
  REQUIRE(PQresultStatus(audit.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(audit.get()) == 1);

  // Drain NOTIFY. PQconsumeInput+PQnotifies on the listener.
  REQUIRE(PQconsumeInput(sub.conn) == 1);
  std::unique_ptr<PGnotify, decltype(&PQfreemem)> notify(PQnotifies(sub.conn),
                                                         PQfreemem);
  REQUIRE(notify != nullptr);
  REQUIRE(std::string{notify->relname} == "plinth_capability_changed");
  std::string payload{notify->extra};
  REQUIRE(payload.find("\"action\":\"register\"") != std::string::npos);
  REQUIRE(payload.find("\"signature\":\"terminal:1:shell\"") !=
          std::string::npos);
  REQUIRE(payload.find("\"scope\":\"instance\"") != std::string::npos);
  REQUIRE(payload.find("\"extension_name\":\"terminal\"") != std::string::npos);
}

// ── register_capability error paths ──────────────────────────────

TEST_CASE("register_capability: duplicate returns capability_exists",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  auto first = register_capability(db, valid_ext_reg(), {});
  REQUIRE(first.has_value());

  auto second = register_capability(db, valid_ext_reg(), {});
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error() == CapabilityError::CAPABILITY_EXISTS);
}

TEST_CASE("register_capability: missing RBAC rule returns rbac_rule_not_found",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  auto result = register_capability(db, valid_ext_reg(), {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::RBAC_RULE_NOT_FOUND);
}

TEST_CASE("register_capability: extension without extension_name rejected",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  auto reg = valid_ext_reg();
  reg.extension_name = std::nullopt;
  auto result = register_capability(db, reg, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::MISSING_EXTENSION_NAME);
}

TEST_CASE("register_capability: extension using kernel namespace rejected",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  auto reg = valid_ext_reg();
  reg.namespace_ = "kernel";
  reg.rbac_rule = "kernel.something";
  auto result = register_capability(db, reg, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::RESERVED_NAMESPACE);
}

TEST_CASE("register_capability: scope=user rejected in 0.2.x",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  auto reg = valid_ext_reg();
  reg.scope = "user";
  auto result = register_capability(db, reg, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::USER_SCOPE_NOT_SUPPORTED);
}

TEST_CASE("register_capability: rule namespace must match capability namespace",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  auto reg = valid_ext_reg();
  reg.rbac_rule = "other.shell.execute";
  auto result = register_capability(db, reg, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::NAMESPACE_MISMATCH);
}

// ── deregister_capability ────────────────────────────────────────

TEST_CASE("deregister_capability removes row, audits, emits NOTIFY",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");

  REQUIRE(register_capability(db, valid_ext_reg(), {}).has_value());

  TestPg sub(db);
  REQUIRE(PQresultStatus(sub.exec("LISTEN plinth_capability_changed").get()) ==
          PGRES_COMMAND_OK);

  auto result = deregister_capability(db, "terminal:1:shell", "instance", {});
  REQUIRE(result.has_value());
  REQUIRE(*result == "terminal:1:shell");

  // Row gone.
  auto row =
      pg.exec_params("SELECT 1 FROM plinth.capabilities WHERE signature = $1",
                     {"terminal:1:shell"});
  REQUIRE(PQntuples(row.get()) == 0);

  // Audit event.
  auto audit = pg.exec_params("SELECT 1 FROM plinth.audit_log "
                              "WHERE action = 'capability.deregistered' "
                              "  AND detail->>'signature' = $1",
                              {"terminal:1:shell"});
  REQUIRE(PQntuples(audit.get()) == 1);

  // NOTIFY payload.
  REQUIRE(PQconsumeInput(sub.conn) == 1);
  std::unique_ptr<PGnotify, decltype(&PQfreemem)> notify(PQnotifies(sub.conn),
                                                         PQfreemem);
  REQUIRE(notify != nullptr);
  std::string payload{notify->extra};
  REQUIRE(payload.find("\"action\":\"deregister\"") != std::string::npos);
  REQUIRE(payload.find("\"signature\":\"terminal:1:shell\"") !=
          std::string::npos);
}

TEST_CASE("deregister_capability: missing row returns capability_not_found",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  auto result = deregister_capability(db, "terminal:1:nope", "instance", {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::CAPABILITY_NOT_FOUND);
}

// ── disable_by_extension / enable_by_extension ───────────────────

TEST_CASE("disable_by_extension / enable_by_extension flip enabled and audit",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);
  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");
  seed_rule(pg, "terminal.ls.execute", "terminal", "terminal");

  // Two capabilities sharing extension_name="terminal".
  auto reg1 = valid_ext_reg();
  auto reg2 = valid_ext_reg();
  reg2.function = "ls";
  reg2.rbac_rule = "terminal.ls.execute";
  REQUIRE(register_capability(db, reg1, {}).has_value());
  REQUIRE(register_capability(db, reg2, {}).has_value());

  TestPg sub(db);
  REQUIRE(PQresultStatus(sub.exec("LISTEN plinth_capability_changed").get()) ==
          PGRES_COMMAND_OK);

  // Disable.
  auto disabled = disable_by_extension(db, "terminal", {});
  REQUIRE(disabled.has_value());
  auto row = pg.exec_params("SELECT COUNT(*) FROM plinth.capabilities "
                            "WHERE extension_name = $1 AND enabled = false",
                            {"terminal"});
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 0)} == "2");

  auto audit_disabled =
      pg.exec_params("SELECT detail->>'count' FROM plinth.audit_log "
                     "WHERE action = 'capability.extension_disabled' "
                     "  AND detail->>'extension_name' = $1",
                     {"terminal"});
  REQUIRE(PQntuples(audit_disabled.get()) == 1);
  REQUIRE(std::string{PQgetvalue(audit_disabled.get(), 0, 0)} == "2");

  REQUIRE(PQconsumeInput(sub.conn) == 1);
  std::unique_ptr<PGnotify, decltype(&PQfreemem)> n1(PQnotifies(sub.conn),
                                                     PQfreemem);
  REQUIRE(n1 != nullptr);
  REQUIRE(std::string{n1->extra}.find("\"action\":\"disable\"") !=
          std::string::npos);

  // Re-enable.
  auto enabled = enable_by_extension(db, "terminal", {});
  REQUIRE(enabled.has_value());
  auto row2 = pg.exec_params("SELECT COUNT(*) FROM plinth.capabilities "
                             "WHERE extension_name = $1 AND enabled = true",
                             {"terminal"});
  REQUIRE(std::string{PQgetvalue(row2.get(), 0, 0)} == "2");

  auto audit_enabled =
      pg.exec_params("SELECT detail->>'count' FROM plinth.audit_log "
                     "WHERE action = 'capability.extension_enabled' "
                     "  AND detail->>'extension_name' = $1",
                     {"terminal"});
  REQUIRE(PQntuples(audit_enabled.get()) == 1);
  REQUIRE(std::string{PQgetvalue(audit_enabled.get(), 0, 0)} == "2");

  REQUIRE(PQconsumeInput(sub.conn) == 1);
  std::unique_ptr<PGnotify, decltype(&PQfreemem)> n2(PQnotifies(sub.conn),
                                                     PQfreemem);
  REQUIRE(n2 != nullptr);
  REQUIRE(std::string{n2->extra}.find("\"action\":\"enable\"") !=
          std::string::npos);
}

TEST_CASE("disable_by_extension with empty extension_name rejected",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  auto result = disable_by_extension(db, "", {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CapabilityError::MISSING_EXTENSION_NAME);
}

// ── unregister_capability (0.4.5 transactional) ────────────────────

TEST_CASE("unregister_capability deletes matching row + fires NOTIFY per scope",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg pg(db);
  seed_rule(pg, "terminal.shell.execute", "terminal", "terminal");
  REQUIRE(register_capability(db, valid_ext_reg(), {}).has_value());

  TestPg sub(db);
  REQUIRE(PQresultStatus(sub.exec("LISTEN plinth_capability_changed").get()) ==
          PGRES_COMMAND_OK);

  TestPg tx(db);
  REQUIRE(PQresultStatus(tx.exec("BEGIN").get()) == PGRES_COMMAND_OK);
  auto result = plinth::capabilities::unregister_capability("terminal", 1,
                                                            "shell", *tx.conn);
  REQUIRE(result.has_value());
  REQUIRE(PQresultStatus(tx.exec("COMMIT").get()) == PGRES_COMMAND_OK);

  // Row deleted.
  auto row = pg.exec_params(
      "SELECT COUNT(*) FROM plinth.capabilities "
      "WHERE namespace='terminal' AND version=1 AND function='shell'",
      {});
  REQUIRE(PQresultStatus(row.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::string{PQgetvalue(row.get(), 0, 0)} == "0");

  // NOTIFY received with action=deregister. Consume any pending.
  PQconsumeInput(sub.conn);
  std::unique_ptr<PGnotify, decltype(&PQfreemem)> notify(PQnotifies(sub.conn),
                                                         PQfreemem);
  REQUIRE(notify != nullptr);
  std::string payload{notify->extra};
  REQUIRE(payload.find("\"action\":\"deregister\"") != std::string::npos);
  REQUIRE(payload.find("\"signature\":\"terminal:1:shell\"") !=
          std::string::npos);
}

TEST_CASE("unregister_capability on missing row is a no-op (idempotent)",
          "[capabilities][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  auto db = pg_config();
  reset_schema(db);

  TestPg tx(db);
  REQUIRE(PQresultStatus(tx.exec("BEGIN").get()) == PGRES_COMMAND_OK);
  auto result = plinth::capabilities::unregister_capability("nonexistent", 1,
                                                            "nope", *tx.conn);
  REQUIRE(result.has_value());
  REQUIRE(PQresultStatus(tx.exec("COMMIT").get()) == PGRES_COMMAND_OK);
}
