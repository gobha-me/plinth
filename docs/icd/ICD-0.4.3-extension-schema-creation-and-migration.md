# ICD-0.4.3-extension-schema-creation-and-migration

**Traces to:** DESIGN-packages-v04x.md §0.4.3 (Extension PG Schema Creation and Migration Execution — the authoritative scope); architecture/03-data.md §1.2 (Extension Database Isolation — `ext_{name}` schema + `search_path` contract); architecture/03-data.md §1.3 (Extension Migration Tracking — the `plinth.migrations` table, already materialized in `migrations/schema.sql:14`).
**Depends on:** ICD-0.1.1-pg-bootstrap (kernel schema bootstrap via sync libpq — 0.4.3 reuses the same admin-connection pattern); ICD-0.4.1-manifest-parsing (`PackageManifest::name` — already regex-validated as `^[a-z][a-z0-9_-]{2,62}$`, safe for direct splicing into `ext_{name}`); ICD-0.4.0-package-structure-validation (the `migrations/` directory validation — 0.4.3 runs only after structural pre-flight passes).
**Milestone:** 0.4.3 — per-extension PG schema creation plus numbered-migration execution with checksum-immutable tracking. Library milestone: no CLI, no HTTP surface. 0.4.4's MIGRATING stage is the sole in-tree caller.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `migrations/schema.sql:14` (the `plinth.migrations` table — already present in the dev-mode schema, shape is the permanent contract); `src/kernel/db/bootstrap.{hpp,cpp}` (the existing sync-libpq bootstrap surface — 0.4.3 adds a sibling module, does not extend `bootstrap.cpp` itself); `docs/icd/ICD-0.4.1-glassworm-defense.md §Out-of-scope` ("Migration SQL scanning... reaches libpq, not `JS_Eval`. Deferred") — resolves the question of whether Layer 2 applies to `.sql` files; DESIGN-packages-v04x.md §0.4.4 (the install lifecycle — 0.4.4's MIGRATING stage is the sole caller of 0.4.3's surface).

---

## Overview

0.4.3 gives the kernel one thing: the ability to stand up an extension's Postgres namespace. When 0.4.4 orchestrates an install, the MIGRATING stage calls a single entry point — `plinth::packages::run_migrations(extension_name, package_root, admin_conn)` — which (a) creates the `ext_{name}` schema if absent, (b) configures PG roles and GRANTs against it, (c) reads `migrations/NNN_*.sql` in numeric order, (d) applies each unseen migration inside its own transaction, and (e) records each success in `plinth.migrations` with a SHA-256 checksum. On any failure the caller gets a structured `MigrationError`; 0.4.4 decides whether the failure rolls the whole install back (first install) or sticks as a partial-upgrade marker (upgrade path).

The boundary is narrow on purpose. 0.4.3 does not know about `plinth.packages`, does not know about zip uploads, does not start Drogon routes, does not touch the capability registry. It is the DDL/migration library. 0.4.4's orchestrator assembles it into the state machine.

**Scope:**

- New `src/kernel/packages/migrations.{hpp,cpp}` translation unit — the library. Public entry `run_migrations()`; internal helpers for schema creation, GRANT application, migration discovery, checksum computation, per-migration transactional apply.
- New `src/kernel/packages/migration_error.hpp` — `MigrationError` enum + `MigrationFailure` struct (kind + file + pg-error + human message). Mirrors the shape of `ManifestParseError` (0.4.1) and `CapabilityError` (0.2.2).
- Admin connection handling — 0.4.3 uses a **dedicated admin libpq connection passed in by the caller**, not the runtime Drogon pool. The caller (0.4.4, or a future admin CLI) owns connection lifetime. 0.4.3 is connection-agnostic at the API boundary — takes a `PGconn*` by pointer, returns without closing it.
- PG role + GRANT model — `ext_{name}_role` role created at schema-create time; granted `USAGE, CREATE` on `ext_{name}` and `USAGE` on `plinth`. The role has no PG-level password; the runtime Drogon pool uses `SET ROLE ext_{name}_role` before each extension-scoped query to switch identity. ALTER/DROP of the role is 0.4.5's uninstall concern.
- Migration file discovery — read `{package_root}/migrations/*.sql`, match `^(\d+)_.+\.sql$` with numeric sort on the leading digit group, warn (not error) on gaps, error on duplicates.
- Per-migration transaction — `BEGIN; <file contents>; INSERT INTO plinth.migrations ...; COMMIT;`. libpq returns the PG error verbatim on failure; 0.4.3 surfaces it up.
- Checksum verification — SHA-256 of the file contents stored in `plinth.migrations.checksum`. Re-running an install with a modified already-applied migration fails with `MigrationError::CHECKSUM_MISMATCH` before any SQL executes.
- Unit-test harness — new `tests/kernel/packages/migrations_test.cpp`. Runs against the `PLINTH_KERNEL_TESTS=ON` PG gate (same pattern as 0.2.3's listener integration test). Fixtures under `tests/fixtures/migration_packages/` (not the 0.4.0 packages tree — migration fixtures can't be valid installable packages without the full manifest set; keep them isolated).

