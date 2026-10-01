// SPDX-License-Identifier: MIT
//
// DB-free admission corpus through the registered QuickJS host functions.
// Settlements and unsubscribe cleanup are explicit simulations, not broker
// dispatch, PostgreSQL acknowledgement or the production cancellation driver.
// Generator bounds below are test budgets, not new production limits.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/js/stdlib_inject.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/emit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <json/value.h>
#include <json/writer.h>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1480'0000'0000'0001ULL, 0x1480'0000'0000'0027ULL,
    0x1480'0000'0000'00C6ULL, 0x1480'0000'0000'BEEFULL};
constexpr int CASES_PER_SEED = 48;
constexpr std::size_t FIXED_REGRESSIONS = 8;
constexpr int MAX_DEPTH = 3;
constexpr int MAX_NODES = 15;
constexpr std::size_t MAX_STRING_BYTES = 64;
constexpr std::size_t MAX_CHANNEL_BYTES = 96;
constexpr std::size_t MAX_BOUNDARY_BYTES = 8192;
constexpr int MAX_JOBS = 128;
constexpr int MAX_SHRINK_ATTEMPTS = 16;
constexpr int INITIAL_ID = 17;
constexpr std::size_t DEFAULT_QUOTA = 64;
constexpr std::size_t DEFAULT_PAYLOAD_BYTES = 8000;
constexpr std::string_view OWN_CHANNEL = "plinth:ext:notes:chat";
constexpr auto SHUTDOWN_BOUND = 2s;

using plinth::js::AsyncOp;
using plinth::js::BridgeContext;
using plinth::js::RuntimePool;

class OwnedValue {
 public:
  OwnedValue(JSContext* ctx, JSValue value) : context(ctx), value(value) {}
  ~OwnedValue() { JS_FreeValue(context, value); }
  OwnedValue(const OwnedValue&) = delete;
  auto operator=(const OwnedValue&) -> OwnedValue& = delete;
  OwnedValue(OwnedValue&&) = delete;
  auto operator=(OwnedValue&&) -> OwnedValue& = delete;
  auto get() const -> JSValue { return value; }

 private:
  JSContext* context;
  JSValue value;
};

class Lease {
 public:
  explicit Lease(RuntimePool& owner) : pool(owner), context(pool.acquire()) {}
  ~Lease() { destroy(); }
  Lease(const Lease&) = delete;
  auto operator=(const Lease&) -> Lease& = delete;
  Lease(Lease&&) = delete;
  auto operator=(Lease&&) -> Lease& = delete;
  auto get() const -> BridgeContext* { return context; }
  auto release() -> void {
    pool.release(context);
    context = nullptr;
  }
  auto destroy() -> void {
    if (context != nullptr) {
      pool.destroy(context);
      context = nullptr;
    }
  }

 private:
  RuntimePool& pool;
  BridgeContext* context;
};

struct GlobalGuard {
  bool rbac = plinth::realtime::broker::is_rbac_enforced();
  std::size_t payload_bytes = plinth::realtime::get_max_payload_bytes();
  GlobalGuard() = default;
  ~GlobalGuard() {
    plinth::realtime::broker::set_rbac_enforce_for_test(rbac);
    plinth::realtime::set_max_payload_bytes(payload_bytes);
    plinth::realtime::broker::reset_audit_windows_for_test();
  }
  GlobalGuard(const GlobalGuard&) = delete;
  auto operator=(const GlobalGuard&) -> GlobalGuard& = delete;
  GlobalGuard(GlobalGuard&&) = delete;
  auto operator=(GlobalGuard&&) -> GlobalGuard& = delete;
};

auto fake_user() -> plinth::capabilities::UserContext {
  return {.user_id = "fake-pubsub-user",
          .username = "fake-caller",
          .auth_type = "session",
          .effective_rules = {},
          .session_id = "fake-session",
          .ip_address = "192.0.2.1"};
}

auto json_source(const Json::Value& value) -> std::string {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  writer["emitUTF8"] = true;
  return Json::writeString(writer, value);
}

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

auto generated_string(std::uint64_t& state) -> std::string {
  constexpr std::array<std::string_view, 5> ATOMS{
      "a", std::string_view{"\0", 1}, "\xC3\xA9", "\xE4\xB8\xAD",
      "\xF0\x9F\x98\x80"};
  const auto target = static_cast<std::size_t>(next(state) % 65U);
  std::string result;
  while (result.size() < target) {
    const auto atom = ATOMS.at(static_cast<std::size_t>(next(state) % 5U));
    if (result.size() + atom.size() > target) {
      break;
    }
    result.append(atom);
  }
  return result;
}

auto tree(std::uint64_t& state, int depth, int& remaining) -> Json::Value {
  REQUIRE(remaining > 0);
  --remaining;
  const auto kind = next(state) % (depth >= MAX_DEPTH ? 5U : 7U);
  switch (kind) {
    case 0: return Json::Value{};
    case 1: return Json::Value{true};
    case 2: return Json::Value{false};
    case 3: return Json::Value{static_cast<int>(next(state) % 201U) - 100};
    case 4: return Json::Value{generated_string(state)};
    default: {
      const bool array = kind == 5;
      Json::Value result(array ? Json::arrayValue : Json::objectValue);
      const auto count = static_cast<int>(next(state) % 3U);
      for (int index = 0; index < count && remaining > 0; ++index) {
        auto child = tree(state, depth + 1, remaining);
        if (array) {
          result.append(std::move(child));
        } else {
          result["k" + std::to_string(index)] = std::move(child);
        }
      }
      return result;
    }
  }
}

