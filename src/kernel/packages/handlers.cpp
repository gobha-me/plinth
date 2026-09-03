#include "kernel/packages/handlers.hpp"

#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/rbac/enforcement.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace plinth::packages {

namespace {

// ICD-0.4.5 §T2 — process-static atomic backing
// `test_seam::upgrade_drain_timeout_ms_override`. Sentinel `SIZE_MAX`
// means "no override set"; any other value is the override (in ms).
// Mirrors the `live_buffer_cap_override` pattern used by
// ws/subscriptions.cpp:38-41.
constexpr std::size_t UPGRADE_DRAIN_SENTINEL_NONE =
    static_cast<std::size_t>(-1);
// process-static atomic backing the test seam, only mutated via
// `set_upgrade_drain_timeout_ms_override`.
std::atomic<std::size_t> g_upgrade_drain_override{UPGRADE_DRAIN_SENTINEL_NONE};

// Resolve the effective drain timeout for an upgrade, honoring the
// test override when set.
auto effective_upgrade_drain_timeout_ms(std::size_t default_ms) -> std::size_t {
  const auto OVERRIDE_VAL =
      g_upgrade_drain_override.load(std::memory_order_acquire);
  return OVERRIDE_VAL == UPGRADE_DRAIN_SENTINEL_NONE ? default_ms
                                                     : OVERRIDE_VAL;
}

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto json_write(const Json::Value& v) -> std::string {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

auto json_resp(drogon::HttpStatusCode code, const Json::Value& body)
    -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(code);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(json_write(body));
  return resp;
}

auto error_body(std::string_view code, std::string_view message)
    -> Json::Value {
  Json::Value out(Json::objectValue);
  Json::Value err(Json::objectValue);
  err["code"] = std::string{code};
  err["message"] = std::string{message};
  out["error"] = err;
  return out;
}

auto failure_to_status(InstallStage stage, std::string_view kind)
    -> drogon::HttpStatusCode {
  if (stage == InstallStage::UPLOADING) {
    if (kind == "upload-too-large") {
      return drogon::k413RequestEntityTooLarge;
    }
    if (kind == "name-already-installed" || kind == "advisory-lock-held" ||
        kind == "disabled-version-present" ||
        kind == "upgrade-version-not-newer" ||
        kind == "upgrade-not-yet-implemented") {
      return drogon::k409Conflict;
    }
    return drogon::k400BadRequest;
  }
  if (stage == InstallStage::VALIDATING) {
    return drogon::k422UnprocessableEntity;
  }
  if (stage == InstallStage::MIGRATING) {
    return drogon::k422UnprocessableEntity;
  }
  return drogon::k500InternalServerError;
}

// Map a 0.4.5 TransitionFailure onto an HTTP status per ICD-0.4.5
// §HTTP Surface tables. Unknown `kind` falls through to 500.
auto transition_failure_to_status(const TransitionFailure& f)
    -> drogon::HttpStatusCode {
  std::string kind;
  if (f.report.is_object() && f.report.contains("kind") &&
      f.report["kind"].is_string()) {
    kind = f.report["kind"].get<std::string>();
  }
  if (kind == "not-found") {
    return drogon::k404NotFound;
  }
  if (kind == "already-disabled" || kind == "already-enabled" ||
      kind == "already-uninstalling" || kind == "invalid-state-transition" ||
      kind == "confirmation-required" || kind == "in-flight-operation") {
    return drogon::k409Conflict;
  }
  if (kind == "checksum-mismatch-on-enable") {
    return drogon::k422UnprocessableEntity;
  }
  return drogon::k500InternalServerError;
}

// Convert an `nlohmann::json` value into the `Json::Value` (jsoncpp)
// shape the response builders use. Single existing entry point —
// `install_package` carries validation reports and InstallFailure
// reports as `nlohmann::json`; the response is built with jsoncpp.
// Round-trip through a string is the simplest correct converter.
auto nlohmann_to_jsoncpp(const nlohmann::json& src) -> Json::Value {
  auto str = src.dump();
  Json::Value out;
  Json::CharReaderBuilder rb;
  auto reader = std::unique_ptr<Json::CharReader>{rb.newCharReader()};
  std::string errs;
  // Json::CharReader::parse takes (begin, end) char pointers.
  reader->parse(str.data(), str.data() + str.size(), &out, &errs);
  return out;
}

auto record_to_json(const PackageRecord& rec) -> Json::Value {
  Json::Value out(Json::objectValue);
  out["id"] = rec.id;
  out["name"] = rec.name;
  out["version"] = rec.version;
  out["state"] = std::string{stage_to_string(rec.state)};
  out["provenance"] = std::string{provenance_to_string(rec.provenance)};
  out["frontend_mount"] = rec.frontend_mount.has_value()
                              ? Json::Value{*rec.frontend_mount}
                              : Json::Value{Json::nullValue};
  out["frontend_entry"] = rec.frontend_entry.has_value()
                              ? Json::Value{*rec.frontend_entry}
                              : Json::Value{Json::nullValue};
  out["warnings"] = Json::Value(Json::arrayValue);
  return out;
}

auto caller_user_id(const drogon::HttpRequestPtr& req) -> std::string {
  auto attrs = req->attributes();
  if (!attrs) {
    return {};
  }
  if (!attrs->find("plinth.user_id")) {
    return {};
  }
  return attrs->get<std::string>("plinth.user_id");
}

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

// ─── POST /api/packages ──────────────────────────────────────────────

auto handle_post_packages(
    const drogon::HttpRequestPtr& req,
    // Drogon handler signature requires `Callback&&`; cb is invoked, not moved.
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const PackageRoutesConfig& cfg) -> void {
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0) {
    cb(json_resp(
        drogon::k400BadRequest,
        error_body("bad-multipart", "multipart/form-data parse failed")));
    return;
  }
  const auto& files = parser.getFiles();
  if (files.empty()) {
    cb(json_resp(
        drogon::k400BadRequest,
        error_body("missing-field", "multipart field 'package' required")));
    return;
  }
  const auto& file = files.front();
  auto content = file.fileContent();