**Out of scope (deferred):**

- **Schema DROP on uninstall.** `DROP SCHEMA ext_{name} CASCADE` is 0.4.5's uninstall concern. 0.4.3 only creates.
- **`ALTER` of already-applied migrations.** Migrations are immutable — the checksum check makes modification impossible. Intentional migration rollback is an admin-side workflow (write a forward-fixing migration), not a 0.4.3 feature.
- **Migration unit skipping / forcing.** No `--force`, no `--skip`. 0.4.5 admin-side recovery may grow these as out-of-band flags; 0.4.3's library surface does not.
- **`plinth.packages` / `plinth.panels` schema.** Both tables live in 0.4.4's scope (see ICD-0.4.4 §Data Model). 0.4.3 writes only to `plinth.migrations`.
- **Atomic cross-migration rollback.** Each migration is its own transaction; a failure in `003_*` does not roll back already-committed `001_*` and `002_*`. This is a deliberate DESIGN §0.4.3 choice (upgrade-failure stickiness: the admin sees which migration failed and can forward-fix).
- **Cross-extension migration ordering or dependencies.** Extensions install independently; no dependency graph.
- **Migration-time `search_path` setting.** DESIGN §0.4.3 specifies `search_path = ext_{name}, plinth` *at runtime*, not at migration time. Migrations run against the admin connection with fully-qualified schema names; runtime `SET search_path` is 0.5.x's `db.*` binding concern (see `DEFERRED.md` entry "Per-op `SET search_path` for `db.*`").
- **GlassWorm Layer 2 scanning of migration SQL.** Explicitly deferred by ICD-0.4.1 §Out-of-scope ("Migration SQL scanning... reaches libpq, not `JS_Eval`"). 0.4.3 does not invoke `pre_eval_scan` on `.sql` files.
- **Cryptographically-signed migrations.** Package signing is a whole-package concern deferred to the 0.8.x / 0.10.x window per DESIGN-packages-v04x.md §7.2. The SHA-256 checksum in 0.4.3 is integrity-against-tampering-between-installs, not integrity-against-adversarial-authoring.

---

## Migration File Layout

A package's migration set lives at `{package_root}/migrations/*.sql`. 0.4.3 reads every file matching:

```
^(\d+)_[a-z0-9_-]+\.sql$
```

- **Sort key.** The leading digit group parsed as `uint64_t` (stops at the first non-digit). `010_foo.sql` sorts *after* `009_bar.sql`, not lexicographically between `001_` and `002_`.
- **Gap handling.** A missing number (e.g. `001_*`, `002_*`, `004_*` present, `003_*` missing) emits a `MigrationWarning` with `kind: "migration-gap"`. The caller (0.4.4) logs the warning and continues — gaps are legal during development, when an early migration has been squashed.
- **Duplicate handling.** Two files with the same leading number (e.g. `001_a.sql` + `001_b.sql`) produce `MigrationError::DUPLICATE_SEQUENCE` before any SQL executes. This is always a packaging bug.
- **Non-matching files.** Files under `migrations/` that do not match the regex (e.g. `README.md`, `.gitkeep`, an editor backup) are silently ignored. Not a warning — packaging conventions vary.
- **No migrations directory.** Absence of `{package_root}/migrations/` is legal and is not an error. `run_migrations()` short-circuits to a successful-no-op after schema creation + GRANT application.

Each migration file is read as UTF-8 bytes, stripped of a single leading UTF-8 BOM if present (cosmetic — PG accepts SQL with or without). Encoding validation beyond "is UTF-8" is not 0.4.3's concern.

---

## Schema + GRANT Contract

