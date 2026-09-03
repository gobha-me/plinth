# Re-evaluation — RE-EVAL following 0.4.0

**Date:** 2026-04-19
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3) — cadence re-eval, not arc-closeout
**Trigger:** Scheduled ROADMAP item `RE-EVAL following 0.4.0   [rewrite session]` — the cadence re-eval that blocks all further 0.4.x code work per ROADMAP preamble ("No further code milestone begins until the preceding re-eval item completes"). CHANGELOG 0.4.0 line 25 notes this line was renamed from `RE-EVAL following 0.4.1` when the 0.4.0 + 0.4.1 milestones collapsed into one tagged ship.
**Cadence position:** Fourth scheduled cadence re-eval. Cadence is `every 4 code milestones` per ROADMAP preamble; 3-part milestones since last cadence re-eval (`RE-EVAL following 0.3.3`, 2026-04-18) are: 0.3.4, 0.3.5, 0.4.0 (combined with 0.4.1). Formal cadence is **3/4** at 0.4.0 (0.3.6 arc-closeout fired a separate re-eval but did not consume the cadence slot — arc-closeouts count toward re-eval history, not the cadence denominator, per `RE-EVAL-0.3.x-arc-closeout.md §10`).
**Scope:** Code-aware gap analysis for 0.4.0 only (narrow — one 3-part milestone since last re-eval + one architect-supplied new-architectural-input). Absorbs `Descusion GlassWorm.md` (root-level discussion file, 2026-04-19) as the first new-architectural-input this project has encountered mid-cadence.

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index-only scan).
- `docs/architecture/05-extensions.md §1 Package Structure; §1.1 Cross-File Manifest Validation; §2 Reserved URL Prefixes; §3 QuickJS Runtime + §3.1 Runtime Limits` — normative source for 0.4.0's filesystem rules, for ICD-0.4.2's cross-file rules, and for the GlassWorm Layer 2 context (the `eval` / `Function` posture already called out at §3).
- `docs/architecture/01-identity.md §2` — normative source for `rbac.json` rule shape used by ICD-0.4.2 CF1 / CF2 / CFW1 rules.

