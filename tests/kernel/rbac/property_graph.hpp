// SPDX-License-Identifier: MIT
#pragma once

// Small, deterministic RBAC model for generated tests. This is deliberately
// independent of PostgreSQL queries and production permission helpers: tests
// compare those implementations with the graph oracle below.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::rbac_property {

inline constexpr std::size_t GROUP_COUNT = 4;
inline constexpr std::size_t RULE_COUNT = 5;
inline constexpr std::size_t EVERYONE = 0;
inline constexpr std::size_t ADMIN = 1;
inline constexpr std::size_t KERNEL_ADMIN = 0;

inline constexpr std::array<std::string_view, RULE_COUNT> RULE_NAMES{
    "kernel.admin", "property89.rule1", "property89.rule2", "property89.rule3",
    "property89.rule4"};

// Fixed corpus checked in so a failed CI run can be reproduced exactly. The
// generator does not depend on std::uniform_int_distribution, whose mapping
// from engine output is not specified across standard libraries.
inline constexpr std::array<std::uint64_t, 12> SEEDS{
    0x0000000000000001ULL, 0x0000000000000059ULL, 0x0000000000002026ULL,
    0x00000000000089abULL, 0x6b1d4f72a5c093e8ULL, 0x873e44aa7f56303bULL,
    0x1ebabc934657028dULL, 0xd84081f603cbad12ULL, 0x746eca79e2fd0f3bULL,
    0xf1a89e1074ce05bdULL, 0x27643e74ce8274afULL, 0xffffffffffffffffULL};

// One authenticated target user. everyone membership is virtual and always
// present. The bootstrap admin group keeps its kernel.admin grant; tests can
// vary whether the target belongs to admin and can grant kernel.admin through
// other groups. A true required bit denotes an OR-list route requirement.
struct Graph {
  std::array<bool, GROUP_COUNT> membership{};
  std::array<std::array<bool, RULE_COUNT>, GROUP_COUNT> grants{};
  std::array<bool, RULE_COUNT> orphan{};
  std::array<bool, RULE_COUNT> required{};

  Graph() {
    membership[EVERYONE] = true;
    grants[ADMIN][KERNEL_ADMIN] = true;
  }

  friend auto operator==(const Graph&, const Graph&) -> bool = default;
};

enum class OpKind : std::uint8_t {
  ADD_MEMBERSHIP,
  REMOVE_MEMBERSHIP,
  ADD_GRANT,
  REMOVE_GRANT,
  ORPHAN_RULE,
  CLEAR_ORPHAN,
};

// Unused index is zero: membership operations use only group, orphan
// operations use only rule, and grant operations use both.
struct Operation {
  OpKind kind{};
  std::size_t group{0};
  std::size_t rule{0};

  friend auto operator==(const Operation&, const Operation&) -> bool = default;
};

struct Case {
  std::uint64_t seed{0};
  Graph initial;
  std::vector<Operation> operations;

  friend auto operator==(const Case&, const Case&) -> bool = default;
};

// SplitMix64 gives a fully specified, reproducible byte stream. This model
// is a test fixture, not a source of cryptographic randomness.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed) {}

  [[nodiscard]] auto next() -> std::uint64_t {
    state_ += 0x9e3779b97f4a7c15ULL;
    auto z = state_;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }

  [[nodiscard]] auto pick(std::size_t ceiling) -> std::size_t {
    if (ceiling == 0) {
      throw std::invalid_argument("RBAC generator ceiling must be positive");
    }
    return static_cast<std::size_t>(next() % ceiling);
  }

 private:
  std::uint64_t state_;
};

