# Re-evaluation — 0.5.x following 0.5.1

**Date:** 2026-04-23
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3) — cadence re-eval, not arc-closeout
**Trigger:** Scheduled ROADMAP item `RE-EVAL following 0.5.1   [rewrite session]` (`docs/ROADMAP.md §0.5`, line 120 post-discharge) — blocks all further 0.5.x code work per ROADMAP preamble ("No further code milestone begins until the preceding re-eval item completes").
**Cadence position:** Seventh scheduled re-evaluation and the first of the 0.5.x arc. Cadence is `every 4 code milestones` per ROADMAP preamble, but this re-eval is scheduled explicitly at the arc-opener + coalescer boundary rather than by 4/4 arithmetic — 0.5.0 and 0.5.1 together closed out the producer half of the realtime bus (listener + emit helper + Layer 3 pubsub publish in 0.5.0; Layer 1 auto-event coalescer in 0.5.1), and the architect flagged a RE-EVAL before 0.5.2 broker opens the consumer half (precedent: `RE-EVAL-0.3.x-arc-closeout.md` fired at 2/4 because 0.3.5 closed the 0.3.X arc). The next cadence slot `RE-EVAL following 0.5.5` (`docs/ROADMAP.md:117`) aligns cleanly with 0.5.2/0.5.3/0.5.4/0.5.5 = 4/4 and needs no edit here.
**Scope:** Code-aware gap analysis across the full 0.5.0–0.5.1.1 window plus the parallel LH-1 stream. The window includes the two tagged milestones **v0.5.0** (PG LISTEN/NOTIFY bridge) and **v0.5.1** (PG auto-event coalescer) plus six four-part follow-ups (0.5.0.1 phase_a/phase_b rename, 0.5.0.2 ICD-LH-1 paper, 0.5.0.3 ICD-0.5.0.3 paper, 0.5.0.4 Tier 2 extension dispatch implementation, 0.5.0.5 ICD-0.5.1 paper, 0.5.1.1 CI red regressions). 0.5.0.4 is load-bearing on its own — it closed the 2026-04-22 DEFERRED "Tier 2 extension capability dispatch" entry that had been open since 0.2.2 and unblocked LH-1. The LH-1 storm-tier diagnostic shipped between 0.5.0.4 and v0.5.1 (2026-04-22, 3 × 120 s clean against the pre-coalescer kernel).

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index-only scan).
- `docs/architecture/02-capabilities.md §1 Capability Registry; §1.13 Drain Primitive; §3 QuickJS Async Bridge; §4 Diagnostic Kernel Surfaces` — normative source for the sync-vs-async resolver contract, Tier 2 in-memory cache semantics, and the LH-0/LH-0.1 diagnostic-surface framing this arc inherits.
- `docs/architecture/03-data.md §3 Realtime + Appendix A + Appendix B` — normative source for the four-layer model, debounced-change-stream contract, 8000-byte payload ceiling, HA realtime posture; §3.1's example envelope shape is the load-bearing reference for the 0.5.1 coalescer deviation (§2.2.2 below).
- `docs/architecture/04-services-ha.md §1 Audit logging; §6.2 State Sharing` — normative for the `plinth:data:*` and `capability.extension.error` audit families the 0.5.0/0.5.0.4/0.5.1 window introduces.
- `docs/architecture/05-extensions.md §1.2 manifest.json; §3 QuickJS Runtime; §3.2 Extension Supervision` — §3.2's "next capability call creates a fresh JS runtime" framing predates the per-extension `RuntimePool` 0.5.0.4 landed; covered in §2.2 below.

### ICDs
- `docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md` — v0.5.0's contract; implemented as PR #66 commit `f3552b3`.
- `docs/icd/ICD-0.5.0.3-extension-dispatch.md` — 0.5.0.4's contract; implemented as PR #71 commit `97b5dae`.
- `docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md` — v0.5.1's contract; implemented as PR #74 commit `90d37fc`.
- `docs/icd/ICD-LH-1-listen-notify-storm.md` — LH-1's contract; implemented as PR #72 commit `7953eae`.
- `docs/icd/ICD-0.2.2-capability-resolution.md`, `ICD-0.2.4-capability-rbac.md`, `ICD-0.2.6-async-dispatch.md` — consulted for the sync-vs-async resolver framing 0.5.0.4 completes.
- `docs/icd/ICD-0.3.3-async-bridge.md`, `ICD-0.3.4-cap-call-from-js.md`, `ICD-0.4.4-package-install-lifecycle.md` — consulted for the `AsyncOp` snapshot pattern, `BridgeContext::extension_name` population, and install-lifecycle hook sites.

