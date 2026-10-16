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

#include "kernel/packages/generation_capture.hpp"
#include "kernel/shell/active_frontend.hpp"
#include "kernel/shell/platform_graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <atomic>
#include <chrono>
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
  std::string platform_profile;
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
        out.platform_profile = resp->getHeader("X-Plinth-Platform-Profile");
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
  REQUIRE(r.csp.find("'sha256-cCDc4AaNiyEAbj29NffEKnWAezVHyPJNEKKLUd8ZTkw='") !=
          std::string::npos);
  REQUIRE(r.csp.substr(0, r.csp.find(';')).find("unsafe-inline") ==
          std::string::npos);
  REQUIRE(r.csp.find("style-src 'self'") != std::string::npos);
  REQUIRE(r.csp.find("connect-src 'self'") != std::string::npos);
  REQUIRE(r.content_type_is_custom);
}

// ── M.02 — Mutable named JS asset revalidates with JS mime ────────────

TEST_CASE("M.02: GET /app/shell.js → 200 with application/javascript and "
          "revalidation",
          "[shell][active-frontend][M.02]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html>"},
      {"shell.js", "export const x = 1;"},
  });
  auto r = capture_app(make_active(root), "shell.js");
  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "export const x = 1;");
  REQUIRE(r.cache_control == "no-cache");
  REQUIRE(r.content_type_is_custom);
}

// ── M.03 — Named index.html gets no-cache + html mime ────────────────

TEST_CASE("mutable frontend styles fonts and module aliases revalidate",
          "[shell][active-frontend][cache]") {
  const std::vector<std::pair<std::string, std::string>> assets{
      {"tokens.css", ":root { --version: 2; }"},
      {"font.woff2", "font-bytes"},
      {"dependency.mjs", "export const version = 2;"},
      {"icon.svg", "<svg/>"},
      {"config.json", "{}"},
  };
  auto root = scratch_client_root(assets);
  for (const auto& [name, contents] : assets) {
    INFO(name);
    auto response = capture_app(make_active(root), name);
    REQUIRE(response.status == drogon::k200OK);
    REQUIRE(response.cache_control == "no-cache");
    REQUIRE(response.body == contents);
  }
}

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

TEST_CASE("entry opt-in resolves the whole asset graph against its package",
          "[shell][active-frontend][cache]") {
  const std::string document =
      "<head><!-- PLINTH_VERSIONED_ASSET_BASE -->"
      "<script type=\"importmap\">{\"imports\":{\"preact\":"
      "\"./vendor/preact.module.js\"}}</script></head>";
  auto root = scratch_client_root({{"index.html", document}});
  auto active = make_active(root);
  for (const std::string version : {"901.0.1", "901.0.2+build.7"}) {
    active.version = version;
    for (const std::string path : {"", "index.html", "nested/route"}) {
      auto response = capture_app(active, path);
      REQUIRE(response.status == drogon::k200OK);
      REQUIRE(response.cache_control == "no-cache");
      REQUIRE(response.body.find("PLINTH_VERSIONED_ASSET_BASE") ==
              std::string::npos);
      const auto encoded_version =
          version == "901.0.1" ? "901.0.1" : "901.0.2%2Bbuild.7";
      REQUIRE(response.body ==
              "<head><base href=\"/ext/shell/" + std::string{encoded_version} +
                  "/\"><script type=\"importmap\">{\"imports\":{\"preact\":"
                  "\"./vendor/preact.module.js\"}}</script></head>");
      REQUIRE(response.csp.find(
                  "'sha256-cCDc4AaNiyEAbj29NffEKnWAezVHyPJNEKKLUd8ZTkw='") !=
              std::string::npos);
    }
  }
  fs::remove_all(root.parent_path());
}

TEST_CASE("asset base encodes route data and custom entries stay unchanged",
          "[shell][active-frontend][cache]") {
  const std::string custom =
      "<head><base href=\"/custom/\"></head><script src=\"entry.js\"></script>";
  auto root = scratch_client_root({
      {"main.html", custom},
      {"index.html", "<!-- PLINTH_VERSIONED_ASSET_BASE -->"},
      {"note.js", "// <!-- PLINTH_VERSIONED_ASSET_BASE -->"},
  });
  auto active = make_active(root);
  active.name = "name\"<>&/";
  active.version = "1.0.0+build\"/";
  auto response = capture_app(active, "");
  REQUIRE(response.body ==
          "<base href=\"/ext/name%22%3C%3E%26%2F/1.0.0%2Bbuild%22%2F/\">");
  REQUIRE(capture_app(active, "note.js").body ==
          "// <!-- PLINTH_VERSIONED_ASSET_BASE -->");
  active.entry = "main.html";
  active.mount = "/console";
  REQUIRE(capture_app(active, "").body == custom);
  REQUIRE(capture_app(active, "deep/link").body == custom);
  fs::remove_all(root.parent_path());
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
  REQUIRE(r2.cache_control == "no-cache");
}

