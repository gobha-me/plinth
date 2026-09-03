#pragma once

// plinth::packages — HTTP surface (ICD-0.4.4 §HTTP Surface).
//
// POST   /api/packages              — multipart/form-data zip upload
// GET    /api/packages              — list (paginated)
// GET    /api/packages/{id}         — single record
//
// All routes require SessionFilter + RbacFilter. `packages.install`
// gates POST; `packages.read` gates both GETs. `GET /ext/*` is a
// separate public route registered by asset_server::register_drogon_handler.

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace plinth::packages {

struct PackageRoutesConfig {
  Config::Database db;
  std::filesystem::path data_dir;
  std::filesystem::path staging_dir;
  std::size_t max_package_size_bytes = 50ULL * 1024ULL * 1024ULL;
  std::size_t upgrade_drain_timeout_ms = 5000; // ICD-0.4.5 §T2
};

// Register the three admin HTTP endpoints against the running Drogon
// app. Called from main.cpp after bootstrap, before `app().run()`.
auto register_package_routes(const PackageRoutesConfig& cfg) -> void;

// Test-only entry points: invoke a handler body directly without going
// through Drogon's listener / filter chain. The 0.6.0.N HTTP test
// fixture uses these to exercise the handlers in-process; the fixture
// is itself responsible for invoking SessionFilter + RbacFilter (or
// directly setting auth attributes on the req) before calling these,
// since the handlers expect `caller_user_id(req)` to resolve.
//
// Same pattern as `plinth::shell::test_seam::dispatch_app` (0.6.0).
namespace test_seam {

auto dispatch_post(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)> cb,
                   const PackageRoutesConfig& cfg) -> void;

auto dispatch_patch(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)> cb,
                    const PackageRoutesConfig& cfg, const std::string& id)
    -> void;

auto dispatch_delete(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)> cb,
                     const PackageRoutesConfig& cfg, const std::string& id)
    -> void;

// ICD-0.4.5 §T2 — process-wide override of the upgrade drain timeout.
// Tests for X.08 / X.09 set this to drive an in-flight cap.call past the
// drain window deterministically without rebuilding the kernel `Config`.
// `nullopt` (default) defers to
// `PackageRoutesConfig::upgrade_drain_timeout_ms`. Pattern mirrors
// `plinth::ws::test_seam::live_buffer_cap_override` (subscriptions.cpp:584).
[[nodiscard]] auto upgrade_drain_timeout_ms_override()
    -> std::optional<std::size_t>;
auto set_upgrade_drain_timeout_ms_override(std::size_t ms) -> void;
auto clear_upgrade_drain_timeout_ms_override() -> void;

} // namespace test_seam

} // namespace plinth::packages
