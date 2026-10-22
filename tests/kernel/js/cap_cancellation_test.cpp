// SPDX-License-Identifier: MIT

#include "async_bridge_fixture.hpp"
#include "kernel/js/execution_lifetime.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/drogon.h>
#include <quickjs.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Generation = std::weak_ptr<plinth::js::ExecutionLifetime>;
using plinth::js::EvalErrorKind;
using plinth::js::EvalResult;

auto demand(bool value, const char* message) -> void {
  if (!value) throw std::runtime_error(message);
}

template <typename Predicate>
auto wait(Predicate&& predicate, const char* message) -> void {
  const auto deadline = Clock::now() + 2s;
  while (Clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::yield();
  }
  demand(false, message);
}

auto same(const Generation& a, const Generation& b) -> bool {
  return !a.owner_before(b) && !b.owner_before(a);
}

struct Snapshot {
  bool visited = false;
  bool cancelled = false;
};

auto snapshot(Generation weak) -> Snapshot {
  auto promise = std::make_shared<std::promise<Snapshot>>();
  auto ready = promise->get_future();
  drogon::app().getLoop()->queueInLoop([weak, promise] {
    Snapshot observed;
    {
      auto lifetime = weak.lock();
      if (lifetime) {
        observed.visited = lifetime->visit([&](plinth::js::BridgeContext& bc) {
          observed.cancelled = bc.cancelled.load(std::memory_order_acquire);
        });
      }
    } // A snapshot never publishes a retained accepted-task lease.
    promise->set_value(observed);
  });
  demand(ready.wait_for(2s) == std::future_status::ready,
         "cap token owner-loop snapshot timed out");
  return ready.get();
}

auto request(Generation weak) -> bool {
  const auto lifetime = weak.lock();
  return lifetime && lifetime->request_cancel();
}

struct Event {
  std::string name;
  Generation generation;
  std::stop_token token;
  bool token_published = false;
  unsigned enters = 0;
  unsigned finishes = 0;
  std::optional<EvalErrorKind> error = std::nullopt;
};

class Observe {
  struct State {
    std::mutex mutex;
    std::vector<Event> events;
    bool failed = false;
  };
  std::shared_ptr<State> state = std::make_shared<State>();
  bool closed = false;

 public:
  Observe() {
    state->events.reserve(8);
    plinth::js::set_execution_observer_for_test(
        [owned = state](
            const plinth::js::ExecutionObservation& observation) noexcept {
          try {
            std::stop_token token;
            {
              auto lifetime = observation.execution.lock();
              if (lifetime) token = lifetime->cancellation_token();
            }
            std::lock_guard lock(owned->mutex);
            Event* found = nullptr;
            for (auto& event : owned->events) {
              if (same(event.generation, observation.execution)) found = &event;
            }
            if (found == nullptr) {
              owned->events.push_back(
                  {observation.extension_name, observation.execution, token});
              found = &owned->events.back();
            }
            if (observation.stage ==
                plinth::js::ExecutionObservationStage::ENTER) {
              ++found->enters;
              found->token_published = token.stop_possible();
            } else {
              ++found->finishes;
              found->error = observation.error_kind;
            }
          } catch (...) {
            std::lock_guard lock(owned->mutex);
            owned->failed = true;
          }
        });
  }
  Observe(const Observe&) = delete;
  auto operator=(const Observe&) -> Observe& = delete;
  ~Observe() {
    if (!closed) {
      try {
        close();
      } catch (...) {
        std::terminate();
      }
    }
  }
  auto entered(std::string_view name) const -> Event {
    Event result;
    wait(
        [&] {
          std::lock_guard lock(state->mutex);
          demand(!state->failed, "cap token observation allocation failed");
          for (const auto& event : state->events) {
            if (event.name == name && event.enters == 1) {
              result = event;
              return true;
            }
          }
          return false;
        },
        "cap token execution ENTER not observed");
    return result;
  }
  auto close() -> void {
    if (closed) return;
    plinth::js::set_execution_observer_for_test({});
    static_cast<void>(snapshot({})); // Fence callbacks copied before removal.
    closed = true;
  }
  auto records() const -> std::vector<Event> {
    std::lock_guard lock(state->mutex);
    demand(!state->failed, "cap token observer failed");
    return state->events;
  }
};

