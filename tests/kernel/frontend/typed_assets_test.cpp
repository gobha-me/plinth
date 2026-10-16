#include "kernel/frontend/typed_assets.hpp"
#include "kernel/security/response_headers.hpp"

// Private pinned-framework serialization is test-only, never an application
// production dependency or an AdmittedAsset construction privilege.
#include "HttpResponseImpl.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace frontend = plinth::frontend;
namespace generation = plinth::packages::generation;
namespace drain = plinth::capabilities::drain;
namespace lifecycle = plinth::lifecycle;
using namespace std::chrono_literals;

class StreamFixture {
 public:
  StreamFixture() {
    std::array<char, 64> name{};
    const std::string pattern = "/tmp/plinth-typed-stream.XXXXXX";
    std::ranges::copy(pattern, name.begin());
    const auto* made = ::mkdtemp(name.data());
    if (made == nullptr) throw std::runtime_error("stream fixture creation");
    root = made;
    std::filesystem::create_directory(root / "client");
    write("client/main.js", "export const value = 1;\n");
    write("server.js", "not a client asset");
  }
  ~StreamFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  StreamFixture(const StreamFixture&) = delete;
  auto operator=(const StreamFixture&) -> StreamFixture& = delete;
  auto write(std::string_view path, std::string_view bytes) const -> void {
    std::ofstream file(root / path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("stream fixture write");
  }
  auto capture(generation::Budget& budget) const -> generation::CaptureResult {
    return generation::capture_artifact(
        {.key = {.package_id = "11111111-1111-1111-1111-111111111111",
                 .name = "typed-stream-fixture",
                 .version = "1.0.0",
                 .profile = 1},
         .root = root,
         .deadline = std::chrono::steady_clock::now() + 10s,
         .cancellation = {}},
        budget);
  }
  std::filesystem::path root;
};

auto guard() -> std::unique_ptr<drain::DispatchGuard> {
  return std::make_unique<drain::DispatchGuard>("typed-stream-fixture");
}
} // namespace

