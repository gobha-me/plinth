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

Use Clang 21 for formatting and clang-tidy. The scripts fail if another major
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

`docker/ci.Dockerfile` provides the Ubuntu 26.04 builder with explicitly
versioned LLVM 21 tools. The **CI image** workflow builds it locally and runs
the complete project build, PostgreSQL/WebSocket tests, formatting, and lint
inside the image on toolchain or dependency changes. It never publishes an
image. The regular CI also retains Clang 20 tests and ASan/UBSan alongside
Clang 21 during the transition.

`docker/Dockerfile` is the distinct production runtime contract. The
**Runtime image** workflow builds its `server-tests` and `runtime` stages for
both supported architectures on pull requests and `main`, runs the complete
server surface, inspects the non-root runtime, and exercises the production
browser and retained-upgrade journeys without logging in to a registry or
pushing an image. Keep build-only dependencies in the builder or test stage;
the final runtime must remain UID/GID 10001 and contain only installed runtime
assets.

Publication is intentionally narrower than ordinary CI. Only a trusted push of
an exact `vMAJOR.MINOR.PATCH` tag can reach the publish job, and the tag must
match `VERSION`, identify v0.6.6 or newer, and point to a commit reachable from
`main`. The latest exact-SHA `push` runs for both **CI** and **CodeQL** must be
terminal-success before publication; a manual, failed, cancelled, pending, or
different-SHA run is not a substitute. The job refuses to replace an existing
GHCR version tag, pushes and tests the multi-architecture result by digest
first, signs provenance and the SBOM, and creates only the exact version tag
after verification. It never creates `latest`, major, or minor aliases.
Repository and release settings, version bumps, tags, and releases remain
maintainer operations; a pull request must never receive registry write or
identity-token permissions.

The `publish` job is additionally fail-closed behind the protected `release`
environment and the repository variable
`PLINTH_RELEASE_APPROVAL_GATE=configured`. A maintainer must create that
environment with required maintainer reviewers before setting the variable;
leave the variable absent until the protection is verified. This repository
contract is deliberate: an unreviewed tag push must not be enough to mint an
official image or its attestations.

GHCR package visibility is a separate maintainer-owned setting and does not
automatically follow repository visibility. For the first supported image, the
maintainer must make `gobha-me/plinth` public only after the digest and
attestations are present. The first tag run intentionally stops before creating
the version tag when an anonymous pull fails. Change the package visibility
without changing package content, then rerun the same tag workflow; it verifies
the anonymous digest pull and confirms the exact version tag resolves to that
same digest. The release is incomplete until those checks and the tag workflow
are terminal green; do not work around them with a mutable alias or a separate
manual build.

Do not add unexplained or wildcard `NOLINT` suppressions. Follow the exact
suppression syntax documented in `AGENTS.md`.

## Security and privacy

Use fictional data and fake credentials in tests and documentation. Report
vulnerabilities privately as described in `SECURITY.md`. Pull requests that
weaken authorization, validation, shutdown ownership, or secret handling need
an explicit contract justification and focused regression tests.
