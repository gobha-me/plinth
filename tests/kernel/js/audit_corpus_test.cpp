// SPDX-License-Identifier: MIT
//
// This DB-free corpus checks the shipped binding and its owned enqueue state.
// Expected JSON is generated here, not through production conversion helpers.
// Explicit settlements below simulate callbacks, not writer/PG acknowledgement.
// Generator bounds are test budgets, not production API limits.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/runtime_pool.hpp"

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
    0x1330'0000'0000'0001ULL, 0x1330'0000'0000'0027ULL,
    0x1330'0000'0000'00C6ULL, 0x1330'0000'0000'BEEFULL};
constexpr int CASES_PER_SEED = 48;
constexpr std::size_t MAX_EVENT_BYTES = 256;
constexpr int MAX_DEPTH = 3; // generated payload root is depth zero
constexpr int MAX_NODES = 15;
constexpr std::size_t MAX_STRING_BYTES = 64;
constexpr int MAX_SHRINK_ATTEMPTS = 16;
constexpr int MAX_JOBS = 64;
constexpr int INITIAL_ID = 17;
constexpr auto SHUTDOWN_BOUND = 2s;
// Independent ICD lists; do not import the binding's validation tables.
constexpr std::array<std::string_view, 6> NAMESPACES{
    "user.", "session.", "pat.", "group.", "rbac.", "capability."};
constexpr std::array<std::string_view, 7> ROOT_KEYS{
    "user_id", "session_id", "ip_address", "extension_id",
    "node_id", "call_depth", "timestamp"};

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
  explicit Lease(plinth::js::RuntimePool& owner)
      : pool(owner), context(pool.acquire()) {}
  ~Lease() { destroy(); }
  Lease(const Lease&) = delete;
  auto operator=(const Lease&) -> Lease& = delete;
  Lease(Lease&&) = delete;
  auto operator=(Lease&&) -> Lease& = delete;
  auto get() const -> plinth::js::BridgeContext* { return context; }
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
  plinth::js::RuntimePool& pool;
  plinth::js::BridgeContext* context;
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

auto json_source(const Json::Value& value) -> std::string {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  writer["emitUTF8"] = true;
  return Json::writeString(writer, value);
}

