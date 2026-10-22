// SPDX-License-Identifier: MIT

#include "native_capability_ownership_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace own = native_capability_corpus::ownership;
namespace fixture = native_capability_corpus::fixture;
using namespace std::chrono_literals;
using plinth::js::EvalErrorKind;

struct Cell {
  unsigned family;
  unsigned variant;
  unsigned replay;
  plinth::js::EvalResult result;
  fixture::Shutdown shutdown;
  bool causal = false;
  bool retired = false;
  bool recovered = false;
  unsigned writes = 0;
  int caps = 0;
  int database = 0;
  unsigned caller_generations = 0;
  unsigned native_generations = 0;
  std::string acknowledgement;
  std::string rejection_prefix;
  std::vector<own::Event> events;
};

struct Epoch {
  unsigned replay;
  bool closed;
  unsigned warm_caps;
  unsigned warm_writes;
  unsigned receipts;
};

auto literal(std::string_view text) -> std::string {
  fixture::demand(
      text.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
          std::string_view::npos,
      "ownership marker literal alphabet");
  return "'" + std::string{text} + "'";
}

auto marker(unsigned family, unsigned variant, unsigned replay, unsigned child)
    -> std::string {
  return "owned_" + std::to_string(family) + "_" + std::to_string(variant) +
         "_" + std::to_string(replay) + "_" + std::to_string(child);
}

auto stage(own::Providers& providers, unsigned domain,
           std::string_view function, unsigned receipt,
           const std::string& response, bool gated, bool rejected = false,
           bool evidence = false) -> void {
  if (gated) providers.hold(receipt);
  providers.stage(
      domain, function,
      "export default async function owned(args,ctx){" +
          providers.write(domain, receipt, evidence) +
          (gated ? "await db.query('" + providers.gate_sql(receipt) + "');"
                 : "") +
          (rejected ? "throw new Error(" + literal(response) + ");}"
                    : "return " + literal(response) + ";}"));
}

auto batch(const own::Providers& providers,
           const std::vector<std::pair<unsigned, std::string>>& functions)
    -> std::string {
  std::string result = "cap.batch([";
  for (std::size_t index = 0; index < functions.size(); ++index) {
    if (index != 0) result += ',';
    result +=
        "['" +
        providers.signature(functions[index].first, functions[index].second) +
        "',null]";
  }
  return result + "])";
}

auto active(const own::Snapshot& state, int next, int concurrent,
            const std::vector<int>& callbacks, const std::vector<int>& pending)
    -> bool {
  return state.visited && !state.cancelled && state.cap_only &&
         state.next == next && state.concurrent == concurrent &&
         state.detached == concurrent && state.callbacks == callbacks &&
         state.pending == pending;
}

auto caller_generation(const own::Observations& observations)
    -> own::Generation {
  own::Generation result;
  own::until(
      [&] {
        result = observations.caller();
        return !result.expired();
      },
      "ownership caller generation not observed");
  return result;
}

auto record(Cell& cell, own::Observations& observations,
            const own::Providers& providers) -> void {
  observations.close();
  cell.events = observations.events();
  for (const auto& event : cell.events) {
    fixture::demand(event.entered == 1 && event.finished == 1 &&
                        event.admitted >= 0,
                    "ownership execution event cardinality");
    if (event.extension.empty()) {
      const bool cancelled_primary = (cell.family == 6 || cell.family == 7) &&
                                     cell.caller_generations == 0;
      fixture::demand(
          cancelled_primary ? event.error == EvalErrorKind::CANCELLED
                            : !event.error.has_value(),
          "ownership caller's actual FINISH classification mismatch");
      ++cell.caller_generations;
      cell.caps += event.admitted;
    } else {
      fixture::demand(std::ranges::any_of(providers.domains,
                                          [&](const own::Domain& domain) {
                                            return domain.extension ==
                                                   event.extension;
                                          }),
                      "ownership event from another provider");
      if (cell.family != 7) {
        const bool deliberate_rejection =
            cell.family == 1 &&
            (event.extension == providers.domains[0].extension ||
             event.extension == providers.domains[1].extension);
        fixture::demand(deliberate_rejection
                            ? event.error ==
                                  EvalErrorKind::PROMISE_REJECTED_UNHANDLED
                            : !event.error.has_value(),
                        "ownership native FINISH classification mismatch");
      }
      ++cell.native_generations;
      cell.database += event.admitted;
    }
  }
}

