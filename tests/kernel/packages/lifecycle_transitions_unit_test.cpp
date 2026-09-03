// PG-independent unit tests for 0.4.5 lifecycle transitions. Companions
// to lifecycle_transitions_test.cpp (which is PG-gated integration).
// Populated incrementally as Slice B commits land: B4 seeds this file
// with the `is_gc_eligible` boundary cases; later commits add SemVer
// upgrade comparator (B5), RBAC reconciliation comparator (B6), and
// the drain state-machine sanity tests.

#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>

using namespace std::chrono_literals;
using plinth::packages::compare_semver;
using plinth::packages::is_gc_eligible;

TEST_CASE("is_gc_eligible: retention boundary is inclusive",
          "[lifecycle_transitions][unit][G.01]") {
  auto now = std::chrono::system_clock::now();
  // Retired exactly 24h ago → eligible (boundary equality).
  REQUIRE(is_gc_eligible(now - 24h, now, 24h));
}

TEST_CASE("is_gc_eligible: within retention window is not eligible",
          "[lifecycle_transitions][unit][G.01]") {
  auto now = std::chrono::system_clock::now();
  REQUIRE_FALSE(is_gc_eligible(now - 1h, now, 24h));
  REQUIRE_FALSE(is_gc_eligible(now - 23h - 59min, now, 24h));
}

TEST_CASE("is_gc_eligible: past retention is eligible",
          "[lifecycle_transitions][unit][G.01]") {
  auto now = std::chrono::system_clock::now();
  REQUIRE(is_gc_eligible(now - 25h, now, 24h));
  REQUIRE(is_gc_eligible(now - 240h, now, 24h));
}

TEST_CASE("is_gc_eligible: future retired_at is never eligible",
          "[lifecycle_transitions][unit][G.01]") {
  // Clock skew guard — a row's retired_at parsed as the future must
  // not trip the GC. The predicate is strictly comparison-based so
  // this is a sanity check, not a defensive branch.
  auto now = std::chrono::system_clock::now();
  REQUIRE_FALSE(is_gc_eligible(now + 1h, now, 24h));
}

TEST_CASE("is_gc_eligible: zero retention collects everything past",
          "[lifecycle_transitions][unit][G.01]") {
  auto now = std::chrono::system_clock::now();
  REQUIRE(is_gc_eligible(now - 1ns, now, 0h));
  REQUIRE(is_gc_eligible(now, now, 0h)); // boundary inclusive
  REQUIRE_FALSE(is_gc_eligible(now + 1ns, now, 0h));
}

// ─── compare_semver: B5 upload collision classifier dependency ───────

TEST_CASE("compare_semver: major/minor/patch ordering",
          "[lifecycle_transitions][unit][semver]") {
  REQUIRE(compare_semver("1.0.0", "1.0.0") == 0);
  REQUIRE(compare_semver("1.0.0", "2.0.0") < 0);
  REQUIRE(compare_semver("2.0.0", "1.0.0") > 0);
  REQUIRE(compare_semver("1.2.0", "1.3.0") < 0);
  REQUIRE(compare_semver("1.2.3", "1.2.4") < 0);
  REQUIRE(compare_semver("1.10.0", "1.9.0") > 0); // numeric, not lex
}

TEST_CASE("compare_semver: build metadata does not affect precedence",
          "[lifecycle_transitions][unit][semver]") {
  REQUIRE(compare_semver("1.0.0+gold", "1.0.0+silver") == 0);
  REQUIRE(compare_semver("1.0.0+build.1", "1.0.0") == 0);
}

TEST_CASE("compare_semver: pre-release is lower than release",
          "[lifecycle_transitions][unit][semver]") {
  REQUIRE(compare_semver("1.0.0-alpha", "1.0.0") < 0);
  REQUIRE(compare_semver("1.0.0", "1.0.0-alpha") > 0);
  REQUIRE(compare_semver("1.0.0-rc.1", "1.0.0") < 0);
}

TEST_CASE("compare_semver: pre-release identifier precedence",
          "[lifecycle_transitions][unit][semver]") {
  // SemVer 11.4 example ladder.
  REQUIRE(compare_semver("1.0.0-alpha", "1.0.0-alpha.1") < 0);
  REQUIRE(compare_semver("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
  REQUIRE(compare_semver("1.0.0-alpha.beta", "1.0.0-beta") < 0);
  REQUIRE(compare_semver("1.0.0-beta", "1.0.0-beta.2") < 0);
  REQUIRE(compare_semver("1.0.0-beta.2", "1.0.0-beta.11") < 0);
  REQUIRE(compare_semver("1.0.0-beta.11", "1.0.0-rc.1") < 0);
  REQUIRE(compare_semver("1.0.0-rc.1", "1.0.0") < 0);
}

TEST_CASE("compare_semver: invalid inputs compare equal",
          "[lifecycle_transitions][unit][semver]") {
  // Callers must is_semver_valid first; graceful 0 for safety.
  REQUIRE(compare_semver("1.0", "1.0.0") == 0);
  REQUIRE(compare_semver("", "1.0.0") == 0);
  REQUIRE(compare_semver("abc", "1.0.0") == 0);
}

// ─── B9 check_single_mountpoint ─────────────────────────────────────

TEST_CASE("check_single_mountpoint: same-filesystem tmpdirs pass",
          "[lifecycle_transitions][unit][mountpoint]") {
  namespace fs = std::filesystem;
  auto base = fs::temp_directory_path() /
              ("plinth_mp_test_" + std::to_string(::getpid()));
  fs::create_directories(base / "data" / "extensions");
  fs::create_directories(base / "staging");
  auto r = plinth::packages::check_single_mountpoint(base / "data",
                                                     base / "staging");
  REQUIRE(r.has_value());
  fs::remove_all(base);
}

TEST_CASE("check_single_mountpoint: missing paths are tolerated",
          "[lifecycle_transitions][unit][mountpoint]") {
  namespace fs = std::filesystem;
  auto base = fs::temp_directory_path() /
              ("plinth_mp_test_missing_" + std::to_string(::getpid()));
  // Neither path exists — bootstrap creates them on first use.
  auto r = plinth::packages::check_single_mountpoint(base / "data",
                                                     base / "staging");
  REQUIRE(r.has_value());
}
