// SPDX-License-Identifier: MIT
//
// Ordinary query/exec admission on actual registered QuickJS functions.
// Host settlement is explicitly simulated, not SQL/driver execution, database
// authorization, transaction orchestration or production cancellation.
// No database client is opened. All generators below have finite test budgets.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/extension_database.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <limits>
#include <memory>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using plinth::js::AsyncOp;
using plinth::js::BridgeContext;
using plinth::js::ExtensionDatabaseClients;
using plinth::js::RuntimePool;

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1510'0000'0000'0001ULL, 0x1510'0000'0000'0027ULL,
    0x1510'0000'0000'00C6ULL, 0x1510'0000'0000'BEEFULL};
constexpr int FAMILIES = 48;
constexpr std::size_t MAX_PARAMS = 4;
constexpr std::size_t MAX_SQL_BYTES = 128;
constexpr std::size_t MAX_VALUE_BYTES = 64;
constexpr int MAX_ARGUMENTS =
    4; // Three public positions plus one ignored extra.
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

class OpaqueRestore {
 public:
  OpaqueRestore(JSContext* ctx, bool clear)
      : context(ctx), original(JS_GetContextOpaque(ctx)) {
    if (clear) {
      JS_SetContextOpaque(context, nullptr);
    }
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

class MetadataRestore {
 public:
  explicit MetadataRestore(BridgeContext& ctx)
      : context(ctx), extension(ctx.extension_name),
        manager(ctx.extension_database_clients) {}
  ~MetadataRestore() {
    context.extension_name = std::move(extension);
    context.extension_database_clients = std::move(manager);
    context.cancelled.store(false);
  }
  MetadataRestore(const MetadataRestore&) = delete;
  auto operator=(const MetadataRestore&) -> MetadataRestore& = delete;
  MetadataRestore(MetadataRestore&&) = delete;
  auto operator=(MetadataRestore&&) -> MetadataRestore& = delete;

 private:
  BridgeContext& context;
  std::string extension;
  std::shared_ptr<ExtensionDatabaseClients> manager;
};

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

auto generated_text(std::uint64_t& state) -> std::string {
  constexpr std::array<std::string_view, 4> ATOMS{
      "a", "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80"};
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

struct Param {
  Json::Value value;
  std::optional<std::string> source;
  std::size_t byte_count = 0;
};

auto bytes_param(std::uint64_t& state, bool subview) -> Param {
  const auto length =
      static_cast<std::size_t>(next(state) % (subview ? 63U : 65U));
  std::string bytes;
  Json::Value storage{Json::arrayValue};
  if (subview) {
    storage.append(231);
  }
  for (std::size_t index = 0; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(next(state) % 256U);
    bytes.push_back(static_cast<char>(byte));
    storage.append(static_cast<int>(byte));
  }
  if (subview) {
    storage.append(199);
  }
  Json::Value tagged{Json::objectValue};
  tagged["__bytea__"] = bytes;
  const auto source =
      "new Uint8Array(" + json_source(storage) + ")" +
      (subview ? ".subarray(1," + std::to_string(length + 1) + ")" : "");
  return {.value = std::move(tagged), .source = source, .byte_count = length};
}

auto generated_params(std::uint64_t& state) -> std::vector<Param> {
  std::vector<Param> result;
  for (std::size_t index = 0; index < MAX_PARAMS; ++index) {
    Param param;
    switch (next(state) % 6U) {
      case 0: param.value = Json::Value{}; break;
      case 1: param.value = true; break;
      case 2: param.value = false; break;
      case 3: param.value = static_cast<int>(next(state) % 129U) - 64; break;
      case 4: {
        constexpr std::array<double, 6> FRACTIONS{-0.75, -0.5, -0.25,
                                                  0.25,  0.5,  0.75};
        param.value = FRACTIONS.at(
            static_cast<std::size_t>(next(state) % FRACTIONS.size()));
        break;
      }
      case 5: param.value = generated_text(state); break;
      default: FAIL("unknown safe parameter kind");
    }
    result.push_back(std::move(param));
  }
  return result;
}

struct CorpusCase {
  int family = 0;
  bool exec = false;
  int argc = 2;
  std::string sql = "SELECT $1";
  std::optional<std::string> sql_literal;
  std::vector<Param> params;
  std::optional<std::string> params_literal;
  std::string options = "undefined";
  bool silent = false;
  bool cancelled = false;
  bool unavailable = false;
  bool simulate_rejection = false;
  std::string extension = "notes";
  std::string error;
};

auto method(const CorpusCase& item) -> std::string {
  return item.exec ? "db.exec" : "db.query";
}

auto params_source(const CorpusCase& item) -> std::string {
  if (item.params_literal.has_value()) {
    return *item.params_literal;
  }
  std::string result = "[";
  for (std::size_t index = 0; index < item.params.size(); ++index) {
    if (index != 0) {
      result += ",";
    }
    const auto& param = item.params[index];
    result += param.source.value_or(json_source(param.value));
  }
  return result + "]";
}

auto input_source(const CorpusCase& item) -> std::string {
  std::string source = "[";
  if (item.argc >= 1) {
    source += item.sql_literal.value_or(json_source(item.sql));
  }
  if (item.argc >= 2) {
    source += "," + params_source(item);
  }
  if (item.argc >= 3) {
    source += "," + item.options;
  }
  if (item.argc >= 4) {
    source += ",37";
  }
  return source + "]";
}

auto values(const CorpusCase& item) -> std::vector<Json::Value> {
  std::vector<Json::Value> result;
  result.reserve(item.params.size());
  for (const auto& param : item.params) {
    result.push_back(param.value);
  }
  return result;
}

auto make_case(std::uint64_t seed, int family) -> CorpusCase {
  CorpusCase item;
  item.family = family;
  item.exec = family >= 24;
  auto state = seed + static_cast<std::uint64_t>(family);
  item.params = generated_params(state);
  item.extension = (seed & 1U) != 0 ? "notes" : "";
  if (!item.exec) {
    switch (family) {
      case 0:
        item.argc = 1;
        item.params.clear();
        break;
      case 1:
        item.params_literal = "undefined";
        item.params.clear();
        break;
      case 2:
        item.params_literal = "null";
        item.params.clear();
        break;
      case 3: item.params.clear(); break;
      case 4:
        item.params = {Param{}, Param{.source = "undefined"},
                       Param{.value = true}, Param{.value = false}};
        break;
      case 5: break;
      case 6:
        item.params = {Param{.value = -1.5}, Param{.value = -0.25},
                       Param{.value = 0}, Param{.value = 0.5}};
        break;
      case 7:
        item.params = {Param{.value = std::string(MAX_VALUE_BYTES, 'a')}};
        break;
      case 8: item.params = {Param{.value = generated_text(state)}}; break;
      case 9: item.params = {bytes_param(state, false)}; break;
      case 10: item.params = {bytes_param(state, true)}; break;
      case 11:
        item.params_literal = "[null,,true]";
        item.params = {Param{}, Param{}, Param{.value = true}};
        break;
      case 12: item.sql = " "; break;
      case 13: item.sql = "SELECT '\xC3\xA9'"; break;
      case 14:
        item.sql = "SELECT " + std::string(MAX_SQL_BYTES - 7, 's');
        break;
      case 15:
        item.argc = 0;
        item.error = "db.query: sql must be a string";
        break;
      case 16:
        item.sql_literal = "23";
        item.error = "db.query: sql must be a string";
        break;
      case 17:
        item.sql.clear();
        item.error = "db.query: sql must be non-empty";
        break;
      case 18:
        item.sql = std::string{"SELECT\0more", 11};
        item.error = "db.query: sql must not contain NUL";
        break;
      case 19:
        item.params_literal = "({fake:1})";
        item.error = "db: params must be an array of scalars";
        break;
      case 20:
        item.params = {Param{.value = 1}, Param{.source = "({fake:1})"}};
        item.error = "db: unsupported parameter type at index 1";
        break;
      case 21:
        item.params = {Param{}, Param{.value = true},
                       Param{.value = std::string{"a\0b", 3}}};
        item.error = "db: TEXT parameter at index 2 contains NUL";
        break;
      case 22: item.cancelled = true; break;
      case 23:
        item.argc = 4;
        item.options = "false";
        item.simulate_rejection = true;
        break;
      default: FAIL("unknown query family");
    }
    return item;
  }
  item.argc = 3;
  switch (family - 24) {
    case 0:
      item.argc = 1;
      item.params.clear();
      break;
    case 1:
      item.params_literal = "undefined";
      item.params.clear();
      break;
    case 2:
      item.params_literal = "null";
      item.params.clear();
      item.options = "null";
      break;
    case 3:
      item.params.clear();
      item.options = "({})";
      break;
    case 4: item.options = "({fake:'ignored'})"; break;
    case 5: item.options = "({silent:undefined})"; break;
    case 6: item.options = "({silent:null})"; break;
    case 7:
      item.options = "({silent:true})";
      item.silent = true;
      break;
    case 8: item.options = "({silent:false})"; break;
    case 9: item.options = "({silent:0})"; break;
    case 10:
      item.options = "({silent:1})";
      item.silent = true;
      break;
    case 11: item.options = "({silent:''})"; break;
    case 12:
      item.options = "({silent:'fake'})";
      item.silent = true;
      break;
    case 13:
      item.options = "({silent:[]})";
      item.silent = true;
      break;
    case 14:
      item.options = "({silent:{}})";
      item.silent = true;
      break;
    case 15:
      item.options = "23";
      item.error = "db.exec: opts must be an object";
      break;
    case 16:
      item.argc = 0;
      item.error = "db.exec: sql must be a string";
      break;
    case 17:
      item.sql_literal = "false";
      item.params_literal = "false";
      item.options = "23";
      item.error = "db.exec: sql must be a string";
      break;
    case 18:
      item.sql.clear();
      item.params_literal = "false";
      item.error = "db.exec: sql must be non-empty";
      break;
    case 19:
      item.sql = std::string{"SELECT\0more", 11};
      item.params_literal = "false";
      item.error = "db.exec: sql must not contain NUL";
      break;
    case 20:
      item.params_literal = "({fake:1})";
      item.options = "23";
      item.error = "db: params must be an array of scalars";
      break;
    case 21:
      item.params = {Param{}, Param{.source = "Symbol('fake')"}};
      item.options = "23";
      item.error = "db: unsupported parameter type at index 1";
      break;
    case 22:
      item.cancelled = true;
      item.options = "({silent:true})";
      item.silent = true;
      break;
    case 23:
      item.argc = 4;
      item.options = "({silent:false})";
      item.params = {bytes_param(state, true)};
      item.simulate_rejection = true;
      break;
    default: FAIL("unknown exec family");
  }
  return item;
}

auto evaluate(BridgeContext& context, const std::string& source) -> OwnedValue {
  return {context.ctx, JS_Eval(context.ctx, source.data(), source.size(),
                               "<db-admission-corpus>", JS_EVAL_TYPE_GLOBAL)};
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

struct Operation {
  AsyncOp::Type type = AsyncOp::Type::DB_QUERY;
  int id = 0;
  std::string sql;
  std::vector<Json::Value> params;
  bool silent = false;
  std::string extension;
  bool manager_pointer = false;
  bool manager_owner = false;
  bool unrelated = true;
};

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
  std::vector<Operation> operations;
  std::vector<Callback> callbacks;
  bool caller_unchanged = true;
  bool snapshots_owned = true;
  bool rejection_keys = true;
  bool reaction = true;
  bool clean = true;
  bool late_clean = true;
  Json::Value result;
};

auto rejected(const CorpusCase& item) -> bool {
  return item.cancelled || item.simulate_rejection;
}

auto result_value(const CorpusCase& item) -> Json::Value {
  Json::Value result{Json::objectValue};
  if (rejected(item)) {
    result["code"] = item.cancelled ? "db.cancelled" : "db.test_rejected";
    result["message"] =
        item.cancelled ? "execution cancelled" : "fake host rejection";
    return result;
  }
  result["row_count"] = 1;
  if (!item.exec) {
    result["rows"] = Json::Value{Json::arrayValue};
    Json::Value row{Json::objectValue};
    row["fake"] = "fixture row, not a PG conversion";
    result["rows"].append(std::move(row));
  }
  return result;
}

auto admitted(const CorpusCase& item) -> bool {
  return item.error.empty() && !item.cancelled;
}

auto oracle(const CorpusCase& item, const Observation& observation)
    -> std::optional<std::string> {
  if (observation.exception_name != (item.error.empty() ? "" : "TypeError") ||
      observation.exception_message != item.error) {
    return "exception";
  }
  const int count = admitted(item) ? 1 : 0;
  if (observation.operations.size() != static_cast<std::size_t>(count)) {
    return "operations";
  }
  if (observation.next_id != INITIAL_ID + count ||
      observation.callbacks.size() != static_cast<std::size_t>(count) ||
      observation.pending_count != 2 * count || observation.concurrent != 0) {
    return "inventory";
  }
  if (count != 0) {
    const auto& callback = observation.callbacks.front();
    const auto& operation = observation.operations.front();
    if (callback.id != INITIAL_ID || callback.name_space != "db" ||
        !callback.resolve || !callback.reject || operation.id != INITIAL_ID ||
        operation.type !=
            (item.exec ? AsyncOp::Type::DB_EXEC : AsyncOp::Type::DB_QUERY)) {
      return "callback";
    }
    if (operation.sql != item.sql || operation.params != values(item) ||
        operation.silent != item.silent ||
        operation.extension != item.extension || !operation.manager_pointer ||
        !operation.manager_owner || !operation.unrelated) {
      return "snapshot";
    }
  }
  if (!observation.caller_unchanged || !observation.snapshots_owned) {
    return "ownership";
  }
  if (item.error.empty() &&
      (observation.immediate !=
           (item.cancelled ? JS_PROMISE_REJECTED : JS_PROMISE_PENDING) ||
       observation.final_state !=
           (rejected(item) ? JS_PROMISE_REJECTED : JS_PROMISE_FULFILLED) ||
       observation.result != result_value(item) ||
       !observation.rejection_keys || !observation.reaction)) {
    return "settlement";
  }
  if (!observation.clean || !observation.late_clean) {
    return "cleanup";
  }
  return std::nullopt;
}

auto observe_operation(const AsyncOp& operation,
                       const std::shared_ptr<ExtensionDatabaseClients>& manager)
    -> Operation {
  const auto& user = operation.cap_user;
  return {
      .type = operation.type,
      .id = operation.callback_id,
      .sql = operation.sql,
      .params = operation.sql_params,
      .silent = operation.silent,
      .extension = operation.bc_extension_name,
      .manager_pointer = operation.extension_database_clients != nullptr &&
                         operation.extension_database_clients == manager,
      .manager_owner =
          !operation.extension_database_clients.owner_before(manager) &&
          !manager.owner_before(operation.extension_database_clients),
      .unrelated =
          operation.audit_event_type.empty() &&
          operation.audit_payload.isNull() && operation.audit_user_id.empty() &&
          operation.audit_session_id.empty() &&
          operation.audit_ip_address.empty() &&
          operation.cap_signature.empty() && operation.cap_args.isNull() &&
          operation.cap_call_depth == 0 && user.user_id.empty() &&
          user.username.empty() && user.auth_type == "anonymous" &&
          user.effective_rules.empty() && user.session_id.empty() &&
          user.ip_address.empty() && operation.pubsub_channel.empty() &&
          operation.pubsub_payload.isNull() && operation.batch_scope_id == 0 &&
          operation.batch_pinned_conn == nullptr &&
          operation.rollback_error.isNull()};
}

auto execute(BridgeContext& context, const CorpusCase& item) -> Observation {
  REQUIRE(context.pending_ops.empty());
  REQUIRE(context.callbacks.empty());
  REQUIRE(context.persistent_callbacks.empty());
  REQUIRE_FALSE(JS_IsJobPending(context.rt));
  MetadataRestore restore{context};
  context.extension_name = item.extension;
  context.next_callback_id = INITIAL_ID;
  const auto manager = context.extension_database_clients;
  REQUIRE(manager != nullptr);
  auto setup = evaluate(
      context, "globalThis.__db_args=" + input_source(item) +
                   ";globalThis.__db_hits=0;globalThis.__db_seen=undefined;"
                   "globalThis.__db_observe=(value)=>{__db_hits++;__db_seen="
                   "value;return value}");
  REQUIRE_FALSE(JS_IsException(setup.get()));
  OwnedValue global{context.ctx, JS_GetGlobalObject(context.ctx)};
  OwnedValue db{context.ctx,
                JS_GetPropertyStr(context.ctx, global.get(), "db")};
  OwnedValue function{
      context.ctx,
      JS_GetPropertyStr(context.ctx, db.get(), item.exec ? "exec" : "query")};
  OwnedValue input{context.ctx,
                   JS_GetPropertyStr(context.ctx, global.get(), "__db_args")};
  REQUIRE(JS_IsFunction(context.ctx, function.get()));
  REQUIRE(item.argc <= MAX_ARGUMENTS);
  std::array<JSValue, MAX_ARGUMENTS> arguments{JS_UNDEFINED, JS_UNDEFINED,
                                               JS_UNDEFINED, JS_UNDEFINED};
  for (int index = 0; index < item.argc; ++index) {
    arguments.at(static_cast<std::size_t>(index)) = JS_GetPropertyUint32(
        context.ctx, input.get(), static_cast<std::uint32_t>(index));
  }
  OpaqueRestore opaque{context.ctx, item.unavailable};
  context.cancelled.store(item.cancelled);
  OwnedValue promise{context.ctx, JS_Call(context.ctx, function.get(), db.get(),
                                          item.argc, arguments.data())};
  for (int index = 0; index < item.argc; ++index) {
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
  observation.next_id = context.next_callback_id;
  observation.pending_count = context.pending_op_count();
  observation.concurrent = context.concurrent_async_ops;
  observation.caller_unchanged = context.extension_name == item.extension &&
                                 context.extension_database_clients == manager;
  for (const auto& [id, callback] : context.callbacks) {
    observation.callbacks.push_back(
        {id, callback.ns_for_cancellation,
         JS_IsFunction(context.ctx, callback.resolve),
         JS_IsFunction(context.ctx, callback.reject)});
  }
  std::ranges::sort(observation.callbacks, {}, &Callback::id);
  // Direct host admission was observed with the cancellation flag unchanged.
  // Job draining and settlement below are explicit owning-host simulations.
  context.cancelled.store(false);
  auto mutation = evaluate(
      context,
      "for(const value of __db_args){"
      "if(value instanceof Uint8Array)value.fill(199);"
      "else if(Array.isArray(value)){for(const element of value){"
      "if(element instanceof Uint8Array)element.fill(199)}value.length=0}"
      "else if(value&&typeof value==='object')value.silent='changed'}"
      "__db_args.length=0");
  REQUIRE_FALSE(JS_IsException(mutation.get()));
  context.extension_name = "changed-owner";
  context.extension_database_clients.reset();
  auto operations = context.take_pending_ops();
  for (const auto& operation : operations) {
    observation.operations.push_back(observe_operation(operation, manager));
  }
  observation.snapshots_owned = operations.size() == (admitted(item) ? 1U : 0U);
  if (!operations.empty()) {
    observation.snapshots_owned =
        observation.snapshots_owned && operations.front().sql == item.sql &&
        operations.front().sql_params == values(item) &&
        operations.front().silent == item.silent &&
        operations.front().bc_extension_name == item.extension &&
        operations.front().extension_database_clients == manager;
  }
  if (!JS_IsException(promise.get())) {
    OwnedValue then{context.ctx,
                    JS_GetPropertyStr(context.ctx, promise.get(), "then")};
    OwnedValue handler{context.ctx, JS_GetPropertyStr(context.ctx, global.get(),
                                                      "__db_observe")};
    std::array<JSValue, 2> handlers{handler.get(), handler.get()};
    OwnedValue observer{
        context.ctx,
        JS_Call(context.ctx, then.get(), promise.get(), 2, handlers.data())};
    REQUIRE_FALSE(JS_IsException(observer.get()));
    // The fixture applies the host's accounting increment, never SQL dispatch.
    context.concurrent_async_ops = static_cast<int>(operations.size());
    if (!operations.empty()) {
      if (item.simulate_rejection) {
        context.reject(operations.front().callback_id,
                       {.code = "db.test_rejected",
                        .message = "fake host rejection",
                        .sqlstate = std::nullopt});
      } else {
        context.resolve(operations.front().callback_id, result_value(item));
      }
    }
    REQUIRE(drain_jobs(context));
    observation.final_state = JS_PromiseState(context.ctx, promise.get());
    if (observation.final_state != JS_PROMISE_PENDING) {
      OwnedValue result{context.ctx,
                        JS_PromiseResult(context.ctx, promise.get())};
      observation.result = observed_json(context.ctx, result.get());
      if (observation.final_state == JS_PROMISE_REJECTED) {
        observation.rejection_keys = rejection_shape(context.ctx, result.get());
      }
    }
    observation.reaction =
        JS_PromiseState(context.ctx, observer.get()) == JS_PROMISE_FULFILLED;
    if (observation.reaction) {
      OwnedValue seen{context.ctx,
                      JS_PromiseResult(context.ctx, observer.get())};
      OwnedValue hits{context.ctx, JS_GetPropertyStr(context.ctx, global.get(),
                                                     "__db_hits")};
      OwnedValue recorded{
          context.ctx,
          JS_GetPropertyStr(context.ctx, global.get(), "__db_seen")};
      int count = 0;
      REQUIRE(JS_ToInt32(context.ctx, &count, hits.get()) == 0);
      observation.reaction =
          count == 1 &&
          observed_json(context.ctx, seen.get()) == result_value(item) &&
          observed_json(context.ctx, recorded.get()) == result_value(item);
    }
  }
  observation.clean =
      context.callbacks.empty() && context.pending_ops.empty() &&
      context.concurrent_async_ops == 0 &&
      context.persistent_callbacks.empty() &&
      context.inflight_detached.load() == 0 && !JS_IsJobPending(context.rt) &&
      !JS_HasException(context.ctx);
  if (!operations.empty()) {
    const int id = operations.front().callback_id;
    context.resolve(id, result_value(item));
    context.reject(id, {.code = "db.test_late",
                        .message = "fake late host outcome",
                        .sqlstate = std::nullopt});
    OwnedValue late{context.ctx,
                    JS_NewString(context.ctx, "fake owned late result")};
    context.resolve_with_js_value(id, JS_DupValue(context.ctx, late.get()));
    observation.late_clean =
        text_value(context.ctx, late.get()) == "fake owned late result" &&
        context.callbacks.empty() && context.pending_ops.empty() &&
        context.concurrent_async_ops == 0 &&
        context.next_callback_id == observation.next_id &&
        !JS_IsJobPending(context.rt) && !JS_HasException(context.ctx);
  }
  // Returned observations own only copied scalar/JSON data, not runtime refs
  // or lazy-manager aliases. Actual operation owners die while the lease lives.
  operations.clear();
  return observation;
}

auto replay(const CorpusCase& item) -> Observation {
  plinth::Config config{};
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   nullptr, "notes");
  Observation observation;
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    const auto manager = lease.get()->extension_database_clients;
    observation = execute(*lease.get(), item);
    REQUIRE(JS_GetContextOpaque(lease.get()->ctx) == lease.get());
    REQUIRE(lease.get()->extension_name == "notes");
    REQUIRE(lease.get()->extension_database_clients == manager);
    lease.release(); // execute's JS-value and operation owners are all gone.
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
  if (!admitted(item) || item.exec) {
    return {.item = std::move(item), .attempts = 0};
  }
  for (int step = 0; step < 3 && attempts < MAX_SHRINK_ATTEMPTS; ++step) {
    auto candidate = item;
    switch (step) {
      case 0: candidate.argc = 2; break;
      case 1:
        candidate.params.clear();
        candidate.params_literal.reset();
        break;
      case 2:
        candidate.sql = "x";
        candidate.sql_literal.reset();
        break;
      default: FAIL("unknown reduction step");
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
    INFO("bounded failure=" << *failure << " attempts=" << reduced.attempts
                            << " input=" << input_source(reduced.item));
  }
  REQUIRE_FALSE(failure.has_value());
}

auto negative_controls() -> void {
  const auto item = make_case(SEEDS.front(), 23);
  const auto valid = replay(item);
  require_observation(item, valid);
  constexpr int CONTROLS = 29;
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
      case 7: broken.operations.front().type = AsyncOp::Type::CAP_CALL; break;
      case 8: ++broken.operations.front().id; break;
      case 9: ++broken.callbacks.front().id; break;
      case 10: broken.callbacks.front().name_space = "cap"; break;
      case 11: broken.callbacks.front().resolve = false; break;
      case 12: broken.callbacks.front().reject = false; break;
      case 13: broken.operations.front().sql = "changed"; break;
      case 14: broken.operations.front().params.clear(); break;
      case 15: broken.operations.front().silent = true; break;
      case 16: broken.operations.front().extension = "changed"; break;
      case 17: broken.operations.front().manager_pointer = false; break;
      case 18: broken.operations.front().manager_owner = false; break;
      case 19: broken.operations.front().unrelated = false; break;
      case 20: broken.caller_unchanged = false; break;
      case 21: broken.snapshots_owned = false; break;
      case 22: broken.immediate = JS_PROMISE_FULFILLED; break;
      case 23: broken.result = "fake wrong settlement"; break;
      case 24: broken.reaction = false; break;
      case 25: broken.clean = false; break;
      case 26: broken.late_clean = false; break;
      case 27: broken.final_state = JS_PROMISE_PENDING; break;
      case 28: broken.rejection_keys = false; break;
      default: FAIL("unknown corruption control");
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
  REQUIRE(reduced.item.argc == 2);
  REQUIRE(reduced.item.sql == "x");
  REQUIRE(reduced.item.params.empty());
  REQUIRE(oracle(reduced.item,
                 replay_with_fault(reduced.item, Fault::drop_operation)) ==
          fault);
  require_observation(reduced.item,
                      replay(reduced.item)); // Minimal, fault off.
}

auto fixed_controls() -> void {
  constexpr std::array<std::string_view, 6> UNSUPPORTED{
      "({fake:1})", "[]", "(()=>1)", "Symbol('fake')", "1n", "new Date(0)"};
  int unsupported_count = 0;
  int unavailable_count = 0;
  int crossed_count = 0;
  for (const bool exec : {false, true}) {
    for (const auto source : UNSUPPORTED) {
      for (const bool cancelled : {false, true}) {
        auto item = make_case(SEEDS.front(), exec ? 24 : 0);
        item.argc = exec ? 3 : 2;
        item.params = {Param{.value = 1}, Param{.source = std::string{source}}};
        item.options =
            exec ? "23" : "undefined"; // Params fail before exec opts.
        item.cancelled = cancelled;
        item.error = "db: unsupported parameter type at index 1";
        require_observation(item, replay(item));
        ++unsupported_count;
      }
    }
    for (const bool cancelled : {false, true}) {
      auto item = make_case(SEEDS.front(), exec ? 24 : 0);
      item.argc = 1;
      item.unavailable = true;
      item.cancelled = cancelled;
      item.error = method(item) + ": bridge context unavailable";
      require_observation(item, replay(item));
      ++unavailable_count;
    }
    for (int stage = 0; stage < 3; ++stage) {
      auto item = make_case(SEEDS.front(), exec ? 24 : 0);
      item.argc = exec ? 3 : 2;
      item.cancelled = true;
      item.options = "23";
      switch (stage) {
        case 0:
          item.sql.clear();
          item.params_literal = "false";
          item.error = method(item) + ": sql must be non-empty";
          break;
        case 1:
          item.params_literal = "false";
          item.error = "db: params must be an array of scalars";
          break;
        case 2:
          item.params = {Param{.value = std::string{"a\0b", 3}}};
          item.error = "db: TEXT parameter at index 0 contains NUL";
          break;
        default: FAIL("unknown crossed precedence stage");
      }
      require_observation(item, replay(item));
      ++crossed_count;
    }
  }
  {
    auto item = make_case(SEEDS.front(), 24);
    item.argc = 3;
    item.params = {Param{.value = 37}};
    item.options = "23";
    item.cancelled = true;
    item.error = "db.exec: opts must be an object";
    require_observation(item, replay(item));
    ++crossed_count;
  }
  REQUIRE(unsupported_count == 24);
  REQUIRE(unavailable_count == 4);
  REQUIRE(crossed_count == 7);
}

auto durable_manager_control() -> void {
  std::weak_ptr<ExtensionDatabaseClients> weak;
  std::vector<AsyncOp> retained;
  {
    plinth::Config config{};
    RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                     nullptr, "notes");
    {
      Lease lease{pool};
      REQUIRE(lease.get() != nullptr);
      weak = lease.get()->extension_database_clients;
      {
        auto promise = evaluate(*lease.get(), "db.query('SELECT $1',[37])");
        REQUIRE(JS_PromiseState(lease.get()->ctx, promise.get()) ==
                JS_PROMISE_PENDING);
        retained = lease.get()->take_pending_ops();
        REQUIRE(retained.size() == 1);
        REQUIRE(retained.front().extension_database_clients == weak.lock());
        REQUIRE(retained.front().batch_pinned_conn == nullptr);
        lease.get()->resolve(retained.front().callback_id, Json::Value{});
        REQUIRE(JS_PromiseState(lease.get()->ctx, promise.get()) ==
                JS_PROMISE_FULFILLED);
        REQUIRE(drain_jobs(*lease.get()));
      }
      lease.destroy();
    }
    REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  }
  // No JS handle/raw context escapes. The real stopped lazy manager remains
  // solely operation-owned, then expires exactly when that owner is removed.
  REQUIRE_FALSE(weak.expired());
  REQUIRE(retained.front().extension_database_clients.use_count() == 1);
  retained.clear();
  REQUIRE(weak.expired());
}

auto ownership_controls() -> void {
  plinth::Config config{};
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   nullptr, "notes");
  {
    Lease settled{pool};
    REQUIRE(settled.get() != nullptr);
    {
      auto marker = evaluate(*settled.get(), "globalThis.dbReleaseMarker=111");
      REQUIRE_FALSE(JS_IsException(marker.get()));
    }
    const auto item = make_case(SEEDS.front(), 10);
    require_observation(item, execute(*settled.get(), item));
    settled.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  {
    Lease reused{pool};
    REQUIRE(reused.get() != nullptr);
    REQUIRE(reused.get()->next_callback_id == 0);
    REQUIRE(reused.get()->callbacks.empty());
    REQUIRE(reused.get()->pending_ops.empty());
    REQUIRE(reused.get()->extension_database_clients != nullptr);
    {
      auto clean = evaluate(
          *reused.get(), "typeof dbReleaseMarker==='undefined'&&typeof "
                         "__db_args==='undefined'&&typeof "
                         "db.query==='function'&&typeof db.exec==='function'");
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
          evaluate(*dirty.get(), "db.exec('SELECT $1',[37],{silent:true})");
      REQUIRE(JS_PromiseState(dirty.get()->ctx, promise.get()) ==
              JS_PROMISE_PENDING);
      REQUIRE(dirty.get()->callbacks.size() == 1);
      REQUIRE(dirty.get()->pending_ops.size() == 1);
    }
    dirty.release(); // Existing unresolved-owner policy destroys, not re-pools.
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 0);
  {
    Lease cancelled{pool};
    REQUIRE(cancelled.get() != nullptr);
    cancelled.get()->cancelled.store(true);
    cancelled.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 0);
  pool.rebuild();
  REQUIRE(pool.free_count() == 1);
  {
    Lease held{pool};
    REQUIRE(held.get() != nullptr);
    {
      auto marker = evaluate(*held.get(), "globalThis.dbRebuildMarker=321");
      REQUIRE_FALSE(JS_IsException(marker.get()));
    }
    pool.rebuild();
    REQUIRE(pool.active_count() == 1);
    REQUIRE(pool.free_count() == 1);
    {
      auto retained = evaluate(*held.get(), "dbRebuildMarker===321");
      REQUIRE_FALSE(JS_IsException(retained.get()));
      REQUIRE(JS_ToBool(held.get()->ctx, retained.get()) == 1);
    }
    REQUIRE_FALSE(pool.shutdown(100ms));
    REQUIRE(pool.acquire() == nullptr);
    REQUIRE(pool.active_count() == 1);
    {
      auto alive = evaluate(
          *held.get(), "typeof db.query==='function'&&dbRebuildMarker===321");
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

struct NumericCase {
  std::string_view name;
  double input;
  bool int_tag;
  Json::ValueType mapped_type;
  Json::Int64 mapped_integer = 0;
};

// Literal expectations are independent of the production conversion helper.
// Float constructors retain their actual float tag, including negative zero.
constexpr std::array<NumericCase, 27> NUMERIC_CASES{{
    {"integer zero", 0.0, true, Json::intValue, 0},
    {"integer one", 1.0, true, Json::intValue, 1},
    {"integer negative one", -1.0, true, Json::intValue, -1},
    {"int32 minimum", -2147483648.0, true, Json::intValue, -2147483648LL},
    {"int32 maximum", 2147483647.0, true, Json::intValue, 2147483647LL},
    {"float 2^31", 2147483648.0, false, Json::intValue, 2147483648LL},
    {"float -(2^31+1)", -2147483649.0, false, Json::intValue, -2147483649LL},
    {"float 2^40", 1099511627776.0, false, Json::intValue, 1099511627776LL},
    {"float -2^40", -1099511627776.0, false, Json::intValue, -1099511627776LL},
    {"inclusive 2^53", 9007199254740992.0, false, Json::intValue,
     9007199254740992LL},
    {"inclusive -2^53", -9007199254740992.0, false, Json::intValue,
     -9007199254740992LL},
    {"positive half", 0.5, false, Json::realValue},
    {"negative half", -0.5, false, Json::realValue},
    {"positive fraction", 42.25, false, Json::realValue},
    {"negative fraction", -42.25, false, Json::realValue},
    {"nearest upper safe neighbour", 9007199254740994.0, false,
     Json::realValue},
    {"nearest lower safe neighbour", -9007199254740994.0, false,
     Json::realValue},
    {"int64 minimum double", -9223372036854775808.0, false, Json::realValue},
    {"last double below 2^63", 9223372036854774784.0, false, Json::realValue},
    {"first double above int64 maximum", 9223372036854775808.0, false,
     Json::realValue},
    {"first double below int64 minimum", -9223372036854777856.0, false,
     Json::realValue},
    {"largest finite double", std::numeric_limits<double>::max(), false,
     Json::realValue},
    {"most negative finite double", -std::numeric_limits<double>::max(), false,
     Json::realValue},
    {"NaN", std::numeric_limits<double>::quiet_NaN(), false, Json::realValue},
    {"positive infinity", std::numeric_limits<double>::infinity(), false,
     Json::realValue},
    {"negative infinity", -std::numeric_limits<double>::infinity(), false,
     Json::realValue},
    {"float negative zero", -0.0, false, Json::intValue, 0},
}};

enum class NumericPhase { normal, cancelled, unavailable };

class CancellationRestore {
 public:
  CancellationRestore(BridgeContext& ctx, bool cancelled)
      : context(ctx), original(ctx.cancelled.load()) {
    context.cancelled.store(cancelled);
  }
  ~CancellationRestore() { context.cancelled.store(original); }
  CancellationRestore(const CancellationRestore&) = delete;
  auto operator=(const CancellationRestore&) -> CancellationRestore& = delete;
  CancellationRestore(CancellationRestore&&) = delete;
  auto operator=(CancellationRestore&&) -> CancellationRestore& = delete;

 private:
  BridgeContext& context;
  bool original;
};

auto require_numeric_clean(const BridgeContext& context) -> void {
  REQUIRE(context.callbacks.empty());
  REQUIRE(context.pending_ops.empty());
  REQUIRE(context.persistent_callbacks.empty());
  REQUIRE(context.concurrent_async_ops == 0);
  REQUIRE(context.inflight_detached.load() == 0);
  REQUIRE(context.pending_op_count() == 0);
  REQUIRE_FALSE(JS_IsJobPending(context.rt));
  REQUIRE_FALSE(JS_HasException(context.ctx));
}

auto require_numeric_double(double actual, double expected) -> void {
  if (std::isnan(expected)) {
    REQUIRE(std::isnan(actual));
  } else if (std::isinf(expected)) {
    REQUIRE(std::isinf(actual));
    REQUIRE(std::signbit(actual) == std::signbit(expected));
  } else {
    REQUIRE(actual == expected);
    REQUIRE(std::signbit(actual) == std::signbit(expected));
  }
}

auto require_numeric_mapping(const Json::Value& actual, const NumericCase& item)
    -> void {
  REQUIRE(actual.type() == item.mapped_type);
  if (item.mapped_type == Json::intValue) {
    REQUIRE(actual.asInt64() == item.mapped_integer);
  } else {
    REQUIRE(item.mapped_type == Json::realValue);
    require_numeric_double(actual.asDouble(), item.input);
  }
}

auto call_numeric(BridgeContext& context, JSValue function, JSValue db,
                  std::array<JSValue, 2>& arguments, NumericPhase phase)
    -> OwnedValue {
  OpaqueRestore opaque{context.ctx, phase == NumericPhase::unavailable};
  CancellationRestore cancelled{context, phase == NumericPhase::cancelled};
  // Both guards restore before the caller inspects or settles the promise.
  return {context.ctx, JS_Call(context.ctx, function, db, 2, arguments.data())};
}

auto execute_numeric(BridgeContext& context, const NumericCase& item, bool exec,
                     NumericPhase phase) -> void {
  require_numeric_clean(context);
  REQUIRE(JS_GetContextOpaque(context.ctx) == &context);
  REQUIRE_FALSE(context.cancelled.load());
  const auto manager = context.extension_database_clients;
  REQUIRE(manager != nullptr);
  const int initial_id = context.next_callback_id;
  OwnedValue number{
      context.ctx,
      item.int_tag
          ? JS_NewInt32(context.ctx, static_cast<std::int32_t>(item.input))
          : JS_NewFloat64(context.ctx, item.input)};
  REQUIRE(JS_VALUE_GET_NORM_TAG(number.get()) ==
          (item.int_tag ? JS_TAG_INT : JS_TAG_FLOAT64));
  double input = 0.0;
  REQUIRE(JS_ToFloat64(context.ctx, &input, number.get()) == 0);
  require_numeric_double(input, item.input);
  OwnedValue params{context.ctx, JS_NewArray(context.ctx)};
  REQUIRE_FALSE(JS_IsException(params.get()));
  REQUIRE(JS_SetPropertyUint32(context.ctx, params.get(), 0,
                               JS_DupValue(context.ctx, number.get())) == 1);
  OwnedValue sql{context.ctx, JS_NewString(context.ctx, "SELECT $1")};
  OwnedValue global{context.ctx, JS_GetGlobalObject(context.ctx)};
  OwnedValue db{context.ctx,
                JS_GetPropertyStr(context.ctx, global.get(), "db")};
  OwnedValue function{context.ctx, JS_GetPropertyStr(context.ctx, db.get(),
                                                     exec ? "exec" : "query")};
  REQUIRE(JS_IsFunction(context.ctx, function.get()));
  std::array<JSValue, 2> arguments{sql.get(), params.get()};
  auto promise =
      call_numeric(context, function.get(), db.get(), arguments, phase);
  REQUIRE(JS_GetContextOpaque(context.ctx) == &context);
  REQUIRE_FALSE(context.cancelled.load());
  REQUIRE(context.extension_database_clients == manager);
  REQUIRE(context.extension_name == "notes");
  if (phase == NumericPhase::unavailable) {
    REQUIRE(JS_IsException(promise.get()));
    OwnedValue exception{context.ctx, JS_GetException(context.ctx)};
    REQUIRE(property_text(context.ctx, exception.get(), "name") == "TypeError");
    REQUIRE(property_text(context.ctx, exception.get(), "message") ==
            std::string{exec ? "db.exec" : "db.query"} +
                ": bridge context unavailable");
    REQUIRE(context.next_callback_id == initial_id);
    require_numeric_clean(context);
    return;
  }
  REQUIRE_FALSE(JS_IsException(promise.get()));
  REQUIRE(JS_PromiseState(context.ctx, promise.get()) ==
          (phase == NumericPhase::normal ? JS_PROMISE_PENDING
                                         : JS_PROMISE_REJECTED));
  auto operations = context.take_pending_ops();
  if (phase == NumericPhase::normal) {
    REQUIRE(operations.size() == 1);
    REQUIRE(context.callbacks.size() == 1);
    REQUIRE(context.pending_op_count() == 1);
    REQUIRE(context.concurrent_async_ops == 0);
    REQUIRE(context.next_callback_id == initial_id + 1);
    const auto& operation = operations.front();
    REQUIRE(operation.callback_id == initial_id);
    const auto callback = context.callbacks.find(initial_id);
    REQUIRE(callback != context.callbacks.end());
    REQUIRE(callback->second.ns_for_cancellation == "db");
    REQUIRE(JS_IsFunction(context.ctx, callback->second.resolve));
    REQUIRE(JS_IsFunction(context.ctx, callback->second.reject));
    const auto snapshot = observe_operation(operation, manager);
    REQUIRE(snapshot.type ==
            (exec ? AsyncOp::Type::DB_EXEC : AsyncOp::Type::DB_QUERY));
    REQUIRE(snapshot.sql == "SELECT $1");
    REQUIRE(snapshot.params.size() == 1);
    REQUIRE_FALSE(snapshot.silent);
    REQUIRE(snapshot.extension == "notes");
    REQUIRE(snapshot.manager_pointer);
    REQUIRE(snapshot.manager_owner);
    REQUIRE(snapshot.unrelated);
    require_numeric_mapping(snapshot.params.front(), item);
    // Caller mutation cannot change the separately owned operation snapshot.
    REQUIRE(JS_SetPropertyUint32(context.ctx, params.get(), 0,
                                 JS_NewInt32(context.ctx, 99)) == 1);
    REQUIRE(operation.sql_params.size() == 1);
    require_numeric_mapping(operation.sql_params.front(), item);
  } else {
    REQUIRE(operations.empty());
    REQUIRE(context.callbacks.empty());
    REQUIRE(context.pending_op_count() == 0);
    REQUIRE(context.concurrent_async_ops == 0);
    REQUIRE(context.next_callback_id == initial_id);
  }
  OwnedValue then{context.ctx,
                  JS_GetPropertyStr(context.ctx, promise.get(), "then")};
  auto handler = evaluate(context, "(value)=>value");
  REQUIRE_FALSE(JS_IsException(handler.get()));
  std::array<JSValue, 2> handlers{handler.get(), handler.get()};
  OwnedValue observer{context.ctx, JS_Call(context.ctx, then.get(),
                                           promise.get(), 2, handlers.data())};
  REQUIRE_FALSE(JS_IsException(observer.get()));
  if (phase == NumericPhase::normal) {
    // Accounting and host settlement are simulated, never database dispatch.
    context.concurrent_async_ops = 1;
    context.resolve(operations.front().callback_id, Json::Value{37});
  }
  operations.clear();
  REQUIRE(drain_jobs(context));
  REQUIRE(JS_PromiseState(context.ctx, promise.get()) ==
          (phase == NumericPhase::normal ? JS_PROMISE_FULFILLED
                                         : JS_PROMISE_REJECTED));
  REQUIRE(JS_PromiseState(context.ctx, observer.get()) == JS_PROMISE_FULFILLED);
  OwnedValue result{context.ctx, JS_PromiseResult(context.ctx, promise.get())};
  OwnedValue observed{context.ctx,
                      JS_PromiseResult(context.ctx, observer.get())};
  if (phase == NumericPhase::normal) {
    int value = 0;
    REQUIRE(JS_ToInt32(context.ctx, &value, result.get()) == 0);
    REQUIRE(value == 37);
    REQUIRE(JS_ToInt32(context.ctx, &value, observed.get()) == 0);
    REQUIRE(value == 37);
  } else {
    REQUIRE(rejection_shape(context.ctx, result.get()));
    REQUIRE(rejection_shape(context.ctx, observed.get()));
    REQUIRE(property_text(context.ctx, result.get(), "code") == "db.cancelled");
    REQUIRE(property_text(context.ctx, result.get(), "message") ==
            "execution cancelled");
    REQUIRE(property_text(context.ctx, observed.get(), "code") ==
            "db.cancelled");
    REQUIRE(property_text(context.ctx, observed.get(), "message") ==
            "execution cancelled");
  }
  require_numeric_clean(context);
}

auto numeric_controls() -> void {
  REQUIRE(NUMERIC_CASES.size() == 27);
  std::array<int, 3> calls{};
  std::array<int, 2> bindings{};
  int contexts = 0;
  int integers = 0;
  int reals = 0;
  for (const bool exec : {false, true}) {
    for (int replay_index = 0; replay_index < 2; ++replay_index) {
      for (const auto phase : {NumericPhase::normal, NumericPhase::cancelled,
                               NumericPhase::unavailable}) {
        plinth::Config config{};
        RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config,
                         1, nullptr, "notes");
        int phase_calls = 0;
        int phase_integers = 0;
        int phase_reals = 0;
        {
          Lease lease{pool};
          REQUIRE(lease.get() != nullptr);
          ++contexts;
          for (const auto& item : NUMERIC_CASES) {
            INFO("numeric=" << item.name << " exec=" << exec
                            << " replay=" << replay_index
                            << " phase=" << static_cast<int>(phase));
            execute_numeric(*lease.get(), item, exec, phase);
            // execute_numeric's JS-value/operation owners have all died.
            require_numeric_clean(*lease.get());
            REQUIRE(JS_GetContextOpaque(lease.get()->ctx) == lease.get());
            REQUIRE_FALSE(lease.get()->cancelled.load());
            ++phase_calls;
            ++calls.at(static_cast<std::size_t>(phase));
            ++bindings.at(exec ? 1U : 0U);
            if (phase == NumericPhase::normal) {
              if (item.mapped_type == Json::intValue) {
                ++phase_integers;
                ++integers;
              } else {
                ++phase_reals;
                ++reals;
              }
            }
          }
          REQUIRE(phase_calls == 27);
          REQUIRE(phase_integers == (phase == NumericPhase::normal ? 12 : 0));
          REQUIRE(phase_reals == (phase == NumericPhase::normal ? 15 : 0));
          lease.release();
        }
        REQUIRE(pool.active_count() == 0);
        REQUIRE(pool.free_count() == 1);
        REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
      }
    }
  }
  REQUIRE(contexts == 12);
  REQUIRE(calls == std::array<int, 3>{108, 108, 108});
  REQUIRE(calls[0] + calls[1] + calls[2] == 324);
  REQUIRE(bindings == std::array<int, 2>{162, 162});
  REQUIRE(integers == 48);
  REQUIRE(reals == 60);
}

} // namespace

TEST_CASE("QuickJS ordinary database calls have a bounded deterministic "
          "admission and ownership corpus",
          "[js][db][corpus][isolated-db-admission]") {
  INFO("seeds=" << SEEDS.size() << " families=" << FAMILIES << " max_params="
                << MAX_PARAMS << " max_sql_bytes=" << MAX_SQL_BYTES
                << " max_value_bytes=" << MAX_VALUE_BYTES << " max_arguments="
                << MAX_ARGUMENTS << " max_jobs=" << MAX_JOBS
                << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS);
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  std::array<int, FAMILIES> coverage{};
  int cases = 0;
  int queries = 0;
  int executions = 0;
  int exceptions = 0;
  int cancelled = 0;
  int queued = 0;
  int simulated_rejections = 0;
  for (const auto seed : SEEDS) {
    for (int family = 0; family < FAMILIES; ++family) {
      INFO("seed=" << seed << " family=" << family);
      const auto item = make_case(seed, family);
      REQUIRE(item.sql.size() <= MAX_SQL_BYTES);
      REQUIRE(item.params.size() <= MAX_PARAMS);
      REQUIRE(item.argc <= MAX_ARGUMENTS);
      for (const auto& param : item.params) {
        REQUIRE(param.byte_count <= MAX_VALUE_BYTES);
        if (param.value.isString()) {
          REQUIRE(param.value.asString().size() <= MAX_VALUE_BYTES);
        }
      }
      require_observation(item, replay(item));
      require_observation(item, replay(item));
      ++coverage.at(static_cast<std::size_t>(family));
      ++cases;
      item.exec ? ++executions : ++queries;
      exceptions += item.error.empty() ? 0 : 1;
      cancelled += item.cancelled ? 1 : 0;
      queued += admitted(item) ? 1 : 0;
      simulated_rejections += item.simulate_rejection ? 1 : 0;
    }
  }
  REQUIRE(cases == 192);
  REQUIRE(queries == 96);
  REQUIRE(executions == 96);
  REQUIRE(exceptions == 56);
  REQUIRE(cancelled == 8);
  REQUIRE(queued == 128);
  REQUIRE(simulated_rejections == 8);
  for (const auto count : coverage) {
    REQUIRE(count == 4);
  }
  negative_controls();
  fixed_controls();
  durable_manager_control();
  ownership_controls();
  numeric_controls();
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}
