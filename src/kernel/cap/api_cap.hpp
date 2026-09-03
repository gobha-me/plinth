#pragma once

// plinth::cap::api_cap — `POST /api/cap/{capability}` HTTP cap-dispatch
// route per ICD-0.6.3 §5.
//
// Translates a browser-side `plinth.call(capability, args)` into a
// kernel-side `call_capability_async` dispatch and returns the result
// (or CapabilityError) as JSON. Reuses the SessionFilter-attached auth
// context per `auth::get_auth_context(req)`; populates a full
// `effective_rules` vector from `plinth.group_rules` (matching the
// RbacFilter SQL) so the resolver's step-3 RBAC check sees the same
// privilege set production callers do.
//
// Implementation deviations from ICD §5.1 (recorded in §17):
//   1. RbacFilter is NOT in the filter chain. The resolver enforces RBAC
//      step 3 from the capability's `rbac_rule` (per ICD-0.2.4 / 0.5.0.4).
//      Wiring RbacFilter would require per-capability rule registration,
//      which doesn't match the per-route static-table pattern.
//   2. CSRF check deferred — Plinth has no CSRF infrastructure today.
//      Lands cohesively across all `/api/*` mutating routes in a
//      follow-up.
//   3. URL `/api/cap/{capability}` accepts the bare dotted name
//      (e.g. `shell.preferences.set`); the handler synthesises the
//      resolver's full triple `<namespace>:1:<function>` by splitting on
//      the first `.`. Multi-version capabilities don't exist yet; future
//      expansion is a query parameter or path-versioning.
//   4. `effective_rules` is populated via the same DB query
//      `RbacFilter::doFilter` runs (sync libpq mirror; matches the
//      production WS path's stated intent — ws/call_dispatch.cpp's
//      admin-only shortcut is documented as LH-0-scope).
//   5. CapabilityError → HTTP code translation: `capability_not_found`
//      → `not_found` (404); `permission_denied` → `rbac_denied` (403);
//      other resolver errors → 500 with the kernel-native snake_case
//      `error_code()` literal.

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

namespace plinth::cap {

// Register `POST /api/cap/{capability}` against drogon::app(). Idempotent
// within one process; call exactly once at bootstrap. Slot in main.cpp
// after the kernel `/api/*` route registrations and before
// `frontend::register_api_frontend_routes` — preserves the catch-all
// ordering rule from ICD-0.6.0 §8.1. The route attaches SessionFilter
// only (not RbacFilter — see deviation #1 in this header).
auto register_cap_routes(const Config::Database& db) -> void;

// ── Test seam ────────────────────────────────────────────────────
// Production callers go through Drogon's dispatch via
// `register_cap_routes`. The seam below lets unit tests exercise the
// handler logic without starting the Drogon listener (mirrors
// `frontend::test_seam::dispatch_tokens_css`).
namespace test_seam {

auto dispatch_post_cap(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const Config::Database& db,
                       const std::string& capability) -> void;

} // namespace test_seam

} // namespace plinth::cap
