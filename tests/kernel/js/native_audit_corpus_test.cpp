// SPDX-License-Identifier: MIT
// Actual injected audit.log -> native dispatcher -> asynchronous PG writer.
// Settlement is NOT a durability acknowledgement. Profiles are synthetic
// BridgeContext snapshots, not HTTP session or PAT authentication evidence.

#include "async_bridge_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <json/json.h>
#include <libpq-fe.h>
#include <quickjs.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
constexpr std::size_t PROFILES = 3;
constexpr std::size_t FAMILIES = 4;
constexpr std::size_t REPLAYS = 2;
constexpr std::size_t WRITES = PROFILES * FAMILIES * REPLAYS;
constexpr std::string_view MUTATED_EXTENSION = "audit_changed";
constexpr std::string_view MUTATED_USER =
    "11111111-1111-4111-8111-000000001999";
constexpr std::string_view MUTATED_SESSION =
    "22222222-2222-4222-8222-000000001999";
constexpr std::string_view MUTATED_IP = "192.0.2.199";
constexpr int MUTATED_DEPTH = 7;

auto demand(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

auto equal(const Json::Value& actual, const Json::Value& expected) -> bool {
  if (actual.isNumeric() && expected.isNumeric())
    return actual.asDouble() == expected.asDouble();
  if (actual.type() != expected.type()) return false;
  if (expected.isObject()) {
    if (actual.getMemberNames() != expected.getMemberNames()) return false;
    for (const auto& key : expected.getMemberNames())
      if (!equal(actual[key], expected[key])) return false;
    return true;
  }
  if (expected.isArray()) {
    if (actual.size() != expected.size()) return false;
    for (Json::ArrayIndex index = 0; index < expected.size(); ++index)
      if (!equal(actual[index], expected[index])) return false;
    return true;
  }
  return actual == expected;
}

auto units(std::initializer_list<int> values) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const auto value : values)
    result.append(value);
  return result;
}

struct Profile {
  std::string extension;
  plinth::capabilities::UserContext user;
  int depth = 0;
  auto owner() const -> std::string {
    return extension.empty() ? "host" : extension;
  }
};

auto profile(std::size_t index) -> Profile {
  Profile result;
  if (index == 0) {
    result.extension = "auditnative";
    result.user.user_id = "11111111-1111-4111-8111-000000001911";
    result.user.session_id = "22222222-2222-4222-8222-000000001911";
    result.user.ip_address = "192.0.2.191";
  } else if (index == 1) {
    result.extension = "auditother";
    result.user.user_id = "11111111-1111-4111-8111-000000001912";
    result.user.ip_address = "2001:db8::191";
    result.depth = 3;
  } else {
    result.depth = 1;
  }
  return result;
}

struct Expected {
  Json::Value payload{Json::objectValue};
  Json::Value text_units{Json::arrayValue};
  Json::Value key_units{Json::arrayValue};
  int encoding = 0;
};

// Literal independent expectations, never production conversions/encoders.
auto expected(std::size_t family) -> Expected {
  Expected result;
  if (family == 1) {
    result.payload["nested"]["user_id"] = "application-detail";
    result.payload["nested"]["timestamp"] = "not-provenance";
    result.payload["items"] = Json::Value{Json::arrayValue};
    result.payload["items"].append(Json::Value{});
    result.payload["items"].append(false);
    result.payload["items"].append(-17);
    result.payload["items"].append(1.5);
    result.payload["empty"] = "";
  } else if (family == 2) {
    result.payload["text"] = "\xc3\xa9\xce\xa9\xf0\x9f\x98\x80";
    result.payload["\xe4\xb8\xad\xf0\x9f\x98\x80"] = "value";
    result.payload["literal"] = R"(\u0000)";
    result.payload["detail_encoding"] = 1; // A caller marker is not authority.
    result.payload["json"] = "not encoded";
    result.text_units = units({233, 937, 55357, 56832});
    result.key_units = units({20013, 55357, 56832});
  } else if (family == 3) {
    result.payload["text"] = std::string{"a\0b", 3};
    result.payload[std::string{"k\0end", 5}] = std::string{"\0", 1};
    result.payload["literal"] = R"(\u0000)";
    result.text_units = units({97, 0, 98});
    result.key_units = units({107, 0, 101, 110, 100});
    result.encoding = 1;
  }
  return result;
}

