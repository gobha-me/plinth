# ICD-0.4.7-rbac-test-execution

> **Amendment 2026-04-22 (0.5.0.1)** — Internal & public surfaces
> renamed from `phase_b` / `phase_a` to `rbac_test` / `rule_validator`
> per naming convention (classes=nouns, methods=verbs, fields=adjectives).
> Schema columns `last_rbac_test_run_at` / `last_rbac_test_result` →
> `last_rbac_test_run_at` / `last_rbac_test_result`; audit events
> `packages.rbac_test_passed` / `_failed` →
> `packages.rbac_test_passed` / `_failed`; namespace
> `plinth::packages::rbac_test` → `plinth::packages::rbac_test`; function
> `plinth::rbac::validate_rules` → `plinth::rbac::validate_rules`;
> Catch2 tags `[rbac_test]` / `[rbac_test_report]` / `[rule_validator]` →
> `[rbac_test]` / `[rbac_test_report]` / `[rule_validator]`. This ICD's
> body is rewritten in place; the original as-shipped names are
> preserved in the v0.4.7 release commit (`4a86e31`).

**Traces to:** `docs/design/DESIGN-packages-v04x.md §0.4.7` (authoritative
contract for RBAC-test execution — ephemeral-user construction, per-rule
invocation, result handling, and re-run semantics);
`DESIGN-packages-v04x.md §4.1` (`plinth.packages.last_rbac_test_run_at` +
`last_rbac_test_result` columns — already in schema; 0.4.7 is the first
writer); `DESIGN-packages-v04x.md §4.2` (`plinth.rbac_rules.test_contract`
— already populated by 0.4.6; 0.4.7 is the first reader);
`architecture/01-identity.md §2.2` (*RBAC Test Validation — Two Phase*;
normative for the rule-validator / RBAC-test split and the "flag, don't
uninstall" failure semantic); `architecture/01-identity.md §2.4` (Rule
Lifecycle — "Extension starts → RBAC integration tests run" is the bullet
this ICD implements).
**Depends on:** ICD-0.4.6-rbac-rule-registration (`test_contract JSONB`
column + typed `RbacManifest` round-trippable shape — 0.4.7 reads
`test_contract` back and invokes its `call` strings); ICD-0.4.5-package-lifecycle-transitions
(`PackageRecord` + `TransitionFailure` + per-name advisory-lock pattern
reused for re-run serialisation; `ACTIVE_FLAGGED` already reserved in
the `InstallStage` enum at `src/kernel/packages/install_lifecycle.hpp:48`
and in the `state` CHECK at `migrations/schema.sql:68`); ICD-0.4.4-package-install-lifecycle
(`install_package` completion handoff — RBAC test fires after the outer
transaction commits and ACTIVE is durable; terminal audit-event pattern
reused); ICD-0.2.2-capability-resolution (`call_capability_async` +
`UserContext` + `CapabilityCall` — the dispatch surface RBAC test
invokes); ICD-0.2.4-capability-rbac (`PERMISSION_DENIED` enum variant —
the exact error shape `assert_deny` expects); ICD-0.2.1-capability-parser
(`parse_signature` — already used by 0.4.6 rule validator Rule A.3 to
structurally validate `test_contract.*.call`; 0.4.7 re-parses the same
string at runtime to build a `CapabilityCall`); ICD-0.1.2-auth-sessions
(`plinth.users` identity storage — 0.4.7 adds one column);
ICD-0.1.4-groups-rbac (`plinth.groups` + `plinth.group_members` +
`plinth.group_rules` — 0.4.7 creates ephemeral memberships to grant a
single rule); ICD-0.1.5-rbac-enforcement (the enforcement path
`call_capability` walks — RBAC test's correctness fully depends on 0.2.4
RBAC-in-dispatch being live); ICD-0.1.7-audit (`audit_sync` shape —
two new event kinds).
**Milestone:** 0.4.7 — the final code milestone of the 0.4 arc.
Executes the `assert_deny` / `assert_allow` test contracts stored by
0.4.6, flags packages whose rule contract is not correctly enforced,
and exposes a re-run CLI. Does NOT extend the
`plinth.rbac_rules.test_contract` schema (additive column is 0.4.6's
contribution), does NOT touch `ICD-0.1.4`'s groups/rules HTTP surface
(the new ephemeral-membership machinery is internal — the API rows stay
visible byte-identical), does NOT introduce sandboxed or reversible
execution (`assert_allow` side effects are the extension's
responsibility per DESIGN §0.4.7), does NOT run on disable / enable /
uninstall paths (only on install completion and on demand).
**Status:** Ready for implementation
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Related:**
[src/kernel/packages/install_lifecycle.cpp:1580-1585](../../src/kernel/packages/install_lifecycle.cpp)
(end of `install_package`, immediately after `emit_installed_audit` —
the fire-and-forget hand-off point);
[src/kernel/packages/install_lifecycle.cpp:1930-1945](../../src/kernel/packages/install_lifecycle.cpp)
(end of `enable_package` — second trigger site for RBAC test,
per DESIGN §0.4.5 *Re-runs Phase-B tests* bullet);
[src/kernel/packages/install_lifecycle.cpp:3120-3130](../../src/kernel/packages/install_lifecycle.cpp)
(end of `upgrade_package` — third trigger site; new-version row's
RBAC test runs against the now-current handlers);
[src/kernel/packages/install_lifecycle.hpp:48](../../src/kernel/packages/install_lifecycle.hpp)
(`InstallStage::ACTIVE_FLAGGED` — enum carries it since 0.4.5; 0.4.7
becomes the first writer);
[migrations/schema.sql:68](../../migrations/schema.sql)
(`state` CHECK constraint — already lists `'ACTIVE_FLAGGED'`; 0.4.7
is the first writer);
[migrations/schema.sql:25-31](../../migrations/schema.sql)
(`plinth.users` — 0.4.7 adds `is_test_user BOOLEAN NOT NULL DEFAULT
false`);
[migrations/schema.sql:82](../../migrations/schema.sql)
(`plinth.rbac_rules.test_contract` — populated by 0.4.6, read by 0.4.7);
[src/kernel/capabilities/resolution.hpp:147-162](../../src/kernel/capabilities/resolution.hpp)
(`call_capability` / `call_capability_async` entry points — the
dispatch surface RBAC test invokes);
[src/kernel/capabilities/resolution.hpp:84-122](../../src/kernel/capabilities/resolution.hpp)
(`UserContext` struct + `anonymous_with_rules` factory — 0.4.7 adds a
parallel `test_user` factory that keys on `user_id + effective_rules`
populated from the ephemeral row);
[src/kernel/capabilities/types.hpp](../../src/kernel/capabilities/types.hpp)
(`CapabilityError::PERMISSION_DENIED` — the exact error variant the
`assert_deny` comparator tests against);
[src/kernel/capabilities/parser.hpp](../../src/kernel/capabilities/parser.hpp)
(`parse_signature` — reused to build a `CapabilityCall` from the
`test_contract.call` string);
[src/kernel/main.cpp:113-167](../../src/kernel/main.cpp)
(argparse subcommand wiring pattern — `test rbac` lands alongside
`serve` + `validate`);
[src/kernel/rbac/rule_registrar.hpp:48-54](../../src/kernel/rbac/rule_registrar.hpp)
(`upsert_extension_rule` signature with `test_contract` — 0.4.7 reads
the column back with a dedicated `fetch_extension_rules_for_rbac_test`
helper; no change to the upsert surface).

---

## Overview

