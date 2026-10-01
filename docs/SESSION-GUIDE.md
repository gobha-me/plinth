# Plinth — Session Guide

**Read this before writing any code.**

## How This Project Works

Plinth uses LLM-assisted development with strict role separation:

- **Architecture sessions** produce design docs, ICDs, contract ratification, and issue-owned sequencing decisions.
- **Code sessions** implement from those documents. They do not make structural decisions.
- **If you encounter a structural question, ask. Do not invent.**

## Key Documents

| Document | Location | Purpose |
|----------|----------|---------|
| Architecture | `docs/ARCHITECTURE.md` (+ `docs/architecture/*.md`) | System intent, trust boundaries and commitment bands. Read the relevant owner before work. |
| Living contracts | [Decision/index](contracts/README.md) | Current-authority entry points and ratification rules. Only ratified scoped contracts describe current protocol; candidates and this index are not completed fills. |
| Backlog | GitHub Issues; `docs/ROADMAP.md` is a navigation aid | Live scope, dependencies and approved sequencing. Check open PRs before new work. |
| Changelog | `docs/CHANGELOG.md` | What shipped. Updated on each merge. |
| ICDs | `docs/icd/` | Active delivery deltas and retained shipped/deviation history; explicit supersession distinguishes them. |
| Design docs | `docs/design/` | Multi-version arc specs (e.g., QuickJS bridge). |

## Workflow

1. Read applicable tracked `AGENTS.md`, the live issue and open PRs.
2. Read the relevant architecture owner, ratified current contract if present,
   and active ICD/delivery history. Use the contract index; do not infer that a
   draft or historical proposal is current policy.
3. Plan bounded work and obtain human approval before implementation.
4. Create an isolated topic branch/worktree; preserve unrelated changes.
5. Implement and write tests. Update any owned current contract and the active
   ICD's deviations in the same behavior-change PR.
6. Validate under `AGENTS.md` and `CONTRIBUTING.md`, then independently review
   and fix findings. Use at most two build jobs and one heavy local build at a
   time in this shared environment.
7. Push the candidate, open its PR, and require green candidate CI before merge.
8. Verify terminal CI on the exact merge SHA and recheck open PRs before new work.
9. Record delivered outcomes and remaining limitations. Tags, releases and
   repository settings are separate maintainer operations, not automatic steps.