auto payload_source(std::size_t family) -> std::string_view {
  constexpr std::array<std::string_view, FAMILIES> SOURCES{
      "({})",
      "({nested:{user_id:'application-detail',timestamp:'not-provenance'},"
      "items:[null,false,-17,1.5],empty:''})",
      R"(({text:'\u00e9\u03a9\ud83d\ude00','\u4e2d\ud83d\ude00':'value',literal:'\\u0000',detail_encoding:1,json:'not encoded'}))",
      R"(({text:'a\u0000b','k\u0000end':'\u0000',literal:'\\u0000'}))"};
  return SOURCES.at(family);
}

auto source(std::size_t family, const std::string& action) -> std::string {
  return "const input=" + std::string{payload_source(family)} +
         R"JS(;
const specialKey=Object.keys(input).find(key=>key.charCodeAt(0)>127||key.includes('\u0000'))||'';
function units(text){return Array.from({length:text.length},(_,i)=>text.charCodeAt(i));}
(async()=>{
  const textUnits=units(input.text||'');const keyUnits=units(specialKey);
  let settlements=0;
  const promise=audit.log(')JS" +
         action + R"JS(',input);
  input.after=99;if(input.nested)input.nested.user_id='changed-after-enqueue';
  const mutated=__native_audit_mutate();
  await promise.then(()=>{++settlements;});
  return {settlements,mutated,inputChanged:input.after===99&&(!input.nested||input.nested.user_id==='changed-after-enqueue'),textUnits,keyUnits};
})()
)JS";
}

// Runs synchronously in the driver's initial JS evaluation, AFTER audit.log
// copied its inputs and BEFORE native dispatch. No borrowed state is retained.
auto mutate_caller(JSContext* ctx, JSValue, int, JSValue*) -> JSValue {
  auto* context =
      static_cast<plinth::js::BridgeContext*>(JS_GetContextOpaque(ctx));
  if (context == nullptr)
    return JS_ThrowInternalError(ctx, "missing audit owner");
  try {
    if (context->pending_ops.size() != 1 ||
        context->pending_ops.front().type !=
            plinth::js::AsyncOp::Type::AUDIT_WRITE)
      return JS_ThrowInternalError(ctx, "audit mutation must follow enqueue");
    context->extension_name = MUTATED_EXTENSION;
    context->user.user_id = MUTATED_USER;
    context->user.session_id = MUTATED_SESSION;
    context->user.ip_address = MUTATED_IP;
    context->call_depth = MUTATED_DEPTH;
    return JS_TRUE;
  } catch (...) {
    return JS_ThrowInternalError(ctx, "audit fixture mutation failed");
  }
}

auto parse(std::string_view text) -> Json::Value {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
  Json::Value result;
  std::string error;
  demand(reader->parse(text.data(), text.data() + text.size(), &result, &error),
         "native audit independent JSON parsing failed");
  return result;
}

auto uuid(std::string_view value) -> bool {
  if (value.size() != 36 || value[14] != '4' ||
      std::string_view{"89ab"}.find(value[19]) == std::string_view::npos)
    return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') return false;
    } else if (std::string_view{"0123456789abcdef"}.find(value[index]) ==
               std::string_view::npos)
      return false;
  }
  return true;
}

struct Row {
  int count = 0;
  std::string id;
  std::string action;
  std::optional<std::string> user;
  std::optional<std::string> session;
  std::optional<std::string> ip;
  std::string node;
  int encoding = -1;
  bool wire_shape = false;
  Json::Value detail;
  double timestamp = 0;
};

