# plinth load harness (LH-0 + LH-0.1 + LH-1)

Standalone external binary that drives the production `plinth` kernel
under sustained HTTP + WS load. Purpose: (a) reproduce the residual
test-harness flake family (`quickjs.c:6678: free_zero_refcount`,
`JS_FreeRuntime: list_empty(&rt->gc_obj_list)`, trantor `bad_weak_ptr`
at teardown) against a real kernel, outside the Catch2 subprocess
model; (b) exercise the v0.5.0 PG `LISTEN/NOTIFY` realtime bus under
sustained Layer-3 storm load. See ICDs at
[`docs/icd/ICD-LH-0-load-harness-scaffold.md`](../docs/icd/ICD-LH-0-load-harness-scaffold.md)
(sync scaffold),
[`docs/icd/ICD-LH-0.1-async-bridge-stress.md`](../docs/icd/ICD-LH-0.1-async-bridge-stress.md)
(async-bridge stress), and
[`docs/icd/ICD-LH-1-listen-notify-storm.md`](../docs/icd/ICD-LH-1-listen-notify-storm.md)
(realtime storm tier), plus the ROADMAP entry at
[`docs/ROADMAP.md`](../docs/ROADMAP.md) §Load Harness.

LH-0 (sync Tier 1 recursion via `lh0:1:chain`) + LH-0.1 (async-bridge
stress via `lh0:1:js_stress`) + LH-1 (storm tier via
`lh1storm:1:burst` + external PG LISTEN subscribers) are shipped.
LH-2 through LH-4 are gated on downstream kernel milestones (0.5.2,
0.5.4, 0.7.1 respectively) per ROADMAP.

## Build

```
make all        # binary at ./build/lh0 + fixture at ./fixtures/driver.zip
```

Requires Go 1.22+ and `zip` in `$PATH`. No CMake coupling; builds and
runs independently of the plinth kernel toolchain.

## Run

### Prereqs
- A running `plinth` kernel with PG backing (docker-compose postgres
  is fine for local testing; the tests fixture in `docker/docker-compose.yml`
  gives you PG on `localhost:5432`).
- An **admin user** seeded into that kernel. The simplest path:
  start plinth with `registration_enabled=false` in config, then
  `POST /api/auth/register` once — the first user is auto-granted
  `kernel.admin` via `ensure_first_user_admin`. See
  [`docs/architecture/01-identity.md`](../docs/architecture/01-identity.md).

### Easy tier (1 min, low concurrency, smoke)

```
./build/lh0 \
  --kernel=http://localhost:8080 \
  --username=admin --password=<admin-password> \
  --tier=easy
```

Prints progress every ~6 s; summary at exit includes p50/p95/p99
latency and error breakdown by kernel error code.

### Medium tier (5 min, moderate concurrency, diagnostic)

```
./build/lh0 \
  --kernel=http://localhost:8080 \
  --username=admin --password=<admin-password> \
  --tier=medium
```

Recommended companion commands (separate terminals):

```
tail -f <plinth-log> | grep -E 'free_zero_refcount|list_empty|bad_weak_ptr|SIGSEGV|SIGABRT'
while :; do ps -o rss,%cpu,pid -C plinth | tail -n +2; sleep 5; done
```

Reproducing any of the above signatures (on a kernel without the
0.4.4.1 deterministic-teardown bundle → validates the bundle holds;
reproducing it on a kernel WITH the bundle → confirms the kernel
itself has a real race, not just the test harness).

### Async tier (2 min, moderate concurrency, async-bridge diagnostic)

```
./build/lh0 \
  --kernel=http://localhost:8080 \
  --username=admin --password=<admin-password> \
  --tier=async
```

Drives `lh0:1:js_stress` instead of the sync `lh0:1:chain`. Each call
evaluates a fixed JS script that does 4 concurrent
`db.query('SELECT pg_sleep(0.01), ...')` and awaits `Promise.all` —
so per-call effective fan-out is ~4 `db.query` operations, and the
harness as a whole saturates the
`dispatch_async_op_detached → signal_completion → JS_ExecutePendingJob`
path the `async_hardening: parallel queries` ctest exercises. Kernel
must be running on a PG-backed instance.

Run with the same kernel-log tail as the medium tier. LH-0.1 goal:
either deterministically reproduce `free_zero_refcount` under
production lifecycle (feeds the fix PR) or confirm the production
kernel tolerates the path (redirects investigation to the test
strategy). See ICD-LH-0.1 §9.

### Storm tier (2 min, Layer-3 emit + external PG LISTEN diagnostic)

```
./build/lh0 \
  --kernel=http://localhost:8080 \
  --username=admin --password=<admin-password> \
  --tier=storm \
  --driver-zip=./fixtures/lh1storm.zip \
  --pg-dsn='postgresql://plinth:plinth@localhost:5432/plinth?sslmode=disable'
```

Installs the `lh1storm` driver extension, grants `lh1storm.burst` to
the admin group, dials 4 producer workers + 4 external `pq.Listener`
subscribers on `plinth:realtime`, and drives
`lh1storm:1:burst {count: 8, bytes: 512}` continuously for 120 s.
Each call fires `count` parallel `pubsub.publish` on
`plinth:ext:lh1storm:storm_event`, exercising the full v0.5.0
Layer-3 emit chain (extension-identity gate → regex validation →
envelope serialisation → `emit_notify_async` → PG `pg_notify`) +
the kernel-side `plinth::realtime::listener` dispatch path. The
external subscribers observe exactly what PG delivered, independent
of the kernel's in-process dispatch state.