0.4.6 registered the typed RBAC rule surface and stashed every rule's
`test` object verbatim into `plinth.rbac_rules.test_contract`. 0.4.7
runs those tests. After a package reaches `ACTIVE`, the kernel
schedules a post-install RBAC test run: ephemeral test users are created,
each rule's `assert_deny` / `assert_allow` is invoked through the
normal `call_capability_async` pipeline, the outcome is compared to the
declared expectation, and the aggregate result is persisted in
`plinth.packages.last_rbac_test_result`. All-pass leaves the package
`ACTIVE` and audits `packages.rbac_test_passed`; any-fail transitions the
package to `ACTIVE_FLAGGED` and audits `packages.rbac_test_failed` with
per-rule detail. The flag is advisory — the extension keeps running —
and is cleared by any successful re-run via the new `plinth test rbac
{extension}` subcommand.

The machinery is deliberately thin. RBAC test is not a sandbox, not a
new dispatch path, and not a new authentication surface. It synthesises
a `UserContext`, calls the real dispatcher, and reads the
`std::expected<CapabilityResult, CapabilityError>` back. The 0.2.4 RBAC
check fires inside `call_capability` unchanged; the whole value of
RBAC test is that it asserts that check fires the way the extension
author said it would.

This is also the arc close-out milestone. After 0.4.7 the 0.4.x
`[strong]`/`[medium]` bands are fully shipped, and the `RE-EVAL
following 0.4.7 (0.4.x arc closeout)` rewrite session opens next.

**Scope:**

- **One schema edit.** `plinth.users` gains `is_test_user BOOLEAN NOT
  NULL DEFAULT false`. Pre-0.7 freeze, direct edit to `schema.sql`.
  Every user-listing query that *returns a set* (admin user browsers,
  test fixtures that assert row counts) gains a `WHERE is_test_user =
  false` predicate; queries that target a specific `username` or `id`
  do not (the 0.4.7 ephemeral-user names are predictable but
  sufficiently collision-resistant to keep the predicate local).
- **New module** `src/kernel/packages/rbac_test_runner.{hpp,cpp}`. Public
  surface: `run_rbac_test(package_id, ctx) -> std::expected<RbacTestReport,
  RbacTestFailure>` (synchronous entry; caller owns scheduling);
  `schedule_rbac_test_detached(package_id, ctx)` (the post-install
  dispatcher — spawns a detached `std::jthread` that calls
  `run_rbac_test` and emits audit). Helper types `RbacTestReport` (shape
  matches `last_rbac_test_result` JSONB: `{run_id, started_at,
  duration_ms, passed: [rule], failed: [{rule, clause, expected,
  actual}], skipped: [rule]}`) and `RbacTestFailure` (operational errors
  — DB unreachable, package not found, etc.; distinct from the
  per-rule failure collection inside `RbacTestReport.failed`).
- **Ephemeral-user machinery** in `src/kernel/rbac/ephemeral_user.{hpp,cpp}`.
  `create_run_users(run_id, rule_name, conn) -> std::expected<RunUserPair,
  std::string>` creates both `__test_denied_{run_id}` (in `everyone`
  only) and `__test_allowed_{run_id}` (in `everyone` + a synthetic
  `__rbac_test_{run_id}` group granted the single rule under test).
  `destroy_run_users(run_id, conn) -> std::expected<void, std::string>`
  is idempotent and reachable from three call sites: the happy-path
  end of `run_rbac_test`, the failure path, and the crash-recovery
  reconciler. `cleanup_orphaned_test_users(older_than, conn)` is the
  reconciler's blanket sweep.
- **Three trigger sites.** `install_package` (after
  `emit_installed_audit`); `enable_package` (after
  `emit_transition_audit("packages.enabled")`); `upgrade_package`
  (after `emit_upgrade_audit("packages.upgrade_completed")`). All
  three call `schedule_rbac_test_detached` — fire-and-forget; the HTTP
  response returns immediately. The trigger site is guarded by the
  post-COMMIT barrier: the detached thread only starts after the PG
  transaction that flipped state to `ACTIVE` is durable, so a crash
  between state=ACTIVE and RBAC test start is indistinguishable from a
  crash before RBAC test start — the reconciler handles both the same
  way.
- **Per-name advisory lock reuse.** RBAC test acquires the existing
  `hashtextextended('plinth.packages.' || name, 0)` lock from the
  0.4.4 install path. Concurrent installs + RBAC test runs for the same
  package serialise; different packages proceed in parallel. This
  also serialises the CLI re-run path with an in-flight install's
  RBAC test — the re-run waits.
- **Crash recovery extension.** `reconcile_in_flight_installs` gains
  two new responsibilities: (a) call
  `cleanup_orphaned_test_users(older_than = 1 hour)` once per
  invocation to garbage-collect users whose RBAC test run died
  mid-execution (and whose membership in `__rbac_test_{run_id}` makes
  them trivially identifiable); (b) for any package at `ACTIVE` with
  `last_rbac_test_run_at IS NULL` AND `installed_at > NOW() - interval
  '1 hour'`, schedule a RBAC test run (the previous trigger was lost to
  the crash). Older-than-1h packages with NULL `last_rbac_test_run_at`
  stay NULL — the admin can re-run manually; bootstrap auto-triggers
  are bounded.
- **New CLI subcommand.** `plinth test rbac {extension}` in
  `src/kernel/main.cpp`, mirroring the existing `serve` / `validate`
  subparser shape. Exit codes 0 (all pass), 1 (any fail — matching
  the "flag" state), 2 (operational error — extension not found, DB
  unavailable, package not in `ACTIVE` or `ACTIVE_FLAGGED`). `--json`
  flag reuses the 0.4.0 `ValidatorOptions.json_output` pattern.
- **Two audit events.** `packages.rbac_test_passed` +
  `packages.rbac_test_failed`. Emitted via the same
  `plinth::log::audit_sync(ctx.db, kind, detail)` surface used at
  install / disable / enable / upgrade. Detail payload mirrors the
  `RbacTestReport` shape (see §Audit Surface). Rate limit: none at the
  RBAC test layer (a package with N failing rules emits one
  aggregate `packages.rbac_test_failed` event, not N).
- **Test fixtures.** A new `tests/fixtures/rbac_test_runner/` tree with 6
  fixture packages: `happy-all-pass`, `broken-assert-deny`,
  `broken-assert-allow`, `mixed-pass-fail`, `no-test-contracts`, and
  `assert-allow-side-effect` (the last one verifies the non-permission
  error passes `assert_allow`). Each fixture is a full `.zip` blob
  the existing `install_package` path consumes; RBAC test exercises
  them by installing, observing the `last_rbac_test_result`, and
  asserting the expected pass/fail partition.
- **CI wiring.** No CMake glob changes (0.4.5.1 grouped test model
  already consumes `tests/kernel/**`). One new test TU in
  `plinth_tests_pg` (the PG group), one in `plinth_tests_pure` (the
  ephemeral-user-factory unit tests + `RbacTestReport` JSON
  round-trip). No new CI job.

**Out of scope (deferred):**

- **Extension-controlled test isolation.** DESIGN §0.4.7 is explicit:
  extensions are responsible for making `assert_allow` idempotent.
  0.4.7 does not provide a sandbox, a transaction barrier, or a
  dry-run mode for capability calls. An extension whose
  `assert_allow: terminal:1:shell('rm -rf /')` fails the production
  filesystem fails itself — RBAC test is a contract enforcement, not a
  safety net. The extension guide documents the responsibility; see
  §Security Constraint 4.
- **Admin notification channel beyond audit.** DESIGN §0.4.7 speaks
  of "admin is notified." 0.4.7 ships the audit event surface only;
  the admin extension UI (0.6a-A) subscribes to the audit stream to
  surface the notification. A dedicated `packages.rbac_test_failed`
  WebSocket event is deferred to 0.5.2 (broker).
- **Per-rule timeout beyond the global `InstallerContext::upgrade_drain_timeout_ms`.**
  OQ4 — see §Open Questions. 0.4.7 reuses the existing 5-second
  drain budget as the per-rule wall-clock cap. A dedicated RBAC test
  timeout knob is deferred.