class AuditPg {
 public:
  explicit AuditPg(const plinth::Config::Database& config) {
    const auto info = plinth::db::connection_info(config) +
                      " connect_timeout=3 options='-c statement_timeout=2000 "
                      "-c lock_timeout=1500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand(connection && PQstatus(connection.get()) == CONNECTION_OK,
           "native audit bounded control connection failed");
  }
  auto query(const std::string& sql, const std::vector<std::string>& args = {})
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    std::vector<const char*> values;
    values.reserve(args.size());
    for (const auto& arg : args)
      values.push_back(arg.c_str());
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(connection.get(), sql.c_str(),
                     static_cast<int>(values.size()), nullptr, values.data(),
                     nullptr, nullptr, 0),
        PQclear};
    demand(result && (PQresultStatus(result.get()) == PGRES_TUPLES_OK ||
                      PQresultStatus(result.get()) == PGRES_COMMAND_OK),
           "native audit bounded control statement failed");
    return result;
  }
  auto count(const std::string& action) -> int {
    const auto result = query(
        "SELECT count(*) FROM plinth.audit_log WHERE action=$1", {action});
    demand(PQntuples(result.get()) == 1, "native audit count cardinality");
    return std::stoi(text(*result, 0, 0));
  }
  auto now() -> double {
    const auto result =
        query("SELECT extract(epoch FROM clock_timestamp())::text");
    demand(PQntuples(result.get()) == 1,
           "native audit server-time cardinality");
    return std::stod(text(*result, 0, 0));
  }
  auto read(const std::string& action) -> Row {
    const auto result =
        query("SELECT "
              "id::text,action,user_id::text,session_id::text,host(ip_address),"
              "node_id,detail_encoding::text,detail::text,"
              "extract(epoch FROM timestamp)::text FROM plinth.audit_log WHERE "
              "action=$1",
              {action});
    Row row;
    row.count = PQntuples(result.get());
    if (row.count != 1) return row;
    row.id = text(*result, 0, 0);
    row.action = text(*result, 0, 1);
    row.user = nullable(*result, 2);
    row.session = nullable(*result, 3);
    row.ip = nullable(*result, 4);
    row.node = text(*result, 0, 5);
    row.encoding = std::stoi(text(*result, 0, 6));
    const auto wire = parse(text(*result, 0, 7));
    row.wire_shape = row.encoding == 0 ? wire.isObject()
                                       : row.encoding == 1 && wire.isString();
    // Independently implement the documented out-of-band wire protocol, not
    // production decode_detail or any caller-controlled sentinel.
    if (row.wire_shape)
      row.detail = row.encoding == 0 ? wire : parse(wire.asString());
    row.timestamp = std::stod(text(*result, 0, 8));
    return row;
  }
  auto pid() const -> int { return PQbackendPID(connection.get()); }
  auto blocked_insert(int blocker) -> int {
    const auto result =
        query("SELECT a.pid FROM pg_stat_activity a WHERE "
              "a.datname=current_database() "
              "AND a.state='active' AND a.wait_event_type='Lock' "
              "AND a.query LIKE 'INSERT INTO plinth.audit_log%' "
              "AND $1::int=ANY(pg_blocking_pids(a.pid)) "
              "AND EXISTS(SELECT 1 FROM pg_locks l WHERE l.pid=a.pid "
              "AND l.relation='plinth.audit_log'::regclass "
              "AND l.mode='RowExclusiveLock' AND NOT l.granted)",
              {std::to_string(blocker)});
    demand(PQntuples(result.get()) <= 1, "native audit waiter is not unique");
    return PQntuples(result.get()) == 1 ? std::stoi(text(*result, 0, 0)) : 0;
  }
  auto disconnect() noexcept -> void { connection.reset(); }

 private:
  static auto text(const PGresult& result, int row, int column) -> std::string {
    demand(PQgetisnull(&result, row, column) == 0,
           "native audit unexpected NULL");
    return {PQgetvalue(&result, row, column),
            static_cast<std::size_t>(PQgetlength(&result, row, column))};
  }
  static auto nullable(const PGresult& result, int column)
      -> std::optional<std::string> {
    if (PQgetisnull(&result, 0, column) != 0) return std::nullopt;
    return text(result, 0, column);
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
};

class Events {
 public:
  explicit Events(const plinth::Config::Database& database) : pg(database) {
    static std::atomic<unsigned> sequence{0};
    suffix = "p" + std::to_string(getpid()) + "s" +
             std::to_string(sequence.fetch_add(1));
  }
  ~Events() {
    if (!finish()) std::terminate();
  }
  Events(const Events&) = delete;
  auto operator=(const Events&) -> Events& = delete;
  auto own(const std::string& owner, const std::string& cell) -> std::string {
    return own_action("ext." + owner + ".native_" + suffix + cell);
  }
  auto own_action(const std::string& action) -> std::string {
    demand(pg.count(action) == 0, "native audit refuses pre-existing event");
    actions.push_back(action);
    return action;
  }
  auto finish() noexcept -> bool {
    if (closed) return true;
    try {
      if (!plinth::lifecycle::async_tasks().drain(5s) ||
          !plinth::log::wait_for_audits(5s))
        return false;
      for (const auto& action : actions) {
        static_cast<void>(
            pg.query("DELETE FROM plinth.audit_log WHERE action=$1", {action}));
        if (pg.count(action) != 0) return false;
      }
      closed = true;
      return true;
    } catch (...) {
      return false;
    }
  }
  AuditPg pg;
  std::string suffix;

