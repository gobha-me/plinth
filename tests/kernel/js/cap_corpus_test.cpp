// SPDX-License-Identifier: MIT
//
// Bounded DB-free admission through the registered capability bindings.
// Host settlements below are explicit fixture simulations, not capability
// resolver/dispatcher execution or the production cancellation cascade.
// No Promise globals are replaced; generator bounds are test-only budgets.

#include <catch2/catch_test_macros.hpp>

#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/js/stdlib/cap_bindings.hpp"
#include "kernel/logging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using plinth::capabilities::UserContext;
using plinth::js::AsyncOp;
using plinth::js::BridgeContext;
using plinth::js::RuntimePool;

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1500'0000'0000'0001ULL, 0x1500'0000'0000'0027ULL,
    0x1500'0000'0000'00C6ULL, 0x1500'0000'0000'BEEFULL};
constexpr int FAMILIES = 48;
constexpr std::size_t MAX_BATCH = 4;
constexpr int MAX_TUPLE = 3;
constexpr std::size_t MAX_SIGNATURE_BYTES = 128;
constexpr int MAX_DEPTH = 3;
constexpr int MAX_NODES = 15;
constexpr std::size_t MAX_STRING_BYTES = 64;
constexpr int MAX_JOBS = 128;
constexpr int MAX_SHRINK_ATTEMPTS = 16;
constexpr int INITIAL_ID = 17;
constexpr auto SHUTDOWN_BOUND = 2s;

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

auto fake_user() -> UserContext {
  return {.user_id = "fake-cap-user",
          .username = "fake-cap-caller",
          .auth_type = "session",
          .effective_rules = {"notes.read", "notes.echo"},
          .session_id = "fake-cap-session",
          .ip_address = "192.0.2.15"};
}