- **Parallel rule invocation.** RBAC test iterates rules sequentially
  inside `run_rbac_test`. `assert_allow` on rule N+1 does not overlap
  `assert_deny` on rule N. DESIGN §0.4.7 does not mandate parallel
  dispatch, and the sequential shape keeps the `std::expected`
  aggregation trivial. The choice is documented; OQ-style
  justification lives in §Open Questions OQ-? and is not a true open
  question — just a deviation-to-watch.
- **RBAC test on disable.** DESIGN §0.4.7 "re-run on demand" is
  satisfied by the CLI. Disable does not re-run RBAC test (the package
  isn't executing; there's nothing to test). Enable does, because
  the handlers just came back online.
- **Test-user password strength / PAT issuance.** Ephemeral users
  have an `argon2id` placeholder hash that no code path ever
  verifies — they authenticate *inside* the kernel via synthesised
  `UserContext`, never over HTTP. The placeholder exists only to
  satisfy the `password_hash TEXT NOT NULL` constraint.
- **RBAC test for kernel capabilities.** Kernel rules
  (`kernel.admin`, `packages.install`, etc.) are registered with
  `test_contract IS NULL` by `bootstrap_groups`; RBAC test skips them.
  See OQ5 for the visibility decision.
- **Historical RBAC test results beyond the latest.**
  `last_rbac_test_result` overwrites; there is no ledger. If the admin
  wants ledgered results, the audit stream has them. Per-rule
  history is 0.10.x polish at the earliest.

---

## State Machine

0.4.5 ended at `ACTIVE` (or `DISABLED` / `SUPERSEDED` / `UNINSTALLING`
on non-install transitions). 0.4.7 adds one new `ACTIVE` ↔
`ACTIVE_FLAGGED` cycle, plus a RBAC test entry from three trigger sites:

```
  ┌──────────┐      RBAC test all-pass         ┌──────────┐
  │          │ ◀────────────────────────────▶│          │
  │  ACTIVE  │                               │ (ACTIVE, │
  │          │      RBAC test any-fail         │  RBAC test │
  │          │                               │  passed) │
  └─────┬────┘                               └──────────┘
        │                                            ▲
        │   RBAC test any-fail                         │
        ▼                                            │
  ┌──────────────┐      RBAC test re-run all-pass     │
  │ ACTIVE_      │ ────────────────────────────────┘
  │ FLAGGED      │
  └──────┬───────┘
         │   disable / enable / uninstall / upgrade: all unchanged —
         │   ACTIVE_FLAGGED is treated identically to ACTIVE at
         │   every non-Phase-B transition.
         ▼
       (0.4.5 transitions)
```

**Trigger sites — all post-COMMIT:**

| Site | Enclosing entry | Firing point | Condition |
|------|-----------------|--------------|-----------|
| T-Install | `install_package` | Immediately after `emit_installed_audit` | Always; the happy-path end-of-install |
| T-Enable  | `enable_package`  | Immediately after `emit_transition_audit("packages.enabled")` | Always; even if previous RBAC test passed |
| T-Upgrade | `upgrade_package` | Immediately after `emit_upgrade_audit("packages.upgrade_completed")` | Fires on the new-version row; the old-version `SUPERSEDED` row does not re-Phase-B |

**Non-triggers (explicit):** disable, uninstall, re-materialisation of
routes/caps during enable (RBAC test fires at the end of enable, not
mid-enable), `reconcile_in_flight_installs` replaying a completed
install (unless `last_rbac_test_run_at IS NULL` AND the install is fresh
— see Recovery).

**RBAC test internal state progression** (observable in logs, not
durably persisted per-step):

```
  START run_id=<uuid>
     │
     ▼
  ACQUIRE advisory lock (hashtextextended 'plinth.packages.<name>')
     │
     ▼
  CREATE synthetic group __rbac_test_<run_id>
     │
     ▼
  FOR each rule in package (order: rbac_rules.id ASC for determinism)
     │
     ├── IF test_contract IS NULL → skip; append to report.skipped
     │
     ├── CREATE __test_allowed_<run_id> if not yet created this run
     │                (single pair per run; rule-level grant
     │                 toggles via group_rules membership, not
     │                 user recreation)
     │   GRANT rule to __rbac_test_<run_id>
     │   GRANT __rbac_test_<run_id> to __test_allowed_<run_id>
     │
     ├── CREATE __test_denied_<run_id> if not yet created this run
     │
     ├── INVOKE assert_deny.call as __test_denied_<run_id>
     │   expect CapabilityError::PERMISSION_DENIED
     │   append to report.passed / report.failed
     │
     ├── INVOKE assert_allow.call as __test_allowed_<run_id>
     │   expect success OR any non-permission CapabilityError
     │   append to report.passed / report.failed
     │
     └── REVOKE rule from __rbac_test_<run_id>
                  (rule-level isolation between rules within one run)
     │
     ▼
  DESTROY __test_denied_<run_id>, __test_allowed_<run_id>,
          __rbac_test_<run_id> (idempotent)
     │
     ▼
  UPDATE plinth.packages SET last_rbac_test_run_at=NOW(),
                             last_rbac_test_result=<report JSONB>,
                             state = CASE WHEN report.failed empty
                                          THEN state  -- keep ACTIVE / ACTIVE_FLAGGED
                                          ELSE 'ACTIVE_FLAGGED' END,
                             state = CASE WHEN report.failed empty
                                               AND state = 'ACTIVE_FLAGGED'
                                          THEN 'ACTIVE'
                                          ELSE state END
         WHERE id = $1
     │
     ▼
  AUDIT packages.rbac_test_passed | packages.rbac_test_failed
     │
     ▼
  RELEASE advisory lock
     │
     ▼
  END
```

The state-update SQL is one statement with two CASE branches to keep
the transition atomic. Phrased as pseudocode above for clarity; the
real statement is in §Data Model.

**Rule-level isolation inside a run.** The same `__test_allowed_{run_id}`
user is reused across every rule; between rules, the single grant on
`__rbac_test_{run_id}` is revoked + re-issued for the next rule. This
keeps user creation to O(1) per run while still enforcing that each
`assert_allow` invocation only has the one rule under test in
`effective_rules`. The alternative — one user per rule — burns
O(rules) INSERTs/DELETEs per run and has no correctness advantage.

---

## Implementation history (file + symbol renames)

The implementation file pair shipped in v0.4.7 as
`src/kernel/packages/rbac_test_runner.{hpp,cpp}` (renamed from
`rbac_test_runner.{hpp,cpp}`), paired with the rename of `phase_a.{hpp,cpp}` to
`rule_validator.{hpp,cpp}`. Rationale: describes what the TU does
(run RBAC tests / validate rules) rather than which DESIGN phase it
implements.

The v0.5.0.1 rename session closed the loop on the public surface —
types, namespaces, methods, audit kinds, schema columns, and Catch2
tags were all brought into alignment with the file names under the
naming convention (classes=nouns, methods=verbs, fields=adjectives).
The body of this ICD reflects post-rename names. Pre-rename names
(`PhaseBReport`, `PhaseBFailure`, `PhaseBRule`, `run_phase_b`,
`schedule_phase_b_detached`, `emit_phase_b_audit`,
`fetch_extension_rules_for_phase_b`, `phase_b_report_from_json`,
`validate_phase_a`, namespace `plinth::packages::phase_b`, Catch2
tags `[phase_b]` / `[phase_b_report]` / `[rbac_phase_a]`, schema
columns `last_phase_b_run_at` / `last_phase_b_result`, audit events
`packages.phase_b_passed` / `_failed`) are preserved in the v0.4.7
release commit (`4a86e31`). Documented in `docs/CHANGELOG.md` v0.5.0.1
and `RE-EVAL-0.4.x-arc-closeout.md §2.1`.