On the first invocation of `run_migrations(name, ...)` for an extension whose `ext_{name}` schema does not yet exist, the sequence is:

```sql
-- All inside a single admin-connection transaction, committed before any migration runs:
BEGIN;
  CREATE SCHEMA ext_{name};

  -- Role is created idempotently; kernel bootstrap may have created it on a prior failed install.
  DO $$ BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'ext_{name}_role') THEN
      CREATE ROLE ext_{name}_role NOLOGIN;
    END IF;
  END $$;

  GRANT USAGE, CREATE ON SCHEMA ext_{name} TO ext_{name}_role;
  GRANT USAGE ON SCHEMA plinth TO ext_{name}_role;
  GRANT SELECT ON plinth.users TO ext_{name}_role;
COMMIT;
```

**Design notes:**

- The schema-creation DDL, role creation, and GRANT are bundled in one transaction — a partial failure here (e.g. `plinth.users` missing) rolls the schema-create back. Migrations then run in their own per-file transactions on top of a known-good schema.
- The `plinth.users` GRANT is the only pre-declared `plinth.*` read the 0.4.3 window exposes. Additional reads (`plinth.groups`, `plinth.rbac_rules`, etc.) are added one-at-a-time as the kernel stdlib grows in 0.5.x — each addition ships as an extension-role GRANT migration on the kernel side, not an ALTER of this 0.4.3 template.
- The `ext_{name}_role` is a PG-level role without a login password. Runtime uses `SET ROLE` from the kernel's admin-connection credentials, which means no extension ever holds a raw PG password. This matches `architecture/05-extensions.md §3 "What is NOT available in the runtime: Raw PG connection strings."`
- `search_path` is deliberately not set during migrations. All DDL inside migration files must use fully-qualified `ext_{name}.table_name`. Rationale: a migration author who writes `CREATE TABLE foo (...)` without qualification would silently create it in `plinth.*` if `search_path` were set wrong, corrupting kernel tables. Forcing qualification is the safer default; runtime code paths (0.5.x `db.*`) set `search_path` at the request boundary where extension scope is unambiguous.

**Idempotence.** Running `run_migrations()` on a package whose schema already exists is legal and is not an error. The schema-create block is skipped (detected via `SELECT 1 FROM pg_namespace WHERE nspname = 'ext_{name}'`), but the GRANT block still runs — idempotent re-application catches the case where a prior install partially committed (schema created, GRANT failed) or where the kernel stdlib added a new default GRANT between installs.

---

## Migration Execution Contract

After schema + GRANT setup succeeds, `run_migrations()` iterates the sorted migration list and, for each file:

1. **Already-applied check.** `SELECT checksum FROM plinth.migrations WHERE extension_name = $1 AND migration_file = $2`.
   - If no row: the migration is new — proceed to step 2.
   - If a row with matching `checksum`: skip — migration is already applied with identical content.
   - If a row with differing `checksum`: abort with `MigrationError::CHECKSUM_MISMATCH`. The caller surfaces this as a hard install-time error. No attempt is made to run subsequent migrations.

2. **Apply.** Inside a single libpq transaction:
   ```sql
   BEGIN;
     -- File contents spliced verbatim. PG parser handles statement separation.
     <contents of 001_create_notes.sql>
     INSERT INTO plinth.migrations (extension_name, migration_file, checksum)
         VALUES ($1, $2, $3);
   COMMIT;
   ```
   On `BEGIN`/statement failure: the transaction is rolled back by libpq. 0.4.3 captures `PQresultErrorMessage()` and `PQresultErrorField(PG_DIAG_SQLSTATE)` into `MigrationFailure`.

3. **Stop on first failure.** If a migration transaction fails, the loop aborts immediately. Subsequent migrations are not attempted. Already-applied migrations stay applied — that is 0.4.4's upgrade-stickiness contract.

**Transaction boundaries.** Each migration runs in its own transaction: the file's SQL + the tracking INSERT commit together or roll back together. This guarantees: (a) `plinth.migrations` never records a migration that didn't actually apply, and (b) a migration that partially applied before a crash cannot leave `plinth.migrations` out of sync with the schema.

**Statement-splitting is PG's job, not ours.** Migration files may contain multiple statements (`CREATE TABLE foo; CREATE INDEX ...;`). libpq's simple-query protocol passes the entire file as one query string; PG parses and executes the statements in order. 0.4.3 does not implement a client-side SQL splitter.