// ── Custom root_redirect honoured by the `/` handler ─────────────────

TEST_CASE("`/` handler honours shell.root_redirect override",
          "[shell][active-frontend]") {
  auto r = capture_redirect("/console/");
  REQUIRE(r.status == drogon::k302Found);
  REQUIRE(r.location == "/console/");
}

TEST_CASE("shell policy bounds resources and preserves authorized styles",
          "[shell][active-frontend][security]") {
  auto root = scratch_client_root({
      {"index.html", "<!doctype html><!-- PLINTH_VERSIONED_ASSET_BASE -->"},
      {"shell.js", "export const example = true;"},
  });
  auto active = make_active(root);
  const auto document = capture_app(active, "");
  REQUIRE(document.body.find("<base href=\"/ext/shell/0.6.1/\">") !=
          std::string::npos);
  for (const auto* directive :
       {"default-src 'self'", "style-src 'self' 'unsafe-inline'",
        "connect-src 'self'", "img-src 'self'", "font-src 'self'",
        "media-src 'self'", "frame-src 'self'", "frame-ancestors 'self'",
        "base-uri 'self'", "form-action 'self'", "object-src 'none'"}) {
    REQUIRE(document.csp.find(directive) != std::string::npos);
  }
  REQUIRE(document.csp.find(
              "script-src 'self' "
              "'sha256-cCDc4AaNiyEAbj29NffEKnWAezVHyPJNEKKLUd8ZTkw='") == 0);
  REQUIRE(capture_app(active, "shell.js").csp == document.csp);
  REQUIRE(capture_app(active, "nested/route").csp == document.csp);
}

TEST_CASE("replacement frontend preserves its existing policy with either base "
          "contract",
          "[shell][active-frontend][security]") {
  const std::string legacy_policy =
      "script-src 'self' "
      "'sha256-cCDc4AaNiyEAbj29NffEKnWAezVHyPJNEKKLUd8ZTkw='; "
      "style-src 'self' 'unsafe-inline'; connect-src 'self'";
  for (const auto& body :
       {std::string{"<!doctype html><base href=\"https://example.invalid/\">"},
        std::string{"<!doctype html><!-- PLINTH_VERSIONED_ASSET_BASE -->"}}) {
    auto root = scratch_client_root({
        {"index.html", body},
        {"app.js", "export const custom = true;"},
    });
    auto active = make_active(root, "/console");
    active.name = "replacement";
    const auto document = capture_app(active, "");
    REQUIRE(document.csp == legacy_policy);
    REQUIRE(capture_app(active, "app.js").csp == legacy_policy);
    REQUIRE(capture_app(active, "nested/route").csp == legacy_policy);
    if (body.find("PLINTH_VERSIONED_ASSET_BASE") == std::string::npos) {
      REQUIRE(document.body == body);
    } else {
      REQUIRE(document.body.find("<base href=\"/ext/replacement/0.6.1/\">") !=
              std::string::npos);
    }
  }
}