  InstallerContext ctx{
      .db = cfg.db,
      .caller_user_id = caller_user_id(req),
      .data_dir = cfg.data_dir,
      .staging_dir = cfg.staging_dir,
      .max_package_size_bytes = cfg.max_package_size_bytes,
      .upgrade_drain_timeout_ms =
          std::chrono::milliseconds{
              effective_upgrade_drain_timeout_ms(cfg.upgrade_drain_timeout_ms)},
  };
  std::span<const std::byte> blob{
      // body is raw bytes; std::span<const std::byte> is the installer API.
      reinterpret_cast<const std::byte*>(content.data()), content.size()};

  // ICD-0.4.4 §HTTP Surface — `?dry_run=1` runs UPLOADING + VALIDATING
  // and returns the ValidationReport without persisting any row or
  // schema (I.19). Inherits collision semantics from the regular path:
  // DISABLED_PRESENT / VERSION_NOT_NEWER / advisory-lock-held all 409
  // before INSERT, so dry-run responses carry the same kinds.
  bool dry_run = (req->getParameter("dry_run") == "1");
  if (dry_run) {
    nlohmann::json vr_report;
    auto r = install_package(blob, Provenance::USER, ctx,
                             /*dry_run=*/true, &vr_report);
    if (!r.has_value()) {
      const auto& f = r.error();
      Json::Value body(Json::objectValue);
      body["state"] = "INSTALL_FAILED";
      body["failed_at_stage"] = std::string{stage_to_string(f.failed_at)};
      body["kind"] = f.kind;
      body["message"] = f.message;
      if (!f.report.is_null()) {
        body["report"] = nlohmann_to_jsoncpp(f.report);
      }
      cb(json_resp(failure_to_status(f.failed_at, f.kind), body));
      return;
    }
    Json::Value body(Json::objectValue);
    body["state"] = "VALIDATING";
    body["name"] = r->name;
    body["version"] = r->version;
    body["frontend_mount"] = r->frontend_mount.has_value()
                                 ? Json::Value{*r->frontend_mount}
                                 : Json::Value{Json::nullValue};
    body["frontend_entry"] = r->frontend_entry.has_value()
                                 ? Json::Value{*r->frontend_entry}
                                 : Json::Value{Json::nullValue};
    body["validation_report"] = nlohmann_to_jsoncpp(vr_report);
    cb(json_resp(drogon::k200OK, body));
    return;
  }

  auto r = install_package(blob, Provenance::USER, ctx);
  if (!r.has_value()) {
    const auto& f = r.error();
    Json::Value body(Json::objectValue);
    body["state"] = "INSTALL_FAILED";
    body["failed_at_stage"] = std::string{stage_to_string(f.failed_at)};
    body["kind"] = f.kind;
    body["message"] = f.message;
    if (!f.package_id.empty()) {
      body["id"] = f.package_id;
    }
    cb(json_resp(failure_to_status(f.failed_at, f.kind), body));
    return;
  }
  cb(json_resp(drogon::k201Created, record_to_json(*r)));
}

