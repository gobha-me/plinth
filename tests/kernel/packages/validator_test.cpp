#include "kernel/packages/validator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#ifndef CMAKE_SOURCE_DIR
#error "CMAKE_SOURCE_DIR must be defined by the build system"
#endif
#ifndef PLINTH_BINARY_PATH
#error "PLINTH_BINARY_PATH must be defined by the build system"
#endif

namespace fs = std::filesystem;
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

// Create the dynamic fixtures (symlinks for R2 + malformed-UTF-8 binary
// payload for ICD-0.4.1 G.11) if they are not already present. The
// fixtures directory is committed with all static content. The binary
// payload is generated rather than committed because the repo has no
// .gitattributes guarantee of byte fidelity for malformed UTF-8.
auto prepare_dynamic_fixtures() -> void {
  auto symlink_fx = fixture("symlink-outside") / "server";
  fs::create_directories(symlink_fx);
  auto escape = symlink_fx / "escape.js";
  if (!fs::exists(fs::symlink_status(escape))) {
    fs::create_symlink("/etc/hostname", escape);
  }
  auto dotdot_fx = fixture("dotdot-path") / "server";
  fs::create_directories(dotdot_fx);
  auto escape2 = dotdot_fx / "escape.js";
  if (!fs::exists(fs::symlink_status(escape2))) {
    fs::create_symlink("../../../etc/hostname", escape2);
  }
  // ICD-0.4.1 G.11 — malformed-UTF-8 fixture. Overlong NUL (C0 80)
  // mid-source. Always rewrite to guard against an editor "fixing"
  // the file on a stale checkout.
  auto bad_utf8 = fixture("unicode-smuggle-malformed-utf8") / "server" /
                  "handlers" / "shell.js";
  fs::create_directories(bad_utf8.parent_path());
  std::ofstream out{bad_utf8, std::ios::binary | std::ios::trunc};
  out << "// malformed UTF-8 follows: ";
  out.put(static_cast<char>(0xC0)); // overlong lead byte
  out.put(static_cast<char>(0x80)); // continuation
  out << " — not valid UTF-8\n";
}

} // namespace

TEST_CASE("validate: valid-minimal passes", "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("valid-minimal"));
  INFO("messages: " << r.messages.size());
  REQUIRE(r.disposition() == 0);
}

TEST_CASE("validate: valid-full passes", "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("valid-full"));
  REQUIRE(r.disposition() == 0);
}

TEST_CASE("validate: missing-manifest errors",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("missing-manifest"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "required-files"));
}

TEST_CASE("validate: missing-capabilities errors",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("missing-capabilities"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "required-files"));
}

TEST_CASE("validate: symlink-outside errors",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("symlink-outside"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "forbidden-paths"));
}

TEST_CASE("validate: dotdot-path errors", "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("dotdot-path"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "forbidden-paths"));
}

TEST_CASE("validate: stray-file errors", "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("stray-file"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "forbidden-paths"));
}

TEST_CASE("validate: bad-manifest-json errors",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("bad-manifest-json"));
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "manifest.json.parse_error"));
}

TEST_CASE("validate: bad-capabilities-json errors",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("bad-capabilities-json"));
  REQUIRE(r.disposition() == 1);
  // The capabilities parser reports a structured rule; either a
  // function.missing or a per-rule namespaced entry suffices.
  bool structural = std::ranges::any_of(r.messages, [](const auto& m) {
    return m.rule.starts_with("capabilities.");
  });
  REQUIRE(structural);
}

TEST_CASE("validate: handler-missing warns only under --structure-only",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  ValidationConfig cfg;
  cfg.cross_file = false; // 0.4.2 suppresses R4 when cross-file is on
  auto r = plinth::packages::validate(fixture("handler-missing"), cfg);
  REQUIRE(r.disposition() == 2);
  REQUIRE(has_rule(r, "handler-missing"));
}

TEST_CASE("validate: panel-missing errors under --structure-only",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  ValidationConfig cfg;
  cfg.cross_file = false; // CF3 takes over when cross-file is on
  auto r = plinth::packages::validate(fixture("panel-missing"), cfg);
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "panel-missing"));
}

TEST_CASE("validate: oversize fails against tight size limit",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  // Fixture content is ~240 bytes; a 100-byte limit triggers R6.
  ValidationConfig cfg;
  cfg.max_size_bytes = 100;
  auto r = plinth::packages::validate(fixture("oversize"), cfg);
  REQUIRE(r.disposition() == 1);
  REQUIRE(has_rule(r, "size-limit"));
}

TEST_CASE("validate: oversize-tunable passes with a larger limit",
          "[packages][validator][fixture]") {
  prepare_dynamic_fixtures();
  ValidationConfig cfg;
  cfg.max_size_bytes = 100UL * 1024UL; // 100 KB; easily fits fixture.
  auto r = plinth::packages::validate(fixture("oversize-tunable"), cfg);
  REQUIRE(r.disposition() == 0);
}

// --- JSON renderer round-trip ---
TEST_CASE("render_json emits valid JSON matching the ICD contract",
          "[packages][validator][render]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("valid-minimal"));
  std::ostringstream os;
  plinth::packages::render_json(r, fixture("valid-minimal"), os);
  auto j = nlohmann::json::parse(os.str());
  REQUIRE(j["exit_code"] == 0);
  REQUIRE(j["messages"].is_array());
  REQUIRE(j.contains("path"));
  REQUIRE(j.contains("files_scanned"));
  REQUIRE(j.contains("total_bytes"));
}

