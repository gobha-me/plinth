# ICD-0.4.2-cross-file-manifest-validation

**Traces to:** DESIGN-packages-v04x.md §0.4.2 (Cross-File Manifest Validation Pass — the authoritative rule set); architecture/05-extensions.md §1.1 (Cross-File Manifest Validation — the architecture-level enumeration); architecture/05-extensions.md §2 (Reserved URL Prefixes — input to rule CF5); architecture/01-identity.md §2 (RBAC rule registration — source of the `rbac.json` rule shape read by CF1 / CF2 / CFW1).
**Depends on:** ICD-0.4.0-package-structure-validation (`validate()` entry point + `ValidationReport` + `ValidationConfig` — 0.4.2 extends, does not replace); ICD-0.4.1-manifest-parsing (`PackageManifest::parse` and `CapabilityManifest::parse` — 0.4.2 consumes the typed outputs; `ManifestParseError` shape).
**Milestone:** 0.4.2 — cross-file manifest validation pass, wired into `plinth validate` as the new default and into 0.4.4's VALIDATING stage at install time.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** `src/kernel/packages/validator.cpp:474` (the existing `validate()` entry — 0.4.2 adds a new post-parse pass, not a replacement); DESIGN-packages-v04x.md §0.4.3 (schema-creation milestone that consumes the validated package; 0.4.2 is 0.4.3's entry criterion); DESIGN-packages-v04x.md §0.4.6 (rule-validator RBAC rule registration — consumes the typed `rbac.json` shape 0.4.2 validates structurally); DESIGN-packages-v04x.md §2 "deferred but committed" (every cross-file rule preserves hot-reload, share primitive, sidecar packaging).

---

## Overview

0.4.2 answers the question 0.4.0 explicitly defers: "given the package files, are they internally consistent as a whole?" — where "consistent" means *the namespace matches, the file references point at files that exist, the RBAC rules reference capabilities the package actually provides, and the package doesn't collide with a reserved kernel name or URL prefix*.

0.4.0 was the single-file pre-flight. 0.4.2 is the whole-package pre-flight. Together they form the full static validation set that 0.4.4's install lifecycle runs at the VALIDATING stage.

0.4.2 does **not** answer "will this install into this specific running kernel?" — that is the runtime-state check set, gated behind the new `--against-running-kernel` flag, which queries a live kernel for package-name collisions, frontend-mount conflicts, and capability-requirement resolution. Runtime-state checks are a superset of cross-file checks, not a replacement; they run after cross-file passes.

The implementation extends `ValidationConfig` with a `cross_file: bool = true` flag (default on — cross-file is the new default disposition of `plinth validate`). A new `src/kernel/packages/cross_file_validator.{hpp,cpp}` translation unit hosts the post-parse pass. The existing `validate()` entry point calls `run_cross_file_validation()` after `run_json_parse()` succeeds on the two required manifests; cross-file findings land in the same `ValidationReport.messages` vector. No new CLI subcommand — cross-file runs under `plinth validate` by default.

**Scope:**

- New library function `plinth::packages::run_cross_file_validation(const ParsedPackage&, const ValidationConfig&, Reporter&)` — takes the already-parsed manifest outputs from 0.4.1 plus raw `nlohmann::json` for `rbac.json` / `panels.json` / `config.json`, and emits findings into the existing `Reporter` surface.
- Typed parser for `panels.json` — `plinth::packages::PanelsManifest::parse()` mirroring the 0.4.1 pattern. (Deferred in 0.4.0 as "accessed via raw `nlohmann::json`"; 0.4.2 promotes it to typed.)
- Typed parser for `config.json` — `plinth::packages::ConfigManifest::parse()`, also mirroring 0.4.1. Validates the default-values object shape (key names, scalar-only values until 0.4.x grows richer config types).
- `rbac.json` remains raw `nlohmann::json` access in 0.4.2 — typed parser is 0.4.6's scope. 0.4.2 touches `rbac.json` only to walk `rule.namespace` and `rule.test.call` fields for the cross-file checks.
- `ValidationConfig` extension: `bool cross_file = true`, `bool against_running_kernel = false`. Both additive, no existing-flag churn.
- CLI integration: cross-file runs by default under `plinth validate`. Opt-out is `--structure-only` (emits the 0.4.0 rule set only). Runtime-state adds `--against-running-kernel` (requires a reachable kernel via config).
- Fixture set: one passing `valid-cross-file/` package plus one failing fixture per CF / CFW rule. Catch2 driver at `tests/kernel/packages/cross_file_validator_test.cpp`.