TEST_CASE("typed assets: physical callback-copy ownership",
          "[frontend][typed_assets]") {
  StreamFixture tree;
  generation::Budget budget;
  auto captured = tree.capture(budget);
  REQUIRE(captured.has_value());
  lifecycle::AsyncTaskRegistry tasks;
  frontend::TypedAssetStreams streams{budget};
  auto made = frontend::typed_assets::test_seam::make_source(
      streams, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE(made.has_value());
  REQUIRE(streams.active_count() == 1);
  REQUIRE(tasks.active_for_test() == 1);
  const auto fenced = drain::begin_drain("typed-stream-fixture");
  REQUIRE(fenced->in_flight.load() == 1);

  auto response = drogon::HttpResponse::newStreamResponse(*made);
  auto transport_copy = response->streamCallback();
  *made = {};
  response
      ->clear(); // Pinned Drogon invokes NULL here, BEFORE transport settles.
  REQUIRE(streams.active_count() == 1);
  REQUIRE(tasks.active_for_test() == 1);
  REQUIRE(fenced->in_flight.load() == 1);
  REQUIRE(transport_copy(nullptr, 0) == 0);
  REQUIRE(streams.active_count() == 1);

  tree.write("client/main.js", "changed after admission");
  std::array<char, 128> bytes{};
  const auto count = transport_copy(bytes.data(), bytes.size());
  REQUIRE(std::string_view{bytes.data(), count} == "export const value = 1;\n");
  REQUIRE(transport_copy(bytes.data(), bytes.size()) == 0);
  REQUIRE(streams.active_count() == 1); // EOF is not source disposal.
  REQUIRE_FALSE(tasks.drain(0ms));
  REQUIRE_FALSE(streams.drain(std::chrono::steady_clock::now()));
  REQUIRE(fenced->in_flight.load() == 1);
  transport_copy = {};
  REQUIRE(streams.active_count() == 0);
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE(fenced->in_flight.load() == 0);
  REQUIRE(streams.drain(std::chrono::steady_clock::now()));
  REQUIRE(tasks.drain(0ms));
  drain::end_drain("typed-stream-fixture");
}

TEST_CASE("typed assets: bounded source admission and stop",
          "[frontend][typed_assets]") {
  StreamFixture tree;
  generation::Budget budget;
  auto captured = tree.capture(budget);
  REQUIRE(captured.has_value());
  lifecycle::AsyncTaskRegistry tasks;
  frontend::TypedAssetStreams streams{budget};
  std::vector<frontend::AssetSource> sources;
  sources.reserve(64);
  for (std::size_t index = 0; index < 64; ++index) {
    auto made = frontend::typed_assets::test_seam::make_source(
        streams, *captured, "client/main.js", guard(), tasks.try_acquire());
    REQUIRE(made.has_value());
    sources.push_back(std::move(*made));
  }
  REQUIRE(streams.active_count() == 64);
  auto excess = frontend::typed_assets::test_seam::make_source(
      streams, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE_FALSE(excess.has_value());
  REQUIRE(excess.error() == frontend::AssetStreamError::BUSY);
  REQUIRE(tasks.active_for_test() == 64);
  sources.back() = {};
  REQUIRE(streams.active_count() == 63);
  auto replacement = frontend::typed_assets::test_seam::make_source(
      streams, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE(replacement.has_value());
  REQUIRE(streams.active_count() == 64);
  REQUIRE(streams.request_stop());
  std::array<char, 1> byte{};
  REQUIRE(sources.front()(byte.data(), byte.size()) == 0);
  REQUIRE(streams.active_count() == 64);
  REQUIRE_FALSE(streams.drain(std::chrono::steady_clock::now()));
  auto closed = frontend::typed_assets::test_seam::make_source(
      streams, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE_FALSE(closed.has_value());
  REQUIRE(closed.error() == frontend::AssetStreamError::CLOSED);
  sources.clear();
  *replacement = {};
  REQUIRE(streams.active_count() == 0);
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE(streams.drain(std::chrono::steady_clock::now()));
}

TEST_CASE("typed assets: state outlives registry and invalid sources refuse",
          "[frontend][typed_assets]") {
  StreamFixture tree;
  generation::Budget budget;
  auto captured = tree.capture(budget);
  REQUIRE(captured.has_value());
  lifecycle::AsyncTaskRegistry tasks;
  auto streams = std::make_unique<frontend::TypedAssetStreams>(budget);
  const auto before_source = budget.used();
  auto made = frontend::typed_assets::test_seam::make_source(
      *streams, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE(made.has_value());
  REQUIRE(budget.used() > before_source);
  streams.reset(); // No raw registry pointer is retained by callback teardown.
  REQUIRE(tasks.active_for_test() == 1);
  REQUIRE((*made)(nullptr, 0) == 0);
  std::array<char, 1> byte{};
  REQUIRE((*made)(byte.data(), byte.size()) == 0);
  *made = {};
  REQUIRE(tasks.active_for_test() == 0);
  REQUIRE(budget.used() == (*captured)->charged_bytes());

  frontend::TypedAssetStreams valid{budget};
  for (const auto path : {"missing", "client", "server.js"}) {
    const auto invalid = frontend::typed_assets::test_seam::make_source(
        valid, *captured, path, guard(), tasks.try_acquire());
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error() == frontend::AssetStreamError::INVALID);
  }
  const auto absent_lease = frontend::typed_assets::test_seam::make_source(
      valid, *captured, "client/main.js", guard(), {});
  REQUIRE_FALSE(absent_lease.has_value());
  REQUIRE(absent_lease.error() == frontend::AssetStreamError::INVALID);
  const auto absent_guard = frontend::typed_assets::test_seam::make_source(
      valid, *captured, "client/main.js", {}, tasks.try_acquire());
  REQUIRE_FALSE(absent_guard.has_value());
  REQUIRE(absent_guard.error() == frontend::AssetStreamError::INVALID);
  generation::Budget foreign;
  frontend::TypedAssetStreams wrong_budget{foreign};
  const auto wrong = frontend::typed_assets::test_seam::make_source(
      wrong_budget, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE_FALSE(wrong.has_value());
  REQUIRE(wrong.error() == frontend::AssetStreamError::INVALID);
  const auto fenced = drain::begin_drain("typed-stream-fixture");
  const auto denied = frontend::typed_assets::test_seam::make_source(
      valid, *captured, "client/main.js", guard(), tasks.try_acquire());
  REQUIRE_FALSE(denied.has_value());
  REQUIRE(denied.error() == frontend::AssetStreamError::INVALID);
  REQUIRE(fenced->in_flight.load() == 0);
  REQUIRE(valid.active_count() == 0);
  REQUIRE(tasks.active_for_test() == 0);
  drain::end_drain("typed-stream-fixture");
}

TEST_CASE("typed assets: bodyless native header serialization",
          "[frontend][typed_assets]") {
  generation::Budget budget;
  frontend::TypedAssetStreams streams{budget};
  const std::string etag = '"' + std::string(64, 'a') + '"';
  for (const bool unchanged : {false, true}) {
    auto response = frontend::typed_assets::test_seam::bodyless_response(
        "client/main.js", etag, 1234, unchanged);
    REQUIRE(response->statusCode() ==
            (unchanged ? drogon::k304NotModified : drogon::k200OK));
    REQUIRE(response->body().empty());
    REQUIRE_FALSE(static_cast<bool>(response->streamCallback()));
    REQUIRE(response->getHeader("Content-Type") ==
            "text/javascript; charset=utf-8");
    REQUIRE(response->getHeader("Cache-Control") == "private, no-store");
    REQUIRE(response->getHeader("Vary") == "Cookie, Authorization");
    REQUIRE(response->getHeader("ETag") == etag);
    REQUIRE_FALSE(response->getHeader("Date").empty());
    REQUIRE(response->getHeader("X-Content-Type-Options") == "nosniff");
    // Application security advice can still add/enforce headers normally.
    response->addHeader("X-Content-Type-Options", "wrong");
    plinth::security::apply_response_headers(response);
    REQUIRE(response->getHeader("X-Content-Type-Options") == "nosniff");
    const auto implementation =
        std::dynamic_pointer_cast<drogon::HttpResponseImpl>(response);
    REQUIRE(implementation != nullptr);
    const auto buffer = unchanged ? implementation->renderToBuffer()
                                  : implementation->renderHeaderForHeadMethod();
    const std::string_view wire{buffer->peek(), buffer->readableBytes()};
    const auto terminator = wire.find("\r\n\r\n");
    REQUIRE(terminator != std::string_view::npos);
    REQUIRE(terminator + 4 == wire.size()); // Actual serialized body is empty.
    REQUIRE(wire.find("content-type: text/javascript; charset=utf-8\r\n") !=
            std::string_view::npos);
    REQUIRE(wire.find("content-type:", wire.find("content-type:") + 1) ==
            std::string_view::npos);
    REQUIRE(wire.find("x-content-type-options: nosniff\r\n") !=
            std::string_view::npos);
    REQUIRE(wire.find("transfer-encoding:") == std::string_view::npos);
    REQUIRE(wire.find("date:") != std::string_view::npos);
    REQUIRE(wire.find("date:", wire.find("date:") + 1) ==
            std::string_view::npos);
    if (unchanged) {
      REQUIRE(wire.find("content-length:") == std::string_view::npos);
    } else {
      REQUIRE(wire.find("content-length: 1234\r\n") != std::string_view::npos);
      REQUIRE(wire.find("content-length:", wire.find("content-length:") + 1) ==
              std::string_view::npos);
    }
    REQUIRE(streams.active_count() == 0);
  }
}
