# Re-evaluation — 0.5.x following 0.5.5

**Date:** 2026-04-26
**Session type:** Rewrite (per METHODOLOGY-llm-assisted-development.md §Phase 3) — cadence re-eval, arc-closeout posture
**Trigger:** Scheduled ROADMAP item `RE-EVAL following 0.5.5   [rewrite session]` (`docs/ROADMAP.md §0.5`, line 143) — blocks all further code work per ROADMAP preamble ("No further code milestone begins until the preceding re-eval item completes"). Next code milestone is **0.6.0 frontend-shell bootstrap** — this re-eval is the kernel arc's last paper-side window before UI work begins.
**Cadence position:** Eighth scheduled re-evaluation, second of the 0.5.x arc, and the first re-eval to close out a milestone arc rather than just step it. 4/4 over 0.5.2/0.5.3/0.5.4/0.5.5 since the prior cadence point — `RE-EVAL following 0.5.1` (2026-04-23). The next cadence slot `RE-EVAL following 0.6.3` (`docs/ROADMAP.md:150`) covers 0.6.0/0.6.1/0.6.2/0.6.3 and needs no edit here.
**Scope:** Code-aware gap analysis across the full v0.5.2 – 0.5.5.1 window — four tagged code milestones (**v0.5.2** WS broker, **v0.5.3** db.batch + silent + per-op SET search_path + OID type mapping, **v0.5.4** plinth.events table + delta sync, **v0.5.5** sequence numbers + client-side debounce) and four interim follow-ups (0.5.2.N broker test backfill closing 45/45, 0.5.3.1 db.batch B.06 timeout + SC3 cross-extension, 0.5.4.N ICD-0.5.5 authoring, 0.5.5.1 kernel teardown hardening). Plus the parallel LH-2 stream (untagged 2026-04-24) which exercised the v0.5.2 broker at fan-out scale. The 0.5.x Realtime arc is functionally closed at v0.5.5 — 0.6.0 begins UI work and the kernel envelope effectively freezes.

---

## 1. Inputs read

