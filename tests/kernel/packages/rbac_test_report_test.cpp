// Pure unit tests for the RbacTestReport shape + JSON round-trip
// (ICD-0.4.7 cases B.01–B.03). No PG, no Drogon.

#include "kernel/packages/rbac_test_runner.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

using plinth::packages::rbac_test::rbac_test_report_from_json;
using plinth::packages::rbac_test::RbacTestReport;
using plinth::packages::rbac_test::rule_outcome_from_json;
using plinth::packages::rbac_test::RuleOutcome;
using plinth::packages::rbac_test::to_json;

namespace {

auto make_pass_outcome(std::string rule, std::string clause) -> RuleOutcome {
  RuleOutcome o;
  o.rule = std::move(rule);
  o.clause = std::move(clause);
  o.expected = "permission_denied";
  o.actual = "permission_denied";
  o.passed = true;
  return o;
}

auto make_fail_outcome(std::string rule, std::string clause) -> RuleOutcome {
  RuleOutcome o;
  o.rule = std::move(rule);
  o.clause = std::move(clause);
  o.expected = "permission_denied";
  o.actual = "success";
  o.passed = false;
  return o;
}

} // namespace

TEST_CASE(
    "B.01 RbacTestReport mixed pass/fail/skipped round-trips through JSON",
    "[rbac_test_report]") {
  RbacTestReport r;
  r.run_id = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  r.package_id = "11111111-2222-3333-4444-555555555555";
  r.package_name = "notes";
  r.package_version = "1.2.3";
  r.started_at = std::chrono::system_clock::time_point{};
  r.duration = std::chrono::milliseconds{134};
  r.passed.push_back(make_pass_outcome("notes.read", "assert_deny"));
  r.passed.push_back(make_pass_outcome("notes.edit", "assert_deny"));
  r.failed.push_back(make_fail_outcome("notes.delete", "assert_deny"));
  r.skipped.emplace_back("notes.admin");

  auto j = to_json(r);
  auto rt = rbac_test_report_from_json(j);

  REQUIRE(rt.run_id == r.run_id);
  REQUIRE(rt.package_id == r.package_id);
  REQUIRE(rt.package_name == r.package_name);
  REQUIRE(rt.package_version == r.package_version);
  REQUIRE(rt.duration == r.duration);
  REQUIRE(rt.passed.size() == 2);
  REQUIRE(rt.failed.size() == 1);
  REQUIRE(rt.skipped == r.skipped);

  REQUIRE(rt.passed[0].rule == "notes.read");
  REQUIRE(rt.passed[0].clause == "assert_deny");
  REQUIRE(rt.failed[0].rule == "notes.delete");

  // overall_passed() follows the failed vector.
  REQUIRE(r.overall_passed() == false);
  REQUIRE(rt.overall_passed() == false);
}

TEST_CASE("B.02 RbacTestReport all-pass boundary: empty failed = overall pass",
          "[rbac_test_report]") {
  RbacTestReport r;
  r.run_id = "run-all-pass";
  r.passed.push_back(make_pass_outcome("a", "assert_deny"));
  r.passed.push_back(make_pass_outcome("b", "assert_deny"));
  r.passed.push_back(make_pass_outcome("c", "assert_deny"));

  REQUIRE(r.overall_passed() == true);
  REQUIRE(r.passed.size() == 3);
  REQUIRE(r.failed.empty());
  REQUIRE(r.skipped.empty());

  auto rt = rbac_test_report_from_json(to_json(r));
  REQUIRE(rt.overall_passed() == true);
  REQUIRE(rt.passed.size() == 3);
}

TEST_CASE("B.03 RbacTestReport only-skipped boundary is still overall pass",
          "[rbac_test_report]") {
  RbacTestReport r;
  r.run_id = "run-only-skipped";
  r.skipped.emplace_back("kernel.admin");
  r.skipped.emplace_back("packages.install");

  REQUIRE(r.overall_passed() == true);
  REQUIRE(r.passed.empty());
  REQUIRE(r.failed.empty());
  REQUIRE(r.skipped.size() == 2);

  auto rt = rbac_test_report_from_json(to_json(r));
  REQUIRE(rt.overall_passed() == true);
  REQUIRE(rt.skipped.size() == 2);
}

TEST_CASE("RuleOutcome JSON round-trip preserves all fields",
          "[rbac_test_report]") {
  RuleOutcome o;
  o.rule = "notes.edit";
  o.clause = "assert_allow";
  o.expected = "success";
  o.actual = "tier3_not_available";
  o.passed = true;
  o.detail["signature"] = "notes:1:edit";

  auto j = to_json(o);
  auto rt = rule_outcome_from_json(j);

  REQUIRE(rt.rule == o.rule);
  REQUIRE(rt.clause == o.clause);
  REQUIRE(rt.expected == o.expected);
  REQUIRE(rt.actual == o.actual);
  REQUIRE(rt.passed == o.passed);
  REQUIRE(rt.detail["signature"].get<std::string>() == "notes:1:edit");
}
