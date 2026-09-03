#include "kernel/packages/asset_server.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace plinth::packages::asset_server {

namespace {

struct RouteHandle {
  std::filesystem::path client_root;
  std::string manifest_checksum;
};

// File-scope state for the process-wide asset route map. All three are
// mutated from multiple threads (request handlers + install lifecycle
// + the shutdown coordinator); they live at file scope so the wildcard handler
// lambda can see them without per-request indirection.
// request-path route map
std::shared_mutex g_mu;
// request-path route map
std::unordered_map<std::string, RouteHandle> g_routes;
// deterministic-closeout gate
std::atomic<bool> g_shutdown_pending{false};

auto compose_key(std::string_view name, std::string_view version)
    -> std::string {
  std::string key;
  key.reserve(name.size() + 1 + version.size());
  key.append(name);
  key.push_back('/');
  key.append(version);
  return key;
}

// MIME table keyed on lowercased extension INCLUDING the leading dot.
// Entries chosen from ICD-0.4.4 §GET /ext/... MIME table.
auto mime_for_extension(std::string_view ext) -> std::string_view {
  // Keep this table sorted by extension for easy future audits.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 13>
      MIME_TABLE = {{
          {".css", "text/css"},
          {".gif", "image/gif"},
          {".html", "text/html; charset=utf-8"},
          {".jpeg", "image/jpeg"},
          {".jpg", "image/jpeg"},
          {".js", "application/javascript"},
          {".json", "application/json"},
          {".mjs", "application/javascript"},
          {".png", "image/png"},
          {".svg", "image/svg+xml"},
          {".webp", "image/webp"},
          {".woff2", "font/woff2"},
      }};
  for (const auto& [e, m] : MIME_TABLE) {
    if (e == ext) {
      return m;
    }
  }
  return "application/octet-stream";
}

// Lowercase ASCII copy. Extension matching is case-insensitive per
// typical web-server convention.
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

// Percent-decode a single path segment. Returns std::nullopt on any
// malformed `%XX`. The decoder rejects NULs outright.
// percent-decoder; splitting would obscure the byte-by-byte scan.
auto percent_decode(std::string_view in) -> std::optional<std::string> {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '%') {
      if (i + 2 >= in.size()) {
        return std::nullopt;
      }
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') {
          return h - '0';
        }
        if (h >= 'a' && h <= 'f') {
          return 10 + (h - 'a');
        }
        if (h >= 'A' && h <= 'F') {
          return 10 + (h - 'A');
        }
        return -1;
      };
      int hi = hex(in[i + 1]);
      int lo = hex(in[i + 2]);
      if (hi < 0 || lo < 0) {
        return std::nullopt;
      }
      char decoded = static_cast<char>((hi << 4) | lo);
      if (decoded == '\0') {
        return std::nullopt;
      }
      out.push_back(decoded);
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Reject any path that — even after percent-decoding — contains a
// traversal component or an absolute reference. Returns the decoded,
// validated path on success. Empty path is allowed (caller handles
// directory-target 404 separately).
auto normalize_request_path(std::string_view raw_path)
    -> std::optional<std::string> {
  if (raw_path.starts_with('/')) {
    // Drogon delivers the captured remainder without a leading
    // slash; a leading slash here would be an attempted absolute-
    // path injection.
    return std::nullopt;
  }
  auto decoded = percent_decode(raw_path);
  if (!decoded.has_value()) {
    return std::nullopt;
  }
  // Walk path components. Reject "." / ".." / empty (consecutive
  // slashes) outright — we don't want canonicalization to hide a
  // traversal from the subsequent escape check.
  std::string_view remaining{*decoded};
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
    if (slash == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(slash + 1);
  }
  return decoded;
}

// Resolve the request path against client_root. Returns the absolute
// path if it resolves inside `client_root`; nullopt on filesystem
// error or escape.
auto resolve_under_root(const std::filesystem::path& client_root,
                        std::string_view normalized_path)
    -> std::optional<std::filesystem::path> {
  std::error_code ec;
  std::filesystem::path combined = client_root / normalized_path;
  auto canonical = std::filesystem::weakly_canonical(combined, ec);
  if (ec) {
    return std::nullopt;
  }
  // weakly_canonical preserves non-existent tail segments but
  // collapses `..` early ones; compare prefix element-wise.
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

auto make_404(const drogon::HttpRequestPtr& /*req*/)
    -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k404NotFound);
  resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
  resp->setBody("not found");
  return resp;
}

auto make_503() -> drogon::HttpResponsePtr {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k503ServiceUnavailable);
  resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
  resp->setBody("shutting down");
  return resp;
}