// Source and a strong pool/context OWNER cross the worker boundary, never a
// borrowed BC pointer. The joined Work below outlives every driver reference.
class Owner {
  plinth::Config config;
  plinth::js::RuntimePool pool;
  plinth::js::BridgeContext* context = nullptr;
  bool closed = false;

 public:
  explicit Owner(std::string name)
      : config(plinth::async_bridge_test::test_config()),
        pool(nullptr, profile(), config, 1, nullptr, std::move(name)),
        context(pool.acquire()) {
    demand(context != nullptr, "cap token runtime acquisition failed");
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;
  ~Owner() {
    if (!closed) {
      try {
        if (!close()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  static auto profile() -> plinth::js::RuntimeLimits {
    auto result = plinth::js::default_runtime_limits();
    // Existing limits-test profile: cancellation, not CPU expiry, is the
    // oracle. Both CPU and wall guards remain finite, below the process bound.
    result.cpu_time_limit = 5s;
    result.wall_clock_limit = 10s;
    return result;
  }
  auto drive(const std::string& source, std::stop_token inherited)
      -> EvalResult {
    return drogon::sync_wait(
        plinth::js::run_on_context(*context, source, nullptr, inherited));
  }
  auto cancel() -> void {
    auto generation =
        context->execution_lifetime.load(std::memory_order_acquire);
    if (generation) static_cast<void>(generation->request_cancel());
  }
  auto clean() const -> bool {
    const auto& bc = *context;
    const auto& batch = bc.batch_state;
    return !bc.execution_lifetime.load(std::memory_order_acquire) &&
           bc.pending_ops.empty() && bc.retired_pending_ops.empty() &&
           bc.callbacks.empty() && bc.persistent_callbacks.empty() &&
           bc.concurrent_async_ops == 0 &&
           bc.inflight_detached.load(std::memory_order_acquire) == 0 &&
           batch.depth == 0 && batch.scope_id == 0 && !batch.pinned_conn &&
           !batch.rollback_owner && batch.timer_id == trantor::InvalidTimerId &&
           batch.timer_loop == nullptr;
  }
  auto release_reacquire() -> bool {
    demand(clean(), "cap token reuse before execution cleanup");
    const auto address = reinterpret_cast<std::uintptr_t>(context);
    pool.release(context);
    context = nullptr;
    demand(pool.active_count() == 0, "cap token clean release refused");
    context = pool.acquire();
    demand(context != nullptr, "cap token reused context unavailable");
    return reinterpret_cast<std::uintptr_t>(context) == address;
  }
  auto marker() const -> bool {
    auto global = JS_GetGlobalObject(context->ctx);
    auto value = JS_GetPropertyStr(context->ctx, global, "__capTokenBody");
    const bool result = !JS_IsUndefined(value);
    JS_FreeValue(context->ctx, value);
    JS_FreeValue(context->ctx, global);
    return result;
  }
  auto install_ack(JSCFunction* function) -> void {
    demand(!context->execution_lifetime.load(std::memory_order_acquire),
           "cap token ACK installation during execution");
    auto global = JS_GetGlobalObject(context->ctx);
    const int result = JS_SetPropertyStr(
        context->ctx, global, "__capTokenEntered",
        JS_NewCFunction(context->ctx, function, "__capTokenEntered", 0));
    JS_FreeValue(context->ctx, global);
    demand(result >= 0, "cap token ACK injection failed");
  }
  auto close() -> bool {
    if (closed) return true;
    if (context != nullptr) {
      if (!clean()) return false;
      pool.destroy(context);
      context = nullptr;
      if (pool.active_count() != 0) return false;
    }
    const bool stopped = pool.shutdown(5s);
    const bool idempotent = pool.shutdown(0ms);
    closed = stopped && idempotent && pool.active_count() == 0 &&
             pool.free_count() == 0;
    return closed;
  }
};

class Work {
  std::shared_ptr<Owner> owner;
  std::shared_ptr<std::promise<EvalResult>> promise =
      std::make_shared<std::promise<EvalResult>>();
  std::future<EvalResult> ready = promise->get_future();
  std::thread worker;

 public:
  Work(std::shared_ptr<Owner> input, std::string source,
       std::stop_token inherited = {})
      : owner(std::move(input)),
        worker([held = owner, receipt = promise,
                owned_source = std::move(source), inherited] {
          try {
            receipt->set_value(held->drive(owned_source, inherited));
          } catch (...) {
            receipt->set_exception(std::current_exception());
          }
        }) {}
  Work(const Work&) = delete;
  auto operator=(const Work&) -> Work& = delete;
  ~Work() {
    if (worker.joinable()) {
      try {
        owner->cancel();
        if (ready.wait_for(8s) != std::future_status::ready) std::terminate();
        worker.join();
      } catch (...) {
        std::terminate();
      }
    }
  }
  auto finish() -> EvalResult {
    demand(ready.wait_for(2s) == std::future_status::ready,
           "cap token actual driver did not finish");
    worker.join();
    return ready.get();
  }
};

// This is only a CPU-entry acknowledgement. It neither resolves a promise nor
// replaces any production binding/handler/cancellation implementation.
std::atomic<std::shared_ptr<std::atomic<bool>>> cpu_entry;
auto acknowledge_cpu(JSContext* /*ctx*/, JSValueConst /*self*/, int /*argc*/,
                     JSValueConst* /*argv*/) -> JSValue {
  if (const auto signal = cpu_entry.load(std::memory_order_acquire)) {
    signal->store(true, std::memory_order_release);
  }
  return JS_UNDEFINED;
}

class Ack {
 public:
  std::shared_ptr<std::atomic<bool>> signal =
      std::make_shared<std::atomic<bool>>(false);
  Ack() { cpu_entry.store(signal, std::memory_order_release); }
  ~Ack() { cpu_entry.store(nullptr, std::memory_order_release); }
  Ack(const Ack&) = delete;
  auto operator=(const Ack&) -> Ack& = delete;
};

class Stopper {
  std::shared_ptr<std::promise<bool>> promise =
      std::make_shared<std::promise<bool>>();
  std::future<bool> ready = promise->get_future();
  std::thread worker;

 public:
  Stopper(std::shared_ptr<std::atomic<bool>> signal, Generation parent)
      : worker([signal = std::move(signal), parent, receipt = promise] {
          try {
            wait([&] { return signal->load(std::memory_order_acquire); },
                 "cap token race ACK missing");
            receipt->set_value(request(parent));
          } catch (...) {
            receipt->set_exception(std::current_exception());
          }
        }) {}
  Stopper(const Stopper&) = delete;
  auto operator=(const Stopper&) -> Stopper& = delete;
  ~Stopper() {
    if (worker.joinable()) {
      if (ready.wait_for(4s) != std::future_status::ready) std::terminate();
      worker.join();
    }
  }
  auto finish() -> bool {
    demand(ready.wait_for(2s) == std::future_status::ready,
           "cap token cross-thread request did not join");
    worker.join();
    return ready.get();
  }
};

auto initialize() -> void {
  // Optional configured PG belongs to the existing shared fixture, not this
  // test: no PG availability prerequisite, SQL operation or credential role.
  plinth::async_bridge_test::ensure_drogon_with_db_running();
}

auto cancelled(const EvalResult& result) -> bool {
  return !result.value.has_value() &&
         result.value.error().kind == EvalErrorKind::CANCELLED;
}

auto answer(const EvalResult& result) -> bool {
  return result.value.has_value() && result.value->isNumeric() &&
         result.value->asDouble() == 42;
}

auto alive(const Event& event) -> bool {
  const auto state = snapshot(event.generation);
  return state.visited && !state.cancelled && event.token.stop_possible() &&
         !event.token.stop_requested();
}

auto events_match(const std::vector<Event>& events, unsigned count,
                  unsigned cancelled_count) -> bool {
  if (events.size() != count) return false;
  unsigned cancelled_events = 0;
  for (const auto& event : events) {
    // A successful retired generation has no remaining stop_source. Observe
    // publication at ENTER, not stop_possible() after legitimate destruction.
    if (event.enters != 1 || event.finishes != 1 || !event.token_published)
      return false;
    if (event.error) {
      if (event.error != EvalErrorKind::CANCELLED) return false;
      ++cancelled_events;
    }
  }
  return cancelled_events == cancelled_count;
}

auto drain() -> bool {
  return plinth::lifecycle::async_tasks().drain(5s) &&
         plinth::log::wait_for_audits(5s) &&
         plinth::lifecycle::async_tasks().active_for_test() == 0;
}

constexpr std::string_view PENDING = "new Promise(()=>{})";

} // namespace

TEST_CASE("Capability cancellation tokens are sticky before child registration",
          "[js][async][cap][cap-cancellation][cap-cancellation-sticky]") {
  initialize();
  Observe observe;
  auto parent = std::make_shared<Owner>("cancel_parent");
  Work parent_work(parent, std::string{PENDING});
  const auto inherited = observe.entered("cancel_parent");
  const bool parent_active = alive(inherited);
  const bool requested = request(inherited.generation);
  const auto parent_result = parent_work.finish();
  const bool sticky = inherited.token.stop_requested();
  const bool parent_closed = parent->close();
  auto child = std::make_shared<Owner>("cancel_sticky_child");
  Work child_work(child, "globalThis.__capTokenBody=true;42", inherited.token);
  const auto child_result = child_work.finish();
  const bool body = child->marker();
  const bool child_closed = child->close();
  observe.close();
  const auto events = observe.records();
  const bool drained = drain();
  REQUIRE(parent_active);
  REQUIRE(requested);
  REQUIRE(sticky);
  REQUIRE(cancelled(parent_result));
  REQUIRE(cancelled(child_result));
  REQUIRE_FALSE(body);
  REQUIRE(parent_closed);
  REQUIRE(child_closed);
  REQUIRE(events_match(events, 2, 2));
  REQUIRE(drained);
}

TEST_CASE(
    "Capability cancellation interrupts an entered CPU child as cancelled",
    "[js][async][cap][cap-cancellation][cap-cancellation-cpu]") {
  initialize();
  Observe observe;
  Ack ack;
  auto parent = std::make_shared<Owner>("cancel_cpu_parent");
  Work parent_work(parent, std::string{PENDING});
  const auto inherited = observe.entered("cancel_cpu_parent");
  const bool parent_active = alive(inherited);
  auto middle = std::make_shared<Owner>("cancel_cpu_middle");
  Work middle_work(middle, std::string{PENDING}, inherited.token);
  const auto middle_generation = observe.entered("cancel_cpu_middle");
  const bool middle_active = alive(middle_generation);
  auto child = std::make_shared<Owner>("cancel_cpu_child");
  child->install_ack(&acknowledge_cpu);
  // The CPU grandchild blocks the shared owner loop. Root cancellation must
  // publish the middle token synchronously, before deferred middle wake/BC
  // notification can run, or the grandchild cannot see its inherited stop.
  Work child_work(
      child,
      "(()=>{let "
      "first=true;for(;;){if(first){first=false;__capTokenEntered();}}})()",
      middle_generation.token);
  wait([&] { return ack.signal->load(std::memory_order_acquire); },
       "cap token CPU entry missing");
  const bool requested = request(inherited.generation);
  const auto child_result = child_work.finish();
  const auto middle_result = middle_work.finish();
  const auto parent_result = parent_work.finish();
  const bool child_closed = child->close();
  const bool middle_closed = middle->close();
  const bool parent_closed = parent->close();
  observe.close();
  const auto events = observe.records();
  const bool drained = drain();
  REQUIRE(parent_active);
  REQUIRE(middle_active);
  REQUIRE_FALSE(same(inherited.generation, middle_generation.generation));
  REQUIRE(ack.signal->load(std::memory_order_acquire));
  REQUIRE(requested);
  REQUIRE(cancelled(child_result)); // Not CPU_TIME_EXCEEDED or wall timeout.
  REQUIRE(cancelled(middle_result));
  REQUIRE(cancelled(parent_result));
  REQUIRE(child_closed);
  REQUIRE(middle_closed);
  REQUIRE(parent_closed);
  REQUIRE(events_match(events, 3, 3));
  REQUIRE(drained);
}

TEST_CASE("Capability cancellation tokens isolate sibling generations",
          "[js][async][cap][cap-cancellation][cap-cancellation-siblings]") {
  initialize();
  Observe observe;
  auto parent = std::make_shared<Owner>("cancel_sibling_parent");
  auto first = std::make_shared<Owner>("cancel_sibling_first");
  auto second = std::make_shared<Owner>("cancel_sibling_second");
  Work parent_work(parent, std::string{PENDING});
  const auto ancestor = observe.entered("cancel_sibling_parent");
  Work first_work(first, std::string{PENDING}, ancestor.token);
  Work second_work(second, std::string{PENDING}, ancestor.token);
  const auto first_generation = observe.entered("cancel_sibling_first");
  const auto second_generation = observe.entered("cancel_sibling_second");
  const bool admitted =
      alive(ancestor) && alive(first_generation) && alive(second_generation);
  const bool first_requested = request(first_generation.generation);
  const auto first_result = first_work.finish();
  const bool isolated = alive(ancestor) && alive(second_generation) &&
                        !ancestor.token.stop_requested();
  const bool parent_requested = request(ancestor.generation);
  const auto second_result = second_work.finish();
  const auto parent_result = parent_work.finish();
  const bool closed = first->close() && second->close() && parent->close();
  observe.close();
  const auto events = observe.records();
  const bool drained = drain();
  REQUIRE(admitted);
  REQUIRE_FALSE(
      same(first_generation.generation, second_generation.generation));
  REQUIRE(first_requested);
  REQUIRE(cancelled(first_result));
  REQUIRE(isolated);
  REQUIRE(parent_requested);
  REQUIRE(cancelled(second_result));
  REQUIRE(cancelled(parent_result));
  REQUIRE(closed);
  REQUIRE(events_match(events, 3, 3));
  REQUIRE(drained);
}

TEST_CASE("Retired capability generations cannot cancel a reused context",
          "[js][async][cap][cap-cancellation][cap-cancellation-reuse]") {
  initialize();
  Observe observe;
  auto parent = std::make_shared<Owner>("cancel_reuse_parent");
  auto child = std::make_shared<Owner>("cancel_reuse_child");
  Work parent_work(parent, std::string{PENDING});
  const auto ancestor = observe.entered("cancel_reuse_parent");
  Work old_work(child, "globalThis.__capTokenBody=true;42", ancestor.token);
  const auto old_result = old_work.finish();
  const auto old_generation = observe.entered("cancel_reuse_child");
  const bool old_retired = !snapshot(old_generation.generation).visited;
  const bool reused = child->release_reacquire();
  const bool reset = !child->marker();
  Work new_work(child, std::string{PENDING});
  std::vector<Event> entered;
  wait(
      [&] {
        entered = observe.records();
        return entered.size() == 3;
      },
      "cap token replacement generation ENTER missing");
  const auto new_generation = entered.back();
  const bool before = alive(new_generation);
  const bool requested = request(ancestor.generation);
  const auto parent_result = parent_work.finish();
  // A queued owner-loop fence follows the actual parent's deferred callbacks.
  static_cast<void>(snapshot({}));
  const bool fenced =
      alive(new_generation) &&
      !same(old_generation.generation, new_generation.generation) &&
      !snapshot(old_generation.generation).visited;
  const bool new_requested = request(new_generation.generation);
  const auto new_result = new_work.finish();
  const bool closed = child->close() && parent->close();
  observe.close();
  const auto events = observe.records();
  const bool drained = drain();
  REQUIRE(answer(old_result));
  REQUIRE(old_retired);
  REQUIRE(reused);
  REQUIRE(reset);
  REQUIRE(before);
  REQUIRE(requested);
  REQUIRE(cancelled(parent_result));
  REQUIRE(fenced);
  REQUIRE(new_requested);
  REQUIRE(cancelled(new_result));
  REQUIRE(closed);
  REQUIRE(events_match(events, 3, 2));
  REQUIRE(drained);
}

TEST_CASE(
    "Capability cancellation callback teardown joins cross-thread requests",
    "[js][async][cap][cap-cancellation][cap-cancellation-teardown]") {
  initialize();
  struct Epoch {
    bool entered;
    bool requested;
    bool parent_cancelled;
    bool child_classified;
    bool closed;
    bool records;
    bool drained;
  };
  std::vector<Epoch> epochs;
  epochs.reserve(8);
  const auto deadline = Clock::now() + 55s;
  for (unsigned epoch = 0; epoch < 8; ++epoch) {
    demand(Clock::now() < deadline, "cap token cross-thread epoch wall budget");
    Observe observe;
    Ack ack;
    auto parent = std::make_shared<Owner>("cancel_race_parent");
    auto child = std::make_shared<Owner>("cancel_race_child");
    Work parent_work(parent, std::string{PENDING});
    const auto ancestor = observe.entered("cancel_race_parent");
    const bool parent_active = alive(ancestor);
    child->install_ack(&acknowledge_cpu);
    Stopper stopper(ack.signal, ancestor.generation);
    Work child_work(child, "(()=>{__capTokenEntered();return 42;})()",
                    ancestor.token);
    const bool requested = stopper.finish();
    const auto child_result = child_work.finish();
    const auto parent_result = parent_work.finish();
    const bool child_classified =
        answer(child_result) || cancelled(child_result);
    const bool closed = child->close() && parent->close();
    observe.close();
    const auto events = observe.records();
    bool classification_matches = false;
    for (const auto& event : events) {
      if (event.name == "cancel_race_child")
        classification_matches = answer(child_result)
                                     ? !event.error.has_value()
                                     : event.error == EvalErrorKind::CANCELLED;
    }
    epochs.push_back(
        {parent_active && ack.signal->load(std::memory_order_acquire),
         requested, cancelled(parent_result),
         child_classified && classification_matches, closed,
         events_match(events, 2, cancelled(child_result) ? 2U : 1U), drain()});
  }
  // Actual concurrent completion/request overlap is attempted, not presumed.
  // Both legal race winners have exact real result and FINISH classifications.
  REQUIRE(epochs.size() == 8);
  for (const auto& epoch : epochs) {
    REQUIRE(epoch.entered);
    REQUIRE(epoch.requested);
    REQUIRE(epoch.parent_cancelled);
    REQUIRE(epoch.child_classified);
    REQUIRE(epoch.closed);
    REQUIRE(epoch.records);
    REQUIRE(epoch.drained);
  }
  REQUIRE(Clock::now() < deadline);
}
