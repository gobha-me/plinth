// SPDX-License-Identifier: MIT
//
// Real registered JS subscriptions and unsubscribe operations. The producer is
// SYNTHETIC broker::dispatch_for_test: no listener parsing or writer INSERT is
// exercised. Registry matches and actual JS callback receipts are independent.

#include "async_bridge_fixture.hpp"
#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <json/json.h>
#include <quickjs.h>

#include <array>
#include <atomic>
#include <chrono>
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
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using plinth::js::BridgeContext;
using plinth::js::RuntimePool;
namespace broker = plinth::realtime::broker;

constexpr std::size_t SCENARIOS = 4;
constexpr std::size_t REPLAYS = 2;
constexpr std::size_t MAX_RECEIPT_BYTES = 4096;

auto demand(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

template <typename Function>
auto on_loop(Function function) -> std::invoke_result_t<Function> {
  using Result = std::invoke_result_t<Function>;
  auto* loop = drogon::app().getLoop();
  demand(loop != nullptr, "pubsub delivery owner loop unavailable");
  if (loop->isInLoopThread()) return function();
  auto task =
      std::make_shared<std::packaged_task<Result()>>(std::move(function));
  auto future = task->get_future();
  loop->queueInLoop([task] { (*task)(); });
  // Do not unwind and free a context while its queued owner-loop task may
  // still refer to it. Expiry is a failed child, never successful cleanup.
  if (future.wait_for(5s) != std::future_status::ready) std::terminate();
  return future.get();
}

struct Receipt {
  unsigned calls = 0;
  bool valid = true;
  Json::Value events{Json::arrayValue};
};

struct ReceiptOwner {
  std::mutex mutex;
  Receipt receipt;
};

// Test-only synchronous observer. It neither replaces JSContext's opaque
// BridgeContext nor settles a promise. Its shared receipt outlives destruction
// of the actual context and every joined producer/owner-loop fence.
std::mutex observer_mutex;
std::unordered_map<JSContext*, std::shared_ptr<ReceiptOwner>> observers;

auto find_observer(JSContext* context) -> std::shared_ptr<ReceiptOwner> {
  std::lock_guard lock(observer_mutex);
  const auto found = observers.find(context);
  return found == observers.end() ? nullptr : found->second;
}

auto capture_receipt(JSContext* context, JSValue /*this_value*/, int argc,
                     JSValue* argv) -> JSValue {
  auto owner = find_observer(context);
  if (!owner) {
    return JS_ThrowInternalError(context, "pubsub test observer unavailable");
  }
  std::lock_guard lock(owner->mutex);
  ++owner->receipt.calls;
  try {
    if (argc != 1 || !JS_IsString(argv[0])) {
      owner->receipt.valid = false;
      return JS_UNDEFINED;
    }
    std::size_t size = 0;
    const auto release = [context](const char* value) {
      JS_FreeCString(context, value);
    };
    std::unique_ptr<const char, decltype(release)> text{
        JS_ToCStringLen(context, &size, argv[0]), release};
    if (!text || size > MAX_RECEIPT_BYTES) {
      owner->receipt.valid = false;
      return JS_UNDEFINED;
    }
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
    Json::Value parsed;
    std::string errors;
    if (!reader->parse(text.get(), text.get() + size, &parsed, &errors)) {
      owner->receipt.valid = false;
      return JS_UNDEFINED;
    }
    owner->receipt.events.append(std::move(parsed));
  } catch (...) {
    owner->receipt.valid = false;
  }
  return JS_UNDEFINED;
}

auto snapshot(const std::shared_ptr<ReceiptOwner>& owner) -> Receipt {
  std::lock_guard lock(owner->mutex);
  return owner->receipt;
}

class BrokerConfiguration {
 public:
  BrokerConfiguration()
      : previous_quota(broker::max_subscriptions_per_conn()),
        previous_rbac(broker::is_rbac_enforced()) {
    demand(broker::js_subscriber_count() == 0,
           "pubsub delivery refuses another fixture's subscriptions");
    broker::stop();
    plinth::Config::Realtime::Broker config;
    config.max_subscriptions_per_conn = 1;
    broker::start(config);
  }
  ~BrokerConfiguration() {
    // stop() alone leaves quota/RBAC changed in the retained grouped process.
    broker::stop();
    plinth::Config::Realtime::Broker restored;
    restored.max_subscriptions_per_conn = previous_quota;
    restored.rbac_enforce = previous_rbac;
    broker::start(restored);
    broker::stop();
  }
  BrokerConfiguration(const BrokerConfiguration&) = delete;
  auto operator=(const BrokerConfiguration&) -> BrokerConfiguration& = delete;

 private:
  std::size_t previous_quota;
  bool previous_rbac;
};

struct LiveState {
  bool retired;
  bool pending;
  bool callbacks;
  bool detached;
  bool concurrent;
  std::size_t persistent;
};

auto idle(const LiveState& state) -> bool {
  return state.retired && !state.pending && !state.callbacks &&
         !state.detached && !state.concurrent;
}

struct Cleanup {
  bool native;
  bool audits;
  bool destroyed;
  bool registry_empty;
  bool observer_removed;
  bool shutdown;
  bool idempotent;
  int active;
  int free;
};

auto clean(const Cleanup& state) -> bool {
  return state.native && state.audits && state.destroyed &&
         state.registry_empty && state.observer_removed && state.shutdown &&
         state.idempotent && state.active == 0 && state.free == 0;
}

class Owner {
 public:
  Owner()
      : receipts(std::make_shared<ReceiptOwner>()),
        config(plinth::async_bridge_test::test_config()),
        pool(nullptr, limits(), config, 1) {
    try {
      on_loop([this] {
        context = pool.acquire();
        demand(context != nullptr, "pubsub delivery context unavailable");
        context->extension_name = "pubdel";
        observer_key = context->ctx;
        {
          std::lock_guard lock(observer_mutex);
          demand(observers.emplace(observer_key, receipts).second,
                 "pubsub delivery duplicate observer owner");
          observer_registered = true;
        }
        JS_UpdateStackTop(context->rt);
        JSValue global = JS_GetGlobalObject(context->ctx);
        JSValue observer = JS_NewCFunction(context->ctx, capture_receipt,
                                           "__native_pubsub_receipt", 1);
        if (JS_IsException(observer)) {
          JS_FreeValue(context->ctx, global);
          throw std::runtime_error(
              "pubsub delivery observer allocation failed");
        }
        const int result = JS_SetPropertyStr(
            context->ctx, global, "__native_pubsub_receipt", observer);
        JS_FreeValue(context->ctx, global);
        demand(result >= 0, "pubsub delivery observer injection failed");
      });
    } catch (...) {
      if (!clean(finish())) std::terminate();
      throw;
    }
  }
  ~Owner() {
    if (!closed) {
      try {
        if (!clean(finish())) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;

  auto execute(const std::string& source) -> void {
    demand(context != nullptr && source.size() <= 2048,
           "pubsub delivery source/owner bound");
    const auto result =
        drogon::sync_wait(plinth::js::run_on_context(*context, source));
    demand(result.value.has_value() && result.value->isBool() &&
               result.value->asBool(),
           "pubsub delivery registered operation did not acknowledge");
  }
  auto state() const -> LiveState {
    demand(context != nullptr, "pubsub delivery attempted freed-context read");
    return on_loop([current = context] {
      return LiveState{
          !current->execution_lifetime.load(std::memory_order_acquire),
          !current->pending_ops.empty() ||
              !current->retired_pending_ops.empty(),
          !current->callbacks.empty(),
          current->inflight_detached.load(std::memory_order_acquire) != 0,
          current->concurrent_async_ops != 0,
          current->persistent_callbacks.size()};
    });
  }
  auto destroy() -> bool {
    // All producers have joined before this fence. The fence's position in
    // the same owner-loop queue acknowledges every preceding JS delivery.
    return on_loop([this] {
      if (context == nullptr) return pool.active_count() == 0;
      if (context->execution_lifetime.load(std::memory_order_acquire) ||
          context->inflight_detached.load(std::memory_order_acquire) != 0) {
        return false;
      }
      pool.destroy(context);
      if (pool.active_count() != 0) return false;
      context = nullptr;
      return true;
    });
  }
  auto finish() -> Cleanup {
    const bool native = plinth::lifecycle::async_tasks().drain(5s);
    const bool audits = plinth::log::wait_for_audits(5s);
    const bool destroyed = destroy();
    on_loop([] {});
    const bool registry_empty = broker::js_subscriber_count() == 0;
    bool removed = !observer_registered;
    if (destroyed && observer_registered) {
      removed = on_loop([key = observer_key, receipt = receipts] {
        std::lock_guard lock(observer_mutex);
        const auto found = observers.find(key);
        if (found == observers.end() || found->second != receipt) return false;
        observers.erase(found);
        return true;
      });
      if (removed) {
        observer_key = nullptr;
        observer_registered = false;
      }
    }
    // Never enter RuntimePool's unbounded destructor with a retained lease.
    const bool shutdown = destroyed && pool.shutdown(5s);
    const bool idempotent = shutdown && pool.shutdown(0ms);
    Cleanup result{native,           audits,   destroyed,  registry_empty,
                   removed,          shutdown, idempotent, pool.active_count(),
                   pool.free_count()};
    closed = clean(result);
    return result;
  }

  std::shared_ptr<ReceiptOwner> receipts;

 private:
  static auto limits() -> plinth::js::RuntimeLimits {
    auto value = plinth::js::default_runtime_limits();
    value.wall_clock_limit = 3s;
    return value;
  }
  plinth::Config config;
  RuntimePool pool;
  BridgeContext* context = nullptr;
  JSContext* observer_key = nullptr;
  bool observer_registered = false;
  bool closed = false;
};

auto channel(std::size_t scenario, std::size_t replay) -> std::string {
  return "plinth:ext:pubdel:case" + std::to_string(scenario) + "r" +
         std::to_string(replay);
}

auto subscribe(const std::string& target, char identity) -> std::string {
  return "pubsub.subscribe('" + target +
         "',function(event){"
         "const units=[];for(let i=0;i<event.payload.text.length;i++)"
         "units.push(event.payload.text.charCodeAt(i));"
         "__native_pubsub_receipt(JSON.stringify({who:'" +
         std::string(1, identity) +
         "',keys:Object.keys(event).sort(),layer:event.layer,"
         "channel:event.channel,payload:event.payload,units}));"
         "}).then(function(unsubscribe){globalThis.__native_unsubscribe="
         "unsubscribe;return typeof unsubscribe==='function';})";
}

auto synthetic_event(std::size_t scenario, std::size_t replay, int phase)
    -> plinth::realtime::DispatchedEvent {
  Json::Value payload{Json::objectValue};
  payload["marker"] = static_cast<int>(100 * scenario + 10 * replay) + phase;
  payload["text"] = replay == 0 ? "\xc3\xa9\xce\xa9\xf0\x9f\x98\x80"
                                : "\xe4\xb8\xad\xe2\x82\xac";
  payload["items"] = Json::Value{Json::arrayValue};
  payload["items"].append(replay == 0);
  payload["items"].append(phase);
  Json::Value envelope{Json::objectValue};
  envelope["layer"] = "extension";
  envelope["channel"] = channel(scenario, replay);
  envelope["payload"] = std::move(payload);
  return {"extension", channel(scenario, replay), std::move(envelope), {}};
}

auto produce(plinth::realtime::DispatchedEvent event) -> std::size_t {
  // The worker owns the entire SYNTHETIC envelope. Joining precedes the
  // owner-loop fence; the returned count is only broker registry matching.
  std::packaged_task<std::size_t()> task{
      [event = std::move(event)] { return broker::dispatch_for_test(event); }};
  auto future = task.get_future();
  std::jthread producer{std::move(task)};
  producer.join();
  const auto matches = future.get();
  on_loop([] {});
  return matches;
}

auto literal_receipt(std::size_t scenario, std::size_t replay, int phase,
                     char identity) -> Json::Value {
  // Literal oracle tables are independent of the producer's marker arithmetic
  // and QuickJS/host conversion helpers. UTF-16 units include the surrogate
  // pair.
  constexpr std::array MARKERS{
      std::array{std::array{1, 2}, std::array{11, 12}},
      std::array{std::array{101, 102}, std::array{111, 112}},
      std::array{std::array{201, 202}, std::array{211, 212}},
      std::array{std::array{301, 302}, std::array{311, 312}}};
  Json::Value result{Json::objectValue};
  result["who"] = std::string(1, identity);
  result["layer"] = "extension";
  result["channel"] = channel(scenario, replay);
  result["keys"] = Json::Value{Json::arrayValue};
  for (const auto* key : {"channel", "layer", "payload"})
    result["keys"].append(key);
  result["payload"]["marker"] =
      MARKERS.at(scenario).at(replay).at(static_cast<std::size_t>(phase - 1));
  result["payload"]["text"] = replay == 0 ? "\xc3\xa9\xce\xa9\xf0\x9f\x98\x80"
                                          : "\xe4\xb8\xad\xe2\x82\xac";
  result["payload"]["items"] = Json::Value{Json::arrayValue};
  result["payload"]["items"].append(replay == 0);
  result["payload"]["items"].append(phase);
  result["units"] = Json::Value{Json::arrayValue};
  if (replay == 0) {
    for (const int unit : {233, 937, 55357, 56832})
      result["units"].append(unit);
  } else {
    for (const int unit : {20013, 8364})
      result["units"].append(unit);
  }
  return result;
}

struct Observation {
  std::size_t scenario;
  std::size_t replay;
  std::size_t first_matches;
  std::size_t second_matches;
  std::size_t first_registrations;
  std::size_t second_registrations;
  Receipt first;
  Receipt second;
  LiveState before;
  LiveState after;
  bool destroyed_between;
  Cleanup cleanup;
};

auto valid(const Observation& value) -> bool {
  if (value.scenario >= SCENARIOS || value.replay >= REPLAYS) return false;
  const bool replacement = value.scenario == 0;
  const bool destroyed = value.scenario == 2;
  Json::Value first{Json::arrayValue};
  first.append(literal_receipt(value.scenario, value.replay, 1, 'A'));
  auto second = first;
  if (replacement)
    second.append(literal_receipt(value.scenario, value.replay, 2, 'B'));
  const bool before = idle(value.before) && value.before.persistent == 1;
  const bool after =
      destroyed
          ? value.destroyed_between
          : idle(value.after) && value.after.persistent ==
                                     static_cast<std::size_t>(
                                         replacement || value.scenario == 3);
  return value.first_matches == 1 && value.first_registrations == 1 &&
         value.second_matches == static_cast<std::size_t>(replacement) &&
         value.second_registrations == static_cast<std::size_t>(replacement) &&
         value.first.valid && value.first.calls == 1 &&
         value.first.events == first && value.second.valid &&
         value.second.calls == (replacement ? 2U : 1U) &&
         value.second.events == second && before && after &&
         value.destroyed_between == destroyed && clean(value.cleanup);
}

auto observe(std::size_t scenario, std::size_t replay) -> Observation {
  Owner owner;
  const auto target = channel(scenario, replay);
  owner.execute(subscribe(target, 'A'));
  const auto first_registrations = broker::js_subscriber_count();
  const auto first_matches = produce(synthetic_event(scenario, replay, 1));
  const auto first = snapshot(owner.receipts);
  const auto before = owner.state();
  bool destroyed = false;
  if (scenario == 0) {
    owner.execute(subscribe(target, 'B'));
  } else if (scenario == 1) {
    owner.execute("__native_unsubscribe().then(function(){return true;})");
  } else if (scenario == 2) {
    destroyed = owner.destroy();
  } else {
    on_loop([] { broker::drain_extension("pubdel", "upgrading"); });
  }
  const auto second_registrations = broker::js_subscriber_count();
  const auto second_matches = produce(synthetic_event(scenario, replay, 2));
  const auto second = snapshot(owner.receipts);
  // An external owned receipt is observed after destruction. Do not read the
  // old context, or manufacture an empty persistent-callback map for drain.
  const auto after = scenario == 2 ? LiveState{} : owner.state();
  const auto cleanup = owner.finish();
  return {scenario,
          replay,
          first_matches,
          second_matches,
          first_registrations,
          second_registrations,
          first,
          second,
          before,
          after,
          destroyed,
          cleanup};
}

auto complete(const std::vector<Observation>& observations) -> bool {
  if (observations.size() != SCENARIOS * REPLAYS) return false;
  std::array<std::array<unsigned, REPLAYS>, SCENARIOS> counts{};
  for (const auto& observation : observations) {
    if (!valid(observation)) return false;
    if (++counts.at(observation.scenario).at(observation.replay) != 1)
      return false;
  }
  for (const auto& row : counts)
    for (const auto count : row)
      if (count != 1) return false;
  return true;
}

auto corrupt(Observation& value, unsigned mutation) -> void {
  switch (mutation) {
    case 0: value.first.events[0]["who"] = "B"; break;
    case 1: value.second.events[0]["payload"]["marker"] = -1; break;
    case 2: value.second.events[0]["units"][0] = 0; break;
    case 3: ++value.second.calls; break;
    case 4: value.second.events.append(value.second.events[0]); break;
    case 5: value.first_matches = 0; break;
    case 6: value.second_matches = 2; break;
    case 7: value.first_registrations = 0; break;
    case 8: value.second_registrations = 2; break;
    case 9: value.before.callbacks = true; break;
    case 10: value.cleanup.native = false; break;
    case 11: value.cleanup.audits = false; break;
    case 12: value.cleanup.destroyed = false; break;
    case 13: value.cleanup.registry_empty = false; break;
    case 14: value.cleanup.observer_removed = false; break;
    case 15: value.cleanup.shutdown = false; break;
    case 16: value.cleanup.idempotent = false; break;
    case 17: value.cleanup.active = 1; break;
    case 18: value.cleanup.free = 1; break;
    case 19: value.second.valid = false; break;
    case 20: value.second.events[0]["layer"] = "system"; break;
    case 21: value.second.events[0]["channel"] = "wrong-channel"; break;
    case 22: value.before.persistent = 0; break;
    default:
      if (value.scenario == 2) {
        value.destroyed_between = false;
      } else {
        value.after.persistent = 2;
      }
      break;
  }
}

} // namespace

TEST_CASE("Native pubsub registered delivery preserves owned cleanup",
          "[js][realtime][integration][native-pubsub-delivery]") {
  const auto deadline = std::chrono::steady_clock::now() + 55s;
  demand(plinth::async_bridge_test::pg_available(),
         "native pubsub delivery requires PostgreSQL");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  demand(plinth::lifecycle::async_tasks().drain(5s) &&
             plinth::log::wait_for_audits(5s),
         "native pubsub delivery requires drained native/audit work");
  BrokerConfiguration broker_configuration;
  demand(broker::max_subscriptions_per_conn() == 1,
         "native pubsub replacement requires observed quota one");
  std::vector<Observation> observations;
  for (std::size_t scenario = 0; scenario < SCENARIOS; ++scenario) {
    for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
      demand(std::chrono::steady_clock::now() < deadline,
             "native pubsub delivery corpus deadline exceeded");
      observations.push_back(observe(scenario, replay));
    }
  }
  REQUIRE(complete(observations));
  unsigned mutation_controls = 0;
  unsigned ledger_controls = 0;
  for (std::size_t index = 0; index < observations.size(); ++index) {
    for (unsigned mutation = 0; mutation < 24; ++mutation) {
      auto changed = observations[index];
      corrupt(changed, mutation);
      CHECK_FALSE(valid(changed));
      ++mutation_controls;
    }
    auto dropped = observations;
    dropped.erase(dropped.begin() + static_cast<std::ptrdiff_t>(index));
    CHECK_FALSE(complete(dropped));
    auto duplicated = observations;
    duplicated[index] = observations[(index + 1) % observations.size()];
    CHECK_FALSE(complete(duplicated));
    ledger_controls += 2;
  }
  CHECK(mutation_controls == 192);
  CHECK(ledger_controls == 16);
  CHECK(broker::js_subscriber_count() == 0);
  {
    std::lock_guard lock(observer_mutex);
    CHECK(observers.empty());
  }
  CHECK(std::chrono::steady_clock::now() < deadline);
}