 private:
  std::vector<std::string> actions;
  bool closed = false;
};

struct Cleanup {
  bool retired = false;
  std::size_t pending = 0;
  std::size_t retired_pending = 0;
  std::size_t callbacks = 0;
  std::size_t persistent = 0;
  int concurrent = 0;
  int detached = 0;
  int jobs = 0;
  bool exception = false;
  int batch_depth = 0;
  std::uint64_t scope = 0;
  bool pinned = false;
  bool rollback = false;
  bool timer = false;
  bool timer_loop = false;
  bool native_drained = false;
  bool audits_drained = false;
  std::size_t native_active = 0;
  int active = 0;
  auto operator==(const Cleanup&) const -> bool = default;
};

struct Shutdown {
  bool stopped = false;
  bool idempotent = false;
  int active = -1;
  int free = -1;
  auto operator==(const Shutdown&) const -> bool = default;
};

struct Execution {
  bool completed = false;
  Json::Value javascript;
  int admitted = -1;
  bool authority_mutated = false;
};

auto limits() -> plinth::js::RuntimeLimits {
  auto result = plinth::js::default_runtime_limits();
  result.wall_clock_limit = 3s;
  return result;
}

class Owner {
 public:
  Owner(const plinth::Config& config_in, const Profile& identity)
      : config(config_in),
        pool(nullptr, limits(), config, 1, &identity.user, identity.extension) {
    context = pool.acquire();
    demand(context != nullptr, "native audit context acquisition failed");
    try {
      context->call_depth = identity.depth;
      JSValue global = JS_GetGlobalObject(context->ctx);
      demand(!JS_IsException(global), "native audit global acquisition failed");
      JSValue function = JS_NewCFunction(context->ctx, mutate_caller,
                                         "__native_audit_mutate", 0);
      if (JS_IsException(function)) {
        JS_FreeValue(context->ctx, global);
        throw std::runtime_error(
            "native audit fixture function allocation failed");
      }
      // JS_SetPropertyStr consumes function on both success and failure.
      const int installed = JS_SetPropertyStr(
          context->ctx, global, "__native_audit_mutate", function);
      JS_FreeValue(context->ctx, global);
      demand(installed >= 0, "native audit fixture function injection failed");
    } catch (...) {
      // A failed constructor has no Owner destructor. This freshly acquired
      // context has never entered a driver and must not strand a pool lease.
      pool.destroy(std::exchange(context, nullptr));
      if (!pool.shutdown(5s)) std::terminate();
      throw;
    }
  }
  ~Owner() {
    try {
      if (!closed && !close()) std::terminate();
    } catch (...) {
      std::terminate();
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;
  auto drive(const std::string& program) -> Execution {
    demand(program.size() < 8192 && !driven,
           "native audit source/driver bound");
    driven = true;
    const auto result =
        drogon::sync_wait(plinth::js::run_on_context(*context, program));
    return {result.value.has_value(), result.value.value_or(Json::Value{}),
            context->next_callback_id,
            context->extension_name == MUTATED_EXTENSION &&
                context->user.user_id == MUTATED_USER &&
                context->user.session_id == MUTATED_SESSION &&
                context->user.ip_address == MUTATED_IP &&
                context->call_depth == MUTATED_DEPTH};
  }
  auto close() -> bool {
    if (closed) return true;
    const bool native = plinth::lifecycle::async_tasks().drain(5s);
    const bool audits = plinth::log::wait_for_audits(5s);
    if (!native || !audits) return false;
    if (context != nullptr) {
      if (context->execution_lifetime.load(std::memory_order_acquire))
        return false;
      const auto& batch = context->batch_state;
      cleanup = {true,
                 context->pending_ops.size(),
                 context->retired_pending_ops.size(),
                 context->callbacks.size(),
                 context->persistent_callbacks.size(),
                 context->concurrent_async_ops,
                 context->inflight_detached.load(std::memory_order_acquire),
                 JS_IsJobPending(context->rt),
                 JS_HasException(context->ctx),
                 batch.depth,
                 batch.scope_id,
                 static_cast<bool>(batch.pinned_conn),
                 static_cast<bool>(batch.rollback_owner),
                 batch.timer_id != trantor::InvalidTimerId,
                 batch.timer_loop != nullptr,
                 native,
                 audits,
                 plinth::lifecycle::async_tasks().active_for_test(),
                 0};
      pool.destroy(std::exchange(context, nullptr));
      cleanup.active = pool.active_count();
    }
    shutdown = {pool.shutdown(5s), pool.shutdown(0ms), pool.active_count(),
                pool.free_count()};
    closed = shutdown == Shutdown{true, true, 0, 0};
    return closed;
  }
  Cleanup cleanup;
  Shutdown shutdown;

 private:
  plinth::Config config;
  plinth::js::RuntimePool pool;
  plinth::js::BridgeContext* context = nullptr;
  bool driven = false;
  bool closed = false;
};

auto clean(const Cleanup& actual) -> bool {
  return actual == Cleanup{.retired = true,
                           .native_drained = true,
                           .audits_drained = true};
}

auto nullable(const std::string& value) -> std::optional<std::string> {
  if (value.empty()) return std::nullopt;
  return value;
}

struct Observation {
  std::size_t profile_index = 0;
  std::size_t family = 0;
  std::size_t replay = 0;
  std::string action;
  Execution execution;
  Row row;
  double before = 0;
  double after = 0;
  Cleanup cleanup;
  Shutdown shutdown;
};

auto matches(const Observation& actual) -> bool {
  if (actual.profile_index >= PROFILES || actual.family >= FAMILIES ||
      actual.replay >= REPLAYS)
    return false;
  const auto identity = profile(actual.profile_index);
  const auto want = expected(actual.family);
  auto detail = want.payload;
  detail["extension_id"] = identity.owner();
  detail["call_depth"] = identity.depth;
  Json::Value javascript{Json::objectValue};
  javascript["settlements"] = 1;
  javascript["mutated"] = true;
  javascript["inputChanged"] = true;
  javascript["textUnits"] = want.text_units;
  javascript["keyUnits"] = want.key_units;
  return actual.execution.completed && actual.execution.admitted == 1 &&
         actual.execution.authority_mutated &&
         equal(actual.execution.javascript, javascript) &&
         actual.row.count == 1 && uuid(actual.row.id) &&
         actual.row.action == actual.action &&
         actual.row.user == nullable(identity.user.user_id) &&
         actual.row.session == nullable(identity.user.session_id) &&
         actual.row.ip == nullable(identity.user.ip_address) &&
         actual.row.node == "test-node" &&
         actual.row.encoding == want.encoding && actual.row.wire_shape &&
         equal(actual.row.detail, detail) && actual.before > 0 &&
         actual.after >= actual.before &&
         actual.row.timestamp >= actual.before &&
         actual.row.timestamp <= actual.after && clean(actual.cleanup) &&
         actual.shutdown == Shutdown{true, true, 0, 0};
}

auto ledger_matches(const std::vector<Observation>& ledger) -> bool {
  if (ledger.size() != WRITES) return false;
  std::array<bool, WRITES> seen{};
  for (const auto& item : ledger) {
    if (!matches(item)) return false;
    const auto index =
        (item.profile_index * FAMILIES + item.family) * REPLAYS + item.replay;
    if (seen[index]) return false;
    seen[index] = true;
    for (const auto& other : ledger)
      if (&item != &other &&
          (item.action == other.action || item.row.id == other.row.id))
        return false;
  }
  return true;
}

auto initialize() -> plinth::Config {
  demand(plinth::async_bridge_test::pg_available(),
         "native audit requires PostgreSQL");
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  demand(plinth::lifecycle::async_tasks().drain(5s) &&
             plinth::log::wait_for_audits(5s),
         "native audit previous producers did not settle");
  // No extra schema reset, global seal or hand-copied shutdown chain.
  return plinth::async_bridge_test::test_config();
}

struct Denial {
  std::string expression;
  bool synchronous;
  std::string code;
  std::string message;
};

auto denial(std::size_t index, const std::string& action) -> Denial {
  switch (index) {
    case 0:
      return {"audit.log('" + action + "')", true, "TypeError",
              "audit.log: expected (event_type: string, payload: object)"};
    case 1:
      return {"audit.log(17,{})", true, "TypeError",
              "audit.log: event_type must be a string"};
    case 2:
      return {"audit.log('',{})", true, "TypeError",
              "audit.log: event_type must be non-empty"};
    case 3:
      return {"audit.log('" + action + "',{})", false, "audit.reserved_prefix",
              "event_type uses kernel-reserved prefix: user."};
    case 4:
      return {"audit.log('" + action + "',{})", false, "audit.invalid_prefix",
              "extension audit events must start with 'ext.auditnative.'"};
    default:
      return {"audit.log('" + action + "',{user_id:'forged'})", false,
              "audit.reserved_field",
              "payload contains non-forgeable field: user_id"};
  }
}

auto denial_source(const Denial& item) -> std::string {
  return "(()=>{let promise;try{promise=" + item.expression + R"JS(;}catch(e){
    return {synchronous:true,code:e.name,message:e.message,settlements:0};}
    return promise.then(()=>({settlements:1}),e=>({synchronous:false,code:e.code,message:e.message,settlements:0}));})()
)JS";
}

