# Re-evaluation — 0.4.X arc close-out

**Date:** 2026-04-22
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3) — arc-closeout, not cadence
**Trigger:** Scheduled ROADMAP item `RE-EVAL following 0.4.7 (0.4.x arc closeout)   [rewrite session]` — the only remaining entry in `## 0.4 — Package System`. Per ROADMAP preamble, no further code milestone begins until this re-eval lands.
**Cadence position:** Sixth scheduled re-evaluation. Cadence is `every 4 code milestones`; 3-part milestones since the last cadence re-eval (`RE-EVAL following 0.4.4`, 2026-04-20, `RE-EVAL-0.4.x-following-0.4.4.md`) are: **0.4.5** (lifecycle transitions + atomic swap + GC), **0.4.6** (RBAC rule registration / Phase A), **0.4.7** (RBAC Phase B test execution). Three 3-part code milestones; arc-closeout is the natural extra trigger that runs the seventh slot ahead of the formal 4/4 cadence (precedent: `RE-EVAL-0.3.x-arc-closeout.md` opening, fired at 2/4 because 0.3.5 closed the 0.3.X arc). Includes the parallel **LH-0** (load-harness scaffold) + **LH-0.1** (async-bridge stress) work and the four-part follow-ups **0.4.5.1** (test-strategy redesign), **0.4.5.2** (ICD-0.4.6 authored), **0.4.6.1** (ICD-0.4.7 authored).
**Scope:** Code-aware gap analysis across the 0.4.5–0.4.7 window plus the LH-0/LH-0.1 stream. Prior-arc work (0.4.0–0.4.4) was covered by `RE-EVAL-0.4.x.md` and `RE-EVAL-0.4.x-following-0.4.4.md`; this session does not re-open it. Closes the 0.4.X arc and stages the 0.5.x realtime arc.

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index-only scan).
- `docs/architecture/01-identity.md §2 Groups and RBAC; §2.1 RBAC Rules; §2.4 Rule Lifecycle` — normative source for 0.4.5 disable-orphans / uninstall-deletes, 0.4.6's typed `rbac.json` parser, 0.4.7's Phase B test-contract handling, and the `orphaned_at` semantics that span all three milestones.
- `docs/architecture/02-capabilities.md §1 Capability Registry; §3 RBAC Integration` — normative for 0.4.5's `unregister_capability` symmetry with `register_capability`, 0.4.7's `call_capability`-via-detached-thread dispatch in Phase B, and the LH-0 `lh0:1:chain` Tier 1 registration.
- `docs/architecture/03-data.md §2 plinth.packages; §3 LISTEN/NOTIFY` — schema-freeze posture (relevant to the 0.4.5 `SUPERSEDED`/`retired_at`/`supersedes_id` additions, the 0.4.6 `rbac_rules.test_contract JSONB` column, and the 0.4.7 `users.is_test_user` column landing pre-freeze under the 0.6.x cutover rule).
- `docs/architecture/04-services-ha.md §1 Audit logging` — normative for the four new audit-event families introduced in this window (lifecycle transitions, RBAC rule registration, Phase B execution, GC retire).
- `docs/architecture/05-extensions.md §1 Package Structure; §2 Reserved URL Prefixes; §3 QuickJS Runtime` — RT1/RT2 runtime-state queries during upgrade validation; consumed unchanged.

### ICDs
- `docs/icd/ICD-0.4.5-package-lifecycle-transitions.md` — 0.4.5's contract; implemented across Slice A (#53) + Slice B (#54).
- `docs/icd/ICD-0.4.6-rbac-rule-registration.md` — 0.4.6's contract; implemented as PR #59.
- `docs/icd/ICD-0.4.7-rbac-test-execution.md` — 0.4.7's contract; implemented across Slices A/B/C (#61/#62/#63).
- `docs/icd/ICD-LH-0-load-harness-scaffold.md` — LH-0's contract; implemented on `feat/lh-0-load-harness-scaffold` (untagged per LH-stream convention).
- `docs/icd/ICD-LH-0.1-async-bridge-stress.md` — LH-0.1's contract; implemented on `feat/lh-0.1-async-bridge-stress` (untagged).
- `docs/icd/ICD-0.4.4-package-install-lifecycle.md` — consulted for the 0.4.5 trigger-site reuse (`install_package` + `reconcile_in_flight_installs`).
- `docs/icd/ICD-0.4.1-manifest-parsing.md` — consulted for the 0.4.6 parser shape mirror (`RbacManifest::parse` ↔ `CapabilityManifest::parse`).
- `docs/icd/ICD-0.2.4-capability-rbac.md` — consulted for the 0.4.7 dispatch path (`call_capability` is the synchronous entry; deviation rationale derives from this).

### Code (spot-verified against ICD §Library Surface / §HTTP Surface text for every shipped TU in the window)
- `src/kernel/packages/install_lifecycle.{hpp,cpp}` — `disable_package` / `enable_package` / `uninstall_package` / `upgrade_package` + `garbage_collect_superseded_versions` all public, `reconcile_in_flight_installs` extended with UNINSTALLING + mid-swap branches; three Phase B trigger sites (`:1583`, `:1940`, `:3124`) confirmed.
- `src/kernel/packages/rbac_test_runner.{hpp,cpp}` — **shipped with file rename from ICD-0.4.7's `phase_b.{hpp,cpp}`** (see §2.1). `run_phase_b` + `schedule_phase_b_detached` + `run_cli_test_rbac` all public; `PhaseBReport` / `PhaseBFailure` shapes match ICD.
- `src/kernel/packages/reserved_names.hpp` — present; `RESERVED_NAMES` promoted out of `cross_file_validator.cpp`'s anon namespace per ICD-0.4.6 OQ5.
- `src/kernel/rbac/rbac_manifest.{hpp,cpp}` — `parse_rbac_manifest` returning `RbacManifestParseResult{value?, messages}` matches ICD-0.4.6 §RbacManifest — C++ surface verbatim.
- `src/kernel/rbac/rule_validator.{hpp,cpp}` — **shipped with file rename from ICD-0.4.6's `phase_a.{hpp,cpp}`** (see §2.1). `validate_phase_a` is the public entry; rule-IDs A.1–A.5 all match.
- `src/kernel/rbac/ephemeral_user.{hpp,cpp}` — `create_run_users` / `destroy_run_users` / `cleanup_orphaned_test_users` / `build_test_user_context` all public; matches ICD-0.4.7 §Ephemeral-user factory.
- `src/kernel/rbac/rule_registrar.{hpp,cpp}` — gained `mark/clear_extension_rules_orphaned`, `delete_extension_rules`, `fetch_extension_rules_for_phase_b`; `upsert_extension_rule` extended with the additive `std::optional<nlohmann::json> test_contract` parameter per ICD-0.4.6.
- `src/kernel/capabilities/registration.{hpp,cpp}` — `unregister_capability(ns, version, function, PGconn&)` shipped here for symmetry with `register_capability_tx` (ICD-0.4.5 said `resolution.{hpp,cpp}` — see §2.1).
- `src/kernel/capabilities/drain.{hpp,cpp}` — `DispatchGuard` + per-extension-name in-flight counter + `wait_for_zero` shipped per ICD-0.4.5 §Atomic Swap T1/T2.
- `src/kernel/ws/call_dispatch.{hpp,cpp}` — WS `call`/`call_result`/`call_error` message types per ICD-LH-0 §3.
- `src/kernel/ws/js_stress.{hpp,cpp}` — process-lifetime `RuntimePool` + `try_dispatch_js_stress` per ICD-LH-0.1 §3 + §4.
- `src/kernel/capabilities/resolution.cpp` — `lh0_chain_handler` + `register_lh0_harness_handlers_locked` called from `init_resolver`; LH-0's Tier 1 registration confirmed.
- `src/kernel/main.cpp` — `validate_cmd` + `test_cmd→rbac_cmd` two-level argparse nest matches ICD-0.4.7 §CLI Surface; `init_js_stress_pool` + `shutdown_js_stress_pool` lifecycle wired into atexit chain per ICD-LH-0.1 §5.
- `migrations/schema.sql` — `SUPERSEDED` state + `retired_at` + `supersedes_id` + `idx_packages_supersedes` from 0.4.5; `rbac_rules.test_contract JSONB` from 0.4.6; `users.is_test_user BOOLEAN` + `idx_users_is_test_user` partial index from 0.4.7. All present.
- `tests/kernel/rbac/{rbac_manifest_test,phase_a_test,ephemeral_user_test}.cpp`, `tests/kernel/packages/{lifecycle_transitions_test,phase_b_test,phase_b_report_test}.cpp`, `tests/kernel/main/cli_test_rbac_test.cpp` — Catch2 case counts match the per-milestone CHANGELOG totals.

### Discussion / design context
- `docs/design/DESIGN-packages-v04x.md §0.4.5 / §0.4.6 / §0.4.7 / §8 atomic swap / §4.1 plinth.packages columns` — read in full for cross-checking the three milestones' implementations against the design contract.
- `docs/design/DESIGN-rbac-philosophy.md` — consulted for 0.4.7 Phase B's "deny-by-default after grant audit" framing.
- `docs/DEFERRED.md` — six active entries enumerated (§5).
- `project_ws_flaky_segfault.md` (memory) — historical context for the WS teardown bandaid ladder; LH-0.1 supplies the empirical resolution (§2.4).
- `project_next_session_lh0.md` (memory) — LH-0.1's diagnostic outcome consumed verbatim into §2.4.

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. The window holds the largest set
of substantive gaps in any 0.4.x re-eval to date — five categorized
items + one zero-gap-confirmation that doubles as the diagnostic
empirical ground for §2.4. Three of the five are direct ICD-amendment
candidates landing in this PR; the other two trigger ROADMAP / DEFERRED
edits.

