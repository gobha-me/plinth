# ICD-0.4.1-manifest-parsing

**Traces to:** DESIGN-packages-v04x.md §0.4.1 (Manifest Parsing), §4.1 (permanent `manifest.json` schema), §5 (Interface Contracts Between Versions — manifest-schema stability guarantees), §7.1 (Shell-SDK-versioning forward compatibility — unknown top-level fields must parse gracefully), Appendix A (shareable[] reserved slot); architecture/05-extensions.md §1.2 (manifest.json shape), §1.3 (capabilities.json shape); architecture/02-capabilities.md §1.2 (capability registration shape — producer contract); architecture/06-frontend.md §2 (frontend.mount semantics consumed at 0.4.2)
**Depends on:** 0.2.1's `plinth::capabilities::parse_signature()` in `src/kernel/capabilities/parser.hpp` (capability-string parser, reused by 0.4.1 for signature field validation in `requires[]`); `src/kernel/capabilities/validation.hpp` — the pure per-field validators 0.2.0's registration API uses (`validate_namespace`, `validate_function`, `validate_version`, etc.); 0.4.1 reuses these rather than re-implementing; ICD-0.1.4-groups-rbac §Rule Registration (rule-name convention the `rbac.json` parser reuses in 0.4.6); `nlohmann::json` (already linked by kernel)
**Milestone:** 0.4.1 — Typed C++ parsers for `manifest.json` and `capabilities.json`. Shared by `plinth validate` (0.4.0) and the runtime installer (0.4.4).
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** ICD-0.4.0-package-structure-validation (consumes the parsers for rule R3); DESIGN-packages-v04x.md §4.1 (the permanent `plinth.packages.manifest_json` column captures what the parser reads); DISCUSSION docs — none directly on the schema (DESIGN-sharing-v011x.md outline references the `shareable[]` slot reserved here); DEFERRED.md (no active deferrals block this milestone; the `shareable[]` reserved-empty contract from Appendix A is a permanent reservation, not a deferral)

---

## Overview

0.4.1 delivers the two typed-struct parsers for the required manifest
files: `manifest.json` (identity + optional frontend + optional
runtime overrides + reserved share slot) and `capabilities.json`
(what the package provides + what it requires).

The schemas themselves are the **permanent contract**. Every
extension built against the 0.4 schemas must continue to install on
any 1.x+ kernel without migration (DESIGN §1, §5). This ICD fixes
the exact field names, types, and validation rules; subsequent arcs
may add optional fields but may not rename or retype existing ones
without a full manifest-version migration (which DESIGN §7.2
explicitly defers indefinitely — there's no `manifest_version` field,
unversioned manifests are v1 forever).

