// SPDX-License-Identifier: MIT

#include "native_capability_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <json/value.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fixture = native_capability_corpus::fixture;
using plinth::js::EvalErrorKind;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum class Control { PRIMARY, RAW, HANDLER_WRAPPER, DEPTH_SEVEN };

struct Identity {
  unsigned family;
  unsigned seed;
  unsigned replay;
  bool batch;
  Control control = Control::PRIMARY;
};

struct Cell {
  Identity identity;
  std::string extension;
  fixture::Execution execution;
  fixture::Shutdown shutdown;
  unsigned target_entries;
  unsigned peer_entries;
  unsigned total_entries;
  bool provider_closed;
};

// The native envelope is observed, not replaced by a test-authored exception.
// Own-key membership and ordinary-object identity are independent observations.
constexpr std::string_view ERROR_OBSERVER = R"JS(
function observeError(error) {
  const own=key=>Object.prototype.hasOwnProperty.call(error,key);
  return {code:error.code,message:error.message,
    keys:Object.getOwnPropertyNames(error).sort(),
    plain:Object.getPrototypeOf(error)===Object.prototype,
    isError:error instanceof Error,typeError:error instanceof TypeError,
    sqlstate:own('sqlstate'),kind:own('kind')};
}
)JS";

auto context_validator(std::string_view extension, unsigned depth)
    -> std::string {
  return R"JS(
function checkContext(ctx) {
  const names=Object.getOwnPropertyNames(ctx).sort();
  const userNames=Object.getOwnPropertyNames(ctx.user).sort();
  if(names.length!==5||names[0]!=='call_depth'||names[1]!=='extension'||
     names[2]!=='ip_address'||names[3]!=='session_id'||names[4]!=='user'||
     userNames.length!==3||userNames[0]!=='auth_type'||userNames[1]!=='id'||
     userNames[2]!=='username'||
     ctx.user.id!=='11111111-1111-1111-1111-111111111187'||
     ctx.user.username!=='native-capability-fixture'||ctx.user.auth_type!=='session'||
     ctx.session_id!==''||ctx.ip_address!==''||ctx.extension!==')JS" +
         std::string{extension} +
         "'||ctx.call_depth!==" + std::to_string(depth) + R"JS()
    throw new Error('independent capability context validation');
}
)JS";
}

auto invocation(bool batch, std::string_view signature, std::string_view args)
    -> std::string {
  return batch ? "cap.batch([['" + std::string{signature} + "'," +
                     std::string{args} + "],['capnative:1:peer',null]])"
               : "cap.call('" + std::string{signature} + "'," +
                     std::string{args} + ")";
}

auto caught_program(const std::string& operation, bool batch,
                    std::string_view success) -> std::string {
  return "(async()=>{" + std::string{ERROR_OBSERVER} +
         "let returned=false;try{const pending=" + operation +
         ";returned=true;const result=await pending;" +
         (batch ? "if(!Array.isArray(result)||result.length!==2||"
                  "Object.getOwnPropertyNames(result).length!==3||"
                  "!Object.prototype.hasOwnProperty.call(result,'0')||"
                  "!Object.prototype.hasOwnProperty.call(result,'1'))"
                  "throw new Error('native capability edge batch shape');"
                : "") +
         "const target=" + (batch ? "result[0]" : "result") + ";" +
         std::string{success} +
         "}catch(error){return "
         "{ok:false,sync:!returned,error:observeError(error)};}})()";
}

// Authored independently from the C++ byte-count/value oracle below. Handler
// returns a bare string; context/receipt metadata never changes its wire size.
auto bytes_argument(unsigned seed) -> std::string_view {
  static constexpr std::array<std::string_view, 4> ARGUMENTS{
      "'A'.repeat(60)", "'A'.repeat(61)", "'A'.repeat(62)",
      "'A'.repeat(59)+'\"'"};
  return ARGUMENTS.at(seed);
}

