#pragma once

// Authenticated, user-filtered application discovery for the shell launcher.
// See docs/icd/ICD-application-discovery-launcher.md section 6.

#include "kernel/config.hpp"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

namespace plinth::frontend {

// Register GET /api/frontend/applications with SessionFilter. Production uses
// Drogon's asynchronous database client and one statement snapshot.
auto register_application_routes() -> void;

namespace test_seam {

// Synchronous libpq-backed dispatch for focused handler tests. Authentication
// attributes must already be present on req, as they are after SessionFilter.
auto dispatch_applications(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const Config::Database& db) -> void;

} // namespace test_seam

} // namespace plinth::frontend
