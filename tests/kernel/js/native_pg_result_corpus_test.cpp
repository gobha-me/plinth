// SPDX-License-Identifier: MIT

#include "async_bridge_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <json/json.h>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using plinth::js::BridgeContext;
using plinth::js::RuntimePool;
constexpr std::size_t FAMILIES = 21;
constexpr std::size_t SEEDS = 4;
constexpr std::size_t EPOCHS = 2;

auto demand(bool condition, const char* message) -> void {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// Numeric JsonCpp storage tags are not the JS number/string contract. The
// independently observed JS kind is compared separately and exactly.
auto equal(const Json::Value& actual, const Json::Value& expected) -> bool {
  if (actual.isNumeric() && expected.isNumeric()) {
    return actual.asDouble() == expected.asDouble();
  }
  if (actual.type() != expected.type()) {
    return false;
  }
  if (expected.isObject()) {
    if (actual.getMemberNames() != expected.getMemberNames()) {
      return false;
    }
    for (const auto& key : expected.getMemberNames()) {
      if (!equal(actual[key], expected[key])) {
        return false;
      }
    }
    return true;
  }
  if (expected.isArray()) {
    if (actual.size() != expected.size()) {
      return false;
    }
    for (Json::ArrayIndex index = 0; index < expected.size(); ++index) {
      if (!equal(actual[index], expected[index])) {
        return false;
      }
    }
    return true;
  }
  return actual == expected;
}

auto list(std::initializer_list<int> values) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const int value : values) {
    result.append(value);
  }
  return result;
}

auto keys(std::initializer_list<const char*> values) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const auto* value : values) {
    result.append(value);
  }
  return result;
}

auto scalar(const Json::Value& value) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["kind"] = value.isNull()     ? "object"
                   : value.isBool()   ? "boolean"
                   : value.isString() ? "string"
                                      : "number";
  result["nil"] = value.isNull();
  result["u8"] = false;
  result["value"] = value;
  result["units"] = Json::Value{};
  result["bytes"] = Json::Value{};
  result["length"] = Json::Value{};
  result["byteLength"] = Json::Value{};
  if (value.isString()) {
    const auto text = value.asString();
    auto units = Json::Value{Json::arrayValue};
    if (text == "\xc3\xa9\xce\xa9") {
      units = list({233, 937}); // Independent UTF-16 oracle, not UTF-8 bytes.
    } else {
      for (const unsigned char unit : text) {
        units.append(static_cast<int>(unit));
      }
    }
    result["length"] = units.size();
    result["units"] = units;
  }
  return result;
}

auto bytes(const Json::Value& value) -> Json::Value {
  auto result = scalar(Json::Value{});
  result["nil"] = false;
  result["u8"] = true;
  result["bytes"] = value;
  result["length"] = value.size();
  result["byteLength"] = value.size();
  return result;
}

auto snapshot(const std::vector<Json::Value>& values,
              const char* column = "value") -> Json::Value {
  Json::Value result{Json::objectValue};
  result["keys"] = keys({"row_count", "rows"});
  result["count"] = static_cast<unsigned>(values.size());
  result["rows"] = Json::Value{Json::arrayValue};
  for (const auto& value : values) {
    Json::Value row{Json::objectValue};
    row["keys"] = keys({column});
    row["own"] = true;
    row["scalar"] = value;
    result["rows"].append(row);
  }
  return result;
}

class StoredRows {
 public:
  explicit StoredRows(const plinth::Config::Database& config) {
    const auto info = plinth::db::connection_info(config) +
                      " connect_timeout=3 options='-c statement_timeout=3000 "
                      "-c lock_timeout=2500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand(connection && PQstatus(connection.get()) == CONNECTION_OK,
           "native PG corpus control connection unavailable");
    static std::atomic<unsigned> sequence{0};
    const auto suffix =
        std::to_string(getpid()) + "_" + std::to_string(sequence.fetch_add(1));
    table = "plinth.pg_corpus_" + suffix;
    const auto schema = query("SELECT to_regnamespace('plinth') IS NOT NULL");
    demand(PQntuples(schema.get()) == 1 &&
               std::string_view{PQgetvalue(schema.get(), 0, 0)} == "t",
           "native PG corpus namespace missing");
    absent(table);
    for (std::size_t seed = 0; seed < SEEDS; ++seed) {
      missing[seed] =
          "plinth.pg_corpus_" + suffix + "_missing_s" + std::to_string(seed);
      demand(missing[seed].size() < 63, "native PG identifier exceeds bound");
      absent(missing[seed]);
    }
    try {
      owned = true; // Lost acknowledgement still belongs to this fixture only.
      command("CREATE TABLE " + table + " (id integer PRIMARY KEY)");
    } catch (...) {
      if (!finish()) {
        std::terminate();
      }
      throw;
    }
  }
  ~StoredRows() {
    if (!finish()) {
      std::terminate();
    }
  }
  StoredRows(const StoredRows&) = delete;
  auto operator=(const StoredRows&) -> StoredRows& = delete;

  auto preseed(std::size_t seed) -> bool {
    command("TRUNCATE TABLE " + table);
    command("INSERT INTO " + table + " SELECT generate_series(1," +
            std::to_string(seed) + ")");
    return count() == static_cast<int>(seed);
  }
  auto count() -> int {
    const auto result = query("SELECT count(*) FROM " + table);
    demand(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1,
           "native PG independent readback shape");
    return std::stoi(PQgetvalue(result.get(), 0, 0));
  }
  auto finish() noexcept -> bool {
    if (!owned) {
      return true;
    }
    if (!plinth::lifecycle::async_tasks().drain(5s) ||
        !plinth::log::wait_for_audits(5s)) {
      return false;
    }
    try {
      command("DROP TABLE IF EXISTS " + table);
      absent(table);
      owned = false;
      return true;
    } catch (...) {
      return false;
    }
  }

  std::string table;
  std::array<std::string, SEEDS> missing;
  unsigned controls = 0;

 private:
  using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
  auto query(const std::string& sql) -> Result {
    ++controls;
    Result result{PQexec(connection.get(), sql.c_str()), PQclear};
    demand(result && PQresultStatus(result.get()) == PGRES_TUPLES_OK,
           "native PG fixture query failed");
    return result;
  }
  auto command(const std::string& sql) -> void {
    ++controls;
    Result result{PQexec(connection.get(), sql.c_str()), PQclear};
    demand(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK,
           "native PG fixture command failed");
  }
  auto absent(const std::string& name) -> void {
    const auto result = query("SELECT to_regclass('" + name + "')");
    demand(PQntuples(result.get()) == 1 && PQgetisnull(result.get(), 0, 0),
           "refuse pre-existing native PG fixture relation");
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
  bool owned = false;
};