**Out of scope (deferred):**

- **Runtime-state rules** (RT1 / RT2 / RT3 below) land behind `--against-running-kernel`. 0.4.2 implements the flag and the three queries (`plinth.packages` scan, `plinth.panels` scan, registry lookup), but a running kernel is required to exercise them. Fixtures for RT rules use a live PG-backed test harness patterned on the 0.2.x `listener_integration_test`; a stand-alone `plinth validate --against-running-kernel` against a closed socket returns exit 1 with a single `{rule: "kernel-unreachable"}` finding.
- **Typed `rbac.json` parser.** Deferred to 0.4.6. 0.4.2 reads `rbac.json` as raw JSON and walks the structure per `architecture/01-identity.md §2` example shape; any structural error is reported under rule `"rbac-json-shape"` without rich field-level error messages.
- **Capability-signature semantic validation beyond the 0.2.1 parser.** CF2's test-contract `call` parse uses the existing `plinth::capabilities::parse_signature` from 0.2.1 — 0.4.2 does not re-implement any of that.
- **Per-file line/column error offsets for cross-file findings.** Single-file parse errors carry line/column (from 0.4.1); a cross-file finding points at a rule + an optional file path, but doesn't attempt to locate the specific byte offset in the consumed JSON. Good enough for the admin-facing error UX 0.4.2 targets.
- **Auto-fix / `plinth validate --fix` ergonomics.** Non-MVP.
- **`shareable[]` semantic validation.** CFW2 (warning) only fires when the array is non-empty. Structural validation of a non-empty array is 0.11 scope.

---

## Cross-File Validation Rules

Rules execute after 0.4.0's R1–R6 finish. If any R-rule reports an error, cross-file rules still run (every finding in one pass) — but rules consuming manifest content rely on the 0.4.1 parser having succeeded; if `manifest.json` failed to parse, rules referencing `manifest.name` or `manifest.frontend.mount` are skipped with a trailing trace entry `{rule: "cross-file-skipped", message: "manifest.json did not parse"}`. Same discipline for `capabilities.json`.

Rules are evaluated in fixed order; a rule failure does not short-circuit the remaining rules.