auto bytes_handler(const fixture::Provider& provider, unsigned seed)
    -> std::string {
  static constexpr std::array<std::string_view, 4> CHECKS{
      "args.length===60", "args.length===61", "args.length===62",
      "args.length===60&&args.charCodeAt(59)===34"};
  static constexpr std::array<std::string_view, 4> RETURNS{
      "'A'.repeat(60)", "'A'.repeat(61)", "'A'.repeat(62)",
      "'A'.repeat(59)+'\"'"};
  return context_validator(provider.extension, 1) +
         "export default async function target(args,ctx){" +
         provider.entry_statement(1) + "checkContext(ctx);" +
         "if(typeof args!=='string'||!(" + std::string{CHECKS.at(seed)} +
         "))throw new Error('independent R08 input length');"
         "for(let i=0;i<args.length;i++){if(args.charCodeAt(i)!==" +
         (seed == 3 ? "(i===59?34:65)" : "65") +
         ")throw new Error('independent R08 input units');}return " +
         std::string{RETURNS.at(seed)} + ";}";
}

// Input construction, handler validation/reconstruction, caller traversal and
// native-result traversal are separate programs. None calls a production
// converter/serializer to manufacture its expected value.
auto depth_argument(unsigned depth) -> std::string {
  return "Array.from({length:" + std::to_string(depth) +
         "}).reduce(value=>({n:value}),7)";
}

auto depth_handler(const fixture::Provider& provider, unsigned input_depth,
                   unsigned output_depth, bool zero_input, bool wrapper)
    -> std::string {
  return context_validator(provider.extension, 1) +
         "export default async function target(args,ctx){" +
         provider.entry_statement(1) + "checkContext(ctx);" +
         (zero_input
              ? "if(args!==0)throw new Error('independent R09 zero input');"
              : "let node=args;for(let edge=0;edge<" +
                    std::to_string(input_depth) +
                    ";edge++){if(node===null||typeof node!=='object'||"
                    "Array.isArray(node)||Object.getOwnPropertyNames(node)."
                    "length!==1||"
                    "!Object.prototype.hasOwnProperty.call(node,'n'))"
                    "throw new Error('independent R09 input "
                    "edge');node=node.n;}"
                    "if(typeof node!=='number'||node!==7)"
                    "throw new Error('independent R09 input leaf');") +
         "let reply=7;let remaining=" + std::to_string(output_depth) +
         ";while(remaining>0){reply={n:reply};--remaining;}return " +
         (wrapper ? "{value:reply}" : "reply") + ";}";
}

constexpr std::string_view DEPTH_OBSERVER = R"JS(
let node=target;let depth=0;let valid=true;
while(node!==null&&typeof node==='object'&&depth<67){
  const names=Object.getOwnPropertyNames(node);
  if(Array.isArray(node)||names.length!==1||names[0]!=='n'){valid=false;break;}
  node=node.n;++depth;
}
return {ok:true,valid,depth,leaf:node,peer:PEER};
)JS";

auto depth_success(bool batch) -> std::string {
  auto program = std::string{DEPTH_OBSERVER};
  const auto offset = program.find("PEER");
  fixture::demand(offset != std::string::npos,
                  "depth observer source identity");
  program.replace(offset, 4, batch ? "result[1]" : "null");
  return program;
}

auto error_function(unsigned family, unsigned seed) -> std::string {
  static constexpr std::array<std::string_view, 8> NAMES{
      "invalid",  "absent",  "deny",   "depth",
      "disabled", "missing", "syntax", "failure"};
  return std::string{NAMES.at(family)} + std::to_string(seed);
}

auto error_signature(unsigned family, unsigned seed) -> std::string {
  if (family == 0) {
    return seed == 0 ? "bad_signature" : "capnative:1:echo:extra";
  }
  return "capnative:1:" + error_function(family, seed);
}

auto stage_error(fixture::Provider& provider, unsigned family, unsigned seed)
    -> void {
  const auto function = error_function(family, seed);
  if (family <= 1) return;
  provider.seed(function, family == 2 ? "capnative.denied" : "capnative.call",
                family != 4);
  if (family == 5) return; // Cache/pool exist; the exact handler file does not.
  if (family == 6) {
    provider.stage(function, seed == 0 ? "export default function broken( {"
                                       : "export default ;");
    return;
  }
  if (family == 7) {
    const auto record =
        "async function entered(){" + provider.entry_statement(1) + "}";
    provider.stage(
        function,
        record +
            (seed == 0
                 ? "export default function failure(){void entered();"
                   "throw new Error('owned-E07-throw');}"
                 : "export default async function failure(){await entered();"
                   "return Promise.reject(new Error('owned-E07-reject'));}"));
    return;
  }
  provider.stage(function, "export default async function target(){" +
                               provider.entry_statement(1) + "return 7;}");
}