auto same_user(const UserContext& left, const UserContext& right) -> bool {
  return left.user_id == right.user_id && left.username == right.username &&
         left.auth_type == right.auth_type &&
         left.effective_rules == right.effective_rules &&
         left.session_id == right.session_id &&
         left.ip_address == right.ip_address;
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
    const auto atom =
        ATOMS.at(static_cast<std::size_t>(next(state) % ATOMS.size()));
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

// Literal fixture expectations, independent of conversion/parser/mapper
// helpers. Tuple validation errors describe JS syntax only; nonempty signatures
// are not certified resolver signatures by this admission corpus.
struct Tuple {
  int arity = 2;
  std::string signature = "notes:1:echo";
  std::optional<std::string> signature_literal;
  Json::Value arguments;
  std::optional<std::string> arguments_literal;
  std::optional<std::string> tuple_literal;
  std::string error;
};

struct CorpusCase {
  int family = 0;
  bool batch = false;
  bool missing = false;
  bool cancelled = false;
  bool temporal_rejection = false;
  std::optional<std::string> outer_literal;
  std::vector<Tuple> tuples{Tuple{}};
  UserContext user = fake_user();
  int depth = 3;
};

constexpr std::string_view SIGNATURE_TYPE =
    "cap.call: signature must be a string";
constexpr std::string_view SIGNATURE_EMPTY =
    "cap.call: signature must be non-empty";
constexpr std::string_view PAYLOAD_TYPE =
    "cap.call: args contains unsupported value";
constexpr std::string_view OUTER_TYPE = "cap.batch: calls must be an array";
constexpr std::string_view TUPLE_TYPE =
    "cap.batch: each call must be a [signature, args?] array";
constexpr std::string_view TUPLE_ARITY =
    "cap.batch: each call must have 1 or 2 elements [signature, args?]";

auto tuple_source(const Tuple& tuple) -> std::string {
  if (tuple.tuple_literal.has_value()) {
    return *tuple.tuple_literal;
  }
  std::string result = "[";
  if (tuple.arity > 0) {
    result += tuple.signature_literal.value_or(json_source(tuple.signature));
  }
  if (tuple.arity > 1) {
    result +=
        "," + tuple.arguments_literal.value_or(json_source(tuple.arguments));
  }
  if (tuple.arity > 2) {
    result += ",37";
  }
  result += "]";
  REQUIRE(tuple.arity <= MAX_TUPLE);
  return result;
}

auto input_source(const CorpusCase& item) -> std::string {
  if (item.missing) {
    return "[]";
  }
  if (!item.batch) {
    return tuple_source(item.tuples.front());
  }
  if (item.outer_literal.has_value()) {
    return "[" + *item.outer_literal + "]";
  }
  std::string result = "[[";
  for (std::size_t index = 0; index < item.tuples.size(); ++index) {
    if (index != 0) {
      result += ",";
    }
    // An outer hole is syntax, not a substituted undefined argument tuple.
    result += tuple_source(item.tuples[index]);
  }
  return result + "]]";
}

auto make_case(std::uint64_t seed, int family) -> CorpusCase {
  CorpusCase item;
  item.family = family;
  auto state = seed + static_cast<std::uint64_t>(family);
  int remaining = MAX_NODES;
  item.tuples.front().arguments = tree(state, 0, remaining);
  item.depth = static_cast<int>(next(state) % 9U);
  item.user.auth_type = (seed & 1U) != 0 ? "session" : "pat";
  if (family < 24) {
    auto& tuple = item.tuples.front();
    switch (family) {
      case 0:
        tuple.arity = 1;
        tuple.arguments = Json::Value{};
        break;
      case 1:
        tuple.arguments_literal = "undefined";
        tuple.arguments = Json::Value{};
        break;
      case 2: tuple.arguments = Json::Value{}; break;
      case 3: tuple.arguments = true; break;
      case 4: tuple.arguments = false; break;
      case 5:
        tuple.arguments = static_cast<int>(next(state) % 201U) - 100;
        break;
      case 6: tuple.arguments = 0.5; break;
      case 7: tuple.arguments = std::string{"a\0\xC3\xA9", 4}; break;
      case 8: tuple.arguments = Json::Value{Json::arrayValue}; break;
      case 9: tuple.arguments = Json::Value{Json::objectValue}; break;
      case 10: tuple.arity = 3; break; // Ordinary extra call arg is ignored.
      case 11: {
        tuple.arguments = Json::Value{Json::objectValue};
        tuple.arguments["nest"]["leaf"] = Json::Value{Json::arrayValue};
        tuple.arguments["nest"]["leaf"].append(9);
        break;
      }
      case 12: tuple.signature = " "; break;
      case 13: tuple.signature = "not-a-resolver-signature"; break;
      case 14:
        tuple.signature = std::string{"notes\0\xC3\xA9:1:echo", 15};
        break;
      case 15: tuple.signature = std::string(MAX_SIGNATURE_BYTES, 's'); break;
      case 16:
        tuple.arity = 0;
        tuple.error = SIGNATURE_TYPE;
        break;
      case 17:
        tuple.signature_literal = "null";
        tuple.error = SIGNATURE_TYPE;
        break;
      case 18:
        tuple.signature_literal = "23";
        tuple.error = SIGNATURE_TYPE;
        break;
      case 19:
        tuple.signature.clear();
        tuple.error = SIGNATURE_EMPTY;
        break;
      case 20:
        tuple.arguments_literal = "Symbol('fake')";
        tuple.error = PAYLOAD_TYPE;
        break;
      case 21:
        tuple.arguments_literal = "1n";
        tuple.error = PAYLOAD_TYPE;
        break;
      case 22: item.cancelled = true; break;
      case 23:
        item.cancelled = true;
        tuple.signature.clear();
        tuple.arguments_literal = "Symbol('fake')";
        tuple.error = SIGNATURE_EMPTY;
        break;
      default: FAIL("unknown call family");
    }
    return item;
  }
  item.batch = true;
  const auto generated = item.tuples.front();
  const auto simple = Tuple{.arity = 1, .arguments = Json::Value{}};
  switch (family - 24) {
    case 0: item.tuples.clear(); break;
    case 1: item.tuples = {simple}; break;
    case 2:
      item.tuples.front().arguments_literal = "undefined";
      item.tuples.front().arguments = Json::Value{};
      break;
    case 3: item.tuples.front().arguments = Json::Value{}; break;
    case 4: break;
    case 5: item.tuples = {generated, simple}; break;
    case 6: item.tuples = {generated, simple, generated, simple}; break;
    case 7:
      item.tuples = {generated, simple, generated, simple};
      item.temporal_rejection = true;
      break;
    case 8: item.outer_literal = "({fake:1})"; break;
    case 9: item.missing = true; break;
    case 10:
      item.tuples.front().arity = 0;
      item.tuples.front().error = TUPLE_ARITY;
      break;
    case 11:
      item.tuples.front().arity = 3;
      item.tuples.front().error = TUPLE_ARITY;
      break;
    case 12:
      item.tuples.front().tuple_literal = "23";
      item.tuples.front().error = TUPLE_TYPE;
      break;
    case 13:
      item.tuples = {
          simple, generated,
          Tuple{.tuple_literal = "null", .error = std::string{TUPLE_TYPE}}};
      break;
    case 14:
      item.tuples = {
          simple, Tuple{.tuple_literal = "", .error = std::string{TUPLE_TYPE}},
          generated};
      break;
    case 15:
      item.tuples = {generated,
                     Tuple{.arity = 0, .error = std::string{TUPLE_ARITY}}};
      break;
    case 16:
      item.tuples = {generated, Tuple{.signature_literal = "false",
                                      .error = std::string{SIGNATURE_TYPE}}};
      break;
    case 17:
      item.tuples = {simple, Tuple{.signature = "",
                                   .error = std::string{SIGNATURE_EMPTY}}};
      break;
    case 18:
      item.tuples = {generated, Tuple{.arguments_literal = "Symbol('fake')",
                                      .error = std::string{PAYLOAD_TYPE}}};
      break;
    case 19:
      item.tuples = {generated, simple, generated, simple};
      item.cancelled = true;
      break;
    case 20:
      item.tuples = {simple, Tuple{.tuple_literal = "null",
                                   .error = std::string{TUPLE_TYPE}}};
      item.cancelled = true;
      break;
    case 21:
      item.tuples = {generated, Tuple{.signature = "",
                                      .error = std::string{SIGNATURE_EMPTY}}};
      item.cancelled = true;
      break;
    case 22:
      item.tuples.front().arguments_literal = "Symbol('fake')";
      item.tuples.front().error = PAYLOAD_TYPE;
      item.cancelled = true;
      break;
    case 23:
      item.tuples.front().tuple_literal = "['notes:1:echo',,]";
      item.tuples.front().arguments = Json::Value{};
      break;
    default: FAIL("unknown batch family");
  }
  return item;
}

struct Expectation {
  std::string exception;
  std::vector<Tuple> admitted;
  JSPromiseStateEnum immediate = JS_PROMISE_PENDING;
  JSPromiseStateEnum final_state = JS_PROMISE_FULFILLED;
};

auto expected(const CorpusCase& item) -> Expectation {
  Expectation result;
  if (item.batch && (item.missing || item.outer_literal.has_value())) {
    result.exception = OUTER_TYPE;
    return result;
  }
  for (const auto& tuple : item.tuples) {
    if (!tuple.error.empty()) {
      result.exception = tuple.error;
      return result;
    }
    if (!item.cancelled) {
      result.admitted.push_back(tuple);
    }
  }
  if (item.batch && item.tuples.empty()) {
    result.immediate = JS_PROMISE_FULFILLED;
  }
  if (item.cancelled && !item.tuples.empty()) {
    result.immediate = item.batch ? JS_PROMISE_PENDING : JS_PROMISE_REJECTED;
    result.final_state = JS_PROMISE_REJECTED;
  } else if (item.temporal_rejection) {
    result.final_state = JS_PROMISE_REJECTED;
  }
  return result;
}

auto evaluate(BridgeContext& context, const std::string& source) -> OwnedValue {
  return {context.ctx, JS_Eval(context.ctx, source.data(), source.size(),
                               "<cap-corpus>", JS_EVAL_TYPE_GLOBAL)};
}

auto text_value(JSContext* context, JSValue value) -> std::string {
  std::size_t size = 0;
  const char* text = JS_ToCStringLen(context, &size, value);
  REQUIRE(text != nullptr);
  std::string result{text, size};
  JS_FreeCString(context, text);
  return result;
}

auto property_text(JSContext* context, JSValue value, const char* name)
    -> std::string {
  OwnedValue property{context, JS_GetPropertyStr(context, value, name)};
  return text_value(context, property.get());
}

// JSON.stringify belongs to the unmodified runtime; JsonCpp parses observed
// JSON, never generates the expected admission values from production helpers.
auto observed_json(JSContext* context, JSValue value) -> Json::Value {
  OwnedValue global{context, JS_GetGlobalObject(context)};
  OwnedValue json{context, JS_GetPropertyStr(context, global.get(), "JSON")};
  OwnedValue stringify{context,
                       JS_GetPropertyStr(context, json.get(), "stringify")};
  OwnedValue result{context,
                    JS_Call(context, stringify.get(), json.get(), 1, &value)};
  REQUIRE_FALSE(JS_IsException(result.get()));
  REQUIRE(JS_IsString(result.get()));
  const auto source = text_value(context, result.get());
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
  Json::Value output;
  std::string errors;
  REQUIRE(reader->parse(source.data(), source.data() + source.size(), &output,
                        &errors));
  return output;
}

auto rejection_shape(JSContext* context, JSValue value) -> bool {
  JSPropertyEnum* properties = nullptr;
  std::uint32_t count = 0;
  REQUIRE(JS_GetOwnPropertyNames(context, &properties, &count, value,
                                 JS_GPN_STRING_MASK) == 0);
  std::vector<std::string> names;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::size_t length = 0;
    const char* name =
        JS_AtomToCStringLen(context, &length, properties[index].atom);
    REQUIRE(name != nullptr);
    names.emplace_back(name, length);
    JS_FreeCString(context, name);
  }
  JS_FreePropertyEnum(context, properties, count);
  std::ranges::sort(names);
  return names == std::vector<std::string>{"code", "message"};
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

struct Callback {
  int id = 0;
  std::string name_space;
  bool resolve = false;
  bool reject = false;
};

struct Observation {
  std::string exception_name;
  std::string exception_message;
  int immediate = -1;
  int final_state = -1;
  int next_id = INITIAL_ID;
  int pending_count = 0;
  int concurrent = 0;
  std::vector<AsyncOp> operations;
  std::vector<Callback> callbacks;
  bool caller_unchanged = true;
  bool snapshots_owned = true;
  bool temporal_owned = true;
  bool rejection_keys = true;
  bool clean = true;
  bool late_clean = true;
  Json::Value result;
};

auto settlement_value(std::size_t index) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["index"] = static_cast<int>(index);
  result["value"] = std::string{"fake\0result", 11};
  return result;
}

