// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <trantor/net/EventLoop.h>
#include <utility>

namespace plinth::lifecycle {
class AsyncTaskLease;
}
namespace plinth::js {
struct BridgeContext;

// One checkout, not one evaluation or subscription. Queued broker work owns
// only this revocable gate; it never owns or prolongs a QuickJS context.
class PubsubDeliveryOwner {
 private:
  struct Visiting {
    explicit Visiting(PubsubDeliveryOwner& owner_in) : owner(owner_in) {
      ++owner.visits;
    }
    ~Visiting() { --owner.visits; }
    PubsubDeliveryOwner& owner;
  };

 public:
  struct Target {
    trantor::EventLoop* loop = nullptr;
    std::uint64_t revision = 0;
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  };

  PubsubDeliveryOwner() = default;
  // Only the pool calls this, before publishing a fresh checkout.
  auto initialize(BridgeContext& context) -> void { current = &context; }

  // Called on the new driver's actual loop BEFORE any mutable JS state. The
  // gate refuses busy/reentrant binding rather than waiting for an invocation;
  // queued old-loop deliveries fail their revision check, not follow a pointer.
  auto bind(trantor::EventLoop& loop,
            const std::shared_ptr<lifecycle::AsyncTaskLease>& accepted)
      -> bool {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || retiring.load(std::memory_order_acquire) ||
        current == nullptr || visits != 0 || !loop.isInLoopThread())
      return false;
    std::lock_guard route_lock(route_mutex);
    if (retiring.load(std::memory_order_acquire)) return false;
    if (target.loop != &loop) {
      target.loop = &loop;
      ++target.revision;
    }
    lineage = accepted;
    return true;
  }

  auto destination() const -> Target {
    std::lock_guard lock(route_mutex);
    if (retiring.load(std::memory_order_acquire)) return {};
    auto result = target;
    result.accepted = lineage.lock();
    return result;
  }

  // Short metadata-only snapshot: never waits for a running JS handler. An
  // active visit publishes the finite lease actually admitted by the broker,
  // including when it did not inherit an existing execution's lineage.
  auto retirement_destination() const -> Target {
    std::lock_guard lock(route_mutex);
    auto result = target;
    result.accepted = active_lineage.lock();
    if (!result.accepted) result.accepted = lineage.lock();
    return result;
  }

  template <typename Function>
  auto visit(Target expected,
             const std::shared_ptr<lifecycle::AsyncTaskLease>& accepted,
             Function&& function) -> bool {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || retiring.load(std::memory_order_acquire) ||
        current == nullptr || visits != 0 || !accepted)
      return false;
    {
      std::lock_guard route_lock(route_mutex);
      if (retiring.load(std::memory_order_acquire) ||
          target.loop != expected.loop ||
          target.revision != expected.revision || target.loop == nullptr ||
          !target.loop->isInLoopThread())
        return false;
      active_lineage = accepted;
    }
    Visiting visiting(*this);
    std::forward<Function>(function)(*current);
    return true;
  }

  // The synchronous host helper may run an unbound checkout on its calling
  // thread, but cannot enter a subscribed checkout from a different loop or
  // re-enter a currently invoked handler. Keep the gate for the whole eval.
  template <typename Function>
  auto synchronous_visit(Function&& function) -> bool {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || retiring.load(std::memory_order_acquire) ||
        current == nullptr || visits != 0)
      return false;
    {
      std::lock_guard route_lock(route_mutex);
      if (target.loop != nullptr && !target.loop->isInLoopThread())
        return false;
    }
    Visiting visiting(*this);
    std::forward<Function>(function)(*current);
    return true;
  }

  // Pool retirement calls this OUTSIDE the pool mutex, before reading/resetting
  // or freeing JS state. Sticky retirement immediately closes delivery/bind
  // admission; a busy returned context is owned by a preallocated pool record,
  // never left checked out for a caller that already returned its void API.
  auto try_revoke(Target* retiring_target = nullptr) -> bool {
    retiring.store(true, std::memory_order_release);
    // Snapshot lineage BEFORE testing the busy gate. A finishing callback may
    // otherwise release its last task between refusal and the pool's snapshot.
    // Keeping only the last weak lineage never pins an idle subscription.
    const auto route = retirement_destination();
    if (retiring_target != nullptr) *retiring_target = route;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || visits != 0) return false;
    current = nullptr;
    return true;
  }

  // Compatibility for final destruction's invariant check. Pool return paths
  // must use the pending-retirement protocol when this cannot revoke now.
  auto revoke() -> bool { return try_revoke(); }

 private:
  mutable std::recursive_mutex mutex;
  mutable std::mutex route_mutex;
  std::atomic<bool> retiring{false};
  BridgeContext* current = nullptr;
  Target target;
  // No subscription permanently pins process admission. Active executions or
  // already queued accepted descendants can share their finite drain lineage.
  std::weak_ptr<lifecycle::AsyncTaskLease> lineage;
  std::weak_ptr<lifecycle::AsyncTaskLease> active_lineage;
  unsigned visits = 0;
};
} // namespace plinth::js