---

## Scope Summary

| Item | Status |
|------|--------|
| RBAC test execution engine | **new** (src/kernel/packages/rbac_test_runner.{hpp,cpp}) |
| Ephemeral-user factory | **new** (src/kernel/rbac/ephemeral_user.{hpp,cpp}) |
| `plinth.users.is_test_user` column | **new** (schema.sql) |
| User-listing query filter | **modify** (auth/groups handler queries) |
| Trigger in `install_package` | **modify** (one call site) |
| Trigger in `enable_package` | **modify** (one call site) |
| Trigger in `upgrade_package` | **modify** (one call site) |
| Trigger in `reconcile_in_flight_installs` | **modify** (recovery branch) |
| `plinth test rbac` subcommand | **new** (main.cpp) |
| `packages.rbac_test_passed` / `_failed` audit kinds | **new** emitters |
| Fetch-rules helper in `rule_registrar.*` | **new** |
| RBAC test result JSON shape | **new** |
| `ACTIVE_FLAGGED` as a first-class write state | **new writers; enum + CHECK already reserved** |
| `last_rbac_test_run_at` / `last_rbac_test_result` columns | **first writers; columns already in schema** |
| `plinth.rbac_rules.test_contract` | **first reader; column already in schema** |

---

## Contracts

### RBAC test driver

```cpp
namespace plinth::packages::rbac_test {

// Per-rule outcome — observable in RbacTestReport.
struct RuleOutcome {
    std::string rule;                      // "notes.edit"
    std::string clause;                    // "assert_deny" | "assert_allow"
    std::string expected;                  // "permission_denied" | "success"
    std::string actual;                    // "permission_denied" | "success" |
                                           // "<other CapabilityError variant>"
    bool        passed;                    // expected == actual (with assert_allow
                                           // matching success OR any non-permission)
    nlohmann::json detail;                 // CapabilityCall args + raw error code
};

// Aggregate RBAC test report. Persisted as JSONB in
// plinth.packages.last_rbac_test_result.
struct RbacTestReport {
    std::string               run_id;       // UUIDv4
    std::string               package_name;
    std::string               package_version;
    std::chrono::system_clock::time_point started_at;
    std::chrono::milliseconds duration;
    std::vector<RuleOutcome>  passed;
    std::vector<RuleOutcome>  failed;
    std::vector<std::string>  skipped;      // rules with test_contract IS NULL
    bool                      overall_passed() const { return failed.empty(); }
};

// Operational-error shape — distinct from per-rule failure. A
// `RbacTestFailure` means the run could not complete (DB unavailable,
// package in a non-ACTIVE state, crash mid-setup); per-rule failures
// are inside `RbacTestReport.failed`.
struct RbacTestFailure {
    std::string kind;     // "package_not_found", "invalid_state",
                          // "db_error", "setup_failed", "cleanup_failed"
    std::string message;
    nlohmann::json detail;
};

// Primary synchronous entry. Acquires the per-name advisory lock,
// creates the ephemeral users, runs every rule, tears down, writes
// last_rbac_test_*, emits audit. Returns the report on success (all
// pass or any-fail both come back as Ok — distinguish via
// report.overall_passed()). Returns RbacTestFailure only for operational
// failures that prevent completion.
auto run_rbac_test(std::string_view package_id,
                 const InstallerContext& ctx)
    -> std::expected<RbacTestReport, RbacTestFailure>;

// Post-install / post-enable / post-upgrade dispatch. Spawns a
// detached std::jthread that calls run_rbac_test and swallows
// RbacTestFailure (logs at error level; the audit row will be absent).
// Returns immediately. The thread holds its own libpq connection —
// it does not share the caller's conn.
//
// Safety: the jthread is detached, so its destructor does not block
// shutdown. drogon::app().quit() does not join; the PG connection
// teardown is defensive (try/catch). An orphaned run on process exit
// leaves `last_rbac_test_run_at IS NULL` — reconcile_in_flight_installs
// picks it up on next boot via the fresh-install window.
auto schedule_rbac_test_detached(std::string_view package_id,
                               const InstallerContext& ctx) -> void;

}  // namespace plinth::packages::rbac_test
```

### Ephemeral-user factory

```cpp
namespace plinth::rbac {

// One RBAC test run creates exactly one of each. run_id is UUIDv4
// (matching the RbacTestReport.run_id). Usernames are derived
// deterministically to make reconciler sweeps trivial:
//     __test_denied_<run_id>
//     __test_allowed_<run_id>
//
// The underscore prefix is the marker — combined with is_test_user=true,
// every set-returning admin/user-listing query excludes them.
struct RunUserPair {
    std::string run_id;
    std::string denied_user_id;
    std::string denied_username;
    std::string allowed_user_id;
    std::string allowed_username;
    std::string allowed_group_id;        // __rbac_test_<run_id>
    std::string allowed_group_name;
};

// Creates the denied user, the allowed user, and the synthetic group.
// `everyone` membership is assigned via the existing
// `plinth.group_members` INSERT. The synthetic group is NOT
// `built_in` and its created_at is NOW(). Preconditions: caller has
// acquired the per-name advisory lock; caller owns the transaction.
// Returns the pair on success, a PG error message on failure. On
// failure the caller MUST call destroy_run_users to back out any
// partial inserts.
auto create_run_users(std::string_view run_id,
                      PGconn& conn) -> std::expected<RunUserPair, std::string>;

// Issue / revoke the single-rule grant on the synthetic group. Called
// at the top of every rule iteration; the revoke is called at the
// bottom so the allowed user has effective_rules == {} between rules.
auto grant_rule_to_run_group(std::string_view run_id,
                             std::string_view rule,
                             PGconn& conn) -> std::expected<void, std::string>;
auto revoke_rule_from_run_group(std::string_view run_id,
                                std::string_view rule,
                                PGconn& conn) -> std::expected<void, std::string>;

// Idempotent teardown. DELETE group_rules → group_members → users →
// groups in dependency order. Safe to call on a run that got halfway
// or all the way; missing rows are no-ops.
auto destroy_run_users(std::string_view run_id,
                       PGconn& conn) -> std::expected<void, std::string>;

// Reconciler sweep. For every user with is_test_user=true AND
// created_at < older_than, destroy it and its associated group. The
// `__rbac_test_<run_id>` naming makes the group-user correspondence
// trivial. Returns the number of runs cleaned up.
auto cleanup_orphaned_test_users(std::chrono::system_clock::time_point older_than,
                                 PGconn& conn)
    -> std::expected<std::size_t, std::string>;

// Synthesise a UserContext for RBAC test dispatch — never authenticates
// over HTTP; the context is handed directly to call_capability_async.
// `effective_rules` is computed from the synthetic group's current
// grants at call time (i.e., the single rule under test for assert_allow,
// empty for assert_deny).
auto build_test_user_context(std::string_view username,
                             std::string_view user_id,
                             std::vector<std::string> effective_rules)
    -> plinth::capabilities::UserContext;

}  // namespace plinth::rbac
```

### Fetch helper

```cpp
namespace plinth::rbac {

struct RbacTestRule {
    std::string    rule;
    std::string    extension_name;
    nlohmann::json test_contract;          // parsed; null JSON for missing
};

// Load every rbac_rules row for the extension, including NULL
// test_contract rows. Consumer is RBAC test (the skip-vs-run decision
// happens in run_rbac_test, not in this helper).
auto fetch_extension_rules_for_rbac_test(std::string_view extension_name,
                                       PGconn& conn)
    -> std::expected<std::vector<RbacTestRule>, std::string>;

}  // namespace plinth::rbac
```

### Dispatch adapter