// Owns the SHARE lock BEFORE the driver starts. Explicit teardown opens the
// physical gate before waiting/joining or letting the Owner destructor drain.
class BlockedRun {
  AuditPg lock;

 public:
  BlockedRun(const plinth::Config& config, std::string program)
      : lock(config.db), owner(std::make_shared<Owner>(config, profile(0))) {
    try {
      static_cast<void>(lock.query("BEGIN"));
      held = true;
      static_cast<void>(
          lock.query("LOCK TABLE plinth.audit_log IN SHARE MODE"));
      std::promise<Execution> promise;
      result = promise.get_future().share();
      worker = std::thread([shared = owner, code = std::move(program),
                            answer = std::move(promise)]() mutable {
        try {
          answer.set_value(shared->drive(code));
        } catch (...) {
          answer.set_exception(std::current_exception());
        }
      });
    } catch (...) {
      // Member destructors still run on constructor failure; open the physical
      // gate before owner destruction, even if thread/promise setup failed.
      unlock();
      throw;
    }
  }
  ~BlockedRun() {
    try {
      if (!finish()) std::terminate();
    } catch (...) {
      std::terminate();
    }
  }
  BlockedRun(const BlockedRun&) = delete;
  auto operator=(const BlockedRun&) -> BlockedRun& = delete;
  auto blocker() const -> int { return lock.pid(); }
  auto acknowledged() const -> bool {
    return result.wait_for(3s) == std::future_status::ready;
  }
  auto execution() const -> Execution { return result.get(); }
  auto finish() -> bool {
    if (closed) return true;
    unlock();
    // Existing driver wall/abandon bounds are unchanged. Never unbounded-join
    // before the observable result, and never destroy a still-borrowed BC.
    if (worker.joinable()) {
      if (result.wait_for(10s) != std::future_status::ready) std::terminate();
      worker.join();
    }
    closed = owner->close();
    return closed;
  }
  std::shared_ptr<Owner> owner;

