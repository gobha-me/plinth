# DESIGN: QuickJS Async Bridge

**Status:** Draft  
**Scale:** 3 — Architecture Arc  
**Milestone:** 0.3 (QuickJS Integration)  
**Author:** the maintainer (Architect) + Claude (Architecture Session)
**Date:** 2026-04-14  
**Depends on:** architecture/02-capabilities.md §3 (Capability Call Flow), architecture/05-extensions.md §3 (QuickJS Runtime)  
**Implementation tasks:** 0.3.0–0.3.5

---

## 1. Why This Exists

This is the hardest engineering problem in Plinth. Everything the
platform does — capability calls, database queries, HTTP requests,
pub/sub — flows through the bridge between QuickJS (single-threaded
JS interpreter) and Drogon (multi-threaded async C++ framework).

If the bridge is synchronous, every `await` in JS pins a Drogon
thread. With 16 threads and 16 concurrent `await db.query()` calls,
the server is deadlocked. No new requests can be handled.

If the bridge is poorly designed, resource leaks, dangling coroutines,
or uncancelled async operations accumulate until the process dies.

If the bridge can't enforce resource limits during async gaps, a
malicious extension can evade memory and CPU constraints by spending
most of its time in `await`.

**This document defines the exact mechanism.** A code session
implementing 0.3.x reads this document, not the architecture doc's
summary.

---

## 2. The Actors

### QuickJS Runtime (`JSRuntime` + `JSContext`)

- Single-threaded. Must only be touched from one thread at a time.
- No built-in event loop. The host (C++) drives execution via
  `JS_ExecutePendingJob`.
- Promises exist in JS but QuickJS doesn't resolve them — the host
  does, by calling resolve/reject functions from C++.
- `JS_SetInterruptHandler` allows the host to terminate execution
  between any two JS opcodes (CPU time enforcement).
- `JS_SetMemoryLimit` hard-caps the JS heap (memory enforcement).
- `JS_FreeRuntime` tears down the entire heap. Safe to call at any
  point — all JS objects, including pending promises, are freed.

### Drogon Event Loop

- Multi-threaded. N event loop threads (default: `hardware_concurrency`).
- Async operations (PG queries, HTTP client requests, timers) are
  dispatched to the event loop and completed via callbacks or
  C++20 coroutines (`drogon::Task<T>`, `co_await`).
- A coroutine that `co_await`s yields its thread back to the event
  loop. The thread handles other requests. When the awaited operation
  completes, the coroutine resumes (possibly on a different thread).

### The Bridge

The C++ layer that connects JS function calls to Drogon async
operations. It is responsible for:

1. Creating and managing JS runtimes
2. Injecting kernel APIs into JS contexts
3. Translating JS `await` into Drogon `co_await`
4. Propagating results and errors between the two worlds
5. Enforcing resource limits
6. Cleaning up on failure or cancellation

---

## 3. Execution Model

### 3.1 One Coroutine Per Extension Invocation

When a capability call arrives that dispatches to a JS handler:

```
HTTP Request arrives at Drogon
    │
    ▼
Drogon route handler (C++ coroutine)
    │
    ▼
Capability resolver → handler is JS extension
    │
    ▼
Bridge::execute(extension, function, args)
    │
    │  Creates or acquires a JSRuntime from the pool
    │  Calls the JS handler function
    │  Returns a drogon::Task<nlohmann::json>
    │
    ▼
JS executes synchronously until it hits an `await`
    │
    │  await db.query("SELECT ...")
    │
    ▼
Bridge intercepts: JS Promise created, C++ callback registered
    │
    ▼
C++ initiates PG query via Drogon async DB client
    │
    │  co_await pgClient->execSqlCoro("SELECT ...")
    │  ─── thread yields to event loop ───
    │  ─── other requests proceed ────────
    │  ─── PG query completes ────────────
    │
    ▼
C++ coroutine resumes (possibly different thread, but same coroutine)
    │
    ▼
Bridge resolves JS Promise with query result
    │
    ▼
JS_ExecutePendingJob → JS continues executing after the await
    │
    │  (more JS code, possibly more awaits)
    │
    ▼
JS handler function returns
    │
    ▼
Bridge extracts return value, converts to nlohmann::json
    │
    ▼
Drogon route handler sends HTTP response
```