// ─── GET /api/packages ───────────────────────────────────────────────

auto parse_int_param(std::string_view s, int default_value, int lo, int hi)
    -> int {
  if (s.empty()) {
    return default_value;
  }
  int parsed = default_value;
  const auto* first = s.data();
  const auto* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, parsed);
  if (ec != std::errc{}) {
    return default_value;
  }
  return std::max(lo, std::min(hi, parsed));
}

auto handle_get_packages(
    const drogon::HttpRequestPtr& req,
    // Drogon handler signature; cb is invoked, not moved.
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const Config::Database& db) -> void {
  std::string limit_s = req->getParameter("limit");
  std::string offset_s = req->getParameter("offset");
  int limit = parse_int_param(limit_s, 50, 1, 200);
  int offset = parse_int_param(offset_s, 0, 0, std::numeric_limits<int>::max());
  bool include_failed = !req->getParameter("include_failed").empty();

  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  auto conn_guard =
      std::unique_ptr<PGconn, decltype(&PQfinish)>(conn, PQfinish);
  if (PQstatus(conn) != CONNECTION_OK) {
    cb(json_resp(drogon::k500InternalServerError,
                 error_body("db-error", "PG connect failed")));
    return;
  }

  std::string limit_str = std::to_string(limit);
  std::string offset_str = std::to_string(offset);
  std::string where =
      include_failed ? ""
                     : " WHERE state NOT IN ('INSTALL_FAILED','UNINSTALLING')";
  std::string sql = "SELECT id::text, name, version, state, provenance, "
                    "       frontend_mount, frontend_entry, installed_at "
                    "FROM plinth.packages" +
                    where +
                    " ORDER BY installed_at DESC LIMIT $1::int OFFSET $2::int";
  std::array<const char*, 2> values = {limit_str.c_str(), offset_str.c_str()};
  PgResultPtr res(PQexecParams(conn, sql.c_str(), 2, nullptr, values.data(),
                               nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    cb(json_resp(drogon::k500InternalServerError,
                 error_body("db-error", PQresultErrorMessage(res.get()))));
    return;
  }

  Json::Value out(Json::objectValue);
  Json::Value rows(Json::arrayValue);
  int row_count = PQntuples(res.get());
  for (int i = 0; i < row_count; ++i) {
    Json::Value row(Json::objectValue);
    row["id"] = PQgetvalue(res.get(), i, 0);
    row["name"] = PQgetvalue(res.get(), i, 1);
    row["version"] = PQgetvalue(res.get(), i, 2);
    row["state"] = PQgetvalue(res.get(), i, 3);
    row["provenance"] = PQgetvalue(res.get(), i, 4);
    row["frontend_mount"] = PQgetisnull(res.get(), i, 5) != 0
                                ? Json::Value{Json::nullValue}
                                : Json::Value{PQgetvalue(res.get(), i, 5)};
    row["frontend_entry"] = PQgetisnull(res.get(), i, 6) != 0
                                ? Json::Value{Json::nullValue}
                                : Json::Value{PQgetvalue(res.get(), i, 6)};
    row["installed_at"] = PQgetvalue(res.get(), i, 7);
    rows.append(std::move(row));
  }
  out["items"] = std::move(rows);
  out["limit"] = limit;
  out["offset"] = offset;
  cb(json_resp(drogon::k200OK, out));
}

// ─── GET /api/packages/{id} ──────────────────────────────────────────

auto handle_get_package_by_id(
    const drogon::HttpRequestPtr& /*req*/,
    // Drogon handler signature; cb is invoked, not moved.
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const Config::Database& db, const std::string& id) -> void {
  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  auto conn_guard =
      std::unique_ptr<PGconn, decltype(&PQfinish)>(conn, PQfinish);
  if (PQstatus(conn) != CONNECTION_OK) {
    cb(json_resp(drogon::k500InternalServerError,
                 error_body("db-error", "PG connect failed")));
    return;
  }

  std::array<const char*, 1> values = {id.c_str()};
  PgResultPtr res(
      PQexecParams(conn,
                   "SELECT id::text, name, version, state, provenance, "
                   "       frontend_mount, frontend_entry, entry_point, "
                   "       manifest_checksum, manifest_json::text, "
                   "       last_install_report::text, installed_at "
                   "FROM plinth.packages WHERE id = $1::uuid",
                   1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    cb(json_resp(drogon::k500InternalServerError,
                 error_body("db-error", PQresultErrorMessage(res.get()))));
    return;
  }
  if (PQntuples(res.get()) == 0) {
    cb(json_resp(drogon::k404NotFound,
                 error_body("not-found", "package not found")));
    return;
  }

  Json::Value row(Json::objectValue);
  row["id"] = PQgetvalue(res.get(), 0, 0);
  row["name"] = PQgetvalue(res.get(), 0, 1);
  row["version"] = PQgetvalue(res.get(), 0, 2);
  row["state"] = PQgetvalue(res.get(), 0, 3);
  row["provenance"] = PQgetvalue(res.get(), 0, 4);
  row["frontend_mount"] = PQgetisnull(res.get(), 0, 5) != 0
                              ? Json::Value{Json::nullValue}
                              : Json::Value{PQgetvalue(res.get(), 0, 5)};
  row["frontend_entry"] = PQgetisnull(res.get(), 0, 6) != 0
                              ? Json::Value{Json::nullValue}
                              : Json::Value{PQgetvalue(res.get(), 0, 6)};
  row["entry_point"] = PQgetvalue(res.get(), 0, 7);
  row["manifest_checksum"] = PQgetvalue(res.get(), 0, 8);

  // JSONB columns: parse and include as nested objects.
  auto parse_or_null = [](const char* s) -> Json::Value {
    if (s == nullptr || *s == '\0') {
      return Json::nullValue;
    }
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    std::istringstream in(s);
    if (!Json::parseFromStream(b, in, &v, &errs)) {
      return Json::nullValue;
    }
    return v;
  };
  row["manifest_json"] = parse_or_null(PQgetvalue(res.get(), 0, 9));
  row["last_install_report"] =
      PQgetisnull(res.get(), 0, 10) != 0
          ? Json::Value{Json::nullValue}
          : parse_or_null(PQgetvalue(res.get(), 0, 10));
  row["installed_at"] = PQgetvalue(res.get(), 0, 11);
  cb(json_resp(drogon::k200OK, row));
}

// ─── PATCH /api/packages/{id} — disable / enable ────────────────────

auto patch_error_body(const TransitionFailure& f) -> Json::Value {
  Json::Value out(Json::objectValue);
  Json::Value err(Json::objectValue);
  std::string kind;
  if (f.report.is_object() && f.report.contains("kind") &&
      f.report["kind"].is_string()) {
    kind = f.report["kind"].get<std::string>();
  }
  err["code"] = kind.empty() ? std::string{"db-error"} : kind;
  err["message"] = f.message;
  out["error"] = err;
  return out;
}

auto patch_success_body(const PackageRecord& rec, std::string_view action)
    -> Json::Value {
  Json::Value out(Json::objectValue);
  out["id"] = rec.id;
  out["name"] = rec.name;
  out["version"] = rec.version;
  out["state"] = std::string{stage_to_string(rec.state)};
  out["action"] = std::string{action};
  return out;
}

auto handle_patch_package(
    const drogon::HttpRequestPtr& req,
    // Drogon handler signature requires `Callback&&`; cb is invoked, not moved.
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const PackageRoutesConfig& cfg, const std::string& id) -> void {
  auto body = req->getJsonObject();
  if (!body || !body->isMember("action") || !(*body)["action"].isString()) {
    cb(json_resp(drogon::k400BadRequest,
                 error_body("invalid-action",
                            "body must be JSON with string `action`")));
    return;
  }
  std::string action = (*body)["action"].asString();
  if (action != "disable" && action != "enable") {
    cb(json_resp(drogon::k400BadRequest,
                 error_body("invalid-action",
                            R"(action must be "disable" or "enable")")));
    return;
  }

  InstallerContext ctx{
      .db = cfg.db,
      .caller_user_id = caller_user_id(req),
      .data_dir = cfg.data_dir,
      .staging_dir = cfg.staging_dir,
      .max_package_size_bytes = cfg.max_package_size_bytes,
      .upgrade_drain_timeout_ms =
          std::chrono::milliseconds{
              effective_upgrade_drain_timeout_ms(cfg.upgrade_drain_timeout_ms)},
  };

  std::expected<PackageRecord, TransitionFailure> r =
      (action == "disable") ? disable_package(id, ctx)
                            : enable_package(id, ctx);

  if (!r.has_value()) {
    cb(json_resp(transition_failure_to_status(r.error()),
                 patch_error_body(r.error())));
    return;
  }
  cb(json_resp(drogon::k200OK, patch_success_body(*r, action)));
}

// ─── DELETE /api/packages/{id}?confirm=true — uninstall ────────────

auto handle_delete_package(
    const drogon::HttpRequestPtr& req,
    // Drogon handler signature; cb is invoked, not moved.
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const PackageRoutesConfig& cfg, const std::string& id) -> void {
  bool confirmed = (req->getParameter("confirm") == "true");
  InstallerContext ctx{
      .db = cfg.db,
      .caller_user_id = caller_user_id(req),
      .data_dir = cfg.data_dir,
      .staging_dir = cfg.staging_dir,
      .max_package_size_bytes = cfg.max_package_size_bytes,
      .upgrade_drain_timeout_ms =
          std::chrono::milliseconds{
              effective_upgrade_drain_timeout_ms(cfg.upgrade_drain_timeout_ms)},
  };

  auto r = uninstall_package(id, confirmed, ctx);
  if (!r.has_value()) {
    cb(json_resp(transition_failure_to_status(r.error()),
                 patch_error_body(r.error())));
    return;
  }
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k204NoContent);
  cb(resp);
}

} // namespace

