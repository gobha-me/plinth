# Re-evaluation — 0.3.X arc close-out

**Date:** 2026-04-19
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3) + arc-closeout release (v0.3.6)
**Trigger:** Architect directive 2026-04-19 — the 0.3.X arc is closing; the docs session is the release (matches the v0.1.8 arc-closeout precedent). Re-eval scope added alongside ICD-authoring for 0.4.0/0.4.1.
**Cadence position:** Third scheduled re-eval. Cadence trigger would fire after four 3-part milestones since the previous re-eval; only 0.3.4 and 0.3.5 have tagged since `RE-EVAL following 0.3.3`, so the formal cadence is at **2/4**. Arc-closeout is a natural additional trigger and the architect elected to run the code-aware half now.
**Scope:** 0.3.4 + 0.3.5 + the 0.3.4.1 four-part fix bundle. Prior 0.3.X work (0.3.0 – 0.3.3 + 0.3.3.1 – 0.3.3.4) was covered by `RE-EVAL-0.3.x.md` and amendments landed there; this session does not re-open that work.

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index, index-only scan).
- `docs/architecture/02-capabilities.md §1 Capability Registry; §3 QuickJS Async Bridge` — still authoritative for `cap.*` surface and the three-tier resolution 0.3.4 wires through.
- `docs/architecture/05-extensions.md §1 Package Structure; §1.1 Cross-File Manifest Validation; §3 QuickJS Runtime + Supervision; §3.1 Runtime Limits` — normative source for the new ICD-0.4.0 / ICD-0.4.1 pair.

### ICDs (0.3.4 + 0.3.5)
- `docs/icd/ICD-0.3.4-cap-call-from-js.md` — 0.3.4's contract; implemented in this window.
- `docs/icd/ICD-0.3.5-runtime-hardening.md` — 0.3.5's contract; implemented in this window.
- `docs/icd/ICD-0.3.3-async-bridge.md` — consulted for 0.3.5's `async_result_size_limit_bytes` enforcement close-out (the field is ICD-0.3.3's; the enforcement is ICD-0.3.5's).
- `docs/icd/ICD-0.3.1-runtime-lifecycle.md` — consulted for §Security Constraint 4 cross-reference in N.42 / N.43.
- `docs/icd/ICD-0.3.2-kernel-stdlib-sync.md` — consulted for the `eval` / `Function` deletion placement in 0.3.5's `inject_kernel_stdlib`.