**Key invariant:** The JSRuntime is only touched from within the
bridge coroutine. The coroutine may be suspended and resumed on
different threads, but QuickJS execution is never concurrent — the
coroutine is the serialization mechanism.

### 3.2 Runtime Pool

Creating a `JSRuntime` + `JSContext` + injecting all kernel APIs
takes measurable time (estimated 1-5ms). For capability calls on
the hot path, this overhead is unacceptable.

**Solution: Runtime pool per extension.**

Each extension has a pool of pre-initialized runtimes:
- Pool size: configurable, default `min(4, thread_count / 2)`
- On capability call: acquire runtime from pool
- On completion: return runtime to pool (after resetting state)
- On pool exhaustion: create a new runtime on-demand (above pool size)
  and destroy it after use (don't grow the pool unboundedly)

**State reset between uses:**
- Global variables set during the previous execution must be cleared
- The extension's module-level state persists (this is intentional —
  it allows extensions to cache data across calls)
- The JS heap is not freed between calls (that would destroy the
  module state) — it's only freed when the runtime is destroyed

**Pool lifecycle:**
- Created when extension is installed and enabled
- Destroyed when extension is disabled or uninstalled
- Rebuilt on extension hot-reload (new code → new runtimes)

### 3.3 The JS ↔ C++ Promise Bridge

This is the core mechanism. It must handle:
- JS calling an async C++ function
- C++ resolving/rejecting the JS Promise
- Multiple nested awaits (JS calls async, which calls async, etc.)
- Error propagation in both directions

#### Registration

Each kernel API function that supports `await` is registered as a
C function in QuickJS:

```cpp
// Pseudo-code for db.query registration
JSValue js_db_query(JSContext* ctx, JSValueConst this_val,
                    int argc, JSValueConst* argv) {
    // 1. Extract SQL string from argv[0]
    std::string sql = js_to_string(ctx, argv[0]);

    // 2. Create a JS Promise + resolve/reject functions
    JSValue resolve_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolve_funcs);

    // 3. Store resolve/reject in bridge context for later
    auto bridge = get_bridge(ctx);
    int callback_id = bridge->register_pending(resolve_funcs);

    // 4. Signal to the coroutine that an async operation is pending
    bridge->enqueue_async_op(AsyncOp{
        .type = AsyncOp::DB_QUERY,
        .callback_id = callback_id,
        .sql = std::move(sql)
    });

    // 5. Return the Promise to JS
    return promise;
}
```

#### Coroutine Loop

The bridge coroutine runs a loop that alternates between JS execution
and async operation processing:

```cpp
drogon::Task<nlohmann::json> Bridge::execute(
    Extension& ext, std::string_view function,
    nlohmann::json args
) {
    auto* runtime = ext.pool.acquire();
    auto guard = ScopeGuard([&] { ext.pool.release(runtime); });

    // Start JS execution
    JSValue result = call_js_function(runtime->ctx, function, args);

    // Process async operations until JS completes
    while (runtime->has_pending_ops()) {
        // Drain all pending async ops
        for (auto& op : runtime->take_pending_ops()) {
            switch (op.type) {
                case AsyncOp::DB_QUERY: {
                    auto db_result = co_await execute_pg_query(op.sql);
                    // Resolve or reject the JS promise
                    runtime->resolve(op.callback_id, db_result);
                    break;
                }
                case AsyncOp::HTTP_REQUEST: {
                    auto http_result = co_await execute_http(op);
                    runtime->resolve(op.callback_id, http_result);
                    break;
                }
                case AsyncOp::CAP_CALL: {
                    auto cap_result = co_await execute_capability(op);
                    runtime->resolve(op.callback_id, cap_result);
                    break;
                }
                // ... other async op types
            }
        }

        // Resume JS execution — process resolved promises
        while (JS_IsJobPending(runtime->rt)) {
            JS_ExecutePendingJob(runtime->rt, &runtime->ctx);
        }

        // JS may have enqueued more async ops — loop continues
    }

    // JS has completed with no pending ops — extract result
    co_return js_to_json(runtime->ctx, result);
}
```

**Critical detail:** Multiple async ops can be pending simultaneously.
If JS code does:

```javascript
const [users, notes] = await Promise.all([
    db.query("SELECT * FROM users"),
    db.query("SELECT * FROM notes")
]);
```

