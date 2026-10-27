#include "kernel/db/bootstrap.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"
#include "kernel/extension_http/dispatch_store.hpp"
#include "kernel/packages/http_claims.hpp"

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

auto ensure_artifact_receipts_schema(const PgConnection& pg) -> void {
  // Retained installations do not rerun schema.sql. Create only the new empty
  // table; do not compute or backfill provenance for existing package rows.
  // Compare against a temporary independently declared expected schema so a
  // same-named weakened/preexisting table cannot silently establish receipts.
  constexpr std::string_view DEFINITION = R"sql((
    package_id UUID NOT NULL REFERENCES plinth.packages(id) ON DELETE CASCADE,
    profile INTEGER NOT NULL CHECK (profile = 1),
    artifact_digest TEXT NOT NULL CHECK (artifact_digest ~ '^[0-9a-f]{64}$'),
    declarations_digest TEXT NOT NULL CHECK (declarations_digest ~ '^[0-9a-f]{64}$'),
    record_count INTEGER NOT NULL CHECK (record_count >= 0 AND record_count <= 4096),
    content_bytes BIGINT NOT NULL CHECK (content_bytes >= 0 AND content_bytes <= 52428800),
    metadata_bytes INTEGER NOT NULL CHECK (metadata_bytes >= 32 AND metadata_bytes <= 1048576),
    PRIMARY KEY (package_id, profile)
  ))sql";
  pg.exec("BEGIN");
  pg.exec("CREATE TABLE IF NOT EXISTS plinth.package_artifact_receipts " +
          std::string{DEFINITION});
  // PostgreSQL forbids a temporary table's FK to a permanent relation. Compare
  // ordinary columns/checks/PK against the temp template and verify the real
  // cascading FK independently below, without changing the live definition.
  auto temporary_definition = std::string{DEFINITION};
  const std::string reference =
      "REFERENCES plinth.packages(id) ON DELETE CASCADE";
  temporary_definition.erase(temporary_definition.find(reference),
                             reference.size());
  pg.exec("CREATE TEMP TABLE plinth_expected_artifact_receipts " +
          temporary_definition + " ON COMMIT DROP");
  if (!pg.has_rows(R"sql(
    SELECT 1 FROM pg_class actual
    JOIN pg_class packages ON packages.oid='plinth.packages'::regclass
    WHERE actual.oid='plinth.package_artifact_receipts'::regclass
      AND actual.relkind='r' AND actual.relpersistence='p'
      AND actual.relowner=packages.relowner
      AND NOT actual.relrowsecurity AND NOT actual.relforcerowsecurity
      AND NOT actual.relispartition
      AND NOT EXISTS (
        SELECT 1 FROM pg_inherits
        WHERE inhrelid=actual.oid OR inhparent=actual.oid)
      AND NOT EXISTS (
        SELECT 1 FROM pg_rewrite WHERE ev_class=actual.oid)
      AND NOT EXISTS (
        SELECT 1 FROM aclexplode(COALESCE(actual.relacl,
                   acldefault('r',actual.relowner))) acl
        WHERE acl.grantee<>actual.relowner)
      AND NOT EXISTS (
        SELECT 1 FROM pg_trigger WHERE tgrelid=actual.oid AND NOT tgisinternal)
      AND (SELECT count(*) FROM pg_constraint c
           WHERE c.conrelid=actual.oid AND c.contype='f')=1
      AND EXISTS (
        SELECT 1 FROM pg_constraint c
        JOIN pg_attribute p ON p.attrelid=packages.oid AND p.attname='id'
        WHERE c.conrelid=actual.oid AND c.contype='f'
          AND c.conkey=ARRAY[1]::smallint[] AND c.confkey=ARRAY[p.attnum]
          AND c.confrelid=packages.oid AND c.convalidated
          AND NOT c.condeferrable AND NOT c.condeferred
          AND c.confdeltype='c' AND c.confupdtype='a' AND c.confmatchtype='s'
          AND (SELECT count(*) FROM pg_trigger t WHERE t.tgconstraint=c.oid)=4
          AND NOT EXISTS (SELECT 1 FROM pg_trigger t
                          WHERE t.tgconstraint=c.oid AND
                                (NOT t.tgisinternal OR t.tgenabled NOT IN ('O','A'))))
      AND EXISTS (
        SELECT 1 FROM pg_constraint c JOIN pg_index i ON i.indexrelid=c.conindid
        WHERE c.conrelid=actual.oid AND c.contype='p'
          AND i.indisvalid AND i.indisready AND i.indisunique
          AND i.indisprimary AND i.indimmediate
          AND i.indpred IS NULL AND i.indexprs IS NULL)
  )sql")) {
    throw std::runtime_error("incompatible artifact receipt table ownership");
  }
  if (pg.has_rows(R"sql(
    WITH columns AS (
      SELECT a.attrelid, a.attnum, a.attname,
             format_type(a.atttypid,a.atttypmod) AS type,
             a.attnotnull, a.attgenerated, a.attidentity, a.attisdropped,
             pg_get_expr(d.adbin,d.adrelid) AS default_value,
             a.attacl
      FROM pg_attribute a LEFT JOIN pg_attrdef d
        ON d.adrelid=a.attrelid AND d.adnum=a.attnum
      WHERE a.attnum>0 AND a.attrelid IN (
        'plinth.package_artifact_receipts'::regclass,
        'pg_temp.plinth_expected_artifact_receipts'::regclass)
    ), actual AS (
      SELECT attnum,attname,type,attnotnull,attgenerated,attidentity,
             attisdropped,default_value,attacl
      FROM columns WHERE attrelid='plinth.package_artifact_receipts'::regclass
    ), expected AS (
      SELECT attnum,attname,type,attnotnull,attgenerated,attidentity,
             attisdropped,default_value,attacl
      FROM columns WHERE attrelid='pg_temp.plinth_expected_artifact_receipts'::regclass
    )
    (SELECT * FROM actual EXCEPT SELECT * FROM expected)
    UNION ALL (SELECT * FROM expected EXCEPT SELECT * FROM actual)
  )sql") ||
      pg.has_rows(R"sql(
    WITH actual AS (
      SELECT contype,convalidated,condeferrable,condeferred,
             pg_get_constraintdef(oid,true) AS definition
      FROM pg_constraint WHERE conrelid='plinth.package_artifact_receipts'::regclass
        AND contype<>'f'
    ), expected AS (
      SELECT contype,convalidated,condeferrable,condeferred,
             pg_get_constraintdef(oid,true) AS definition
      FROM pg_constraint WHERE conrelid='pg_temp.plinth_expected_artifact_receipts'::regclass
    )
    (SELECT * FROM actual EXCEPT SELECT * FROM expected)
    UNION ALL (SELECT * FROM expected EXCEPT SELECT * FROM actual)
  )sql")) {
    throw std::runtime_error("incompatible artifact receipt storage contract");
  }
  pg.exec("COMMIT");
}