auto register_package_routes(const PackageRoutesConfig& cfg) -> void {
  constexpr auto SF = "plinth::auth::SessionFilter";
  constexpr auto RF = "plinth::rbac::RbacFilter";

  drogon::app().registerHandler(
      "/api/packages",
      [cfg](const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        handle_post_packages(req, std::move(cb), cfg);
      },
      {drogon::Post, SF, RF});
  rbac::register_rule_requirement(drogon::Post, "/api/packages",
                                  {"packages.install"});

  drogon::app().registerHandler(
      "/api/packages",
      [db = cfg.db](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        handle_get_packages(req, std::move(cb), db);
      },
      {drogon::Get, SF, RF});
  rbac::register_rule_requirement(drogon::Get, "/api/packages",
                                  {"packages.read"});

  drogon::app().registerHandler(
      "/api/packages/{id}",
      [db = cfg.db](const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                    const std::string& id) {
        handle_get_package_by_id(req, std::move(cb), db, id);
      },
      {drogon::Get, SF, RF});
  rbac::register_rule_requirement(drogon::Get, "/api/packages/{id}",
                                  {"packages.read"});

  drogon::app().registerHandler(
      "/api/packages/{id}",
      [cfg](const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& id) {
        handle_patch_package(req, std::move(cb), cfg, id);
      },
      {drogon::Patch, SF, RF});
  rbac::register_rule_requirement(drogon::Patch, "/api/packages/{id}",
                                  {"packages.install"});

  drogon::app().registerHandler(
      "/api/packages/{id}",
      [cfg](const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& id) {
        handle_delete_package(req, std::move(cb), cfg, id);
      },
      {drogon::Delete, SF, RF});
  rbac::register_rule_requirement(drogon::Delete, "/api/packages/{id}",
                                  {"packages.install"});

  spdlog::info("packages HTTP routes registered");
}