**Checksum.** SHA-256 of the migration file's raw UTF-8 bytes (post-BOM-strip), hex-encoded. `ext_{name}` is *not* included in the checksum — it's the file content alone, so an identical migration file shipped in two packages would have the same checksum (the `(extension_name, migration_file)` PK in `plinth.migrations` disambiguates). The kernel implements SHA-256 via OpenSSL (already a transitive dependency through Drogon — no new library).

**Advisory lock.** Before the first migration, acquire a PG advisory lock keyed by `hashtextextended('plinth.migrations.' || extension_name, 0)` — a 63-bit hash that libpq accepts. Release in the same connection after the last migration. This prevents two concurrent `run_migrations()` invocations on the same extension from racing each other. 0.4.4's installer already holds a package-level lock per DESIGN §0.4.5; this inner advisory lock is a defense-in-depth for direct library use (e.g. future admin CLI).

---

## Library Surface

```cpp
// src/kernel/packages/migration_error.hpp — NEW in 0.4.3
#pragma once
#include <optional>
#include <string>

namespace plinth::packages {

enum class MigrationError {
    DUPLICATE_SEQUENCE,       // two migration files share a leading number
    INVALID_FILENAME,         // file under migrations/ but matches the regex incorrectly (edge: "01a_foo.sql")
    CHECKSUM_MISMATCH,        // already-applied migration file was modified
    SCHEMA_CREATE_FAILED,     // CREATE SCHEMA or GRANT transaction failed
    MIGRATION_APPLY_FAILED,   // per-migration transaction failed (PG error surfaced)
    DB_CONNECTION_BAD,        // PGconn is null or in a non-OK status at entry
    READ_FAILED,              // filesystem error reading a migration file
    ADVISORY_LOCK_FAILED,     // pg_try_advisory_lock returned false (another process holds it)
};

struct MigrationFailure {
    MigrationError kind;
    std::string extension_name;
    std::optional<std::string> migration_file;  // nullopt for schema/role errors
    std::optional<std::string> pg_sqlstate;     // e.g. "42P07" (duplicate_table); nullopt for non-PG errors
    std::string message;                         // human-readable, suitable for admin surfacing
};

struct MigrationWarning {
    std::string kind;       // "migration-gap", etc. — string-keyed for forward compatibility
    std::string detail;
};

}  // namespace plinth::packages
```

```cpp
// src/kernel/packages/migrations.hpp — NEW in 0.4.3
#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "migration_error.hpp"

// Forward-declare libpq without pulling the header here.
struct pg_conn;
using PGconn = pg_conn;

namespace plinth::packages {

struct MigrationReport {
    std::vector<std::string> applied;    // filenames, newest last
    std::vector<std::string> skipped;    // filenames whose checksum matched an existing row
    std::vector<MigrationWarning> warnings;
};

// Idempotent: callable multiple times. Creates ext_{name} schema + role + GRANTs on first call,
// runs unapplied migrations in numeric order, records each in plinth.migrations.
// PGconn must be an open admin connection with CREATE SCHEMA / CREATE ROLE / GRANT privileges.
// Connection lifetime is the caller's — run_migrations does not PQfinish().
auto run_migrations(std::string_view extension_name,
                    const std::filesystem::path& package_root,
                    PGconn& admin_conn)
    -> std::expected<MigrationReport, MigrationFailure>;

}  // namespace plinth::packages
```

**Design notes:**

- `std::expected<MigrationReport, MigrationFailure>` mirrors the 0.3.0.1 kernel refactor convention — caller uses `result.has_value()` for the success/failure disjunction, dereferences for the report, `.error()` for the failure.
- `extension_name` is `std::string_view` because the validated-regex name is always safely owned by the caller's `PackageManifest`. The function copies into the `ext_{name}` splice buffer; no lifetime concern.
- `PGconn&` rather than raw `PGconn*` signals the pre-condition: non-null. Null pointer → undefined behavior, matching the existing `bootstrap.cpp` convention; 0.4.3 does not add defensive null checks.
- No async variant. Migrations are installer-path code, not request-path code; latency is dominated by the DDL/DML itself (100–1000 ms order), not by wire round-trips. The sync libpq path is explicit and debuggable.

---

## Error Taxonomy

Every `MigrationError` kind, with the condition that produces it and the caller-surfaced message format:

| Kind | Condition | Message format |
|---|---|---|
| `DUPLICATE_SEQUENCE` | Two files in `migrations/` share a leading integer (`001_a.sql` + `001_b.sql`). | `"duplicate migration sequence {n}: {file_a}, {file_b}"` |
| `INVALID_FILENAME` | File under `migrations/` starts with digits but fails the full regex (e.g. `01a_foo.sql`, `001-no-underscore.sql`). | `"invalid migration filename: {filename} (expected NNN_description.sql)"` |
| `CHECKSUM_MISMATCH` | `plinth.migrations` row exists for `(extension, file)` but the on-disk file's SHA-256 differs. | `"migration {filename} was modified after being applied (expected checksum {stored}, got {computed})"` |
| `SCHEMA_CREATE_FAILED` | `CREATE SCHEMA` / `CREATE ROLE` / `GRANT` transaction rolled back. | `"schema or role setup for ext_{name} failed: {pg_error}"` — `pg_sqlstate` populated. |
| `MIGRATION_APPLY_FAILED` | Per-migration transaction rolled back from PG error. | `"migration {filename} failed: {pg_error}"` — `pg_sqlstate` populated. |
| `DB_CONNECTION_BAD` | `PQstatus(&admin_conn) != CONNECTION_OK` at entry. | `"admin database connection is not OK: {PQerrorMessage}"` |
| `READ_FAILED` | `std::filesystem` error opening/reading a migration file (permissions, mid-install deletion). | `"failed to read migration file {filename}: {stderr_message}"` |
| `ADVISORY_LOCK_FAILED` | `pg_try_advisory_lock()` returned false — another process holds the lock for this extension. | `"another migration is in progress for ext_{name}"` |

`MigrationWarning.kind` values (string-keyed, additive):

- `"migration-gap"` — `"migration sequence {start} → {end} skips numbers: {missing_list}"`
- Future warning kinds added here as the mechanism grows; consumers MUST tolerate unknown `kind` values.

---

## Security Constraints

1. **`extension_name` splicing.** The caller guarantees `extension_name` matches the 0.4.1 regex `^[a-z][a-z0-9_-]{2,62}$`. 0.4.3 re-asserts this via `assert` in debug builds; production builds trust the caller. Direct string concatenation into the `ext_{name}` SQL string is safe under this invariant — the regex excludes every SQL meta-character. No additional escaping, no parameterized DDL (PG does not support parameterized DDL for schema names regardless).

2. **Admin connection privilege.** `run_migrations()` demands a connection with `CREATE` on the PG database and `CREATE ROLE` privilege. The caller (0.4.4 / future admin CLI) sources this from the kernel-admin PG credentials established at bootstrap. A non-admin caller gets `SCHEMA_CREATE_FAILED` with `pg_sqlstate = "42501"` (insufficient privilege) on first invocation.

