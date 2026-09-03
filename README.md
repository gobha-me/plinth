# Plinth

**The kernel ships empty. Extensions are the product.**

Plinth is an early-stage, self-hosted application kernel written in C++23. It
provides identity, authorization, groups, capability dispatch, PostgreSQL-backed
storage, realtime pub/sub, audit logging, metrics, high-availability
coordination, and a sandboxed QuickJS extension runtime.

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

The example temporarily enables registration so the first local account can
become administrator. Disable `registration_enabled` immediately afterward.
The example database credentials are development-only. See
[Configuration](docs/CONFIGURATION.md) before changing the bind address or
deploying behind a TLS reverse proxy.

To stop the development database and keep its volume:

```bash
docker compose -f docker/docker-compose.yml down
```

## Development

Run the repository checks before opening a pull request:

```bash
tools/format.sh --check
tools/lint.sh
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 1
tools/public_readiness.py
```

PostgreSQL-backed tests use the `PLINTH_PG_*` variables shown in CI. See
[Contributing](CONTRIBUTING.md) for the complete workflow.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Configuration and deployment](docs/CONFIGURATION.md)
- [Extension guide](docs/EXTENSION-GUIDE.md)
- [Shutdown dependency graph](docs/architecture/shutdown.md)
- [Roadmap](docs/ROADMAP.md)
- [Changelog](docs/CHANGELOG.md)
- [Public history and provenance](docs/PUBLIC-HISTORY.md)
- [Security policy](SECURITY.md)

## License

Plinth is licensed under the [MIT License](LICENSE). Dependency licenses and
vendored-code notices are recorded in [third-party notices](THIRD_PARTY_NOTICES.md).