For conflicting source/tests, active ICDs or current contracts, follow the
[authority and conflict rules](contracts/README.md#authority-and-conflicts).
Trace the discrepancy and seek architect approval for structural changes;
do not normalize accidental implementation behavior into policy. Initial
auth/session/PAT and later capability contract fills need separate plans,
reconciliation, review and ratification.

## Rules

1. **Nothing merges without a test.**
2. **Justify every change.** If it contradicts the architecture doc, either the change is wrong or the doc needs updating through an architecture session.
3. **Human approval before implementation.** Present what files you'll change, what you'll add, and why.
4. **One file per capability handler** in extensions (`server/handlers/`).
5. **No structural decisions in code sessions.** Architecture sessions decide structure.

## Schema Rules — Two Phases

The phase/version examples below are historical development guidance, not
current migration or retained-data authority. Read the current
[data architecture](architecture/03-data.md),
[extension database authority](architecture/extension-database-isolation.md)
and [deployment/recovery contract](KUBERNETES.md) before operating on data.
Do not infer destructive reset permission or present upgrade guarantees from
these legacy examples; unresolved freeze decisions remain issue-owned.

**This is critical. Read carefully.**

### Phase 1: Milestones 0.1 through 0.6 — Schema is fluid

During early development, the database schema changes constantly.
Numbered migrations during this phase create debt (dozens of tiny
ALTER TABLE files that could have been one CREATE TABLE).

- There is ONE schema file: `migrations/schema.sql`
- It represents the **current desired state** of the `plinth.*` schema
- When the schema changes, you **edit schema.sql directly**
- On startup with `dev_mode: true`, the kernel drops and recreates
  the plinth schema from `schema.sql` — **this is destructive**
- There is no production data to preserve during this phase
- Extension schemas follow the same pattern during Phase 1

**When you need to change a table during 0.1–0.6:**
1. Edit `migrations/schema.sql`
2. Restart the kernel (dev_mode recreates everything)
3. Done. No migration file needed.

### Phase 2: Milestone 0.7 onward — Migrations are immutable

At the end of 0.6, the core schema has been shaken out through
actual use (API, UI, tests). The schema freezes.

- `schema.sql` is renamed to `migrations/001_baseline.sql`
- All subsequent changes are numbered: `002_add_metrics.sql`, etc.
- Migrations are **append-only and checksummed**
- Never edit an applied migration — write a new one to fix mistakes
- The kernel detects fresh install vs. upgrade and runs accordingly

**When you need to change a table during 0.7+:**
1. Write a new migration file: `migrations/NNN_description.sql`
2. The kernel runs it on next startup
3. The old migration is untouched

### dev_mode Flag

```json
{ "dev_mode": true }
```

- `true` (development): Drop and recreate plinth schema on startup.
  Verbose logging. Extension hot-reload enabled.
- `false` (production): Only run pending migrations. No destructive
  operations.

## Build

```bash
plinth_build_dir="$(mktemp -d /tmp/plinth-topic-build.XXXXXX)"
cmake -S . -B "$plinth_build_dir" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$plinth_build_dir" --parallel 2
ctest --test-dir "$plinth_build_dir" --output-on-failure --parallel 1
```

Use a fresh task-owned build directory and a disposable PostgreSQL fixture for
the full PG/WebSocket surface. Focused tests, pinned format/lint, sanitizers and
lifecycle checks follow `AGENTS.md`; unavailable checks are reported explicitly.

## Test grouping convention (since 0.4.5.1)

This is a historical grouping account, not the complete current inventory or
permission to bypass isolated tests. Current registered selectors, isolation
gates and resource locks are owned by [CMakeLists.txt](../CMakeLists.txt);
[AGENTS.md](../AGENTS.md) and [CONTRIBUTING.md](../CONTRIBUTING.md) own current
validation guidance. Read those owners rather than relying on the legacy
four-group count, line numbers or Gitea CI references below.

CTest registers **four grouped subprocesses** (one Catch2 instance per group,
with process-lifetime fixtures where required):

| CTest entry | Tag selector | Fixture | Notes |
|---|---|---|---|
| `plinth_tests_pure` | `~[integration] ~[ws] ~[js]` | none | Parser, validator, crypto, unit-async |
| `plinth_tests_js` | `[js]` | RuntimePool; async cases also use process-lifetime Drogon + PG | QuickJS group; PG resource lock |
| `plinth_tests_pg` | `[integration] ~[ws] ~[js]` | libpq + `reset_schema`; selected cases may start process-lifetime Drogon + QuickJS | General PG integration group |
| `plinth_tests_ws` | `[ws]` | Drogon HTTP listener + DbClient + `reset_schema` | Holds `plinth_pg_schema` + `plinth_ws_port_28099` RESOURCE_LOCKs |

**Adding a new test:** pick the group whose tags match what your
TEST_CASE needs and add it to the corresponding test TU. Overlap
rule is **Drogon > PG > JS > pure** (`[js][integration]`,
`[js][ws]`, `[async][ws]` are all empty intersections). If your
test needs a fixture the group doesn't already provide, you've
picked the wrong group.

**Fixture mapping.** The 0.3.4.1 split (`ensure_drogon_running()` no-DB vs
`ensure_drogon_with_db_running()` with DbClient) stays intact. Both are
process-lifetime, `call_once`-owned fixtures inside their grouped subprocess;
tests must not attempt to replace their frozen configuration. Pure tests
invoke neither fixture; JS-without-async tests use RuntimePool directly.

**Async history.** `[js][async]` used to run per test because grouped execution
amplified a back-pressure/refcount race. The 0.5.5.2
`AnyCompletionAwaiter` suspension fix closed that race; these cases now belong
to the grouped `plinth_tests_js` entry. Do not reintroduce per-test discovery
as a reliability workaround.

**IDE escape hatch.** Set `PLINTH_DEVELOPER_TEST_DISCOVERY=ON` at
configure time to add per-TEST_CASE `catch_discover_tests` entries
with a `dev.` prefix for IDE test-explorer integration. Default
OFF; CI never flips it.

**Reference:** `CMakeLists.txt:719–755` (the `add_test` + RESOURCE_LOCK
+ TIMEOUT entries); `.gitea/workflows/ci.yml:44`
(`--output-junit junit.xml` for per-TEST_CASE CI visibility);
`docs/CHANGELOG.md` 0.4.5.1 entry.

## Validate an Extension

```bash
./plinth validate ./path/to/extension/
```

## PG Schema Convention

- Kernel tables: `plinth.*`
- Extension tables: `ext_{extension_name}.*`
- Never write cross-schema queries in extension code.