auto collect(const Identity& identity) -> Cell {
  fixture::Provider provider;
  provider.stage("peer",
                 "export default async function peer(){" +
                     provider.entry_statement(2) +
                     (identity.family == 8 ? "return 'p';}" : "return 7;}"));
  provider.seed("peer");
  auto runtime_limits = fixture::limits();
  unsigned caller_depth = 0;
  std::string source;
  if (identity.control == Control::DEPTH_SEVEN) {
    caller_depth = 7;
    provider.seed("target");
    provider.stage("target",
                   context_validator(provider.extension, 8) +
                       "export default async function target(args,ctx){" +
                       provider.entry_statement(1) +
                       "checkContext(ctx);return {depth:ctx.call_depth};}");
    source = caught_program(
        invocation(identity.batch, "capnative:1:target", "null"),
        identity.batch,
        "return {ok:true,depth:target.depth,peer:" +
            std::string{identity.batch ? "result[1]" : "null"} + "};");
  } else if (identity.family == 8) {
    runtime_limits.async_result_size_limit_bytes = 64;
    provider.seed("target");
    provider.stage("target", bytes_handler(provider, identity.seed));
    source = caught_program(
        invocation(identity.batch, "capnative:1:target",
                   bytes_argument(identity.seed)),
        identity.batch,
        "return {ok:true,value:target,peer:" +
            std::string{identity.batch ? "result[1]" : "null"} + "};");
  } else if (identity.family == 9) {
    const bool wrapper = identity.control == Control::HANDLER_WRAPPER;
    const bool zero_input = wrapper || identity.seed == 3;
    const unsigned depth = identity.seed == 0   ? 63U
                           : identity.seed == 1 ? 64U
                                                : 65U;
    provider.seed("target");
    provider.stage("target",
                   depth_handler(provider, depth, wrapper ? 64U : depth,
                                 zero_input, wrapper));
    const auto operation = invocation(identity.batch, "capnative:1:target",
                                      zero_input ? "0" : depth_argument(depth));
    source = identity.control == Control::RAW
                 ? "(async()=>{return await " + operation + ";})()"
                 : caught_program(operation, identity.batch,
                                  depth_success(identity.batch));
  } else {
    const unsigned family = identity.family - 10;
    stage_error(provider, family, identity.seed);
    caller_depth = family == 3 ? 8 + identity.seed : 0;
    source = caught_program(invocation(identity.batch,
                                       error_signature(family, identity.seed),
                                       "null"),
                            identity.batch, "return {ok:true};");
  }
  fixture::Caller caller(provider.config, fixture::user(), runtime_limits);
  auto execution = caller.run(source, static_cast<int>(caller_depth));
  const auto target_entries = provider.receipt_count(1);
  const auto peer_entries = provider.receipt_count(2);
  const auto total_entries = provider.total_receipts();
  const auto shutdown = caller.finish();
  const auto extension = provider.extension;
  const bool provider_closed = provider.finish();
  return {identity,       extension,    std::move(execution), shutdown,
          target_entries, peer_entries, total_entries,        provider_closed};
}

auto native_error_shape(const Json::Value& observed, std::string_view code)
    -> bool {
  if (!observed.isObject() || observed.size() != 3 ||
      !observed["ok"].isBool() || observed["ok"].asBool() ||
      !observed["sync"].isBool() || observed["sync"].asBool())
    return false;
  const auto& error = observed["error"];
  return error.isObject() && error.size() == 8 && error["code"].isString() &&
         error["code"].asString() == code && error["message"].isString() &&
         error["keys"].isArray() && error["keys"].size() == 2 &&
         error["keys"][0].isString() && error["keys"][0].asString() == "code" &&
         error["keys"][1].isString() &&
         error["keys"][1].asString() == "message" && error["plain"].isBool() &&
         error["plain"].asBool() && error["isError"].isBool() &&
         !error["isError"].asBool() && error["typeError"].isBool() &&
         !error["typeError"].asBool() && error["sqlstate"].isBool() &&
         !error["sqlstate"].asBool() && error["kind"].isBool() &&
         !error["kind"].asBool();
}

