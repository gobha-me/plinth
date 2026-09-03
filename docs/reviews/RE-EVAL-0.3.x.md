# Re-evaluation — following 0.3.3

**Date:** 2026-04-18
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3)
**Trigger:** `RE-EVAL following 0.3.3` item on ROADMAP, reached after v0.3.3.1 merged (commit `3531b1c`). No code milestone past 0.3.3 begins until this item completes.
**Cadence position:** Second scheduled re-eval under the 2026-04-17 cadence. Follow-up to `RE-EVAL-0.2.x`.
**Scope:** 0.2.6.x tail + 0.3.x. Both halves — code-aware first, then structural.

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index — §5 source tree, §8 open questions)
- `docs/architecture/02-capabilities.md §2 Kernel Standard Library; §3 QuickJS Async Bridge`
- `docs/architecture/06-frontend.md` (no 0.3.x touchpoints — confirmed)

### ICDs (0.2.6.x + 0.3.x)
- `ICD-0.2.6-async-dispatch.md`
- `ICD-0.3.0-quickjs-vendoring.md`
- `ICD-0.3.1-runtime-lifecycle.md`
- `ICD-0.3.2-kernel-stdlib-sync.md`
- `ICD-0.3.3-async-bridge.md` (the primary drift target — 0.3.3.1 amendments to follow)
- No ICD for 0.3.3.1: by architect choice, the contract is `DEFERRED.md` (resolved entries) + `ICD-0.3.3 §Critical Invariants`. This re-eval's job is to fold the 0.3.3.1 surface into ICD-0.3.3 itself.

### Code
- `src/kernel/js/` — all 12 TUs (bridge_context, runtime_pool, stdlib_inject, run_on_context, eval, conversion, async_op, stdlib/{log,config,crypto,db,audit}_bindings, db_error_map)
- `src/kernel/ws/connection_registry.{hpp,cpp}` — the 0.3.3.1 static-destruction-order guard
- `src/kernel/capabilities/{resolution,batch}.hpp` — ICD-0.2.6 wrappers
- `tests/kernel/js/` — `TEST_CASE` inventory (17 in `async_bridge_test.cpp`; 5 / 5 / 7 in stdlib / eval / runtime_pool)

### Discussion / design context
- `DESIGN-quickjs-bridge.md` — cross-check that 0.3.x implementation did not narrow 0.3.4+ options (§7.3 streaming opacity, §6.3 cancellation cascade)
- All six `docs/discussion/*.md` files re-surveyed. Two new since RE-EVAL-0.2.x: `DISCUSSION-persona-rbac.md`, `DISCUSSION-post-shell-application-order.md` (both 2026-04-17).
- `docs/DEFERRED.md` — 3 active entries + 1 resolved entry (parallel-fanout / runtime-binder, folded into 0.3.3.1).

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. Each has a proposed resolution; disposition below (§4).

### 2.1 Interface-drift — ICD-0.3.3 §BridgeContext Async Activation

**ICD-0.3.3 §BridgeContext** (line 39) declares: *"Fields reserved-but-dormant in ICD-0.3.1 become populated and active in 0.3.3. **No new struct field is introduced.**"*

**Code** (`src/kernel/js/bridge_context.hpp:125–147`) adds four fields in 0.3.3.1:
- `std::atomic<int> inflight_detached{0}`
- `std::mutex wake_mu`
- `int wake_count = 0`
- `std::coroutine_handle<> waiter_handle`

Plus a new method `signal_completion()` (declaration missing from ICD §Methods list).

The ICD's negative claim ("no new field") was true at 0.3.3 and became false at 0.3.3.1. The code header (lines 125–143) documents the pattern excellently; the ICD does not.

**Resolution:** Amend ICD-0.3.3 §BridgeContext Async Activation with an "Implementation deviation (0.3.3.1 parallel dispatch)" subsection listing the four new fields, the `signal_completion()` method, and a one-paragraph rationale pointing at `DEFERRED.md` §parallel-fanout (resolved) and the `BridgeContext`-UAF window the wake-driven drain closes. Precedent: RE-EVAL-0.2.x §2.1–2.6 amended ICD-0.2.2 the same way. **Fixed in this session.**

### 2.2 Interface-drift — ICD-0.3.3 §PromiseCallbacks field shape

