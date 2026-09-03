# Re-evaluation — RE-EVAL following 0.4.4

**Date:** 2026-04-20
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3) — cadence re-eval, not arc-closeout
**Trigger:** Scheduled ROADMAP item `RE-EVAL following 0.4.4   [rewrite session]` — the cadence re-eval that blocks all further 0.4.x code work per ROADMAP preamble ("No further code milestone begins until the preceding re-eval item completes").
**Cadence position:** Fifth scheduled cadence re-eval. Cadence is `every 4 code milestones` per ROADMAP preamble; 3-part milestones since the last cadence re-eval (`RE-EVAL following 0.4.0`, 2026-04-19, `RE-EVAL-0.4.x.md`) are: **0.4.1** (GlassWorm), **0.4.2** (cross-file validation), **0.4.3** (migrations library), **0.4.4** (install lifecycle, slices A+B). Formal cadence is **4/4** at 0.4.4.
**Scope:** Code-aware gap analysis across the full 0.4.1–0.4.4 window. Load-bearing external signal from the architect (2026-04-20): *"schedule [the WS teardown test-strategy redesign] sooner than later"* — this re-eval scopes a dedicated follow-up (§2.2).

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index-only scan).
- `docs/architecture/05-extensions.md §1 Package Structure; §1.1 Cross-File Manifest Validation; §3 QuickJS Runtime + §3.1 Runtime Limits` — normative source for 0.4.1–0.4.4's filesystem + runtime rules.
- `docs/architecture/01-identity.md §2` — normative source for `rbac.json` rule shape referenced by cross-file CF1/CF2 (0.4.2).
- `docs/architecture/03-data.md §2` — schema-freeze posture (relevant to the 0.4.4 `plinth.packages` + `plinth.panels` additions landing pre-freeze).

### ICDs
- `docs/icd/ICD-0.4.1-glassworm-defense.md` — 0.4.1's contract; implemented in this window.
- `docs/icd/ICD-0.4.2-cross-file-manifest-validation.md` — 0.4.2's contract.
- `docs/icd/ICD-0.4.3-extension-schema-creation-and-migration.md` — 0.4.3's contract.
- `docs/icd/ICD-0.4.4-package-install-lifecycle.md` — 0.4.4's contract.
- `docs/icd/ICD-0.4.0-package-structure-validation.md` + `ICD-0.4.1-manifest-parsing.md` — consulted for the cross-file layering relationship with 0.4.2.

### Code
Spot-verified declarations against ICD §Library Surface / §HTTP Surface text for every shipped TU in the window:

- `src/kernel/security/unicode_scanner.{hpp,cpp}` — `scan_for_invisible_unicode` free function + `UnicodeFinding` / `UnicodeScanResult` / `UnicodeScanConfig` structs match `ICD-0.4.1 §Scanner Primitive` verbatim.
- `src/kernel/js/eval_guard.{hpp,cpp}` — `pre_eval_scan` exported; called from `eval.cpp:282`, `runtime_pool.cpp:764`, `run_on_context.cpp:819` (three `JS_Eval` sites enumerated in ICD §Layer 2).
- `src/kernel/js/eval.hpp:45` — `EvalErrorKind::UNICODE_SMUGGLE_DETECTED` present.
- `src/kernel/packages/validator.cpp` — `run_unicode_scan_pass` inserted between `run_json_parse` and `run_handler_files` (lines 736–738).
- `src/kernel/packages/cross_file_validator.{hpp,cpp}` — `ParsedPackage`, `run_cross_file_validation`, `run_runtime_state_validation` all public; CF1..CF7 + CFW1..CFW4 rule IDs all present.
- `src/kernel/packages/validator.hpp:25,29` — `Phase` enum + `Phase phase` field on `ValidationMessage`.
- `src/kernel/main.cpp:75,77,149,153` — `--structure-only` + `--against-running-kernel` CLI flags.
- `src/kernel/packages/migrations.hpp:38–49` — `run_migrations(name, package_root, admin_conn)` and `drop_schema_and_migrations(name, admin_conn)` signatures match ICD §Library Surface.
- `src/kernel/packages/migration_error.hpp:18–28` — `MigrationError` enum has 9 kinds (ICD's 8 + `DROP_FAILED`; §4 row covers this).
- `tests/fixtures/migration_packages/` — 15 fixtures, matches ICD §Test Cases.
- `src/kernel/packages/install_lifecycle.hpp:84–106` — `install_package`, `install_shell_if_needed`, `reconcile_in_flight_installs` all public.
- `migrations/schema.sql:119,157,146,150` — `plinth.packages` + `plinth.panels` tables; `uniq_packages_name_active` + `uniq_packages_mount_active` partial indexes.
- `src/kernel/packages/asset_server.hpp:62–85` — `cancel_all_registrations`, `restore_routes`, `dispatch_for_test` all public.
- `tests/kernel/packages/asset_server_test.cpp:75` — `dispatch_for_test` is the call path (the Slice B C13 retreat from the trantor teardown flake).

### Discussion / design context
- `docs/design/DESIGN-glassworm-defense-v0x.md` — authored in the prior rewrite session; 0.4.1 shipped against this DESIGN + its paired ICD.
- `docs/design/DESIGN-packages-v04x.md §§0.4.2 0.4.3 0.4.4 0.4.5 0.4.6 0.4.7` — normative source for all 0.4.x code milestones; §0.4.5–§0.4.7 remain the contract for the next-N window until ICDs land.
- `docs/DEFERRED.md` — five active entries re-examined (§5).

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. The window closed **without operational drift** on ICD-declared surfaces (§1 Code subsection documents the ICD-to-code match on every load-bearing symbol). Two real actionable gaps land in this session, plus one ratification-only deviations catalog (§4). The WS teardown test-strategy item (§2.2) is the substantive piece of work this re-eval schedules.

### 2.1 `arch-silent-on-code` — ICD-0.4.4 I.18 / I.19 / I.20 deferred without a DEFERRED.md entry

CHANGELOG `v0.4.4 §ICD deviations` item **(d)** records that three ICD-0.4.4 test cases were deferred to a follow-up PR: I.18 (concurrent POST same-name), I.19 (`?dry_run=1`), I.20 (RBAC denial 403). The rationale is sound — all three need a `/api/packages` HTTP test harness the v0.4.4 window did not invest in — and the library-level install path is fully exercised by I.01–I.10 + crash-recovery cases. The deferral is recorded **only** in CHANGELOG; `docs/DEFERRED.md` §Active has no entry.

DEFERRED.md's stated role per its preamble is *"single running log of design decisions that were intentionally deferred in a code milestone"* — CHANGELOG mentions alone are explicitly named as insufficient. The I.18/I.19/I.20 deferral is the canonical shape for this log.

**Resolution.** New DEFERRED.md entry dated 2026-04-20 under §Active, titled *"ICD-0.4.4 I.18/I.19/I.20 — HTTP test harness for /api/packages"*, pointing at a future follow-up PR (no ROADMAP slot required; harness is small, lands ad-hoc when the next HTTP-surface work touches this code). Cross-reference from ICD-0.4.4 §Test Cases (inline footnote) is a light follow-up; flagged as **no-code-change** but worth adding next time this ICD is edited. **Fixed in this session** (DEFERRED.md entry).

### 2.2 `arch-silent-on-code` — WS teardown test-strategy redesign, now architect-requested

The `bad_weak_ptr` teardown race in `trantor::EventLoop::loop` (canonical DEFERRED.md entry, `project_ws_flaky_segfault.md` in session memory) fired again on Slice B CI #12164, this time on the new asset-server tests (I.13 / I.14 / I.15). Slice B's C13 commit sidestepped by exposing `asset_server::dispatch_for_test` and rewriting the tests to call the handler directly — no Drogon listener, no WS stack, no TimerQueue, no race. The fix works, but the pattern is diagnostic: the bandaid ladder so far is
`g_shutdown_pending` (0.3.3.1) → `plinth::log::shutdown` (0.3.4.1) → `drain_pending_jobs` + `cancel_all_timers` (0.4.0.1) → `dispatch_for_test` direct-call (0.4.4.1, slice B C13). Four rungs on the same phenomenon. Architect signal 2026-04-20: *"schedule test-strategy redesign sooner than later"*.

**Working hypothesis (architect, 2026-04-20).** The race is a **test-harness teardown artifact, not a production-path race**. Evidence: every observed occurrence fires AFTER Catch2's "All tests passed" line — no assertion has ever failed to it; the crash is in `std::atexit`-registered teardown callables racing Catch2's per-process reporter exit under `catch_discover_tests`' subprocess-per-test model. Production `plinth` uses Drogon's SIGTERM handler (not our atexit chain) and does not create/destroy hundreds of JS runtimes per minute the way subprocess-per-test does. This premise is the **architect's perception, not a proven audit** — no dedicated code review of the production shutdown path against the Catch2 atexit chain has been performed. The §Investigation Gate below is the escape hatch if that hypothesis turns out wrong.

**Scope as framed by this hypothesis.** 0.4.4.1 is a **rethink of how we test/exercise the code**, not a patch to production. All six candidates below are test-harness-only changes: test fixture lifecycle, test-subprocess model, test binary split, test-side lazy initialization. None of them modifies production shutdown ordering, production connection lifecycle, or production runtime lifecycles. If the right answer is cheaper test-harness discipline, 0.4.4.1 delivers it and the production shutdown path stays untouched.

This re-eval scopes a dedicated follow-up. Per the architect's disposition in the planning phase of this session (accepted recommendation), it lands as a **four-part follow-up 0.4.4.1 `WS teardown test-strategy redesign`** (un-tagged per the 3-part X.Y.Z-only rule), ahead of 0.4.5 code work. The candidate redesign menu lifted from `project_ws_flaky_segfault.md §Candidate redesigns` is the implementing session's starting input — the selection among options is deferred to that session (precedent: 0.4.0.1 picked from its own candidate list at implementation time):

1. **Per-subprocess fresh PG database** — `CREATE DATABASE plinth_test_{pid}` at subprocess start; drop at atexit. Eliminates cross-subprocess PG state contention that amplifies teardown windows.
2. **Reduce `async_bridge_fixture.cpp` `pool_size = 80` default** — drop toward 10 except on the one test that explicitly needs fan-out. Closes the "too many clients" symptom that compounds 91-test cascades.
3. **Kill the subprocess-per-test model for WS/JS integration** — switch those suites to Catch2's single-process runner so Drogon + JS state is initialized once per process, torn down once.
4. **Split test binaries by framework dependency** — `plinth_tests_pure` (parser/validator/security), `plinth_tests_drogon` (HTTP + WS only), `plinth_tests_js`, `plinth_tests_pg`. One init / one teardown per binary.
5. **Lazy Drogon** — extend the 0.3.4.1 split (`ensure_drogon_running` vs `..._with_db`) toward "ensure only what this test needs." Cap.* doesn't need HTTP listener; identity tests don't need JS runtime. `asset_server::dispatch_for_test` is the Slice B C13 prototype of this pattern.
6. **Explicit teardown sequencing from `main_test.cpp`** — replace `std::atexit` registration with an explicit `return run_tests_then_teardown()`. Gives deterministic ordering vs Catch2's per-process reporter exit.

No single option is obviously right; the implementing session should pick 1–2 based on cost and return. The ICD-0.4.0.1 precedent stands — *"candidate list at implementation time"* is the workflow.

**Investigation gate (mandatory first step of 0.4.4.1).** Before picking any candidate, the implementing session runs a diagnostic pass to **confirm or falsify the architect's working hypothesis**. Two inputs, one static + one empirical:

1. **Static read.** Walk the production shutdown path in `src/kernel/main.cpp`'s service-mode branch against the Catch2 atexit chain in `tests/kernel/ws/ws_test_fixture.cpp` and `tests/kernel/js/async_bridge_fixture.cpp`. List every callable registered on each side; annotate which ones overlap (i.e. would be invoked on both the production SIGTERM path and the test atexit path with the same ordering and the same weak_ptr relationships).
2. **Empirical read (load-harness LH-0).** Run the standalone Load Harness (see ROADMAP §Load Harness, LH-0) against the production `plinth` binary. LH-0's scaffold is separately scheduled as a parallel-test-framework item gated on 0.4.4 (already shipped) — if LH-0 has landed by the time 0.4.4.1 starts, its load output is the empirical audit this gate calls for: does the production kernel ever exhibit `bad_weak_ptr` / `free_zero_refcount` / `JS_FreeRuntime list_empty` signatures under sustained HTTP + cap.call load? If LH-0 has not landed, 0.4.4.1 proceeds on the static read alone (still cheap, still informative), with the empirical audit deferred to LH-0's own shipping session.
3. **Gate.** If every observed crash signature traces to state that exists *only* on the Catch2 subprocess-per-test path (e.g. repeated Drogon singleton create/destroy, `std::atexit` ordering against a Catch2 reporter, fixture-owned connection pools that production never exhausts), **and LH-0's output (if available) shows zero known-bad signatures on production**, the hypothesis is confirmed — proceed to candidate selection. If either the static walk OR LH-0 surfaces evidence that production hits any part of the same race, **stop**. Do not pick a candidate. Do not implement. Escalate to the architect for a design conversation; the scope materially changes from "test-harness redesign" to "production kernel race fix" and deserves its own design review + ICD + band label.

This gate is cheap (a reading pass over two short files + a backtrace taxonomy, plus whatever LH-0 output exists) and protects against the worst failure mode: a test-harness redesign that plasters over a real production race.

**Resolution.** New ROADMAP item **0.4.4.1 WS teardown test-strategy redesign [strong]** inserted ahead of 0.4.5. Scope is "(1) run the investigation gate above; (2) if gate passes, pick one or two of the six candidates, implement, demonstrate CI greenness on a tight-loop full-suite sample matching the 0.4.0.1 methodology (≥20 runs, across CI and local container); (3) if gate fails, stop and escalate." No CHANGELOG deviation carried forward; no new DEFERRED.md entry (the WS flake entry stays active as the phenomenon being addressed). **Fixed in this session** (ROADMAP edit + scope framing). Code work is out of scope for this PR.

### 2.3 Observation — ICD-0.4.4 prose / enum internal inconsistency on `ACTIVE_FLAGGED`

ICD-0.4.4 line 39 says *"ACTIVE_FLAGGED. Reserved in the state enum, never entered in 0.4.4."* The C++ `InstallStage` enum declared in ICD §Library Surface (lines 347–350) has **only 8 values** (UPLOADING, VALIDATING, MIGRATING, REGISTERING, EXTRACTING, ACTIVATING, ACTIVE, INSTALL_FAILED) — no ACTIVE_FLAGGED, no DISABLED, no UNINSTALLING. The persisted state space (the `migrations/schema.sql` CHECK constraint) has 11 values, reserving ACTIVE_FLAGGED / DISABLED / UNINSTALLING ahead of 0.4.5 + 0.4.7 writers.

Shipped code matches the ICD-declared `InstallStage` enum exactly — 8 values, 0.4.5's writers will extend it when they need to. Shipped schema also matches the ICD-declared `CHECK` exactly — 11 values. The drift is purely in the ICD's line-39 prose, which refers to "state enum" when it means "schema CHECK state vocabulary". Operationally zero-gap.

**Resolution.** No amendment. Too minor to edit; a future reader tripping on the prose can be pointed at the §Library Surface enum + the schema CHECK list — both are authoritative and unambiguous. Recorded here so the next re-eval reader knows this was noticed and dismissed. **No action.**

### 2.4 No interface-drift

Re-reading the four CHANGELOG deviation sections against each ICD and against the shipped code: every deviation is ratified in-CHANGELOG, every symbol the ICDs declare ships at the declared name, every exit code / HTTP status / JSON field matches the ICD table. The full consolidated catalog is §4. No ICD amendments are required to discharge this window.

### 2.5 No missing-test-for-arch-claim

Cross-checked:
- ICD-0.4.1 G.01–G.17 → `tests/kernel/security/unicode_scanner_test.cpp` + `tests/kernel/js/eval_guard_test.cpp` + `validator_test.cpp` (fixture cases G.09–G.13). All 17 cases present; benchmark `BM_UnicodeScanner_CleanAscii_1MiB` lands ~16× over the 100 MB/s ICD floor.
- ICD-0.4.2 CF1..CF7 + CFW1..CFW4 + RT1..RT3 → `cross_file_validator_test.cpp` + panels/config parser tests; 15 static fixtures + 3 runtime-state fixtures (gated behind `PLINTH_KERNEL_TESTS=ON`).
- ICD-0.4.3 M.01–M.15 → `migrations_parsing_test.cpp` + `migrations_test.cpp`; 12 pure-logic + 16 PG-gated cases, the latter SKIP when `PLINTH_PG_HOST` is unset.
- ICD-0.4.4 I.01–I.17 → `install_lifecycle_test.cpp` + `crash_recovery_test.cpp` + `asset_server_test.cpp`; I.18–I.20 deferred per §2.1. Coverage on the library path is complete; coverage on the HTTP path is gated on §2.1's resolution.

### 2.6 `arch-silent-on-code` — accepted deviations surface examined for ICD-amendment candidates

Four windows, 17 total deviations (§4 catalogs them). Walked each row with the question *"does this deviation's rationale point at an ICD wording bug rather than a kernel-convention alignment?"* None qualify. Every deviation either (a) cites an existing kernel-convention precedent, (b) is permitted by the ICD's §Appendix latitude text, or (c) is a forced consequence of an external constraint (PG role-drop ordering, Drogon route-deregistration limitation, trantor teardown race). **No ICD amendments.**

### 2.7 DEFERRED.md divergence — one addition, zero removals

Re-examined `docs/DEFERRED.md` §Active entries (five):
1. **Per-op `SET search_path` for `db.*`** — active, pointer updated in `RE-EVAL-0.3.x-arc-closeout §2.8` to 0.5.x. 0.4.3 correctly did not pick it up (library-only, no runtime). Cross-reference holds.
2. **`db.*` PG-type→JS-type mapping** — active, same 0.5.x pointer (arc-closeout §2.8 update). Holds.
3. **WS teardown flake** — active; this re-eval materially advances it via §2.2's new 0.4.4.1 task. The DEFERRED entry stays; the entry + the task together carry the work.
4. **MEMORY_LIMIT classifier peak-tracking** — watchlist, resolved; no regression.
5. **Drogon `PgBatchConnection` `SqlError` typing** — active, ad-hoc; no change.

**Addition:** §2.1 adds one new entry for I.18/I.19/I.20.

### 2.8 No cadence-drift

The 4-milestone cadence held cleanly this window. The prior session (`RE-EVAL-0.4.x.md §2.7`) inserted 0.4.1 GlassWorm mid-arc and preemptively adjusted the cadence line from `RE-EVAL following 0.4.5` → `RE-EVAL following 0.4.4`. This re-eval is exactly that item discharged. The arc has three remaining 3-part milestones (0.4.5, 0.4.6, 0.4.7) plus this §2.2's 0.4.4.1 follow-up (not a 3-part, does not count toward cadence per the preamble). Next cadence slot falls on **0.4.7 arc-closeout** — see §7.

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.4.1 §Scanner Primitive** — `scan_for_invisible_unicode` signature, `UnicodeFinding` / `UnicodeScanResult` / `UnicodeScanConfig` shapes, 13 scanned ranges all present in `unicode_scanner.hpp`. Policy (threshold 50 default, BOM always counted, strict UTF-8) matches ICD verbatim.
- **ICD-0.4.1 §Layer 1 / Layer 2** — `run_unicode_scan_pass` placement, three `JS_Eval` guard sites, async-path failure-object convention, three audit events with 1 Hz `(layer, source_path)` rate-limit LRU all match.
- **ICD-0.4.2 §Cross-File Validation Rules** — CF1..CF7 + CFW1..CFW4 all implemented with the rule IDs matching the ICD table. `ParsedPackage` exposed for future 0.4.4 install-lifecycle reuse (confirmed used).
- **ICD-0.4.2 §CLI Contract** — `--structure-only` / `--against-running-kernel` / `--kernel` flags + `PLINTH_KERNEL_URL` fallback all present. Mutually-exclusive argparse group rejects the conflicting flag combination.
- **ICD-0.4.3 §Library Surface** — `run_migrations(name, package_root, admin_conn) → std::expected<MigrationReport, MigrationFailure>` and `drop_schema_and_migrations(name, admin_conn)` both shipped; advisory-lock serialization via `hashtextextended` + SHA-256 via reused `plinth::auth::sha256_hex` both match.
- **ICD-0.4.3 §Test Cases** — 15 fixtures under `tests/fixtures/migration_packages/`; 12 unit + 16 PG-gated Catch2 cases.
- **ICD-0.4.4 §State Machine** — UPLOADING → VALIDATING → MIGRATING → REGISTERING → EXTRACTING → ACTIVATING → ACTIVE (or INSTALL_FAILED at any stage) matches the C++ enum; per-name PG advisory lock + first-install invariant + 409 on collision all present.
- **ICD-0.4.4 §HTTP Surface** — `POST /api/packages` (multipart), `GET /api/packages` (paginated with `?limit`/`?offset`/`?include_failed`), `GET /api/packages/{id}` all shipped; RBAC gates (`packages.install` / `packages.read`) applied; 413/409/422/500 HTTP table matches.
- **ICD-0.4.4 §Data Model** — `plinth.packages` (11-state CHECK, UUID PK, `uniq_packages_name_active` + `uniq_packages_mount_active` partial indexes) + `plinth.panels` (CASCADE-DELETE, panel_type CHECK, slot_type nullable-with-CHECK) both present at `migrations/schema.sql:119–157`.
- **ICD-0.4.4 §Library Surface — `dispatch_for_test`** — Slice B C13's sidestep of the trantor teardown race is called from `asset_server_test.cpp:75`; function is exported at `asset_server.hpp:80–85`.
- **Cross-arc invariants** (re-checked): no new long-running threads in 0.4.x outside the WS + JS subsystems already accounted for; no new DbClient pool; `validate()` remains pure per ICD-0.4.0 §Security Constraint 3 across the 0.4.1 + 0.4.2 extensions.

---

## 4. Accepted deviations log — 0.4.1 through 0.4.4

Consolidated table for the window. Prior-window deviations are in `RE-EVAL-0.4.x.md §4` (0.4.0, four rows); this table does not duplicate them.

| Milestone | Deviation | Rationale | Status |
|---|---|---|---|
| 0.4.1 | CMake edits required — new `plinth_unicode_scanner_benchmark` `add_executable` stanza + `PLINTH_BENCHMARK_KERNEL_SOURCES` extended; new TUs added to both `plinth` and `plinth_tests` source lists (`CMakeLists.txt:398–420` are explicit, not globbed). | ICD §Appendix claim "benchmarks globbed" didn't match current `main`; executables are explicitly listed, only the `tidy` target uses the glob. | ratified |
| 0.4.1 | Config file JSON-parsed (not YAML) despite `.yml.example` filename. New `security.unicode_scanner.{enabled,threshold,log_findings}` block uses JSON syntax. | Existing kernel convention (`nlohmann::json` since 0.1.1); example file already carried the documentary misnomer. | ratified |
| 0.4.1 | Layer 2 async path uses `co_return EvalResult{.value = std::unexpected(*err), .duration = ...}` instead of ICD pseudocode's `co_return std::unexpected(...)`. | ICD pseudocode was structurally wrong for `drogon::Task<EvalResult>` (the function returns `Task<EvalResult>`, not `Task<expected<...>>`). Implementation uses the pre-existing failure pattern at `run_on_context.cpp:851`. | ratified |
| 0.4.2 | Reporter lives in `src/kernel/packages/detail/reporter.hpp` (kernel-internal `detail/`), not promoted into public `validator.hpp`. | Keeps `ValidationMessage` aggregate construction an implementation detail; public API surface is minimized. | ratified |
| 0.4.2 | RT1..RT3 runtime-state rules are stubbed with a single `runtime-validate-unimplemented` error; HTTP path to kernel's `POST /api/packages/validate` is 0.4.4 scope. | Cross-file-error gating on RT skip (`runtime-state-skipped` warning) still fires, so skip semantics are covered today; full RT wiring needs 0.4.4's `/api/packages` HTTP surface. | ratified |
| 0.4.2 | CF3 `realpath` check uses `fs::weakly_canonical` + string-prefix comparison inline, not promoted out of `validator.cpp`. | CF3 is the only cross-file caller; the check is three lines; DRY-at-function-boundary was not yet load-bearing. | ratified |
| 0.4.2 | `ConfigEntry::default_value` widens to `std::variant<monostate,bool,int64_t,double,string>` (ICD said "scalar"). | Captures JSON's int-vs-float distinction without losing precision; `std::monostate` models JSON null cleanly. | ratified |
| 0.4.2 | `panels.json` top-level array form accepted with `panels.shape.array_at_root` warning rather than rejected. | Matches 0.4.0 R5's tolerance posture; future `panels.shape.*` warnings/errors can tighten without breaking existing test packages. | ratified |
| 0.4.2 | Phase enum in JSON output uses snake_case `"structure"` / `"cross_file"` / `"runtime_state"` (ICD proposed dash form `"cross-file"`). | Matches every other kernel enum→JSON projection (capability scope, provider_type, etc.). | ratified |
| 0.4.3 | `drop_schema_and_migrations` issues `DROP OWNED BY ext_{name}_role CASCADE` before `DROP ROLE` (ICD silent on ordering). | PG semantics force this — `SELECT ON plinth.users` grant otherwise blocks `DROP ROLE` with "role has privileges that must be revoked first." | ratified |
| 0.4.3 | `MigrationError::DROP_FAILED` added beyond ICD's 8-kind enum. | Teardown helper needs a distinct failure shape from `SCHEMA_CREATE_FAILED`; the ICD enumerated main-path kinds only. | ratified |
| 0.4.3 | Pure-logic helpers promoted to `namespace detail` header `migrations_internal.hpp`. | Enables `migrations_parsing_test.cpp` to exercise helpers without the PG harness; ICD called for the two test files but did not prescribe helper exposure. | ratified |
| 0.4.3 | Fixture SQL uses `{EXT_NAME}` placeholder substituted at stage time. | Tests need real DDL referencing an ephemeral schema; the ICD didn't address fixture SQL vs per-test schema naming. | ratified |
| 0.4.4 (slice A) | Single Drogon wildcard regex `/ext/([^/]+)/([^/]+)/(.*)` route for asset serving (not per-(name,version) registration). | Drogon v1.9.12 offers no route deregistration primitive; the map-guarded trampoline achieves per-install register/deregister without mutating Drogon's route table. | ratified |
| 0.4.4 (slice A) | `ValidationConfig::in_process_registry` flag wired but not consumed by slice A's installer (RT1/RT2 handled inline, RT3 via manifest parse). | The flag stays reserved for the CLI's `plinth validate --against-running-kernel` loopback path when that wiring lands (0.10.5 CLI hardening, or sooner if demand appears). | ratified |
| 0.4.4 (slice A) | `reconcile_in_flight_installs` is a stub in slice A. | Production binary calls it during bootstrap; slice B (same PR cycle) provides the body. Shipping-order trade-off; no shipped behavior delta. | ratified (slice B delivered body) |
| 0.4.4 (slice A) | Unit test coverage limited to pure-helper round-trips in slice A. | Fixture + PG-gated + Drogon-harness cases land in slice B on the same branch before PR open. | ratified (slice B delivered) |
| 0.4.4 (slice B) | 7 fixtures (6 dirs + `not-a-zip.bin`) under `tests/fixtures/install_lifecycle/` instead of 16 per ICD. | Redundant cases share base trees; dynamic cases (oversized, path-traversal, unknown route) are cheaper to generate in the driver; coverage is equivalent at the observable level. | ratified |
| 0.4.4 (slice B) | No separate `tests/kernel/http_test_fixture.{hpp,cpp}`; asset-server tests reuse `ws_test_fixture`'s Drogon instance. | Drogon's process-wide `app()` singleton makes a second fixture infeasible; the ws fixture's server startup registers the asset handler in one line. | ratified; compounded by §2.2 |
| 0.4.4 (slice B) | No `fork()`/`exec()`/SIGKILL subprocess harness. Crash recovery exercised by seeding `plinth.packages` rows with manufactured in-flight states + invoking `reconcile_in_flight_installs()` in-process. | Reconciler correctness is a function of `(state, on-disk tree presence)`; both inputs are trivially constructible without a subprocess. Also sidesteps the parked trantor teardown flake. | ratified |
| 0.4.4 (slice B) | I.18 / I.19 / I.20 deferred to a follow-up HTTP-harness PR. | The three cases require the `/api/packages` HTTP surface with a session + RBAC seed path; library-level install path is fully covered by I.01–I.10; not blocking `v0.4.4`. | ratified, now in DEFERRED.md (§2.1) |
| 0.4.4 (slice B C13) | `asset_server::dispatch_for_test` shim added so asset-server tests can call the handler directly, skipping the Drogon listener + WS stack. | CI #12164 surfaced the parked trantor `bad_weak_ptr` teardown flake on I.13/I.14/I.15 (asset-server tests inherited the race by reusing `ws_test_fixture`). Direct-call tests: no TimerQueue, no race. | ratified; feeds §2.2 |

**Observation.** 22 deviations across the four 3-part milestones (3 + 6 + 4 + 9 including slice A/B/C13 split). Zero retractions. Rationale taxonomy roughly: 5 kernel-convention alignment, 6 external-constraint (PG/Drogon/trantor), 5 ICD-text imprecision permitted by §Appendix, 6 shipping-order or scope trade-offs (slice A/B split + I.18–I.20 deferral). None point back at an ICD contract bug. The 0.4.4 deviations cluster around test-harness design — the same theme §2.2's redesign task will address.

This table is the 0.4.1–0.4.4 window's consolidated reference. Future re-evals in the 0.4.x window should append, not rebuild.

---

## 5. Known-issues preserved

Five `DEFERRED.md` §Active entries re-examined (§2.7 enumerates); no new status changes beyond what §2.1 adds and §2.2 advances.

### 5.1 WS teardown flake — actively being addressed by §2.2

The `bad_weak_ptr` teardown race fired again in this window, specifically on Slice B CI #12164 against new asset-server tests. Slice B's C13 sidestepped via `dispatch_for_test`. The DEFERRED entry stays active; §2.2's 0.4.4.1 task is the dedicated follow-up the maintainer's 2026-04-20 signal asked for. When that task lands, the DEFERRED entry moves to §Resolved.

### 5.2 Per-op `SET search_path` + `db.*` PG→JS type mapping

Both 0.5.x-pointed; 0.4.3 was correctly not the pickup point (library-only). No action this window.

### 5.3 Drogon `PgBatchConnection` `SqlError` typing + MEMORY_LIMIT classifier watchlist

Unchanged.

---

## 6. Forward ICD presence check

Per METHODOLOGY §3.1 *Forward ICD presence check*:

**Next-N window (N=3):**

| Milestone | Band (incoming) | Band (outgoing) | ICD exists? | Action |
|---|---|---|---|---|
| 0.4.4.1 (WS teardown redesign) | new | **strong** | N/A (scope captured in §2.2; four-part follow-ups don't get standalone ICDs) | Implementing session works from `project_ws_flaky_segfault.md §Candidate redesigns` + §2.2 menu. |
| 0.4.5 (disable / enable / uninstall / upgrade) | medium | **strong** | ❌ | `DESIGN-packages-v04x.md §0.4.5` is contract-by-pointer (precedent: 0.4.1 GlassWorm shipped with DESIGN contract, ICD authored subsequently). ICD-0.4.5 authoring is the next paper-session trigger after this PR + 0.4.4.1 merge. |
| 0.4.6 (RBAC Phase A) | medium | medium | ❌ | `DESIGN-packages-v04x.md §0.4.6` is contract. Per horizon rule (one-ahead), ICD authored after 0.4.5 code ships. |

**After-N window (milestones 4–7 out, M=7):** 0.4.7, 0.5.0–0.5.2 (realtime scope). All `[medium]` with DESIGN coverage (`DESIGN-packages-v04x §0.4.7`, architecture `04-realtime.md` for 0.5.x). No forward-ICD-presence action.

**Forward-check result.** One ICD backlog item (ICD-0.4.5). The contract-by-pointer posture (§2.5 in `RE-EVAL-0.4.x.md`, §9.3 there) is invoked; DESIGN-packages-v04x §0.4.5 is the authoritative contract until ICD-0.4.5 lands. The horizon rule is preserved (one ahead of in-flight code: 0.4.4.1 ships first, then ICD-0.4.5, then 0.4.5 code).

---

## 7. Disposition — what was fixed here vs. scheduled

### Fixed in this PR (doc-only)

1. `docs/reviews/RE-EVAL-0.4.x-following-0.4.4.md` — **new** (this file).
2. `docs/ROADMAP.md` —
   - `RE-EVAL following 0.4.4` line discharged.
   - **New `0.4.4.1 WS teardown test-strategy redesign [strong]`** inserted at the head of §0.4, ahead of `0.4.5`.
   - **New parallel `## Load Harness (parallel test framework; gated on capability unlocks)` section** between §0.4 and §0.5. Five items LH-0..LH-4 with independent gating and schedule (LH-0 `[strong]`, already unlocked by 0.4.4; LH-1..LH-4 `[medium]`, each gated on a downstream capability). External observability (ps/top/perf) — no dependency on `plinth.metrics` (0.7.1). LH-4 cross-validates `plinth.metrics` against the external ground truth LH-0..LH-3 collect.
   - Next cadence line **added** at arc boundary: `RE-EVAL following 0.4.7 (0.4.x arc closeout) [rewrite session]` — matches the 0.3.6 arc-closeout precedent (arc has three remaining 3-part milestones before 0.5.0, so the next cadence slot naturally lands on arc boundary).
3. `docs/DEFERRED.md` — **new** §Active entry: `ICD-0.4.4 I.18/I.19/I.20 — HTTP test harness for /api/packages`.
4. `docs/CHANGELOG.md` — **new** Rewrite-session entry under 2026-04-20.

### Scheduled for future sessions (no action this PR)

- **0.4.4.1 WS teardown test-strategy redesign** — code session. Works from §2.2's candidate menu. Lands before 0.4.5 code work begins. Four-part follow-up, no tag.
- **ICD-0.4.5 authoring** — paper session. Fires after 0.4.4.1 merges, before 0.4.5 code. One-ahead horizon preserved.
- **ICD-0.4.4 inline footnote for I.18/I.19/I.20** — cosmetic; picked up whenever the ICD is next touched.
- **WS teardown DEFERRED entry → §Resolved** — moves when 0.4.4.1 ships with a demonstrated CI-loop-green sample.

---

## 8. Band label review

Per the Plinth labeling rule and this re-eval's triggers:

| Milestone | Current | New | Rationale |
|---|---|---|---|
| 0.4.4.1 (WS teardown redesign) | — (new insertion) | **strong** | Load-bearing for the next 3 milestones (0.4.4.1 itself + 0.4.5 + 0.4.6 — each touches or inherits test-harness state). Architect signal 2026-04-20 upgrades urgency. |
| LH-0 (load-harness scaffold, parallel) | — (new insertion) | **strong** | Architect-added 2026-04-20 as a parallel test framework. Immediate empirical input to 0.4.4.1's investigation gate. Gate already met (0.4.4 shipped). External observability (ps/top/perf) — no dependency on 0.7.1 metrics. |
| LH-1 / LH-2 / LH-3 (realtime tiers, parallel) | — (new insertions) | medium | Extend LH-0 as each realtime capability lands. Non-blocking; schedule can slip. |
| LH-4 (consolidated + metrics cross-validation, parallel) | — (new insertion) | medium | Gated on 0.7.1 plinth.metrics. Validates internal metrics against the external ground truth LH-0..LH-3 were already collecting. |
| 0.4.5 | medium | **strong** | Enters next-N window after this re-eval. ICD authoring scheduled; DESIGN §0.4.5 is contract meanwhile. |
| 0.4.6 | medium | medium | Still in next-N; ICD authoring deferred one more cycle per horizon rule. |
| 0.4.7 | medium | medium | After-N; DESIGN §0.4.7 coverage sufficient. |
| 0.5.x | medium | medium | Unchanged. |
| 0.6.x | medium | medium | Unchanged. |
| 0.6a-* | fuzzy | fuzzy | Unchanged. |
| 0.7.x – 1.0 | fuzzy | fuzzy | Unchanged. |

**Promotions applied:** 0.4.4.1 (new, `[strong]`), LH-0 (new, `[strong]`), LH-1/2/3/4 (new, `[medium]`), 0.4.5 (`[medium]` → `[strong]`). No demotions. Landed in §7 item 2.

---

## 9. Observations for methodology / future re-evals

### 9.1 Bandaid-fatigue threshold on teardown races

The WS teardown flake has accumulated four distinct rungs of bandaid since 0.3.3:
`g_shutdown_pending` (0.3.3.1) → `plinth::log::shutdown` (0.3.4.1) → `drain_pending_jobs` + `cancel_all_timers` (0.4.0.1) → `dispatch_for_test` direct-call (0.4.4.1 slice B C13). Each rung shipped with a "zero occurrences in 20 local runs" claim; each rung was falsified by a later CI run that found a new sub-path. The bandaid pattern has hit diminishing returns — each rung addresses one sub-path by name, but the `catch_discover_tests` subprocess-per-test model keeps producing new teardown windows the bandaid list doesn't cover.

**Methodology takeaway.** When the bandaid count on a single phenomenon passes ~3, redesign instead of adding a fourth. Candidate rule addition to METHODOLOGY §Phase 3 failure-mode table: *"Bandaid-ladder on a recurring phenomenon"* — flagged when the same failure signature class accrues ≥3 point fixes across consecutive milestones, re-eval responds with a redesign-task scheduling rather than documenting a fourth bandaid. This re-eval is the first instance of the rule in action (§2.2 schedules the redesign instead of documenting a fifth rung); the methodology amendment itself is light-touch and not urgent — flagged for architect consideration in the next paper-session slot.

### 9.2 First cadence with a mid-arc insertion held up cleanly

The prior re-eval (`RE-EVAL-0.4.x.md §2.7, §9.2`) flagged mid-arc 3-part milestone insertion (0.4.1 GlassWorm) as a new-in-Plinth pattern and preemptively adjusted the cadence line. Four milestones later, that adjustment held: 0.4.1 + 0.4.2 + 0.4.3 + 0.4.4 count as 4/4, this re-eval discharges cleanly, and the arc-closeout slot (§7) lands naturally at 0.4.7. No cadence-arithmetic surprises.

### 9.3 Deviations clustering around test-harness design is load-bearing signal

Five of the nine 0.4.4 deviations (slice A + slice B + C13) are test-harness trade-offs: no separate `http_test_fixture`, no `fork()/SIGKILL` harness, fewer fixtures than ICD specified, I.18–I.20 deferred, `dispatch_for_test` shim. That cluster was not random — it reflects the underlying bandaid-ladder phenomenon (§9.1). §2.2's 0.4.4.1 task is the structural fix. Future re-evals should watch for deviation-clustering as a leading indicator of structural debt (a weak pattern right now; flagged here as a data point for the next cadence re-eval to either confirm or reject).

### 9.4 Forward-ICD-presence rule still paying off

The 0.3.3.4 METHODOLOGY amendment's forward-ICD check has caught one more potential drift this cycle: ICD-0.4.5 would otherwise have started code work without a contract (DESIGN-only posture). §6's explicit scheduling + §2.2's positioning 0.4.4.1 ahead of 0.4.5 together preserve the one-ahead horizon. Three-for-three signal that the rule is working as designed.

---

## 10. Exit criteria

Per METHODOLOGY §3.3:

- [x] Architecture documents read; no architecture-doc amendment required this window. The GlassWorm integration referenced in the prior re-eval as a future touch to `architecture/05-extensions.md §3.1` remains deferred (tied to 0.6.x's Layer 3; not in this re-eval's remit).
- [x] Band promotions applied (0.4.4.1 new `[strong]`, 0.4.5 `[medium]`→`[strong]` — §8).
- [x] Roadmap milestone labels reviewed; two promotions; arc-closeout cadence line inserted; no completed-milestone lines to trim (already trimmed per ship-time preamble rule on each milestone).
- [x] ICD amendments land in this PR — **none** required by §2 (zero-drift window). ICD-0.4.5 authoring is scheduled, not in this PR.
- [x] Forward ICD presence check passed (§6).
- [x] Known-issues preserved in durable storage (DEFERRED.md — §2.1 adds one new entry; §2.7 enumerates status).
- [x] Session output committed to the documentation tree (`docs/reviews/RE-EVAL-0.4.x-following-0.4.4.md` — this file).
- [x] Code untouched. No C++ / CMake / CI-YAML edits.
- [x] CHANGELOG entry for this session. **Un-tagged** per 2026-04-18 "3-part X.Y.Z tag only" rule (cadence re-eval is not an arc-closeout).

---

## 11. Next actions for the architect

1. **Review this artifact.** Flag any disposition you disagree with. Particularly:
   - §2.2 disposition (0.4.4.1 WS teardown redesign as a four-part follow-up at `[strong]`, positioned ahead of 0.4.5). Candidate-menu selection deferred to the implementing session — architect may prefer to prescribe one option now rather than at implementation time.
   - §7 arc-closeout cadence line insertion (`RE-EVAL following 0.4.7 (0.4.x arc closeout)`). Reversible; alternative is to let the cadence slot float and re-decide at the next re-eval.
   - §9.1 methodology observation (bandaid-fatigue threshold rule) — worth a future paper-session METHODOLOGY amendment, or leave it organic in the re-eval log?
2. **Confirm the §2.2 working hypothesis and investigation-gate trigger.** The 0.4.4.1 scoping assumes the race is test-harness-only and production is unaffected. The hypothesis is the architect's perception, not a proven audit. The gate has two inputs: a static walk of production shutdown vs the Catch2 atexit chain, plus (if already shipped) LH-0's empirical load output against the production binary. If either input surfaces evidence that production hits any part of the same race, 0.4.4.1 stops before candidate selection and a design conversation replaces the implementing session. Architect is pinged at gate-fail regardless of which candidate looked attractive. A gate-fail finding reopens the scoping: new ICD, likely a new band label, likely a different position on the roadmap.
3. **Parallel Load Harness sequencing.** LH-0 and 0.4.4.1 are now interlocked: LH-0 provides the empirical half of 0.4.4.1's gate. Options: (a) ship LH-0 first, run 0.4.4.1 with full gate (static + empirical); (b) ship 0.4.4.1 first on static gate alone, run LH-0 afterward as ongoing audit; (c) run in parallel with separate implementing sessions. The schedule is intentionally loose ("schedule can slip as necessary" — architect 2026-04-20) — no downstream milestone blocks on Load Harness items. Language/topology decision on LH-0 (Rust, Go, C++ — standalone binary in all cases) is made at LH-0's implementation kickoff, not here.
4. **Confirm 0.4.5 code session entry conditions.** Per ROADMAP: (a) this PR merged, (b) 0.4.4.1 WS teardown redesign shipped, (c) ICD-0.4.5 authored in a subsequent docs session, then 0.4.5 code work begins. Architect may choose to author ICD-0.4.5 before 0.4.4.1 lands if the test-harness redesign appears large — loosens the linear dependency. If §2.2's investigation gate fails and 0.4.4.1 expands into a production race fix, this order may need to re-arrange — see §11 item 2.
5. **Confirm next cadence re-eval position.** §7 proposes `RE-EVAL following 0.4.7 (0.4.x arc closeout)` at 4/4 cadence (0.4.5, 0.4.6, 0.4.7, then arc closeout). §2.8 applies this. Reversible if preferred.
6. **No tag on this session** per the 2026-04-18 "3-part X.Y.Z tag only" rule. CHANGELOG Rewrite-session entry is the ledger.