auto generated_string(std::uint64_t& state) -> std::string {
  constexpr std::array<std::string_view, 5> ATOMS{
      "a", std::string_view{"\0", 1}, "\xC3\xA9", "\xE4\xB8\xAD",
      "\xF0\x9F\x98\x80"};
  std::string result;
  const auto target = static_cast<std::size_t>(next(state) % 65U);
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
  const auto choice = next(state) % (depth >= MAX_DEPTH ? 5U : 7U);
  switch (choice) {
    case 0: return Json::Value{};
    case 1: return Json::Value{true};
    case 2: return Json::Value{false};
    case 3: return Json::Value{static_cast<int>(next(state) % 201U) - 100};
    case 4: return Json::Value{generated_string(state)};
    default: {
      const bool array = choice == 5;
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
  int category = 0; // indexed semantic family, preserved during shrinking
  int argc = 2;
  std::string owner;
  int call_depth = 0;
  std::string event;
  std::string event_floor;
  std::optional<std::string> event_literal;
  std::optional<std::string> payload_literal;
  Json::Value payload{Json::objectValue};
  bool cancelled = false;
  bool simulate_rejection = false;
  Result want = Result::pending;
  std::string code;
  std::string message;
  int namespace_slot = -1;
  int root_slot = -1;
};

auto owner_name(const CorpusCase& item) -> std::string {
  return item.owner.empty() ? "host" : item.owner;
}

auto fake_user() -> plinth::capabilities::UserContext {
  return {.user_id = "fake-user",
          .username = "fake-caller",
          .auth_type = "session",
          .effective_rules = {},
          .session_id = "fake-session",
          .ip_address = "192.0.2.1"};
}

auto type_error(CorpusCase& item, std::string message) -> void {
  item.want = Result::exception;
  item.code = "TypeError";
  item.message = "audit.log: " + std::move(message);
}

auto rejection(CorpusCase& item, std::string code, std::string message)
    -> void {
  item.want = Result::rejected;
  item.code = std::move(code);
  item.message = std::move(message);
}

auto generated_case(std::uint64_t seed, int index) -> CorpusCase {
  auto state = seed ^ (static_cast<std::uint64_t>(index) << 32U);
  CorpusCase item;
  item.category = index;
  constexpr std::array<std::string_view, 4> OWNERS{"demo", "", "widget_2",
                                                   "demo"};
  item.owner = OWNERS.at(static_cast<std::size_t>(seed % 4U));
  item.call_depth = static_cast<int>(next(state) % 8U);
  item.simulate_rejection = (next(state) & 1U) != 0;
  item.event_floor = "ext." + owner_name(item) + ".";
  item.event = item.event_floor + "case" + std::to_string(next(state) % 1000U);
  item.payload["marker"] = static_cast<int>(next(state) % 101U);
  item.payload["nested"]["note"] = generated_string(state);
  int remaining = 7; // reserve room for root, marker, nested, note and gates
  item.payload["nested"]["tree"] = tree(state, 2, remaining);
  constexpr std::string_view ARGUMENTS =
      "expected (event_type: string, payload: object)";
  if (index == 0 || index == 1) {
    item.argc = index;
    type_error(item, std::string{ARGUMENTS});
  } else if (index == 2 || index == 43) {
    constexpr std::array<std::string_view, 6> WRONG{
        "null", "true", "42", "({})", "[]", "undefined"};
    item.event_literal = WRONG.at(static_cast<std::size_t>(next(state) % 6U));
    item.cancelled = index == 43;
    type_error(item, "event_type must be a string");
  } else if (index == 3 || index == 45) {
    item.event.clear();
    item.event_floor.clear();
    item.cancelled = index == 45;
    type_error(item, "event_type must be non-empty");
  } else if ((index >= 4 && index <= 9) || index == 44) {
    constexpr std::array<std::string_view, 6> WRONG{
        "undefined", "null", "true", "42", "'payload'", "[]"};
    const auto slot = index == 44 ? 1 : index - 4;
    item.payload_literal = WRONG.at(static_cast<std::size_t>(slot));
    item.cancelled = index == 44;
    type_error(item, "payload must be a plain object");
  } else if ((index >= 10 && index <= 15) || index == 46) {
    const auto slot =
        index == 46 ? static_cast<int>(next(state) % 6U) : index - 10;
    item.namespace_slot = slot;
    item.event_floor = NAMESPACES.at(static_cast<std::size_t>(slot));
    item.event = item.event_floor + "event";
    item.payload["user_id"] = "fake-user";
    if (index == 46) {
      item.event.push_back('\0');
      item.event_floor = item.event;
    }
    rejection(item, "audit.reserved_prefix",
              "event_type uses kernel-reserved prefix: " +
                  std::string{NAMESPACES.at(static_cast<std::size_t>(slot))});
  } else if (index == 16 || index == 20 || index == 21) {
    item.event = index == 16 ? "Ext.demo.event" : "ext.demo.event";
    if (index == 20 || index == 21) {
      item.event.insert(static_cast<std::size_t>(index == 20 ? 0 : 2), 1, '\0');
    }
    item.event_floor = item.event;
    rejection(item, "audit.invalid_prefix",
              "extension audit events must start with 'ext.<id>.'");
  } else if (index >= 17 && index <= 19) {
    item.event = index == 17   ? "ext.sibling.event"
                 : index == 18 ? "ext." + owner_name(item)
                               : "ext." + owner_name(item) + "X.event";
    item.event_floor = item.event;
    rejection(item, "audit.invalid_prefix",
              "extension audit events must start with 'ext." +
                  owner_name(item) + ".'");
  } else if (index == 22 || index == 23) {
    item.event = "ext." + owner_name(item) + ".event";
    item.event.insert(index == 22 ? 4U : item.event.size(), 1, '\0');
    item.event_floor = item.event;
    rejection(item, "audit.invalid_prefix",
              "extension audit event_type must not contain NUL");
  } else if (index >= 24 && index <= 30) {
    item.root_slot = index - 24;
    const auto key = ROOT_KEYS.at(static_cast<std::size_t>(item.root_slot));
    item.payload[std::string{key}] = (next(state) & 1U) != 0
                                         ? Json::Value{}
                                         : Json::Value{"matching-or-forged"};
    rejection(item, "audit.reserved_field",
              "payload contains non-forgeable field: " + std::string{key});
  } else if (index >= 31 && index <= 37) {
    const auto key = ROOT_KEYS.at(static_cast<std::size_t>(index - 31));
    item.payload[std::string{key} + "_"] = "near-miss";
    item.payload["nested"][std::string{key}] = "application-detail";
  } else if (index == 38) {
    item.event = item.event_floor; // no new suffix grammar
  } else if (index == 39) {
    item.event_floor += "\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
    item.event = item.event_floor + "event";
  } else if (index >= 40 && index <= 42) {
    item.cancelled = true;
    item.payload["extension_id"] = owner_name(item);
    if (index == 41) {
      item.event = "user.login";
    } else if (index == 42) {
      item.event = "malformed.event";
    }
    item.event_floor = item.event;
    rejection(item, "audit.cancelled", "execution cancelled");
  } else if (index == 47) {
    item.event.resize(MAX_EVENT_BYTES, 'x');
    item.payload["nested"]["note"] = std::string(MAX_STRING_BYTES, 'n');
  }
  return item;
}

auto expression(const CorpusCase& item) -> std::string {
  if (item.argc == 0) {
    return "audit.log()";
  }
  const auto event = item.event_literal.value_or(json_source(item.event));
  if (item.argc == 1) {
    return "audit.log(" + event + ')';
  }
  return "audit.log(" + event + ",input)";
}

auto check_bounds(const CorpusCase& item) -> void {
  Bounds bounds;
  measure(item.payload, 0, bounds);
  REQUIRE(item.event.size() <= MAX_EVENT_BYTES);
  REQUIRE(bounds.depth <= MAX_DEPTH);
  REQUIRE(bounds.nodes <= MAX_NODES);
  REQUIRE(bounds.string_bytes <= MAX_STRING_BYTES);
}

struct Operation {
  plinth::js::AsyncOp::Type type = plinth::js::AsyncOp::Type::AUDIT_WRITE;
  int id = INITIAL_ID;
  std::string event;
  Json::Value detail;
  std::string user;
  std::string session;
  std::string ip;
};

struct Observation {
  Result result = Result::other;
  std::string code;
  std::string message;
  std::string owner_after_call;
  std::vector<Operation> operations;
  std::vector<int> callback_ids;
  bool callback_shape = true;
  int id_delta = 0;
  int concurrent = 0;
  bool mutation_ok = true;
  bool settlement_ok = true;
  bool jobs_drained = true;
  bool final_pending = false;
  bool final_callbacks = false;
  bool final_exception = false;
  int final_id_delta = 0;
  int final_concurrent = 0;
  int final_inflight = 0;
};

auto enriched(const CorpusCase& item) -> Json::Value {
  auto result = item.payload;
  result["extension_id"] = owner_name(item);
  result["call_depth"] = item.call_depth;
  return result;
}

struct Failure {
  std::string signature;
  std::string message;
};

auto oracle(const CorpusCase& item, const Observation& observed)
    -> std::optional<Failure> {
  const auto fail = [&item](std::string field) -> std::optional<Failure> {
    return Failure{std::to_string(item.category) + ':' + field,
                   "audit corpus mismatch: " + std::move(field)};
  };
  if (observed.result != item.want) {
    return fail("result_kind");
  }
  if (observed.code != item.code || observed.message != item.message) {
    return fail("error_envelope");
  }
  if (observed.owner_after_call != item.owner) {
    return fail("context_owner");
  }
  const int admitted = item.want == Result::pending ? 1 : 0;
  if (observed.operations.size() != static_cast<std::size_t>(admitted)) {
    return fail("operation_delta");
  }
  if (observed.callback_ids.size() != static_cast<std::size_t>(admitted)) {
    return fail("callback_delta");
  }
  if (observed.id_delta != admitted || observed.concurrent != 0) {
    return fail("admission_counters");
  }
  if (!observed.callback_shape) {
    return fail("callback_shape");
  }
  if (admitted != 0) {
    const auto& operation = observed.operations.front();
    if (operation.type != plinth::js::AsyncOp::Type::AUDIT_WRITE ||
        operation.id != INITIAL_ID ||
        observed.callback_ids.front() != INITIAL_ID) {
      return fail("operation_identity");
    }
    if (operation.event != item.event) {
      return fail("event_snapshot");
    }
    if (operation.detail != enriched(item)) {
      return fail("owned_detail");
    }
    const auto user = fake_user();
    if (operation.user != user.user_id ||
        operation.session != user.session_id ||
        operation.ip != user.ip_address) {
      return fail("caller_snapshot");
    }
  }
  if (!observed.mutation_ok || !observed.settlement_ok) {
    return fail("simulated_settlement");
  }
  if (!observed.jobs_drained || observed.final_pending ||
      observed.final_callbacks || observed.final_exception ||
      observed.final_concurrent != 0 || observed.final_inflight != 0 ||
      observed.final_id_delta != admitted) {
    return fail("dirty_final_state");
  }
  return std::nullopt;
}

auto evaluate(plinth::js::BridgeContext& context, std::string_view source)
    -> OwnedValue {
  JS_UpdateStackTop(context.rt);
  return {context.ctx, JS_Eval(context.ctx, source.data(), source.size(),
                               "<audit-corpus>", JS_EVAL_TYPE_GLOBAL)};
}

auto string_property(JSContext* context, JSValueConst value, const char* name)
    -> std::string {
  OwnedValue property{context, JS_GetPropertyStr(context, value, name)};
  if (!JS_IsString(property.get())) {
    return "<nonstring>";
  }
  std::size_t length = 0;
  const char* bytes = JS_ToCStringLen(context, &length, property.get());
  if (bytes == nullptr) {
    return "<conversion-failed>";
  }
  std::string result{bytes, length};
  JS_FreeCString(context, bytes);
  return result;
}

auto execute(plinth::js::BridgeContext& context, const CorpusCase& item)
    -> Observation {
  REQUIRE(context.pending_ops.empty());
  REQUIRE(context.callbacks.empty());
  REQUIRE(context.next_callback_id == 0);
  REQUIRE(context.extension_name == item.owner);
  const auto caller = fake_user();
  REQUIRE(context.user.user_id == caller.user_id);
  REQUIRE(context.user.session_id == caller.session_id);
  REQUIRE(context.user.ip_address == caller.ip_address);
  REQUIRE(context.user.username == caller.username);
  REQUIRE(context.user.auth_type == caller.auth_type);
  REQUIRE(context.user.effective_rules.empty());
  REQUIRE_FALSE(JS_HasException(context.ctx));
  REQUIRE_FALSE(JS_IsJobPending(context.rt));
  context.next_callback_id = INITIAL_ID;
  context.call_depth = item.call_depth;
  const auto original_owner = context.extension_name;
  const int original_depth = context.call_depth;
  const auto original_user = context.user.user_id;
  const auto original_session = context.user.session_id;
  const auto original_ip = context.user.ip_address;
  context.cancelled.store(item.cancelled);
  const auto payload = item.payload_literal.value_or(json_source(item.payload));
  auto result = evaluate(context, "globalThis.input=" + payload +
                                      ";globalThis.settled='none';"
                                      "globalThis.corpusPromise=" +
                                      expression(item) + ";corpusPromise");
  // Cancellation precedence was observed at binding entry. Job draining is a
  // separate explicit simulation, not the production cancellation driver.
  context.cancelled.store(false);
  Observation observed;
  if (JS_IsException(result.get())) {
    observed.result = Result::exception;
    OwnedValue exception{context.ctx, JS_GetException(context.ctx)};
    observed.code = string_property(context.ctx, exception.get(), "name");
    observed.message = string_property(context.ctx, exception.get(), "message");
  } else {
    const auto state = JS_PromiseState(context.ctx, result.get());
    observed.result = state == JS_PROMISE_PENDING    ? Result::pending
                      : state == JS_PROMISE_REJECTED ? Result::rejected
                                                     : Result::other;
    if (state == JS_PROMISE_REJECTED) {
      OwnedValue reason{context.ctx,
                        JS_PromiseResult(context.ctx, result.get())};
      observed.code = string_property(context.ctx, reason.get(), "code");
      observed.message = string_property(context.ctx, reason.get(), "message");
    }
    auto reaction =
        evaluate(context, "corpusPromise.then(()=>{settled='resolved'},"
                          "error=>{settled=error.code})");
    observed.mutation_ok = !JS_IsException(reaction.get());
  }
  observed.owner_after_call = context.extension_name;
  observed.id_delta = context.next_callback_id - INITIAL_ID;
  observed.concurrent = context.concurrent_async_ops;
  for (const auto& [id, callbacks] : context.callbacks) {
    observed.callback_ids.push_back(id);
    observed.callback_shape = observed.callback_shape &&
                              callbacks.ns_for_cancellation == "audit" &&
                              JS_IsFunction(context.ctx, callbacks.resolve) &&
                              JS_IsFunction(context.ctx, callbacks.reject);
  }
  std::ranges::sort(observed.callback_ids);
  if (observed.result == Result::pending) {
    auto mutation = evaluate(context, "input.marker=-99;input.after=true;"
                                      "if(input.nested){"
                                      "input.nested.note='changed';"
                                      "input.nested.after=true}");
    observed.mutation_ok =
        observed.mutation_ok && !JS_IsException(mutation.get());
    context.extension_name = "different-owner";
    context.call_depth = 99;
    context.user.user_id = "different-user";
    context.user.session_id = "different-session";
    context.user.ip_address = "192.0.2.2";
  }
  for (const auto& operation : context.pending_ops) {
    observed.operations.push_back(
        {operation.type, operation.callback_id, operation.audit_event_type,
         operation.audit_payload, operation.audit_user_id,
         operation.audit_session_id, operation.audit_ip_address});
  }
  // Consume admitted work locally; never run the dispatcher or touch a DB.
  auto owned_operations = context.take_pending_ops();
  REQUIRE(owned_operations.size() == observed.operations.size());
  for (const int id : observed.callback_ids) {
    if (item.simulate_rejection) {
      context.reject(id, {.code = "audit.test_settlement",
                          .message = "simulated outcome",
                          .sqlstate = std::nullopt});
    } else {
      context.resolve_with_js_value(id, JS_UNDEFINED);
    }
  }
  if (observed.result == Result::pending) {
    const auto state = JS_PromiseState(context.ctx, result.get());
    OwnedValue settled{context.ctx,
                       JS_PromiseResult(context.ctx, result.get())};
    observed.settlement_ok =
        item.simulate_rejection
            ? state == JS_PROMISE_REJECTED &&
                  string_property(context.ctx, settled.get(), "code") ==
                      "audit.test_settlement" &&
                  string_property(context.ctx, settled.get(), "message") ==
                      "simulated outcome"
            : state == JS_PROMISE_FULFILLED && JS_IsUndefined(settled.get());
  }
  for (int index = 0; index < MAX_JOBS && JS_IsJobPending(context.rt);
       ++index) {
    JSContext* job_context = nullptr;
    if (JS_ExecutePendingJob(context.rt, &job_context) < 0) {
      observed.jobs_drained = false;
      break;
    }
  }
  observed.jobs_drained = observed.jobs_drained && !JS_IsJobPending(context.rt);
  observed.final_exception = JS_HasException(context.ctx);
  observed.final_pending = !context.pending_ops.empty();
  observed.final_callbacks = !context.callbacks.empty();
  observed.final_id_delta = context.next_callback_id - INITIAL_ID;
  observed.final_concurrent = context.concurrent_async_ops;
  observed.final_inflight = context.inflight_detached.load();
  // Record an unexpected exception before consuming its owned value safely.
  if (observed.final_exception) {
    OwnedValue exception{context.ctx, JS_GetException(context.ctx)};
  }
  // These are deliberately mutated test inputs, not per-execution reset
  // fields. Restore the pool's fixed caller/callee before testing reuse.
  if (observed.result == Result::pending) {
    context.extension_name = original_owner;
    context.call_depth = original_depth;
    context.user.user_id = original_user;
    context.user.session_id = original_session;
    context.user.ip_address = original_ip;
  }
  return observed; // all JSValue owners die before the caller releases its
                   // lease
}

enum class Fault { none, drop_operation };

auto fresh(const CorpusCase& item, Fault fault = Fault::none) -> Observation {
  plinth::Config config{};
  const auto user = fake_user();
  plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                               config, 1, &user, item.owner);
  Lease lease{pool};
  REQUIRE(lease.get() != nullptr);
  auto result = execute(*lease.get(), item);
  lease.destroy();
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  if (fault == Fault::drop_operation) {
    result.operations.clear();
  }
  return result;
}

auto minimal_payload(const CorpusCase& item) -> Json::Value {
  Json::Value result{Json::objectValue};
  if (item.root_slot >= 0) {
    result[std::string{ROOT_KEYS.at(
        static_cast<std::size_t>(item.root_slot))}] = Json::Value{};
  } else if ((item.category >= 10 && item.category <= 15) ||
             item.category == 46) {
    result["user_id"] = Json::Value{};
  } else if (item.category >= 40 && item.category <= 42) {
    result["extension_id"] = Json::Value{};
  } else if (item.category >= 31 && item.category <= 37) {
    const auto key = ROOT_KEYS.at(static_cast<std::size_t>(item.category - 31));
    result[std::string{key} + "_"] = Json::Value{};
    result["nested"][std::string{key}] = Json::Value{};
  }
  return result;
}

struct Reduction {
  CorpusCase minimal;
  int attempts = 0;
  bool reproduced = false;
};

auto shrink(CorpusCase item, std::string_view signature, Fault fault)
    -> Reduction {
  const auto original = oracle(item, fresh(item, fault));
  if (!original.has_value() || original->signature != signature) {
    return {std::move(item), 0, false};
  }
  int attempts = 0;
  while (attempts < MAX_SHRINK_ATTEMPTS) {
    std::vector<CorpusCase> candidates;
    if (!item.event_literal.has_value() &&
        item.event.size() > item.event_floor.size()) {
      auto candidate = item;
      candidate.event.resize(item.event_floor.size() +
                             (item.event.size() - item.event_floor.size()) / 2);
      candidates.push_back(std::move(candidate));
    }
    if (!item.payload_literal.has_value()) {
      auto candidate = item;
      candidate.payload = minimal_payload(item);
      if (json_source(candidate.payload).size() <
          json_source(item.payload).size()) {
        candidates.push_back(std::move(candidate));
      }
    }
    bool improved = false;
    for (auto& candidate : candidates) {
      if (attempts == MAX_SHRINK_ATTEMPTS) {
        break;
      }
      ++attempts;
      REQUIRE(candidate.category == item.category);
      check_bounds(candidate);
      const auto failure = oracle(candidate, fresh(candidate, fault));
      if (failure.has_value() && failure->signature == signature) {
        item = std::move(candidate);
        improved = true;
        break;
      }
    }
    if (!improved) {
      break;
    }
  }
  const auto final = oracle(item, fresh(item, fault));
  REQUIRE(final.has_value());
  REQUIRE(final->signature == signature);
  return {std::move(item), attempts, true};
}

auto assert_observation(const CorpusCase& item, const Observation& observed)
    -> void {
  check_bounds(item);
  const auto failure = oracle(item, observed);
  if (failure.has_value()) {
    const auto reduced = shrink(item, failure->signature, Fault::none);
    INFO("category=" << item.category << " expression=" << expression(item)
                     << " payload=" << json_source(item.payload)
                     << " signature=" << failure->signature
                     << " shrink_attempts=" << reduced.attempts
                     << " fresh_reproduced=" << reduced.reproduced);
    if (reduced.reproduced) {
      INFO("minimized=" << expression(reduced.minimal) << " minimized_payload="
                        << json_source(reduced.minimal.payload));
      FAIL(failure->message);
    }
    INFO("Fresh context did not reproduce this failure; no minimized claim."
         " Pool/lifecycle-only failures retain the original case.");
    FAIL(failure->message);
  }
}

auto ideal(const CorpusCase& item) -> Observation {
  Observation result;
  result.result = item.want;
  result.code = item.code;
  result.message = item.message;
  result.owner_after_call = item.owner;
  if (item.want == Result::pending) {
    const auto user = fake_user();
    result.operations.push_back(
        {plinth::js::AsyncOp::Type::AUDIT_WRITE, INITIAL_ID, item.event,
         enriched(item), user.user_id, user.session_id, user.ip_address});
    result.callback_ids.push_back(INITIAL_ID);
    result.id_delta = 1;
    result.final_id_delta = 1;
  }
  return result;
}

auto negative_controls() -> void {
  const auto accepted = generated_case(SEEDS.front(), 47);
  const auto good = ideal(accepted);
  REQUIRE_FALSE(oracle(accepted, good).has_value());
  for (int control = 0; control < 30; ++control) {
    INFO("oracle_negative_control=" << control);
    auto corrupt = good;
    switch (control) {
      case 0: corrupt.operations.clear(); break;
      case 1: corrupt.operations.push_back(corrupt.operations.front()); break;
      case 2: corrupt.result = Result::rejected; break;
      case 3: corrupt.callback_ids.clear(); break;
      case 4: corrupt.callback_ids.push_back(INITIAL_ID + 1); break;
      case 5: corrupt.callback_shape = false; break;
      case 6:
        corrupt.operations.front().type = plinth::js::AsyncOp::Type::DB_QUERY;
        break;
      case 7: ++corrupt.operations.front().id; break;
      case 8: corrupt.operations.front().event = "wrong.event"; break;
      case 9: corrupt.operations.front().detail["marker"] = -999; break;
      case 10:
        corrupt.operations.front().detail["extension_id"] = "sibling";
        break;
      case 11: corrupt.operations.front().detail["call_depth"] = 99; break;
      case 12: corrupt.operations.front().user = "wrong-user"; break;
      case 13: corrupt.operations.front().session = "wrong-session"; break;
      case 14: corrupt.operations.front().ip = "192.0.2.99"; break;
      case 15: ++corrupt.id_delta; break;
      case 16: corrupt.concurrent = 1; break;
      case 17: corrupt.final_callbacks = true; break;
      case 18: corrupt.final_exception = true; break;
      case 19: corrupt.final_id_delta = 0; break;
      case 20: ++corrupt.callback_ids.front(); break;
      case 21: corrupt.jobs_drained = false; break;
      case 22: corrupt.final_pending = true; break;
      case 23: corrupt.final_concurrent = 1; break;
      case 24: corrupt.final_inflight = 1; break;
      case 25: corrupt.owner_after_call = "sibling"; break;
      case 26: corrupt.settlement_ok = false; break;
      case 27: ++corrupt.final_id_delta; break;
      case 28: corrupt.code = "wrong.code"; break;
      case 29: corrupt.mutation_ok = false; break;
      default: FAIL("unknown negative control");
    }
    REQUIRE(oracle(accepted, corrupt).has_value());
  }
  const auto rejected = generated_case(SEEDS.front(), 24);
  auto corrupt = ideal(rejected);
  REQUIRE_FALSE(oracle(rejected, corrupt).has_value());
  corrupt.operations = good.operations;
  REQUIRE(oracle(rejected, corrupt).has_value());
  corrupt = ideal(rejected);
  corrupt.code = "wrong.code";
  REQUIRE(oracle(rejected, corrupt).has_value());
  const auto injected =
      oracle(accepted, fresh(accepted, Fault::drop_operation));
  REQUIRE(injected.has_value());
  const auto reduced =
      shrink(accepted, injected->signature, Fault::drop_operation);
  REQUIRE(reduced.reproduced);
  REQUIRE(reduced.attempts > 0);
  REQUIRE(reduced.attempts <= MAX_SHRINK_ATTEMPTS);
  REQUIRE(reduced.minimal.category == accepted.category);
  REQUIRE(reduced.minimal.event.size() < accepted.event.size());
  const auto replay =
      oracle(reduced.minimal, fresh(reduced.minimal, Fault::drop_operation));
  REQUIRE(replay.has_value());
  REQUIRE(replay->signature == injected->signature);
  REQUIRE_FALSE(oracle(reduced.minimal, fresh(reduced.minimal)).has_value());

  auto lifecycle_only = good;
  lifecycle_only.final_pending = true;
  const auto nonfresh_failure = oracle(accepted, lifecycle_only);
  REQUIRE(nonfresh_failure.has_value());
  const auto nonfresh =
      shrink(accepted, nonfresh_failure->signature, Fault::none);
  REQUIRE_FALSE(nonfresh.reproduced);
  REQUIRE(nonfresh.attempts == 0);
  REQUIRE(nonfresh.minimal.category == accepted.category);
  REQUIRE(nonfresh.minimal.event == accepted.event);
  REQUIRE(nonfresh.minimal.payload == accepted.payload);
  REQUIRE(expression(nonfresh.minimal) == expression(accepted));

  // Guard the combined triggers, not merely an integer category label.
  for (const int category : {10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29,
                             30, 31, 32, 33, 34, 35, 36, 37, 40, 41, 42, 46}) {
    auto minimal = generated_case(SEEDS.front(), category);
    minimal.payload = minimal_payload(minimal);
    minimal.event = minimal.event_floor;
    INFO("minimal_trigger_control=" << category);
    if ((category >= 10 && category <= 15) || category == 46) {
      REQUIRE(minimal.payload.isMember("user_id"));
    } else if (category >= 40 && category <= 42) {
      REQUIRE(minimal.payload.isMember("extension_id"));
    } else if (category >= 24 && category <= 30) {
      REQUIRE(minimal.payload.isMember(
          std::string{ROOT_KEYS.at(static_cast<std::size_t>(category - 24))}));
    } else {
      const auto key = ROOT_KEYS.at(static_cast<std::size_t>(category - 31));
      REQUIRE(minimal.payload.isMember(std::string{key} + "_"));
      REQUIRE(minimal.payload["nested"].isMember(std::string{key}));
    }
    check_bounds(minimal);
    REQUIRE_FALSE(oracle(minimal, fresh(minimal)).has_value());
  }
}

} // namespace