TEST_CASE("captured bundled platform pins one document graph without mutable "
          "entry IO or permanent artifact lease",
          "[shell][active-frontend][platform-graph]") {
  namespace generation = plinth::packages::generation;
  namespace shell = plinth::shell;
  shell::test_seam::set_active_frontend(std::nullopt);
  generation::Budget budget;
  const generation::CaptureRequest request{
      .key = {.package_id = "00000000-0000-4000-8000-000000000046",
              .name = "shell",
              .version = "test-version",
              .profile = 1},
      .root = fs::path{CMAKE_SOURCE_DIR} / "client" / "shell",
      .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10},
      .cancellation = {},
  };
  auto artifact = generation::capture_artifact(request, budget);
  REQUIRE(artifact.has_value());
  const auto* template_file = (*artifact)->find("client/index.html");
  REQUIRE(template_file != nullptr);
  // The module-syntax example in an HTML comment precedes the import map.
  // Recognition must locate the actual trusted entry tag, not this example.
  REQUIRE(template_file->content.find("<script type=\"module\"> on the page") <
          template_file->content.find("PLINTH_PLATFORM_IMPORT_MAP_BEGIN"));
  generation::Budget no_space{0};
  auto busy = shell::prepare_platform(**artifact, no_space);
  REQUIRE_FALSE(busy.has_value());
  REQUIRE(busy.error() == shell::PlatformGraphError::BUSY);
  auto prepared = shell::prepare_platform(**artifact, budget);
  REQUIRE(prepared.has_value());
  REQUIRE((*prepared)->key() == request.key);
  REQUIRE((*prepared)->artifact_digest() == (*artifact)->digest());
  REQUIRE((*prepared)->client_graph_digest().size() == 64);
  REQUIRE((*prepared)->profile() ==
          "v1:" + request.key.package_id + ":" + (*artifact)->digest());
  const auto base = "/_plinth/platform/" + request.key.package_id + "/" +
                    (*artifact)->digest() + "/client/";
  REQUIRE((*prepared)->base() == base);
  const auto& body = (*prepared)->document();
  REQUIRE(body.size() <= std::size_t{64} * 1024);
  REQUIRE(body.find("<base href=\"" + base + "\">") != std::string::npos);
  REQUIRE(body.find("name=\"plinth-platform-profile\" content=\"" +
                    (*prepared)->profile()) != std::string::npos);
  REQUIRE(body.find("\"@plinth/frontend/sdk\":\"" + base + "sdk.js\"") !=
          std::string::npos);
  REQUIRE(body.find("\"preact\":\"" + base + "vendor/preact.module.js\"") !=
          std::string::npos);
  REQUIRE(body.find("\"preact/hooks\":\"" + base +
                    "vendor/preact-hooks.module.js\"") != std::string::npos);
  REQUIRE(body.find("\"htm\":\"" + base + "vendor/htm.module.js\"") !=
          std::string::npos);
  REQUIRE(body.find("/api/frontend/sdk.js") == std::string::npos);
  REQUIRE(body.find("PLINTH_PLATFORM_IMPORT_MAP_BEGIN") == std::string::npos);
  const auto import_map = body.find("<script type=\"importmap\">");
  REQUIRE(import_map != std::string::npos);
  REQUIRE(body.find("<script type=\"importmap\">", import_map + 1) ==
          std::string::npos);
  REQUIRE(import_map <
          body.find("<script type=\"module\" src=\"shell.js\"></script>"));
  const auto& policy = (*prepared)->policy();
  REQUIRE(policy.starts_with("script-src 'self' 'sha256-"));
  REQUIRE(policy.substr(0, policy.find(';')).find("unsafe-inline") ==
          std::string::npos);
  REQUIRE(policy.find("object-src 'none'") != std::string::npos);
  REQUIRE(policy.find("frame-ancestors 'self'") != std::string::npos);

  // No template/path/asset is read from this intentionally missing directory.
  shell::ActiveFrontend active{
      .id = request.key.package_id,
      .name = "shell",
      .version = request.key.version,
      .mount = "/app",
      .entry = "index.html",
      .client_dir = "/no-such-platform-client",
      .bundled = true,
      .platform = {},
  };
  REQUIRE(shell::publish_platform_frontend(active, *prepared));
  active.platform = shell::current_platform();
  auto served = capture_app(active, "nested/route");
  REQUIRE(served.status == drogon::k200OK);
  REQUIRE(served.body == body);
  REQUIRE(served.csp == policy);
  REQUIRE(served.platform_profile == (*prepared)->profile());
  REQUIRE(served.cache_control == "no-cache");
  auto wrong = active;
  wrong.bundled = false;
  REQUIRE_FALSE(shell::publish_platform_frontend(wrong, *prepared));
  wrong = active;
  wrong.version = "other-version";
  REQUIRE_FALSE(shell::publish_platform_frontend(wrong, *prepared));
  const auto capture_charge = (*artifact)->charged_bytes();
  const auto before_release = budget.used();
  artifact->reset();
  REQUIRE(budget.used() == before_release - capture_charge);
  REQUIRE(shell::current_platform()->document() == body);
  shell::retire_platform_frontend(active.id);
  REQUIRE_FALSE(shell::current_platform());
  shell::test_seam::set_active_frontend(std::nullopt);

  // A physically captured but changed/extra client graph is not recognized by
  // version/profile labels. Only the compiled complete graph is supported.
  auto copy_root = scratch_client_root({}).parent_path();
  fs::copy(request.root, copy_root,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  {
    std::ofstream changed(copy_root / "client" / "sdk.js", std::ios::binary);
    changed << "export const changed = true;";
  }
  auto changed_request = request;
  changed_request.root = copy_root;
  changed_request.deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{10};
  auto changed = generation::capture_artifact(changed_request, budget);
  REQUIRE(changed.has_value());
  auto incompatible = shell::prepare_platform(**changed, budget);
  REQUIRE_FALSE(incompatible.has_value());
  REQUIRE(incompatible.error() == shell::PlatformGraphError::INCOMPATIBLE);
  fs::remove_all(copy_root);
}
