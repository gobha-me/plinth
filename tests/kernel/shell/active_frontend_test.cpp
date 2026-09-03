// ICD-0.6.1 §12.2 — manifest-driven mount routing (M.* test family).
//
// Library-level coverage of the `plinth::shell::active_frontend`
// SPA-fallback handler logic. Tests inject the active frontend
// directly via `test_seam::set_active_frontend` and dispatch
// requests via `test_seam::dispatch_app`, bypassing Drogon's
// listener (which pulls in the WebSocket stack and its parked
// trantor TimerQueue teardown race per project_ws_flaky_segfault.md).
//
// M.06 (route ordering — `/api/*` not shadowed by frontend mount)
// and M.07 (`/ext/*` not shadowed) are documented manual smoke
// tests in `tests/shell/`; the ordering itself is enforced
// structurally by `main.cpp` placing the active-frontend route
// registration AFTER the kernel API + asset-server registrations.

#include "kernel/shell/active_frontend.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_af_test_counter{0};

// Build a throwaway client/ tree under /tmp with the given files.
// Returns the client/ root for use in an ActiveFrontend record.
auto scratch_client_root(
    const std::vector<std::pair<std::string, std::string>>& files) -> fs::path {
  auto id = std::to_string(::getpid()) + "_" +
            std::to_string(g_af_test_counter.fetch_add(1));
  auto base = fs::temp_directory_path() / ("plinth_af_test_" + id);
  auto client_root = base / "client";
  fs::create_directories(client_root);
  for (const auto& [name, body] : files) {
    std::ofstream out(client_root / name, std::ios::binary);
    out << body;
    out.close();
  }
  return client_root;
}

// Build a synthetic ActiveFrontend snapshot over the given client_root,
// mount, and entry. Used by every M.* case to seed test_seam.
auto make_active(const fs::path& client_root, std::string mount = "/app",
                 std::string entry = "index.html")
    -> plinth::shell::ActiveFrontend {
  return plinth::shell::ActiveFrontend{
      .id = "00000000-0000-0000-0000-000000000000",
      .name = "shell",
      .version = "0.6.1",
      .mount = std::move(mount),
      .entry = std::move(entry),
      .client_dir = client_root,
  };
}

struct Captured {
  bool received{false};
  drogon::HttpStatusCode status{drogon::k200OK};
  std::string body;
  std::string location;
  std::string cache_control;
  std::string csp;
  bool content_type_is_custom{false};
};

auto capture_app(const plinth::shell::ActiveFrontend& active,
                 const std::string& path_after_mount) -> Captured {
  plinth::shell::test_seam::set_active_frontend(active);
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  Captured out;
  plinth::shell::test_seam::dispatch_app(
      req,
      [&out](const drogon::HttpResponsePtr& resp) {
        out.received = true;
        if (!resp) {
          return;
        }
        out.status = resp->statusCode();
        out.body = std::string{resp->body()};
        out.location = resp->getHeader("Location");
        out.cache_control = resp->getHeader("Cache-Control");
        out.csp = resp->getHeader("Content-Security-Policy");
        out.content_type_is_custom =
            resp->getContentType() == drogon::CT_CUSTOM;
      },
      path_after_mount);
  return out;
}

auto capture_redirect(std::string_view target) -> Captured {
  auto resp = plinth::shell::test_seam::make_root_redirect(target);
  Captured out;
  out.received = true;
  if (resp) {
    out.status = resp->statusCode();
    out.location = resp->getHeader("Location");
  }
  return out;
}

} // namespace

// ── M.01 — `/` redirects to `/app/` (root → mount) ──────────────────

TEST_CASE("M.01: GET / returns 302 to /app/; GET /app/ serves index.html",
          "[shell][active-frontend][M.01]") {
  auto redir = capture_redirect("/app/");
  REQUIRE(redir.received);
  REQUIRE(redir.status == drogon::k302Found);
  REQUIRE(redir.location == "/app/");

  auto root = scratch_client_root({
      {"index.html", "<!doctype html><h1>shell</h1>"},
      {"shell.js", "console.log('shell');"},
  });
  auto r = capture_app(make_active(root), "");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "<!doctype html><h1>shell</h1>");
  REQUIRE(r.cache_control == "no-cache");
  REQUIRE(r.csp.find("script-src 'self'") != std::string::npos);
  REQUIRE(r.csp.find("style-src 'self'") != std::string::npos);
  REQUIRE(r.csp.find("connect-src 'self'") != std::string::npos);
  REQUIRE(r.content_type_is_custom);
}

