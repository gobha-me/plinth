#include "advisory_lock_harness.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace plinth::lock_test {

namespace {

constexpr std::size_t STDOUT_CAP_BYTES = 4UL * 1024UL;
constexpr std::size_t READ_CHUNK_BYTES = 1024;
constexpr long SELECT_TICK_USEC = 50'000; // 50 ms

struct ChildSlot {
  pid_t pid = -1;
  int read_fd = -1;
  bool closed = false;
  bool reaped = false;
  int status = 0;
  std::string buffer;
  std::chrono::steady_clock::time_point fork_at;
};

// Pull whatever's available on `slot.read_fd` into `slot.buffer`,
// capped at STDOUT_CAP_BYTES. Sets `slot.closed=true` on EOF.
auto drain_pipe(ChildSlot& slot) -> void {
  if (slot.closed) {
    return;
  }
  std::array<char, READ_CHUNK_BYTES> chunk{};
  while (true) {
    ssize_t n = ::read(slot.read_fd, chunk.data(), chunk.size());
    if (n > 0) {
      const auto ROOM = STDOUT_CAP_BYTES > slot.buffer.size()
                            ? STDOUT_CAP_BYTES - slot.buffer.size()
                            : 0;
      const auto TAKE =
          std::min<std::size_t>(static_cast<std::size_t>(n), ROOM);
      slot.buffer.append(chunk.data(), TAKE);
      continue;
    }
    if (n == 0) {
      slot.closed = true;
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    // Any other error: treat as EOF — child is unreachable.
    slot.closed = true;
    return;
  }
}

auto reap_if_exited(ChildSlot& slot) -> void {
  if (slot.reaped) {
    return;
  }
  int status = 0;
  pid_t r = ::waitpid(slot.pid, &status, WNOHANG);
  if (r == slot.pid) {
    slot.status = status;
    slot.reaped = true;
  }
}

auto sigkill_and_reap(ChildSlot& slot) -> void {
  if (slot.reaped) {
    return;
  }
  ::kill(slot.pid, SIGKILL);
  int status = 0;
  // Blocking waitpid post-SIGKILL — kernel reaps quickly.
  ::waitpid(slot.pid, &status, 0);
  slot.status = status;
  slot.reaped = true;
}

auto exit_code_from(int status) -> int {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return -WTERMSIG(status);
  }
  return -1;
}

auto set_nonblocking(int fd) -> void {
  int flags = ::fcntl(fd, F_GETFL, 0);
  // — fcntl is a varargs C API
  if (flags == -1) {
    return;
  }
  (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Fork one child running `fn(conn, idx, total)`; returns parent-side
// `ChildSlot` with read_fd open and pid set. Child path never returns —
// it `_exit`s after running the lambda.
auto fork_one(const plinth::Config::Database& db, int idx, int total,
              const ChildFn& fn) -> ChildSlot {
  std::array<int, 2> pipe_fds{-1, -1};
  if (::pipe(pipe_fds.data()) != 0) {
    throw std::runtime_error(
        std::string{"AdvisoryLockHarness: pipe() failed: "} +
        std::strerror(errno));
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    throw std::runtime_error(
        std::string{"AdvisoryLockHarness: fork() failed: "} +
        std::strerror(errno));
  }

  if (pid == 0) {
    // ── Child path ───────────────────────────────────────────────
    ::close(pipe_fds[0]); // close parent's read end
    if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
      // Best-effort: continue even if redirection fails.
    }
    ::close(pipe_fds[1]);

    // Open a fresh libpq conn — we MUST NOT inherit a pre-fork conn
    // (fd state, internal buffers, and PG backend session would all
    // be shared/aliased across the parent). Each child owns its conn.
    auto conninfo = build_conninfo(db);
    PGconn* conn = PQconnectdb(conninfo.c_str());
    int rc = 0;
    if (PQstatus(conn) != CONNECTION_OK) {
      // Surface the connection error in stdout; rc=2 distinguishes
      // from lambda-reported failures. std::cout (not std::cerr)
      // because stdout is the redirected pipe back to the parent.
      std::cout << "CONNECT_FAIL: " << PQerrorMessage(conn) << '\n';
      std::cout.flush();
      rc = 2;
    } else {
      try {
        rc = fn(conn, idx, total);
      } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << '\n';
        std::cout.flush();
        rc = 3;
      } catch (...) {
        std::cout << "EXCEPTION: unknown\n";
        std::cout.flush();
        rc = 3;
      }
    }
    if (conn != nullptr) {
      PQfinish(conn);
    }
    // _exit skips Catch2 destructors / reporter flush. atexit handlers
    // are also skipped — that's intentional (the parent owns
    // process-lifetime state we don't want a forked child mutating).
    std::cout.flush();
    std::_Exit(rc);
  }

  // ── Parent path ──────────────────────────────────────────────────
  ::close(pipe_fds[1]); // close child's write end
  set_nonblocking(pipe_fds[0]);
  ChildSlot slot;
  slot.pid = pid;
  slot.read_fd = pipe_fds[0];
  slot.fork_at = std::chrono::steady_clock::now();
  return slot;
}

auto outcome_from(ChildSlot& slot) -> ChildOutcome {
  ChildOutcome out;
  out.pid = slot.pid;
  out.exit_code = exit_code_from(slot.status);
  out.stdout_text = std::move(slot.buffer);
  out.wall = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - slot.fork_at);
  if (slot.read_fd >= 0) {
    ::close(slot.read_fd);
  }
  return out;
}

} // namespace

