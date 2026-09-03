# Plinth — Session Guide

**Read this before writing any code.**

## How This Project Works

Plinth uses LLM-assisted development with strict role separation:

- **Architecture sessions** (this Claude project) produce design docs, ICDs, and roadmap decisions.
- **Code sessions** implement from those documents. They do not make structural decisions.
- **If you encounter a structural question, ask. Do not invent.**

## Key Documents

| Document | Location | Purpose |
|----------|----------|---------|
| Architecture | `docs/ARCHITECTURE.md` (+ `docs/architecture/*.md`) | Source of truth. Read the relevant sub-document before any work. |
| Roadmap | `docs/ROADMAP.md` | What to build next. Each task is one squash-merge to main. |
| Changelog | `docs/CHANGELOG.md` | What shipped. Updated on each merge. |
| ICDs | `docs/icd/` | Interface contracts. All implementations trace to these. |
| Design docs | `docs/design/` | Multi-version arc specs (e.g., QuickJS bridge). |

## Workflow

1. Read the roadmap task you're implementing
2. Read the relevant architecture section and ICD
3. Create a branch: `git checkout -b 0.1.2-auth-sessions`
4. Plan the work, present to human for approval
5. Implement + write Catch2 tests
6. Ensure CI passes: `mkdir -p build && cd build && cmake .. && make -j$(nproc) && ctest`
7. Squash merge to main: one commit per task
8. Tag: `git tag v0.1.2`
9. Update CHANGELOG.md

## Rules

1. **Nothing merges without a test.**
2. **Justify every change.** If it contradicts the architecture doc, either the change is wrong or the doc needs updating through an architecture session.
3. **Human approval before implementation.** Present what files you'll change, what you'll add, and why.
4. **One file per capability handler** in extensions (`server/handlers/`).
5. **No structural decisions in code sessions.** Architecture sessions decide structure.

## Schema Rules — Two Phases

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
mkdir -p build && cd build
cmake ..
make -j$(nproc)
ctest --output-on-failure
./plinth --version
```

## Test grouping convention (since 0.4.5.1)

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