### 2.1 Interface-drift — ICD-0.4.6 + ICD-0.4.7 reference renamed file paths

ICD-0.4.6 §Library surface changes (lines 123, 127, 134, 384, 631, 641,
694, 764–766, 831) names the new TUs as `src/kernel/rbac/rbac_manifest.{hpp,cpp}`
+ `src/kernel/rbac/phase_a.{hpp,cpp}`. ICD-0.4.7 §Phase B driver
(lines 132, 376, 847, 851, 959, 1130) names the new TU as
`src/kernel/packages/phase_b.{hpp,cpp}`. The shipped 0.4.7 PR renamed
both: `phase_a.{hpp,cpp}` → `rule_validator.{hpp,cpp}` (pairs
symmetrically with the existing `rule_registrar` in the same directory)
and `phase_b.{hpp,cpp}` → `rbac_test_runner.{hpp,cpp}` (describes what
the TU does rather than which DESIGN phase it implements). Test
directories and CMake targets followed. The CHANGELOG v0.4.7 entry
flags the rename as the last "Deviations" bullet and explicitly punts
the ICD update to RE-EVAL ("Types/namespaces/methods stay for RE-EVAL
discussion").

The deviation is load-bearing — both ICDs are full-text contracts that
a fresh session will grep for "phase_a.cpp" / "phase_b.cpp" and find
files that don't exist. Per METHODOLOGY §Phase 2 Constraint #4, the
ICD owns the deviation record, not just the CHANGELOG.

**Resolution.** Two ICD amendments land in this PR:

1. ICD-0.4.6 — new "Implementation deviation (0.4.6 file rename)"
   subsection ahead of §Library surface changes naming the rename
   `phase_a.{hpp,cpp}` → `rule_validator.{hpp,cpp}` and the rationale
   (symmetry with `rule_registrar`; description of what the TU does
   rather than which DESIGN phase it implements). Public types,
   namespaces, methods, audit kinds, schema columns, and Catch2 tags
   stay locked into the public surface — only the on-disk filename
   changed. Inline references throughout the ICD are unchanged; the
   subsection is the durable record.
2. ICD-0.4.7 — symmetric "Implementation deviation (0.4.7 file rename)"
   subsection naming the `phase_b.{hpp,cpp}` → `rbac_test_runner.{hpp,cpp}`
   rename. Same rationale shape; same scope (file paths only).

Pattern matches `RE-EVAL-0.3.x-arc-closeout.md §2.3` (`check_result_size`
placement amendment to ICD-0.3.5). **Fixed in this session.**

### 2.2 Interface-drift — ICD-0.4.5 places `unregister_capability` in `resolution.{hpp,cpp}`; shipped in `registration.{hpp,cpp}`

ICD-0.4.5 §Library surface narration (line 5 of the Related list) names
the new symbol's home as `src/kernel/capabilities/resolution.{hpp,cpp}`.
Slice A shipped it in `src/kernel/capabilities/registration.{hpp,cpp}`
for symmetry with the existing `register_capability_tx` — the v0.4.5
CHANGELOG flags this as the only Slice A interface-drift item. The
public symbol name (`unregister_capability`) and signature
(`(namespace, version, function, PGconn&) -> std::expected<void, ...>`)
are unchanged; only the header file housing the declaration moved.

Severity is lower than §2.1 — Slice A's CHANGELOG row explicitly named
the alternate location, and `git log` on the symbol resolves cleanly
— but it's the same drift class. ICD readers grepping for the symbol
in `resolution.{hpp,cpp}` find nothing.

**Resolution.** ICD-0.4.5 — one-line "Implementation deviation (0.4.5
file placement)" note on the `unregister_capability` line in §Library
surface naming `registration.{hpp,cpp}` as the actual home with the
symmetry rationale. Lower-effort amendment than §2.1 (single line, no
new subsection). **Fixed in this session.**

### 2.3 Arch-silent-on-code — `src/kernel/capabilities/drain.{hpp,cpp}` is a new kernel primitive without an architecture-doc entry

0.4.5 Slice B introduced `src/kernel/capabilities/drain.{hpp,cpp}`
(per-extension-name in-flight counter + condvar `wait_for_zero` +
`DispatchGuard` spliced into `call_capability` after `parse_signature`)
to support the atomic-swap drain phase (T1/T2). The file is real
kernel infrastructure — every capability dispatch now passes through
a `DispatchGuard` constructor that performs a relaxed atomic load on
`g_active_count` (hot path) and increments only when a drain is
active. Test coverage is solid (8 unit cases) and the ICD-0.4.5 §State
Machine narration covers the T1/T2 ordering, but the architecture doc
itself (`02-capabilities.md`) does not mention drain as a primitive.

The primitive is structurally important — it sits between every
capability call and the dispatcher, and any future code session
authoring a new dispatcher path (Tier 3 sidecar, async wrapper, etc.)
will need to know about it. Today they would have to discover it by
reading `resolution.cpp`.

**Resolution.** New paragraph appended to `architecture/02-capabilities.md
§3 RBAC Integration` (or a small new §3.5 subsection — judgment call
at edit time) describing `DispatchGuard` as a per-extension-name
in-flight counter consumed by `upgrade_package` and (eventually)
`uninstall_package`'s drain phases, with the contract that the hot
path is a relaxed atomic load when no drain is active. **Fixed in
this session.**

### 2.4 Arch-silent-on-code — LH-stream is a new parallel test framework that the architecture tree does not describe

LH-0 + LH-0.1 introduced an entire parallel test framework (`load-harness/`
top-level directory, Go binary, no CMake coupling, deliberately not
linked against `plinth` or `plinth_tests`). The kernel surface added
on its behalf is real:

- `src/kernel/ws/call_dispatch.{hpp,cpp}` — WS `call`/`call_result`/
  `call_error` message types reused for any future "RPC-over-WS"
  pattern.
- `src/kernel/ws/js_stress.{hpp,cpp}` — process-lifetime RuntimePool
  + dispatch fork in `on_call` for the `lh0:1:js_stress` signature.
- `src/kernel/capabilities/resolution.cpp` — `register_lh0_harness_handlers_locked`
  registers `lh0:1:chain` as a kernel Tier 1 capability.

ICD-LH-0 §4 + ICD-LH-0.1 §4 both explicitly declare the `js_stress`
dispatch fork as a "diagnostic-only deviation, not a blueprint for
extension dispatch." That declaration lives in the LH ICDs only —
the kernel architecture tree (`02-capabilities.md`) does not record
that the dispatch fork exists, which means a future code session
auditing `ws/call_dispatch.cpp::on_call` for "why is there a special
case here?" would reasonably be confused.

The ROADMAP already absorbed LH-0 / LH-0.1 (the 2026-04-20 re-eval
inserted the parallel `## Load Harness` section and bumped LH-0 to
`[strong]`). The architecture-tree gap is the unfinished half — this
re-eval closes it.

**Resolution.** Two-step doc absorption:

1. New paragraph in `architecture/02-capabilities.md` §3 (or a new
   subsection) noting that `lh0:1:chain` is a kernel Tier 1
   capability registered for load-harness use, RBAC-gated by
   `kernel.admin`, and is the canonical pattern for "diagnostic-only
   internal capabilities" — distinct from extension-provided
   capabilities.
2. New paragraph or subsection in the same architecture file (or in
   `04-services-ha.md` if it fits better at edit time) documenting
   the `js_stress` dispatch fork as a deliberate diagnostic-only
   deviation, with an explicit "not a blueprint" framing copied from
   ICD-LH-0.1 §4. Pin the deviation to the LH stream so future
   sessions know it does NOT generalize to extension dispatch.

Empirical ground for the absorption: LH-0.1's 3-trial run on
2026-04-21 (133,755 `js_stress` calls / ~535,020 `db.query` ops /
~535k `signal_completion` callbacks) reproduced **zero**
`free_zero_refcount`, `list_empty(&rt->gc_obj_list)`, or
`bad_weak_ptr` against the production kernel. Per ICD-LH-0.1 §9.2,
this is a meaningful data point: the production kernel lifecycle
tolerates the path the Catch2 subprocess model trips on. The
architectural narrative (LH-stream is a stress + diagnostic
framework distinct from the Catch2 harness) needs the doc trail.
**Fixed in this session.**

### 2.5 Arch-silent-on-code — `0.4.5.1` test-strategy redesign reorganized ctest topology without an architecture-doc trail

