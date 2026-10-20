// SPDX-License-Identifier: MIT
//
// Tier 2 extension capability dispatch — ICD-0.5.0.3 §Test plan
// groups R/E/P/H/C. Exercises `plinth::extensions::dispatch` both
// directly and through the resolver's `call_capability_async` path,
// covering the handler-invocation pipeline, error taxonomy, identity
// propagation, and lifecycle race windows.
//
// Library-level install: each case stages an extension on-disk under
// a temp data_dir, seeds the Tier 2 cache with the matching row, and
// calls `create_pool` directly. This avoids the full install_package
// + PG round-trip for cases that exercise dispatch alone; PG-backed
// install paths are covered by lifecycle_transitions_test.cpp's
// existing D.*/U.*/X.* groups now that those transitions call
// `plinth::extensions::create_pool` / `destroy_pool`.

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/eval_guard.hpp"
#include "kernel/logging.hpp"

#include "../js/async_bridge_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using plinth::async_bridge_test::ensure_drogon_running;
using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::test_config;

namespace {

// Global counter to derive unique scratch paths per test instance
// (Catch2 runs tests in the same subprocess; a new test case starts
// in the same process as the previous).
std::atomic<std::uint64_t> scratch_counter{0};

// Per-case scratch environment: temp data_dir + initialized registry.
// The registry's `cfg_ptr` is stashed from the Config pointer passed
// to `init_registry`, so the Config must outlive the scratch.
struct ExtScratch {
  plinth::Config cfg{};
  fs::path base;

  ExtScratch() {
    // PG + DbClient required so resolver `emit_rbac_denied` and
    // registry `emit_extension_error_audit` paths land on a real
    // audit row instead of asserting inside Drogon's
    // DbClientManager (debug-build contract). Every test in this
    // file is `[integration]` + `pg_available()`-gated for that
    // reason. Caller must check `pg_available()` before constructing.
    ensure_drogon_with_db_running();

    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_extdisp_" + id);
    fs::create_directories(base / "data");

    cfg = test_config();
    cfg.packages_data_dir = (base / "data").string();
    // init_registry tries to SELECT from plinth.packages. In
    // these unit-level tests the scan may find nothing (tests do
    // not seed the table) but the PG connect succeeds, so
    // `cfg_ptr` is set for `create_pool` to consume.
    plinth::extensions::init_registry(cfg);
  }

  ~ExtScratch() {
    (void)plinth::extensions::shutdown_registry();
    plinth::capabilities::clear_resolver_for_test();
    std::error_code ec;
    fs::remove_all(base, ec);
  }

  ExtScratch(const ExtScratch&) = delete;
  auto operator=(const ExtScratch&) -> ExtScratch& = delete;
  ExtScratch(ExtScratch&&) = delete;
  auto operator=(ExtScratch&&) -> ExtScratch& = delete;

  // Stage an extension on disk at {data_dir}/extensions/<name>/1.0.0/
  // + the `active` symlink. Handlers is a list of (function, source)
  // pairs written verbatim to server/handlers/<fn>.js. The caller
  // also seeds the Tier 2 cache for any capabilities they intend to
  // dispatch via `call_capability_async`.
  auto stage_extension(
      std::string_view name,
      std::initializer_list<std::pair<std::string_view, std::string_view>>
          handlers) const -> void {
    fs::path ext_root = base / "data" / "extensions" / std::string{name};
    fs::path version_dir = ext_root / "1.0.0";
    fs::create_directories(version_dir / "server" / "handlers");

    // server/main.js — re-export per fixture convention (stub
    // contents; the registry's dispatcher reads handlers/<fn>.js
    // directly, not server/main.js).
    std::ofstream(version_dir / "server" / "main.js")
        << "// extension dispatch fixture\n";

    for (const auto& [fn, src] : handlers) {
      std::ofstream(version_dir / "server" / "handlers" /
                    (std::string{fn} + ".js"))
          << src;
    }

    // Create (or replace) the `active` symlink to point at 1.0.0.
    fs::path active = ext_root / "active";
    std::error_code ec;
    fs::remove(active, ec);
    fs::create_directory_symlink("1.0.0", active, ec);
  }
};

// Seed a single Tier 2 cache row matching an extension handler.
auto seed_tier2(std::string_view signature, std::string_view extension_name,
                std::string_view rbac_rule = "ext.call") -> void {
  plinth::capabilities::CachedCapability entry{
      .signature = std::string{signature},
      .provider_type = "extension",
      .extension_name = std::string{extension_name},
      .scope = "instance",
      .user_id = {},
      .rbac_rule = std::string{rbac_rule},
      .enabled = true,
  };
  plinth::capabilities::seed_tier2_cache_for_test(entry);
}

auto admin_ctx() -> plinth::capabilities::UserContext {
  return plinth::capabilities::UserContext{
      .user_id = "11111111-1111-1111-1111-111111111111",
      .username = "tester",
      .auth_type = "session",
      .effective_rules = {"kernel.admin"}, // universal match
      .session_id = {},
      .ip_address = {},
  };
}

auto require_persisted_audit(std::string_view action, std::string_view user,
                             std::string_view code = {},
                             std::string_view extension = {},
                             std::string_view function = {}) -> void {
  REQUIRE(plinth::log::wait_for_audits(2s));
  const auto conninfo =
      plinth::db::connection_info(test_config().db) + " connect_timeout=3";
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{
      PQconnectdb(conninfo.c_str()), PQfinish};
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  const std::array<std::string, 5> values{
      std::string{action}, std::string{user}, std::string{code},
      std::string{extension}, std::string{function}};
  const std::array<const char*, 5> params{values[0].c_str(), values[1].c_str(),
                                          values[2].c_str(), values[3].c_str(),
                                          values[4].c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> result{
      PQexecParams(connection.get(),
                   "SELECT count(*) FROM plinth.audit_log "
                   "WHERE action=$1 AND user_id=$2::uuid "
                   "AND ($3='' OR detail->>'code'=$3) "
                   "AND ($4='' OR detail->>'extension'=$4) "
                   "AND ($5='' OR detail->>'function'=$5)",
                   5, nullptr, params.data(), nullptr, nullptr, 0),
      PQclear};
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(result.get()) == 1);
  REQUIRE(std::string_view{PQgetvalue(result.get(), 0, 0)} == "1");
}

auto run_dispatch(std::string_view ext_name, std::string_view fn,
                  const Json::Value& args)
    -> std::expected<Json::Value, plinth::js::PromiseRejection> {
  return drogon::sync_wait(
      plinth::extensions::dispatch(ext_name, fn, args, admin_ctx(), 0));
}

[[nodiscard]] auto query_is_active(PGconn* connection, std::string_view marker)
    -> bool {
  const auto pattern = "%" + std::string{marker} + "%";
  const std::array<const char*, 1> params{pattern.c_str()};
  std::unique_ptr<PGresult, decltype(&PQclear)> result{
      PQexecParams(connection,
                   "SELECT EXISTS (SELECT 1 FROM pg_stat_activity "
                   "WHERE pid <> pg_backend_pid() AND state = 'active' "
                   "AND query LIKE $1)",
                   1, nullptr, params.data(), nullptr, nullptr, 0),
      &PQclear};
  return PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
         std::string_view{PQgetvalue(result.get(), 0, 0)} == "t";
}

auto run_resolver(std::string_view signature, const Json::Value& args,
                  std::string* ext_code_out = nullptr,
                  std::string* ext_msg_out = nullptr)
    -> plinth::capabilities::ResolveResult {
  plinth::capabilities::CapabilityCall call{
      .signature = std::string{signature},
      .args = args,
      .call_depth = 0,
  };
  return drogon::sync_wait(plinth::capabilities::call_capability_async(
      std::move(call), admin_ctx(), ext_code_out, ext_msg_out));
}

class ModuleGatePolicy {
 public:
  ModuleGatePolicy() {
    plinth::js::set_unicode_scanner_policy(true, 50, false);
  }
  ~ModuleGatePolicy() {
    plinth::extensions::set_module_engine_observer_for_test({});
    plinth::js::set_unicode_scanner_policy(true, 50, false);
  }
  ModuleGatePolicy(const ModuleGatePolicy&) = delete;
  auto operator=(const ModuleGatePolicy&) -> ModuleGatePolicy& = delete;
  ModuleGatePolicy(ModuleGatePolicy&&) = delete;
  auto operator=(ModuleGatePolicy&&) -> ModuleGatePolicy& = delete;
};

struct ModuleEngineCounts {
  int compiled = 0;
  int evaluated = 0;
  int outer_compiled = 0;
  int outer_evaluated = 0;
};

class ModuleEngineWatch {
 public:
  ModuleEngineWatch(std::string target, std::string outer)
      : state(std::make_shared<State>(std::move(target), std::move(outer))) {
    // Independent shared ownership survives an already copied observer. The
    // callback performs only fixed string comparisons and atomic increments.
    plinth::extensions::set_module_engine_observer_for_test(
        [owned = state](std::string_view id,
                        plinth::extensions::ModuleEngineStage stage) noexcept {
          const bool compile =
              stage == plinth::extensions::ModuleEngineStage::COMPILE;
          if (id == owned->target) {
            (compile ? owned->compiled : owned->evaluated)
                .fetch_add(1, std::memory_order_relaxed);
          }
          if (id == owned->outer) {
            (compile ? owned->outer_compiled : owned->outer_evaluated)
                .fetch_add(1, std::memory_order_relaxed);
          }
        });
  }
  ~ModuleEngineWatch() {
    plinth::extensions::set_module_engine_observer_for_test({});
  }
  ModuleEngineWatch(const ModuleEngineWatch&) = delete;
  auto operator=(const ModuleEngineWatch&) -> ModuleEngineWatch& = delete;
  ModuleEngineWatch(ModuleEngineWatch&&) = delete;
  auto operator=(ModuleEngineWatch&&) -> ModuleEngineWatch& = delete;