ICD-0.3.3 §PromiseCallbacks (lines 64–67) declares `{resolve, reject}`. Shipped code (`src/kernel/js/async_op.hpp:25–33`) adds `ns_for_cancellation: std::string` (values: `"db"` | `"audit"`) and `register_pending` becomes a 3-arg form `(resolve, reject, ns_for_cancellation)`. The field is load-bearing for the cascade's per-namespace reject codes (`db.cancelled` vs `audit.cancelled`).

**Resolution:** Amend ICD-0.3.3 §PromiseCallbacks struct definition + `register_pending` signature. **Fixed in this session.**

### 2.3 Interface-drift — ICD-0.3.3 §Coroutine Dispatch Loop + §Back-Pressure

ICD §Loop Structure pseudo-code (line 148 onward) serializes `co_await dispatch_async_op(bc, op)` inside the `for (auto& op : bc.take_pending_ops())` body. §Back-Pressure (line 519) describes "insert at head of `pending_ops`, break out of dispatch phase."

Shipped code replaces both with `dispatch_ops_batch_fanout` — each op is spawned fire-and-forget via `drogon::async_run` and `inflight_detached` is bumped; the outer loop suspends on `AnyCompletionAwaiter`. Back-pressure saturation re-queues the remainder but no longer serially `co_await`s.

This is the single largest code-vs-ICD delta in 0.3.x, landed as accepted per `DEFERRED.md` §parallel-fanout (now resolved).

**Resolution:** Rewrite ICD-0.3.3 §Loop Structure and §Back-Pressure to describe the 0.3.3.1 fan-out model (spawn-detached, `inflight_detached` gate, `AnyCompletionAwaiter` suspend). Reference `DEFERRED.md` §parallel-fanout (Resolved) for the design rationale. **Fixed in this session.**

### 2.4 Interface-drift — ICD-0.3.3 §Cancellation Cascade Step 3

ICD §Cancellation Cascade Step 3 (line 476) declares `co_await drogon::sleepCoro(...5s)` — a blind 5-second drain. Shipped 0.3.3.1 replaces this with `while (inflight_detached > 0 && now < deadline) co_await AnyCompletionAwaiter{bc};` — a wake-driven bounded drain, explicitly closing the `BridgeContext`-UAF window that naive parallel fan-out opens.

**Resolution:** Amend ICD §Cancellation Cascade Step 3 to reflect the wake-driven drain + the UAF-closure rationale. The 5-second ceiling remains. **Fixed in this session.**

### 2.5 Arch-silent-on-code — `ConnectionRegistry::initiate_shutdown()` + `g_shutdown_pending`

0.3.3.1 added a file-scope `std::atomic<bool> g_shutdown_pending` in `src/kernel/ws/connection_registry.cpp` + a public `initiate_shutdown()` static method + flag-checks on every public method. The fixture's `atexit` calls `initiate_shutdown()` *before* `drogon::app().quit()`, so late `handleConnectionClosed` events during the Drogon drain find the singleton's hash table untouched.

The pattern is documented fully in the CHANGELOG v0.3.3.1 entry and in the `.cpp` header comment. It is **not** in:
- `docs/icd/ICD-0.1.6-websocket.md` — zero matches for `initiate_shutdown` / `g_shutdown_pending` / `static destruction` / `teardown`.
- `docs/architecture/**` — same.

The pattern is reusable (Meyers singleton + file-scope gate flipped before app teardown) and will come up again for other kernel singletons whose destruction ordering interacts with Drogon's event-loop pool.

**Resolution:** Append an `Implementation Notes (0.3.3.1)` footer to `ICD-0.1.6-websocket.md` naming the pattern and pointing at `connection_registry.cpp` for the reference implementation. Precedent: ICD-0.1.6's own existing *Implementation Notes (0.1.6)* footer. **Fixed in this session.**

### 2.6 Arch-silent-on-code — `conversion.{hpp,cpp}` and `SqlBinderAwaiter`

