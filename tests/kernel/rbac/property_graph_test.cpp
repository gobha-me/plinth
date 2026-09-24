// SPDX-License-Identifier: MIT

#include "property_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>

namespace rp = plinth::rbac_property;

TEST_CASE("RBAC property graph generator is stable and structurally valid",
          "[rbac][property][pure]") {
  std::set<std::string> distinct;
  std::array<bool, 6> seen_kinds{};
  for (const auto seed : rp::SEEDS) {
    const auto one = rp::generate(seed);
    const auto two = rp::generate(seed);
    REQUIRE(one == two);
    CHECK(one.seed == seed);
    CHECK(one.initial.membership[rp::EVERYONE]);
    CHECK(one.initial.grants[rp::ADMIN][rp::KERNEL_ADMIN]);
    CHECK_FALSE(one.initial.orphan[rp::KERNEL_ADMIN]);
    CHECK_FALSE(one.operations.empty());
    distinct.insert(rp::describe(one));

    auto state = one.initial;
    for (const auto& op : one.operations) {
      seen_kinds[static_cast<std::size_t>(op.kind)] = true;
      rp::apply(state, op);
      CHECK(state.membership[rp::EVERYONE]);
      CHECK(state.grants[rp::ADMIN][rp::KERNEL_ADMIN]);
      CHECK_FALSE(state.orphan[rp::KERNEL_ADMIN]);
    }
    CHECK(state == rp::after(one));
  }
  CHECK(distinct.size() == rp::SEEDS.size());
  for (const bool seen : seen_kinds) {
    CHECK(seen);
  }
}

TEST_CASE("RBAC property oracle honors virtual everyone and group union",
          "[rbac][property][pure]") {
  rp::Graph graph;
  graph.required[2] = true;
  CHECK_FALSE(rp::allows(graph));

  // No materialized everyone membership is needed for its grants.
  graph.grants[rp::EVERYONE][2] = true;
  CHECK(rp::effective(graph)[2]);
  CHECK(rp::allows(graph));

  graph.grants[rp::EVERYONE][2] = false;
  graph.grants[2][2] = true;
  graph.grants[3][3] = true;
  graph.membership[2] = true;
  CHECK(rp::effective(graph)[2]);
  CHECK_FALSE(rp::effective(graph)[3]);
  rp::apply(graph, {rp::OpKind::ADD_MEMBERSHIP, 3, 0});
  CHECK(rp::effective(graph)[3]);
  CHECK(rp::allows(graph));

  rp::apply(graph, {rp::OpKind::REMOVE_MEMBERSHIP, 2, 0});
  CHECK_FALSE(rp::effective(graph)[2]);
  CHECK_FALSE(rp::allows(graph));
  rp::apply(graph, {rp::OpKind::ADD_GRANT, 3, 2});
  CHECK(rp::allows(graph));
  rp::apply(graph, {rp::OpKind::REMOVE_GRANT, 3, 2});
  CHECK_FALSE(rp::allows(graph));
}

TEST_CASE("RBAC property oracle filters orphaned rules and grants admin",
          "[rbac][property][pure]") {
  rp::Graph graph;
  graph.required[4] = true;
  graph.membership[2] = true;
  graph.grants[2][4] = true;
  CHECK(rp::allows(graph));

  rp::apply(graph, {rp::OpKind::ORPHAN_RULE, 0, 4});
  CHECK(graph.grants[2][4]); // lifecycle keeps the grant for re-enable
  CHECK_FALSE(rp::effective(graph)[4]);
  CHECK_FALSE(rp::allows(graph));
  rp::apply(graph, {rp::OpKind::CLEAR_ORPHAN, 0, 4});
  CHECK(rp::allows(graph));

  graph.membership[rp::ADMIN] = true;
  graph.grants[2][4] = false;
  CHECK(rp::effective(graph)[rp::KERNEL_ADMIN]);
  CHECK(rp::allows(graph)); // kernel.admin matches any nonempty requirement

  graph.required[4] = false;
  graph.membership[rp::ADMIN] = false;
  CHECK(rp::allows(graph)); // authenticated-only route
}

TEST_CASE("RBAC property operations reject structural-invalid mutations",
          "[rbac][property][pure]") {
  rp::Graph graph;
  CHECK_THROWS_AS(
      rp::apply(graph, {rp::OpKind::REMOVE_MEMBERSHIP, rp::EVERYONE, 0}),
      std::out_of_range);
  CHECK_THROWS_AS(
      rp::apply(graph, {rp::OpKind::REMOVE_GRANT, rp::ADMIN, rp::KERNEL_ADMIN}),
      std::out_of_range);
  CHECK_THROWS_AS(
      rp::apply(graph, {rp::OpKind::ORPHAN_RULE, 0, rp::KERNEL_ADMIN}),
      std::out_of_range);
  CHECK(graph.membership[rp::EVERYONE]);
  CHECK(graph.grants[rp::ADMIN][rp::KERNEL_ADMIN]);
  CHECK_FALSE(graph.orphan[rp::KERNEL_ADMIN]);
}

TEST_CASE("RBAC property shrinker deterministically removes irrelevant state",
          "[rbac][property][pure]") {
  rp::Case original;
  original.seed = 89;
  original.initial.membership[2] = true;
  original.initial.membership[3] = true;
  original.initial.grants[2][1] = true;
  original.initial.grants[3][3] = true;
  original.initial.orphan[1] = true;
  original.initial.required[1] = true;
  original.initial.required[2] = true;
  original.operations = {
      {rp::OpKind::ADD_MEMBERSHIP, 2, 0},
      {rp::OpKind::ORPHAN_RULE, 0, 2},
      {rp::OpKind::ADD_GRANT, 3, 4},
      {rp::OpKind::REMOVE_MEMBERSHIP, 3, 0},
  };
  const auto fails = [](const rp::Case& test_case) {
    return test_case.initial.required[2] &&
           std::ranges::any_of(
               test_case.operations, [](const rp::Operation& op) {
                 return op.kind == rp::OpKind::ORPHAN_RULE && op.rule == 2;
               });
  };

  const auto one = rp::shrink(original, fails);
  const auto two = rp::shrink(original, fails);
  REQUIRE(one.minimized == two.minimized);
  CHECK(one.checks == two.checks);
  REQUIRE(one.minimized.operations.size() == 1);
  CHECK(one.minimized.operations[0] ==
        rp::Operation{rp::OpKind::ORPHAN_RULE, 0, 2});
  CHECK(one.minimized.initial.required[2]);
  CHECK_FALSE(one.minimized.initial.required[1]);
  CHECK(one.minimized.initial.membership[rp::EVERYONE]);
  CHECK(one.minimized.initial.grants[rp::ADMIN][rp::KERNEL_ADMIN]);
  CHECK(one.minimized.seed == 89);
  CHECK(rp::describe(one.minimized).find("seed=89") != std::string::npos);

  const auto limited = rp::shrink(original, fails, 1);
  CHECK(limited.minimized == original);
  CHECK(limited.checks == 1);
  const auto not_failing =
      rp::shrink(original, [](const rp::Case&) { return false; });
  CHECK(not_failing.minimized == original);
  CHECK(not_failing.checks == 1);
}