auto warm(own::Providers& providers) -> unsigned {
  fixture::Caller caller(providers.config);
  unsigned result = 0;
  for (unsigned domain = 0; domain < 4; ++domain) {
    stage(providers, domain, "warm", 90000U + domain, "warm", false);
    const auto observed =
        caller.run("(async()=>{return await " +
                   own::invocation(providers, domain, "warm") + ";})()");
    fixture::demand(observed.result.value.has_value() &&
                        observed.result.value->isString() &&
                        observed.result.value->asString() == "warm" &&
                        fixture::clean(observed.cleanup) &&
                        observed.admitted == 1 &&
                        providers.entries(domain, 90000U + domain) == 1,
                    "ownership real provider warmup failed");
    ++result;
  }
  fixture::demand(fixture::stopped(caller.finish()),
                  "ownership warm caller shutdown failed");
  return result;
}

auto ordering(own::Providers& providers, unsigned family, unsigned variant,
              unsigned replay) -> Cell {
  const unsigned base = 100U + (family * 2U + variant) * 10U;
  const unsigned a = variant;
  const unsigned b = 1U - variant;
  const auto before = providers.total();
  own::Caller caller(providers.config, family == 3 ? 2 : 8);
  own::Observations observations;
  std::string source;
  std::vector<std::pair<unsigned, std::string>> functions;
  if (family == 3) {
    for (unsigned index = 0; index < 5; ++index) {
      const auto function = "fan" + std::to_string(index);
      const auto domain = index % 2U == 0 ? a : b;
      stage(providers, domain, function, base + index,
            marker(family, variant, replay, index), true);
      functions.emplace_back(domain, function);
    }
    source = "(async()=>{return await " + batch(providers, functions) + ";})()";
  } else {
    stage(providers, a, "first", base, marker(family, variant, replay, 0),
          family != 2, family == 1);
    if (family != 2)
      stage(providers, b, "second", base + 1U,
            marker(family, variant, replay, 1), true, family == 1);
    const auto operation =
        family == 2 ? "cap.batch([['" + providers.signature(a, "first") +
                          "',null]," + (variant == 0 ? "42" : "[]") + "])"
                    : batch(providers, {{a, "first"}, {b, "second"}});
    if (family == 0) {
      source = "(async()=>{return await " + operation + ";})()";
    } else {
      stage(providers, 3, "barrier", base + 3U, "barrier", true, false, true);
      if (family == 1)
        stage(providers, 2, "ack", base + 2U, "ack", false, false, true);
      source =
          "(async()=>{let returned=false;try{const pending=" + operation +
          ";returned=true;await pending;throw new Error('missing "
          "rejection');}catch(error){const actual={"
          "code:typeof "
          "error.code==='string'?error.code:'',message:String(error.message),"
          "detail:" +
          (family == 1
               ? "Object.getOwnPropertyNames(error).sort().join(',')+'|'+("
                 "Object.getPrototypeOf(error)===Object.prototype)+'|'+(error "
                 "instanceof "
                 "Error)+'|'+Object.prototype.hasOwnProperty.call(error,'kind')"
               : "String(error instanceof TypeError)+'|'+String(returned)") +
          "};" +
          (family == 1
               ? "await " + own::invocation(providers, 2, "ack", "actual") + ";"
               : "") +
          "await " + own::invocation(providers, 3, "barrier", "actual") +
          ";return actual;}})()";
    }
  }
  own::Run run(providers, *caller.context, source);
  const auto generation = caller_generation(observations);
  bool causal = false;
  std::string acknowledgement;
  if (family == 3) {
    own::until(
        [&] {
          return providers.waiter(a, base) && providers.waiter(b, base + 1U) &&
                 providers.entries(a, base) == 1 &&
                 providers.entries(b, base + 1U) == 1 &&
                 providers.entries(a, base + 2U) == 0 &&
                 providers.entries(b, base + 3U) == 0 &&
                 providers.entries(a, base + 4U) == 0 &&
                 active(own::snapshot(generation), 5, 2, {0, 1, 2, 3, 4},
                        {2, 3, 4});
        },
        "ownership fanout first wave missing");
    providers.unlock(base);
    providers.unlock(base + 1U);
    own::until(
        [&] {
          return providers.waiter(a, base + 2U) &&
                 providers.waiter(b, base + 3U) &&
                 providers.entries(a, base + 2U) == 1 &&
                 providers.entries(b, base + 3U) == 1 &&
                 providers.entries(a, base + 4U) == 0 &&
                 active(own::snapshot(generation), 5, 2, {2, 3, 4}, {4});
        },
        "ownership fanout second wave missing");
    providers.unlock(base + 2U);
    providers.unlock(base + 3U);
    own::until(
        [&] {
          return providers.waiter(a, base + 4U) &&
                 providers.entries(a, base + 4U) == 1 &&
                 active(own::snapshot(generation), 5, 1, {4}, {});
        },
        "ownership fanout final wave missing");
    providers.unlock(base + 4U);
    causal = true;
  } else if (family == 2) {
    own::until(
        [&] {
          return providers.entries(a, base) == 1 &&
                 providers.entries(3, base + 3U) == 1 &&
                 providers.waiter(3, base + 3U) &&
                 providers.detail(3, base + 3U, "detail") == "true|false";
        },
        "ownership executed prefix and caught TypeError barrier missing");
    causal = providers.entries(b, base + 1U) == 0;
    providers.unlock(base + 3U);
  } else {
    own::until(
        [&] {
          return providers.waiter(a, base) && providers.waiter(b, base + 1U) &&
                 active(own::snapshot(generation), 2, 2, {0, 1}, {});
        },
        "ownership reverse-order initial generation missing");
    providers.unlock(base + 1U);
    own::until(
        [&] {
          const auto state = own::snapshot(generation);
          return providers.waiter(a, base) && state.visited &&
                 !state.cancelled && state.cap_only &&
                 std::ranges::find(state.callbacks, 0) !=
                     state.callbacks.end() &&
                 std::ranges::find(state.callbacks, 1) == state.callbacks.end();
        },
        "ownership actual host B-before-A delivery missing");
    if (family == 1) {
      own::until(
          [&] {
            return providers.entries(2, base + 2U) == 1 &&
                   providers.entries(3, base + 3U) == 1 &&
                   providers.waiter(3, base + 3U) && providers.waiter(a, base);
          },
          "ownership first rejection outer-catch ACK missing");
      fixture::demand(providers.detail(2, base + 2U, "code") ==
                              "cap.handler_threw" &&
                          providers.detail(2, base + 2U, "detail") ==
                              "code,message|true|false|false",
                      "ownership ACK did not carry actual native error fields");
      acknowledgement = providers.detail(2, base + 2U, "message");
      fixture::demand(
          acknowledgement.starts_with(providers.domains[b].extension +
                                      ":second: promise_rejected:") &&
              acknowledgement.size() <= 1024 &&
              acknowledgement.find(marker(family, variant, replay, 1)) !=
                  std::string::npos &&
              acknowledgement.find(marker(family, variant, replay, 0)) ==
                  std::string::npos,
          "ownership ACK was not first temporal B rejection");
    }
    causal = true;
    providers.unlock(base);
    if (family == 1) providers.unlock(base + 3U);
  }
  auto result = run.finish();
  const bool retired = own::pristine(*caller.context) &&
                       plinth::lifecycle::async_tasks().drain(5s) &&
                       plinth::log::wait_for_audits(5s);
  const auto shutdown = caller.finish();
  Cell cell{family,
            variant,
            replay,
            std::move(result),
            shutdown,
            causal,
            retired,
            true,
            providers.total() - before,
            0,
            0,
            0,
            0,
            acknowledgement,
            family == 1
                ? providers.domains[b].extension + ":second: promise_rejected:"
                : "",
            {}};
  record(cell, observations, providers);
  return cell;
}

