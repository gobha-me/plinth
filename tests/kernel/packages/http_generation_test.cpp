#include "kernel/packages/http_generation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
namespace generation = plinth::packages::generation;
namespace http = plinth::packages::http_generation;
namespace fs = std::filesystem;

class HttpTree {
 public:
  HttpTree() {
    std::array<char, 64> name{};
    std::ranges::copy(std::string_view{"/tmp/plinth-http-capture.XXXXXX"},
                      name.begin());
    const auto* made = ::mkdtemp(name.data());
    if (made == nullptr) throw std::runtime_error("HTTP capture fixture");
    root = made;
  }
  ~HttpTree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  HttpTree(const HttpTree&) = delete;
  auto operator=(const HttpTree&) -> HttpTree& = delete;
  auto write(std::string_view path, std::string_view bytes) const -> void {
    fs::create_directories((root / path).parent_path());
    std::ofstream file(root / path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("HTTP capture fixture write");
  }
  auto capture(generation::Budget& budget) const -> generation::CaptureResult {
    return generation::capture_artifact(
        {.key = {.package_id = "11111111-1111-1111-1111-111111111111",
                 .name = "http-fixture",
                 .version = "1.0.0",
                 .profile = 1},
         .root = root,
         .deadline =
             std::chrono::steady_clock::now() + std::chrono::seconds{10},
         .cancellation = {}},
        budget);
  }
  fs::path root;
};

auto load(const http::HttpModuleIndex& index,
          const http::HttpModuleSource& importer, std::string_view specifier)
    -> http::HttpModuleSource {
  auto result = index.resolve_import(importer, specifier);
  REQUIRE(result);
  return *result;
}
} // namespace

TEST_CASE("HTTP module index retains full immutable captured source without "
          "evaluation",
          "[packages][http-generation]") {
  generation::Budget budget;
  std::optional<http::HttpModuleSource> retained;
  std::size_t captured_charge = 0;
  {
    HttpTree tree;
    // Invalid JS is deliberately just data at this layer. Embedded NUL and
    // non-UTF8 payload retain their lengths; engine acceptance is a later gate.
    const std::string source{"A\0B\xff", 4};
    tree.write("server/http.js", source);
    tree.write("server/lib/child.js", "throw new Error('must not run');");
    tree.write("client/index.html", "full capture remains retained");
    auto artifact = tree.capture(budget);
    REQUIRE(artifact);
    captured_charge = (*artifact)->charged_bytes();
    auto index = http::HttpModuleIndex::create(*artifact, budget);
    REQUIRE(index);
    REQUIRE(budget.used() == captured_charge + 4096);
    REQUIRE(index->artifact() == *artifact);
    REQUIRE(index->artifact()->find("client/index.html") != nullptr);
    auto entry = index->entry();
    REQUIRE(entry.path() == "server/http.js");
    REQUIRE(entry.bytes() == std::string_view{source});
    const auto imported = index->resolve_import(entry, "./lib/child.js");
    REQUIRE(imported);
    REQUIRE(imported->path() == "server/lib/child.js");
    REQUIRE(imported->bytes() == "throw new Error('must not run');");
    tree.write("server/http.js", "changed");
    fs::remove_all(tree.root / "server");
    REQUIRE(entry.bytes() == std::string_view{source});
    REQUIRE(load(*index, entry, "./lib/child.js").bytes() == imported->bytes());
    retained = entry;
  }
  REQUIRE(budget.used() == captured_charge + 4096);
  REQUIRE(retained->bytes() == std::string_view{"A\0B\xff", 4});
  retained.reset();
  REQUIRE(budget.used() == 0);
}

TEST_CASE("HTTP module index resolves explicit relative imports only inside "
          "captured server",
          "[packages][http-generation]") {
  HttpTree tree;
  generation::Budget budget;
  tree.write("server/http.js", "entry");
  tree.write("server/lib/child.js", "child");
  tree.write("server/shared.js", "shared");
  tree.write("server/unicod\xc3\xa9.js", "unicode");
  tree.write("server/space name.js", "space");
  tree.write("client/secret.js", "not available");
  tree.write("secret.js", "not available");
  const auto artifact = tree.capture(budget);
  REQUIRE(artifact);
  const auto index = http::HttpModuleIndex::create(*artifact, budget);
  REQUIRE(index);
  const auto entry = index->entry();
  const auto child = index->resolve_import(entry, "./lib/child.js");
  REQUIRE(child);
  REQUIRE(load(*index, *child, "../shared.js").bytes() == "shared");
  REQUIRE(load(*index, entry, "./lib/../shared.js").bytes() == "shared");
  REQUIRE(load(*index, entry, "./unicod\xc3\xa9.js").bytes() == "unicode");
  REQUIRE(load(*index, entry, "./space name.js").bytes() == "space");
  const std::array<std::string_view, 18> rejected{
      "",
      "shared.js",
      "/server/shared.js",
      "file:///server/shared.js",
      "https://example.test/x.js",
      "node:fs",
      "../secret.js",
      "../client/secret.js",
      "./lib/../../secret.js",
      "./lib//child.js",
      "./lib/",
      "./",
      "./lib\\child.js",
      "./lib/\nchild.js",
      std::string_view{"./lib/\0child.js", 15},
      "./lib/\xff.js",
      ".",
      ".."};
  for (const auto value : rejected) {
    INFO(value.size());
    const auto result = index->resolve_import(entry, value);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == http::ModuleError::IMPORT_REJECTED);
  }
  for (const auto value : {"./shared", "./missing.js", "./lib"}) {
    const auto result = index->resolve_import(entry, value);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == http::ModuleError::MODULE_MISSING);
  }
}

