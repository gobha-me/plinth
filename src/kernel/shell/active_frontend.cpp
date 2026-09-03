#include "kernel/shell/active_frontend.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace plinth::shell {

namespace {

constexpr std::string_view STRICT_CSP =
    "script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "connect-src 'self'";

constexpr std::string_view CACHE_INDEX = "no-cache";
constexpr std::string_view CACHE_IMMUTABLE =
    "public, max-age=31536000, immutable";

// Process-wide cached active-frontend state for handler lookups.
// Resolved at register_active_frontend_routes time and held under
// shared_mutex so test-seam writes are visible across threads.
// process-lifetime cache
std::shared_mutex g_af_mu;
std::optional<ActiveFrontend> g_active;

auto active_snapshot() -> std::optional<ActiveFrontend> {
  std::shared_lock lk(g_af_mu);
  return g_active;
}

auto store_active(std::optional<ActiveFrontend> a) -> void {
  std::unique_lock lk(g_af_mu);
  g_active = std::move(a);
}

auto make_redirect(std::string_view location) -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k302Found);
  resp->addHeader("Location", std::string{location});
  return resp;
}

auto make_404() -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k404NotFound);
  resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
  resp->setBody("not found");
  return resp;
}

auto ascii_lower(std::string_view s) -> std::string {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
    out.push_back(c);
  }
  return out;
}

auto mime_for_extension(std::string_view ext) -> std::string_view {
  if (ext == ".html") {
    return "text/html; charset=utf-8";
  }
  if (ext == ".js" || ext == ".mjs") {
    return "application/javascript";
  }
  if (ext == ".css") {
    return "text/css";
  }
  if (ext == ".woff2") {
    return "font/woff2";
  }
  if (ext == ".svg") {
    return "image/svg+xml";
  }
  if (ext == ".png") {
    return "image/png";
  }
  if (ext == ".json") {
    return "application/json";
  }
  if (ext == ".ico") {
    return "image/x-icon";
  }
  return "application/octet-stream";
}

// Reject path-traversal: per asset_server::normalize_request_path —
// reject empty components, `.` / `..` segments, and embedded NULs.
auto normalize_path(std::string_view raw) -> std::optional<std::string> {
  std::string out;
  out.reserve(raw.size());
  std::string_view remaining{raw};
  while (!remaining.empty()) {
    auto slash = remaining.find('/');
    std::string_view component = (slash == std::string_view::npos)
                                     ? remaining
                                     : remaining.substr(0, slash);
    if (component.empty() || component == "." || component == "..") {
      return std::nullopt;
    }
    if (component.find('\0') != std::string_view::npos) {
      return std::nullopt;
    }
    if (!out.empty()) {
      out.push_back('/');
    }
    out.append(component);
    if (slash == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(slash + 1);
  }
  return out;
}

// SPA fallback heuristic per ICD-0.6.0 §8.2: empty path or path whose
// last segment has no `.` extension serves the entry file.
auto path_targets_html_fallback(std::string_view normalized) -> bool {
  if (normalized.empty()) {
    return true;
  }
  auto last_slash = normalized.find_last_of('/');
  auto basename = (last_slash == std::string_view::npos)
                      ? normalized
                      : normalized.substr(last_slash + 1);
  return basename.find('.') == std::string_view::npos;
}

auto resolve_under_root(const std::filesystem::path& client_root,
                        std::string_view normalized_path)
    -> std::optional<std::filesystem::path> {
  std::error_code ec;
  std::filesystem::path combined = client_root / normalized_path;
  auto canonical = std::filesystem::weakly_canonical(combined, ec);
  if (ec) {
    return std::nullopt;
  }
  auto root_canon = std::filesystem::weakly_canonical(client_root, ec);
  if (ec) {
    return std::nullopt;
  }
  auto [rmatch, _] = std::ranges::mismatch(root_canon, canonical);
  if (rmatch != root_canon.end()) {
    return std::nullopt;
  }
  return canonical;
}

auto serve_file(const std::filesystem::path& full_path,
                std::string_view content_type, std::string_view cache_control)
    -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newFileResponse(
      full_path.string(), "", drogon::CT_CUSTOM, std::string{content_type});
  resp->addHeader("Cache-Control", std::string{cache_control});
  resp->addHeader("Content-Security-Policy", std::string{STRICT_CSP});
  return resp;
}

auto handle_app_request(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    std::string_view path) -> void {
  auto active = active_snapshot();
  if (!active.has_value()) {
    std::move(cb)(make_404());
    return;
  }
  auto normalized = normalize_path(path);
  if (!normalized.has_value()) {
    std::move(cb)(make_404());
    return;
  }
  bool fallback = path_targets_html_fallback(*normalized);
  auto target_path = fallback ? active->entry : *normalized;
  auto resolved = resolve_under_root(active->client_dir, target_path);
  if (!resolved.has_value()) {
    std::move(cb)(make_404());
    return;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(*resolved, ec) || ec) {
    std::move(cb)(make_404());
    return;
  }
  std::string ext = ascii_lower(resolved->extension().string());
  std::string_view mime = mime_for_extension(ext);
  bool serving_index = (resolved->filename().string() == active->entry);
  std::string_view cache = serving_index ? CACHE_INDEX : CACHE_IMMUTABLE;
  std::move(cb)(serve_file(*resolved, mime, cache));
}

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

