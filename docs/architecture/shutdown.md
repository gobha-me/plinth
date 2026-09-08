# Shutdown ownership and dependency graph

Plinth has one normal shutdown owner:
`lifecycle::ShutdownCoordinator`. Production `main` and the Drogon-backed test
fixtures invoke the same coordinator; neither keeps a hand-written teardown
sequence. `atexit` and signal handlers do not perform normal teardown.

`SIGINT` and `SIGTERM` are blocked before any service thread starts. A
main-owned signal thread consumes them with `sigtimedwait` from before logging
initialization through final teardown. It requests cancellation and arms the
process deadline; main invokes the coordinator outside signal context.
Drogon's signal handling is disabled.

## Dependency graph

The coordinator executes these nodes in order:

1. `close_ingress`
   - Flip the global HTTP/TCP admission gate.
   - Disable extension asset routes.
   - Cancel WebSocket timers, initiate normal WebSocket closes, and close the
     connection registry to later mutation.
2. `drain_http_requests`
   - Wait for every HTTP handler admitted before the ingress gate closed to
     produce its response. A pre-handling fallback rejects requests that raced
     past the earlier synchronous gate.
3. `stop_listeners`
   - Join the capability PostgreSQL listener so it cannot admit cache work.
     Realtime LISTEN remains alive as a downstream consumer of accepted writes.
4. `drain_async_tasks`
   - Drain standalone WS capability and replay coroutines. Admission closes in
     `close_ingress`; every accepted coroutine owns a completion lease.
5. `drain_rbac_workers`
   - Close the owned RBAC worker registry, request cancellation, and join both
     top-level runs and their timed capability invocations.
6. `drain_extension_dispatches`
   - Close extension dispatch admission, drain every accepted shared runtime
     lease, and destroy the extension pools.
7. `drain_js_stress_dispatches`
   - Close diagnostic JS admission, drain every accepted shared runtime lease,
     and destroy the diagnostic pool.
8. `flush_database_state`
   - Discard any remaining database batch scopes.
   - Flush coalescer windows and join its event loop.
   - Send a unique internal notification on the existing realtime LISTEN
     connection. PostgreSQL delivers notifications in transaction commit order;
     acknowledgment on that same connection proves that every earlier envelope
     reached the synchronous writer handler. The listener pauses dispatch at
     this boundary, retaining its connection and thread.
   - Drain and join the events writer, including its in-flight INSERT/COMMIT
     work. Queue drops and failed INSERTs make this step fail even if the queue
     subsequently becomes empty.
   - Stop and join the realtime listener, then stop the realtime broker.
9. `close_audit_gate`
   - Prevent any later audit call from entering Drogon's database manager.
     The spdlog sinks remain open.
10. `stop_drogon`
   - Release registry-owned WebSocket connections and state on their original
     IO loops, and wait for each release acknowledgment before stopping loops.
     Timeout retains pending owners and prevents Drogon teardown.
   - Stop listeners and event loops, destroy Drogon's database manager, and
     join the thread running `app().run()`.
11. `close_log_sinks`
   - Run only after the Drogon thread has joined.

The first seven nodes establish this dependency relation:

```text
ingress -> capability listener -> owned workers -> runtime leases
  -> coalescer flush -> realtime delivery acknowledgment -> durable writer drain
  -> realtime listener/broker stop -> WS owner release on IO loops
  -> Drogon database/event loops -> logging sinks
```

## Bounds and failure policy

One 40-second deadline is shared by the owned-worker and runtime-drain nodes;
every graph node receives only the remaining budget. Subsystems with their own
drains keep their tighter configured limits inside that outer bound. Repeated
calls are idempotent. A 50-second process watchdog starts at the first consumed
shutdown signal, or at normal teardown entry. Its absolute deadline is never
restarted during partial startup, coordinator execution, the Drogon join, or
logging shutdown. It is the final bound if an operating-system or third-party
call fails to honor its cooperative deadline; it writes directly to stderr
and exits with status 2 without running unsafe destructors or logging code.

If an owned drain misses the deadline, the coordinator stops at that node. It
does not destroy a runtime, database manager, or logger still reachable by live
work. Production writes the failed node directly to stderr and uses an
immediate failing process exit rather than entering unsafe teardown. The
coordinator itself remains retryable so tests can release a deliberately
blocked worker and prove a later drain joins it.

The realtime marker uses a separate internal PostgreSQL channel and never
enters event storage or replay. A disconnected/reconnected listener cannot
certify delivery from its previous session; marker errors and deadline expiry
fail the drain. Realtime disabled or never started is an idempotent no-op.
There is no sleep-based notification settling period. This graceful-drain
barrier does not make PostgreSQL notifications a durable outbox across crashes
or earlier connection outages.

Partial startup uses the same graph through a stack owner in `main`. Every stop
operation is idempotent and tolerates a component that never started, so an
exception or validated early return unwinds only live state without a separate
cleanup path.

## Startup database cancellation

The main startup scope bounds each direct libpq operation to ten seconds and
checks signal cancellation while polling at intervals of at most 50 ms.
Connection establishment and both simple and parameterized SQL use libpq's
nonblocking protocol, including output flushing and multi-statement result
draining. Existing shorter connection timeouts remain effective. Capability
and realtime listener threads use five-second scopes with their own stop
tokens; RBAC workers also carry their owner's cancellation token.

Startup failure is sticky. A timeout or cancellation prevents error handling,
synchronous auditing, rollback guards, or advisory-lock guards from opening
another blocking operation. Explicit checkpoints between startup phases and
inside reconciliation, migration, and extension initialization loops unwind
through the normal coordinator. Runtime request threads outside these scopes
retain their existing database operation policy.

Coordinator teardown temporarily enters its own bounded database scope without
the cancelled startup token. Accepted final writes and coalescer audits can
therefore complete before the listener marker and writer drain. The startup
scope's failure remains sticky when control returns, and the signal owner's
original 50-second deadline is never reset. A database timeout or unconfirmed
cancellation during this drain makes shutdown fail. The realtime listener's
scope carries only its own thread token; the coordinator requests that token
after marker acknowledgment and writer persistence, not at the initial signal.

For an in-flight statement, the connection owner opens an authenticated
nonblocking control connection and sends `pg_cancel_backend` for its own
backend PID. One two-second cleanup deadline covers that connection, the cancel
query, and consuming the original statement's completion. This avoids libpq
16's blocking `PQcancel`. The owner then closes its connection; no other thread
touches its `PGconn` or socket. Unconfirmed cancellation is reported as failure,
including by listener and worker drains. Closing the client socket alone is
not accepted as proof that a lock-waiting statement stopped.

The control connection needs one available PostgreSQL connection slot. If
authentication, capacity, or connectivity prevents cancellation confirmation,
shutdown reports failure; it does not claim a clean database drain.

The process watchdog also covers synchronous DNS inside `PQconnectStart`,
filesystem operations, and third-party initialization that cannot honor a stop
token. Watchdog termination is a failure fallback, not clean cancellation.

Subprocess coverage holds production at an incomplete loopback PostgreSQL
handshake, a groups-bootstrap table lock, and a package-reconciliation row
lock, then sends each shutdown signal. Lock tests retain the conflicting
transaction until the cancelled backend disappears. Fresh boot, restart,
ordinary active-work shutdown, and partial-startup tests remain applicable.
