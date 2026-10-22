// SPDX-License-Identifier: MIT
//
// Actual injected publication -> coroutine dispatcher -> protected PG outbox.
// The committed receipt is not evidence of listener or subscriber delivery.
// JSONB-supported text only: no U+0000 or unpaired UTF-16 surrogates.

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

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
constexpr std::size_t FAMILIES = 6;
constexpr std::array<unsigned, 2> SEEDS{0x1891U, 0x18A7U};
constexpr std::size_t REPLAYS = 2;
constexpr std::size_t PUBLICATIONS = FAMILIES * SEEDS.size() * REPLAYS;

auto demand(bool condition, const char* message) -> void {
  if (!condition) throw std::runtime_error(message);
}

// Numeric storage tags and object serialization order are not JS/JSON value
// identity. Types, array order, member sets and exact UTF-8 strings are.
auto equal(const Json::Value& actual, const Json::Value& expected) -> bool {
  if (actual.isNumeric() && expected.isNumeric())
    return actual.asDouble() == expected.asDouble();
  if (actual.type() != expected.type()) return false;
  if (expected.isObject()) {
    if (actual.getMemberNames() != expected.getMemberNames()) return false;
    for (const auto& key : expected.getMemberNames()) {
      if (!equal(actual[key], expected[key])) return false;
    }
    return true;
  }
  if (expected.isArray()) {
    if (actual.size() != expected.size()) return false;
    for (Json::ArrayIndex index = 0; index < expected.size(); ++index) {
      if (!equal(actual[index], expected[index])) return false;
    }
    return true;
  }
  return actual == expected;
}

auto list(std::initializer_list<int> values) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const int value : values)
    result.append(value);
  return result;
}

struct Expected {
  Json::Value payload;
  Json::Value units;
  Json::Value key_units{Json::objectValue};
};

// Independent literal oracles, not a conversion/serialization of the JS input.
auto expected(std::size_t family, std::size_t seed) -> Expected {
  Expected result;
  switch (family) {
    case 0: break;
    case 1: result.payload = seed != 0; break;
    case 2: result.payload = seed == 0 ? -17.0 : 2147483648.0; break;
    case 3:
      result.payload = seed == 0 ? "" : "\xc3\xa9\xce\xa9\xf0\x9f\x98\x80";
      result.units = seed == 0 ? list({}) : list({233, 937, 55357, 56832});
      break;
    case 4:
      result.payload = Json::Value{Json::arrayValue};
      result.units = Json::Value{Json::arrayValue};
      if (seed != 0) {
        result.payload.append(17);
        result.payload.append("\xc3\xa9\xce\xa9\xf0\x9f\x98\x80");
        result.payload.append(true);
        result.payload.append(Json::Value{});
        result.units.append(Json::Value{});
        result.units.append(list({233, 937, 55357, 56832}));
        result.units.append(Json::Value{});
        result.units.append(Json::Value{});
      }
      break;
    default:
      result.payload = Json::Value{Json::objectValue};
      result.units = Json::Value{Json::objectValue};
      if (seed != 0) {
        result.payload["z"] = 1.5;
        result.payload["\xc3\xa9"] = "\xe4\xb8\xad\xf0\x9f\x98\x80";
        result.payload["empty"] = "";
        result.payload["a"] = Json::Value{Json::arrayValue};
        result.payload["a"].append(false);
        result.payload["a"].append(9);
        result.units["z"] = Json::Value{};
        result.units["\xc3\xa9"] = list({20013, 55357, 56832});
        result.units["empty"] = list({});
        result.units["a"] = Json::Value{Json::arrayValue};
        result.units["a"].append(Json::Value{});
        result.units["a"].append(Json::Value{});
        result.key_units["z"] = list({122});
        result.key_units["\xc3\xa9"] = list({233});
        result.key_units["empty"] = list({101, 109, 112, 116, 121});
        result.key_units["a"] = list({97});
      }
      break;
  }
  return result;
}

