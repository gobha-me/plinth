# Contributing

Thank you for improving Plinth. It is a C++23 project with deliberately strict
lifecycle, authorization, and static-analysis contracts.

## Workflow

1. Discuss substantial behavior or protocol changes in an issue first.
2. Create a focused topic branch; do not commit directly to `main`.
3. Add adversarial and failure-path tests with the implementation.
4. Run the checks below and describe the exact results in the pull request.
5. Keep unrelated formatting or refactoring out of the change.

By contributing, you agree that your contribution is licensed under the MIT
license in this repository. No contributor license agreement is required.

## Local checks

Use Clang 20 for formatting and clang-tidy. The scripts fail if another major
version is selected.

```bash
tools/format.sh --check
tools/lint.sh
tools/public_readiness.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 1
```

PostgreSQL and WebSocket coverage uses a disposable database configured with
`PLINTH_PG_*`; the GitHub workflow is the canonical example. Lifecycle changes
also require normal return, SIGINT, SIGTERM, active-work, and partial-startup
coverage plus ASan/UBSan and feasible TSan runs.

Do not add unexplained or wildcard `NOLINT` suppressions. Follow the exact
suppression syntax documented in `AGENTS.md`.

## Security and privacy

Use fictional data and fake credentials in tests and documentation. Report
vulnerabilities privately as described in `SECURITY.md`. Pull requests that
weaken authorization, validation, shutdown ownership, or secret handling need
an explicit contract justification and focused regression tests.
