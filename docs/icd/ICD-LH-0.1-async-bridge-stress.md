# ICD-LH-0.1 — Async-Bridge Stress Tier

**Status**: Active (diagnostic — infrastructure, not a kernel feature)
**Authored**: 2026-04-21
**Roadmap**: `docs/ROADMAP.md` §Load Harness, LH-0.1 `[strong]`

## 1. Purpose

LH-0.1 extends the LH-0 scaffold with a single new kernel capability
and a single new harness tier that together drive the JS async-bridge
coroutine path (`run_on_context` → `dispatch_async_op_detached` →
`signal_completion` → `JS_ExecutePendingJob`) under sustained load.

LH-0 shipped 2026-04-21 and executed 2,000,000+ `lh0:1:chain` calls
with zero reproductions of `free_zero_refcount`,
`list_empty(&rt->gc_obj_list)`, or `bad_weak_ptr`. `lh0:1:chain` is
pure-C++ Tier 1 recursion; it never enters the coroutine dispatch
loop. LH-0.1 is the targeted diagnostic for the async path the
`async_hardening: parallel queries honour max_concurrent cap` ctest
case exercises — that test is the only current site that reliably
fires `free_zero_refcount` on CI, but it fires under the Catch2
subprocess lifecycle, not the production kernel lifecycle. LH-0.1
drives the same path through the production kernel via real HTTP/WS.

Two outcomes from a 3-trial harness run are both useful:

- **Reproduction** on current HEAD confirms a real kernel race and
  unblocks a dedicated fix PR (the "no per-signature bandaids"
  direction from 2026-04-21 then applies to the fix, not to LH-0.1).
- **Zero reproductions** is evidence the production lifecycle already
  tolerates the path the Catch2 subprocess model trips on, and
  redirects investigation to the test-strategy redesign the maintainer flagged
  on 2026-04-20.

## 2. Non-goals for LH-0.1

- Fixing the race. LH-0.1 is the diagnostic; the fix ships in its own
  PR once a reliable reproduction is in hand.
- Widening the synchronous Tier 1 handler signature to
  `drogon::Task<>`. All existing Tier 1 handlers stay sync; LH-0.1
  routes around the Tier 1 map rather than widening it.
- Extension-capability dispatch from C++
  (`dispatch_tier2` / `provider_type=extension`). Still deferred.
- Widening `ConnState` rules beyond the existing `is_admin` boolean.
  LH-0.1 uses the same admin-gate LH-0 uses.
- Robustness hardening for mid-call connection drops — handled
  minimally (connection check before send) but not comprehensively.
- Result-size or memory-limit tuning. LH-0.1 runs with the default
  profile from `default_runtime_limits()`.

## 3. Kernel contract — `lh0:1:js_stress` signature

A new kernel-scope signature reachable over the same WS `call` frame
shape LH-0 introduced (ICD-LH-0 §3). The signature is handled *outside*
the Tier 1 map via a dispatch fork in `plinth::ws::on_call`; see §4.

Signature: `lh0:1:js_stress`
RBAC: synthetic — admin-only, enforced inline at dispatch time.
Args: `[script: string]`. Other shapes surface as `invalid_call`.

Return shape on success (top-level promise resolution):

```json
{
  "type": "call_result",
  "id": "<echoed>",
  "value": <json value returned by run_on_context>,
  "resolved_tier": "tier1",
  "provider_type": "kernel"
}
```

Return shape on failure:

```json
{
  "type": "call_error",
  "id": "<echoed>",
  "code": "<mapped code>",
  "message": "<EvalError.message, truncated if very long>"
}
```

Error-code mapping — LH-0.1 uses a catch-all to keep the diagnostic
minimal:

| Condition                                       | `code`              |
|-------------------------------------------------|---------------------|
| Non-admin caller                                | `permission_denied` |
| Args not `[string]`                             | `invalid_call`      |
| `run_on_context` returns any `EvalError`        | `js_eval_error`     |
| `RuntimePool::acquire` returns `nullptr`        | `resource_exhausted`|
| Pool is shut down when a dispatch races atexit  | `internal_error`    |