struct Bounds {
  int nodes = 0;
  int depth = 0;
  std::size_t string_bytes = 0;
};

auto measure(const Json::Value& value, int depth, Bounds& result) -> void {
  ++result.nodes;
  result.depth = std::max(result.depth, depth);
  if (value.isString()) {
    result.string_bytes =
        std::max(result.string_bytes, value.asString().size());
  } else if (value.isArray()) {
    for (const auto& child : value) {
      measure(child, depth + 1, result);
    }
  } else if (value.isObject()) {
    for (const auto& key : value.getMemberNames()) {
      result.string_bytes = std::max(result.string_bytes, key.size());
      measure(value[key], depth + 1, result);
    }
  }
}

enum class Result { exception, rejected, pending, other };

struct CorpusCase {
  int category = 0;
  bool subscribe = false;
  int argc = 2;
  std::string channel{OWN_CHANNEL};
  std::optional<std::string> channel_literal;
  std::optional<std::string> payload_literal;
  Json::Value payload{Json::objectValue};
  std::string owner = "notes";
  std::vector<std::string> rules;
  bool cancelled = false;
  bool rbac = true;
  bool throwing_handler = false;
  bool simulate_rejection = false;
  std::size_t prefill = 0;
  Result want = Result::pending;
  std::string code;
};

auto type_error(CorpusCase& item) -> void {
  item.want = Result::exception;
  item.code = "TypeError";
}

auto rejection(CorpusCase& item, std::string code) -> void {
  item.want = Result::rejected;
  item.code = std::move(code);
}

auto generated_case(std::uint64_t seed, int index) -> CorpusCase {
  auto state = seed ^ (static_cast<std::uint64_t>(index) << 32U);
  CorpusCase item;
  item.category = index;
  item.subscribe = index >= 28;
  item.simulate_rejection = index == 0 && (seed & 1U) != 0;
  item.payload["marker"] = static_cast<int>(next(state) % 201U) - 100;
  item.payload["noise"] = std::string(16U + next(state) % 49U, 'a');
  int remaining = MAX_NODES - 3;
  item.payload["detail"] = tree(state, 1, remaining);
  switch (index) {
    case 0: break;
    case 1: item.payload = Json::Value{}; break;
    case 2: item.payload = true; break;
    case 3: {
      item.payload = Json::Value{Json::arrayValue};
      item.payload.append(17);
      item.payload.append(generated_string(state));
      break;
    }
    case 4: item.payload = std::string{"a\0\xC3\xA9", 4}; break;
    case 5: item.payload = Json::Value{Json::objectValue}; break;
    case 6:
      item.argc = 0;
      type_error(item);
      break;
    case 7:
      item.argc = 1;
      type_error(item);
      break;
    case 8:
      item.channel_literal = "123";
      type_error(item);
      break;
    case 9:
      item.channel_literal = "null";
      type_error(item);
      break;
    case 10:
      item.channel.clear();
      rejection(item, "pubsub.channel_invalid");
      break;
    case 11:
      item.channel = "wrong:ext:notes:chat";
      rejection(item, "pubsub.channel_invalid");
      break;
    case 12:
      item.channel += std::string{"\0tail", 5};
      rejection(item, "pubsub.channel_invalid");
      break;
    case 13:
      item.channel += "\xC3\xA9";
      rejection(item, "pubsub.channel_invalid");
      break;
    case 14:
      item.channel += std::string(64, 'a');
      rejection(item, "pubsub.channel_invalid");
      break;
    case 15:
      item.channel = "plinth:data:ext_notes.notes";
      rejection(item, "pubsub.channel_invalid");
      break;
    case 16:
      item.channel = "plinth:system:packages.installed";
      rejection(item, "pubsub.channel_invalid");
      break;
    case 17:
      item.owner.clear();
      rejection(item, "pubsub.extension_mismatch");
      break;
    case 18:
      item.channel = "plinth:ext:other:chat";
      rejection(item, "pubsub.extension_mismatch");
      break;
    case 19:
      item.cancelled = true;
      rejection(item, "pubsub.cancelled");
      break;
    case 20:
      item.cancelled = true;
      item.channel = "invalid";
      rejection(item, "pubsub.cancelled");
      break;
    case 21:
      item.cancelled = true;
      item.channel_literal = "false";
      type_error(item);
      break;
    case 22:
      item.payload_literal = "Symbol('fake')";
      type_error(item);
      break;
    case 23:
      item.payload_literal = "17n";
      type_error(item);
      break;
    case 24:
      item.payload_literal = "(()=>{const v={};v.self=v;return v})()";
      type_error(item);
      break;
    case 25:
    case 26:
    case 27: {
      // Independent compact ASCII fixture, not the production size helper.
      constexpr std::string_view EMPTY_ENVELOPE =
          "{\"channel\":\"plinth:ext:notes:chat\",\"layer\":\"extension\","
          "\"payload\":\"\"}";
      const auto bytes = DEFAULT_PAYLOAD_BYTES - EMPTY_ENVELOPE.size();
      item.payload =
          std::string(bytes - 1U + static_cast<std::size_t>(index - 25), 'a');
      if (index == 27) {
        // Fixed below/equal/above controls retain the documented ceiling.
        rejection(item, "pubsub.payload_too_large");
      }
      break;
    }
    case 28: item.rbac = (seed & 1U) != 0; break;
    case 29:
      item.channel = "plinth:ext:other:chat";
      rejection(item, "pubsub.rbac_denied");
      break;
    case 30:
      item.channel = "plinth:ext:other:chat";
      item.rules = {"other.realtime.subscribe.chat"};
      break;
    case 31:
      item.channel = "plinth:ext:other:chat";
      item.rules = {"kernel.admin"};
      item.rbac = (seed & 1U) != 0;
      break;
    case 32: item.channel = "plinth:data:ext_notes.notes"; break;
    case 33:
      item.channel = "plinth:data:ext_other.notes";
      item.rules = {"other.realtime.subscribe"};
      break;
    case 34:
      item.channel = "plinth:data:ext_other.notes";
      rejection(item, "pubsub.rbac_denied");
      break;
    case 35:
      item.channel = "plinth:system:packages.installed";
      rejection(item, "pubsub.layer_unsupported");
      break;
    case 36:
      item.channel += std::string{"\0tail", 5};
      rejection(item, "pubsub.channel_invalid");
      break;
    case 37:
      item.owner.clear();
      rejection(item, "pubsub.extension_mismatch");
      break;
    case 38:
      item.payload_literal = "123";
      type_error(item);
      break;
    case 39:
      item.argc = 1;
      type_error(item);
      break;
    case 40:
      item.channel_literal = "false";
      item.cancelled = true;
      type_error(item);
      break;
    case 41:
      item.cancelled = true;
      rejection(item, "pubsub.cancelled");
      break;
    case 42:
      item.cancelled = true;
      item.channel = "invalid";
      rejection(item, "pubsub.cancelled");
      break;
    case 43:
      item.prefill = DEFAULT_QUOTA;
      item.channel = "plinth:ext:notes:additional";
      rejection(item, "pubsub.quota_exceeded");
      break;
    case 44: item.prefill = DEFAULT_QUOTA; break;
    case 45: item.prefill = 1; break;
    case 46: item.throwing_handler = true; break;
    case 47:
      item.channel = "plinth:ext:other:chat";
      item.rules = {"other.realtime.subscribe.chat"};
      item.rbac = false;
      rejection(item, "pubsub.rbac_denied");
      break;
    default: FAIL("unknown pubsub corpus category");
  }
  return item;
}