namespace test_seam {

auto dispatch_post(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)> cb,
                   const PackageRoutesConfig& cfg) -> void {
  handle_post_packages(req, std::move(cb), cfg);
}

auto dispatch_patch(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)> cb,
                    const PackageRoutesConfig& cfg, const std::string& id)
    -> void {
  handle_patch_package(req, std::move(cb), cfg, id);
}

auto dispatch_delete(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)> cb,
                     const PackageRoutesConfig& cfg, const std::string& id)
    -> void {
  handle_delete_package(req, std::move(cb), cfg, id);
}

auto upgrade_drain_timeout_ms_override() -> std::optional<std::size_t> {
  const auto OVERRIDE_VAL =
      g_upgrade_drain_override.load(std::memory_order_acquire);
  if (OVERRIDE_VAL == UPGRADE_DRAIN_SENTINEL_NONE) {
    return std::nullopt;
  }
  return OVERRIDE_VAL;
}

auto set_upgrade_drain_timeout_ms_override(std::size_t ms) -> void {
  if (ms == UPGRADE_DRAIN_SENTINEL_NONE) {
    ms = UPGRADE_DRAIN_SENTINEL_NONE - 1;
  }
  g_upgrade_drain_override.store(ms, std::memory_order_release);
}

auto clear_upgrade_drain_timeout_ms_override() -> void {
  g_upgrade_drain_override.store(UPGRADE_DRAIN_SENTINEL_NONE,
                                 std::memory_order_release);
}

} // namespace test_seam

} // namespace plinth::packages