struct Cleanup {
  bool retired;
  std::size_t pending;
  std::size_t retired_pending;
  std::size_t callbacks;
  std::size_t persistent;
  int concurrent;
  int detached;
  int depth;
  std::uint64_t scope;
  bool pinned;
  bool rollback;
  bool timer;
  bool timer_loop;
  bool drained;
  bool audits;
  std::size_t native_active;
  int pool_active;
  auto operator==(const Cleanup&) const -> bool = default;
};

auto clean(const Cleanup& actual) -> bool {
  const Cleanup expected{true,  0,     0,     0,     0,    0,    0, 0, 0,
                         false, false, false, false, true, true, 0, 0};
  return actual == expected;
}

struct Execution {
  plinth::js::EvalResult result;
  Cleanup cleanup;
};

auto drive(RuntimePool& pool, const std::string& source) -> Execution {
  demand(source.size() <= 8192, "native PG source bound exceeded");
  const auto destroy = [&pool](BridgeContext* context) {
    pool.destroy(context);
  };
  std::unique_ptr<BridgeContext, decltype(destroy)> context{pool.acquire(),
                                                            destroy};
  demand(context != nullptr, "native PG corpus context unavailable");
  auto result = drogon::sync_wait(plinth::js::run_on_context(*context, source));
  const auto& batch = context->batch_state;
  Cleanup state{!context->execution_lifetime.load(std::memory_order_acquire),
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
                plinth::lifecycle::async_tasks().drain(5s),
                plinth::log::wait_for_audits(5s),
                plinth::lifecycle::async_tasks().active_for_test(),
                0};
  context.reset(); // No access after destroy; refusal is observed, not hidden.
  state.pool_active = pool.active_count();
  return {std::move(result), state};
}

struct Program {
  std::string sql;
  std::string parameters = "[]";
};