// `<mount>` may or may not have a trailing slash in the manifest. The
// route registrar normalises to "no trailing slash + (.*)" so the
// captured remainder skips the leading "/" (matches existing 0.6.0
// dispatch shape: `/app/(.*)` captures everything after `/app/`).
auto trim_trailing_slash(std::string_view s) -> std::string {
  while (!s.empty() && s.back() == '/') {
    s.remove_suffix(1);
  }
  return std::string{s};
}

} // namespace

auto resolve_active_frontend(const Config::Database& db,
                             const std::filesystem::path& data_dir)
    -> std::optional<ActiveFrontend> {
  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::warn("shell::resolve_active_frontend: PG connect failed: {}",
                 PQerrorMessage(conn));
    PQfinish(conn);
    return std::nullopt;
  }
  auto cleanup = [](PGconn* c) { PQfinish(c); };
  std::unique_ptr<PGconn, decltype(cleanup)> guard(conn, cleanup);

  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexec(conn,
             "SELECT id::text, name, version, frontend_mount, frontend_entry "
             "FROM plinth.packages "
             "WHERE frontend_mount IS NOT NULL "
             "  AND state IN ('ACTIVE', 'ACTIVE_FLAGGED') "
             "LIMIT 2"), // LIMIT 2 to detect singleton-violation
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::warn("shell::resolve_active_frontend: SELECT failed: {}",
                 PQresultErrorMessage(res.get()));
    return std::nullopt;
  }
  int n = PQntuples(res.get());
  if (n == 0) {
    return std::nullopt;
  }
  if (n > 1) {
    spdlog::error("shell::resolve_active_frontend: multiple ACTIVE frontends "
                  "in plinth.packages (n={}); routes will not be registered",
                  n);
    return std::nullopt;
  }
  ActiveFrontend a;
  a.id = std::string{PQgetvalue(res.get(), 0, 0)};
  a.name = std::string{PQgetvalue(res.get(), 0, 1)};
  a.version = std::string{PQgetvalue(res.get(), 0, 2)};
  a.mount = std::string{PQgetvalue(res.get(), 0, 3)};
  a.entry = std::string{PQgetvalue(res.get(), 0, 4)};
  a.client_dir = data_dir / "extensions" / a.name / a.version / "client";
  return a;
}

auto register_active_frontend_routes(const Config::Shell& cfg_shell,
                                     const ActiveFrontend& active) -> void {
  if (!cfg_shell.enabled) {
    spdlog::info("shell: disabled (shell.enabled=false) — skipping `/` "
                 "redirect and active-frontend handler registration");
    return;
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(active.client_dir, ec) || ec) {
    spdlog::warn("shell: active frontend client_dir missing at {} — "
                 "{}/* will return 404",
                 active.client_dir.string(), active.mount);
    store_active(std::nullopt);
    return;
  }
  store_active(active);

  drogon::app().registerHandler(
      "/",
      [redirect = cfg_shell.root_redirect](
          const drogon::HttpRequestPtr& /*req*/,
          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::move(callback)(make_redirect(redirect));
      },
      {drogon::Get});

  auto base = trim_trailing_slash(active.mount);
  auto pattern = base + "/(.*)";
  drogon::app().registerHandlerViaRegex(
      pattern,
      // Drogon's regex handler delivers cb as Callback&&; we forward via
      // std::move inside handle_app_request.
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& cb,
         const std::string& path) {
        handle_app_request(req, std::move(cb), path);
      },
      {drogon::Get});

  spdlog::info("shell: registered `/` → 302 {} and `{}` → SPA fallback "
               "(active frontend {} {})",
               cfg_shell.root_redirect, pattern, active.name, active.version);
}

auto register_routes_for_active_frontend(const Config::Shell& cfg_shell,
                                         const Config::Database& db,
                                         const std::filesystem::path& data_dir)
    -> bool {
  if (!cfg_shell.enabled) {
    spdlog::info("shell: disabled (shell.enabled=false) — skipping active-"
                 "frontend resolution + route registration");
    return false;
  }
  auto active = resolve_active_frontend(db, data_dir);
  if (!active.has_value()) {
    spdlog::warn(
        "shell: no ACTIVE frontend in plinth.packages — `/` and "
        "frontend-mount paths will return 404 until install completes");
    store_active(std::nullopt);
    return false;
  }
  register_active_frontend_routes(cfg_shell, *active);
  return true;
}

namespace test_seam {

auto make_root_redirect(std::string_view target) -> drogon::HttpResponsePtr {
  return make_redirect(target);
}

auto dispatch_app(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)> cb,
                  const std::string& path_after_mount) -> void {
  handle_app_request(req, std::move(cb), path_after_mount);
}

auto set_active_frontend(std::optional<ActiveFrontend> active) -> void {
  store_active(std::move(active));
}

} // namespace test_seam

} // namespace plinth::shell
