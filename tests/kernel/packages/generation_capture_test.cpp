#include "kernel/packages/generation_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
namespace generation = plinth::packages::generation;
namespace fs = std::filesystem;

class CaptureTree {
 public:
  CaptureTree() {
    std::array<char, 64> name{};
    const std::string pattern = "/tmp/plinth-generation-capture.XXXXXX";
    std::ranges::copy(pattern, name.begin());
    const auto* made = ::mkdtemp(name.data());
    if (made == nullptr) throw std::runtime_error("capture fixture creation");
    root = made;
  }
  ~CaptureTree() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  CaptureTree(const CaptureTree&) = delete;
  auto operator=(const CaptureTree&) -> CaptureTree& = delete;
  auto write(std::string_view name, std::string_view bytes) const -> void {
    std::ofstream file(root / name, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("capture fixture write");
  }
  auto request() const -> generation::CaptureRequest {
    return {.key = {.package_id = "11111111-1111-1111-1111-111111111111",
                    .name = "fixture",
                    .version = "1.0.0",
                    .profile = 1},
            .root = root,
            .deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{10},
            .cancellation = {}};
  }
  fs::path root;
};

struct CaptureDrainers {
  std::promise<void> start;
  std::shared_future<void> released = start.get_future().share();
  std::array<std::jthread, 2> threads;
  bool open = false;
  auto release() -> void {
    if (open) return;
    start.set_value();
    open = true;
  }
  // Open the start barrier BEFORE member jthreads join on assertion unwind
  // or failure to construct the second thread.
  ~CaptureDrainers() { release(); }
};
} // namespace