| # | Rule | Severity | Condition |
|---|------|----------|-----------|
| CF1 | `rbac.json` rule namespace matches a capability namespace | error | For every `rules[].namespace` in `rbac.json`, that value appears as a `provides[].namespace` in `capabilities.json`, OR is a reserved kernel prefix (`kernel`, `system`, `plinth` — exact list from kernel bootstrap). Missing match emits one error per orphaned rule. |
| CF2 | RBAC test-contract `call` references an existing capability | error | For every `rules[].test.call` in `rbac.json`, parse via `plinth::capabilities::parse_signature` (0.2.1). If parse succeeds, the resulting `{namespace, version, function}` must match a `provides[]` entry in `capabilities.json`. Parse failure is its own error with `rule: "rbac-test-call-parse"`. |
| CF3 | Panels reference existing client files | error | For every entry in `panels.json`, `client_path` (relative path under `client/`) must resolve to an existing regular file under `client/panels/` or `client/components/`. The typed `PanelsManifest::parse` surfaces any structural error first; CF3 runs only on parsed entries. |
| CF4 | Provided capabilities have handler files (error, was warning) | error | DESIGN-packages-v04x §0.4.2 upgrades 0.4.0's R4 warning to an error at the cross-file level. For every `provides[]` entry in `capabilities.json` declaring a `function`, `server/handlers/{function}.js` must exist. R4 (warning) and CF4 (error) are separate rules emitting separate messages — both may appear in a report, but CF4 being an error sets the disposition. |
| CF5 | `frontend.mount` does not collide with reserved prefix | error | If `manifest.frontend.mount` is set, the normalized value (leading slash, no trailing slash) must not be `/` *if* `/api`, `/ext`, `/s`, `/healthz`, `/metrics`, `/ws` are kernel-reserved (root redirect is configurable — `/` is permitted for a shell/frontend extension). The normalized value must not start with any of `/api/`, `/ext/`, `/s/`, `/healthz`, `/metrics`, `/ws` (with or without trailing slash). Source of truth: `architecture/05-extensions.md §2`. |
| CF6 | Package `name` is not reserved | error | `manifest.name` must not equal `kernel`, `plinth`, `system`. Case-sensitive match (the 0.4.1 parser already enforces the `^[a-z][a-z0-9_-]{2,62}$` regex, so case variants are already rejected at parse time). |
| CF7 | Capability namespace matches package name | error | For every `provides[]` entry in `capabilities.json`, `namespace` must equal `manifest.name`, OR the package must declare itself a provider extension (deferred — see §Open Questions; until then, namespace-ne-name fails CF7 unconditionally). |
| CFW1 | Every provided capability has at least one RBAC rule | warning | For every `provides[]` entry in `capabilities.json`, at least one `rules[]` entry in `rbac.json` has `namespace == provides[].namespace`. A package with provided capabilities but no `rbac.json` triggers one warning per capability. Per architecture/05-extensions.md §1.1 this is informational — the package can still install, but an un-ruled capability can never be granted to a group. |
| CFW2 | `shareable[]` is empty (reserved) | warning | If `manifest.shareable` is present and non-empty, emit a warning citing that the share primitive is not yet available (deferred to 0.11 per DESIGN-packages-v04x §2 "deferred and uncommitted"). No structural validation of the array entries — they're treated as opaque strings. |
| CFW3 | `entry_point` does not import from `client/` | warning | Static string scan: open `manifest.entry_point` (already validated to exist by 0.4.0's R1-adjacent logic), look for `import … from "./client/…"` or `require("./client/…")` patterns. A match emits a warning — sign of a mis-structured package mixing server and client code. Bare-string match only; 0.4.2 does not invoke a JS parser. |
| CFW4 | `runtime.memory_limit_mb` exceeds kernel maximum | warning | If `manifest.runtime.memory_limit_mb` is set and exceeds the kernel's default maximum (`architecture/05-extensions.md §3.1` — 64 MB as of today), emit a warning noting the value will be clamped at install time. The kernel's maximum is a build-time constant exposed via `plinth::js::runtime_config::get_max_memory_mb()`; 0.4.2 reads it without needing a running kernel. |
| RT1 | No other installed package claims same `name` | error (runtime-state) | `SELECT 1 FROM plinth.packages WHERE name = ? AND state != 'UNINSTALLING'` returns no rows (excepting the case where the matching row is the same extension being upgraded — matched by `provenance` or version-comparison logic that belongs to 0.4.4, not 0.4.2; 0.4.2 emits the raw collision finding and lets 0.4.4 decide upgrade-vs-reject). |
| RT2 | No other installed package claims same `frontend.mount` | error (runtime-state) | `SELECT 1 FROM plinth.packages WHERE frontend_mount = ? AND state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED')` returns no rows (a DISABLED package still owns its mount until uninstalled; installing a new extension at the same mount must uninstall or remap the disabled one first). |
| RT3 | Every `requires` capability exists in the registry | error (runtime-state) | For every `requires[]` entry in `capabilities.json`, invoke `plinth::capabilities::lookup_signature(sig)` against the running registry. A miss emits an error with the missing signature; a package depending on `kernel:*` capabilities that are themselves deferred (per 0.2.x DEFERRED entries) emits a warning with rule `"deferred-requires"` instead of a hard error — the install will proceed but the capability will return `capability_not_found` at runtime until the kernel capability ships. |

**Rule-prefix reserved namespaces** (CF1): `kernel`, `system`, `plinth`. Matches the CF6 reserved-name list. Expansion requires a kernel-bootstrap change and is tracked via the `architecture/01-identity.md §2` rule-convention document.

---

## CLI Contract

```
$ plinth validate <path> [--max-size <bytes>] [--json] [--quiet]
                        [--structure-only | --against-running-kernel]
```

**New flags (additive; 0.4.0 flags unchanged):**

- `--structure-only` — run the 0.4.0 rule set R1–R6 only. Skip cross-file (CF/CFW) and runtime-state (RT) rules. Rationale: extension authors mid-edit may want the fast pre-flight before manifests are consistent yet. Default is cross-file-on, matching DESIGN §0.4.2's "new default disposition."
- `--against-running-kernel` — in addition to the default cross-file set, run the three RT rules. Implies network/PG connectivity. Reads kernel config from `~/.config/plinth/client.yml` (new file — see §Open Questions) or `--kernel <url>` for an explicit target. On unreachable kernel, emit one error `{rule: "kernel-unreachable", message: "..."}` and abort RT rules (cross-file findings still report). `--structure-only` and `--against-running-kernel` are mutually exclusive; combining both emits a CLI usage error (exit 1 with stderr message, no rule findings).

**Exit codes (unchanged from 0.4.0):**

| Code | Meaning |
|------|---------|
| 0 | Pass with zero errors, zero warnings. |
| 1 | One or more errors (any rule R*, CF*, or RT*). |
| 2 | Pass with warnings (any CFW*, or R4 if `--structure-only`). |

**`--json` shape extension:** each `messages[]` entry gains a `phase` field — one of `"structure"`, `"cross-file"`, `"runtime-state"` — to let CI scripts filter by validation layer. Additive; legacy consumers ignore the new field.

---

## Library Surface

```cpp
// src/kernel/packages/validator.hpp — ValidationConfig extension
namespace plinth::packages {

struct ValidationConfig {
    std::size_t max_size_bytes = 50 * 1024 * 1024;  // 0.4.0
    bool cross_file = true;                          // NEW in 0.4.2 — default on
    bool against_running_kernel = false;             // NEW in 0.4.2
    std::optional<std::string> kernel_url;           // NEW in 0.4.2 — populated by CLI --kernel
};

}  // namespace plinth::packages
```

```cpp
// src/kernel/packages/cross_file_validator.hpp — NEW in 0.4.2
namespace plinth::packages {

struct ParsedPackage {
    PackageManifest manifest;           // from 0.4.1
    CapabilityManifest capabilities;    // from 0.4.1
    std::optional<PanelsManifest> panels;   // NEW typed parser in 0.4.2
    std::optional<ConfigManifest> config;   // NEW typed parser in 0.4.2
    std::optional<nlohmann::json> rbac_raw;  // typed parser deferred to 0.4.6
};

auto run_cross_file_validation(const ParsedPackage& pkg,
                               const std::filesystem::path& package_root,
                               const ValidationConfig& cfg,
                               Reporter& r) -> void;

auto run_runtime_state_validation(const ParsedPackage& pkg,
                                  const ValidationConfig& cfg,
                                  Reporter& r) -> void;

}  // namespace plinth::packages
```

```cpp
// src/kernel/packages/panels_manifest.hpp — NEW in 0.4.2 (mirror of capabilities_manifest.hpp)
namespace plinth::packages {

struct PanelEntry {
    std::string id;
    std::string client_path;
    std::optional<std::string> title;
    // structurally-validated fields per DESIGN-packages-v04x §0.4.2 panels schema
};

struct PanelsManifest {
    std::vector<PanelEntry> panels;
};

auto parse_panels(std::string_view raw) -> ParseResult<PanelsManifest>;

}  // namespace plinth::packages
```

`ConfigManifest` follows the same shape. Struct fields track the DESIGN-packages-v04x config-schema discussion.

**Why two entry points (cross-file vs runtime-state):** cross-file is deterministic from files on disk; runtime-state needs a live kernel. Separating lets 0.4.4's install lifecycle run cross-file synchronously at VALIDATING and runtime-state against the in-process registry without the roundtrip through the CLI's HTTP path. The CLI plumbs `ValidationConfig.against_running_kernel = true` and invokes the HTTP path; the in-process install lifecycle passes the flag false at the CLI-layer and instead calls the in-process registry directly (detail for 0.4.4's ICD).

**Existing `validate()` composition:** after the 0.4.0 file walk, if `cfg.cross_file` is true, parse all five possible manifest files into a `ParsedPackage`, then call `run_cross_file_validation`. If `cfg.against_running_kernel` is true, further call `run_runtime_state_validation`. Findings accumulate in the same `ValidationReport.messages` — no separate reports.

---

## Fixture Packages

Under `tests/fixtures/packages/`, one directory per cross-file test case. 0.4.0's existing fixtures remain; 0.4.2 adds ~15 more covering CF / CFW / RT.

| Fixture | Expected disposition | Rule under test |
|---|---|---|
| `valid-cross-file/` | 0 (pass) | Baseline: full package with `capabilities.json` + `rbac.json` + `panels.json` + handlers + panel client files all consistent. Cross-file rules all pass. Superset of 0.4.0's `valid-full/`. |
| `rbac-orphan-namespace/` | 1 (error) | CF1: `rbac.json` declares a rule with `namespace: "other"` while `capabilities.json` provides only `namespace: "my-ext"`. |
| `rbac-test-bad-call/` | 1 (error) | CF2: `rbac.json` rule's `test.call` is `"my-ext:99:unknown"` — parses but doesn't match any provided capability. |
| `rbac-test-parse-fail/` | 1 (error) | CF2 parse-failure variant: `test.call` is `"not a valid signature"`. |
| `panel-missing-client-file/` | 1 (error) | CF3: `panels.json` entry references `client/panels/ghost.js` which doesn't exist. |
| `cf4-handler-missing/` | 1 (error) | CF4: same construction as 0.4.0's `handler-missing/` but expected disposition is 1 (error) not 2 (warning) — demonstrates the severity upgrade at cross-file level. Under `--structure-only` this fixture returns 2. |
| `frontend-mount-reserved-api/` | 1 (error) | CF5: `manifest.frontend.mount = "/api/custom"`. |
| `frontend-mount-reserved-ext/` | 1 (error) | CF5: `manifest.frontend.mount = "/ext"` (exact match of reserved prefix). |
| `name-reserved-kernel/` | 1 (error) | CF6: `manifest.name = "kernel"`. |
| `capability-namespace-mismatch/` | 1 (error) | CF7: `manifest.name = "notes"` but `capabilities.json` provides `namespace: "other-ns"`. |
| `capability-without-rule/` | 2 (warning) | CFW1: `capabilities.json` provides a capability, no matching `rbac.json` rule. Informational. |
| `shareable-non-empty/` | 2 (warning) | CFW2: `manifest.shareable = ["public-surface"]`. Warning only. |
| `entry-imports-client/` | 2 (warning) | CFW3: `server/main.js` contains `import foo from "../client/lib.js"`. |
| `memory-over-max/` | 2 (warning) | CFW4: `manifest.runtime.memory_limit_mb = 4096`. |
| `valid-cross-file-structure-only-vs-default/` | 0 / 1 | Dual-mode: same fixture passes under `--structure-only` (0) and fails under default cross-file (1, because `rbac.json` declares a rule with namespace mismatch that R-rules don't check). Proves the flag flips disposition. |

**Runtime-state fixtures** (require live PG + kernel; opt-in via `PLINTH_KERNEL_TESTS=ON` CMake flag, matching the 0.2.x integration-test pattern):

| Fixture | Expected disposition (kernel running) | Rule |
|---|---|---|
| `rt-name-collision/` | 1 (error) | RT1: pre-install a package with `name: "existing-pkg"`; validating a new package with same name triggers collision. |
| `rt-mount-collision/` | 1 (error) | RT2: pre-install a package with `frontend.mount: "/custom"`; validating another package with same mount triggers collision. |
| `rt-missing-requires/` | 1 (error) | RT3: `capabilities.json` requires `"unknown-ext:1:fn(int)->result"`; registry has no match. |

Test count target: **~15 static fixtures + 3 runtime-state fixtures = ~18 total**. Static fixtures live alongside 0.4.0's; runtime-state fixtures gated behind the CMake flag with a skip message when the flag is off.

---

## Implementation Hints

Non-normative observations for the implementing session.

- **Parse-once discipline.** `validate()` opens each manifest file at most once; the `ParsedPackage` threads the results through both the existing rule set and the new cross-file set. Re-parsing in `run_cross_file_validation` is a bug.
- **`rbac.json` raw walk pattern.** Mirror 0.4.0's R5 raw-JSON access for `panels.json`. Paths: `rbac.rules[].namespace`, `rbac.rules[].test.call`. Fail the specific rule with `{rule: "rbac-json-shape", path: "rbac.json", message: "rules[N].namespace missing"}` if any expected path is absent. 0.4.6's typed parser will produce richer errors later.
- **`parse_signature` reuse.** `plinth::capabilities::parse_signature(sv) -> std::expected<Signature, ParseError>` from 0.2.1 is the single source of truth for capability-invocation strings. CF2 calls it for every `test.call`; RT3 calls it for every `requires[]`. No alternate parser.
- **`runtime_config::get_max_memory_mb()` accessor.** If the kernel doesn't expose this today, 0.4.2 adds a thin accessor header under `src/kernel/js/runtime_config.{hpp,cpp}` or similar — the build-time constant is already in the QuickJS hardening surface (0.3.1), just needs a getter. Minimal patch.
- **CLI flag mutual exclusion.** `argparse` already in use; the existing pattern at `src/kernel/main.cpp` for `validate_cmd` handles flag declarations. Use `argparse::Argument::add_mutually_exclusive_group` for `--structure-only` vs `--against-running-kernel`.
- **Kernel HTTP client for `--against-running-kernel`.** Minimal — a single `POST /api/packages/validate` endpoint on the kernel side (declared in this ICD for 0.4.4 to implement; 0.4.2 stubs it as a `Not Implemented` response if not yet wired). Alternative: extend the existing admin session-auth path. Architect decision at implementation time.
- **`entry_point` static import scan.** Use `std::regex` with pattern `import\s+.*?\s+from\s+['"](\.\./?client/|\./client/)` — good enough for CFW3's bare-string match. Escape paths for `client/` at the beginning of the string too (`"./client/"` or `"../client/"`). Not a proper JS parser; false positives on non-import string literals are acceptable at warning severity.

---

## Security Constraints

1. **Runtime-state queries are admin-authenticated.** `POST /api/packages/validate --against-running-kernel` requires a valid admin session with `packages.install` or `packages.validate` rule (new rule name TBD). A PAT-holder without the rule gets 403. Rationale: the query surfaces information about which packages are installed and at which mount points; not every authenticated user should see that.
2. **CF3 / CF5 path checks are closed-domain.** Resolution never follows symlinks (the 0.4.0 R2 check rejected them outright for the whole package; if R2 missed one, CF3's `std::filesystem::exists` would follow). CF3 uses `is_regular_file` after a `realpath`-bounded check — the resolved path must remain under the package root.
3. **No capability-handler invocation during validation.** 0.4.2 never loads a package's `server/handlers/*.js` into the QuickJS runtime. Static validation only. (The 0.3.5 pre-eval scan from the GlassWorm defense would still apply to *every* `JS_Eval`, but 0.4.2 doesn't invoke `JS_Eval` itself.)
4. **`stdout` / `stderr` discipline, unchanged from 0.4.0.** Findings go to stdout; CLI-layer errors (unknown flag, flag conflict, unreachable kernel at the HTTP layer) go to stderr.

---

## Out-of-Scope — Architecture-Preserving Constraints

Per DESIGN §2 "deferred but committed" / "deferred and uncommitted" rules, 0.4.2 must not foreclose any of:

- **Hot-reload (0.10.4).** Cross-file validation is a pure static pass; no listener, no state. When 0.10.4 adds hot-reload, it may re-invoke `validate()` on the new on-disk state — no new surface needed.
- **Share primitive (0.11).** CFW2 warns on non-empty `shareable[]` without semantic validation. When 0.11 lands, CFW2 becomes structural validation; the warning→error promotion is a line-level ICD-0.4.2 amendment, not a new rule.
- **Sidecar packaging (0.8.x).** Sidecars deferred at the package-format layer; if they share the manifest format, CF rules apply unchanged. If they define separate manifests, 0.4.2 doesn't validate them, and that's fine. No foreclosure.
- **Provider extensions (CF7 deferred).** The CF7 rule currently rejects `namespace != name` unconditionally. A future milestone (not yet in the 0.4 ROADMAP; earliest candidate is 0.4.7 RBAC tests which have the most to gain from provider extensions) adds an explicit `provider_extension: true` field in `manifest.json` to opt in. 0.4.2's CF7 becomes conditional at that point; the opt-in flag is designed-in at this ICD but not consumed.
- **GlassWorm Unicode defense (0.4.1 per ROADMAP).** ICD-0.4.2 does not scan source bytes for invisible Unicode; that's the new 0.4.1 milestone's job and hooks at `validator.cpp:474` before cross-file runs. 0.4.2 composes upstream of cross-file by virtue of the validator's pass ordering. No coupling at the ICD surface.

---

## Milestone Criteria

All ~15 static fixtures pass their Catch2 assertions under default and sanitizer builds. `--structure-only` on the `cf4-handler-missing/` fixture returns 2; default mode returns 1 (proves the severity flip). `--json` output includes the `phase` field for every message. `run-clang-tidy-20` clean on the new translation units. `ctest` 100% pass count matches the existing baseline + the new fixture tests. Runtime-state fixtures are green under `PLINTH_KERNEL_TESTS=ON` against a live test kernel; green otherwise (CMake gate skips them with the standard "PG-backed tests skipped without DB" message).

### CI Wiring

- `src/kernel/packages/cross_file_validator.hpp` — **new**. Public library surface.
- `src/kernel/packages/cross_file_validator.cpp` — **new**. Implementation.
- `src/kernel/packages/panels_manifest.{hpp,cpp}` — **new**. Typed parser.
- `src/kernel/packages/config_manifest.{hpp,cpp}` — **new**. Typed parser.
- `src/kernel/packages/validator.cpp` — extended: the existing `validate()` entry calls `run_cross_file_validation` after `run_json_parse`; `ValidationConfig` gains `cross_file`, `against_running_kernel`, `kernel_url`.
- `src/kernel/main.cpp` — `validate_cmd` argparse gains `--structure-only` and `--against-running-kernel` flags. Mutual-exclusion group. Threads the flags into `ValidationConfig` before calling `validate()`.
- `src/kernel/js/runtime_config.{hpp,cpp}` — **new** if needed (thin getter for `max_memory_mb` build-time constant).
- `tests/fixtures/packages/` — 15 new static fixture directories + 3 new runtime-state fixture directories (gated).
- `tests/kernel/packages/cross_file_validator_test.cpp` — **new** Catch2 driver.
- `tests/kernel/packages/panels_manifest_test.cpp` — **new**. Parser tests.
- `tests/kernel/packages/config_manifest_test.cpp` — **new**. Parser tests.
- `CMakeLists.txt` — add new sources to the kernel library; add new test files to `plinth_tests`. The 0.3.3.2 `KERNEL_SOURCES` glob already covers `src/kernel/packages/**` and `tests/kernel/**`.
- No new CI job. Runtime-state fixtures gated behind `PLINTH_KERNEL_TESTS` (existing pattern).

---

## Entry / Exit

**Entry:** ICD-0.4.0 + ICD-0.4.1 merged (the base validator, parser, and library surface). The new 0.4.1 GlassWorm milestone (if it merges first per cadence) does not block 0.4.2 — the two layers compose, neither hard-depends on the other. DEFERRED.md unchanged since 0.4.0.

**Exit:** All static fixtures green under Catch2 default and sanitizer builds; `run-clang-tidy-20` clean; `plinth validate` CLI manually verified with and without `--structure-only` against the 15 fixtures; `plinth validate --against-running-kernel` manually verified against a local kernel; CHANGELOG entry describes the cross-file rule set + new flags; ROADMAP `0.4.2` line removed; DEFERRED.md amended if the `provider_extension` field or `kernel_url` client config decisions surface new deferrals.

---

## Open Questions (resolve during implementation)

1. **Client-side kernel URL discovery.** `--against-running-kernel` needs a URL. Options: (a) new `~/.config/plinth/client.yml` with `kernel_url:` key; (b) env var `PLINTH_KERNEL_URL`; (c) explicit `--kernel <url>` flag only; (d) discover from a `.plinth-client` file in package root or ancestors (git-style). Proposed: (c) explicit flag + (b) env fallback, defer (a) until a client-side tool needs broader config. Document in PR.
2. **Provider extension opt-in flag.** CF7's conditional-relaxation hook. Proposed: add a nullable `bool provider_extension` to the 0.4.1 `PackageManifest` typed shape at this ICD (the field is optional and defaults to false; 0.4.2 reads it but CF7's conditional relaxation remains deferred). This way no manifest-format change is needed when the feature unlocks. Alternative: wait until the actual milestone lands and amend 0.4.1's parser then. Architect decision at implementation time.
3. **`--json` `phase` field enum.** Three values `"structure"`, `"cross-file"`, `"runtime-state"` — or just freeform strings? Proposed: strict enum with a documented list, since the CI-scripting audience wants to filter deterministically. Document the enum in the PR.
4. **CF4 / R4 double-report.** R4 (warning) and CF4 (error) both fire on the same missing-handler condition. The user sees two messages in the default cross-file run. Proposed: in default mode, suppress R4 and emit only CF4 (the error is strictly more informative than the warning); in `--structure-only`, emit only R4. Alternative: emit both and rely on severity ordering. Architect preference.
5. **`entry_point` import-scan false positives.** String-literal `"./client/..."` appearing in a comment or non-import context triggers CFW3. Proposed: accept the false positives at warning severity (the alternative is a full JS AST parse, which is out of scope). Document in the PR so extension authors know to expect occasional spurious warnings.
6. **RT1 upgrade-vs-reject ambiguity.** RT1 cannot distinguish an install (collision = error) from an upgrade (collision = expected, continue). 0.4.2 always flags the collision; 0.4.4's install lifecycle decides whether to translate that flag into an error (new install) or a green-light (upgrade detected via version comparison). Document the split in both ICDs.

---

## Appendix: Example Cross-File Validation Output

```console
$ plinth validate ./my-notes-ext/
error: rbac-orphan-namespace: rules[0].namespace 'other' does not match any capability namespace in capabilities.json
  hint: rename the rule's namespace to 'notes' or declare the 'other' capability in capabilities.json
error: cf4-handler-missing: capability 'notes.edit' has no server/handlers/edit.js
  hint: create server/handlers/edit.js, or remove notes.edit from capabilities.json
warning: shareable-non-empty: manifest.shareable is reserved for the deferred share primitive (see ROADMAP 0.11); value will be ignored at install time
validated 14 files, 2 errors, 1 warnings (structure: 0, cross-file: 3, runtime-state: 0)
$ echo $?
1
```

```console
$ plinth validate ./my-notes-ext/ --structure-only --json
{"path":"/tmp/plinth/my-notes-ext","exit_code":2,"messages":[{"severity":"warning","rule":"handler-missing","phase":"structure","path":"capabilities.json","message":"capability 'notes.edit' declared but server/handlers/edit.js not found"}],"files_scanned":14,"total_bytes":12873}
$ echo $?
2
```

```console
$ plinth validate ./my-notes-ext/ --against-running-kernel --kernel http://localhost:8080
error: rt-missing-requires: capability 'kernel:1:storage.put(string, bytes) -> result' not found in registry
  hint: this capability is deferred (kernel 0.10); the package will install but calls will return capability_not_found until the kernel capability ships
validated 14 files, 1 errors, 0 warnings (structure: 0, cross-file: 0, runtime-state: 1)
$ echo $?
1
```