  auto counts() const -> ModuleEngineCounts {
    return {.compiled = state->compiled.load(std::memory_order_relaxed),
            .evaluated = state->evaluated.load(std::memory_order_relaxed),
            .outer_compiled =
                state->outer_compiled.load(std::memory_order_relaxed),
            .outer_evaluated =
                state->outer_evaluated.load(std::memory_order_relaxed)};
  }

 private:
  struct State {
    State(std::string target_in, std::string outer_in)
        : target(std::move(target_in)), outer(std::move(outer_in)) {}
    const std::string target;
    const std::string outer;
    std::atomic<int> compiled{0};
    std::atomic<int> evaluated{0};
    std::atomic<int> outer_compiled{0};
    std::atomic<int> outer_evaluated{0};
  };
  std::shared_ptr<State> state;
};

struct ModuleDispatchObservation {
  std::expected<Json::Value, plinth::js::PromiseRejection> result;
  ModuleEngineCounts counts;
};

auto observe_module_dispatch(std::string_view function, const Json::Value& args,
                             std::string target, bool nested = false)
    -> ModuleDispatchObservation {
  ModuleEngineWatch watch{std::move(target),
                          nested ? "modulegate/" + std::string{function} : ""};
  auto result = run_dispatch("modulegate", function, args);
  return {.result = std::move(result), .counts = watch.counts()};
}

auto check_module_entries(const ModuleDispatchObservation& observed,
                          int compiled, int evaluated, bool nested = false)
    -> void {
  CHECK(observed.counts.compiled == compiled);
  CHECK(observed.counts.evaluated == evaluated);
  if (nested) {
    // A zero targeted count is meaningful only with a live outer observer.
    CHECK(observed.counts.outer_compiled == 1);
    CHECK(observed.counts.outer_evaluated == 1);
  }
}

auto module_args(std::string source, std::string id, bool caught = true)
    -> Json::Value {
  REQUIRE(source.size() <= 4096);
  REQUIRE(id.size() <= 128);
  Json::Value args{Json::objectValue};
  args["src"] = source;
  args["id"] = id;
  args["caught"] = caught;
  return args;
}

auto marked_module(std::size_t findings, bool async_default = false)
    -> std::string {
  REQUIRE(findings <= 50);
  std::string source{"// "};
  for (std::size_t index = 0; index < findings; ++index) {
    source += "\xEF\xB8\x8F"; // literal UTF-8 U+FE0F, not a scanner oracle
  }
  source += "\nglobalThis.module_body=(globalThis.module_body||0)+1;"
            "export default ";
  if (async_default) {
    source += "async ";
  }
  source += "function(){globalThis.module_called="
            "(globalThis.module_called||0)+1;return 42;}";
  return source;
}

auto check_nested_value(const ModuleDispatchObservation& observed) -> void {
  REQUIRE(observed.result.has_value());
  const auto& value = *observed.result;
  REQUIRE(value.isObject());
  CHECK(value.size() == 4);
  CHECK(value["caught"].type() == Json::booleanValue);
  CHECK_FALSE(value["caught"].asBool());
  CHECK(value["value"].type() == Json::intValue);
  CHECK(value["value"].asInt() == 42);
  CHECK(value["body"].type() == Json::intValue);
  CHECK(value["body"].asInt() == 1);
  CHECK(value["called"].type() == Json::intValue);
  CHECK(value["called"].asInt() == 1);
}

auto check_nested_caught(const ModuleDispatchObservation& observed,
                         std::string_view name, std::string_view message,
                         int body = 0, int called = 0) -> void {
  REQUIRE(observed.result.has_value());
  const auto& value = *observed.result;
  REQUIRE(value.isObject());
  CHECK(value["caught"].type() == Json::booleanValue);
  CHECK(value["caught"].asBool());
  CHECK(value["name"].type() == Json::stringValue);
  CHECK(value["name"].asString() == name);
  CHECK(value["message"].type() == Json::stringValue);
  CHECK_FALSE(value["message"].asString().empty());
  CHECK(value["message"].asString().find(message) != std::string::npos);
  CHECK(value["body"].type() == Json::intValue);
  CHECK(value["body"].asInt() == body);
  CHECK(value["called"].type() == Json::intValue);
  CHECK(value["called"].asInt() == called);
}

auto check_module_error(const ModuleDispatchObservation& observed,
                        std::string_view code, std::string_view message)
    -> void {
  CHECK_FALSE(observed.result.has_value());
  if (!observed.result) {
    CHECK(observed.result.error().code == code);
    CHECK_FALSE(observed.result.error().message.empty());
    CHECK(observed.result.error().message.find(message) != std::string::npos);
    CHECK_FALSE(observed.result.error().sqlstate.has_value());
    CHECK(observed.result.error().message.size() <= 1024);
  }
}

// These regressions isolate two real reverse-conversion entry points. The JS
// observer returns ASCII-key metadata only; it never echoes the problematic
// object through the forward converter to manufacture an expected round trip.
constexpr std::string_view REVERSE_NAME_OBSERVER = R"(
  function observeNames(value) {
    const own = (object, name) =>
      Object.prototype.hasOwnProperty.call(object, name);
    const units = name => Array.from({length:name.length},
      (_, index) => name.charCodeAt(index));
    const snapshot = (object, names) => {
      const actual = Object.getOwnPropertyNames(object).sort();
      return {count:actual.length, names:actual.map(units),
        members:names.map(name => {
          const owned = own(object, name);
          return {owned, kind:owned ? typeof object[name] : 'missing',
            value:owned ? object[name] : null};
        })};
    };
    globalThis.reverse_name_visits = (globalThis.reverse_name_visits || 0) + 1;
    return {entered:true, visits:globalThis.reverse_name_visits,
      ordinary:{plainOwn:own(value, 'plain'), plain:value.plain,
        numberOwn:own(value, '7'), number:value['7'],
        unicodeOwn:own(value, '\u00e9'), unicode:value['\u00e9'],
        prefixOwn:own(value, 'p'), prefix:value.p,
        prototypeOwn:own(value, '__proto__'),
        prototypeOrdinary:Object.getPrototypeOf(value) === Object.prototype,
        prototypeMarker:Object.getPrototypeOf(value).prototypeMarker,
        prototypeParentOrdinary:
          Object.getPrototypeOf(Object.getPrototypeOf(value)) === Object.prototype},
      root:snapshot(value, ['', '\0', 'plain', '7', '\u00e9', 'p',
        'p\0suffix', 'p\0', '\0lead', 'm\0n\0z', '\u00e9\0\u03a9',
        'q'.repeat(64) + '\0tail']),
      nested:snapshot(value.nested, ['n', 'n\0x', '', '\0']),
      items:[snapshot(value.items[0], ['a', 'a\0']),
             snapshot(value.items[1], ['\u00e9', '\u00e9\0x'])]};
  }
)";