auto check_bounds(const CorpusCase& item) -> void {
  REQUIRE(item.category >= 0);
  REQUIRE(item.category < CASES_PER_SEED);
  REQUIRE(item.channel.size() <= MAX_CHANNEL_BYTES);
  REQUIRE(item.prefill <= DEFAULT_QUOTA);
  Bounds bounds;
  measure(item.payload, 0, bounds);
  REQUIRE(bounds.nodes <= MAX_NODES);
  REQUIRE(bounds.depth <= MAX_DEPTH);
  if (item.category >= 25 && item.category <= 27) {
    REQUIRE(bounds.string_bytes <= MAX_BOUNDARY_BYTES);
  } else {
    REQUIRE(bounds.string_bytes <= MAX_STRING_BYTES);
  }
  REQUIRE(item.channel_literal.value_or("").size() <= MAX_STRING_BYTES);
  REQUIRE(item.payload_literal.value_or("").size() <= MAX_STRING_BYTES);
}

auto evaluate(BridgeContext& context, const std::string& source) -> OwnedValue {
  return {context.ctx, JS_Eval(context.ctx, source.data(), source.size(),
                               "<pubsub-corpus>", JS_EVAL_TYPE_GLOBAL)};
}

auto text_value(JSContext* context, JSValue value) -> std::string {
  std::size_t length = 0;
  const char* bytes = JS_ToCStringLen(context, &length, value);
  REQUIRE(bytes != nullptr);
  std::string result{bytes, length};
  JS_FreeCString(context, bytes);
  return result;
}

auto property_text(JSContext* context, JSValue value, const char* property)
    -> std::string {
  OwnedValue field{context, JS_GetPropertyStr(context, value, property)};
  return text_value(context, field.get());
}

auto drain_jobs(BridgeContext& context) -> bool {
  for (int index = 0; index < MAX_JOBS && JS_IsJobPending(context.rt);
       ++index) {
    JSContext* job_context = nullptr;
    if (JS_ExecutePendingJob(context.rt, &job_context) < 0) {
      return false;
    }
  }
  return !JS_IsJobPending(context.rt);
}

auto handler_source(bool throwing) -> std::string {
  return "(value)=>{newHits++;seen=JSON.stringify(value);" +
         std::string(throwing ? "throw new Error('fake-handler');" : "") + "}";
}

auto prefill(BridgeContext& context, std::size_t count) -> void {
  for (std::size_t index = 0; index < count; ++index) {
    const auto channel = index == 0
                             ? std::string{OWN_CHANNEL}
                             : "plinth:ext:notes:q" + std::to_string(index);
    auto promise =
        evaluate(context, "pubsub.subscribe(" + json_source(channel) +
                              ",()=>{oldHits++})");
    REQUIRE_FALSE(JS_IsException(promise.get()));
    REQUIRE(JS_PromiseState(context.ctx, promise.get()) == JS_PROMISE_PENDING);
    auto operations = context.take_pending_ops();
    REQUIRE(operations.size() == 1);
    const auto& operation = operations.front();
    REQUIRE(operation.type == AsyncOp::Type::PUBSUB_SUBSCRIBE);
    REQUIRE(operation.pubsub_channel == channel);
    context.resolve_with_js_value(
        operation.callback_id,
        plinth::js::make_unsubscribe_function(context.ctx, channel));
    REQUIRE(JS_PromiseState(context.ctx, promise.get()) ==
            JS_PROMISE_FULFILLED);
    OwnedValue token{context.ctx, JS_PromiseResult(context.ctx, promise.get())};
    REQUIRE(JS_IsFunction(context.ctx, token.get()));
    REQUIRE(drain_jobs(context));
  }
  REQUIRE(context.persistent_callbacks.size() == count);
  REQUIRE(context.callbacks.empty());
}