### Code
- `src/kernel/js/stdlib/cap_bindings.{hpp,cpp}` — **new in 0.3.4.** The `cap.*` binding surface.
- `src/kernel/js/async_op.hpp` — `CAP_CALL` activated; `cap_signature` + `cap_args` payload fields added.
- `src/kernel/js/bridge_context.hpp` — `UserContext user{anonymous()}` field added in 0.3.4; `memory_limit_hit` atomic latch added in 0.3.4.1.
- `src/kernel/js/runtime_pool.{hpp,cpp}` — constructor param `const UserContext* user` added in 0.3.4; interrupt callback sampling for memory-peak latch added in 0.3.4.1.
- `src/kernel/js/run_on_context.cpp` — CAP_CALL dispatch arm added; `check_result_size` anonymous-namespace helper added at `dispatch_async_op_detached` fan-in for 0.3.5; `drive_jobs` samples memory peak after each pending-job drain (0.3.4.1).
- `src/kernel/js/conversion.{hpp,cpp}` — `sample_memory_peak` / `was_memory_limit_hit` / `is_runtime_near_memory_limit` helpers added in 0.3.4.1; `classify_rejection` now consults the memory-peak latch as primary OOM-upgrade path.
- `src/kernel/js/stdlib_inject.cpp` — `disable_dynamic_code_entrypoints` helper added at injection top for 0.3.5 N.43.
- `src/kernel/js/eval.hpp` — `ASYNC_RESULT_SIZE_EXCEEDED` variant added.
- `src/kernel/logging.{hpp,cpp}` — `plinth::log::shutdown()` added in 0.3.4.1 (audit gate mirror of 0.3.3.1's `ConnectionRegistry::initiate_shutdown()`).
- `tests/kernel/js/async_bridge_fixture.{hpp,cpp}` — split into `ensure_drogon_running()` (no DbClient) vs `ensure_drogon_with_db_running()` (with DbClient) in 0.3.4.1.
- `tests/kernel/ws/ws_test_fixture.cpp` — atexit now calls `plinth::log::shutdown()` before `drogon::app().quit()` (0.3.4.1).
- `tests/kernel/js/limits_test.cpp` (**new**, 0.3.5) + `async_hardening_test.cpp` (**new**, 0.3.5) — the 11 N.* cases.

### Discussion / design context
- `docs/design/DESIGN-quickjs-bridge.md §§4.1 4.2 4.3 6.2 7.2 7.3 9.1` — cross-checked for 0.3.5's adversarial-test mapping + `eval` posture ratification.
- `docs/design/DESIGN-packages-v04x.md` — read in full for the ICD-0.4.0 / ICD-0.4.1 authoring ahead of the re-eval.
- `docs/DEFERRED.md` — three active entries + resolved log. Two existing entries (per-op `SET search_path`; `db.*` PG-type→JS-type mapping) unchanged by this window; two **new** entries added by this session for the WS teardown flake and the MEMORY_LIMIT classifier flake (see §4).

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. Surface is narrower than the
0.3.3 re-eval because (a) the ICD-0.3.4 / ICD-0.3.5 authoring used
the forward-ICD-presence-check discipline the 0.3.3 re-eval didn't
have, so ICD scope was tighter at the start; (b) 0.3.4.1 was
deliberately shipped as a four-part follow-up with the same "contract
by pointer" posture described in `RE-EVAL-0.3.x.md §6.1`.

### 2.1 Interface-drift — ICD-0.3.4 §BridgeContext Additions: `memory_limit_hit` field

ICD-0.3.4 §BridgeContext Additions names one new field (`user` value-
copy). `src/kernel/js/bridge_context.hpp` in 0.3.4.1 adds a second
field `std::atomic<bool> memory_limit_hit{false}` — part of the OOM
peak-tracking classifier uplift. The field is load-bearing (it
closes test 264 `MEMORY_LIMIT` flake) but is documented only in the
CHANGELOG entry and the conversion.cpp comment; not in any ICD.

Pattern matches `RE-EVAL-0.3.x.md §2.1` exactly (0.3.3 ICD declared
"no new fields"; 0.3.3.1 added four). The 0.3.4.1 fix is the 0.3.3.1
shape again: a four-part follow-up shipping code under the "contract
by pointer" deferral (CHANGELOG + DEFERRED.md + the parent ICD's
invariants), relying on the next re-eval — this one — to absorb the
surface into the owning ICD.

**Resolution:** Amend ICD-0.3.4 §BridgeContext Additions with an
"Implementation deviation (0.3.4.1 memory-limit peak tracking)"
subsection naming the new field, `sample_memory_peak` /
`was_memory_limit_hit` helpers in `conversion.{hpp,cpp}`, and the
interrupt-callback + `drive_jobs` sampling points. Precedent:
`RE-EVAL-0.3.x.md §§2.1, 2.2, 2.6`. **Fixed in this session.**

### 2.2 Interface-drift — ICD-0.3.4 fixture split + `plinth::log::shutdown`

ICD-0.3.4 §CI Wiring (end of ICD) lists the test fixture as
`tests/kernel/js/async_bridge_fixture.cpp` — one entry point
`ensure_drogon_running()`. 0.3.4.1 split it into two
(`ensure_drogon_running()` no-DB + `ensure_drogon_with_db_running()`
with DbClient), both sharing a `std::call_once` — cap.*, bc.*, and
audit-validation-only tests no longer pay the Drogon-DbClient
teardown cost. Same shape: load-bearing, documented in CHANGELOG +
DEFERRED.md, not in the ICD.

Plus the new public `plinth::log::shutdown()` in
`src/kernel/logging.{hpp,cpp}` — mirror of 0.3.3.1's
`ConnectionRegistry::initiate_shutdown()`. The pattern (audit
shutdown gate) is not in any ICD.

**Resolution:** Fold into the same ICD-0.3.4 "Implementation
deviation (0.3.4.1)" subsection as §2.1. Additionally, add an
"Implementation Notes (0.3.4.1)" footer to ICD-0.1.7 (audit) naming
the `plinth::log::shutdown()` pattern — mirrors the 0.1.6 WS footer
that RE-EVAL-0.3.x added for `ConnectionRegistry::initiate_shutdown`.
**Fixed in this session.**

### 2.3 Interface-drift — ICD-0.3.5 `check_result_size` placement