auto final_value(const CorpusCase& item, const Expectation& want)
    -> Json::Value {
  if (want.final_state == JS_PROMISE_REJECTED) {
    Json::Value value{Json::objectValue};
    value["code"] = item.cancelled ? "cap.cancelled" : "cap.test_first";
    value["message"] =
        item.cancelled ? "execution cancelled" : "first temporal rejection";
    return value;
  }
  if (!item.batch) {
    return settlement_value(0);
  }
  Json::Value values{Json::arrayValue};
  for (std::size_t index = 0; index < want.admitted.size(); ++index) {
    values.append(settlement_value(index));
  }
  return values;
}

auto oracle(const CorpusCase& item, const Observation& observation)
    -> std::optional<std::string> {
  const auto want = expected(item);
  if (observation.exception_name !=
          (want.exception.empty() ? "" : "TypeError") ||
      observation.exception_message != want.exception) {
    return "exception";
  }
  const auto count = want.admitted.size();
  if (observation.operations.size() != count) {
    return "operations";
  }
  if (observation.next_id != INITIAL_ID + static_cast<int>(count) ||
      observation.callbacks.size() != count ||
      observation.pending_count != static_cast<int>(2 * count) ||
      observation.concurrent != 0) {
    return "inventory";
  }
  for (std::size_t index = 0; index < count; ++index) {
    const auto& operation = observation.operations[index];
    const auto& tuple = want.admitted[index];
    const auto& callback = observation.callbacks[index];
    const int id = INITIAL_ID + static_cast<int>(index);
    if (operation.type != AsyncOp::Type::CAP_CALL ||
        operation.callback_id != id || callback.id != id ||
        callback.name_space != "cap" || !callback.resolve || !callback.reject) {
      return "callback";
    }
    if (operation.cap_signature != tuple.signature ||
        operation.cap_args != tuple.arguments ||
        !same_user(operation.cap_user, item.user) ||
        operation.cap_call_depth != item.depth || !operation.sql.empty() ||
        !operation.sql_params.empty() || operation.silent ||
        !operation.audit_event_type.empty() ||
        !operation.audit_payload.isNull() ||
        !operation.pubsub_channel.empty() ||
        !operation.pubsub_payload.isNull() || operation.batch_scope_id != 0 ||
        operation.batch_pinned_conn != nullptr) {
      return "snapshot";
    }
  }
  if (!observation.caller_unchanged || !observation.snapshots_owned) {
    return "ownership";
  }
  if (want.exception.empty() &&
      (observation.immediate != want.immediate ||
       observation.final_state != want.final_state ||
       observation.result != final_value(item, want))) {
    return "settlement";
  }
  if (!observation.temporal_owned || !observation.rejection_keys) {
    return "temporal";
  }
  if (!observation.clean || !observation.late_clean) {
    return "cleanup";
  }
  return std::nullopt;
}