The bridge sees two DB_QUERY ops after the first JS_ExecutePendingJob
call. Both are `co_await`ed. Drogon runs both PG queries concurrently.
When both complete, the bridge resolves both promises, and JS continues.

**This is where parallel performance comes from.** `Promise.all()` in
extension JS actually runs queries in parallel, not sequentially.

---

## 4. Resource Limits

### 4.1 Memory Limit

Set via `JS_SetMemoryLimit(runtime->rt, limit_bytes)` at runtime
creation. QuickJS enforces this on every allocation — if the JS
heap exceeds the limit, `malloc` inside QuickJS returns NULL, which
QuickJS converts to a JS exception (`InternalError: out of memory`).

**During async gaps:** When JS is suspended (waiting for a co_await
to complete), the JS heap is idle. No JS code is executing, so no
allocations happen. The memory limit doesn't need special enforcement
during gaps — the heap simply exists at its current size.

**However:** The C++ side of the bridge allocates memory for async
operation results (PG query result sets, HTTP response bodies). This
memory is NOT counted against the JS heap limit. It's C++ heap memory.

**Mitigation:** The bridge enforces a separate **result size limit**
per async operation. If a PG query returns 100MB of rows, the bridge
truncates/rejects before injecting into the JS heap. Default: 16MB
per individual async result. Configurable per-extension.

### 4.2 CPU Time Limit

QuickJS supports an interrupt handler via `JS_SetInterruptHandler`.
The kernel installs a handler that checks elapsed CPU time:

```cpp
int interrupt_handler(JSRuntime* rt, void* opaque) {
    auto* ctx = static_cast<BridgeContext*>(opaque);
    auto elapsed = now() - ctx->execution_start;
    if (elapsed > ctx->cpu_time_limit) {
        return 1; // terminate JS execution
    }
    return 0; // continue
}
```

The interrupt handler fires between JS opcodes (not during C++ async
operations). When JS is suspended at an `await`, no JS opcodes are
executing, so the interrupt handler doesn't fire.

**CPU time accounting:** The timer measures **JS execution time only**,
not wall-clock time including async waits. If JS runs for 100ms, then
awaits a PG query for 2 seconds, then runs for 100ms more, the CPU
time used is 200ms, not 2200ms.

Implementation: start the timer when JS begins executing
(`JS_ExecutePendingJob` or initial `call_js_function`). Pause when
the coroutine yields for an async op. Resume when JS execution
resumes. This requires tracking enter/exit points in the bridge loop.

```cpp
// In the bridge loop:
ctx->resume_cpu_timer();
while (JS_IsJobPending(runtime->rt)) {
    JS_ExecutePendingJob(runtime->rt, &runtime->ctx);
}
ctx->pause_cpu_timer();

// co_await happens here — timer is paused
auto result = co_await execute_pg_query(op.sql);

ctx->resume_cpu_timer();
runtime->resolve(op.callback_id, result);
// JS runs again with timer active
```

### 4.3 Total Wall-Clock Timeout

Separate from CPU time limit. A single extension invocation cannot
run for more than N seconds of wall-clock time (default: 30 seconds,
configurable). This catches extensions that chain long async waits
to stay alive indefinitely.

If the wall-clock timeout fires:
1. All pending async operations are cancelled
2. The JS runtime is terminated (interrupt handler returns 1)
3. An error is returned to the caller
4. The runtime is destroyed (not returned to pool — state is unknown)

### 4.4 Call Depth Limit

Extension A calls `cap.call("B:1:func")`, which is handled by
extension B, which calls `cap.call("C:1:func")`, etc. Each hop
creates a new coroutine (or reuses one from the pool).

**Risk:** Infinite recursion via capability calls, exhausting
coroutines, stack, and memory.

**Mitigation:** The kernel tracks call depth per request. Each
capability dispatch increments a depth counter passed via the
bridge context. Default max depth: 8. Exceeding the limit returns
an error: "capability call depth limit exceeded."

The depth counter is per originating request, not per extension.
If an HTTP request triggers A → B → C, the depth is 3 regardless
of which extensions are involved.

---

## 5. Error Propagation

### 5.1 C++ Error → JS Exception

