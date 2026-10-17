#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages::generation {

inline constexpr std::size_t CAPTURE_ATTEMPT_BYTES =
    std::size_t{64} * 1024 * 1024;

struct GenerationKey {
  std::string package_id;
  std::string name;
  std::string version;
  std::uint32_t profile = 1;
  auto operator==(const GenerationKey&) const -> bool = default;
};

struct BudgetState;

// Charges explicit source allocations, not allocator/framework/JS overhead.
// Shared state outlives Budget when a captured artifact remains leased.
class BudgetTicket {
 public:
  BudgetTicket() = default;
  ~BudgetTicket();
  BudgetTicket(BudgetTicket&& other) noexcept;
  auto operator=(BudgetTicket&& other) noexcept -> BudgetTicket&;
  BudgetTicket(const BudgetTicket&) = delete;
  auto operator=(const BudgetTicket&) -> BudgetTicket& = delete;
  [[nodiscard]] auto bytes() const noexcept -> std::size_t { return charged; }
  auto shrink(std::size_t bytes) noexcept -> void;

 private:
  friend class Budget;
  BudgetTicket(std::shared_ptr<BudgetState> state, std::size_t bytes);
  std::shared_ptr<BudgetState> state;
  std::size_t charged = 0;
};

class Budget {
 public:
  explicit Budget(std::size_t capacity = std::size_t{256} * 1024 * 1024);
  [[nodiscard]] auto try_reserve(std::size_t bytes)
      -> std::optional<BudgetTicket>;
  [[nodiscard]] auto used() const -> std::size_t;
  [[nodiscard]] auto capacity() const -> std::size_t;
  [[nodiscard]] auto owns(const BudgetTicket& ticket) const noexcept -> bool;

 private:
  std::shared_ptr<BudgetState> state;
};

class CapturedArtifact;

// A bounded metadata LRU. Dropping an entry never destroys an external lease;
// charged bytes stay owned until the final artifact reference is released.
// Cache membership is not readiness, authority or a browser-owner budget.
class GenerationCache {
 public:
  explicit GenerationCache(Budget budget, std::size_t max_entries = 128);
  // Refuses a different digest for an already cached exact key. Durable
  // receipt/current-generation validation remains the caller's responsibility.
  [[nodiscard]] auto publish(std::shared_ptr<const CapturedArtifact> artifact)
      -> bool;
  [[nodiscard]] auto find(const GenerationKey& key, std::string_view digest)
      -> std::shared_ptr<const CapturedArtifact>;
  // Atomically reserve with respect to this cache's reclaim decisions. Only
  // cache-exclusive LRU owners are reclaimed; external leases stay charged.
  [[nodiscard]] auto reserve_capture() -> std::optional<BudgetTicket>;
  auto retire(std::string_view package_id) -> void;
  auto clear() -> void;
  [[nodiscard]] auto size() const -> std::size_t;

 private:
  mutable std::mutex mutex;
  Budget budget;
  BudgetTicket metadata_ticket;
  std::size_t maximum;
  std::vector<std::shared_ptr<const CapturedArtifact>> entries;
};

} // namespace plinth::packages::generation