auto exact_native_error(const Json::Value& observed, std::string_view code,
                        const std::string& message) -> bool {
  return native_error_shape(observed, code) &&
         observed["error"]["message"].asString() == message;
}

auto numeric(const Json::Value& value, unsigned expected) -> bool {
  return value.isNumeric() && value.asDouble() == static_cast<double>(expected);
}

auto peer_matches(const Json::Value& value, bool batch, bool bytes = false)
    -> bool {
  if (!batch) return value.isNull();
  return bytes ? value.isString() && value.asString() == "p"
               : numeric(value, 7);
}

auto native_depth(const Json::Value& value, unsigned expected) -> bool {
  const Json::Value* node = &value;
  for (unsigned edge = 0; edge < expected; ++edge) {
    if (!node->isObject() || node->size() != 1 || !node->isMember("n"))
      return false;
    node = &(*node)["n"];
  }
  return numeric(*node, 7);
}

auto semantic_matches(const Cell& cell) -> bool {
  const auto& id = cell.identity;
  const auto& outcome = cell.execution.result.value;
  if (id.control == Control::RAW) {
    if (id.batch && id.seed == 1) {
      return !outcome.has_value() &&
             outcome.error().kind == EvalErrorKind::INTERNAL &&
             outcome.error().message ==
                 "js_to_json: max conversion depth exceeded";
    }
    if (!outcome.has_value()) return false;
    return id.batch ? outcome->isArray() && outcome->size() == 2 &&
                          native_depth((*outcome)[0], 63) &&
                          numeric((*outcome)[1], 7)
                    : native_depth(*outcome, 64);
  }
  if (!outcome.has_value()) return false;
  const auto& actual = *outcome;
  if (id.control == Control::DEPTH_SEVEN) {
    return actual.isObject() && actual.size() == 3 && actual["ok"].isBool() &&
           actual["ok"].asBool() && numeric(actual["depth"], 8) &&
           peer_matches(actual["peer"], id.batch);
  }
  if (id.family == 8) {
    if (id.seed == 2) {
      return exact_native_error(actual, "async.result_size_exceeded",
                                "result size 65 bytes exceeds limit 64 bytes");
    }
    // Literal independent wire sizes are 63/64/65/64; no serializer is used.
    static constexpr std::array<unsigned, 4> LENGTHS{60, 61, 62, 59};
    auto expected = std::string(LENGTHS.at(id.seed), 'A');
    if (id.seed == 3) expected.push_back('"');
    return actual.isObject() && actual.size() == 3 && actual["ok"].isBool() &&
           actual["ok"].asBool() && actual["value"].isString() &&
           actual["value"].asString() == expected &&
           peer_matches(actual["peer"], id.batch, true);
  }
  if (id.family == 9) {
    if (id.control == Control::HANDLER_WRAPPER || id.seed == 3) {
      return exact_native_error(
          actual, "cap.handler_threw",
          cell.extension +
              ":target: internal: js_to_json: max conversion depth exceeded");
    }
    if (id.seed == 2) {
      // Synchronous admission TypeError is not a native rejection envelope.
      return actual.isObject() && actual.size() == 3 && actual["ok"].isBool() &&
             !actual["ok"].asBool() && actual["sync"].isBool() &&
             actual["sync"].asBool() && actual["error"].isObject() &&
             actual["error"]["isError"].isBool() &&
             actual["error"]["isError"].asBool() &&
             actual["error"]["typeError"].isBool() &&
             actual["error"]["typeError"].asBool() &&
             actual["error"]["message"].isString() &&
             actual["error"]["message"].asString() ==
                 "cap.call: args contains unsupported value";
    }
    return actual.isObject() && actual.size() == 5 && actual["ok"].isBool() &&
           actual["ok"].asBool() && actual["valid"].isBool() &&
           actual["valid"].asBool() &&
           numeric(actual["depth"], id.seed == 0 ? 63 : 64) &&
           numeric(actual["leaf"], 7) && peer_matches(actual["peer"], id.batch);
  }
  const unsigned family = id.family - 10;
  static constexpr std::array<std::string_view, 8> CODES{
      "cap.invalid_signature",   "cap.not_found",
      "cap.permission_denied",   "cap.call_depth_exceeded",
      "cap.capability_disabled", "cap.handler_not_found",
      "cap.handler_load_failed", "cap.handler_threw"};
  if (!native_error_shape(actual, CODES.at(family))) return false;
  const auto message = actual["error"]["message"].asString();
  if (family <= 4) {
    return message == std::string{CODES.at(family)} + ": " +
                          error_signature(family, id.seed);
  }
  const auto target = cell.extension + ':' + error_function(family, id.seed);
  if (family == 5) return message == target;
  // Confirmed by the first independently reviewed native execution. Accept
  // ONE classification, not whichever historical kind happens to pass.
  const auto marker = family == 6    ? "SyntaxError"
                      : id.seed == 0 ? "owned-E07-throw"
                                     : "owned-E07-reject";
  return message.starts_with(target + ": promise_rejected:") &&
         message.size() <= 1024 && message.find(marker) != std::string::npos;
}