auto build_conninfo(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password;
}

AdvisoryLockHarness::AdvisoryLockHarness(plinth::Config::Database d)
    : db(std::move(d)) {
}

auto AdvisoryLockHarness::run(int n, std::chrono::milliseconds child_timeout,
                              const ChildFn& fn) -> std::vector<ChildOutcome> {
  if (n <= 0) {
    throw std::invalid_argument("AdvisoryLockHarness::run: n must be positive");
  }

  std::vector<ChildSlot> slots;
  slots.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    slots.push_back(fork_one(db, i, n, fn));
  }

  const auto DEADLINE = std::chrono::steady_clock::now() + child_timeout;

  // Drain pipes and reap until all children exited or deadline passed.
  while (true) {
    bool all_done = true;
    for (auto& s : slots) {
      if (!s.reaped || !s.closed) {
        all_done = false;
        break;
      }
    }
    if (all_done) {
      break;
    }
    if (std::chrono::steady_clock::now() >= DEADLINE) {
      break;
    }

    fd_set rset;
    FD_ZERO(&rset);
    int nfds = -1;
    for (auto& s : slots) {
      if (!s.closed && s.read_fd >= 0) {
        FD_SET(s.read_fd, &rset);
        nfds = std::max(nfds, s.read_fd);
      }
    }
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = SELECT_TICK_USEC;
    if (nfds >= 0) {
      (void)::select(nfds + 1, &rset, nullptr, nullptr, &tv);
    } else {
      // No open pipes — just sleep a tick before re-checking
      // waitpid on the remaining children.
      ::usleep(static_cast<useconds_t>(SELECT_TICK_USEC));
    }
    for (auto& s : slots) {
      drain_pipe(s);
      reap_if_exited(s);
    }
  }

  std::vector<ChildOutcome> outcomes;
  outcomes.reserve(static_cast<std::size_t>(n));
  for (auto& s : slots) {
    bool timed_out = false;
    if (!s.reaped) {
      sigkill_and_reap(s);
      timed_out = true;
    }
    // One more drain pass post-reap to capture any remaining bytes.
    drain_pipe(s);
    auto out = outcome_from(s);
    out.timed_out = timed_out;
    outcomes.push_back(std::move(out));
  }
  return outcomes;
}

auto AdvisoryLockHarness::run_with_contention(
    std::chrono::milliseconds child_timeout, const ChildFn& child_fn,
    const std::function<void()>& parent_fn) -> ChildOutcome {
  auto slot = fork_one(db, /*idx=*/0, /*total=*/1, child_fn);

  const auto DEADLINE = std::chrono::steady_clock::now() + child_timeout;

  // Wait for "READY\n" to appear in the child's stdout.
  bool ready = false;
  while (!ready) {
    if (std::chrono::steady_clock::now() >= DEADLINE) {
      break;
    }
    if (slot.closed) {
      break;
    } // child exited before signalling

    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(slot.read_fd, &rset);
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = SELECT_TICK_USEC;
    (void)::select(slot.read_fd + 1, &rset, nullptr, nullptr, &tv);
    drain_pipe(slot);
    if (slot.buffer.find("READY\n") != std::string::npos) {
      ready = true;
    }
  }

  if (ready) {
    try {
      parent_fn();
    } catch (...) {
      // Parent's failure must not orphan the child — drop through to
      // the standard reap path before re-throwing.
      sigkill_and_reap(slot);
      drain_pipe(slot);
      (void)outcome_from(slot);
      throw;
    }
  }

  // Wait for the child to exit naturally; SIGKILL if it overstays.
  while (!slot.reaped) {
    if (std::chrono::steady_clock::now() >= DEADLINE) {
      break;
    }
    fd_set rset;
    FD_ZERO(&rset);
    if (!slot.closed) {
      FD_SET(slot.read_fd, &rset);
    }
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = SELECT_TICK_USEC;
    (void)::select(slot.read_fd + 1, &rset, nullptr, nullptr, &tv);
    drain_pipe(slot);
    reap_if_exited(slot);
  }

  bool timed_out = false;
  if (!slot.reaped) {
    sigkill_and_reap(slot);
    timed_out = true;
  }
  drain_pipe(slot);

  auto out = outcome_from(slot);
  out.timed_out = timed_out;
  return out;
}

} // namespace plinth::lock_test
