#include "kernel/capabilities/registration.hpp"
#include "kernel/capabilities/validation.hpp"

#include <array>
#include <cstring>
#include <expected>
#include <json/value.h>
#include <json/writer.h>
#include <libpq-fe.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

namespace plinth::capabilities {

namespace {

constexpr const char* CHANGE_CHANNEL = "plinth_capability_changed";
constexpr const char* SQLSTATE_UNIQUE_VIOLATION = "23505";

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

struct PgConn {
  PGconn* conn = nullptr;

  explicit PgConn(const Config::Database& db) {
    conn = PQconnectdb(build_conninfo(db).c_str());
  }
  ~PgConn() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }

  PgConn(const PgConn&) = delete;
  auto operator=(const PgConn&) -> PgConn& = delete;
  PgConn(PgConn&&) = delete;
  auto operator=(PgConn&&) -> PgConn& = delete;

  [[nodiscard]] auto ok() const -> bool {
    return conn != nullptr && PQstatus(conn) == CONNECTION_OK;
  }
};

auto sqlstate(PGresult* res) -> std::string {
  const char* s = PQresultErrorField(res, PG_DIAG_SQLSTATE);
  return s == nullptr ? "" : std::string{s};
}

auto is_unique_violation(PGresult* res) -> bool {
  return sqlstate(res) == SQLSTATE_UNIQUE_VIOLATION;
}

auto json_write(const Json::Value& v) -> std::string {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

// Emit NOTIFY via pg_notify(), which unlike the raw NOTIFY statement
// accepts the payload as a parameter (so we can pass JSON safely).
auto send_notify(PGconn* conn, std::string_view action,
                 std::string_view signature, std::string_view scope,
                 std::optional<std::string_view> extension_name) -> void {
  Json::Value payload(Json::objectValue);
  payload["action"] = std::string{action};
  payload["signature"] = std::string{signature};
  payload["scope"] = std::string{scope};
  if (extension_name.has_value()) {
    payload["extension_name"] = std::string{*extension_name};
  } else {
    payload["extension_name"] = Json::nullValue;
  }
  auto payload_str = json_write(payload);

  std::array<const char*, 2> values = {CHANGE_CHANNEL, payload_str.c_str()};
  PgResultPtr res{PQexecParams(conn, "SELECT pg_notify($1, $2)", 2, nullptr,
                               values.data(), nullptr, nullptr, 0),
                  PQclear};
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::error("NOTIFY {} failed: {}", CHANGE_CHANNEL,
                  PQresultErrorMessage(res.get()));
  }
}

auto rbac_rule_exists(PGconn* conn, const std::string& rule) -> bool {
  std::array<const char*, 1> values = {rule.c_str()};
  PgResultPtr res{
      PQexecParams(conn, "SELECT 1 FROM plinth.rbac_rules WHERE rule = $1", 1,
                   nullptr, values.data(), nullptr, nullptr, 0),
      PQclear};
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::error("rbac_rule_exists probe failed: {}",
                  PQresultErrorMessage(res.get()));
    return false;
  }
  return PQntuples(res.get()) > 0;
}

// `failure()` wraps std::unexpected for call-site locality. The matching
// `success()` helper is not needed: `return std::string{...}` relies on
// std::expected's implicit T-to-expected<T,E> conversion.
auto failure(CapabilityError e) -> RegisterResult {
  return std::unexpected(e);
}