// SQL/JS inputs are deliberately separate from the literal result oracles.
auto program(std::size_t family, std::size_t seed, const StoredRows& stored)
    -> Program {
  constexpr std::array<std::string_view, 4> narrow{"-32768", "-1", "0",
                                                   "32767"};
  constexpr std::array<std::string_view, 4> integer{"-2147483648", "-1", "0",
                                                    "2147483647"};
  constexpr std::array<std::string_view, 4> safe{
      "-9007199254740991", "-2147483649", "2147483648", "9007199254740991"};
  constexpr std::array<std::string_view, 4> outer{
      "-9223372036854775808", "-9007199254740992", "9007199254740992",
      "9223372036854775807"};
  constexpr std::array<std::string_view, 4> real{"-1.5", "-0.5", "0.5", "1.5"};
  constexpr std::array<std::string_view, 4> doubles{"-1048576.25", "-0.125",
                                                    "0.125", "1048576.25"};
  constexpr std::array<std::string_view, 4> numeric{"-123.5", "-0.25", "0.25",
                                                    "123.5"};
  constexpr std::array<std::string_view, 4> text{"''", "'t'", "'true'",
                                                 "'\xc3\xa9\xce\xa9'"};
  constexpr std::array<std::string_view, 4> varchar{"'false'", "'007'", "'f'",
                                                    "''"};
  constexpr std::array<std::string_view, 4> names{"'alpha'", "'t'", "'007'",
                                                  "'omega'"};
  constexpr std::array<std::string_view, 4> chars{"'A'", "'t'", "'f'", "'7'"};
  constexpr std::array<std::string_view, 4> uuid{
      "'00000000-0000-0000-0000-000000000000'",
      "'11111111-1111-1111-1111-111111111111'",
      "'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'",
      "'ffffffff-ffff-ffff-ffff-ffffffffffff'"};
  const auto parameter = [](std::string_view sql, std::string_view value) {
    return Program{std::string{sql}, "[" + std::string{value} + "]"};
  };
  switch (family) {
    case 0: {
      constexpr std::array<std::string_view, 4> sql{
          "SELECT $1::boolean AS value", "SELECT $1::bigint AS value",
          "SELECT $1::text AS value", "SELECT $1::bytea AS value"};
      return parameter(sql[seed], "null");
    }
    case 1:
      return parameter("SELECT $1::boolean AS value",
                       seed % 2 == 0 ? "true" : "false");
    case 2:
      return parameter("SELECT ($1::bigint)::smallint AS value", narrow[seed]);
    case 3:
      return parameter("SELECT ($1::bigint)::integer AS value", integer[seed]);
    case 4: return parameter("SELECT $1::bigint AS value", safe[seed]);
    case 5:
      return {"SELECT (" + std::string{outer[seed]} + ")::bigint AS value"};
    case 6:
      return parameter("SELECT ($1::double precision)::real AS value",
                       real[seed]);
    case 7:
      return parameter("SELECT $1::double precision AS value", doubles[seed]);
    case 8:
      return {"SELECT " + std::string{numeric[seed]} + "::numeric AS value"};
    case 9: return parameter("SELECT $1::text AS value", text[seed]);
    case 10:
      return parameter("SELECT ($1::text)::varchar AS value", varchar[seed]);
    case 11: return parameter("SELECT ($1::text)::name AS value", names[seed]);
    case 12:
      return parameter("SELECT ($1::text)::\"char\" AS value", chars[seed]);
    case 13: return parameter("SELECT ($1::text)::uuid AS value", uuid[seed]);
    case 14: {
      constexpr std::array<std::string_view, 4> hex{"", "00", "0080ff",
                                                    "5c00ff80"};
      return {"SELECT decode('" + std::string{hex[seed]} + "','hex') AS value"};
    }
    case 15: {
      constexpr std::array<std::string_view, 4> sql{
          "SELECT 0 AS value WHERE false", "SELECT 21 AS value",
          "SELECT value FROM (VALUES(31),(32)) AS r(value) ORDER BY value",
          "SELECT value FROM (VALUES(41),(42),(43)) AS r(value) ORDER BY "
          "value"};
      return {std::string{sql[seed]}};
    }
    case 16: {
      constexpr std::array<std::string_view, 4> sql{
          "SELEC 1", "SELECT (1", "SELECT 1 +", "SELECT 1 WHERE"};
      return {std::string{sql[seed]}};
    }
    case 17: return {"SELECT 1 FROM " + stored.missing[seed]};
    case 18:
      return {"SELECT missing_s" + std::to_string(seed) +
              " FROM (VALUES (1)) AS known(present)"};
    case 19: {
      constexpr std::array<std::string_view, 4> sql{
          "SELECT 1 / 0 AS value", "SELECT -7 / (2-2) AS value",
          "SELECT 0 / (9-9) AS value", "SELECT (6*7) / (3-3) AS value"};
      return {std::string{sql[seed]}};
    }
    default: return {"DELETE FROM " + stored.table};
  }
}

constexpr std::string_view OBSERVER = R"JS(
const own=(o,k)=>Object.prototype.hasOwnProperty.call(o,k);
function scalar(v) {
  const u=v instanceof Uint8Array;
  return {kind:typeof v,nil:v===null,u8:u,value:u?null:v,
    units:typeof v==='string'?Array.from({length:v.length},(_,i)=>v.charCodeAt(i)):null,
    bytes:u?Array.from(v):null,length:u||typeof v==='string'?v.length:null,
    byteLength:u?v.byteLength:null};
}
function snapshot(r,column='value') {
  return {keys:Object.keys(r).sort(),count:r.row_count,
    rows:r.rows.map(row=>({keys:Object.keys(row).sort(),own:own(row,column),scalar:scalar(row[column])}))};
}
let queries=0,execs=0;const operations=[];
async function query(sql,params=[],role='primary') {
  try { const value=await db.query(sql,params);++queries;
    operations.push({role,route:'query',settled:'fulfilled'});return value;
  } catch(e) { ++queries;operations.push({role,route:'query',settled:'rejected'});throw e; }
}
(async()=>{
const input=params.length?scalar(params[0]):{kind:'none'};
let result=null,second=null,retained=null,changed=null,independent=null;
let error=null,recovery=null,affected=null,empty=null;
let secondChanged=null,retainedAfterSecond=null;
)JS";

auto source(std::size_t family, const Program& spec, const StoredRows& stored)
    -> std::string {
  demand(spec.sql.size() <= 2048 && spec.parameters.size() <= 512,
         "native PG program input bound");
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  auto result = "const sql=" + Json::writeString(writer, spec.sql) +
                ";const params=" + spec.parameters + ";" +
                std::string{OBSERVER};
  if (family < 14 || family == 15) {
    result += "result=snapshot(await query(sql,params));";
  } else if (family == 14) {
    result += R"JS(
const first=await query(sql),held=first.rows[0].value;
result=snapshot(first);const other=await query("SELECT decode('110080ff','hex') AS value",[],'byte-followup');
const next=other.rows[0].value;second=snapshot(other);retained=snapshot(first);
independent=held!==next&&held.buffer!==next.buffer;
if(held.length)held[0]^=1;changed=snapshot(first);
next[0]^=1;secondChanged=snapshot(other);retainedAfterSecond=snapshot(first);
)JS";
  } else if (family < 20) {
    result += R"JS(
try { await query(sql); } catch(e) {
error={code:e.code,state:e.sqlstate,messagePresent:typeof e.message==='string'&&e.message.length>0,
 codeType:typeof e.code,stateType:typeof e.sqlstate,messageType:typeof e.message,
 ownCode:own(e,'code'),ownState:own(e,'sqlstate'),ownMessage:own(e,'message')};
}
recovery=snapshot(await query('SELECT 42 AS answer',[],'error-recovery'),'answer');
)JS";
  } else {
    result += "const ack=await "
              "db.exec(sql,[],{silent:true});++execs;operations.push({role:'"
              "primary',route:'exec',settled:'fulfilled'});";
    result += "affected={keys:Object.keys(ack).sort(),count:ack.row_count};";
    result += "empty=snapshot(await query('SELECT count(*)::integer AS "
              "remaining FROM " +
              stored.table + "',[],'exec-readback'),'remaining');";
  }
  result += R"JS(