auto reverse_name_payload(bool full, bool prototype_object) -> Json::Value {
  Json::Value value{Json::objectValue};
  value["plain"] = 103;
  value["7"] = 104;
  value["\xC3\xA9"] = 105;
  value["p"] = 201;
  // End-state controls for ordinary setter semantics, not DefineProperty or
  // null-prototype policy. Registry input uses a primitive; native settlement
  // uses an object. The primitive's unchanged prototype is not proof that its
  // setter executed. Neither input mutates Object.prototype itself.
  if (prototype_object) {
    value["__proto__"]["prototypeMarker"] = 901;
  } else {
    value["__proto__"] = 901;
  }
  value["nested"]["n"] = 301;
  value["items"] = Json::Value{Json::arrayValue};
  Json::Value first{Json::objectValue};
  first["a"] = 401;
  Json::Value second{Json::objectValue};
  second["\xC3\xA9"] = 403;
  if (full) {
    value[""] = 101;
    value[std::string{"\0", 1}] = 102;
    value[std::string{"p\0suffix", 8}] = 202;
    value[std::string{"p\0", 2}] = 203;
    value[std::string{"\0lead", 5}] = 204;
    value[std::string{"m\0n\0z", 5}] = 205;
    value[std::string{"\xC3\xA9\0\xCE\xA9", 5}] = 206;
    value[std::string(64, 'q') + std::string{"\0tail", 5}] = 207;
    value["nested"][std::string{"n\0x", 3}] = 302;
    value["nested"][""] = 303;
    value["nested"][std::string{"\0", 1}] = 304;
    first[std::string{"a\0", 2}] = 402;
    second[std::string{"\xC3\xA9\0x", 4}] = 404;
  }
  value["items"].append(first);
  value["items"].append(second);
  return value;
}

auto literal_name_units(std::initializer_list<std::initializer_list<int>> names)
    -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const auto& name : names) {
    Json::Value units{Json::arrayValue};
    for (const int unit : name) {
      units.append(unit);
    }
    result.append(units);
  }
  return result;
}

auto literal_name_snapshot(const Json::Value& names, int count,
                           std::initializer_list<std::optional<int>> values)
    -> Json::Value {
  Json::Value result{Json::objectValue};
  result["count"] = count;
  result["names"] = names;
  result["members"] = Json::Value{Json::arrayValue};
  for (const auto value : values) {
    Json::Value member{Json::objectValue};
    member["owned"] = value.has_value();
    member["kind"] = value.has_value() ? "number" : "missing";
    member["value"] = value.has_value() ? Json::Value{*value} : Json::Value{};
    result["members"].append(member);
  }
  return result;
}