- `src/kernel/js/conversion.{hpp,cpp}` consolidates JS↔JSON helpers (`js_to_json`, `json_to_js`, extract_error variants) that 0.3.0–0.3.2 had duplicated as anonymous-namespace helpers. Not referenced by any ICD.
- `SqlBinderAwaiter` (inherits `drogon::CallbackAwaiter<Result>`) replaces the 0.3.3 `std::promise/std::future` bridge for runtime-sized params. Named in CHANGELOG + `DEFERRED.md` §runtime-binder (resolved); not in ICD.

Both are internal implementation detail but load-bearing enough to name in the ICD §CI Wiring file list.

**Resolution:** One-line additions to ICD-0.3.3 §CI Wiring under the 0.3.3.1 Implementation deviation subsection. **Fixed in this session.**

### 2.7 Missing-test-for-arch-claim — ICD-0.3.3 §Tests claims 23 cases; 17 delivered

ICD-0.3.3 §CI Wiring (line 638) declares: *"Twenty-three cases across six groups."* §Tests (lines 589–628) enumerates cases A.1–A.6, B.7–B.11, C.12–C.15, D.16–D.17, E.18–E.22, F.23.

`grep -c "^TEST_CASE" tests/kernel/js/async_bridge_test.cpp` returns **17**. Audit against the ICD enumeration:

| Group | ICD cases | Delivered | Gap |
|---|---|---|---|
| A Correctness | A.1–A.6 (6) | All 6 + bonus `parallel with runtime params` (0.3.3.1) | ✓ |
| B Resource limits | B.7 (memory), B.8 (CPU), B.9 (CPU-excludes-async), B.10 (wall-clock), B.11 (max_concurrent) | B.10 (as "wall-clock cancel during fan-out"), B.11 (as concurrency-limit-zero + 8-parallel stress) | **B.7, B.8, B.9 absent** |
| C Cancellation | C.12–C.15 (4) | Covered indirectly by B.10 wall-clock test | **C.12–C.15 absent as discrete cases** |
| D Concurrency | D.16 (10 concurrent), D.17 (10× 5-query) | 8-parallel stress (0.3.3.1 bonus) | **D.16, D.17 absent** |
| E audit | E.18–E.22 (5) | All 5 ✓ | ✓ |
| F TSan smoke | F.23 | Absent | **F.23 absent** |

Net: 11 ICD-declared cases absent, offset by 4 bonus 0.3.3.1 / sync-TypeError cases. Not all gaps are equally load-bearing — B.10's wall-clock test does exercise the C cancellation paths end-to-end, and F.23 depends on the 0.5.x TSan CI job that doesn't exist yet.

**Resolution:** New roadmap item — a small test-backfill slot to close the Group B (resource-limit) gaps and the Group D (concurrency-at-scale) gaps. F.23 TSan smoke is gated on the 0.5.x TSan job and stays deferred there (already tracked by the "ThreadSanitizer CI job for WebSocket + LISTEN/NOTIFY concurrency" testing-security item). Cancellation (C.12–C.15) can either be folded into the test-backfill or consciously relaxed to "covered by B.10 end-to-end" — architect choice. **Scheduled as new roadmap item (see §5).**

### 2.8 Missing-test-for-arch-claim — ICD-0.3.1 §Security Constraint 4 + ICD-0.3.2 §Security Constraint 5

- ICD-0.3.1 §Security Constraint 4 ("defensive route-to-destroy on cancelled release") — the code path exists in `runtime_pool.cpp`; no Catch2 case explicitly invokes `release()` on a cancelled context.
- ICD-0.3.2 §Security Constraint 5 (non-forgeable identity — caller cannot overwrite kernel-computed fields via `log.*` payload) — no Catch2 case asserts caller-supplied `extension_id` is NOT overwritten when the kernel enriches the log event.

Both constraints are enforced in code; both are untested. Small additions, same risk posture as RE-EVAL-0.2.x §2.8 (anonymous-identity safeguard, which became 0.2.6.1).

**Resolution:** Fold into the test-backfill roadmap item. **Scheduled.**

### 2.9 Code-diverged-from-arch — DEFERRED.md entries still live

Audit of `docs/DEFERRED.md` **Active** section against current ROADMAP:

1. **Per-op `SET search_path` for `db.*`** — points at "ROADMAP **0.4.x** (must land before any extension code calls `db.*`)". The 0.4.x band exists; no specific task slot carries this constraint yet.
2. **`db.*` PG-type→JS-type mapping** — points at "ROADMAP **0.3.4** prerequisite." 0.3.4 exists on the roadmap as `cap.call() from JS`, no explicit note about the type mapping work riding along.
3. **Drogon `PgBatchConnection` SqlError typing** — "no ROADMAP slot yet — ad-hoc when needed."