auto execute(BridgeContext& context, const CorpusCase& item) -> Observation {
  REQUIRE(context.pending_ops.empty());
  REQUIRE(context.callbacks.empty());
  REQUIRE(context.persistent_callbacks.empty());
  REQUIRE_FALSE(JS_IsJobPending(context.rt));
  context.next_callback_id = INITIAL_ID;
  context.user = item.user;
  context.call_depth = item.depth;
  auto setup = evaluate(context, "globalThis.__cap_args=" + input_source(item));
  REQUIRE_FALSE(JS_IsException(setup.get()));
  OwnedValue global{context.ctx, JS_GetGlobalObject(context.ctx)};
  OwnedValue cap{context.ctx,
                 JS_GetPropertyStr(context.ctx, global.get(), "cap")};
  OwnedValue function{
      context.ctx,
      JS_GetPropertyStr(context.ctx, cap.get(), item.batch ? "batch" : "call")};
  REQUIRE(JS_IsFunction(context.ctx, function.get()));
  OwnedValue input{context.ctx,
                   JS_GetPropertyStr(context.ctx, global.get(), "__cap_args")};
  const int argc = item.missing ? 0
                   : item.batch ? 1
                                : item.tuples.front().arity;
  std::array<JSValue, MAX_TUPLE> arguments{JS_UNDEFINED, JS_UNDEFINED,
                                           JS_UNDEFINED};
  for (int index = 0; index < argc; ++index) {
    arguments.at(static_cast<std::size_t>(index)) = JS_GetPropertyUint32(
        context.ctx, input.get(), static_cast<std::uint32_t>(index));
  }
  context.cancelled.store(item.cancelled);
  OwnedValue promise{context.ctx, JS_Call(context.ctx, function.get(),
                                          cap.get(), argc, arguments.data())};
  for (int index = 0; index < argc; ++index) {
    JS_FreeValue(context.ctx, arguments.at(static_cast<std::size_t>(index)));
  }
  Observation observation;
  if (JS_IsException(promise.get())) {
    OwnedValue exception{context.ctx, JS_GetException(context.ctx)};
    observation.exception_name =
        property_text(context.ctx, exception.get(), "name");
    observation.exception_message =
        property_text(context.ctx, exception.get(), "message");
  } else {
    observation.immediate = JS_PromiseState(context.ctx, promise.get());
  }
  // Capture admission before clearing the interrupt flag. Job draining below
  // is a separately labelled host simulation, not cancellation-driver proof.
  observation.next_id = context.next_callback_id;
  observation.pending_count = context.pending_op_count();
  observation.concurrent = context.concurrent_async_ops;
  observation.caller_unchanged =
      same_user(context.user, item.user) && context.call_depth == item.depth;
  for (const auto& [id, callback] : context.callbacks) {
    observation.callbacks.push_back(
        {id, callback.ns_for_cancellation,
         JS_IsFunction(context.ctx, callback.resolve),
         JS_IsFunction(context.ctx, callback.reject)});
  }
  std::ranges::sort(observation.callbacks, {}, &Callback::id);
  context.cancelled.store(false);
  // Mutate actual JS inputs and every caller identity component after enqueue.
  auto mutation =
      evaluate(context, "(function mutate(v,d){if(v&&typeof v==='object'&&d<6){"
                        "for(const k of Object.keys(v))mutate(v[k],d+1);"
                        "if(Array.isArray(v))v.length=0;else for(const k of "
                        "Object.keys(v))v[k]='changed';"
                        "}})(__cap_args,0)");
  REQUIRE_FALSE(JS_IsException(mutation.get()));
  context.user = {.user_id = "changed-user",
                  .username = "changed-name",
                  .auth_type = "anonymous",
                  .effective_rules = {"changed.rule"},
                  .session_id = "changed-session",
                  .ip_address = "192.0.2.99"};
  context.call_depth = 99;
  observation.operations = context.take_pending_ops();
  const auto want = expected(item);
  observation.snapshots_owned =
      observation.operations.size() == want.admitted.size();
  for (std::size_t index = 0;
       index < std::min(observation.operations.size(), want.admitted.size());
       ++index) {
    const auto& operation = observation.operations[index];
    observation.snapshots_owned =
        observation.snapshots_owned &&
        operation.cap_signature == want.admitted[index].signature &&
        operation.cap_args == want.admitted[index].arguments &&
        same_user(operation.cap_user, item.user) &&
        operation.cap_call_depth == item.depth;
  }
  // Simulate the owning host's in-flight accounting; admission itself did not
  // increment this counter, which was captured independently above.
  context.concurrent_async_ops =
      static_cast<int>(observation.operations.size());
  if (item.temporal_rejection && observation.operations.size() == MAX_BATCH) {
    context.reject(observation.operations[3].callback_id,
                   {.code = "cap.test_first",
                    .message = "first temporal rejection",
                    .sqlstate = std::nullopt});
    REQUIRE(drain_jobs(context));
    observation.temporal_owned =
        context.callbacks.size() == 3 && context.concurrent_async_ops == 3 &&
        JS_PromiseState(context.ctx, promise.get()) == JS_PROMISE_REJECTED;
    {
      OwnedValue first{context.ctx,
                       JS_PromiseResult(context.ctx, promise.get())};
      observation.temporal_owned =
          observation.temporal_owned &&
          observed_json(context.ctx, first.get()) == final_value(item, want);
    }
    context.reject(observation.operations[1].callback_id,
                   {.code = "cap.test_second",
                    .message = "later rejection at smaller index",
                    .sqlstate = std::nullopt});
    REQUIRE(drain_jobs(context));
    observation.temporal_owned =
        observation.temporal_owned && context.callbacks.size() == 2;
    context.resolve(observation.operations[2].callback_id, settlement_value(2));
    context.resolve(observation.operations[0].callback_id, settlement_value(0));
  } else {
    for (std::size_t remaining = observation.operations.size(); remaining > 0;
         --remaining) {
      const auto index = remaining - 1;
      context.resolve(observation.operations[index].callback_id,
                      settlement_value(index));
      REQUIRE(context.callbacks.size() == index);
      REQUIRE(drain_jobs(context));
      if (item.batch && observation.exception_name.empty() && remaining > 1) {
        observation.temporal_owned =
            observation.temporal_owned &&
            JS_PromiseState(context.ctx, promise.get()) == JS_PROMISE_PENDING;
      }
    }
  }
  REQUIRE(drain_jobs(context));
  if (!JS_IsException(promise.get())) {
    observation.final_state = JS_PromiseState(context.ctx, promise.get());
    if (observation.final_state != JS_PROMISE_PENDING) {
      OwnedValue result{context.ctx,
                        JS_PromiseResult(context.ctx, promise.get())};
      observation.result = observed_json(context.ctx, result.get());
      if (observation.final_state == JS_PROMISE_REJECTED) {
        observation.rejection_keys = rejection_shape(context.ctx, result.get());
      }
    }
  }
  observation.clean =
      context.callbacks.empty() && context.pending_ops.empty() &&
      context.concurrent_async_ops == 0 &&
      context.persistent_callbacks.empty() &&
      context.inflight_detached.load() == 0 && !JS_IsJobPending(context.rt) &&
      !JS_HasException(context.ctx);
  if (!observation.operations.empty()) {
    const int settled = observation.operations.front().callback_id;
    context.resolve(settled, settlement_value(0));
    context.reject(settled, {.code = "cap.test_late",
                             .message = "fake late outcome",
                             .sqlstate = std::nullopt});
    OwnedValue late{context.ctx,
                    JS_NewString(context.ctx, "fake-owned-late-value")};
    context.resolve_with_js_value(settled,
                                  JS_DupValue(context.ctx, late.get()));
    observation.late_clean =
        text_value(context.ctx, late.get()) == "fake-owned-late-value" &&
        context.callbacks.empty() && context.pending_ops.empty() &&
        context.concurrent_async_ops == 0 &&
        context.next_callback_id == observation.next_id &&
        !JS_IsJobPending(context.rt) && !JS_HasException(context.ctx);
  }
  return observation;
}