When an async operation fails (PG query error, HTTP timeout,
capability call denied), the bridge **rejects** the JS Promise:

```cpp
void BridgeContext::reject(int callback_id, const std::string& error) {
    JSValue error_obj = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, error_obj, "message",
        JS_NewString(ctx, error.c_str()), JS_PROP_WRITABLE);
    JS_Call(ctx, reject_funcs[callback_id], JS_UNDEFINED, 1, &error_obj);
    JS_FreeValue(ctx, error_obj);
}
```

In JS, this means:
```javascript
try {
    const rows = await db.query("INVALID SQL");
} catch (e) {
    // e.message = "ERROR: syntax error at or near ..."
    log.error("Query failed: " + e.message);
}
```

Unhandled rejections (no `try/catch` around an `await`) propagate up
as unhandled exceptions. The bridge catches these at the top level
and converts them to an error response.

### 5.2 JS Exception → C++ Error

If JS throws during synchronous execution (not during an await), the
bridge detects it via `JS_IsException`:

```cpp
JSValue result = JS_Call(ctx, func, ...);
if (JS_IsException(result)) {
    JSValue exception = JS_GetException(ctx);
    // Extract message, stack trace
    // Return as error from the coroutine
    co_return Error{"Extension threw: " + extract_message(exception)};
}
```

### 5.3 Error Context Preservation

Every error propagated through the bridge includes:
- **Origin:** Which layer generated it (JS, PG, HTTP, capability)
- **Extension:** Which extension was executing
- **Capability:** Which capability was being handled
- **Stack trace:** JS stack trace if available
- **Duration:** How long the execution ran before failing

This context is logged to `plinth.audit_log` and returned to the
caller in a structured error response.

---

## 6. Cancellation and Cleanup

### 6.1 Cancellation Triggers

An execution can be cancelled by:
1. **Wall-clock timeout** (§4.3)
2. **Node shutdown** (graceful shutdown signal)
3. **Extension disable** (admin action while execution is in flight)
4. **Client disconnect** (HTTP connection closed, WebSocket dropped)

### 6.2 Cancellation Procedure

**Step 1: Signal cancellation.**
Set a cancellation flag on the bridge context. The next interrupt
handler check will see it and terminate JS execution.

**Step 2: Cancel in-flight async operations.**
Each pending async op has a cancellation token (Drogon provides
`co_await` cancellation via `drogon::Task`). The bridge iterates
pending ops and cancels each:
- PG queries: connection is returned to pool, query result is
  discarded when it eventually completes
- HTTP requests: connection is closed
- Capability calls: downstream bridge contexts are cancelled
  (recursive cancellation)

**Step 3: Tear down JS runtime.**
`JS_FreeRuntime` is called. This frees all JS objects including
pending promises, closures, and module state. It is safe to call
at any point — QuickJS handles dangling references internally.

**Step 4: Do NOT return runtime to pool.**
A cancelled runtime's state is unknown. It is destroyed, not recycled.
The pool creates a fresh runtime to replace it.

**Step 5: Report.**
Log the cancellation reason and duration to audit. Return an
appropriate error to the caller.

### 6.3 Teardown Order

This is critical. Wrong order → use-after-free or dangling callbacks.

```
1. Set cancellation flag
2. Cancel all pending C++ async operations
3. Wait for cancelled operations to complete/abort
   (with a hard timeout of 5 seconds)
4. Resolve/reject all pending JS promises with cancellation errors
   (so QuickJS can clean up internal promise state)
5. Call JS_FreeRuntime
6. Free bridge context
```

Step 3 is important: cancelled PG queries may still be in-flight at
the PG server. The connection is returned to the pool, and the result
is discarded when it arrives. We don't wait indefinitely — after 5
seconds, we abandon and let PG clean up on its side.

Step 4 ensures QuickJS doesn't leak internal promise tracking state.
Even though we're about to free the runtime, resolving promises
allows QuickJS's destructor to run cleanly.

---

## 7. Concurrency Within an Extension

### 7.1 Serial vs. Parallel Awaits

**Serial:**
```javascript
const a = await db.query("SELECT 1");
const b = await db.query("SELECT 2");
```
Two queries, executed sequentially. Total time ≈ sum of both.