A richer taxonomy (`wall_clock_exceeded`, `memory_limit`,
`promise_rejected_unhandled`) can slot in later if any caller branches
on it. Today neither the harness nor the test cases do.

## 4. Dispatch fork

Tier 1 handlers are sync — signature
`(const Json::Value& args, const UserContext& ctx, int call_depth) -> HandlerOutcome`.
Widening them to `drogon::Task<>` ripples through every existing
handler and into the resolver's Step 3 RBAC path. Out of scope for a
diagnostic PR.

Instead, `plinth::ws::on_call`
([src/kernel/ws/call_dispatch.cpp:64](../../src/kernel/ws/call_dispatch.cpp:64))
gains a single early-fork before it invokes
`plinth::capabilities::call_capability`:

```cpp
if (try_dispatch_js_stress(conn, call_id, signature, args,
                           state->is_admin)) {
    return;
}
// ... fall through to existing sync call_capability
```

`try_dispatch_js_stress` lives in a new TU `src/kernel/ws/js_stress.{hpp,cpp}`.
It returns `true` when the signature matched (whether dispatch
succeeded or failed) so the sync resolver never sees the frame; it
returns `false` for every other signature, so LH-0's verified
`lh0:1:chain` path stays untouched.

The helper pattern used by the coroutine body —
`drogon::async_run([...]() -> drogon::Task<> { ... })` — matches the
existing detached-dispatch pattern in `run_on_context.cpp`. No new
threading surface.

This is an explicit, diagnostic-scope deviation from the "every call
goes through the resolver" shape. It is NOT the blueprint for future
extension dispatch (0.3.4+); that work carries its own design and
will wire into the Tier 1/2 pipeline through the proper widening.

## 5. Process-lifetime RuntimePool

LH-0.1 owns a single file-scope `RuntimePool` in `js_stress.cpp`,
constructed with:

- `ext = nullptr` — host-side (not an extension pool)
- `runtime_limits = default_runtime_limits()` — production profile,
  so the stress is realistic against the shape a real extension gets
- `cfg = <main kernel Config>`, threaded via
  `init_js_stress_pool(cfg)` from `main.cpp`
- `pool_size = -1` — default formula (`min(4, hardware_concurrency() / 2)`)
- `user = nullptr` — `UserContext::anonymous()` is fine; RBAC is
  enforced at the dispatch fork before the BC is even acquired

Lifetime:

- `init_js_stress_pool(cfg)` called from `main.cpp` after
  `init_resolver(cfg.db)` and after `drogon::app().createDbClient(...)`.
- `shutdown_js_stress_pool()` called from the existing
  `std::atexit(...)` lambda in `main.cpp`, **before**
  `drogon::app().quit()`. Pool destruction pumps QuickJS pending jobs
  via the 0.4.0.1 `drain_pending_jobs` helper, which must run while
  Drogon's event loop is still alive (deterministic-teardown
  convention, `feedback_deterministic_teardown.md`).
- `tests/kernel/ws/ws_test_fixture.cpp` performs the same
  init/shutdown pair in its own bootstrap / atexit chain.

The pool is held in a file-scope `std::unique_ptr` under a mutex.
Not a leaked singleton: LH-0.1 controls both init and shutdown, so
the Meyers-singleton teardown-ordering issue closed in 0.4.5
commit `af8bbdd` does not apply.

## 6. WS frame shape

No change from ICD-LH-0 §3. The same `call` / `call_result` /
`call_error` frame types carry LH-0.1's new signature; the `id`
correlation key carries through; the RBAC model is identical (admin
synthesized from `ConnState::is_admin`). The harness reuses
`wsclient.Call` unchanged.

## 7. Connection-drop semantics