auto ensure_typed_panel_schema(const PgConnection& pg) -> void {
  // Existing declaration JSON is never evidence of validation. The additive
  // marker defaults to legacy and no migration interprets old metadata.
  const bool marker_existed =
      pg.has_rows("SELECT 1 FROM information_schema.columns WHERE "
                  "table_schema='plinth' AND table_name='panels' AND "
                  "column_name='validated_format_version'");
  const bool allocator_existed = pg.has_rows(
      "SELECT 1 WHERE to_regclass('plinth.handler_install_order_allocator') "
      "IS NOT NULL");
  if (marker_existed && !allocator_existed) {
    throw std::runtime_error("retained handler order allocator is missing");
  }
  pg.exec("BEGIN");
  pg.exec("ALTER TABLE plinth.panels ADD COLUMN IF NOT EXISTS "
          "validated_format_version SMALLINT NOT NULL DEFAULT 0");
  pg.exec("ALTER TABLE plinth.panels ADD COLUMN IF NOT EXISTS "
          "logical_install_order NUMERIC(20,0)");
  pg.exec("DO $upgrade$ BEGIN "
          "IF NOT EXISTS (SELECT 1 FROM pg_constraint "
          "WHERE conrelid='plinth.panels'::regclass AND "
          "conname='chk_panels_validated_format_version') THEN "
          "ALTER TABLE plinth.panels ADD CONSTRAINT "
          "chk_panels_validated_format_version CHECK "
          "(validated_format_version IN (0,2)); END IF; "
          "IF NOT EXISTS (SELECT 1 FROM pg_constraint "
          "WHERE conrelid='plinth.panels'::regclass AND "
          "conname='chk_panels_logical_install_order') THEN "
          "ALTER TABLE plinth.panels ADD CONSTRAINT "
          "chk_panels_logical_install_order CHECK "
          "(logical_install_order IS NULL OR "
          "(validated_format_version=2 AND panel_type='float' AND "
          "logical_install_order>=1 AND "
          "logical_install_order<=18446744073709551615)); "
          "END IF; END $upgrade$");
  pg.exec("CREATE TABLE IF NOT EXISTS "
          "plinth.handler_install_order_allocator ("
          "id SMALLINT PRIMARY KEY DEFAULT 1, "
          "last_value NUMERIC(20,0) NOT NULL DEFAULT 0, "
          "CONSTRAINT chk_handler_install_order_allocator_id CHECK(id=1), "
          "CONSTRAINT chk_handler_install_order_allocator_value CHECK "
          "(last_value>=0 AND last_value<=18446744073709551615))");

  const auto ordinary_column =
      " AND NOT a.attisdropped AND a.attgenerated='' AND a.attidentity='' ";
  const auto column_query =
      "SELECT 1 FROM pg_attribute a LEFT JOIN pg_attrdef d "
      "ON d.adrelid=a.attrelid AND d.adnum=a.attnum WHERE ";
  if (!pg.has_rows(std::string{column_query} +
                   "a.attrelid='plinth.panels'::regclass "
                   "AND a.attname='validated_format_version' " +
                   ordinary_column +
                   "AND a.atttypid='smallint'::regtype AND a.attnotnull "
                   "AND pg_get_expr(d.adbin,d.adrelid)='0'") ||
      !pg.has_rows(std::string{column_query} +
                   "a.attrelid='plinth.panels'::regclass "
                   "AND a.attname='logical_install_order' " +
                   ordinary_column +
                   "AND format_type(a.atttypid,a.atttypmod)='numeric(20,0)' "
                   "AND NOT a.attnotnull AND d.oid IS NULL") ||
      !pg.has_rows(
          std::string{column_query} +
          "a.attrelid='plinth.handler_install_order_allocator'::regclass "
          "AND a.attname='id' " +
          ordinary_column +
          "AND a.atttypid='smallint'::regtype AND a.attnotnull "
          "AND pg_get_expr(d.adbin,d.adrelid)='1'") ||
      !pg.has_rows(
          std::string{column_query} +
          "a.attrelid='plinth.handler_install_order_allocator'::regclass "
          "AND a.attname='last_value' " +
          ordinary_column +
          "AND format_type(a.atttypid,a.atttypmod)='numeric(20,0)' "
          "AND a.attnotnull AND pg_get_expr(d.adbin,d.adrelid)='0'")) {
    throw std::runtime_error("incompatible typed panel storage columns");
  }
  // Compare the actual validated expressions, not only constraint names.
  // PostgreSQL adds formatting parentheses and implicit numeric/text casts.
  const auto constraint_query =
      "SELECT 1 FROM pg_constraint c WHERE c.contype='c' AND c.convalidated "
      "AND regexp_replace(replace(pg_get_expr(c.conbin,c.conrelid), "
      "chr(39),''),'[[:space:]()]|::numeric|::text','','g')=";
  if (!pg.has_rows(std::string{constraint_query} +
                   "'validated_format_version=ANYARRAY[0,2]' "
                   "AND c.conrelid='plinth.panels'::regclass "
                   "AND c.conname='chk_panels_validated_format_version'") ||
      !pg.has_rows(std::string{constraint_query} +
                   "'logical_install_orderISNULLORvalidated_format_version=2AND"
                   "panel_type=floatANDlogical_install_order>=1AND"
                   "logical_install_order<=18446744073709551615' "
                   "AND c.conrelid='plinth.panels'::regclass "
                   "AND c.conname='chk_panels_logical_install_order'") ||
      !pg.has_rows(std::string{constraint_query} +
                   "'id=1' AND c.conrelid="
                   "'plinth.handler_install_order_allocator'::regclass "
                   "AND c.conname='chk_handler_install_order_allocator_id'") ||
      !pg.has_rows(
          std::string{constraint_query} +
          "'last_value>=0ANDlast_value<=18446744073709551615' "
          "AND c.conrelid='plinth.handler_install_order_allocator'::regclass "
          "AND c.conname='chk_handler_install_order_allocator_value'")) {
    throw std::runtime_error("incompatible typed panel storage constraints");
  }
  if (!pg.has_rows(
          "SELECT 1 FROM pg_constraint c JOIN pg_attribute a "
          "ON a.attrelid=c.conrelid AND a.attnum=c.conkey[1] "
          "WHERE c.conrelid='plinth.handler_install_order_allocator'::regclass "
          "AND c.contype='p' AND cardinality(c.conkey)=1 AND a.attname='id'")) {
    throw std::runtime_error("incompatible handler order allocator identity");
  }
  // Only a truly new migration seeds zero. A missing retained singleton cannot
  // be repaired from surviving ranks: GC may have removed the highest one.
  if (!allocator_existed) {
    pg.exec(
        "INSERT INTO plinth.handler_install_order_allocator (id,last_value) "
        "VALUES (1,0)");
  }
  if (!pg.has_rows("SELECT 1 WHERE (SELECT count(*) FROM "
                   "plinth.handler_install_order_allocator)=1 AND EXISTS "
                   "(SELECT 1 FROM plinth.handler_install_order_allocator "
                   "WHERE id=1)")) {
    throw std::runtime_error("retained handler order allocator is missing");
  }
  pg.exec("COMMIT");
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

  // Existing rows remain ordinary JSONB. Only explicitly marked new rows may
  // contain serialized detail, so historical caller data cannot impersonate an
  // encoding marker. Run before any startup seeding/writer or audit route.
  if (!pg.has_rows("SELECT 1 FROM information_schema.columns "
                   "WHERE table_schema='plinth' AND table_name='audit_log' "
                   "AND column_name='detail_encoding'")) {
    pg.exec("ALTER TABLE plinth.audit_log ADD COLUMN IF NOT EXISTS "
            "detail_encoding SMALLINT NOT NULL DEFAULT 0 "
            "CONSTRAINT chk_audit_log_detail_encoding "
            "CHECK (detail_encoding IN (0, 1))");
  }
  if (!pg.has_rows(
          "SELECT 1 FROM pg_attribute a "
          "JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum "
          "JOIN pg_constraint c ON c.conrelid=a.attrelid "
          "WHERE a.attrelid='plinth.audit_log'::regclass "
          "AND a.attname='detail_encoding' AND NOT a.attisdropped "
          "AND a.attgenerated='' AND a.attidentity='' "
          "AND a.atttypid='smallint'::regtype AND a.attnotnull "
          "AND pg_get_expr(d.adbin,d.adrelid)='0' "
          "AND c.conname='chk_audit_log_detail_encoding' "
          "AND c.contype='c' AND c.convalidated "
          "AND pg_get_expr(c.conbin,c.conrelid)="
          "'(detail_encoding = ANY (ARRAY[0, 1]))'")) {
    throw std::runtime_error("incompatible audit detail encoding schema");
  }

  // Kernel-owned, idempotent upgrade for the #31 launcher visibility marker.
  // Existing installations skip schema.sql, so additive kernel schema changes
  // must be applied before any startup reconciler or route queries the column.
  const bool launcher_schema_complete = pg.has_rows(
      "SELECT 1 WHERE EXISTS (SELECT 1 FROM information_schema.columns "
      "WHERE table_schema='plinth' AND table_name='packages' "
      "AND column_name='application_ready') "
      "AND to_regclass('plinth.uniq_packages_name_application_ready') "
      "IS NOT NULL AND EXISTS (SELECT 1 FROM pg_constraint "
      "WHERE conrelid='plinth.packages'::regclass "
      "AND conname='chk_packages_application_ready_state')");
  if (!launcher_schema_complete) {
    pg.exec("BEGIN");
    pg.exec("ALTER TABLE plinth.packages ADD COLUMN IF NOT EXISTS "
            "application_ready BOOLEAN NOT NULL DEFAULT FALSE");
    pg.exec("UPDATE plinth.packages SET application_ready = FALSE "
            "WHERE application_ready AND state NOT IN "
            "('ACTIVE','ACTIVE_FLAGGED')");
    pg.exec("CREATE UNIQUE INDEX IF NOT EXISTS "
            "uniq_packages_name_application_ready ON plinth.packages(name) "
            "WHERE application_ready");
    pg.exec("DO $upgrade$ BEGIN "
            "IF NOT EXISTS (SELECT 1 FROM pg_constraint "
            "WHERE conrelid='plinth.packages'::regclass "
            "AND conname='chk_packages_application_ready_state') THEN "
            "ALTER TABLE plinth.packages ADD CONSTRAINT "
            "chk_packages_application_ready_state CHECK "
            "(NOT application_ready OR state IN "
            "('ACTIVE','ACTIVE_FLAGGED')); END IF; END $upgrade$");
    pg.exec("COMMIT");
  }

  ensure_typed_panel_schema(pg);
  ensure_artifact_receipts_schema(pg);

  // Retained legacy manifests stay inert: create/check only empty kernel
  // metadata tables, never reconstruct HTTP authority from package JSON.
  // The store checks independent schema templates on this same transaction.
  pg.exec("BEGIN");
  if (!packages::http_claims::ensure_schema(*pg.conn)) {
    throw std::runtime_error("incompatible HTTP claims schema");
  }
  if (!extension_http::ensure_schema(*pg.conn)) {
    throw std::runtime_error("incompatible HTTP dispatch schema");
  }
  pg.exec("COMMIT");

  // Kernel-owned, idempotent upgrade for #37 invite registration. Existing
  // retained installations skip schema.sql, so create the digest-only invite
  // table before auth routes can accept invite-mode registrations.
  pg.exec("BEGIN");
  pg.exec("CREATE TABLE IF NOT EXISTS plinth.registration_invites ("
          "id UUID PRIMARY KEY DEFAULT gen_random_uuid(), "
          "token_hash TEXT UNIQUE NOT NULL, "
          "created_by_user_id UUID NOT NULL REFERENCES plinth.users(id), "
          "created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(), "
          "expires_at TIMESTAMPTZ NOT NULL, revoked_at TIMESTAMPTZ, "
          "used_at TIMESTAMPTZ, "
          "used_by_user_id UUID REFERENCES plinth.users(id), "
          "CONSTRAINT chk_registration_invites_token_hash "
          "CHECK (token_hash ~ '^[0-9a-f]{64}$'), "
          "CONSTRAINT chk_registration_invites_expiry "
          "CHECK (expires_at > created_at), "
          "CONSTRAINT chk_registration_invites_use_pair "
          "CHECK ((used_at IS NULL) = (used_by_user_id IS NULL)), "
          "CONSTRAINT chk_registration_invites_terminal_state "
          "CHECK (revoked_at IS NULL OR used_at IS NULL))");
  pg.exec("CREATE INDEX IF NOT EXISTS idx_registration_invites_active_expiry "
          "ON plinth.registration_invites(expires_at) "
          "WHERE revoked_at IS NULL AND used_at IS NULL");
  pg.exec("COMMIT");

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