The parsers return typed structs on success and structured
`ManifestParseError` on failure. They do **not** perform cross-file
validation (that's 0.4.2), do not verify capability requirements
against a running registry (0.4.2 `--against-running-kernel`), and
do not execute anything.

**Scope:**

- `src/kernel/packages/manifest.{hpp,cpp}` (**new**) — `PackageManifest`
  struct + `PackageManifest::parse()` entry point.
- `src/kernel/packages/capabilities_manifest.{hpp,cpp}` (**new**) —
  `CapabilityManifest` struct + `CapabilityManifest::parse()`.
- Shared `ManifestParseError` type in a companion header
  `src/kernel/packages/manifest_error.hpp` (**new**) — used by both
  parsers and the 0.4.6 `rbac.json` parser that lands later in the
  arc. Structured shape: `{file, line, column, field_path, rule,
  message}`. All five localization fields are `std::optional<…>`
  except `message` + `file`.
- SemVer 2.0.0 validation for `manifest.version` (the parser
  rejects non-conformant strings with a structured error rather than
  accepting and surfacing later).
- SPDX-license handling: whitelist validation — any non-whitelisted
  value produces a **warning** (not error), matching DESIGN §0.4.1's
  "install-time warning" language. 0.4.1's parser collects warnings
  alongside errors; 0.4.0 surfaces them.
- `shareable[]` reserved-empty enforcement per Appendix A (parse-
  must-be-array, empty-else-warning).
- Unknown-top-level-field handling: **accept silently**, per DESIGN
  §7.1. A shell-SDK-versioning field in a future arc lands as a new
  top-level key; the 0.4.1 parser must not reject it. Silent accept
  (not warn) because an extension built against a future kernel will
  carry future fields that a 0.4.1 parser has no business warning
  about.
- Round-trip test: `parse → serialize → parse` yields an identical
  struct. Guards against field-name typos and default-value drift.

**Out of scope (deferred):**

- **Cross-file semantic validation** — namespace-matches-name,
  panel-references-file, frontend-mount-against-reserved-prefixes,
  etc. All 0.4.2. This ICD's §Validation Rules enumerates only
  *within-file* rules.
- **`rbac.json` / `panels.json` / `config.json` parsers** — 0.4.6
  (`rbac.json` with test contracts) / 0.4.2 (other two as part of
  cross-file). 0.4.1 ships only the two required-file parsers.
- **Runtime-state validation** — duplicate-name checks, installed-
  version collisions, capability-exists-in-registry checks. 0.4.2 and
  later.
- **`manifest.runtime` clamping** — reading the overrides against
  kernel maxima is the installer's job (0.4.4). The parser
  accepts the field structurally and stores the values verbatim.
- **`frontend.mount` reserved-prefix collision** — 0.4.2 cross-file /
  runtime-state. The parser validates the leading-slash and
  no-overlap-with-`/ext` rules (those are self-contained; `/ext` is
  the one reserved prefix knowable without cross-file context); all
  other reserved-prefix collisions (`/api`, `/s`, `/healthz`,
  `/metrics`, `/ws`, `/`) are 0.4.2.
- **SPDX license list maintenance.** 0.4.1 ships the whitelist as a
  `constexpr std::array<std::string_view, N>` matching DESIGN §0.4.1
  (MIT, Apache-2.0, GPL-3.0, AGPL-3.0, BSD-3-Clause, ISC, Unlicense).
  Adding entries is a one-line edit, not a milestone.
- **Capability-signature OID-driven type validation** — the parser
  reuses 0.2.1's `parse_signature` which already validates namespace
  / version / function / param types. Runtime-state OID mapping is
  tracked in DEFERRED.md.

---

## PackageManifest — Shape and Rules

The normative schema is DESIGN-packages-v04x.md §0.4.1. This ICD
pins the C++ surface.

```cpp
// src/kernel/packages/manifest.hpp
namespace plinth::packages {

struct RuntimeOverrides {
    std::optional<std::uint64_t> memory_limit_mb;
    std::optional<std::uint64_t> cpu_time_limit_ms;
    std::optional<std::uint64_t> max_stack_depth;
};

struct FrontendMount {
    std::string mount;   // leading-slash; non-empty; not "/ext" or sub
    std::string entry;   // relative path inside client/; non-empty
};

struct PackageManifest {
    std::string name;                                // required
    std::string version;                             // required; SemVer 2.0.0
    std::string description;                         // required
    std::string author;                              // required
    std::string license;                             // required; SPDX or warn
    std::string entry_point;                         // required; path inside package
    std::optional<FrontendMount> frontend;           // optional
    std::optional<RuntimeOverrides> runtime;         // optional
    std::vector<Json::Value> shareable;              // reserved; must be empty in 0.4

    // Unknown top-level fields preserved verbatim for forward
    // compatibility. Not exposed via a method; round-trip serializer
    // emits them before writing the canonical fields so the output is
    // stable under parse/serialize cycles.
    Json::Value unknown_fields = Json::objectValue;

    static auto parse(std::string_view json_text,
                      std::string_view source_path)
        -> std::expected<PackageManifest, ManifestParseError>;

    auto serialize() const -> std::string;
};

}  // namespace plinth::packages
```

**Field rules:**

| Field | Required | Type | Validator | Error / Warning |
|-------|----------|------|-----------|-----------------|
| `name` | Yes | string | matches `^[a-z][a-z0-9-]{1,63}$` | error `manifest.name.invalid` |
| `version` | Yes | string | SemVer 2.0.0 (major.minor.patch, optional prerelease, optional build metadata) | error `manifest.version.invalid_semver` |
| `description` | Yes | string | non-empty, ≤ 1024 chars | error `manifest.description.{missing,too_long}` |
| `author` | Yes | string | non-empty, ≤ 256 chars | error `manifest.author.{missing,too_long}` |
| `license` | Yes | string | **whitelist** (warn if not on list) | warning `manifest.license.unknown_spdx` |
| `entry_point` | Yes | string | non-empty, relative path, no `..`, no leading `/` | error `manifest.entry_point.{missing,invalid_path}` |
| `frontend` | No | object | if present, both `mount` and `entry` required | error `manifest.frontend.{mount,entry}.missing` |
| `frontend.mount` | If `frontend` | string | starts with `/`, non-empty, not exactly `/ext` or any sub-path of `/ext/` | error `manifest.frontend.mount.{leading_slash,reserved_ext_prefix}` |
| `frontend.entry` | If `frontend` | string | non-empty, relative path, no `..`, no leading `/` | error `manifest.frontend.entry.invalid_path` |
| `runtime` | No | object | if present, each child field must be a non-negative integer | error `manifest.runtime.<field>.invalid` |
| `runtime.memory_limit_mb` | No | uint64 | `> 0` | error `manifest.runtime.memory_limit_mb.zero` |
| `runtime.cpu_time_limit_ms` | No | uint64 | `> 0` | error `manifest.runtime.cpu_time_limit_ms.zero` |
| `runtime.max_stack_depth` | No | uint64 | `> 0` | error `manifest.runtime.max_stack_depth.zero` |
| `shareable` | No | array | must be an array if present; must be empty in 0.4.x | warning `manifest.shareable.non_empty_reserved` (non-empty → warning + ignored) |

**SemVer implementation.** A hand-rolled validator over
`std::string_view` is sufficient — no regex, no pulling in a SemVer
library. The grammar (SemVer 2.0.0 §§2, 9, 10) is:
`[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z\-]+(\.[0-9A-Za-z\-]+)*)?(\+[0-9A-Za-z\-]+(\.[0-9A-Za-z\-]+)*)?`.
Leading zeros on core numerics are rejected per spec. Reference
implementation (if the hand-roll gets unwieldy): the `semver` header-
only library at `/usr/include/semver` — can be vendored in-tree later
if the kernel grows multiple SemVer consumers.

**Reserved-ext-prefix check.** `frontend.mount` must not equal `/ext`
and must not start with `/ext/`. The full reserved-prefix set
(`/api`, `/s`, `/healthz`, `/metrics`, `/ws`, `/`) is 0.4.2's
concern because it requires reading `architecture/05-extensions.md §2`
at runtime and producing structurally identical rules. The exact-
string check on `/ext` here is self-contained — `/ext` is permanent,
well-known, and the common misuse case for extension authors.

**Unknown top-level fields.** Collect into `unknown_fields`
(`Json::Value` of object-shape). The `serialize()` method emits them
first in the output (before the known fields) so round-trip stability
is maintained. No warning, no error — DESIGN §7.1 is explicit.

**Round-trip guarantee.** For every valid manifest `M`,
`PackageManifest::parse(PackageManifest::parse(M).serialize()).value()`
must equal the first `parse(M).value()` by field equality. Tested in
§Milestone Criteria.

---

## CapabilityManifest — Shape and Rules

The capabilities schema itself is defined in
architecture/02-capabilities.md §1.2. This ICD pins the parser's
C++ surface.

```cpp
// src/kernel/packages/capabilities_manifest.hpp
namespace plinth::packages {

struct CapabilityParam {
    std::string name;
    std::string type;        // "string" | "number" | "boolean" | "object" | "array" | "buffer"
};

struct ProvidedCapability {
    std::string namespace_;  // trailing underscore per kernel convention; avoid keyword collision
    int version;
    std::string function;
    std::vector<CapabilityParam> params;
    std::string returns;
    std::string scope;       // "instance" | "user"
    std::string description;
    std::optional<std::string> rbac_rule;  // optional override; validated against ICD-0.1.4 regex
};

struct CapabilityManifest {
    std::vector<ProvidedCapability> provides;
    std::vector<std::string> requires_;    // trailing underscore — keyword
    Json::Value unknown_fields = Json::objectValue;

    static auto parse(std::string_view json_text,
                      std::string_view source_path)
        -> std::expected<CapabilityManifest, ManifestParseError>;

    auto serialize() const -> std::string;
};

}  // namespace plinth::packages
```

**Field rules:**

| Field | Required | Type | Validator |
|---|---|---|---|
| `provides` | No | array of objects | Empty array OK — a package with no capabilities (UI-only shell extension) is valid. |
| `provides[].namespace` | Yes (per entry) | string | delegated to `plinth::capabilities::validate_namespace()` (reused from 0.2.0 — `src/kernel/capabilities/validation.hpp`; regex `[a-z][a-z0-9_]{0,63}`, note underscore not dash — capability namespace ≠ package `name`) |
| `provides[].version` | Yes | integer | delegated to `plinth::capabilities::validate_version()` (>= 1) |
| `provides[].function` | Yes | string | delegated to `plinth::capabilities::validate_function()` (regex `[a-z][a-z0-9_.]{0,127}` plus dot-position rules) |
| `provides[].params` | No | array | defaults to empty array; each entry must be `{name, type}` |
| `provides[].params[].name` | Yes | string | non-empty, `^[a-z_][a-zA-Z0-9_]*$` |
| `provides[].params[].type` | Yes | string | one of the documented type literals |
| `provides[].returns` | Yes | string | non-empty; canonical names documented in architecture/02-capabilities.md §1.2 |
| `provides[].scope` | Yes | string | must be `"instance"` or `"user"` |
| `provides[].description` | Yes | string | non-empty, ≤ 1024 chars |
| `provides[].rbac_rule` | No | string | if present, matches rule-name regex `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$` (normative source: DESIGN-packages-v04x.md §0.4.6 Phase-A rule conventions; ICD-0.1.4 carries the `<namespace>.<action>.<resource>` prose form of the same rule) |
| `requires` | No | array of strings | Each string is a capability signature parseable by 0.2.1's `parse_signature`; 0.4.1 forwards every entry through the parser and surfaces any `INVALID_CAPABILITY` as `capabilities.requires[N].invalid_signature`. |

**Unknown top-level fields.** Same rule as `PackageManifest` —
collected into `unknown_fields`, round-tripped through serialization.

**`requires_` trailing underscore.** `requires` is not a C++ keyword,
but C++20 added `requires` as an unreserved identifier-with-special-
meaning; using it as a field name triggers `readability-identifier-
naming` only in specific contexts, but the trailing-underscore form
is the kernel convention (see `ConnectionRegistry` precedent per
project_plinth_state and RE-EVAL-0.3.x.md §2.5). Same rationale for
`namespace_`: `namespace` is a keyword.

---

## ManifestParseError

```cpp
// src/kernel/packages/manifest_error.hpp
namespace plinth::packages {

struct ManifestParseError {
    std::string file;                    // source path from parse() arg
    std::optional<std::size_t> line;     // 1-indexed, from nlohmann::json where available
    std::optional<std::size_t> column;   // 1-indexed, from nlohmann::json where available
    std::optional<std::string> field_path;  // JSON-pointer, e.g. "/provides/0/namespace"
    std::string rule;                    // stable, e.g. "manifest.name.invalid"
    std::string message;                 // human-readable
    std::optional<std::string> remediation;
    enum class Severity : std::uint8_t { Error, Warning } severity = Severity::Error;
};

}  // namespace plinth::packages
```

**Line/column source.** `nlohmann::json::parse_error` carries `byte`
(byte offset). The parser translates to line/column by maintaining
a newline-index on the input buffer. For non-structural errors
(e.g., semantic rule failures), `line`/`column` may be `std::nullopt`
— the line is the line of the *field* that failed, and extracting
that from `nlohmann::json` after it has stripped whitespace and
reshaped the tree requires source-location threading that isn't free.
Best-effort: populate line/column when readily available (JSON parse
errors), leave empty otherwise. The `field_path` JSON-pointer
always populates for post-structural errors.

**`rule` names are stable, namespaced strings.** CI scripts grep on
these; breaking them is a contract break. The scheme is
`<file-stem>.<field-path>.<failure-mode>` — e.g.,
`manifest.name.invalid`, `capabilities.requires[0].invalid_signature`.

**`remediation`** is populated when the parser has a concrete hint:
"remove the trailing comma", "license must be one of MIT,
Apache-2.0, ...", "add a description field". Populating is
best-effort; the validation itself does not depend on it.

---

## Capability-Signature Reuse

`requires[]` entries are parsed via the existing
`plinth::capabilities::parse_signature()` from 0.2.1. If it returns
`INVALID_CAPABILITY`, the 0.4.1 parser emits a `ManifestParseError`
with `rule = "capabilities.requires[N].invalid_signature"` and
`message` carrying the raw signature + the error subcode.

Per-field validators for the `provides[].namespace` / `.function`
fields also delegate to the same kernel registration-side validators
the 0.2.0 API exposes. The intent is that a manifest-side
registration and a kernel-side registration of the same capability
produce the same errors — there is one source of truth.

**New header split?** If the existing `validation.hpp` in
`src/kernel/capabilities/` is not public enough to link from
`src/kernel/packages/`, split it into a public header. Internal
detail; flagged for the implementing session.

---

## Security Constraints

1. **JSON parsing under arbitrary input.** `nlohmann::json::parse` is
   the one interface consuming user-authored bytes. It must be
   called with `allow_exceptions = false` / the non-throwing overload
   so adversarial JSON (trailing-comma bombs, deep nesting) produces
   a structured error rather than throwing out of the parser. A
   `recursion_limit` of 128 frames is the kernel's default; DESIGN
   §0.4.0 size limits (50 MB) bound the input, so a 128-deep nest
   cannot be reached in practice on valid manifests.
2. **No code execution.** The parser never evaluates any manifest
   value. `entry_point` is stored as a string, not opened. `frontend.
   entry` is stored as a string, not stat()ed (that's 0.4.2's job).
3. **No filesystem side effects.** The parser takes a
   `std::string_view` of already-loaded bytes. If the caller wants
   file-reading semantics, they read the file themselves and pass
   the contents in. `source_path` is for error-reporting only.
4. **No environment-variable expansion.** `${…}` strings in
   manifest fields are literal strings — no interpolation. DESIGN
   is silent on this; explicitly declaring it here forecloses a
   future expansion feature being added "implicitly" (if anyone
   later wants interpolation, it's a design decision, not a silent
   parser change).
5. **Deterministic serialization.** `PackageManifest::serialize()`
   always emits fields in a fixed order (unknown fields first per
   the round-trip rule, then the known fields in declaration
   order). Makes PR diffs reviewable and keeps
   `manifest_checksum` (DESIGN §4.1 `plinth.packages.manifest_
   checksum`) stable across reparses.

---

## Milestone Criteria

All validation rules above have at least one Catch2 case asserting
the correct error emission shape. All `PackageManifest` fields have
at least one round-trip test case (parse → serialize → parse → field
equality). `CapabilityManifest::requires_` parses every example
signature from architecture/02-capabilities.md §1.1.
`run-clang-tidy-20` is clean on the new translation units. `ctest`
100% pass on the added tests.

### CI Wiring

- `src/kernel/packages/manifest.hpp` — **new**.
- `src/kernel/packages/manifest.cpp` — **new**.
- `src/kernel/packages/capabilities_manifest.hpp` — **new**.
- `src/kernel/packages/capabilities_manifest.cpp` — **new**.
- `src/kernel/packages/manifest_error.hpp` — **new**.
- `tests/kernel/packages/manifest_test.cpp` — **new**. One
  `TEST_CASE` per validation rule, plus one round-trip case over
  the `valid-full/` fixture from ICD-0.4.0.
- `tests/kernel/packages/capabilities_manifest_test.cpp` — **new**.
  One `TEST_CASE` per `requires[]` signature shape (valid, invalid-
  namespace, invalid-version, invalid-type literal, etc.) plus
  round-trip + empty-provides baseline.
- `CMakeLists.txt` — add the four new source files to the kernel
  library. The 0.3.3.2 `KERNEL_SOURCES` glob already covers
  `src/kernel/packages/**` and `tests/kernel/packages/**` for the
  `tidy` target. If `src/kernel/capabilities/validation.hpp` needs
  exposing (see §Capability-Signature Reuse), split + re-include
  it here.
- No new CI job. No new CMake option.

### Test count target

~18 Catch2 cases (12 in `manifest_test.cpp`, 6 in
`capabilities_manifest_test.cpp`). Adjust in the implementing
session; this count is guidance, not a target.

---

## Entry / Exit

**Entry:** 0.4.0's scope is resolved (the parsers have a concrete
consumer; the ICD-0.4.0 R3 rule reads structural errors from the
parsers by `rule` name, so the stable-rule-name convention lands
with 0.4.1). In practice 0.4.0 and 0.4.1 ship in one branch that
squashes to a single commit; the ICDs are paired for this reason
(see §Rationale in the plan file of the authoring session).

