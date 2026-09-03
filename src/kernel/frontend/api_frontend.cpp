#include "kernel/frontend/api_frontend.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace plinth::frontend {

namespace {

enum class ResolveStatus : std::uint8_t {
  OK,
  NONE_ACTIVE,
  MULTIPLE_ACTIVE,
  ERROR
};

// Flattened result: status carries the case discriminator; name/version
// are populated only when status == OK. Empty strings on the other
// branches. Avoids `std::optional<ActiveRow>` indirection that
// confuses `bugprone-unchecked-optional-access` across the switch.
struct ResolveResult {
  ResolveStatus status{ResolveStatus::ERROR};
  std::string name;
  std::string version;
};

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

// Resolve the single ACTIVE frontend row from `plinth.packages`. Mirrors
// the LIMIT 2 detection in `shell::resolve_active_frontend` but reports
// the n==0 vs n>1 distinction so the handler can return the right 503
// diagnostic body per ICD §6.4. The column subset is narrower (name +
// version only) — this handler builds a redirect URL, not an asset path.
auto resolve_active(const Config::Database& db) -> ResolveResult {
  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::warn("frontend::api_frontend: PG connect failed: {}",
                 PQerrorMessage(conn));
    PQfinish(conn);
    return {.status = ResolveStatus::ERROR};
  }
  auto cleanup = [](PGconn* c) { PQfinish(c); };
  std::unique_ptr<PGconn, decltype(cleanup)> guard(conn, cleanup);

  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexec(conn, "SELECT name, version "
                   "FROM plinth.packages "
                   "WHERE frontend_mount IS NOT NULL "
                   "  AND state IN ('ACTIVE', 'ACTIVE_FLAGGED') "
                   "LIMIT 2"),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::warn("frontend::api_frontend: SELECT failed: {}",
                 PQresultErrorMessage(res.get()));
    return {.status = ResolveStatus::ERROR};
  }
  int n = PQntuples(res.get());
  if (n == 0) {
    return {.status = ResolveStatus::NONE_ACTIVE};
  }
  if (n > 1) {
    return {.status = ResolveStatus::MULTIPLE_ACTIVE};
  }
  return {
      .status = ResolveStatus::OK,
      .name = std::string{PQgetvalue(res.get(), 0, 0)},
      .version = std::string{PQgetvalue(res.get(), 0, 1)},
  };
}

auto make_redirect(std::string_view name, std::string_view version,
                   std::string_view target_path) -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k302Found);
  resp->addHeader("Location", "/ext/" + std::string{name} + "/" +
                                  std::string{version} +
                                  std::string{target_path});
  resp->addHeader("Cache-Control", "no-cache");
  resp->setBody("");
  return resp;
}

auto make_503(std::string_view error_kind, std::string_view message)
    -> drogon::HttpResponsePtr {
  nlohmann::json body = {
      {"error", error_kind},
      {"code", 503},
      {"message", message},
  };
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k503ServiceUnavailable);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  resp->addHeader("Cache-Control", "no-cache");
  return resp;
}

// Shared body for the two `/api/frontend/*` routes. `target_path` is
// the suffix appended after `/ext/{name}/{version}` — `/css/tokens.css`
// for the v0.6.2 `tokens.css` route, `/client/sdk.js` for the v0.6.3
// `sdk.js` route. `asset_label` appears in the 503 message for operator
// debugging.
auto handle_redirect(const Config::Database& db, std::string_view target_path,
                     std::string_view asset_label,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb)
    -> void {
  auto r = resolve_active(db);
  switch (r.status) {
    case ResolveStatus::OK:
      std::move(cb)(make_redirect(r.name, r.version, target_path));
      return;
    case ResolveStatus::NONE_ACTIVE:
      std::move(cb)(make_503("no_active_frontend",
                             "no active frontend installed; cannot serve " +
                                 std::string{asset_label}));
      return;
    case ResolveStatus::MULTIPLE_ACTIVE:
      std::move(cb)(make_503("multiple_active_frontends",
                             "multiple active frontends detected; "
                             "single-frontend invariant violated"));
      return;
    case ResolveStatus::ERROR:
      // PG outage / SQL failure: fail-safe to no_active_frontend.
      // The handler is documented (§6.4) as 503-only; we don't
      // surface a separate kind for transport errors.
      std::move(cb)(
          make_503("no_active_frontend",
                   "active-frontend resolution failed; cannot serve " +
                       std::string{asset_label}));
      return;
  }
}

} // namespace

auto register_api_frontend_routes(const Config::Database& db) -> void {
  drogon::app().registerHandler(
      "/api/frontend/tokens.css",
      // Drogon delivers cb as Callback&&; we forward via std::move into
      // handle_redirect.
      [db](const drogon::HttpRequestPtr& /*req*/,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        handle_redirect(db, "/css/tokens.css", "tokens.css", std::move(cb));
      },
      {drogon::Get});
  spdlog::info("frontend: registered GET /api/frontend/tokens.css");

  // ICD-0.6.3 §5 — sibling route for the Client SDK module.
  drogon::app().registerHandler(
      "/api/frontend/sdk.js",
      // same forwarding rationale as the tokens.css route above.
      [db](const drogon::HttpRequestPtr& /*req*/,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        handle_redirect(db, "/sdk.js", "sdk.js", std::move(cb));
      },
      {drogon::Get});
  spdlog::info("frontend: registered GET /api/frontend/sdk.js");
}

namespace test_seam {

auto dispatch_tokens_css(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const Config::Database& db) -> void {
  handle_redirect(db, "/css/tokens.css", "tokens.css", std::move(cb));
}

auto dispatch_sdk_js(const drogon::HttpRequestPtr& /*req*/,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                     const Config::Database& db) -> void {
  handle_redirect(db, "/sdk.js", "sdk.js", std::move(cb));
}

} // namespace test_seam

} // namespace plinth::frontend