3. **Migration SQL is admin-authored, not user-authored.** Installers upload packages that contain migration SQL. The package is admin-authenticated at the upload boundary (0.4.4's `POST /api/packages` is `packages.install`-gated). Migration files are therefore trusted-on-upload in the same sense `entry_point` JS is trusted-on-upload. GlassWorm Layer 2 scanning does not apply per ICD-0.4.1 §Out-of-scope.

4. **No migration capability signing.** The SHA-256 checksum is a tamper-detection surface against between-install modification, not an authenticity surface against an adversarial package author. Package signing is a separate concern deferred per DESIGN-packages-v04x.md §7.2.

5. **Advisory-lock keyspace collision.** The lock key is `hashtextextended('plinth.migrations.' || name, 0)`. PG's 64-bit advisory-lock keyspace makes accidental collision with other kernel advisory-lock users vanishingly unlikely, but 0.4.3 pins its prefix (`'plinth.migrations.'`) as reserved. Other advisory-lock users must use a different prefix. Captured as a kernel-wide convention in this ICD's text; no enforcement mechanism.

---

## Test Cases

Under `tests/fixtures/migration_packages/`, one directory per test case. Fixtures contain only a `migrations/` directory plus a stub `manifest.json` (name field only — full package structure is not the 0.4.3 concern). Fixtures gated behind `PLINTH_KERNEL_TESTS=ON` because every test needs a live PG connection.

| # | Fixture | Expected outcome | Rule under test |
|---|---|---|---|
| M.01 | `empty/` | Success; empty `MigrationReport`; schema + role created | Schema-only path, no migrations |
| M.02 | `single-migration/` (one `001_create_foo.sql`) | Success; `applied: ["001_create_foo.sql"]` | Happy path, one migration |
| M.03 | `three-migrations-in-order/` | Success; applied in 001/002/003 order; `plinth.migrations` has 3 rows | Numeric-sort order |
| M.04 | `three-migrations-010-vs-002/` (`001`, `002`, `010`) | Success; applied in 001/002/010 order | Numeric not lexicographic |
| M.05 | `gap-001-004/` (`001`, `004`) | Success; `warnings: [{kind: "migration-gap", detail: "skips 002, 003"}]` | Gap is warning, not error |
| M.06 | `duplicate-sequence/` (`001_a.sql`, `001_b.sql`) | `MigrationError::DUPLICATE_SEQUENCE` | Two files, same leading number |
| M.07 | `invalid-filename/` (`001.sql`, no underscore) | `MigrationError::INVALID_FILENAME` | Regex mismatch |
| M.08 | `non-sql-extension/` (`001_foo.txt`) | Silently ignored; success with empty report | Non-matching files skipped |
| M.09 | `no-migrations-dir/` | Success; empty report; schema + role created | Missing `migrations/` is legal |
| M.10 | `bad-sql/` (`001_syntax_error.sql`) | `MigrationError::MIGRATION_APPLY_FAILED`; `pg_sqlstate` populated | PG error surfaced |
| M.11 | `checksum-mismatch/` (run once, modify file, run again) | Second run: `MigrationError::CHECKSUM_MISMATCH` | Immutability enforcement |
| M.12 | `idempotent-rerun/` (run twice, unmodified) | Second run: success; `skipped: [...]` lists all migrations | Re-run is no-op |
| M.13 | `role-already-exists/` (pre-create the role manually) | Success; role reuse works | Idempotent role creation |
| M.14 | `concurrent-same-extension/` (Catch2 parallel; two threads call `run_migrations` for same name) | One succeeds, one returns `ADVISORY_LOCK_FAILED` | Advisory lock serialization |
| M.15 | `non-utf8-bom/` (`001_foo.sql` with UTF-8 BOM) | Success; BOM stripped before checksum; migration applies | BOM handling |

Test count target: **15 fixtures, 17–20 Catch2 cases** (some fixtures drive multiple cases — M.11 requires a run/modify/run sequence within one case).

**Isolation.** Each Catch2 case generates a throwaway extension name (`ext_test_{uuid}`) so parallel tests do not collide on schema or advisory-lock state. Teardown drops `ext_test_{uuid}` and removes its `plinth.migrations` rows. The `PLINTH_KERNEL_TESTS=ON` gate's existing PG connection harness (0.2.3 `listener_integration_test` fixture) is extended with a `migration_test_fixture` RAII helper.

**Non-PG-backed cases.** Pure-logic tests — filename regex parsing, sort ordering, checksum computation — live in a separate `migrations_parsing_test.cpp` without the PG gate, running under the default `ctest` build.

---

## CI Wiring

- `src/kernel/packages/migration_error.hpp` — **new**. Error enum + `MigrationFailure` / `MigrationWarning` structs.
- `src/kernel/packages/migrations.hpp` — **new**. Public surface.
- `src/kernel/packages/migrations.cpp` — **new**. Implementation. Dependencies: libpq (via `<libpq-fe.h>`, already linked by `bootstrap.cpp`), OpenSSL `<openssl/evp.h>` for SHA-256, `<filesystem>`, `<regex>` (acceptable — the filename regex is executed once per file at install time, not on a hot path).
- `src/kernel/packages/CMakeLists.txt` equivalent — source additions fall under the existing `KERNEL_SOURCES` glob added in 0.3.3.2. No manual `add_sources` needed.
- `tests/fixtures/migration_packages/` — 15 new fixture directories.
- `tests/kernel/packages/migrations_test.cpp` — **new**. Catch2 driver, PG-gated.
- `tests/kernel/packages/migrations_parsing_test.cpp` — **new**. Catch2 driver, no PG dependency. Pure filename/sort/checksum logic.
- `run-clang-tidy-20 -p build src/kernel/packages/ tests/kernel/packages/` zero findings on new TUs.
- `ctest` 100% pass without PG connection (non-PG cases); with `PLINTH_KERNEL_TESTS=ON` and a live PG, all M.01–M.15 green.
- No new CI job. Migration tests run inside the existing `PLINTH_KERNEL_TESTS` block in `.gitea/workflows/ci.yml`.

---

## Entry / Exit

**Entry:** ICD-0.4.0 + ICD-0.4.1 + ICD-0.4.2 merged. `migrations/schema.sql:14` already defines `plinth.migrations`. The sync-libpq kernel bootstrap in `src/kernel/db/bootstrap.cpp` is operational (from 0.1.1). No new `plinth.*` table is added in this milestone — 0.4.3 writes only to the existing `plinth.migrations`.

**Exit:** All 15 fixtures green under `PLINTH_KERNEL_TESTS=ON`; pure-logic tests green under default `ctest`; `run-clang-tidy-20` clean on new TUs; CHANGELOG entry describes the library surface + the role/GRANT model + checksum immutability; ROADMAP `0.4.3` line removed. `DEFERRED.md` amended if the per-op `search_path` question surfaces a new deferral shape (expected not — the 2026-04-18 entry already points at 0.4.3 and is specifically about runtime `db.*`, not migration time).

---

## Open Questions (resolve during implementation)

1. **Admin connection source.** 0.4.4's installer caller needs to supply the `PGconn&`. Options: (a) it opens a dedicated admin connection per install from a kernel-config-provided DSN; (b) it acquires an admin-credentialed connection from a small kernel-owned admin pool (single connection, likely); (c) reuses the Drogon runtime pool via `app().getDbClient()` + superuser credentials. Proposed: (a) — explicit per-install admin connection. Rationale: installs are rare, DDL requires elevated privilege, keeping the high-privilege channel narrow and explicit prevents accidental runtime elevation. Alternative: (b) if connection-open cost is measurable. Architect decision at implementation time; 0.4.3's library surface is agnostic (takes `PGconn&`), so the decision is 0.4.4's concern even though the question surfaces here.

2. **Runtime `search_path` setting.** `DEFERRED.md` entry "Per-op `SET search_path` for `db.*`" (2026-04-18) points at 0.4.3 but the per-op setting is actually 0.5.x `db.*` binding scope (the 0.5.x milestone is the first user of the ext_{name} schema at runtime). Proposed resolution: close the DEFERRED.md entry as moved to 0.5.x — 0.4.3 documents the contract (`search_path = ext_{name}, plinth` at runtime; migrations run with fully-qualified names) but does not implement the binding. Alternative: amend 0.4.3 scope to include an immediate `SET ROLE ext_{name}_role; SET search_path = ext_{name}, plinth;` helper in `src/kernel/db/` for 0.5.x to consume. Architect decision; defaults to deferral.

3. **Partial-install rollback semantics.** A new install that fails at migration 3-of-5 leaves `ext_{name}` + role + GRANTs + migrations 1–2 applied. DESIGN §0.4.3 is clear on the upgrade case (stickiness, admin forward-fixes) but silent on the first-install case. Two options: (a) **clean slate** — on a first-install migration failure, drop `ext_{name}` CASCADE, drop the role, delete the two applied rows from `plinth.migrations`, return failure; (b) **stickiness always** — leave the partial state for admin inspection, identical to upgrade. Proposed: (a) for first install, (b) for upgrade. 0.4.3 cannot distinguish first-install from upgrade on its own — the caller (0.4.4) does. Proposed library shape: 0.4.3's `run_migrations` does not roll back; 0.4.4 calls a companion `drop_schema_and_migrations(name)` if it determines first-install-failure disposition. This keeps 0.4.3's library surface idempotent and monotonic. Architect decision at implementation time; captured here because the answer shapes ICD-0.4.4.

4. **`ext_{name}_role` privilege on shared kernel tables.** 0.4.3 ships with `GRANT SELECT ON plinth.users TO ext_{name}_role`. Future kernel-stdlib work will add more reads (`plinth.groups`, `plinth.rbac_rules`, etc. per the `db.*` capability — 0.5.x). Proposed: each addition is a separate kernel-side migration applied at kernel-bootstrap time; 0.4.3 establishes the pattern but does not pre-seed the list. Alternative: grant `USAGE ON SCHEMA plinth` only, defer all table-level GRANTs to the per-stdlib-function work. Architect decision — the first approach (explicit GRANTs per exposed table) is lower-privilege and easier to audit. Document the decision in the PR.

5. **SHA-256 vs. xxhash.** Tamper-detection-between-installs does not require cryptographic strength; xxhash-128 would be ~10× faster. The migration-apply path is 100–1000 ms dominated by PG, so the hash cost is invisible either way. Proposed: SHA-256 for defensibility (if signing lands in 0.8.x+, the checksum column is already cryptographic and the same value does double duty). Alternative: xxhash for compactness. Architect preference.

---

## Appendix: Example Migration Sequence

Concrete walk-through for a hypothetical Notes extension, anchoring the contract:

```
Initial install (version 1.0.0, migrations/ contains 001_create_notes.sql and 002_add_index.sql):

  run_migrations("notes", pkg_root, admin_conn)
    → PQstatus(admin_conn) == CONNECTION_OK
    → pg_try_advisory_lock(hash('plinth.migrations.notes')) → true
    → SELECT 1 FROM pg_namespace WHERE nspname = 'ext_notes' → empty
    → BEGIN; CREATE SCHEMA ext_notes; CREATE ROLE ext_notes_role NOLOGIN;
             GRANT USAGE, CREATE ON SCHEMA ext_notes TO ext_notes_role;
             GRANT USAGE ON SCHEMA plinth TO ext_notes_role;
             GRANT SELECT ON plinth.users TO ext_notes_role; COMMIT;
    → scan migrations/: ["001_create_notes.sql", "002_add_index.sql"]
    → SELECT checksum FROM plinth.migrations WHERE extension_name='notes' AND migration_file='001_create_notes.sql'
      → empty (new)
    → read file → sha256 = "a1b2c3..." → BEGIN; <file contents>; INSERT INTO plinth.migrations (...) VALUES ('notes', '001_create_notes.sql', 'a1b2c3...'); COMMIT;
    → [same for 002_add_index.sql, sha256 = "d4e5f6..."]
    → pg_advisory_unlock(...)
    → return MigrationReport{ applied: ["001_create_notes.sql", "002_add_index.sql"], skipped: [], warnings: [] }

Upgrade install (version 1.1.0, migrations/ now contains 001, 002, AND 003_add_tags.sql):

  run_migrations("notes", new_pkg_root, admin_conn)
    → advisory lock, connection check pass
    → ext_notes schema already exists → skip CREATE SCHEMA block
    → GRANT block re-runs (idempotent; covers the "maybe a new default GRANT landed since last install" case)
    → scan migrations/: ["001", "002", "003"]
    → 001: checksum matches → skip
    → 002: checksum matches → skip
    → 003: new → apply
    → return MigrationReport{ applied: ["003_add_tags.sql"], skipped: ["001_create_notes.sql", "002_add_index.sql"], warnings: [] }

Tampered-upgrade (attacker substitutes a malicious 001_create_notes.sql in the uploaded package):

  run_migrations("notes", tampered_pkg_root, admin_conn)
    → advisory lock, connection check pass
    → schema exists, GRANT block runs
    → 001: file sha256 = "ffffff..." ≠ stored "a1b2c3..."
    → return MigrationFailure{ kind: CHECKSUM_MISMATCH, migration_file: "001_create_notes.sql",
                              message: "migration 001_create_notes.sql was modified after being applied (expected checksum a1b2c3..., got ffffff...)" }
    → 0.4.4 surfaces this as INSTALL_FAILED; admin sees a clear "tampering or package-build regression" signal.

Mid-migration failure (upgrade with a buggy 003_add_tags.sql):

  run_migrations("notes", pkg_root_v110_buggy, admin_conn)
    → 001: skip (checksum match)
    → 002: skip (checksum match)
    → 003: BEGIN; <buggy SQL>; → PG error 42P01 (undefined_table)
    → libpq auto-ROLLBACK
    → return MigrationFailure{ kind: MIGRATION_APPLY_FAILED, migration_file: "003_add_tags.sql",
                              pg_sqlstate: "42P01",
                              message: "migration 003_add_tags.sql failed: relation \"ext_notes.legacy\" does not exist" }
    → 0.4.4 on upgrade path: set plinth.packages state = INSTALL_FAILED, extension stays at 1.0.0,
        plinth.migrations has rows for 001 + 002 but none for 003 (confirmed by the advisory-lock release).
        Admin forward-fixes by shipping a corrected 003 in a new upload.
```