struct Operation {
  AsyncOp::Type type;
  int id = 0;
  std::string channel;
  Json::Value payload;
};

struct Observation {
  Result result = Result::other;
  std::string code;
  bool rejection_shape = true;
  int id_delta = 0;
  std::vector<int> callback_ids;
  bool callback_shape = true;
  std::vector<Operation> operations;
  std::size_t persistent = 0;
  bool caller_unchanged = true;
  bool owned_payload = true;
  bool settlement = true;
  bool handler = true;
  bool unsubscribe = true;
  bool clean = true;
};

auto oracle(const CorpusCase& item, const Observation& observed)
    -> std::optional<std::string> {
  const bool admitted = item.want == Result::pending;
  const int count = admitted ? 1 : 0;
  if (observed.result != item.want) {
    return "result";
  }
  if (observed.code != item.code) {
    return "code";
  }
  if (!observed.rejection_shape) {
    return "rejection_shape";
  }
  if (observed.id_delta != count) {
    return "id_delta";
  }
  if (observed.operations.size() != static_cast<std::size_t>(count)) {
    return "operation_count";
  }
  if (observed.callback_ids !=
      (admitted ? std::vector<int>{INITIAL_ID} : std::vector<int>{})) {
    return "callback_ids";
  }
  if (!observed.callback_shape) {
    return "callback_shape";
  }
  if (admitted) {
    const auto& operation = observed.operations.front();
    const auto type = item.subscribe ? AsyncOp::Type::PUBSUB_SUBSCRIBE
                                     : AsyncOp::Type::PUBSUB_PUBLISH;
    if (operation.type != type) {
      return "operation_type";
    }
    if (operation.id != INITIAL_ID) {
      return "operation_id";
    }
    if (operation.channel != item.channel) {
      return "channel";
    }
    if (!item.subscribe && operation.payload != item.payload) {
      return "payload";
    }
  }
  const std::size_t subscriptions =
      item.prefill +
      ((admitted && item.subscribe && item.prefill == 0) ? 1U : 0U);
  if (observed.persistent != subscriptions) {
    return "persistent";
  }
  if (!observed.caller_unchanged) {
    return "caller";
  }
  if (!observed.owned_payload) {
    return "owned_payload";
  }
  if (!observed.settlement) {
    return "settlement";
  }
  if (!observed.handler) {
    return "handler";
  }
  if (!observed.unsubscribe) {
    return "unsubscribe";
  }
  if (!observed.clean) {
    return "cleanup";
  }
  return std::nullopt;
}