RBAC test does NOT introduce a new capability-call entry point. It
consumes the existing `call_capability_async` (coroutine form) from
`src/kernel/capabilities/resolution.hpp` and awaits it on the RBAC test
thread via the same `drogon::app().getLoop()->queueInLoop` +
promise/future bridge pattern used by the CLI validator. Concretely:

```cpp
// inside run_rbac_test
auto parsed = plinth::capabilities::parse_signature(call_string);
if (!parsed) return rule_outcome_failed(/* parse_error */);

plinth::capabilities::CapabilityCall call{
    .signature  = std::string{call_string},
    .args       = args_from_contract,      // test_contract[clause]["args"] if present
    .call_depth = 0,
};

auto prom = std::promise<plinth::capabilities::ResolveResult>{};
auto fut  = prom.get_future();
drogon::app().getLoop()->queueInLoop(
    [&, call]() mutable -> drogon::AsyncTask {
        auto out = co_await plinth::capabilities::call_capability_async(call, ctx);
        prom.set_value(std::move(out));
    });
auto out = fut.get();  // blocks the RBAC test thread, not the event loop
```

For `assert_deny`: the outcome is PASS iff `!out && out.error() ==
PERMISSION_DENIED`. Any other `CapabilityError` variant or a successful
dispatch is a FAIL. For `assert_allow`: the outcome is PASS iff `out`
is engaged OR (`!out` AND `out.error() != PERMISSION_DENIED`). A
`PERMISSION_DENIED` on `assert_allow` is a FAIL. This is the full
semantic contract — there is no third "expected some other error"
case.

**Args handling.** `test_contract.assert_deny.call` is a signature
string like `"notes:1:edit"`. DESIGN §0.4.7's example shows the call as
a bare signature; ICD-0.4.6 §Schema §rbac.json also treats `call` as
just the signature string (not a signature + args envelope). So Phase
B invokes with `args = {}` (empty Json::Value) by default. If the
`test_contract` shape ever gains an `args` field, the fetch helper
passes it through as an opaque `Json::Value`; the parser for that
shape is 0.4.7's addition to `parse_rbac_manifest`. See OQ-? under
Open Questions.

### Audit surface

Two new kinds, both emitted via `plinth::log::audit_sync(ctx.db, kind,
detail)`:

**`packages.rbac_test_passed`**

```json
{
  "package_id": "uuid",
  "package_name": "notes",
  "package_version": "1.2.3",
  "run_id": "uuid",
  "rule_count_passed": 7,
  "rule_count_skipped": 2,
  "duration_ms": 134,
  "triggered_by": "install" | "enable" | "upgrade" | "cli"
}
```

**`packages.rbac_test_failed`**

```json
{
  "package_id": "uuid",
  "package_name": "notes",
  "package_version": "1.2.3",
  "run_id": "uuid",
  "rule_count_passed": 5,
  "rule_count_failed": 2,
  "rule_count_skipped": 0,
  "duration_ms": 142,
  "triggered_by": "install",
  "failures": [
    {
      "rule": "notes.edit",
      "clause": "assert_deny",
      "expected": "permission_denied",
      "actual": "success"
    },
    {
      "rule": "notes.delete",
      "clause": "assert_allow",
      "expected": "success",
      "actual": "permission_denied"
    }
  ]
}
```

No rate limiting at the RBAC test layer — one package-install emits at
most one `packages.rbac_test_*` event. A package with N failed rules
emits one aggregate event, not N.

### CLI surface

```
plinth test rbac <extension> [--json] [--run-id <uuid>]
```

- `<extension>`: package name, matching `plinth.packages.name WHERE
  state IN ('ACTIVE', 'ACTIVE_FLAGGED')`. If the match is ambiguous
  (should not be, by the `uniq_packages_name_active` index; defensive
  coding only), the CLI exits 2 with message `multiple active
  packages for name`.
- `--json`: emit the `RbacTestReport` as JSON on stdout; suppress the
  human-readable summary. Reuses the 0.4.0 `--json` convention.
- `--run-id <uuid>`: force a specific run_id (for test harnesses that
  need deterministic ephemeral-user names). Defaults to UUIDv4.

Exit codes:
- `0` — all rules passed; if the package was `ACTIVE_FLAGGED` on
  entry, it is now `ACTIVE`.
- `1` — any rule failed; package is `ACTIVE_FLAGGED`.
- `2` — operational error (package not found, not in active state,
  DB unreachable, lock acquisition timeout). `stderr` carries the
  message.

The CLI path shares `run_rbac_test` with the post-install trigger — one
code path, two entry points. `triggered_by` on the audit event is
`"cli"` when invoked via the subcommand.

---

## Data Model

### Schema edits

```sql
-- migrations/schema.sql, plinth.users (line ~25 area)
ALTER TABLE plinth.users
    ADD COLUMN is_test_user BOOLEAN NOT NULL DEFAULT false;

-- Index (optional but low-cost): test users are always a small minority
-- and the existing username UNIQUE index handles the common lookup.
-- The reconciler's sweep uses is_test_user directly:
CREATE INDEX idx_users_is_test_user
    ON plinth.users(id)
    WHERE is_test_user = true;
```

**No edit required** on `plinth.packages` — `state` CHECK already
includes `'ACTIVE_FLAGGED'`, and `last_rbac_test_run_at` /
`last_rbac_test_result` are already present (all added by 0.4.4 /
0.4.5). 0.4.7 is their first writer.

**No edit required** on `plinth.rbac_rules` — `test_contract JSONB`
was added by 0.4.6. 0.4.7 is its first reader.

### The RBAC test state-update SQL

```sql
UPDATE plinth.packages
   SET last_rbac_test_run_at = NOW(),
       last_rbac_test_result = $2::jsonb,
       state = CASE
                   WHEN $3::bool THEN                    -- report.overall_passed()
                       CASE state
                           WHEN 'ACTIVE_FLAGGED' THEN 'ACTIVE'
                           ELSE state
                       END
                   ELSE 'ACTIVE_FLAGGED'
               END
 WHERE id = $1::uuid
   AND state IN ('ACTIVE', 'ACTIVE_FLAGGED')              -- defensive; never flip
                                                           -- an UNINSTALLING row
 RETURNING state;
```

The `RETURNING state` gives the caller the post-update state for the
audit `detail` payload without a second `SELECT`.

### Ephemeral-user row shape

```sql
-- denied user
INSERT INTO plinth.users (id, username, password_hash, is_test_user)
VALUES (gen_random_uuid(),
        '__test_denied_' || $1,                  -- $1 = run_id
        '$argon2id$v=19$m=65536,t=3,p=4$placeholder$placeholder',
        true)
RETURNING id;

-- allowed user (same shape, different prefix)
INSERT INTO plinth.users (id, username, password_hash, is_test_user)
VALUES (gen_random_uuid(),
        '__test_allowed_' || $1,
        '$argon2id$v=19$m=65536,t=3,p=4$placeholder$placeholder',
        true)
RETURNING id;

-- synthetic group
INSERT INTO plinth.groups (id, name, description, built_in)
VALUES (gen_random_uuid(),
        '__rbac_test_' || $1,
        'RBAC test test group; auto-cleaned',
        false)
RETURNING id;

-- everyone membership for both
INSERT INTO plinth.group_members (group_id, user_id)
SELECT g.id, u.id
FROM plinth.groups g, plinth.users u
WHERE g.name = 'everyone' AND u.username IN (
    '__test_denied_'  || $1,
    '__test_allowed_' || $1
);

-- allowed user → synthetic group
INSERT INTO plinth.group_members (group_id, user_id)
SELECT g.id, u.id
FROM plinth.groups g, plinth.users u
WHERE g.name     = '__rbac_test_' || $1
  AND u.username = '__test_allowed_' || $1;
```

The `password_hash` is a syntactically-valid argon2id token that no
code path attempts to verify. The placeholder is a constant string
baked into `ephemeral_user.cpp` — documented at the call site so a
future reader does not mistake it for a credential.