auto replay(const CorpusCase& item) -> Observation {
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, "notes");
  Observation observation;
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    observation = execute(*lease.get(), item);
    // All external JS values returned from execute have already been freed.
    lease.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  return observation;
}

enum class Fault { none, drop_operation };

auto replay_with_fault(const CorpusCase& item, Fault fault) -> Observation {
  auto observation = replay(item);
  if (fault == Fault::drop_operation && !observation.operations.empty()) {
    observation.operations.pop_back();
  }
  return observation;
}

struct Shrunk {
  CorpusCase item;
  int attempts = 0;
};

auto shrink(CorpusCase item, std::string_view failure, Fault fault) -> Shrunk {
  int attempts = 0;
  if (item.batch || item.tuples.empty() || item.cancelled ||
      !expected(item).exception.empty()) {
    return {.item = std::move(item), .attempts = 0};
  }
  // A finite semantic reduction list; it neither runs arbitrary JS nor claims
  // exhaustive minimization. Every accepted refinement gets a fresh runtime.
  for (int step = 0; step < 3 && attempts < MAX_SHRINK_ATTEMPTS; ++step) {
    auto candidate = item;
    auto& tuple = candidate.tuples.front();
    switch (step) {
      case 0: tuple.arity = 2; break;
      case 1:
        tuple.arguments = Json::Value{};
        tuple.arguments_literal.reset();
        break;
      case 2:
        tuple.signature = "x";
        tuple.signature_literal.reset();
        break;
      default: FAIL("unknown shrink step");
    }
    ++attempts;
    const auto found = oracle(candidate, replay_with_fault(candidate, fault));
    if (found.has_value() && *found == failure) {
      item = std::move(candidate);
    }
  }
  return {.item = std::move(item), .attempts = attempts};
}

auto require_observation(const CorpusCase& item, const Observation& observation)
    -> void {
  const auto failure = oracle(item, observation);
  if (failure.has_value()) {
    const auto reduced = shrink(item, *failure, Fault::none);
    INFO("bounded failure=" << *failure
                            << " shrink_attempts=" << reduced.attempts
                            << " input=" << input_source(reduced.item));
  }
  REQUIRE_FALSE(failure.has_value());
}

