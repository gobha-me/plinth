# Shutdown ownership and dependency graph

Plinth has one normal shutdown owner:
`lifecycle::ShutdownCoordinator`. Production `main` and the Drogon-backed test
fixtures invoke the same coordinator; neither keeps a hand-written teardown
sequence. `atexit` and signal handlers do not perform normal teardown.

`SIGINT` and `SIGTERM` are blocked before any service thread starts. The
service-owning main thread consumes them with `sigtimedwait`, outside signal
context, and invokes the coordinator. Drogon's signal handling is disabled.

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
   - Join the capability and realtime PostgreSQL listeners so they cannot
     admit new cache or event work.
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
   - Drain and join the events writer.
   - Discard any remaining database batch scopes.
   - Stop the realtime broker.
   - Flush coalescer windows and join its event loop.
9. `close_audit_gate`
   - Prevent any later audit call from entering Drogon's database manager.
     The spdlog sinks remain open.
10. `stop_drogon`
   - Stop listeners and event loops, destroy Drogon's database manager, and
     join the thread running `app().run()`.
11. `close_log_sinks`
   - Run only after the Drogon thread has joined.

The first seven nodes establish this dependency relation:

```text
ingress -> listeners -> owned workers -> runtime leases
                                      -> database-backed flushes
                                      -> Drogon database/event loops
                                      -> logging sinks
```

## Bounds and failure policy

One 40-second deadline is shared by the owned-worker and runtime-drain nodes;
every graph node receives only the remaining budget. Subsystems with their own
drains keep their tighter configured limits inside that outer bound. Repeated
calls are idempotent. A 50-second process watchdog is the final bound if an
operating-system or third-party call fails to honor its cooperative deadline;
it writes a fixed diagnostic and exits with status 2 without running unsafe
destructors.

If an owned drain misses the deadline, the coordinator stops at that node. It
does not destroy a runtime, database manager, or logger still reachable by live
work. Production records the failed node, flushes the diagnostic log, and uses
an immediate failing process exit rather than entering unsafe teardown. The
coordinator itself remains retryable so tests can release a deliberately
blocked worker and prove a later drain joins it.

Partial startup uses the same graph through a stack owner in `main`. Every stop
operation is idempotent and tolerates a component that never started, so an
exception or validated early return unwinds only live state without a separate
cleanup path.