### User-listing query filter

Queries that return a *set* of users gain `WHERE is_test_user = false`.
Queries that target a specific username or id do not (the match
already filters). Survey of current surfaces (all in
`src/kernel/auth/handlers.cpp` and `src/kernel/groups/handlers.cpp`):

| Query | Filter needed? | Rationale |
|-------|---------------|-----------|
| `auth/handlers.cpp:147` `SELECT COUNT(*)` for first-user seed | **yes** | Count-visible |
| `auth/handlers.cpp:237` `SELECT COUNT(*)` for first-user seed (second spot) | **yes** | Count-visible |
| `auth/handlers.cpp:277` `SELECT disabled_at WHERE username = $1` | no | Targeted |
| `auth/handlers.cpp:336` `SELECT id, password_hash, disabled_at WHERE username = $1` | no | Targeted |
| `groups/handlers.cpp:575` `SELECT id WHERE id = $1::uuid` | no | Targeted |

Two filter sites only — both in first-user bootstrap. The ICD author
is the source of truth on this list; the implementing session must
re-survey before landing, because drift between ICD and code is the
load-bearing risk here.

---

## Test Cases

Test prefix: **B.*** for pure unit (RBAC test reporting, user-factory
SQL round-trip, CLI arg parsing) and **PB.*** for PG-gated integration
(full RBAC test drive through `install_package` + assertion on
`last_rbac_test_result` + audit rows). Distinct from ICD-0.4.5 (D/U/E/X)
and ICD-0.4.6 (P).

