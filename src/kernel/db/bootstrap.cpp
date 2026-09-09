#include "kernel/db/bootstrap.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include <fstream>
#include <libpq-fe.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace plinth::db {

namespace {

// RAII wrapper for PGconn — closes on destruction
struct PgConnection {
  PGconn* conn = nullptr;

  explicit PgConnection(const std::string& conninfo)
      : conn(plinth::db::connect(conninfo.c_str())) {
    if (PQstatus(conn) != CONNECTION_OK) {
      std::string err = PQerrorMessage(conn);
      PQfinish(conn);
      conn = nullptr;
      throw std::runtime_error("PG connect failed: " + err);
    }
  }

  ~PgConnection() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }

  PgConnection(const PgConnection&) = delete;
  auto operator=(const PgConnection&) -> PgConnection& = delete;
  PgConnection(PgConnection&&) = delete;
  auto operator=(PgConnection&&) -> PgConnection& = delete;

  auto exec(const std::string& sql) const -> void {
    // PQexec returns a non-null PGresult even on error
    std::unique_ptr<PGresult, decltype(&PQclear)> res(
        plinth::db::exec(conn, sql.c_str()), PQclear);

    auto status = PQresultStatus(res.get());
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
      throw std::runtime_error(std::string("PG exec failed: ") +
                               PQresultErrorMessage(res.get()));
    }
  }

  // Execute a query and return true if it returns at least one row
  [[nodiscard]] auto has_rows(const std::string& sql) const -> bool {
    std::unique_ptr<PGresult, decltype(&PQclear)> res(
        plinth::db::exec(conn, sql.c_str()), PQclear);

    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
      throw std::runtime_error(std::string("PG query failed: ") +
                               PQresultErrorMessage(res.get()));
    }
    return PQntuples(res.get()) > 0;
  }

  [[nodiscard]] auto query_strings(const std::string& sql) const
      -> std::vector<std::string> {
    std::unique_ptr<PGresult, decltype(&PQclear)> res(
        plinth::db::exec(conn, sql.c_str()), PQclear);
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
      throw std::runtime_error(std::string("PG query failed: ") +
                               PQresultErrorMessage(res.get()));
    }

    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(PQntuples(res.get())));
    for (int row = 0; row < PQntuples(res.get()); ++row) {
      values.emplace_back(PQgetvalue(res.get(), row, 0));
    }
    return values;
  }

  [[nodiscard]] auto quote_identifier(std::string_view identifier) const
      -> std::string {
    char* quoted =
        PQescapeIdentifier(conn, identifier.data(), identifier.size());
    if (quoted == nullptr) {
      throw std::runtime_error(std::string("PG identifier escaping failed: ") +
                               PQerrorMessage(conn));
    }
    std::string result{quoted};
    PQfreemem(quoted);
    return result;
  }

  [[nodiscard]] auto quote_literal(std::string_view value) const
      -> std::string {
    char* quoted = PQescapeLiteral(conn, value.data(), value.size());
    if (quoted == nullptr) {
      throw std::runtime_error(std::string("PG literal escaping failed: ") +
                               PQerrorMessage(conn));
    }
    std::string result{quoted};
    PQfreemem(quoted);
    return result;
  }
};