auto reverse_name_expected(bool full, bool prototype_object) -> Json::Value {
  // UTF-16 units and values below are literal oracles, independent of the
  // payload's UTF-8 names and the production converters. Names use JS's
  // lexicographic order, including empty, NUL-only and prefix distinctions.
  Json::Value expected{Json::objectValue};
  expected["entered"] = true;
  expected["visits"] = 1;
  auto& ordinary = expected["ordinary"];
  ordinary["plainOwn"] = true;
  ordinary["plain"] = 103;
  ordinary["numberOwn"] = true;
  ordinary["number"] = 104;
  ordinary["unicodeOwn"] = true;
  ordinary["unicode"] = 105;
  ordinary["prefixOwn"] = true;
  ordinary["prefix"] = 201;
  ordinary["prototypeOwn"] = false;
  ordinary["prototypeOrdinary"] = !prototype_object;
  ordinary["prototypeMarker"] =
      prototype_object ? Json::Value{901} : Json::Value{};
  ordinary["prototypeParentOrdinary"] = prototype_object;
  expected["items"] = Json::Value{Json::arrayValue};
  if (!full) {
    expected["root"] = literal_name_snapshot(
        literal_name_units({{55},
                            {105, 116, 101, 109, 115},
                            {110, 101, 115, 116, 101, 100},
                            {112},
                            {112, 108, 97, 105, 110},
                            {233}}),
        6,
        {std::nullopt, std::nullopt, 103, 104, 105, 201, std::nullopt,
         std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    expected["nested"] =
        literal_name_snapshot(literal_name_units({{110}}), 1,
                              {301, std::nullopt, std::nullopt, std::nullopt});
    expected["items"].append(literal_name_snapshot(literal_name_units({{97}}),
                                                   1, {401, std::nullopt}));
    expected["items"].append(literal_name_snapshot(literal_name_units({{233}}),
                                                   1, {403, std::nullopt}));
    return expected;
  }
  auto names = literal_name_units({{},
                                   {0},
                                   {0, 108, 101, 97, 100},
                                   {55},
                                   {105, 116, 101, 109, 115},
                                   {109, 0, 110, 0, 122},
                                   {110, 101, 115, 116, 101, 100},
                                   {112},
                                   {112, 0},
                                   {112, 0, 115, 117, 102, 102, 105, 120},
                                   {112, 108, 97, 105, 110}});
  Json::Value long_units{Json::arrayValue};
  for (int index = 0; index < 64; ++index) {
    long_units.append(113);
  }
  for (const int unit : {0, 116, 97, 105, 108}) {
    long_units.append(unit);
  }
  names.append(long_units);
  names.append(literal_name_units({{233}})[0]);
  names.append(literal_name_units({{233, 0, 937}})[0]);
  expected["root"] = literal_name_snapshot(
      names, 14, {101, 102, 103, 104, 105, 201, 202, 203, 204, 205, 206, 207});
  expected["nested"] =
      literal_name_snapshot(literal_name_units({{}, {0}, {110}, {110, 0, 120}}),
                            4, {301, 302, 303, 304});
  expected["items"].append(literal_name_snapshot(
      literal_name_units({{97}, {97, 0}}), 2, {401, 402}));
  expected["items"].append(literal_name_snapshot(
      literal_name_units({{233}, {233, 0, 120}}), 2, {403, 404}));
  return expected;
}

auto check_reverse_name_snapshot(const Json::Value& actual,
                                 const Json::Value& expected,
                                 std::string_view location) -> void {
  INFO("member-name location=" << location);
  REQUIRE(actual.isObject());
  CHECK(actual.size() == 3);
  CHECK(actual["count"] == expected["count"]);
  // Complete unit arrays encode exact length, byte-independent names and
  // membership; no substring or either-spelling acceptance is used.
  CHECK(actual["names"] == expected["names"]);
  REQUIRE(actual["members"].isArray());
  CHECK(actual["members"].size() == expected["members"].size());
  for (Json::ArrayIndex index = 0; index < expected["members"].size();
       ++index) {
    INFO("literal member index=" << index);
    CHECK(actual["members"][index] == expected["members"][index]);
  }
}

struct ReverseNativeEntries {
  std::atomic<int> ordinary{0};
  std::atomic<int> full{0};
  std::atomic<int> invalid{0};
};

auto check_reverse_name_entrypoint(bool promise_settlement) -> void {
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  for (int replay = 0; replay < 2; ++replay) {
    INFO("reverse-name path=" << (promise_settlement ? "cap.call" : "arguments")
                              << " fresh replay=" << replay);
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    ExtScratch scratch;
    auto native = std::make_shared<ReverseNativeEntries>();
    if (promise_settlement) {
      plinth::capabilities::register_tier1_handler(
          "kernel:1:reverse_properties", "kernel.admin",
          [native](const Json::Value& args,
                   const plinth::capabilities::UserContext& user,
                   int depth) -> plinth::capabilities::HandlerOutcome {
            if (!args.isObject() || args.size() != 1 ||
                !args["mode"].isString() ||
                user.user_id != "11111111-1111-1111-1111-111111111111" ||
                user.username != "tester" || depth != 2) {
              native->invalid.fetch_add(1, std::memory_order_relaxed);
              return std::unexpected(
                  plinth::capabilities::CapabilityError::INVALID_CAPABILITY);
            }
            const auto mode = args["mode"].asString();
            if (mode == "ordinary") {
              native->ordinary.fetch_add(1, std::memory_order_relaxed);
              return reverse_name_payload(false, true);
            }
            if (mode == "full") {
              native->full.fetch_add(1, std::memory_order_relaxed);
              return reverse_name_payload(true, true);
            }
            native->invalid.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(
                plinth::capabilities::CapabilityError::INVALID_CAPABILITY);
          });
    }
    const auto source =
        std::string{REVERSE_NAME_OBSERVER} +
        (promise_settlement
             ? "export default async function(args) { const value = await "
               "cap.call('kernel:1:reverse_properties', {mode:args.mode}); "
               "return observeNames(value); }"
             : "export default function(args) { return observeNames(args); }");
    REQUIRE(source.size() <= 4096);
    scratch.stage_extension("reverseprops", {{"inspect", source}});
    seed_tier2("reverseprops:1:inspect", "reverseprops");
    REQUIRE(plinth::extensions::create_pool("reverseprops"));
    REQUIRE(plinth::extensions::has_pool("reverseprops"));
    ModuleEngineWatch engines{"reverseprops/inspect", ""};
    Json::Value ordinary_args = reverse_name_payload(false, false);
    Json::Value full_args = reverse_name_payload(true, false);
    if (promise_settlement) {
      // Only clean ASCII mode arguments cross registry injection. The native
      // literal payload reaches JS solely through actual owned cap.call
      // delivery.
      ordinary_args = Json::Value{Json::objectValue};
      ordinary_args["mode"] = "ordinary";
      full_args = Json::Value{Json::objectValue};
      full_args["mode"] = "full";
    }
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    const auto ordinary = run_resolver("reverseprops:1:inspect", ordinary_args);
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    const auto full = run_resolver("reverseprops:1:inspect", full_args);
    REQUIRE(std::chrono::steady_clock::now() < deadline);

    // Finish real ownership cleanup before the intentionally red member-name
    // assertions. ExtScratch remains an unwinding fallback on earlier failures.
    plinth::extensions::destroy_pool("reverseprops");
    REQUIRE_FALSE(plinth::extensions::has_pool("reverseprops"));
    REQUIRE(plinth::extensions::shutdown_registry(5s));
    REQUIRE(plinth::extensions::inflight_dispatch_count_for_test() == 0);
    REQUIRE(plinth::extensions::failed_pool_count_for_test() == 0);
    REQUIRE(plinth::extensions::pending_pool_constructions_for_test() == 0);
    REQUIRE(plinth::extensions::shutdown_registry(0ms));
    REQUIRE(engines.counts().compiled == 2);
    REQUIRE(engines.counts().evaluated == 2);
    if (promise_settlement) {
      REQUIRE(native->ordinary.load(std::memory_order_relaxed) == 1);
      REQUIRE(native->full.load(std::memory_order_relaxed) == 1);
      REQUIRE(native->invalid.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(ordinary.has_value());
    REQUIRE(full.has_value());
    CHECK(ordinary->resolved_tier == "tier2");
    CHECK(ordinary->provider_type == "extension");
    CHECK(full->resolved_tier == "tier2");
    CHECK(full->provider_type == "extension");

    const auto expected_ordinary =
        reverse_name_expected(false, promise_settlement);
    REQUIRE(ordinary->data == expected_ordinary);
    // Causal controls mutate actual validated ordinary-path observations, not
    // expected fixtures. Distinct prefix and member values must be detected.
    auto wrong_prefix = ordinary->data;
    wrong_prefix["ordinary"]["prefix"] = 202;
    CHECK_FALSE(wrong_prefix == expected_ordinary);
    auto wrong_member = ordinary->data;
    wrong_member["root"]["members"][5]["value"] = 999;
    CHECK_FALSE(wrong_member == expected_ordinary);

    const auto expected_full = reverse_name_expected(true, promise_settlement);
    REQUIRE(full->data.isObject());
    CHECK(full->data.size() == 6);
    CHECK(full->data["entered"] == expected_full["entered"]);
    CHECK(full->data["visits"] == expected_full["visits"]);
    CHECK(full->data["ordinary"] == expected_full["ordinary"]);
    check_reverse_name_snapshot(full->data["root"], expected_full["root"],
                                "root");
    check_reverse_name_snapshot(full->data["nested"], expected_full["nested"],
                                "nested");
    REQUIRE(full->data["items"].isArray());
    CHECK(full->data["items"].size() == 2);
    check_reverse_name_snapshot(full->data["items"][0],
                                expected_full["items"][0], "items[0]");
    check_reverse_name_snapshot(full->data["items"][1],
                                expected_full["items"][1], "items[1]");
    REQUIRE(std::chrono::steady_clock::now() < deadline);
  }
}

} // namespace

// Sentinel for ordinary cases when PG isn't available. The strict module gate
// case requires PG; ordinary cases use `SKIP_WITHOUT_PG();` so
// `plinth_tests_pg` SKIPs cleanly when the env is absent instead of tripping
// the DbClientManager assert via ExtScratch's Drogon bring-up. Must remain a
// function-like macro rather than a helper function: Catch2's `SKIP(...)`
// relies on exception-based flow from within the test body; a wrapper function
// works at runtime but confuses the compiler's reachability analysis for tests
// that touch objects whose RAII teardown races with Catch2's SKIP handler —
// this bit repeatedly in 0.5.0.4 pre-merge. Keep the macro; suppress the lint.
#define SKIP_WITHOUT_PG()                                                      \
  do {                                                                         \
    if (!pg_available()) {                                                     \
      SKIP("PG not available");                                                \
    }                                                                          \
  } while (false)

TEST_CASE("Extension argument injection preserves JSON member names",
          "[cap][ext][integration][argument-property-names]") {
  REQUIRE(pg_available());
  check_reverse_name_entrypoint(false);
}

TEST_CASE("Capability promise settlement preserves JSON member names",
          "[cap][ext][integration][promise-property-names]") {
  REQUIRE(pg_available());
  check_reverse_name_entrypoint(true);
}

TEST_CASE("Extension module loading enforces the original source gate",
          "[cap][ext][integration][module-source-gate]") {
  REQUIRE(pg_available());
  ModuleGatePolicy policy;
  ExtScratch scratch;
  constexpr std::string_view LOAD = R"(
    export default async function(args) {
      globalThis.module_body = 0;
      globalThis.module_called = 0;
      if (!args.caught) {
        const mod = globalThis.import_from_src(args.src, args.id);
        return await mod.default();
      }
      try {
        const mod = globalThis.import_from_src(args.src, args.id);
        const value = await mod.default();
        return {caught:false, value, body:globalThis.module_body,
                called:globalThis.module_called};
      } catch (e) {
        return {caught:true, name:e.name, message:e.message,
                body:globalThis.module_body, called:globalThis.module_called};
      }
    }
  )";
  constexpr std::string_view COERCION = R"(
    export default function(args) {
      const trace = [];
      const source = {toString() {
        trace.push('source');
        if (args.fail === 'source') throw new Error('source-marker');
        return args.src;
      }};
      const id = {toString() {
        trace.push('id');
        if (args.fail === 'id') throw new Error('id-marker');
        return args.id;
      }};
      try {
        const mod = args.fail === 'arity'
          ? globalThis.import_from_src(source)
          : globalThis.import_from_src(source, id);
        return {caught:false, trace:trace.join(','), value:mod.default()};
      } catch (e) {
        return {caught:true, trace:trace.join(','), name:e.name, message:e.message};
      }
    }
  )";
  const auto below = marked_module(49);
  const auto blocked = marked_module(50);
  std::string malformed{"// original bytes: "};
  malformed += std::string{"\xC0\x80", 2}; // explicit overlong UTF-8 encoding
  malformed += "\nexport default function(){return 42;}";
  scratch.stage_extension(
      "modulegate",
      {{"load", LOAD},
       {"coercion", COERCION},
       {"host49", below},
       {"host50", blocked},
       {"badutf8", "export default function(){return 42;}"},
       {"syntax", "export default function() {"},
       {"importthrow", "throw new Error('import-marker');export default "
                       "function(){return 42;}"},
       {"defaultthrow",
        "export default function(){throw new Error('default-marker');}"},
       {"missingdefault", "export const value=42;"},
       {"noncallable", "export default 17;"},
       {"tla", "await 0;export default function(){return 42;}"},
       {"fresh", "export default function(){return 41;}"}});
  REQUIRE(plinth::extensions::create_pool("modulegate"));

  constexpr std::string_view GATE_MESSAGE =
      "unicode-smuggle: <module> contains 50 invisible Unicode characters "
      "(threshold 50)";

  // Both engine-stage counters are causally live before zero-entry assertions.
  const auto clean = observe_module_dispatch(
      "load", module_args(marked_module(0), "nested/clean"), "nested/clean",
      true);
  check_nested_value(clean);
  check_module_entries(clean, 1, 1, true);
  const auto async_default = observe_module_dispatch(
      "load", module_args(marked_module(0, true), "nested/async"),
      "nested/async", true);
  check_nested_value(async_default);
  check_module_entries(async_default, 1, 1, true);

  const auto permitted = observe_module_dispatch(
      "load", module_args(below, "nested/threshold"), "nested/threshold", true);
  check_nested_value(permitted);
  check_module_entries(permitted, 1, 1, true);
  const auto denied =
      observe_module_dispatch("load", module_args(blocked, "nested/threshold"),
                              "nested/threshold", true);
  check_nested_caught(denied, "InternalError", GATE_MESSAGE);
  check_module_entries(denied, 0, 0, true);

  // Replay identical source/id with scanning disabled. This must execute the
  // actual module, rather than satisfy the denial checks through a dead hook.
  plinth::js::set_unicode_scanner_policy(false, 50, false);
  const auto disabled =
      observe_module_dispatch("load", module_args(blocked, "nested/threshold"),
                              "nested/threshold", true);
  check_nested_value(disabled);
  check_module_entries(disabled, 1, 1, true);
  plinth::js::set_unicode_scanner_policy(true, 50, false);
  const auto muted =
      observe_module_dispatch("load", module_args(blocked, "nested/threshold"),
                              "nested/threshold", true);
  check_nested_caught(muted, "InternalError", GATE_MESSAGE);
  check_module_entries(muted, 0, 0, true);
  plinth::js::set_unicode_scanner_policy(true, 50, true);
  const auto logging =
      observe_module_dispatch("load", module_args(blocked, "nested/threshold"),
                              "nested/threshold", true);
  check_nested_caught(logging, "InternalError", GATE_MESSAGE);
  check_module_entries(logging, 0, 0, true);
  plinth::js::set_unicode_scanner_policy(true, 50, false);

  std::string invalid_noisy{"// "};
  for (int index = 0; index < 50; ++index) {
    invalid_noisy += "\xEF\xB8\x8F";
  }
  invalid_noisy += "\nexport default function( {";
  const auto gate_before_syntax = observe_module_dispatch(
      "load", module_args(invalid_noisy, "nested/threshold"),
      "nested/threshold", true);
  check_nested_caught(gate_before_syntax, "InternalError", GATE_MESSAGE);
  check_module_entries(gate_before_syntax, 0, 0, true);
  const auto ordinary_syntax = observe_module_dispatch(
      "load", module_args("export default function( {", "nested/syntax"),
      "nested/syntax", true);
  check_nested_caught(ordinary_syntax, "SyntaxError", "");
  check_module_entries(ordinary_syntax, 1, 0, true);

  // The parser's acceptance of NUL is not the oracle. Findings after that byte
  // must be rejected before either engine call, using the full source length.
  std::string after_nul{"export default function(){return 42;}"};
  after_nul.push_back('\0');
  after_nul += invalid_noisy;
  const auto nul = observe_module_dispatch(
      "load", module_args(after_nul, "nested/threshold"), "nested/threshold",
      true);
  check_nested_caught(nul, "InternalError", GATE_MESSAGE);
  check_module_entries(nul, 0, 0, true);

  const auto uncaught = observe_module_dispatch(
      "load", module_args(blocked, "nested/threshold", false),
      "nested/threshold", true);
  check_module_error(uncaught, "cap.handler_threw", GATE_MESSAGE);
  if (!uncaught.result) {
    CHECK(uncaught.result.error().message.find("promise_rejected:") !=
          std::string::npos);
    CHECK(uncaught.result.error().message.find("unicode_smuggle_detected:") ==
          std::string::npos);
  }
  check_module_entries(uncaught, 0, 0, true);

  const auto host_below =
      observe_module_dispatch("host49", Json::Value{}, "modulegate/host49");
  REQUIRE(host_below.result.has_value());
  CHECK(host_below.result->type() == Json::intValue);
  CHECK(host_below.result->asInt() == 42);
  check_module_entries(host_below, 1, 1);
  plinth::js::set_unicode_scanner_policy(false, 50, false);
  const auto host_disabled =
      observe_module_dispatch("host50", Json::Value{}, "modulegate/host50");
  REQUIRE(host_disabled.result.has_value());
  CHECK(host_disabled.result->type() == Json::intValue);
  CHECK(host_disabled.result->asInt() == 42);
  check_module_entries(host_disabled, 1, 1);
  plinth::js::set_unicode_scanner_policy(true, 50, false);
  const auto host_denied =
      observe_module_dispatch("host50", Json::Value{}, "modulegate/host50");
  check_module_error(host_denied, "cap.handler_load_failed", GATE_MESSAGE);
  if (!host_denied.result) {
    CHECK(host_denied.result.error().message.find(
              "unicode_smuggle_detected:") != std::string::npos);
  }
  check_module_entries(host_denied, 0, 0);
  const auto valid_original =
      observe_module_dispatch("badutf8", Json::Value{}, "modulegate/badutf8");
  REQUIRE(valid_original.result.has_value());
  CHECK(valid_original.result->type() == Json::intValue);
  CHECK(valid_original.result->asInt() == 42);
  check_module_entries(valid_original, 1, 1);
  scratch.stage_extension("modulegate", {{"badutf8", malformed}});
  const auto original_bytes =
      observe_module_dispatch("badutf8", Json::Value{}, "modulegate/badutf8");
  check_module_error(original_bytes, "cap.handler_load_failed",
                     "unicode-smuggle: <module> failed UTF-8 decode");
  check_module_entries(original_bytes, 0, 0);

  seed_tier2("modulegate:1:host50", "modulegate");
  {
    ModuleEngineWatch watch{"modulegate/host50", ""};
    std::string code;
    std::string message;
    const auto resolved =
        run_resolver("modulegate:1:host50", Json::Value{}, &code, &message);
    CHECK_FALSE(resolved.has_value());
    if (!resolved) {
      CHECK(resolved.error() ==
            plinth::capabilities::CapabilityError::EXTENSION_DISPATCH_FAILED);
      CHECK(code == "cap.handler_load_failed");
      CHECK(message.find("unicode_smuggle_detected:") != std::string::npos);
      CHECK(message.find(GATE_MESSAGE) != std::string::npos);
    }
    CHECK(watch.counts().compiled == 0);
    CHECK(watch.counts().evaluated == 0);
  }

  struct CoercionCase {
    std::string_view fail;
    std::string_view id;
    std::string_view trace;
    std::string_view name;
    std::string_view message;
    bool noisy;
  };
  constexpr std::array<CoercionCase, 5> COERCIONS{{
      {"", "nested/coercion", "source,id", "", "", false},
      {"source", "nested/coercion", "source", "Error", "source-marker", true},
      {"id", "nested/coercion", "source,id", "Error", "id-marker", true},
      {"arity", "nested/coercion", "", "TypeError", "expected (src, id) args",
       false},
      {"", "nested/coercion", "source,id", "InternalError", GATE_MESSAGE, true},
  }};
  for (const auto& item : COERCIONS) {
    INFO("coercion id=" << item.id << " fail=" << item.fail);
    auto args = module_args(item.noisy ? blocked : marked_module(0),
                            std::string{item.id});
    args["fail"] = std::string{item.fail};
    const auto observed =
        observe_module_dispatch("coercion", args, std::string{item.id}, true);
    REQUIRE(observed.result.has_value());
    const auto& value = *observed.result;
    REQUIRE(value.isObject());
    CHECK(value["trace"].type() == Json::stringValue);
    CHECK(value["trace"].asString() == item.trace);
    CHECK(value["caught"].type() == Json::booleanValue);
    CHECK(value["caught"].asBool() == !item.name.empty());
    if (item.name.empty()) {
      CHECK(value["value"].type() == Json::intValue);
      CHECK(value["value"].asInt() == 42);
      check_module_entries(observed, 1, 1, true);
    } else {
      CHECK(value["name"].asString() == item.name);
      CHECK(value["message"].asString().find(item.message) !=
            std::string::npos);
      check_module_entries(observed, 0, 0, true);
    }
  }

  const auto import_throw = observe_module_dispatch(
      "load",
      module_args("globalThis.module_body=1;"
                  "throw new Error('nested-import-marker');"
                  "export default function(){return 42;}",
                  "nested/import-throw"),
      "nested/import-throw", true);
  check_nested_caught(import_throw, "Error", "nested-import-marker", 1);
  check_module_entries(import_throw, 1, 1, true);
  const auto default_throw = observe_module_dispatch(
      "load",
      module_args("globalThis.module_body=1;export default function(){"
                  "globalThis.module_called=1;"
                  "throw new Error('nested-default-marker');}",
                  "nested/default-throw"),
      "nested/default-throw", true);
  check_nested_caught(default_throw, "Error", "nested-default-marker", 1, 1);
  check_module_entries(default_throw, 1, 1, true);
  const auto uncaught_import = observe_module_dispatch(
      "load",
      module_args("throw new Error('uncaught-import-marker');"
                  "export default function(){return 42;}",
                  "nested/uncaught-import", false),
      "nested/uncaught-import", true);
  check_module_error(uncaught_import, "cap.handler_threw",
                     "uncaught-import-marker");
  check_module_entries(uncaught_import, 1, 1, true);
  const auto uncaught_default = observe_module_dispatch(
      "load",
      module_args("export default function(){"
                  "throw new Error('uncaught-default-marker');}",
                  "nested/uncaught-default", false),
      "nested/uncaught-default", true);
  check_module_error(uncaught_default, "cap.handler_threw",
                     "uncaught-default-marker");
  check_module_entries(uncaught_default, 1, 1, true);

  struct HostErrorCase {
    std::string_view function;
    std::string_view code;
    std::string_view marker;
    int evaluated;
  };
  constexpr std::array<HostErrorCase, 6> HOST_ERRORS{{
      {"syntax", "cap.handler_load_failed", "SyntaxError", 0},
      {"importthrow", "cap.handler_load_failed", "import-marker", 1},
      {"defaultthrow", "cap.handler_threw", "default-marker", 1},
      {"missingdefault", "cap.handler_threw", "", 1},
      {"noncallable", "cap.handler_threw", "", 1},
      {"tla", "cap.handler_load_failed", "top-level await (not supported)", 1},
  }};
  for (const auto& item : HOST_ERRORS) {
    INFO("host module=" << item.function);
    const auto observed =
        observe_module_dispatch(item.function, Json::Value{},
                                "modulegate/" + std::string{item.function});
    check_module_error(observed, item.code, item.marker);
    check_module_entries(observed, 1, item.evaluated);
  }

  const auto first_fresh =
      observe_module_dispatch("fresh", Json::Value{}, "modulegate/fresh");
  REQUIRE(first_fresh.result.has_value());
  CHECK(first_fresh.result->type() == Json::intValue);
  CHECK(first_fresh.result->asInt() == 41);
  check_module_entries(first_fresh, 1, 1);
  scratch.stage_extension("modulegate",
                          {{"fresh", "export default function(){return 42;}"}});
  const auto second_fresh =
      observe_module_dispatch("fresh", Json::Value{}, "modulegate/fresh");
  REQUIRE(second_fresh.result.has_value());
  CHECK(second_fresh.result->type() == Json::intValue);
  CHECK(second_fresh.result->asInt() == 42);
  check_module_entries(second_fresh, 1, 1);

  REQUIRE(plinth::extensions::shutdown_registry(5s));
  REQUIRE(plinth::extensions::inflight_dispatch_count_for_test() == 0);
  REQUIRE(plinth::extensions::shutdown_registry(0ms));
}

// ─── Group R — Resolve (happy paths) ─────────────────────────────

TEST_CASE("R.01: cap_call_extdispatch_echo_happy_path",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"echo", R"(
            export default function echo(args) {
              return { x: args.x, doubled: args.x * 2 };
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:echo", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  Json::Value args(Json::objectValue);
  args["x"] = 42;
  auto r = run_resolver("extdispatch:1:echo", args);
  REQUIRE(r.has_value());
  REQUIRE(r->data["x"].asInt() == 42);
  REQUIRE(r->data["doubled"].asInt() == 84);
  REQUIRE(r->resolved_tier == "tier2");
  REQUIRE(r->provider_type == "extension");
}

TEST_CASE("R.02: resolver task owns temporary inputs across suspension",
          "[cap][res][ext][integration][lifetime]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"slow_identity", R"(
            export default async function slow_identity(args, ctx) {
              await db.query('SELECT pg_sleep(0.05)');
              return {
                marker: args.marker,
                username: ctx.user.username,
              };
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:slow_identity", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  Json::Value args(Json::objectValue);
  args["marker"] = "owned-call";

  // Task has an initial suspend: both arguments are destroyed at the end of
  // this statement, before the resolver starts. The extension handler then
  // suspends again on pg_sleep, exercising frame ownership across the full
  // asynchronous dispatch rather than only an eager success path.
  auto pending = plinth::capabilities::call_capability_async(
      plinth::capabilities::CapabilityCall{
          .signature = "extdispatch:1:slow_identity",
          .args = std::move(args),
          .call_depth = 0,
      },
      plinth::capabilities::UserContext{
          .user_id = "u-temporary",
          .username = "temporary-user",
          .auth_type = "session",
          .effective_rules = {"kernel.admin"},
          .session_id = "session-temporary",
          .ip_address = "127.0.0.1",
      });

  auto result = drogon::sync_wait(pending);
  REQUIRE(result.has_value());
  REQUIRE(result->data["marker"].asString() == "owned-call");
  REQUIRE(result->data["username"].asString() == "temporary-user");
}

// ─── Group E — Errors ────────────────────────────────────────────

TEST_CASE("E.01: handler_throw_maps_to_cap_handler_threw",
          "[cap][res][ext][integration]") {
  // Emits a `capability.extension.error` audit row; requires a
  // DbClient attached to drogon::app(). Gate on PG so the
  // DbClient path doesn't assert inside Drogon's debug
  // `DbClientManager::getDbClient` when the client is absent.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"thrower", R"(
            export default function thrower() {
              throw new Error("boom");
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:thrower", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:thrower", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() ==
          plinth::capabilities::CapabilityError::EXTENSION_DISPATCH_FAILED);
  REQUIRE(code == "cap.handler_threw");
  REQUIRE(msg.find("boom") != std::string::npos);
  require_persisted_audit("capability.extension.error", admin_ctx().user_id,
                          "cap.handler_threw", "extdispatch", "thrower");
}

TEST_CASE("E.02: unknown_signature_rejects_at_resolver",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  // No cache seed, no pool — bare miss at the resolver.
  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:ghost", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() ==
          plinth::capabilities::CapabilityError::CAPABILITY_NOT_FOUND);
  REQUIRE(code.empty()); // never reaches the extension dispatcher
}

TEST_CASE("E.03: missing_handler_file_rejects_cap_handler_not_found",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"echo", R"(export default function echo() { return {}; })"},
      });
  seed_tier2("extdispatch:1:gone", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  // The capability is cached; the handler file is not on disk.
  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:gone", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(code == "cap.handler_not_found");
  // ICD §Handler invocation step 2 — do not leak the fs path.
  REQUIRE(msg.find("/tmp") == std::string::npos);
  REQUIRE(msg.find("data/extensions") == std::string::npos);
  REQUIRE(msg.find("extdispatch:gone") != std::string::npos);
}

TEST_CASE("E.04: handler_syntax_error_rejects_cap_handler_load_failed",
          "[cap][res][ext][integration]") {
  // `cap.handler_load_failed` also emits a
  // `capability.extension.error` audit row (ICD §Audit); gate on PG
  // for the same reason as E.01.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"broken", "export default function broken( { return 1; }"},
      });
  seed_tier2("extdispatch:1:broken", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:broken", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(code == "cap.handler_load_failed");
  require_persisted_audit("capability.extension.error", admin_ctx().user_id,
                          "cap.handler_load_failed", "extdispatch", "broken");
}