**Exit:** All ~18 Catch2 cases pass under default and sanitizer
builds; `run-clang-tidy-20` clean on the new files; CHANGELOG entry
for 0.4.1 describes the parser surface + stable error-name convention;
ROADMAP `0.4.1` line removed; DEFERRED.md unchanged.

---

## Open Questions (resolve during implementation)

1. **`validation.hpp` header-split.** The 0.2.0 validators
   (`is_valid_namespace`, `is_valid_function_name`, version integer
   rules) live in a private header. 0.4.1 needs them. Split into a
   public `include/plinth/capabilities/validation.hpp` or re-expose
   the existing one? The implementing session decides; the
   behaviour is non-negotiable — same validators on both sides.
2. **Stable-rule-name prefixes.** This ICD uses `manifest.` /
   `capabilities.` prefixes. The 0.4.6 `rbac.json` parser will use
   `rbac.`. The 0.4.2 cross-file validation pass will add
   `cross-file.`. All four prefixes should be disjoint and stable.
   Propose documenting this in a small table in 0.4.2's ICD when it
   is authored.
3. **`runtime.cpu_time_limit_ms` / `memory_limit_mb` upper bounds.**
   Should the parser also reject obviously-absurd values (e.g.,
   `memory_limit_mb = 1_000_000`)? Proposed: no — clamping against
   kernel maxima is 0.4.4's job; the parser accepts any positive
   integer. A manifest declaring 1 PB of memory parses cleanly and
   gets clamped at install time to the kernel maximum. Rationale:
   separation of concerns — the parser is syntactic.