auto expected_entries(const Identity& id) -> std::pair<unsigned, unsigned> {
  if (id.control != Control::PRIMARY) return {1, id.batch ? 1U : 0U};
  if (id.family == 8) return {1, id.batch ? 1U : 0U};
  if (id.family == 9) {
    return id.seed == 2 ? std::pair{0U, 0U} : std::pair{1U, id.batch ? 1U : 0U};
  }
  return {id.family == 17 ? 1U : 0U, id.batch && id.family != 13 ? 1U : 0U};
}

auto matches(const Cell& cell) -> bool {
  const auto [target, peer] = expected_entries(cell.identity);
  const bool no_admission = cell.identity.family == 9 &&
                            cell.identity.control == Control::PRIMARY &&
                            cell.identity.seed == 2;
  return cell.provider_closed && fixture::clean(cell.execution.cleanup) &&
         cell.execution.admitted == (no_admission          ? 0
                                     : cell.identity.batch ? 2
                                                           : 1) &&
         fixture::stopped(cell.shutdown) && cell.target_entries == target &&
         cell.peer_entries == peer && cell.total_entries == target + peer &&
         semantic_matches(cell);
}

auto inventory_slot(const Identity& id, bool errors)
    -> std::optional<std::size_t> {
  if (id.replay >= 2) return std::nullopt;
  const auto route = id.batch ? 1U : 0U;
  if (errors) {
    if (id.family < 10 || id.family > 17 || id.seed >= 2 ||
        id.control != Control::PRIMARY)
      return std::nullopt;
    return ((id.replay * 8 + id.family - 10) * 2 + id.seed) * 2 + route;
  }
  if (id.control == Control::PRIMARY) {
    if (id.family < 8 || id.family > 9 || id.seed >= 4) return std::nullopt;
    return ((id.replay * 2 + id.family - 8) * 4 + id.seed) * 2 + route;
  }
  const auto base = 32 + id.replay * 7;
  if (id.control == Control::RAW && id.family == 9) {
    if (!id.batch && id.seed == 1) return base;
    if (id.batch && id.seed == 0) return base + 1;
    if (id.batch && id.seed == 1) return base + 2;
  }
  if (id.control == Control::HANDLER_WRAPPER && id.family == 9 && id.seed == 3)
    return base + 3 + route;
  if (id.control == Control::DEPTH_SEVEN && id.family == 13 && id.seed == 0)
    return base + 5 + route;
  return std::nullopt;
}

