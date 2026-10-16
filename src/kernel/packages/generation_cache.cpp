#include "kernel/packages/generation_cache.hpp"

#include "kernel/packages/generation_capture.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace plinth::packages::generation {

struct BudgetState {
  explicit BudgetState(std::size_t capacity) : maximum(capacity) {}
  std::mutex mutex;
  const std::size_t maximum;
  std::size_t used = 0;
};

BudgetTicket::BudgetTicket(std::shared_ptr<BudgetState> owner,
                           std::size_t bytes)
    : state(std::move(owner)), charged(bytes) {
}
BudgetTicket::~BudgetTicket() {
  shrink(0);
}
BudgetTicket::BudgetTicket(BudgetTicket&& other) noexcept
    : state(std::move(other.state)), charged(std::exchange(other.charged, 0)) {
}
auto BudgetTicket::operator=(BudgetTicket&& other) noexcept -> BudgetTicket& {
  if (this != &other) {
    shrink(0);
    state = std::move(other.state);
    charged = std::exchange(other.charged, 0);
  }
  return *this;
}
auto BudgetTicket::shrink(std::size_t bytes) noexcept -> void {
  if (bytes >= charged) return;
  std::lock_guard lock(state->mutex);
  state->used -= charged - bytes;
  charged = bytes;
}
Budget::Budget(std::size_t capacity)
    : state(std::make_shared<BudgetState>(capacity)) {
}
auto Budget::try_reserve(std::size_t bytes) -> std::optional<BudgetTicket> {
  std::lock_guard lock(state->mutex);
  if (bytes > state->maximum - state->used) return std::nullopt;
  state->used += bytes;
  return BudgetTicket{state, bytes};
}
auto Budget::used() const -> std::size_t {
  std::lock_guard lock(state->mutex);
  return state->used;
}
auto Budget::capacity() const -> std::size_t {
  return state->maximum;
}
auto Budget::owns(const BudgetTicket& ticket) const noexcept -> bool {
  return ticket.state == state;
}

GenerationCache::GenerationCache(Budget source_budget, std::size_t max_entries)
    : budget(std::move(source_budget)), maximum(max_entries) {
  // Metadata capacity only: eviction never imposes this count on leased
  // artifacts or authorized applications.
  if (max_entries > 4096)
    throw std::invalid_argument("generation.cache.capacity");
  auto ticket = budget.try_reserve(
      max_entries * sizeof(std::shared_ptr<const CapturedArtifact>));
  if (!ticket) throw std::length_error("generation.cache.budget");
  metadata_ticket = std::move(*ticket);
  entries.reserve(max_entries);
}
auto GenerationCache::publish(std::shared_ptr<const CapturedArtifact> artifact)
    -> bool {
  if (!artifact || !artifact->belongs_to(budget)) return false;
  std::lock_guard lock(mutex);
  for (const auto& entry : entries) {
    if (entry->key() == artifact->key() &&
        entry->digest() != artifact->digest())
      return false;
  }
  std::erase_if(entries, [&](const auto& entry) {
    return entry->key() == artifact->key();
  });
  if (maximum == 0) return true;
  if (entries.size() == maximum) entries.erase(entries.begin());
  entries.push_back(std::move(artifact));
  return true;
}
auto GenerationCache::find(const GenerationKey& key, std::string_view digest)
    -> std::shared_ptr<const CapturedArtifact> {
  std::lock_guard lock(mutex);
  const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
    return entry->key() == key && entry->digest() == digest;
  });
  if (found == entries.end()) return {};
  auto result = *found;
  std::rotate(found, found + 1, entries.end());
  return result;
}
auto GenerationCache::reserve_capture() -> std::optional<BudgetTicket> {
  std::lock_guard lock(mutex);
  if (auto ticket = budget.try_reserve(CAPTURE_ATTEMPT_BYTES)) return ticket;
  auto entry = entries.begin();
  while (entry != entries.end()) {
    if (entry->use_count() == 1) {
      entry = entries.erase(entry);
      if (auto ticket = budget.try_reserve(CAPTURE_ATTEMPT_BYTES))
        return ticket;
    } else {
      ++entry;
    }
  }
  return std::nullopt;
}
auto GenerationCache::retire(std::string_view package_id) -> void {
  std::lock_guard lock(mutex);
  std::erase_if(entries, [&](const auto& entry) {
    return entry->key().package_id == package_id;
  });
}
auto GenerationCache::clear() -> void {
  std::lock_guard lock(mutex);
  entries.clear();
}
auto GenerationCache::size() const -> std::size_t {
  std::lock_guard lock(mutex);
  return entries.size();
}

} // namespace plinth::packages::generation
