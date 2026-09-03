#include "kernel/audit/handlers.hpp"
#include "kernel/auth/middleware.hpp"
#include "kernel/logging.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <drogon/drogon.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace plinth::audit {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
using SharedCb = std::shared_ptr<Callback>;

auto share(Callback&& cb) -> SharedCb {
  return std::make_shared<Callback>(std::move(cb));
}

auto json_error(drogon::HttpStatusCode status, const std::string& error_code,
                const std::string& message) -> drogon::HttpResponsePtr {
  Json::Value json;
  json["error"] = error_code;
  json["message"] = message;
  auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
  resp->setStatusCode(status);
  return resp;
}

constexpr int DEFAULT_LIMIT = 100;
constexpr int MAX_LIMIT = 500;
constexpr int UUID_STR_LEN = 36;

// ── Query parameter parsing ─────────────────────────────────────────

struct AuditQuery {
  int limit = DEFAULT_LIMIT;
  int offset = 0;
  std::string action;
  std::string user_id;
  std::string start_ts;
  std::string end_ts;
};

// Parse a non-negative integer query param. Empty → default.
// Returns false and sets `err_out` on invalid input.
auto parse_int(const std::string& raw, int default_v, int min_v, int max_v,
               int& out, std::string& err_out) -> bool {
  if (raw.empty()) {
    out = default_v;
    return true;
  }
  int value = 0;
  // std::from_chars takes a (begin, end) pointer pair per the C++17 API
  auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
  // against the computed end pointer above
  if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
    err_out = "invalid_parameter";
    return false;
  }
  if (value < min_v || value > max_v) {
    err_out = "invalid_parameter";
    return false;
  }
  out = value;
  return true;
}

auto is_uuid_shape(const std::string& s) -> bool {
  if (s.size() != UUID_STR_LEN) {
    return false;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
    } else {
      bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
      if (!is_hex) {
        return false;
      }
    }
  }
  return true;
}

// Minimal ISO-8601-shape check (YYYY-MM-DD at minimum). PostgreSQL
// timestamptz parser handles the rest; we fail fast on obvious junk.
auto is_iso8601_shape(const std::string& s) -> bool {
  if (s.size() < 10) {
    return false;
  }
  for (std::size_t i = 0; i < 10; ++i) {
    char c = s[i];
    if (i == 4 || i == 7) {
      if (c != '-') {
        return false;
      }
    } else {
      if (c < '0' || c > '9') {
        return false;
      }
    }
  }
  return true;
}

auto parse_query(const drogon::HttpRequestPtr& req, AuditQuery& q,
                 std::string& err_out) -> bool {
  if (!parse_int(req->getParameter("limit"), DEFAULT_LIMIT, 1, MAX_LIMIT,
                 q.limit, err_out)) {
    return false;
  }
  if (!parse_int(req->getParameter("offset"), 0, 0, INT32_MAX, q.offset,
                 err_out)) {
    return false;
  }

  q.action = req->getParameter("action");

  q.user_id = req->getParameter("user_id");
  if (!q.user_id.empty() && !is_uuid_shape(q.user_id)) {
    err_out = "invalid_parameter";
    return false;
  }

  q.start_ts = req->getParameter("start");
  if (!q.start_ts.empty() && !is_iso8601_shape(q.start_ts)) {
    err_out = "invalid_parameter";
    return false;
  }

  q.end_ts = req->getParameter("end");
  if (!q.end_ts.empty() && !is_iso8601_shape(q.end_ts)) {
    err_out = "invalid_parameter";
    return false;
  }

  return true;
}

// SQL uses CASE WHEN to keep the parameter count fixed — empty-string
// sentinel means "no filter for this column". Avoids having to
// dynamically build the WHERE clause or rely on ambiguous short-circuit
// semantics when the $N::uuid / $N::timestamptz cast would otherwise
// fail on an empty string.
constexpr const char* SELECT_SQL =
    "SELECT id, timestamp, action, user_id, session_id, "
    "       detail, ip_address, node_id "
    "FROM plinth.audit_log "
    "WHERE CASE WHEN $1 = '' THEN TRUE ELSE action = $1 END "
    "  AND CASE WHEN $2 = '' THEN TRUE ELSE user_id = $2::uuid END "
    "  AND CASE WHEN $3 = '' THEN TRUE ELSE timestamp >= $3::timestamptz END "
    "  AND CASE WHEN $4 = '' THEN TRUE ELSE timestamp <= $4::timestamptz END "
    "ORDER BY timestamp DESC "
    "LIMIT $5 OFFSET $6";

constexpr const char* COUNT_SQL =
    "SELECT COUNT(*) AS total FROM plinth.audit_log "
    "WHERE CASE WHEN $1 = '' THEN TRUE ELSE action = $1 END "
    "  AND CASE WHEN $2 = '' THEN TRUE ELSE user_id = $2::uuid END "
    "  AND CASE WHEN $3 = '' THEN TRUE ELSE timestamp >= $3::timestamptz END "
    "  AND CASE WHEN $4 = '' THEN TRUE ELSE timestamp <= $4::timestamptz END";

// ── Response shaping ────────────────────────────────────────────────