auto negative_controls() -> void {
  auto item = make_case(SEEDS.front(), 10);
  const auto valid = replay(item);
  REQUIRE_FALSE(oracle(item, valid).has_value());
  constexpr int CONTROLS = 24;
  int detected = 0;
  for (int index = 0; index < CONTROLS; ++index) {
    auto broken = valid;
    switch (index) {
      case 0: broken.exception_name = "TypeError"; break;
      case 1: broken.exception_message = "fake unexpected exception"; break;
      case 2: broken.operations.clear(); break;
      case 3: ++broken.next_id; break;
      case 4: broken.callbacks.clear(); break;
      case 5: ++broken.pending_count; break;
      case 6: ++broken.concurrent; break;
      case 7: broken.operations.front().type = AsyncOp::Type::DB_QUERY; break;
      case 8: ++broken.operations.front().callback_id; break;
      case 9: broken.callbacks.front().name_space = "db"; break;
      case 10: broken.callbacks.front().resolve = false; break;
      case 11: broken.callbacks.front().reject = false; break;
      case 12: broken.operations.front().cap_signature = "changed"; break;
      case 13: broken.operations.front().cap_args = "changed"; break;
      case 14:
        broken.operations.front().cap_user.effective_rules.clear();
        break;
      case 15: ++broken.operations.front().cap_call_depth; break;
      case 16: broken.caller_unchanged = false; break;
      case 17: broken.snapshots_owned = false; break;
      case 18: broken.immediate = JS_PROMISE_FULFILLED; break;
      case 19: broken.result = "fake wrong settlement"; break;
      case 20: broken.temporal_owned = false; break;
      case 21: broken.clean = false; break;
      case 22: broken.late_clean = false; break;
      case 23: broken.rejection_keys = false; break;
      default: FAIL("unknown oracle corruption");
    }
    REQUIRE(oracle(item, broken).has_value());
    ++detected;
  }
  REQUIRE(detected == CONTROLS);
  const auto fault =
      oracle(item, replay_with_fault(item, Fault::drop_operation));
  REQUIRE(fault == std::optional<std::string>{"operations"});
  const auto reduced = shrink(item, *fault, Fault::drop_operation);
  REQUIRE(reduced.attempts == 3);
  REQUIRE(reduced.attempts <= MAX_SHRINK_ATTEMPTS);
  REQUIRE(reduced.item.tuples.front().arity == 2);
  REQUIRE(reduced.item.tuples.front().signature == "x");
  REQUIRE(reduced.item.tuples.front().arguments.isNull());
  REQUIRE(oracle(reduced.item,
                 replay_with_fault(reduced.item, Fault::drop_operation)) ==
          fault);
  // Fixed minimal regression also passes with the injected observer fault off.
  require_observation(reduced.item, replay(reduced.item));
}

