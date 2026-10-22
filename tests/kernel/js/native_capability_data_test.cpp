// SPDX-License-Identifier: MIT

#include "native_capability_fixture.hpp"
#include "native_capability_oracles.hpp"
#include "native_capability_programs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <json/value.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace corpus = native_capability_corpus;
namespace fixture = native_capability_corpus::fixture;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::size_t FAMILIES = 8;
constexpr std::size_t SEEDS = 4;
constexpr std::size_t REPLAYS = 2;
constexpr std::size_t ROUTES = 2;
constexpr std::size_t CELLS = 128;
constexpr unsigned RECEIPTS_PER_REPLAY = 96;

// Only trusted ASCII setup literals/identities use this helper. Unicode and
// embedded-NUL value/key expectations remain the independent literal arrays
// in native_capability_oracles.hpp, never decoded from the JS programs.
auto ascii_units(std::string_view literal) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const unsigned char unit : literal) {
    fixture::demand(unit <= 127, "non-ASCII capability setup identity");
    result.append(static_cast<int>(unit));
  }
  return result;
}

auto ascii_descriptor(std::string_view literal) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "string";
  result["units"] = ascii_units(literal);
  return result;
}

auto object_descriptor(
    std::initializer_list<std::pair<std::string_view, Json::Value>> members)
    -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "object";
  result["entries"] = Json::Value{Json::arrayValue};
  for (const auto& [key, value] : members) {
    Json::Value entry{Json::objectValue};
    entry["key"] = ascii_units(key);
    entry["value"] = value;
    result["entries"].append(entry);
  }
  return result;
}

auto expected_context(std::string_view extension) -> Json::Value {
  Json::Value depth{Json::objectValue};
  depth["type"] = "number";
  depth["value"] = 1;
  depth["safe"] = true;
  depth["finite"] = true;
  const auto identity = object_descriptor({
      {"id", ascii_descriptor("11111111-1111-1111-1111-111111111187")},
      {"username", ascii_descriptor("native-capability-fixture")},
      {"auth_type", ascii_descriptor("session")},
  });
  return object_descriptor({{"user", identity},
                            {"session_id", ascii_descriptor("")},
                            {"ip_address", ascii_descriptor("")},
                            {"call_depth", depth},
                            {"extension", ascii_descriptor(extension)}});
}

struct Cell {
  std::size_t replay;
  std::size_t family;
  std::size_t seed;
  std::size_t route;
  fixture::Execution execution;
  unsigned target_entries;
  unsigned peer_entries;
};

struct Owner {
  std::size_t replay;
  std::string extension;
  fixture::Shutdown shutdown;
  bool provider_finished;
  unsigned receipts;
};

auto owner_for(const std::vector<Owner>& owners, std::size_t replay)
    -> const Owner* {
  for (const auto& owner : owners) {
    if (owner.replay == replay) return &owner;
  }
  return nullptr;
}

auto owners_complete(const std::vector<Owner>& owners) -> bool {
  if (owners.size() != REPLAYS) return false;
  std::array<unsigned, REPLAYS> counts{};
  for (const auto& owner : owners) {
    if (owner.replay >= REPLAYS || owner.extension.empty() ||
        !fixture::stopped(owner.shutdown) || !owner.provider_finished ||
        owner.receipts != RECEIPTS_PER_REPLAY)
      return false;
    ++counts[owner.replay];
  }
  return counts[0] == 1 && counts[1] == 1 &&
         owners[0].extension != owners[1].extension;
}