TEST_CASE("generation capture: bounded immutable physical inventory",
          "[packages][generation_capture]") {
  CaptureTree tree;
  generation::Budget budget;
  SECTION("empty physical root has the independent empty artifact digest") {
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE(result.has_value());
    REQUIRE((*result)->records().empty());
    REQUIRE((*result)->digest() ==
            "a161ab513d8f7b37477fbace66e49aeb3e6d8b825ecd08093ea5f619364ef506");
  }
  SECTION("independent binary vector and detached immutable bytes") {
    tree.write("hello.txt", std::string_view{"A\0B\n", 4});
    fs::permissions(tree.root / "hello.txt",
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::group_read | fs::perms::others_read);
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE(result.has_value());
    REQUIRE((*result)->digest() ==
            "2980df2ad79e3c7a334cbea99bc241c784f57bafdee55a8c9fb2aecb4a38411b");
    REQUIRE((*result)->records().size() == 1);
    REQUIRE((*result)->key() == tree.request().key);
    const auto* file = (*result)->find("hello.txt");
    REQUIRE(file != nullptr);
    REQUIRE(file->content == std::string_view{"A\0B\n", 4});
    REQUIRE(file->mode == 0644);
    tree.write("hello.txt", "changed");
    fs::remove(tree.root / "hello.txt");
    REQUIRE(file->content == std::string_view{"A\0B\n", 4});
    REQUIRE(budget.used() == (*result)->charged_bytes());
    REQUIRE(budget.used() < std::size_t{64} * 1024 * 1024);
  }
  SECTION("directories hidden files and zero payload are included") {
    fs::create_directory(tree.root / ".hidden");
    fs::create_directory(tree.root / "empty");
    tree.write(".hidden/value", "value");
    tree.write("zero", "");
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE(result.has_value());
    REQUIRE((*result)->records().size() == 4);
    REQUIRE((*result)->find(".hidden") != nullptr);
    REQUIRE((*result)->find("empty")->kind ==
            plinth::packages::restore::ArtifactKind::DIRECTORY);
    REQUIRE((*result)->find("zero")->content.empty());
    REQUIRE((*result)->find(".hidden/value")->content == "value");
    REQUIRE((*result)->find("missing") == nullptr);
    const auto again = generation::capture_artifact(tree.request(), budget);
    REQUIRE(again.has_value());
    REQUIRE((*again)->digest() == (*result)->digest());
  }
  SECTION("symbolic links are refused before following") {
    tree.write("real", "not followed");
    fs::create_symlink("real", tree.root / "link");
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::INVALID);
    REQUIRE(budget.used() == 0);
  }
  SECTION("root ancestor links are refused too") {
    fs::create_directory(tree.root / "real");
    fs::create_directory(tree.root / "real/child");
    fs::create_directory_symlink("real", tree.root / "alias");
    auto request = tree.request();
    request.root = tree.root / "alias/child";
    const auto result = generation::capture_artifact(request, budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::INVALID);
  }
  SECTION("hard links and special files are never payload") {
    tree.write("real", "linked");
    fs::create_hard_link(tree.root / "real", tree.root / "second");
    REQUIRE_FALSE(generation::capture_artifact(tree.request(), budget));
    fs::remove(tree.root / "real");
    fs::remove(tree.root / "second");
    REQUIRE(::mkfifo((tree.root / "pipe").c_str(), 0600) == 0);
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::INVALID);
    REQUIRE(budget.used() == 0);
  }
  SECTION("setid modes and invalid UTF8 paths are refused") {
    tree.write("setid", "x");
    REQUIRE(::chmod((tree.root / "setid").c_str(), 04700) == 0);
    REQUIRE_FALSE(generation::capture_artifact(tree.request(), budget));
    fs::remove(tree.root / "setid");
    tree.write(std::string{"bad\xff", 4}, "x");
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::INVALID);
  }
  SECTION("payload size is checked before sparse-file allocation or read") {
    tree.write("large", "");
    fs::resize_file(tree.root / "large",
                    plinth::packages::restore::INVENTORY_CONTENT_MAX_BYTES + 1);
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::TOO_LARGE);
    REQUIRE(budget.used() == 0);
  }
  SECTION("record and component ceilings bound traversal itself") {
    auto directory = tree.root;
    for (std::size_t index = 0; index != 32; ++index) {
      directory /= "x";
      fs::create_directory(directory);
    }
    std::ofstream(directory / "too-deep").put('x');
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::TOO_LARGE);
    REQUIRE(budget.used() == 0);
  }
  SECTION("record ceiling rejects 4097 tiny files independently of payload") {
    for (std::size_t index = 0; index != 4097; ++index)
      tree.write("f" + std::to_string(index), "");
    const auto result = generation::capture_artifact(tree.request(), budget);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == generation::CaptureError::TOO_LARGE);
    REQUIRE(budget.used() == 0);
  }
  SECTION("cancel deadline and pressure refuse without retained allocation") {
    auto request = tree.request();
    std::stop_source cancellation;
    cancellation.request_stop();
    request.cancellation = cancellation.get_token();
    const auto cancelled = generation::capture_artifact(request, budget);
    REQUIRE_FALSE(cancelled.has_value());
    REQUIRE(cancelled.error() == generation::CaptureError::CANCELLED);
    request.cancellation = {};
    request.deadline = std::chrono::steady_clock::now();
    const auto expired = generation::capture_artifact(request, budget);
    REQUIRE_FALSE(expired.has_value());
    REQUIRE(expired.error() == generation::CaptureError::DEADLINE);
    generation::Budget small{1024};
    const auto busy = generation::capture_artifact(tree.request(), small);
    REQUIRE_FALSE(busy.has_value());
    REQUIRE(busy.error() == generation::CaptureError::BUSY);
    REQUIRE(small.used() == 0);
    REQUIRE(budget.used() == 0);
  }
  SECTION("owned worker completes joins and seals without callbacks") {
    tree.write("value", "owned");
    generation::CaptureWorker worker{budget};
    auto pending = worker.try_submit(tree.request());
    REQUIRE(pending.has_value());
    const auto result = pending->get();
    REQUIRE(result.has_value());
    REQUIRE((*result)->find("value")->content == "owned");
    REQUIRE(worker.drain(std::chrono::steady_clock::now() +
                         std::chrono::seconds{5}));
    REQUIRE(worker.drain(std::chrono::steady_clock::now() +
                         std::chrono::seconds{5}));
    const auto refused = worker.try_submit(tree.request());
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == generation::CaptureError::CANCELLED);
  }
  SECTION("concurrent drainers serialize actual join and remain idempotent") {
    generation::CaptureWorker worker{budget};
    std::array<bool, 2> drained{};
    std::array<std::exception_ptr, 2> errors{};
    std::array<std::promise<void>, 2> started;
    std::array ready{started[0].get_future(), started[1].get_future()};
    CaptureDrainers drainers;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    for (std::size_t index = 0; index < drainers.threads.size(); ++index) {
      drainers.threads[index] =
          std::jthread([&, index, deadline, released = drainers.released] {
            started[index].set_value();
            try {
              released.get();
              drained[index] = worker.drain(deadline);
            } catch (...) {
              errors[index] = std::current_exception();
            }
          });
    }
    for (auto& entered : ready)
      REQUIRE(entered.wait_for(std::chrono::seconds{2}) ==
              std::future_status::ready);
    drainers.release();
    for (auto& drainer : drainers.threads)
      drainer.join();
    for (std::size_t index = 0; index < drained.size(); ++index) {
      REQUIRE_FALSE(errors[index]);
      REQUIRE(drained[index]);
    }
    REQUIRE(worker.drain(std::chrono::steady_clock::now() +
                         std::chrono::seconds{2}));
    const auto refused = worker.try_submit(tree.request());
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == generation::CaptureError::CANCELLED);
  }
}