auto mapper_controls() -> void {
  using Error = plinth::capabilities::CapabilityError;
  struct Row {
    Error error;
    std::string_view code;
  };
  constexpr std::array<Row, 21> ROWS{{
      {Error::INVALID_NAMESPACE, "cap.internal"},
      {Error::INVALID_VERSION, "cap.internal"},
      {Error::INVALID_FUNCTION, "cap.internal"},
      {Error::INVALID_SCOPE, "cap.internal"},
      {Error::INVALID_PROVIDER_TYPE, "cap.internal"},
      {Error::INVALID_DESCRIPTION, "cap.internal"},
      {Error::INVALID_CAPABILITY, "cap.invalid_signature"},
      {Error::MISSING_EXTENSION_NAME, "cap.internal"},
      {Error::RESERVED_NAMESPACE, "cap.internal"},
      {Error::NAMESPACE_MISMATCH, "cap.internal"},
      {Error::CAPABILITY_EXISTS, "cap.internal"},
      {Error::CAPABILITY_NOT_FOUND, "cap.not_found"},
      {Error::RBAC_RULE_NOT_FOUND, "cap.internal"},
      {Error::USER_SCOPE_NOT_SUPPORTED, "cap.internal"},
      {Error::DB_ERROR, "cap.internal"},
      {Error::CAPABILITY_DISABLED, "cap.capability_disabled"},
      {Error::TIER3_NOT_AVAILABLE, "cap.tier3_not_available"},
      {Error::CALL_DEPTH_EXCEEDED, "cap.call_depth_exceeded"},
      {Error::PERMISSION_DENIED, "cap.permission_denied"},
      {Error::ASYNC_REQUIRED, "cap.async_required"},
      {Error::EXTENSION_DISPATCH_FAILED, "cap.internal"},
  }};
  constexpr std::string_view SIGNATURE = "notes:1:echo";
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, "notes");
  const auto settle_mapped = [&pool](const plinth::js::PromiseRejection& actual,
                                     std::string_view code,
                                     std::string_view message) {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    {
      auto promise = evaluate(*lease.get(), "cap.call('notes:1:echo')");
      REQUIRE(JS_PromiseState(lease.get()->ctx, promise.get()) ==
              JS_PROMISE_PENDING);
      const auto operations = lease.get()->take_pending_ops();
      REQUIRE(operations.size() == 1);
      REQUIRE(operations.front().type == AsyncOp::Type::CAP_CALL);
      REQUIRE(operations.front().callback_id == 0);
      REQUIRE(operations.front().cap_args.isNull());
      lease.get()->reject(operations.front().callback_id, actual);
      REQUIRE(JS_PromiseState(lease.get()->ctx, promise.get()) ==
              JS_PROMISE_REJECTED);
      OwnedValue result{lease.get()->ctx,
                        JS_PromiseResult(lease.get()->ctx, promise.get())};
      Json::Value expected_result{Json::objectValue};
      expected_result["code"] = std::string{code};
      expected_result["message"] = std::string{message};
      REQUIRE(observed_json(lease.get()->ctx, result.get()) == expected_result);
      REQUIRE(rejection_shape(lease.get()->ctx, result.get()));
      REQUIRE(drain_jobs(*lease.get()));
      REQUIRE(lease.get()->callbacks.empty());
      REQUIRE(lease.get()->pending_ops.empty());
      REQUIRE_FALSE(JS_HasException(lease.get()->ctx));
    }
    lease.release();
  };
  std::size_t count = 0;
  for (const auto& row : ROWS) {
    const auto actual =
        plinth::js::capability_error_to_rejection(row.error, SIGNATURE);
    REQUIRE(actual.code == row.code);
    REQUIRE(actual.message == std::string{row.code} + ": notes:1:echo");
    REQUIRE_FALSE(actual.sqlstate.has_value());
    settle_mapped(actual, row.code, std::string{row.code} + ": notes:1:echo");
    ++count;
  }
  REQUIRE(count == 21);
  struct Detail {
    std::string code;
    std::string message;
    std::string expected_code;
    std::string expected_message;
  };
  const std::array<Detail, 3> DETAILS{{
      {"cap.handler_threw", "fake bounded handler message", "cap.handler_threw",
       "fake bounded handler message"},
      {"cap.handler_threw", "", "cap.handler_threw",
       "cap.handler_threw: notes:1:echo"},
      {"", "ignored fake detail", "cap.internal", "cap.internal: notes:1:echo"},
  }};
  for (const auto& detail : DETAILS) {
    const auto actual = plinth::js::capability_error_to_rejection(
        Error::EXTENSION_DISPATCH_FAILED, SIGNATURE, detail.code,
        detail.message);
    REQUIRE(actual.code == detail.expected_code);
    REQUIRE(actual.message == detail.expected_message);
    REQUIRE_FALSE(actual.sqlstate.has_value());
    settle_mapped(actual, detail.expected_code, detail.expected_message);
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

auto fixed_precedence_controls() -> void {
  constexpr std::size_t REGRESSIONS = 4;
  std::size_t executed = 0;
  auto unsupported = make_case(SEEDS.front(), 22);
  unsupported.tuples.front().arguments_literal = "Symbol('fake')";
  unsupported.tuples.front().error = PAYLOAD_TYPE;
  require_observation(unsupported, replay(unsupported));
  ++executed;
  auto signature_first = unsupported;
  signature_first.tuples.front().signature_literal = "false";
  signature_first.tuples.front().error = SIGNATURE_TYPE;
  require_observation(signature_first, replay(signature_first));
  ++executed;
  // A single plain object with one back-edge is a bounded fixed graph, not
  // part of the generated depth-three JSON trees. The existing conversion
  // depth guard rejects it; this fixture introduces no conversion policy.
  auto cycle = make_case(SEEDS.front(), 22);
  cycle.cancelled = false;
  cycle.tuples.front().arguments_literal =
      "(()=>{const value={fake:1};value.self=value;return value})()";
  cycle.tuples.front().error = PAYLOAD_TYPE;
  require_observation(cycle, replay(cycle));
  ++executed;
  cycle.cancelled = true;
  require_observation(cycle, replay(cycle));
  ++executed;
  REQUIRE(executed == REGRESSIONS);
}

auto fixed_bridge_unavailable_control() -> void {
  // Restore opaque state even if an assertion throws. This guard is scoped
  // inside the external JS-value owners and dies before the lease is returned.
  class OpaqueRestore {
   public:
    explicit OpaqueRestore(JSContext* ctx)
        : context(ctx), original(JS_GetContextOpaque(ctx)) {
      JS_SetContextOpaque(context, nullptr);
    }
    ~OpaqueRestore() { JS_SetContextOpaque(context, original); }
    OpaqueRestore(const OpaqueRestore&) = delete;
    auto operator=(const OpaqueRestore&) -> OpaqueRestore& = delete;
    OpaqueRestore(OpaqueRestore&&) = delete;
    auto operator=(OpaqueRestore&&) -> OpaqueRestore& = delete;

   private:
    JSContext* context;
    void* original;
  };
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, "notes");
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    {
      OwnedValue global{lease.get()->ctx, JS_GetGlobalObject(lease.get()->ctx)};
      OwnedValue cap{lease.get()->ctx,
                     JS_GetPropertyStr(lease.get()->ctx, global.get(), "cap")};
      OwnedValue function{
          lease.get()->ctx,
          JS_GetPropertyStr(lease.get()->ctx, cap.get(), "call")};
      OwnedValue signature{lease.get()->ctx,
                           JS_NewString(lease.get()->ctx, "notes:1:echo")};
      REQUIRE(JS_IsFunction(lease.get()->ctx, function.get()));
      {
        const OpaqueRestore temporarily_empty{lease.get()->ctx};
        REQUIRE(JS_GetContextOpaque(lease.get()->ctx) == nullptr);
        JSValue argument = signature.get();
        OwnedValue result{
            lease.get()->ctx,
            JS_Call(lease.get()->ctx, function.get(), cap.get(), 1, &argument)};
        REQUIRE(JS_IsException(result.get()));
        OwnedValue exception{lease.get()->ctx,
                             JS_GetException(lease.get()->ctx)};
        REQUIRE(property_text(lease.get()->ctx, exception.get(), "name") ==
                "TypeError");
        REQUIRE(property_text(lease.get()->ctx, exception.get(), "message") ==
                "cap.call: bridge context unavailable");
      }
      REQUIRE(JS_GetContextOpaque(lease.get()->ctx) == lease.get());
    }
    REQUIRE(lease.get()->callbacks.empty());
    REQUIRE(lease.get()->pending_ops.empty());
    REQUIRE(lease.get()->next_callback_id == 0);
    REQUIRE(lease.get()->concurrent_async_ops == 0);
    REQUIRE_FALSE(JS_HasException(lease.get()->ctx));
    REQUIRE_FALSE(JS_IsJobPending(lease.get()->rt));
    lease.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
}