auto execute(BridgeContext& context, const CorpusCase& item) -> Observation {
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  REQUIRE(context.pending_ops.empty());
  REQUIRE(context.callbacks.empty());
  REQUIRE(context.persistent_callbacks.empty());
  REQUIRE_FALSE(JS_HasException(context.ctx));
  REQUIRE_FALSE(JS_IsJobPending(context.rt));
  plinth::realtime::broker::set_rbac_enforce_for_test(item.rbac);
  context.extension_name = item.owner;
  context.user = fake_user();
  context.user.effective_rules = item.rules;
  const auto caller = context.user;
  const auto original_depth = context.call_depth;
  auto setup = evaluate(context, "globalThis.oldHits=0;globalThis.newHits=0;"
                                 "globalThis.seen='none';true");
  REQUIRE_FALSE(JS_IsException(setup.get()));
  prefill(context, item.prefill);
  context.next_callback_id = INITIAL_ID;
  auto channel = evaluate(
      context, item.channel_literal.value_or(json_source(item.channel)));
  const auto input =
      item.subscribe
          ? item.payload_literal.value_or(handler_source(item.throwing_handler))
          : item.payload_literal.value_or(json_source(item.payload));
  auto argument = evaluate(context, "globalThis.input=(" + input + ");input");
  REQUIRE_FALSE(JS_IsException(channel.get()));
  REQUIRE_FALSE(JS_IsException(argument.get()));
  OwnedValue global{context.ctx, JS_GetGlobalObject(context.ctx)};
  OwnedValue pubsub{context.ctx,
                    JS_GetPropertyStr(context.ctx, global.get(), "pubsub")};
  OwnedValue function{
      context.ctx, JS_GetPropertyStr(context.ctx, pubsub.get(),
                                     item.subscribe ? "subscribe" : "publish")};
  REQUIRE(JS_IsFunction(context.ctx, function.get()));
  std::array<JSValue, 2> arguments{channel.get(), argument.get()};
  // All JS setup precedes cancellation; call the actual registered C binding
  // directly so the interrupt handler cannot preempt the admission observation.
  context.cancelled.store(item.cancelled);
  OwnedValue promise{context.ctx,
                     JS_Call(context.ctx, function.get(), JS_UNDEFINED,
                             item.argc, arguments.data())};
  Observation observed;
  if (JS_IsException(promise.get())) {
    observed.result = Result::exception;
    OwnedValue error{context.ctx, JS_GetException(context.ctx)};
    observed.code = property_text(context.ctx, error.get(), "name");
  } else {
    const auto state = JS_PromiseState(context.ctx, promise.get());
    observed.result = state == JS_PROMISE_PENDING    ? Result::pending
                      : state == JS_PROMISE_REJECTED ? Result::rejected
                                                     : Result::other;
    if (state == JS_PROMISE_REJECTED) {
      OwnedValue error{context.ctx,
                       JS_PromiseResult(context.ctx, promise.get())};
      observed.code = property_text(context.ctx, error.get(), "code");
      OwnedValue message{
          context.ctx, JS_GetPropertyStr(context.ctx, error.get(), "message")};
      OwnedValue sqlstate{
          context.ctx, JS_GetPropertyStr(context.ctx, error.get(), "sqlstate")};
      observed.rejection_shape =
          JS_IsString(message.get()) && JS_IsUndefined(sqlstate.get());
    }
  }
  // Explicit simulated completion below is separate from cancellation-driver
  // semantics, and must not relabel the inline result observed above.
  context.cancelled.store(false);
  observed.id_delta = context.next_callback_id - INITIAL_ID;
  observed.persistent = context.persistent_callbacks.size();
  for (const auto& [id, callbacks] : context.callbacks) {
    observed.callback_ids.push_back(id);
    observed.callback_shape = observed.callback_shape &&
                              callbacks.ns_for_cancellation == "pubsub" &&
                              JS_IsFunction(context.ctx, callbacks.resolve) &&
                              JS_IsFunction(context.ctx, callbacks.reject);
  }
  std::ranges::sort(observed.callback_ids);
  observed.caller_unchanged =
      context.extension_name == item.owner &&
      context.call_depth == original_depth &&
      context.user.user_id == caller.user_id &&
      context.user.username == caller.username &&
      context.user.auth_type == caller.auth_type &&
      context.user.session_id == caller.session_id &&
      context.user.ip_address == caller.ip_address &&
      context.user.effective_rules == caller.effective_rules;
  auto operations = context.take_pending_ops();
  for (const auto& operation : operations) {
    observed.operations.push_back({operation.type, operation.callback_id,
                                   operation.pubsub_channel,
                                   operation.pubsub_payload});
  }
  if (observed.result == Result::pending && !item.subscribe &&
      item.payload.isObject()) {
    auto mutation =
        evaluate(context, "input.marker=-999;input.extra='changed';true");
    observed.owned_payload = !JS_IsException(mutation.get());
    for (const auto& operation : operations) {
      observed.owned_payload =
          observed.owned_payload && operation.pubsub_payload == item.payload;
    }
  }
  for (const int id : observed.callback_ids) {
    if (item.simulate_rejection) {
      context.reject(id, {.code = "pubsub.test_settlement",
                          .message = "simulated outcome",
                          .sqlstate = std::nullopt});
    } else if (item.subscribe) {
      context.resolve_with_js_value(
          id, plinth::js::make_unsubscribe_function(context.ctx, item.channel));
    } else {
      context.resolve_with_js_value(id, JS_UNDEFINED);
    }
  }
  if (observed.result == Result::pending) {
    const auto state = JS_PromiseState(context.ctx, promise.get());
    OwnedValue value{context.ctx, JS_PromiseResult(context.ctx, promise.get())};
    if (item.simulate_rejection) {
      observed.settlement = state == JS_PROMISE_REJECTED &&
                            property_text(context.ctx, value.get(), "code") ==
                                "pubsub.test_settlement" &&
                            property_text(context.ctx, value.get(),
                                          "message") == "simulated outcome";
    } else if (item.subscribe) {
      observed.settlement = state == JS_PROMISE_FULFILLED &&
                            JS_IsFunction(context.ctx, value.get());
      Json::Value envelope{Json::objectValue};
      envelope["channel"] = item.channel;
      envelope["payload"]["marker"] = 37;
      envelope["payload"]["text"] = std::string{"x\0\xC3\xA9", 4};
      context.invoke_callback(item.channel, envelope);
      context.invoke_callback("plinth:ext:notes:absent", envelope);
      auto invocation =
          evaluate(context, "newHits===1&&oldHits===0&&seen===JSON.stringify(" +
                                json_source(envelope) + ")");
      observed.handler = !JS_IsException(invocation.get()) &&
                         JS_ToBool(context.ctx, invocation.get()) == 1 &&
                         !JS_HasException(context.ctx);
      const auto persistent_before = context.persistent_callbacks.size();
      const int unsubscribe_id = context.next_callback_id;
      OwnedValue unsubscribe_promise{
          context.ctx,
          JS_Call(context.ctx, value.get(), JS_UNDEFINED, 0, nullptr)};
      auto unsub_ops = context.take_pending_ops();
      observed.unsubscribe =
          !JS_IsException(unsubscribe_promise.get()) &&
          JS_PromiseState(context.ctx, unsubscribe_promise.get()) ==
              JS_PROMISE_PENDING &&
          unsub_ops.size() == 1 && context.callbacks.size() == 1 &&
          context.persistent_callbacks.size() == persistent_before &&
          context.next_callback_id == unsubscribe_id + 1;
      if (unsub_ops.size() == 1) {
        const auto& operation = unsub_ops.front();
        observed.unsubscribe =
            observed.unsubscribe &&
            operation.type == AsyncOp::Type::PUBSUB_UNSUBSCRIBE &&
            operation.callback_id == unsubscribe_id &&
            operation.pubsub_channel == item.channel;
      }
      auto callback = context.callbacks.find(unsubscribe_id);
      observed.unsubscribe =
          observed.unsubscribe && callback != context.callbacks.end();
      if (callback != context.callbacks.end()) {
        observed.unsubscribe =
            observed.unsubscribe &&
            callback->second.ns_for_cancellation == "pubsub" &&
            JS_IsFunction(context.ctx, callback->second.resolve) &&
            JS_IsFunction(context.ctx, callback->second.reject);
      }
      // Simulate the dispatcher's persistent-value ownership cleanup only.
      auto handler = context.persistent_callbacks.find(item.channel);
      if (handler != context.persistent_callbacks.end()) {
        JS_FreeValue(context.ctx, handler->second);
        context.persistent_callbacks.erase(handler);
      }
      // Match the existing dispatcher's host settlement seam, without running
      // broker deregistration or claiming its end-to-end return contract.
      context.resolve(unsubscribe_id, Json::Value{});
      observed.unsubscribe =
          observed.unsubscribe &&
          JS_PromiseState(context.ctx, unsubscribe_promise.get()) ==
              JS_PROMISE_FULFILLED;
      OwnedValue unsub_result{
          context.ctx,
          JS_PromiseResult(context.ctx, unsubscribe_promise.get())};
      observed.unsubscribe =
          observed.unsubscribe && JS_IsNull(unsub_result.get());
      context.invoke_callback(item.channel, envelope);
      auto after = evaluate(context, "newHits===1&&oldHits===0");
      observed.unsubscribe = observed.unsubscribe &&
                             !JS_IsException(after.get()) &&
                             JS_ToBool(context.ctx, after.get()) == 1;
    } else {
      observed.settlement =
          state == JS_PROMISE_FULFILLED && JS_IsUndefined(value.get());
    }
  }
  context.drop_persistent_callbacks();
  observed.clean = drain_jobs(context) && context.callbacks.empty() &&
                   context.pending_ops.empty() &&
                   context.persistent_callbacks.empty() &&
                   context.concurrent_async_ops == 0 &&
                   context.inflight_detached.load() == 0 &&
                   !JS_HasException(context.ctx) && !context.waiter_handle &&
                   plinth::realtime::broker::js_subscriber_count() == 0;
  if (JS_HasException(context.ctx)) {
    OwnedValue exception{context.ctx, JS_GetException(context.ctx)};
  }
  return observed; // Every JSValue owner dies before lease release/destruction.
}