**Parallel:**
```javascript
const [a, b] = await Promise.all([
    db.query("SELECT 1"),
    db.query("SELECT 2")
]);
```
Two queries, executed concurrently by Drogon. Total time ≈ max of both.

The bridge naturally supports this: `Promise.all` creates both
promises before any `await` suspends JS. The bridge sees both
DB_QUERY ops in its pending queue and dispatches both to Drogon.

### 7.2 Maximum Concurrent Async Operations Per Execution

An extension could call `Promise.all([...1000 queries...])`. Each
query uses a PG connection from the pool. 1000 concurrent queries
would exhaust the connection pool.

**Mitigation:** Per-execution concurrent async op limit.
Default: 16. Configurable per-extension.

When the limit is reached, additional async ops are queued internally
in the bridge and dispatched as earlier ops complete. This is
transparent to JS — the promises still resolve, just not all at once.

### 7.3 cap.batch() Implementation

`cap.batch()` is syntactic sugar over `Promise.all()` with capability
calls:

```javascript
// This:
const results = await cap.batch([
    ["terminal:1:shell", "ls"],
    ["fs:1:read", "/etc/hostname"]
]);

// Is equivalent to:
const results = await Promise.all([
    cap.call("terminal:1:shell", "ls"),
    cap.call("fs:1:read", "/etc/hostname")
]);
```

The difference: `cap.batch()` can optimize at the dispatch layer.
If multiple calls target the same remote node (Tier 3), the bridge
bundles them into a single HTTP request to that node. This is an
optimization opportunity, not a correctness requirement. The initial
implementation can simply expand to `Promise.all`.

---

## 8. Integration with Kernel Systems

### 8.1 Capability Dispatch

When JS calls `cap.call("terminal:1:shell", "ls")`:

1. Bridge creates a CAP_CALL async op
2. The coroutine resolves the capability (Tier 1/2/3)
3. If the target is another JS extension on the same node:
   - A NEW coroutine is spawned (or a runtime acquired from the
     target extension's pool)
   - The call depth counter is incremented
   - The result is returned when the target completes
4. If the target is a sidecar or remote node:
   - HTTP request dispatched via Drogon
   - Result returned via the normal async path

**Same-node JS→JS calls** do NOT reuse the caller's runtime. Each
extension gets its own runtime from its own pool. This maintains
isolation and prevents stack overflow within QuickJS.

### 8.2 Realtime Events

When `db.exec()` is called from JS and the debounced change event
fires, the event goes through PG NOTIFY → kernel WS broker → clients.
The JS extension doesn't need to do anything — the kernel's db layer
handles event emission.

If JS calls `pubsub.publish()` (Layer 3 event), the bridge creates
an async op that calls PG NOTIFY directly. This is a fast path —
NOTIFY doesn't require a co_await (it's part of the current PG
transaction).

### 8.3 Storage

`storage.get()` and `storage.put()` are synchronous filesystem calls
in the default implementation. The bridge wraps them in a Drogon
`co_await` on a thread pool to avoid blocking the event loop thread
on filesystem I/O. This is transparent to JS.

---

## 9. Implementation Sequence

Per the roadmap, this maps to milestone 0.3:

### 0.3.0 — QuickJS Vendored + Basic Eval

**What:** Vendor QuickJS source. Build as static library via CMake.
Verify: create a JSRuntime, eval `"1 + 1"`, get `2` back in C++.

**Scope:** No bridge, no async, no kernel APIs. Just prove QuickJS
compiles and runs within Drogon's process.

**Tests:**
- Create runtime, eval simple expression, verify result
- Create runtime, eval syntax error, verify error handling
- Create runtime with memory limit, exceed it, verify failure
- Create/destroy 1000 runtimes (leak check)

### 0.3.1 — Runtime Lifecycle + Limits

**What:** Build the runtime pool. Configure memory limit, CPU time
limit, stack depth per-runtime. Implement interrupt handler for CPU
time enforcement.

**Scope:** Pool acquire/release, limit configuration, interrupt
handler. No async bridge yet — all JS execution is synchronous.

**Tests:**
- Pool: acquire N runtimes, release, acquire again (reuse)
- Pool exhaustion: acquire more than pool size, verify on-demand
  creation
- Memory limit: run JS that allocates beyond limit, verify error
- CPU time: run infinite loop, verify termination within limit
- Stack depth: deep recursion, verify termination

