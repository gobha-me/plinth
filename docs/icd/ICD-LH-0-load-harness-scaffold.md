# ICD-LH-0 — Load Harness Scaffold

**Status**: Active (sketch-level — infrastructure, not a kernel feature)
**Authored**: 2026-04-21
**Roadmap**: `docs/ROADMAP.md` §Load Harness, LH-0 `[strong]`

## 1. Purpose

LH-0 is the first milestone of the parallel Load Harness stream. It
stands up a standalone external binary that drives the production
`plinth` kernel under sustained HTTP + WebSocket load, using observability
tools the kernel already supports (spdlog, process-level `ps`/`top`,
SQL queries on `plinth.packages`).

Two concerns motivate it:

1. **Reproduce residual flakes** (`quickjs.c:6678: free_zero_refcount`,
   `JS_FreeRuntime: list_empty(&rt->gc_obj_list)`, `bad_weak_ptr` at
   teardown) against the **production** kernel, not the Catch2
   subprocess-per-test harness. The 0.4.4.1 deterministic-teardown
   bundle closed these under tests by exposing `cancel_all_*` from
   atexit; LH-0 empirically audits whether the kernel itself still
   hits the same family under load or whether the flakes were
   test-harness-only.
2. **Establish the scaffold** that LH-1 through LH-4 build on:
   auth, install, WS transport, and per-call latency accounting.

## 2. Non-goals for LH-0

- Hard / crushing tiers — LH-4.
- `plinth.metrics` integration — LH-4 (the harness writes to stdout
  today; CI regression gating is deferred until the metrics
  subsystem lands).
- LISTEN/NOTIFY stress — LH-1 (gated on 0.5.0).
- WS fan-out stress — LH-2 (gated on 0.5.2).
- Reconnect / delta-sync stress — LH-3 (gated on 0.5.4).
- Fixing the residual flakes. LH-0 is the diagnostic; fixes ship
  as their own PRs once LH-0 produces reliable reproductions.

## 3. Kernel contract — WS `call` message type

LH-0 adds one new message type to the `/ws/events` protocol defined
in ICD-0.1.6. The frame is authoritative; future harnesses and the
eventual frontend SDK will key off the same shape.

### 3.1 Inbound (client → server)

```json
{
  "type": "call",
  "id": "<client-correlation-id>",
  "signature": "<namespace>:<version>:<function>",
  "args": [<positional-arg-1>, <positional-arg-2>, ...]
}
```

- `id` is opaque to the kernel — echoed on the response frame so the
  client can demux concurrent calls on one WS connection.
- `signature` is parsed by the existing `capabilities::parse_signature`.
- `args` may be any JSON value; `null`/absent is normalized to an empty
  array at handler entry.

### 3.2 Outbound success (server → client)

```json
{
  "type": "call_result",
  "id": "<echoed>",
  "value": <handler-return-value>,
  "resolved_tier": "tier1" | "tier2",
  "provider_type": "kernel" | "extension" | "sidecar"
}
```

### 3.3 Outbound error (server → client)

```json
{
  "type": "call_error",
  "id": "<echoed>",
  "code": "<CapabilityError snake_case>",
  "message": "<human-readable>"
}
```

`code` is whatever `capabilities::error_code(CapabilityError)` returns
for the resolver's failure, OR the sentinel `"invalid_call"` when the
inbound frame was syntactically malformed (missing/non-string
`signature`).

### 3.4 RBAC

The handler runs through the existing `call_capability` pipeline —
step-3 RBAC enforcement against `rbac_rule` on the Tier 1 / Tier 2
entry (ICD-0.2.4).

For LH-0, `ConnState` carries only the boolean `is_admin` flag
populated at WS auth time (ICD-0.1.6 §Auth). The `on_call` handler
synthesizes `UserContext.effective_rules` as `["kernel.admin"]` when
`is_admin=true`, empty otherwise. Non-admin WS clients therefore
cannot dispatch any RBAC-gated capability via `call`. Widening the
ConnState rule set (fetching the full effective-rules vector at WS
auth) is a future extension — tracked out-of-band, not LH-0 scope.

### 3.5 Pre-auth frames

A `call` frame arriving before `authenticated=true` is silently
dropped (no response). This matches the ICD-0.1.6 convention for
other authenticated frame types (`subscribe` / `unsubscribe` have the
same behavior).

## 4. Kernel contract — `lh0:1:chain` Tier 1 capability

LH-0 relies on a single recursive kernel capability that exercises
the sync dispatch path at depth N. Registered by
`register_lh0_harness_handlers_locked` inside `init_resolver` — always
on, no runtime toggle.

Signature: `lh0:1:chain`
RBAC: `kernel.admin`
Args: `[depth: int]` (missing / non-int → depth=1, terminal)

Return shape (depth=3):
```json
{
  "depth": 3,
  "sub": {
    "depth": 2,
    "sub": {"depth": 1, "terminal": true}
  }
}
```

At `depth > MAX_CALL_DEPTH` (8 today) the recursive invocation
surfaces `call_depth_exceeded` through the resolver's standard Step 2
check — the handler does not special-case depth itself.

## 5. Harness binary — `lh0`