auto recovery(own::Providers& providers, own::Caller& caller, unsigned receipt)
    -> bool {
  stage(providers, 0, "recover", receipt, "recovered", false);
  own::Run run(
      providers, *caller.context,
      "(async()=>{if(Object.prototype.hasOwnProperty.call(globalThis,'__"
      "ownedMarker'))throw new Error('old generation state');return await " +
          own::invocation(providers, 0, "recover") + ";})()");
  const auto result = run.finish();
  return result.value.has_value() && result.value->isString() &&
         result.value->asString() == "recovered" &&
         providers.entries(0, receipt) == 1 && own::pristine(*caller.context);
}

auto lifetime(own::Providers& providers, unsigned phase, unsigned route,
              unsigned replay) -> Cell {
  const unsigned arity = route == 0 ? 1U : 2U;
  const unsigned base = 300U + (phase * 2U + route) * 10U;
  const auto before = providers.total();
  own::Caller caller(providers.config);
  own::Observations observations;
  std::vector<std::pair<unsigned, std::string>> functions;
  for (unsigned index = 0; index < arity; ++index) {
    const auto function = "life" + std::to_string(index);
    stage(providers, index, function, base + index,
          marker(phase + 4U, route, replay, index), phase == 1 || phase == 3);
    functions.emplace_back(index, function);
  }
  const auto operation = route == 0 ? own::invocation(providers, 0, "life0")
                                    : batch(providers, functions);
  const auto source =
      "(async()=>{globalThis.__ownedMarker='kept';const value=await " +
      operation +
      ";return {value,marked:globalThis.__ownedMarker==='kept'};})()";
  if (phase == 2)
    caller.context->cancelled.store(true, std::memory_order_release);
  own::Run run(providers, *caller.context, source);
  bool causal = false;
  own::Generation generation;
  if (phase == 1 || phase == 3) {
    generation = caller_generation(observations);
    own::until(
        [&] {
          bool waiters = true;
          for (unsigned index = 0; index < arity; ++index)
            waiters = waiters && providers.waiter(index, base + index) &&
                      providers.entries(index, base + index) == 1;
          return waiters &&
                 active(own::snapshot(generation), static_cast<int>(arity),
                        static_cast<int>(arity),
                        route == 0 ? std::vector<int>{0}
                                   : std::vector<int>{0, 1},
                        {});
        },
        "ownership held-generation admission missing");
    if (phase == 1) {
      caller.pool.rebuild();
      const auto after = own::snapshot(generation);
      causal =
          caller.pool.active_count() == 1 && caller.pool.free_count() == 1 &&
          own::same_generation(generation, observations.caller()) &&
          active(after, static_cast<int>(arity), static_cast<int>(arity),
                 route == 0 ? std::vector<int>{0} : std::vector<int>{0, 1}, {});
      for (unsigned index = 0; index < arity; ++index)
        providers.unlock(base + index);
    } else {
      bool cancelled = false;
      {
        auto current = generation.lock();
        cancelled = current && current->request_cancel();
      } // Never retain this strong generation across return/drain assertions.
      fixture::demand(cancelled,
                      "ownership admitted caller cancellation refused");
    }
  }
  auto result = run.finish();
  const int primary_admitted = caller.context->next_callback_id;
  bool retired = own::pristine(*caller.context, phase == 3);
  bool recovered = false;
  if (phase == 0) {
    const auto address = reinterpret_cast<std::uintptr_t>(caller.context);
    retired =
        retired && !caller.context->cancelled.load(std::memory_order_acquire);
    caller.pool.release(caller.context);
    caller.context = nullptr;
    const bool returned = caller.pool.active_count() == 0;
    caller.acquire();
    causal =
        returned && reinterpret_cast<std::uintptr_t>(caller.context) == address;
  } else if (phase == 1) {
    caller.destroy();
    caller.acquire();
  } else if (phase == 2) {
    causal = primary_admitted == 0;
    for (unsigned index = 0; index < arity; ++index)
      causal = causal && providers.entries(index, base + index) == 0;
    caller.destroy();
    caller.acquire();
  } else {
    // Physical native ownership is separate from a logically timed-out callee.
    // No propagation/nonpropagation expectation is imposed on the callee flag.
    causal = !result.value.has_value() &&
             result.value.error().kind == EvalErrorKind::CANCELLED &&
             !own::snapshot(generation).visited &&
             !plinth::lifecycle::async_tasks().drain(0ms) &&
             plinth::lifecycle::async_tasks().active_for_test() > 0;
    for (unsigned index = 0; index < arity; ++index)
      causal = causal && providers.waiter(index, base + index);
    retired =
        retired && caller.context->inflight_detached.load(
                       std::memory_order_acquire) <= static_cast<int>(arity);
    caller.destroy();
    caller.acquire();
    own::Run replacement(providers, *caller.context,
                         "(()=>{if(Object.prototype.hasOwnProperty.call("
                         "globalThis,'__ownedMarker'))throw new Error('retired "
                         "generation leaked');return 42;})()");
    const auto fresh = replacement.finish();
    causal = causal && fresh.value.has_value() && fresh.value->isNumeric() &&
             fresh.value->asDouble() == 42 && own::pristine(*caller.context);
    for (unsigned index = 0; index < arity; ++index)
      providers.unlock(base + index);
  }
  fixture::demand(plinth::lifecycle::async_tasks().drain(5s) &&
                      plinth::log::wait_for_audits(5s),
                  "ownership accepted native work failed physical drain");
  recovered = recovery(providers, caller, base + 8U);
  retired =
      retired && primary_admitted == (phase == 2 ? 0 : static_cast<int>(arity));
  const auto shutdown = caller.finish();
  Cell cell{phase + 4U,
            route,
            replay,
            std::move(result),
            shutdown,
            causal,
            retired,
            recovered,
            providers.total() - before,
            0,
            0,
            0,
            0,
            {},
            {},
            {}};
  record(cell, observations, providers);
  return cell;
}