4. **Round-trip canonical whitespace.** `serialize()` uses
   nlohmann's 2-space-indent dump by default. Propose documenting
   that the canonical form is 2-space-indent with sorted keys
   *within* the known-field block (order within `unknown_fields` is
   whatever `nlohmann::json` stored). Keeps manifest diffs review-
   friendly without forcing extension authors to format their
   source files a specific way.

---

## Appendix: Example `manifest.json` Parsing

Input:
```json
{
  "name": "notes",
  "version": "1.2.3",
  "description": "Markdown notes",
  "author": "jeff",
  "license": "MIT",
  "entry_point": "server/main.js",
  "future_field_for_shell_sdk": "ignored-silently",
  "frontend": {
    "mount": "/app",
    "entry": "index.html"
  },
  "runtime": {
    "memory_limit_mb": 128
  },
  "shareable": []
}
```

Parse result:
- `name = "notes"`
- `version = "1.2.3"`
- `frontend = {mount: "/app", entry: "index.html"}`
- `runtime = {memory_limit_mb: 128, cpu_time_limit_ms: nullopt, max_stack_depth: nullopt}`
- `shareable = []`
- `unknown_fields = {"future_field_for_shell_sdk": "ignored-silently"}`

Round-trip serialize output (trimmed for brevity):
```json
{
  "future_field_for_shell_sdk": "ignored-silently",
  "name": "notes",
  "version": "1.2.3",
  ...
  "shareable": []
}
```

Unknown fields appear first (round-trip stability). Re-parsing yields
the identical struct.

---

## Appendix: Error Example

Input:
```json
{
  "name": "NotLowercase",
  "version": "1.2",
  "entry_point": "/server/main.js"
}
```

Parse result (`std::unexpected<ManifestParseError>`): three errors
surface. The parser collects all rule failures in one pass (does not
stop at the first) and returns a single `ManifestParseError` **whose
`message` summarizes the count**, with full findings in the report
that 0.4.0 assembles. If 0.4.1's `parse()` caller wants per-field
detail, they set a mode flag (TBD in implementation — either
`parse_collect` returning `std::vector<ManifestParseError>` or a
single top-level error with a `children` field; both shapes are
acceptable, the implementing session picks).

Findings at ruleset-level:
- `rule: "manifest.name.invalid"` (capital `N`, underscore/digit
  prefix → fails `^[a-z][a-z0-9-]{1,63}$`).
- `rule: "manifest.version.invalid_semver"` (`1.2` is not SemVer —
  missing patch).
- `rule: "manifest.entry_point.invalid_path"` (leading `/` disallowed).