auto complete_inventory(const std::vector<Cell>& cells, bool errors) -> bool {
  const std::size_t count = errors ? 64 : 46;
  if (cells.size() != count) return false;
  std::array<unsigned, 64> seen{};
  for (const auto& cell : cells) {
    const auto slot = inventory_slot(cell.identity, errors);
    if (!slot || *slot >= count || ++seen[*slot] != 1) return false;
  }
  for (std::size_t slot = 0; slot < count; ++slot) {
    if (seen[slot] != 1) return false;
  }
  return true;
}

struct CopiedControlCounts {
  unsigned prior = 0;
  unsigned admitted = 0;
  unsigned native_code = 0;
  unsigned native_message = 0;
  unsigned exact_message = 0;
  unsigned bounded_prefix = 0;
  unsigned bounded_marker = 0;
  unsigned raw_kind = 0;
  unsigned raw_message = 0;
  unsigned synchronous_identity = 0;
  unsigned synchronous_message = 0;
  unsigned success_value = 0;

  auto total() const -> unsigned {
    // The three message subcategories partition native_message, not extra
    // faults to count a second time.
    return prior + admitted + native_code + native_message + raw_kind +
           raw_message + synchronous_identity + synchronous_message +
           success_value;
  }
  auto operator==(const CopiedControlCounts&) const -> bool = default;
};

auto native_error_cell(const Identity& id) -> bool {
  if (id.control == Control::HANDLER_WRAPPER) return true;
  if (id.control != Control::PRIMARY) return false;
  return (id.family == 8 && id.seed == 2) || (id.family == 9 && id.seed == 3) ||
         (id.family >= 10 && id.family <= 17);
}

auto corrupt_required_message(Cell& cell, CopiedControlCounts& counts) -> void {
  auto& message = (*cell.execution.result.value)["error"]["message"];
  const auto& id = cell.identity;
  if (id.family >= 16 && id.control == Control::PRIMARY) {
    auto text = message.asString();
    if (id.seed == 0) {
      // Leave the required marker intact, but invalidate the required prefix.
      text.front() = '!';
      ++counts.bounded_prefix;
    } else {
      // Appending arbitrary text would still satisfy this bounded oracle.
      // Remove EVERY required-marker occurrence while preserving the prefix.
      const std::string_view marker =
          id.family == 16 ? "SyntaxError" : "owned-E07-reject";
      auto position = text.find(marker);
      while (position != std::string::npos) {
        text.replace(position, marker.size(), marker.size(), '!');
        position = text.find(marker);
      }
      ++counts.bounded_marker;
    }
    message = text;
  } else {
    message = "independent wrong required message";
    ++counts.exact_message;
  }
}

auto corrupt_success_value(Cell& cell) -> void {
  const auto& id = cell.identity;
  auto& result = *cell.execution.result.value;
  if (id.control == Control::RAW) {
    Json::Value* leaf = id.batch ? &result[0] : &result;
    const unsigned depth = id.batch ? 63U : 64U;
    for (unsigned edge = 0; edge < depth; ++edge) {
      leaf = &(*leaf)["n"];
    }
    *leaf = 8;
  } else if (id.family == 8) {
    result["value"] = result["value"].asString() + '!';
  } else {
    // Change the actual shallow depth observation, not an unrelated field.
    result["depth"] = id.control == Control::DEPTH_SEVEN ? 7 : 62;
  }
}