return {input,result,second,retained,changed,independent,error,recovery,affected,empty,queries,execs,operations,secondChanged,retainedAfterSecond};
})()
)JS";
  return result;
}

auto expected_scalar(std::size_t family, std::size_t seed) -> Json::Value {
  // This table never reads Program, server result, driver field or converter.
  constexpr std::array<double, 4> small{-32768, -1, 0, 32767};
  constexpr std::array<double, 4> integer{-2147483648.0, -1, 0, 2147483647.0};
  constexpr std::array<double, 4> safe{-9007199254740991.0, -2147483649.0,
                                       2147483648.0, 9007199254740991.0};
  constexpr std::array<const char*, 4> outer{
      "-9223372036854775808", "-9007199254740992", "9007199254740992",
      "9223372036854775807"};
  constexpr std::array<double, 4> real{-1.5, -0.5, 0.5, 1.5};
  constexpr std::array<double, 4> doubles{-1048576.25, -0.125, 0.125,
                                          1048576.25};
  constexpr std::array<double, 4> numeric{-123.5, -0.25, 0.25, 123.5};
  constexpr std::array<const char*, 4> text{"", "t", "true",
                                            "\xc3\xa9\xce\xa9"};
  constexpr std::array<const char*, 4> varchar{"false", "007", "f", ""};
  constexpr std::array<const char*, 4> names{"alpha", "t", "007", "omega"};
  constexpr std::array<const char*, 4> chars{"A", "t", "f", "7"};
  constexpr std::array<const char*, 4> uuid{
      "00000000-0000-0000-0000-000000000000",
      "11111111-1111-1111-1111-111111111111",
      "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
      "ffffffff-ffff-ffff-ffff-ffffffffffff"};
  switch (family) {
    case 0: return scalar(Json::Value{});
    case 1: return scalar(seed % 2 == 0);
    case 2: return scalar(small[seed]);
    case 3: return scalar(integer[seed]);
    case 4: return scalar(safe[seed]);
    case 5: return scalar(outer[seed]);
    case 6: return scalar(real[seed]);
    case 7: return scalar(doubles[seed]);
    case 8: return scalar(numeric[seed]);
    case 9: return scalar(text[seed]);
    case 10: return scalar(varchar[seed]);
    case 11: return scalar(names[seed]);
    case 12: return scalar(chars[seed]);
    case 13: return scalar(uuid[seed]);
    default: {
      const std::array<Json::Value, 4> payload{
          list({}), list({0}), list({0, 128, 255}), list({92, 0, 255, 128})};
      return bytes(payload[seed]);
    }
  }
}

