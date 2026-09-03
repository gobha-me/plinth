#pragma once

// HTTP integration-test fixture for `/api/packages` (ICD-0.4.4 §HTTP
// Surface). Built in 0.6.0.N session 2 (HTTP fixture + I.18 + I.20).
//
// The fixture drives real HTTP through the Drogon listener that
// `tests/kernel/ws/ws_test_fixture` already starts on port 28099 — the
// route registration was added there alongside the WS routes
// (matches production main.cpp:453-462 wiring). Tests therefore
// exercise the full production path: SessionFilter → RbacFilter →
// `handle_post_packages` (per `feedback_real_code_paths.md`).
//
// Why HTTP instead of the Session-1 `test_seam::dispatch_post`:
// `RbacFilter::doFilter` reads `req->getMatchedPathPattern()` to look
// up required rules. That field is only set by Drogon's router; manual
// in-process invocation would either need to reach into the private
// `HttpRequestImpl.h` header to call `setMatchedPathPattern`, or skip
// RbacFilter entirely (defeating I.20). The seam stays available for
// future handler-only tests that don't need filters.

#include "../ws/ws_test_fixture.hpp"

#include <cstddef>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::http_test {

class HttpTestFixture {
 public:
  // Resets the plinth schema, clears the per-process packages tempdir,
  // and ensures the Drogon test server (with /api/packages registered)
  // is running. Throws if PG is unavailable — call
  // `plinth::ws_test::pg_available()` and SKIP first.
  HttpTestFixture();
  ~HttpTestFixture() = default;

  HttpTestFixture(const HttpTestFixture&) = delete;
  auto operator=(const HttpTestFixture&) -> HttpTestFixture& = delete;
  HttpTestFixture(HttpTestFixture&&) = delete;
  auto operator=(HttpTestFixture&&) -> HttpTestFixture& = delete;

  // Seed a user, mark them admin (joins the bootstrap admin group +
  // grants packages.install via the kernel.admin rule), and create a
  // session row. Returns the raw session token.
  auto seed_admin(std::string_view username = "admin") -> std::string;

  // Seed a user with NO group memberships and create a session row.
  // Returns the raw session token. POSTs with this token reach
  // RbacFilter and get 403 (I.20).
  auto seed_non_admin(std::string_view username = "alice") -> std::string;

  // Read the canonical valid-install fixture zip baked into the build
  // tree by `plinth_install_fixture_zips`. Bytes are exactly what the
  // production `POST /api/packages` accepts. Same fixture used by
  // `install_lifecycle_test.cpp` (I.01, I.04, I.13, I.14, I.18-I.20).
  [[nodiscard]] static auto read_valid_install_zip() -> std::vector<std::byte>;

  // Read any install_lifecycle fixture zip by source-dir name. Names
  // listed at `CMakeLists.txt PLINTH_INSTALL_FIXTURES` (valid-install,
  // valid-install-no-panels, valid-install-frontend, missing-manifest,
  // fail-validator, fail-migration). I.19 uses `valid-install` for the
  // happy path and `fail-validator` for the validation-failure path.
  [[nodiscard]] static auto read_install_zip(std::string_view fixture_name)
      -> std::vector<std::byte>;

  // Build a `POST /api/packages` request with `bytes` posted as a
  // single multipart file. Cookie `plinth_session=<session_token>`
  // wired so SessionFilter accepts it.
  [[nodiscard]] auto build_post(std::span<const std::byte> bytes,
                                std::string_view session_token,
                                std::string_view query = "") const
      -> drogon::HttpRequestPtr;

  // Build a `PATCH /api/packages/{id}` request with JSON body
  // `{"action": "<action>"}`. Action must be `"disable"` or `"enable"`
  // for production to accept; the builder is unopinionated so tests
  // can drive 400 invalid-action paths too. Static — PATCH carries no
  // body bytes so no per-fixture scratch staging is needed (cf.
  // `build_post` which writes the multipart upload to scratch_dir).
  [[nodiscard]] static auto build_patch(std::string_view id,
                                        std::string_view action,
                                        std::string_view session_token)
      -> drogon::HttpRequestPtr;

  // Build a `DELETE /api/packages/{id}` request. `confirm=true`
  // appends the `?confirm=true` query the production handler
  // requires; pass `false` to drive the 400 missing-confirmation
  // path. Static — same rationale as `build_patch`.
  [[nodiscard]] static auto build_delete(std::string_view id, bool confirm,
                                         std::string_view session_token)
      -> drogon::HttpRequestPtr;

  // Send a request synchronously. Throws on transport error
  // (Drogon's `ReqResult` non-Ok); the caller asserts on the
  // returned response's status code + body.
  [[nodiscard]] auto dispatch(const drogon::HttpRequestPtr& req)
      -> drogon::HttpResponsePtr;

  // Spawn one thread per request, gate them on a barrier so they
  // hit the listener concurrently, return responses in input order.
  // Used by I.18 to race two same-name installs at the advisory
  // lock.
  [[nodiscard]] auto dispatch_concurrent(
      std::vector<drogon::HttpRequestPtr> reqs)
      -> std::vector<drogon::HttpResponsePtr>;

 private:
  // Per-fixture libpq handle for seeding users + sessions. Held as a
  // member so `seed_admin` / `seed_non_admin` are properly instance
  // methods rather than incidentally-static helpers, and so the
  // libpq connection is reused across seeds within a test.
  plinth::ws_test::TestPg pg;
  // newFileUploadRequest reads from disk; we stage the zip bytes into
  // this dir and reuse for build_post.
  std::filesystem::path scratch_dir;
  drogon::HttpClientPtr client;
};

} // namespace plinth::http_test
