// Asset-server request-dispatch tests (ICD-0.4.4 I.13–I.15).
//
// These tests call `asset_server::dispatch_for_test` directly rather
// than going through Drogon's listener + regex router. The production
// dispatch path is identical — `handle_request` (the body of the
// regex handler) is what `dispatch_for_test` invokes — but skipping
// the listener means no WebSocket stack, no trantor TimerQueue, and
// no parked `std::bad_weak_ptr` teardown race
// (project_ws_flaky_segfault.md). The `newFileResponse` call inside
// `handle_request` is synchronous and works without a running Drogon
// app instance.
//
// I.13 verifies happy-path GET on a registered (name, version) — 200
// with Cache-Control: immutable + ETag. I.14 verifies URL-encoded
// path traversal resolves to 404 (not 200 or 500). I.15 verifies
// requests for an unregistered (name, version) return 404.

#include "kernel/packages/asset_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_scratch_counter{0};

// Creates a throwaway tree:
//   /tmp/plinth_asset_test_<pid>_<counter>/extensions/<name>/<version>/client/<file>
// Returns the client/ root so it can be handed to `register_routes`.
auto scratch_package(const std::string& name, const std::string& version,
                     const std::string& file, const std::string& body)
    -> fs::path {
  auto id = std::to_string(::getpid()) + "_" +
            std::to_string(g_scratch_counter.fetch_add(1));
  auto base = fs::temp_directory_path() / ("plinth_asset_test_" + id);
  auto client_root = base / "extensions" / name / version / "client";
  fs::create_directories(client_root);
  std::ofstream out(client_root / file, std::ios::binary);
  out << body;
  out.close();
  return client_root;
}

// Captures the HttpResponsePtr the dispatcher hands back, for assertion.
struct CapturedResponse {
  bool received{false};
  drogon::HttpStatusCode status{drogon::k200OK};
  std::string body;
  bool content_type_is_custom{false};
  std::string cache_control;
  std::string etag;
};

// Synchronously calls `dispatch_for_test` and returns what the handler
// would have written to its callback. The dispatcher path inside
// `handle_request` is synchronous — there is no event loop involvement
// for the 200 / 404 / 503 paths exercised here.
auto dispatch_and_capture(const std::string& name, const std::string& version,
                          const std::string& path) -> CapturedResponse {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  CapturedResponse out;
  plinth::packages::asset_server::dispatch_for_test(
      req,
      [&out](const drogon::HttpResponsePtr& resp) {
        out.received = true;
        if (resp) {
          out.status = resp->statusCode();
          out.body = std::string{resp->body()};
          // Drogon's HttpResponse stores Content-Type as an enum
          // (getContentType -> ContentType) when set via
          // CT_CUSTOM. The custom string itself is written into
          // the serialized HTTP frame but has no public getter,
          // so the best we can do without Drogon's serializer is
          // assert `getContentType() == CT_CUSTOM` (meaning the
          // handler DID set it, which proves the MIME lookup
          // path ran). MIME-table correctness is covered by the
          // non-PG unit tests for `mime_for_extension`.
          out.content_type_is_custom =
              resp->getContentType() == drogon::CT_CUSTOM;
          out.cache_control = resp->getHeader("Cache-Control");
          out.etag = resp->getHeader("ETag");
        }
      },
      name, version, path);
  return out;
}

} // namespace

TEST_CASE("I.13: GET /ext/{name}/{version}/main.js on ACTIVE returns 200 "
          "with immutable cache + ETag",
          "[asset_server][integration][I.13]") {
  auto client_root =
      scratch_package("i13-notes", "1.2.3", "main.js", "export default 42;\n");
  plinth::packages::asset_server::register_routes("i13-notes", "1.2.3",
                                                  client_root, "checksum-i13");

  auto r = dispatch_and_capture("i13-notes", "1.2.3", "main.js");

  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k200OK);
  REQUIRE(r.body == "export default 42;\n");
  REQUIRE(r.content_type_is_custom); // MIME-table lookup ran + set CT_CUSTOM
  REQUIRE(r.cache_control.find("immutable") != std::string::npos);
  REQUIRE(r.cache_control.find("max-age") != std::string::npos);
  REQUIRE(r.etag.find("checksum-i13") != std::string::npos);

  plinth::packages::asset_server::unregister_routes("i13-notes", "1.2.3");
}

TEST_CASE("I.14: URL-encoded path traversal resolves to 404, not 200 or 500",
          "[asset_server][integration][I.14]") {
  auto client_root = scratch_package("i14-notes", "1.2.3", "main.js", "ok\n");
  // Also place a "secret" file OUTSIDE client/ that traversal could hit.
  auto secret_path = client_root.parent_path() / "secret.txt";
  {
    std::ofstream out(secret_path);
    out << "never-serve-this\n";
  }
  plinth::packages::asset_server::register_routes("i14-notes", "1.2.3",
                                                  client_root, "checksum-i14");

  // The Drogon regex router would URL-decode before handing `path`
  // to the handler — we pre-decode here to mirror that behaviour.
  // `..%2f` decodes to `../`; the handler's `normalize_request_path`
  // + `resolve_under_root` must reject the traversal.
  auto r = dispatch_and_capture("i14-notes", "1.2.3", "../secret.txt");

  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k404NotFound);
  REQUIRE(r.body.find("never-serve-this") == std::string::npos);

  plinth::packages::asset_server::unregister_routes("i14-notes", "1.2.3");
}

TEST_CASE("I.15: GET /ext/{unknown}/{version}/file returns 404",
          "[asset_server][integration][I.15]") {
  // No register_routes call — (i15-unknown, 9.9.9) isn't in the map.
  auto r = dispatch_and_capture("i15-unknown", "9.9.9", "anything.js");

  REQUIRE(r.received);
  REQUIRE(r.status == drogon::k404NotFound);
}
