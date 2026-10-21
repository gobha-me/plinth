# Drogon patches

Local unified-diff patches applied to the Drogon source tree after
`FetchContent_Declare(drogon ...)` populates it, before
`FetchContent_MakeAvailable` builds it. See the `PATCH_COMMAND` entry
in the top-level `CMakeLists.txt`.

Each patch is a minimal surgical change — we keep these small and
upstreamable. Breaking changes belong in a fork.

## Patches

### `pg-sqlstate-errors.patch`

Backports the three production-file changes from upstream
[`4d078eeab6805c82e8e109479bc52beda4c0a2db`](https://github.com/drogonframework/drogon/commit/4d078eeab6805c82e8e109479bc52beda4c0a2db)
("Report typed errors for PostgreSQL queries", #2616, Ughur Aghakishiyev)
to the pinned Drogon 1.9.12 commit for Plinth issue #92. The implementation is
unchanged from upstream; Plinth carries its own real-driver and registered-JS
regressions rather than importing upstream's standalone test executables.

Both PostgreSQL implementations copy the authoritative server SQLSTATE,
message and causal query into the existing SqlError hierarchy before freeing
the PGresult. A skipped pipeline command retains a separate generic Failure;
it does not inherit the earlier command's SQLSTATE. Result-less transport and
shutdown errors remain generic. No sender/queue-selector changes are included.
Plinth no longer synthesizes SQLSTATE from diagnostic prose.

The fix is also included in released Drogon 1.9.14. A full upgrade changes
97 commits plus Trantor and conflicts with the owned-shutdown patch in five
paths. This narrow backport retains both dependency pins and all four existing
safeguards without an unrelated lifecycle rebase. No license change is made.
The normal forward/reverse apply check fails closed on incompatible source;
review this patch AND the ownership patch when updating Drogon. Retire this
patch only after equivalent upstream behavior and all Plinth regressions pass
on the replacement dependency pair. Runtime autoBatch=false is not a substitute
for a separately compiled LIBPQ_BATCH_MODE=OFF regression run.

### `sql-binder-cancel.patch`

Adds a small `SqlBinder::cancel() noexcept` operation against the same pinned
Drogon source. Unexecuted binders normally execute in their destructor, even
if parameter or callback setup unwinds. Plinth's owned SQL awaiters use a
stack setup guard to abandon incomplete binders and explicitly execute only
after both callbacks are installed. Already-dispatched SQL is not cancelled.
Callback completion state remains separately owned and revocable if dispatch
throws. This is an internal native lifetime repair, not a new SDK cancellation
API; no change to the upstream version, source pin or license is made.

### `ftype-accessor.patch`

Added **0.5.3** (ICD-0.5.3-db-batch-silent-mode §OID-Driven PG-Type →
JS-Type Mapping).

Adds a public `drogon::orm::Field::oid()` accessor that delegates to
the already-public `drogon::orm::Result::oid(column)` via the existing
`friend class Field` relationship. Lets Plinth read the PG column OID
per-field during result conversion, replacing the 0.3.3 string-parse
heuristic (`"t"/"f"` → bool, integer parse → number, etc.) that
mis-classifies text rows like `SELECT 'true'::text`.

Three lines added to `orm_lib/inc/drogon/orm/Field.h` in the public
section. No .cpp change — the accessor is inline.

Will be filed upstream at <https://github.com/drogonframework/drogon>
on 0.5.3 ship. When merged, this patch can be retired.

### `connection-whitespace.patch`

Applied against the pinned Drogon 1.9.12 source commit
`89aca8c7993c8194f2c109c1d06a3b45bf363d5d` for Plinth issue #12.

Quote connection values containing any libpq ASCII whitespace, including
tabs and newlines. Drogon's existing quote/backslash escaping is preserved.
This covers the runtime pool created by `createDbClient`; that API and
`PostgresConfig` do not accept a prebuilt connection string. Plinth's direct
libpq connections use `kernel/db/connection_info.hpp` instead.

The production subprocess credential tests exercise fresh startup, restart,
frontend queries, and runtime pool authentication with whitespace, quotes,
and backslashes. The upstream version, source SHA, and MIT license remain
unchanged; builds include this documented local modification. The patch
has not been submitted upstream and must be reviewed when updating Drogon.

### `db-client-owned-shutdown.patch`

Applied against the pinned Drogon commit for Plinth issue #95. Drogon's
thread-pool `DbClientImpl` callbacks previously locked a weak client reference
before doing work. If a callback held the final strong reference, callback
return destroyed `EventLoopThreadPool` on its own `DbLoop` and
`std::thread::join()` aborted with `Resource deadlock avoided`.

The patch gives callbacks an independent shared shutdown token, replaces the
queued raw-`this` connection initializer and timeout captures with guarded weak
references, and adds an idempotent deadline-aware drain around connection
disconnect and destruction. Both PostgreSQL pipeline and non-pipeline
implementations settle buffered and active transaction operations with an
explicit shutdown failure, invalidate caller-held idle transactions, and clear
callback cycles before releasing the connection. SQLite and MySQL retain their
existing unbounded `closeAll()` behavior; finite `closeAllFor()` is fail-closed
outside PostgreSQL. A small Trantor extension requests private-loop stop, waits
within the same deadline, and joins each thread before the client reports success.
Plinth retains a durable client owner and calls the bounded drain from its
lifecycle owner before releasing that owner, so destruction remains off-loop.
The controlled loop cleanup signal is deadline-aware; the immediately following
standard `std::thread::join()` has no timed form and relies on the process
watchdog for its absolute bound. The patch must be reviewed and replayed when
updating Drogon; current upstream still has the same callback ownership shape.
The shutdown mutex uses `try_lock_for(deadline - steady_clock::now())` so the
same absolute bound also compiles under Clang 21 TSan with libstdc++ 14.
That libstdc++ release's `timed_mutex::try_lock_until(steady_clock)` path
instantiates a clock-lock overload disabled by TSan (GCC PR121496). The
relative call takes its supported fallback with the remaining steady-clock
budget, rather than resetting the configured timeout.
