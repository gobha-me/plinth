# Plinth

**The kernel ships empty. Extensions are the product.**

Plinth is an early-stage, self-hosted application kernel written in C++23. Its
implemented kernel provides identity, authorization, groups, local capability
dispatch, PostgreSQL-backed extension data, realtime pub/sub, audit logging,
and a sandboxed QuickJS extension runtime. Beginning with the first released
v0.6.6 image digest, a single-instance Kubernetes and Traefik deployment is
supported. File storage, general metrics, sidecars, and
multi-node coordination remain roadmap work; see the
[roadmap](docs/ROADMAP.md) for their owning issues.

Plinth is pre-1.0 software. Its interfaces and storage contracts can change,
and it has not yet received an independent security review. Keep development
instances on loopback and read the [security guidance](SECURITY.md) before any
network exposure.

## Quick start

You need CMake 3.20 or newer, a C++23 compiler, Git, PostgreSQL development
headers, OpenSSL, zlib, libargon2, libzip dependencies, and Node.js. CMake
fetches the source dependencies pinned in `third_party/dependencies.json`.

Start the disposable PostgreSQL/pgvector development service:

```bash
docker compose -f docker/docker-compose.yml up -d postgres
```

Build, copy the strict JSON example, and start Plinth on loopback:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
cp config.json.example config.json
./build/plinth serve --config config.json
```

The example keeps ordinary registration disabled. Set a high-entropy
`PLINTH_BOOTSTRAP_TOKEN`, then create the first administrator through
`POST /api/auth/bootstrap`; remove the token from the process environment once
bootstrap succeeds. The example database credentials are development-only. See
[Configuration](docs/CONFIGURATION.md) before changing the bind address or
deploying behind a TLS reverse proxy.

To stop the development database and keep its volume:

```bash
docker compose -f docker/docker-compose.yml down
```

## Production container image

Supported runtime images begin with Plinth v0.6.6. Releases v0.6.5 and older
do not have a supported image. A release publishes only its exact immutable
version tag, for example `ghcr.io/gobha-me/plinth:v0.6.6`; there is no
`latest`, major, or minor alias. Prefer the `name@sha256:digest` reference
recorded by the release workflow over even the exact version tag.

The image supports `linux/amd64` and `linux/arm64`, runs as UID and GID 10001,
and contains the kernel, migrations, bundled shell, licenses, and SBOM without
the build toolchain. It expects PostgreSQL and persistent data to be supplied
by the operator. After replacing the example digest with the one recorded for
the release:

```bash
image='ghcr.io/gobha-me/plinth@sha256:REPLACE_WITH_RELEASE_DIGEST'
docker pull "$image"
gh attestation verify "oci://$image" \
  --repo gobha-me/plinth \
  --signer-workflow gobha-me/plinth/.github/workflows/runtime-image.yml
docker run --rm "$image" --version
```

The exact digest, its signed provenance, and its SBOM are the release identity;
do not substitute an unversioned or locally rebuilt image. See
[Configuration](docs/CONFIGURATION.md) for the runtime paths and
[Kubernetes and Traefik deployment](docs/KUBERNETES.md) for the supported
single-instance orchestration boundary. GHCR package visibility is maintained
separately from the repository; a release is supported only after its recorded
digest is available to an unauthenticated pull.

## Development

Run the repository checks before opening a pull request. The deployment checks
require Python 3 with PyYAML in addition to the toolchain listed above:

```bash
tools/format.sh --check
tools/lint.sh
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 1
tools/public_readiness.py
.github/scripts/install-deployment-tools.sh contract
export PATH="${PLINTH_DEPLOYMENT_TOOLS_DIR:-/tmp/plinth-deployment-tools/bin}:$PATH"
python3 tests/deployment/helm_contract_test.py
```

PostgreSQL-backed tests use the `PLINTH_PG_*` variables shown in CI. See
[Contributing](CONTRIBUTING.md) for the complete workflow.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Configuration and deployment](docs/CONFIGURATION.md)
- [Kubernetes and Traefik deployment](docs/KUBERNETES.md)
- [Retained-install bundled shell upgrades](docs/bundled-shell-upgrade.md)
- [Extension guide](docs/EXTENSION-GUIDE.md)
- [Shutdown dependency graph](docs/architecture/shutdown.md)
- [Roadmap](docs/ROADMAP.md)
- [Changelog](docs/CHANGELOG.md)
- [Public history and provenance](docs/PUBLIC-HISTORY.md)
- [Security policy](SECURITY.md)

## License

Plinth is licensed under the [MIT License](LICENSE). Dependency licenses and
vendored-code notices are recorded in [third-party notices](THIRD_PARTY_NOTICES.md).