 private:
  auto unlock() noexcept -> void {
    if (!held) return;
    try {
      static_cast<void>(lock.query("ROLLBACK"));
    } catch (...) {
      lock.disconnect();
    }
    held = false;
  }
  std::shared_future<Execution> result;
  std::thread worker;
  bool held = false;
  bool closed = false;
};

} // namespace

TEST_CASE("Native audit writes preserve the owned corpus",
          "[js][audit][integration][native-audit-corpus]") {
  const auto config = initialize();
  Events events{config.db};
  std::vector<Observation> ledger;
  ledger.reserve(WRITES);
  unsigned mutations = 0;
  unsigned unit_mutations = 0;
  unsigned payload_mutations = 0;
  for (std::size_t caller = 0; caller < PROFILES; ++caller) {
    for (std::size_t family = 0; family < FAMILIES; ++family) {
      for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
        INFO("synthetic_profile=" << caller << " family=" << family
                                  << " replay=" << replay);
        const auto identity = profile(caller);
        const auto action =
            events.own(identity.owner(), "c" + std::to_string(caller) + "f" +
                                             std::to_string(family) + "r" +
                                             std::to_string(replay));
        Owner owner{config, identity};
        const auto before = events.pg.now();
        const auto execution = owner.drive(source(family, action));
        const bool closed = owner.close();
        const auto row = events.pg.read(action);
        const auto after = events.pg.now();
        Observation actual{caller,        family,        replay, action,
                           execution,     row,           before, after,
                           owner.cleanup, owner.shutdown};
        // Complete actual cleanup before any assertions or copied mutations.
        REQUIRE(closed);
        REQUIRE(matches(actual));
        const auto reject = [&](const Observation& changed) {
          REQUIRE_FALSE(matches(changed));
          ++mutations;
        };
        auto changed = actual;
        changed.execution.completed = false;
        reject(changed);
        for (const int count : {0, 2}) {
          changed = actual;
          changed.execution.admitted = count;
          reject(changed);
          changed = actual;
          changed.execution.javascript["settlements"] = count;
          reject(changed);
          changed = actual;
          changed.row.count = count;
          reject(changed);
        }
        changed = actual;
        changed.execution.authority_mutated = false;
        reject(changed);
        changed = actual;
        changed.execution.javascript["mutated"] = false;
        reject(changed);
        changed = actual;
        changed.execution.javascript["inputChanged"] = false;
        reject(changed);
        changed = actual;
        changed.row.id.clear();
        reject(changed);
        changed = actual;
        changed.row.action += "x";
        reject(changed);
        changed = actual;
        changed.row.user = "changed";
        reject(changed);
        changed = actual;
        changed.row.session = "changed";
        reject(changed);
        changed = actual;
        changed.row.ip = "192.0.2.1";
        reject(changed);
        changed = actual;
        changed.row.node += "x";
        reject(changed);
        changed = actual;
        changed.row.encoding = 1 - changed.row.encoding;
        reject(changed);
        changed = actual;
        changed.row.wire_shape = false;
        reject(changed);
        changed = actual;
        changed.row.detail["extension_id"] = "peer";
        reject(changed);
        changed = actual;
        changed.row.detail["call_depth"] = 99;
        reject(changed);
        changed = actual;
        changed.row.detail["unexpected"] = false;
        reject(changed);
        changed = actual;
        changed.row.timestamp = before - 1;
        reject(changed);
        changed = actual;
        changed.cleanup.retired = false;
        reject(changed);
        changed = actual;
        changed.cleanup.native_active = 1;
        reject(changed);
        changed = actual;
        changed.shutdown.idempotent = false;
        reject(changed);
        const auto reject_payload = [&](const Json::Value& payload) {
          auto corrupted = actual;
          corrupted.row.detail = payload;
          REQUIRE_FALSE(matches(corrupted));
          ++payload_mutations;
        };
        if (family == 1) {
          auto payload = actual.row.detail;
          payload["nested"]["user_id"] = "wrong-application-detail";
          reject_payload(payload);
          payload = actual.row.detail;
          payload["items"][2] = -16;
          reject_payload(payload);
        } else if (family >= 2) {
          auto payload = actual.row.detail;
          payload["text"] = family == 2 ? "\xc3\xaa\xce\xa9\xf0\x9f\x98\x80"
                                        : std::string{"a\0c", 3};
          reject_payload(payload);
          payload = actual.row.detail;
          payload["literal"] = R"(\u0000x)";
          reject_payload(payload);
          payload = actual.row.detail;
          const auto original_key =
              family == 2 ? std::string{"\xe4\xb8\xad\xf0\x9f\x98\x80"}
                          : std::string{"k\0end", 5};
          const auto moved = payload[original_key];
          payload.removeMember(original_key);
          payload[family == 2 ? "other" : std::string{"j\0end", 5}] = moved;
          reject_payload(payload);
        }
        if (family >= 2) {
          for (const auto* field : {"textUnits", "keyUnits"}) {
            changed = actual;
            changed.execution.javascript[field][0] =
                changed.execution.javascript[field][0].asInt() + 1;
            REQUIRE_FALSE(matches(changed));
            ++unit_mutations;
          }
        }
        ledger.push_back(std::move(actual));
      }
    }
  }
  REQUIRE(mutations == 25 * WRITES);
  REQUIRE(unit_mutations == 24);
  REQUIRE(payload_mutations == 48);
  REQUIRE(ledger_matches(ledger));
  unsigned ledger_controls = 0;
  auto changed = ledger;
  changed.pop_back();
  REQUIRE_FALSE(ledger_matches(changed));
  ++ledger_controls;
  changed = ledger;
  changed.push_back(ledger.front());
  REQUIRE_FALSE(ledger_matches(changed));
  ++ledger_controls;
  changed = ledger;
  changed.back() = changed.front();
  REQUIRE_FALSE(ledger_matches(changed));
  ++ledger_controls;
  changed = ledger;
  changed.back().row.id = changed.front().row.id;
  REQUIRE_FALSE(ledger_matches(changed));
  ++ledger_controls;
  changed = ledger;
  changed.back().action = changed.front().action;
  changed.back().row.action = changed.back().action;
  REQUIRE_FALSE(ledger_matches(changed));
  ++ledger_controls;
  REQUIRE(ledger_controls == 5);

  unsigned denials = 0;
  for (std::size_t item = 0; item < 6; ++item) {
    for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
      INFO("denial=" << item << " replay=" << replay);
      const auto cell =
          "d" + std::to_string(item) + "r" + std::to_string(replay);
      std::string action;
      if (item == 3 || item == 4) {
        action = events.own_action((item == 3 ? "user." : "ext.peer.") +
                                   events.suffix + cell);
      } else
        action = events.own("auditnative", cell);
      const auto want = denial(item, action);
      Owner owner{config, profile(0)};
      const auto actual = owner.drive(denial_source(want));
      const bool closed = owner.close();
      // Non-string/empty events have no event identity. Their action is only
      // an owned cleanup sentinel; exact TypeError and zero admission below
      // are the real rejection evidence, not a fictitious event-row receipt.
      const auto count = events.pg.count(action);
      Json::Value javascript{Json::objectValue};
      javascript["synchronous"] = want.synchronous;
      javascript["code"] = want.code;
      javascript["message"] = want.message;
      javascript["settlements"] = 0;
      REQUIRE(closed);
      REQUIRE(actual.completed);
      REQUIRE(actual.admitted == 0);
      REQUIRE_FALSE(actual.authority_mutated);
      REQUIRE(equal(actual.javascript, javascript));
      REQUIRE(count == 0);
      REQUIRE(clean(owner.cleanup));
      REQUIRE(owner.shutdown == Shutdown{true, true, 0, 0});
      ++denials;
    }
  }
  REQUIRE(denials == 12);
  REQUIRE(events.finish());
  REQUIRE(events.finish());
}