None of these are blocking, but the forward pointers for (1) and (2) are weak. A future code session picking up 0.4.x might miss the `SET search_path` requirement.

**Resolution:** Amend `DEFERRED.md` entries (1) and (2) to point at specific ROADMAP tasks — if no exact task exists, reference the ROADMAP band and clarify that the entry is a sub-task of it. No new ROADMAP items; this is cross-reference cleanup. **Fixed in this session.**

### 2.10 Arch-silent-on-repo — 0.3.3.1 untagged

`git tag --list` ends at `v0.3.3`. Commit `3531b1c` (0.3.3.1 parallel fanout merge) is not tagged. Per `feedback_tagging_rule` (architect): "Tags mark releases (milestone close-outs, arc completions)." 0.3.3.1 is a code milestone with `[strong]` band on the roadmap (now marked `[x]`). Architect confirmed 2026-04-18: "I only tag on X.Y.Z (sometimes I do forget)."

Not a doc gap; a process hygiene note.

**Resolution:** Flag for architect in §8 Next Actions — tag `v0.3.3.1` at commit `3531b1c` if desired, or accept the omission and adopt a tag-only-on-X.Y.Z convention (would affect future 0.3.3.N, 0.4.x.N etc.). **Scheduled as architect decision.**

### 2.11 Observation — ROADMAP as changelog

ROADMAP preamble declares: *"Completed milestones are removed (see CHANGELOG.md for history)."* In practice, 0.1.0 through 0.3.3.1 are all still listed as `- [x]` inline. The file has drifted from index into history.

**Resolution:** Trim the ROADMAP per its own preamble rule — remove completed items. History lives in CHANGELOG + `git log`. The surviving document becomes forward-looking only. **Fixed in this session.**

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.3.0 §Eval API.** `plinth::js::eval` signature + `std::expected<Json::Value, EvalError>` return + 16 MiB memory ceiling + error classification match `src/kernel/js/eval.{hpp,cpp}`.
- **ICD-0.3.1 §RuntimePool.** `acquire` / `release` / `destroy` / `rebuild` shapes match `runtime_pool.{hpp,cpp}`. Pool sizing formula, on-demand transient contexts above `pool_size`, state reset on release, JS interrupt callback, `JS_SetMaxStackSize` with 1 MiB C-stack cap — all faithful. The 0.3.2 extension (BridgeContext as context opaque; stdlib re-injected after `clear_global_own_props`) is a documented addition, not a deviation.
- **ICD-0.3.1 §BridgeContext timer bracket.** `resume_cpu_timer` / `pause_cpu_timer` / `cpu_time_limit_exceeded` / `wall_clock_exceeded` match `bridge_context.{hpp,cpp}`.
- **ICD-0.3.2 §Injected surface.** `log.{info,warn,error}` / `config.get` / `crypto.*` — signatures, argument coercion, error shapes match the per-namespace bindings under `src/kernel/js/stdlib/`.
- **ICD-0.3.3 §AsyncOp contract.** `type`, `callback_id`, and per-variant payload fields in `async_op.hpp` match the ICD table (including reserved variants `HTTP_REQUEST` / `CAP_CALL` / `STORAGE_*` / `PUBSUB_PUBLISH`). `sql_params: std::vector<Json::Value>` type annotation present in ICD line 111.
- **ICD-0.3.3 §Critical Invariants.** 0.3.3.1's parallel fan-out preserves all three: (i) QuickJS state touched only on main loop (detached tasks `queueInLoop` back for `resolve`/`reject`); (ii) CPU-timer bracket around `JS_Eval` / `JS_ExecutePendingJob`; (iii) FIFO back-pressure preserved by `dispatch_ops_batch_fanout`'s re-queue-on-saturation.
- **ICD-0.3.3 §Result Shapes.** `db.query` → `{rows, row_count}`, `db.exec` → `{row_count}`, rejection envelope `{code, message[, sqlstate]}` — all match dispatch arms in `run_on_context.cpp`.
- **ICD-0.3.3 §What Must Not Be Decided Yet bullet 9** (streaming opacity from DISCUSSION-streaming-and-media §0). 0.3.3 keeps `db.query` as a concrete-shape API, not a general capability return envelope — `cap.call` opacity preserved for 0.3.4+.
- **ICD-0.2.6 §Async dispatch wrapper.** `call_capability_async` / `batch_call_capability_async` signatures match `resolution.hpp` / `batch.hpp`. ICD-0.2.2 §Implementation deviation collapse-to-one-line-pointer executed per RE-EVAL-0.2.x §4 forecast.
- **DEFERRED.md resolved entry.** The 0.3.3 parallel-fanout / runtime-binder deferrals cleanly moved to "Resolved" in 0.3.3.1 with code cross-references.