ICD-0.3.5 §Where It Applies lists each `run_*_outcome` helper by
name (`run_db_query_outcome`, `run_db_exec_outcome`,
`run_audit_write_outcome`, `run_cap_call_outcome`). The shipped code
places the check once at `dispatch_async_op_detached`'s success-outcome
fan-in, covering all four paths uniformly. This is listed as an
Accepted Deviation in the 0.3.5 CHANGELOG entry and the rationale is
sound (DRYer, same observable behaviour — per ICD §Semantics point 4,
"measurement runs in the detached task, before the `queueInLoop`-
landed `bc.resolve`"). The ICD text, though, still enumerates four
helpers — a reader of ICD-0.3.5 would not know where to grep for the
check.

**Resolution:** Amend ICD-0.3.5 §Where It Applies with a one-paragraph
"Implementation deviation (0.3.5 placement)" note naming
`dispatch_async_op_detached` as the single fan-in point and pointing
at the 0.3.5 CHANGELOG entry for the rationale. **Fixed in this
session.**

### 2.4 Scale-reduction deviation — N.39 at 4 × 2 instead of 100 × 8

ICD-0.3.5 §Adversarial Test Battery row 3 specifies "100 synthetic
`cap.call` entries against a fast Tier 1 stub; `max_concurrent_async_
ops=8`." The delivered test runs 4 × 2. Reason: any higher fan-out
on a single BridgeContext hits the pre-existing parallel-dispatch
requeue race described in `project_ws_flaky_segfault.md` §Fourth
occurrence (the K.33 comment / 0.3.4.1 bundle) — not a test bug, a
real production race that 0.3.4.1 narrowed but did not fully close.
4 × 2 is the minimum scale that still drives the requeue path and
proves the correctness property (back-pressure bounds in-flight to
`max_concurrent_async_ops`).

This deviation is ratified via the 0.3.5 CHANGELOG "Accepted
deviations" entry (item 1). Re-eval's job is to confirm the
architect-acceptance is recorded in a durable place — it is.

**Resolution:** No ICD amendment; the deviation is already captured
in the CHANGELOG "same footing as 0.2.0/0.2.2/..." language. The ICD
text at row N.39 could carry a `**Deviation in 0.3.5:**` pointer, but
it's low-value — the CHANGELOG entry is authoritative and any reader
puzzled by the shipped scale can `git log` to the 0.3.5 commit. **No
action.**

### 2.5 Missing-test-for-arch-claim — none surfaced

Cross-checked ICD-0.3.4 §Tests declaration ("13 cases across Groups
G–M") against `grep -c TEST_CASE tests/kernel/js/async_bridge_test.cpp`
— returns 30 after the 0.3.4 additions (0.3.3 had 17, 0.3.3.3
back-filled to 19, 0.3.4 added 13 new → 32; 0.3.4 pruned two via
fold-ins per CHANGELOG). All 13 G.24–M.36 cases are present. ICD-
0.3.5 declares 11 cases (N.37–N.47); both new test files combined
deliver 11. Zero drift on counts.

### 2.6 Arch-silent-on-code — none surfaced beyond §2.1–§2.3

Nothing in `src/kernel/js/` additions is unaccounted for by either
an ICD or an intentional-deferral CHANGELOG note. `classify_rejection`
gained one case (`async.result_size_exceeded`) — ICD-0.3.5 §New
EvalErrorKind Variant documents it.

### 2.7 Observation — `eval` / `Function` deletion architect-ratified in-PR

ICD-0.3.5 §Open Questions 1 flagged the `eval` / `Function` deletion
posture for architect ratification at PR time. The 0.3.5 CHANGELOG
records the deletion shipped without amendment — tacit ratification.
Not a drift; worth recording here so the ICD text's "Open Question"
can be marked "Resolved: deleted per PR ratification" in a later
touch.

**Resolution:** One-line amendment to ICD-0.3.5 §Open Questions 1
marking the question resolved (*delete posture shipped*). **Fixed in
this session.**

### 2.8 Observation — DEFERRED.md §§1 and 2 still live; ROADMAP cross-refs hold

Re-examined `docs/DEFERRED.md` active entries:

1. **Per-op `SET search_path` for `db.*`** — points at ROADMAP 0.4.3.
   ROADMAP 0.4.3 exists (band `[medium]`). Cross-reference holds.
2. **`db.*` PG-type→JS-type OID mapping** — points at ROADMAP 0.3.4
   as prerequisite-with-no-dedicated-slot. 0.3.4 shipped and the
   heuristic stays; no OID mapping was added. The deferral carries
   forward to the 0.4.x `db.*` work or a future dedicated slot.
3. **Drogon `PgBatchConnection` SqlError typing** — unchanged.

**Resolution:** Amend entry 2 to point at 0.4.x (more specifically,
0.4.3 which is the first milestone that introduces real per-extension
DB connections where OID mapping becomes load-bearing). Entry 1
unchanged. **Fixed in this session.**

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.3.4 §CAP_CALL dispatch arm.** `run_on_context.cpp` CAP_CALL
  arm awaits `call_capability_async(CapabilityCall{sig, args,
  bc.call_depth}, bc.user)`; success → `bc.resolve` with
  `CapabilityResult::data`; failure → `capability_error_to_rejection`.
  Matches ICD.
- **ICD-0.3.4 §Error Mapping.** The eight `cap.*` rejection codes map
  1:1 to `CapabilityError` variants per the ICD table. Code in
  `stdlib/cap_bindings.cpp` uses an explicit switch (the 0.3.4
  "Accepted deviations" item 1 ratified this in CHANGELOG; the ICD
  table is the authoritative mapping and matches the switch cases).
- **ICD-0.3.4 §Security Constraint 3 — identity non-leak.** Test M.35
  asserts `JSON.stringify(r)` contains no sentinel `user_id`,
  `Object.keys(cap) === ["batch", "call"]`, and `cap.whoami ===
  undefined`. Code matches.
- **ICD-0.3.5 §New Enforcement: Result-Size Cap.** Strict `>`
  semantic, measurement before JS receives value, default 16 MiB —
  all match code + CHANGELOG description.
- **ICD-0.3.5 §Security Constraint 5 (eval/Function deletion).** The
  deletion runs BEFORE `inject_kernel_stdlib` per the constraint text;
  `stdlib_inject.cpp`'s `disable_dynamic_code_entrypoints` is the
  first call in both the initial `create_entry` path and the
  post-`clear_global_own_props` `release()` path.
- **Call-depth enforcement.** ICD-0.3.4 §Call Depth threads
  `bc.call_depth` into the `CapabilityCall` it dispatches; the
  dispatcher itself enforces `MAX_CALL_DEPTH=8` (ICD-0.2.2, unchanged
  in 0.3.4). N.44's depth-chain test confirms end-to-end.
- **ICD-0.3.3 §Critical Invariants** (re-checked in light of 0.3.4 +
  0.3.4.1 changes): all three still hold — JS state touched only on
  main loop (dispatch_async_op_detached `queueInLoop`s for resolve /
  reject), CPU-timer bracket around `JS_Eval` + pending-job drain,
  FIFO back-pressure preserved.

---

## 4. Accepted deviations log — 0.3.0 through 0.3.5 consolidated

Single authoritative list. One row per accepted deviation. "Status"
is `ratified` if the CHANGELOG entry for that milestone records the
architect-level acceptance; `ratified-in-PR` if the PR body carried
the ratification.

| Milestone | Deviation | Rationale | Status |
|---|---|---|---|
| 0.2.0 (prior arc — precedent) | Registration API is sync libpq rather than async Drogon | No runtime caller until 0.2.2; async wrapper deferred to 0.4.x if measurements warrant | ratified |
| 0.2.2 (prior arc — precedent) | Synchronous dispatch; Tier 1 handlers are `{"not_implemented"}` stubs | No coroutine users yet; first real caller triggers async wrapper (landed as 0.2.6) | ratified |
| 0.2.4 (prior arc — precedent) | No DB fallback for rule lookup; audit is async via existing `plinth::log::audit` Drogon writer | Keeps hot path DB-free; audit already async-capable | ratified |
| 0.2.5 (prior arc — precedent) | `cap.batch` sequential dispatch; no Tier 3 multiplexing; `BatchResult::failed_index` beyond strict ICD surface | ICD permits `Promise.all` initial impl; Tier 3 concurrency is 0.8 scope; `failed_index` is for audit / debugging | ratified |
| 0.3.0 | None | Implementation tracks roadmap bullet exactly | — |
| 0.3.0.1 | None (pure refactor to `std::expected`) | No behavior change | — |
| 0.3.0.2 | Tidy-fix scope expanded by one check (`performance-unnecessary-copy-initialization`) | Architect-approved expansion at PR time | ratified-in-PR |
| 0.3.1 | No separate `interrupt_handler.{hpp,cpp}` TU; `eval_on_context` helper added; `pending_ops` / async fields omitted until 0.3.3 | Callback is ~15 lines, anonymous namespace in `runtime_pool.cpp`; tests need a host-eval entry point; async fields dormant until caller lands | ratified |
| 0.3.2 | `JS_SetContextOpaque` for BridgeContext lookup; `ConfigProjection` on BridgeContext (value-copy vs pointer); single test file `stdlib_test.cpp`; one-shot `eval.cpp` path stdlib-free | Avoids thread_local; decouples pool lifetime from Config lifetime; ICD §CI Wiring listed single file; unifies with pool path in 0.3.3 | ratified |
| 0.3.2.1 | Paper session; no code deviations possible | — | — |
| 0.3.3 | Parallel fan-out via `dispatch_ops_batch_fanout` (ICD described serial `co_await`); wake-driven cancellation-cascade drain (ICD described 5-second `sleepCoro`) | DEFERRED.md §parallel-fanout rationale; `BridgeContext`-UAF closure; `RE-EVAL-0.3.x.md §§2.1–2.4` folded the surface back into ICD-0.3.3 | ratified |
| 0.3.3.1 | Shipped without its own ICD ("contract by pointer") | CHANGELOG + DEFERRED.md + parent ICD's Critical Invariants constitute the contract; re-eval catches the ICD up | ratified |
| 0.3.3.2 | None (tidy + CI hygiene) | — | — |
| 0.3.3.3 | Group C cancellation cases folded into B.10 wall-clock end-to-end test; bridge-classifier uplift scope-expanded mid-PR | Architect decision (Group C → B.10); CI surfaced B.7's misclassification at reject time, scope uplift kept the ICD-B.7 → MEMORY_LIMIT contract instead of baking a bridge bug into the test | ratified |
| 0.3.3.4 | Paper session (ICDs 0.3.4 + 0.3.5 + METHODOLOGY §3.1 forward-ICD-presence rule) | Forward-presence rule patches the gap that produced this session | ratified |
| 0.3.4 | Error-code mapping is explicit switch (not `"cap." + error_string`); `detail::js_to_json` returns `std::expected` (pseudocode treated as throw); M.35 uses sentinel+surface check (not field-by-field sweep); L.34 accepts either `cap.cancelled` or outer `CANCELLED`/`WALL_CLOCK_EXCEEDED` | Three `CapabilityError` variants don't map through `error_string` uniformly; `js_to_json` already returns `std::expected`; equivalent coverage without redundant asserts; cascade `ns → code` mapping makes both outcomes correct | ratified |
| 0.3.4.1 (four-part) | Shipped without its own ICD ("contract by pointer" pattern, same shape as 0.3.3.1); adds `BridgeContext::memory_limit_hit` field + split async_bridge_fixture entry points + `plinth::log::shutdown()` audit gate | CHANGELOG + DEFERRED.md capture; this re-eval absorbs the surface into ICD-0.3.4 (§§2.1–2.2 above) and ICD-0.1.7 (audit footer) | ratified |
| 0.3.5 | N.39 scale reduced to 4 × 2 (ICD: 100 × 8); `check_result_size` placed at `dispatch_async_op_detached` fan-in instead of per-helper | Full 100 × 8 on a single BridgeContext triggers the WS flake requeue race; 4 × 2 is smallest scale exercising the same property. Single fan-in placement is DRYer with same observable behaviour. | ratified |
| 0.3.5 | `eval` / `Function` deletion shipped without explicit architect reply on ICD Open Question 1 | Tacit ratification via PR merge without amendment | ratified-in-PR |

**Observation.** Sixteen deviations across eleven 3-part milestones
(0.3.0 → 0.3.5 plus the four-part follow-ups 0.3.0.1, 0.3.0.2,
0.3.2.1, 0.3.3.1, 0.3.3.2, 0.3.3.3, 0.3.3.4, 0.3.4.1). Zero
deviations were retracted after shipping. The ratification pattern
is consistent — architect reviews in PR body or CHANGELOG entry, no
retroactive rollbacks.

This table is the consolidated reference anyone inheriting the 0.3.X
surface should consult first. Each CHANGELOG entry has the per-
milestone detail; this table is the index.

---

## 5. Known-issues preserved

Both items below are captured in `docs/DEFERRED.md` §Active
(new entries added by this session) so they survive when memory-file
notes age out. Paragraphs here are the re-eval's framing; DEFERRED.md
has the actionable future-implementer notes.

### 5.1 WS teardown flake — `bad_weak_ptr` in trantor::EventLoop

Sixth occurrence on the pre-0.3.5 post-merge CI (2026-04-19). Stack is
canonical: `terminate called after throwing std::bad_weak_ptr` →
`trantor::EventLoop::loop+0x2f2` → `std::unexpected` → `abort`, firing
*after* Catch2 printed "All tests passed" on a WS-specific test (test
216 `WS auth succeeds with a valid session token`). Post-test-complete
race; ctest did not fail the run.

0.3.3 closed the client-side `WsTestClient::stop()` race (queue onto
event loop + block on promise). 0.3.3.1 closed the `ConnectionRegistry`
sub-path (`g_shutdown_pending` flag + `initiate_shutdown()` called
before `app().quit()`). 0.3.3.2 surfaced a third sub-path (audit write
from `handleConnectionClosed`) which 0.3.4.1 closed with
`plinth::log::shutdown()`. The WS-specific teardown path inside
trantor's `EventLoop::loop` itself remains open.

0.3.5 PR CI was clean — no new occurrence — but the sub-path race
fires on post-merge runs independent of scheduled CI. Status parked
per architect framing ("these random issues are annoying" — suggests
acceptance posture for now, not immediate-fix ask). A dedicated fix
would need to extend the `g_shutdown_pending` pattern to whatever
EventLoop-internal callable is being destroyed before its last
`weak_ptr` resolves. Candidate diagnostic: extend the SIGSEGV
backtrace handler (shipped in 0.3.3.1 per `main_test.cpp`) with a
specific capture of the trantor teardown stack.

Tracked in DEFERRED.md §Active entry `2026-04-19 — WS teardown flake
(bad_weak_ptr in trantor::EventLoop)`.

### 5.2 MEMORY_LIMIT classifier flake — test 264

Test 264 `async_bridge: memory limit tripped between awaits yields
MEMORY_LIMIT` failed on post-merge CI for 0.3.4 with
`EvalErrorKind` expected 2 (MEMORY_LIMIT), observed 9/`\t`
(PROMISE_REJECTED_UNHANDLED / garbled data). Root-caused in 0.3.4.1
to two layers:

1. When the OOM fires deep in an async function body, the classifier
   cannot allocate temporary strings to read `reason.name` /
   `reason.message`, so name/message come back empty and the
   InternalError-based path misses.
2. A live runtime-stats check at classify time is already unwound —
   the async frame has released its accumulator and `malloc_size` is
   well below `malloc_limit`.

0.3.4.1 shipped the peak-tracker fix (`BridgeContext::memory_limit_hit`
atomic latch, interrupt-callback + drive_jobs sampling,
`sample_memory_peak` / `was_memory_limit_hit` helpers). The flake
closed on the 0.3.4.1 post-merge CI. No re-occurrence on 0.3.5 CI.

Tracked in DEFERRED.md §Active entry `2026-04-19 — MEMORY_LIMIT
peak-tracker fix (resolved in 0.3.4.1; watch-list entry)` —
*resolved-and-watching* posture: entry exists so any future
regression on this signal has a context pointer; no active work.

---

## 6. Forward ICD presence check

Per METHODOLOGY §3.1 *Forward ICD presence check*:

**Next-N window (N=3):**

| Milestone | Band | ICD exists? | Status |
|---|---|---|---|
| 0.4.0 | [strong] (after §7 slide) | ✅ `ICD-0.4.0-package-structure-validation.md` | authored this session |
| 0.4.1 | [strong] (after §7 slide) | ✅ `ICD-0.4.1-manifest-parsing.md` | authored this session |
| 0.4.2 | [medium] | ❌ not authored | DESIGN-packages-v04x.md §0.4.2 is the contract until a dedicated ICD-authoring slot fires. Per the "one milestone ahead" horizon rule, ICD-0.4.2 is authored in the docs session slotted ahead of 0.4.2 (likely after 0.4.1 merges, paired with 0.4.3). |

**After-N window (milestones 4–7 out, M=7):** 0.4.3, 0.4.4, 0.4.5,
0.4.6, 0.4.7. All [medium], all covered by DESIGN-packages-v04x.md
§§0.4.3–0.4.7. No action — the horizon rule requires ICDs one ahead,
not seven.

**Forward-check result:** No urgent ICD backlog. The 0.4.2
ICD-authoring slot is the next paper-session trigger, to fire after
0.4.1 merges. The arc-closeout session (this one) does not take
0.4.2's ICD because (a) it would push beyond the "one ahead"
horizon at authoring time and (b) 0.4.0/0.4.1 share parser types,
0.4.2 is a distinct concern, pairing 0.4.0+0.4.1 is the natural
grouping.

---

## 7. Disposition — what was fixed here vs. scheduled

### Fixed in this PR (doc-only)

1. `docs/icd/ICD-0.4.0-package-structure-validation.md` — **new**.
2. `docs/icd/ICD-0.4.1-manifest-parsing.md` — **new**.
3. `docs/reviews/RE-EVAL-0.3.x-arc-closeout.md` — **new** (this file).
4. `docs/icd/ICD-0.3.4-cap-call-from-js.md` — new "Implementation
   deviation (0.3.4.1 memory-limit peak tracking)" subsection naming
   `BridgeContext::memory_limit_hit`, the `sample_memory_peak` /
   `was_memory_limit_hit` helpers, the fixture split, and the
   `plinth::log::shutdown()` audit gate (§§2.1, 2.2).
5. `docs/icd/ICD-0.1.7-audit.md` — new "Implementation Notes
   (0.3.4.1)" footer naming `plinth::log::shutdown()` — precedent:
   ICD-0.1.6's own 0.3.3.1 footer (§2.2).
6. `docs/icd/ICD-0.3.5-runtime-hardening.md` —
   - §Where It Applies: "Implementation deviation (0.3.5 placement)"
     note (§2.3).
   - §Open Questions 1 marked **Resolved: deleted posture shipped,
     architect ratification tacit via PR merge** (§2.7).
7. `docs/DEFERRED.md` —
   - Entry 2 (`db.*` PG-type→JS-type OID mapping) pointer updated to
     ROADMAP 0.4.3 (§2.8).
   - Two **new** Active entries for the WS teardown flake and the
     MEMORY_LIMIT peak-tracker watch-list (§5).
8. `docs/ROADMAP.md` —
   - 0.4.0 and 0.4.1 promoted `[medium]` → `[strong]` per §Band
     labels below.
   - Trim completed 0.3.X items per preamble rule (0.3.5 line
     removed; 0.3.0–0.3.4 should already be absent from the first
     re-eval's trim).
9. `docs/CHANGELOG.md` — v0.3.6 arc-closeout entry at the top.

### Scheduled for future sessions (no action this PR)

- **ICD-0.4.2 authoring.** Next docs session slot, to fire after 0.4.1
  merges. Paired with 0.4.3 in a "ICD authoring for 0.4.2 / 0.4.3"
  session — matching the 0.3.3.4 pattern.
- **Cadence re-eval.** `RE-EVAL following 0.4.1` remains on the
  ROADMAP per the preamble cadence rule (every four 3-part
  milestones). This arc-closeout re-eval does NOT displace the
  scheduled one; both are on-track.
- **WS teardown flake dedicated fix.** Not scheduled; architect
  may pick up when tolerance shifts. Captured in DEFERRED.md §5.1.
- **MEMORY_LIMIT flake watchlist entry.** Passive; no scheduled
  action unless a regression surfaces.

---

## 8. Band label review

Per the Plinth labeling rule and the arc-closeout trigger:

| Milestone | Current | New | Rationale |
|---|---|---|---|
| 0.4.0 | medium | **strong** | ICD-0.4.0 authored this session. Load-bearing for the next 3 milestones (entry criterion for 0.4.1 code session). |
| 0.4.1 | medium | **strong** | ICD-0.4.1 authored this session. Load-bearing for 0.4.2+ (parser is shared). |
| 0.4.2 | medium | medium | No ICD. DESIGN-packages-v04x.md §0.4.2 is the contract; promotion waits on ICD authoring. |
| 0.4.3 — 0.4.7 | medium | medium | DESIGN coverage sufficient at this horizon; ICDs land in future one-ahead sessions. |
| 0.5.x | medium | medium | Unchanged. |
| 0.6.x | medium | medium | Unchanged. |
| 0.6a-* | fuzzy | fuzzy | Unchanged. |
| 0.7.x – 1.0 | fuzzy | fuzzy | Unchanged. |

**Promotions applied:** 0.4.0 and 0.4.1 (two items). No demotions.
Landed in §7 item 8.

---

## 9. Observations for methodology / future re-evals

### 9.1 "Contract by pointer" pattern working as designed

The 0.3.3.1 re-eval flagged this pattern (`RE-EVAL-0.3.x.md §6.1`);
0.3.4.1 is its second instance (`BridgeContext::memory_limit_hit`
field + fixture split + audit gate). Both shipped without their own
ICD; both relied on the next re-eval to absorb the surface into the
parent ICD. This re-eval did exactly that (§§2.1, 2.2).

The pattern is working. Observation for methodology: when a 4-part
follow-up ships with "contract by pointer," the CHANGELOG entry could
explicitly name the expected re-eval that will absorb it. 0.3.4.1's
CHANGELOG does name the pattern ("Four-part follow-up, no git tag")
but not the expected re-eval. Minor, but a one-line discipline
tightens the deferral-visibility story the next time someone reads
`git log` between ship and re-eval.

### 9.2 Arc-closeout re-eval is lighter than cadence re-eval

This session's §2 lists 8 items; `RE-EVAL-0.3.x.md §2` listed 11.
Scope difference: the 0.3.3 re-eval swept 0.2.6.x + 0.3.0–0.3.3 + all
fixtures + all tests and found a large `async_bridge_test.cpp` test-
count drift (23 declared vs 17 delivered). This session swept only
the 0.3.4 + 0.3.5 window — 2 milestones, not 4 — and the
forward-ICD-presence rule added in 0.3.3.4 prevented the 0.3.4 code
session from starting without an ICD, which would have been the
biggest drift source.

Methodology-level takeaway: the arc-closeout re-eval's narrower scope
is proportional-investigation-appropriate and doesn't need to
duplicate the cadence re-eval's depth. For cost-control, future
arc-closeout re-evals can follow the same template (inputs read +
new deviations + known-issues preserved + forward-ICD check + band
review + disposition) without rediscovering the prior arc's zero-gap
baseline.

### 9.3 Deviation log as a consolidated artifact

This re-eval consolidates 16 per-milestone deviations into §4. The
per-milestone CHANGELOG entries each named their own deviations; the
consolidated table is a reader-convenience layer for anyone
inheriting the 0.3.X surface. Future re-evals could consider adopting
this as a standard section, updated incrementally rather than
rebuilt-from-scratch each cycle.

---

## 10. Exit criteria

Per METHODOLOGY §3.3:

- [x] Architecture documents read; no gap found that requires an
      architecture-doc amendment.
- [x] Band promotions applied (0.4.0, 0.4.1 — §8).
- [x] Roadmap milestone labels reviewed; two promotions; completed
      0.3.X items trimmed per preamble rule (§7 item 8).
- [x] ICD amendments land in this PR (ICD-0.3.4, ICD-0.1.7 audit
      footer, ICD-0.3.5 — §7 items 4, 5, 6).
- [x] Forward ICD presence check passed (§6).
- [x] Known-issues preserved in durable storage (DEFERRED.md — §5,
      §7 item 7).
- [x] Session output committed to the documentation tree
      (`docs/reviews/RE-EVAL-0.3.x-arc-closeout.md` — this file).
- [x] Code untouched. No C++ / CMake / CI-YAML edits.
- [x] CHANGELOG entry for this release (v0.3.6 arc close-out).

---

## 11. Next actions for the architect

1. **Review this artifact.** Flag any disposition you disagree with,
   especially the ICD-0.3.4 / ICD-0.3.5 / ICD-0.1.7 amendments — they
   add text to existing ICDs and the amendments should read cleanly
   next to the original text.
2. **Tag v0.3.4 and v0.3.5.** Both missing from `git tag --list` as
   of this session. The arc-closeout v0.3.6 tag assumes the sequence
   is v0.3.3 → v0.3.4 → v0.3.5 → v0.3.6; filling the two gaps before
   merging this PR keeps the tag list coherent.
3. **Tag v0.3.6 on merge.** Matches v0.1.8 arc-closeout precedent.
4. **Confirm next cadence re-eval position.** Counting from this arc-
   closeout: if 0.4.0 and 0.4.1 ship under the §8 promotion and the
   `RE-EVAL following 0.4.1` slot fires as currently placed, the
   cadence position is 2/4 at 0.4.1 (0.3.4, 0.3.5, 0.4.0, 0.4.1).
   The arc-closeout counts toward the *re-eval history*, not the
   cadence denominator, so the next cadence re-eval is **after
   0.4.1 as scheduled** — no shift.
