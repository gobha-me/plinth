#pragma once

// plinth::shell::active_frontend — manifest-driven mount dispatch for
// the active frontend extension (ICD-0.6.1 §4.4).
//
// 0.6.0's `register_shell_routes` wired hardcoded `/` redirect + `/app/*`
// SPA-fallback assuming the bundled shell. ICD-0.6.1 generalises:
//
//   • The active-frontend `plinth.packages` row (provenance='bundled' OR
//     'user', frontend_mount IS NOT NULL, state IN ACTIVE/ACTIVE_FLAGGED)
//     drives the mount + entry. The active_frontend resolver returns the
//     row's mount/entry/client_dir; the route registrar wires
//     `<mount>(.*)` against it.
//
//   • Bytes still come from `<data_dir>/extensions/<name>/<version>/client/`
//     (unchanged from 0.6.0). Strict CSP + immutable-cache for named
//     assets / no-cache for the SPA index file (`frontend.entry`) is
//     preserved verbatim.
//
//   • Path-traversal hardening (`weakly_canonical` prefix check, reject
//     `..` / `.` / NUL components) is preserved verbatim.
//
//   • SPA-fallback heuristic (no extension or empty path → entry file)
//     unchanged from 0.6.0 §8.2.

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace plinth::shell {

struct ActiveFrontend {
  std::string id;      // plinth.packages.id
  std::string name;    // e.g. "shell"
  std::string version; // e.g. "0.6.1"
  std::string mount;   // e.g. "/app" (no trailing slash)
  std::string entry;   // e.g. "index.html"
  std::filesystem::path
      client_dir; // <data_dir>/extensions/<name>/<version>/client/
};

// Resolve the active frontend row from `plinth.packages`. Picks the
// single ACTIVE row with `frontend_mount IS NOT NULL`. Returns nullopt
// if there's no active frontend (e.g. admin uninstalled the shell);
// the caller treats that as "skip route registration" — `GET /` and
// `<mount>/*` return 404 in that case (ICD-0.6.1 §4.7).
auto resolve_active_frontend(const Config::Database& db,
                             const std::filesystem::path& data_dir)
    -> std::optional<ActiveFrontend>;

// Register `/` redirect + `<mount>(.*)` SPA fallback against drogon::app().
// Idempotent within one process; call exactly once at bootstrap, after
// every `/api/*`, `/ws`, `/healthz`, `/ext/*` registration so the catch-
// all glob does NOT shadow kernel API surfaces (ICD-0.6.0 §8.1 ordering
// rule preserved).
//
// No-op when `cfg.enabled == false` (config-driven headless deployment).
auto register_active_frontend_routes(const Config::Shell& cfg_shell,
                                     const ActiveFrontend& active) -> void;

// Combined helper: resolve + register. Use from main.cpp; returns
// false if no active frontend (caller may log; the kernel keeps
// running headless).
auto register_routes_for_active_frontend(const Config::Shell& cfg_shell,
                                         const Config::Database& db,
                                         const std::filesystem::path& data_dir)
    -> bool;

// ── Test seams ────────────────────────────────────────────────────
// Production callers go through Drogon's regex dispatch via
// `register_active_frontend_routes`. The seams below let unit tests
// exercise the handler logic without starting the Drogon listener.
namespace test_seam {

[[nodiscard]] auto make_root_redirect(std::string_view target)
    -> drogon::HttpResponsePtr;

// Invoke the SPA-fallback handler logic with a synthesized request.
// `path_after_mount` is the captured remainder (no leading slash).
auto dispatch_app(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)> cb,
                  const std::string& path_after_mount) -> void;

// Override the active-frontend used by `dispatch_app`. Pass nullopt
// to simulate "no active frontend installed". Replaces any
// resolution from a prior `register_active_frontend_routes` call.
auto set_active_frontend(std::optional<ActiveFrontend> active) -> void;

} // namespace test_seam

} // namespace plinth::shell