---

## 4. Disposition — what was fixed here vs. scheduled

### Fixed in this PR (doc-only)

1. ICD-0.3.3 §BridgeContext Async Activation — new "Implementation deviation (0.3.3.1 parallel dispatch)" subsection (covers §§2.1, 2.2, 2.6).
2. ICD-0.3.3 §Coroutine Dispatch Loop + §Back-Pressure — rewritten to describe fan-out model (§2.3).
3. ICD-0.3.3 §Cancellation Cascade — Step 3 amended to wake-driven drain (§2.4).
4. ICD-0.3.3 §CI Wiring — `conversion.{hpp,cpp}` and `SqlBinderAwaiter` named (§2.6).
5. ICD-0.1.6 — `Implementation Notes (0.3.3.1)` footer added describing the `ConnectionRegistry::initiate_shutdown()` + `g_shutdown_pending` pattern (§2.5).
6. `docs/DEFERRED.md` — Active entries (1) and (2) cross-reference cleanup (§2.9).
7. `docs/ROADMAP.md` — trim completed items per preamble rule (§2.11); band promotions (§5.1); new items (§5.2); check `RE-EVAL following 0.3.3`.
8. `docs/CHANGELOG.md` — this session's entry.

### Scheduled as new roadmap items (see §5)

- Test backfill for ICD-0.3.3 Groups B/C/D + ICD-0.3.1 §Constraint 4 + ICD-0.3.2 §Constraint 5 (`[strong]`).
- Tests-tidy sweep + CMake glob decision (bundled-from-memory, `[strong]`).
- CI .yml hygiene cleanups (bundled-from-memory, `[strong]`).

### Scheduled as architect decision

- Tag `v0.3.3.1` at `3531b1c`, or not — architect-decide (§2.10, §8).

---

## 5. Paper pass

### 5.1 Band label review

Per the Plinth labeling rule (strong = ICD content exists; medium = DESIGN doc exists, ICDs not yet written; fuzzy = sketch only) and the maintainer's 2026-04-18 approval to promote eligible items:

| Milestone | Current | New | Rationale |
|---|---|---|---|
| 0.3.4 | medium | medium | No ICD yet. DESIGN-quickjs-bridge.md §0.3.4 is the contract surface. Promotion waits on a dedicated ICD-authoring slot (see §5.2). |
| 0.3.5 | medium | medium | No ICD. Same posture as 0.3.4; typically bundled with 0.3.4 in a single ICD-authoring session. |
| 0.4.x | medium | medium | DESIGN-packages-v04x.md covers them. ICDs not yet written; promotion waits on authoring. |
| 0.5.x | medium | medium | `architecture/03-data.md §3` is authority; ICDs pending. |
| 0.6.x | medium | medium | DESIGN-shell-v06x.md covers them; ICDs pending. |
| 0.6a-* | fuzzy | fuzzy | Unchanged. |
| 0.7.x – 0.10.x, 1.0 | fuzzy | fuzzy | Unchanged. |

No promotions applied. 0.3.0 / 0.3.1 / 0.3.2 were candidates per the maintainer's approval but are being trimmed from the roadmap in §2.11 — they no longer carry a band because they no longer exist as pending items. The band-promotion question resolves to "n/a — trimmed."

### 5.2 New roadmap items

Two hygiene items (bundled-from-memory per plan) and one test-backfill item:

**0.3.3.2 — Tests-tidy sweep + CMake scope decision** `[strong]`
Fix 7 pre-existing clang-tidy findings in `tests/kernel/ws/ws_test_fixture.cpp` (see `project_tests_tidy_gap.md` memory). Architect choice: widen `KERNEL_SOURCES` glob to include `tests/kernel/**`, OR add an explicit exclusion comment to `CMakeLists.txt:428-431`. Pair with §2.10 hygiene (CI .yml cleanups from `project_ci_followups_0211.md`: drop `-DCMAKE_C_COMPILER=clang-18` from fuzz job, add `--pull=always` to container blocks).

**0.3.3.3 — Test backfill for ICD-0.3.3 Groups B/C/D + security constraints** `[strong]`
Close the gap between ICD-0.3.3's 23-case declaration and the 17 cases delivered (see §2.7). Specifically: Group B.7 / B.8 / B.9 resource-limit cases; Group D.16 / D.17 scale-concurrency cases; ICD-0.3.1 §Security Constraint 4 defensive-release test; ICD-0.3.2 §Security Constraint 5 non-forgeable-identity test. Group C cancellation cases covered indirectly by Group B.10 — optional to backfill as discrete cases. F.23 TSan smoke stays gated on the existing 0.5.x TSan CI job.

### 5.3 Discussion docs — promote / archive assessment

Six files now. Two new since RE-EVAL-0.2.x:

| File | Status | Feeds | Assessment |
|---|---|---|---|
| DISCUSSION-ai-bridge.md | parked | ~0.12+ | No change. |
| DISCUSSION-cross-cutting-composition.md | parked | ~0.9.x | No change. |
| DISCUSSION-ha-scale-and-offload.md | parked | ~0.9.x+ | No change. |
| DISCUSSION-streaming-and-media.md | parked | 0.6.3 / 0.8.x / 0.10.x; **0.3.x opacity constraint met (see §3 zero-gap)** | Unpark-check passed; stays parked. |
| DISCUSSION-persona-rbac.md | parked (new) | ~0.13+, future DESIGN-persona-rbac.md | No action. Discussion capture correctly marked "not authoritative." |
| DISCUSSION-post-shell-application-order.md | parked (new) | post-0.6 extension ordering | No action. Same posture as persona-rbac; this is an ordering/dependency capture, not a design commitment. |

No promotions, no archives.

### 5.4 Roadmap trim (executed)

Per §2.11, remove all `- [x]` entries from `docs/ROADMAP.md`. History is in `docs/CHANGELOG.md` and `git log main`. The roadmap becomes a forward-looking document only. Preamble's own rule was the authority.

Trim affects: all of 0.1.x (9 items); all of 0.2.x + 0.2.6.x (11 items); the first `RE-EVAL following 0.2.x` line; 0.3.0 / 0.3.0.1 / 0.3.0.2 / 0.3.1 / 0.3.2 / 0.3.3 / 0.3.3.1 (7 items). Testing & Security section's `[x]` items also trimmed; remaining pending items stay. Section headings for fully-completed milestones (0.1, 0.2) are removed.

---

## 6. Observations for methodology / future re-evals

### 6.1 "Sub-milestone with contract cited by pointer" pattern

0.3.3.1 was shipped deliberately without its own ICD. The CHANGELOG entry states plainly: *"No ICD: contract is DEFERRED.md + ICD-0.3.3 §Critical Invariants."* This is a new pattern alongside the *caller-triggered implementation* pattern captured in RE-EVAL-0.2.x §6.1.

**What the pattern looks like:**
- A focused follow-up milestone (Nth-digit task under an existing X.Y.Z milestone) resolves a DEFERRED.md entry by **shipping code**, not by authoring a new ICD.
- The contract for the new surface is cited by pointer into the parent ICD's invariants + the DEFERRED entry itself.
- The next re-eval is the mechanism that catches the ICD up to the shipped reality.

**Why this is acceptable:**
- Authoring an ICD-0.3.3.1 would duplicate ICD-0.3.3 §Critical Invariants + DEFERRED.md §parallel-fanout rationale. The Invariants + DEFERRED entry already *are* the contract in everything but name.
- The re-eval cadence enforces that the parent ICD absorbs the sub-milestone surface before the cadence closes. This re-eval did exactly that (§2.1–§2.4, §2.6).