enum class Fault { none, drop_operation };

auto fresh(const CorpusCase& item, Fault fault = Fault::none) -> Observation {
  GlobalGuard global_guard;
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, item.owner);
  Observation result;
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    result = execute(*lease.get(), item);
    if (fault == Fault::drop_operation && !result.operations.empty()) {
      result.operations.pop_back();
    }
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);
  return result;
}

struct ShrinkResult {
  CorpusCase minimal;
  int attempts = 0;
  bool reproduced = false;
};

auto shrink(CorpusCase item, std::string_view signature, Fault fault)
    -> ShrinkResult {
  if (oracle(item, fresh(item, fault)) != signature) {
    return {std::move(item), 0, false};
  }
  int attempts = 0;
  // Only ordinary object noise can shrink without changing a semantic family.
  // Fixed threshold/invalid/type/cancellation regressions stay fixed inputs.
  while (item.category == 0 && item.payload["noise"].isString() &&
         !item.payload["noise"].asString().empty() &&
         attempts < MAX_SHRINK_ATTEMPTS) {
    auto candidate = item;
    const auto noise = item.payload["noise"].asString();
    candidate.payload["noise"] = noise.substr(0, noise.size() / 2);
    ++attempts;
    REQUIRE(candidate.category == item.category);
    check_bounds(candidate);
    if (oracle(candidate, fresh(candidate, fault)) != signature) {
      break;
    }
    item = std::move(candidate);
  }
  REQUIRE(oracle(item, fresh(item, fault)) == signature);
  return {std::move(item), attempts, true};
}

auto assert_observation(const CorpusCase& item, const Observation& observed)
    -> void {
  check_bounds(item);
  if (const auto failure = oracle(item, observed); failure.has_value()) {
    const auto reduced = shrink(item, *failure, Fault::none);
    INFO("category=" << item.category << " signature=" << *failure
                     << " shrink_attempts=" << reduced.attempts
                     << " fresh_reproduced=" << reduced.reproduced);
    FAIL("pubsub corpus contract mismatch");
  }
}

auto negative_controls() -> void {
  const auto publish = generated_case(SEEDS.front(), 0);
  const auto admitted = fresh(publish);
  REQUIRE_FALSE(oracle(publish, admitted).has_value());
  for (int index = 0; index < 14; ++index) {
    INFO("oracle_negative_control=" << index);
    auto corrupt = admitted;
    switch (index) {
      case 0: corrupt.result = Result::other; break;
      case 1: corrupt.code = "wrong"; break;
      case 2: corrupt.operations.clear(); break;
      case 3: corrupt.operations.push_back(corrupt.operations.front()); break;
      case 4: ++corrupt.operations.front().id; break;
      case 5:
        corrupt.operations.front().type = AsyncOp::Type::AUDIT_WRITE;
        break;
      case 6: corrupt.operations.front().channel = "wrong"; break;
      case 7: corrupt.operations.front().payload = Json::Value{}; break;
      case 8: corrupt.callback_ids.clear(); break;
      case 9: corrupt.callback_shape = false; break;
      case 10: corrupt.caller_unchanged = false; break;
      case 11: corrupt.owned_payload = false; break;
      case 12: corrupt.settlement = false; break;
      case 13: corrupt.clean = false; break;
      default: FAIL("unknown oracle control");
    }
    REQUIRE(oracle(publish, corrupt).has_value());
  }
  const auto subscribe = generated_case(SEEDS.front(), 44);
  const auto replacement = fresh(subscribe);
  REQUIRE_FALSE(oracle(subscribe, replacement).has_value());
  for (int index = 0; index < 4; ++index) {
    auto corrupt = replacement;
    switch (index) {
      case 0: corrupt.handler = false; break;
      case 1: corrupt.unsubscribe = false; break;
      case 2: ++corrupt.persistent; break;
      case 3: ++corrupt.id_delta; break;
      default: FAIL("unknown subscription control");
    }
    REQUIRE(oracle(subscribe, corrupt).has_value());
  }
  const auto denied = generated_case(SEEDS.front(), 29);
  auto reason = fresh(denied);
  REQUIRE_FALSE(oracle(denied, reason).has_value());
  reason.rejection_shape = false;
  REQUIRE(oracle(denied, reason) == "rejection_shape");
  const auto broken = oracle(publish, fresh(publish, Fault::drop_operation));
  REQUIRE(broken == "operation_count");
  const auto reduced = shrink(publish, *broken, Fault::drop_operation);
  REQUIRE(reduced.reproduced);
  REQUIRE(reduced.attempts > 0);
  REQUIRE(reduced.attempts <= MAX_SHRINK_ATTEMPTS);
  REQUIRE(reduced.minimal.category == publish.category);
  REQUIRE(reduced.minimal.payload["noise"].asString().empty());
  REQUIRE_FALSE(oracle(reduced.minimal, fresh(reduced.minimal)).has_value());
}