auto matches_cell(const Cell& cell, const std::vector<Owner>& owners) -> bool {
  const auto* owner = owner_for(owners, cell.replay);
  if (owner == nullptr || cell.family >= FAMILIES || cell.seed >= SEEDS ||
      cell.route >= ROUTES || !fixture::clean(cell.execution.cleanup) ||
      cell.execution.admitted != (cell.route == 1 ? 2 : 1) ||
      !cell.execution.result.value.has_value() || cell.target_entries != 1 ||
      cell.peer_entries != (cell.route == 1 ? 1U : 0U))
    return false;
  const auto& value = *cell.execution.result.value;
  if (!value.isObject() || value.size() != 4 || !value.isMember("arg") ||
      !value.isMember("reply") || !value.isMember("ctx") ||
      !value.isMember("peer"))
    return false;
  if (!corpus::matches_tree(value["arg"],
                            corpus::expected_data(cell.family, cell.seed)) ||
      !corpus::matches_tree(
          value["reply"],
          corpus::expected_data(cell.family, (cell.seed + 1) % SEEDS)) ||
      !corpus::matches_tree(value["ctx"], expected_context(owner->extension)))
    return false;
  return cell.route == 0
             ? value["peer"].isNull()
             : corpus::matches_tree(value["peer"], ascii_descriptor("peer"));
}

auto ledger_complete(const std::vector<Cell>& cells,
                     const std::vector<Owner>& owners) -> bool {
  if (cells.size() != CELLS || !owners_complete(owners)) return false;
  std::array<
      std::array<std::array<std::array<unsigned, ROUTES>, SEEDS>, FAMILIES>,
      REPLAYS>
      counts{};
  unsigned targets = 0;
  unsigned peers = 0;
  for (const auto& cell : cells) {
    if (cell.replay >= REPLAYS || !matches_cell(cell, owners)) return false;
    ++counts[cell.replay][cell.family][cell.seed][cell.route];
    targets += cell.target_entries;
    peers += cell.peer_entries;
  }
  for (const auto& replay : counts)
    for (const auto& family : replay)
      for (const auto& seed : family)
        for (const unsigned count : seed)
          if (count != 1) return false;
  // Entry writes acknowledge actual native handler entry, not argument
  // validation. Literal trees above independently establish argument/reply
  // observations. Setup/control SQL never increments these native roles.
  return targets == 128 && peers == 64 && targets + peers == 192;
}

auto run_cell(fixture::Provider& provider, fixture::Caller& caller,
              std::size_t replay, std::size_t family, std::size_t seed,
              std::size_t route) -> Cell {
  const auto ordinal =
      static_cast<unsigned>((family * SEEDS + seed) * ROUTES + route);
  const unsigned target_marker = ordinal * 2U;
  const unsigned peer_marker = target_marker + 1U;
  const auto target = corpus::data_handler(
      family, seed, provider.entry_statement(target_marker));
  const auto peer = "export default async function nativePeer(){" +
                    provider.entry_statement(peer_marker) + "return 'peer';}";
  const auto source = corpus::data_caller(
      family, seed, route == 1, "capnative:1:data", "capnative:1:peer");
  fixture::demand(target.size() <= 16384 && peer.size() <= 16384 &&
                      source.size() <= 16384,
                  "native capability data source budget");
  provider.stage("data", target);
  provider.stage("peer", peer);
  auto execution = caller.run(source);
  const unsigned target_entries = provider.receipt_count(target_marker);
  const unsigned peer_entries = provider.receipt_count(peer_marker);
  return {replay,         family,      seed, route, std::move(execution),
          target_entries, peer_entries};
}