auto expected(std::size_t family, std::size_t seed) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["input"]["kind"] = "none";
  for (const auto* key : {"result", "second", "retained", "changed",
                          "independent", "error", "recovery", "affected",
                          "empty", "secondChanged", "retainedAfterSecond"}) {
    result[key] = Json::Value{};
  }
  result["queries"] = family == 14 || (family >= 16 && family < 20) ? 2 : 1;
  result["execs"] = family == 20 ? 1 : 0;
  result["operations"] = Json::Value{Json::arrayValue};
  Json::Value primary{Json::objectValue};
  primary["role"] = "primary";
  primary["route"] = family == 20 ? "exec" : "query";
  primary["settled"] = family >= 16 && family < 20 ? "rejected" : "fulfilled";
  result["operations"].append(primary);
  if (family == 14 || family >= 16) {
    Json::Value follow{Json::objectValue};
    follow["role"] = family == 14   ? "byte-followup"
                     : family == 20 ? "exec-readback"
                                    : "error-recovery";
    follow["route"] = "query";
    follow["settled"] = "fulfilled";
    result["operations"].append(follow);
  }
  if (family <= 13 && family != 5 && family != 8) {
    result["input"] = expected_scalar(family, seed);
  }
  if (family < 15) {
    result["result"] = snapshot({expected_scalar(family, seed)});
    if (family == 14) {
      result["second"] = snapshot({bytes(list({17, 0, 128, 255}))});
      result["retained"] = result["result"];
      auto mutated = expected_scalar(14, seed);
      if (!mutated["bytes"].empty()) {
        mutated["bytes"][0] = mutated["bytes"][0].asInt() ^ 1;
      }
      result["changed"] = snapshot({mutated});
      result["secondChanged"] = snapshot({bytes(list({16, 0, 128, 255}))});
      result["retainedAfterSecond"] = result["changed"];
      result["independent"] = true;
    }
  } else if (family == 15) {
    const std::array<std::vector<Json::Value>, 4> rows{
        {{},
         {scalar(21)},
         {scalar(31), scalar(32)},
         {scalar(41), scalar(42), scalar(43)}}};
    result["result"] = snapshot(rows[seed]);
  } else if (family < 20) {
    constexpr std::array<const char*, 4> codes{
        "db.syntax_error", "db.undefined_table", "db.undefined_column",
        "db.internal"};
    constexpr std::array<const char*, 4> states{"42601", "42P01", "42703",
                                                "22012"};
    auto& error = result["error"];
    error["code"] = codes[family - 16];
    error["state"] = states[family - 16];
    error["messagePresent"] = true;
    for (const auto* key : {"codeType", "stateType", "messageType"}) {
      error[key] = "string";
    }
    for (const auto* key : {"ownCode", "ownState", "ownMessage"}) {
      error[key] = true;
    }
    result["recovery"] = snapshot({scalar(42)}, "answer");
  } else {
    result["affected"]["keys"] = keys({"row_count"});
    result["affected"]["count"] = static_cast<unsigned>(seed);
    result["empty"] = snapshot({scalar(0)}, "remaining");
  }
  return result;
}

using Ledger =
    std::array<std::array<std::array<unsigned, EPOCHS>, SEEDS>, FAMILIES>;
auto complete(const Ledger& ledger) -> bool {
  for (const auto& family : ledger) {
    for (const auto& seed : family) {
      for (const unsigned count : seed) {
        if (count != 1) {
          return false;
        }
      }
    }
  }
  return true;
}

auto corrupt_cleanup(Cleanup& state, unsigned field) -> void {
  switch (field) {
    case 0: state.retired = false; break;
    case 1: ++state.pending; break;
    case 2: ++state.retired_pending; break;
    case 3: ++state.callbacks; break;
    case 4: ++state.persistent; break;
    case 5: ++state.concurrent; break;
    case 6: ++state.detached; break;
    case 7: ++state.depth; break;
    case 8: ++state.scope; break;
    case 9: state.pinned = true; break;
    case 10: state.rollback = true; break;
    case 11: state.timer = true; break;
    case 12: state.timer_loop = true; break;
    case 13: state.drained = false; break;
    case 14: state.audits = false; break;
    case 15: ++state.native_active; break;
    default: ++state.pool_active; break;
  }
}

struct Cell {
  std::size_t family;
  std::size_t seed;
  std::size_t epoch;
  Execution execution;
  bool durable;
};
struct Shutdown {
  bool stopped;
  bool idempotent;
  int active;
  int free;
};
auto stopped(const Shutdown& state) -> bool {
  return state.stopped && state.idempotent && state.active == 0 &&
         state.free == 0;
}