auto matches(const Cell& cell) -> bool {
  if (cell.family >= 8 || cell.variant >= 2 || cell.replay >= 2 ||
      !cell.causal || !cell.retired || !cell.recovered ||
      !fixture::stopped(cell.shutdown))
    return false;
  const unsigned arity = cell.variant == 0 ? 1U : 2U;
  static constexpr std::array<int, 4> ORDER_CAPS{2, 4, 2, 5};
  static constexpr std::array<int, 4> ORDER_DATABASE{4, 7, 3, 10};
  const auto caps = cell.family < 4    ? ORDER_CAPS[cell.family]
                    : cell.family == 6 ? 1
                                       : static_cast<int>(arity + 1);
  const auto database =
      cell.family < 4 ? ORDER_DATABASE[cell.family]
      : cell.family == 6
          ? 1
          : static_cast<int>((cell.family == 4 ? arity : arity * 2U) + 1U);
  if (cell.caps != caps || cell.database != database ||
      cell.writes != static_cast<unsigned>(caps) ||
      cell.native_generations != static_cast<unsigned>(caps) ||
      cell.caller_generations != (cell.family < 4    ? 1U
                                  : cell.family == 7 ? 3U
                                                     : 2U))
    return false;
  if (cell.family == 6 || cell.family == 7)
    return !cell.result.value.has_value() &&
           cell.result.value.error().kind == EvalErrorKind::CANCELLED;
  if (!cell.result.value.has_value()) return false;
  const auto& value = *cell.result.value;
  if (cell.family == 1)
    return value.isObject() && value.size() == 3 && value["code"].isString() &&
           value["code"].asString() == "cap.handler_threw" &&
           value["message"].isString() &&
           value["message"].asString() == cell.acknowledgement &&
           !cell.rejection_prefix.empty() &&
           cell.acknowledgement.starts_with(cell.rejection_prefix) &&
           cell.acknowledgement.size() <= 1024 &&
           cell.acknowledgement.find(marker(1, cell.variant, cell.replay, 1)) !=
               std::string::npos &&
           cell.acknowledgement.find(marker(1, cell.variant, cell.replay, 0)) ==
               std::string::npos &&
           value["detail"].isString() &&
           value["detail"].asString() == "code,message|true|false|false";
  if (cell.family == 2)
    return value.isObject() && value.size() == 3 && value["code"].isString() &&
           value["code"].asString().empty() && value["message"].isString() &&
           value["message"].asString() ==
               (cell.variant == 0
                    ? "cap.batch: each call must be a [signature, args?] array"
                    : "cap.batch: each call must have 1 or 2 elements "
                      "[signature, args?]") &&
           value["detail"].isString() &&
           value["detail"].asString() == "true|false";
  const auto* payload = &value;
  unsigned count = cell.family == 3 ? 5U : cell.family == 0 ? 2U : arity;
  if (cell.family >= 4) {
    if (!value.isObject() || value.size() != 2 || !value["marked"].isBool() ||
        !value["marked"].asBool() || !value.isMember("value"))
      return false;
    payload = &value["value"];
  }
  if (cell.family >= 4 && cell.variant == 0)
    return payload->isString() &&
           payload->asString() ==
               marker(cell.family, cell.variant, cell.replay, 0);
  if (!payload->isArray() || payload->size() != count) return false;
  for (unsigned index = 0; index < count; ++index) {
    if (!(*payload)[index].isString() ||
        (*payload)[index].asString() !=
            marker(cell.family, cell.variant, cell.replay, index))
      return false;
  }
  return true;
}

