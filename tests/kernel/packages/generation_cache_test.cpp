#include "kernel/packages/generation_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace generation = plinth::packages::generation;
class CacheTree {
 public:
  CacheTree() {
    std::array<char, 64> name{};
    const std::string pattern = "/tmp/plinth-generation-cache.XXXXXX";
    std::ranges::copy(pattern, name.begin());
    const auto* made = ::mkdtemp(name.data());
    if (made == nullptr) throw std::runtime_error("cache fixture creation");
    root = made;
    write("a");
  }
  ~CacheTree() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  CacheTree(const CacheTree&) = delete;
  auto operator=(const CacheTree&) -> CacheTree& = delete;
  auto write(std::string_view value) const -> void {
    std::ofstream file(root / "value", std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!file) throw std::runtime_error("cache fixture write");
  }
  auto capture(generation::Budget& budget, std::string identity) const
      -> std::shared_ptr<const generation::CapturedArtifact> {
    generation::CaptureRequest request{
        .key = {.package_id = std::move(identity),
                .name = "fixture",
                .version = "1.0.0"},
        .root = root,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10},
        .cancellation = {}};
    auto result = generation::capture_artifact(request, budget);
    REQUIRE(result.has_value());
    return std::move(*result);
  }
  std::filesystem::path root;
};
} // namespace

TEST_CASE("generation cache: charged shared ownership not authority",
          "[packages][generation_cache]") {
  generation::Budget budget;
  SECTION("tickets transfer shrink refuse growth and release exactly once") {
    generation::Budget tiny{10};
    auto first = tiny.try_reserve(7);
    REQUIRE(first.has_value());
    REQUIRE(tiny.used() == 7);
    REQUIRE_FALSE(tiny.try_reserve(4));
    auto moved = std::move(*first);
    REQUIRE(first->bytes() == 0);
    REQUIRE(moved.bytes() == 7);
    moved.shrink(20);
    REQUIRE(tiny.used() == 7);
    moved.shrink(3);
    REQUIRE(tiny.used() == 3);
    moved = {};
    REQUIRE(tiny.used() == 0);
  }
  SECTION(
      "LRU metadata eviction and retirement retain external immutable leases") {
    CacheTree tree;
    generation::GenerationCache cache{budget, 2};
    const auto baseline = budget.used();
    auto first = tree.capture(budget, "first");
    auto second = tree.capture(budget, "second");
    REQUIRE(cache.publish(first));
    REQUIRE(cache.publish(second));
    REQUIRE(cache.find(first->key(), first->digest()) == first);
    auto third = tree.capture(budget, "third");
    REQUIRE(cache.publish(third));
    REQUIRE(cache.size() == 2);
    REQUIRE_FALSE(cache.find(second->key(), second->digest()));
    REQUIRE(second->find("value")->content == "a");
    const auto used = budget.used();
    cache.retire("first");
    REQUIRE_FALSE(cache.find(first->key(), first->digest()));
    REQUIRE(budget.used() == used);
    cache.clear();
    first.reset();
    second.reset();
    third.reset();
    REQUIRE(budget.used() == baseline);
  }
  SECTION("a cached generation digest cannot be silently replaced") {
    CacheTree tree;
    generation::GenerationCache cache{budget};
    const auto first = tree.capture(budget, "same");
    REQUIRE(cache.publish(first));
    tree.write("changed");
    const auto changed = tree.capture(budget, "same");
    REQUIRE(changed->digest() != first->digest());
    REQUIRE_FALSE(cache.publish(changed));
    REQUIRE(cache.find(first->key(), first->digest()) == first);
    REQUIRE_FALSE(cache.find(changed->key(), changed->digest()));
  }
  SECTION("foreign budget owners cannot bypass aggregate accounting") {
    CacheTree tree;
    generation::Budget other;
    generation::GenerationCache cache{budget};
    const auto foreign = tree.capture(other, "foreign");
    REQUIRE_FALSE(cache.publish(foreign));
    REQUIRE(cache.size() == 0);
    REQUIRE_FALSE(cache.find(foreign->key(), foreign->digest()));
  }
  SECTION("reserve reclaims unleased bytes before the next full attempt") {
    CacheTree tree;
    generation::Budget constrained{
        generation::CAPTURE_ATTEMPT_BYTES +
        sizeof(std::shared_ptr<const generation::CapturedArtifact>)};
    generation::GenerationCache cache{constrained, 1};
    auto first = tree.capture(constrained, "first");
    const std::weak_ptr<const generation::CapturedArtifact> observed = first;
    REQUIRE(cache.publish(first));
    REQUIRE_FALSE(constrained.try_reserve(generation::CAPTURE_ATTEMPT_BYTES));
    REQUIRE_FALSE(cache.reserve_capture());
    REQUIRE(cache.size() == 1);
    first.reset();
    auto reservation = cache.reserve_capture();
    REQUIRE(reservation.has_value());
    REQUIRE(cache.size() == 0);
    REQUIRE(observed.expired());
    generation::CaptureRequest request{
        .key = {.package_id = "next", .name = "fixture", .version = "1.0.0"},
        .root = tree.root,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10},
        .cancellation = {}};
    const auto next =
        generation::capture_artifact(request, std::move(*reservation));
    REQUIRE(next.has_value());
    REQUIRE((*next)->find("value")->content == "a");
  }
  SECTION(
      "more than 32 leased generations do not become a cache eligibility cap") {
    CacheTree tree;
    generation::GenerationCache cache{budget, 2};
    std::vector<std::shared_ptr<const generation::CapturedArtifact>> leased;
    for (std::size_t index = 0; index != 40; ++index) {
      auto artifact =
          tree.capture(budget, "generation-" + std::to_string(index));
      REQUIRE(cache.publish(artifact));
      leased.push_back(std::move(artifact));
    }
    REQUIRE(cache.size() == 2);
    for (const auto& artifact : leased)
      REQUIRE(artifact->find("value")->content == "a");
    cache.clear();
    REQUIRE(leased.size() == 40);
    leased.clear();
    REQUIRE(budget.used() ==
            2 * sizeof(std::shared_ptr<const generation::CapturedArtifact>));
  }
}