class Owner {
 public:
  Owner()
      : config(plinth::async_bridge_test::test_config()),
        pool(nullptr, limits(), config, 1) {}
  ~Owner() noexcept {
    // RuntimePool's own destructor is unbounded. Never enter it with a known
    // outstanding owner after an exception or a failed bounded close.
    if (!closed) {
      try {
        if (!pool.shutdown(5s)) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;
  auto finish() -> Shutdown {
    const bool first = pool.shutdown(5s);
    const bool second = pool.shutdown(0ms);
    Shutdown result{first, second, pool.active_count(), pool.free_count()};
    closed = stopped(result);
    return result;
  }
  plinth::Config config;
  RuntimePool pool;

 private:
  static auto limits() -> plinth::js::RuntimeLimits {
    auto result = plinth::js::default_runtime_limits();
    result.wall_clock_limit = 3s;
    return result;
  }
  bool closed = false;
};

struct Identity {
  std::size_t family;
  std::size_t seed;
  std::size_t epoch;
};
auto valid(const Identity& id) -> bool {
  return id.family < FAMILIES && id.seed < SEEDS && id.epoch < EPOCHS;
}
auto identities_complete(const std::vector<Identity>& records) -> bool {
  Ledger counts{};
  for (const auto& id : records) {
    if (!valid(id)) return false;
    ++counts[id.family][id.seed][id.epoch];
  }
  return complete(counts);
}
struct Operation {
  Identity id;
  std::string role;
  std::string route;
  std::string settled;
};
auto operations_complete(const std::vector<Operation>& records) -> bool {
  std::array<Ledger, 4> counts{};
  for (const auto& record : records) {
    if (!valid(record.id)) return false;
    std::size_t role = 4;
    if (record.role == "primary") role = 0;
    if (record.role == "byte-followup" && record.id.family == 14) role = 1;
    if (record.role == "error-recovery" && record.id.family >= 16 &&
        record.id.family < 20)
      role = 2;
    if (record.role == "exec-readback" && record.id.family == 20) role = 3;
    if (role == 4) return false;
    const bool exec = record.id.family == 20 && role == 0;
    const bool rejected =
        record.id.family >= 16 && record.id.family < 20 && role == 0;
    if (record.route != (exec ? "exec" : "query") ||
        record.settled != (rejected ? "rejected" : "fulfilled"))
      return false;
    ++counts[role][record.id.family][record.id.seed][record.id.epoch];
  }
  for (std::size_t role = 0; role < 4; ++role) {
    for (std::size_t family = 0; family < FAMILIES; ++family) {
      const bool needed = role == 0 || (role == 1 && family == 14) ||
                          (role == 2 && family >= 16 && family < 20) ||
                          (role == 3 && family == 20);
      for (const auto& seed : counts[role][family]) {
        for (const unsigned count : seed) {
          if (count != (needed ? 1U : 0U)) return false;
        }
      }
    }
  }
  return true;
}
struct OwnerRecord {
  std::size_t seed;
  std::size_t epoch;
  Shutdown state;
};
auto owners_complete(const std::vector<OwnerRecord>& records) -> bool {
  std::array<std::array<unsigned, EPOCHS>, SEEDS> counts{};
  for (const auto& record : records) {
    if (record.seed >= SEEDS || record.epoch >= EPOCHS ||
        !stopped(record.state))
      return false;
    ++counts[record.seed][record.epoch];
  }
  for (const auto& seed : counts) {
    for (const unsigned count : seed)
      if (count != 1) return false;
  }
  return true;
}

// Corrupt only retained actual observations after EVERY clean baseline passed.
auto corrupt_scalar(Json::Value& value, unsigned mutation) -> void {
  switch (mutation) {
    case 0: value["scalar"]["kind"] = "undefined"; break;
    case 1: value["scalar"]["value"] = "corrupted"; break;
    case 2: value["scalar"]["nil"] = !value["scalar"]["nil"].asBool(); break;
    case 3: value["keys"][0] = "renamed"; break;
    default: value["own"] = false; break;
  }
}

auto flip_byte(Json::Value& values, bool last = false) -> void {
  if (values.empty()) {
    values.append(1);
  } else {
    const Json::ArrayIndex index = last ? values.size() - 1 : 0;
    values[index] = values[index].asInt() ^ 1;
  }
}

} // namespace

TEST_CASE("QuickJS native PostgreSQL calls have a bounded result corpus",
          "[js][async][db][corpus][native-pg-results]") {
  const auto deadline = Clock::now() + 55s;
  REQUIRE(plinth::async_bridge_test::pg_available());
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  const auto setup = plinth::async_bridge_test::test_config();
  plinth::async_bridge_test::reset_schema(setup.db);
  StoredRows stored{setup.db};
  Ledger ledger{};
  std::vector<Cell> observations;
  std::array<Shutdown, SEEDS * EPOCHS> shutdowns{};
  observations.reserve(168);
  for (std::size_t seed = 0; seed < SEEDS; ++seed) {
    for (std::size_t epoch = 0; epoch < EPOCHS; ++epoch) {
      // Config is separately stack-owned for each fresh owner and outlives
      // pool.
      Owner owner;
      auto& pool = owner.pool;
      for (std::size_t family = 0; family < FAMILIES; ++family) {
        demand(Clock::now() < deadline, "native PG corpus collection deadline");
        bool durable = true;
        if (family == 20) {
          durable = stored.preseed(seed);
        }
        auto execution =
            drive(pool, source(family, program(family, seed, stored), stored));
        if (family == 20) {
          durable = stored.count() == 0 && durable;
        }
        observations.push_back(
            {family, seed, epoch, std::move(execution), durable});
        ++ledger[family][seed][epoch];
      }
      shutdowns[seed * EPOCHS + epoch] = owner.finish();
    }
  }
  const bool removed = stored.finish();
  // Production-equivalent owners and fixture data retire before any oracle can
  // abort this test. Failed bounded shutdown remains a failure, never a leak.
  REQUIRE(removed);
  REQUIRE(stored.controls == 41);
  REQUIRE(observations.size() == 168);
  REQUIRE(complete(ledger));
  for (const auto& state : shutdowns) {
    REQUIRE(stopped(state));
  }
  unsigned queries = 0;
  unsigned execs = 0;
  std::vector<Identity> identities;
  std::vector<Operation> operations;
  std::vector<OwnerRecord> owners;
  for (const auto& cell : observations) {
    INFO("native PG family=" << cell.family << " seed=" << cell.seed
                             << " epoch=" << cell.epoch);
    REQUIRE(clean(cell.execution.cleanup));
    REQUIRE(cell.durable);
    REQUIRE(cell.execution.result.value.has_value());
    const auto& actual = *cell.execution.result.value;
    INFO("native PG actual=" << actual.toStyledString());
    REQUIRE(equal(actual, expected(cell.family, cell.seed)));
    queries += actual["queries"].asUInt();
    execs += actual["execs"].asUInt();
    identities.push_back({cell.family, cell.seed, cell.epoch});
    for (const auto& record : actual["operations"]) {
      operations.push_back({{cell.family, cell.seed, cell.epoch},
                            record["role"].asString(),
                            record["route"].asString(),
                            record["settled"].asString()});
    }
  }
  for (std::size_t seed = 0; seed < SEEDS; ++seed) {
    for (std::size_t epoch = 0; epoch < EPOCHS; ++epoch)
      owners.push_back({seed, epoch, shutdowns[seed * EPOCHS + epoch]});
  }
  REQUIRE(queries == 208);
  REQUIRE(execs == 8);
  REQUIRE(identities_complete(identities));
  REQUIRE(operations_complete(operations));
  REQUIRE(operations.size() == 216);
  REQUIRE(owners_complete(owners));

  unsigned common_controls = 0, scalar_controls = 0, string_controls = 0;
  unsigned numeric_controls = 0, byte_controls = 0, envelope_controls = 0;
  unsigned error_controls = 0, affected_controls = 0, cleanup_controls = 0;
  for (const auto& cell : observations) {
    const auto& actual = *cell.execution.result.value;
    const auto oracle = expected(cell.family, cell.seed);
    const auto reject = [&](const Json::Value& corrupt) {
      REQUIRE_FALSE(equal(corrupt, oracle));
    };
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
      auto corrupt = actual;
      if (mutation == 0) corrupt["input"]["kind"] = "wrong";
      if (mutation == 1) corrupt["queries"] = actual["queries"].asUInt() + 1;
      if (mutation == 2) corrupt.removeMember("result");
      reject(corrupt);
      ++common_controls;
    }
    if (cell.family < 14) {
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto corrupt = actual;
        corrupt_scalar(corrupt["result"]["rows"][0], mutation);
        reject(corrupt);
        ++scalar_controls;
      }
      if (cell.family == 5 || cell.family >= 9) {
        for (unsigned mutation = 0; mutation < 2; ++mutation) {
          auto corrupt = actual;
          auto& value = corrupt["result"]["rows"][0]["scalar"];
          if (mutation == 0)
            value["units"].append(65535);
          else
            value["length"] = value["length"].asUInt() + 1;
          reject(corrupt);
          ++string_controls;
        }
      }
      if ((cell.family >= 2 && cell.family <= 4) ||
          (cell.family >= 6 && cell.family <= 8)) {
        auto corrupt = actual;
        auto& value = corrupt["result"]["rows"][0]["scalar"]["value"];
        value = value.asDouble() + 1;
        reject(corrupt);
        ++numeric_controls;
      }
    } else if (cell.family == 14) {
      for (unsigned mutation = 0; mutation < 11; ++mutation) {
        auto corrupt = actual;
        auto& value = corrupt["result"]["rows"][0]["scalar"];
        switch (mutation) {
          case 0: flip_byte(value["bytes"]); break;
          case 1: flip_byte(value["bytes"], true); break;
          case 2: value["length"] = value["length"].asUInt() + 1; break;
          case 3: value["byteLength"] = value["byteLength"].asUInt() + 1; break;
          case 4: value["u8"] = false; break;
          case 5:
            flip_byte(corrupt["retained"]["rows"][0]["scalar"]["bytes"]);
            break;
          case 6: corrupt["second"]["rows"][0]["scalar"]["bytes"][1] = 1; break;
          case 7:
            flip_byte(corrupt["changed"]["rows"][0]["scalar"]["bytes"]);
            break;
          case 8: corrupt["independent"] = false; break;
          case 9:
            corrupt["secondChanged"]["rows"][0]["scalar"]["bytes"][2] = 129;
            break;
          default:
            flip_byte(
                corrupt["retainedAfterSecond"]["rows"][0]["scalar"]["bytes"]);
            break;
        }
        reject(corrupt);
        ++byte_controls;
      }
    } else if (cell.family == 15) {
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto corrupt = actual;
        auto& value = corrupt["result"];
        if (mutation == 0) value["count"] = value["count"].asUInt() + 1;
        if (mutation == 1) {
          if (value["rows"].size() >= 2)
            std::swap(value["rows"][0], value["rows"][1]);
          else
            value["rows"].append(Json::Value{});
        }
        if (mutation == 2) {
          if (value["rows"].empty())
            value["rows"].append(Json::Value{});
          else
            value["rows"][0]["keys"][0] = "wrong";
        }
        if (mutation == 3) value["keys"][0] = "wrong";
        reject(corrupt);
        ++envelope_controls;
      }
    } else if (cell.family < 20) {
      constexpr std::array<const char*, 9> fields{
          "code",    "state",    "codeType",   "stateType",     "messageType",
          "ownCode", "ownState", "ownMessage", "messagePresent"};
      for (const auto* field : fields) {
        auto corrupt = actual;
        corrupt["error"][field] = corrupt["error"][field].isBool()
                                      ? Json::Value{false}
                                      : Json::Value{"wrong"};
        reject(corrupt);
        ++error_controls;
      }
    } else {
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto corrupt = actual;
        if (mutation == 0)
          corrupt["affected"]["count"] = static_cast<unsigned>(cell.seed + 1);
        if (mutation == 1) corrupt["affected"]["keys"].append("rows");
        if (mutation == 2) corrupt["empty"]["rows"][0]["scalar"]["value"] = 1;
        if (mutation == 3) corrupt["empty"]["count"] = 0;
        reject(corrupt);
        ++affected_controls;
      }
    }
    for (unsigned field = 0; field < 17; ++field) {
      auto corrupt = cell.execution.cleanup;
      corrupt_cleanup(corrupt, field);
      REQUIRE_FALSE(clean(corrupt));
      ++cleanup_controls;
    }
  }
  unsigned ledger_controls = 0;
  for (std::size_t index = 0; index < identities.size(); ++index) {
    auto dropped = identities;
    dropped.erase(dropped.begin() + static_cast<std::ptrdiff_t>(index));
    REQUIRE_FALSE(identities_complete(dropped));
    ++ledger_controls;
    auto duplicated = identities;
    duplicated.push_back(identities[index]);
    REQUIRE_FALSE(identities_complete(duplicated));
    ++ledger_controls;
  }
  for (std::size_t seed = 0; seed < SEEDS; ++seed) {
    for (std::size_t epoch = 0; epoch < EPOCHS; ++epoch) {
      auto corrupt = identities;
      std::erase_if(corrupt, [seed, epoch](const Identity& id) {
        return id.seed == seed && id.epoch == epoch;
      });
      REQUIRE_FALSE(identities_complete(corrupt));
      ++ledger_controls;
    }
  }
  for (std::size_t epoch = 0; epoch < EPOCHS; ++epoch) {
    auto corrupt = identities;
    std::erase_if(corrupt,
                  [epoch](const Identity& id) { return id.epoch == epoch; });
    REQUIRE_FALSE(identities_complete(corrupt));
    ++ledger_controls;
  }
  for (std::size_t index = 0; index < operations.size(); ++index) {
    auto dropped = operations;
    dropped.erase(dropped.begin() + static_cast<std::ptrdiff_t>(index));
    REQUIRE_FALSE(operations_complete(dropped));
    ++ledger_controls;
    auto duplicated = operations;
    duplicated.push_back(operations[index]);
    REQUIRE_FALSE(operations_complete(duplicated));
    ++ledger_controls;
  }
  for (std::size_t index = 0; index < owners.size(); ++index) {
    auto dropped = owners;
    dropped.erase(dropped.begin() + static_cast<std::ptrdiff_t>(index));
    REQUIRE_FALSE(owners_complete(dropped));
    ++ledger_controls;
    auto duplicated = owners;
    duplicated.push_back(owners[index]);
    REQUIRE_FALSE(owners_complete(duplicated));
    ++ledger_controls;
  }
  for (unsigned field = 0; field < 3; ++field) {
    auto corrupt = identities;
    if (field == 0) corrupt[0].family = FAMILIES;
    if (field == 1) corrupt[0].seed = SEEDS;
    if (field == 2) corrupt[0].epoch = EPOCHS;
    REQUIRE_FALSE(identities_complete(corrupt));
    ++ledger_controls;
  }
  for (unsigned field = 0; field < 2; ++field) {
    auto corrupt = operations;
    if (field == 0)
      corrupt[0].role = "unknown";
    else
      corrupt[0].route = "exec";
    REQUIRE_FALSE(operations_complete(corrupt));
    ++ledger_controls;
  }
  auto foreign_owner = owners;
  foreign_owner[0].seed = SEEDS;
  REQUIRE_FALSE(owners_complete(foreign_owner));
  ++ledger_controls;
  unsigned shutdown_controls = 0;
  for (const auto& state : shutdowns) {
    for (unsigned field = 0; field < 4; ++field) {
      auto corrupt = state;
      if (field == 0) corrupt.stopped = false;
      if (field == 1) corrupt.idempotent = false;
      if (field == 2) corrupt.active = 1;
      if (field == 3) corrupt.free = 1;
      REQUIRE_FALSE(stopped(corrupt));
      ++shutdown_controls;
    }
  }
  CHECK(common_controls == 504);
  CHECK(scalar_controls == 560);
  CHECK(string_controls == 96);
  CHECK(numeric_controls == 48);
  CHECK(byte_controls == 88);
  CHECK(envelope_controls == 32);
  CHECK(error_controls == 288);
  CHECK(affected_controls == 32);
  CHECK(cleanup_controls == 2856);
  CHECK(ledger_controls == 800);
  CHECK(shutdown_controls == 32);
  CHECK(Clock::now() < deadline);
}