auto ledger(const std::vector<Cell>& cells, const std::vector<Epoch>& epochs,
            bool ordering_case) -> bool {
  if (cells.size() != 16 || epochs.size() != 2) return false;
  std::array<unsigned, 16> seen{};
  std::array<unsigned, 2> owners{};
  unsigned caps = 0;
  unsigned database = 0;
  unsigned writes = 0;
  for (const auto& cell : cells) {
    if (!matches(cell) || (ordering_case ? cell.family >= 4 : cell.family < 4))
      return false;
    const auto family = ordering_case ? cell.family : cell.family - 4U;
    if (++seen[(cell.replay * 4U + family) * 2U + cell.variant] != 1)
      return false;
    caps += static_cast<unsigned>(cell.caps);
    database += static_cast<unsigned>(cell.database);
    writes += cell.writes;
  }
  for (const auto count : seen)
    if (count != 1) return false;
  for (const auto& epoch : epochs) {
    if (epoch.replay >= 2 || ++owners[epoch.replay] != 1 || !epoch.closed ||
        epoch.warm_caps != 4 || epoch.warm_writes != 4 ||
        epoch.receipts != (ordering_case ? 30U : 21U))
      return false;
  }
  return owners[0] == 1 && owners[1] == 1 &&
         caps == (ordering_case ? 52U : 34U) &&
         database == (ordering_case ? 96U : 46U) && writes == caps;
}

