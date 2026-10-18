// SPDX-License-Identifier: MIT
//
// Actual registered public db.batch admission and unmodified installed JS
// orchestration. Host settlement/reset is EXPLICITLY SIMULATED: no SQL driver,
// authorization, durable commit, timer delivery or production cancellation
// proof. Alias-null owner tokens must NEVER be dispatched as transactions.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/extension_database.hpp"
#include "kernel/js/native_rollback_owner.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"

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

namespace {

using plinth::js::AsyncOp;
using plinth::js::BridgeContext;
using plinth::js::ExtensionDatabaseClients;
using plinth::js::NativeRollbackOwner;
using plinth::js::RuntimePool;
using Type = AsyncOp::Type;

constexpr std::array<std::uint64_t, 3> SEEDS{0x90BA'0000'0000'0001ULL,
                                             0x90BA'0000'0000'0027ULL,
                                             0x90BA'0000'0000'BEEFULL};
constexpr int ADMISSION_FAMILIES = 30;
constexpr int FLOW_FAMILIES = 16;
constexpr int FAMILIES = ADMISSION_FAMILIES + FLOW_FAMILIES;
constexpr int INITIAL_ID = 31;
constexpr int MAX_JOBS = 128;
constexpr int MAX_OPERATIONS = 5;
constexpr int MAX_SHRINK_ATTEMPTS = 8;
constexpr std::size_t MAX_SOURCE_BYTES = 4096;
constexpr std::size_t MAX_TEXT_BYTES = 24;
constexpr int MAX_INPUT_NODES = 24;
constexpr int MAX_INPUT_DEPTH = 3;
constexpr auto SHUTDOWN_BOUND = std::chrono::seconds{2};

class Value {
 public:
  Value(JSContext* ctx, JSValue input) : context(ctx), value(input) {}
  ~Value() { JS_FreeValue(context, value); }
  Value(const Value&) = delete;
  auto operator=(const Value&) -> Value& = delete;
  Value(Value&&) = delete;
  auto operator=(Value&&) -> Value& = delete;
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
  OpaqueRestore(JSContext* ctx, bool unavailable)
      : context(ctx), original(JS_GetContextOpaque(ctx)) {
    if (unavailable) {
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

auto json_source(const Json::Value& value) -> std::string {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}

auto evaluate(BridgeContext& context, const std::string& source) -> Value {
  REQUIRE(source.size() <= MAX_SOURCE_BYTES);
  JS_UpdateStackTop(context.rt);
  return {context.ctx, JS_Eval(context.ctx, source.data(), source.size(),
                               "<db-batch-corpus>", JS_EVAL_TYPE_GLOBAL)};
}

auto text(JSContext* context, JSValue value) -> std::string {
  std::size_t length = 0;
  const char* input = JS_ToCStringLen(context, &length, value);
  REQUIRE(input != nullptr);
  std::string result{input, length};
  JS_FreeCString(context, input);
  return result;
}

auto property_text(JSContext* context, JSValue value, const char* name)
    -> std::string {
  Value property{context, JS_GetPropertyStr(context, value, name)};
  return text(context, property.get());
}

// Only observations pass through JSON.stringify/JsonCpp. Expectations below
// are literal values, never a production JS converter or DB error mapper.
auto observed_json(JSContext* context, JSValue value) -> Json::Value {
  Value encoded{context,
                JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED)};
  REQUIRE(JS_IsString(encoded.get()));
  const auto source = text(context, encoded.get());
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
  Json::Value result;
  std::string errors;
  REQUIRE(reader->parse(source.data(), source.data() + source.size(), &result,
                        &errors));
  return result;
}

auto drain(BridgeContext& context) -> void {
  int jobs = 0;
  while (JS_IsJobPending(context.rt) && jobs < MAX_JOBS) {
    JSContext* job_context = nullptr;
    REQUIRE(JS_ExecutePendingJob(context.rt, &job_context) >= 0);
    ++jobs;
  }
  REQUIRE_FALSE(JS_IsJobPending(context.rt));
  REQUIRE_FALSE(JS_HasException(context.ctx));
}

auto envelope(std::string_view code, std::string_view message) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["code"] = std::string{code};
  result["message"] = std::string{message};
  return result;
}

enum class Flow {
  value,
  promise,
  begin_rejection,
  sync_throw,
  promise_rejection,
  string_throw,
  null_rejection,
  commit_rejection,
  rollback_rejection,
  timeout,
  operation_rejection,
  nested_rejection,
  begin_cancelled,
};

struct Input {
  int family = 0;
  bool admission = true;
  std::string argument = "function(){__batch_body_calls++;return 1;}";
  int argc = 1;
  bool unavailable = false;
  bool cancelled = false;
  bool nested = false;
  std::string exception;
  Json::Value rejection;
  Flow flow = Flow::value;
  int operations = 0;
  int number = 0;
  std::string label;
};

enum class Fault {
  none,
  drop_operation,
  clone_settlement,
  invoke_admission_callback,
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9E37'79B9'7F4A'7C15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return value ^ (value >> 31U);
}

auto admission_input(int family) -> Input {
  constexpr std::array<std::string_view, 12> INVALID{"undefined",
                                                     "undefined",
                                                     "null",
                                                     "false",
                                                     "23",
                                                     "'fake'",
                                                     "({fake:1})",
                                                     "[]",
                                                     "Symbol('fake')",
                                                     "1n",
                                                     "Promise.resolve(1)",
                                                     "new Date(0)"};
  Input input;
  input.family = family;
  if (family < 24) {
    const auto index = static_cast<std::size_t>(family % 12);
    input.argument = INVALID.at(index);
    input.argc = index == 0 ? 0 : 1;
    input.unavailable = family >= 12;
    input.cancelled = family >= 12;
    input.nested = family >= 12;
    input.exception = "db.batch: fn must be a function";
    return input;
  }
  input.unavailable = family == 24 || family == 25;
  input.cancelled = family == 25 || family == 26 || family == 27;
  input.nested = family == 25 || family == 27 || family == 28;
  if (input.unavailable) {
    input.exception = "db.batch: bridge context unavailable";
  } else if (input.cancelled) {
    input.rejection = envelope("db.cancelled", "execution cancelled");
  } else if (input.nested) {
    input.rejection = envelope("db.batch.nested_not_allowed",
                               "db.batch cannot nest inside another db.batch");
  } else {
    // Extra arguments are ignored by the actual public function.
    input.admission = false;
    input.argc = 2;
  }
  return input;
}

auto make_input(std::uint64_t seed, int family) -> Input {
  auto input = family < ADMISSION_FAMILIES ? admission_input(family) : Input{};
  input.family = family;
  auto state = seed + static_cast<std::uint64_t>(family);
  input.number = static_cast<int>(next(state) % 129U) - 64;
  input.label = "fake-" + std::to_string(next(state) % 100000U);
  if (family < ADMISSION_FAMILIES) {
    return input;
  }
  input.admission = false;
  const int choice = family - ADMISSION_FAMILIES;
  constexpr std::array<Flow, FLOW_FAMILIES> FLOWS{Flow::value,
                                                  Flow::promise,
                                                  Flow::value,
                                                  Flow::promise,
                                                  Flow::value,
                                                  Flow::begin_rejection,
                                                  Flow::sync_throw,
                                                  Flow::promise_rejection,
                                                  Flow::string_throw,
                                                  Flow::null_rejection,
                                                  Flow::commit_rejection,
                                                  Flow::rollback_rejection,
                                                  Flow::timeout,
                                                  Flow::operation_rejection,
                                                  Flow::nested_rejection,
                                                  Flow::begin_cancelled};
  input.flow = FLOWS.at(static_cast<std::size_t>(choice));
  if (choice >= 2 && choice <= 4) {
    input.operations = choice - 1;
  }
  if (input.flow == Flow::operation_rejection) {
    input.operations = 1;
  }
  return input;
}

auto body_error() -> Json::Value {
  auto result = envelope("fake.body", "fake body failure");
  result["detail"] = 7;
  return result;
}

auto expected_result(const Input& input) -> Json::Value {
  switch (input.flow) {
    case Flow::begin_rejection:
      return envelope("db.internal", "fake begin failure");
    case Flow::begin_cancelled:
      return envelope("db.cancelled", "execution cancelled");
    case Flow::commit_rejection: {
      auto result = envelope("db.internal", "fake commit failure");
      result["sqlstate"] = "XX000";
      return result;
    }
    case Flow::sync_throw:
    case Flow::promise_rejection:
    case Flow::rollback_rejection: return body_error();
    case Flow::string_throw: return "fake thrown string";
    case Flow::null_rejection: return Json::Value{};
    case Flow::timeout:
      return envelope("db.batch.timeout", "batch wall-clock exceeded");
    case Flow::operation_rejection:
      return envelope("db.internal", "fake operation failure");
    case Flow::nested_rejection:
      return envelope("db.batch.nested_not_allowed",
                      "db.batch cannot nest inside another db.batch");
    case Flow::value:
    case Flow::promise: {
      if (input.family == ADMISSION_FAMILIES) {
        return input.number; // Literal scalar success alongside object cases.
      }
      Json::Value result{Json::objectValue};
      result["number"] = input.number;
      result["label"] = input.label;
      return result;
    }
  }
  FAIL("unknown flow");
  return {};
}

auto rollback_flow(Flow flow) -> bool {
  return flow == Flow::sync_throw || flow == Flow::promise_rejection ||
         flow == Flow::rollback_rejection || flow == Flow::string_throw ||
         flow == Flow::null_rejection || flow == Flow::timeout ||
         flow == Flow::operation_rejection || flow == Flow::nested_rejection;
}

auto begin_failed(Flow flow) -> bool {
  return flow == Flow::begin_rejection || flow == Flow::begin_cancelled;
}

auto preserves_body_identity(Flow flow) -> bool {
  return flow == Flow::value || flow == Flow::promise ||
         flow == Flow::sync_throw || flow == Flow::promise_rejection ||
         flow == Flow::string_throw || flow == Flow::null_rejection ||
         flow == Flow::rollback_rejection;
}

auto body_source(const Input& input) -> std::string {
  constexpr std::string_view ORIGINAL = "__batch_original";
  std::string body = "function(){__batch_body_calls++;";
  switch (input.flow) {
    case Flow::sync_throw:
    case Flow::string_throw:
      return body + "throw " + std::string{ORIGINAL} + ";}";
    case Flow::promise_rejection:
    case Flow::null_rejection:
    case Flow::rollback_rejection:
      return body + "return Promise.reject(" + std::string{ORIGINAL} + ");}";
    case Flow::nested_rejection: return body + "return db.batch(()=>1);}";
    default: break;
  }
  body += "var p=Promise.resolve();";
  for (int index = 0; index < input.operations; ++index) {
    const auto method = index % 2 == 0 ? "exec" : "query";
    body += "p=p.then(function(){return db." + std::string{method} +
            "(__batch_sql[" + std::to_string(index) + "],__batch_params[" +
            std::to_string(index) + "]" +
            (index % 2 == 0 ? ",{silent:true}" : "") + ");});";
  }
  if (input.operations != 0 || input.flow == Flow::promise) {
    return body + "return p.then(function(){return " + std::string{ORIGINAL} +
           ";});}";
  }
  return body + "return " + std::string{ORIGINAL} + ";}";
}

auto expected_params(const Input& input, int index)
    -> std::vector<Json::Value> {
  return {Json::Value{input.number + index}, Json::Value{input.label},
          Json::Value{}, Json::Value{true}};
}

auto expected_sql(int index) -> std::string {
  return index % 2 == 0
             ? "UPDATE notes SET body=$2 WHERE id=$1 AND $3::text IS NULL "
               "AND $4::boolean"
             : "SELECT $1,$2,$3,$4";
}

auto setup_source(const Input& input) -> std::string {
  std::string source =
      "globalThis.__batch_body_calls=0;globalThis.__batch_hits=0;"
      "globalThis.__batch_seen=null;globalThis.__batch_sql=[];"
      "globalThis.__batch_params=[];globalThis.__batch_observe=function(v){"
      "__batch_hits++;__batch_seen=v;return v;};";
  source +=
      "globalThis.__batch_original=" + json_source(expected_result(input)) +
      ";";
  for (int index = 0; index < input.operations; ++index) {
    Json::Value params{Json::arrayValue};
    for (const auto& value : expected_params(input, index)) {
      params.append(value);
    }
    source += "__batch_sql.push(" + json_source(expected_sql(index)) +
              ");__batch_params.push(" + json_source(params) + ");";
  }
  source += "globalThis.__batch_fn=" +
            (input.admission ? input.argument : body_source(input)) + ";";
  return source;
}

struct Operation {
  Type type = Type::DB_BATCH_BEGIN;
  int id = 0;
  std::size_t count = 0;
  std::string sql;
  std::vector<Json::Value> params;
  bool silent = false;
  std::string extension;
  bool scope = false;
  bool manager = false;
  bool transaction = false;
  bool rollback_owner = false;
  bool transferred = false;
  bool unrelated = false;
  Json::Value error;
  auto operator==(const Operation&) const -> bool = default;
};

struct Observation {
  std::string exception_name;
  std::string exception_message;
  int immediate = -1;
  int final_state = -1;
  int next_id = INITIAL_ID;
  int body_calls = 0;
  int hits = 0;
  bool initial_state = true;
  bool inventory = true;
  bool snapshots = true;
  bool owners_released = true;
  bool reaction = true;
  bool reaction_identity = true;
  std::optional<bool> original_identity;
  bool clean = true;
  bool late_clean = true;
  Json::Value result;
  std::vector<Operation> operations;
  auto operator==(const Observation&) const -> bool = default;
};

auto read_integer(BridgeContext& context, const char* name) -> int {
  Value global{context.ctx, JS_GetGlobalObject(context.ctx)};
  Value property{context.ctx,
                 JS_GetPropertyStr(context.ctx, global.get(), name)};
  int result = -1;
  REQUIRE(JS_ToInt32(context.ctx, &result, property.get()) == 0);
  return result;
}

auto clean(const BridgeContext& context) -> bool {
  return context.callbacks.empty() && context.pending_ops.empty() &&
         context.retired_pending_ops.empty() &&
         context.persistent_callbacks.empty() &&
         context.concurrent_async_ops == 0 &&
         context.inflight_detached.load() == 0 &&
         !JS_IsJobPending(context.rt) && !JS_HasException(context.ctx);
}

auto observe_operation(const AsyncOp& operation, const BridgeContext& context,
                       std::uint64_t scope,
                       const std::shared_ptr<ExtensionDatabaseClients>& manager)
    -> Operation {
  const bool terminal = operation.type == Type::DB_BATCH_COMMIT ||
                        operation.type == Type::DB_BATCH_ROLLBACK;
  const auto& user = operation.cap_user;
  return {
      .type = operation.type,
      .id = operation.callback_id,
      .count = operation.batch_operation_count,
      .sql = operation.sql,
      .params = operation.sql_params,
      .silent = operation.silent,
      .extension = operation.bc_extension_name,
      .scope = scope != 0 && operation.batch_scope_id == scope,
      .manager = operation.extension_database_clients == manager &&
                 !operation.extension_database_clients.owner_before(manager) &&
                 !manager.owner_before(operation.extension_database_clients),
      .transaction =
          operation.batch_pinned_conn.get() == nullptr &&
          operation.batch_pinned_conn.use_count() == (terminal ? 1 : 2),
      .rollback_owner =
          operation.batch_rollback_owner != nullptr &&
          operation.batch_rollback_owner.use_count() == (terminal ? 1 : 2),
      .transferred =
          !terminal || (context.batch_state.pinned_conn.use_count() == 0 &&
                        !context.batch_state.rollback_owner),
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
          operation.pubsub_payload.isNull() &&
          !operation.rollback_request_pending,
      .error = operation.rollback_error};
}

auto inventory(const BridgeContext& context, int id) -> bool {
  if (context.callbacks.size() != 1 || context.pending_ops.size() != 1 ||
      context.pending_op_count() != 2 || context.concurrent_async_ops != 0) {
    return false;
  }
  const auto found = context.callbacks.find(id);
  return found != context.callbacks.end() &&
         found->second.ns_for_cancellation == "db" &&
         JS_IsFunction(context.ctx, found->second.resolve) &&
         JS_IsFunction(context.ctx, found->second.reject);
}

auto settle(BridgeContext& context, const AsyncOp& operation,
            const Input& input) -> void {
  context.concurrent_async_ops =
      1; // Simulated owning-host dispatch accounting.
  if (operation.type == Type::DB_BATCH_BEGIN && begin_failed(input.flow)) {
    context.batch_state = BridgeContext::BatchState{};
    context.reject(operation.callback_id,
                   {.code = input.flow == Flow::begin_cancelled ? "db.cancelled"
                                                                : "db.internal",
                    .message = input.flow == Flow::begin_cancelled
                                   ? "execution cancelled"
                                   : "fake begin failure",
                    .sqlstate = std::nullopt});
  } else if (operation.type == Type::DB_BATCH_COMMIT &&
             input.flow == Flow::commit_rejection) {
    context.reject(operation.callback_id, {.code = "db.internal",
                                           .message = "fake commit failure",
                                           .sqlstate = "XX000"});
  } else if (operation.type == Type::DB_BATCH_ROLLBACK &&
             input.flow == Flow::rollback_rejection) {
    context.reject(operation.callback_id, {.code = "db.internal",
                                           .message = "fake rollback failure",
                                           .sqlstate = std::nullopt});
  } else if (operation.type == Type::DB_EXEC &&
             input.flow == Flow::operation_rejection) {
    context.reject(operation.callback_id, {.code = "db.internal",
                                           .message = "fake operation failure",
                                           .sqlstate = std::nullopt});
  } else {
    context.resolve(operation.callback_id, Json::Value{});
  }
}

struct Owners {
  std::weak_ptr<int> transaction;
  std::weak_ptr<NativeRollbackOwner> rollback;
};

auto install_owners(BridgeContext& context, const Input& input) -> Owners {
  auto token = std::make_shared<int>(input.number);
  Owners owners;
  owners.transaction = token;
  context.batch_state.pinned_conn =
      std::shared_ptr<drogon::orm::Transaction>{token, nullptr};
  context.batch_state.rollback_owner =
      std::make_shared<NativeRollbackOwner>(nullptr);
  owners.rollback = context.batch_state.rollback_owner;
  context.batch_state.timed_out = input.flow == Flow::timeout;
  return owners;
}

auto simulate_operations(BridgeContext& context, const Input& input,
                         Observation& observation) -> void {
  const auto manager = context.extension_database_clients;
  const auto scope = context.batch_state.scope_id;
  Owners owners;
  std::vector<int> ids;
  for (int step = 0; !context.pending_ops.empty() && step < MAX_OPERATIONS;
       ++step) {
    const int id = context.pending_ops.front().callback_id;
    observation.inventory = observation.inventory && inventory(context, id);
    auto operations = context.take_pending_ops();
    REQUIRE(operations.size() == 1);
    auto& operation = operations.front();
    observation.operations.push_back(
        observe_operation(operation, context, scope, manager));
    observation.snapshots =
        observation.snapshots && context.batch_state.depth == 1 &&
        context.batch_state.scope_id == scope &&
        context.batch_state.ops_in_batch ==
            static_cast<std::size_t>(
                step == 0
                    ? 0
                    : (step <= input.operations ? step : input.operations));
    const bool terminal = operation.type == Type::DB_BATCH_COMMIT ||
                          operation.type == Type::DB_BATCH_ROLLBACK;
    if (operation.type != Type::DB_BATCH_BEGIN) {
      observation.snapshots =
          observation.snapshots && !owners.transaction.expired() &&
          !owners.rollback.expired() &&
          !owners.transaction.owner_before(operation.batch_pinned_conn) &&
          !operation.batch_pinned_conn.owner_before(owners.transaction) &&
          !owners.rollback.owner_before(operation.batch_rollback_owner) &&
          !operation.batch_rollback_owner.owner_before(owners.rollback) &&
          context.batch_state.timed_out == (input.flow == Flow::timeout);
    }
    if (operation.type == Type::DB_BATCH_BEGIN && !begin_failed(input.flow)) {
      owners = install_owners(context, input);
    }
    if (operation.type == Type::DB_EXEC || operation.type == Type::DB_QUERY) {
      // Mutate the caller and BC after enqueue, then re-read the owned op.
      const auto extension = context.extension_name;
      context.extension_name = "changed-owner";
      context.extension_database_clients.reset();
      auto mutation =
          evaluate(context, "__batch_params[" + std::to_string(step - 1) +
                                "][0]=999;__batch_sql[" +
                                std::to_string(step - 1) + "]='changed'");
      REQUIRE_FALSE(JS_IsException(mutation.get()));
      observation.snapshots =
          observation.snapshots && operation.sql == expected_sql(step - 1) &&
          operation.sql_params == expected_params(input, step - 1) &&
          operation.bc_extension_name == extension &&
          operation.extension_database_clients == manager;
      context.extension_name = extension;
      context.extension_database_clients = manager;
    }
    // Native finalization is simulated only after observing terminal transfer.
    if (terminal) {
      context.batch_state = BridgeContext::BatchState{};
    }
    settle(context, operation, input);
    ids.push_back(id);
    operations.clear(); // No native transaction call is possible.
    drain(context);
  }
  REQUIRE(context.pending_ops.empty());
  observation.owners_released =
      owners.transaction.expired() && owners.rollback.expired();
  observation.next_id = context.next_callback_id;
  for (const int id : ids) {
    context.resolve(id, Json::Value{123});
    context.reject(id, {.code = "fake.late",
                        .message = "fake late completion",
                        .sqlstate = std::nullopt});
    context.resolve_with_js_value(id, JS_NewString(context.ctx, "fake late"));
  }
  drain(context);
  observation.late_clean =
      clean(context) && context.next_callback_id == observation.next_id;
}

auto strictly_equal(BridgeContext& context, JSValue left, JSValue right)
    -> bool {
  auto compare = evaluate(context, "(function(a,b){return a===b;})");
  REQUIRE(JS_IsFunction(context.ctx, compare.get()));
  std::array<JSValue, 2> arguments{left, right};
  Value result{context.ctx, JS_Call(context.ctx, compare.get(), JS_UNDEFINED, 2,
                                    arguments.data())};
  REQUIRE(JS_IsBool(result.get()));
  return JS_ToBool(context.ctx, result.get()) == 1;
}

auto settlement_sample(BridgeContext& context, JSValue original, Fault fault)
    -> Value {
  if (fault != Fault::clone_settlement) {
    return {context.ctx, JS_DupValue(context.ctx, original)};
  }
  // Deliberately corrupt only the observed settlement sample. Production
  // bindings, orchestrator, Promise and the real outer promise are untouched.
  const auto source = json_source(observed_json(context.ctx, original));
  return {context.ctx, JS_ParseJSON(context.ctx, source.data(), source.size(),
                                    "<db-batch-clone-control>")};
}

auto observe_promise(BridgeContext& context, JSValue promise,
                     const Input& input, Observation& observation, Fault fault)
    -> void {
  observation.final_state = JS_PromiseState(context.ctx, promise);
  REQUIRE(observation.final_state != JS_PROMISE_PENDING);
  Value actual{context.ctx, JS_PromiseResult(context.ctx, promise)};
  auto result = settlement_sample(context, actual.get(), fault);
  REQUIRE_FALSE(JS_IsException(result.get()));
  if (fault == Fault::clone_settlement) {
    REQUIRE(JS_IsObject(actual.get()));
    REQUIRE(observed_json(context.ctx, result.get()) ==
            observed_json(context.ctx, actual.get()));
    REQUIRE_FALSE(strictly_equal(context, result.get(), actual.get()));
  }
  observation.result = observed_json(context.ctx, result.get());
  observation.body_calls = read_integer(context, "__batch_body_calls");
  observation.hits = read_integer(context, "__batch_hits");
  Value global{context.ctx, JS_GetGlobalObject(context.ctx)};
  Value seen{context.ctx,
             JS_GetPropertyStr(context.ctx, global.get(), "__batch_seen")};
  observation.reaction =
      observed_json(context.ctx, seen.get()) == observation.result;
  // Normal samples alias the actual PromiseResult; even host-mapped errors
  // must reach the reaction as that same JS value. They have no preexisting
  // JS identity in the C++ JSON host fixture, so no original-body claim.
  observation.reaction_identity =
      strictly_equal(context, seen.get(), result.get());
  if (!input.admission && preserves_body_identity(input.flow)) {
    Value original{context.ctx, JS_GetPropertyStr(context.ctx, global.get(),
                                                  "__batch_original")};
    observation.original_identity =
        strictly_equal(context, result.get(), original.get()) &&
        strictly_equal(context, seen.get(), original.get());
  }
}

auto execute(BridgeContext& context, const Input& input,
             std::uint64_t* admitted_scope = nullptr, Fault fault = Fault::none)
    -> Observation {
  REQUIRE(clean(context));
  context.next_callback_id = INITIAL_ID;
  context.batch_state.depth = input.nested ? 1 : 0;
  context.batch_state.scope_id = input.nested ? 987 : 0;
  context.batch_state.ops_in_batch = input.nested ? 2 : 0;
  const auto manager = context.extension_database_clients;
  auto setup = evaluate(context, setup_source(input));
  REQUIRE_FALSE(JS_IsException(setup.get()));
  Value global{context.ctx, JS_GetGlobalObject(context.ctx)};
  Value db{context.ctx, JS_GetPropertyStr(context.ctx, global.get(), "db")};
  Value batch{context.ctx, JS_GetPropertyStr(context.ctx, db.get(), "batch")};
  Value function{context.ctx,
                 JS_GetPropertyStr(context.ctx, global.get(), "__batch_fn")};
  REQUIRE(JS_IsFunction(context.ctx, batch.get()));
  std::array<JSValue, 2> arguments{function.get(), JS_NULL};
  Observation observation;
  {
    OpaqueRestore opaque{context.ctx, input.unavailable};
    context.cancelled.store(input.cancelled);
    Value promise{context.ctx, JS_Call(context.ctx, batch.get(), db.get(),
                                       input.argc, arguments.data())};
    if (JS_IsException(promise.get())) {
      Value exception{context.ctx, JS_GetException(context.ctx)};
      observation.exception_name =
          property_text(context.ctx, exception.get(), "name");
      observation.exception_message =
          property_text(context.ctx, exception.get(), "message");
    } else {
      observation.immediate = JS_PromiseState(context.ctx, promise.get());
    }
    observation.initial_state =
        context.cancelled.load() == input.cancelled &&
        context.extension_name == "notes" &&
        context.extension_database_clients == manager &&
        context.batch_state.depth ==
            (input.admission ? (input.nested ? 1 : 0) : 1) &&
        context.batch_state.ops_in_batch == (input.nested ? 2U : 0U) &&
        (input.admission
             ? context.batch_state.scope_id == (input.nested ? 987U : 0U)
             : context.batch_state.scope_id != 0);
    if (admitted_scope != nullptr) {
      *admitted_scope = context.batch_state.scope_id;
    }
    context.cancelled.store(false); // Only after observing public admission.
    if (!JS_IsException(promise.get())) {
      Value then{context.ctx,
                 JS_GetPropertyStr(context.ctx, promise.get(), "then")};
      Value handler{context.ctx, JS_GetPropertyStr(context.ctx, global.get(),
                                                   "__batch_observe")};
      std::array<JSValue, 2> handlers{handler.get(), handler.get()};
      Value reaction{context.ctx, JS_Call(context.ctx, then.get(),
                                          promise.get(), 2, handlers.data())};
      REQUIRE_FALSE(JS_IsException(reaction.get()));
      simulate_operations(context, input, observation);
      drain(context);
      observe_promise(context, promise.get(), input, observation, fault);
      observation.reaction =
          observation.reaction &&
          JS_PromiseState(context.ctx, reaction.get()) == JS_PROMISE_FULFILLED;
    }
  }
  if (fault == Fault::invoke_admission_callback) {
    REQUIRE(input.admission);
    REQUIRE(JS_IsFunction(context.ctx, function.get()));
    // Corrupt only the test callback's invocation count after public admission
    // was observed. This exercises its real instrumentation on sync-error and
    // inline-rejection paths without replacing the registered function.
    Value invoked{context.ctx, JS_Call(context.ctx, function.get(),
                                       JS_UNDEFINED, 0, nullptr)};
    REQUIRE_FALSE(JS_IsException(invoked.get()));
  }
  // Include synchronous TypeError paths in the callback and reaction oracle.
  observation.body_calls = read_integer(context, "__batch_body_calls");
  observation.hits = read_integer(context, "__batch_hits");
  observation.next_id = context.next_callback_id;
  observation.clean =
      clean(context) &&
      (input.admission ||
       (context.batch_state.depth == 0 && context.batch_state.scope_id == 0 &&
        context.batch_state.ops_in_batch == 0 &&
        !context.batch_state.timed_out &&
        context.batch_state.pinned_conn.use_count() == 0 &&
        !context.batch_state.rollback_owner &&
        context.batch_state.timer_loop == nullptr &&
        context.batch_state.timer_id == trantor::InvalidTimerId));
  // Admission sentinels are owned by this host simulation, not native work.
  context.batch_state = BridgeContext::BatchState{};
  return observation;
}

auto expected_types(const Input& input) -> std::vector<Type> {
  if (input.admission) {
    return {};
  }
  std::vector<Type> result{Type::DB_BATCH_BEGIN};
  if (begin_failed(input.flow)) {
    return result;
  }
  for (int index = 0; index < input.operations; ++index) {
    result.push_back(index % 2 == 0 ? Type::DB_EXEC : Type::DB_QUERY);
  }
  result.push_back(rollback_flow(input.flow) ? Type::DB_BATCH_ROLLBACK
                                             : Type::DB_BATCH_COMMIT);
  return result;
}

auto operation_oracle(const Input& input, const Operation& operation,
                      std::size_t index, Type type) -> bool {
  const bool dml = type == Type::DB_EXEC || type == Type::DB_QUERY;
  const bool begin = type == Type::DB_BATCH_BEGIN;
  const auto dml_index = static_cast<int>(index) - 1;
  const auto count =
      begin || dml ? 0U : static_cast<std::size_t>(input.operations);
  const auto error =
      type == Type::DB_BATCH_ROLLBACK ? expected_result(input) : Json::Value{};
  return operation.type == type &&
         operation.id == INITIAL_ID + static_cast<int>(index) &&
         operation.count == count && operation.scope && operation.manager &&
         operation.extension == "notes" && operation.unrelated &&
         operation.transaction == !begin &&
         operation.rollback_owner == !begin && operation.transferred &&
         operation.error == error &&
         operation.sql == (dml ? expected_sql(dml_index) : "") &&
         operation.params == (dml ? expected_params(input, dml_index)
                                  : std::vector<Json::Value>{}) &&
         operation.silent == (type == Type::DB_EXEC);
}

auto oracle(const Input& input, const Observation& observation)
    -> std::optional<std::string> {
  if (observation.exception_message != input.exception ||
      observation.exception_name !=
          (input.exception.empty() ? "" : "TypeError")) {
    return "exception";
  }
  const auto types = expected_types(input);
  if (observation.operations.size() != types.size()) {
    return "operations";
  }
  for (std::size_t index = 0; index < types.size(); ++index) {
    if (!operation_oracle(input, observation.operations[index], index,
                          types[index])) {
      return "operation snapshot";
    }
  }
  if (observation.next_id != INITIAL_ID + static_cast<int>(types.size()) ||
      !observation.inventory || !observation.initial_state) {
    return "inventory";
  }
  if (!observation.snapshots || !observation.owners_released) {
    return "ownership";
  }
  if (observation.body_calls !=
      (input.admission || begin_failed(input.flow) ? 0 : 1)) {
    return "body invocation";
  }
  if (observation.original_identity.has_value() !=
          (!input.admission && preserves_body_identity(input.flow)) ||
      (observation.original_identity.has_value() &&
       !*observation.original_identity) ||
      !observation.reaction_identity) {
    return "identity";
  }
  if (!input.exception.empty() && observation.hits != 0) {
    return "settlement";
  }
  if (input.exception.empty()) {
    const bool success = !input.admission && (input.flow == Flow::value ||
                                              input.flow == Flow::promise);
    const auto result =
        input.admission ? input.rejection : expected_result(input);
    if (observation.immediate !=
            (input.admission ? JS_PROMISE_REJECTED : JS_PROMISE_PENDING) ||
        observation.final_state !=
            (success ? JS_PROMISE_FULFILLED : JS_PROMISE_REJECTED) ||
        observation.result != result || observation.hits != 1 ||
        !observation.reaction) {
      return "settlement";
    }
  }
  if (!observation.clean || !observation.late_clean) {
    return "cleanup";
  }
  return std::nullopt;
}

auto replay(const Input& input, Fault fault = Fault::none) -> Observation {
  plinth::Config config{};
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   nullptr, "notes");
  Observation observation;
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    observation = execute(*lease.get(), input, nullptr, fault);
    REQUIRE(JS_GetContextOpaque(lease.get()->ctx) == lease.get());
    lease.release(); // All JS values die before context reset/reuse.
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  return observation;
}

auto replay_with_fault(const Input& input, Fault fault) -> Observation {
  auto observation = replay(input);
  if (fault == Fault::drop_operation && !observation.operations.empty()) {
    observation.operations.pop_back();
  }
  return observation;
}

struct Shrunk {
  Input input;
  int attempts = 0;
};

auto shrink(Input input, std::string_view failure, Fault fault) -> Shrunk {
  int attempts = 0;
  if (input.admission) {
    return {.input = std::move(input), .attempts = attempts};
  }
  for (int step = 0; step < 3 && attempts < MAX_SHRINK_ATTEMPTS; ++step) {
    auto candidate = input;
    if (step == 0) {
      candidate.operations = 0;
    } else if (step == 1) {
      candidate.number = 0;
    } else {
      candidate.label.clear();
    }
    ++attempts;
    const auto found = oracle(candidate, replay_with_fault(candidate, fault));
    if (found.has_value() && *found == failure) {
      input = std::move(candidate);
    }
  }
  return {.input = std::move(input), .attempts = attempts};
}

auto require_observation(const Input& input, const Observation& observation)
    -> void {
  const auto failure = oracle(input, observation);
  if (failure.has_value()) {
    const auto reduced = shrink(input, *failure, Fault::none);
    INFO("failure=" << *failure << " attempts=" << reduced.attempts
                    << " input=" << setup_source(reduced.input));
  }
  REQUIRE_FALSE(failure.has_value());
}

auto clone_identity_controls() -> int {
  constexpr std::array<int, 6> CHOICES{1, 2, 4, 6, 7, 11};
  int detected = 0;
  for (const int choice : CHOICES) {
    const auto input = make_input(SEEDS.front(), ADMISSION_FAMILIES + choice);
    const auto valid = replay(input);
    require_observation(input, valid);
    REQUIRE(valid.original_identity == std::optional<bool>{true});
    const auto clone = replay(input, Fault::clone_settlement);
    REQUIRE(clone.result == valid.result);
    REQUIRE(clone.operations == valid.operations);
    REQUIRE(clone.original_identity == std::optional<bool>{false});
    REQUIRE_FALSE(clone.reaction_identity);
    REQUIRE(oracle(input, clone) == std::optional<std::string>{"identity"});
    ++detected;
  }
  REQUIRE(detected == 6);
  return detected;
}

auto admission_invocation_controls() -> int {
  int detected = 0;
  for (int family = 24; family <= 28; ++family) {
    const auto input = make_input(SEEDS.front(), family);
    const auto valid = replay(input);
    require_observation(input, valid);
    REQUIRE(valid.body_calls == 0);
    const auto invoked = replay(input, Fault::invoke_admission_callback);
    REQUIRE(invoked.body_calls == 1);
    REQUIRE(invoked.exception_message == valid.exception_message);
    REQUIRE(invoked.result == valid.result);
    REQUIRE(invoked.operations.empty());
    REQUIRE(oracle(input, invoked) ==
            std::optional<std::string>{"body invocation"});
    ++detected;
  }
  REQUIRE(detected == 5);
  return detected;
}

auto negative_controls() -> void {
  const auto input = make_input(SEEDS.front(), ADMISSION_FAMILIES + 4);
  const auto valid = replay(input);
  require_observation(input, valid);
  constexpr int CONTROLS = 32;
  int detected = 0;
  for (int index = 0; index < CONTROLS; ++index) {
    auto broken = valid;
    switch (index) {
      case 0: broken.exception_name = "TypeError"; break;
      case 1: broken.exception_message = "fake unexpected error"; break;
      case 2: broken.operations.clear(); break;
      case 3: ++broken.next_id; break;
      case 4: broken.inventory = false; break;
      case 5: broken.initial_state = false; break;
      case 6: broken.snapshots = false; break;
      case 7: broken.owners_released = false; break;
      case 8: broken.operations.back().type = Type::CAP_CALL; break;
      case 9: ++broken.operations.back().id; break;
      case 10: ++broken.operations.back().count; break;
      case 11: broken.operations.back().scope = false; break;
      case 12: broken.operations.back().manager = false; break;
      case 13: broken.operations.back().transaction = false; break;
      case 14: broken.operations.back().rollback_owner = false; break;
      case 15: broken.operations.back().transferred = false; break;
      case 16: broken.operations.back().unrelated = false; break;
      case 17: broken.operations.back().error = "fake"; break;
      case 18: broken.operations[1].sql = "changed"; break;
      case 19: broken.operations[1].params.clear(); break;
      case 20: broken.operations[1].silent = false; break;
      case 21: broken.operations[1].extension = "changed"; break;
      case 22: broken.immediate = JS_PROMISE_FULFILLED; break;
      case 23: broken.final_state = JS_PROMISE_PENDING; break;
      case 24: broken.result = "fake result"; break;
      case 25: ++broken.body_calls; break;
      case 26: ++broken.hits; break;
      case 27: broken.reaction = false; break;
      case 28: broken.clean = false; break;
      case 29: broken.reaction_identity = false; break;
      case 30: broken.original_identity = false; break;
      case 31: broken.original_identity.reset(); break;
      default: FAIL("unknown corruption control");
    }
    REQUIRE(oracle(input, broken).has_value());
    ++detected;
  }
  auto late = valid;
  late.late_clean = false;
  REQUIRE(oracle(input, late).has_value());
  ++detected;
  detected += clone_identity_controls();
  detected += admission_invocation_controls();
  REQUIRE(detected == 44);
  const auto reduced = shrink(input, "operations", Fault::drop_operation);
  REQUIRE(reduced.attempts == 3);
  REQUIRE(reduced.attempts <= MAX_SHRINK_ATTEMPTS);
  REQUIRE(reduced.input.operations == 0);
  REQUIRE(reduced.input.number == 0);
  REQUIRE(reduced.input.label.empty());
  REQUIRE(oracle(reduced.input,
                 replay_with_fault(reduced.input, Fault::drop_operation)) ==
          std::optional<std::string>{"operations"});
  require_observation(reduced.input, replay(reduced.input));
}

auto ownership_controls() -> void {
  plinth::Config config{};
  RuntimePool pool(nullptr, plinth::js::default_runtime_limits(), config, 1,
                   nullptr, "notes");
  const auto input = make_input(SEEDS.front(), ADMISSION_FAMILIES + 2);
  std::array<std::uint64_t, 2> scopes{};
  for (int repetition = 0; repetition < 2; ++repetition) {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    REQUIRE(clean(*lease.get()));
    REQUIRE(lease.get()->next_callback_id == 0);
    REQUIRE(lease.get()->batch_state.depth == 0);
    require_observation(
        input, execute(*lease.get(), input,
                       &scopes.at(static_cast<std::size_t>(repetition))));
    lease.release();
    REQUIRE(pool.free_count() == 1);
  }
  REQUIRE(scopes[0] != 0);
  REQUIRE(scopes[1] != 0);
  REQUIRE(scopes[0] != scopes[1]);
  Owners owners;
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    {
      auto promise = evaluate(*lease.get(), "db.batch(()=>1)");
      REQUIRE(JS_PromiseState(lease.get()->ctx, promise.get()) ==
              JS_PROMISE_PENDING);
      owners = install_owners(*lease.get(), input);
      REQUIRE(lease.get()->callbacks.size() == 1);
      REQUIRE(lease.get()->pending_ops.size() == 1);
    }
    // All test JSValues are already gone; pool owns dirty callback references.
    lease.release();
    REQUIRE(pool.active_count() == 0);
    REQUIRE(pool.free_count() == 0);
    REQUIRE(owners.transaction.expired());
    REQUIRE(owners.rollback.expired());
  }
  {
    Lease lease{pool};
    REQUIRE(lease.get() != nullptr);
    REQUIRE(clean(*lease.get()));
    require_observation(input, execute(*lease.get(), input));
    {
      auto marker =
          evaluate(*lease.get(), "globalThis.__batch_rebuild_marker=739");
      REQUIRE_FALSE(JS_IsException(marker.get()));
    }
    REQUIRE(pool.active_count() == 1);
    REQUIRE(pool.free_count() == 0);
    pool.rebuild();
    REQUIRE(pool.active_count() == 1);
    REQUIRE(pool.free_count() == 1);
    {
      auto retained = evaluate(
          *lease.get(),
          "__batch_rebuild_marker===739&&typeof db.batch==='function'");
      REQUIRE_FALSE(JS_IsException(retained.get()));
      REQUIRE(JS_ToBool(lease.get()->ctx, retained.get()) == 1);
    }
    // A checked-out lease prevents invalidating live QuickJS ownership.
    REQUIRE_FALSE(pool.shutdown(std::chrono::milliseconds{0}));
    REQUIRE(pool.acquire() == nullptr);
    REQUIRE(pool.active_count() == 1);
    {
      auto alive = evaluate(
          *lease.get(),
          "__batch_rebuild_marker===739&&typeof db.batch==='function'");
      REQUIRE_FALSE(JS_IsException(alive.get()));
      REQUIRE(JS_ToBool(lease.get()->ctx, alive.get()) == 1);
    }
    lease.destroy();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
  REQUIRE(pool.acquire() == nullptr);
}

} // namespace

TEST_CASE("QuickJS db.batch has a bounded deterministic admission and "
          "orchestration corpus",
          "[js][db][corpus][isolated-db-batch]") {
  INFO("seeds=" << SEEDS.size() << " families=" << FAMILIES
                << " max_source_bytes=" << MAX_SOURCE_BYTES
                << " max_input_nodes=" << MAX_INPUT_NODES << " max_input_depth="
                << MAX_INPUT_DEPTH << " max_text_bytes=" << MAX_TEXT_BYTES
                << " max_operations=" << MAX_OPERATIONS << " max_jobs="
                << MAX_JOBS << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS);
  REQUIRE_FALSE(plinth::log::is_audit_ready());
  std::array<int, FAMILIES> coverage{};
  int admissions = 0;
  int flows = 0;
  int dml = 0;
  int cases = 0;
  for (const auto seed : SEEDS) {
    for (int family = 0; family < FAMILIES; ++family) {
      INFO("seed=" << seed << " family=" << family);
      const auto input = make_input(seed, family);
      REQUIRE(input.label.size() <= MAX_TEXT_BYTES);
      REQUIRE(input.operations >= 0);
      REQUIRE(input.operations <= 3);
      // Fixed grammar: result object (3 nodes), array/four scalars/SQL per
      // DML, plus callback/argument nodes; no recursive input production.
      REQUIRE(3 + input.operations * 6 + 3 <= MAX_INPUT_NODES);
      REQUIRE(3 <= MAX_INPUT_DEPTH);
      REQUIRE(setup_source(input).size() <= MAX_SOURCE_BYTES);
      const auto first = replay(input);
      const auto second = replay(input);
      require_observation(input, first);
      require_observation(input, second);
      REQUIRE(first == second);
      ++coverage.at(static_cast<std::size_t>(family));
      input.admission ? ++admissions : ++flows;
      dml += input.operations;
      ++cases;
    }
  }
  REQUIRE(cases == 138);
  REQUIRE(admissions == 87);
  REQUIRE(flows == 51);
  REQUIRE(dml == 21);
  for (const int count : coverage) {
    REQUIRE(count == 3);
  }
  negative_controls();
  ownership_controls();
  REQUIRE_FALSE(plinth::log::is_audit_ready());
}