0.4.5.1 (the `0.4.4.1`-from-prior-re-eval, finally shipped on
2026-04-21 as PR #57) restructured CTest from `catch_discover_tests`
494-subprocess model to **4 grouped entries + 45 per-TEST_CASE
entries = 49 subprocesses**. The grouping topology
(`plinth_tests_pure` / `_js` / `_pg` / `_ws`) is a structural
decision: future test additions need to know which group their
TEST_CASE belongs in, which Catch2 tags route into which group,
and which fixture (`ensure_drogon_running` vs
`ensure_drogon_with_db_running` vs neither) is correct for their
group. The CHANGELOG entry covers the immediate "what shipped"
narrative; no doc captures the durable convention.

The `[js][async]` exception (45 cases stayed per-TEST_CASE) is
also a load-bearing point: the residual race has not been
root-caused (cf. `project_ws_flaky_segfault.md §Candidate root
causes`), so future contributors adding `[js][async]` cases need
to know they will land in the per-TEST_CASE bucket and not in
the `_js` group.

**Resolution.** New short subsection in `SESSION-GUIDE.md` (the
project's contributor-facing doc) titled "Test grouping
convention (0.4.5.1)" naming the four groups, the tag-routing
matrix, the fixture mapping, and the `[js][async]` exception
with a pointer to `project_ws_flaky_segfault.md` for the open
investigation. SESSION-GUIDE.md is the right home (not the
architecture tree) — this is contributor-onboarding information,
not a system-design constraint. **Fixed in this session.**

### 2.6 Forward-ICD presence check — none of 0.5.0 / 0.5.1 / 0.5.2 has an ICD

Per METHODOLOGY §3.1 *Forward ICD presence check*, every pending
`[strong]` milestone in the next-N=3 window needs an ICD before
code work begins. Today none of 0.5.0 / 0.5.1 / 0.5.2 has one —
all three are `[medium]` per ROADMAP §0.5. Architecture coverage
is at the `architecture/03-data.md §3 LISTEN/NOTIFY` and
`04-services-ha.md` level (DESIGN-style, but not pinned into an
ICD).

The horizon rule (one ahead of in-flight code) requires ICD-0.5.0
before 0.5.0 starts. With this re-eval discharging the
`RE-EVAL following 0.4.7` slot, 0.5.0 is the next code milestone
that becomes eligible — so ICD-0.5.0 must exist before that
session opens.

**Resolution.** Schedule a dedicated `0.4.7.2 ICD-0.5.0 authored`
docs session ahead of the 0.5.0 code session. Matches the recent
0.4.x cadence pattern (each ICD got its own dedicated 4-part
docs slot: 0.4.4.2 → ICD-0.4.5; 0.4.5.2 → ICD-0.4.6; 0.4.6.1 →
ICD-0.4.7). Not authored in this re-eval because (a) keeps this
re-eval's scope clean — code-aware gap analysis already produces
five amendments; piling a fifth ICD on top would dilute the
focus, and (b) ICD-0.5.0 needs its own architecture pre-read of
`03-data.md §3` + `04-services-ha.md` and a deliberate
DESIGN-vs-ICD authoring pass that fits naturally into a
dedicated paper session.

0.5.1 and 0.5.2 stay `[medium]` after this re-eval; their ICDs
land in subsequent X.Y.Z.N slots one-ahead of their respective
code sessions (`0.5.0.1 ICD-0.5.1 authored`, etc.). The 0.5.x
arc thus follows the same cadence as the 0.4.x arc on this
axis. **Scheduled — does not land in this PR.**

### 2.7 Observation — `ACTIVE_FLAGGED` enum vs schema CHECK consistency (re-noted)

`RE-EVAL-0.4.x-following-0.4.4.md §2.3` flagged a prose-only
inconsistency in ICD-0.4.4 line 39 (`InstallStage` enum has 8
values vs schema CHECK's 11). 0.4.7 introduces the first writer of
`ACTIVE_FLAGGED` (any-fail Phase B run), so the C++ enum is
extended in this window — but the ICD-0.4.4 prose remains
untouched. The drift is now smaller (the enum has acquired the
state) but the ICD line still reads as if `ACTIVE_FLAGGED` is
"reserved, never entered." **No action** — too minor to amend;
recorded here so the next re-eval reader knows this is a
known-and-dismissed pattern. Future ICD-0.4.4 touches should
clean it up incidentally.

### 2.8 No missing-test-for-arch-claim

Cross-checked:

- **ICD-0.4.5 D.* / U.* / X.* / G.* test matrix.** ICD enumerates
  ~25 cases; CHANGELOG v0.4.5 §B10 deferred ten X/G cases
  (X.05/X.06/X.07/X.08/X.09/X.10/X.11/X.12/X.13 + G.03) to a
  follow-up. The deferred set is captured in CHANGELOG but
  **not** in DEFERRED.md — same pattern as `RE-EVAL-0.4.x-following-
  0.4.4.md §2.1` for I.18/I.19/I.20. Should land as a DEFERRED.md
  entry. See §7 disposition.
- **ICD-0.4.6 P.* / M.* / N.* test matrix.** ICD pins 12 cases;
  CHANGELOG v0.4.6 reports 20 cases shipped (11 pure + 9 PG-gated,
  exceeding the ICD floor by 8 — additional positive coverage).
  Zero drift on counts, zero deferrals.
- **ICD-0.4.7 B.* / PB.* test matrix.** ICD pins 23; CHANGELOG
  v0.4.7 reports 24 (added PB.07b + PB.07c CLI edge cases).
  Zero drift, exceeds the floor.
- **ICD-LH-0 + ICD-LH-0.1.** Both ICDs declare 4 Catch2 cases
  (call shape + RBAC + error paths) plus the harness-side smoke
  output as the ship gate. Both delivered the 4 cases plus the
  3-trial / 1-trial runs respectively. Zero drift.

### 2.9 No interface-drift beyond §§2.1 + 2.2

Re-reading every CHANGELOG deviation row in the window against the
shipped ICDs and the shipped code: every other deviation is
either ratified in-CHANGELOG with a stable-symbol-name posture,
permitted by the ICD's §Appendix latitude, or a forced
consequence of an external constraint. Only the §2.1 + §2.2 file
relocations require ICD amendments. Full consolidated catalog in
§4.

### 2.10 DEFERRED.md divergence — one missing entry, one stale entry

Re-examined `docs/DEFERRED.md` §Active entries (six):

1. **I.18/I.19/I.20** (2026-04-20) — still active. 0.4.5 added
   PATCH/DELETE handlers but did not retrofit a Drogon HTTP
   fixture; 0.4.6 was paper-only on the rules side; 0.4.7 added
   the `plinth test rbac` CLI subcommand which doesn't touch the
   HTTP surface. Cross-reference holds (the entry already names
   "0.4.5's disable/enable/uninstall paths, 0.4.6's RBAC
   registration, or a dedicated HTTP-test-fixture PR" as the
   pickup candidates; none of those happened). **No change.**
2. **Per-op `SET search_path` for `db.*`** — points at 0.5.x.
   0.5.0 opens the picking-up window. **No change** (the entry
   becomes load-bearing the moment ICD-0.5.0 is authored).
3. **`db.*` PG-type→JS-type mapping** — same 0.5.x pointer.
   Same posture. **No change.**
4. **WS teardown flake** — entry is materially STALE in light
   of LH-0.1's empirical resolution. The architect's "test-only,
   not production" hypothesis (RE-EVAL §2.2 from prior cycle)
   is no longer a hypothesis — it's an empirical finding. The
   DEFERRED entry should reflect this. **Update needed**
   (§7 disposition).
5. **MEMORY_LIMIT classifier** — watchlist, no regression.
   **No change.**
6. **Drogon `PgBatchConnection` `SqlError` typing** — unchanged.
   **No change.**

**Addition needed:** new entry for the ICD-0.4.5 deferred X.* / G.*
test cases (per §2.8). Lands in §7 disposition.

### 2.11 Decision-embedded-in-CHANGELOG — naming technical debt + deferred-test debt need ROADMAP slots, not just DEFERRED.md entries

Architect signal during this re-eval (2026-04-22): the v0.4.7 file
renames caught the on-disk paths, but the residual `phase_a` /
`phase_b` jargon throughout namespaces, types, methods, helper
functions, audit-event strings, schema columns, and Catch2 tags is
unaddressed technical debt. v0.4.7 CHANGELOG explicitly punted
("Types/namespaces/methods stay for RE-EVAL discussion"); §2.1 of
this re-eval amended the ICDs to record the file-rename half but
did not surface the rest of the debt as ROADMAP-visible work.

Code-aware confirmation of scope (grep walk over `src/kernel/` +
`tests/kernel/` + `migrations/schema.sql`):

- **Pure internal** (clean rename, no external observers):
  `namespace plinth::packages::phase_b`, `validate_phase_a` (in
  `plinth::rbac`), types `PhaseBReport` / `PhaseBFailure`, methods
  `run_phase_b` / `schedule_phase_b_detached` /
  `fetch_extension_rules_for_phase_b` / `emit_phase_b_audit` /
  `wait_phase_b_settled` (test helper), Catch2 tags `[phase_b]` /
  `[phase_b_report]` / `[rbac_phase_a]`, fixture-target name
  `plinth_rbac_test_runner_fixture_zips` (already renamed),
  test TUs (already renamed). Estimated ~1 session for the full
  pass.
- **Public-surface** (need a separate decision on whether and how
  to migrate): audit-event strings `packages.phase_b_passed` /
  `_failed` (external consumers grep on these), schema columns
  `plinth.packages.last_phase_b_run_at` / `last_phase_b_result`
  (persisted DB state in any installed PG carrying 0.4.7+ rows).
  Pre-0.7 schema-freeze still allows direct edit to the schema
  side, but the audit-string side touches every existing log
  consumer and benefits from at least an explicit notice.

Companion observation: the existing DEFERRED.md entries
(I.18/I.19/I.20 for HTTP test harness; X.05–X.13 + G.03 for
extended upgrade/GC coverage; the residual `[js][async]` Catch2
race) are in the source-of-truth log but do not appear on the
ROADMAP. A fresh session reading the ROADMAP top-to-bottom does
not see them. The architect's framing is right: deferred items
deserve scheduled placement, not just the DEFERRED.md ledger.

**Resolution.** New `## 0.4.x cleanup follow-ups (cross-cutting;
scheduled between milestones)` section in the ROADMAP, ahead of
`## Load Harness`, with four scheduled items:

1. **`0.4.7.3 phase_a / phase_b internal-symbol rename`** `[strong]`
   — pure internal rename described above; estimated ~1 session.
   Schedule before 0.5.0 if time permits, otherwise as 0.5.0.1.
2. **`0.4.7.4 Public-surface phase_b rename + migration plan`**
   `[medium]` — decision document for the audit + schema items;
   if "rename" wins, the migration ships as a discrete follow-up;
   if "leave as `phase_b`" wins, the slot closes with the
   resolution recorded.
3. **`0.5.x.N HTTP test harness for /api/packages`** `[strong]`
   — picks up I.18/I.19/I.20 + X.05–X.13 + G.03 against a new
   `tests/kernel/packages/http_test_fixture.{hpp,cpp}`. Schedule
   alongside the first 0.5.x milestone that adds new HTTP surface.
4. **`0.5.x.N [js][async] kernel-side refcount investigation`**
   `[medium]` — root-cause the residual Catch2 sequential-run race;
   LH-0.1 confirmed production unaffected. Win condition: enable
   `[js][async]` grouping in 0.4.5.1's ctest model.

DEFERRED.md entries for I.18/I.19/I.20, X.05–X.13/G.03, and the
WS-teardown `[js][async]` residual are updated to point at the new
ROADMAP slots (§7 disposition).

Methodology observation captured for §9 below — the "deferred
items live only in DEFERRED.md" pattern is itself a small drift
class. The DEFERRED.md preamble already says "single running log
of design decisions that were intentionally deferred"; what it
does not say is "every entry that needs scheduling gets a
ROADMAP slot pointing back here." This re-eval's §2.11 closes
the gap for the 0.4.x arc; future arcs benefit from a
methodology-level companion rule.

**Fixed in this session** (ROADMAP edit + DEFERRED.md cross-
reference updates).

### 2.12 No cadence-drift

The 4-milestone cadence held cleanly. Prior re-eval discharged at
0.4.4; this arc-closeout fires at 3/4 (0.4.5 + 0.4.6 + 0.4.7) per
the arc-boundary trigger (precedent: `RE-EVAL-0.3.x-arc-closeout.md`
fired at 2/4 because 0.3.5 closed the 0.3.X arc). The next
cadence slot lands at `RE-EVAL following 0.5.1` (already on
ROADMAP) — see §8 for band-slide implications.

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.4.5 §State Machine.** Six-phase upgrade choreography
  (T0 ACTIVATING declares intent → T1 begin_drain → T2 wait_for_zero
  with timeout → T3 single PG tx + symlink rename → T4 route +
  capability cutover → T5 retention timer owned by 0.7.x scheduler)
  matches `install_lifecycle.cpp::upgrade_package`. The
  `supersedes_id` FK with `ON DELETE SET NULL` is in
  `migrations/schema.sql`; partial index `idx_packages_supersedes`
  on `supersedes_id IS NOT NULL` confirmed.
- **ICD-0.4.5 §RBAC Reconciliation on Upgrade.** Three-case set-diff
  by namespaced `rule` string (new in v2 → INSERT; in v1 only →
  `orphaned_at = NOW()`; in both → UPDATE description / test_contract /
  extension_name in place; clear `orphaned_at` on v2 rules)
  implemented in `reconcile_rbac_on_upgrade` matches the ICD verbatim.
- **ICD-0.4.5 §Crash Recovery.** UNINSTALLING + SUPERSEDED-orphan +
  ACTIVATING-with-supersedes_id cases all handled by
  `reconcile_in_flight_installs` extensions. `resolve_upgrade_mid_swap`
  helper covers the predecessor-gone / predecessor-ACTIVE /
  predecessor-SUPERSEDED branches per ICD §Crash Recovery flowchart.
- **ICD-0.4.5 §Garbage Collection.** Pure `is_gc_eligible(retired_at,
  now, retention)` predicate + non-blocking `pg_try_advisory_xact_lock`
  per row + DELETE then `fs::remove_all({data_dir}/extensions/{name}/
  {version})` matches §GC Contract. FK auto-NULLs `supersedes_id` on
  the child ACTIVE row; verified in test G.02.
- **ICD-0.4.6 §Phase A Validation Rules A.1–A.5.** All five rules
  shipped with the declared rule-IDs; rule A.5 (PG collision check)
  is the only PG-coupled rule and is gated correctly.
- **ICD-0.4.6 §upsert_extension_rule extended signature.** Additive
  `std::optional<nlohmann::json> test_contract` parameter; INSERT
  becomes 5→6 columns with `$5::jsonb` cast at the server; UPDATE
  gains `test_contract = EXCLUDED.test_contract`; conflict target
  unchanged. All match.
- **ICD-0.4.6 §rbac.json shape (permanent contract).** Stable
  `rbac.*` error-code prefix disjoint from `manifest.*` /
  `capabilities.*` / `cross-file.*` (established in 0.4.0/0.4.1)
  matches the parser's emitted codes.
- **ICD-0.4.7 §Phase B driver.** `run_phase_b` synchronous entry +
  `schedule_phase_b_detached` jthread scheduler shipped per
  contract; per-rule wall-clock bounded by
  `ctx.upgrade_drain_timeout_ms{5000}` per OQ4. Audit emission
  + state update inside the same advisory-lock window matches
  ICD §Crash Recovery requirement.
- **ICD-0.4.7 §Trigger Sites.** All three (`:1583` post-install,
  `:1940` post-enable, `:3124` post-upgrade) call
  `schedule_phase_b_detached` post-COMMIT — confirmed against
  `install_lifecycle.cpp` with the offsets named in the CHANGELOG
  v0.4.7 entry.
- **ICD-0.4.7 §Reconciler extension.** `cleanup_orphaned_test_users(NOW()
  - 1h, conn)` once per `reconcile_in_flight_installs` invocation;
  per-package Phase B rescheduling for `last_phase_b_run_at IS NULL
  AND installed_at > NOW() - interval '1 hour'`. Older NULL rows
  stay NULL per ICD §Crash Recovery. The early-return in the
  no-in-flight-rows branch was removed so Phase B passes always
  execute.
- **ICD-0.4.7 §CLI Surface.** Two-level argparse nest (`plinth
  test rbac <extension>` + `--json` + `--run-id` + `--config`);
  exit codes 0 (all-pass), 1 (any-fail), 2 (operational error)
  per ICD verbatim.
- **ICD-LH-0 §Kernel contract.** WS `call`/`call_result`/`call_error`
  message types + `lh0:1:chain` Tier 1 registration + RBAC gate
  on `kernel.admin` — all match. The 4 Catch2 cases cover the
  shape / unknown-signature / invalid-call / non-admin-RBAC paths.
- **ICD-LH-0.1 §Kernel contract.** `lh0:1:js_stress(script)`
  signature with admin-only RBAC + dispatch fork in `on_call`
  before `call_capability` + process-lifetime RuntimePool with
  `default_runtime_limits()` (16 MiB mem, 100 ms CPU, 30 s wall,
  8 concurrent async ops) — all match. Pool init in
  `main.cpp` after `init_resolver`; pool teardown in atexit
  before `drogon::app().quit()` per ICD §Process-lifetime
  RuntimePool.
- **Cross-arc invariants (re-checked in light of 0.4.5–0.4.7
  changes):** the leaked-singleton `ConnectionRegistry` fix from
  0.4.5 Slice B (`af8bbdd`, `connection_registry.cpp:33`) closed
  the Meyers-singleton teardown sub-path of the WS flake family;
  combined with the prior `g_shutdown_pending` (0.3.3.1) +
  `plinth::log::shutdown` (0.3.4.1) + `drain_pending_jobs` +
  `cancel_all_timers` (0.4.0.1) bundle, the production-side
  protective gates are intact. LH-0.1's 3-trial run validates
  the bundle empirically (§2.4).

---

## 4. Accepted deviations log — 0.4.5 through 0.4.7 + LH-0/LH-0.1

Consolidated table for the window. Prior-window deviations are in
`RE-EVAL-0.4.x.md §4` (0.4.0, four rows) and
`RE-EVAL-0.4.x-following-0.4.4.md §4` (0.4.1–0.4.4, 22 rows); this
table does not duplicate them.

| Milestone | Deviation | Rationale | Status |
|---|---|---|---|
| 0.4.5 (Slice A) | `unregister_capability` placed in `capabilities/registration.{hpp,cpp}` (ICD said `resolution.{hpp,cpp}`) | Symmetry with `register_capability_tx` already in `registration.cpp`; same file, opposite verb | ratified; ICD-0.4.5 amended in §7 (§2.2) |
| 0.4.5 (Slice B) | B3 drain semantic — counter tracks post-`begin_drain` dispatches only | Pre-drain in-flights hold `resolution.cpp`'s `state_mutex` shared lock, naturally serialized with REGISTERING's unique lock; ICD §Appendix A scenario converges on the same outcome via shared-lock bracketing | ratified |
| 0.4.5 (Slice B) | B5/B6 lock release before dispatch (install_package releases its advisory lock before calling upgrade_package on a separate PG session) | Re-acquire is a cooperative-lock edge; concurrent upload snagging the lock produces a sane 409 `in-flight-operation`; not worth holding the lock across two-session boundary | ratified |
| 0.4.5 (Slice B) | B6 REGISTERING-upgrade strategy — DELETE old extension rows then INSERT new set inside the tx, not ON CONFLICT UPSERT | NOTIFY buffering makes DELETE+INSERT atomic to registry-cache listeners on COMMIT; UPSERT path can't express the swap atomicity | ratified |
| 0.4.5 (Slice B) | B10 partial X.* / G.* coverage — X.05–X.13 + G.03 deferred to a follow-up | Out-of-time within window; library-level upgrade path fully covered by X.01/X.02/X.04 + G.02; HTTP-fixture work shares the I.18/I.19/I.20 deferral character (see §2.10 for new DEFERRED entry) | ratified, now in DEFERRED.md (§7) |
| 0.4.5 (Slice B) | Singleton-leak fix on `ConnectionRegistry` (`static auto* inst = new ConnectionRegistry()`) replacing Meyers singleton | Compiler-registered destructor runs LIFO with user `std::atexit` handlers; `cancel_all_timers` lambda dereferenced destroyed `conns` map before fix; "leak the singleton, OS reclaims at exit" matches the same rationale `0.4.0.1` documented for `g_shutdown_pending` being a file-scope atomic | ratified |
| 0.4.5.1 | `[js][async]` 45 cases stay per-TEST_CASE while the other 4 groups consolidate to 49-subprocess model | Grouping `[js][async]` AMPLIFIED an existing parallel-dispatch refcount race (~10–15% per-TEST_CASE → ~15–25% grouped); root cause is kernel-side, not test-strategy; LH-0.1 confirmed production unaffected | ratified |
| 0.4.5.1 | `cancel_all_timers` reworked to iterate `ConnState` (`shared_ptr` we own) not `WSConn` (drogon-managed control block) | PR-CI #12174 fired the canonical `bad_weak_ptr` SEGV mid-PR; architect-approved on the spot as "rework cancel_all_timers"; outside the §2.2 six-option menu but a structural fix in the same spirit | ratified-in-PR |
| 0.4.5.1 | Verification methodology shift — tight-loop now greps `LastTest.log`, not just ctest stdout | v1 `sig_hits=0` claim was misleading (signal-handler backtraces never surface on ctest stdout); v2 with `LastTest.log` grep + signature watchlist is the durable pattern for any future tight-loop verification | ratified |
| 0.4.5.2 | Paper-only docs session; no code deviations possible | — | — |
| 0.4.6 | Rule A.2 narrower than CF1 (package name ∪ reserved set, vs CF1's any-provided-namespace) | DESIGN §0.4.6 followed verbatim; CF1 unchanged; captured as ICD-0.4.6 OQ2 | ratified |
| 0.4.6 | `validate_phase_a` unified signature (A.1–A.4 pure, A.5 PG-coupled) rather than split into pure + PG sub-APIs | Single call site in installer + single error list in caller loop; Catch2 split via PG-gated tags | ratified |
| 0.4.6 | `test_contract` parameter additive but not defaulted | Project convention — every existing caller recompiles; two in-tree callers updated in same commit | ratified |
| 0.4.6 | `register_extension_rbac_rules` helper extracted from `run_stage_registering` | clang-tidy cognitive-complexity threshold (25); inlining Phase A + upsert loop pushed function to 26 | ratified |
| 0.4.6 | `phase_a.{hpp,cpp}` shipped as `rule_validator.{hpp,cpp}` (file rename, public surface unchanged) | Symmetry with existing `rule_registrar` in same directory; describes what the TU does rather than which DESIGN phase | ratified; ICD-0.4.6 amended in §7 (§2.1) |
| 0.4.6.1 | Paper-only docs session; no code deviations possible | — | — |
| 0.4.7 | Synchronous `call_capability` on detached `std::thread`, not `call_capability_async` + `queueInLoop` + promise bridge | Async wrapper is currently `co_return call_capability(call, ctx)` with no real suspension; sync form observationally identical and avoids dragging Drogon into the test fixture; future Tier 3 sidecar can upgrade | ratified |
| 0.4.7 | Slice A seeded SQL directly instead of driving `install_package` | Kept early Phase B integration tests hermetic; Slice B added install-driven PB.13/PB.14 once fixture infrastructure was in place | ratified (Slice B delivered) |
| 0.4.7 | PB.15 semantics rewritten as "sequential re-runs each acquire fresh advisory lock" rather than ICD's concurrent-contention assertion | Phase B uses `pg_try_advisory_lock` (non-blocking); contended runs return `lock_failed` without emitting an audit; ICD's "both complete, distinct run_ids" wording needs a blocking variant; Slice C's CLI provides the synchronous entry a future contention test would use | ratified |
| 0.4.7 | PB.14 fixture reuse — uses `valid-install` + `upgrade-v2` from `tests/fixtures/lifecycle_transitions/` instead of adding a seventh `happy-all-pass-v2` fixture | Neither declares `test_contracts`, so Phase B runs skip-only; assertion is about `last_phase_b_run_at` populating on the new row, satisfied by skip-only path | ratified |
| 0.4.7 | PB.13 Tier 2 cache reload in test (calls `reload_tier2_cache(s.ctx.db)` after `install_package`) | Detached Phase B worker sees `capability_not_found` because the test binary has no LISTEN/NOTIFY listener; production always has the listener running in serve mode | ratified |
| 0.4.7 | CF4 handler-file presence surfaced during fixture authoring (not an ICD deviation) | Capability-manifest cross-file validation requires handler files per declared capability function; three fixture-build-time errors caught and fixed before landing | informational |
| 0.4.7 | Lifecycle-test serialization via new `wait_phase_b_settled` helper | Lifecycle transitions (0.4.5) use the same per-name advisory lock Phase B holds; tests calling `install_package` immediately followed by `disable_package`/`upgrade_package` must serialize via the helper or race the lock | ratified |
| 0.4.7 | Unified `run_phase_b` with optional `run_id_override` instead of overload | CLI `--run-id` flag + PB.07* test determinism need same thread-through; default-empty parameter lighter than new entry point | ratified |
| 0.4.7 | 24 test cases vs ICD's 23 (added PB.07b unknown-extension-exit-2, PB.07c `--json` round-trip) | Same footing as CHANGELOG-listed test-count deviations in prior milestones | ratified |
| 0.4.7 | `phase_b.{hpp,cpp}` shipped as `rbac_test_runner.{hpp,cpp}` (file rename; types/namespaces/methods/audit-kinds/schema-columns unchanged) | Describes what the TU does (run RBAC tests) rather than which DESIGN phase; v0.4.7 CHANGELOG explicitly punts ICD update to RE-EVAL | ratified; ICD-0.4.7 amended in §7 (§2.1) |
| 0.4.7 | OQ resolutions architect-confirmed 2026-04-21: OQ1 ACTIVE_FLAGGED blocks no 0.4.5 transition; OQ2 per-execution `run_id` (UUIDv4); OQ3 reuse install advisory lock; OQ4 reuse `upgrade_drain_timeout_ms{5000}` as per-rule cap; OQ5 skipped rules in `PhaseBReport` + audit | All Open Questions resolved at PR time | ratified-in-PR |
| LH-0 | `ConnState.effective_rules` populates `is_admin` only; `on_call` synthesizes `["kernel.admin"]` vs `[]` for dispatch RBAC | Widening to full effective-rules adds one SELECT at auth time; kept out of LH-0 scope; non-admin WS callers can't invoke RBAC-gated caps via `call` today | ratified |
| LH-0 | No CI wiring | ROADMAP defers harness-in-CI to LH-4 (alongside `plinth.metrics` as regression-tracked substrate) | ratified |
| LH-0 | Async-bridge stress not exercised by `lh0:1:chain` (recurses through sync `call_capability` only) | Tier 1 lookup + RBAC + call-depth tracking exercised; JS async-bridge stress is LH-0.1 scope | ratified, LH-0.1 delivered |
| LH-0 | Bug discovery + fix during smoke (workers shared session token → ConnectionRegistry displaced; tight-loop on ws_closed) | Per-worker `POST /api/auth/login` + worker exits on connection-death; ICD §5.2 documents the fix | ratified |
| LH-0.1 | Dispatch fork in `on_call` (recognized BEFORE `call_capability`); not in Tier 1 map | Deliberate diagnostic-scope deviation per ICD-LH-0.1 §4; explicitly NOT a blueprint for extension dispatch; absorbed into architecture/02-capabilities.md by §2.4 of this re-eval | ratified; arch-doc amended in §7 (§2.4) |
| LH-0.1 | Single fixed `asyncStressScript` (not parameterised `--script-file`) | Diagnostic-only purpose; LH-0.2 adds parameterisation when a caller needs multiple async shapes | ratified, deferred |
| LH-0.1 | `js_eval_error` is catch-all for every `EvalErrorKind` | Caller (harness) doesn't branch on kind; richer error taxonomy deferred until needed | ratified |
| LH-0.1 | No fix shipped for the diagnosed signature class (zero reproductions) | LH-0.1 is the diagnostic only; production kernel produced zero reproductions, so no signature to fix; the test-strategy redesign (0.4.5.1) is the actual response | ratified |

**Observation.** 24 deviations across the three 3-part milestones
(0.4.5 + 0.4.6 + 0.4.7) plus 8 deviations across the LH stream. Zero
were retracted after shipping. Rationale clusters: 7 ICD-text
imprecision permitted by §Appendix latitude, 6 kernel-convention
alignment, 5 file-relocations / symbol-symmetry choices, 4 external-
constraint forced (PG semantics, Drogon API limits, advisory-lock
non-blocking semantics), 4 LH-stream deliberate-diagnostic deviations,
3 deferred coverage (Slice B X/G cases, LH-0 effective-rules, LH-0
CI wiring), 3 PG-fixture / serialization choices. Two file-rename
deviations require ICD amendments (§2.1, §2.2); one DEFERRED.md
addition tracks the Slice B X/G cases (§7).

Combined with prior-window tables: 0.4.0 had 4, 0.4.1–0.4.4 had 22,
0.4.5–0.4.7 + LH had 32. **Total 0.4.x arc: 58 ratified deviations,
zero retractions.** This table is the 0.4.5–0.4.7 + LH window's
consolidated reference. Any future inheriting reader should consult
the three re-eval tables in sequence.

---

## 5. Known-issues preserved

Six `DEFERRED.md` §Active entries re-examined (§2.10 enumerates).
Status changes:

### 5.1 WS teardown flake — empirically grounded as test-only by LH-0.1

The architect's working hypothesis from `RE-EVAL-0.4.x-following-0.4.4.md
§2.2` (race is a test-harness teardown artifact, not a production-path
race) was promoted to an empirical finding by LH-0.1's 2026-04-21
diagnostic run: 3 trials × 2 minutes against the production kernel
at concurrency=4, totaling 133,755 `js_stress` calls / ~535,020
`db.query` operations / ~535k `signal_completion` callbacks, with
**zero reproductions** of `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`,
or `bad_weak_ptr`. RSS stable 24 → 26 MiB; clean SIGTERM shutdown
afterwards.

Combined with the 0.4.5 Slice B leaked-singleton fix (`af8bbdd`,
`connection_registry.cpp:33`) which closed the Meyers-singleton sub-path
that had been bandaged across the 0.3.3.1 / 0.3.4.1 / 0.4.0.1 / 0.4.4.1
sequence, the WS teardown flake's production exposure is now
demonstrated zero. The residual flake is scoped to the `[js][async]`
Catch2 subprocess path (per 0.4.5.1's "Async caveat") — a different
sub-path with a different root-cause candidate list.

**DEFERRED.md update needed**: rewrite the entry to reflect
(a) the 0.4.5 leaked-singleton fix closed the Meyers-singleton
sub-path, (b) LH-0.1 empirically validated the production kernel as
clean for the canonical signatures, (c) the residual is scoped to
`[js][async]` test-strategy, and (d) the entry stays Active because
the `[js][async]` race is unresolved — but the framing changes from
"hypothesis: test-only" to "confirmed: test-only, scoped to
`[js][async]` subprocess lifecycle." See §7.

### 5.2 I.18/I.19/I.20 HTTP test harness

Unchanged. None of 0.4.5/0.4.6/0.4.7 retrofitted the missing fixture.
The DEFERRED entry's "next pickup candidate" framing remains valid;
0.5.x's first HTTP-touching milestone (likely 0.5.2 WS broker if it
wires HTTP control surfaces, or whichever 0.5.x milestone next adds
`/api/*` endpoints) will be the natural moment.

### 5.3 ICD-0.4.5 deferred X.* / G.* test cases (NEW entry)

Per §2.10, the v0.4.5 CHANGELOG §B10 deferred X.05/X.06/X.07/X.08/X.09/
X.10/X.11/X.12/X.13 + G.03 to a follow-up. Currently CHANGELOG-only.
Per the I.18/I.19/I.20 precedent, this should land as a DEFERRED.md
§Active entry. Lands in §7 disposition.

### 5.4 Per-op `SET search_path` + `db.*` PG-type→JS-type mapping + Drogon SqlError + MEMORY_LIMIT classifier

All unchanged. 0.5.x picks up the first two; the latter two stay
ad-hoc / watchlist.

---

## 6. Forward ICD presence check

Per METHODOLOGY §3.1 *Forward ICD presence check*:

**Next-N window (N=3) — code milestones in scheduled order:**

| Milestone | Band (incoming) | Band (outgoing after §8) | ICD exists? | Action |
|---|---|---|---|---|
| 0.5.0 PG LISTEN/NOTIFY bridge | medium | **strong** | ❌ | Schedule `0.4.7.2 ICD-0.5.0 authored` docs session ahead of 0.5.0 code (one-ahead horizon). DESIGN reference: `architecture/03-data.md §3 LISTEN/NOTIFY`. |
| 0.5.1 DB layer auto-event emission | medium | medium | ❌ | Stays one milestone further out; ICD authored in `0.5.0.1`-style slot ahead of 0.5.1 code. |
| 0.5.2 WebSocket broker | medium | medium | ❌ | After-N at this re-eval (3rd code milestone out); horizon rule satisfied by deferring to a `0.5.1.1` slot. |

**LH stream (parallel; not in cadence count per ROADMAP preamble):**

| Milestone | Band (incoming) | Band (outgoing) | ICD exists? | Action |
|---|---|---|---|---|
| LH-1 LISTEN/NOTIFY subscribe + notify-storm tier | medium | medium | ❌ | Gated on 0.5.0 (shipping). When 0.5.0 ships and LH-1 enters its scheduling window, author ICD-LH-1 in a paired slot with the LH-1 code session — same pattern as ICD-LH-0.1 + LH-0.1. |
| LH-2 / LH-3 / LH-4 | medium | medium | ❌ | After-N; ICDs author one-ahead of each as their gates open. |

**After-N window (milestones 4–7 out, M=7):** RE-EVAL following 0.5.1
(rewrite session, not code), 0.5.3 db.batch + silent mode, 0.5.4
plinth.events table + delta sync, 0.5.5 sequence numbers + debounce.
All `[medium]`, all covered by `architecture/03-data.md` at the design
level. No forward-ICD-presence action.

**Forward-check result.** One immediate ICD authoring obligation
(ICD-0.5.0). Scheduled as `0.4.7.2` per §2.6 — the dedicated 4-part
docs slot one-ahead of the 0.5.0 code session. Pattern matches the
recent 0.4.x cadence (every code milestone got its own dedicated
4-part docs predecessor).

---

## 7. Disposition — what was fixed here vs. scheduled

### Fixed in this PR (doc-only)

1. `docs/reviews/RE-EVAL-0.4.x-arc-closeout.md` — **new** (this
   file).
2. `docs/icd/ICD-0.4.5-package-lifecycle-transitions.md` — one-line
   "Implementation deviation (0.4.5 file placement)" amendment on
   the `unregister_capability` line in §Library surface naming
   `registration.{hpp,cpp}` as the actual home (§2.2).
3. `docs/icd/ICD-0.4.6-rbac-rule-registration.md` — new
   "Implementation deviation (0.4.6 file rename)" subsection ahead
   of §Library surface changes naming `phase_a.{hpp,cpp}` →
   `rule_validator.{hpp,cpp}` and the rationale (§2.1).
4. `docs/icd/ICD-0.4.7-rbac-test-execution.md` — symmetric
   "Implementation deviation (0.4.7 file rename)" subsection
   naming `phase_b.{hpp,cpp}` → `rbac_test_runner.{hpp,cpp}`
   (§2.1).
5. `docs/architecture/02-capabilities.md` — new paragraph (§3 or
   new §3.5) on `DispatchGuard` as the per-extension drain primitive
   consumed by upgrade (§2.3) + new paragraph on `lh0:1:chain` as
   a kernel Tier 1 capability for diagnostic load-harness use +
   the `js_stress` dispatch fork as a deliberate diagnostic-only
   deviation, with explicit "not a blueprint" framing (§2.4).
6. `docs/SESSION-GUIDE.md` — new "Test grouping convention
   (0.4.5.1)" subsection naming the four ctest groups, the
   tag-routing matrix, the fixture mapping, and the `[js][async]`
   per-TEST_CASE exception (§2.5).
7. `docs/DEFERRED.md` —
   - WS teardown flake entry rewritten to reflect the LH-0.1
     empirical resolution (§5.1): scope narrowed from "trantor
     EventLoop teardown bandaid family" to "`[js][async]` Catch2
     subprocess race"; framing flipped from "hypothesis: test-only"
     to "confirmed: test-only" with the LH-0.1 evidence inline.
     Entry stays Active.
   - **New** §Active entry: `ICD-0.4.5 X.05/X.06/X.07/X.08/X.09/
     X.10/X.11/X.12/X.13 + G.03 — extended upgrade and GC test
     coverage` (§5.3), pointing at the same HTTP-fixture-needed
     framing as I.18/I.19/I.20.
8. `docs/ROADMAP.md` —
   - `RE-EVAL following 0.4.7 (0.4.x arc closeout)` line removed
     (discharged by this session).
   - `0.5.0 PG LISTEN/NOTIFY bridge in kernel` promoted `[medium]`
     → `[strong]` per §6 + §8.
   - **New** `0.4.7.2 ICD-0.5.0 authored` line inserted between
     §0.4 closeout and §0.5 ahead of `0.5.0` (one-ahead horizon
     per §6).
   - **New** `## 0.4.x cleanup follow-ups (cross-cutting; scheduled
     between milestones)` section ahead of `## Load Harness`
     scheduling four cleanup items as ROADMAP-visible work
     (§2.12 disposition):
     - `0.4.7.3 phase_a / phase_b internal-symbol rename` `[strong]`
     - `0.4.7.4 Public-surface phase_b rename + migration plan` `[medium]`
     - `0.5.x.N HTTP test harness for /api/packages` `[strong]` —
       absorbs DEFERRED.md I.18/I.19/I.20 + X.05–X.13/G.03 entries
     - `0.5.x.N [js][async] kernel-side refcount investigation` `[medium]` —
       absorbs DEFERRED.md WS-teardown entry's residual scope
9. `docs/CHANGELOG.md` — **new** Rewrite-session entry under
   2026-04-22.

### Scheduled for future sessions (no action this PR)

- **`0.4.7.2 ICD-0.5.0 authored`** — paper docs session, fires
  before 0.5.0 code begins (next ROADMAP item after this re-eval
  merges).
- **`0.4.7.3 phase_a / phase_b internal-symbol rename`** — code
  session, scheduled before 0.5.0 if time permits or as 0.5.0.1
  otherwise. Pure internal rename, ~1 session estimated. Public-
  surface symbols (audit strings, schema columns) deliberately
  out-of-scope; tracked separately as 0.4.7.4.
- **`0.4.7.4 Public-surface phase_b rename + migration plan`** —
  decision document + (if green-lit) one-shot migration. Decision
  may resolve as "leave the public surface" if the cost-vs-clarity
  trade-off doesn't justify the migration; in that case the slot
  closes with that resolution recorded.
- **`0.5.x.N HTTP test harness for /api/packages`** — picks up
  ICD-0.4.4 I.18/I.19/I.20 + ICD-0.4.5 X.05–X.13 + G.03. Lands
  alongside the first 0.5.x milestone that adds new HTTP surface
  or as a dedicated 4-part follow-up. DEFERRED.md entries point
  at this slot.
- **`0.5.x.N [js][async] kernel-side refcount investigation`** —
  test-strategy cleanup (production confirmed clean by LH-0.1).
  Win condition: enable `[js][async]` grouping and recover the
  ~10× subprocess-count reduction. DEFERRED.md WS-teardown entry
  points at this slot.
- **0.5.0 PG LISTEN/NOTIFY bridge** — first 0.5.x code milestone.
  Entry condition: `0.4.7.2` ships first.
- **ICD-0.5.1 / ICD-0.5.2 authoring** — 4-part docs slots one-ahead
  of each 0.5.x code session as their windows open. Pattern
  inherited from 0.4.x cadence.
- **ICD-0.4.4 prose polish on `ACTIVE_FLAGGED`** — picked up
  next time ICD-0.4.4 is edited (incidental cleanup).

---

## 8. Band label review

Per the Plinth labeling rule and this re-eval's triggers:

| Milestone | Current | New | Rationale |
|---|---|---|---|
| `0.4.7.2 ICD-0.5.0 authored` | — (new insertion) | n/a (paper-session, no band) | Forward-ICD presence rule per §6. Lands ahead of 0.5.0 code session. |
| `0.4.7.3 phase_a / phase_b internal-symbol rename` | — (new insertion) | **strong** | Pure internal rename, ~1 session estimated. Source-of-truth-restoration (file paths renamed in v0.4.7, types/methods/tags follow); load-bearing for the 4-arc-out cohort because every reader of the renamed file expects matching symbol names. §2.11. |
| `0.4.7.4 Public-surface phase_b rename + migration plan` | — (new insertion) | medium | Decision-document slot; outcome may be "do not migrate" with that resolution recorded. Lower urgency than 0.4.7.3 because the public surface is at least internally consistent (audit string + schema column + emit-callsite all use the same `phase_b` token). §2.11. |
| `0.5.x.N HTTP test harness for /api/packages` | — (new insertion) | **strong** | Absorbs ICD-0.4.4 I.18/I.19/I.20 + ICD-0.4.5 X.05–X.13 + G.03 deferred coverage. The shared HTTP fixture is the gating dependency for all 13 cases. Promotion to strong reflects that 0.5.x will likely surface the natural pairing point. §2.11. |
| `0.5.x.N [js][async] kernel-side refcount investigation` | — (new insertion) | medium | Test-strategy cleanup; production confirmed clean by LH-0.1. Medium not strong because the path forward depends on a tight TEST_CASE-level repro that doesn't exist today; promotion to strong follows once the entry condition is met. §2.11. |
| 0.5.0 PG LISTEN/NOTIFY bridge | medium | **strong** | Enters next-N=3 window after this re-eval. ICD authoring scheduled (§7); DESIGN-by-pointer to `architecture/03-data.md §3` is the contract until ICD lands (precedent: 0.4.1 GlassWorm contract-by-pointer). |
| 0.5.1 DB layer auto-event emission | medium | medium | Still in next-N; ICD authoring deferred one more cycle per horizon rule. Promotion happens at `RE-EVAL following 0.5.1`. |
| RE-EVAL following 0.5.1 | rewrite-session | rewrite-session (unchanged) | Cadence slot already on ROADMAP; preserved. |
| 0.5.2 WS broker | medium | medium | After-N at this re-eval (3rd code milestone out); horizon rule defers ICD to `0.5.1.1` slot. |
| 0.5.3 / 0.5.4 / 0.5.5 | medium | medium | After-N; DESIGN coverage via `03-data.md` sufficient. |
| LH-1 / LH-2 / LH-3 / LH-4 | medium | medium | Parallel stream; gated on downstream capabilities. Not in cadence count. |
| 0.6.x | medium | medium | Unchanged. The Layer-3 GlassWorm integration noted in `RE-EVAL-0.4.x.md §10` remains a future arch-doc-edit candidate but is out of this re-eval's remit. |
| 0.6a-* | fuzzy | fuzzy | Unchanged. |
| 0.7.x – 1.0 | fuzzy | fuzzy | Unchanged. |

**Promotions applied:** 0.5.0 (`[medium]` → `[strong]`). One promotion.
No demotions. **Five new ROADMAP items inserted** per §7 item 8:
`0.4.7.2 ICD-0.5.0 authored` (paper-session) ahead of 0.5.0; and the
new `## 0.4.x cleanup follow-ups (cross-cutting)` section with
`0.4.7.3` `[strong]`, `0.4.7.4` `[medium]`, `0.5.x.N HTTP test
harness` `[strong]`, `0.5.x.N [js][async] investigation` `[medium]`.
The cleanup-section items are scheduling-flexible by design (each
notes "schedule alongside X or as dedicated Y") rather than
hard-sequenced, matching the cross-cutting pattern of `## Testing
& Security` and `## Load Harness`.

---

## 9. Observations for methodology / future re-evals

### 9.1 Investigation gate worked as designed

`RE-EVAL-0.4.x-following-0.4.4.md §2.2` introduced an "investigation
gate" as a mandatory first step for the then-scoped 0.4.4.1 (later
shipped as 0.4.5.1) — a static-walk-plus-empirical-load-test gate
that would either confirm the architect's "test-only, not production"
hypothesis or escalate to a redesign of scope. The gate fired in
two halves:

1. **Static half** — landed in 0.4.4.1's CHANGELOG §Investigation
   gate (PASS): walk of `main.cpp:224–231` vs the test fixtures
   showed production atexit chain matches tests on every step;
   `JS_FreeRuntime list_empty` is unambiguously test-only;
   `bad_weak_ptr` timer race is protected the same way on both
   sides.
2. **Empirical half** — landed in LH-0.1's 2026-04-21 diagnostic
   run: zero production-side reproductions across 3 trials.

Both halves confirmed the hypothesis. The 0.4.5.1 candidate-selection
proceeded with confidence (Option 2 + Option 5 + mid-PR
cancel_all_timers rework), the production kernel did not need a
race fix, and the `[js][async]` residual was correctly scoped as
test-strategy not production.

**Methodology takeaway.** The "investigation gate" pattern is
portable. Any future re-eval that produces a "scope-this-as-test-only"
recommendation should consider scheduling a parallel empirical-half
task (load-harness style) to validate. The cost-benefit is favorable:
LH-0.1 was small (~2 days) and prevented a misallocated kernel-fix
session that could have spent multiple cycles bandaging a
non-production race. Worth a future paper-session METHODOLOGY
amendment naming the pattern as an option in the §3 toolbox.

### 9.2 Bandaid-fatigue observation from prior re-eval validated

`RE-EVAL-0.4.x-following-0.4.4.md §9.1` proposed a "bandaid-fatigue
threshold" rule: when ≥3 point fixes accumulate on the same failure
signature class, the next re-eval should redesign instead of
documenting a fourth bandaid. 0.4.4.1's 4-rung ladder
(`g_shutdown_pending` → `plinth::log::shutdown` → `drain_pending_jobs`
+ `cancel_all_timers` → `dispatch_for_test`) was the trigger.

Since then: 0.4.5 Slice B added the leaked-singleton fix
(`af8bbdd`) — another rung, same family, ratified at PR time.
0.4.5.1 added the `cancel_all_timers` ConnState-iteration rework
mid-PR — yet another. Both were correct fixes, both shipped under
schedule pressure, and neither went through a "stop, redesign"
gate. Counting strictly, the WS teardown bandaid count is now
six rungs.

The salvation here is that LH-0.1 confirmed the production kernel
is clean — so the bandaids were retroactively justifiable as
test-harness hardening, not as papering over a production bug.
But the rule from `§9.1` did NOT fire to gate the late-arriving
fixes; they shipped because they were each individually small
and obviously correct.

**Methodology refinement.** The bandaid-fatigue rule needs a
companion: **architect-acceptance posture is a valid cap on
the rule**. If the architect has a working hypothesis that the
race class is test-harness-only and is empirically validating
that hypothesis (LH stream), additional teardown bandaids ship
under the umbrella of the empirical validation rather than
triggering yet another redesign. Worth a one-sentence amendment
to the rule's wording when it lands in METHODOLOGY.

### 9.3 File-rename interface drift is a recurring pattern

§2.1 + §2.2 catch the third instance of "ICD names file path X,
code ships at file path Y" in the 0.4.x arc:

1. 0.4.5 Slice A — `unregister_capability` placed in
   `registration.{hpp,cpp}` not `resolution.{hpp,cpp}`
   (§2.2; CHANGELOG-flagged at ship time).
2. 0.4.6 — `phase_a.{hpp,cpp}` shipped as `rule_validator.{hpp,cpp}`
   (§2.1; flagged in v0.4.7 CHANGELOG when the rename happened
   alongside the v0.4.7 Phase B rename).
3. 0.4.7 — `phase_b.{hpp,cpp}` shipped as `rbac_test_runner.{hpp,cpp}`
   (§2.1; flagged in v0.4.7 CHANGELOG explicitly punting to
   RE-EVAL).

All three deviations land in this PR as ICD amendments. Pattern
observation: ICD-authoring sessions tend to use the DESIGN doc's
phase-name nouns (`phase_a`, `phase_b`) as default file names,
while shipping sessions tend to rename to "what does this TU do"
(`rule_validator`, `rbac_test_runner`) for grep-friendliness and
symmetry with surrounding files. Neither is wrong — but the ICD
record should reflect the shipped reality so fresh sessions don't
grep for nonexistent paths.

**Methodology observation.** Future ICD-authoring sessions should
favor descriptive-of-function file names over phase-name nouns
when ambiguity exists, OR explicitly note "the implementing
session is free to rename the file path; only the public symbols
are normative." Lower-effort encoding, less re-eval cleanup.
Worth a future paper-session METHODOLOGY note (Phase 1 ICD
authoring guidance section).

### 9.4 Forward-ICD-presence rule continues to pay off

The 0.3.3.4 METHODOLOGY amendment's forward-ICD check has now
caught one more potential drift this cycle: ICD-0.5.0 would
otherwise have started code work without a contract. §6's
explicit scheduling preserves the one-ahead horizon, matching
the 0.4.x cadence pattern exactly.

**Four-for-four signal** that the rule is working as designed
(ICD-0.4.0 + ICD-0.4.1 caught at 0.3.6 arc-closeout; ICD-0.4.2
caught at `RE-EVAL following 0.4.0`; ICD-0.4.5 caught at
`RE-EVAL following 0.4.4`; ICD-0.5.0 caught here). No methodology
amendment needed — the rule is operating exactly as intended.

### 9.5 Deferred-item visibility — DEFERRED.md alone is not enough

§2.11 surfaced this directly: deferred items live in DEFERRED.md as
the source-of-truth log, but a fresh session reading the ROADMAP
top-to-bottom does not see them. ROADMAP carries scheduled work;
DEFERRED.md carries "this was decided not to ship now." The two
are complementary, but DEFERRED entries that need eventual
scheduling are invisible until either (a) someone happens to read
the file, or (b) a re-eval surfaces them.

The 0.4.x arc accumulated 4 such items: I.18/I.19/I.20 (entry
2026-04-20), X.05–X.13/G.03 (entry 2026-04-22, just added by §5.3
of this re-eval), the residual `[js][async]` Catch2 race (entry
2026-04-19, rewritten in §5.1), and the implicit `phase_a` /
`phase_b` naming debt (CHANGELOG-only until §2.11). Each got picked
up by this re-eval, but the gap between deferral and pickup was
2–3 milestones for some — not catastrophic, but a longer feedback
loop than necessary.

**Methodology takeaway.** A companion rule for DEFERRED.md: every
entry that needs eventual scheduling (vs. parked-indefinitely)
gets a `[ ]` ROADMAP slot pointing back at the DEFERRED entry,
inserted at the cadence point the re-eval determines. The slot
can be `[medium]` or `[strong]` per the usual band rules; the
ROADMAP is then the scheduling ledger and DEFERRED.md is the
detail ledger, mutually cross-referenced. Items truly parked
indefinitely (e.g. Drogon `PgBatchConnection` SqlError typing)
stay DEFERRED-only with no ROADMAP slot. This re-eval applies the
rule retroactively to the four 0.4.x-arc items; future re-evals
should apply it inline to any new DEFERRED entry as part of §3
disposition. Worth a one-paragraph METHODOLOGY amendment in the
next paper-session slot.

### 9.6 Arc-closeout re-eval scope discipline held

This session's §2 lists 6 substantive items + §2.7 noted-and-
dismissed + §2.8 zero-drift confirmation. Comparable to
`RE-EVAL-0.3.x-arc-closeout.md` (8 items) and well within the
proportional-investigation budget. The architecture-doc edits
(§2.3, §2.4) are the most substantive output — both are real
gaps that the prior cadence re-eval missed because the LH stream
was still in flight. Arc-closeout's slightly-broader scope
caught them; cadence re-eval's tighter scope might not have.

**Methodology observation.** The arc-closeout cadence trigger
(re-eval at arc boundary, even if the strict 4/4 cadence isn't
hit) is justifying its cost across the 0.3.x and 0.4.x arcs.
Worth keeping as a soft rule alongside the strict cadence
counter.

---

## 10. Exit criteria

Per METHODOLOGY §3.3:

- [x] Architecture documents read; two architecture-doc amendments
      land in this PR (`architecture/02-capabilities.md` for
      `DispatchGuard` + `lh0:1:chain` + `js_stress` fork — §2.3 +
      §2.4). The Layer-3 GlassWorm integration noted in
      `RE-EVAL-0.4.x.md §10` remains future-touch out of remit.
- [x] Band promotions applied (0.5.0 `[medium]` → `[strong]` — §8).
- [x] Roadmap milestone labels reviewed; one promotion; arc-closeout
      cadence line discharged; new `0.4.7.2 ICD-0.5.0 authored`
      docs slot inserted; new `## 0.4.x cleanup follow-ups`
      section added with four scheduled items (`0.4.7.3` + `0.4.7.4`
      + `0.5.x.N HTTP fixture` + `0.5.x.N [js][async] investigation`
      — §7 item 8 + §2.11).
- [x] ICD amendments land in this PR — three (ICD-0.4.5 file
      placement, ICD-0.4.6 file rename, ICD-0.4.7 file rename;
      §2.1 + §2.2 + §7 items 2–4).
- [x] Forward ICD presence check passed (§6). One scheduled
      authoring slot (`0.4.7.2`).
- [x] Known-issues preserved in durable storage (DEFERRED.md —
      WS-teardown entry rewritten + new ICD-0.4.5 X/G entry; §5,
      §7 item 7).
- [x] SESSION-GUIDE.md edit lands the test-grouping convention
      (§2.5, §7 item 6) — contributor-onboarding home, not
      architecture tree.
- [x] Session output committed to the documentation tree
      (`docs/reviews/RE-EVAL-0.4.x-arc-closeout.md` — this file).
- [x] Code untouched. No C++ / CMake / CI-YAML edits.
- [x] CHANGELOG entry for this session — un-tagged per
      `feedback_tagging_rule.md` (interim doc patches get
      CHANGELOG entries only, no tag). Tagging of the v0.4.7
      release commit is the architect's responsibility, separate
      from this PR.

---

## 11. Next actions for the architect

1. **Review this artifact.** Flag any disposition you disagree
   with. Particularly:
   - §2.1 + §2.2 ICD file-rename amendments — three ICDs touched
     with new "Implementation deviation" subsections; light edits
     but they touch the public ICD record. Confirm the rename
     rationales match your intent.
   - §2.3 + §2.4 architecture-doc edits — `DispatchGuard` paragraph
     in `02-capabilities.md` + `lh0:1:chain` and `js_stress` fork
     paragraphs (also in `02-capabilities.md`, or split into
     `04-services-ha.md` if you prefer). Both are new
     architectural primitives that the 0.4.5 / LH stream
     introduced; the doc tree should reflect them. If you'd
     rather defer to a focused 0.4.x architecture-rewrite session,
     reverse the disposition and note here.
   - §2.5 SESSION-GUIDE.md test-grouping convention — durable
     contributor doc; confirm SESSION-GUIDE.md is the right home
     vs. a new `docs/testing.md` or similar.
   - §2.6 + §6 forward-ICD scheduling — `0.4.7.2 ICD-0.5.0
     authored` as a dedicated docs slot vs. authoring ICD-0.5.0
     here. Reversible if you prefer the v0.3.6 precedent
     (arc-closeout authors next-arc ICDs in same PR).
   - §5.1 + §7 DEFERRED.md WS-teardown rewrite — promotes
     the architect's hypothesis to an empirical finding, scopes
     the entry down to `[js][async]` subprocess race. Confirm
     the framing reads correctly to you.
   - §9.1 + §9.2 + §9.3 + §9.5 methodology observations — four
     candidate METHODOLOGY amendments noted (investigation-gate
     pattern, bandaid-rule architect-acceptance cap, ICD file-name
     guidance, deferred-items-need-ROADMAP-slots companion rule).
     All low-touch; flag any worth landing in a near-term paper
     session.
   - §2.11 cleanup-section disposition — four new ROADMAP items
     (`0.4.7.3` `[strong]`, `0.4.7.4` `[medium]`, `0.5.x.N` HTTP
     fixture `[strong]`, `0.5.x.N` `[js][async]` investigation
     `[medium]`). Confirm scheduling preferences; some items
     (especially 0.4.7.3 vs 0.4.7.4 ordering) carry trade-offs
     between "rename everything together" vs "rename internal now,
     decide on public surface later." Reversible.
2. **Tag `v0.4.7`.** Per your message at session start, this is
   yours to handle when this re-eval merges. Tag candidate is
   `4a86e31` (the Slice C merge commit); arc-closeout per
   `feedback_tagging_rule.md`.
3. **Confirm 0.5.0 entry conditions.** Per ROADMAP: (a) this
   re-eval merged; (b) `v0.4.7` tagged; (c) `0.4.7.2 ICD-0.5.0
   authored` shipped as a dedicated docs session. Then 0.5.0
   code work begins.
4. **Confirm next cadence re-eval position.** §2.11 leaves
   `RE-EVAL following 0.5.1` as the next cadence slot
   (already on ROADMAP, unchanged by this re-eval). Counting
   from this arc-closeout: 0.5.0 + 0.5.1 = 2 milestones; the
   cadence position at `RE-EVAL following 0.5.1` is **2/4**,
   matching the `RE-EVAL following 0.5.5` arc boundary if 0.5.x
   ships as five milestones. If the LH-1 / LH-2 milestones land
   in parallel during 0.5.x, they remain outside the cadence
   count per ROADMAP preamble (parallel stream).
5. **No tag on this session** per `feedback_tagging_rule.md`
   (interim doc patches get CHANGELOG entries only). The CHANGELOG
   Rewrite-session entry is the ledger.
