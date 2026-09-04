// Catch2 test binary entry point.
//
// Owns Catch2's Session explicitly so process-level fixture coordinators run
// before static destruction. Also installs a small set of fatal-signal
// handlers so intermittent
// crashes (notably the WS segfault/abort tracked in
// project_ws_flaky_segfault.md) print a symbolic backtrace before the
// default SIGABRT/SIGSEGV handler takes over. Cheap to add, always-on
// for tests, and it gives us a stack for any occurrence going forward
// — ICD-0.3.3 shipped this per DEFERRED.md §WS-flake follow-up.

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <plinth/version.hpp>

#include "test_process.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <functional>
#include <mutex>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace plinth::test_process {

namespace {

std::mutex shutdown_mu;
std::vector<std::function<void()>> shutdown_actions;

} // namespace

auto register_shutdown(std::function<void()> action) -> void {
  std::lock_guard lock(shutdown_mu);
  shutdown_actions.push_back(std::move(action));
}

auto run_shutdowns() -> bool {
  std::vector<std::function<void()>> actions;
  {
    std::lock_guard lock(shutdown_mu);
    actions.swap(shutdown_actions);
  }
  bool clean = true;
  for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
    try {
      (*it)();
    } catch (...) {
      clean = false;
    }
  }
  return clean;
}

} // namespace plinth::test_process

namespace {

// SIGABRT / SIGSEGV / SIGILL / SIGFPE handler. Writes a banner + 64
// frames of backtrace to stderr, then restores the default handler and
// re-raises so the process still aborts (and core dumps are still
// produced if enabled). Uses only async-signal-safe calls (write,
// backtrace_symbols_fd) inside the handler.
extern "C" void plinth_fatal_signal_handler(int sig) {
  static constexpr int MAX_FRAMES = 64;
  std::array<void*, MAX_FRAMES> frames{};
  int frame_count = ::backtrace(frames.data(), MAX_FRAMES);

  const char* name = "UNKNOWN";
  switch (sig) {
    case SIGABRT: name = "SIGABRT"; break;
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGILL: name = "SIGILL"; break;
    case SIGFPE: name = "SIGFPE"; break;
    case SIGBUS: name = "SIGBUS"; break;
    default: break;
  }
  // Fixed-format write so we don't invoke printf (not async-signal-
  // safe). Write the banner first so the reader knows what follows.
  static constexpr std::string_view BANNER =
      "\n*** plinth_tests: fatal signal caught — backtrace follows ***\n"
      "signal=";
  static constexpr std::string_view END_BANNER = "*** end backtrace ***\n";
  (void)::write(STDERR_FILENO, BANNER.data(), BANNER.size());
  (void)::write(STDERR_FILENO, name, std::strlen(name));
  (void)::write(STDERR_FILENO, "\n", 1);
  ::backtrace_symbols_fd(frames.data(), frame_count, STDERR_FILENO);
  (void)::write(STDERR_FILENO, END_BANNER.data(), END_BANNER.size());

  // Restore default handler and re-raise so the process still aborts
  // (preserves ctest's "Subprocess aborted" detection + any core
  // dumps configured with ulimit -c).
  (void)::signal(sig, SIG_DFL);
  (void)::raise(sig);
}

auto install_signal_handlers() noexcept -> void {
  for (int sig : {SIGABRT, SIGSEGV, SIGILL, SIGFPE, SIGBUS}) {
    (void)::signal(sig, &plinth_fatal_signal_handler);
  }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
  install_signal_handlers();
  int result = Catch::Session().run(argc, argv);
  if (!plinth::test_process::run_shutdowns() && result == 0) {
    result = 1;
  }
  return result;
}

TEST_CASE("Version string is not empty", "[kernel]") {
  REQUIRE_FALSE(std::string(plinth::VERSION).empty());
}

TEST_CASE("Version components match the version string", "[kernel]") {
  REQUIRE(plinth::VERSION_MAJOR >= 0);
  REQUIRE(plinth::VERSION_MINOR >= 0);
  REQUIRE(plinth::VERSION_PATCH >= 0);

  const auto expected = std::string("v") +
                        std::to_string(plinth::VERSION_MAJOR) + "." +
                        std::to_string(plinth::VERSION_MINOR) + "." +
                        std::to_string(plinth::VERSION_PATCH);
  const std::string version(plinth::VERSION);
  REQUIRE((version == expected || version.starts_with(expected + "-")));
}