auto ownership_controls() -> void {
  plinth::Config config{};
  const auto user = fake_user();
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   &user, "notes");
  {
    Lease settled{pool};
    REQUIRE(settled.get() != nullptr);
    {
      auto marker = evaluate(*settled.get(), "globalThis.capReleaseMarker=111");
      REQUIRE_FALSE(JS_IsException(marker.get()));
    }
    const auto item = make_case(SEEDS.front(), 0);
    require_observation(item, execute(*settled.get(), item));
    settled.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  {
    Lease reused{pool};
    REQUIRE(reused.get() != nullptr);
    REQUIRE(reused.get()->next_callback_id == 0);
    REQUIRE(reused.get()->call_depth == 0);
    REQUIRE(reused.get()->callbacks.empty());
    REQUIRE(reused.get()->pending_ops.empty());
    REQUIRE_FALSE(JS_IsJobPending(reused.get()->rt));
    {
      auto clean =
          evaluate(*reused.get(),
                   "typeof capReleaseMarker==='undefined'&&typeof "
                   "__cap_args==='undefined'&&typeof "
                   "cap.call==='function'&&typeof cap.batch==='function'");
      REQUIRE_FALSE(JS_IsException(clean.get()));
      REQUIRE(JS_ToBool(reused.get()->ctx, clean.get()) == 1);
    }
    reused.release();
  }
  {
    Lease dirty{pool};
    REQUIRE(dirty.get() != nullptr);
    {
      auto promise =
          evaluate(*dirty.get(), "cap.call('notes:1:echo',{fake:1})");
      REQUIRE(JS_PromiseState(dirty.get()->ctx, promise.get()) ==
              JS_PROMISE_PENDING);
      REQUIRE(dirty.get()->callbacks.size() == 1);
      REQUIRE(dirty.get()->pending_ops.size() == 1);
    }
    dirty.release(); // Persistent unresolved callback owners force destruction.
  }
  REQUIRE(pool.free_count() == 0);
  REQUIRE(pool.active_count() == 0);
  {
    Lease cancelled{pool};
    REQUIRE(cancelled.get() != nullptr);
    REQUIRE(cancelled.get()->callbacks.empty());
    cancelled.get()->cancelled.store(true);
    cancelled.release();
  }
  REQUIRE(pool.free_count() == 0);
  REQUIRE(pool.active_count() == 0);
  pool.rebuild();
  REQUIRE(pool.free_count() == 1);
  {
    Lease held{pool};
    REQUIRE(held.get() != nullptr);
    {
      auto marker = evaluate(*held.get(), "globalThis.capRebuildMarker=321");
      REQUIRE_FALSE(JS_IsException(marker.get()));
    }
    pool.rebuild();
    REQUIRE(pool.active_count() == 1);
    REQUIRE(pool.free_count() == 1);
    {
      auto retained = evaluate(*held.get(), "capRebuildMarker===321");
      REQUIRE_FALSE(JS_IsException(retained.get()));
      REQUIRE(JS_ToBool(held.get()->ctx, retained.get()) == 1);
    }
    REQUIRE_FALSE(pool.shutdown(100ms));
    REQUIRE(pool.acquire() == nullptr);
    REQUIRE(pool.active_count() == 1);
    {
      auto alive = evaluate(
          *held.get(), "typeof cap.call==='function'&&capRebuildMarker===321");
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

TEST_CASE("QuickJS capability calls have a bounded deterministic admission and "
          "batch corpus",
          "[js][cap][corpus][isolated-cap]") {
  INFO("seeds=" << SEEDS.size() << " families=" << FAMILIES
                << " max_batch=" << MAX_BATCH << " max_tuple=" << MAX_TUPLE
                << " max_signature_bytes=" << MAX_SIGNATURE_BYTES
                << " max_depth=" << MAX_DEPTH << " max_nodes=" << MAX_NODES
                << " max_string_bytes=" << MAX_STRING_BYTES << " max_jobs="
                << MAX_JOBS << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS);
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  std::array<int, FAMILIES> coverage{};
  int cases = 0;
  int calls = 0;
  int batches = 0;
  int exceptions = 0;
  int cancelled = 0;
  int prefixes = 0;
  int temporal = 0;
  for (const auto seed : SEEDS) {
    for (int family = 0; family < FAMILIES; ++family) {
      INFO("seed=" << seed << " family=" << family);
      const auto item = make_case(seed, family);
      REQUIRE(item.tuples.size() <= MAX_BATCH);
      for (const auto& tuple : item.tuples) {
        REQUIRE(tuple.signature.size() <= MAX_SIGNATURE_BYTES);
        REQUIRE(tuple.arity <= MAX_TUPLE);
        Bounds bounds;
        measure(tuple.arguments, 0, bounds);
        REQUIRE(bounds.depth <= MAX_DEPTH);
        REQUIRE(bounds.nodes <= MAX_NODES);
        REQUIRE(bounds.string_bytes <= MAX_STRING_BYTES);
      }
      const auto want = expected(item);
      // Both observations execute fresh registered bindings on owned runtimes;
      // replay never substitutes a cached observation for an actual run.
      require_observation(item, replay(item));
      require_observation(item, replay(item));
      ++coverage.at(static_cast<std::size_t>(family));
      ++cases;
      item.batch ? ++batches : ++calls;
      exceptions += want.exception.empty() ? 0 : 1;
      cancelled += item.cancelled ? 1 : 0;
      prefixes += !want.exception.empty() && !want.admitted.empty() ? 1 : 0;
      temporal += item.temporal_rejection ? 1 : 0;
    }
  }
  REQUIRE(cases == 192);
  REQUIRE(calls == 96);
  REQUIRE(batches == 96);
  REQUIRE(exceptions == 84);
  REQUIRE(cancelled == 24);
  REQUIRE(prefixes == 24);
  REQUIRE(temporal == 4);
  for (const auto count : coverage) {
    REQUIRE(count == 4);
  }
  negative_controls();
  mapper_controls();
  fixed_precedence_controls();
  fixed_bridge_unavailable_control();
  ownership_controls();
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}