auto reset_development_schemas(const PgConnection& pg) -> void {
  std::vector<std::string> extension_roles;
  if (pg.has_rows(
          "SELECT 1 WHERE to_regclass('plinth.extension_database_credentials') "
          "IS NOT NULL")) {
    extension_roles = pg.query_strings(
        "SELECT role_name FROM plinth.extension_database_credentials");
  }
  // Every package migration is constrained to an `ext_<name>` schema and an
  // `ext_<name>_role`. A dev reset that only drops `plinth` leaves those
  // objects behind, so the next first-boot is not actually a clean boot.
  //
  // Dev mode already promises a destructive reset of its dedicated database.
  // Enumerate the reserved schema namespace from PostgreSQL itself so orphaned
  // schemas from a partial install are removed even when no package row
  // survived. Quote every identifier despite the reserved-name filter.
  auto schemas =
      pg.query_strings("SELECT schema_name FROM information_schema.schemata "
                       "WHERE schema_name LIKE 'ext\\_%' ESCAPE '\\' "
                       "ORDER BY schema_name");

  for (const auto& schema : schemas) {
    extension_roles.push_back(schema + "_role");
    pg.exec("DROP SCHEMA IF EXISTS " + pg.quote_identifier(schema) +
            " CASCADE");
  }
  pg.exec("DROP SCHEMA IF EXISTS plinth CASCADE");

  for (const auto& role : extension_roles) {
    auto quoted_role = pg.quote_identifier(role);
    if (pg.has_rows("SELECT 1 FROM pg_roles WHERE rolname = " +
                    pg.quote_literal(role))) {
      pg.exec("DROP OWNED BY " + quoted_role + " CASCADE");
      if (!pg.has_rows(
              "SELECT 1 FROM pg_shdepend d JOIN pg_roles r ON r.oid=d.refobjid "
              "WHERE d.refclassid='pg_authid'::regclass AND r.rolname=" +
              pg.quote_literal(role) +
              " AND d.dbid NOT IN (0, (SELECT oid FROM pg_database WHERE "
              "datname=current_database()))")) {
        pg.exec("DROP ROLE " + quoted_role);
      }
    }
  }
}

} // namespace

auto load_schema_sql(const std::string& migrations_dir) -> std::string {
  auto path = migrations_dir + "/schema.sql";
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("cannot open schema file: " + path);
  }

  std::ostringstream buf;
  buf << file.rdbuf();
  auto sql = buf.str();

  if (sql.empty()) {
    throw std::runtime_error("schema file is empty: " + path);
  }

  return sql;
}

auto bootstrap_schema(const Config::Database& db_cfg,
                      const std::string& migrations_dir, bool dev_mode)
    -> void {
  auto schema_sql = load_schema_sql(migrations_dir);
  auto conninfo = plinth::db::connection_info(db_cfg);

  spdlog::info("connecting to {}:{}/{}", db_cfg.host, db_cfg.port,
               db_cfg.database);
  PgConnection pg(conninfo);
  spdlog::info("connected to PostgreSQL");

  if (dev_mode) {
    spdlog::warn(
        "dev_mode: dropping and recreating plinth and extension schemas");
    reset_development_schemas(pg);
    pg.exec(schema_sql);
    spdlog::info("schema created from schema.sql");
  } else {
    bool exists =
        pg.has_rows("SELECT schema_name FROM information_schema.schemata "
                    "WHERE schema_name = 'plinth'");

    if (exists) {
      spdlog::info("plinth schema already exists — skipping bootstrap");
    } else {
      spdlog::info("fresh install — creating schema from schema.sql");
      pg.exec(schema_sql);
      spdlog::info("schema created");
    }
  }
  std::ifstream isolation_file(migrations_dir + "/extension_database.sql");
  if (!isolation_file.is_open()) {
    throw std::runtime_error(
        "cannot open extension database privilege migration");
  }
  std::ostringstream isolation_sql;
  isolation_sql << isolation_file.rdbuf();
  pg.exec("BEGIN");
  pg.exec(isolation_sql.str());
  for (const auto& extension :
       pg.query_strings("SELECT DISTINCT COALESCE(credentials.extension_name, "
                        "packages.name, substring(n.nspname FROM 5)) "
                        "FROM pg_namespace n LEFT JOIN "
                        "plinth.extension_database_credentials credentials "
                        "ON credentials.schema_name=n.nspname LEFT JOIN "
                        "plinth.packages packages "
                        "ON left('ext_' || packages.name, 63)=n.nspname "
                        "WHERE n.nspname LIKE 'ext\\_%' ESCAPE '\\'")) {
    pg.exec("SELECT plinth.provision_extension_database(" +
            pg.quote_literal(extension) + ")");
  }
  pg.exec("COMMIT");
}

} // namespace plinth::db
