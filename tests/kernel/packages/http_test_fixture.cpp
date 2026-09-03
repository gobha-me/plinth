#include "http_test_fixture.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <drogon/HttpAppFramework.h>
#include <drogon/UploadFile.h>
#include <fstream>
#include <future>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <utility>

namespace plinth::http_test {

namespace fs = std::filesystem;

namespace {

// Per-process counter so concurrent fixtures (parallel TEST_CASEs in
// one subprocess) don't fight over the same scratch path.
auto next_scratch_id() -> std::uint64_t {
  // counter
  static std::atomic<std::uint64_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

auto make_scratch_dir() -> fs::path {
  auto p = fs::temp_directory_path() /
           ("plinth_http_fixture_" + std::to_string(::getpid()) + "_" +
            std::to_string(next_scratch_id()));
  fs::create_directories(p);
  return p;
}

// Wipe `<packages_data_dir>/extensions` between tests so a prior install
// of the canonical "notes" extension doesn't poison the next install.
auto clear_packages_dir() -> void {
  auto extensions_dir =
      fs::path{plinth::ws_test::packages_data_dir()} / "data" / "extensions";
  std::error_code ec;
  fs::remove_all(extensions_dir, ec);
  fs::create_directories(extensions_dir, ec);
}

} // namespace

HttpTestFixture::HttpTestFixture()
    : pg(plinth::ws_test::test_config().db), scratch_dir(make_scratch_dir()) {
  // Ensure the test server is running with /api/packages registered
  // (route registration was added to `start_test_server()` in 0.6.0.N
  // session 2). Reset DB schema so each test sees a fresh
  // `plinth.packages` and `plinth.users`/`plinth.sessions`.
  auto port = plinth::ws_test::test_server_port();
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  clear_packages_dir();

  auto base = "http://127.0.0.1:" + std::to_string(port);
  client = drogon::HttpClient::newHttpClient(base);
}

auto HttpTestFixture::seed_admin(std::string_view username) -> std::string {
  auto user_id =
      plinth::ws_test::insert_user(pg, std::string{username}, "test-password");
  plinth::ws_test::make_admin(pg, user_id);
  auto raw_token = "raw-admin-token-" + std::string{username};
  plinth::ws_test::insert_session(pg, user_id, raw_token);
  return raw_token;
}

auto HttpTestFixture::seed_non_admin(std::string_view username) -> std::string {
  auto user_id =
      plinth::ws_test::insert_user(pg, std::string{username}, "test-password");
  auto raw_token = "raw-nonadmin-token-" + std::string{username};
  plinth::ws_test::insert_session(pg, user_id, raw_token);
  return raw_token;
}

auto HttpTestFixture::read_install_zip(std::string_view fixture_name)
    -> std::vector<std::byte> {
  auto path = fs::path{CMAKE_BINARY_DIR} / "fixtures" /
              (std::string{fixture_name} + ".zip");
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error(std::string{fixture_name} + ".zip missing at " +
                             path.string() +
                             " — was plinth_install_fixture_zips built?");
  }
  in.seekg(0, std::ios::end);
  auto n = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(n));
  // needs char*; std::byte is bit-compatible.
  in.read(reinterpret_cast<char*>(bytes.data()),
          static_cast<std::streamsize>(n));
  return bytes;
}

auto HttpTestFixture::read_valid_install_zip() -> std::vector<std::byte> {
  return read_install_zip("valid-install");
}

auto HttpTestFixture::build_post(std::span<const std::byte> bytes,
                                 std::string_view session_token,
                                 std::string_view query) const
    -> drogon::HttpRequestPtr {
  // Drogon's UploadFile reads from disk; stage bytes to a per-call
  // file in the fixture's scratch dir.
  auto upload_path =
      scratch_dir / ("upload-" + std::to_string(next_scratch_id()) + ".zip");
  {
    std::ofstream out(upload_path, std::ios::binary);
    // ostream::write needs char*; std::byte is bit-compatible.
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  drogon::UploadFile upload(upload_path.string(), "package.zip", "package");
  auto req = drogon::HttpRequest::newFileUploadRequest({upload});
  req->setMethod(drogon::Post);
  auto path = std::string{"/api/packages"};
  if (!query.empty()) {
    path += "?";
    path += std::string{query};
  }
  req->setPath(path);
  req->addCookie("plinth_session", std::string{session_token});
  return req;
}

auto HttpTestFixture::build_patch(std::string_view id, std::string_view action,
                                  std::string_view session_token)
    -> drogon::HttpRequestPtr {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Patch);
  req->setPath("/api/packages/" + std::string{id});
  req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  req->setBody(R"({"action":")" + std::string{action} + R"("})");
  req->addCookie("plinth_session", std::string{session_token});
  return req;
}

auto HttpTestFixture::build_delete(std::string_view id, bool confirm,
                                   std::string_view session_token)
    -> drogon::HttpRequestPtr {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Delete);
  auto path = "/api/packages/" + std::string{id};
  if (confirm) {
    path += "?confirm=true";
  }
  req->setPath(path);
  req->addCookie("plinth_session", std::string{session_token});
  return req;
}

auto HttpTestFixture::dispatch(const drogon::HttpRequestPtr& req)
    -> drogon::HttpResponsePtr {
  // Use an explicit promise/future bridge rather than HttpClient's
  // built-in synchronous overload so concurrent dispatch shares the
  // same `client` without the assert-fired-from-loop surprise. The
  // async overload is documented safe to call cross-thread.
  auto promise = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
  auto future = promise->get_future();
  client->sendRequest(
      req,
      [promise](drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
        if (r != drogon::ReqResult::Ok || !resp) {
          promise->set_exception(std::make_exception_ptr(std::runtime_error(
              "drogon::HttpClient transport error: ReqResult=" +
              std::to_string(static_cast<int>(r)))));
          return;
        }
        promise->set_value(resp);
      },
      /*timeout=*/30.0);
  return future.get();
}

auto HttpTestFixture::dispatch_concurrent(
    std::vector<drogon::HttpRequestPtr> reqs)
    -> std::vector<drogon::HttpResponsePtr> {
  auto n = reqs.size();
  std::vector<drogon::HttpResponsePtr> responses(n);
  std::barrier sync(static_cast<std::ptrdiff_t>(n));
  std::vector<std::thread> threads;
  threads.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    threads.emplace_back([this, i, &reqs, &responses, &sync]() {
      sync.arrive_and_wait();
      responses[i] = dispatch(reqs[i]);
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  return responses;
}

} // namespace plinth::http_test