TEST_CASE("E.05: destroyed_pool_rejects_extension_not_loaded",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"echo", R"(export default function echo() { return {}; })"},
      });
  seed_tier2("extdispatch:1:echo", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));
  plinth::extensions::destroy_pool("extdispatch");

  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:echo", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(code == "cap.extension_not_loaded");
}

TEST_CASE(
    "E.06: sync_call_capability_on_extension_entry_returns_async_required",
    "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  // Only the cache seed — no on-disk handler needed; the sync path
  // rejects before reaching `dispatch_tier2`'s extension arm.
  seed_tier2("extdispatch:1:echo", "extdispatch");

  plinth::capabilities::CapabilityCall call{
      .signature = "extdispatch:1:echo",
      .args = Json::Value{},
      .call_depth = 0,
  };
  auto r = plinth::capabilities::call_capability(call, admin_ctx());
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == plinth::capabilities::CapabilityError::ASYNC_REQUIRED);
}

// ─── Group P — Propagation + identity ─────────────────────────────

TEST_CASE("P.02: extension_handler_cannot_spoof_other_extension_publish",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  // Handler attempts to publish on another extension's channel
  // prefix. The pubsub binding's identity gate rejects.
  s.stage_extension("extdispatch", {
                                       {"spoof", R"(
            export default async function spoof() {
              try {
                await pubsub.publish("plinth:ext:other:test", {hi: 1});
                return { ok: true };
              } catch (e) {
                return { caught: e.code || "<no-code>" };
              }
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:spoof", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  auto r = run_resolver("extdispatch:1:spoof", Json::Value{});
  REQUIRE(r.has_value());
  // ICD §Security Constraint 3 — `bc.extension_name` is set by the
  // pool (the callee's own name); publishing on another extension's
  // channel hits the identity gate at
  // `pubsub_bindings.cpp:154-167` and rejects with
  // `pubsub.extension_mismatch`.
  REQUIRE(r->data["caught"].asString() == "pubsub.extension_mismatch");
}

TEST_CASE("P.03: rbac_enforced_at_resolver_not_callee",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"echo",
           R"(export default function echo() { return { ok: true }; })"},
      });
  // Seed with a specific rule that our non-admin context lacks.
  seed_tier2("extdispatch:1:echo", "extdispatch", "extdispatch.call");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  // Non-admin, no matching rules → resolver denies BEFORE dispatch.
  plinth::capabilities::UserContext no_rules{
      .user_id = "22222222-2222-2222-2222-222222222222",
      .username = "nobody",
      .auth_type = "session",
      .effective_rules = {},
      .session_id = {},
      .ip_address = {},
  };
  plinth::capabilities::CapabilityCall call{
      .signature = "extdispatch:1:echo",
      .args = Json::Value{},
      .call_depth = 0,
  };
  auto r = drogon::sync_wait(
      plinth::capabilities::call_capability_async(call, no_rules));
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() ==
          plinth::capabilities::CapabilityError::PERMISSION_DENIED);
  require_persisted_audit("capability.rbac.denied", no_rules.user_id);
}