The dispatched coroutine captures `std::weak_ptr<WebSocketConnection>`
(Drogon's `WebSocketConnectionPtr` is a shared_ptr). On resolution:

```cpp
auto locked = weak_conn.lock();
if (locked && locked->connected()) {
    locked->sendJson(frame);
}
```

No cancellation is propagated back to the in-flight BridgeContext
when the conn drops. The BC completes naturally, the result is
discarded, and the BC is released/destroyed per the normal pattern.
This is acceptable for the diagnostic use case — wall-clock and
result-size limits already bound the work a dropped-conn BC can do.

## 8. Harness — `--tier=async`

New tier profile in `load-harness/internal/tiers/tiers.go`:

| Name    | Concurrency | Depth | Duration |
|---------|-------------|-------|----------|
| async   | 4           | n/a   | 120s     |

Depth is unused — `lh0:1:js_stress` takes a script string, not a
depth int. The script is a fixed constant in `cmd/lh0/main.go`:

```js
(async()=>{
  const ps=[];
  for(let i=0;i<4;i++)ps.push(db.query(`SELECT pg_sleep(0.01), ${i} AS x`));
  return (await Promise.all(ps)).length;
})()
```

Per-call effective fan-out is ~16 concurrent `db.query` operations
across the harness (4 workers × 4 queries per call). Each call
settles in ~50 ms server-side (4 × 10 ms pg_sleep, parallelised).
Target per-run: ~9,600 js_stress calls ≈ 38,400 `db.query` ≈ 38,400
`signal_completion` callbacks. Overridable via `--concurrency` /
`--duration`.

The harness dispatch stays tier-keyed: `"async"` workers send
`{signature:"lh0:1:js_stress", args:[asyncStressScript]}` instead of
the `lh0:1:chain` path the `easy` / `medium` workers use. Per-call
timeout stays at 10 s (well above the ~50 ms settle time).

## 9. Success criteria

### 9.1 Baseline (every run)

- Harness exits 0 under `--tier=async` default (zero worker-level
  errors other than `ws_closed` on the expected ctrl-C path).
- p99 latency finite (no runaway queues).
- No kernel crash signatures (`SIGSEGV`, `SIGABRT`) observed in the
  kernel log during the run.

### 9.2 Diagnostic mandate (driving the 2026-04-21 session)

Under `--tier=async` × 3 trials, paired with kernel-side log tailing:

- **Reproduction** of at least one of
  `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`, or
  `trantor::EventLoop ... bad_weak_ptr` on current `main` HEAD
  confirms a real kernel race under the production lifecycle and
  unblocks a targeted fix PR.
- **Zero reproductions** on 3 trials is also a meaningful data
  point: it indicates the production kernel tolerates the path the
  Catch2 subprocess model trips on, and refocuses the investigation
  on the test-strategy redesign the maintainer flagged on 2026-04-20.

## 10. Future work / deferred

- **LH-0.2 (parameterised script)** if we need multiple async shapes
  (cap.call recursion, nested Promise.all, long-tail awaits). Today
  a single fixed script is enough; deferring the flag until a caller
  actually needs it.
- **Fix PR for the reproduced signature** — scoped as a standalone
  3-part task once LH-0.1 produces a reliable repro. Out of scope
  for LH-0.1 itself.
- **Extension dispatch** (ICD-0.3.4-to-the-extent-it-touches-C++
  dispatch) so extension async handlers go through the full
  resolver pipeline, not the LH-0.1 fork. Unscheduled.

## 11. References

- `docs/icd/ICD-LH-0-load-harness-scaffold.md` — base WS `call`
  frame shape + harness scaffold.
- `docs/icd/ICD-0.3.3-async-bridge.md` — coroutine dispatch loop +
  `signal_completion` spec.
- `docs/icd/ICD-0.3.5-runtime-hardening.md` — adversarial cases
  N.37–N.47 + async-result-size spec.
- `tests/kernel/js/async_hardening_test.cpp:151` — `async_hardening:
  parallel queries honour max_concurrent cap`, the reference ctest
  repro that fires `free_zero_refcount` under the Catch2 subprocess
  lifecycle.
- `project_next_session_lh0.md` (session memory) — 2026-04-21
  Option A direction that motivated this milestone.
- `project_ws_flaky_segfault.md` (session memory) — full history of
  the flake family this diagnostic targets.
- `feedback_deterministic_teardown.md` (session memory) — atexit
  convention the pool's shutdown path follows.