[[nodiscard]] inline auto generate(std::uint64_t seed) -> Case {
  Generator rng{seed};
  Case out;
  out.seed = seed;

  for (std::size_t group = 1; group < GROUP_COUNT; ++group) {
    out.initial.membership[group] = rng.pick(2) == 1;
  }
  for (std::size_t group = 0; group < GROUP_COUNT; ++group) {
    for (std::size_t rule = 0; rule < RULE_COUNT; ++rule) {
      if (group == ADMIN && rule == KERNEL_ADMIN) {
        continue;
      }
      // Sparse edges make both allow and deny states common.
      out.initial.grants[group][rule] =
          rng.pick(rule == KERNEL_ADMIN ? 8 : 3) == 0;
    }
  }
  for (std::size_t rule = 1; rule < RULE_COUNT; ++rule) {
    out.initial.orphan[rule] = rng.pick(4) == 0;
  }
  for (std::size_t rule = 0; rule < RULE_COUNT; ++rule) {
    out.initial.required[rule] = rng.pick(3) == 0;
  }
  if (!std::ranges::any_of(out.initial.required,
                           [](bool bit) { return bit; })) {
    out.initial.required[1 + rng.pick(RULE_COUNT - 1)] = true;
  }

  const auto steps = 8 + rng.pick(9);
  out.operations.reserve(steps);
  for (std::size_t step = 0; step < steps; ++step) {
    const auto kind = static_cast<OpKind>(rng.pick(6));
    Operation op{.kind = kind};
    switch (kind) {
      case OpKind::ADD_MEMBERSHIP:
      case OpKind::REMOVE_MEMBERSHIP:
        op.group = 1 + rng.pick(GROUP_COUNT - 1);
        break;
      case OpKind::ADD_GRANT:
      case OpKind::REMOVE_GRANT:
        op.group = rng.pick(GROUP_COUNT);
        op.rule = rng.pick(RULE_COUNT);
        if (op.group == ADMIN && op.rule == KERNEL_ADMIN) {
          op.rule = 1 + rng.pick(RULE_COUNT - 1);
        }
        break;
      case OpKind::ORPHAN_RULE:
      case OpKind::CLEAR_ORPHAN: op.rule = 1 + rng.pick(RULE_COUNT - 1); break;
    }
    out.operations.push_back(op);
  }
  return out;
}

inline auto apply(Graph& graph, const Operation& op) -> void {
  switch (op.kind) {
    case OpKind::ADD_MEMBERSHIP:
    case OpKind::REMOVE_MEMBERSHIP:
      if (op.group == EVERYONE || op.group >= GROUP_COUNT) {
        throw std::out_of_range("cannot mutate virtual everyone membership");
      }
      graph.membership[op.group] = op.kind == OpKind::ADD_MEMBERSHIP;
      return;
    case OpKind::ADD_GRANT:
    case OpKind::REMOVE_GRANT:
      if (op.group >= GROUP_COUNT || op.rule >= RULE_COUNT ||
          (op.group == ADMIN && op.rule == KERNEL_ADMIN)) {
        throw std::out_of_range("invalid mutable RBAC grant");
      }
      graph.grants[op.group][op.rule] = op.kind == OpKind::ADD_GRANT;
      return;
    case OpKind::ORPHAN_RULE:
    case OpKind::CLEAR_ORPHAN:
      if (op.rule == KERNEL_ADMIN || op.rule >= RULE_COUNT) {
        throw std::out_of_range("cannot orphan kernel.admin");
      }
      graph.orphan[op.rule] = op.kind == OpKind::ORPHAN_RULE;
      return;
  }
  throw std::invalid_argument("unknown RBAC property operation");
}

[[nodiscard]] inline auto after(const Case& test_case) -> Graph {
  Graph result = test_case.initial;
  for (const auto& op : test_case.operations) {
    apply(result, op);
  }
  return result;
}

[[nodiscard]] inline auto effective(const Graph& graph)
    -> std::array<bool, RULE_COUNT> {
  std::array<bool, RULE_COUNT> rules{};
  for (std::size_t group = 0; group < GROUP_COUNT; ++group) {
    if (!graph.membership[group]) {
      continue;
    }
    for (std::size_t rule = 0; rule < RULE_COUNT; ++rule) {
      rules[rule] =
          rules[rule] || (graph.grants[group][rule] && !graph.orphan[rule]);
    }
  }
  return rules;
}

[[nodiscard]] inline auto allows(const Graph& graph) -> bool {
  const auto rules = effective(graph);
  bool has_requirement = false;
  for (std::size_t rule = 0; rule < RULE_COUNT; ++rule) {
    if (graph.required[rule]) {
      has_requirement = true;
      if (rules[rule]) {
        return true;
      }
    }
  }
  // A route without required rules is authenticated-only. Otherwise the
  // explicit kernel.admin rule is a universal match within RBAC.
  return !has_requirement || rules[KERNEL_ADMIN];
}