### 0.3.2 — Kernel Standard Library (Synchronous)

**What:** Inject the kernel API functions that DON'T need async:
`log.*`, `config.get()`, `crypto.*`. These are pure C++ functions
callable from JS with no `await` needed.

**Scope:** Function registration, type conversion (JS ↔ C++),
error handling for bad arguments.

**Tests:**
- `log.info("hello")` → verify message appears in kernel log
- `config.get("key")` → verify correct value returned
- `crypto.hash("sha256", "data")` → verify correct hash
- Call with wrong argument types → verify JS exception

### 0.3.3 — Async Bridge (THE CRITICAL TASK)

**What:** Implement the coroutine-based async bridge as described
in §3 of this document. Wire up `db.query()` and `db.exec()` as
the first async APIs.

**Scope:** Bridge coroutine loop, promise creation/resolution,
async op queue, PG query dispatch via Drogon async client, CPU
timer pause/resume during async gaps.

**Entry criteria:** 0.3.1 (runtime pool) and 0.3.2 (sync APIs) are
complete and tested.

**Exit criteria:** The following ALL pass:

**Correctness tests:**
- `await db.query("SELECT 1")` → returns result
- `await db.query("INVALID")` → JS exception with PG error message
- Sequential awaits: `a = await q1; b = await q2;` → both complete
- Parallel awaits: `Promise.all([q1, q2])` → both queries run
  concurrently (verify via timing — parallel should be ~1x, not ~2x
  the single query time)
- Nested awaits: `await` inside a `.then()` callback
- Unhandled rejection: `await` without try/catch on bad query →
  bridge catches and reports

**Resource limit tests:**
- Memory limit during JS execution (between awaits) → enforced
- CPU time limit during JS execution (between awaits) → enforced
- CPU time limit does NOT count async wait time
- Wall-clock timeout → cancels execution, returns error
- Concurrent query limit → excess queries queued, all eventually
  complete

**Cancellation tests:**
- Cancel during PG query → runtime torn down cleanly, no leak
- Cancel during JS execution → interrupt handler fires, clean
  teardown
- Double cancel → no crash
- Cancel with pending promises → clean teardown (§6.3 order)

**Concurrency tests:**
- 100 concurrent capability calls to JS extension → all complete,
  runtime pool manages allocation
- 100 concurrent calls each doing `Promise.all([5 queries])` →
  PG connection pool not exhausted (concurrent op limit enforced)

### 0.3.4 — cap.call() from JS

**What:** Wire up `cap.call()` in the JS runtime to dispatch through
the capability registry. This connects the JS bridge to the rest of
the kernel.

**Scope:** RBAC check, three-tier resolution, call depth tracking.

**Tests:**
- JS calls capability provided by same extension (Tier 1)
- JS calls capability provided by another JS extension (Tier 2)
- JS calls capability without RBAC permission → denied
- Call depth: A → B → C → D ... → exceed limit → error
- `cap.batch()` with mixed tiers → all resolve

### 0.3.5 — Memory + CPU Hardening

**What:** Stress testing and edge case hardening. Run adversarial
JS code designed to break the bridge.

**Tests (adversarial):**
- Allocate until memory limit, then `await`, then allocate more →
  limit still enforced
- Infinite loop inside a `.then()` callback → CPU limit fires
- `Promise.all` with 10,000 entries → concurrent op limit prevents
  PG pool exhaustion
- Create promises in a loop without awaiting → memory limit fires
- Throw exception inside promise `.catch()` → doesn't crash bridge
- Return non-serializable value from handler → bridge returns error,
  doesn't crash
- `eval()` attempt (disabled by default) → error, not execution
- Deeply nested `cap.call` chains → depth limit enforced
- Wall-clock timeout with 100 pending async ops → all cancelled
  cleanly

---

## 10. Data Structures

### BridgeContext