TEST_CASE("P.04: handler_receives_ctx_with_caller_identity_and_audit_frame",
          "[cap][res][ext][integration]") {
  // 0.6.3.N — proves the runtime_registry wrapper injects a second
  // positional `ctx` carrying the audit-frame projection of the
  // caller's UserContext. Pre-fix, every shell handler signature
  // `(args, ctx)` saw `ctx === undefined` and `ctx.user.id` threw
  // TypeError. The handler under test reads each field the
  // wrapper now binds and returns them so the assertion side
  // verifies the wire end-to-end.
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"echo_ctx", R"(
            export default function echo_ctx(_args, ctx) {
              return {
                user_id:   ctx.user.id,
                username:  ctx.user.username,
                auth_type: ctx.user.auth_type,
                session_id: ctx.session_id,
                ip_address: ctx.ip_address,
                call_depth: ctx.call_depth,
                extension:  ctx.extension,
                rules_present: 'effective_rules' in ctx.user,
              };
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:echo_ctx", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  plinth::capabilities::UserContext caller{
      .user_id = "u-p04",
      .username = "alice",
      .auth_type = "session",
      .effective_rules = {"kernel.admin"}, // universal match for resolver
      .session_id = "sess-abc",
      .ip_address = "10.0.0.7",
  };
  plinth::capabilities::CapabilityCall call{
      .signature = "extdispatch:1:echo_ctx",
      .args = Json::Value{},
      .call_depth = 0,
  };
  auto r = drogon::sync_wait(
      plinth::capabilities::call_capability_async(call, caller));
  REQUIRE(r.has_value());
  REQUIRE(r->data["user_id"].asString() == "u-p04");
  REQUIRE(r->data["username"].asString() == "alice");
  REQUIRE(r->data["auth_type"].asString() == "session");
  REQUIRE(r->data["session_id"].asString() == "sess-abc");
  REQUIRE(r->data["ip_address"].asString() == "10.0.0.7");
  // `bc.call_depth` is set to caller_call_depth + 1 at dispatch entry
  // (runtime_registry.cpp:718) so the handler observes the depth of
  // its own frame, not the caller's.
  REQUIRE(r->data["call_depth"].asInt() == 1);
  REQUIRE(r->data["extension"].asString() == "extdispatch");
  // ctx.user MUST NOT carry effective_rules — handlers should not
  // self-introspect RBAC; resolver step 3 owns the gate.
  REQUIRE(r->data["rules_present"].asBool() == false);
}

