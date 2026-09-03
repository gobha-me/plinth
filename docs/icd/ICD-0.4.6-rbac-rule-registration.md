# ICD-0.4.6-rbac-rule-registration

> **Amendment 2026-04-22 (0.5.0.1)** — `plinth::rbac::validate_rules`
> renamed to `plinth::rbac::validate_rules`; Catch2 tag `[rbac_phase_a]`
> renamed to `[rule_validator]`; the `ManifestParseError` value type is
> `ManifestParseError` (no separate type landed). Prose references to
> "rule validator" (concept) are rewritten to "rule validator". Body is
> rewritten in place; pre-rename names preserved in v0.4.7 release
> commit (`4a86e31`). See ICD-0.4.7 amendment and CHANGELOG v0.5.0.1.

**Traces to:** DESIGN-packages-v04x.md §0.4.6 (authoritative contract for
the `rbac.json` typed parser, rule validator validation rules, and the
`test_contract JSONB` storage column); DESIGN-packages-v04x.md §4.2
(`plinth.rbac_rules.test_contract` — new nullable JSONB column populated
by 0.4.6); architecture/01-identity.md §2.1 (RBAC rule registration
contract — rbac.json shape is the public schema extensions author
against), §2.2 (*RBAC Test Validation — Two Phase* — rule validator is
install-time structural, RBAC test is post-install execution; 0.4.6 is
rule validator only), §2.4 (Rule Lifecycle — 0.4.6 sits on the "Extension
installs → kernel reads `rbac.json`, performs rule validator validation,
registers rules" bullet).
**Depends on:** ICD-0.1.4-groups-rbac (rule storage contract and rule-
name regex `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$`; the
`plinth.rbac_rules` columns the typed parser populates); ICD-0.2.0
(capability-namespace regex + `validate_namespace` per-field
validator); ICD-0.2.1-capability-parser (`plinth::capabilities::parse_signature`
consumed by Rule A.3 for `test.*.call` validation); ICD-0.4.0-package-structure-validation
(`Severity` enum + stable-rule-name convention this ICD extends with
`rbac.*` prefix); ICD-0.4.1-manifest-parsing (`plinth::packages::ManifestParseError`
shape reused; `CapabilityManifest::parse` + `ParseResult` pattern
mirrored); ICD-0.4.2-cross-file-manifest-validation (CF1/CF2/CFW1
earlier-stage filters — 0.4.6 re-checks with PG context, does not
replace them); ICD-0.4.4-package-install-lifecycle (REGISTERING stage
hook point — `run_stage_registering` calls rule validator before
`upsert_extension_rule`; the minimal shim landed in 0.4.4 gets layered,
not rewritten); ICD-0.4.5-package-lifecycle-transitions (upgrade
reconciliation at `reconcile_rbac_on_upgrade` in
`src/kernel/packages/install_lifecycle.cpp:686-764` consumes the new
`test_contract` parameter; same-extension rule re-registration is the
Rule A.5 exemption).
**Milestone:** 0.4.6 — the final paper-parser + Phase-A surface of the
0.4 arc. Does NOT run the tests themselves (RBAC test — 0.4.7 scope), does
NOT change the `plinth.rbac_rules` storage contract exposed to
`ICD-0.1.4`'s CRUD surface (the new column is internal; groups/rules
HTTP endpoints stay byte-identical), does NOT touch cap-dispatch or
audit plumbing.
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:** [src/kernel/packages/install_lifecycle.cpp:640-678](../../src/kernel/packages/install_lifecycle.cpp)
(`parse_rbac_rules` minimal shim — replaced by typed parser),
[install_lifecycle.cpp:680-764](../../src/kernel/packages/install_lifecycle.cpp)
(`reconcile_rbac_on_upgrade` — upsert call at line 720-723 gains
`test_contract` argument), [install_lifecycle.cpp:879-896](../../src/kernel/packages/install_lifecycle.cpp)
(REGISTERING first-install branch — rule validator fires here, error surface
routes through the existing RollbackGuard),
[src/kernel/rbac/rule_registrar.hpp:20-22](../../src/kernel/rbac/rule_registrar.hpp)
(additive `test_contract` comment — the ICD the file anticipates is
this one), [src/kernel/rbac/rule_registrar.cpp:36-67](../../src/kernel/rbac/rule_registrar.cpp)
(INSERT…ON CONFLICT DO UPDATE — extended with one new column),
[src/kernel/packages/validator.cpp:380-401](../../src/kernel/packages/validator.cpp)
(`parse_rbac_raw` — stays as structural pre-check; typed parser is a
new entry point, not a replacement),
[src/kernel/packages/cross_file_validator.cpp:119-213](../../src/kernel/packages/cross_file_validator.cpp)
(CF1/CF2 offline filters — kept; rule validator re-checks with PG context),
[src/kernel/packages/cross_file_validator.cpp:38-40](../../src/kernel/packages/cross_file_validator.cpp)
(`RESERVED_NAMES = {kernel, plinth, system}` — reused verbatim for
Rule A.2), [src/kernel/packages/manifest_error.hpp](../../src/kernel/packages/manifest_error.hpp)
(`ManifestParseError` + `Severity` — the parse-error shape Rule A.*
structural errors reuse), [src/kernel/capabilities/parser.hpp](../../src/kernel/capabilities/parser.hpp)
(`parse_signature` reused by Rule A.3),
[migrations/schema.sql:76-84](../../migrations/schema.sql)
(`plinth.rbac_rules` — one `ALTER TABLE ADD COLUMN` before the table's
trailing blank line; pre-0.7 freeze, direct edit allowed).

---

## Overview

0.4.4 shipped a minimal shim — `parse_rbac_rules(package_root)` at
`install_lifecycle.cpp:647` reads `rbac.json`, filters to entries with
a non-empty `rule` + `namespace_`, and feeds them to
`upsert_extension_rule` inside the REGISTERING transaction. There is
no typed struct, no validation, no error surfacing beyond "rule insert
failed". `ICD-0.4.4` §29 calls this out explicitly: "minimal rule-
insert shim … rule validator validation lands in 0.4.6." ICD-0.4.5 §26 extends
the shim with three-way reconciliation but preserves the lack of
structural validation.

0.4.6 closes that gap. It lands:

1. **A typed `rbac.json` parser** (`plinth::rbac::parse_rbac_manifest`),
   mirroring the `plinth::packages::CapabilityManifest::parse` pattern
   from ICD-0.4.1 — returns a `std::optional<RbacManifest>` + collected
   `std::vector<ManifestParseError>` so the caller surfaces
   errors-and-warnings in one pass. Same stable-rule-name convention as
   ICD-0.4.0 / ICD-0.4.1, new prefix `rbac.*`.
2. **rule validator validation** (`plinth::rbac::validate_rules`) —
   five structural rules from DESIGN §0.4.6, three of which overlap
   with the existing CF1/CF2 filters (kept; see §Relationship to
   CF1/CF2/CFW1) and two of which are new: name-regex and
   cross-extension collision.
3. **`plinth.rbac_rules.test_contract JSONB`** column, nullable, holds
   the rule's `test` object verbatim for 0.4.7 RBAC test.
4. **Extended `upsert_extension_rule` signature** — one additional
   parameter, `std::optional<nlohmann::json> test_contract`, threaded
   through the INSERT…ON CONFLICT DO UPDATE. Additive; callers that
   omit the argument stay source-compatible, but 0.4.4-shipped callers
   at `install_lifecycle.cpp:889-894` and `install_lifecycle.cpp:720-723`
   must be updated to pass the new value through (trivial; same
   transaction, same ordering).
5. **rule validator firing point** — inside `run_stage_registering`
   (first-install path) and `run_stage_registering_upgrade` (upgrade
   path), between the existing state-UPDATE-to-REGISTERING and the
   existing `upsert_extension_rule` loop. Any finding with
   `Severity::ERROR` triggers the existing RollbackGuard.

What 0.4.6 does **not** do: execute the RBAC tests (that is 0.4.7
RBAC test), alter the `/api/groups/*` CRUD surface in ICD-0.1.4 (the new
column is internal — `SELECT …` from the groups endpoints stays
identical), touch capability dispatch, touch the audit pipeline.

Scope for this milestone is deliberately narrow — small parser, small
validator, one SQL column, ~12 test cases. The 0.4.4 lifecycle
infrastructure already carries rule validator's firing point and the upgrade
reconciliation; 0.4.6 fills in the checks and the storage.

---

## Implementation deviation (0.4.6 file rename)

The `rule_validator.{hpp,cpp}` file pair named throughout this ICD shipped
in v0.4.7 as **`src/kernel/rbac/rule_validator.{hpp,cpp}`** as part of
the v0.4.7 file-rename pass that also moved `rbac_test_runner.{hpp,cpp}` to
`rbac_test_runner.{hpp,cpp}` (see ICD-0.4.7's matching Implementation
deviation subsection). Rationale: pairs symmetrically with the
existing `rule_registrar.{hpp,cpp}` in the same directory and
describes what the TU does (validate RBAC rules) rather than which
DESIGN phase it implements. **Public symbols, types, namespaces,
methods, audit kinds, schema columns, and Catch2 tags are unchanged.**
The DESIGN concept name ("rule validator") stays locked into the public
surface — only the on-disk filename moved. References below to
`rule_validator.{hpp,cpp}` should be read as `rule_validator.{hpp,cpp}`.
Documented in `docs/CHANGELOG.md` v0.4.7 deviations and
`RE-EVAL-0.4.x-arc-closeout.md §2.1`.

---

## Scope

- **New:** `src/kernel/rbac/rbac_manifest.{hpp,cpp}` — `RbacManifest` +
  `RbacRule` structs + `parse_rbac_manifest(json_text, source_path) →
  RbacManifestParseResult`. Mirrors the `CapabilityManifest::parse`
  pattern in [src/kernel/packages/capabilities_manifest.hpp](../../src/kernel/packages/capabilities_manifest.hpp).
- **New:** `src/kernel/rbac/rule_validator.{hpp,cpp}` — `validate_rules` +
  `ManifestParseError` value type. Takes the parsed `RbacManifest`, the
  parsed `plinth::packages::CapabilityManifest`, the package name, and
  a `PGconn&` (for Rule A.5). Returns
  `std::vector<plinth::packages::ManifestParseError>` using the existing
  error shape so the install-lifecycle caller surfaces the failures
  through the same path as other validation errors.
- **Extended:** `src/kernel/rbac/rule_registrar.{hpp,cpp}` —
  `upsert_extension_rule` gains one parameter:
  `std::optional<nlohmann::json> test_contract`. The SQL moves from
  5-column INSERT / 3-column UPDATE set to 6-column / 4-column; the
  conflict target (`rule`) is unchanged.
- **Extended:** `src/kernel/packages/install_lifecycle.cpp` —
  `run_stage_registering` at [:879-896](../../src/kernel/packages/install_lifecycle.cpp)
  now calls `plinth::rbac::parse_rbac_manifest` + `validate_rules`
  *before* the `upsert_extension_rule` loop; the loop passes each
  rule's `test` object through as `test_contract`. Same treatment for
  `run_stage_registering_upgrade` + `reconcile_rbac_on_upgrade` at
  [:680-764](../../src/kernel/packages/install_lifecycle.cpp).
- **New schema edit** in `migrations/schema.sql` at the
  `plinth.rbac_rules` table definition (currently lines 76-84): one
  additional column `test_contract JSONB` after `orphaned_at`. Nullable.
  Pre-0.7 freeze — direct edit is the convention
  (`docs/SESSION-GUIDE.md`).
- **Test fixtures** under `tests/fixtures/rbac/` (new subtree; see
  §Test Cases for the full catalogue). Reuse of the existing
  `tests/fixtures/packages/rbac-*` fixtures where they already cover a
  case (e.g. `rbac-test-parse-fail` feeds P.M3, `rbac-test-bad-call`
  feeds P.M4).
- **Catch2 coverage** in new `tests/kernel/rbac/rbac_manifest_test.cpp`
  and `tests/kernel/rbac/rule_validator_test.cpp`. Round-trip case in the
  former; all P.* cases in the latter, PG-gated where Rule A.5 needs
  the database.
- **Stable-rule-name prefix `rbac.*`** added to the naming catalogue
  (ICD-0.4.1 §OQ2). Error codes: `rbac.rule.invalid_name`,
  `rbac.rule.namespace_mismatch`, `rbac.test.call.parse_error`,
  `rbac.test.call.unresolved`, `rbac.rule.name_collision`. Parse-level
  errors (missing required field, wrong type) follow the same prefix
  (e.g. `rbac.rules.not_array`).
