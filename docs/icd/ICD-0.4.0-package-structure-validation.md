# ICD-0.4.0-package-structure-validation

**Traces to:** DESIGN-packages-v04x.md §0.4.0 (Package Structure Validation), §2 (Arc overview — what 0.4.7 yields + 0.4.0's narrow slice), §9 (Arc entry criteria); architecture/05-extensions.md §1 (Package Structure — the layout that 0.4.0 validates); architecture/05-extensions.md §1.1 (Cross-File Manifest Validation — out-of-scope boundary for 0.4.0; belongs to 0.4.2); architecture/05-extensions.md §3.1 (Runtime Limits — `maximum package size` = 50 MB default)
**Depends on:** ICD-0.4.1-manifest-parsing (`PackageManifest::parse` / `CapabilityManifest::parse` entry points consumed by rule R3 for the two required files; `ManifestParseError` structured-error shape). Optional files (`panels.json`, `rbac.json`, `config.json`) are inspected via raw `nlohmann::json` access in 0.4.0 — their typed parsers ship in 0.4.2 (`panels.json`, `config.json`) and 0.4.6 (`rbac.json`). No runtime dependencies (0.4.0 is a CLI tool with no kernel / no DB / no network).
**Milestone:** 0.4.0 — `plinth validate <path>` CLI command. Static structural validation of a package directory on disk.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `src/kernel/main.cpp:155–167` (`validate` subcommand already wired with TODO body — 0.4.0 fills the TODOs); DESIGN-packages-v04x.md §2 (the arc-level "what 0.4.0 must not foreclose" rule: hot-reload, share primitive, sidecar packaging, remote registry); DEFERRED.md (no active entries block 0.4.0)

---

## Overview

`plinth validate <path>` is the first tool in the 0.4 arc. It answers
a single question: "Is this directory a structurally valid Plinth
package?" — where "structurally" means *the filesystem layout, the
forbidden-path rules, the JSON-parses rules, and the basic size
limits* from `architecture/05-extensions.md §1` and DESIGN-packages-v04x
§0.4.0.

It does **not** answer "will this install?" — that is 0.4.2's
cross-file validation job (every `rbac.json` rule's namespace matches
a capability; every panel references an existing client file; no
frontend mount collisions with a running kernel's other extensions).
0.4.0 is the cheap pre-flight that an extension author runs before
zipping. 0.4.2's `--against-running-kernel` adds the runtime-state
checks; 0.4.4 orchestrates the whole install lifecycle.

The implementation fills in the TODO body at `src/kernel/main.cpp:155–167`
and adds a new `src/kernel/packages/validator.{hpp,cpp}` translation
unit. The parsers themselves live in a sibling TU per ICD-0.4.1.

**Scope:**

- `plinth validate <path>` CLI command, pre-wired at
  `main.cpp:155–167` (current body: TODO comments + log line + exit
  1). 0.4.0 replaces the TODO body with a call into a new
  `plinth::packages::validate(path, config) -> ValidationReport`
  library function and maps the report's disposition to the exit code
  (below).
- `plinth::packages::validate()` runs six validation rules against
  the on-disk layout (see §Validation Rules).
- `ValidationReport` type — vector of `ValidationMessage{severity, path,
  rule, message, remediation?}`; severity is `error | warning`; pass
  = no errors (warnings permitted).
- CLI exit codes (strict): `0 = pass with no warnings`, `1 =
  error(s) present`, `2 = pass with warnings only`. Matches the
  `plinth validate` pre-commit-hook expectation (extension authors
  will gate commits on exit 0).
- Text and `--json` output formats (text is default; `--json` emits
  a machine-readable shape suitable for CI scripting).
- CI fixture set: one passing package plus one failing package per
  rule under `tests/fixtures/packages/` (new directory). Catch2
  test driver at `tests/kernel/packages/validator_test.cpp` invokes
  the library function directly; a shell-level test under
  `tests/kernel/packages/cli_test.sh` exercises the CLI surface
  (exit code mapping).

**Out of scope (deferred):**

- **Cross-file validation** (capability ↔ rbac.json namespace match,
  panel ↔ client file match, mount-prefix collisions) — 0.4.2. The
  `architecture/05-extensions.md §1.1` enumeration lives entirely in
  0.4.2's scope. 0.4.0 does **not** read `rbac.json` or `panels.json`
  for namespace consistency; it only checks that they parse as JSON
  if present.
- **Runtime-state validation** (package-name collisions, duplicate
  frontend mounts, capability-requirement resolution against a live
  registry) — 0.4.2 gated behind `--against-running-kernel`.
- **Schema / migration execution** — 0.4.3.
- **Install lifecycle orchestration** (uploading, extracting,
  registering, activating) — 0.4.4.
- **Capability-signature semantic validation** — belongs to 0.4.1's
  parser (the 0.2.1 `parse_signature` already exists; 0.4.1 uses it).
  0.4.0 surfaces the per-capability signature error via the parser's
  return, but 0.4.0 itself does not re-parse signatures.
- **Typo suggestions / auto-fix.** A nice-to-have that can land as a
  post-MVP ergonomic pass; not required for 0.4.0 exit criteria.
- **Code-signing / provenance verification.** DESIGN §7.2 — deferred
  indefinitely.
- **Package-to-package dependency validation.** DESIGN §7.3 — rejected
  architecturally (capabilities are the dependency unit).

---

## Validation Rules

Six rules, one test fixture per failure mode plus one all-good
fixture. Rules are evaluated in fixed order; a rule failure does not
short-circuit the remaining rules (the CLI reports every finding in
one pass). Individual per-file JSON-parse failures do short-circuit
*within that file only* (you can't validate further rules on a
manifest that doesn't parse) — the report still lists every
whole-file failure across the suite.

| # | Rule | Severity | Stops within-file? | Condition |
|---|------|----------|---------------------|-----------|
| R1 | Required files present | error | no | `manifest.json` and `capabilities.json` both exist at the package root. Missing either produces one error per missing file. |
| R2 | Forbidden paths absent | error | no | No symlinks anywhere in the tree. No path component equal to `..` (path-traversal). No absolute path (applies when validating an archive; for directory input, `realpath` must remain under the supplied path's `realpath` — detects symlinks outside). No files outside the declared layout directories (everything sits under one of: `server/`, `client/`, `migrations/`, or is a top-level manifest file from §Known-Files below). |
| R3 | JSON files parse | error | yes (for that file) | `manifest.json`, `capabilities.json`, and any optional `rbac.json` / `panels.json` / `config.json` parse with `nlohmann::json::parse`. The 0.4.1 `PackageManifest::parse` / `CapabilityManifest::parse` are invoked for the two required files to surface structural errors (missing `name`, bad SemVer, etc.); their `ManifestParseError` translates 1:1 into `ValidationMessage{severity: error, rule: "json-structure"}`. |
| R4 | Handlers declared in `capabilities.json` have source files | warning | no | For every `provides[].function` in `capabilities.json`, check `server/handlers/{function}.js` exists. Per architecture/05-extensions.md §1.1 this is a warning; DESIGN-packages-v04x.md §0.4.2 upgrades it to an error during cross-file validation. 0.4.0 keeps the warning severity to match the architecture doc; 0.4.2 re-classifies. |
| R5 | Panels referenced in `panels.json` have source files | error | no | If `panels.json` exists, every entry with a `client_path` string field is checked against `client/panels/{value}` and `client/components/{value}`. 0.4.0 accesses the field via raw `nlohmann::json` (the typed `PanelsManifest` parser is deferred to 0.4.2). The exact field name (`client_path` vs a structured alternative) is finalized when 0.4.2 authors the typed parser; for 0.4.0, any entry carrying a string-typed `client_path` triggers the check. Missing file is an error because a panel with no source file can never mount. |
| R6 | Size limit respected | error | no | Sum of all file sizes under the package root (excluding VCS metadata — `.git`, `.hg`, `.svn`) ≤ configured maximum (default 50 MB per architecture/05-extensions.md §3.1). Configurable via `--max-size <bytes>` flag. |

**Known-files at package root** (for R2): `manifest.json`,
`capabilities.json`, `rbac.json`, `panels.json`, `config.json`,
`README.md`, `LICENSE`, `CHANGELOG.md`, `.plinthignore` (reserved for
future pack-time exclusion rules — see §Open Questions).

**Archive vs. directory input:** 0.4.0 accepts a directory path only.
Validating a zipped package goes through 0.4.4's install lifecycle
which unpacks first. Rationale: rule R2's absolute-path check is
natural on archives (zip entries carry paths) but awkward on
directories (the "absolute" concept doesn't apply to an already-
checked-out tree). Keeping 0.4.0 directory-only sidesteps the
duplication; `plinth install` is the zip path.

---

## CLI Contract

```
$ plinth validate <path> [--max-size <bytes>] [--json] [--quiet]
```

**Arguments:**

- `<path>` (positional, required) — directory path to validate.
  Must exist. Must be readable. Non-existent path or unreadable path
  produces exit code 1 with a single error: `{rule: "input", message:
  "path does not exist"}` or `{rule: "input", message: "path is not
  readable"}`. No other rules run.

**Options:**

- `--max-size <bytes>` — override the default 50 MB size limit. Useful
  for dev-time packages with large test fixtures.
- `--json` — emit machine-readable JSON instead of text. JSON shape:
  ```json
  {
    "path": "<absolute path that was validated>",
    "exit_code": <0 | 1 | 2>,
    "messages": [
      {
        "severity": "error" | "warning",
        "rule": "<rule-name>",
        "path": "<path relative to package root, or null>",
        "message": "<human-readable>",
        "remediation": "<optional actionable hint>"
      }
    ]
  }
  ```
  One object per invocation. Newline-terminated. No streaming.
- `--quiet` — suppress text output entirely; rely on exit code. Does
  not affect `--json`; the two flags compose. Rationale: CI scripts
  want exit code and don't want `stderr` noise; `--json` output goes
  to stdout.

**Exit codes:**

| Code | Meaning |
|------|---------|
| 0 | Validation passed; zero errors, zero warnings. |
| 1 | Validation failed; at least one error. |
| 2 | Validation passed with warnings; zero errors, ≥ 1 warning. Package is installable but something is non-ideal (e.g., a declared capability has no handler file — R4). |

**Text output:**

- One line per message, prefixed with severity (`error:` / `warning:`)
  and rule name, followed by `path:` reference when applicable.
  Terminal colour if `stdout` is a TTY (`error` red, `warning`
  yellow); `--quiet` suppresses text entirely; `NO_COLOR=1` env var
  (spec: `https://no-color.org/`) suppresses colour. spdlog is **not**
  used for validate output — validate is a CLI tool whose audience is
  an extension author at a terminal, not a running kernel.
- Trailing summary line: `validated <N> files, <E> errors, <W>
  warnings`. Omitted under `--quiet`.
- If remediation hint is populated, emit it indented on the line
  following the finding. Example:
  ```
  error: required-files: missing manifest.json at ./my-ext/
    hint: create manifest.json with at minimum name, version, and entry_point fields
  ```

**`--json` vs. text** — same rules fire; text is human-formatted, JSON
is structured. Exit code is identical across formats.

---

## Library Surface

```cpp
// src/kernel/packages/validator.hpp
namespace plinth::packages {

enum class Severity : std::uint8_t { Error, Warning };

struct ValidationMessage {
    Severity severity;
    std::string rule;             // e.g. "required-files", "forbidden-paths"
    std::optional<std::string> path;   // relative to package root, when applicable
    std::string message;
    std::optional<std::string> remediation;
};

struct ValidationReport {
    std::vector<ValidationMessage> messages;
    std::size_t files_scanned = 0;
    std::size_t total_bytes = 0;
    auto error_count() const noexcept -> std::size_t;
    auto warning_count() const noexcept -> std::size_t;
    auto disposition() const noexcept -> int;  // 0 / 1 / 2 per §CLI Contract
};

struct ValidationConfig {
    std::size_t max_size_bytes = 50 * 1024 * 1024;  // default 50 MB
};

auto validate(const std::filesystem::path& package_root,
              const ValidationConfig& cfg = {}) -> ValidationReport;

}  // namespace plinth::packages
```

**Why a library function and not inline in `main.cpp`:** the same
validator runs in 0.4.4 during the install lifecycle's VALIDATING
stage, and in 0.4.2 under the `--against-running-kernel` extension.
Both call through the same library entry point; only the cross-file /
runtime-state rule set differs.

**No exceptions on the happy-or-warning path.** Returning
`ValidationReport` with the `messages` vector is the reporting
channel; the function does not throw for detectable validation
failures. It **does** throw `std::filesystem::filesystem_error` for
unrecoverable I/O problems (permission denied mid-scan, path deleted
during scan); `main.cpp` catches this in the existing
`catch (const std::exception&)` arm at line 168 and maps to exit code
1 with a synthesized error message. Rationale: these are genuinely
exceptional conditions; wrapping them in the report machinery would
mask the root cause.

---

## Fixture Packages

Under `tests/fixtures/packages/`, one directory per test case. Catch2
cases iterate the directory and assert the report disposition +
specific message presence.

| Fixture | Expected disposition | Rule under test |
|---|---|---|
| `valid-minimal/` | 0 (pass) | Baseline: `manifest.json` + `capabilities.json` only, one capability with a handler file. |
| `valid-full/` | 0 (pass) | All optional files present: `rbac.json`, `panels.json`, `config.json`, `README.md`; `server/handlers/` for every capability; `client/panels/` for every panel; `migrations/` with one migration. |
| `missing-manifest/` | 1 (error) | R1: no `manifest.json`. |
| `missing-capabilities/` | 1 (error) | R1: no `capabilities.json`. |
| `symlink-outside/` | 1 (error) | R2: a `server/escape.js` is a symlink to `/etc/hostname`. |
| `dotdot-path/` | 1 (error) | R2: a `server/../escape.js` — only reproducible in archive-mode; for directory-input fixture, a symlink resolving outside the package root stands in. |
| `stray-file/` | 1 (error) | R2: a top-level `random.js` not in any known layout directory. |
| `bad-manifest-json/` | 1 (error) | R3: `manifest.json` has a trailing comma. Parser reports line + column. |
| `bad-capabilities-json/` | 1 (error) | R3: `capabilities.json` has a missing `"function"` field — structural error from 0.4.1's parser. |
| `handler-missing/` | 2 (warning) | R4: `capabilities.json` declares `greet` but `server/handlers/greet.js` does not exist. |
| `panel-missing/` | 1 (error) | R5: `panels.json` references `client/panels/missing.js` which doesn't exist. |
| `oversize/` | 1 (error) | R6: a 60 MB blob under `client/assets/`; validate with default 50 MB limit. |
| `oversize-tunable/` | 0 (pass) | Same blob as `oversize/`; run with `--max-size 104857600` (100 MB). Proves the flag works. |

Test count target: **~13 fixtures**, one per meaningful path. Not
every rule gets two fixtures (size-exceed and under-limit, handler
present and missing — only one negative per rule); the `valid-full/`
fixture is the positive case for all non-negative rules jointly.

---

## Implementation Hints

The ICD does not prescribe the internal code shape beyond the public
surface above. These are observations for the implementing session;
they are non-normative.

- **Filesystem walk.** `std::filesystem::recursive_directory_iterator`
  with `follow_directory_symlink = false`. Every entry is inspected
  for symlink status (R2) before its content is read. VCS directories
  (`.git`, `.hg`, `.svn`) skip descent but count toward neither R2
  (expected stray files) nor R6 (size).
- **Size accounting.** Sum via `std::filesystem::file_size` on the
  filtered entries. Fail R6 as soon as the running total crosses the
  limit to avoid scanning an arbitrarily large tree.
- **JSON parsing** uses the same `nlohmann::json` facade the kernel
  already links. 0.4.1's parsers take a `std::string_view` + a
  `ManifestParseError*` out-parameter; 0.4.0 forwards the string,
  collects the error, maps it to a `ValidationMessage`.
- **Unicode path handling.** `std::filesystem::path` native encoding
  on Linux is UTF-8. For the reports, use `path::generic_string()`
  (forward-slash separators) so the output is stable across
  platforms — even though 0.4.0 only runs on Linux today, the fixture
  golden files shouldn't need per-platform variants.
- **Handler-file suffix.** Architecture doc says `.js`. 0.4.0 matches
  exact `<function>.js`. `.mjs` / `.ts` / bundler output — out of
  scope until the bundler story lands (0.6.x shell introduces the
  first bundled client; server-side bundling is unclaimed scope).

---

## Security Constraints

1. **`realpath` every candidate path before `file_size` / `is_regular_file`.**
   A symlink-to-`/dev/zero` would read forever otherwise. The R2
   symlink-detection pass happens before R6 size accounting for exactly
   this reason.
2. **Archive magic-number check is not 0.4.0's job.** Directory input
   means the bytes on disk are whatever the filesystem said. Magic-
   number / MIME checks belong to 0.4.4's UPLOADING stage.
3. **No kernel state access.** 0.4.0 never touches PG, never touches
   `plinth.packages`, never reads any config file except the one
   passed explicitly. A user running `plinth validate` must not
   inadvertently connect to the configured database. This means
   `load_config` is **not** called on the validate path; the
   `ValidationConfig` struct is constructed from CLI flags only.
4. **stdout / stderr discipline.** Text output goes to stdout for
   the findings (machine-parseable with `grep`); errors from the
   CLI layer itself (unrecognized flag, path-not-exist) go to stderr
   per POSIX convention. `--json` always goes to stdout.

---

## Out-of-Scope — Architecture-Preserving Constraints

Per DESIGN §2 "deferred but committed" / "deferred and uncommitted"
rules, 0.4.0 must not foreclose any of:

- **Hot-reload (0.10.4).** 0.4.0 is a pure static analyzer; it
  doesn't observe file changes. Nothing to preserve.
- **Share primitive (0.11 if picked up).** Reserved `shareable[]`
  slot is 0.4.1's concern; 0.4.0 invokes the 0.4.1 parser which
  accepts the reserved-empty array.
- **Sidecar packaging (0.8.x).** If sidecars eventually share the
  manifest format, validate rules R1–R6 apply unchanged. If sidecars
  define a separate manifest, 0.4.0 doesn't validate them — and
  that's fine. Design is silent, not foreclosing.
- **Remote package registry.** 0.4.0 only runs on local directories.

---

## Milestone Criteria

All 13 fixtures (approx.) pass their Catch2 assertions. `--json`
output parses as valid JSON for every fixture. CLI exit codes map
correctly to report disposition. `run-clang-tidy-20` clean on the new
translation units. `ctest` 100% pass count matches the existing
baseline + the new fixture tests.

### CI Wiring

- `src/kernel/packages/validator.hpp` — **new**. Public library
  surface.
- `src/kernel/packages/validator.cpp` — **new**. Implementation.
- `src/kernel/main.cpp` — replaces TODO body at lines 155–167 with
  a call into `plinth::packages::validate(path, cfg)` and exits
  via `report.disposition()`. Adds `--max-size` / `--json` /
  `--quiet` flag declarations to the existing `validate_cmd`
  argparse object.
- `tests/fixtures/packages/` — **new directory**. 13 fixture
  packages.
- `tests/kernel/packages/validator_test.cpp` — **new**. Catch2
  driver. One `TEST_CASE` per fixture.
- `tests/kernel/packages/cli_test.sh` — **new**. Shell-script
  driver for the CLI-surface exit-code tests. Wired as a
  `add_test(NAME validate_cli ...)` entry in `CMakeLists.txt`;
  invokes the built binary directly and asserts exit codes.
- `CMakeLists.txt` — add the two new source files to the
  kernel library; add the new test directory to
  `plinth_tests`. The 0.3.3.2 `KERNEL_SOURCES` glob already
  covers the new `src/kernel/packages/**` and `tests/kernel/**`
  files for the `tidy` target.
- No new CI job. No new CMake option.

---

## Entry / Exit

**Entry:** ICD-0.4.1 implementation merged (the validator calls into
0.4.1's parsers for rules R3 and for R5's panel-field lookup).
Without 0.4.1, R3 degrades to a naive `nlohmann::json::parse` with no
structural error detail — acceptable for a dev preview but insufficient
for the 0.4.0 exit criteria. ICD-0.4.1 lands first in a single
code session that also ships the 0.4.0 library; or 0.4.0 and 0.4.1
ship in the same branch and squash together (architect chooses at
implementation time).

**Exit:** All 13 fixture tests pass under Catch2 default and sanitizer
builds; `run-clang-tidy-20` clean; `plinth validate` CLI manually
verified against all fixtures with correct exit codes and text /
JSON output; CHANGELOG entry describes the validator scope + fixture
set; ROADMAP `0.4.0` line removed; DEFERRED.md unchanged (no new
deferrals expected from this milestone).

---

## Open Questions (resolve during implementation)

1. **`.plinthignore` reservation.** Reserve the filename so a future
   milestone (pack-time exclusion rules) can use it without breaking
   0.4.0-validated packages? Recommended: yes — add to Known-files
   for R2 with a comment that the file is currently ignored, not
   consumed. Costs nothing.
2. **R2 directory-input vs. archive-input divergence.** 0.4.0 accepts
   directories only; archive validation is 0.4.4. The `..` path-
   traversal check is natural on archives, awkward on directories.
   Proposed: symlink-resolving-outside-root is the directory-input
   analogue, ships in 0.4.0. The literal `..` rule in R2's
   description becomes R2a (archive-mode, 0.4.4) vs R2b (directory-
   mode, 0.4.0). Document this split in the PR.
3. **Concurrent filesystem mutation during scan.** If a file is
   deleted or size-changed mid-scan, the walk may race. Proposed:
   catch `std::filesystem::filesystem_error` at the entry level,
   record a warning with rule `"filesystem-race"`, continue. The
   exit disposition is still coherent (pass-with-warning is the
   safer default than a hard failure on a transient mutation).
4. **R6 size accounting granularity.** Include empty directories?
   Dotfiles? Proposed: include empty-dir inodes at 0 bytes (they
   don't affect the total); skip `.git` / `.hg` / `.svn` trees
   entirely (VCS metadata is not the package). Other dotfiles
   (e.g., `.plinthignore`, `.env.example`) count normally. Document
   in the PR.

---

## Appendix: Example `validate` Invocation

```console
$ plinth validate ./my-notes-ext/
error: required-files: missing capabilities.json at ./my-notes-ext/
  hint: create capabilities.json with a provides[] entry for each exported capability
warning: handler-missing: capabilities.json declares notes.edit but server/handlers/edit.js not found
  hint: create server/handlers/edit.js, or remove notes.edit from capabilities.json
validated 14 files, 1 errors, 1 warnings
$ echo $?
1
```

```console
$ plinth validate ./notes-ext/ --json
{"path":"/tmp/plinth/notes-ext","exit_code":0,"messages":[],"files_scanned":22,"total_bytes":412873}
$ echo $?
0
```

```console
$ plinth validate ./big-ext/ --max-size 104857600
validated 31 files, 0 errors, 0 warnings
$ echo $?
0
```