// ─── Group H — Hot reload / lifecycle ─────────────────────────────

TEST_CASE("H.01: destroy_pool_followed_by_create_pool_cycles",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"ping",
           R"(export default function ping() { return { ok: true }; })"},
      });
  seed_tier2("extdispatch:1:ping", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  auto r1 = run_resolver("extdispatch:1:ping", Json::Value{});
  REQUIRE(r1.has_value());
  REQUIRE(r1->data["ok"].asBool());

  plinth::extensions::destroy_pool("extdispatch");
  std::string code1;
  auto r2 = run_resolver("extdispatch:1:ping", Json::Value{}, &code1);
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(code1 == "cap.extension_not_loaded");

  REQUIRE(plinth::extensions::create_pool("extdispatch"));
  auto r3 = run_resolver("extdispatch:1:ping", Json::Value{});
  REQUIRE(r3.has_value());
  REQUIRE(r3->data["ok"].asBool());
}

TEST_CASE("H.02: shutdown rejects new work and drains accepted dispatches",
          "[cap][res][ext][integration][lifecycle]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"slow", R"(
            export default async function slow() {
              await db.query('SELECT pg_sleep(0.2)');
              return { ok: true };
            }
        )"},
                                   });
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  auto dispatch = std::async(std::launch::async, [] {
    return run_dispatch("extdispatch", "slow", Json::Value{});
  });
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (plinth::extensions::inflight_dispatch_count_for_test() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(plinth::extensions::inflight_dispatch_count_for_test() == 1);

  REQUIRE_FALSE(plinth::extensions::shutdown_registry(0ms));
  auto rejected = run_dispatch("extdispatch", "slow", Json::Value{});
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().code == "cap.extension_not_loaded");

  REQUIRE(dispatch.wait_for(2s) == std::future_status::ready);
  auto result = dispatch.get();
  REQUIRE(result.has_value());
  REQUIRE((*result)["ok"].asBool());
  REQUIRE(plinth::extensions::shutdown_registry(1s));
}