### ICDs
- `docs/icd/ICD-0.4.0-package-structure-validation.md` — 0.4.0's contract; implemented in this window.
- `docs/icd/ICD-0.4.1-manifest-parsing.md` — 0.4.1's contract (the pre-collapse one); shipped in the same branch as ICD-0.4.0.
- `docs/icd/ICD-0.3.5-runtime-hardening.md` — consulted for the GlassWorm Layer 2 composability check (the 0.3.5 `eval` / `Function` deletion is upstream of the proposed Layer 2 scan; Layer 2 composes without modifying 0.3.5's surface).
- `docs/icd/ICD-0.3.1-runtime-lifecycle.md` — consulted to enumerate the three `JS_Eval` call sites Layer 2 must hook.

### Code
- `src/kernel/packages/manifest_error.hpp` / `manifest.{hpp,cpp}` / `capabilities_manifest.{hpp,cpp}` / `validator.{hpp,cpp}` — **new in 0.4.0.** Seven files.
- `src/kernel/main.cpp:155–185` — `validate` subcommand body filled in by 0.4.0.
- `tests/fixtures/packages/` — 13 fixture directories (verified present; matches CHANGELOG 0.4.0 §Verification line 96).
- `tests/kernel/packages/manifest_test.cpp` / `capabilities_manifest_test.cpp` / `validator_test.cpp` — three new test files; 18 TEST_CASE in validator_test.cpp (13 fixture + 1 render-JSON + 4 CLI-exit-code; matches CHANGELOG line 63).
- `src/kernel/js/eval.cpp:277` / `runtime_pool.cpp:697` / `run_on_context.cpp:816` — the three `JS_Eval` call sites enumerated in support of the GlassWorm Layer 2 integration surface (no new code touched; pointer discovery only).
- `src/kernel/packages/validator.cpp:474–490` — `validate()` entry; the Layer 1 injection point is the `run_json_parse()` → `run_handler_files()` boundary in this file.

### Discussion / design context
- `Descusion GlassWorm.md` (repo root; 2026-04-19 input from architect). Proposes Unicode invisible-character scanner as kernel security primitive. Full absorption in this session (§2.N + new DESIGN doc + new ROADMAP milestone).
- `docs/design/DESIGN-packages-v04x.md §0.4.2 Cross-File Manifest Validation Pass` — read in full for ICD-0.4.2 authoring ahead of the re-eval.
- `docs/design/DESIGN-quickjs-bridge.md §§4.1 4.2 9.1` — cross-checked against the proposed Layer 2 scan placement (scan runs before `JS_Eval`, inside the existing runtime-pool / coroutine paths; no bridge invariant violated).
- `docs/DEFERRED.md` — five active entries enumerated (two 0.3.3-era entries still pointing at 0.4.3; WS teardown flake carrying from 0.3.4.1; MEMORY_LIMIT classifier watchlist; Drogon batch-abort pattern-match). No new entries added by this session.

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. Surface is narrower than `RE-EVAL-0.3.x.md` (11 items) and `RE-EVAL-0.3.x-arc-closeout.md` (8 items) because only one 3-part milestone shipped since the last re-eval. The novel element is §2.5 — a new-architectural-input that did not exist as a gap until the architect introduced it mid-cadence.

### 2.1 No interface-drift — ICD-0.4.0 four accepted deviations all carry CHANGELOG ratification

CHANGELOG 0.4.0 lines 70–92 enumerate four accepted deviations:

1. `parse()` return shape `struct ParseResult { std::optional<T> value; std::vector<ManifestParseError> messages; }` vs ICD-normative `std::expected<T, ManifestParseError>`.
2. JSON library — `nlohmann::json` for `shareable` / `unknown_fields` vs ICD snippet's `Json::Value`.
3. Severity enum naming — `Severity::ERROR` / `Severity::WARNING` vs ICD's `Error` / `Warning`.
4. CLI test driver — `popen()`-based `TEST_CASE`s inside `validator_test.cpp` vs ICD's `tests/kernel/packages/cli_test.sh` + `add_test`.

Each has an architect-facing rationale recorded in the CHANGELOG. Items 2 and 3 match the kernel's pre-existing conventions (`nlohmann::json` is used throughout; `CapabilityError::INVALID_NAMESPACE` + `EvalErrorKind::SYNTAX_ERROR` precedents). Item 1 is explicitly permitted by ICD-0.4.0 §Appendix's "permits the implementing session to pick between shapes". Item 4 has the "no shell-test infra in the repo" framing that the 0.3.5 N.39 pattern established.

**Resolution:** No ICD amendment. CHANGELOG is authoritative for all four. Follows the 0.3.5 N.39 precedent (`RE-EVAL-0.3.x-arc-closeout.md §2.4`) and the general rule that "ICD text could carry a **Deviation in 0.4.0:** pointer, but it's low-value — the CHANGELOG entry is authoritative and any reader puzzled by the shipped shape can `git log` to the 0.4.0 commit." **No action.**

### 2.2 No missing-test-for-arch-claim

Cross-checked ICD-0.4.0 §Fixture Packages declaration ("Test count target: ~13 fixtures") against `ls tests/fixtures/packages/` — returns 13. Cross-checked ICD-0.4.0 §Milestone Criteria against `grep -c TEST_CASE tests/kernel/packages/validator_test.cpp` — returns 18 (13 fixture + 1 render-JSON + 4 CLI). Both match the CHANGELOG's stated counts. Zero drift on test-count declarations.

### 2.3 No arch-silent-on-code beyond §2.5

Nothing in `src/kernel/packages/` additions is unaccounted for by either an ICD or the CHANGELOG 0.4.0 deviation list. The 0.4.0 scope landed clean — every TU introduced matches ICD-0.4.0 §CI Wiring, every rule on every fixture matches ICD-0.4.0 §Validation Rules.

### 2.4 Observation — `PLINTH_BINARY_PATH` compile definition + `add_dependencies` wire

CHANGELOG 0.4.0 lines 66–68 note: `CMakeLists.txt gains a PLINTH_BINARY_PATH compile definition + add_dependencies(plinth_tests plinth) so the popen-based CLI tests can find the built binary`. This wiring is part of the item-4 deviation (popen instead of shell script) and is mentioned only in CHANGELOG. Follow-up question: should it be called out in ICD-0.4.0 §CI Wiring as part of the deviation's implementation detail?

**Resolution:** No amendment required. The ICD's §CI Wiring describes the test harness at the `add_test` / `plinth_tests` level; the compile-definition mechanism is an implementation detail of the deviated test shape and belongs with the deviation's CHANGELOG entry, not the ICD. **No action.**

### 2.5 Arch-silent-on-architecture-input — GlassWorm class of supply-chain attack not covered by any existing DESIGN or architecture section (NEW category)

A new category extending the `arch-silent-on-code` case. The architecture documents (02-capabilities, 03-data, 05-extensions, 06-frontend) do not mention invisible-Unicode scanning, supply-chain attack vectors, or source-text sanitization. `Descusion GlassWorm.md` (repo root, architect-supplied 2026-04-19) proposes a kernel security primitive — a Unicode invisible-character scanner — that gates three integration layers: package install (0.4.x), QuickJS source load (0.3.x), frontend render (0.6.x).

The gap is load-bearing: v0.3.5's hardening surface already deletes `globalThis.eval` / `globalThis.Function` per ICD-0.3.5 §Security Constraint 5, which closes the in-runtime eval surface — but the upstream `JS_Eval` call sites run host-submitted source verbatim, so a package with invisible-Unicode-smuggled payloads can still execute via `JS_Eval` at extension load time. The package validator (0.4.0) performs structural validation but does not inspect source bytes for encoding-level attacks. Both entry points are wide open.

**Resolution.** Three-part disposition landing in this PR:

1. New DESIGN doc at `docs/design/DESIGN-glassworm-defense-v0x.md` defining the kernel security primitive (Scale-3), the three integration layers, and the scanner contract. Author: this session. Source material: `Descusion GlassWorm.md` + the code-survey of `validator.cpp:474` and the three `JS_Eval` call sites (`eval.cpp:277` / `runtime_pool.cpp:697` / `run_on_context.cpp:816`).
2. New ROADMAP milestone 0.4.1 GlassWorm Unicode defense layer [strong] — reclaims the 0.4.1 slot collapsed into 0.4.0 (see CHANGELOG 0.4.0 line 23–24). Scope: scanner primitive + Layer 1 (package-install gate) + Layer 2 (QuickJS source-load gate). Layer 3 (frontend render) deferred to 0.6.x per the DESIGN §6.2.
3. Forward-ICD-authoring slot (ICD-0.4.1-glassworm-defense) — triggered by the new [strong] band on 0.4.1. Authored ahead of the 0.4.1 code session per METHODOLOGY §3.1 "forward ICD presence check". This session does NOT author ICD-0.4.1; DESIGN §6 is the contract until the ICD lands (precedent: `RE-EVAL-0.3.x-arc-closeout.md §6` declared 0.4.2 "DESIGN is contract until ICD slot fires"). The ICD-0.4.1 slot is the next paper-session trigger after this PR merges.

**Methodology observation** captured for §9 below — this is the project's first `arch-silent-on-architecture-input` finding. Prior RE-EVALs processed new architectural input only through their forward-ICD-presence arm (ICD-0.4.0 / 0.4.1 authoring at arc-closeout); no RE-EVAL has previously absorbed a discussion-file-level architectural primitive as a gap finding. The category is a legitimate extension of `arch-silent-on-code` — the shape of the fix (DESIGN doc + new milestone + deferred ICD) matches existing precedents.

**Fixed in this session.** The DESIGN doc, ROADMAP insertion, and band promotions all land in this PR.

### 2.6 Forward-ICD presence check — ICD-0.4.2 authored this session

Per METHODOLOGY §3.1 *Forward ICD presence check*, pending [strong] milestones in the next-N window need ICDs. The arc-closeout re-eval's §6 table predicted ICD-0.4.2 would be authored "after 0.4.1 merges" — that moment arrived (0.4.0 + 0.4.1 shipped as PR #40 on 2026-04-19), and 0.4.2 enters the next-N window the moment this session promotes it to [strong].

**Resolution.** ICD-0.4.2 authored this session at `docs/icd/ICD-0.4.2-cross-file-manifest-validation.md`. Source contract: `DESIGN-packages-v04x.md §0.4.2 Cross-File Manifest Validation Pass` (lines 148–183). Follows ICD-0.4.0's section template exactly (fixture-directory naming, not alpha groups; filesystem-centric tests). 15 static fixtures + 3 runtime-state fixtures (gated behind `PLINTH_KERNEL_TESTS=ON`). **Fixed in this session.**

### 2.7 Cadence-drift from mid-arc milestone insertion

This session inserts a new 0.4.1 (GlassWorm) milestone mid-arc, shifting the cadence arithmetic for future re-eval slots. Pre-session ROADMAP had `RE-EVAL following 0.4.5   [rewrite session]` (line 48). Counting from this re-eval, four code milestones out is 0.4.1 (new) + 0.4.2 + 0.4.3 + 0.4.4 — not 0.4.5. The existing 0.4.5 slot is one milestone off cadence after the insertion.

**Resolution.** Shift `RE-EVAL following 0.4.5` → `RE-EVAL following 0.4.4` to preserve the 4-milestone cadence. This is an opinionated adjustment — architect may prefer to keep 0.4.5 and note a 5-milestone cycle in this PR's discussion — but the preamble's "every 4 code milestones" rule supports the shift. Flagged for §9 as a methodology observation: mid-arc milestone insertion is a first-in-Plinth pattern and may benefit from a documented rule. **Fixed in this session** (ROADMAP edit). Reversible if architect prefers.

### 2.8 DEFERRED.md entries — zero new from this window, two existing entries cross-referenced

Re-examined `docs/DEFERRED.md` active entries (five):

1. **Per-op `SET search_path` for `db.*`** (2026-04-18; points at ROADMAP 0.4.3). Still live. ROADMAP 0.4.3 exists at `[medium]`. Cross-reference holds.
2. **`db.*` PG-type→JS-type mapping** (2026-04-18; pointer updated 2026-04-19 by arc-closeout §2.8 to ROADMAP 0.4.3). Still live. Cross-reference holds.
3. **WS teardown flake** (`bad_weak_ptr` in `trantor::EventLoop::loop`, 2026-04-19). 6 prior occurrences recorded; 0.4.0 merge CI post-merge state not enumerated in memory (memory is 4 days old per the system reminder). Still live until explicitly resolved.
4. **MEMORY_LIMIT classifier** (2026-04-19; resolved in 0.3.4.1, watchlist). Passive; no action.
5. **Drogon `PgBatchConnection` `SqlError` typing** (2026-04-18; unchanged).

No new DEFERRED entries from this session — 0.4.0 shipped cleanly, the GlassWorm absorption produces a new ROADMAP milestone (not a deferral), and ICD-0.4.2 authoring produces no new deferrals. **No action.**

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.4.0 §Validation Rules R1–R6.** `validator.cpp` runs six rules matching the ICD's rule table. Rule R2's `realpath`-bounded symlink check matches ICD §Security Constraints item 1.
- **ICD-0.4.0 §CLI Contract.** Exit codes 0 / 1 / 2 map to report disposition per the ICD table. `--max-size` / `--json` / `--quiet` flags all present. NO_COLOR env var respected per CHANGELOG line 55.
- **ICD-0.4.0 §Library Surface.** Public types `Severity`, `ValidationMessage`, `ValidationReport`, `ValidationConfig` all present in `validator.hpp` (with the Severity-naming deviation covered in §2.1 item 3).
- **ICD-0.4.1 §Parser surface.** `PackageManifest::parse` + `CapabilityManifest::parse` both shipped; `ManifestParseError` shape matches the ICD's grep-friendly `<file-stem>.<field-path>.<failure-mode>` rule convention.
- **ICD-0.4.0 §Out-of-Scope constraints.** Every "do not foreclose" item (hot-reload, share primitive, sidecar packaging, remote registry) remains preserved — 0.4.0's pure-static-analyzer posture doesn't touch any of them.
- **Cross-arc invariants** (re-checked in light of 0.4.0's wiring): no new threads, no new DB pools, no new Drogon filters, no new global state. `validate()` is pure w.r.t. kernel state per ICD-0.4.0 §Security Constraint 3.

---

## 4. Accepted deviations log — 0.4.0

Consolidated table for the 0.4.x-to-date window. Prior-arc deviations (0.3.x) are consolidated in `RE-EVAL-0.3.x-arc-closeout.md §4` (16 rows); this table does not duplicate them.

| Milestone | Deviation | Rationale | Status |
|---|---|---|---|
| 0.4.0 | `parse()` return shape: `struct ParseResult { std::optional<T> value; std::vector<ManifestParseError> messages; }` vs ICD's `std::expected<T, ManifestParseError>` | ICD §Appendix permits the implementing session to pick between "single top-level with children" and "vector" shapes | ratified |
| 0.4.0 | JSON library: `nlohmann::json` for `shareable` / `unknown_fields` vs ICD snippet's `Json::Value` | Matches kernel convention (`nlohmann::json` used throughout); declared surface semantics unchanged | ratified |
| 0.4.0 | Severity enum naming: `Severity::ERROR` / `Severity::WARNING` vs ICD's `Severity::Error` / `Severity::Warning` | Matches kernel convention (`CapabilityError::INVALID_NAMESPACE`, `EvalErrorKind::SYNTAX_ERROR` precedents) | ratified |
| 0.4.0 | CLI test driver: `popen()`-based `TEST_CASE`s inside `validator_test.cpp` vs ICD's `tests/kernel/packages/cli_test.sh` + `add_test` | No pre-existing shell-test infra in the repo; one-harness discipline reduces wiring surface; functionally equivalent coverage | ratified |

**Observation.** Four deviations across one 3-part milestone (0.4.0). Zero were retracted after shipping. Three of four (items 2, 3, 4) cite pre-existing kernel conventions as the reason for deviation — a signal that the ICD authors (this same session-model's prior passes) were insufficiently calibrated on kernel conventions during ICD-0.4.1's initial draft. Not a failure — ICDs converge on conventions through the shipping feedback loop.

This table is the 0.4.x window's consolidated reference. Future re-evals in the 0.4.x window should append, not rebuild.

---

## 5. Known-issues preserved

Two items cross-referenced from `docs/DEFERRED.md`:

### 5.1 WS teardown flake — `bad_weak_ptr` in `trantor::EventLoop`

Seventh-or-later occurrence status unknown from this session (memory's "6th occurrence" record is 4 days old per the memory system reminder; 0.4.0 merge CI state not verified in-session). Framing unchanged from `RE-EVAL-0.3.x-arc-closeout.md §5.1`: sub-path race fires on post-merge runs independent of scheduled CI; `g_shutdown_pending` + `initiate_shutdown` + `plinth::log::shutdown` bundle holds on the audit and DbClient sub-paths; the trantor `EventLoop::loop` teardown itself remains open.

No new diagnostic data from this session (0.4.0 shipped without QuickJS activity in its tests; the flake signature has never surfaced in package-validator tests). Status parked per architect framing in arc-closeout §5.1. DEFERRED.md entry remains the canonical reference.

### 5.2 MEMORY_LIMIT classifier watchlist

No regression. Entry stays in `DEFERRED.md` as resolved-and-watching per arc-closeout §5.2.

No new DEFERRED entries added by this session.

---

## 6. Forward ICD presence check

Per METHODOLOGY §3.1 *Forward ICD presence check*:

**Next-N window (N=3):**

| Milestone | Band | ICD exists? | Status |
|---|---|---|---|
| 0.4.1 (GlassWorm) | [strong] (after §8) | DESIGN only — `docs/design/DESIGN-glassworm-defense-v0x.md` authored this session | contract-by-pointer (precedent: 0.3.3.1 contract-by-pointer, 0.3.4.1 contract-by-pointer). ICD-0.4.1-glassworm-defense authored in the next docs-session slot ahead of 0.4.1 code. |
| 0.4.2 | [medium] → [strong] after §8 | ✅ `docs/icd/ICD-0.4.2-cross-file-manifest-validation.md` | **authored this session** |
| 0.4.3 | [medium] | ❌ not authored | `DESIGN-packages-v04x.md §0.4.3` is contract until a dedicated ICD slot fires. Per the "one milestone ahead" horizon rule, ICD-0.4.3 is authored after ICD-0.4.1-glassworm-defense (so the docs chain stays at most one ahead of in-flight code). |

**After-N window (milestones 4–7 out, M=7):** 0.4.4, 0.4.5, 0.4.6, 0.4.7. All [medium], all covered by `DESIGN-packages-v04x §§0.4.4–0.4.7`. No action — the horizon rule requires ICDs one ahead, not seven.

**Forward-check result:** No urgent ICD backlog beyond the 0.4.1 DESIGN→ICD step. The standard one-ahead horizon is preserved (ICD-0.4.1 authored before 0.4.1 code ships; ICD-0.4.3 authored before 0.4.3 code ships).

**Special case — 0.4.1 DESIGN-not-ICD posture.** Kernel security primitives historically ship with a DESIGN doc as the primary authoritative contract (DESIGN-quickjs-bridge.md for the async bridge, DESIGN-capability-registry.md for the registry). The 0.3.x arc authored ICDs *from* DESIGN; the same posture applies here. 0.4.1's DESIGN is fully sufficient to block code work until the paired ICD lands, and the contract-by-pointer pattern (precedent: 0.3.3.1) is the accepted stopgap. §9.3 captures this as a methodology observation.

---

## 7. Disposition — what was fixed here vs. scheduled

### Fixed in this PR (doc-only)

1. `docs/design/DESIGN-glassworm-defense-v0x.md` — **new** (§2.5). Kernel security primitive; Unicode invisible-char scanner; three-layer decomposition; Scale-3; milestone 0.4.1 scope.
2. `docs/icd/ICD-0.4.2-cross-file-manifest-validation.md` — **new** (§2.6). Forward-ICD-horizon cleared.
3. `docs/reviews/RE-EVAL-0.4.x.md` — **new** (this file).
4. `docs/ROADMAP.md` — 0.4.0 completed-line trimmed; `RE-EVAL following 0.4.0` line removed (discharged by this session); **new 0.4.1 GlassWorm milestone** inserted at `[strong]`; **0.4.2 promoted to `[strong]`**; `RE-EVAL following 0.4.5` → `RE-EVAL following 0.4.4` (§2.7 cadence-drift correction).
5. `docs/CHANGELOG.md` — **new** Rewrite session entry.

### Scheduled for future sessions (no action this PR)

- **ICD-0.4.1-glassworm-defense authoring.** Next docs-session slot; fires after this PR merges. Content is derivative of `DESIGN-glassworm-defense-v0x.md §3–§6` (scanner contract + three layers + configuration + per-version scope + test fixtures). One-ahead of 0.4.1 code.
- **ICD-0.4.3 authoring.** Paired session with ICD-0.4.1-glassworm-defense (not naturally co-scoped; they land in separate docs sessions if the horizon-rule-one-ahead discipline holds). Alternative: defer ICD-0.4.3 until after 0.4.1 ships, matching the 0.3.x cadence where ICDs landed in paper-session pairs (ICD-0.3.4 + ICD-0.3.5 together). Architect decision at implementation time.
- **Cadence re-eval.** `RE-EVAL following 0.4.4` (adjusted from 0.4.5 by §2.7) remains on ROADMAP per the preamble cadence rule. This session does NOT displace the scheduled one; it is the scheduled one.
- **WS teardown flake dedicated fix.** Not scheduled; architect may pick up when tolerance shifts. Captured in DEFERRED.md §5.1 unchanged.
- **MEMORY_LIMIT flake watchlist entry.** Passive; no scheduled action unless a regression surfaces.

---

## 8. Band label review

Per the Plinth labeling rule and this re-eval's triggers:

| Milestone | Current | New | Rationale |
|---|---|---|---|
| 0.4.1 (GlassWorm) | — (new insertion) | **strong** | DESIGN-glassworm-defense-v0x.md authored this session. Load-bearing for the next 3 milestones (0.4.1 is itself next; 0.4.2's cross-file layer composes downstream; 0.4.3 consumes pre-scanned packages). Kernel security primitive; every extension depends on it structurally. |
| 0.4.2 | medium | **strong** | ICD-0.4.2 authored this session. Load-bearing for 0.4.3+ (cross-file validation is the entry criterion for 0.4.4's VALIDATING stage). |
| 0.4.3 — 0.4.7 | medium | medium | DESIGN coverage sufficient at this horizon; ICDs land in future one-ahead sessions. |
| 0.5.x | medium | medium | Unchanged. |
| 0.6.x | medium | medium | Unchanged. Layer 3 GlassWorm integration is deferred into 0.6.x scope (specifically a new sub-item at 0.6.3 when DESIGN-shell-v06x.md consumes this DESIGN doc). |
| 0.6a-* | fuzzy | fuzzy | Unchanged. |
| 0.7.x – 1.0 | fuzzy | fuzzy | Unchanged. |

**Promotions applied:** 0.4.1 (new; [strong]), 0.4.2 ([medium]→[strong]). No demotions. Landed in §7 item 4.

---

## 9. Observations for methodology / future re-evals

### 9.1 First instance of `arch-silent-on-architecture-input`

§2.5 is the project's first re-eval finding in this extended category. The RE-EVAL-0.2.x.md / RE-EVAL-0.3.x.md / arc-closeout all processed new architectural material only through forward-ICD-presence checks or prior-arc ICD authoring — none absorbed a discussion-level primitive as a gap finding.

The shape of the fix (DESIGN doc + new ROADMAP milestone + deferred ICD) matches existing precedents (`RE-EVAL-0.3.x-arc-closeout §2.1–§2.3` shapes, but in reverse — this one proposes a new DESIGN instead of absorbing drift into an existing one). Future re-evals that encounter similar `Descusion *.md` input files at repo root can follow this pattern:

1. Identify the gap as `arch-silent-on-architecture-input`.
2. Decide scale (Scale-3 kernel primitive; Scale-2 subsystem; Scale-1 local policy).
3. Produce a DESIGN doc in `docs/design/` if Scale-2 or Scale-3; otherwise a direct ICD amendment.
4. Reserve a ROADMAP slot; document the band label.
5. Schedule the forward-ICD authoring.

A METHODOLOGY-level amendment naming the category and the five-step playbook would be light-touch; not required for this session to discharge the gap, but worth a future paper-session if the pattern repeats.

### 9.2 Mid-arc milestone insertion is a first-in-Plinth pattern

§2.7 inserts a new 0.4.1 milestone into an in-flight arc, shifting cadence for downstream re-eval slots. Prior Plinth precedent: the 0.3.X arc inserted follow-ups (0.3.0.1, 0.3.0.2, 0.3.2.1, 0.3.3.1, 0.3.3.2, 0.3.3.3, 0.3.3.4, 0.3.4.1) as four-part un-tagged items, never as new 3-part milestones. This session's 0.4.1 GlassWorm insertion is the first mid-arc 3-part addition.

Observations:
- The re-eval's ROADMAP-cadence-arithmetic shifted naturally (§2.7 RE-EVAL line update).
- The band-label review (§8) handled the new milestone without special-casing.
- The CHANGELOG's per-tag history remains coherent (0.4.0 is tagged; 0.4.1 will tag when GlassWorm ships; no four-part accounting needed).
- The horizon-rule forward-ICD check (§6) exposed one edge: the new milestone ships with a DESIGN doc but no ICD at re-eval time, and the contract-by-pointer posture (precedent: 0.3.3.1) is invoked.

Methodology-level takeaway: mid-arc insertion is architecturally OK if triggered by a new-arch-input gap (§2.5) rather than scope creep. Arc scope-creep (bumping new milestones into a running arc to catch up to ship-pressure) remains a different, worse pattern.

### 9.3 Contract-by-pointer: second instance of the DESIGN-not-ICD variant

The arc-closeout re-eval §9.1 flagged "contract by pointer" as a pattern (4-part follow-ups shipping without their own ICD, relying on the parent ICD + CHANGELOG). This session adds a new variant: a 3-part milestone shipping with DESIGN as the primary contract and an ICD authored subsequently.

Precedent for the variant, outside this session's work:
- DESIGN-quickjs-bridge.md was the contract for ICDs 0.3.0–0.3.5 (each ICD explicitly traces-to DESIGN).
- DESIGN-capability-registry.md was the contract for ICDs 0.2.0–0.2.5.

What's novel is the time-ordering: in the prior cases, DESIGN pre-dated the first ICD by weeks or months (DESIGN-quickjs-bridge 2026-04-14, ICD-0.3.0 slightly after). In this session, DESIGN and the next docs-session ICD are conceptually back-to-back — DESIGN authored today, ICD authored in the immediate-next slot. The contract-by-pointer deferral window is narrower.

Observation: no methodology amendment needed. The pattern is a legitimate variant and scales down gracefully.

### 9.4 Forward-ICD-presence rule is paying off

The 0.3.3.4 METHODOLOGY amendment added the forward-ICD-presence check. This re-eval's §2.6 + §6 are direct products of that amendment — without the rule, 0.4.2 could have started code work with no ICD (mirroring the 0.3.4 pre-amendment drift). The arc-closeout caught 0.3.4; this cadence re-eval catches 0.4.2. Two-for-two signal that the rule is working as designed.

Related: §2.5 is not a forward-ICD-presence miss (the architectural input arrived mid-cadence, not as a scheduled milestone), so it falls outside the rule's coverage. The `arch-silent-on-architecture-input` category (§9.1) is the complementary rule for that class of gap.

---

## 10. Exit criteria

Per METHODOLOGY §3.3:

- [x] Architecture documents read; the GlassWorm gap (§2.5) requires an architecture-doc amendment *in substance* (adding a new kernel security primitive), which is landing as a new DESIGN doc rather than an edit to an existing architecture/*.md file. Future touch: when `DESIGN-glassworm-defense-v0x.md` is consumed by 0.6.x, a sentence in `architecture/05-extensions.md §3.1` referencing the Layer 2 scan would be appropriate (defer to 0.4.1's ICD authoring session as a light edit).
- [x] Band promotions applied (0.4.1 new + [strong], 0.4.2 medium→strong — §8).
- [x] Roadmap milestone labels reviewed; two promotions; completed 0.4.0 line trimmed per preamble rule; RE-EVAL cadence line shifted to match insertion (§7 item 4).
- [x] ICD amendments land in this PR — none required by §2 (zero-drift window for 0.4.0). ICD-0.4.2 is new authoring, not an amendment.
- [x] Forward ICD presence check passed (§6). ICD-0.4.1-glassworm-defense authored in next docs session; ICD-0.4.3 paired per §7.
- [x] Known-issues preserved in durable storage (DEFERRED.md — §5; no new entries).
- [x] Session output committed to the documentation tree (`docs/reviews/RE-EVAL-0.4.x.md` — this file).
- [x] Code untouched. No C++ / CMake / CI-YAML edits.
- [x] CHANGELOG entry for this session. Rewrite-session entry per `CHANGELOG.md` precedent line `## Rewrite session — 2026-04-18 — RE-EVAL following 0.3.3`. **Un-tagged** per 2026-04-18 "3-part X.Y.Z tag only" rule (cadence re-eval is not an arc-closeout, which was v0.3.6's case).

---

## 11. Next actions for the architect

1. **Review this artifact.** Flag any disposition you disagree with. Particularly:
   - §2.5 GlassWorm disposition (new DESIGN doc + new 0.4.1 milestone + deferred ICD-0.4.1-glassworm-defense) — the three architect decisions were locked in the planning phase before this session's authoring; re-confirm any structural concerns now.
   - §2.7 RE-EVAL line shift (`following 0.4.5` → `following 0.4.4`) — reversible if you prefer the 5-milestone cycle.
   - §9.1 methodology observation (new category `arch-silent-on-architecture-input`) — worth a future paper-session METHODOLOGY amendment, or leave it organic in the re-eval log?
2. **Approve the DESIGN-glassworm-defense-v0x §8 Open Questions.** Six questions (threshold default, BOM policy, call-depth ordering, audit rate-limit, normalization skip, disabled-state audit event) — most pick defaults, but explicit approval in the PR body moves them from "open" to "resolved at PR time" per the 0.3.5 Open Questions ratification pattern.
3. **Confirm 0.4.1 code session entry conditions.** Per ROADMAP, 0.4.1 GlassWorm needs (a) this PR merged, (b) ICD-0.4.1-glassworm-defense authored in the next docs session, then code work begins.
4. **Confirm next cadence re-eval position.** Counting from this one: `RE-EVAL following 0.4.4` at 4/4 cadence (0.4.1, 0.4.2, 0.4.3, 0.4.4). §2.7 applies this adjustment; architect can revert if preferred.
5. **No tag on this session** per the 2026-04-18 "3-part X.Y.Z tag only" rule. CHANGELOG Rewrite-session entry is the ledger.