### Code (spot-verified against ICD §Library Surface / §Handler Surface text for every shipped TU in the window)
- `src/kernel/realtime/listener.{hpp,cpp}` — `register_handler`, `start_listener(const Config::Database&, const Config::Realtime::Listener&)`, `stop_listener`, `apply_notification_for_test` all public per ICD-0.5.0 §Listener Subsystem. Signature extended from the ICD-pinned `(const Config::Database&)` per v0.5.0 §Scope deviations — ratified in §4.
- `src/kernel/realtime/emit.{hpp,cpp}` — `emit_notify(PGconn&, envelope)`, `emit_notify_async(DbClientPtr, envelope)`, `validate_envelope`, `set_max_payload_bytes` / `get_max_payload_bytes` all public per ICD-0.5.0 §NOTIFY Emission Helper.
- `src/kernel/realtime/channel.{hpp,cpp}` — `validate_channel`, `channel_layer`, `channel_extension` all public per ICD-0.5.0 §Channel Naming Scheme. Pure-function hand-rolled validator replaces `std::regex` per §OQ3 (deviation ratified in the v0.5.0 CHANGELOG entry, called out in §4).
- `src/kernel/realtime/coalescer.{hpp,cpp}` — `CoalescerRegistry::instance()` / `start` / `shutdown` / `record_write` / `drain_extension` + six test seams (`apply_flush_for_test`, `open_window_count_for_test`, `set_emit_hook_for_test`, `clear_emit_hook_for_test`, `clear_windows_for_test`, `set_db_client_for_test`) all public per ICD-0.5.1 §Coalescer State Machine.
- `src/kernel/realtime/sql_classify.{hpp,cpp}` — `classify_sql(std::string_view sql, std::string_view ext_name) -> std::optional<SqlClass>` pure function public per ICD-0.5.1 §SQL Classification.
- `src/kernel/extensions/runtime_registry.{hpp,cpp}` — `init_registry(const Config&)`, `shutdown_registry()`, `create_pool(std::string_view)`, `destroy_pool(std::string_view)`, `dispatch(name, fn, args, caller, call_depth)` all public per ICD-0.5.0.3 §RuntimeRegistry.
- `src/kernel/capabilities/resolution.cpp` — `dispatch_tier2` extension arm rejects on sync path with `CapabilityError::ASYNC_REQUIRED` and dispatches to `plinth::extensions::dispatch` on async path; five new `cap.*` codes flow through `EXTENSION_DISPATCH_FAILED` per ICD-0.5.0.3 §Error taxonomy.
- `src/kernel/js/async_op.hpp` — `bc_extension_name` field added (0.5.1), `pubsub_channel` / `pubsub_payload` / `PUBSUB_PUBLISH` variant (0.5.0).
- `src/kernel/js/run_on_context.cpp` — `run_pubsub_publish_outcome` arm (0.5.0); `run_db_exec_outcome` coalescer hook after `affectedRows()` capture (0.5.1); `run_cap_call_outcome` unchanged (extension dispatch composed via `call_capability_async`).
- `src/kernel/js/stdlib/pubsub_bindings.cpp` — 7-step validation chain per ICD-0.5.0 §`pubsub.publish` (sync arg types → cancelled check → channel regex → Layer-3 gate → extension-identity gate → payload-size check → enqueue).
- `src/kernel/js/stdlib/db_bindings.cpp` — populates `op.bc_extension_name = bc.extension_name` at DB_EXEC enqueue per ICD-0.5.1 §AsyncOp extension-identity snapshot.
- `src/kernel/config.{hpp,cpp}` — `Config::Realtime` substruct (0.5.0) with `apply_realtime` that *throws* on invalid input (deliberate convention deviation ratified in v0.5.0 CHANGELOG); `Config::Realtime::Coalescer` substruct (0.5.1); `apply_realtime` split into `apply_realtime_{listener, notify, coalescer}` helpers for clang-tidy cognitive-complexity compliance.
- `src/kernel/main.cpp` — atexit chain now runs in order `ConnectionRegistry::initiate_shutdown` → `realtime::stop_listener` → `CoalescerRegistry::instance().shutdown()` → `extensions::shutdown_registry()` → `drogon::app().quit()` → `log::shutdown`. The 0.5.0 CHANGELOG deviation (listener placement "between `capabilities::stop_notify_listener` and `ws::shutdown_js_stress_pool`" in ICD-0.5.0 traces) is superseded by the actual slot between `ConnectionRegistry::initiate_shutdown` and `log::shutdown`; ratified in §4.
- `src/kernel/packages/install_lifecycle.cpp` — three `CoalescerRegistry::instance().drain_extension(pkg.name)` sites (DISABLE, UNINSTALL, UPGRADE); five `extensions::create_pool` / `destroy_pool` sites per ICD-0.5.0.3 §Lifecycle.
- `src/kernel/audit/handlers.cpp` + `src/kernel/groups/handlers.cpp` — 0.5.1.1 added `drogon::app().isRunning()` guard + `std::call_once` around `drogon::registerHandler` calls (production path unchanged; grouped-subprocess re-entry made idempotent). Described in 0.5.1.1 CHANGELOG.
- `tests/kernel/realtime/{sql_classify, coalescer, coalescer_integration}_test.cpp` — 22 + 21 + 5 = 48 new Catch2 cases per ICD-0.5.1 §Test count target. Library path fully covered; PG-gated integration covered.
- `tests/kernel/capabilities/dispatch_extension_test.cpp` — 10 cases shipped per 0.5.0.4 CHANGELOG; 10 ICD-0.5.0.3 cases deferred (see §2.3).
- `tests/kernel/realtime/{listener, emit}_test.cpp` + `tests/kernel/js/pubsub_test.cpp` — 21 new cases for v0.5.0 (R.01–R.10, E.01–E.06, P.01–P.05). `pubsub_test.cpp:117` P.01 carries the pre-existing ~15% flake noted in 0.5.1.1 (see §2.6).