TEST_CASE(
    "HTTP module index keeps literal percent query and fragment filename bytes",
    "[packages][http-generation]") {
  HttpTree tree;
  generation::Budget budget;
  tree.write("server/http.js", "entry");
  tree.write("server/%2e%2e.js", "literal percent");
  tree.write("server/mod.js?x#y", "literal suffix");
  tree.write("server/mod.js", "plain");
  const auto artifact = tree.capture(budget);
  REQUIRE(artifact);
  const auto index = http::HttpModuleIndex::create(*artifact, budget);
  REQUIRE(index);
  REQUIRE(load(*index, index->entry(), "./%2e%2e.js").bytes() ==
          "literal percent");
  REQUIRE(load(*index, index->entry(), "./mod.js?x#y").bytes() ==
          "literal suffix");
  REQUIRE(load(*index, index->entry(), "./mod.js").bytes() == "plain");
  REQUIRE_FALSE(index->resolve_import(index->entry(), "./mod.js?missing"));
  REQUIRE_FALSE(index->resolve_import(index->entry(), "./%2e%2e/secret.js"));
}

TEST_CASE("HTTP module index requires an actual entry same budget and same "
          "source owner",
          "[packages][http-generation]") {
  HttpTree tree;
  generation::Budget budget;
  generation::Budget foreign_budget;
  REQUIRE_FALSE(http::HttpModuleIndex::create(nullptr, budget));
  auto empty = tree.capture(budget);
  REQUIRE(empty);
  auto missing = http::HttpModuleIndex::create(*empty, budget);
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error() == http::ModuleError::ENTRY_REQUIRED);
  fs::create_directories(tree.root / "server/http.js");
  auto directory = tree.capture(budget);
  REQUIRE(directory);
  REQUIRE_FALSE(http::HttpModuleIndex::create(*directory, budget));
  fs::remove_all(tree.root / "server");
  tree.write("server/http.js", "entry");
  auto artifact = tree.capture(budget);
  REQUIRE(artifact);
  auto foreign = http::HttpModuleIndex::create(*artifact, foreign_budget);
  REQUIRE_FALSE(foreign);
  REQUIRE(foreign.error() == http::ModuleError::INVALID_CAPTURE);
  auto first = http::HttpModuleIndex::create(*artifact, budget);
  auto second = http::HttpModuleIndex::create(*artifact, budget);
  REQUIRE(first);
  REQUIRE(second);
  const auto crossed = second->resolve_import(first->entry(), "./http.js");
  REQUIRE_FALSE(crossed);
  REQUIRE(crossed.error() == http::ModuleError::IMPORT_REJECTED);
  auto held = budget.try_reserve(budget.capacity() - budget.used() - 4095);
  REQUIRE(held);
  const auto before = budget.used();
  const auto exhausted = http::HttpModuleIndex::create(*artifact, budget);
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error() == http::ModuleError::BUDGET_UNAVAILABLE);
  REQUIRE(budget.used() == before);
}

TEST_CASE("HTTP module import resolution bounds component and byte work",
          "[packages][http-generation]") {
  HttpTree tree;
  generation::Budget budget;
  tree.write("server/http.js", "entry");
  std::string deepest = "server";
  for (int depth = 1; depth < 31; ++depth)
    deepest += "/d";
  tree.write(deepest + "/last.js", "deep");
  const auto artifact = tree.capture(budget);
  REQUIRE(artifact);
  const auto index = http::HttpModuleIndex::create(*artifact, budget);
  REQUIRE(index);
  const auto specifier = "." + deepest.substr(6) + "/last.js";
  const auto deep = index->resolve_import(index->entry(), specifier);
  REQUIRE(deep);
  REQUIRE(deep->bytes() == "deep");
  const auto overflow = index->resolve_import(*deep, "./d/last.js");
  REQUIRE_FALSE(overflow);
  REQUIRE(overflow.error() == http::ModuleError::IMPORT_REJECTED);
  const auto long_name =
      index->resolve_import(index->entry(), "./" + std::string(1024, 'x'));
  REQUIRE_FALSE(long_name);
  REQUIRE(long_name.error() == http::ModuleError::IMPORT_REJECTED);
  const auto missing = index->resolve_import(*deep, "../last.js");
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error() == http::ModuleError::MODULE_MISSING);
}