**Cost to flag:**
- The sub-milestone's surface is invisible to anyone reading only ICD-0.3.3 until the next re-eval absorbs it. For 0.3.3.1 that window was ~1 day (0.3.3.1 merged 2026-04-18; re-eval 2026-04-18). For a future sub-milestone landing further before its cadence re-eval, the window could be weeks.

**Suggestion for a future methodology patch (not this session):**
When a sub-milestone intentionally ships without an ICD ("contract by pointer"), the CHANGELOG entry should explicitly state which re-eval cadence is expected to absorb the surface into the parent ICD. The discipline is a one-line addition. This re-eval's RE-EVAL-0.3.x session would have cited "RE-EVAL following 0.3.3 absorbs this" at shipping time, making the deferral visible to any fresh session reading `git log` before the re-eval runs.

### 6.2 ICD-test-count declarations are a good drift canary

ICD-0.3.3 §CI Wiring declared "Twenty-three cases." Counting `TEST_CASE` in the test file was a 5-second check that surfaced §2.7 (the largest code-aware finding this session, excluding ICD interface drift). This kind of numeric assertion in ICDs is load-bearing for re-eval efficiency — the ICD author knew enough to write "23", and counting to 23 was trivial to verify. Worth noting as best-practice: **declare a test count when possible** so future re-evals can audit cheaply.

### 6.3 Re-eval scope held, just barely

RE-EVAL-0.2.x §6.2 flagged that re-eval scope would grow. For 0.3.x specifically, we narrowed reading to `src/kernel/js/` + `ws/connection_registry` + capabilities touch-surface, rather than full-tree sweep. The "proportional investigation" guidance in METHODOLOGY §3.3 held: read the code that was changed since the last re-eval, not the full kernel. Worth confirming at RE-EVAL following 0.4.x whether this subsystem-focused approach remains sufficient.

---

## 7. Exit criteria

Per METHODOLOGY §3.3:

- [x] Architecture document and roadmap updated per §3.1's questions.
- [x] Band promotions / demotions applied (none this cycle — rationale in §5.1).
- [x] Roadmap milestone labels reviewed; two hygiene items + one test-backfill item added; completed items trimmed per preamble rule.
- [x] New discussion docs written? None surfaced from gap analysis.
- [x] Session output committed to the documentation tree (`docs/reviews/RE-EVAL-0.3.x.md` — this file).
- [x] Code untouched. No C++ / CMake / CI-YAML edits.
- [x] CHANGELOG entry for the rewrite session.

---

## 8. Next actions for the architect

1. **Review this artifact.** Flag any gap disposition you disagree with, especially the ICD-0.3.3 amendments (§§2.1–2.4, 2.6) — they materially rewrite the async bridge's contract text.
2. **Decide whether to tag `v0.3.3.1`** at commit `3531b1c` (§2.10). Either is fine; your 2026-04-18 direction suggests "might have forgotten" — if so, retroactive tag is simple. Alternatively, adopt an "only tag on X.Y.Z" convention and close the question.

> **Resolved in-session (2026-04-18):** adopt the tightened "3-part X.Y.Z only" rule. 0.3.3.1 stays untagged; future 0.3.3.2 / 0.3.3.3 also untagged; v0.3.4 captures the accumulated 4-part work in its tag range. Existing 4-part tags (v0.2.1.1a/b, v0.3.0.1, v0.3.0.2, etc.) stay in place — no history rewrites. One real forget surfaced: **v0.3.0** was never tagged; retroactively tagged at `59cd659`. ROADMAP preamble updated to document the tightened rule. Memory `feedback_tagging_rule.md` updated.
3. **Decide on Group C (cancellation) test backfill.** The 0.3.3.3 roadmap item leaves it optional: either backfill C.12–C.15 as discrete cases, or accept "covered by B.10 end-to-end" and tighten the ICD language.
4. **Next re-eval:** `RE-EVAL following 0.3.3` is this session. The cadence fires again after four more code milestones — if 0.3.3.2 + 0.3.3.3 + 0.3.4 + 0.3.5 ship in sequence, that's exactly four, so the next scheduled re-eval lands between 0.3.5 and 0.4.0. The existing `RE-EVAL following 0.4.1` roadmap item moves one slot earlier or stays put — architect-decide at the next roadmap touch.