// Shared INSERT + NOTIFY body for both overloads. Precondition: caller
// has validated `reg`. On success returns the canonical signature; on
// failure returns a CapabilityError. Does NOT emit audit — caller
// decides whether to commit+audit (sync overload) or defer (tx
// overload; install lifecycle emits terminal audit after COMMIT).
auto insert_and_notify(PGconn* conn, const CapabilityRegistration& reg,
                       const std::string& signature) -> RegisterResult {
  if (!rbac_rule_exists(conn, reg.rbac_rule)) {
    return failure(CapabilityError::RBAC_RULE_NOT_FOUND);
  }

  // Columns: namespace, version, function, signature, provider_type,
  //          extension_name, scope, description, rbac_rule
  auto version_str = std::to_string(reg.version);
  std::string ext_name = reg.extension_name.value_or(std::string{});
  std::array<const char*, 9> values = {
      reg.namespace_.c_str(),
      version_str.c_str(),
      reg.function.c_str(),
      signature.c_str(),
      reg.provider_type.c_str(),
      ext_name.empty() ? nullptr : ext_name.c_str(), // NULL for kernel
      reg.scope.c_str(),
      reg.description.c_str(),
      reg.rbac_rule.c_str(),
  };
  PgResultPtr res{
      PQexecParams(conn,
                   "INSERT INTO plinth.capabilities "
                   "(namespace, version, function, signature, provider_type, "
                   " extension_name, scope, description, rbac_rule) "
                   "VALUES ($1, $2::int, $3, $4, $5, $6, $7, $8, $9)",
                   9, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear};

  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    if (is_unique_violation(res.get())) {
      return failure(CapabilityError::CAPABILITY_EXISTS);
    }
    spdlog::error("register_capability INSERT failed for {}: {}", signature,
                  PQresultErrorMessage(res.get()));
    return failure(CapabilityError::DB_ERROR);
  }

  send_notify(conn, "register", signature, reg.scope,
              reg.extension_name.has_value()
                  ? std::optional<std::string_view>{*reg.extension_name}
                  : std::optional<std::string_view>{std::nullopt});

  return signature;
}

} // namespace