TEST_CASE("QuickJS audit.log has a bounded deterministic contract corpus",
          "[js][audit][corpus]") {
  INFO("seeds=" << SEEDS.size() << " cases_per_seed=" << CASES_PER_SEED
                << " event_utf8_bytes=" << MAX_EVENT_BYTES
                << " root_depth=0 max_depth=" << MAX_DEPTH << " max_nodes="
                << MAX_NODES << " string_bytes=" << MAX_STRING_BYTES
                << " shrink_attempts=" << MAX_SHRINK_ATTEMPTS);
  negative_controls();
  // Ordinary minimal fixtures remain even when the generated shapes evolve.
  for (const int category : {0, 1, 3, 5, 10, 16, 18, 22, 24, 31, 38, 40, 43}) {
    auto minimal = generated_case(SEEDS.front(), category);
    minimal.payload = minimal_payload(minimal);
    minimal.event = minimal.event_floor;
    INFO("minimal_regression_category=" << category);
    assert_observation(minimal, fresh(minimal));
  }
  for (const auto seed : SEEDS) {
    INFO("seed=" << seed);
    const auto first = generated_case(seed, 0);
    plinth::Config config{};
    const auto user = fake_user();
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1, &user, first.owner);
    std::array<bool, NAMESPACES.size()> namespaces{};
    std::array<bool, ROOT_KEYS.size()> roots{};
    for (int index = 0; index < CASES_PER_SEED; ++index) {
      INFO("case=" << index);
      const auto item = generated_case(seed, index);
      if (item.namespace_slot >= 0) {
        namespaces.at(static_cast<std::size_t>(item.namespace_slot)) = true;
      }
      if (item.root_slot >= 0) {
        roots.at(static_cast<std::size_t>(item.root_slot)) = true;
      }
      Lease lease{pool};
      REQUIRE(lease.get() != nullptr);
      const auto observed = execute(*lease.get(), item);
      if (index % 3 == 0) {
        lease.destroy();
      } else {
        lease.release();
      }
      assert_observation(item, observed);
      assert_observation(item, fresh(item));
      REQUIRE(pool.active_count() == 0);
      if (index % 8 == 7) {
        pool.rebuild();
      }
    }
    REQUIRE(
        std::ranges::all_of(namespaces, [](bool covered) { return covered; }));
    REQUIRE(std::ranges::all_of(roots, [](bool covered) { return covered; }));
    Lease held{pool};
    REQUIRE(held.get() != nullptr);
    REQUIRE_FALSE(pool.shutdown(0ms));
    held.destroy();
    REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
    REQUIRE(pool.acquire() == nullptr);
  }
}