auto assert_fine_controls(const Cell& cell, CopiedControlCounts& counts)
    -> unsigned {
  auto corrupted = cell;
  ++corrupted.execution.admitted;
  REQUIRE_FALSE(matches(corrupted));
  ++counts.admitted;
  const auto& id = cell.identity;
  if (native_error_cell(id)) {
    corrupted = cell;
    (*corrupted.execution.result.value)["error"]["code"] =
        "cap.independent_wrong_code";
    REQUIRE_FALSE(matches(corrupted));
    ++counts.native_code;
    corrupted = cell;
    corrupt_required_message(corrupted, counts);
    REQUIRE_FALSE(matches(corrupted));
    ++counts.native_message;
    return 3;
  }
  if (id.control == Control::RAW && id.batch && id.seed == 1) {
    corrupted = cell;
    corrupted.execution.result.value.error().kind =
        EvalErrorKind::RUNTIME_ERROR;
    REQUIRE_FALSE(matches(corrupted));
    ++counts.raw_kind;
    corrupted = cell;
    corrupted.execution.result.value.error().message =
        "independent wrong host depth message";
    REQUIRE_FALSE(matches(corrupted));
    ++counts.raw_message;
    return 3;
  }
  if (id.family == 9 && id.control == Control::PRIMARY && id.seed == 2) {
    corrupted = cell;
    (*corrupted.execution.result.value)["error"]["typeError"] = false;
    REQUIRE_FALSE(matches(corrupted));
    ++counts.synchronous_identity;
    corrupted = cell;
    (*corrupted.execution.result.value)["error"]["message"] =
        "independent wrong synchronous depth message";
    REQUIRE_FALSE(matches(corrupted));
    ++counts.synchronous_message;
    return 3;
  }
  corrupted = cell;
  corrupt_success_value(corrupted);
  REQUIRE_FALSE(matches(corrupted));
  ++counts.success_value;
  return 2;
}

// Each positive observation is also tested against independently corrupted
// receipt, teardown and actual-result copies. No extra native calls are made.
auto assert_cell(const Cell& cell, CopiedControlCounts& counts) -> unsigned {
  INFO("capability edge family="
       << cell.identity.family << " seed=" << cell.identity.seed
       << " replay=" << cell.identity.replay << " batch=" << cell.identity.batch
       << " control=" << static_cast<unsigned>(cell.identity.control));
  if (cell.execution.result.value.has_value()) {
    INFO("actual=" << cell.execution.result.value->toStyledString());
  } else {
    INFO("host error=" << cell.execution.result.value.error().message);
  }
  REQUIRE(matches(cell));
  auto corrupted = cell;
  ++corrupted.target_entries;
  REQUIRE_FALSE(matches(corrupted));
  corrupted = cell;
  ++corrupted.peer_entries;
  REQUIRE_FALSE(matches(corrupted));
  corrupted = cell;
  ++corrupted.total_entries;
  REQUIRE_FALSE(matches(corrupted));
  corrupted = cell;
  corrupted.provider_closed = false;
  REQUIRE_FALSE(matches(corrupted));
  corrupted = cell;
  corrupted.execution.cleanup.callbacks = 1;
  REQUIRE_FALSE(matches(corrupted));
  corrupted = cell;
  corrupted.shutdown.idempotent = false;
  REQUIRE_FALSE(matches(corrupted));
  corrupted = cell;
  corrupted.execution.result.value = Json::Value{"independent wrong result"};
  REQUIRE_FALSE(matches(corrupted));
  counts.prior += 7;
  return 7 + assert_fine_controls(cell, counts);
}

auto assert_inventory(const std::vector<Cell>& cells, bool errors) -> void {
  REQUIRE(complete_inventory(cells, errors));
  auto corrupted = cells;
  corrupted.back() = corrupted.front();
  REQUIRE_FALSE(complete_inventory(corrupted, errors));
  corrupted = cells;
  corrupted.pop_back();
  REQUIRE_FALSE(complete_inventory(corrupted, errors));
  corrupted = cells;
  corrupted.push_back(corrupted.front());
  REQUIRE_FALSE(complete_inventory(corrupted, errors));
  corrupted = cells;
  corrupted.front().identity.replay = 2;
  REQUIRE_FALSE(complete_inventory(corrupted, errors));
  corrupted = cells;
  corrupted.front().identity.family = 99;
  REQUIRE_FALSE(complete_inventory(corrupted, errors));
  corrupted = cells;
  corrupted.front().identity.batch = !corrupted.front().identity.batch;
  REQUIRE_FALSE(complete_inventory(corrupted, errors));
}

} // namespace