auto fixed_regressions() -> void {
  std::array<CorpusCase, FIXED_REGRESSIONS> items;
  items.at(0) = generated_case(SEEDS.front(), 22);
  items.at(0).cancelled = true;
  items.at(0).channel = "invalid";
  // Conversion/type validation precedes cancellation and channel validation.
  items.at(1) = generated_case(SEEDS.front(), 15);
  items.at(1).owner.clear();
  // A disallowed publishing layer precedes the absent-extension identity gate.
  items.at(2) = generated_case(SEEDS.front(), 43);
  items.at(2).channel = "invalid";
  rejection(items.at(2), "pubsub.channel_invalid");
  items.at(3) = generated_case(SEEDS.front(), 43);
  items.at(3).channel = "plinth:ext:other:chat";
  rejection(items.at(3), "pubsub.rbac_denied");
  // At full quota, malformed/cross-extension denial retains earlier precedence.
  constexpr std::string_view PREFIX = "plinth:ext:notes:";
  for (std::size_t index = 4; index < FIXED_REGRESSIONS; ++index) {
    const bool subscribe = index >= 6;
    const bool too_long = index % 2 != 0;
    items.at(index) = generated_case(SEEDS.front(), subscribe ? 28 : 0);
    const std::size_t channel_bytes = too_long ? 64U : 63U;
    items.at(index).channel =
        std::string{PREFIX} + std::string(channel_bytes - PREFIX.size(), 'a');
    REQUIRE(items.at(index).channel.size() == channel_bytes);
    if (too_long) {
      rejection(items.at(index), "pubsub.channel_invalid");
    }
  }
  std::array<bool, FIXED_REGRESSIONS> covered{};
  for (std::size_t index = 0; index < items.size(); ++index) {
    INFO("fixed_regression=" << index);
    check_bounds(items.at(index));
    assert_observation(items.at(index), fresh(items.at(index)));
    covered.at(index) = true;
  }
  REQUIRE(std::ranges::all_of(covered, [](bool ran) { return ran; }));
}

