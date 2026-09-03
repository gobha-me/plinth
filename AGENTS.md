# Plinth contributor guidance

## Scope and authority

- Work through topic branches and pull requests. Preserve unrelated local
  changes and never force-push unique history or bypass required checks.
- Releases, repository settings, and archived private history remain maintainer
  operations; do not mutate them without explicit approval.
- Keep secrets out of source, logs, tests, fixtures, and review artifacts. Use
  fake credentials for hermetic tests.
- Report suspected vulnerabilities through GitHub private vulnerability
  reporting, not a public issue.

## C++ tooling

- Plinth uses C++23 and LLVM/Clang-derived formatting, pinned to Clang 20.
- Run `tools/format.sh --check`; use `tools/format.sh --fix` for mechanical
  normalization.
- Run `tools/lint.sh`. It uses Clang/clang-tidy 20 and defaults to two jobs.
- `NOLINT`, `NOLINTNEXTLINE`, `NOLINTBEGIN`, and `NOLINTEND` are exceptional.
  They must name exact checks and include an inline ASCII `-- justification`.
  Bare, wildcard, unexplained, nested, mismatched, or unclosed suppressions are
  forbidden. Prefer fixing the code or narrowing the configured policy.
- Build matrices in this 16 GiB environment with `--parallel 2`. Put large
  build trees under `/tmp`, not the shared `/config` volume.

## Lifecycle contract

- Production `main` owns startup and shutdown through one idempotent, bounded
  coordinator. Tests use that same coordinator and dependency order.
- Shutdown order is: close ingress; cancel timers and stop listeners; cancel
  and join owned async work; flush database-backed state while the database is
  alive; destroy extension and JavaScript runtimes; stop Drogon/event loops;
  close logging last.
- Every thread, timer, callback, coroutine, runtime, and database operation has
  an explicit owner and join/cancel protocol. Do not detach work or let raw
  pointers cross asynchronous lifetime boundaries.
- Exit handlers may diagnose only. Do not add normal teardown to `atexit` or
  signal handlers, mask races with sleeps/leaks, swallow shutdown failures, or
  weaken tests.
- Partial startup must unwind only initialized components through the same
  coordinator used by normal and signal exits.

## Validation

- Run focused tests first, then the full PostgreSQL/WebSocket suite with a
  task-owned disposable database.
- Lifecycle changes require subprocess coverage for normal return, SIGINT,
  SIGTERM, active WebSockets/timers, database/realtime work, extension
  runtimes, and injected partial-startup failures.
- Run ASan/UBSan and feasible TSan lifecycle coverage. Report unavailable
  checks explicitly.
- Before merge, require green CI on the candidate SHA. After merge, require
  terminal CI on the exact merge SHA before any later tag or release.