auto handle_request(
    const drogon::HttpRequestPtr& req,
    // Drogon's handler signature requires `Callback&&`; the callback is invoked
    // (not moved-from) and Drogon owns the storage.
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const std::string& name, const std::string& version,
    const std::string& path) -> void {
  if (g_shutdown_pending.load(std::memory_order_acquire)) {
    cb(make_503());
    return;
  }

  std::filesystem::path client_root;
  std::string checksum;
  {
    std::shared_lock lk(g_mu);
    auto it = g_routes.find(compose_key(name, version));
    if (it == g_routes.end()) {
      cb(make_404(req));
      return;
    }
    client_root = it->second.client_root;
    checksum = it->second.manifest_checksum;
  }

  auto normalized = normalize_request_path(path);
  if (!normalized.has_value()) {
    cb(make_404(req));
    return;
  }

  auto resolved = resolve_under_root(client_root, *normalized);
  if (!resolved.has_value()) {
    cb(make_404(req));
    return;
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(*resolved, ec) || ec) {
    cb(make_404(req));
    return;
  }

  std::string ext = ascii_lower(resolved->extension().string());
  std::string_view mime = mime_for_extension(ext);

  auto resp = drogon::HttpResponse::newFileResponse(
      resolved->string(), // fullPath
      "",                 // attachmentFileName (none — in-line serve)
      drogon::CT_CUSTOM,  // custom content-type (we set it below)
      std::string{mime});
  resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
  if (!checksum.empty()) {
    resp->addHeader("ETag", std::string{"W/\""} + checksum + "\"");
  }
  cb(resp);
}

} // namespace

auto register_drogon_handler() -> void {
  // Regex path: segment-safe capture for name + version + remainder.
  // `([^/]+)` forbids slashes in name/version; `(.*)` accepts zero or
  // more path remainder chars (including none for directory hits).
  drogon::app().registerHandlerViaRegex(
      "/ext/([^/]+)/([^/]+)/(.*)",
      // Drogon handler signature; cb is forwarded via std::move.
      [](const drogon::HttpRequestPtr& req,
         std::function<void(const drogon::HttpResponsePtr&)>&& cb,
         const std::string& name, const std::string& version,
         const std::string& path) {
        handle_request(req, std::move(cb), name, version, path);
      },
      {drogon::Get});
  spdlog::info("asset server registered at /ext/{{name}}/{{version}}/*");
}

auto register_routes(std::string_view name, std::string_view version,
                     const std::filesystem::path& client_root,
                     std::string_view manifest_checksum) -> void {
  std::unique_lock lk(g_mu);
  g_routes.insert_or_assign(
      compose_key(name, version),
      RouteHandle{.client_root = client_root,
                  .manifest_checksum = std::string{manifest_checksum}});
}

auto unregister_routes(std::string_view name, std::string_view version)
    -> void {
  std::unique_lock lk(g_mu);
  g_routes.erase(compose_key(name, version));
}

auto dispatch_for_test(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)> cb,
                       const std::string& name, const std::string& version,
                       const std::string& path) -> void {
  handle_request(req, std::move(cb), name, version, path);
}

auto cancel_all_registrations() noexcept -> void {
  g_shutdown_pending.store(true, std::memory_order_release);
  try {
    std::unique_lock lk(g_mu);
    g_routes.clear();
  } catch (...) {
    // exception here is intentionally swallowed so shutdown can
    // proceed.
    // noexcept — swallow any exception from map clear / lock
  }
}

namespace {

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

} // namespace

auto restore_routes(const Config::Database& db,
                    const std::filesystem::path& data_dir) -> void {
  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::warn("asset_server::restore_routes: PG connect failed: {}",
                 PQerrorMessage(conn));
    PQfinish(conn);
    return;
  }
  auto cleanup = [](PGconn* c) { PQfinish(c); };
  std::unique_ptr<PGconn, decltype(cleanup)> guard(conn, cleanup);

  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexec(conn, "SELECT name, version, manifest_checksum "
                   "FROM plinth.packages "
                   "WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED')"),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::warn("asset_server::restore_routes: SELECT failed: {}",
                 PQresultErrorMessage(res.get()));
    return;
  }

  int row_count = PQntuples(res.get());
  int restored = 0;
  for (int i = 0; i < row_count; ++i) {
    std::string_view name{PQgetvalue(res.get(), i, 0)};
    std::string_view version{PQgetvalue(res.get(), i, 1)};
    std::string_view checksum{PQgetvalue(res.get(), i, 2)};
    auto client_root = data_dir / "extensions" / std::string{name} /
                       std::string{version} / "client";
    std::error_code ec;
    if (!std::filesystem::is_directory(client_root, ec) || ec) {
      spdlog::warn("asset_server::restore_routes: client dir missing "
                   "for {} {} at {} — skipping",
                   name, version, client_root.string());
      continue;
    }
    register_routes(name, version, client_root, checksum);
    ++restored;
  }
  spdlog::info("asset server restored {} package route(s)", restored);
}

} // namespace plinth::packages::asset_server