auto corrupt_cleanup(fixture::Cleanup& state, unsigned field) -> void {
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

auto corrupt_shutdown(fixture::Shutdown& state, unsigned field) -> void {
  switch (field) {
    case 0: state.stopped = false; break;
    case 1: state.idempotent = false; break;
    case 2: ++state.active; break;
    default: ++state.free; break;
  }
}

// Mutate observations only after their real baseline and owner cleanup pass.
// Prefer a different value in the same descriptor kind, including a valid
// finite/safe number. Null has no second same-kind value, and empty containers
// have no leaf: those cases have explicitly counted type/cardinality sentinels.
auto corrupt_same_kind_leaf(Json::Value& descriptor) -> bool {
  const auto type = descriptor["type"].asString();
  if (type == "boolean") {
    descriptor["value"] = !descriptor["value"].asBool();
    return true;
  }
  if (type == "number") {
    const double value = descriptor["value"].asDouble();
    descriptor["value"] = descriptor["safe"].asBool()
                              ? (value == 0.0 ? 1.0 : 0.0)
                              : (value == 0.5 ? 1.25 : 0.5);
    return true;
  }
  if (type == "string") {
    auto& units = descriptor["units"];
    if (units.empty()) {
      units.append(65);
    } else {
      units[0] = units[0].asUInt() ^ 1U;
    }
    return true;
  }
  if (type == "array") {
    for (auto& item : descriptor["items"])
      if (corrupt_same_kind_leaf(item)) return true;
  } else if (type == "object") {
    for (auto& entry : descriptor["entries"])
      if (corrupt_same_kind_leaf(entry["value"])) return true;
  }
  return false;
}

enum class ValueFault { NONE, SAME_KIND_LEAF, NULL_TYPE, EMPTY_CARDINALITY };

auto corrupt_degenerate_value(Json::Value& descriptor) -> ValueFault {
  const auto type = descriptor["type"].asString();
  if (type == "null") {
    descriptor["type"] = "boolean";
    descriptor["value"] = false;
    return ValueFault::NULL_TYPE;
  }
  if (type == "array") {
    auto& items = descriptor["items"];
    if (items.empty()) {
      Json::Value item{Json::objectValue};
      item["type"] = "null";
      items.append(item);
      return ValueFault::EMPTY_CARDINALITY;
    }
    for (auto& item : items) {
      const auto fault = corrupt_degenerate_value(item);
      if (fault != ValueFault::NONE) return fault;
    }
  } else if (type == "object") {
    auto& entries = descriptor["entries"];
    if (entries.empty()) {
      Json::Value entry{Json::objectValue};
      entry["key"] = Json::Value{Json::arrayValue};
      entry["key"].append(1);
      entry["value"]["type"] = "null";
      entries.append(entry);
      return ValueFault::EMPTY_CARDINALITY;
    }
    for (auto& entry : entries) {
      const auto fault = corrupt_degenerate_value(entry["value"]);
      if (fault != ValueFault::NONE) return fault;
    }
  }
  return ValueFault::NONE;
}

struct ValueFaultCounts {
  unsigned same_kind = 0;
  unsigned null_type = 0;
  unsigned empty_cardinality = 0;
  auto total() const -> unsigned {
    return same_kind + null_type + empty_cardinality;
  }
};

auto count_value_fault(ValueFaultCounts& counts, ValueFault fault) -> void {
  switch (fault) {
    case ValueFault::SAME_KIND_LEAF: ++counts.same_kind; break;
    case ValueFault::NULL_TYPE: ++counts.null_type; break;
    case ValueFault::EMPTY_CARDINALITY: ++counts.empty_cardinality; break;
    case ValueFault::NONE: FAIL("no independent value fault was applied");
  }
}

struct TargetedCounts {
  ValueFaultCounts arg;
  ValueFaultCounts reply;
  unsigned member_units = 0;
  unsigned embedded_nul = 0;
  unsigned array_order = 0;
  unsigned admitted = 0;
  auto total() const -> unsigned {
    // Embedded-NUL mutations are a subset of member-unit mutations.
    return arg.total() + reply.total() + member_units + array_order + admitted;
  }
};

auto targeted_controls(const Cell& actual, const std::vector<Owner>& owners,
                       TargetedCounts& counts) -> void {
  for (const auto* field : {"arg", "reply"}) {
    auto wrong = actual;
    auto& descriptor = (*wrong.execution.result.value)[field];
    const auto fault = corrupt_same_kind_leaf(descriptor)
                           ? ValueFault::SAME_KIND_LEAF
                           : corrupt_degenerate_value(descriptor);
    REQUIRE(fault != ValueFault::NONE);
    CHECK_FALSE(matches_cell(wrong, owners));
    count_value_fault(
        std::string_view{field} == "arg" ? counts.arg : counts.reply, fault);
  }
  if (actual.family == 5) {
    auto wrong = actual;
    auto& entries = (*wrong.execution.result.value)["arg"]["entries"];
    bool changed_nul = false;
    for (auto& entry : entries) {
      for (auto& unit : entry["key"]) {
        if (unit.asUInt() == 0) {
          unit = 1;
          changed_nul = true;
          break;
        }
      }
      if (changed_nul) break;
    }
    if (!changed_nul) {
      auto& key = entries[0]["key"];
      if (key.empty()) {
        key.append(1);
      } else {
        key[0] = key[0].asUInt() ^ 1U;
      }
    }
    CHECK_FALSE(matches_cell(wrong, owners));
    ++counts.member_units;
    counts.embedded_nul += changed_nul ? 1U : 0U;
  }
  const auto& argument = (*actual.execution.result.value)["arg"];
  if (argument["type"].asString() == "array" && argument["items"].size() > 1) {
    auto wrong = actual;
    auto& items = (*wrong.execution.result.value)["arg"]["items"];
    std::swap(items[0], items[1]);
    CHECK_FALSE(matches_cell(wrong, owners));
    ++counts.array_order;
  }
  auto wrong = actual;
  ++wrong.execution.admitted;
  CHECK_FALSE(matches_cell(wrong, owners));
  ++counts.admitted;
}

auto value_controls(const Cell& actual, const std::vector<Owner>& owners)
    -> unsigned {
  unsigned controls = 0;
  auto wrong = actual;
  wrong.execution.result.value->removeMember("arg");
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  wrong = actual;
  (*wrong.execution.result.value)["arg"]["type"] = "corrupted";
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  wrong = actual;
  (*wrong.execution.result.value)["reply"]["type"] = "corrupted";
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  wrong = actual;
  auto& context_entries = (*wrong.execution.result.value)["ctx"]["entries"];
  const auto first = context_entries[0];
  context_entries[1] = first; // Duplicate a key without changing cardinality.
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  wrong = actual;
  (*wrong.execution.result.value)["reply"] =
      (*wrong.execution.result.value)["arg"];
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  wrong = actual;
  wrong.target_entries = 0;
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  wrong = actual;
  ++wrong.peer_entries;
  CHECK_FALSE(matches_cell(wrong, owners));
  ++controls;
  return controls;
}

} // namespace