auto literal(std::size_t family, std::size_t seed) -> std::string_view {
  constexpr std::array<std::array<std::string_view, 2>, FAMILIES> INPUTS{{
      {"null", "null"},
      {"false", "true"},
      {"-17", "2147483648"},
      {"''", R"('\u00e9\u03a9\ud83d\ude00')"},
      {"[]", R"([17,'\u00e9\u03a9\ud83d\ude00',true,null])"},
      {"({})",
       R"(({z:1.5,'\u00e9':'\u4e2d\ud83d\ude00',empty:'',a:[false,9]}))"},
  }};
  return INPUTS.at(family).at(seed);
}

auto source(std::size_t family, std::size_t seed, const std::string& channel)
    -> std::string {
  // The fixture channel contains only a validated constant prefix and digits.
  return "const channel='" + channel +
         "';const payload=" + std::string{literal(family, seed)} + R"JS(;
function units(v) {
  if(typeof v==='string')return Array.from({length:v.length},(_,i)=>v.charCodeAt(i));
  if(Array.isArray(v))return v.map(units);
  if(v!==null&&typeof v==='object') {
    const result={};for(const key of Object.keys(v))result[key]=units(v[key]);
    return result;
  }
  return null;
}
(async()=>{
  let acknowledgements=0;
  const ack=await pubsub.publish(channel,payload).then(value=>{
    ++acknowledgements;return value;
  });
  const keyUnits={};
  if(payload!==null&&!Array.isArray(payload)&&typeof payload==='object')
    for(const key of Object.keys(payload))keyUnits[key]=units(key);
  return {acknowledgements,ack,ackKind:typeof ack,channel,payload,
    units:units(payload),keyUnits};
})()
)JS";
}

struct Receipt {
  int rows = 0;
  std::int64_t id = 0;
  Json::Value envelope;
};

class Outbox {
 public:
  explicit Outbox(const plinth::Config::Database& config) {
    const auto info = plinth::db::connection_info(config) +
                      " connect_timeout=3 options='-c statement_timeout=3000 "
                      "-c lock_timeout=2500'";
    connection.reset(PQconnectdb(info.c_str()));
    demand(connection && PQstatus(connection.get()) == CONNECTION_OK,
           "native pubsub independent control connection unavailable");
    static std::atomic<unsigned> sequence{0};
    prefix = "plinth:ext:pubnative:p" + std::to_string(getpid()) + "s" +
             std::to_string(sequence.fetch_add(1));
    channels.reserve(PUBLICATIONS);
  }
  ~Outbox() {
    if (!finish()) std::terminate();
  }
  Outbox(const Outbox&) = delete;
  auto operator=(const Outbox&) -> Outbox& = delete;