### Discussion / design context
- `docs/design/DESIGN-quickjs-bridge.md §3.2 Runtime Pool; §8.1 Capability Dispatch` — normative design behind the per-extension `RuntimePool` model 0.5.0.4 consumes.
- `docs/design/DESIGN-capability-registry.md §Tier 2 Cache Invalidation` — the 0.2.3 precedent ICD-0.5.0's listener generalizes.
- `docs/DEFERRED.md` — six active entries re-examined (§5).
- `project_ws_flaky_segfault.md`, `project_plinth_state.md`, `project_post_0_5_0_candidates.md` (session memory) — consumed into §2.5 and §5.
- `feedback_deterministic_teardown.md`, `feedback_icd_horizon.md`, `feedback_tagging_rule.md` — consumed into §3 and §4.

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. The window closes with **three ICD-amendment items**, **three architecture-document amendments**, **one DEFERRED.md substantive update plus three pointer tightens**, and **one ROADMAP insertion** (ICD-0.5.2 authoring four-part follow-up). No missed-test items block any ship criteria; 10 deferred test cases from ICD-0.5.0.3 stay in the backlog as a scheduled follow-up.

### 2.1 Interface-drift — ICD-0.5.1 `shutdown_drain` audit never wired; replaced with silent skip

ICD-0.5.1 §Audit Events declares `realtime.coalescer.shutdown_drain` as a post-drain audit emitted from `CoalescerRegistry::shutdown()` completion. The v0.5.1 implementation ships with the audit **dropped entirely** — the `shutdown()` method performs the synchronous window drain but does not call `plinth::log::audit`.

The deviation rationale is load-bearing: `shutdown()` runs from main's atexit chain, which fires *after* `spdlog::shutdown()` has been called (in `main`, on `drogon::app().run()` return) and *after* SIGTERM has nulled drogon's `DbClientManager`. Both audit paths (`plinth::log::audit` dereferences the null DbClient manager via `drogon::app().getDbClient`; an `spdlog` fallback dereferences the null default logger) crash with SIGSEGV. The v0.5.1 CHANGELOG §Scope deviations records this with a gdb-confirmed reproduction on the first LH-1 trial.

The ICD text is load-bearing — a fresh session will read §Audit Events, expect three audits, find two, and either re-add the third (reintroducing the crash) or hunt for the missing call site. The CHANGELOG deviation is not sufficient; per METHODOLOGY §Phase 2 Constraint #4, the ICD owns the deviation record.

**Resolution.** ICD-0.5.1 — new "Implementation deviation (v0.5.1 ship)" subsection ahead of §Audit Events naming the dropped audit and the lifecycle-ordering rationale. The equivalent diagnostic lives in the timer-fired `realtime.coalescer.flush_failed` audit (reachable while the kernel is up) and the `open_window_count_for_test` seam (asserts zero post-shutdown). A future main.cpp lifecycle cleanup (move `spdlog::shutdown` to the end of the atexit chain) would let the audit be reinstated; the deviation note flags this as a follow-up shape. **Fixed in this session.**

### 2.2 Arch-silent-on-code — three architecture documents missed structural changes in the 0.5.0–0.5.1 window

#### 2.2.1 `architecture/02-capabilities.md §3` silent on the sync-vs-async dispatch arm + per-extension `RuntimePool`