- **CHANGELOG 0.4.6 entry** on implementation merge (not this ICD
  session).

---

## Out of scope (deferred)

- **RBAC test test execution.** DESIGN §0.4.7. The `test_contract JSONB`
  column is populated by 0.4.6 but never read in 0.4.6; the column
  exists so 0.4.7 has somewhere to load from. RBAC test's ephemeral test
  users, `__test_denied_{run_id}` / `__test_allowed_{run_id}`,
  `ACTIVE_FLAGGED` state transitions, the re-run-on-demand CLI — all
  0.4.7.
- **Rule-name collision across **disabled** extensions.** Rule A.5
  checks collision against *all* rows in `plinth.rbac_rules` regardless
  of the owning extension's state. A disabled extension's rule still
  counts as a collision — correct per DESIGN §0.4.6 (the rule string
  is the unique identity), but it means an admin disabling extension
  A to install extension B that reuses A's rule name will get
  P.M5-style errors until they uninstall A. This is the intended
  behavior; documented here so the implementing session does not
  "helpfully" auto-skip disabled-extension rows.
- **`capabilities.json` rbac_rule back-reference check.** ICD-0.4.1
  `CapabilityManifest.provides[].rbac_rule` is an optional override
  pointing to a rule *name*. A 0.4.6 implementation could assert that
  every `provides[].rbac_rule` string also appears as an `rbac.json`
  `rule` — but this is a cross-file check on the **capabilities**
  side of the relation, symmetric to CF1. Classified as CFW-style
  warning material, deferred to the next arc RE-EVAL if it surfaces
  as a real problem (fixtures already exist to smoke-test).
- **Richer `test` object shape.** DESIGN §0.4.6 shows exactly
  `{assert_deny, assert_allow}` each with `{call, expect}`. The 0.4.6
  typed parser validates that shape when `test` is present but stores
  the whole object verbatim in `test_contract` so 0.4.7 can extend
  (e.g. `{assert_deny: {call, expect, fixture_user_groups: [...]}}`)
  without a schema change. The 0.4.6 parser is permissive on unknown
  fields inside `test` per the same DESIGN §7.1 "silent accept unknown"
  rule the manifest parser applies.
- **Removal of CF1 / CF2 / CFW1.** Keep. See §Relationship. rule validator
  fires later in the pipeline (after PG is live) with authoritative
  semantics; CF1/CF2 fire earlier (no PG) and catch the same structural
  issues for offline `plinth validate` CLI use.
- **Upgrading error codes at CF1 / CF2 to use the new `rbac.*` prefix.**
  Leave them. CF1 emits `rbac-orphan-namespace`, CF2 emits
  `rbac-test-call-parse` / `rbac-test-call`. They are grep-stable; the
  0.4.6 rule validator codes are disjoint (`rbac.rule.namespace_mismatch` etc.)
  so deduping at the reporter level still works by-code.

---

## rbac.json — shape (permanent contract)

Same as DESIGN-packages-v04x.md §0.4.6. Repeated here so the ICD is
self-contained for implementors.

```json
{
  "rules": [
    {
      "rule": "notes.edit",
      "namespace": "notes",
      "description": "Edit notes",
      "test": {
        "assert_deny": {
          "call": "notes:1:edit('hello')",
          "expect": "permission_denied"
        },
        "assert_allow": {
          "call": "notes:1:edit('hello')",
          "expect": "success"
        }
      }
    }
  ]
}
```

**Field rules (within-file, enforced by the parser; see §rule validator
Validation Rules for the *across-file* rules):**

| Field | Required | Type | Validator | Error code |
|-------|----------|------|-----------|------------|
| (root) | — | object | must be an object with a `"rules"` key | `rbac.root.not_object` |
| `rules` | Yes | array | must be array; may be empty | `rbac.rules.not_array` |
| `rules[].rule` | Yes | string | non-empty | `rbac.rule.missing` / `rbac.rule.not_string` |
| `rules[].namespace` | Yes | string | non-empty | `rbac.namespace.missing` |
| `rules[].description` | Yes | string | non-empty, ≤ 1024 chars | `rbac.description.{missing,too_long}` |
| `rules[].test` | No | object | if present, must be object | `rbac.test.not_object` |
| `rules[].test.assert_deny` | No | object | if present, must be `{call, expect}` | `rbac.test.assert_deny.shape` |
| `rules[].test.assert_allow` | No | object | if present, must be `{call, expect}` | `rbac.test.assert_allow.shape` |
| `rules[].test.assert_*.call` | Yes-in-assert | string | non-empty | `rbac.test.call.missing` |
| `rules[].test.assert_*.expect` | Yes-in-assert | string | literal `"permission_denied"` (in `assert_deny`) or `"success"` (in `assert_allow`) | `rbac.test.expect.invalid` |

**Unknown fields** at any level are **accepted silently**, matching the
DESIGN §7.1 forward-compatibility contract ICD-0.4.1 already applies
to manifest.json.

**Missing `test` object** is valid — stored as `test_contract = NULL`.
RBAC test simply has nothing to run for that rule.

**`rules: []`** is valid. A package may register zero rules; common for
UI-only extensions.

---

## RbacManifest — C++ surface

```cpp
// src/kernel/rbac/rbac_manifest.hpp
#pragma once

#include "kernel/packages/manifest_error.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::rbac {

struct RbacRule {
    std::string rule;
    // NOLINTNEXTLINE(readability-identifier-naming) — `namespace` is a keyword.
    std::string namespace_;
    std::string description;
    std::optional<nlohmann::json> test;  // whole `test` object verbatim, or nullopt
};

struct RbacManifest {
    std::vector<RbacRule> rules;
    nlohmann::json unknown_fields = nlohmann::json::object();

    auto serialize() const -> std::string;
};

struct RbacManifestParseResult {
    std::optional<RbacManifest>                value;
    std::vector<packages::ManifestParseError>  messages;
};

// Pure: no PG, no filesystem. Takes already-loaded bytes and a source
// path (for error reporting only; not opened). Collects all structural
// errors and warnings in one pass — returns value = nullopt only if the
// JSON itself fails to parse or the root is not an object.
auto parse_rbac_manifest(std::string_view json_text,
                         std::string_view source_path)
    -> RbacManifestParseResult;

}  // namespace plinth::rbac
```

**Semantics:**