TEST_CASE("Native capability byte and conversion-depth boundaries retain their "
          "exact edges",
          "[js][async][cap][corpus][native-capability-edges]") {
  const auto deadline = Clock::now() + 55s;
  fixture::initialize();
  std::vector<Cell> cells;
  cells.reserve(46);
  for (unsigned replay = 0; replay < 2; ++replay) {
    for (unsigned family = 8; family <= 9; ++family) {
      for (unsigned seed = 0; seed < 4; ++seed) {
        for (const bool batch : {false, true}) {
          fixture::demand(Clock::now() < deadline,
                          "capability R08/R09 collection deadline");
          cells.push_back(collect({family, seed, replay, batch}));
        }
      }
    }
    // Six raw controls: C D64, B [D63,7], B [D64,7], both owner replays.
    cells.push_back(collect({9, 1, replay, false, Control::RAW}));
    cells.push_back(collect({9, 0, replay, true, Control::RAW}));
    cells.push_back(collect({9, 1, replay, true, Control::RAW}));
    for (const bool batch : {false, true}) {
      cells.push_back(collect({9, 3, replay, batch, Control::HANDLER_WRAPPER}));
      cells.push_back(collect({13, 0, replay, batch, Control::DEPTH_SEVEN}));
    }
  }
  // All owned contexts/pools/registry clients and fixture identities have
  // already been shut down before any semantic assertion can stop the test.
  REQUIRE(cells.size() == 46);
  assert_inventory(cells, false);
  unsigned primary = 0;
  unsigned controls = 0;
  unsigned entries = 0;
  unsigned negative_controls = 0;
  CopiedControlCounts control_counts;
  for (const auto& cell : cells) {
    negative_controls += assert_cell(cell, control_counts);
    primary += cell.identity.control == Control::PRIMARY ? 1U : 0U;
    controls += cell.identity.control == Control::PRIMARY ? 0U : 1U;
    entries += cell.total_entries;
  }
  REQUIRE(primary == 32);
  REQUIRE(controls == 14);
  REQUIRE(entries == 64); // R08 24 + R09 18 + separately counted controls 22.
  const CopiedControlCounts expected_controls{.prior = 322,
                                              .admitted = 46,
                                              .native_code = 12,
                                              .native_message = 12,
                                              .exact_message = 12,
                                              .bounded_prefix = 0,
                                              .bounded_marker = 0,
                                              .raw_kind = 2,
                                              .raw_message = 2,
                                              .synchronous_identity = 4,
                                              .synchronous_message = 4,
                                              .success_value = 28};
  REQUIRE(control_counts == expected_controls);
  REQUIRE(negative_controls == 432);
  REQUIRE(control_counts.total() == negative_controls);
  REQUIRE(Clock::now() < deadline);
}

TEST_CASE("Native capability error families retain literal rejection and entry "
          "contracts",
          "[js][async][cap][corpus][native-capability-errors]") {
  const auto deadline = Clock::now() + 55s;
  fixture::initialize();
  std::vector<Cell> cells;
  cells.reserve(64);
  for (unsigned replay = 0; replay < 2; ++replay) {
    for (unsigned family = 0; family < 8; ++family) {
      for (unsigned seed = 0; seed < 2; ++seed) {
        for (const bool batch : {false, true}) {
          fixture::demand(Clock::now() < deadline,
                          "capability E00/E07 collection deadline");
          cells.push_back(collect({family + 10, seed, replay, batch}));
        }
      }
    }
  }
  REQUIRE(cells.size() == 64);
  assert_inventory(cells, true);
  unsigned entries = 0;
  unsigned negative_controls = 0;
  CopiedControlCounts control_counts;
  for (const auto& cell : cells) {
    negative_controls += assert_cell(cell, control_counts);
    entries += cell.total_entries;
  }
  REQUIRE(entries ==
          36); // Entered E07 8; admitted/drained peers 28, E03 peers 0.
  const CopiedControlCounts expected_controls{.prior = 448,
                                              .admitted = 64,
                                              .native_code = 64,
                                              .native_message = 64,
                                              .exact_message = 48,
                                              .bounded_prefix = 8,
                                              .bounded_marker = 8,
                                              .raw_kind = 0,
                                              .raw_message = 0,
                                              .synchronous_identity = 0,
                                              .synchronous_message = 0,
                                              .success_value = 0};
  REQUIRE(control_counts == expected_controls);
  REQUIRE(negative_controls == 640);
  REQUIRE(control_counts.total() == negative_controls);
  REQUIRE(Clock::now() < deadline);
}