| # | Type | Scenario | Fixture / Seed | Expected outcome |
|---|------|----------|----------------|------------------|
| B.01 | Happy — report shape | Build a `RbacTestReport` with 2 passed / 1 failed / 1 skipped, serialise to JSONB, re-parse | in-test | Field-by-field equality; `overall_passed() == false` |
| B.02 | Report — all-pass boundary | Build a report with 3 passed / 0 failed / 0 skipped | in-test | `overall_passed() == true` |
| B.03 | Report — only-skipped boundary | Build a report with 0 passed / 0 failed / 2 skipped | in-test | `overall_passed() == true` (empty failed) |
| B.04 | Ephemeral-user derivation | Invoke `create_run_users("abc")` and then `destroy_run_users("abc")` — direct on a PG fixture | pure-PG unit | Two users + one group inserted then deleted; `is_test_user = true` on both |
| B.05 | Rule-grant cycle | `grant_rule_to_run_group → revoke_rule_from_run_group → grant` is idempotent | pure-PG unit | Second grant succeeds (ON CONFLICT DO NOTHING or DELETE-first semantics — ICD pins via comment) |
| B.06 | Cleanup sweep | Seed 3 test users, call `cleanup_orphaned_test_users(now + 1h)` | pure-PG unit | All 3 deleted; return value == 3 |
| B.07 | Cleanup skips fresh | Seed 3 test users, call `cleanup_orphaned_test_users(now - 1h)` | pure-PG unit | 0 deleted |
| B.08 | CLI arg parsing | `plinth test rbac notes --json` | in-test argparse drive | Subcommand recognised; flag parsed |
| PB.01 | Happy — install fires RBAC test, all pass | Install `tests/fixtures/rbac_test_runner/happy-all-pass/` (3 rules, 2 with test_contract, 1 without) | fixture package | `state = 'ACTIVE'`; `last_rbac_test_result.failed` empty; `last_rbac_test_result.skipped` length 1; audit row `packages.rbac_test_passed` |
| PB.02 | Broken `assert_deny` — rule allows what it claims to deny | Install `tests/fixtures/rbac_test_runner/broken-assert-deny/` (rule grants capability to `everyone`) | fixture package | `state = 'ACTIVE_FLAGGED'`; `last_rbac_test_result.failed` length 1, clause `assert_deny`; audit row `packages.rbac_test_failed` |
| PB.03 | Broken `assert_allow` — rule does not grant | Install `tests/fixtures/rbac_test_runner/broken-assert-allow/` (rule's capability missing `rbac_rule` annotation) | fixture package | `state = 'ACTIVE_FLAGGED'`; `failed` length 1, clause `assert_allow` |
| PB.04 | Mixed pass/fail | Install `tests/fixtures/rbac_test_runner/mixed-pass-fail/` (3 rules: 1 all-pass, 1 broken deny, 1 broken allow) | fixture package | `state = 'ACTIVE_FLAGGED'`; `passed` length 1, `failed` length 2 |
| PB.05 | Package with no `test_contract` | Install `tests/fixtures/rbac_test_runner/no-test-contracts/` (3 rules, all with `test: absent`) | fixture package | `state = 'ACTIVE'`; `last_rbac_test_result.skipped` length 3; audit row `packages.rbac_test_passed` (empty `failures`) |
| PB.06 | `assert_allow` with non-permission error passes | Install `tests/fixtures/rbac_test_runner/assert-allow-side-effect/` (capability throws `invalid_argument` on test call; grant is correct) | fixture package | PASS — DESIGN §0.4.7 distinguishes permission error from any other error |
| PB.07 | Re-run via CLI clears flag | After PB.02, invoke `plinth test rbac <ext>` (requires fixture author to fix the grant; test seeds the fix via direct INSERT to group_rules) | built on PB.02 | Second run returns exit 0; `state = 'ACTIVE'`; second audit row `packages.rbac_test_passed` |
| PB.08 | Ephemeral user cleanup on success | Count `plinth.users WHERE is_test_user = true` before and after PB.01 | PB.01 extension | After = before (i.e., zero if fixture is the only test content) |
| PB.09 | Ephemeral user cleanup on failure | Same check after PB.02 | PB.02 extension | After = before — cleanup path runs regardless of RBAC test outcome |
| PB.10 | Crash-recovery user cleanup | Manually insert `__test_denied_<uuid>` + group with `created_at = NOW() - interval '2 hour'`; run `reconcile_in_flight_installs` | in-test seed | Row deleted; reconciler returns count >= 1 |
| PB.11 | Crash-recovery RBAC test trigger | Manually set `last_rbac_test_run_at = NULL` on a fresh (`installed_at > NOW() - 1h`) ACTIVE row; run `reconcile_in_flight_installs` | in-test seed | `last_rbac_test_run_at` set post-reconcile; either state remains ACTIVE or flips to ACTIVE_FLAGGED |
| PB.12 | Crash-recovery skips old | Same as PB.11 but `installed_at = NOW() - interval '2 hour'` | in-test seed | `last_rbac_test_run_at` remains NULL |
| PB.13 | Enable fires RBAC test | Install a passing package, disable it, enable it (after seeding `group_rules` so the rule is still granted) | existing 0.4.5 fixtures reused + rbac_test fixture | Enable emits a second `packages.rbac_test_passed` audit row |
| PB.14 | Upgrade fires RBAC test on new-version row | Install v1.0.0 (passing) then upgrade to v1.0.1 (broken deny) | two rbac_test fixtures | New row `ACTIVE_FLAGGED`; old `SUPERSEDED` row `last_rbac_test_result` untouched |
| PB.15 | Sequential run serialisation | Trigger two RBAC test runs for the same package back-to-back (CLI + post-install) | PB.01 extension | Second acquires advisory lock after first releases; no interleaving in the audit stream |

**Total new test cases:** 23. Split: 8 pure (`B.*` — no Drogon/PG
requirements beyond the in-process libpq for B.04-B.07), 15 PG-gated
(`PB.*`). Fixture count: 6 packages under `tests/fixtures/rbac_test_runner/`.

### CI wiring

- `src/kernel/packages/rbac_test_runner.{hpp,cpp}` — new.
- `src/kernel/rbac/ephemeral_user.{hpp,cpp}` — new.
- `src/kernel/rbac/rule_registrar.{hpp,cpp}` — modify (new
  `fetch_extension_rules_for_rbac_test`).
- `src/kernel/packages/install_lifecycle.cpp` — modify (three trigger
  sites + reconciler branch).
- `src/kernel/main.cpp` — modify (new subcommand).
- `src/kernel/auth/handlers.cpp` — modify (2 COUNT queries).
- `migrations/schema.sql` — ALTER + INDEX.
- `tests/kernel/packages/rbac_test_runner_test.cpp` — new (PB.*).
- `tests/kernel/rbac/ephemeral_user_test.cpp` — new (B.04-B.07).
- `tests/kernel/packages/rbac_test_report_test.cpp` — new (B.01-B.03).
- `tests/kernel/main/cli_test_rbac_test.cpp` — new (B.08).
- `tests/fixtures/rbac_test_runner/*` — new subtree, 6 fixture directories.
- `CMakeLists.txt` — no edit. `tests/kernel/**` glob picks them up;
  `[pg]` tag routes `rbac_test_runner_test.cpp` into `plinth_tests_pg`.

### Test count target

~23 Catch2 cases. Pre-0.4.7 baseline is 494 on main after v0.4.6;
expected post-ship total ≈ 517 (subject to `PLINTH_KERNEL_TESTS=ON`
PG gate on the 15 `PB.*` cases).

---

## Security

1. **`assert_allow` side effects are the extension's responsibility.**
   DESIGN §0.4.7 is load-bearing on this. An extension whose
   `assert_allow: email:1:send_welcome` fires a real welcome email
   during every RBAC test run — whether post-install, post-enable,
   post-upgrade, or on CLI re-run — has authored an extension that
   spams its users. The kernel does not attempt to mitigate. The
   extension guide (deferred to 0.10.7) documents the responsibility
   and provides patterns: use a test-scoped argument (`{to:
   '__test_sink@plinth.local'}`), a dedicated no-op capability for
   the `assert_allow.call`, or a capability argument that
   short-circuits on an ephemeral user id. 0.4.7 does nothing to
   enforce this — it is an extension-author social contract, not a
   kernel boundary.

2. **Ephemeral users cannot authenticate over HTTP.** Their
   `password_hash` is a syntactically-valid-but-meaningless
   placeholder. The `SessionFilter` path calls `argon2id_verify`
   against the stored hash; the placeholder never matches any
   attacker-supplied password. Even if the hash were known, the
   username `__test_denied_<uuid>` is not special-cased in the HTTP
   pipeline — it is a regular disabled-style account. The
   `is_test_user = true` marker plus username-prefix is the
   "excluded from all user-listing queries" guarantee of DESIGN
   §0.4.7. The session-issuance path does not gain a new filter;
   ephemeral users can technically create a session if an attacker
   both knows the hash placeholder AND issues a login — both of
   which are implausible. The belt-and-braces variant (a
   `disabled_at = NOW()` on every ephemeral user) is OQ-? — see
   Open Questions.

3. **Advisory lock starvation.** The per-name lock is held for the
   duration of RBAC test (~100-500 ms per rule × N rules). A malicious
   extension with 1000 rules could hold the lock for minutes, which
   would block other PATCH / DELETE / upgrade calls for that name.
   Mitigation: 0.4.6 Rule A.1 enforces rule-name regex (bounded
   cardinality per namespace in practice; but no hard cap on total
   rule count). Recommend: OQ4 — reuse the 5-second drain budget as
   a per-rule wall-clock cap; a rule whose call takes longer than 5
   s is recorded as `actual: "timeout"` and treated as a failure.
   This converts a slow/malicious extension into an `ACTIVE_FLAGGED`
   state instead of a DoS.

4. **Attacker with DB write access.** Out of scope — they have the
   keys to the castle. The `is_test_user = true` predicate is
   trivial to bypass. The audit trail survives a write (append-only
   by convention; the hard guarantee is 0.10.6's pass).

5. **Cross-extension rule injection.** Cannot happen — Rule A.5 (in
   0.4.6) already rejects cross-extension rule collisions at
   install time. RBAC test's `fetch_extension_rules_for_rbac_test`
   filter-by-extension_name is a defensive second layer.

6. **Audit volume.** RBAC test emits at most one audit row per package
   install / enable / upgrade / CLI invocation. An attacker who spams
   installs is rate-limited at the installer (0.4.4 has no explicit
   rate limit; the PG advisory lock serialises per-name and the
   HTTP RBAC gate requires `packages.install`). RBAC test does not
   amplify the attack surface.

---

## Entry / Exit

**Entry criteria:**

- `v0.4.6` merged on main (done — tag `v0.4.6`, merge commit
  `b126ebd`, 2026-04-21). `plinth.rbac_rules.test_contract` is live
  and populated by existing installs.
- `ICD-0.1.5-rbac-enforcement` path fully operational (done —
  `call_capability`'s RBAC check at step 3 has shipped since 0.2.4
  and is exercised by every install today).
- `call_capability_async` available (done — v0.3.3 added the
  coroutine wrapper at `src/kernel/capabilities/resolution.cpp:451`).
- `reconcile_in_flight_installs` body shipped and tested (done —
  v0.4.4 Slice B + v0.4.5 Slice A extensions; 0.4.7 adds two
  additional branches).
- The 0.4.5.1 grouped-test model is live (done — PR #57, commit
  `3d887a6`); `[pg]` tag routes PB.* cases into `plinth_tests_pg`
  without CMake edits.

**Exit criteria:**

- All 23 test cases pass under the 0.4.5.1 grouped-test model.
  Default build green; sanitizer build green.
- `run-clang-tidy-20` zero findings on new translation units
  (`rbac_test_runner.{hpp,cpp}`, `ephemeral_user.{hpp,cpp}`) and modified TUs
  (`install_lifecycle.cpp`, `main.cpp`, `auth/handlers.cpp`,
  `rule_registrar.{hpp,cpp}`).
- A test extension with one pass-rule and one deliberately-broken
  deny-rule installs; the resulting `last_rbac_test_result` has
  `failed` length 1 and `state` is `ACTIVE_FLAGGED`. `plinth test
  rbac` re-runs and returns exit 1. After manually correcting the
  grant, a third `plinth test rbac` run returns exit 0 and
  `state = 'ACTIVE'`. This is the DESIGN §0.4.7 exit criterion
  verbatim, restated as an executable demo path.
- Ephemeral users are cleaned up on both success and failure
  paths. `SELECT COUNT(*) FROM plinth.users WHERE is_test_user =
  true` equals zero at quiescence in the full test run.
- `CHANGELOG.md` 0.4.7 entry describes the three new TUs, one
  schema edit, and two new audit kinds. Deviations (if any) listed
  per the 0.4.6 precedent.
- `docs/ROADMAP.md` 0.4.7 entry is removed (arc close-out). The
  `RE-EVAL following 0.4.7 (0.4.x arc closeout)` item becomes the
  next scheduled work.

---

## Open Questions

Five questions intentionally surfaced for architect sign-off before
implementation begins. Each carries a proposed resolution; the
implementing session may deviate only with a CHANGELOG note.

**OQ1 — Does `ACTIVE_FLAGGED` block `PATCH /api/packages/{id}
action=disable`?** The natural read of DESIGN §0.4.7 is "the flag is
a warning, not a disable" — implying disable still works normally.
But an admin investigating a flagged package may want to disable-and-
investigate; forcing them to fix the flag first would be hostile.
**Proposed: no, `ACTIVE_FLAGGED` does not block any 0.4.5 transition.**
`disable_package` / `enable_package` / `uninstall_package` /
`upgrade_package` all treat `ACTIVE_FLAGGED` identically to `ACTIVE`
(the predicates already list both). Enable re-runs RBAC test
automatically; upgrade re-runs on the new-version row. Uninstall
carries the flag away entirely. The only difference from `ACTIVE` is
the admin-visible warning.

**OQ2 — `run_id` lifetime.** Two options: (a) one `run_id` per
package lifetime, written to `plinth.packages.rbac_test_run_id` and
reused across every RBAC test run; (b) one `run_id` per execution, a
fresh UUIDv4 each time. **Proposed: per-execution (b).** Rationale:
the audit stream is the ledger of results; each audit row needs a
unique identifier to correlate with ephemeral-user cleanup debugging.
A per-lifetime id makes it impossible to tell two runs apart in the
audit log without reading the timestamp — noisy. The schema cost is
zero (run_id lives in `last_rbac_test_result` JSONB, not in a column).

**OQ3 — Concurrent re-run serialisation.** The per-name advisory lock
already serialises installs + RBAC test. A CLI re-run acquires the
same lock; if an install is mid-flight (RBAC test scheduled but not
yet started), the re-run waits. Alternative: a separate
`rbac_test_lock_<name>` key so install and RBAC test can overlap.
**Proposed: reuse the existing install lock.** Rationale: RBAC test's
whole purpose is to observe steady-state handlers after install
COMMIT — running in parallel with another install-in-flight on the
same name would race the `plinth.capabilities` cache refresh. The
single lock is a correctness boundary, not an incidental choice.
Different packages proceed in parallel (different lock keys).

**OQ4 — Per-rule wall-clock budget.** DESIGN §0.4.7 is silent on
timeouts. A malicious or buggy capability that blocks forever would
hold the advisory lock forever. **Proposed: reuse
`InstallerContext::upgrade_drain_timeout_ms{5000}` as the per-rule
timeout.** Rationale: 5 s is already the project's "this should have
finished by now" threshold for the lifecycle layer; reusing the knob
keeps the surface small. On timeout, the rule outcome is
`actual: "timeout"` and counts as a failure. A dedicated
`rbac_test_per_rule_timeout_ms` config knob is a 0.10.x polish if the
5 s default turns out to be wrong.

**OQ5 — `test_contract IS NULL` rules: skipped-visible or
silently-ignored?** Two options: (a) report them in
`RbacTestReport.skipped` and in the audit event (`rule_count_skipped`);
(b) silently drop them from the report. **Proposed: report (a).**
Rationale: the admin audit stream is the "is my RBAC contract being
enforced" surface. A package that declares 10 rules but only tests 3
deserves visibility into the 7 untested rules. The audit cost is one
integer; the visibility gain is load-bearing for security review.
Kernel rules (`kernel.admin`, etc.) all have `test_contract IS NULL`
— the skipped list doubles as a "rules registered but not
self-testing" audit surface.

**Additional notes-to-architect that aren't true OQs:**

- **OQ-N1 — Per-user-vs-per-rule ephemeral user.** The scope §State
  Machine section chose one user pair per run, with grant toggling
  between rules. The alternative (one user pair per rule) is
  rejected for O(rules) insert/delete overhead with no correctness
  advantage. Documented here for the architect's record; not a true
  question.
- **OQ-N2 — Sequential vs parallel rule invocation.** Chose
  sequential; DESIGN §0.4.7 does not mandate parallel, and
  `std::expected` aggregation is trivial sequentially. Documented;
  not a true question.
- **OQ-N3 — Disabled-at on ephemeral users.** See §Security Constraint 2.
  Proposed: do not set `disabled_at`. Rationale: `is_test_user = true`
  plus username prefix plus placeholder hash is sufficient defense.
  `disabled_at` adds a column write + a teardown path without
  strengthening the threat model. If a belt-and-braces admin wants
  it, OQ-style answer lives in a post-0.4.7 hardening pass.
- **OQ-N4 — `test_contract.call` args field.** DESIGN §0.4.7's
  example shows `call` as a bare signature. ICD-0.4.6 treats it the
  same way. The `args: Json::Value` field in `CapabilityCall` defaults
  to empty. If a real extension needs `args` for a deny test (e.g.,
  `notes:1:edit('<note-id>')`), the `test_contract` shape grows an
  optional `args` field; 0.4.6's parser is extensible (the `test`
  object is stored verbatim so extensions don't wait on a kernel
  release). Not a 0.4.7 question.

---

## Deviations to Watch (none proposed; list is informational)

- **Detached `std::jthread` vs `drogon::AsyncTask` for the scheduler.**
  Chose `std::jthread` to keep RBAC test off the Drogon event loop (the
  libpq-style SQL wants its own connection; detached from the loop is
  simpler than a coroutine thread pool). An implementing session that
  prefers a coroutine-shaped scheduler may substitute — audit the
  shutdown behavior: `drogon::app().quit()` must not block on pending
  RBAC test runs, and the detached thread MUST NOT hold a `DbClient*`
  singleton that the 0.4.5 `af8bbdd` fix scopes to a DbClient-ready
  window.
- **`call_capability_async` via `queueInLoop` + promise bridge vs
  direct synchronous `call_capability`.** Chose the async form for
  uniformity with 0.3.3+ callers and so Tier 3 (0.8.x) sidecar calls
  transparently Work Later. A synchronous alternative is possible but
  the coroutine form is the project's invariant going forward.
- **Ephemeral-user password_hash as a hardcoded placeholder.**
  Documented at the call site. The hash is a syntactically-valid
  argon2id token that verifies against no password.
- **No parallel rule dispatch.** Sequential. See OQ-N2.
- **No dedicated RBAC test lock.** Reuses install lock. See OQ3.
- **Audit emitted post-state-update.** If the audit emission fails
  (DB gone), the state is still correct — rediscover on next boot via
  `reconcile_in_flight_installs`. The alternative (audit first, then
  state) would leave an audit row with no corresponding state
  transition if the UPDATE fails — harder to reconcile.

---

## Notes for the Implementing Session

- Read `project_next_session_lh0.md` before writing tests. The async-
  bridge flake family was confirmed as test-harness-only (LH-0.1); do
  not chase `free_zero_refcount` or `bad_weak_ptr` as a RBAC test issue
  if it surfaces. It is subprocess-lifecycle-specific and 0.4.5.1's
  grouped-test model is the current containment.
- The trigger-site edits in `install_lifecycle.cpp` must be post-
  COMMIT. A common failure mode is firing RBAC test before the outer
  transaction commits, which means the package state observable to
  RBAC test is still `ACTIVATING` (not `ACTIVE`) and the capabilities
  aren't in the Tier 2 cache. Fire after `emit_installed_audit` —
  that function's emission already guarantees the state commit is
  durable.
- The `InstallerContext` passed to `schedule_rbac_test_detached` MUST
  be a value copy (the detached thread outlives the caller's stack).
  The context carries `Config::Database` + paths + timeouts; the copy
  is cheap and the explicit copy makes the lifetime contract obvious.
- When surveying the user-listing query sites in §Data Model, ignore
  tests that seed their own users — the `is_test_user = true`
  predicate is production-only. `auth_integration_test.cpp:201` and
  `:300` in particular target specific rows and do not need the
  filter.
- The CLI subcommand lives in `src/kernel/main.cpp`'s subparser
  registry around line 166-167; follow the 0.4.0 `run_validate`
  pattern (argparse subcommand + a `run_*` function in the same TU).
  Keep `main.cpp` thin per `feedback_main_size.md` — push every
  non-argparse line into `rbac_test_runner.cpp`.

---

**This document is the contract for 0.4.7.** It does not extend
`DESIGN-packages-v04x.md §0.4.7`; it turns the DESIGN's execution-
model bullets into a signed-off test matrix, a state machine, a
storage/audit shape, and a CLI surface. Any structural deviation
requires a new architecture session and a revision of DESIGN-packages-v04x.md.