- The parser succeeds (returns `value = RbacManifest{...}`) as long as
  the JSON is well-formed and the root is an object with an array
  `rules` key. Per-rule structural errors accumulate in `messages`; a
  rule with a structural error is **not** included in `value->rules`
  (prevents downstream consumers from operating on partially-valid
  data). This matches the ICD-0.4.0 §R3 "structural error → rule is
  dropped from the parsed view" convention used by `CapabilityManifest`.
- Unknown top-level fields go into `unknown_fields` for round-trip
  stability (same as `PackageManifest`).
- `RbacRule.test` is stored as a whole `nlohmann::json` node — the
  parser only validates the shape when `test` is present; the
  `test_contract JSONB` column stores the exact bytes the manifest
  contained (modulo `nlohmann::json`'s canonicalization), so 0.4.7 has
  full access to any unknown fields inside `test`.
- `serialize()` is symmetric to `PackageManifest::serialize()` —
  2-space indent, unknown fields first, known fields in declaration
  order. Reused by the round-trip Catch2 case (P.R1).

**Why a separate namespace from `plinth::packages`.** RBAC is a kernel
concern with its own directory (`src/kernel/rbac/`). The existing
`rule_registrar.{hpp,cpp}` already lives under `plinth::rbac`; the
typed parser sits alongside it, not under `plinth::packages`. The
`ManifestParseError` type is borrowed from `plinth::packages`
(via `#include "kernel/packages/manifest_error.hpp"`) so the error
shape stays unified; the *error code strings* carry the `rbac.*`
prefix, disjoint from the `manifest.*` / `capabilities.*` /
`cross-file.*` prefixes ICD-0.4.0 and ICD-0.4.1 established.

---

## rule validator Validation Rules

Each rule runs against an already-parsed `RbacManifest` (so the
within-file shape errors have been collected). The five rules are
ICD-normative; DESIGN §0.4.6 is the traceability source. Each
produces a `ManifestParseError` with `Severity::ERROR`.

### Rule A.1 — Rule name follows convention

Every `rules[].rule` matches `^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$`
— lowercase segments separated by dots, 2–5 segments total, first
character of each segment is a letter, other characters are
lowercase-alphanumerics. Matches the regex published in
`architecture/01-identity.md §2.1` and reused by
`CapabilityManifest.provides[].rbac_rule` per ICD-0.4.1 §CapabilityManifest
§"`provides[].rbac_rule` if present".

- **Error code:** `rbac.rule.invalid_name`
- **Message template:** `rbac.json rules[{i}].rule '{value}' does not
  match the required pattern`
- **Remediation:** `"use lowercase letters, digits, and dots;
  e.g. 'notes.edit' or 'terminal.shell.execute'"`
- **Fixture:** new `tests/fixtures/rbac/name-invalid-uppercase/`
  (single rule named `Notes.Edit`) — P.M1.

### Rule A.2 — namespace matches package name or a reserved kernel namespace

Every `rules[].namespace` equals the parsed `PackageManifest.name` or
appears in the reserved kernel set — currently `{"kernel", "plinth",
"system"}`, pulled from [cross_file_validator.cpp:38-40](../../src/kernel/packages/cross_file_validator.cpp)'s
`RESERVED_NAMES` array (source of truth). The implementing session
should **expose** `RESERVED_NAMES` via a small accessor in
`src/kernel/packages/reserved_names.hpp` rather than duplicating the
array in `rule_validator.cpp` — one source, two consumers (CF1 and rule validator).

- **Error code:** `rbac.rule.namespace_mismatch`
- **Message template:** `rbac.json rules[{i}].namespace '{value}' does
  not match package name '{package_name}' or any reserved namespace`
- **Remediation:** `"rename the rule's namespace to '{package_name}' or
  one of: kernel, plinth, system"`
- **Fixture:** new `tests/fixtures/rbac/namespace-foreign/` (package
  name `notes`, rule namespace `terminal`) — P.M2.
- **Rationale for the narrower check than CF1.** CF1 permits
  `rules[].namespace` to match *any* provided capability namespace
  (which for a multi-namespace package is a superset of the package
  name). rule validator is the authoritative installer-side check; DESIGN
  §0.4.6 is explicit on "the package name or a reserved kernel
  namespace." See §OQ2.

### Rule A.3 — test call parses as a capability signature

If `rules[].test.assert_deny.call` or `rules[].test.assert_allow.call`
is present, the string parses cleanly through
[plinth::capabilities::parse_signature](../../src/kernel/capabilities/parser.hpp)
(ICD-0.2.1). Function calls carry arguments in parentheses; the parser
slices on the first `(` if present (so `notes:1:edit('hello')` parses
to `{namespace: "notes", version: 1, function: "edit"}` and the
argument list is ignored for signature purposes).

- **Error code:** `rbac.test.call.parse_error`
- **Message template:** `rbac.json rules[{i}].test.{which}.call
  '{value}' is not a valid capability signature (parser error:
  {subcode})`
- **Remediation:** `"format: <namespace>:<version>:<function>
  (e.g. notes:1:create or notes:1:edit('foo'))"`
- **Fixture:** existing `tests/fixtures/packages/rbac-test-parse-fail/`
  — reuse as P.M3. The implementing session copies the fixture's
  `rbac.json` into `tests/fixtures/rbac/test-call-parse-fail/` (the
  outer-package fixture also has a capabilities.json, which rule validator
  does not need for this rule).

### Rule A.4 — test call resolves to a declared capability

The parsed `{namespace, version, function}` from Rule A.3 matches an
entry in the sibling `CapabilityManifest.provides`. Comparison is
exact on all three fields.

- **Error code:** `rbac.test.call.unresolved`
- **Message template:** `rbac.json rules[{i}].test.{which}.call
  '{value}' does not reference any provided capability`
- **Remediation:** `"add the capability to capabilities.json or fix
  the signature to match an existing provides[] entry"`
- **Fixture:** existing `tests/fixtures/packages/rbac-test-bad-call/`
  — reuse as P.M4.
- **Overlap with CF2.** CF2 performs the same check at the cross-file
  phase; rule validator re-checks so the installer pipeline is self-contained
  (a package installed without passing through CF2 — e.g. via a future
  bypass or an API we don't have today — still hits the rule).

### Rule A.5 — rule-name unique across extensions

For every `rules[].rule` string, issue a `SELECT extension_name FROM
plinth.rbac_rules WHERE rule = $1`. If the row exists and its
`extension_name ≠ current_package_name`, emit the error. Same
extension re-registering the same rule name (upgrade) is permitted —
the row is UPDATEd in place by `upsert_extension_rule`, so Rule A.5
skips when the existing row's `extension_name` matches.

- **Error code:** `rbac.rule.name_collision`
- **Message template:** `rbac.json rules[{i}].rule '{value}' is already
  registered by extension '{other_extension_name}'`
- **Remediation:** `"rename this rule; rule names are a global
  identity space across extensions (ICD-0.1.4 §Rule Registration)"`
- **Fixture:** synthetic — P.M5 seeds a row with
  `extension_name='terminal'` + `rule='notes.edit'` and then drives
  the installer for a fresh package named `notes` carrying the same
  rule name.
- **Rule A.5 requires `PGconn&`.** This is the one rule that makes
  `validate_rules` database-coupled; Rules A.1–A.4 are pure.
  Splitting the API to keep Rules A.1–A.4 pure and Rule A.5 in a
  separate `validate_rules_collisions(conn, …)` is attractive but
  yields two call sites + two error lists; the combined API is simpler
  and only the Catch2 cases care about the internal split (they can
  mock the PG arm with a `std::function<std::optional<std::string>(std::string_view)>`
  collision probe — see §Test Cases).

---

## Integration Points

### First-install REGISTERING stage

Current code, `run_stage_registering` at [install_lifecycle.cpp:879-896](../../src/kernel/packages/install_lifecycle.cpp):

```cpp
auto upd1 = update_packages_state(admin, id, "REGISTERING");
if (!upd1.has_value()) { return std::unexpected(upd1.error()); }

// rbac.json rules — 0.4.4 shim …
auto rules = parse_rbac_rules(package_root);
for (const auto& rule : rules) {
    auto u = rbac::upsert_extension_rule(*admin, rule.rule,
                                         rule.namespace_, rule.description,
                                         std::string{name});
    …
}
```

0.4.6 replaces this block with:

```cpp
auto upd1 = update_packages_state(admin, id, "REGISTERING");
if (!upd1.has_value()) { return std::unexpected(upd1.error()); }

// 0.4.6 — typed parse + rule validator validation.
auto rbac_text = read_file(package_root / "rbac.json");  // already exists in the helper
auto rm = plinth::rbac::parse_rbac_manifest(rbac_text, "rbac.json");
for (const auto& msg : rm.messages) {
    if (msg.severity == plinth::packages::Severity::ERROR) {
        return std::unexpected("rbac parse error: " + msg.rule
                               + " — " + msg.message);
    }
}
if (rm.value.has_value()) {
    auto findings = plinth::rbac::validate_rules(*rm.value, cm, name, *admin);
    for (const auto& f : findings) {
        if (f.severity == plinth::packages::Severity::ERROR) {
            return std::unexpected("rbac phase-a: " + f.rule + " — " + f.message);
        }
    }
    for (const auto& r : rm.value->rules) {
        auto u = plinth::rbac::upsert_extension_rule(
            *admin, r.rule, r.namespace_, r.description,
            std::string{name}, r.test);
        if (!u.has_value()) {
            return std::unexpected("rbac rule insert failed: " + r.rule);
        }
    }
}
```

The outer `RollbackGuard` already ROLLBACKs the REGISTERING transaction
on `std::unexpected` return; state goes to INSTALL_FAILED by the
existing 0.4.4 machinery.

### Upgrade reconciliation

`reconcile_rbac_on_upgrade` at [install_lifecycle.cpp:686-764](../../src/kernel/packages/install_lifecycle.cpp).
Two edits:

1. Change the `v2_rules` parameter type from
   `const std::vector<RbacRuleEntry>&` to
   `const std::vector<plinth::rbac::RbacRule>&` (remove the old shim
   type; it is dead after the first-install switch).
2. The `upsert_extension_rule` call at [:720-723](../../src/kernel/packages/install_lifecycle.cpp)
   gains the `r.test` argument.

rule validator runs *before* `reconcile_rbac_on_upgrade` — in the upgrade
variant of `run_stage_registering`, same shape as first-install (parse
→ validate → then either upsert loop (first-install) or reconciliation
(upgrade)). Rule A.5 skips same-extension matches, so an upgrade that
reuses the same rule names is silent.

### Asset load order invariant

rule validator runs *before* `upsert_extension_rule` / `register_capability_tx`.
A package whose `capabilities.json` references an `rbac_rule` override
still benefits: `register_capability_tx`'s `rbac_rule_exists`
precondition (ICD-0.2.0 / ICD-0.4.4) is still satisfied by the
post-Phase-A upsert. No ordering change.

### Dry-run validation — `plinth validate` CLI

0.4.0 `plinth validate` runs CF1/CF2 but not rule validator (no PG). The
implementing session does NOT wire rule validator into the CLI. A
`--against-running-kernel` flag that exposes Rule A.5 via a live PG
connection is out of scope (future arc if it surfaces); CF1/CF2
cover A.2 / A.3 / A.4 at the offline phase, and A.1 / A.5 are
install-time-only concerns by design.

---

## Relationship to CF1 / CF2 / CFW1

| CF | Current scope (offline, no PG) | rule validator mirror | Disposition |
|----|--------------------------------|----------------|-------------|
| CF1 `rbac-orphan-namespace` | rule.namespace ∈ {reserved} ∪ capability.namespaces | Rule A.2 (package name ∪ reserved — **narrower**) | Keep CF1. rule validator is authoritative on package name match; CF1 stays as a broader offline filter so `plinth validate` still accepts rule namespaces that match *any* capability namespace even when they don't match the package name. See §OQ2 for the deliberate divergence. |
| CF2 `rbac-test-call-parse` / `rbac-test-call` | test.call parses + resolves to a capability | Rules A.3 / A.4 | Keep CF2. rule validator re-checks with the same semantics. Error codes disjoint (`rbac.test.call.parse_error` / `rbac.test.call.unresolved` vs `rbac-test-call-parse` / `rbac-test-call`) so both codes can surface in one report without a dedup requirement. rule validator is authoritative; CF2 fires earlier for `plinth validate` offline users. |
| CFW1 `capability-without-rule` | warn if a capability has no rule covering its namespace | — | Keep. No rule validator equivalent — it's a *warning* layered on the capabilities manifest side, not an error on the rbac side. |

**No deletions, no deprecations.** The duplicate emissions at install
time (CF1 + rule validator A.2 for the same mismatch) are tolerable: CF1
runs during the pre-install cross-file validation pass (Drogon HTTP
handler in `handlers.cpp`), rule validator runs later in the REGISTERING
stage. The installer only reaches REGISTERING if CF1/CF2 passed, so in
practice rule validator's duplicate codes fire only on **new** mismatches
CF1/CF2 cannot see — principally A.5 (PG required) and A.2's
narrower-than-CF1 wording.

---

## Schema edit

[migrations/schema.sql:76-84](../../migrations/schema.sql) currently:

```sql
CREATE TABLE plinth.rbac_rules (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    rule           TEXT        UNIQUE NOT NULL,
    namespace      TEXT        NOT NULL,
    description    TEXT        NOT NULL,
    extension_name TEXT        NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    orphaned_at    TIMESTAMPTZ
);
```

0.4.6 edits to:

```sql
CREATE TABLE plinth.rbac_rules (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    rule           TEXT        UNIQUE NOT NULL,
    namespace      TEXT        NOT NULL,
    description    TEXT        NOT NULL,
    extension_name TEXT        NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    orphaned_at    TIMESTAMPTZ,
    test_contract  JSONB
);
```

Nullable — rules without a `test` object have `test_contract IS NULL`.
Pre-0.7 freeze rules (ROADMAP preamble) permit the direct edit; no
numbered migration file.

No index on the new column (RBAC test's lookups are per-extension via
`extension_name`, already covered by the table's row-level access
pattern). No FK — `test_contract` is data, not a reference.

No change to the `plinth.group_rules` FK to `plinth.rbac_rules.id`;
0.4.6 touches only one column on one table.

---

## Library surface changes

### `upsert_extension_rule` — extended signature

```cpp
// src/kernel/rbac/rule_registrar.hpp — BEFORE (0.4.4)
auto upsert_extension_rule(PGconn& conn,
                           std::string_view rule,
                           std::string_view namespace_,
                           std::string_view description,
                           std::string_view extension_name)
    -> std::expected<void, std::string>;
```

```cpp
// src/kernel/rbac/rule_registrar.hpp — AFTER (0.4.6)
auto upsert_extension_rule(PGconn& conn,
                           std::string_view rule,
                           std::string_view namespace_,
                           std::string_view description,
                           std::string_view extension_name,
                           std::optional<nlohmann::json> test_contract)
    -> std::expected<void, std::string>;
```

**Additive, not defaulted.** the maintainer's project convention (see memory
`feedback_deterministic_teardown.md` + `feedback_tagging_rule.md`
nuances) favors explicit-over-defaulted for cross-subsystem contracts.
The two existing callers — `install_lifecycle.cpp:889` and
`install_lifecycle.cpp:720` — are updated in the same PR. Out-of-tree
callers (none known) recompile.

SQL change in `rule_registrar.cpp`:

```sql
-- BEFORE
INSERT INTO plinth.rbac_rules
    (rule, namespace, description, extension_name)
VALUES ($1, $2, $3, $4)
ON CONFLICT (rule) DO UPDATE
  SET namespace      = EXCLUDED.namespace,
      description    = EXCLUDED.description,
      extension_name = EXCLUDED.extension_name,
      orphaned_at    = NULL;

-- AFTER
INSERT INTO plinth.rbac_rules
    (rule, namespace, description, extension_name, test_contract)
VALUES ($1, $2, $3, $4, $5::jsonb)
ON CONFLICT (rule) DO UPDATE
  SET namespace      = EXCLUDED.namespace,
      description    = EXCLUDED.description,
      extension_name = EXCLUDED.extension_name,
      test_contract  = EXCLUDED.test_contract,
      orphaned_at    = NULL;
```

`$5` is `NULL` (not `'null'::jsonb`) when `test_contract == std::nullopt`.
Implementing session passes `nullptr` for the corresponding entry in
the `paramValues` array to `PQexecParams`.

### `parse_rbac_manifest` — new entry point

See §RbacManifest — C++ surface.

### `validate_rules` — new entry point

```cpp
// src/kernel/rbac/rule_validator.hpp
#pragma once

#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest_error.hpp"
#include "kernel/rbac/rbac_manifest.hpp"

#include <string_view>
#include <vector>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::rbac {

// Runs Rules A.1 through A.5 against the parsed rbac.json. Returns all
// findings (no stopping at the first). Rule A.5 touches PG; the caller
// owns the transaction (rule validator is called inside the REGISTERING
// stage's BEGIN / COMMIT).
auto validate_rules(const RbacManifest& rbac,
                      const plinth::packages::CapabilityManifest& caps,
                      std::string_view package_name,
                      PGconn& conn)
    -> std::vector<plinth::packages::ManifestParseError>;

}  // namespace plinth::rbac
```

---

## Test Cases

Test prefix: **P.*** (Phase-A; distinct from ICD-0.4.5's D/U/E/X/R
per-transition prefixes and ICD-0.3.4's G/M). Twelve cases total: one
happy, five mechanical-invalid (one per rule validator rule), four edge, two
round-trip.

| # | Type | Scenario | Fixture / Seed | Expected outcome |
|---|------|----------|----------------|------------------|
| P.01 | Happy | Package `notes` with rule `notes.edit`, full test object | `tests/fixtures/rbac/happy-full/` | `parse_rbac_manifest` returns value; `validate_rules` returns empty vector; row inserted with `test_contract` = the full `test` JSON |
| P.M1 | Rule A.1 — name regex | Package `notes` with rule `Notes.Edit` | `tests/fixtures/rbac/name-invalid-uppercase/` | One finding, code `rbac.rule.invalid_name` |
| P.M2 | Rule A.2 — namespace mismatch | Package `notes`, rule namespace `terminal` (not reserved, not `notes`) | `tests/fixtures/rbac/namespace-foreign/` | One finding, code `rbac.rule.namespace_mismatch` |
| P.M3 | Rule A.3 — call parse fail | Package `notes`, rule test call `notes:badversion:edit` | copy of `tests/fixtures/packages/rbac-test-parse-fail/rbac.json` | One finding, code `rbac.test.call.parse_error` |
| P.M4 | Rule A.4 — call unresolved | Package `notes` with `capabilities.json` providing `notes:1:create`; rbac rule test call `notes:1:nonexistent` | copy of `tests/fixtures/packages/rbac-test-bad-call/rbac.json` | One finding, code `rbac.test.call.unresolved` |
| P.M5 | Rule A.5 — cross-extension collision | Seed a row with `rule='notes.edit', extension_name='terminal'`; install package `notes` declaring rule `notes.edit` | synthetic, in-test seed | One finding, code `rbac.rule.name_collision`, message carries the other extension name |
| P.E1 | Edge — empty rules | `{"rules": []}` | `tests/fixtures/rbac/empty-rules/` | Empty findings; no rows inserted |
| P.E2 | Edge — assert_deny only | `{"rules": [{"rule": ..., "namespace": ..., "description": ..., "test": {"assert_deny": {...}}}]}` | `tests/fixtures/rbac/test-deny-only/` | Parse OK; `test_contract` round-trips just the `assert_deny` key |
| P.E3 | Edge — assert_allow only | Symmetric to P.E2 | `tests/fixtures/rbac/test-allow-only/` | Parse OK; `test_contract` round-trips just the `assert_allow` key |
| P.E4 | Edge — no test object | `{"rules": [{"rule": ..., "namespace": ..., "description": "no-test"}]}` | `tests/fixtures/rbac/no-test/` | Parse OK; `test_contract IS NULL` after upsert |
| P.N1 | Negative — same-extension re-registration | Seed row `rule='notes.edit', extension_name='notes'`; re-run rule validator for package `notes` | synthetic, in-test seed | Empty findings (Rule A.5 exemption); `upsert_extension_rule` UPDATEs in place |
| P.R1 | Round-trip | Serialize a parsed `RbacManifest` and re-parse; assert equality on `rules[]` | `tests/fixtures/rbac/happy-full/` | Field-by-field equality on every `RbacRule` member, including `test` |

**P.M5 mocking strategy.** The implementing session has two options:
(a) PG-gated test (runs under `PLINTH_KERNEL_TESTS=ON` with the async-
bridge fixture's PG setup; seeds one row, asserts the error, tears
down); (b) pure unit with a collision-probe functor injected into
`validate_rules` — e.g.
`auto findings = validate_rules_with_probe(rbac, caps, name,
[](std::string_view rule_name) { return "terminal"; });`. The
recommended route is (a): the extra plumbing for (b) adds a second
public entry point only tests use, and the PG path is already stable
under the 0.4.5.1 grouped-test runner. The implementing session picks.

**Total new test cases:** 12. Distributed across
`tests/kernel/rbac/rbac_manifest_test.cpp` (P.01, P.E1-E4, P.R1 — six
cases, no PG) and `tests/kernel/rbac/rule_validator_test.cpp` (P.M1-M5, P.N1
— six cases, PG-gated for M5+N1). Six pure, six PG-gated.

### CI wiring

- `src/kernel/rbac/rbac_manifest.{hpp,cpp}` — new.
- `src/kernel/rbac/rule_validator.{hpp,cpp}` — new.
- `src/kernel/rbac/rule_registrar.{hpp,cpp}` — modify.
- `src/kernel/packages/install_lifecycle.cpp` — modify (two
  call-sites).
- `src/kernel/packages/reserved_names.hpp` — new (exposes
  `RESERVED_NAMES` for reuse; moves the array out of
  `cross_file_validator.cpp`'s anonymous namespace).
- `migrations/schema.sql` — one-line addition.
- `tests/kernel/rbac/rbac_manifest_test.cpp` — new.
- `tests/kernel/rbac/rule_validator_test.cpp` — new.
- `tests/fixtures/rbac/` — new subtree, 6 fixture directories.
- `CMakeLists.txt` — no edit required. The 0.4.5.1 grouped-test model
  (`plinth_tests_pg` / `plinth_tests_pure`) already consumes the
  `tests/kernel/**` glob. `rule_validator_test.cpp` carries the `[pg]` tag;
  `rbac_manifest_test.cpp` is tagless-pure.
- No new CI job.

### Test count target

~12 Catch2 cases. Adjust in the implementing session.

---

## Security

1. **Adversarial JSON.** `parse_rbac_manifest` uses
   `nlohmann::json::parse` with `allow_exceptions = false` per
   ICD-0.4.1 §Security Constraints. Same 50 MB upstream size cap
   (enforced by 0.4.0's file-read layer) bounds the input;
   `recursion_limit = 128` bounds depth.
2. **SQL injection.** `upsert_extension_rule` uses `PQexecParams`
   with positional parameters. The `$5::jsonb` cast is server-side;
   the client never concatenates the JSON string into the SQL body.
3. **No execution.** rule validator never invokes a capability. The
   `test.*.call` strings are parsed only through `parse_signature`;
   the signature parser is pure and has 0.2.1 fuzz coverage
   (`tests/kernel/capabilities/fuzz_parser.cpp`).
4. **No filesystem side effects.** rule validator reads `rbac.json` via the
   existing 0.4.0 `read_file` helper (bounded size) — no directory
   traversal, no symlink follow. The parser takes bytes, not a path.
5. **Rule A.5 read-only.** Rule A.5 issues a `SELECT` only; no
   write-side-effect. The write is the *subsequent* `upsert_extension_rule`
   call, outside rule validator.

---

## Entry / Exit

**Entry criteria:**
- v0.4.5 is merged on main (done — tag `v0.4.5`, merge commit
  `ecf734a`).
- ICD-0.2.1's `parse_signature` is available (done — v0.2.1).
- ICD-0.1.4's rule storage contract is stable (done — v0.1.4 through
  v0.4.5 edits only `orphaned_at` semantics; `test_contract` is
  additive).
- ICD-0.4.1's `CapabilityManifest` parser is in use (done — consumed by
  0.4.4 REGISTERING stage, still in use at 0.4.5 upgrade branch).

**Exit criteria:**
- Every rule validator rule has a named test case (P.01, P.M1–P.M5, P.N1 —
  7 of 12).
- Each edge case (empty rules, asserts-missing, no-test) has coverage
  (P.E1–P.E4 — 4 of 12).
- Round-trip `parse → serialize → parse` case (P.R1 — 1 of 12).
- Catch2 full suite passes under default and sanitizer builds.
- `run-clang-tidy-20` zero findings on new translation units
  (`rbac_manifest.cpp`, `rule_validator.cpp`, `reserved_names.hpp`) and
  modified TUs (`rule_registrar.cpp`, `install_lifecycle.cpp`).
- `CHANGELOG.md` 0.4.6 entry describes the typed parser, rule validator rules,
  and the one-column schema edit.
- `docs/ROADMAP.md §0.4` line for 0.4.6 is removed.
- No regression on the 0.4.5 test matrix (459/459 → 459+12 = 471 Catch2
  cases, subject to the `PLINTH_KERNEL_TESTS=ON` PG gate on P.M5 /
  P.N1).

---

## Open Questions

**OQ1 — Rule A.5 collision check implementation.** The `plinth.rbac_rules.rule`
column already carries `UNIQUE` (schema.sql:78), so the INSERT in
`upsert_extension_rule` will fail at the DB layer when a different
extension has registered the same rule. Doing the SELECT in rule validator is
strictly a UX improvement — fails fast with a structured error
including `other_extension_name`, vs the terser PG `unique_violation`
message. Recommend: **do the SELECT in rule validator; keep the UNIQUE as
backstop**. Rationale: the error message is the load-bearing piece —
"rule is already registered by extension 'terminal'" is actionable; a
bare unique-violation is not.

**OQ2 — Rule A.2 narrower than CF1 (package name vs any capability
namespace).** DESIGN §0.4.6 says "matches the package name or a
reserved kernel namespace." CF1 (offline) accepts any capability
namespace (superset — a package providing capabilities in namespaces
`notes` and `notesfe` could register rules in either). Preserving CF1
as-is is the lower-blast-radius option; tightening CF1 to match rule validator
would regress extension authors who rely on CF1 accepting multiple
namespaces. Recommend: **follow DESIGN verbatim in rule validator (package
name OR reserved), keep CF1 as-is.** Any fixture that passes CF1 but
fails rule validator emits a clear error at install time, not at
`plinth validate` time — the admin learns at install, not at upload.
If this turns out to be a real ergonomic issue, a 0.4.7 RE-EVAL can
broaden Rule A.2; the schema does not need to change.

**OQ3 — CF1/CF2 disposition.** Keep both. See §Relationship to
CF1/CF2/CFW1. Error-code namespaces disjoint, reporters carry both.
No deprecation.

**OQ4 — Error shape.** Reuse `plinth::packages::ManifestParseError`
with a new `rbac.*` prefix. Do not invent a parallel `ManifestParseError`
type. Reason: the installer caller already knows how to iterate
`ManifestParseError`; a second type doubles the surface and the
`field_path` / `remediation` semantics are identical.

**OQ5 — `RESERVED_NAMES` promotion.** Currently a `static constexpr` in
`cross_file_validator.cpp`'s anonymous namespace (lines 38-40). The
implementing session must expose it — either a new
`src/kernel/packages/reserved_names.hpp` with a free function
`is_reserved_kernel_namespace(std::string_view) -> bool`, or a `public:`
method on a new `ReservedNames` namespace. The free function is the
simpler fit; the new file's only content is the `constexpr` array +
the accessor. Ancillary motion — flag in the commit message.

**OQ6 — Missing `rbac.json` vs empty `rbac.json`.** A package with no
`rbac.json` file at all is different from one with `{"rules": []}`.
The existing 0.4.4 behavior (`parse_rbac_rules` at
`install_lifecycle.cpp:651` returns empty on missing file) is
preserved: missing file → no rules, no rule validator run, no error. Present
file with bad shape → rule validator error. Tested implicitly by P.E1 (empty
array) and a new P.E5 if the implementing session wants to add it
(optional — the 0.4.4 test matrix already covers the missing-file
case).

---

## Appendix: Error Example

Input `rbac.json`:
```json
{
  "rules": [
    {
      "rule": "Notes.Edit",
      "namespace": "terminal",
      "description": "edit notes",
      "test": {
        "assert_deny": {
          "call": "notes:badversion:edit",
          "expect": "permission_denied"
        }
      }
    }
  ]
}
```

Sibling `capabilities.json` declares `provides[] = [{namespace:
"notes", version: 1, function: "create"}]`, sibling `manifest.json`
declares `name: "notes"`.

rule validator output (`std::vector<ManifestParseError>`):

```
[
  { rule: "rbac.rule.invalid_name",
    field_path: "/rules/0/rule",
    message: "rbac.json rules[0].rule 'Notes.Edit' does not match the required pattern",
    remediation: "use lowercase letters, digits, and dots; e.g. 'notes.edit' or 'terminal.shell.execute'",
    severity: ERROR },
  { rule: "rbac.rule.namespace_mismatch",
    field_path: "/rules/0/namespace",
    message: "rbac.json rules[0].namespace 'terminal' does not match package name 'notes' or any reserved namespace",
    remediation: "rename the rule's namespace to 'notes' or one of: kernel, plinth, system",
    severity: ERROR },
  { rule: "rbac.test.call.parse_error",
    field_path: "/rules/0/test/assert_deny/call",
    message: "rbac.json rules[0].test.assert_deny.call 'notes:badversion:edit' is not a valid capability signature (parser error: invalid_version)",
    remediation: "format: <namespace>:<version>:<function> (e.g. notes:1:create or notes:1:edit('foo'))",
    severity: ERROR }
]
```

No row inserted. The installer's `RollbackGuard` ROLLBACKs REGISTERING;
state goes to INSTALL_FAILED with the three errors surfaced in the
detail JSON of the `packages.install_failed` audit event.

---

## Appendix: Round-trip Example

Input:
```json
{
  "rules": [
    {
      "rule": "notes.edit",
      "namespace": "notes",
      "description": "Edit notes",
      "test": {
        "assert_deny": { "call": "notes:1:edit('hello')", "expect": "permission_denied" },
        "assert_allow": { "call": "notes:1:edit('hello')", "expect": "success" }
      },
      "future_field": "ignored-silently"
    }
  ],
  "unknown_top_level": true
}
```

Parse:
- `rules.size() == 1`
- `rules[0].rule == "notes.edit"`, `namespace_ == "notes"`,
  `description == "Edit notes"`
- `rules[0].test == { assert_deny: {...}, assert_allow: {...},
  future_field: "ignored-silently" }` (the entire `test` object
  including unknown fields, stored verbatim)
- `unknown_fields == { "unknown_top_level": true }`

Serialize emits unknown top-level fields first, then `"rules"`. Within
each rule, unknown keys in `test` are preserved in the dumped JSON.
Re-parsing yields the identical struct.