auto register_capability(const Config::Database& db_cfg,
                         const CapabilityRegistration& reg,
                         const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult {
  if (auto e = validate_registration(reg)) {
    return failure(*e);
  }

  auto signature = make_signature(reg);

  PgConn pg(db_cfg);
  if (!pg.ok()) {
    spdlog::error("register_capability: PG connect failed: {}",
                  PQerrorMessage(pg.conn));
    return failure(CapabilityError::DB_ERROR);
  }

  auto result = insert_and_notify(pg.conn, reg, signature);
  if (!result.has_value()) {
    return result;
  }

  Json::Value detail(Json::objectValue);
  detail["signature"] = signature;
  detail["provider_type"] = reg.provider_type;
  detail["extension_name"] = reg.extension_name.has_value()
                                 ? Json::Value{*reg.extension_name}
                                 : Json::Value{Json::nullValue};
  detail["scope"] = reg.scope;
  detail["rbac_rule"] = reg.rbac_rule;
  plinth::log::audit_sync(db_cfg, "capability.registered", detail);
  static_cast<void>(audit_ctx); // sync path ignores user/session context

  return signature;
}

auto register_capability_tx(PGconn& conn, const CapabilityRegistration& reg)
    -> RegisterResult {
  if (auto e = validate_registration(reg)) {
    return failure(*e);
  }
  auto signature = make_signature(reg);
  return insert_and_notify(&conn, reg, signature);
}

auto unregister_capability(std::string_view ns, int version,
                           std::string_view function, PGconn& conn)
    -> std::expected<void, std::string> {
  std::string ns_s{ns};
  auto version_s = std::to_string(version);
  std::string fn_s{function};
  std::array<const char*, 3> values = {ns_s.c_str(), version_s.c_str(),
                                       fn_s.c_str()};
  // RETURNING captures scope per deleted row so NOTIFY fires precisely
  // for each Tier 2 cache entry that needs eviction.
  PgResultPtr res{
      PQexecParams(
          &conn,
          "DELETE FROM plinth.capabilities "
          "WHERE namespace = $1 AND version = $2::int AND function = $3 "
          "RETURNING scope",
          3, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear};
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  std::ostringstream sig_ss;
  sig_ss << ns_s << ":" << version << ":" << fn_s;
  auto signature = sig_ss.str();
  int rows = PQntuples(res.get());
  for (int i = 0; i < rows; ++i) {
    std::string_view scope{PQgetvalue(res.get(), i, 0)};
    send_notify(&conn, "deregister", signature, scope, std::nullopt);
  }
  return {};
}

auto deregister_capability(const Config::Database& db_cfg,
                           std::string_view signature, std::string_view scope,
                           const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult {
  if (auto e = validate_scope(scope)) {
    return failure(*e);
  }

  PgConn pg(db_cfg);
  if (!pg.ok()) {
    spdlog::error("deregister_capability: PG connect failed: {}",
                  PQerrorMessage(pg.conn));
    return failure(CapabilityError::DB_ERROR);
  }

  std::string signature_str{signature};
  std::string scope_str{scope};
  std::array<const char*, 2> values = {signature_str.c_str(),
                                       scope_str.c_str()};
  PgResultPtr res{PQexecParams(pg.conn,
                               "DELETE FROM plinth.capabilities "
                               "WHERE signature = $1 AND scope = $2",
                               2, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear};
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    spdlog::error("deregister_capability DELETE failed: {}",
                  PQresultErrorMessage(res.get()));
    return failure(CapabilityError::DB_ERROR);
  }
  const char* affected = PQcmdTuples(res.get());
  if (affected == nullptr || std::strcmp(affected, "0") == 0) {
    return failure(CapabilityError::CAPABILITY_NOT_FOUND);
  }

  send_notify(pg.conn, "deregister", signature, scope, std::nullopt);

  Json::Value detail(Json::objectValue);
  detail["signature"] = std::string{signature};
  detail["scope"] = std::string{scope};
  plinth::log::audit_sync(db_cfg, "capability.deregistered", detail);
  static_cast<void>(audit_ctx);

  return std::string{signature};
}

namespace {

// Shared body for enable/disable — both differ only by the target enabled
// value and the audit action name.
auto set_enabled_by_extension(const Config::Database& db_cfg,
                              std::string_view extension_name,
                              bool target_enabled,
                              std::string_view audit_action,
                              std::string_view notify_action,
                              const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult {
  if (extension_name.empty()) {
    return failure(CapabilityError::MISSING_EXTENSION_NAME);
  }

  PgConn pg(db_cfg);
  if (!pg.ok()) {
    spdlog::error("set_enabled_by_extension: PG connect failed: {}",
                  PQerrorMessage(pg.conn));
    return failure(CapabilityError::DB_ERROR);
  }

  std::string enabled_str = target_enabled ? "true" : "false";
  std::string ext_str{extension_name};
  std::array<const char*, 2> values = {enabled_str.c_str(), ext_str.c_str()};
  PgResultPtr res{PQexecParams(pg.conn,
                               "UPDATE plinth.capabilities "
                               "SET enabled = $1::boolean "
                               "WHERE extension_name = $2",
                               2, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear};
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    spdlog::error("set_enabled_by_extension UPDATE failed: {}",
                  PQresultErrorMessage(res.get()));
    return failure(CapabilityError::DB_ERROR);
  }
  const char* affected = PQcmdTuples(res.get());
  std::string affected_str = affected == nullptr ? "0" : std::string{affected};

  // For bulk operations the signature field in the NOTIFY payload is
  // empty — the listener scans by extension_name when the signature is
  // empty, per ICD-0.2.2 §Cache Invalidation ("If extension_name is
  // provided, mark all matching entries…").
  send_notify(pg.conn, notify_action, /*signature=*/"", /*scope=*/"",
              std::optional<std::string_view>{extension_name});

  Json::Value detail(Json::objectValue);
  detail["extension_name"] = std::string{extension_name};
  detail["count"] = affected_str;
  plinth::log::audit_sync(db_cfg, audit_action, detail);
  static_cast<void>(audit_ctx);

  return std::string{extension_name};
}

} // namespace

auto disable_by_extension(const Config::Database& db_cfg,
                          std::string_view extension_name,
                          const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult {
  return set_enabled_by_extension(db_cfg, extension_name,
                                  /*target_enabled=*/false,
                                  "capability.extension_disabled", "disable",
                                  audit_ctx);
}

auto enable_by_extension(const Config::Database& db_cfg,
                         std::string_view extension_name,
                         const plinth::log::AuditCtx& audit_ctx)
    -> RegisterResult {
  return set_enabled_by_extension(db_cfg, extension_name,
                                  /*target_enabled=*/true,
                                  "capability.extension_enabled", "enable",
                                  audit_ctx);
}

} // namespace plinth::capabilities