// ── M.02 — Named JS asset gets immutable cache + JS mime ─────────────

TEST_CASE("M.02: GET /app/shell.js → 200 with application/javascript and "
          "immutable cache",
          "[shell][active-frontend][M.02]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html>"},
      {"shell.js", "export const x = 1;"},
  });
  auto r = capture_app(make_active(root), "shell.js");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "export const x = 1;");
  REQUIRE(r.cache_control.find("immutable") != std::string::npos);
  REQUIRE(r.cache_control.find("max-age=31536000") != std::string::npos);
  REQUIRE(r.content_type_is_custom);
}

// ── M.03 — Named index.html gets no-cache + html mime ────────────────

TEST_CASE("M.03: GET /app/index.html serves with no-cache + html mime",
          "[shell][active-frontend][M.03]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html><span>direct</span>"},
  });
  auto r = capture_app(make_active(root), "index.html");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "<!doctype html><span>direct</span>");
  REQUIRE(r.cache_control == "no-cache");
}

// ── M.04 — SPA fallback for path with no extension ───────────────────

TEST_CASE("M.04: GET /app/login (no extension, SPA fallback) → 200 index.html",
          "[shell][active-frontend][M.04]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html><span>SPA</span>"},
  });
  auto r = capture_app(make_active(root), "login");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "<!doctype html><span>SPA</span>");
  REQUIRE(r.cache_control == "no-cache");
}

// ── M.05 — Named asset miss returns 404 (no SPA fallback) ────────────

TEST_CASE("M.05: GET /app/missing.css → 404 (extension means no SPA fallback)",
          "[shell][active-frontend][M.05]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html>"},
  });
  auto r = capture_app(make_active(root), "missing.css");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k404NotFound);
}

// ── M.08 — No active frontend (post-uninstall) returns 404 ───────────

TEST_CASE("M.08: no active frontend → /app/* returns 404",
          "[shell][active-frontend][M.08]") {
  plinth::shell::test_seam::set_active_frontend(std::nullopt);
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  Captured out;
  plinth::shell::test_seam::dispatch_app(
      req,
      [&out](const drogon::HttpResponsePtr& resp) {
        out.received = true;
        if (resp) {
          out.status = resp->statusCode();
        }
      },
      "");
  REQUIRE(out.received);
  REQUIRE(out.status == drogon::k404NotFound);
}

// ── Path-traversal hardening (ICD-0.6.0 §11.1 contract preserved) ────

TEST_CASE("active_frontend rejects `..` traversal with 404",
          "[shell][active-frontend][traversal]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html>"},
  });
  auto r = capture_app(make_active(root), "../etc/passwd");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k404NotFound);
}

TEST_CASE("active_frontend rejects embedded NUL with 404",
          "[shell][active-frontend][traversal]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html>"},
  });
  std::string evil_path = std::string{"foo"} + '\0' + std::string{".html"};
  auto r = capture_app(make_active(root), evil_path);
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k404NotFound);
}

// ── Custom mount + entry — manifest-driven dispatch (ICD-0.6.1 §4.4) ─

TEST_CASE("active_frontend honours non-default mount and entry from manifest",
          "[shell][active-frontend]") {
  auto root = scratch_client_root({
      {"main.html", "<!doctype html><span>custom</span>"},
      {"app.js", "/* custom */"},
  });
  auto r = capture_app(make_active(root, "/console", "main.html"), "");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "<!doctype html><span>custom</span>");
  REQUIRE(r.cache_control == "no-cache");

  auto r2 = capture_app(make_active(root, "/console", "main.html"), "app.js");
  REQUIRE(r2.received);
  REQUIRE(r2.status == drogon::k200OK);
  REQUIRE(r2.cache_control.find("immutable") != std::string::npos);
}

// ── Custom root_redirect honoured by the `/` handler ─────────────────

TEST_CASE("`/` handler honours shell.root_redirect override",
          "[shell][active-frontend]") {
  auto r = capture_redirect("/console/");
  REQUIRE(r.status == drogon::k302Found);
  REQUIRE(r.location == "/console/");
}