```cpp
struct BridgeContext {
    JSRuntime* rt;
    JSContext* ctx;
    Extension* extension;

    // Async operation queue
    std::vector<AsyncOp> pending_ops;

    // Promise resolve/reject functions (indexed by callback_id)
    std::unordered_map<int, std::pair<JSValue, JSValue>> callbacks;
    int next_callback_id = 0;

    // Resource tracking
    std::chrono::steady_clock::time_point execution_start;
    std::chrono::steady_clock::time_point cpu_timer_start;
    std::chrono::nanoseconds cpu_time_accumulated{0};
    std::chrono::milliseconds cpu_time_limit;
    std::chrono::milliseconds wall_clock_limit;
    int concurrent_async_ops = 0;
    int max_concurrent_async_ops;

    // Call depth (shared across the request chain)
    int call_depth;
    int max_call_depth;

    // Cancellation
    std::atomic<bool> cancelled{false};

    // Methods
    int register_pending(JSValue resolve_funcs[2]);
    void resolve(int callback_id, const nlohmann::json& result);
    void reject(int callback_id, const std::string& error);
    std::vector<AsyncOp> take_pending_ops();
    bool has_pending_ops() const;
    void resume_cpu_timer();
    void pause_cpu_timer();
    bool cpu_limit_exceeded() const;
    bool wall_clock_exceeded() const;
};
```

### AsyncOp

```cpp
struct AsyncOp {
    enum Type {
        DB_QUERY,
        DB_EXEC,
        HTTP_REQUEST,
        CAP_CALL,
        STORAGE_GET,
        STORAGE_PUT,
        PUBSUB_PUBLISH,
    };

    Type type;
    int callback_id;

    // Operation-specific data (variant or union)
    std::string sql;            // DB_QUERY, DB_EXEC
    HttpRequest http_req;       // HTTP_REQUEST
    CapabilityCall cap_call;    // CAP_CALL
    StorageOp storage_op;       // STORAGE_GET, STORAGE_PUT
    PubSubOp pubsub_op;        // PUBSUB_PUBLISH

    // Options
    bool silent = false;        // DB_EXEC: suppress realtime event
};
```

### RuntimePool

```cpp
class RuntimePool {
    Extension& extension;
    std::vector<BridgeContext*> available;
    std::mutex mu;
    int pool_size;
    int active_count = 0;

public:
    BridgeContext* acquire();   // Get from pool or create on-demand
    void release(BridgeContext*);  // Return to pool (reset state)
    void destroy(BridgeContext*);  // Destroy (cancelled/failed, don't reuse)
    void rebuild();             // Destroy all, create fresh (hot-reload)
};
```

---

## 11. Open Questions (To Resolve During Implementation)

1. **Runtime pool sizing:** Is `min(4, threads/2)` the right default?
   Needs benchmarking. Too small = contention. Too large = memory waste.

2. **PG connection pool interaction:** Drogon has its own PG connection
   pool. With N runtimes each doing M concurrent queries, the total
   concurrent PG connections needed is N*M. Need to ensure the Drogon
   PG pool is sized appropriately (at least `pool_size * max_concurrent_async_ops`).

3. **Module state persistence:** Between calls, the JS module's
   top-level state persists (intentional for caching). But this means
   memory usage can grow over time. Should the pool periodically
   destroy and recreate runtimes to prevent heap fragmentation?
   Or rely on the memory limit to catch runaway growth?

4. **Thread affinity:** Drogon may resume a coroutine on a different
   thread than where it was suspended. QuickJS doesn't care (it's not
   thread-aware), but the bridge must ensure no concurrent access.
   The coroutine serializes access. Verify this holds under load.

5. **`db.exec` with `{ silent: true }` implementation:** The bridge
   needs to pass the `silent` flag through to the kernel DB layer.
   This is straightforward but the async op needs to carry the option.

---

## 12. What This Document Does NOT Cover

- **Frontend SDK** (`plinth.useData`, `plinth.subscribe`): Covered in
  `architecture/03-data.md §3` (realtime pub/sub) and will have its
  own ICD.
- **Sidecar communication**: Tier 3 dispatch details are in
  `architecture/04-services-ha.md §5` (Sidecar Contract).
- **RBAC enforcement**: How the capability resolver checks permissions
  is covered in `architecture/01-identity.md §2` (Groups and RBAC) and
  `architecture/02-capabilities.md §1` (Capability Registry).
- **Extension installation and manifest parsing**: Covered in
  `architecture/05-extensions.md §1` (Package Structure) and milestone
  0.4.

This document covers ONLY the runtime execution bridge between
QuickJS and Drogon. Everything above (how a request reaches the
bridge) and below (how async operations are executed) is defined
elsewhere.
