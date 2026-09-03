#pragma once

// plinth::frontend::api_frontend — `/api/frontend/{tokens.css,sdk.js}`
// indirections per ICD-0.6.2 §6 + ICD-0.6.3 §5.
//
// Each route returns 302 with `Location: /ext/{name}/{version}/<target>`
// when a single ACTIVE frontend exists; 503 with a JSON diagnostic body
// (`no_active_frontend` / `multiple_active_frontends`) otherwise. Both
// routes share `resolve_active()` + `handle_redirect()`; only the
// target-path suffix differs (`/css/tokens.css` vs `/client/sdk.js`).
//
// Implementation deviation from ICD §6.5: the route registers WITHOUT
// the auth filter. Rationale: ICD §6.6 has the shell self-reference
// `/api/frontend/tokens.css` from its own login page (pre-auth), so an
// auth-required route would 401 the unstyled login UX. The bytes are
// not user-specific (§6.5 itself acknowledges) and audit is off (§10.3),
// so the auth filter's stated purpose ("surface user-id for audit")
// does not fire. Recorded in the §17 amendment block.

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

namespace plinth::frontend {

// Register `GET /api/frontend/tokens.css` and `GET /api/frontend/sdk.js`
// against drogon::app(). Idempotent within one process; call exactly
// once at bootstrap. Slot in main.cpp AFTER the kernel `/api/*` route
// registrations and BEFORE `register_routes_for_active_frontend`,
// preserving the catch-all ordering rule from ICD-0.6.0 §8.1.
auto register_api_frontend_routes(const Config::Database& db) -> void;

// ── Test seam ────────────────────────────────────────────────────
// Production callers go through Drogon's dispatch via
// `register_api_frontend_routes`. The seams below let unit tests
// exercise the handler logic without starting the Drogon listener
// (mirrors `plinth::shell::test_seam::dispatch_app`).
namespace test_seam {

auto dispatch_tokens_css(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const Config::Database& db) -> void;

auto dispatch_sdk_js(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                     const Config::Database& db) -> void;

} // namespace test_seam

} // namespace plinth::frontend
