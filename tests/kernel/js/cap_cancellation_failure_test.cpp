// SPDX-License-Identifier: MIT

#include "native_capability_fixture.hpp"

#include "kernel/js/execution_lifetime.hpp"
#include "kernel/js/native_setup_failure.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/drogon.h>
#include <quickjs.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace fixture = native_capability_corpus::fixture;
using plinth::js::EvalErrorKind;
using plinth::js::NativeSetupFailurePoint;
using Clock = std::chrono::steady_clock;
using Generation = std::weak_ptr<plinth::js::ExecutionLifetime>;
using namespace std::chrono_literals;

class FailureScope {
 public:
  explicit FailureScope(NativeSetupFailurePoint point) {
    plinth::js::set_native_setup_failure_for_test(point);
  }
  ~FailureScope() { plinth::js::clear_native_setup_failure_for_test(); }
  FailureScope(const FailureScope&) = delete;
  auto operator=(const FailureScope&) -> FailureScope& = delete;
};

class Owner {
  plinth::Config config = plinth::async_bridge_test::test_config();
  bool closed = false;

 public:
  plinth::js::RuntimePool pool;
  plinth::js::BridgeContext* context = nullptr;

  explicit Owner(std::chrono::milliseconds wall = 2s)
      : pool(nullptr, profile(wall), config, 1) {
    acquire();
  }
  ~Owner() {
    if (!closed) {
      try {
        if (!fixture::stopped(finish())) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;

  static auto profile(std::chrono::milliseconds wall)
      -> plinth::js::RuntimeLimits {
    auto limits = plinth::js::default_runtime_limits();
    limits.wall_clock_limit = wall;
    return limits;
  }
  auto acquire() -> void {
    fixture::demand(context == nullptr, "fault owner acquired twice");
    context = pool.acquire();
    fixture::demand(context != nullptr, "fault owner context unavailable");
  }
  auto destroy() -> void {
    fixture::demand(context != nullptr && !context->execution_lifetime.load(
                                              std::memory_order_acquire),
                    "fault owner context still has an execution");
    pool.destroy(std::exchange(context, nullptr));
    fixture::demand(pool.active_count() == 0, "fault owner lease not returned");
  }
  auto cleanup() -> fixture::Cleanup {
    const auto& batch = context->batch_state;
    fixture::Cleanup actual{
        !context->execution_lifetime.load(std::memory_order_acquire),
        context->pending_ops.size(),
        context->retired_pending_ops.size(),
        context->callbacks.size(),
        context->persistent_callbacks.size(),
        context->concurrent_async_ops,
        context->inflight_detached.load(std::memory_order_acquire),
        batch.depth,
        batch.scope_id,
        static_cast<bool>(batch.pinned_conn),
        static_cast<bool>(batch.rollback_owner),
        batch.timer_id != trantor::InvalidTimerId,
        batch.timer_loop != nullptr,
        plinth::lifecycle::async_tasks().drain(2s),
        plinth::log::wait_for_audits(2s),
        plinth::lifecycle::async_tasks().active_for_test(),
        0};
    destroy();
    actual.pool_active = pool.active_count();
    return actual;
  }
  auto finish() -> fixture::Shutdown {
    if (context != nullptr) destroy();
    const bool first = pool.shutdown(2s);
    const bool second = pool.shutdown(0ms);
    const fixture::Shutdown actual{first, second, pool.active_count(),
                                   pool.free_count()};
    closed = fixture::stopped(actual);
    return actual;
  }
};

auto drive(plinth::js::BridgeContext& context, const std::string& source,
           std::stop_token inherited = {}) -> plinth::js::EvalResult {
  return drogon::sync_wait(
      plinth::js::run_on_context(context, source, nullptr, inherited));
}

struct Snapshot {
  Generation generation;
  bool visited = false;
  bool cancelled = false;
  bool sticky = false;
  bool empty_native = false;
  Clock::time_point started;
};

auto observe(const Generation& generation) -> Snapshot {
  Snapshot result;
  result.generation = generation;
  {
    auto lifetime = generation.lock();
    if (lifetime) {
      result.sticky = lifetime->cancellation_requested();
      result.visited =
          lifetime->visit([&](const plinth::js::BridgeContext& context) {
            result.cancelled =
                context.cancelled.load(std::memory_order_acquire);
            result.started = context.execution_start;
            result.empty_native =
                context.next_callback_id == 0 && context.pending_ops.empty() &&
                context.callbacks.empty() &&
                context.concurrent_async_ops == 0 &&
                context.inflight_detached.load(std::memory_order_acquire) == 0;
          });
    }
  } // No strong generation/task lease survives acknowledgement publication.
  return result;
}

auto snapshot(const Generation& generation) -> Snapshot {
  auto promise = std::make_shared<std::promise<Snapshot>>();
  auto ready = promise->get_future();
  drogon::app().getLoop()->queueInLoop([generation, promise] {
    try {
      promise->set_value(observe(generation));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  fixture::demand(ready.wait_for(2s) == std::future_status::ready,
                  "fault generation snapshot deadline");
  return ready.get();
}

struct Acknowledgement {
  std::promise<Snapshot> promise;
};
std::mutex acknowledgement_mutex;
std::shared_ptr<Acknowledgement> acknowledgement;

// Called synchronously by the real JS evaluation. queueInLoop never executes
// inline: its acknowledgement runs only after the driver yields on the still
// pending pure-JS promise. It neither settles that promise nor wakes its
// driver.
auto suspended_ack(JSContext* context, JSValue /*this_value*/, int /*argc*/,
                   JSValue* /*argv*/) -> JSValue {
  try {
    std::shared_ptr<Acknowledgement> owned;
    {
      std::lock_guard lock(acknowledgement_mutex);
      owned = acknowledgement;
    }
    fixture::demand(owned != nullptr, "fault acknowledgement has no owner");
    auto* bridge =
        static_cast<plinth::js::BridgeContext*>(JS_GetContextOpaque(context));
    fixture::demand(bridge != nullptr, "fault acknowledgement has no context");
    Generation generation =
        bridge->execution_lifetime.load(std::memory_order_acquire);
    drogon::app().getLoop()->queueInLoop([owned, generation] {
      try {
        owned->promise.set_value(observe(generation));
      } catch (...) {
        owned->promise.set_exception(std::current_exception());
      }
    });
    return JS_UNDEFINED;
  } catch (...) {
    return JS_ThrowInternalError(context,
                                 "fault suspension acknowledgement failed");
  }
}

class AcknowledgementScope {
 public:
  explicit AcknowledgementScope(std::shared_ptr<Acknowledgement> owned) {
    std::lock_guard lock(acknowledgement_mutex);
    fixture::demand(!acknowledgement,
                    "fault acknowledgement already installed");
    acknowledgement = std::move(owned);
  }
  ~AcknowledgementScope() {
    std::shared_ptr<Acknowledgement> retired;
    {
      std::lock_guard lock(acknowledgement_mutex);
      retired.swap(acknowledgement);
    }
  }
  AcknowledgementScope(const AcknowledgementScope&) = delete;
  auto operator=(const AcknowledgementScope&) -> AcknowledgementScope& = delete;
};

class Driver {
  std::shared_ptr<Owner> owner;
  std::stop_source parent;
  std::promise<plinth::js::EvalResult> promise;
  std::future<plinth::js::EvalResult> result = promise.get_future();
  std::thread worker;

 public:
  Driver(std::shared_ptr<Owner> input, std::stop_source source)
      : owner(std::move(input)), parent(std::move(source)), worker([this] {
          try {
            promise.set_value(drive(
                *owner->context, "__capFailureSuspended();new Promise(()=>{})",
                parent.get_token()));
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
        }) {}
  ~Driver() {
    if (worker.joinable()) {
      try {
        static_cast<void>(parent.request_stop());
        if (result.wait_for(7s) != std::future_status::ready) std::terminate();
        worker.join();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Driver(const Driver&) = delete;
  auto operator=(const Driver&) -> Driver& = delete;
  auto pending_until(Clock::time_point deadline) -> bool {
    return result.wait_until(deadline) == std::future_status::timeout;
  }
  auto finish() -> plinth::js::EvalResult {
    fixture::demand(
        result.wait_for(2s) == std::future_status::ready,
        "fault child did not finish within existing wall-wake bound");
    worker.join();
    return result.get();
  }
};

auto recovery(Owner& owner) -> bool {
  owner.acquire();
  const auto result = drive(*owner.context, "40+2");
  const auto cleanup = owner.cleanup();
  return result.value.has_value() && result.value->isNumeric() &&
         result.value->asDouble() == 42 && fixture::clean(cleanup);
}

} // namespace

TEST_CASE("Capability cancellation setup faults retire owned contexts",
          "[js][async][cap][hardening][cap-cancellation-setup-faults]") {
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  const std::array points{
      NativeSetupFailurePoint::execution_lifetime_setup,
      NativeSetupFailurePoint::inherited_cancel_registration};
  for (const auto point : points) {
    for (unsigned replay = 0; replay < 2; ++replay) {
      INFO("point=" << static_cast<unsigned>(point) << " replay=" << replay);
      Owner owner;
      std::stop_source parent;
      plinth::js::EvalResult actual;
      NativeSetupFailurePoint consumed;
      {
        const FailureScope failure(point);
        actual = drive(*owner.context, "42", parent.get_token());
        consumed = plinth::js::native_setup_failure_observed_for_test();
      }
      const auto cleanup = owner.cleanup();
      const bool recovered = recovery(owner);
      const auto shutdown = owner.finish();
      REQUIRE(consumed == point);
      REQUIRE_FALSE(actual.value.has_value());
      REQUIRE(actual.value.error().kind == EvalErrorKind::RUNTIME_ERROR);
      REQUIRE(fixture::clean(cleanup));
      REQUIRE(recovered);
      REQUIRE(fixture::stopped(shutdown));
    }
  }
}

TEST_CASE("Capability cancellation notification faults retain sticky wall-wake "
          "cancellation",
          "[js][async][cap][hardening][cap-cancellation-notification-faults]") {
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  for (unsigned replay = 0; replay < 2; ++replay) {
    INFO("replay=" << replay);
    auto owner = std::make_shared<Owner>(500ms);
    auto shared = std::make_shared<Acknowledgement>();
    auto acknowledged = shared->promise.get_future();
    const AcknowledgementScope scope(shared);
    JSValue global = JS_GetGlobalObject(owner->context->ctx);
    JSValue function = JS_NewCFunction(owner->context->ctx, &suspended_ack,
                                       "__capFailureSuspended", 0);
    const bool installed =
        !JS_IsException(function) &&
        JS_SetPropertyStr(owner->context->ctx, global, "__capFailureSuspended",
                          function) >= 0;
    JS_FreeValue(owner->context->ctx, global);
    fixture::demand(installed,
                    "fault suspension native function installation failed");
    std::stop_source parent;
    Driver driver(owner, parent);
    fixture::demand(acknowledged.wait_for(2s) == std::future_status::ready,
                    "fault suspended-child acknowledgement deadline");
    const auto initial = acknowledged.get();
    bool requested = false;
    bool no_early_completion = false;
    NativeSetupFailurePoint consumed;
    Snapshot stopped;
    plinth::js::EvalResult actual;
    {
      const FailureScope failure(
          NativeSetupFailurePoint::inherited_cancel_notification);
      requested = parent.request_stop();
      consumed = plinth::js::native_setup_failure_observed_for_test();
      stopped = snapshot(initial.generation);
      no_early_completion = driver.pending_until(initial.started + 450ms);
      actual = driver.finish();
    }
    const auto elapsed = Clock::now() - initial.started;
    const auto cleanup = owner->cleanup();
    const bool recovered = recovery(*owner);
    const auto shutdown = owner->finish();
    REQUIRE(initial.visited);
    REQUIRE_FALSE(initial.cancelled);
    REQUIRE_FALSE(initial.sticky);
    REQUIRE(initial.empty_native);
    REQUIRE(requested);
    REQUIRE(consumed == NativeSetupFailurePoint::inherited_cancel_notification);
    REQUIRE(stopped.visited);
    REQUIRE(stopped.sticky);
    REQUIRE_FALSE(stopped.cancelled);
    REQUIRE(stopped.empty_native);
    REQUIRE(no_early_completion);
    REQUIRE_FALSE(actual.value.has_value());
    REQUIRE(actual.value.error().kind == EvalErrorKind::CANCELLED);
    REQUIRE(actual.duration >= 450ms);
    REQUIRE(elapsed < 2s);
    REQUIRE(fixture::clean(cleanup));
    REQUIRE(recovered);
    REQUIRE(fixture::stopped(shutdown));
  }
}