TEST_CASE("H.03: final active lease retires a removed pool on the owner loop",
          "[cap][res][ext][integration][lifecycle]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  const auto conninfo =
      "host=" + s.cfg.db.host + " port=" + std::to_string(s.cfg.db.port) +
      " dbname=" + s.cfg.db.database + " user=" + s.cfg.db.user +
      " password=" + s.cfg.db.password;
  std::unique_ptr<PGconn, decltype(&PQfinish)> control{
      PQconnectdb(conninfo.c_str()), &PQfinish};
  REQUIRE(PQstatus(control.get()) == CONNECTION_OK);
  const auto lock_key = 950'000'000LL + static_cast<long long>(::getpid());
  const auto lock_sql =
      "SELECT pg_advisory_lock(" + std::to_string(lock_key) + ")";
  const auto unlock_sql =
      "SELECT pg_advisory_unlock(" + std::to_string(lock_key) + ")";
  {
    std::unique_ptr<PGresult, decltype(&PQclear)> locked{
        PQexec(control.get(), lock_sql.c_str()), &PQclear};
    REQUIRE(PQresultStatus(locked.get()) == PGRES_TUPLES_OK);
  }
  const auto handler = std::string{R"(
            export default async function slow() {
              await db.query('SELECT pg_advisory_xact_lock()"} +
                       std::to_string(lock_key) +
                       R"()');
              return { ok: true };
            }
        )";
  s.stage_extension("extdispatch", {
                                       {"slow", handler},
                                   });
  REQUIRE(plinth::extensions::create_pool("extdispatch"));
  const auto handoffs_before =
      plinth::extensions::owner_loop_handoff_count_for_test();

  auto dispatch = std::async(std::launch::async, [] {
    return run_dispatch("extdispatch", "slow", Json::Value{});
  });
  struct UnlockGuard {
    PGconn* connection;
    const std::string* sql;
    bool armed = true;
    ~UnlockGuard() {
      if (armed) {
        PQclear(PQexec(connection, sql->c_str()));
      }
    }
  } unlock_guard{control.get(), &unlock_sql};
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (plinth::extensions::inflight_dispatch_count_for_test() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(plinth::extensions::inflight_dispatch_count_for_test() == 1);
  const auto query_marker =
      "pg_advisory_xact_lock(" + std::to_string(lock_key) + ")";
  deadline = std::chrono::steady_clock::now() + 2s;
  while (!query_is_active(control.get(), query_marker) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(query_is_active(control.get(), query_marker));

  plinth::extensions::destroy_pool("extdispatch");
  auto rejected = run_dispatch("extdispatch", "slow", Json::Value{});
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().code == "cap.extension_not_loaded");

  s.stage_extension("extdispatch", {
                                       {"generation", R"(
            export default function generation() {
              return { generation: "replacement" };
            }
        )"},
                                   });
  REQUIRE(plinth::extensions::create_pool("extdispatch"));
  auto replacement = run_dispatch("extdispatch", "generation", Json::Value{});
  REQUIRE(replacement.has_value());
  REQUIRE((*replacement)["generation"].asString() == "replacement");

  {
    std::unique_ptr<PGresult, decltype(&PQclear)> unlocked{
        PQexec(control.get(), unlock_sql.c_str()), &PQclear};
    REQUIRE(PQresultStatus(unlocked.get()) == PGRES_TUPLES_OK);
  }
  unlock_guard.armed = false;

  REQUIRE(dispatch.wait_for(2s) == std::future_status::ready);
  auto result = dispatch.get();
  REQUIRE(result.has_value());
  REQUIRE((*result)["ok"].asBool());
  deadline = std::chrono::steady_clock::now() + 2s;
  while ((plinth::extensions::inflight_dispatch_count_for_test() != 0 ||
          plinth::extensions::owner_loop_handoff_count_for_test() ==
              handoffs_before) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(plinth::extensions::inflight_dispatch_count_for_test() == 0);
  REQUIRE(plinth::extensions::owner_loop_handoff_count_for_test() >
          handoffs_before);
  REQUIRE(plinth::extensions::failed_pool_count_for_test() == 0);
  replacement = run_dispatch("extdispatch", "generation", Json::Value{});
  REQUIRE(replacement.has_value());
  REQUIRE((*replacement)["generation"].asString() == "replacement");
}