auto row_to_event(const drogon::orm::Row& row) -> Json::Value {
  Json::Value e;
  e["id"] = row["id"].as<std::string>();
  e["timestamp"] = row["timestamp"].as<std::string>();
  e["action"] = row["action"].as<std::string>();
  if (!row["user_id"].isNull()) {
    e["user_id"] = row["user_id"].as<std::string>();
  }
  if (!row["session_id"].isNull()) {
    e["session_id"] = row["session_id"].as<std::string>();
  }

  Json::Value detail_json(Json::objectValue);
  auto detail_str = row["detail"].as<std::string>();
  Json::CharReaderBuilder reader_b;
  std::unique_ptr<Json::CharReader> reader(reader_b.newCharReader());
  std::string errs;
  // Json::CharReader::parse takes a (begin, end) pointer pair
  reader->parse(detail_str.data(), detail_str.data() + detail_str.size(),
                &detail_json, &errs);
  e["detail"] = detail_json;

  if (!row["ip_address"].isNull()) {
    e["ip_address"] = row["ip_address"].as<std::string>();
  }
  e["node_id"] = row["node_id"].as<std::string>();
  return e;
}

// ── Handler ─────────────────────────────────────────────────────────

auto handle_list_audit(const drogon::HttpRequestPtr& req, Callback&& callback)
    -> void {
  auto ctx = auth::get_auth_context(req);
  if (!ctx.has_value()) {
    std::move(callback)(json_error(drogon::k401Unauthorized,
                                   "not_authenticated", "Not authenticated"));
    return;
  }

  AuditQuery q;
  std::string err;
  if (!parse_query(req, q, err)) {
    std::move(callback)(
        json_error(drogon::k400BadRequest, err, "Invalid query parameter"));
    return;
  }

  auto db = drogon::app().getDbClient();
  auto cb = share(std::move(callback));

  db->execSqlAsync(
      SELECT_SQL,
      [cb, q, db](const drogon::orm::Result& select_result) {
        Json::Value events(Json::arrayValue);
        for (const auto& row : select_result) {
          events.append(row_to_event(row));
        }

        // Sequential COUNT: keeps control flow linear. Admin endpoint,
        // so the extra round-trip is acceptable.
        db->execSqlAsync(
            COUNT_SQL,
            [cb, events](const drogon::orm::Result& count_result) {
              Json::Value body;
              body["events"] = events;
              body["total"] = count_result[0]["total"].as<int64_t>();
              (*cb)(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            [cb](const drogon::orm::DrogonDbException& e) {
              spdlog::error("audit count failed: {}", e.base().what());
              (*cb)(json_error(drogon::k500InternalServerError,
                               "internal_error", "Failed to query audit log"));
            },
            q.action, q.user_id, q.start_ts, q.end_ts);
      },
      [cb](const drogon::orm::DrogonDbException& e) {
        spdlog::error("audit select failed: {}", e.base().what());
        (*cb)(json_error(drogon::k500InternalServerError, "internal_error",
                         "Failed to query audit log"));
      },
      q.action, q.user_id, q.start_ts, q.end_ts, q.limit, q.offset);
}

// ── Retention ───────────────────────────────────────────────────────

auto build_conninfo(const Config::Database& db_cfg) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db_cfg.host << " port=" << db_cfg.port
     << " dbname=" << db_cfg.database << " user=" << db_cfg.user
     << " password=" << db_cfg.password;
  return ss.str();
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

auto register_audit_routes() -> void {
  // RBAC rule registration is idempotent and needs to be visible to any
  // test that calls this from within a grouped Catch2 subprocess
  // (0.4.5.1) AFTER drogon has started via
  // ensure_drogon_with_db_running().
  rbac::register_rule_requirement(drogon::Get, "/api/audit", {"kernel.admin"});

  // Drogon's `registerHandler` fires a `!routersInit_` assertion once
  // `app().run()` has been invoked. In production main() calls this
  // helper before run(). In tests the grouped pg subprocess may have
  // started drogon in an earlier TEST_CASE; skip the handler step
  // there — tests look at list_registered_rules() for RBAC coverage,
  // not the live HTTP handler. The call_once guards against duplicate
  // registrations before drogon starts (production path).
  if (drogon::app().isRunning()) {
    return;
  }
  static std::once_flag once;
  std::call_once(once, [] {
    constexpr auto SF = "plinth::auth::SessionFilter";
    constexpr auto RF = "plinth::rbac::RbacFilter";
    drogon::app().registerHandler(
        "/api/audit",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
          handle_list_audit(req, std::move(callback));
        },
        {drogon::Get, SF, RF});
    spdlog::info("audit routes registered");
  });
}

auto purge_older_than(const Config::Database& db, int retention_days)
    -> int64_t {
  if (retention_days <= 0) {
    return 0;
  }

  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    throw std::runtime_error("audit purge: PG connect failed: " + err);
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);

  auto interval = std::to_string(retention_days) + " days";
  std::array<const char*, 1> params = {interval.c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexecParams(conn,
                   "DELETE FROM plinth.audit_log "
                   "WHERE timestamp < NOW() - $1::interval",
                   1, nullptr, params.data(), nullptr, nullptr, 0),
      PQclear);

  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    throw std::runtime_error(std::string("audit purge SQL failed: ") +
                             PQresultErrorMessage(res.get()));
  }

  const char* affected = PQcmdTuples(res.get());
  if (affected == nullptr || *affected == '\0') {
    return 0;
  }
  return std::stoll(affected);
}

} // namespace plinth::audit