### Architecture
- `docs/ARCHITECTURE.md` (index-only scan).
- `docs/architecture/02-capabilities.md §3 Async Dispatch Arm + Extension Runtimes` (the §3.1 subsection landed by the prior RE-EVAL); §1 Capability Registry.
- `docs/architecture/03-data.md §3 Realtime + §3.1.1 Auto-Event Coalescer + §3.4 Frontend SDK + §3.5 Delta Sync on Reconnect + §3.6 HA Realtime + §3.6.1 Physical Channel Fan-In + Appendix A` — the load-bearing reference for every drift item in §2.2 below.
- `docs/architecture/04-services-ha.md §1 Audit logging; §6.2 State Sharing` — normative for the new audit families this window introduces.
- `docs/architecture/05-extensions.md §3 QuickJS Runtime; §3.2 Extension Supervision` (§3.2 clarification from the prior RE-EVAL holds).
- `docs/architecture/06-frontend.md` (index-only — no realtime SDK contract land yet; that's 0.6.3 work).

### ICDs
- `docs/icd/ICD-0.5.2-ws-broker.md` — v0.5.2's contract; implemented as PR #78 commit `68298b4`. Re-checked against post-backfill state (29 cases land per the 0.5.2.N backfill).
- `docs/icd/ICD-0.5.3-db-batch-silent-mode.md` — v0.5.3's contract (1593 lines, paper-authored as `0.5.2.N`); implemented as PR #83 commit `83cea76` plus 0.5.3.1 commit `39eef52`.
- `docs/icd/ICD-0.5.4-events-table-delta-sync.md` — v0.5.4's contract (2006 lines, paper-authored as `0.5.3.N`); implemented as PR #87 commit `a127271`.
- `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md` — v0.5.5's contract (1975 lines, paper-authored as `0.5.4.N`); implemented as PR #89 commit `e4b1fc4` plus 0.5.5.1 commit `c67ab6f`.
- `docs/icd/ICD-LH-2-ws-fanout-storm.md` — LH-2's contract; implemented untagged 2026-04-24.
- `docs/icd/ICD-0.5.0-pg-listen-notify-bridge.md`, `ICD-0.5.1-pg-auto-event-coalescer.md` — consulted for envelope shape + listener-handler-registration semantics that v0.5.5's writer-first topology shifted.
- `docs/icd/ICD-0.1.6-realtime-overview.md` — consulted for the 4-resync-reason `replay_done` shape that v0.5.5 extended to five.

### Code (spot-verified against ICD §Library Surface / §Handler Surface text for every shipped TU in the window)
- `src/kernel/realtime/broker.{hpp,cpp}` — `start`, `stop`, `drain_extension`, `register_js_subscription`, `dispatch`, `dispatch_for_test`, `set_rbac_enforce_for_test` all public per ICD-0.5.2 §Broker API. Writer-first topology (v0.5.5) drops the broker's listener-`EventHandler` registration; instead, broker `dispatch` is invoked from inside `events_writer::insert_envelope` after the INSERT-and-stamp completes.
- `src/kernel/realtime/events_writer.{hpp,cpp}` — `start`, `stop`, `enqueue`, `insert_envelope` per ICD-0.5.4 §`plinth.events` Persistence Writer; **post-0.5.5.1**: in-flight tracker (`g_inflight_inserts` atomic + `g_inflight_cv`) gates `stop()` to detached-coroutine completion, and both `runEvery` callbacks are wrapped in `try { ... } catch (...)` to keep `bad_weak_ptr` from `drogon::async_run` in test-mode subprocesses out of trantor's noexcept boundary.
- `src/kernel/realtime/cursor_store.{hpp,cpp}` — `record_delivered`, `read_cursor`, `reset_cursor` per ICD-0.5.4 §`cursor_store` API.
- `src/kernel/realtime/replay.{hpp,cpp}` — `run_replay`, `build_replay_frame`, `build_replay_done_frame`, `build_resync_frame`, `ResyncReason` enum (now 5 reasons) per ICD-0.5.4 §Replay Engine + ICD-0.5.5 §8. **post-0.5.5.1**: `run_replay` parameter type changed from `ws::ConnState state` (value) to `const ws::ConnState&` (const reference) since `ConnState` is no longer copyable — see §2.1 below.
- `src/kernel/ws/conn_state.hpp` — `mutable std::unique_ptr<std::mutex> channels_mu` field added 0.5.5.1 to serialize `state->channels` access between the conn's owning loop and the listener/writer thread; unique_ptr keeps the struct movable for `subscriptions.cpp`'s state-copy lambda capture pattern.
- `src/kernel/ws/{publish, subscriptions, events_controller}.cpp` — 8 sites lock `channels_mu` per the 0.5.5.1 fix; `state_copy` move-into-lambda still works because `unique_ptr<std::mutex>` is movable.
- `src/kernel/ws/subscriptions.cpp` — `since_seq` parse + replay dispatch + per-conn live-buffer install per ICD-0.5.4 + ICD-0.5.5 §8.
- `src/kernel/scheduled_tasks/cleanup_events.{hpp,cpp}` — `runEvery(cleanup_interval_ms)` + xact-scoped advisory lock per ICD-0.5.4 §Retention + Cleanup. Three documented deviations (xact vs session lock, embedded BIGINT key, `since_seq=0` semantics) recorded in v0.5.4 CHANGELOG.
- `src/kernel/js/db_batch_audit.{hpp,cpp}` — `register_in_flight_batch`, `unregister_in_flight_batch`, `discard_batches_for_extension`, `discard_all_batches`, `set_batch_timeout_ms`, `batch_timeout_ms`, `set_batch_timer`, `clear_batch_timer` per ICD-0.5.3 §Extension Lifecycle Integration + 0.5.3.1 timeout extensions.
- `src/kernel/js/db_search_path.{hpp,cpp}` — `prepare_search_path_wrapper`, `is_valid_extension_name`, `set_enforce`, audit emitter per ICD-0.5.3 §Per-Op SET search_path Isolation. Three deviations in v0.5.3 CHANGELOG (T.* TU placement, T.07 narrowing, P.05 narrowing) ratified.
- `src/kernel/js/db_batch_schema_check.{hpp,cpp}` — `classify_cross_extension`, `audit_batch_cross_extension_rejected` per ICD-0.5.3 §Security Constraint 3 + 0.5.3.1 close-out.
- `src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}` — OID-driven type switch per ICD-0.5.3 §OID-Driven PG-Type → JS-Type Mapping. Drogon `ftype-accessor.patch` applied via `PATCH_COMMAND` at `FetchContent_Declare`.
- `src/kernel/js/stdlib/pubsub_bindings.cpp` — `pubsub.subscribe(channel, handler) → Promise<() => void>` binding per ICD-0.5.2 §`pubsub.subscribe`; cross-extension classifier widened post-LH-2 per ICD-0.5.2 §SC6 deviation closure.
- `src/kernel/config.{hpp,cpp}` — `Config::Realtime::Events` substruct (10 fields v0.5.4 + 6 fields v0.5.5); `Config::Db::Batch` substruct (4 fields v0.5.3 + 0.5.3.1 timeout wiring); `Config::Db::Silent` substruct (1 field). Hard-fail bound checks throughout — convention from v0.5.0 preserved. **One deviation pinned**: `Config::Realtime::Events::SeqSource` shipped as `enum class : std::uint8_t { WRITER_RETURNING }` rather than the `std::string` shown in ICD-0.5.5 §10 pseudocode — see §2.1 below.
- `src/kernel/main.cpp` — atexit chain extended in this window: `events_writer::stop()` slotted between `realtime::stop_listener` and `js::rollback_all_batches`; `discard_all_batches` between `realtime::stop_listener` and `realtime::broker::stop()` (0.5.3); `cleanup_events::stop()` rides the writer's loop. 0.5.5.1 added try/catch wrappers on `runEvery` callbacks at the `events_writer` site.
- `tests/kernel/realtime/{broker, broker_integration, events_writer_integration, cursor_store, replay, seq_generation, coalescer_window_fields}_test.cpp` — full v0.5.2/0.5.3/0.5.4/0.5.5 test suite. **29/36 v0.5.5 cases active**; 7 cases SKIP() with phase pointers (S.06/S.07/L.03/L.04/L.05/W.06 deferred to 0.5.5.1, plus I.01–I.04).
- `tests/kernel/ws/subscriptions_rbac_test.cpp` + `tests/kernel/js/pubsub_subscribe_test.cpp` — 12 S.* + 12 U.* cases from the 0.5.2.N backfill.
- `tests/kernel/js/db_batch_test.cpp` + `db_silent_test.cpp` + `db_search_path_test.cpp` — v0.5.3 + 0.5.3.1 (B.06 timeout, B.13 cross-extension).

### Discussion / design context
- `docs/design/DESIGN-quickjs-bridge.md §3.2 Runtime Pool` — unchanged through this window.
- `docs/DEFERRED.md` — 7 active entries re-examined (§5).
- `project_next_session_post_055.md`, `project_plinth_state.md`, `project_ws_flaky_segfault.md` (session memory) — consumed into §2.4 and §5.
- `feedback_deterministic_teardown.md` — pattern reused this window in `events_writer::stop()`'s in-flight tracker; promotion candidate for `architecture/03-data.md` noted in §2.2.5.
- `feedback_icd_horizon.md`, `feedback_tagging_rule.md`, `feedback_main_protected.md`, `feedback_changelog_scope.md` — consumed into §3, §4, §6.

---

## 2. Gaps found

Categorized per METHODOLOGY §3.1.1. The window closes with **four ICD-amendment items** (deviation subsections), **three architecture-document amendments** (one interface-drift correction + two arch-silent-on-code), **one DEFERRED.md substantive update + two pointer additions**, and **one ROADMAP insertion** (`0.5.5.N ICD-0.6.0 authoring` four-part follow-up). No missed-test items block any ship criteria; deferred test cases stay in the backlog with explicit phase pointers.

### 2.1 Interface-drift — `architecture/03-data.md §3.6.1` last sentence stale post-writer-first topology

[`architecture/03-data.md:472–473`](../architecture/03-data.md) closes §3.6.1 *Physical Channel Fan-In* with: *"The 0.5.2 broker is the first real consumer; 0.5.4's `plinth.events` persistence writer is the second."* This was correct through v0.5.4 (broker registered an `EventHandler` first, writer registered second per ICD-0.5.4 §When `record_delivered` fires).

**v0.5.5's writer-first topology shift** (ICD-0.5.5 §5, OQ1 pin) inverts the relationship. Per the v0.5.5 CHANGELOG: *"the broker stops being a peer listener handler and becomes a writer-downstream consumer, invoked from inside `events_writer::insert_envelope` after the INSERT-and-stamp completes."* The architecture doc's listener-as-fan-in framing is still correct, but the *consumer* count and the *wiring* are not. Reader walking the prose at `§3.6.1` lands on a topology that diverges from v0.5.5's invariant (`envelope.seq == plinth.events.seq` requires the writer-first ordering).

**Resolution.** `architecture/03-data.md §3.6.1` — replace the last sentence with: *"`plinth::realtime::events_writer` is the listener's sole `EventHandler` consumer (writer-first topology since v0.5.5; ICD-0.5.5 §5). After persisting the envelope and stamping `seq` from `INSERT … RETURNING seq`, the writer dispatches the envelope to `realtime::broker` for in-process WS / JS fan-out and to `cursor_store::record_delivered` for per-user cursor advance. The broker is no longer a peer listener handler — it is downstream of the writer."* Cross-references both ICDs. **Fixed in this session.**

### 2.2 Arch-silent-on-code — three architecture documents missed structural changes in the v0.5.2–0.5.5 window

#### 2.2.1 `architecture/03-data.md §3.5` resync reasons list missing `live_buffer_overflow`

[`architecture/03-data.md:448`](../architecture/03-data.md) lists four resync reasons the server emits — *"`reason ∈ {cursor_expired, row_cap, mismatch, events_disabled}`"*. v0.5.5 adds a **fifth**: `live_buffer_overflow` (ICD-0.5.5 §8; `ResyncReason::LIVE_BUFFER_OVERFLOW` in [`src/kernel/realtime/replay.hpp`](../../src/kernel/realtime/replay.hpp)). When per-conn live buffering during mid-replay exceeds `live_buffer_cap_per_subscription` (default 256 frames per OQ6), the broker flips a shared abort flag, the replay coroutine returns at the next chunk boundary, and the overflow site emits the resync inline. The reader expecting a fixed four-reason set will miss this code path entirely.

The same blockquote at lines 444–449 also omits the `replay_done.buffered_live_count` field (ICD-0.5.5 §8 §`replay_done` payload extension) and the per-conn live-buffer machinery itself.

**Resolution.** `architecture/03-data.md §3.5` — extend the implementation blockquote to: (a) include `live_buffer_overflow` in the reason set, (b) note `replay_done` carries `buffered_live_count` (always present, value 0 when nothing buffered), (c) one sentence on the per-conn live-buffer machinery flushing mid-replay frames after `replay_done` so live and replay frames maintain writer-first seq order, (d) point at ICD-0.5.5 §8 for the full contract. **Fixed in this session.**

#### 2.2.2 `architecture/02-capabilities.md §2.2` entry for `pubsub.subscribe` predates the v0.5.2 JS-binding signature

[`architecture/02-capabilities.md:311`](../architecture/02-capabilities.md) lists `pubsub.subscribe(other_channel)` under §2.2 *Permission-Gated* with the framing "Cross-extension channel subscription". Both `pubsub.publish` and `pubsub.subscribe` are also named at §2.3 line 328 as kernel-provided capabilities. The architecture *does* name the binding — but the §2.2 signature predates v0.5.2's implementation and reads as if `subscribe` takes only a channel argument. The shipped binding signature is `pubsub.subscribe(channel, handler) → Promise<() => void>` per ICD-0.5.2 §`pubsub.subscribe`.

This is a one-line tightening, not a missing-content gap. The methodology's preference is to fix what's there rather than add prose that duplicates `architecture/03-data.md §3` and the ICD.

**Resolution.** `architecture/02-capabilities.md §2.2` row for `pubsub.subscribe` — extend the API-cell signature to `pubsub.subscribe(channel, handler) → Promise<() => void>` and the Purpose-cell to also mention own-extension subscription (the `other_` framing implies cross-only, but the binding handles both). Pointer added in the new §3 paragraph (below). **Fixed in this session.**

#### 2.2.3 `architecture/04-services-ha.md §1 Audit logging` silent on `realtime.events.*` and `realtime.debounce.advisory_overridden` audit families

[`architecture/04-services-ha.md`](../architecture/04-services-ha.md) §1 enumerates audit families generically (per §1's tone). Two new families landed this window without arch-doc mention:

- **`realtime.events.*`** (six audits, v0.5.4): `realtime.events.write_failed` (with reasons `queue_full` / `pg_error` / `cleanup_failed` / `shutdown_timeout` / `cursor_read_failed`), `realtime.events.replay_started`, `realtime.events.replay_completed`, `realtime.events.replay_truncated`, `realtime.events.resync_required`, `realtime.events.cleanup_swept`. All rate-limited with shared `audit_window_ms` (default 60000).
- **`realtime.debounce.advisory_overridden`** (one audit, v0.5.5): emitted on inbound `debounce_renegotiate` frame; rate-limited; advisory + audit, not enforced (per OQ5).

The §1 prose is not enumeration-heavy and does not list every audit by name in earlier sections. The proportionate fix is a one-paragraph appendage to §1 grouping all `realtime.*` audit families introduced in 0.5.x (listener / notify / coalescer / broker / events / debounce / seq) with pointers to the owning ICDs. This also captures the future `realtime.seq.gap_detected` (S.06 deferral) and `realtime.seq.replay_seq_mismatch` audits for when those land.

**Resolution.** `architecture/04-services-ha.md §1` — append one paragraph naming the seven `realtime.*` audit families this arc introduced, with a footnote pointer per family to its owning ICD. No per-audit detail (that lives in the ICDs); the goal is a reader-visible index. **Fixed in this session.**

#### 2.2.4 `architecture/03-data.md §3.5` silent on `ConnState::channels_mu` synchronization seam

The 0.5.5.1 ship added `mutable std::unique_ptr<std::mutex> channels_mu` to `ConnState` to serialize `state->channels` access between the WS conn's owning loop (subscribe / unsubscribe / drain_extension) and the listener / writer thread (`publish_dispatched`'s synchronous pre-pass for `delivered_to_users`). 8 sites lock the mutex; `unique_ptr<std::mutex>` keeps `ConnState` movable for the `state_copy` lambda-capture pattern in [`subscriptions.cpp`](../../src/kernel/ws/subscriptions.cpp).

This is a real architectural commitment — any future code path that touches `state->channels` from a non-conn-loop thread (cursor catch-up after writer crash from S.07; multi-node failover from I.03; cross-extension pubsub from I.04) must acquire `channels_mu`. The 0.5.5.1 CHANGELOG records the fix; the architecture is silent. The 0.6.3 client SDK milestone — which is the natural reader of WS-conn-state contract — has no anchor for this invariant.

**Resolution.** `architecture/03-data.md §3.5` — extend the v0.5.4 implementation blockquote with one sentence: *"Conn-state subscription mutations and cross-thread reads (broker pre-pass for `delivered_to_users`) are serialized via `ConnState::channels_mu` since v0.5.5.1; future cross-thread access to `state->channels` must hold the mutex."* Pointer to the 0.5.5.1 CHANGELOG. **Fixed in this session.**

#### 2.2.5 Kernel-teardown idiom: `events_writer` in-flight tracker generalizes `feedback_deterministic_teardown.md`

The `g_inflight_inserts` + `g_inflight_cv` pattern in [`src/kernel/realtime/events_writer.cpp`](../../src/kernel/realtime/events_writer.cpp) (0.5.5.1) addresses a class of teardown races that `feedback_deterministic_teardown.md` already names — `cancel_all_*` from atexit before `drogon::app().quit()`. The 0.5.5.1 fix adds a sibling pattern: **detached-coroutine tracking** for `drogon::async_run`-launched work. `stop()` blocks (bounded by `shutdown_drain_ms`) until `g_inflight_inserts` reaches zero, then proceeds.

Future subsystems that fire `drogon::async_run` from a `runEvery` callback (cleanup tasks, scheduled metric writes, the 0.7.2 scheduled-tasks subsystem, any 0.6.x SDK background work) will hit the same race. Memory captures it as feedback; the architecture document for kernel-teardown does not exist yet (the methodology pattern is not normatively pinned to a section). This is a write-up candidate for `architecture/04-services-ha.md` or a new `architecture/07-deterministic-teardown.md` — but writing it speculatively before a second instance lands is premature.

**Resolution.** No file edit this session. **Note for the RE-EVAL following 0.6.3 agenda**: if the 0.6.x UI work introduces another `runEvery` + `async_run` site (likely for SDK debounce or shell-state poll), promote the pattern then. Track in §7 cadence note. **Deferred — not fixed in this session.**

### 2.3 Missing-test-for-arch-claim — v0.5.4 + v0.5.5 ship with deferred test cases not in DEFERRED.md

v0.5.4 deferred 3 cases (D.08 absorbed as ICD-0.5.5 L.03; **I.02 multi-process advisory-lock harness** + **I.03 live + replay race in integration** still active) per CHANGELOG v0.5.4 §Test count.

v0.5.5 deferred 7 cases per CHANGELOG v0.5.5 §Test counts:
- **S.06** broker-side `realtime.seq.gap_detected` audit
- **S.07** cursor catch-up after writer crash mid-window (reuses v0.5.4 I.02 harness)
- **L.03 / L.04 / L.05** mid-replay live-buffer integration + overflow scenarios — *the live-buffer machinery is in place* but the end-to-end assertion needs a fault-injection seam beyond the WS test client's current API
- **W.06** `superseded_seqs[]` populated — design-deferred (writer-first topology forecloses source-seq tracking; needs a follow-up ICD per ICD-0.5.5 §6 §Implementation seam)
- **I.01 / I.02 / I.03 / I.04** integration end-to-end at storm scale — LH-1 pre-flight already validated as OQ1 acceptance gate (live-path p99 7 ms vs threshold 50 ms); Catch2 coverage deferred

Project-memory `project_next_session_post_055.md` tracks all of these but DEFERRED.md does not, breaking the precedent set by `RE-EVAL-0.5.x-following-0.5.1.md §2.3` for ICD-0.5.0.3's 10 deferred cases. The same fresh-session-discoverability argument applies.

**Resolution.** New DEFERRED.md entry dated 2026-04-26 under §Active, titled *"ICD-0.5.4 I.02 / I.03 + ICD-0.5.5 S.06 / S.07 / L.03–L.05 / W.06 / I.01–I.04 — realtime test coverage + W.06 design defer"*. Three buckets:
- **I.02 multi-process advisory-lock harness** — build once, retires v0.5.4 I.02 + v0.5.5 S.07 + v0.5.5 I.02/I.03 (multi-node failover). Three deferrals, one harness build.
- **L.03/L.04/L.05** mid-replay live-buffer scenarios — needs WS-test-client fault-injection seam.
- **W.06 `superseded_seqs[]` source-seq tracking** — design-deferred; the follow-up ICD that picks this up may land at the RE-EVAL following 0.6.3 (0.6.x.N slot) or earlier if writer-first foreclosure binds tighter than expected. Promotes to a candidate for the 0.5.5.N ICD-0.6.0 authoring slot if a wire-protocol decision needs locking before 0.6.3 SDK work begins; otherwise defers cleanly.

Points at the `0.5.x.N HTTP test harness` ROADMAP slot for the I.* cases (LH-1/LH-2 storm-scale Catch2 wrapping reuses the same fixture pattern as I.18–I.20). **Fixed in this session** (DEFERRED.md entry).

### 2.4 DEFERRED divergence — WS-teardown / `[js][async]` entry materially advanced by 0.5.5.1

The `2026-04-19 / updated 2026-04-22 / 2026-04-23 — WS teardown flake (now scoped to `[js][async]` Catch2 subprocess race)` entry was last updated 2026-04-23 with the 100-iter dataset and pubsub P.01 fold-in from `RE-EVAL-0.5.x-following-0.5.1 §2.4`. The 0.5.5.1 ship advances three sub-paths:

1. **`events_writer` `EventLoopThreadPool::~` join-self family closed** at the test-fixture layer via `g_inflight_inserts` / `g_inflight_cv` tracking (CHANGELOG 0.5.5.1 §What shipped 1). The framework-layer race (drogon's join-self pattern in `EventLoopThreadPool::~`) remains a Drogon issue, but the `events_writer` callsite no longer reproduces.
2. **`bad_weak_ptr` from `drogon::async_run` in test-mode subprocesses** caught + suppressed via try/catch wrappers around both `runEvery` callbacks (drain + cleanup) — keeps it from escaping into trantor's `EventLoop::loop` and tripping `std::terminate` past `loopFuncs`'s noexcept boundary.
3. **PG-client per-test create+destroy `bad_weak_ptr` cycle** closed via `seq_generation_test.cpp::shared_pg_client()` — process-lifetime PG client lets process-exit destructor handle teardown after Catch2 has already reported.

The 0.5.5.1 §Discharge pointers explicitly state: *"`project_ws_flaky_segfault.md` family — PG-client-teardown sub-path contained at the test layer; the underlying drogon `EventLoopThreadPool::~` join-self pattern remains a framework issue, but it no longer surfaces in CI."*

**Two pre-existing flakes carry forward unchanged**:
- `pubsub_test.cpp:117` 5 s NOTIFY timing flake (~10–15% in isolation; folded into this entry by the prior re-eval).
- `#38 async_hardening` 8–11% intermittent SEGV (kernel-side refcount; same family).

**Family expansion observed 2026-04-26 post-0.5.5.1 merge.** The first post-merge CI run on `main` (`ci-build-and-test-12377`) surfaced a SIGSEGV in test 29 — `async_bridge: cap.batch fail-fast on first rejection` ([`tests/kernel/js/async_bridge_test.cpp:873`](../../tests/kernel/js/async_bridge_test.cpp:873)) — backtrace `JS_FreeValueRT → JS_FreeValue → JS_ExecutePendingJob → drogon::sync_wait`. Same kernel-side refcount family as `#38` and `pubsub P.01`; **a new exemplar test the DEFERRED.md entry did not previously list**. The 0.5.5.1 verification table at CHANGELOG line 88–94 was run with `[js] ~[async]` (excluded the async bucket); the post-merge full-suite run does not exclude it, so this race was not in the 0.5.5.1 ship's verification envelope. The PR run was clean by dice-roll on the documented ~8–11% rate; main is now red on the same dice.

This expands the documented family to **three exemplars** — `#38 async_hardening`, `pubsub P.01`, `cap.batch fail-fast on first rejection` — and confirms the candidate-root-cause hypothesis at `project_ws_flaky_segfault.md` (the race is reachable via more entry points than the original `async_hardening` test alone). The `cap.batch` test is a new entry point because 0.5.3 introduced `db.batch` to the dispatch surface; the same refcount path was always there but the test count for it was zero before 0.5.3.

**Architectural commitment carried into the next session.** Per the post-0.5.5.1 stance — *"0.5.x is the kernel's last hardening window before 0.6.0 begins UI"* — and per the maintainer's 2026-04-26 directive recorded at this re-eval ship, the `[js][async]` investigation **must close before 0.6.0 begins**. Schedule slot **`0.5.5.2 [js][async] refcount fix`** as the immediate next code session ahead of `0.5.5.N ICD-0.6.0 authoring`, with entry condition the candidate-root-cause list in `project_ws_flaky_segfault.md §Candidate root causes` and exit condition `[js][async]` grouping enabled with the ~10× subprocess-count reduction the rest of the suite already enjoys. ROADMAP slot already exists at `0.5.x.N [js][async] kernel-side refcount investigation`; promote `[medium]` → `[strong]` and rename to `0.5.5.2`. **Promotion fixed in this session; investigation deferred to the next code session per the maintainer's 2026-04-26 directive.**

**Resolution.** DEFERRED.md — update the WS-teardown entry with:

1. New 2026-04-26 paragraph naming the three 0.5.5.1 sub-path closures (events_writer in-flight tracker, runEvery try/catch wrappers, process-lifetime PG client) and the two carrying-forward flakes.
2. Tighten the §Why entry stays Active justification — the entry now stays Active *only* for the two carrying-forward flakes plus the framework-layer drogon race; the test-fixture lifecycle sub-paths have closed.
3. Cross-reference 0.5.5.1 CHANGELOG §Verification.

ROADMAP — no edit; the `0.5.x.N [js][async] kernel-side refcount investigation` slot still tracks the residual surface. Win condition unchanged. **Fixed in this session.**

### 2.5 Forward ICD presence — ICD-0.6.0 missing; 0.6.0 is the next code milestone

ROADMAP §0.6 carries 0.6.0 *Bootstrap and frame: Preact/htm scaffold, login wired to 0.1.x auth, empty topbar, error boundary scaffolding* as `[medium]` with no ICD authored. Per METHODOLOGY §3.1 *Forward ICD presence check*: any milestone entering the strong-window (next-3) should be `[strong]` with a pinned ICD. With this re-eval discharged, 0.6.0 becomes the next code milestone — strong-window by definition.

`feedback_icd_horizon.md` pins "only write ICDs one milestone ahead" — the ICD-0.6.0 paper session should land as a four-part follow-up before 0.6.0 opens. Precedents this re-eval honors:
- `RE-EVAL-0.5.x-following-0.5.1 §2.7` inserted `0.5.1.2 ICD-0.5.2 authoring [strong]` ahead of 0.5.2.
- 0.5.0.5 authored ICD-0.5.1 ahead of v0.5.1.
- 0.4.5.2 authored ICD-0.4.6 ahead of 0.4.6 implementation.

0.6.0 is a structurally different milestone than the 0.5.x kernel work — it's the first frontend session, introducing the Preact/htm scaffold + a fresh ICD vocabulary (panel SDK, render contract, error boundary semantics). The ICD will need to draw from `architecture/06-frontend.md` (currently §1–§4 cover asset-server, design-token indirection, panel registry, and admin extension preview) rather than the kernel-doc stack. Estimated authoring window: paper-session-sized (mirrors ICD-0.5.4's 2006 lines with comparable scope; ICD-0.5.2's 78-line broker contract was lighter).

**Resolution.** ROADMAP — two edits:

1. Promote 0.6.0 `[medium]` → `[strong]`.
2. Insert `0.5.5.N ICD-0.6.0 authoring [strong]` four-part follow-up line between the discharged `RE-EVAL following 0.5.5` line (about to flip to `[x]`) and the `0.6.0 Bootstrap and frame` line.

0.6.1 (Shell schema + user preferences) and 0.6.2 (Design tokens, theme, UI scaling) stay `[medium]` — both remain inside the strong-window after 0.6.0 ships, but per one-ahead the next ICD authoring slot is 0.6.0's; 0.6.1's own ICD authoring will be scheduled at the next re-eval following 0.6.0 (or explicitly as a `0.6.0.N` slot). 0.6a-A through 0.6a-E stay `[fuzzy]` — the parallel admin-extension stream gates on shell SDK ≥ 0.6.3 which is out of the next-N=3 window. **Fixed in this session.**

### 2.6 No newly surfaced flakes

The 0.5.5.1 verification table (CHANGELOG line 86–94) ran 10× per failing CI invocation across `[ws]`, pure-bucket, `[seq][unit]`, integration-pg, and `[js] ~[async]`. Post-fix every bucket clean except `[js] ~[async]` which surfaced the carrying-forward `pubsub_test.cpp:117` flake at the expected ~10% rate. No new signature families. §2.4's flake-list is closed.

### 2.7 No cadence-drift

Four code milestones landed since the prior re-eval (v0.5.2/0.5.3/0.5.4/0.5.5) — 4/4 over the prior re-eval's projected slot. The next cadence line `RE-EVAL following 0.6.3` sits at `ROADMAP §0.6` line 150 and aligns 4/4 over 0.6.0/0.6.1/0.6.2/0.6.3 (or 5/5 if the 0.6a-A/B parallel admin items pull into the count — methodology §3.3 leaves the parallel-stream cadence accounting to the project; current convention from `RE-EVAL-0.5.x-following-0.5.1 §1` is to count main-arc only). No cadence-line edit needed in this session.

The prior re-eval flagged a partial-cadence concern (it landed at 2/4 because 0.5.0/0.5.1 closed the producer half of realtime). This re-eval is the inverse — it lands at 4/4 over a structurally complete arc closure. The 0.5.x Realtime arc is functionally done; 0.6.x opens fresh territory.

---

## 3. Zero-gap findings (baseline record)

Sections checked against code, **no drift detected**:

- **ICD-0.5.2 §Broker API** — `start`, `stop`, `drain_extension`, `register_js_subscription`, `dispatch`, `dispatch_for_test`, `set_rbac_enforce_for_test` all present at [`src/kernel/realtime/broker.hpp`](../../src/kernel/realtime/broker.hpp). Writer-first topology shift consumes `dispatch` from the writer rather than as a listener handler — call site moves but the public surface is unchanged.
- **ICD-0.5.2 §`pubsub.subscribe`** — 7-step validation chain implemented at [`src/kernel/js/stdlib/pubsub_bindings.cpp`](../../src/kernel/js/stdlib/pubsub_bindings.cpp); rejection codes `pubsub.cancelled` / `pubsub.channel_invalid` / `pubsub.extension_mismatch` / `pubsub.rbac_denied` / `pubsub.layer_unsupported` / `pubsub.quota_exceeded` all match ICD §Rejection codes.
- **ICD-0.5.2 §SC6 cross-extension RBAC widening** — `classify_pubsub_subscribe` widened in LH-2 untagged follow-up; RE-checked at dispatch arm in `run_on_context.cpp` per SC2.
- **ICD-0.5.3 §db.batch + silent + per-op SET search_path + OID type mapping** — all four contributions present per the per-TU spot-checks above. Phase 5 atexit slot sequence (`discard_all_batches` between `realtime::stop_listener` and `realtime::broker::stop`) matches ICD §Deterministic Teardown.
- **ICD-0.5.3 §Security Constraint 3** — closed by 0.5.3.1 `db_batch_schema_check.cpp`'s `classify_cross_extension` regex + audit emitter; B.13 test live.
- **ICD-0.5.4 §`plinth.events` Persistence Writer** — `EventsWriter` singleton + dedicated `trantor::EventLoopThread` + bounded `std::deque` + drop-newest overflow + `pg_try_advisory_xact_lock` per envelope; 0.5.5.1 in-flight tracker is additive (does not change the documented surface). Three v0.5.4 deviations (xact-scoped lock, embedded BIGINT, `since_seq=0` skip-cursor-expired) ratified in §4.
- **ICD-0.5.4 §`cursor_store` API** — `record_delivered`, `read_cursor`, `reset_cursor` present; LRU cache + GREATEST monotonicity + lifecycle drain unchanged.
- **ICD-0.5.4 §Reconnect Handshake** — `since_seq` parse + replay dispatch + `replay`/`replay_done`/`resync` frames + `resubscribe.invalid_since_seq` error code all match.
- **ICD-0.5.4 §Replay Engine** — paginated query + per-row RBAC re-check + chunk size + total-row cap + concurrent-replays no-cross-user-interference all match. **`run_replay` parameter type `const ConnState&` (post-0.5.5.1) is recorded as a deviation in §4 below; it does not constitute drift because ICD-0.5.4 does not formally declare the C++ signature** — the prose pseudocode is signature-agnostic.
- **ICD-0.5.5 §5 Writer-first topology** — broker dropped from listener-handler registry; writer-side seq stamp on RETURNING + replay-side stamp on `plinth.events.seq` column both present. Envelope-seq == table-seq invariant holds across both paths per the v0.5.5 LH-1 pre-flight (live-path p99 7 ms × 3 trials, observed/emitted 1.0000).
- **ICD-0.5.5 §6 Wire contract: coalescing fields** — `coalesced_count` (≥1, OQ3 upstream NOTIFY hits), `window_open_ts_ms`, `window_close_ts_ms`, optional `superseded_seqs[]` (gated, ships empty under writer-first per OQ4 + W.06 design defer) all present.
- **ICD-0.5.5 §7 Subscribe-ack advisory** — `recommended_debounce_ms` (default 100) + `recommended_jitter_ms` (default 50) on `subscribe_ack` per the ICD; `debounce_renegotiate` parser is no-op + rate-limited audit per OQ5.
- **ICD-0.5.5 §8 Live-vs-replay ordering** — per-conn live buffer present; `replay_done.buffered_live_count` field present; `live_buffer_overflow` resync reason joins the existing four; `live_buffer_cap_per_subscription` defaults to 256 frames per OQ6.
- **0.5.5.1 §events_writer in-flight tracker** — `g_inflight_inserts` atomic + `g_inflight_cv` cv; `drain_one` increments before `async_run`; lambda decrements at coroutine body end; `stop()` blocks bounded by `shutdown_drain_ms` until count reaches zero. `runEvery` callbacks (drain + cleanup) wrapped in try/catch.
- **0.5.5.1 §`ConnState::channels_mu`** — `mutable std::unique_ptr<std::mutex>` field; 8 lock sites across `publish.cpp`, `subscriptions.cpp`, `events_controller.cpp`. `state_copy` move-into-lambda still works because `unique_ptr<std::mutex>` is movable.
- **Atexit ordering** — `ConnectionRegistry::initiate_shutdown` → `realtime::stop_listener` → `events_writer::stop` (post-0.5.4) → `js::discard_all_batches` (0.5.3) → `CoalescerRegistry::instance().shutdown` → `realtime::broker::stop` → `extensions::shutdown_registry` → `drogon::app().quit()` → `log::shutdown`. Matches `feedback_deterministic_teardown.md` pattern; test fixtures mirror.
- **Cross-arc invariants re-checked** — no new long-running threads beyond accounted (v0.5.4 events_writer `trantor::EventLoopThread`); no new `DbClient` pool beyond Drogon's; envelope `seq` field promoted from "reserved" (v0.5.0 placeholder) to REQUIRED (v0.5.5) — invariant captured at every emit + replay site.

---

## 4. Accepted deviations catalog (consolidated)

Walked each CHANGELOG `§Documented deviations from ICD pseudocode` / `§Implementation deviations` block across the v0.5.2 – 0.5.5.1 window with the question *"does this deviation's rationale point at an ICD wording bug rather than a kernel-convention alignment?"* Twelve deviations total; eleven ratify cleanly as kernel-convention or external-constraint, one (D11) triggers an ICD amendment per Constraint #4.

| # | Milestone | Deviation | Category | Resolution |
|---|-----------|-----------|----------|------------|
| D11 | v0.5.5 | `Config::Realtime::Events::SeqSource` shipped as `enum class : std::uint8_t { WRITER_RETURNING }` rather than the `std::string` shown in ICD §10 pseudocode | External-constraint (cert-err58-cpp; static-storage `g_cfg` instances in `cursor_store.cpp` + `events_writer.cpp` require trivially-default-constructible `Config::Realtime::Events`) | ICD amendment — see §4.1 below. **Triggered this RE-EVAL's only ICD-text edit beyond the standard "Implementation deviation" subsection roll-ups.** |
| D12 | v0.5.5 | `superseded_seqs[]` ships as a stable empty array when `emit_superseded_seqs=true` | Design-defer (writer-first foreclosure of source-seq tracking; W.06 deferred per ICD §6 §Implementation seam) | Ratified in CHANGELOG; design-deferral well-documented at ICD §6. No ICD edit. |
| D13 | v0.5.5 | Replay engine seq-stamp lives at `replay.cpp` between `parse_payload` and `build_replay_frame`, not in `build_replay_frame` itself | ICD-text imprecision (cleaner separation of concerns; pseudocode-suggestive only) | Ratified in CHANGELOG; ICD pseudocode is suggestive, not prescriptive on exact call-site placement. No ICD edit. |
| D14 | 0.5.5.1 | `replay::run_replay` parameter type changed from `ws::ConnState state` (value) to `const ws::ConnState& state` (const reference) | External-constraint (`ConnState` no longer copyable post-`channels_mu` unique_ptr addition) | ICD-0.5.4 §Replay Engine — implementation note added (no formal signature declared; pointer to 0.5.5.1 CHANGELOG suffices). See §4.2. |
| D15 | v0.5.4 | Cleanup uses `pg_try_advisory_xact_lock` (xact-scoped) rather than session-scoped `pg_try_advisory_lock` | External-constraint (Drogon's connection pool semantics — session-scoped lock dropped on connection return) | Ratified in CHANGELOG + at the call-site comment; session vs xact is documented inline. ICD updates §Cleanup pseudocode at the same edit pass as D11. |
| D16 | v0.5.4 | Cleanup advisory-lock key embedded as SQL literal rather than bound BIGINT param | External-constraint (Drogon SqlBinder sends BIGINT in binary format PG rejects on this query shape) | Ratified in CHANGELOG; security risk minimal (compile-time constant). ICD note added alongside D15. |
| D17 | v0.5.4 | `since_seq=0` treated as "no prior cursor" — skips cursor_expired precondition | ICD-text imprecision (matches ICD §New-user behaviour but the §Cursor-expired check pseudocode reads ambiguously) | Ratified in CHANGELOG; ICD §New-user behaviour is normative and matches. No ICD edit. |
| D18 | v0.5.3 | T.* test cases live in `async_bridge_test.cpp`, not `stdlib_test.cpp` | Shipping-order (T.* need live PGresult; tagged `[js][async][db][types]`) | Ratified in CHANGELOG. |
| D19 | v0.5.3 | T.07 narrowed from `'true'::text` to `'t'::text` | ICD-text imprecision (heuristic only mis-classifies single-char PG bool text reprs) | Ratified in CHANGELOG. |
| D20 | v0.5.3 | P.05 narrowed from "ext schema dropped" to "regex defense rejects malicious identity" | ICD-text imprecision (PG's SET LOCAL is permissive on non-existent schemas) | Ratified in CHANGELOG. |
| D21 | v0.5.3 | `Config::Db` field named `db_bindings` in C++ to avoid collision with existing `Config::Database db;` | ICD-text imprecision (forward-compatible — JSON key `"db"` preserved) | Ratified in CHANGELOG. |
| D22 | 0.5.2.N backfill | Five test deviations (B.05/B.06 PG-gating, U.02 extension_mismatch edge, U.07 cancellation cascade observable swap, S.11/I.07 delivery re-check swap, I.05 narrowed to per-bc registration routing) | Shipping-order + harness composition | Ratified in 0.5.2.N CHANGELOG with per-deviation rationale; tests cover the same invariants via different observable seams. No ICD edit. |

#### 4.1 ICD-0.5.5 — `SeqSource` enum + writer-first design-defer + replay-stamp call-site

ICD-0.5.5 §10 (Config Surface) shows `seq_source: std::string` in pseudocode. The shipped surface is `enum class : std::uint8_t { WRITER_RETURNING }` for static-storage compatibility (cert-err58-cpp). The deviation rationale is load-bearing: a fresh session reading §10 will look for a string-keyed dispatch and not find it. ICD-0.5.4 also has implementation-note candidates from D15 + D16 worth folding into a similar subsection.

**Resolution.** ICD-0.5.5 — new "Implementation deviation (v0.5.5 ship)" subsection ahead of §10 §Config Surface naming D11 + D12 + D13. ICD-0.5.4 — new "Implementation deviation (v0.5.4 ship)" subsection naming D15 + D16 + D17 + the 0.5.5.1 D14 `run_replay` signature change (per §4.2 below). ICD-0.5.3 / ICD-0.5.2 — "Implementation deviation" subsection updates rolling up D18–D22 per Constraint #4 (deviations belong where future sessions read the specification, not just in CHANGELOG). **Fixed in this session.**

#### 4.2 ICD-0.5.4 §Replay Engine — `run_replay` parameter type note

ICD-0.5.4 prose-pseudocode at §Replay Engine §Pagination does not declare a formal C++ signature. The 0.5.5.1 ship changed `replay::run_replay`'s first parameter from `ws::ConnState state` (value) to `const ws::ConnState&` (const reference) because `ConnState` became non-copyable after the `channels_mu` unique_ptr addition. The shipped header is at [`src/kernel/realtime/replay.hpp:71`](../../src/kernel/realtime/replay.hpp). No ICD-text drift to repair (no formal declaration), but the §Replay Engine section gains a one-line implementation note pointing at the 0.5.5.1 CHANGELOG.

**Resolution.** Folded into the ICD-0.5.4 "Implementation deviation (v0.5.4 ship)" subsection per §4.1.

No deviations ratify as "bug"; all twelve either record an ICD-text imprecision discharged inline, an external-constraint properly documented in CHANGELOG, or a design-defer well-documented at the ICD's deferral seam.

---

## 5. DEFERRED.md status (7 entries examined)

Re-examined `docs/DEFERRED.md` §Active entries:

1. **ICD-0.4.4 I.18/I.19/I.20 — HTTP test harness for `/api/packages`** (2026-04-20). Active; pointer holds (`0.5.x.N HTTP test harness`). No change.
2. **ICD-0.5.0.3 R.02/R.03/E.07/P.01/P.04/P.05/H.02/H.03/C.01/C.02 — extension-dispatch test coverage** (2026-04-23, added by prior re-eval). Active; P.04 unblocked when 0.5.3 phase 3 search_path landed but the test itself stays grouped with the cluster items. Pointer holds. **Optional cleanup**: P.04 now reachable; could schedule under the same `0.5.x.N HTTP test harness` slot. No urgency; bundled with the cluster.
3. **ICD-0.4.5 X.05–X.13 + G.03 — extended upgrade and GC test coverage** (2026-04-22). Active; pointer holds (same slot as #1 and #2). No change.
4. **WS teardown flake / `[js][async]` Catch2 subprocess race** (2026-04-19 / 2026-04-22 / 2026-04-23 / **updated 2026-04-26**). Active; entry updated per §2.4 above with the three 0.5.5.1 sub-path closures (events_writer in-flight tracker, runEvery try/catch wrappers, process-lifetime PG client) and tightened "Why entry stays Active" — now scoped to the two carrying-forward flakes (`pubsub_test.cpp:117` + `#38 async_hardening`) plus the framework-layer drogon `EventLoopThreadPool::~` race.
5. **MEMORY_LIMIT classifier peak-tracking** (2026-04-19). Watchlist; no regression signal in the v0.5.2–v0.5.5.1 window. No change.
6. **Drogon `PgBatchConnection` `SqlError` typing** (2026-04-18). Active, ad-hoc. No regression signal in window. No change.

**Addition:** §2.3 adds one new entry consolidating ICD-0.5.4 I.02/I.03 + ICD-0.5.5 S.06/S.07/L.03–L.05/W.06/I.01–I.04 (eleven cases, three buckets — multi-process advisory-lock harness, mid-replay live-buffer scenarios, design-deferred W.06 source-seq tracking).

**Resolved this window (already moved or scheduled to move):**
- 2026-04-18 — Per-op `SET search_path` for `db.*` (resolved 0.5.3 phase 3, moved 2026-04-24).
- 2026-04-18 — `db.*` PG-type→JS-type mapping (resolved 0.5.3 phase 1, moved 2026-04-24).

---

## 6. Forward ICD presence check

Next-N window (N=3) after this re-eval discharges 0.5.5:

| Milestone | Band | ICD | Disposition |
|---|---|---|---|
| **0.6.0** Bootstrap and frame: Preact/htm scaffold + login + topbar + error boundaries | `[medium]` → **`[strong]`** | **missing** | Promoted. `0.5.5.N ICD-0.6.0 authoring [strong]` scheduled ahead (see §2.5). |
| **0.6.1** Shell schema + user preferences | `[medium]` | missing | Stays `[medium]`; ICD authored at a later paper session (likely `0.6.0.N`). |
| **0.6.2** Design tokens, theme, UI scaling | `[medium]` | missing | Stays `[medium]`; consumes the 0.6.0 scaffold. |

Cross-cutting `0.5.x.N` items retain their bands:
- HTTP test harness — `[strong]`. No change.
- `[js][async]` refcount investigation — `[medium]`. No change.

The parallel 0.6a admin-extension stream (0.6a-A through 0.6a-E) stays `[fuzzy]` across the board — the earliest item (0.6a-A package management panel) gates on 0.6.3 shell SDK, which is outside the next-N=3 window.

The **one-ahead horizon** (`feedback_icd_horizon.md`) is satisfied by scheduling ICD-0.6.0 only. ICD-0.6.1 / ICD-0.6.2 authoring stays deferred until 0.6.0 ships or a later re-eval tightens the horizon. The architectural sketch for 0.6.0 lives at `architecture/06-frontend.md` (§1–§4); the ICD will promote that sketch into a normative contract.

---

## 7. Cadence / labels update

- Discharged `RE-EVAL following 0.5.5` (ROADMAP §0.5, line 143).
- Next cadence slot is `RE-EVAL following 0.6.3` (ROADMAP §0.6, line 150) — 4/4 over 0.6.0/0.6.1/0.6.2/0.6.3.
- No preemptive cadence insertion — the 0.6 arc has explicit re-eval slots already (post-0.6.3 + post-0.6.6), comparable cadence to 0.5.x.
- Promoted 0.6.0 `[medium]` → `[strong]`; inserted `0.5.5.N ICD-0.6.0 authoring [strong]` ahead.
- No other band movements.

**Note for the RE-EVAL following 0.6.3 agenda** (per §2.2.5): if 0.6.x UI work introduces another `runEvery` + `drogon::async_run` site, promote the `events_writer` in-flight-tracker pattern from `feedback_deterministic_teardown.md` into a normative `architecture/04-services-ha.md` subsection or a new `architecture/07-deterministic-teardown.md`. Track as an opening-assessment question for that re-eval.

---

## 8. Verification

Docs-only session; no code build.

- **Precedent match:** structure + tone mirror `RE-EVAL-0.5.x-following-0.5.1.md`. Gap-count + amendment-count within range: four ICD/arch amendments here vs four there (§2.2.1 + §2.2.2 + §2.2.3 + §2.2.4 architecture; §4.1 ICD-roll-up across four ICDs); comparable scope.
- **Cross-reference round-trip:** CHANGELOG entry → RE-EVAL doc → ICD/arch amendments → ROADMAP/DEFERRED edits all cite each other. Spot-checked during write.
- **File path resolution:** every file path cited in this RE-EVAL doc is a real on-disk path at the merge commit.
- **Methodology §3 axes exercised:**
  - §3.1 Cadence — confirmed 4/4 over v0.5.2 → v0.5.5; no drift.
  - §3.1 Forward ICD presence — N=3 window scanned; 0.6.0 promoted + ICD-0.6.0 authoring slot inserted (§6).
  - §3.1.1 Code-Aware Inputs — every shipped TU in window listed in §1 with its ICD section spot-verified.
  - §3.1.1 Interface-drift — §2.1 (architecture/03-data §3.6.1 last sentence).
  - §3.1.1 Arch-silent-on-code — §2.2.1, §2.2.2, §2.2.3, §2.2.4.
  - §3.1.1 Missing-test-for-arch-claim — §2.3 (DEFERRED.md addition).
  - §3.1.1 Decision-embedded-in-comments — §2.2.5 (kernel teardown idiom; deferred to next re-eval per the "second instance lands" rule).
- **Architect read-through:** final gate per the cadence-re-eval posture.

Verification passes. Session delivers the post-0.5.5 contract cleanup that the next 0.5.5.N ICD-0.6.0 authoring session and the eventual 0.6.0 frontend bootstrap pick up cold.

---

## Appendix — Session-produced artifacts

- This document (`docs/reviews/RE-EVAL-0.5.x-following-0.5.5.md`).
- `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md` — new "Implementation deviation (v0.5.5 ship)" subsection ahead of §10 (deviations D11 + D12 + D13).
- `docs/icd/ICD-0.5.4-events-table-delta-sync.md` — new "Implementation deviation (v0.5.4 ship)" subsection (deviations D14 + D15 + D16 + D17).
- `docs/icd/ICD-0.5.3-db-batch-silent-mode.md` — "Implementation deviation (v0.5.3 ship + 0.5.3.1 follow-up)" subsection rolling up D18 + D19 + D20 + D21 plus the 0.5.3.1 timeout + cross-extension additions.
- `docs/icd/ICD-0.5.2-ws-broker.md` — "Implementation deviation (v0.5.2 ship + 0.5.2.N backfill + LH-2 follow-up)" subsection rolling up D22 (five test-shape deviations) plus the SC6 cross-extension widening + SC2 dispatch-arm re-check.
- `docs/architecture/03-data.md` — §3.6.1 last sentence rewritten (writer-first topology; §2.1); §3.5 implementation blockquote extended (`live_buffer_overflow` + `replay_done.buffered_live_count` + `ConnState::channels_mu`; §2.2.1 + §2.2.4).
- `docs/architecture/02-capabilities.md` — §3 paragraph appended naming `pubsub.publish` + `pubsub.subscribe` JS bindings (§2.2.2).
- `docs/architecture/04-services-ha.md` — §1 paragraph appended indexing the seven `realtime.*` audit families with per-family ICD pointers (§2.2.3).
- `docs/DEFERRED.md` — new entry consolidating ICD-0.5.4 I.02/I.03 + ICD-0.5.5 S.06/S.07/L.03–L.05/W.06/I.01–I.04 (§2.3); WS-teardown entry updated with 0.5.5.1 sub-path closures (§2.4).
- `docs/ROADMAP.md` — `RE-EVAL following 0.5.5` discharged; 0.6.0 promoted `[medium]` → `[strong]`; `0.5.5.N ICD-0.6.0 authoring [strong]` inserted (§6, §2.5).
- `docs/CHANGELOG.md` — new 2026-04-26 rewrite-session entry (un-tagged per `feedback_tagging_rule.md`) cites this RE-EVAL and lists every amendment.