// --- CLI exit codes via popen() ---
namespace {

struct CliResult {
  int exit_code = -1;
  std::string stdout_buf;
};

// integration-testing a CLI binary's exit code + stdout; the command is built
// from compile-time constants + fixture paths under our control, not user
// input.
auto run_cli(const std::string& args) -> CliResult {
  std::string cmd = PLINTH_BINARY_PATH;
  cmd += " ";
  cmd += args;
  cmd += " 2>/dev/null";
  CliResult r;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return r;
  }
  std::array<char, 4096> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) !=
         nullptr) {
    r.stdout_buf.append(buf.data());
  }
  int status = ::pclose(pipe);
  if (WIFEXITED(status)) {
    r.exit_code = WEXITSTATUS(status);
  }
  return r;
}

} // namespace

TEST_CASE("CLI: validate exits 0 on valid-minimal",
          "[packages][validator][cli]") {
  prepare_dynamic_fixtures();
  auto res = run_cli("validate --quiet " + fixture("valid-minimal").string());
  REQUIRE(res.exit_code == 0);
}

TEST_CASE("CLI: validate exits 1 on missing-manifest",
          "[packages][validator][cli]") {
  prepare_dynamic_fixtures();
  auto res =
      run_cli("validate --quiet " + fixture("missing-manifest").string());
  REQUIRE(res.exit_code == 1);
}

TEST_CASE("CLI: validate exits 2 on handler-missing with --structure-only",
          "[packages][validator][cli]") {
  prepare_dynamic_fixtures();
  // 0.4.2: the default cross-file mode upgrades R4's warning to a
  // CF4 error; the 0.4.0 warning-only disposition is still exercised
  // by the --structure-only flag.
  auto res = run_cli("validate --quiet --structure-only " +
                     fixture("handler-missing").string());
  REQUIRE(res.exit_code == 2);
}

TEST_CASE("CLI: --json output parses as JSON", "[packages][validator][cli]") {
  prepare_dynamic_fixtures();
  auto res = run_cli("validate --json " + fixture("valid-minimal").string());
  REQUIRE(res.exit_code == 0);
  auto j = nlohmann::json::parse(res.stdout_buf);
  REQUIRE(j["exit_code"] == 0);
}

// ─── ICD-0.4.1 Layer 1 — GlassWorm fixture cases (G.09–G.13) ─────────
//
// All five tests pass `log_findings=false` so the validator's audit
// path is bypassed. The audit emit reaches `plinth::log::audit`, which
// in production rejects pre-init or no-DbClient state via
// `g_audit_ready`. In a Catch2 process that has previously hit
// `ensure_drogon_running()` (e.g. the eval_guard_test suite earlier in
// the run), `g_audit_ready` is true but no DbClient was created — and
// debug-build Drogon asserts inside `getDbClient()`. The gate behavior
// (rule emission + disposition) is what we cover here; the audit-emit
// pipeline has no test coverage in 0.4.1 (Layer 1 audit fires only
// from the future kernel installer at 0.4.4).
namespace {

auto cfg_no_audit() -> plinth::packages::ValidationConfig {
  plinth::packages::ValidationConfig c;
  c.unicode_scanner_log_findings = false;
  return c;
}

} // namespace

// G.09 — variation-selectors burst inside server/handlers/shell.js.
TEST_CASE("validate: rejects variation-selector smuggle",
          "[packages][validator][unicode]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(
      fixture("unicode-smuggle-variation-selectors"), cfg_no_audit());
  REQUIRE(has_rule(r, "unicode-smuggle"));
  REQUIRE(r.disposition() == 1);
}

// G.10 — bidi-override burst inside server/handlers/shell.js.
TEST_CASE("validate: rejects bidi-override smuggle",
          "[packages][validator][unicode]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("unicode-smuggle-bidi-override"),
                                      cfg_no_audit());
  REQUIRE(has_rule(r, "unicode-smuggle"));
  REQUIRE(r.disposition() == 1);
}

// G.11 — malformed UTF-8 (overlong NUL) inside server/handlers/shell.js.
TEST_CASE("validate: rejects malformed UTF-8 in handler source",
          "[packages][validator][unicode]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("unicode-smuggle-malformed-utf8"),
                                      cfg_no_audit());
  REQUIRE(has_rule(r, "unicode-smuggle"));
  REQUIRE(r.disposition() == 1);
  // The hint should mention UTF-8 decode (verifies decode_error path).
  auto found = std::ranges::find_if(
      r.messages, [](const auto& m) { return m.rule == "unicode-smuggle"; });
  REQUIRE(found != r.messages.end());
  REQUIRE(found->message.find("UTF-8") != std::string::npos);
}

// G.12 — legitimate emoji baseline (5 emoji × 1 VS each = 5 findings).
// Scoped to --structure-only so the test isolates unicode-scanner
// behavior from the 0.4.2 cross-file pass (the fixture's capability
// namespace legitemoji != manifest.name legit-emoji would otherwise
// trip CF7).
TEST_CASE("validate: accepts legitimate-emoji fixture",
          "[packages][validator][unicode]") {
  prepare_dynamic_fixtures();
  auto cfg = cfg_no_audit();
  cfg.cross_file = false;
  auto r = plinth::packages::validate(fixture("unicode-legitimate-emoji"), cfg);
  REQUIRE_FALSE(has_rule(r, "unicode-smuggle"));
  REQUIRE(r.disposition() == 0);
}

// G.13 — regression guard: 0.4.0's valid-full fixture stays clean.
TEST_CASE("validate: valid-full fixture stays free of unicode-smuggle",
          "[packages][validator][unicode]") {
  prepare_dynamic_fixtures();
  auto r = plinth::packages::validate(fixture("valid-full"), cfg_no_audit());
  REQUIRE_FALSE(has_rule(r, "unicode-smuggle"));
}