0.5.0.4 (PR #71, `97b5dae`) introduced a structural change to capability dispatch: extension-provider entries are now **async-only**. Sync `call_capability` rejects with `CapabilityError::ASYNC_REQUIRED` / `cap.async_required`; async `call_capability_async` dispatches into `plinth::extensions::dispatch`. Five new `cap.*` rejection codes (`cap.async_required`, `cap.extension_not_loaded`, `cap.handler_not_found`, `cap.handler_load_failed`, `cap.handler_threw`) extend the `cap.*` taxonomy. Per-extension `plinth::js::RuntimePool` instances are owned by a new process-lifetime `plinth::extensions::RuntimeRegistry` with install-lifecycle hooks (`create_pool` / `destroy_pool`). The `server/handlers/<fn>.js` ES-module + default-export convention is now normative (promoted from fixture observation by ICD-0.5.0.3).

The architecture document is silent on every one of these surfaces. §3 points at `DESIGN-quickjs-bridge.md` for runtime mechanics and `ICD-0.5.0.3` appears only in `ICD-0.5.0.3`'s own References list; the arch-doc reader has no pointer into the async-arm contract or the RuntimePool ownership model. §4 Diagnostic Kernel Surfaces documents LH-0 + LH-0.1 but not LH-1 (which ships zero new kernel surface per ICD-LH-1 §3, so §4 omission is defensible — see §2.2.3 for the broader note).

**Resolution.** `architecture/02-capabilities.md` — amend in two places:

1. New subsection `§3.1 Async Dispatch Arm + Extension Runtimes` appended to `§3 QuickJS Async Bridge`. Describes `call_capability_async` as the sole entry for extension-provider capabilities (sync path rejects with `cap.async_required`); summarises the five new `cap.*` rejection codes as a reader-visible taxonomy; names `plinth::extensions::RuntimeRegistry` as the owner of per-extension `RuntimePool` instances with install-lifecycle hooks; points at ICD-0.5.0.3 as the contract.
2. One-paragraph addition to `§3 QuickJS Async Bridge` preamble after "uses C++20 coroutines" noting that extension-dispatch is the canonical async path (distinguishes from the `lh0:1:js_stress` fork's diagnostic bypass in §4).

Pattern matches `RE-EVAL-0.4.x-arc-closeout.md §2.3` (`drain.{hpp,cpp}` paragraph appended to §3). **Fixed in this session.**

#### 2.2.2 `architecture/03-data.md §3.1` ids-present example contradicts v0.5.1 envelope shape

ICD-0.5.1 §OQ4 explicitly pins `ids` **always absent** in v0.5.1 envelopes — an acknowledged deviation from `architecture/03-data.md §3.1`'s example which shows `"ids": ["a1","a2","..."]`. The v0.5.1 CHANGELOG confirms this was pinned per the ICD recommendation (no architect redirect). The deviation is deliberate and load-bearing: the 0.5.5 monotonic-seq + client-SDK work picks up the `ids` slot when the optimistic-update path actually ships; forcing v0.5.1 to populate `ids` would require rewriting INSERTs with `RETURNING id` (not universally applicable) and UPDATE/DELETE `WHERE`-clause introspection (much harder). The architect-level rationale is correct; the architecture document simply hasn't caught up.

Additionally, `architecture/03-data.md §3` is silent on three real v0.5.0/v0.5.1 surfaces a reader needs to find:

- The `CoalescerRegistry` subsystem + its dedicated `trantor::EventLoopThread` + the fixed-duration (non-extending) 50 ms window semantics.
- The single physical PG channel `plinth:realtime` as the fan-in carrier for all Layer 1/2/3 envelopes (the `channel` field inside the envelope carries the logical layer-specific channel name).
- The `window_ms` envelope field v0.5.1 populates.

**Resolution.** `architecture/03-data.md` — three edits:

1. §3.1's example envelope updated to match v0.5.1 shape: `ids` removed from the example, `window_ms: 50` added, a footnote flags "v0.5.1 omits `ids`; 0.5.5 may reintroduce via `RETURNING id` wrapping — see ICD-0.5.1 §OQ4 and ICD-0.5.5 (future)". The two example envelopes in §3.1 (insert-update mixed, insert-count-only-truncated) both get the update.
2. New subsection `§3.1.1 Auto-Event Coalescer (subsystem)` inserted after the Debounce mechanism bullets. Names `CoalescerRegistry` as the owner, the dedicated `trantor::EventLoopThread` as the timer driver, the fixed-duration window semantics (not classic debounce — flush at `opened_at + window_ms`, not `last_write + window_ms`), the drain-on-DISABLE/UPGRADE/UNINSTALL contract, and points at ICD-0.5.1 for the full spec.
3. New subsection `§3.6.1 Physical Channel Fan-In` appended to §3.6 HA Realtime. Names `plinth:realtime` as the single physical PG channel carrying every envelope; distinguishes from the logical per-layer channel names in the envelope body; references the v0.5.0 listener as the fan-in consumer.

**Fixed in this session.**

#### 2.2.3 `architecture/05-extensions.md §3.2` stale runtime-lifecycle framing

Line 273 of `architecture/05-extensions.md` says "After a failure, the extension remains available. The next capability call creates a fresh JS runtime." This framing predates the per-extension `RuntimePool` 0.5.0.4 landed — today, a failure tears down the specific `BridgeContext` (ICD-0.3.1 `destroy(bc)`) and the next call acquires a fresh `BridgeContext` from the pool; the pool itself persists until the extension's DISABLE/UPGRADE/UNINSTALL transition (or kernel shutdown). The "fresh JS runtime per call" framing is operationally wrong in a way that matters for readers trying to understand why crash-recovery is cheap.

**Resolution.** `architecture/05-extensions.md §3.2` — one-paragraph clarification appended to the "On failure" bullet list, noting that the `BridgeContext` is destroyed but the `RuntimePool` persists (per ICD-0.5.0.3 §RuntimeRegistry); the next call acquires a fresh context from the pool, not a new runtime. Pointer into ICD-0.5.0.3 §Lifecycle for the full ownership story. **Fixed in this session.**

### 2.3 Missing-test-for-arch-claim — ICD-0.5.0.3 ships with 10 deferred test cases

0.5.0.4 CHANGELOG §Tests explicitly flags 10 of the ICD-0.5.0.3 test plan cases as TODO: R.02 (WS end-to-end), R.03 (audit attribution), E.07 (call-depth 8-deep chain), P.01 (pubsub from handler), P.04 (`db.query` from handler — tied to DEFERRED `SET search_path`), P.05 (cross-extension recursion), H.02 (UPGRADE rebuild), H.03 (DISABLE mid-dispatch race), C.01 (cancellation propagation), C.02 (wall-clock expiry tear-down). The rationale is harness-cost plus three cases (P.04, P.05, E.07) having external dependencies (search_path wiring, two-fixture install harness, 8-deep nested fixture respectively).

The library-level dispatch contract is proven end-to-end by R.01, E.01–E.06, P.02 (pubsub identity gate), P.03 (RBAC at resolver), and H.01 (destroy/create cycle) — 10 cases shipped. The deferred 10 cover extension-reachable surfaces (WS, audit, nested, cross-extension) that the LH-1 storm tier already exercises under a different test discipline (production kernel, not Catch2 subprocess). No operational gap.

**Resolution.** New DEFERRED.md entry dated 2026-04-23 under §Active, titled *"ICD-0.5.0.3 R.02/R.03/E.07/P.01/P.04/P.05/H.02/H.03/C.01/C.02 — extension-dispatch test coverage"*. Three of the ten (P.04 `db.query`, P.05 cross-extension, C.02 wall-clock) are deferred on external-dependency grounds; the other seven are harness-deferred. Points at the `0.5.x.N HTTP test harness` ROADMAP slot for the R.02 case (WS end-to-end through `POST /api/packages` flow) and names the remaining nine as ad-hoc follow-up candidates when the fixture harness extends. **Fixed in this session** (DEFERRED.md entry).

### 2.4 DEFERRED divergence — WS-teardown entry's empirical footing materially advanced by 0.5.1.1

The `2026-04-19 / updated 2026-04-22 — WS teardown flake (now scoped to `[js][async]` Catch2 subprocess race)` entry was last updated 2026-04-22 with LH-0.1's empirical finding (production kernel clean under load). 0.5.1.1 added a second empirical dataset on v0.5.1's commit, and the arithmetic materially revises one claim in the existing entry:

- **#38 per-TEST_CASE (current CI mode): 92 pass / 8 SEGV = 8.0% flake rate.** Signatures `free_zero_refcount` / `list_empty(gc_obj_list)` match the `project_ws_flaky_segfault.md §Candidate root causes` list.
- **#38 under grouped `[hardening]` single subprocess: 89 pass / 11 SEGV = 11.0%.** Grouping does NOT help — the 2026-04-21 claim "`[js][async][hardening]` alone clean" does not hold at 100-iter scale on v0.5.1's commit.
- **#47 `plinth_tests_js` P.01 pubsub (`tests/kernel/js/pubsub_test.cpp:117` `cv.wait_for(5s, ...)` timed out): ~15% in isolation**, never filed. Pre-existing from v0.5.0 merge (commit `f3552b3` — `[js][realtime][integration]` tag-overlap violation in the fixture class).

The pubsub P.01 flake is a new observation from the 0.5.1.1 verification pass that should enter the DEFERRED log and be folded into the existing `0.5.x.N [js][async] kernel-side refcount investigation` ROADMAP scope (signature family points at the same root cause — sequential subprocess runs under `async_bridge_fixture`; the `[realtime]` fixture composes `async_bridge_fixture` so the teardown race reaches it).

**Resolution.** DEFERRED.md — update the WS-teardown entry with:

1. The three new measurements (100-iter per-TEST_CASE, 100-iter grouped-hardening, isolated P.01 rate).
2. Explicit invalidation of the 2026-04-21 "[js][async][hardening] alone clean" claim at 100-iter scale.
3. One additional paragraph noting the pubsub P.01 flake as a sibling signature in the same family; cross-references 0.5.1.1 CHANGELOG §Verification.

ROADMAP — update the `0.5.x.N [js][async] kernel-side refcount investigation` scope line to include pubsub P.01 as an exemplar of the family. **Fixed in this session.**

### 2.5 DEFERRED divergence — `db.*` search_path + type-mapping pointers stale

Two active DEFERRED entries (2026-04-18 per-op `SET search_path` and 2026-04-18 `db.*` PG-type→JS-type mapping) currently point at "0.5.x `db.*` binding work" as the resolution slot. 0.5.0 shipped `emit_notify_async` which does NOT go through the extension `db.*` dispatch arm (kernel-direct writes). 0.5.1's coalescer HOOKS the DB_EXEC path but reads sql/params already bound by the extension — it does not wrap. 0.5.0.4 unblocked extension-provider `db.query` via the handler but routed through the same existing `run_db_query_outcome` path with no search_path or type-mapping change. The first real opportunity to wire either is **0.5.3 db.batch() + silent mode**, which per ROADMAP line 114 opens the transactional wrapper the `SET LOCAL search_path TO ext_<id>, plinth` pattern naturally joins onto.

ICD-0.5.0.3 §Implementation latitude anticipated this — explicitly noted "Whether to update `DEFERRED.md §2026-04-18` (search_path for `db.*`) in 0.5.0.4 since the P.04 test depends on it" and left the call open. 0.5.0.4 deferred P.04 + preserved the DEFERRED entry, which is the correct ship posture.

**Resolution.** DEFERRED.md — tighten both entries' pointer text from "0.5.x db.* binding work" → "0.5.3 `db.batch()` + silent mode". Rationale subsection extended to cite 0.5.0.4's P.04 deferral as the latest confirming data point. **Fixed in this session.**

### 2.6 Newly surfaced flake — pubsub P.01 in `[js][realtime][integration]`

See §2.4 for treatment — folded into the WS-teardown entry. Observation-only here; no standalone DEFERRED entry + no new ROADMAP slot (the existing `[js][async]` refcount slot absorbs it).

### 2.7 Forward ICD presence — ICD-0.5.2 missing; 0.5.2 is the next code milestone

ROADMAP §0.5 carries 0.5.2 WS broker fan-out as `[medium]` with no ICD authored. Per METHODOLOGY §3.1 Phase 0 *Roadmap Milestone Labels*, any milestone entering the strong-window (next-3) should be `[strong]` with a pinned ICD. With this re-eval discharged, 0.5.2 becomes the next code milestone — strong-window by definition.

`feedback_icd_horizon.md` pins "only write ICDs one milestone ahead" — the ICD-0.5.2 paper session should land as a four-part follow-up before 0.5.2 opens. Precedent: 0.4.5.2 authored ICD-0.4.6 ahead of 0.4.6 implementation; 0.5.0.5 authored ICD-0.5.1 ahead of v0.5.1.

**Resolution.** ROADMAP — two edits:

1. Promote 0.5.2 `[medium]` → `[strong]`.
2. Insert `0.5.1.2 ICD-0.5.2 authoring [strong]` four-part follow-up line between the discharged `RE-EVAL following 0.5.1` line and the `0.5.2 WebSocket broker` line.

0.5.3 (`db.batch()` + silent) and 0.5.4 (`plinth.events`) stay `[medium]` — both remain inside the strong-window after 0.5.2 ships, but per one-ahead the next ICD authoring slot is 0.5.2's; 0.5.3's own ICD authoring will be scheduled at the next re-eval following 0.5.2 (or explicitly as `0.5.2.N`). **Fixed in this session.**

### 2.8 No cadence-drift

The arc-opener boundary absorbed this scheduled re-eval cleanly. Two tagged 3-part code milestones landed (v0.5.0, v0.5.1) plus a substantial four-part follow-up (0.5.0.4) that closes a 2.0-era deferral. The next cadence line `RE-EVAL following 0.5.5` sits at `ROADMAP.md §0.5` (line 127 post-merge) and aligns 4/4 over 0.5.2/0.5.3/0.5.4/0.5.5. No cadence-line edit needed in this session.

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.5.0 §Listener Subsystem** — `register_handler`, `start_listener`, `stop_listener`, `apply_notification_for_test` all present at `src/kernel/realtime/listener.hpp:42–69`. `start_listener` signature-extension ratified in §4 (deliberate deviation per v0.5.0 CHANGELOG, not drift).
- **ICD-0.5.0 §Channel Naming Scheme** — `validate_channel` + `channel_layer` + `channel_extension` all present at `src/kernel/realtime/channel.hpp:28–37`. Hand-rolled dispatcher vs `std::regex` ratified in §4 (OQ3 pin). Three-layer regex shape + 63-byte ceiling enforced in `channel.cpp`.
- **ICD-0.5.0 §Payload Envelope Contract** — `layer` + `channel` required; `schema`/`table`/`ops`/`seq`/`truncated`/`payload`/`emitted_at` optional. Exact shape in `validate_envelope`. Envelope reserves `seq` slot for 0.5.5 (unpopulated in 0.5.0/0.5.1); `truncated` slot reserved for 0.5.1 (populated when truncation fires).
- **ICD-0.5.0 §NOTIFY Emission Helper** — `emit_notify(PGconn&, envelope)` + `emit_notify_async(DbClientPtr, envelope)` + `set_max_payload_bytes` / `get_max_payload_bytes` all present at `src/kernel/realtime/emit.hpp`.
- **ICD-0.5.0 §`pubsub.publish` JS binding** — 7-step validation chain implemented at `src/kernel/js/stdlib/pubsub_bindings.cpp` in ICD order. Rejection codes `pubsub.cancelled` / `pubsub.channel_invalid` / `pubsub.extension_mismatch` / `pubsub.payload_too_large` / `pubsub.pg_error` all match ICD §Rejection codes.
- **ICD-0.5.0 §Audit Events** — `realtime.listener.started`, `realtime.listener.reconnected`, `realtime.notify.rejected` all emitted from the matching call sites. LH-1 §7.1 confirmed zero `listener.reconnected` under storm-tier load (kernel-log artifact).
- **ICD-0.5.0.3 §RuntimeRegistry** — `init_registry` / `shutdown_registry` / `create_pool` / `destroy_pool` / `dispatch` all present at `src/kernel/extensions/runtime_registry.hpp:52–98`. Lifecycle hooks at the documented `install_lifecycle.cpp` sites.
- **ICD-0.5.0.3 §Error taxonomy** — five new `cap.*` rejection codes all shipped with matching spellings per 0.5.0.4 CHANGELOG §Shipped surface.
- **ICD-0.5.0.3 §Handler contract** — `server/handlers/<fn>.js` ES-module + default-export convention implemented via the fixed wrapper source + `import_from_src` QuickJS intrinsic. `bc.extension_name = target extension` (pool-populated) + `bc.user = caller's UserContext` (value-copy) per Security Constraints 1+3.
- **ICD-0.5.0.3 §Security Constraints** — all eight constraints shipped as pinned:
  - 1 Caller identity authoritative (value-copy on `BridgeContext`).
  - 2 RBAC at caller boundary (resolver check pre-dispatch; no re-check in callee).
  - 3 `bc.extension_name` pool-set (pubsub identity gate matches).
  - 4 Call depth `caller + 1` across extension boundary (MAX_CALL_DEPTH=8 uniform).
  - 5 Handler source read from active install (no traversal; signature-parser validates `<fn>`).
  - 6 Per-hop `RuntimeLimits` (no additive composition; wall-clock resets each hop).
  - 7 Cancellation cascades across boundary (`bc.cancelled` threaded into target).
  - 8 No mutable bridge (JSON round-trip).
- **ICD-0.5.1 §SQL Classification** — three supported shapes (qualified INSERT / UPDATE / DELETE) + unqualified-with-implicit-schema fallback + skip cases (DDL / SELECT / CTE writes / BEGIN/COMMIT/SET / multi-statement) all implemented in `classify_sql`. Case-insensitive keyword match with non-ident delimiter check (no partial-match bug).
- **ICD-0.5.1 §Coalescer State Machine** — first-write opens fixed-duration window; subsequent writes accumulate (no timer extension); zero-row no-ops; cross-extension writes warn; flush builds envelope + emits via `emit_notify_async`; erase on flush. Seven test seams enable integration + state-machine coverage without PG dependency.
- **ICD-0.5.1 §Envelope Assembly** — `{layer:"data", channel:"plinth:data:<schema>.<table>", schema, table, ops:[{insert, count}, {update, count}, {delete, count}], window_ms}`. Always-three ops pinned per OQ7; `ids` absent per OQ4; `seq` absent (0.5.5); `emitted_at` absent (0.5.4 persistence layer).
- **ICD-0.5.1 §Truncation Heuristic** — drop `ids` (absent in 0.5.1; no-op), drop envelope + audit `flush_failed` with `reason="payload_too_large"` on overshoot. Exercised by T.01–T.04 test cases.
- **ICD-LH-1 §Harness producer + subscriber** — `lh1storm` driver extension (name corrected from `ext_lh1_storm` per 0.5.0.4 CHANGELOG §Next scheduled work), external PG LISTEN subscriber via `github.com/lib/pq`, 4×4×16×512 B default storm-tier profile all shipped. LH-1 3-trial diagnostic produced zero reproductions (pre-coalescer commit); v0.5.1 regression run 3-trial produced zero reproductions at measured throughput improvement.
- **Atexit ordering** — `ConnectionRegistry::initiate_shutdown` → `realtime::stop_listener` → `CoalescerRegistry::instance().shutdown()` → `extensions::shutdown_registry()` → `drogon::app().quit()` → `log::shutdown`. Matches `feedback_deterministic_teardown.md` pattern; `ws_test_fixture.cpp` + `async_bridge_fixture.cpp` mirror.
- **0.5.1.1 deterministic test fixes** — four `tier3_not_available` → `async_required` expectation flips in `listener_integration_test.cpp` (match the 0.5.0.4 ICD-0.5.0.3 taxonomy); one in `rbac_test_runner_test.cpp` (same family); `register_audit_routes` + `register_group_routes` wrapped in `drogon::app().isRunning()` guard + `std::call_once` (grouped-subprocess idempotent, production-path unchanged). Benchmark build break closed by `benchmarks/extension_dispatch_stub.cpp` + `plinth_quickjs` link.
- **Cross-arc invariants re-checked:** no new long-running threads beyond the ones accounted for (v0.5.0 listener jthread, v0.5.1 coalescer `trantor::EventLoopThread`); no new `DbClient` pool; `validate()` pure constraint (ICD-0.4.0 §Security Constraint 3) unchanged in 0.5.x.

---

## 4. Accepted deviations catalog (consolidated)

Walked each CHANGELOG `§Scope deviations from ICD` / `§Shipped surface` block across the window with the question *"does this deviation's rationale point at an ICD wording bug rather than a kernel-convention alignment?"* Seven deviations total; six ratify cleanly as kernel-convention or external-constraint, one triggered §2.1's ICD amendment.

| # | Milestone | Deviation | Category | Resolution |
|---|-----------|-----------|----------|------------|
| D1 | v0.5.0 | `start_listener` signature extended to `(const Config::Database&, const Config::Realtime::Listener&)` | ICD-text imprecision | Ratified; R.02 (`enabled=false` no-spawn) directly testable, `reconnect_backoff_ms` flows in-API. Forward-compatible. |
| D2 | v0.5.0 | `apply_realtime` throws `std::runtime_error` rather than warn-and-default | Kernel-convention (deliberate new convention for realtime block) | Ratified in CHANGELOG; hard-rejection of out-of-range realtime knobs prevents production mistakes. |
| D3 | v0.5.0 | Channel validator is hand-rolled rather than `std::regex` | OQ3 implementation latitude | Ratified; ~40 LOC dispatcher gives precise rejection reasons and zero ICU dependency. |
| D4 | v0.5.0 | R.04 simplified to bad-port reconnect + stop-interrupt timing check | ICD-text imprecision (live-toggle infeasible in CI) | Ratified; covers the same invariant (reconnect loop + interruptible backoff). |
| D5 | v0.5.0 | `packages.installed` smoke-retrofit dropped | Shipping-order | Ratified; E.05 was `LAYER_MISMATCH` throughout, not a smoke; no smoke-emission needed. |
| D6 | v0.5.0 | Atexit slot "between `ConnectionRegistry::initiate_shutdown` and `log::shutdown`" vs ICD Traces' "between `capabilities::stop_notify_listener` and `ws::shutdown_js_stress_pool`" | ICD-text imprecision | Ratified; shipped slot is the correct dependency ordering for listener-lifetime-requires-log invariant (per ICD-0.5.0 §Ordering rationale — the text §Ordering is right; only Traces is stale). No ICD edit warranted. |
| D7 | LH-1 | Driver extension `ext_lh1_storm` → `lh1storm`; dropped `pubsub.publish` RBAC rule + grant | ICD-text imprecision | Ratified in LH-1 CHANGELOG + 0.5.0.4 CHANGELOG §Next scheduled work. Extension-identity gate is sufficient; RBAC rule was redundant. |
| D8 | 0.5.0.4 | 10 ICD-0.5.0.3 test cases deferred (R.02/R.03/E.07/P.01/P.04/P.05/H.02/H.03/C.01/C.02) | Shipping-order | New DEFERRED entry landed in §2.3. |
| D9 | 0.5.0.4 | `call_capability_async` signature extended with two optional output pointers (`ext_detail_code_out`, `ext_detail_message_out`) | ICD-text imprecision (forward-compatible) | Ratified; zero-behavior-change for non-extension callers. |
| D10 | v0.5.1 | `realtime.coalescer.shutdown_drain` audit dropped entirely | External-constraint (spdlog + drogon torn down before atexit fires) | ICD amendment — see §2.1. Triggered this RE-EVAL's only ICD text edit. |

No deviations ratify as "bug"; all ten either record the ICD-text imprecision discharged inline or flag a kernel-convention choice properly documented in CHANGELOG.

---

## 5. DEFERRED.md status (7 entries examined)

Re-examined `docs/DEFERRED.md` §Active entries:

1. **ICD-0.4.4 I.18/I.19/I.20 — HTTP test harness for `/api/packages`** (2026-04-20). Active, pointer holds (`0.5.x.N HTTP test harness`). No change.
2. **ICD-0.4.5 X.05–X.13 + G.03 — extended upgrade and GC test coverage** (2026-04-22). Active, pointer holds (same slot as #1). No change.
3. **Per-op `SET search_path` for `db.*`** (2026-04-18). Active; pointer tightened "0.5.x `db.*` binding" → "0.5.3 `db.batch()` + silent mode" per §2.5.
4. **`db.*` PG-type→JS-type mapping** (2026-04-18). Active; pointer tightened per §2.5 (same slot as #3).
5. **WS teardown flake / `[js][async]` Catch2 subprocess race** (2026-04-19 / updated 2026-04-22 / 2026-04-23). Active; entry updated per §2.4 with the 100-iter empirical dataset, invalidation of the 2026-04-21 "[js][async][hardening] clean" claim, and folding-in of the pubsub P.01 observation.
6. **MEMORY_LIMIT classifier peak-tracking** (2026-04-19). Watchlist, resolved. No regression signal in the 0.5.x window. No change.
7. **Drogon `PgBatchConnection` `SqlError` typing** (2026-04-18). Active, ad-hoc. No change.

**Addition:** §2.3 adds one new entry for the ICD-0.5.0.3 deferred test cases.

**Resolved this window (moved to §Resolved earlier):**

- 2026-04-22 — Tier 2 extension capability dispatch (resolved 0.5.0.4).
- 2026-04-22 — `phase_a` / `phase_b` public-surface rename (resolved 0.5.0.1).

---

## 6. Forward ICD presence check

Next-N window (N=3) after this re-eval discharges 0.5.1:

| Milestone | Band | ICD | Disposition |
|---|---|---|---|
| **0.5.2** WS broker fan-out | `[medium]` → **`[strong]`** | **missing** | Promoted. `0.5.1.2 ICD-0.5.2 authoring [strong]` scheduled ahead (see §2.7). |
| **0.5.3** `db.batch()` + silent mode | `[medium]` | missing | Stays `[medium]`; ICD authored at a later paper session (likely `0.5.2.N`). |
| **0.5.4** `plinth.events` + delta sync | `[medium]` | missing | Stays `[medium]`. DESIGN pointer in `architecture/03-data.md §3.5` holds. |

Cross-cutting `0.5.x.N` items (HTTP test harness `[strong]`; `[js][async]` refcount investigation `[medium]`) retain their bands — no promotion/demotion warranted.

The **one-ahead horizon** (`feedback_icd_horizon.md`) is satisfied by scheduling ICD-0.5.2 only. ICD-0.5.3 authoring stays deferred until 0.5.2 ships or a later re-eval tightens the horizon.

---

## 7. Cadence / labels update

- Discharged `RE-EVAL following 0.5.1` (ROADMAP §0.5, line 120).
- Next cadence slot is `RE-EVAL following 0.5.5` (ROADMAP §0.5, line 127) — 4/4 over 0.5.2/0.5.3/0.5.4/0.5.5.
- No preemptive cadence insertion — 0.5.x has enough explicit slots that the arc-level cadence is not at risk.
- Promoted 0.5.2 `[medium]` → `[strong]`; inserted `0.5.1.2 ICD-0.5.2 authoring [strong]` ahead.
- No other band movements.

---

## 8. Verification

Docs-only session; no code build.

- **Precedent match:** structure + tone mirror `RE-EVAL-0.4.x-following-0.4.4.md` (the closest cadence-re-eval precedent). Gap-count + amendment-count within range: four ICD/arch amendments here vs three there; comparable scope.
- **Cross-reference round-trip:** CHANGELOG entry → RE-EVAL doc → ICD/arch amendments → ROADMAP/DEFERRED edits all cite each other. Spot-checked during write.
- **File path resolution:** every file path cited in this RE-EVAL doc is a real on-disk path at the merge commit.
- **Architect read-through:** final gate per the cadence-re-eval posture.

Verification passes. Session delivers the post-0.5.1 contract cleanup the next 0.5.2 broker session picks up cold.

---

## Appendix — Session-produced artifacts

- This document (`docs/reviews/RE-EVAL-0.5.x-following-0.5.1.md`).
- `docs/icd/ICD-0.5.1-pg-auto-event-coalescer.md` — new "Implementation deviation (v0.5.1 ship)" subsection (§2.1); new "Resolved Open Questions (v0.5.1)" appendix.
- `docs/architecture/02-capabilities.md` — new `§3.1 Async Dispatch Arm + Extension Runtimes` subsection and preamble paragraph (§2.2.1).
- `docs/architecture/03-data.md` — envelope example edits (§3.1, `ids` removed, `window_ms` added); new `§3.1.1 Auto-Event Coalescer (subsystem)` subsection; new `§3.6.1 Physical Channel Fan-In` subsection (§2.2.2).
- `docs/architecture/05-extensions.md` — `§3.2` runtime-lifecycle clarification paragraph (§2.2.3).
- `docs/DEFERRED.md` — new ICD-0.5.0.3 test-coverage entry (§2.3); WS-teardown entry updated with 100-iter data + P.01 fold-in (§2.4); search_path + PG-type-mapping pointers tightened to 0.5.3 (§2.5).
- `docs/ROADMAP.md` — `RE-EVAL following 0.5.1` discharged; 0.5.2 promoted `[medium]` → `[strong]`; `0.5.1.2 ICD-0.5.2 authoring [strong]` inserted; `0.5.x.N [js][async] refcount investigation` scope-line mentions pubsub P.01 (§2.4, §2.7).
- `docs/CHANGELOG.md` — new 2026-04-23 rewrite-session entry (un-tagged per `feedback_tagging_rule.md`) cites this RE-EVAL and lists every amendment.