TEST_CASE("QuickJS native capability calls have a bounded data corpus",
          "[js][async][cap][native-capability][native-capability-data]") {
  const auto deadline = Clock::now() + 55s;
  fixture::initialize();
  std::vector<Cell> observations;
  std::vector<Owner> owners;
  observations.reserve(CELLS);
  owners.reserve(REPLAYS);
  for (std::size_t replay = 0; replay < REPLAYS; ++replay) {
    fixture::Provider provider;
    fixture::Caller caller(provider.config, fixture::user());
    provider.seed("data");
    provider.seed("peer");
    for (std::size_t family = 0; family < FAMILIES; ++family) {
      for (std::size_t seed = 0; seed < SEEDS; ++seed) {
        for (std::size_t route = 0; route < ROUTES; ++route) {
          fixture::demand(Clock::now() < deadline,
                          "native capability data wall budget");
          observations.push_back(
              run_cell(provider, caller, replay, family, seed, route));
        }
      }
    }
    const unsigned receipts = provider.total_receipts();
    const auto shutdown = caller.finish();
    const bool finished = provider.finish();
    owners.push_back(
        {replay, provider.extension, shutdown, finished, receipts});
  }
  // Every real call and both caller/provider owners have completed cleanup
  // before validating any data or mutating copies of actual observations.
  REQUIRE(Clock::now() < deadline);
  REQUIRE(ledger_complete(observations, owners));

  unsigned value_count = 0;
  unsigned cleanup_count = 0;
  unsigned identity_count = 0;
  unsigned order_count = 0;
  TargetedCounts targeted;
  for (const auto& actual : observations) {
    INFO("replay=" << actual.replay << " family=" << actual.family
                   << " seed=" << actual.seed << " route=" << actual.route);
    REQUIRE(matches_cell(actual, owners));
    value_count += value_controls(actual, owners);
    targeted_controls(actual, owners, targeted);
    auto reordered = actual;
    auto& entries = (*reordered.execution.result.value)["ctx"]["entries"];
    for (Json::ArrayIndex index = 0; index < entries.size() / 2; ++index) {
      std::swap(entries[index], entries[entries.size() - 1 - index]);
    }
    CHECK(matches_cell(reordered, owners));
    ++order_count;
    for (unsigned field = 0; field < 17; ++field) {
      auto wrong = actual;
      corrupt_cleanup(wrong.execution.cleanup, field);
      CHECK_FALSE(matches_cell(wrong, owners));
      ++cleanup_count;
    }
    auto wrong = actual;
    wrong.route = 1 - actual.route;
    CHECK_FALSE(matches_cell(wrong, owners));
    ++identity_count;
    wrong = actual;
    wrong.family = (actual.family + 1) % FAMILIES;
    CHECK_FALSE(matches_cell(wrong, owners));
    ++identity_count;
    wrong = actual;
    wrong.seed = (actual.seed + 1) % SEEDS;
    CHECK_FALSE(matches_cell(wrong, owners));
    ++identity_count;
    wrong = actual;
    wrong.replay = 1 - actual.replay;
    CHECK_FALSE(matches_cell(wrong, owners));
    ++identity_count;
  }
  REQUIRE(value_count == 896);
  REQUIRE(targeted.arg.same_kind == 104);
  REQUIRE(targeted.arg.null_type == 16);
  REQUIRE(targeted.arg.empty_cardinality == 8);
  REQUIRE(targeted.arg.total() == 128);
  REQUIRE(targeted.reply.same_kind == 104);
  REQUIRE(targeted.reply.null_type == 16);
  REQUIRE(targeted.reply.empty_cardinality == 8);
  REQUIRE(targeted.reply.total() == 128);
  REQUIRE(targeted.member_units == 16);
  REQUIRE(targeted.embedded_nul == 4);
  REQUIRE(targeted.array_order == 20);
  REQUIRE(targeted.admitted == 128);
  REQUIRE(targeted.total() == 420);
  REQUIRE(value_count + targeted.total() == 1316);
  REQUIRE(cleanup_count == 2176);
  REQUIRE(identity_count == 512);
  REQUIRE(order_count == 128);

  auto missing = observations;
  missing.pop_back();
  CHECK_FALSE(ledger_complete(missing, owners));
  auto duplicate = observations;
  duplicate.back() = duplicate.front();
  CHECK_FALSE(ledger_complete(duplicate, owners));
  auto bad_family = observations;
  bad_family.front().family = FAMILIES;
  CHECK_FALSE(ledger_complete(bad_family, owners));
  auto bad_seed = observations;
  bad_seed.front().seed = SEEDS;
  CHECK_FALSE(ledger_complete(bad_seed, owners));
  auto bad_route = observations;
  bad_route.front().route = ROUTES;
  CHECK_FALSE(ledger_complete(bad_route, owners));
  auto bad_replay = observations;
  bad_replay.front().replay = REPLAYS;
  CHECK_FALSE(ledger_complete(bad_replay, owners));

  unsigned shutdown_controls = 0;
  for (std::size_t index = 0; index < owners.size(); ++index) {
    for (unsigned field = 0; field < 4; ++field) {
      auto wrong = owners;
      corrupt_shutdown(wrong[index].shutdown, field);
      CHECK_FALSE(owners_complete(wrong));
      ++shutdown_controls;
    }
    auto wrong = owners;
    wrong[index].provider_finished = false;
    CHECK_FALSE(owners_complete(wrong));
    wrong = owners;
    --wrong[index].receipts;
    CHECK_FALSE(owners_complete(wrong));
  }
  REQUIRE(shutdown_controls == 8);
  auto missing_owner = owners;
  missing_owner.pop_back();
  CHECK_FALSE(ledger_complete(observations, missing_owner));
  auto duplicate_owner = owners;
  duplicate_owner.back() = duplicate_owner.front();
  CHECK_FALSE(ledger_complete(observations, duplicate_owner));
  REQUIRE(Clock::now() < deadline);
}