auto exercise(bool ordering_case) -> void {
  const auto deadline = own::Clock::now() + 55s;
  fixture::initialize();
  std::vector<Cell> cells;
  std::vector<Epoch> epochs;
  cells.reserve(16);
  epochs.reserve(2);
  for (unsigned replay = 0; replay < 2; ++replay) {
    own::Providers providers;
    const unsigned warm_caps = warm(providers);
    const unsigned warm_writes = providers.total();
    for (unsigned family = 0; family < 4; ++family) {
      for (unsigned variant = 0; variant < 2; ++variant) {
        fixture::demand(own::Clock::now() < deadline,
                        "ownership corpus collection wall budget");
        cells.push_back(ordering_case
                            ? ordering(providers, family, variant, replay)
                            : lifetime(providers, family, variant, replay));
      }
    }
    const unsigned receipts = providers.total();
    const bool closed = providers.finish();
    epochs.push_back({replay, closed, warm_caps, warm_writes, receipts});
  }
  // All callers/registry owners/private clients/observers/gates have actually
  // finished before Catch assertions or copied-observation corruption controls.
  // Helper-only controls: cheap polls must not replace the wall-clock budget,
  // and an expired budget must still fail even for an immediately true probe.
  unsigned polls = 0;
  own::until([&] { return ++polls == 2001; },
             "ownership poll-count control timed out");
  REQUIRE(polls == 2001);
  CHECK_THROWS_WITH(
      own::until([] { return true; }, "ownership expired-budget control", 0ms),
      "ownership expired-budget control");
  REQUIRE(own::Clock::now() < deadline);
  REQUIRE(ledger(cells, epochs, ordering_case));
  unsigned controls = 0;
  for (const auto& cell : cells) {
    INFO("ownership family=" << cell.family << " variant=" << cell.variant
                             << " replay=" << cell.replay);
    REQUIRE(matches(cell));
    for (unsigned field = 0; field < 10; ++field) {
      auto wrong = cell;
      switch (field) {
        case 0: wrong.causal = false; break;
        case 1: wrong.retired = false; break;
        case 2: wrong.recovered = false; break;
        case 3: ++wrong.caps; break;
        case 4: ++wrong.database; break;
        case 5: ++wrong.writes; break;
        case 6: ++wrong.caller_generations; break;
        case 7: ++wrong.native_generations; break;
        case 8: wrong.shutdown.idempotent = false; break;
        default: wrong.result.value = Json::Value{false}; break;
      }
      CHECK_FALSE(matches(wrong));
      ++controls;
    }
  }
  REQUIRE(controls == 160);
  auto missing = cells;
  missing.pop_back();
  CHECK_FALSE(ledger(missing, epochs, ordering_case));
  auto duplicate = cells;
  duplicate.back() = duplicate.front();
  CHECK_FALSE(ledger(duplicate, epochs, ordering_case));
  auto invalid = cells;
  invalid.front().family = 8;
  CHECK_FALSE(ledger(invalid, epochs, ordering_case));
  auto missing_owner = epochs;
  missing_owner.pop_back();
  CHECK_FALSE(ledger(cells, missing_owner, ordering_case));
  auto duplicate_owner = epochs;
  duplicate_owner.back() = duplicate_owner.front();
  CHECK_FALSE(ledger(cells, duplicate_owner, ordering_case));
  auto unclosed = epochs;
  unclosed.front().closed = false;
  CHECK_FALSE(ledger(cells, unclosed, ordering_case));
  REQUIRE(own::Clock::now() < deadline);
}

} // namespace

TEST_CASE("Native capability batches retain causal ordering and bounded fanout",
          "[js][async][cap][corpus][native-capability-ordering]") {
  exercise(true);
}

TEST_CASE("Native capability callers retain bounded generation ownership",
          "[js][async][cap][corpus][native-capability-ownership]") {
  exercise(false);
}