[[nodiscard]] inline auto operation_name(OpKind kind) -> std::string_view {
  switch (kind) {
    case OpKind::ADD_MEMBERSHIP: return "add_member";
    case OpKind::REMOVE_MEMBERSHIP: return "remove_member";
    case OpKind::ADD_GRANT: return "add_grant";
    case OpKind::REMOVE_GRANT: return "remove_grant";
    case OpKind::ORPHAN_RULE: return "orphan";
    case OpKind::CLEAR_ORPHAN: return "clear_orphan";
  }
  return "unknown";
}

template <std::size_t N>
inline auto append_bits(std::ostringstream& out,
                        const std::array<bool, N>& bits) -> void {
  for (const bool bit : bits) {
    out << (bit ? '1' : '0');
  }
}

[[nodiscard]] inline auto describe(const Case& test_case) -> std::string {
  std::ostringstream out;
  out << "seed=" << test_case.seed << " members=";
  append_bits(out, test_case.initial.membership);
  out << " grants=";
  for (const auto& group : test_case.initial.grants) {
    append_bits(out, group);
    out << '/';
  }
  out << " orphan=";
  append_bits(out, test_case.initial.orphan);
  out << " required=";
  append_bits(out, test_case.initial.required);
  out << " ops=[";
  for (std::size_t i = 0; i < test_case.operations.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    const auto& op = test_case.operations[i];
    out << operation_name(op.kind) << '(' << op.group << ',' << op.rule << ')';
  }
  out << ']';
  return out.str();
}

struct ShrinkResult {
  Case minimized;
  std::size_t checks{0};
};

using FailurePredicate = std::function<bool(const Case&)>;

// Greedy deletion, in fixed order, to a locally minimal failing case. Every
// accepted step removes one operation or one optional initial-state bit.
// max_checks bounds expensive production/PG replays in failure handling.
[[nodiscard]] inline auto shrink(Case failing, const FailurePredicate& fails,
                                 std::size_t max_checks = 256) -> ShrinkResult {
  ShrinkResult result{.minimized = std::move(failing)};
  if (max_checks == 0) {
    return result;
  }
  ++result.checks;
  if (!fails(result.minimized)) {
    return result;
  }
  auto try_accept = [&](Case candidate) -> bool {
    if (result.checks >= max_checks) {
      return false;
    }
    ++result.checks;
    if (!fails(candidate)) {
      return false;
    }
    result.minimized = std::move(candidate);
    return true;
  };

  bool changed = true;
  while (changed && result.checks < max_checks) {
    changed = false;
    for (std::size_t i = 0; i < result.minimized.operations.size(); ++i) {
      Case candidate = result.minimized;
      candidate.operations.erase(candidate.operations.begin() +
                                 static_cast<std::ptrdiff_t>(i));
      if (try_accept(std::move(candidate))) {
        changed = true;
        break;
      }
    }
    if (changed) {
      continue;
    }
    for (std::size_t group = 1; group < GROUP_COUNT && !changed; ++group) {
      if (!result.minimized.initial.membership[group]) {
        continue;
      }
      Case candidate = result.minimized;
      candidate.initial.membership[group] = false;
      changed = try_accept(std::move(candidate));
    }
    for (std::size_t group = 0; group < GROUP_COUNT && !changed; ++group) {
      for (std::size_t rule = 0; rule < RULE_COUNT && !changed; ++rule) {
        if ((group == ADMIN && rule == KERNEL_ADMIN) ||
            !result.minimized.initial.grants[group][rule]) {
          continue;
        }
        Case candidate = result.minimized;
        candidate.initial.grants[group][rule] = false;
        changed = try_accept(std::move(candidate));
      }
    }
    for (std::size_t rule = 1; rule < RULE_COUNT && !changed; ++rule) {
      if (!result.minimized.initial.orphan[rule]) {
        continue;
      }
      Case candidate = result.minimized;
      candidate.initial.orphan[rule] = false;
      changed = try_accept(std::move(candidate));
    }
    for (std::size_t rule = 0; rule < RULE_COUNT && !changed; ++rule) {
      if (!result.minimized.initial.required[rule]) {
        continue;
      }
      Case candidate = result.minimized;
      candidate.initial.required[rule] = false;
      changed = try_accept(std::move(candidate));
    }
  }
  return result;
}

} // namespace plinth::rbac_property