  auto own(std::size_t family, std::size_t seed, std::size_t replay)
      -> std::string {
    auto channel = prefix + "f" + std::to_string(family) + "s" +
                   std::to_string(SEEDS.at(seed)) + "r" +
                   std::to_string(replay);
    demand(channel.size() <= 63 && read(channel).rows == 0,
           "native pubsub refuses existing fixture channel");
    channels.push_back(channel); // Own before issuing the native publication.
    return channel;
  }
  auto read(const std::string& channel) -> Receipt {
    const auto result =
        query("SELECT id,payload::text FROM plinth.realtime_outbox "
              "WHERE payload->>'channel'=$1 ORDER BY id",
              channel);
    Receipt receipt;
    receipt.rows = PQntuples(result.get());
    if (receipt.rows != 1) return receipt;
    demand(PQnfields(result.get()) == 2 &&
               PQgetisnull(result.get(), 0, 0) == 0 &&
               PQgetisnull(result.get(), 0, 1) == 0,
           "native pubsub receipt shape");
    receipt.id = std::stoll(PQgetvalue(result.get(), 0, 0));
    const auto* begin = PQgetvalue(result.get(), 0, 1);
    const auto length = PQgetlength(result.get(), 0, 1);
    Json::CharReaderBuilder builder;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
    std::string errors;
    demand(reader->parse(begin, begin + length, &receipt.envelope, &errors),
           "native pubsub receipt was not JSON");
    return receipt;
  }
  auto finish() noexcept -> bool {
    if (closed) return true;
    try {
      if (!plinth::lifecycle::async_tasks().drain(5s) ||
          !plinth::log::wait_for_audits(5s))
        return false;
      for (const auto& channel : channels) {
        static_cast<void>(query(
            "DELETE FROM plinth.realtime_outbox WHERE payload->>'channel'=$1 "
            "RETURNING id",
            channel));
        if (read(channel).rows != 0) return false;
      }
      closed = true;
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  auto query(const char* sql, const std::string& channel)
      -> std::unique_ptr<PGresult, decltype(&PQclear)> {
    const std::array<const char*, 1> values{channel.c_str()};
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(connection.get(), sql, 1, nullptr, values.data(), nullptr,
                     nullptr, 0),
        PQclear};
    demand(result && PQresultStatus(result.get()) == PGRES_TUPLES_OK,
           "native pubsub independent outbox operation failed");
    return result;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
  std::string prefix;
  std::vector<std::string> channels;
  bool closed = false;
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

struct Shutdown {
  bool stopped;
  bool idempotent;
  int active;
  int free;
  auto operator==(const Shutdown&) const -> bool = default;
};

auto profile() -> plinth::js::RuntimeLimits {
  auto result = plinth::js::default_runtime_limits();
  result.wall_clock_limit = 3s;
  return result;
}

class Owner {
 public:
  explicit Owner(const plinth::Config& config)
      : pool(nullptr, profile(), config, 1, nullptr, "pubnative") {}
  ~Owner() {
    try {
      if (!closed && finish() != Shutdown{true, true, 0, 0}) std::terminate();
    } catch (...) {
      std::terminate();
    }
  }
  Owner(const Owner&) = delete;
  auto operator=(const Owner&) -> Owner& = delete;

  struct Execution {
    plinth::js::EvalResult result;
    Cleanup cleanup;
    int admitted;
  };
  auto run(const std::string& program) -> Execution {
    demand(context == nullptr && program.size() < 8192,
           "native pubsub owner source/acquisition bound");
    context = pool.acquire();
    demand(context != nullptr, "native pubsub context unavailable");
    auto result =
        drogon::sync_wait(plinth::js::run_on_context(*context, program));
    const auto& batch = context->batch_state;
    const int admitted = context->next_callback_id;
    Cleanup cleanup{
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
        plinth::lifecycle::async_tasks().drain(5s),
        plinth::log::wait_for_audits(5s),
        plinth::lifecycle::async_tasks().active_for_test(),
        0};
    demand(cleanup.retired && cleanup.drained && cleanup.audits,
           "native pubsub refuses destruction before native retirement/drain");
    pool.destroy(std::exchange(context, nullptr));
    cleanup.pool_active = pool.active_count();
    return {std::move(result), cleanup, admitted};
  }
  auto finish() -> Shutdown {
    if (!plinth::lifecycle::async_tasks().drain(5s) ||
        !plinth::log::wait_for_audits(5s))
      return {false, false, -1, -1};
    if (context != nullptr) {
      if (context->execution_lifetime.load(std::memory_order_acquire))
        return {false, false, pool.active_count(), pool.free_count()};
      pool.destroy(std::exchange(context, nullptr));
    }
    const bool first = pool.shutdown(5s);
    const bool second = pool.shutdown(0ms);
    const Shutdown state{first, second, pool.active_count(), pool.free_count()};
    closed = state == Shutdown{true, true, 0, 0};
    return state;
  }

 private:
  plinth::js::RuntimePool pool;
  plinth::js::BridgeContext* context = nullptr;
  bool closed = false;
};

struct Observation {
  std::size_t family;
  std::size_t seed;
  std::size_t replay;
  std::string channel;
  bool completed;
  Json::Value javascript;
  Receipt receipt;
  Cleanup cleanup;
  Shutdown shutdown;
  int admitted;
};

auto matches(const Observation& actual, const Expected& want,
             const std::string& channel) -> bool {
  constexpr Cleanup CLEAN{true,  0,     0,     0,     0,    0,    0, 0, 0,
                          false, false, false, false, true, true, 0, 0};
  Json::Value envelope{Json::objectValue};
  envelope["layer"] = "extension";
  envelope["channel"] = channel;
  envelope["payload"] = want.payload;
  Json::Value javascript{Json::objectValue};
  javascript["acknowledgements"] = 1;
  // The native dispatcher returns JSON null, notwithstanding the older
  // binding comment describing Promise<undefined>. Pin execution, not prose.
  javascript["ack"] = Json::Value{};
  javascript["ackKind"] = "object";
  javascript["channel"] = channel;
  javascript["payload"] = want.payload;
  javascript["units"] = want.units;
  javascript["keyUnits"] = want.key_units;
  return actual.completed && actual.admitted == 1 &&
         actual.channel == channel && actual.receipt.rows == 1 &&
         actual.receipt.id > 0 && equal(actual.receipt.envelope, envelope) &&
         equal(actual.javascript, javascript) && actual.cleanup == CLEAN &&
         actual.shutdown == Shutdown{true, true, 0, 0};
}

// Same-kind finite substitutions where possible; null has no second same-kind
// value, so its explicitly labeled fallback changes shape to false.
auto corrupt_value(Json::Value& value) -> void {
  if (value.isNull())
    value = false;
  else if (value.isBool())
    value = !value.asBool();
  else if (value.isNumeric())
    value = value.asDouble() + 1.0;
  else if (value.isString())
    value = value.asString() + "x";
  else if (value.isArray())
    value.append(false);
  else
    value["unexpected"] = false;
}

auto corrupt_unit(Json::Value& value) -> bool {
  if (value.isNumeric()) {
    value = value.asInt() + 1;
    return true;
  }
  if (value.isArray()) {
    for (auto& child : value) {
      if (corrupt_unit(child)) return true;
    }
  } else if (value.isObject()) {
    for (const auto& key : value.getMemberNames()) {
      if (corrupt_unit(value[key])) return true;
    }
  }
  return false;
}

auto ledger_matches(const std::vector<Observation>& ledger) -> bool {
  if (ledger.size() != PUBLICATIONS) return false;
  std::array<bool, PUBLICATIONS> seen{};
  for (const auto& item : ledger) {
    if (item.family >= FAMILIES || item.seed >= SEEDS.size() ||
        item.replay >= REPLAYS)
      return false;
    const auto index =
        (item.family * SEEDS.size() + item.seed) * REPLAYS + item.replay;
    if (seen[index]) return false;
    seen[index] = true;
    if (!matches(item, expected(item.family, item.seed), item.channel))
      return false;
    for (const auto& other : ledger) {
      if (&item != &other && (item.channel == other.channel ||
                              item.receipt.id == other.receipt.id))
        return false;
    }
  }
  return true;
}

} // namespace

TEST_CASE("Native pubsub publish acknowledgements preserve the owned corpus",
          "[js][realtime][integration][native-pubsub-publish]") {
  REQUIRE(plinth::async_bridge_test::pg_available());
  plinth::async_bridge_test::ensure_drogon_with_db_running();
  REQUIRE(plinth::lifecycle::async_tasks().drain(5s));
  REQUIRE(plinth::log::wait_for_audits(5s));
  const auto config = plinth::async_bridge_test::test_config();
  plinth::async_bridge_test::reset_schema(config.db);
  Outbox outbox{config.db};
  std::vector<Observation> ledger;
  ledger.reserve(PUBLICATIONS);
  unsigned mutation_controls = 0;
  unsigned unit_controls = 0;
  unsigned key_unit_controls = 0;
  for (std::size_t family = 0; family < FAMILIES; ++family) {
    for (std::size_t seed = 0; seed < SEEDS.size(); ++seed) {
      for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
        INFO("family=" << family << " seed=" << SEEDS[seed]
                       << " replay=" << replay);
        const auto channel = outbox.own(family, seed, replay);
        Owner owner{config};
        const auto execution = owner.run(source(family, seed, channel));
        const auto shutdown = owner.finish();
        const auto receipt = outbox.read(channel);
        Observation actual{family,
                           seed,
                           replay,
                           channel,
                           execution.result.value.has_value(),
                           execution.result.value.value_or(Json::Value{}),
                           receipt,
                           execution.cleanup,
                           shutdown,
                           execution.admitted};
        const auto want = expected(family, seed);
        // All actual native, audit and pool cleanup precedes assertions.
        REQUIRE(matches(actual, want, channel));
        const auto reject = [&](const Observation& corrupted) {
          REQUIRE_FALSE(matches(corrupted, want, channel));
          ++mutation_controls;
        };
        auto changed = actual;
        changed.completed = false;
        reject(changed);
        changed = actual;
        ++changed.admitted;
        reject(changed);
        changed = actual;
        changed.receipt.rows = 0;
        reject(changed);
        changed = actual;
        changed.receipt.rows = 2;
        reject(changed);
        changed = actual;
        changed.receipt.id = 0;
        reject(changed);
        changed = actual;
        changed.receipt.envelope["layer"] = "data";
        reject(changed);
        changed = actual;
        changed.receipt.envelope["channel"] = channel + "x";
        reject(changed);
        changed = actual;
        corrupt_value(changed.receipt.envelope["payload"]);
        reject(changed);
        for (const int count : {0, 2}) {
          changed = actual;
          changed.javascript["acknowledgements"] = count;
          reject(changed);
        }
        changed = actual;
        changed.javascript["ack"] = false;
        reject(changed);
        changed = actual;
        changed.javascript["ackKind"] = "undefined";
        reject(changed);
        changed = actual;
        corrupt_value(changed.javascript["payload"]);
        reject(changed);
        changed = actual;
        corrupt_value(changed.javascript["units"]);
        reject(changed);
        changed = actual;
        changed.javascript["keyUnits"]["unexpected"] = list({120});
        reject(changed);
        // Change a real observed code unit, not merely an array's shape.
        // Only the three nonempty text-bearing seed-1 families have one.
        if (family >= 3 && seed == 1) {
          changed = actual;
          REQUIRE(corrupt_unit(changed.javascript["units"]));
          REQUIRE_FALSE(matches(changed, want, channel));
          ++unit_controls;
        }
        if (family == 5 && seed == 1) {
          changed = actual;
          REQUIRE(corrupt_unit(changed.javascript["keyUnits"]));
          REQUIRE_FALSE(matches(changed, want, channel));
          ++key_unit_controls;
        }
        changed = actual;
        changed.cleanup.retired = false;
        reject(changed);
        changed = actual;
        changed.cleanup.native_active = 1;
        reject(changed);
        changed = actual;
        changed.shutdown.idempotent = false;
        reject(changed);
        ledger.push_back(std::move(actual));
      }
    }
  }
  REQUIRE(mutation_controls == 18 * PUBLICATIONS);
  REQUIRE(unit_controls == 6);
  REQUIRE(key_unit_controls == 2);
  REQUIRE(ledger_matches(ledger));
  auto changed = ledger;
  changed.pop_back();
  REQUIRE_FALSE(ledger_matches(changed));
  changed = ledger;
  changed.push_back(ledger.front());
  REQUIRE_FALSE(ledger_matches(changed));
  changed = ledger;
  changed.back() = changed.front();
  REQUIRE_FALSE(ledger_matches(changed));
  changed = ledger;
  changed.back().receipt.id = changed.front().receipt.id;
  REQUIRE_FALSE(ledger_matches(changed));
  changed = ledger;
  changed.back().channel = changed.front().channel;
  REQUIRE_FALSE(ledger_matches(changed));
  REQUIRE(outbox.finish());
  REQUIRE(outbox.finish());
}