TEST_CASE("Native audit acknowledgement precedes blocked persistence",
          "[js][audit][integration][native-audit-blocked]") {
  const auto config = initialize();
  Events events{config.db};
  const auto action = events.own("auditnative", "blocked");
  const auto before = events.pg.now();
  BlockedRun blocked{config, source(2, action)};
  const int blocker = blocked.blocker();
  const bool acknowledged = blocked.acknowledged();
  int waiter = 0;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    waiter = events.pg.blocked_insert(blocker);
    if (waiter != 0) break;
  } while (std::chrono::steady_clock::now() < deadline);
  const auto prior = events.pg.count(action);
  const bool unsettled = !plinth::log::wait_for_audits(0ms);
  // No REQUIRE before unlock/join/drain; exceptions also use unlock-first RAII.
  const bool closed = blocked.finish();
  const auto execution = blocked.execution();
  const auto row = events.pg.read(action);
  const auto after = events.pg.now();
  const Observation actual{0,
                           2,
                           0,
                           action,
                           execution,
                           row,
                           before,
                           after,
                           blocked.owner->cleanup,
                           blocked.owner->shutdown};
  REQUIRE(acknowledged);
  REQUIRE(waiter > 0);
  REQUIRE(waiter != blocker);
  REQUIRE(prior == 0);
  REQUIRE(unsettled);
  REQUIRE(closed);
  REQUIRE(matches(actual));
  REQUIRE(events.finish());
  REQUIRE(events.finish());
}
