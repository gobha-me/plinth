// SPDX-License-Identifier: MIT
//
// Private deterministic ownership probe. A default-off broker test hook denies
// wrong-loop delivery BEFORE BC/QuickJS access. Thus an unrepaired route fails
// its predicate without running JS on the wrong thread or racing destruction.

#include "async_bridge_fixture.hpp"
#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/execution_lifetime.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <quickjs.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using plinth::js::BridgeContext;
using plinth::js::EvalResult;
using plinth::js::ExecutionLifetime;
using plinth::js::RuntimePool;
namespace broker = plinth::realtime::broker;

auto demand(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

template <typename Function>
auto on_loop(trantor::EventLoop* loop, Function function)
    -> std::invoke_result_t<Function> {
  using Result = std::invoke_result_t<Function>;
  demand(loop != nullptr, "pubsub owner probe loop unavailable");
  if (loop->isInLoopThread()) return function();
  auto task =
      std::make_shared<std::packaged_task<Result()>>(std::move(function));
  auto ready = task->get_future();
  loop->queueInLoop([task] { (*task)(); });
  // A timed-out task may still borrow an owned context. Never unwind and free
  // that context; a failed process is preferable to unsafe fixture cleanup.
  if (ready.wait_for(3s) != std::future_status::ready) std::terminate();
  return ready.get();
}

auto fence(trantor::EventLoop* loop) -> void {
  on_loop(loop, [] {});
}

class OwnedLoop {
 public:
  OwnedLoop() {
    thread.run();
    fence(thread.getLoop());
  }
  ~OwnedLoop() {
    if (!closed && !finish()) std::terminate();
  }
  OwnedLoop(const OwnedLoop&) = delete;
  auto operator=(const OwnedLoop&) -> OwnedLoop& = delete;
  auto loop() -> trantor::EventLoop* { return thread.getLoop(); }
  auto finish() -> bool {
    if (closed) return true;
    thread.requestStop();
    closed = thread.waitFor(3s);
    return closed;
  }

 private:
  trantor::EventLoopThread thread;
  bool closed = false;
};

struct Attempt {
  trantor::EventLoop* loop;
  std::thread::id thread;
  bool context;
  bool channel;
  bool active;
  bool visited;
  bool gate_held;
  bool allowed;
};

struct Callback {
  trantor::EventLoop* loop;
  std::thread::id thread;
  bool context;
  bool number;
  int marker;
};

struct Evidence {
  unsigned checkpoints = 0;
  bool acknowledged = false;
  bool ack_active = false;
  bool ack_visited = false;
  bool ack_on_owner = false;
  bool gate_timeout = false;
  bool failed = false;
  std::vector<Attempt> attempts;
  std::vector<Callback> callbacks;
};

struct State {
  explicit State(trantor::EventLoop* expected, bool should_block)
      : owner_loop(expected), block_owner(should_block) {}
  std::mutex mutex;
  std::condition_variable condition;
  trantor::EventLoop* const owner_loop;
  const bool block_owner;
  std::thread::id owner_thread;
  BridgeContext* context = nullptr; // Identity only outside its owner loop.
  JSContext* js_context = nullptr;  // Identity only outside its owner loop.
  std::string channel;
  std::weak_ptr<ExecutionLifetime> generation;
  bool gate_released = false;
  bool gate_waiting = false;
  Evidence evidence;
};

std::atomic<std::shared_ptr<State>> native_state;

auto release_gate(const std::shared_ptr<State>& state) -> void {
  {
    std::lock_guard lock(state->mutex);
    state->gate_released = true;
  }
  state->condition.notify_all();
}

auto acknowledge(const std::shared_ptr<State>& state,
                 std::weak_ptr<ExecutionLifetime> weak) noexcept -> void {
  try {
    bool active = false;
    bool visited = false;
    {
      auto generation = weak.lock();
      if (generation) {
        active = generation->active();
        visited = generation->visit([](BridgeContext&) {});
      }
    } // No strong generation or accepted task lease spans ACK or the gate.
    std::unique_lock lock(state->mutex);
    state->evidence.acknowledged = true;
    state->evidence.ack_active = active;
    state->evidence.ack_visited = visited;
    state->evidence.ack_on_owner =
        trantor::EventLoop::getEventLoopOfCurrentThread() == state->owner_loop;
    state->gate_waiting = state->block_owner;
    state->condition.notify_all();
    if (state->block_owner && !state->condition.wait_for(lock, 5s, [&] {
          return state->gate_released;
        })) {
      state->evidence.gate_timeout = true;
    }
    state->gate_waiting = false;
  } catch (...) {
    std::lock_guard lock(state->mutex);
    state->evidence.failed = true;
    state->condition.notify_all();
  }
}

auto checkpoint(JSContext* context, JSValue /*this_value*/, int /*argc*/,
                JSValue* /*argv*/) -> JSValue {
  const auto state = native_state.load(std::memory_order_acquire);
  if (!state) return JS_ThrowInternalError(context, "owner probe absent");
  try {
    demand(context == state->js_context &&
               trantor::EventLoop::getEventLoopOfCurrentThread() ==
                   state->owner_loop,
           "owner probe checkpoint ran outside expected loop");
    auto* bridge = static_cast<BridgeContext*>(JS_GetContextOpaque(context));
    demand(bridge == state->context, "owner probe checkpoint identity");
    std::weak_ptr<ExecutionLifetime> weak =
        bridge->execution_lifetime.load(std::memory_order_acquire);
    {
      std::lock_guard lock(state->mutex);
      ++state->evidence.checkpoints;
      state->generation = weak;
    }
    // This task can execute only after run_on_context yields to this loop.
    // The unresolved top-level JS promise keeps the real generation active.
    state->owner_loop->queueInLoop([state, weak] { acknowledge(state, weak); });
  } catch (...) {
    std::lock_guard lock(state->mutex);
    state->evidence.failed = true;
    state->condition.notify_all();
    return JS_ThrowInternalError(context, "owner probe checkpoint failed");
  }
  return JS_UNDEFINED;
}

auto callback(JSContext* context, JSValue /*this_value*/, int argc,
              JSValue* argv) -> JSValue {
  const auto state = native_state.load(std::memory_order_acquire);
  if (!state) return JS_ThrowInternalError(context, "owner receipt absent");
  try {
    const bool number = argc == 1 && JS_IsNumber(argv[0]);
    int marker = -1;
    const bool converted = number && JS_ToInt32(context, &marker, argv[0]) == 0;
    const Callback receipt{trantor::EventLoop::getEventLoopOfCurrentThread(),
                           std::this_thread::get_id(),
                           context == state->js_context, converted, marker};
    std::lock_guard lock(state->mutex);
    state->evidence.callbacks.push_back(receipt);
  } catch (...) {
    std::lock_guard lock(state->mutex);
    state->evidence.failed = true;
  }
  return JS_UNDEFINED;
}

auto attempt(const std::shared_ptr<State>& state, const BridgeContext* identity,
             std::string_view channel) noexcept -> bool {
  try {
    std::weak_ptr<ExecutionLifetime> weak;
    {
      std::lock_guard lock(state->mutex);
      weak = state->generation;
    }
    bool active = false;
    bool visited = false;
    {
      auto generation = weak.lock();
      if (generation) {
        active = generation->active();
        visited = generation->visit([](BridgeContext&) {});
      }
    }
    const auto* actual = trantor::EventLoop::getEventLoopOfCurrentThread();
    const bool context = identity == state->context;
    const bool target = channel == state->channel;
    const bool allowed =
        actual == state->owner_loop && context && target && active && visited;
    std::lock_guard lock(state->mutex);
    state->evidence.attempts.push_back(
        {trantor::EventLoop::getEventLoopOfCurrentThread(),
         std::this_thread::get_id(), context, target, active, visited,
         state->gate_waiting && !state->gate_released, allowed});
    return allowed;
  } catch (...) {
    std::lock_guard lock(state->mutex);
    state->evidence.failed = true;
    return false;
  }
}

class BrokerConfiguration {
 public:
  BrokerConfiguration()
      : quota(broker::max_subscriptions_per_conn()),
        rbac(broker::is_rbac_enforced()) {
    demand(broker::js_subscriber_count() == 0,
           "owner probe refuses pre-existing subscriptions");
    broker::stop();
    broker::start(plinth::Config::Realtime::Broker{});
  }
  ~BrokerConfiguration() {
    broker::set_js_delivery_probe_for_test({}, {});
    broker::stop();
    plinth::Config::Realtime::Broker restored;
    restored.max_subscriptions_per_conn = quota;
    restored.rbac_enforce = rbac;
    broker::start(restored);
    broker::stop();
  }
  BrokerConfiguration(const BrokerConfiguration&) = delete;
  auto operator=(const BrokerConfiguration&) -> BrokerConfiguration& = delete;

 private:
  std::size_t quota;
  bool rbac;
};

class Owner {
 public:
  Owner(trantor::EventLoop* loop, bool dedicated)
      : state(std::make_shared<State>(loop, dedicated)),
        config(plinth::async_bridge_test::test_config()) {
    state->channel = dedicated ? "plinth:ext:pubowner:dedicated"
                               : "plinth:ext:pubowner:control";
  }
  ~Owner() {
    if (!closed) {
      try {
        release_gate(state);
        cancel();
        if (!finish()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;

  auto initialize() -> void {
    std::shared_ptr<State> empty;
    demand(native_state.compare_exchange_strong(empty, state),
           "owner probe native observer already owned");
    observation_owned = true;
    on_loop(state->owner_loop, [this] {
      pool = std::make_unique<RuntimePool>(
          nullptr, plinth::js::default_runtime_limits(), config, 1);
      context = pool->acquire();
      demand(context != nullptr, "owner probe context unavailable");
      context->extension_name = "pubowner";
      state->context = context;
      state->js_context = context->ctx;
      state->owner_thread = std::this_thread::get_id();
      inject("__pubsub_owner_checkpoint", checkpoint);
      inject("__pubsub_owner_receipt", callback);
    });
    broker::set_js_delivery_probe_for_test(
        state->channel, [shared = state](const BridgeContext* identity,
                                         std::string_view channel) noexcept {
          return attempt(shared, identity, channel);
        });
    probe_owned = true;
  }
  auto cancel() -> void {
    // The physical gate must be released before queueing onto its loop.
    release_gate(state);
    on_loop(state->owner_loop, [this] {
      if (context != nullptr) {
        auto generation =
            context->execution_lifetime.load(std::memory_order_acquire);
        if (generation) static_cast<void>(generation->request_cancel());
      }
    });
  }
  auto finish() -> bool {
    release_gate(state);
    fence(drogon::app().getLoop());
    fence(state->owner_loop);
    if (probe_owned) {
      broker::set_js_delivery_probe_for_test({}, {});
      probe_owned = false;
    }
    fence(drogon::app().getLoop());
    fence(state->owner_loop);
    const bool native = plinth::lifecycle::async_tasks().drain(5s);
    const bool audits = plinth::log::wait_for_audits(5s);
    const bool stopped = on_loop(state->owner_loop, [this] {
      if (!pool) return true;
      if (context != nullptr) {
        if (context->execution_lifetime.load(std::memory_order_acquire) ||
            context->inflight_detached.load(std::memory_order_acquire) != 0)
          return false;
        pool->destroy(context);
        if (pool->active_count() != 0) return false;
        context = nullptr;
      }
      const bool first = pool->shutdown(5s);
      const bool second = first && pool->shutdown(0ms);
      return first && second && pool->active_count() == 0 &&
             pool->free_count() == 0;
    });
    if (observation_owned && stopped) {
      auto expected = state;
      demand(native_state.compare_exchange_strong(expected, {}),
             "owner probe native observer changed ownership");
      observation_owned = false;
    }
    closed = native && audits && stopped && !observation_owned &&
             broker::js_subscriber_count() == 0;
    return closed;
  }
  auto borrowed_context() const -> BridgeContext& {
    demand(context != nullptr, "owner probe missing active context");
    return *context;
  }

  const std::shared_ptr<State> state;

 private:
  auto inject(const char* name, JSCFunction* function) -> void {
    JSValue value = JS_NewCFunction(context->ctx, function, name, 1);
    demand(!JS_IsException(value), "owner probe native function allocation");
    JSValue global = JS_GetGlobalObject(context->ctx);
    const int result = JS_SetPropertyStr(context->ctx, global, name, value);
    JS_FreeValue(context->ctx, global);
    demand(result >= 0, "owner probe native function injection");
  }
  plinth::Config config;
  std::unique_ptr<RuntimePool> pool;
  BridgeContext* context = nullptr;
  bool observation_owned = false;
  bool probe_owned = false;
  bool closed = false;
};

auto run_from_owner(std::shared_ptr<Owner> owner, std::string source)
    -> drogon::Task<EvalResult> {
  co_await drogon::switchThreadCoro(owner->state->owner_loop);
  co_return co_await plinth::js::run_on_context(owner->borrowed_context(),
                                                source);
}

class Run {
 public:
  explicit Run(std::shared_ptr<Owner> owner_in)
      : owner(std::move(owner_in)), ready(promise.get_future()),
        worker([this, held = owner] {
          try {
            const auto source = "(async function(){await pubsub.subscribe('" +
                                held->state->channel +
                                "',function(event){__pubsub_owner_receipt("
                                "event.payload.marker);"
                                "});__pubsub_owner_checkpoint();"
                                "return await new Promise(function(){});})()";
            promise.set_value(drogon::sync_wait(run_from_owner(held, source)));
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
        }) {}
  ~Run() {
    if (worker.joinable()) {
      try {
        owner->cancel();
        if (ready.wait_for(5s) != std::future_status::ready) std::terminate();
        worker.join();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Run(const Run&) = delete;
  auto operator=(const Run&) -> Run& = delete;
  auto finish() -> EvalResult {
    owner->cancel();
    demand(ready.wait_for(5s) == std::future_status::ready,
           "owner probe driver did not cancel");
    worker.join();
    return ready.get();
  }

 private:
  std::shared_ptr<Owner> owner;
  std::promise<EvalResult> promise;
  std::future<EvalResult> ready;
  std::thread worker;
};

auto await_ack(const std::shared_ptr<State>& state) -> bool {
  std::unique_lock lock(state->mutex);
  demand(state->condition.wait_for(lock, 3s,
                                   [&] {
                                     return state->evidence.acknowledged ||
                                            state->evidence.failed;
                                   }),
         "owner probe suspension acknowledgement timed out");
  demand(!state->evidence.failed && state->evidence.acknowledged,
         "owner probe checkpoint failed");
  return state->gate_waiting && !state->gate_released;
}

auto produce(const std::string& channel) -> std::size_t {
  // Synthetic producer only: no listener parsing, events INSERT or wire claim.
  plinth::realtime::DispatchedEvent event;
  event.layer = "extension";
  event.channel = channel;
  event.envelope["layer"] = "extension";
  event.envelope["channel"] = channel;
  event.envelope["payload"]["marker"] = 73;
  std::packaged_task<std::size_t()> task{
      [event = std::move(event)] { return broker::dispatch_for_test(event); }};
  auto result = task.get_future();
  std::jthread producer{std::move(task)};
  if (result.wait_for(3s) != std::future_status::ready) std::terminate();
  producer.join();
  return result.get();
}

struct Result {
  bool dedicated;
  bool distinct_loops;
  bool gate_held;
  trantor::EventLoop* expected_loop;
  std::thread::id expected_thread;
  std::size_t matches;
  Evidence evidence;
  bool cancelled;
  bool cleanup;
  bool loop_joined;
};

auto observe(bool dedicated) -> Result {
  std::unique_ptr<OwnedLoop> target;
  if (dedicated) target = std::make_unique<OwnedLoop>();
  auto* loop = target ? target->loop() : drogon::app().getLoop();
  auto owner = std::make_shared<Owner>(loop, dedicated);
  owner->initialize();
  Run running(owner);
  const bool held = await_ack(owner->state);
  const auto matches = produce(owner->state->channel);
  fence(drogon::app().getLoop());
  release_gate(owner->state);
  fence(loop);
  Evidence evidence;
  {
    std::lock_guard lock(owner->state->mutex);
    evidence = owner->state->evidence;
  }
  const auto result = running.finish();
  const bool cancelled =
      !result.value.has_value() &&
      result.value.error().kind == plinth::js::EvalErrorKind::CANCELLED;
  const bool cleanup = owner->finish();
  const bool joined = target == nullptr || target->finish();
  return {dedicated,
          loop != drogon::app().getLoop(),
          held,
          loop,
          owner->state->owner_thread,
          matches,
          std::move(evidence),
          cancelled,
          cleanup,
          joined};
}

auto route_is_owned(const Result& result) -> bool {
  if (result.evidence.attempts.size() != 1) return false;
  const auto& actual = result.evidence.attempts[0];
  return actual.loop == result.expected_loop &&
         actual.thread == result.expected_thread && actual.context &&
         actual.channel && actual.active && actual.visited && actual.allowed;
}

auto callback_is_owned(const Result& result) -> bool {
  if (result.evidence.callbacks.size() != 1) return false;
  const auto& actual = result.evidence.callbacks[0];
  return actual.loop == result.expected_loop &&
         actual.thread == result.expected_thread && actual.context &&
         actual.number && actual.marker == 73;
}

} // namespace

TEST_CASE("Pubsub dispatch remains on the registered JavaScript owner loop",
          "[js][realtime][pubsub-owner-regression]") {
  demand(plinth::async_bridge_test::pg_available(),
         "pubsub owner probe requires PostgreSQL");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  demand(plinth::lifecycle::async_tasks().drain(5s) &&
             plinth::log::wait_for_audits(5s),
         "pubsub owner probe requires drained native/audit work");
  BrokerConfiguration broker_configuration;
  const auto control = observe(false);
  const auto dedicated = observe(true);
  for (const auto* result : {&control, &dedicated}) {
    CAPTURE(result->dedicated, result->evidence.attempts.size(),
            result->evidence.callbacks.size());
    CHECK(result->distinct_loops == result->dedicated);
    CHECK(result->gate_held == result->dedicated);
    CHECK(result->matches == 1);
    CHECK(result->evidence.checkpoints == 1);
    CHECK(result->evidence.acknowledged);
    CHECK(result->evidence.ack_active);
    CHECK(result->evidence.ack_visited);
    CHECK(result->evidence.ack_on_owner);
    CHECK_FALSE(result->evidence.failed);
    CHECK_FALSE(result->evidence.gate_timeout);
    if (result->evidence.attempts.size() == 1) {
      const auto& actual = result->evidence.attempts[0];
      const bool app_loop = actual.loop == drogon::app().getLoop();
      const bool expected_loop = actual.loop == result->expected_loop;
      CAPTURE(app_loop, expected_loop, actual.active, actual.visited,
              actual.context, actual.channel, actual.gate_held, actual.allowed);
      CHECK(actual.context);
      CHECK(actual.channel);
      CHECK(actual.active);
      CHECK(route_is_owned(*result));
    } else {
      CHECK(result->evidence.attempts.size() == 1);
    }
    CHECK(callback_is_owned(*result));
    CHECK(result->cancelled);
    CHECK(result->cleanup);
    CHECK(result->loop_joined);
  }
  CHECK(broker::js_subscriber_count() == 0);
  CHECK(native_state.load(std::memory_order_acquire) == nullptr);
}

namespace {
namespace retirement {

struct Receipt {
  int handler;
  int marker;
  bool owner_loop;
};

struct Observations {
  explicit Observations(trantor::EventLoop* target) : loop(target) {}
  trantor::EventLoop* const loop;
  std::mutex mutex;
  std::condition_variable changed;
  bool parked = false;
  bool released = false;
  bool park_timeout = false;
  bool allow_delivery = true;
  bool failed = false;
  bool setup_observed = false;
  bool setup_before_stale = false;
  bool busy_requested = false;
  bool busy_entered = false;
  bool busy_released = false;
  bool busy_completed = false;
  bool busy_timeout = false;
  bool retry_observed = false;
  unsigned stale_attempts = 0;
  BridgeContext* identity = nullptr;
  JSContext* js_identity = nullptr;
  std::vector<Receipt> receipts;
};

std::atomic<std::shared_ptr<Observations>> recording;

auto open(const std::shared_ptr<Observations>& observed) -> void {
  {
    std::lock_guard lock(observed->mutex);
    observed->released = true;
    observed->busy_released = true;
  }
  observed->changed.notify_all();
}

auto record(JSContext* context, JSValue /*this_value*/, int argc, JSValue* argv)
    -> JSValue {
  const auto observed = recording.load(std::memory_order_acquire);
  if (!observed)
    return JS_ThrowInternalError(context, "retirement receipt not owned");
  try {
    int handler = -1;
    int marker = -1;
    const bool values = argc == 2 && JS_IsNumber(argv[0]) &&
                        JS_IsNumber(argv[1]) &&
                        JS_ToInt32(context, &handler, argv[0]) == 0 &&
                        JS_ToInt32(context, &marker, argv[1]) == 0;
    std::unique_lock lock(observed->mutex);
    observed->failed =
        observed->failed || !values || context != observed->js_identity;
    observed->receipts.push_back(
        {handler, marker,
         trantor::EventLoop::getEventLoopOfCurrentThread() == observed->loop});
    if (observed->busy_requested) {
      // This is an ACTUAL native callback frame, inside the checkout's visit.
      // Do not touch JS during the bounded gate; an off-loop return must keep
      // this context owned until the callback and its JS stack have returned.
      observed->busy_entered = true;
      observed->changed.notify_all();
      if (!observed->changed.wait_for(lock, 5s,
                                      [&] { return observed->busy_released; }))
        observed->busy_timeout = true;
      observed->busy_completed = true;
    }
  } catch (...) {
    std::lock_guard lock(observed->mutex);
    observed->failed = true;
  }
  return JS_UNDEFINED;
}

auto permitted(const std::shared_ptr<Observations>& observed,
               const BridgeContext* identity) noexcept -> bool {
  try {
    std::lock_guard lock(observed->mutex);
    if (!observed->allow_delivery) {
      ++observed->stale_attempts;
      return false; // Before BC/JS access, including an unrepaired stale BC.
    }
    const bool correct =
        identity == observed->identity &&
        trantor::EventLoop::getEventLoopOfCurrentThread() == observed->loop;
    observed->failed = observed->failed || !correct;
    return correct;
  } catch (...) {
    return false;
  }
}

class Owner {
 public:
  struct BusyReturn {
    bool prompt;
    bool held;
    int checked_out;
    std::size_t pending;
    bool shutdown_refused;
  };
  explicit Owner(trantor::EventLoop* loop)
      : observed(std::make_shared<Observations>(loop)),
        config(plinth::async_bridge_test::test_config()) {}
  ~Owner() {
    if (!closed) {
      try {
        open(observed);
        if (!finish()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;

  auto initialize() -> void {
    std::shared_ptr<Observations> empty;
    demand(recording.compare_exchange_strong(empty, observed),
           "retirement observer already owned");
    observation_owned = true;
    on_loop(observed->loop, [this] {
      pool = std::make_unique<RuntimePool>(
          nullptr, plinth::js::default_runtime_limits(), config, 1);
      context = pool->acquire();
      demand(context != nullptr, "retirement context unavailable");
      context->extension_name = "pubret";
      observed->identity = context;
      observed->js_identity = context->ctx;
    });
    broker::set_js_delivery_probe_for_test(
        "plinth:ext:pubret:queued",
        [state = observed](const BridgeContext* identity,
                           std::string_view) noexcept {
          return permitted(state, identity);
        });
    probe_owned = true;
  }
  auto inject() -> void {
    JSValue function =
        JS_NewCFunction(context->ctx, record, "__pubsub_retirement_receipt", 2);
    demand(!JS_IsException(function), "retirement receipt allocation failed");
    JSValue global = JS_GetGlobalObject(context->ctx);
    const int result = JS_SetPropertyStr(
        context->ctx, global, "__pubsub_retirement_receipt", function);
    JS_FreeValue(context->ctx, global);
    demand(result >= 0, "retirement receipt injection failed");
  }
  auto borrowed() -> BridgeContext& {
    demand(context != nullptr, "retirement missing checked-out context");
    return *context;
  }
  auto park() -> void {
    {
      std::lock_guard lock(observed->mutex);
      observed->allow_delivery = false;
    }
    observed->loop->queueInLoop([state = observed] {
      std::unique_lock lock(state->mutex);
      state->parked = true;
      state->changed.notify_all();
      if (!state->changed.wait_for(lock, 5s, [&] { return state->released; }))
        state->park_timeout = true;
    });
    std::unique_lock lock(observed->mutex);
    demand(
        observed->changed.wait_for(lock, 3s, [&] { return observed->parked; }),
        "retirement owner-loop park not acknowledged");
  }
  auto retire(bool reuse) -> bool {
    // Actual release/destroy runs only after the previous driver and all JS
    // jobs settled, while the original loop is positively parked without JS.
    // This serial host retirement never races a QuickJS access or free.
    return on_loop(drogon::app().getLoop(), [this, reuse] {
      {
        std::lock_guard lock(observed->mutex);
        demand(observed->parked && !observed->released &&
                   !observed->park_timeout,
               "retirement requires an exclusively parked owner loop");
      }
      demand(context != nullptr &&
                 !context->execution_lifetime.load(std::memory_order_acquire) &&
                 context->pending_ops.empty() &&
                 context->retired_pending_ops.empty() &&
                 context->callbacks.empty() &&
                 context->inflight_detached.load(std::memory_order_acquire) ==
                     0 &&
                 !JS_IsJobPending(context->rt),
             "retirement requires fully settled actual JS ownership");
      const auto* previous = context;
      old_checkout =
          context->pubsub_delivery_owner.load(std::memory_order_acquire);
      if (!reuse) {
        pool->destroy(context);
        if (pool->active_count() != 0) return false;
        context = nullptr;
        return broker::js_subscriber_count() == 0;
      }
      pool->release(context);
      if (pool->active_count() != 0) return false;
      context = nullptr;
      demand(pool->free_count() == 1,
             "retirement release did not retain entry");
      context = pool->acquire();
      demand(context != nullptr, "retirement replacement checkout unavailable");
      context->extension_name = "pubret";
      observed->identity = context;
      observed->js_identity = context->ctx;
      const std::weak_ptr<plinth::js::PubsubDeliveryOwner> replacement =
          context->pubsub_delivery_owner.load(std::memory_order_acquire);
      const bool different_checkout = old_checkout.owner_before(replacement) ||
                                      replacement.owner_before(old_checkout);
      return context == previous && different_checkout &&
             broker::js_subscriber_count() == 0;
    });
  }
  auto release_busy() -> BusyReturn {
    return on_loop(drogon::app().getLoop(), [this] {
      {
        std::lock_guard lock(observed->mutex);
        demand(observed->busy_entered && !observed->busy_released &&
                   !observed->busy_timeout,
               "busy return requires a positively held actual callback");
      }
      demand(context != nullptr, "busy return missing checked-out context");
      old_checkout =
          context->pubsub_delivery_owner.load(std::memory_order_acquire);
      const auto started = std::chrono::steady_clock::now();
      pool->release(context);
      const auto elapsed = std::chrono::steady_clock::now() - started;
      const auto checked_out = pool->active_count();
      // A successful void return transfers ownership to the pool's pending
      // retirement record. Never dereference the relinquished BC afterward.
      if (checked_out == 0) context = nullptr;
      const auto pending = pool->pending_retirement_count();
      const bool shutdown_refused = !pool->shutdown(0ms);
      std::lock_guard lock(observed->mutex);
      const bool held = observed->busy_entered && !observed->busy_released &&
                        !observed->busy_completed && !observed->busy_timeout;
      return BusyReturn{elapsed < 2s, held, checked_out, pending,
                        shutdown_refused};
    });
  }
  auto pending_retirements() const -> std::size_t {
    return pool ? pool->pending_retirement_count() : 0;
  }
  auto observe_retry() -> void {
    // Arm after release/shutdown(0ms), so only the first global drain can
    // acknowledge this actual retry of the still-queued retirement.
    pool->set_retirement_retry_observer_for_test([state = observed] {
      {
        std::lock_guard lock(state->mutex);
        state->retry_observed = true;
      }
      state->changed.notify_all();
    });
  }
  auto finish() -> bool {
    open(observed);
    fence(drogon::app().getLoop());
    fence(observed->loop);
    fence(observed->loop); // Also acknowledge destruction of prior queue tails.
    if (probe_owned) {
      broker::set_js_delivery_probe_for_test({}, {});
      probe_owned = false;
    }
    if (pool) pool->set_retirement_retry_observer_for_test({});
    const bool native = plinth::lifecycle::async_tasks().drain(5s);
    const bool audits = plinth::log::wait_for_audits(5s);
    const bool stopped = on_loop(observed->loop, [this] {
      if (!pool) return true;
      if (context != nullptr) {
        if (context->execution_lifetime.load(std::memory_order_acquire) ||
            context->inflight_detached.load(std::memory_order_acquire) != 0)
          return false;
        pool->destroy(context);
        if (pool->active_count() != 0) return false;
        context = nullptr;
      }
      const bool first = pool->shutdown(5s);
      const bool second = first && pool->shutdown(0ms);
      return first && second && pool->active_count() == 0 &&
             pool->free_count() == 0;
    });
    if (stopped && observation_owned) {
      auto expected = observed;
      demand(recording.compare_exchange_strong(expected, {}),
             "retirement observer changed ownership");
      observation_owned = false;
    }
    closed = native && audits && stopped && !observation_owned &&
             broker::js_subscriber_count() == 0;
    return closed;
  }
  auto old_owner_expired() const -> bool { return old_checkout.expired(); }

  const std::shared_ptr<Observations> observed;

 private:
  plinth::Config config;
  std::unique_ptr<RuntimePool> pool;
  BridgeContext* context = nullptr;
  std::weak_ptr<plinth::js::PubsubDeliveryOwner> old_checkout;
  bool observation_owned = false;
  bool probe_owned = false;
  bool closed = false;
};

auto subscription(std::shared_ptr<Owner> owner, int identity,
                  std::shared_ptr<std::promise<EvalResult>> completion)
    -> drogon::Task<> {
  try {
    owner->inject();
    const auto source =
        "pubsub.subscribe('plinth:ext:pubret:queued',function(event){"
        "__pubsub_retirement_receipt(" +
        std::to_string(identity) +
        ",event.payload.marker);}).then(function(){return true;})";
    completion->set_value(
        co_await plinth::js::run_on_context(owner->borrowed(), source));
  } catch (...) {
    completion->set_exception(std::current_exception());
  }
}

class Subscription {
 public:
  explicit Subscription(std::shared_ptr<Owner> target)
      : owner(std::move(target)),
        completion(std::make_shared<std::promise<EvalResult>>()),
        result(completion->get_future().share()) {}
  ~Subscription() {
    if (queued && !settled) {
      open(owner->observed);
      if (result.wait_for(3s) != std::future_status::ready) std::terminate();
      fence(owner->observed->loop);
    }
  }
  Subscription(const Subscription&) = delete;
  auto operator=(const Subscription&) -> Subscription& = delete;
  auto queue(int identity) -> void {
    demand(!queued, "retirement subscription queued twice");
    queued = true;
    owner->observed->loop->queueInLoop(
        [held = owner, promise = completion, identity] {
          auto task = subscription(held, identity, promise);
          drogon::async_run(
              [task = std::move(task)]() mutable noexcept -> drogon::Task<> {
                return std::move(task);
              });
        });
  }
  auto succeeded() -> bool {
    demand(result.wait_for(3s) == std::future_status::ready,
           "retirement actual subscription did not settle");
    const auto& actual = result.get();
    fence(owner->observed->loop);
    settled = true;
    return actual.value.has_value() && actual.value->isBool() &&
           actual.value->asBool();
  }
  auto queue_order_receipt() -> void {
    owner->observed->loop->queueInLoop(
        [state = owner->observed, ready = result] {
          bool settled = false;
          try {
            if (ready.wait_for(0s) == std::future_status::ready) {
              const auto& value = ready.get().value;
              settled = value.has_value() && value->isBool() &&
                        value->asBool() && broker::js_subscriber_count() == 1;
            }
          } catch (...) {
            settled = false;
          }
          std::lock_guard lock(state->mutex);
          state->setup_observed = true;
          state->setup_before_stale = settled;
        });
  }

 private:
  std::shared_ptr<Owner> owner;
  std::shared_ptr<std::promise<EvalResult>> completion;
  std::shared_future<EvalResult> result;
  bool queued = false;
  bool settled = false;
};

auto produce_marker(int marker) -> std::size_t {
  plinth::realtime::DispatchedEvent event;
  event.layer = "extension";
  event.channel = "plinth:ext:pubret:queued";
  event.envelope["layer"] = "extension";
  event.envelope["channel"] = event.channel;
  event.envelope["payload"]["marker"] = marker;
  std::packaged_task<std::size_t()> task{
      [event = std::move(event)] { return broker::dispatch_for_test(event); }};
  auto ready = task.get_future();
  std::jthread producer{std::move(task)};
  if (ready.wait_for(3s) != std::future_status::ready) std::terminate();
  producer.join();
  return ready.get();
}

struct Result {
  bool reuse;
  bool subscribed;
  std::size_t initial_matches;
  std::size_t queued_matches;
  bool queue_owned;
  bool retired;
  bool replacement_ready;
  bool setup_observed;
  bool setup_before_stale;
  unsigned stale_attempts;
  bool old_expired;
  std::size_t fresh_matches;
  std::vector<Receipt> before;
  std::vector<Receipt> after;
  bool observed_failure;
  bool park_timeout;
  bool cleanup;
  bool joined;
};

auto observe(bool reuse) -> Result {
  OwnedLoop target;
  auto owner = std::make_shared<Owner>(target.loop());
  owner->initialize();
  Subscription initial(owner);
  initial.queue(0);
  const bool subscribed = initial.succeeded();
  const auto first = produce_marker(101);
  fence(target.loop());
  fence(target.loop());
  demand(plinth::lifecycle::async_tasks().drain(3s),
         "retirement initial delivery did not drain");
  std::vector<Receipt> before;
  {
    std::lock_guard lock(owner->observed->mutex);
    before = owner->observed->receipts;
  }
  Subscription replacement(owner);
  owner->park();
  if (reuse) {
    // Queue setup and its positive settlement receipt BEFORE the producer can
    // queue the stale snapshot. Setup executes only after loop unpark, using
    // the new checkout installed by retire() while the loop is still parked.
    replacement.queue(1);
    replacement.queue_order_receipt();
  }
  const auto queued = produce_marker(102);
  const bool queue_owned =
      plinth::lifecycle::async_tasks().active_for_test() == 1;
  const bool retired = owner->retire(reuse);
  open(owner->observed);
  fence(target.loop());
  fence(target.loop());
  const bool ready = !reuse || replacement.succeeded();
  const bool old_expired = owner->old_owner_expired();
  unsigned attempts = 0;
  bool setup = false;
  bool ordered = false;
  {
    std::lock_guard lock(owner->observed->mutex);
    attempts = owner->observed->stale_attempts;
    setup = owner->observed->setup_observed;
    ordered = owner->observed->setup_before_stale;
    demand(owner->observed->receipts.size() == before.size(),
           "retirement stale snapshot invoked a JavaScript callback");
    owner->observed->allow_delivery = true;
  }
  const auto fresh = reuse ? produce_marker(103) : std::size_t{0};
  fence(target.loop());
  fence(target.loop());
  std::vector<Receipt> after;
  bool failed = false;
  bool timeout = false;
  {
    std::lock_guard lock(owner->observed->mutex);
    after = owner->observed->receipts;
    failed = owner->observed->failed;
    timeout = owner->observed->park_timeout;
  }
  const bool cleanup = owner->finish();
  const bool joined = target.finish();
  return {reuse,  subscribed, first,   queued,   queue_owned, retired,
          ready,  setup,      ordered, attempts, old_expired, fresh,
          before, after,      failed,  timeout,  cleanup,     joined};
}

struct BusyResult {
  bool subscribed;
  std::size_t matches;
  Owner::BusyReturn returned;
  bool retry_observed;
  bool first_drain;
  bool completed;
  bool timeout;
  bool failed;
  std::size_t pending_after;
  bool old_expired;
  std::vector<Receipt> receipts;
  bool cleanup;
  bool joined;
};

class FirstDrain {
 public:
  explicit FirstDrain(std::shared_ptr<Observations> state)
      : observed(std::move(state)) {
    std::packaged_task<bool()> task{
        [] { return plinth::lifecycle::async_tasks().drain(2s); }};
    result = task.get_future();
    worker = std::jthread(std::move(task));
  }
  ~FirstDrain() {
    // Unconditionally release the real callback BEFORE joining. No rescuing
    // second drain or fence may run until this first drain has joined.
    open(observed);
    if (worker.joinable()) {
      if (result.wait_for(3s) != std::future_status::ready) std::terminate();
      worker.join();
    }
  }
  FirstDrain(const FirstDrain&) = delete;
  auto operator=(const FirstDrain&) -> FirstDrain& = delete;
  auto complete() -> bool {
    if (result.wait_for(3s) != std::future_status::ready) std::terminate();
    worker.join();
    return result.get();
  }

 private:
  std::shared_ptr<Observations> observed;
  std::future<bool> result;
  std::jthread worker;
};

auto observe_busy() -> BusyResult {
  OwnedLoop target;
  auto owner = std::make_shared<Owner>(target.loop());
  owner->initialize();
  Subscription initial(owner);
  initial.queue(2);
  const bool subscribed = initial.succeeded();
  {
    std::lock_guard lock(owner->observed->mutex);
    owner->observed->busy_requested = true;
  }
  // SYNTHETIC producer, real registry match and actual injected JS callback.
  // The independent receipt is obtained inside the native C callback, not
  // inferred from the broker's registry-match return value.
  const auto matches = produce_marker(201);
  {
    std::unique_lock lock(owner->observed->mutex);
    demand(owner->observed->changed.wait_for(
               lock, 3s, [&] { return owner->observed->busy_entered; }),
           "busy return actual callback did not enter its bounded gate");
  }
  const auto returned = owner->release_busy();
  owner->observe_retry();
  FirstDrain draining(owner->observed);
  bool retry_observed = false;
  {
    std::unique_lock lock(owner->observed->mutex);
    demand(owner->observed->changed.wait_for(
               lock, 3s, [&] { return owner->observed->retry_observed; }),
           "first global drain did not retry actual queued retirement");
    retry_observed = owner->observed->retry_observed;
  }
  open(owner->observed);
  // Snapshot the SAME first drain before fences, pool retries or another
  // global drain can rescue completion. Assertions run only after cleanup.
  const bool first_drain = draining.complete();
  fence(target.loop());
  fence(target.loop()); // Pending destruction and prior owned queue tails.
  demand(plinth::lifecycle::async_tasks().drain(5s),
         "busy return actual delivery/retirement did not drain");
  const auto pending_after = owner->pending_retirements();
  const bool old_expired = owner->old_owner_expired();
  std::vector<Receipt> receipts;
  bool completed = false;
  bool timeout = false;
  bool failed = false;
  {
    std::lock_guard lock(owner->observed->mutex);
    receipts = owner->observed->receipts;
    completed = owner->observed->busy_completed;
    timeout = owner->observed->busy_timeout;
    failed = owner->observed->failed;
  }
  const bool cleanup = owner->finish();
  const bool joined = target.finish();
  return {subscribed, matches, returned, retry_observed, first_drain,
          completed,  timeout, failed,   pending_after,  old_expired,
          receipts,   cleanup, joined};
}

} // namespace retirement
} // namespace

TEST_CASE("Pubsub queued delivery rejects retired checkout ownership",
          "[js][realtime][pubsub-owner-retirement]") {
  demand(plinth::async_bridge_test::pg_available(),
         "pubsub retirement requires PostgreSQL");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  demand(plinth::lifecycle::async_tasks().drain(5s) &&
             plinth::log::wait_for_audits(5s),
         "pubsub retirement requires settled native/audit work");
  BrokerConfiguration broker_configuration;
  const auto destroyed = retirement::observe(false);
  const auto reused = retirement::observe(true);
  const auto busy = retirement::observe_busy();
  for (const auto* result : {&destroyed, &reused}) {
    CAPTURE(result->reuse, result->stale_attempts);
    CHECK(result->subscribed);
    CHECK(result->initial_matches == 1);
    CHECK(result->queued_matches == 1);
    CHECK(result->queue_owned);
    CHECK(result->retired);
    CHECK(result->replacement_ready);
    CHECK(result->setup_observed == result->reuse);
    CHECK(result->setup_before_stale == result->reuse);
    CHECK(result->stale_attempts == 0);
    CHECK(result->old_expired);
    CHECK(result->fresh_matches == static_cast<std::size_t>(result->reuse));
    REQUIRE(result->before.size() == 1);
    CHECK(result->before[0].handler == 0);
    CHECK(result->before[0].marker == 101);
    CHECK(result->before[0].owner_loop);
    REQUIRE(result->after.size() == (result->reuse ? 2U : 1U));
    CHECK(result->after[0].handler == 0);
    CHECK(result->after[0].marker == 101);
    CHECK(result->after[0].owner_loop);
    if (result->reuse) {
      CHECK(result->after[1].handler == 1);
      CHECK(result->after[1].marker == 103);
      CHECK(result->after[1].owner_loop);
    }
    CHECK_FALSE(result->observed_failure);
    CHECK_FALSE(result->park_timeout);
    CHECK(result->cleanup);
    CHECK(result->joined);
  }
  CHECK(busy.subscribed);
  CHECK(busy.matches == 1);
  CHECK(busy.returned.prompt);
  CHECK(busy.returned.held);
  CHECK(busy.returned.checked_out == 0);
  CHECK(busy.returned.pending == 1);
  CHECK(busy.returned.shutdown_refused);
  CHECK(busy.retry_observed);
  CHECK(busy.first_drain);
  CHECK(busy.completed);
  CHECK_FALSE(busy.timeout);
  CHECK_FALSE(busy.failed);
  CHECK(busy.pending_after == 0);
  CHECK(busy.old_expired);
  REQUIRE(busy.receipts.size() == 1);
  CHECK(busy.receipts[0].handler == 2);
  CHECK(busy.receipts[0].marker == 201);
  CHECK(busy.receipts[0].owner_loop);
  CHECK(busy.cleanup);
  CHECK(busy.joined);
  CHECK(retirement::recording.load(std::memory_order_acquire) == nullptr);
  CHECK(broker::js_subscriber_count() == 0);
}