auto settled_release_controls() -> void {
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, "notes");
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    {
      auto setup = evaluate(
          *lease.get(), "globalThis.oldHits=0;globalThis.releaseMarker=111");
      REQUIRE_FALSE(JS_IsException(setup.get()));
    }
    prefill(*lease.get(), 1);
    REQUIRE(lease.get()->callbacks.empty());
    REQUIRE(lease.get()->pending_ops.empty());
    REQUIRE(lease.get()->persistent_callbacks.size() == 1);
    REQUIRE(lease.get()->next_callback_id == 1);
    {
      Json::Value payload{Json::objectValue};
      payload["marker"] = 37;
      lease.get()->invoke_callback(std::string{OWN_CHANNEL}, payload);
      auto invoked = evaluate(*lease.get(), "oldHits===1&&releaseMarker===111");
      REQUIRE_FALSE(JS_IsException(invoked.get()));
      REQUIRE(JS_ToBool(lease.get()->ctx, invoked.get()) == 1);
      // ID zero was settled by prefill. Late completions on the live context
      // must leave all inventories unchanged; this is not a cancellation run.
      lease.get()->resolve(0, payload);
      lease.get()->reject(0, {.code = "pubsub.test_late",
                              .message = "simulated late outcome",
                              .sqlstate = std::nullopt});
      OwnedValue late_value{lease.get()->ctx,
                            JS_NewString(lease.get()->ctx, "fake-late-value")};
      lease.get()->resolve_with_js_value(
          0, JS_DupValue(lease.get()->ctx, late_value.get()));
      REQUIRE(text_value(lease.get()->ctx, late_value.get()) ==
              "fake-late-value");
      REQUIRE(lease.get()->callbacks.empty());
      REQUIRE(lease.get()->pending_ops.empty());
      REQUIRE(lease.get()->persistent_callbacks.size() == 1);
      REQUIRE(lease.get()->next_callback_id == 1);
      REQUIRE(lease.get()->concurrent_async_ops == 0);
      REQUIRE(lease.get()->inflight_detached.load() == 0);
      REQUIRE_FALSE(JS_HasException(lease.get()->ctx));
      REQUIRE_FALSE(JS_IsJobPending(lease.get()->rt));
    }
    // No external JSValue owner survives this normal release. The settled
    // persistent handler deliberately remains owned by the context until reset.
    lease.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  {
    Lease reused{pool};
    REQUIRE(reused.get() != nullptr);
    REQUIRE(reused.get()->callbacks.empty());
    REQUIRE(reused.get()->pending_ops.empty());
    REQUIRE(reused.get()->persistent_callbacks.empty());
    REQUIRE(reused.get()->next_callback_id == 0);
    REQUIRE_FALSE(JS_HasException(reused.get()->ctx));
    REQUIRE_FALSE(JS_IsJobPending(reused.get()->rt));
    {
      auto reset =
          evaluate(*reused.get(), "typeof oldHits==='undefined'&&"
                                  "typeof releaseMarker==='undefined'&&"
                                  "typeof pubsub.subscribe==='function'");
      REQUIRE_FALSE(JS_IsException(reset.get()));
      REQUIRE(JS_ToBool(reused.get()->ctx, reset.get()) == 1);
    }
    reused.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

auto abandonment_controls() -> void {
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, "notes");
  {
    Lease dirty{pool};
    REQUIRE(dirty.get() != nullptr);
    {
      auto promise = evaluate(
          *dirty.get(), "pubsub.subscribe('plinth:ext:notes:chat',()=>{})");
      REQUIRE(JS_PromiseState(dirty.get()->ctx, promise.get()) ==
              JS_PROMISE_PENDING);
      REQUIRE(dirty.get()->callbacks.size() == 1);
      REQUIRE(dirty.get()->pending_ops.size() == 1);
      REQUIRE(dirty.get()->persistent_callbacks.size() == 1);
    }
    dirty.release(); // Existing dirty-state policy destroys, never re-pools.
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 0);
  {
    Lease fresh_lease{pool};
    REQUIRE(fresh_lease.get() != nullptr);
    REQUIRE(fresh_lease.get()->callbacks.empty());
    REQUIRE(fresh_lease.get()->pending_ops.empty());
    REQUIRE(fresh_lease.get()->persistent_callbacks.empty());
    REQUIRE_FALSE(JS_HasException(fresh_lease.get()->ctx));
    fresh_lease.get()->cancelled.store(true);
    fresh_lease.release(); // Cancelled contexts also use defensive destruction.
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 0);
  pool.rebuild();
  REQUIRE(pool.free_count() == 1);
  {
    Lease held{pool};
    REQUIRE(held.get() != nullptr);
    {
      auto marker = evaluate(*held.get(), "globalThis.rebuildMarker=321");
      REQUIRE_FALSE(JS_IsException(marker.get()));
    }
    pool.rebuild();
    REQUIRE(pool.active_count() == 1);
    {
      auto retained = evaluate(*held.get(), "rebuildMarker===321");
      REQUIRE_FALSE(JS_IsException(retained.get()));
      REQUIRE(JS_ToBool(held.get()->ctx, retained.get()) == 1);
    }
    REQUIRE_FALSE(pool.shutdown(100ms));
    REQUIRE(pool.active_count() == 1);
    REQUIRE(pool.acquire() == nullptr);
    {
      auto alive =
          evaluate(*held.get(), "typeof pubsub.subscribe==='function'");
      REQUIRE_FALSE(JS_IsException(alive.get()));
      REQUIRE(JS_ToBool(held.get()->ctx, alive.get()) == 1);
    }
    held.destroy();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  REQUIRE(pool.acquire() == nullptr);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

} // namespace

TEST_CASE(
    "QuickJS pubsub has a bounded deterministic admission and callback corpus",
    "[js][pubsub][corpus][isolated-pubsub]") {
  INFO("seeds=" << SEEDS.size() << " cases_per_seed=" << CASES_PER_SEED
                << " max_depth=" << MAX_DEPTH << " max_nodes=" << MAX_NODES
                << " max_string_bytes=" << MAX_STRING_BYTES
                << " max_boundary_bytes=" << MAX_BOUNDARY_BYTES
                << " fixed_regressions=" << FIXED_REGRESSIONS
                << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS);
  GlobalGuard global_guard;
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);
  REQUIRE(plinth::realtime::broker::max_subscriptions_per_conn() ==
          DEFAULT_QUOTA);
  REQUIRE(plinth::realtime::get_max_payload_bytes() == DEFAULT_PAYLOAD_BYTES);
  plinth::realtime::broker::reset_audit_windows_for_test();
  negative_controls();
  fixed_regressions();
  settled_release_controls();
  for (const auto seed : SEEDS) {
    INFO("seed=" << seed);
    plinth::Config config{};
    const auto user = fake_user();
    RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                     &user, "notes");
    std::array<int, CASES_PER_SEED> coverage{};
    int admitted = 0;
    int rejected = 0;
    int exceptions = 0;
    for (int index = 0; index < CASES_PER_SEED; ++index) {
      INFO("case=" << index);
      const auto item = generated_case(seed, index);
      check_bounds(item);
      {
        Lease lease{pool};
        REQUIRE(lease.get() != nullptr);
        const auto observed = execute(*lease.get(), item);
        assert_observation(item, observed);
        if (index % 3 == 0) {
          lease.destroy();
        } else {
          lease.release();
        }
      }
      assert_observation(item, fresh(item));
      ++coverage.at(static_cast<std::size_t>(item.category));
      admitted += item.want == Result::pending ? 1 : 0;
      rejected += item.want == Result::rejected ? 1 : 0;
      exceptions += item.want == Result::exception ? 1 : 0;
      REQUIRE(pool.active_count() == 0);
      if (index % 8 == 7) {
        pool.rebuild();
      }
    }
    REQUIRE(
        std::ranges::all_of(coverage, [](int count) { return count == 1; }));
    REQUIRE(admitted > 0);
    REQUIRE(rejected > 0);
    REQUIRE(exceptions > 0);
    REQUIRE(admitted + rejected + exceptions == CASES_PER_SEED);
    REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
    REQUIRE(pool.acquire() == nullptr);
  }
  abandonment_controls();
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  REQUIRE(plinth::realtime::broker::js_subscriber_count() == 0);
}
