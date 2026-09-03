#pragma once

// plinth::packages::asset_server — public extension-asset serving.
//
// ICD-0.4.4 §ACTIVATING. Serves `GET /ext/{name}/{version}/*` against
// `{data_dir}/extensions/{name}/{version}/client/{path}`.
//
// Design choice vs ICD: the ICD implies one Drogon route per installed
// `(name, version)` pair. Drogon offers no route-deregistration
// primitive, so we register **one** regex route at bootstrap and route
// inside the handler through an internal `(name, version) -> RouteHandle`
// map. `register_routes` / `unregister_routes` mutate the map (fast,
// lock-guarded); the Drogon route list never changes after boot.
//
// Deterministic closeout: the process coordinator calls
// `cancel_all_registrations()` before `drogon::app().quit()`, flipping a
// shutdown flag so the wildcard trampoline returns 503 while teardown drains.

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace plinth::packages::asset_server {

// Register the single wildcard Drogon handler. Must be called exactly
// once per process, from the service-mode main thread, BEFORE
// `drogon::app().run()`. The handler pattern is:
//   /ext/([^/]+)/([^/]+)/(.*)
// — captures name, version, and the remainder. Requests with zero-
// length `path` (e.g. `/ext/foo/1.0.0/`) hit the handler and resolve
// to the `client/` directory (→ 404 per §4.1 no directory index).
auto register_drogon_handler() -> void;

// Add a (name, version) → `client_root` entry to the in-memory route
// map. Subsequent `GET /ext/{name}/{version}/*` requests will resolve
// against `client_root`. Replaces any prior entry for the same pair.
// `manifest_checksum` is stored and returned as the ETag on every
// served asset.
auto register_routes(std::string_view name, std::string_view version,
                     const std::filesystem::path& client_root,
                     std::string_view manifest_checksum) -> void;

// Remove the (name, version) entry. Future requests for that pair
// return 404. Used by 0.4.4 ACTIVATING rollback and by 0.4.5's
// uninstall path. Safe to call with no matching entry.
auto unregister_routes(std::string_view name, std::string_view version) -> void;

// Replay every ACTIVE or ACTIVE_FLAGGED package's route entry from
// `plinth.packages`. Called once from main.cpp after bootstrap but
// before `drogon::app().run()` so ACTIVE packages survive kernel
// restarts. A failure on one row is logged and skipped; never aborts
// bootstrap (one broken package must not prevent kernel startup).
auto restore_routes(const Config::Database& db,
                    const std::filesystem::path& data_dir) -> void;

// Deterministic-closeout hook. Clears the route map, flips the
// shutdown flag so the wildcard handler serves 503. It is noexcept so the
// coordinator can always close this ingress path.
auto cancel_all_registrations() noexcept -> void;

// Direct-dispatch entry for tests. Mirrors the signature of the
// Drogon regex handler registered by `register_drogon_handler` — takes
// the decomposed `(name, version, path)` tuple and invokes `cb` with
// either the served file response or a 404 / 503. Exposed so unit
// tests can exercise path resolution, MIME lookup, cache headers, and
// URL-traversal rejection WITHOUT starting the Drogon listener (which
// pulls in the WebSocket stack and its parked trantor TimerQueue
// teardown race — project_ws_flaky_segfault.md). Not intended for
// production callers; production dispatch goes through the regex
// handler registered via `register_drogon_handler`.
auto dispatch_for_test(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)> cb,
                       const std::string& name, const std::string& version,
                       const std::string& path) -> void;

} // namespace plinth::packages::asset_server