Required companion shell (separate terminal):

```
tail -f <plinth-log> | grep -E \
  'free_zero_refcount|list_empty|bad_weak_ptr|SIGSEGV|SIGABRT|realtime\.'
```

Expect exactly one `realtime listener: subscribed to plinth:realtime`
and zero `realtime.notify.rejected` / `realtime.listener.reconnected`
events per run. Baseline exit-0 criteria per ICD-LH-1 §7.1: zero
worker errors, zero subscriber parse errors, observed/emitted ratio
≥ 0.99 per subscriber, p99 lag < 5 s.

Storm-tier overrides:

```
--subscribers N      # override subscriber count (default 4)
--burst-size K       # override per-call burst size (default 8)
--payload-bytes B    # override payload body length (default 512)
--pg-dsn <dsn>       # PG DSN for subscriber LISTEN (or $PLINTH_PG_DSN)
```

`--concurrency` acts as the producer worker count `M`.

**BurstSize default note:** the tier's default `--burst-size=8`
deviates from ICD-LH-1 §6.1's original 16. The kernel's
`default_runtime_limits().max_concurrent_async_ops` is 8, not 32 as
the ICD assumed; per-BridgeContext fan-out > 8 currently hits a
pre-existing async-bridge requeue spin in the outer coroutine loop
(tracked in `docs/CHANGELOG.md` LH-1 §Secondary finding +
`tests/kernel/js/async_hardening_test.cpp:151` comment). Override via
`--burst-size` only after the companion fan-out-requeue fix lands.

### Overrides

```
--concurrency 16   # worker count
--depth        10  # cap.call chain depth per call
--duration     10m # harness lifetime
--driver-zip   path/to/custom.zip   # skip default driver install
--keep-driver  # don't uninstall on exit (useful for kernel state inspection)
```

## What it exercises (and what it does not)

LH-0 drives these surfaces:
- `POST /api/auth/login` — once, at startup.
- `POST /api/packages` — once, at startup (installs
  `fixtures/driver.zip`).
- `GET /ws/events` + `{type:"auth",token:...}` — per worker.
- `{type:"call",signature:"lh0:1:chain",...}` — continuously.
- `DELETE /api/packages/{id}?confirm=true` — once, at exit (unless
  `--keep-driver`).

Under `--tier=async`, LH-0.1 additionally exercises:
- `{type:"call",signature:"lh0:1:js_stress",args:["<script>"]}` — continuously.
- The kernel's `run_on_context` coroutine loop + `dispatch_async_op_detached` +
  `signal_completion` + `JS_ExecutePendingJob` path — the same surface
  the `async_hardening: parallel queries` ctest reliably trips under
  the Catch2 subprocess lifecycle.
- `db.query` binding end-to-end (kernel JS stdlib → Drogon DbClient).

Under `--tier=storm`, LH-1 additionally exercises:
- Tier-2 extension capability dispatch
  (`plinth::extensions::runtime_registry` + `resolution.cpp` async
  extension arm + `call_dispatch.cpp` WS `co_await
  call_capability_async` migration, all shipped in 0.5.0.4).
- `pubsub.publish` JS binding + extension-identity gate + Layer-3
  channel regex validation + `emit_notify_async` → Drogon DbClient
  → `SELECT pg_notify('plinth:realtime', $envelope)`.
- Kernel-side `plinth::realtime::listener` (subscribes on startup
  and runs for the lifetime of the kernel; LH-1 produces NOTIFYs
  that the listener dispatches against its default no-op handler
  path, the consumer-registered path lands with 0.5.2 WS broker).
- External PG `LISTEN "plinth:realtime"` subscribers observe
  exactly what PG delivered (bypasses in-process dispatch — cleaner
  diagnostic).

What it **does not** exercise today:
- WS broker fan-out (LH-2 / 0.5.2). No client-side
  `pubsub.subscribe` binding yet; storm tier's subscribers are
  external `pq.Listener` connections.
- Delta-sync on reconnect (LH-3 / 0.5.4). No `plinth.events` table
  yet; storm tier has no replay protocol.
- Hard + crushing tiers + `plinth.metrics` integration (LH-4 /
  0.7.1). Storm-tier observability stays on `ps` / `top` / kernel
  stderr tailing per LH-0 / LH-0.1 convention.

## Directory layout

```
load-harness/
├── cmd/lh0/main.go           # flag parsing + orchestration
├── internal/
│   ├── httpclient/           # login + install + uninstall + list + RBAC grant
│   ├── wsclient/             # dial + auth + correlation-matched call
│   ├── pglisten/             # storm-tier external PG LISTEN subscriber
│   ├── tiers/                # easy + medium + async + storm profile definitions
│   └── observe/              # latency recorder + subscriber recorder + summary printer
├── fixtures/driver/          # LH-0 / LH-0.1 driver extension source tree
├── fixtures/lh1storm/        # LH-1 storm-tier driver extension source tree
│                             # (both zipped by `make fixtures`)
├── go.mod                    # github.com/gobha-me/plinth/load-harness module
├── Makefile
└── README.md
```