External Go 1.22+ binary. Lives at `load-harness/cmd/lh0/`, builds via
`load-harness/Makefile` (no CMake integration). Dep: `github.com/gorilla/websocket`.

### 5.1 Tier profiles

| Name    | Concurrency | Depth | Duration |
|---------|-------------|-------|----------|
| easy    | 2           | 4     | 60s      |
| medium  | 8           | 8     | 5m       |

Each `--tier` field can be overridden individually via
`--concurrency` / `--depth` / `--duration` on the command line.

### 5.2 Flow

1. `POST /api/auth/login` → capture `plinth_session` cookie.
2. Optional `POST /api/packages` (multipart `package`) with
   `fixtures/driver.zip` — exercises the install lifecycle under
   LH-0 load. Poll is not strictly needed: the `InstallPackage` call
   itself runs to terminal state (ACTIVE or INSTALL_FAILED) before
   returning.
3. For each of N workers, log in separately — the kernel's
   `ConnectionRegistry::register_connection` displaces duplicate
   `(auth_type, id)` pairs (ICD-0.1.6 §Auth), so workers must hold
   distinct session IDs. N × `POST /api/auth/login` at startup.
4. Fan out N workers (one per `--concurrency`). Each:
   - Dials `/ws/events`.
   - Sends `{type:"auth", token:<worker's session-cookie-value>}`.
   - Loops `{type:"call", id:<counter>, signature:"lh0:1:chain",
     args:[depth]}` with 10 s per-call timeout.
   - Records latency on `call_result`, the kernel error `code` on
     `call_error`, `ws_timeout` for the 10 s per-call timeout, or
     `ws_closed` when the connection itself dies (connection death
     exits the worker to avoid tight-looping).
5. On `--duration` elapsed OR SIGINT/SIGTERM: cancel workers, join,
   print p50/p95/p99 + error breakdown + `plinth.packages` state
   histogram (fetched via `GET /api/packages`).
6. Unless `--keep-driver`, `DELETE /api/packages/{id}?confirm=true`.

## 6. Success criteria

### 6.1 Baseline (every run)

- Harness exits 0 under `--tier=easy`.
- `packages by state` histogram shows zero `INSTALL_FAILED` rows if
  `--driver-zip` was used.
- p99 latency finite (no runaway queues).

### 6.2 Diagnostic mandate (driving 2026-04-21 session)

Under `--tier=medium` × 3 trials, paired with kernel-side log tailing:

- **Reproduction** of at least one of
  `free_zero_refcount`, `list_empty(&rt->gc_obj_list)`, or
  `trantor::EventLoop ... bad_weak_ptr` on current
  `main` HEAD confirms a real kernel race (not test-harness-specific)
  and unblocks a targeted fix PR.
- **Zero reproductions** on current HEAD is *also* useful: it's evidence
  that the 0.4.4.1 deterministic-teardown bundle closes the paths
  under the production kernel's lifecycle, and the residual CI
  flakes are scoped to the Catch2 `catch_discover_tests` subprocess
  model — which would then justify the test-strategy redesign track
  the maintainer flagged on 2026-04-20.

## 7. Observability

All external for LH-0:

- **Harness stdout**: periodic progress ticks + final summary.
- **Kernel stderr / log**: `tail -f <log> | grep -E
  'free_zero_refcount|list_empty|bad_weak_ptr|SIGSEGV|SIGABRT'`.
- **Process**: `ps -o rss,%cpu,pid -C plinth` sampled every 5 s.
- **DB state**: `SELECT state, count(*) FROM plinth.packages GROUP BY
  state;` before and after.

No `plinth.metrics` — LH-4 wires harness → metrics after the metrics
subsystem lands (gated on 0.7.1).

## 8. Future work / deferred

- **LH-0.1** — JS async-bridge stress. Requires either (a) an
  HTTP/WS-reachable path that invokes a coroutine-based handler, or
  (b) extension-capability dispatch from C++ (not yet wired;
  `dispatch_tier2` returns `tier3_not_available` for
  `provider_type=extension` today). The `free_zero_refcount` flake
  signature fires in `async_hardening: parallel queries` which
  traverses `SqlBinderAwaiter` → `dispatch_async_op_detached` →
  `signal_completion`. LH-0.1 should drive that same path.
- **Widen ConnState rules** so non-admin WS callers can invoke
  capabilities they're entitled to via their extension RBAC rules.
  One extra SELECT at auth time, cached for connection lifetime.
- **Go unit tests** — `go test ./...` has no suite yet. Current
  coverage is end-to-end-only (README §Run and §Success criteria).

## 9. References

- `docs/ROADMAP.md` §Load Harness — stream definition + gating.
- `docs/icd/ICD-0.1.6-websocket-events.md` — base WS protocol.
- `docs/icd/ICD-0.2.2-capability-resolution.md` — `call_capability`
  pipeline + call-depth semantics.
- `docs/icd/ICD-0.2.4-capability-rbac.md` — step-3 RBAC enforcement
  + `kernel.admin` universal match.
- `project_ws_flaky_segfault.md` (session memory) — full history of
  the teardown flake family.
- `project_next_session_lh0.md` (session memory) — 2026-04-21
  direction that motivated this milestone.
