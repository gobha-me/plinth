#include "kernel/packages/validator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

#ifndef CMAKE_SOURCE_DIR
#error "CMAKE_SOURCE_DIR must be defined by the build system"
#endif

namespace fs = std::filesystem;
using plinth::packages::Phase;
using plinth::packages::Severity;
using plinth::packages::ValidationConfig;
using plinth::packages::ValidationReport;

namespace {

auto fixtures_root() -> fs::path {
  return fs::path{CMAKE_SOURCE_DIR} / "tests" / "fixtures" / "packages";
}

auto fixture(std::string_view name) -> fs::path {
  return fixtures_root() / name;
}

auto has_rule(const ValidationReport& r, std::string_view rule) -> bool {
  return std::ranges::any_of(r.messages,
                             [&](const auto& m) { return m.rule == rule; });
}

auto has_phase_rule(const ValidationReport& r, Phase phase,
                    std::string_view rule) -> bool {
  return std::ranges::any_of(r.messages, [&](const auto& m) {
    return m.phase == phase && m.rule == rule;
  });
}

} // namespace

// ─── Baseline ────────────────────────────────────────────────────────

TEST_CASE("cross-file: valid-cross-file passes cleanly",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("valid-cross-file"));
  INFO("messages: " << r.messages.size());
  if (!r.messages.empty()) {
    for (const auto& m : r.messages) {
      UNSCOPED_INFO("  " << m.rule << ": " << m.message);
    }
  }
  REQUIRE(r.disposition() == 0);
}

// ─── CF1 ─────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CF1 rbac-orphan-namespace errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("rbac-orphan-namespace"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "rbac-orphan-namespace"));
}

// ─── CF2 ─────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CF2 rbac-test-bad-call errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("rbac-test-bad-call"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "rbac-test-call"));
}

TEST_CASE("cross-file: CF2 rbac-test-parse-fail errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("rbac-test-parse-fail"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "rbac-test-call-parse"));
}

// ─── CF3 ─────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CF3 panel-missing-client-file errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("panel-missing-client-file"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "panel-missing-client-file"));
}

// ─── CF4 + flag-flip with --structure-only ───────────────────────────

TEST_CASE("cross-file: CF4 handler-missing errors under default",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("cf4-handler-missing"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "cf4-handler-missing"));
  // R4 must be suppressed when cross-file is on.
  REQUIRE_FALSE(has_rule(r, "handler-missing"));
}

TEST_CASE("cross-file: CF4 fixture drops to warning under --structure-only",
          "[packages][cross-file][fixture]") {
  ValidationConfig cfg;
  cfg.cross_file = false;
  auto r = plinth::packages::validate(fixture("cf4-handler-missing"), cfg);
  // R4 is a warning; no cross-file pass ran.
  REQUIRE(r.disposition() == 2);
  REQUIRE(has_rule(r, "handler-missing"));
  REQUIRE_FALSE(has_rule(r, "cf4-handler-missing"));
}

// ─── CF5 ─────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CF5 frontend-mount-reserved-api errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("frontend-mount-reserved-api"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "frontend-mount-reserved"));
}

TEST_CASE("cross-file: CF5 frontend-mount-reserved-ext errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("frontend-mount-reserved-ext"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "frontend-mount-reserved"));
}

// ─── CF6 ─────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CF6 name-reserved-kernel errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("name-reserved-kernel"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "name-reserved"));
}

// ─── CF7 ─────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CF7 capability-namespace-mismatch errors",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("capability-namespace-mismatch"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(
      has_phase_rule(r, Phase::CROSS_FILE, "capability-namespace-mismatch"));
}

// ─── CFW1 ────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CFW1 capability-without-rule warns",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("capability-without-rule"));
  REQUIRE(r.disposition() == 2);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "capability-without-rule"));
}

// ─── CFW2 ────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CFW2 shareable-non-empty warns",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("shareable-non-empty"));
  REQUIRE(r.disposition() == 2);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "shareable-non-empty"));
}

// ─── CFW3 ────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CFW3 entry-imports-client warns",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("entry-imports-client"));
  REQUIRE(r.disposition() == 2);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "entry-imports-client"));
}

// ─── CFW4 ────────────────────────────────────────────────────────────

TEST_CASE("cross-file: CFW4 memory-over-max warns",
          "[packages][cross-file][fixture]") {
  auto r = plinth::packages::validate(fixture("memory-over-max"));
  REQUIRE(r.disposition() == 2);
  REQUIRE(has_phase_rule(r, Phase::CROSS_FILE, "memory-over-max"));
}

// ─── Flag-flip proof ─────────────────────────────────────────────────

TEST_CASE("cross-file: structure-only vs default flip",
          "[packages][cross-file][fixture]") {
  ValidationConfig structure_only;
  structure_only.cross_file = false;
  auto r_struct = plinth::packages::validate(
      fixture("valid-cross-file-structure-only-vs-default"), structure_only);
  REQUIRE(r_struct.disposition() == 0);

  auto r_default = plinth::packages::validate(
      fixture("valid-cross-file-structure-only-vs-default"));
  REQUIRE(r_default.disposition() == 1);
  REQUIRE(
      has_phase_rule(r_default, Phase::CROSS_FILE, "rbac-orphan-namespace"));
}

// ─── Runtime-state stub ──────────────────────────────────────────────

TEST_CASE("cross-file: --against-running-kernel stubs until 0.4.4",
          "[packages][cross-file][runtime-state]") {
  ValidationConfig cfg;
  cfg.against_running_kernel = true;
  cfg.kernel_url = "http://localhost:9999";
  auto r = plinth::packages::validate(fixture("valid-cross-file"), cfg);
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::RUNTIME_STATE,
                         "runtime-validate-unimplemented"));
}

TEST_CASE("cross-file: upgrade_from_id whitelists the RT1 predecessor row",
          "[packages][cross-file][runtime-state]") {
  ValidationConfig cfg;
  cfg.against_running_kernel = true;
  cfg.kernel_url = "http://localhost:9999";
  cfg.upgrade_from_id = "3c7e8b1e-1234-4567-89ab-cdef01234567";
  auto r = plinth::packages::validate(fixture("valid-cross-file"), cfg);
  REQUIRE(r.disposition() == 0);
  REQUIRE_FALSE(has_rule(r, "runtime-validate-unimplemented"));
}

TEST_CASE("cross-file: --against-running-kernel skipped on cross-file errors",
          "[packages][cross-file][runtime-state]") {
  ValidationConfig cfg;
  cfg.against_running_kernel = true;
  cfg.kernel_url = "http://localhost:9999";
  auto r = plinth::packages::validate(fixture("rbac-orphan-namespace"), cfg);
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_phase_rule(r, Phase::RUNTIME_STATE, "runtime-state-skipped"));
  REQUIRE_FALSE(has_rule(r, "runtime-validate-unimplemented"));
}

// ─── Phase field in JSON output ──────────────────────────────────────

TEST_CASE("cross-file: render_json emits phase strings",
          "[packages][cross-file][json]") {
  auto r = plinth::packages::validate(fixture("rbac-orphan-namespace"));
  std::ostringstream os;
  plinth::packages::render_json(r, fixture("rbac-orphan-namespace"), os);
  auto out = os.str();
  REQUIRE(out.find("\"phase\":\"cross_file\"") != std::string::npos);
}
