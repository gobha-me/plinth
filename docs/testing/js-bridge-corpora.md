# Deterministic JavaScript bridge corpora

Issue #90 tracks bounded coverage of every exposed QuickJS-to-host boundary.
An ordinary integration test or a successful aggregate test run is not evidence
that every requested malformed-input and ownership target was exercised.
Missing, unbuildable, skipped or incomplete targets must remain explicit.

## Existing slices and remaining scope

| Slice | Named target | Boundary covered by that slice |
| --- | --- | --- |
| Crypto (#119) | `plinth_tests_crypto_corpus` | Hash, random bytes, equality and byte-view inputs |
| Log/config (#121) | `plinth_tests_log_corpus`, `plinth_tests_config_corpus` | Registered log callbacks and allowed configuration projection |
| Shared conversion (#124) | `plinth_tests_conversion_corpus` | Shared JS-to-JSON conversion and error classification |
| Synchronous host evaluation (#176, parent #90) | `plinth_tests_sync_conversion_corpus` | Actual one-shot and pooled synchronous evaluation, their distinct private result converters/classifiers, and owned runtime lifecycle |
| Reverse tagged JSON values (parent #90) | `plinth_tests_reverse_json_registry_values`, `plinth_tests_reverse_json_promise_values` | Distinct actual argument-injection and capability-result tag interpretation, literal fallback observations, buffer independence and fresh runtime ownership |
| Extension module import (parent #90) | `plinth_tests_module_import_corpus` | Actual staged handler loading, registered nested source import, compile/evaluate entry, namespace/default invocation and fresh-owner replacement replay |
| Audit (#133) | `plinth_tests_audit_corpus` | Audit admission, owned snapshots and simulated host settlement |
| Native audit (#191, parent #90) | `plinth_tests_native_audit_corpus`, `plinth_tests_native_audit_acknowledgement` | Actual registered audit dispatch, committed PostgreSQL rows and separately gated fire-and-forget acknowledgement before persistence |
| Pubsub (#148) | `plinth_tests_pubsub_corpus` | Publish/subscribe admission, callback replacement/invocation, returned unsubscribe enqueue and runtime ownership |
| Native pubsub (#189, parent #90) | `plinth_tests_native_pubsub_publish`, `plinth_tests_native_pubsub_delivery` | Actual PostgreSQL outbox acknowledgement and separately tested registered JS delivery/cleanup with an explicitly synthetic broker producer |
| Native pubsub checkout ownership | `plinth_tests_native_pubsub_owner`, `plinth_tests_native_pubsub_retirement` | Separately required actual owner-loop routing and queued checkout-retirement controls; not the full notification chain |
| Capability (#150) | `plinth_tests_cap_corpus` | Call/batch admission, owned snapshots, real Promise.all composition with simulated host settlement and runtime ownership |
| Native capability acceptance inventory (#187, parent #90) | `plinth_tests_native_capability_data`, `plinth_tests_native_capability_edges`, `plinth_tests_native_capability_errors`, `plinth_tests_native_capability_ordering`, `plinth_tests_native_capability_ownership`, `plinth_tests_native_capability_nested` | Real resolver/extension dispatch, native settlement, causal composition and separately required nested cancellation; registration is not delivery evidence |
| Ordinary database (#151) | `plinth_tests_db_admission_corpus` | Query/exec admission, parameter and client-manager snapshots, simulated host settlement and runtime ownership; no SQL execution |
| Public database batch (#174, parent #90) | `plinth_tests_db_batch_corpus` | Public callback admission and installed batch orchestration with simulated host completion/reset; no SQL execution |
| Native binary database parameters (parent #90) | `plinth_tests_native_bytea_query`, `plinth_tests_native_bytea_exec`, `plinth_tests_native_bytea_batch` | Real registered query/exec and installed batch dispatch, typed PostgreSQL bytes, durable storage, rollback and owned teardown |
| Authoritative PostgreSQL errors (#92) | `plinth_tests_sqlstate_native`, `plinth_tests_sqlstate_pipeline`, `plinth_tests_sqlstate_projection` | Real driver diagnostics/query ownership, deterministic causal errors versus skipped pipeline commands, and registered query/exec/batch error projection |
| Native PostgreSQL results (#185, parent #90) | `plinth_tests_native_pg_result_corpus` | Selected real scalar input/result kinds, returned BYTEA lifetime, ordered envelopes, server errors/recovery, affected rows and fresh-owner cleanup |

These are bounded slices, not comprehensive coverage claims. In particular,
simulated admission/settlement and actual native execution are separate evidence;
neither replaces the other. Later native capability, SQL, import and reverse
conversion tests extend the earlier slices rather than leaving those entire
surfaces untested. Their remaining branches and applicable ownership cases
still require explicit accounting. Parent #90 stays open while requested
coverage remains incomplete.

### Registered-entry inventory

The current production registration sites are `stdlib_inject.cpp`, its seven
`stdlib/*_bindings.cpp` registrars and the per-dispatch intrinsic installed by
`extensions/runtime_registry.cpp`. Counting each log level and the returned
unsubscribe callback gives **21 native entry identities**. The following is a
source/target inventory, not a claim that every input or schedule has passed.
Targets named in the slice table above retain their narrower documented scope.

| Native entries | Count | Existing bounded target ownership |
| --- | --- | --- |
| `log.debug`, `log.info`, `log.warn`, `log.error` | 4 | Log corpus, log regression and C-string allocation-failure regression |
| `config.get` | 1 | Config corpus, allowed projection and runtime ownership |
| `crypto.hash`, `crypto.randomBytes`, `crypto.timingSafeEqual` | 3 | Crypto corpus and numeric regression |
| `db.query`, `db.exec` | 2 | Ordinary admission plus separate native binary parameters, selected PostgreSQL results and SQLSTATE gates |
| `db.batch` | 1 | Public admission/orchestration plus separate native transactions, binary batch, timeout and cancellation regressions |
| `__db_batch_begin__`, `__db_batch_commit__`, `__db_batch_rollback__` | 3 | Valid installed orchestration is covered; a dedicated direct malformed-entry corpus remains incomplete |
| `audit.log` | 1 | Audit admission/authority plus native persistence and acknowledgement gates |
| `cap.call`, `cap.batch` | 2 | Capability admission plus native data, edges, errors, ordering, ownership and nested cancellation gates |
| `pubsub.publish`, `pubsub.subscribe`, returned unsubscribe callback | 3 | Pubsub admission plus native publish/delivery and checkout/retirement ownership gates |
| `import_from_src` | 1 | Module-source regression and actual module-import corpus |

The installed `__db_batch_orchestrator__` is an additional directly callable
JavaScript function, not a 22nd native callback. Its public sequencing has
coverage; direct malformed helper/orchestrator combinations remain separate
work. Existing helper scope parsing uses `JS_ToInt64` followed by a positivity
check. A future corpus must characterize that coercion and error precedence,
not infer a strict JavaScript-integer contract from an error message or turn
the internal globals into a newly promised SDK API.

`test_host_sleep` and test receipt observers are not production entries.
Reserved, unimplemented HTTP/storage operation variants are not registered
fuzz targets. They do not become missing exposed targets solely because an
enum or historical milestone mentions them.

### Distinct conversion and ownership coverage

The JS/JSON implementations in `conversion.cpp`, `eval.cpp`, `runtime_pool.cpp`,
`bridge_context.cpp` and the extension registry do not inherit exhaustive
coverage from one shared corpus. The shared and synchronous corpora exercise
their actual respective entry points. Reverse conversion has separate
`plinth_tests_reverse_json_registry_arguments` and
`plinth_tests_reverse_json_promise_settlement` regressions; native capability,
pubsub delivery and BYTEA tests additionally exercise real typed settlement.
These are finite observations, not proof of every reverse branch.

The remaining result/conversion ledger includes distinct native JSON/JSONB
and temporal string branches, unknown-OID fallback, disabled-type-flag
heuristics and finite numeric/column-name boundaries outside the selected
PostgreSQL result corpus. The [reverse tagged-value corpus](#reverse-tagged-json-values-and-ownership)
separately enumerates singleton BYTEA-tag interpretation and fallback; its
literal observations complement the native binary round trips. This is one bounded remainder, not a requirement to
implement every future PostgreSQL type or promote arrays to a new SDK type.

Runtime reset, cancellation and teardown are not wholly outstanding: existing
pool, native cleanup/setup-failure, capability cancellation/generation, pubsub
retirement and production coordinator tests provide route-specific coverage.
Each new entry/branch corpus still needs its applicable ownership cases and
actual sanitizer evidence. No combination of existing green tests proves
every cancellation schedule, authentication, a full broker/network journey,
or that a fire-and-forget acknowledgement proves persistence.

## Native audit persistence and acknowledgement

This pair extends rather than replaces #133's DB-free admission and simulated
settlement corpus. Both cases call actual RuntimePool-injected `audit.log`
through the native dispatcher with the existing PostgreSQL fixture and
production process shutdown order. They remain ordinary `[js]` cases, not new
isolated fixtures. Neither case may seal the process-global audit writer or
stop the shared Drogon fixture as a per-case cleanup shortcut.

The committed-row corpus requires 24 admitted native writes, with bounded fixed
inputs and fresh-owner replays. Independent PostgreSQL readback checks the
actual stored rows, payloads and trusted provenance against literal test
expectations; the production converter or audit detail decoder is not the
oracle. Existing namespace, reserved-field and denial checks remain separate
required observations, not extra successful writes. Missing/duplicate rows or
ledger cells and corrupted copies of passing observations must be rejected.
Fixture-selected user/session/IP, node, extension and call-depth profiles are
synthetic trusted setup identities. They do not prove HTTP authentication,
credential validation or permission assignment.

The 12 denial calls include eight with exact event-action zero-row readback.
The four non-string/empty-event calls instead prove the exact synchronous
TypeError and zero native admission; their cleanup sentinels are not event-row
receipts for inputs that have no valid event identity.

The acknowledgement case adds one admitted write, bringing the pair's required
native-write inventory to 25. A separately owned PostgreSQL control connection
causally blocks its INSERT while the JavaScript acknowledgement is observed;
independent storage observations distinguish that settlement from a committed
row. The gate is then released and actual audit persistence must drain before
durable readback and teardown. This exercises the current fire-and-forget
admission acknowledgement, not a persistence acknowledgement or a newly chosen
public contract for the Promise's resolved value. No simulated host settlement,
sleep-based readiness or manually completed audit callback substitutes for the
native path.

Every exit path must release the owned SQL gate before joining work or draining
its owners. Native/audit barriers, context retirement, empty callback and
operation ownership, and bounded idempotent RuntimePool shutdown precede final
assertions and deletion of exact task-owned audit rows. The production fixture
owns final process teardown; neither case pauses/seals global audit admission for later
grouped tests. Real database/setup/teardown failures fail rather than skip.

Each exact CTest requires the named single case, fresh positive Catch XML, zero
failures/expected failures/skips and a successful child exit after cleanup. The
inner/outer bounds are 60/75 seconds; both use `plinth_pg_schema` and `needs_pg`.
Both existing ASan/UBSan lanes and selected TSan discover exactly two and run
them verbosely and serially under the existing fatal sanitizer settings. All
prior audit, native ownership, lifecycle and full compiler/image gates remain
required. Wiring alone is not an execution or sanitizer pass.

```sh
ctest --test-dir "$audit_build_dir" --verbose --output-on-failure --parallel 1 \
  -R '^plinth_tests_native_audit_(corpus|acknowledgement)$'
```

This is finite native audit coverage, not exhaustive malformed-input,
allocation-failure, every cancellation/race schedule or HTTP authentication
coverage. Parent #90 remains open for its other requested boundaries. No
production policy, public API, permission, version, asset or release change is
made by this slice.

## Pubsub admission and callback ownership

The test calls actual RuntimePool-injected QuickJS functions. Expected argument
and admission results, operation snapshots and callback state are checked
independently; production authorization/conversion helpers are not the oracle.
Fixed seeds and finite byte/node/depth/job/shrink budgets make failures
reproducible. Harness budgets do not change production quotas or API limits.
Explicit semantic-family coverage and corrupted-oracle controls prevent an
empty or vacuous corpus from being treated as proof.

Publish coverage distinguishes synchronous argument/conversion failures,
inline Promise rejections, and admitted owned operation snapshots. Subscribe
coverage includes existing layer/extension/RBAC gates, quota admission and
replacement at quota. The replacement handler is actually invoked, rather
than inferred from successful subscription settlement. The actual unsubscribe
function is constructed, delivered through the existing host settlement seam
and invoked to observe its captured-channel operation.

Host settlement in this corpus is deliberately simulated. It is not proof of
broker delivery, PostgreSQL acknowledgement, permission rechecks at dispatch,
broker deregistration, production dispatcher result values, threaded work or
the production cancellation cascade. Fixture-selected host settlement values
do not establish a worker's return-value contract.
Existing integration and lifecycle tests remain separate required gates.
All JS values are freed while their context is alive; release, rebuild,
destruction and shutdown tests obey the owner/lease protocol and never use a
destroyed context pointer.

## Isolation and fail-closed execution

Pubsub denials can enter the audit path. The corpus therefore runs in a fresh
DB-free process without initialized logging, Drogon or broker background work,
and is tagged `[isolated-pubsub]`. It is excluded only from the shared JS
fixture process, not the full test suite. The independent isolation inventory
requires exactly five known logger fixtures, this one exact pubsub case and
the one exact capability corpus, the one exact ordinary DB admission corpus,
the one exact public DB batch corpus, the synchronous conversion corpus and
the existing isolated audit-drain fixture,
preserves all other ordinary JS fixtures and pins the required strict gate.

`plinth_tests_pubsub_corpus` uses `tests/tools/verify_catch_case.py` with the
exact named case, a fresh task-owned XML report, positive assertions, zero
skips/failures, and a successful process exit after teardown. Its inner bound
is 60 seconds and outer CTest bound is 75 seconds. Both existing ASan/UBSan
lanes explicitly discover exactly one target and execute it; ordinary compiler
and server/CI-image suites execute the complete CTest inventory.

```sh
ctest --test-dir "$pubsub_build_dir" --output-on-failure --parallel 1 \
  -R '^plinth_tests_pubsub_corpus$'
```

Select a freshly configured build of the current source. An old binary,
reused success XML, console summary, zero discovered cases or pending CI is
not coverage. Candidate and terminal exact-merge CI remain delivery gates;
corpus success is not an authorization to publish a release or a vulnerability.

## Native pubsub acknowledgement and registered delivery

The original publish/delivery pair extends, rather than replaces, #148's
DB-free admission corpus. Both call actual RuntimePool-injected functions through the coroutine
dispatcher, retain ordinary `[js]` group inclusion, and run in separate strict
processes with the existing PostgreSQL fixture and production shutdown order.
They do not use the `[isolated-pubsub]` admission-only tag.

The publish ledger requires six supported payload families, two fixed seeds
and two fresh-owner replays:24 actual publications. Literal expectations cover
null, boolean, safe integer, valid Unicode string, Unicode-key object and
bounded nested containers. An independent libpq lookup keyed by a unique
channel requires exactly one committed outbox row with the exact layer,
channel and payload value. String/member-name code units are checked without
making JSON object serialization order an oracle. Successful publication
checks the actual native acknowledgement, not the binding comment's return
type or a truthiness approximation. NUL and lone-surrogate JSONB publication
are not claimed as supported successful values.

The delivery ledger has four scenarios replayed under two fresh owners:
quota-one callback replacement, actual unsubscribe after observed delivery,
context destruction after queued delivery settles, and extension drain.
A joined worker enters the existing `broker::dispatch_for_test` seam, and
owner-loop observations check actual callback identity, payload and count
separately from broker registry matches. The producer is synthetic: it skips
listener parsing and the events-writer INSERT. A matching registration alone
is not proof that its JavaScript callback executed. Extension drain removes
broker registrations; context destruction still owns persistent JS callback
cleanup. Quota/RBAC configuration must be restored for later grouped cases.

Corrupted copies of actual passing observations and missing/duplicate replay
entries must fail the test-only validators. Owners and captured state survive
until queued work settles; native/audit drains, context retirement, zero live
registrations and bounded idempotent pool shutdown remain acceptance gates.
Each exact CTest uses fresh positive Catch XML with zero skips/failures, a
60-second inner bound,75-second outer bound and `plinth_pg_schema` lock.
Both ASan/UBSan lanes and selected TSan explicitly discover exactly four and
execute them serially with existing fatal sanitizer options.

```sh
ctest --test-dir "$pubsub_build_dir" --verbose --output-on-failure --parallel 1 \
  -R '^plinth_tests_native_pubsub_(publish|delivery|owner|retirement)$'
```

These gates do not establish the full listener-to-writer-to-broker chain,
delivery-time revocation, production cancellation races, every malformed
payload policy, or complete parent #90 coverage. An outbox acknowledgement
does not prove listener delivery, and synthetic dispatch does not prove a
PostgreSQL publication caused delivery. Those targets remain incomplete.

The separate ownership cases use bounded causal loop gates and actual native
subscriptions. A default-off C++ observer can safely deny a wrong-thread
invocation before JavaScript access; the routing oracle additionally requires
the real callback receipt on its actual owner. Retirement coverage is separate
from the original delivery corpus's fully settled destruction journey: queued
destruction, same-address checkout reuse, and returning a context during a
positively held actual callback. The last cell requires checked-out0/pending1,
refused zero-budget shutdown, then pending0 after the callback, queued cleanup
and native drain really complete. These
checks do not establish exploitation, a deployed affected caller, every possible
schedule, or a listener-to-writer delivery guarantee. The owning shutdown
protocol is documented in [shutdown ownership](../architecture/shutdown.md).

## Authoritative PostgreSQL diagnostics

The SQLSTATE gates use six literal server errors: syntax42601, relation42P01,
column42703, division22012, unique23505, and an unmapped P1234 whose message
deliberately resembles syntax/relation errors and a SQLSTATE42601 marker.
An independent libpq connection establishes each native state for identical
SQL and parameters. Twelve public-client error observations retain the actual
exception beyond callback and bounded client close, requiring the exact state
and original query; fourteen readiness/recovery controls use the same backend
within each client epoch. Runtime autoBatch flags are not two compiled drivers.

The pipeline case owns four fresh connection/cache epochs: cold preparation
and warmed parameterized execution, each repeated twice. One atomic two-command
submission identifies the cause and a cached skipped follower. The cause must
retain its own diagnostics; the follower must remain a distinct generic Failure
without inherited SQLSTATE/query. Exact callbacks, actual idle after consumed
Sync, same-connection recovery and disconnect-before-loop-join are required.
These four epochs do not establish every possible network/backpressure or mixed
pending/waiting-queue schedule. The repair changes no sender or queue selector.

Registered bindings run two fresh runtime epochs: six errors through both query
and exec (24 observations), plus two real transaction failures per epoch (four
batch observations). They require own string error fields, literal code/state,
benign recovery, independent durable rollback readback, native/audit drain,
context retirement and destruction, cleared callbacks/pins/timers, and bounded
idempotent pool shutdown. They use the existing `[batch-transaction]` isolation
contract and a dedicated strict process with PLINTH_TEST_DB_POOL_SIZE=1; they
must not inherit a previously initialized larger pool.
The independent control has explicit three-second libpq/server limits. The
registered client keeps the shared fixture's existing connection/GUC defaults:
its per-context three-second JS cancellation is not a three-second server SQL
deadline. Native/audit drains are bounded separately; the strict subprocess
watchdog covers uncooperative native work and teardown and must fail, not pass,
when it fires. The 45-second admission deadline starts before fixture setup.

Missing PostgreSQL, a skipped case, incomplete observations or failed teardown
fails the strict gate. Pipeline builds discover all three named CTests. A fresh
LIBPQ_BATCH_MODE=OFF build instead compiles PgConnection.cc, not
PgBatchConnection.cc, and runs the two applicable native/projection gates;
the pipeline-only case is not registered. Actual on-loop PQpipelineStatus is
checked against that compiled route. The Clang21 compiler CI lane requires this
second configuration and its compile/archive inventory. Both ASan/UBSan lanes
and selected TSan explicitly discover and execute all three pipeline gates;
the complete ordinary suite retains every existing gate, including binary
parameter ownership and shutdown regressions.

These are #92's focused error-contract gates, not the remaining #90 native
result corpus. They do not replace its required168 cells, prove every error
policy, or authorize publication. Actual assertion/test/TU counts must be read
from fresh built inventories and reports, not inferred from this description.

## Native PostgreSQL result and error corpus

This test-only slice follows actual RuntimePool-injected `db.query`/`db.exec`
through the native dispatcher, real PostgreSQL, result/OID conversion, Promise
settlement and final owned observations. It does not fabricate driver fields,
call production converters as an oracle, or simulate host settlement. Fixed
SQL programs and independent literal expected values remain separate.

The required ledger contains168 primary cells:16 result families times four
fixed seeds times two fresh owner epochs, four server-error families times
four seeds times two epochs, and affected-row counts0/1/2/3 times two epochs.
Eight independent Config/RuntimePool owners use a fresh context per cell.
The selected families cover SQL NULL, BOOL, INT2/INT4, safe-number and outer
decimal-string INT8 results, exactly representable finite FLOAT4/FLOAT8/NUMERIC,
TEXT/VARCHAR/NAME/internal `"char"`/UUID, copied BYTEA results and ordered0–3-row
envelopes. The four required error pairs are syntax42601, missing-relation42P01,
missing-column42703 and division22012, with literal expected binding codes and
one benign recovery on the same live context after each rejection.

Native null/boolean/number/string parameters are retained where admitted.
Narrow integer result programs declare the documented BIGINT input type before
casting to SMALLINT/INTEGER; fractional inputs declare DOUBLE PRECISION before
requesting a REAL result. This checks those explicit input/result programs,
not arbitrary server type inference or direct-to-narrow-column insertion.
Outer INT8, NUMERIC, BYTEA output and ordered-envelope families deliberately use
literal SQL; they do not replace the real parameter families or prove BYTEA
input binding. Separate binary-input and authoritative-error gates remain.

Exactly216 registered native operations comprise168 primary calls,32 error
recovery queries, eight returned-buffer follow-up queries and eight affected-row
readback queries (208 query, eight exec). Independent libpq fixture setup,
acknowledged preseed/count checks and durable-empty readbacks are separate
control operations. The affected-row family uses documented `silent:true` to
isolate this result contract from realtime emission; real silent-use audit work
must drain. It is not proof of default realtime delivery. Missing-relation
names have independent absence checks; no pre-existing object is replaced.

Oracles require actual JS kinds, exact scalar values, complete UTF-16 code
units/lengths and byte arrays, own-key sets, row order/counts and owned string
error fields without pinning server diagnostic prose. BYTEA retains its first
array through a second real query and a local mutation of a separate sentinel.
Corrupted copies of passing observations and dropped/duplicated/invalid ledger
identities must fail their test-only validators. Real contexts, owners and
fixture cleanup complete before these oracle/control assertions.

Live pre-destroy observations include execution retirement, queues, callbacks,
concurrent/detached counts, batch depth/scope/pin/rollback/timer fields and real
native/audit drains. Actual context destruction and zero pool-active counts
must be observed, not inferred from a destroy call. Every owner requires bounded
idempotent shutdown and zero active/free counts. Uncooperative teardown fails
closed; a strict process watchdog is not a successful cleanup result.

`plinth_tests_native_pg_result_corpus` selects one exact positive-assertion,
zero-skip Catch/XML case with PostgreSQL schema locking. Both ASan/UBSan lanes
and selected TSan explicitly discover exactly one target and execute it
verbosely with fatal sanitizer settings. Existing grouped PostgreSQL/WebSocket
and full compiler/image gates remain required. Runtime and assertion counts
must come from fresh executions, not these required matrix counts.

This finite slice does not complete #90. JSON/JSONB, nonfinite/rounding/precision
and endpoint policy, time rendering, exotic names/OIDs, arrays/domains, broader
permission/cancellation/inference behavior, installed extension authority and
other bridge surfaces remain outstanding. It changes no production policy,
API, permission, version or release promise.

## Capability admission and batch composition

The capability corpus uses registered `cap.call` and `cap.batch` bindings with
fixed seeds, independent argument/admission/operation expectations and finite
payload, batch, callback-job and shrink budgets. It checks default arguments,
validation/cancellation precedence, owned caller/depth/signature/argument
snapshots and finite error mapping. Production conversion, resolver or mapper
helpers do not compute expected outcomes.

The runtime's unmodified `Promise.all` is exercised with explicitly simulated
host completions: reverse completion must retain input order, and the first
rejection in time wins while remaining callbacks retain owners until settled.
Later invalid tuples preserve the already admitted prefix, matching existing
incremental, non-atomic expansion; this does not promise rollback. Nonempty
signature bytes are tested at admission, not accepted by a resolver oracle.

This slice does not execute the resolver, authorize a real capability, enforce
dispatch recursion/backpressure, run a worker, or exercise the production
cancellation cascade. Exotic getter/proxy/replaceable-global/allocation-failure
policies and alternate conversions remain outside its bounded scope. Valid
runtime leases and JS values obey the owner/reset/shutdown protocol; no value
survives its context or is used after destruction.

`plinth_tests_cap_corpus` has an exact-case fresh Catch/XML gate with positive
assertions, zero failures/skips, a zero child exit, and 60/75-second inner/outer
deadlines. `[isolated-cap]` separates it from the grouped dispatcher/DB fixture,
not from full CTest. The independent known-case inventory requires exactly
five logger fixtures, one pubsub case, one capability case, one ordinary DB
admission case, one public DB batch case, one synchronous conversion case and
the audit-drain fixture, retaining every
ordinary JS fixture. Both sanitizer lanes explicitly discover and execute this
gate; compiler and image suites keep the complete CTest inventory.

## Native capability round-trips and owned cancellation

#187 follows the actual injected `cap.call` and unmodified `cap.batch` through
resolver/RBAC, staged extension handlers, native asynchronous completion and
caller Promise settlement. This inventory describes required observations,
not a claim that the issue, all six gates or their delivery CI have passed.
The admission-only slice above remains a separate required gate.

The primary ledger is frozen independently from boundary controls, warmups,
child operations and cleanup. Both call entries and two fresh-owner replays
are explicit dimensions; O/L use two fixed variants rather than four seeds.

| Primary family | Dimensions | Required observations |
| --- | --- | --- |
| R00–R07: null, boolean, integer, fraction, string, member names, arrays and trees | 8 families × 4 seeds × 2 entries × 2 replays | 128 |
| R08–R09: byte and conversion-depth edges | 2 families × 4 seeds × 2 entries × 2 replays | 32 |
| E00–E07: resolver, authorization, depth and handler errors | 8 families × 2 seeds × 2 entries × 2 replays | 64 |
| O00–O03: batch ordering, rejection, incremental admission and fanout | 4 families × 2 variants × 2 replays | 16 |
| L00–L03: release/reuse, held rebuild, pre-cancel and blocked-work retirement | 4 families × 2 entries × 2 replays | 16 |
| Primary total, excluding all controls and SC7 | | 256 |

Fourteen additional real boundary controls distinguish raw caller conversion,
handler wrapping and a shallow depth control. They bring the five non-nested
cases to 270 observations, not 274: SC7's four nested-cancellation observations
are a separate required ledger (two entries × two fresh-provider replays).
Neither a count of 270 nor successful caller retirement substitutes for SC7.

Literal typed descriptors and independently authored JS programs retain actual
JS kinds, exact finite/safe-number values, UTF-16 value/member-name code units,
array order and complete own-key sets. Reordered object entries remain valid;
duplicate/missing keys, changed code units and type/value mutations do not.
Oracles never invoke production converters or use their output as policy.
R08 uses an explicit fixture quota and independent 63/64/65/64-byte expectations;
R09 distinguishes input admission, handler-result conversion and outer caller
conversion at depths 63/64/65. Fixture quotas do not change production limits.
Native rejection codes, ordinary-object identity, own error fields and exact
messages or independently bounded handler prefixes remain distinct from
synchronous `TypeError` and final host-evaluation classification.

The data case requires 192 durable handler-entry writes (128 target and 64 peer),
not 128 native operations. R08/R09 and their extra controls require 64 writes;
the error case requires 36, including admitted peers. These 292 writes are
entry acknowledgements, not a count of successful final results. Independent
libpq setup/readback/lock operations are not injected capability or DB calls.

The corresponding R/E caller CAP-registration counts are 192/64/96, totaling
352, including the 14 edge controls' 22 registrations and 22 entry writes.
Excluding those controls, R/E account for 330 registrations and 270 writes.
O/L separately require 86 CAP registrations, 142 native DB operations and 86
entry writes (O: 52/96/52; L: 34/46/34). These counts already include 12 O
ACK/barrier support calls and 16 L recovery calls; do not add them again.
Primary target/prefix/fanout calls alone account for 58 of those registrations.
Four registry warmup epochs add 16 CAP calls, 16 native INSERTs and 16 entry
writes separately. Four L03 replacement-generation pure-JS checks add no CAP or
DB operation. O/L observers require 52 caller and 86 callee generations, each
with one ENTER and one FINISH (276 stage callbacks), excluding warmup.
Polling, snapshots, provisioning, independent control SQL and audit persistence
have no fixed query-count claim.

O/L preserve distinct handler entry, physical SQL waiter, host delivery,
JS catch acknowledgement and final return observations. Reverse completion
cannot change input-order results; first rejection in time cannot become the
first input-index rejection. A later invalid tuple must preserve actual
execution of its admitted prefix, not promise atomic rollback. Fanout checks
actual queued callbacks, active operations and successive released SQL gates.
Release/reuse and held rebuild require old/new generation identity and pristine
context state; pre-cancel requires zero primary admission. L03 observes a
retired caller while accepted native SQL is still physically blocked, then
acknowledged unlock, native/audit drain and fresh-generation recovery. SC7
separately requires caller-only cancellation to reach the distinct owned callee,
its actual rejection and final caller/callee classifications while preserving
issued-SQL ownership. No direct child cancellation can satisfy that obligation.

Observers retain copied metadata and weak generation identities, not task
leases or contexts. Owner-loop snapshots and observer closure are fenced.
Restricted extension DB identities and owned SQL gates use existing provisioned
authority, with acknowledged unlock before join on every unwind path. Every
caller requires execution retirement, cleared queues/callbacks/batch pins and
timers, actual context destruction, bounded idempotent pool shutdown and zero
remaining pool owners. L03 permits bounded abandoned inflight work at caller
retirement, but still requires its separately observed final native/audit drain.
R/E check 17 cleanup fields; O/L check pristine context state, physical drains
and the four shutdown fields separately. Provisioned-provider cleanup in the
five non-nested cases verifies registry/client shutdown, schema/role/credential
removal and task-file removal. SC7 instead removes its owned receipt relation
and temporary files while preserving the existing provisioned DB identity.

Copied positive observations are independently corrupted after owner teardown.
The data case pins 1316 value/type/entry controls, 2176 cleanup-field controls,
512 identity controls and 128 accepted object-order permutations. Its separate
six row-ledger, eight shutdown, four owner-finish/receipt and two owner-ledger
negative controls bring the rejection-control count to 4024; object-order
permutations remain positive controls. The edge and error cases pin 432 and 640
copied negative controls respectively, plus six inventory negatives each.
Each O/L case pins 160 copied field controls and six ledger/epoch negatives.
Two helper-only checks per O/L case reject a premature poll-count ceiling and
acceptance after an expired wall budget; they are not additional native
operations or primary cells.

Each of the six named CTests uses exact-case fresh positive/no-skip Catch XML,
a successful post-teardown process exit, a 60-second process bound, a 75-second
CTest bound and the PostgreSQL schema lock. Missing PostgreSQL, missing cells,
unknown settlement, failed drain or watchdog termination fails closed. Both
ASan/UBSan lanes and selected TSan explicitly discover exactly six and execute
all six with fatal sanitizer settings. The ordinary grouped JS tests and full
PostgreSQL/WebSocket/compiler/image gates remain required. Five selected passes,
discovery-only output or an old report cannot establish six-case/full-suite
success. Candidate and fresh exact-merge CI remain delivery gates.

This finite slice does not close parent #90 or establish new API, grant,
atomic-batch, cancellation-preemption or result-normalization policy. Broader
transport/layer/scope combinations and the other bridge boundaries remain
separate work. Registration and documentation do not authorize public security
disclosure, a release or an asset/version change.

## Ordinary database admission and ownership

The database corpus calls the registered ordinary `db.query` and `db.exec`
bindings without opening a database connection or dispatching SQL. Finite
literal argument/error expectations and operation/callback snapshots are
independent of production parameter/conversion helpers. It checks defaults,
bounded plain inputs and byte views, validation/cancellation ordering, exec
options and owned SQL/parameter/extension metadata. A real lazy database-client
manager alias is observed without calling it to obtain a connection.

A separate 27-input numeric matrix calls both registered bindings in normal,
cancelled and unavailable-context phases, with two independent owner replays:
324 calls across 12 owned contexts. Literal expectations check integer tags,
floating negative zero, fractions, inclusive safe-integer endpoints and their
outer neighbours, int64 edges, finite extrema, NaN and infinities. Input tags
and sign bits, stored JSON types and values, guard outcomes and complete
between-call settlement are checked directly, without stringification or the
production converter as oracle. Each context owns its 27 sequential calls;
no JS value or operation survives its lease. This proves numeric admission
mapping, not how a database driver accepts or persists nonfinite parameters.

Results and rejections are deliberately settled through the host seam as a
simulation. This does not prove driver parameter binding, SQL authorization,
PostgreSQL result/OID conversion, SQLSTATE fidelity, durable commits, production
dispatch/backpressure or the production cancellation cascade. Public batch
orchestration has the separate bounded slice below; broader transaction
dispatch and denial ownership remain separate targets. Exotic accessors/proxies,
allocation failure and broader coercion
policy are outside this bounded slice; no production limits or policies change.

Fresh replays, explicit family counts, corrupted-observation controls and bounded
reduction prevent vacuous success. JS values are freed before lease changes;
clean/dirty/cancelled release, held-owner rebuild and bounded shutdown preserve
real ownership without fake transactions or dangling-context calls.

`plinth_tests_db_admission_corpus` requires its exact case, fresh Catch/XML,
positive assertions, zero failures/expected failures/skips and a zero child exit,
with 60/75-second inner/outer deadlines. `[isolated-db-admission]` isolates it
from the grouped DB/dispatcher fixture, not from full CTest. The independent
inventory keeps exactly five logger fixtures and one each of pubsub, capability,
ordinary DB admission, public DB batch, synchronous conversion and audit drain, preserving every ordinary
JS fixture. Both sanitizer
lanes explicitly discover and execute it; full compiler/image suites remain
required. Coverage is delivered only after candidate and exact-merge CI pass.

## Public database batch admission and installed orchestration

The batch corpus calls the actual registered `db.batch` and its unmodified
installed JavaScript orchestrator. Fixed seeds, explicit semantic families,
two fresh-runtime replays per generated case, finite input/job/shrink budgets
and corrupted-observation controls prevent a missing or vacuous target from
being accepted. Expected public argument and error precedence, operation
sequence, owned metadata, callback inventory and result/error identity come
from literal test expectations rather than production conversion helpers.

The public admission matrix distinguishes synchronous invalid-callback and
unavailable-context errors from cancellation and nesting Promise rejections.
Orchestration covers BEGIN failure, immediate and Promise body results,
synchronous throws and rejected bodies, COMMIT success/errors, timeout-triggered
rollback, and both rollback outcomes. Observations precede simulated settlement
and check body/outer-settlement counts. Duplicate late completions cannot
settle an outer result again. The transaction and rollback-owner control blocks
are observed before and after their transfer into a terminal operation; the
alias-null transaction token is never dispatched or invoked as a database.

Host completions and terminal `batch_state` cleanup are deliberately simulated.
Pool release/reuse checks occur only after that explicit fixture cleanup;
they do not establish that clean release itself resets a transaction. Dirty
release, held-owner rebuild and bounded/idempotent shutdown keep all JS values
within a live owned context. This is not proof of real BEGIN/COMMIT/ROLLBACK,
driver/result conversion, authorization, timers, transaction durability or
the production cancellation cascade. Direct internal-helper malformed-entry
coverage, exotic accessors/global replacements and the remaining parent #90
inventory stay separate; no production policy or limit changes. Later native
transaction/cancellation regressions do not turn this simulated corpus into
native execution evidence.

`plinth_tests_db_batch_corpus` requires exactly its named case, fresh Catch/XML,
positive assertions, zero failures/expected failures/skips and successful child
teardown, with 60/75-second inner/outer bounds. `[isolated-db-batch]` keeps it
DB-free and separate from grouped DB/dispatcher fixtures, not out of full
CTest. Independent targeted and baseline discovery require its one exact
case/tag and the strict gate while retaining every ordinary fixture. Both
ASan/UBSan lanes and the TSan lane explicitly discover and execute this target;
the unchanged retained 16 and generation/installed/admin 43 TSan selections
remain separate. Normal compiler and image suites keep the full inventory.
Candidate and exact-merge CI remain delivery gates, not release authority.

## One-shot and pooled synchronous evaluation

The synchronous corpus calls the actual `plinth::js::eval` and
`eval_on_context` entry points. Their private conversion and error-classification
implementations are distinct from the shared helpers covered by #124. One exact
Catch case exercises both APIs, with 32 semantic families, four fixed seeds and
two replays per entry point: 256 primary observations each, 512 combined.
Independent coverage assertions require exactly eight observations in every
entry-point/family cell; aggregate success cannot hide a missing API or family.

Finite source, value/key byte, size, nesting, type, numeric and property-failure
cases use literal expected JSON values, bytes and error kinds. Production
converters/classifiers, result stringification and one API's output do not
compute the other API's oracle. Corrupted-observation controls exercise value,
type, byte length, key identity, nesting, classification and coverage failures.
Bounded reduction preserves the API, semantic family and failure signature.
The source cap is 16,384 bytes. Ordinary trees have at most 63 nodes, depth
four and width eight; strings have at most 4,096 decoded bytes. Dedicated depth
boundaries reach at most 66 edges and 67 nodes. Generator budgets bound the
harness; they add no production limit or policy.

Supported result shapes and existing rejection boundaries are checked separately
from explicitly labelled implementation characterization. In particular,
synthetic exception names/messages exercise classifier vocabulary and are not
evidence that an allocation or stack guard fired. Parser failures, uncaught
runtime exceptions and host conversion failures retain separate expectations.
The two APIs need not return identical diagnostics or classify every exception
identically. Exotic-object and coercion policies are not expanded by this test.

Numeric expectations distinguish the pinned runtime's internal representation
from mathematical integrality. The int32 family produces its minimum through
`(-2147483647 - 1)`, which yields an integer-tagged QuickJS value. Separate
representation checks retain the original `-2147483648` literal: QuickJS parses
the positive operand outside int32 range and preserves its floating tag when
applying unary minus, so these converter copies return JsonCpp `realValue`.
Both forms must preserve exactly the same numeric value. Two fresh replays of
both forms through both APIs add eight representation observations outside
the 512 primary observations. Eight paired corruptions replace each result
with the other JSON numeric type while retaining its value; the existing
numeric-value mutation controls remain required. These checks characterize
the pinned representation; they do not promise integer normalization for all
mathematically integral JavaScript numbers.

All generated JavaScript is finite, including getters and proxy traps. The
standalone helper owns and destroys a fresh runtime on each call and has no CPU
or wall-clock interrupt budget. Follow-up successful calls exercise that real
teardown boundary. Pooled evaluation uses real leases: success can release and
reuse a context after its enumerable globals reset, while failures destroy the
context before a clean acquisition. Rebuild preserves a checked-out owner's
state while replacing free contexts; a separate check rebuilds after all
leases drain. Bounded shutdown checks refusal with a held owner, retry after owner release,
refused acquisition after shutdown and idempotence. No JS value or pointer is
used after its owner is destroyed. A finite pre-cancelled pooled computation
checks interrupt classification against the same uncancelled computation;
it does not test the production cancellation cascade.

`plinth_tests_sync_conversion_corpus` uses the exact named case, fresh Catch/XML,
positive assertions, zero failures/expected failures/skips, successful child
teardown and unchanged 60/75-second inner/outer deadlines. The independent
inventory requires exactly its known case and semantic/isolation tags, validates
the complete strict gate and its working directory, and excludes only that
one case from the grouped JS process. The original ten named exclusions and
all ordinary fixtures remain present. The shared conversion gate keeps its
90-second deadline and its existing selector with only
`~[isolated-sync-conversion]` appended, so it cannot duplicate the new case.
Unknown matching execution gates and invalid exemption metadata fail closed.

Both ASan/UBSan lanes explicitly discover one target and execute it verbosely.
The TSan lane adds this one target alongside the retained 16 focused lifecycle
gates, one DB batch gate and 43 generation/installed/admin selections: 61
selected gates, rather than the full configured inventory. This synchronous
slice does not claim production threaded dispatch or cancellation coverage.
Actual sanitizer and CI outcomes must come from the candidate and exact-merge
runs; configuration or self-tests alone do not establish those outcomes.

Parent #90 stays open. This synchronous slice does not itself exercise extension
import, reverse conversion, SQL/driver execution, capability dispatch or native
cancellation; their later named gates retain their own finite coverage. The
current remaining entry/branch ledger is above, not a blanket assertion that
those surfaces are untested. This work does not authorize a version bump,
release or publication.

## Extension module import

`Extension module imports have a bounded deterministic corpus` is a real
PostgreSQL/Drogon extension-dispatch test, not a simulated settlement or a
renamed source-gate regression. It stages actual handlers through `ExtScratch`,
calls the registry dispatcher and registered `import_from_src`, and observes the
existing trusted C++ seam at both actual compile and evaluate calls. Literal
typed JSON, error names, seeded error markers and stage counts are independent
oracles; production scanners, converters and one row's output are not oracles.
Parser-generated SyntaxError and TypeError prose is deliberately not pinned.

Four fixed seeds vary literal values, module ids and finite comment placement.
Two fresh-owner replay epochs execute every selected row: host-original
precedence H has 32 observations, nested gating N has 32, compile/evaluate C has
24, namespace/default D has 24, and replacement/fresh-owner R has 12, for 124
actual dispatches. Every seed/replay/entry ledger cell is required exactly once;
separate family and total assertions cannot be satisfied by dropping a row.
Actual observations first satisfy the clean oracle, then 124 wrong-target-stage,
80 wrong-outer-stage and 124 wrong-result corruptions must fail it.
Selected compile-family observations additionally reject eight wrong literal
values, eight same-value/wrong-JSON-type results, eight wrong error names and
eight wrong seeded error markers. Each corruption follows a passing unmodified
actual observation and has a separate required count.
Dropping each of the 124 ledger cells
must also fail the coverage validator.

H executes a clean handler, a 50-marker replacement, a clean restoration with
a different literal return value and
one fixed malformed UTF-8 form, repeated for each seed. Zero target engine
entries, not inspection of a released context, prove pre-entry denial. N uses
a valid 49-marker source without NUL, then three 50-marker denials: ordinary,
markers before NUL, and markers after NUL with invalid syntax. This sparse
matrix does not assert parser acceptance of a 49-marker NUL-containing source.
Denied nested rows also require complete literal diagnostics, including the
first finding's UTF-8 byte offset and `variation-selector` range. Expected
offsets are independent literal tables for the fixed inputs, not scanner
results or values copied from an observed failure.
C separates success, syntax failure before evaluation, and a seeded module-body
throw. D checks missing/noncallable defaults and a throwing callable default,
including literal body/default-call counters. R uses seeds 0 and 3: seed 0's A
is denied, while seed 3's A successfully returns literal `{value:901,prior:1}`
after setting its global marker. Replacement B at the same handler/id must
return its own literal value with `prior:0`, then B is replayed on a new owner.
Thus the absent-A-marker oracle follows an actually observed successful marker,
not only a source that failed before its body. Compile-family syntax rows remain
separate.

Source is capped at 4,096 bytes and module ids at 128 bytes; programs have no
unbounded loops, getters, proxy traps, intentionally unresolved promises or
unbounded pending work. The actual async loader uses bounded microtasks drained
by the existing runtime, with no work retained across owner boundaries.
Eight seed/replay owners
plus four fresh R owners each explicitly require bounded successful registry
shutdown, zero in-flight dispatches and an idempotent second shutdown. No JS
value, context or Promise is retained across an owner boundary. A steady-clock
60-second deadline is checked between observations and shutdowns; exceeding it
fails rather than skipping a row. These are harness budgets, not new production
limits. There is no fuzz loop or reducer in this finite matrix.

`plinth_tests_module_import_corpus` requires real PG availability, the one exact
Catch case, fresh strict XML, positive assertions, zero failures/skips and a
successful child exit. The inner/outer bounds remain 60/75 seconds; the gate
uses the PG schema resource lock and `needs_pg` provisioner. Both ASan/UBSan
lanes and TSan explicitly discover exactly one gate and run it verbosely;
ordinary PG aggregation and full compiler/image inventories retain the case.
The pre-existing module-source regression remains a separate required gate.
Actual sanitizer/runtime outcomes require execution evidence, not this wiring.

This slice does not prove HTTP installation, package authorization, external
module resolution, arbitrary coercion/exotic-object policy, allocator exhaustion,
production cancellation or unbounded async behavior. Native dispatch, SQL and
reverse-conversion tests elsewhere retain their separate finite evidence; they
are neither execution credit for this import corpus nor wholly absent. The
remaining entry/branch ledger is above. SDK/HTTP journeys are separate from
this corpus. Parent #90 stays open; no production policy, public API,
asset/version, grant or release changes are made here.

## Native PostgreSQL binary parameters

Three separate strict gates exercise actual RuntimePool-injected `db.query`,
`db.exec` and installed `db.batch` through `run_on_context` and the pinned native
driver. Twelve fixed inputs are each attempted in two fresh pool epochs on
every route: 72 primary observations, plus two separate rollback controls.
Inputs distinguish empty from SQL NULL, ASCII, leading/interior NUL, high bytes,
single backslash, bytea-hex/octal-looking text, nonempty/empty offset views and
all 256 byte values. Independent literal hex, byte-array, length and null/type
expectations are not generated from the production binder or observed output.
Offset-view backing buffers are mutated after enqueue but before await; the
observed SQL bytes must still equal the original snapshot, without sentinels.

Each primary query checks SQL hex, octet length, nullness and complete returned
Uint8Array bytes. Exec and batch insert into a fixture-owned table with
`silent:true`, check affected rows and read actual stored data through the
registered query path. Batch also observes the row inside its real transaction;
after commit, an independent libpq connection checks durable values. Two
user-throw rollbacks require their literal marker, the complete inside-byte
observation, successful subsequent recovery and independent durable row absence.
No mock callback, copied orchestrator or simulated driver result is used here.

Every execution requires actual native/audit drain, retired execution lifetime,
empty queues/callbacks/batch ownership and zero detached/native work before its
context is destroyed. Each of six pool owners requires bounded successful and
idempotent shutdown. The fixture resets schemas only in a task-owned disposable
database; native/audit barriers precede table removal. This is real DDL/storage
coverage, not a mutation-free or realtime-delivery test.

Clean observations must pass before test-only corruption controls are applied.
The complete suite requires 932 controls: query268, exec316 and batch348.
These reject eight result corruptions per primary observation (including
Uint8Array identity and empty/NULL substitution), cleanup/count corruption,
durable/affected/inside mismatches, wrong rollback data and missing/duplicated
ledger cells or replay epochs. An early failed baseline does not receive credit
for later controls it never executed. Full two-epoch coverage and all counters
are required; dropping rows cannot turn a failing corpus into a passing one.

Each exact named case requires PG availability, fresh positive XML, zero
failures/skips and a successful process exit after teardown. All three gates
use the PG schema lock and `needs_pg`; the inner/outer bounds are 60/75 seconds.
Both ASan/UBSan lanes and TSan explicitly discover exactly three and execute
them verbosely, without removing prior selections. Ordinary full suites retain
the real cases as well. Wiring alone is not actual sanitizer or CI evidence.

This finite binary-parameter slice does not cover all SQLSTATE/result types,
arbitrary coercions, allocator failure, the driver's maximum int-sized payload,
untyped parameter inference, other database backends, production cancellation
under every interleaving or complete #90 coverage. Ordinary TEXT/NUL admission
and existing native ownership/setup/lifecycle regressions remain separate
required gates. No new public method, permission, quota or release is introduced.


## Reverse tagged JSON values and ownership

The two local reverse converters have different entry contracts. Actual
extension argument injection preserves tag-shaped JSON objects recursively.
Actual capability Promise settlement recognizes an exact singleton
`{"__bytea_hex__": "<valid even-length hex>"}` as a fresh `Uint8Array`;
malformed, differently named, non-string and multi-member objects follow
ordinary JSON object conversion. This records the existing internal routes
and the BYTEA special case retained by the scoped
[capability conversion amendment](../icd/ICD-0.3.4-cap-call-from-js.md#type-conversion--additions-beyond-033).
It adds no public tag API or database result-mapping policy.

The separate strict targets are `plinth_tests_reverse_json_registry_values`
and `plinth_tests_reverse_json_promise_values`. They use the actual resolver,
staged extension and pool. The second route additionally invokes a registered
native handler through real awaited `cap.call` and owned Promise delivery.
The fixture handler supplies JSON values; PostgreSQL result conversion has
its own targets.

Eighteen fixed families cross root, nested-object and nested-array placements
in each route and two fresh runtime-owner epochs: 216 primary observations.

| Family | Literal payload or distinguishing property |
| --- | --- |
| Empty hex | Empty string |
| Zero byte | `00` |
| Mixed-case hex | `00Aa80Ff` |
| Complete byte range | Hex for all 256 byte values in order |
| Odd length | `0` |
| Non-hex | `0g` |
| Prefix retained | Literal backslash-x prefix before `00` |
| Embedded NUL | Hex-looking prefix and suffix separated by U+0000 |
| Null tag value | JSON null |
| Number tag value | `17` |
| Boolean tag value | false |
| Array tag value | `["00"]` |
| Object tag value | `{"hex":"00"}` |
| Additional member | Valid-looking tag plus an ordinary marker member |
| NUL-suffixed name | Full tag name followed by U+0000 and `tail` |
| Near-miss name | Tag spelling with one underscore removed |
| Invalid first nibble | `g0`, exercising high-nibble rejection separately |
| Even-length NUL | `0` followed by U+0000, reaching byte validation |

A bounded iterative JavaScript observer uses at most 32 input nodes while
retaining the existing runtime stack, CPU and memory limits. Its observations
retain kinds, complete enumerable own-key/value code units and literal byte
arrays; array length is observed separately.
The complete inventory is validated first. Each received value must match
its literal oracle immediately before that receipt's corruption controls;
missing and duplicated cells must fail inventory validation. Valid Promise
buffer cells retain their first buffer through another actual native call
and mutate the two results independently. Reuse observations within a live
pool remain separate from generation identity; expired generation receipts
and bounded, idempotent pool/registry retirement establish the ownership
controls.

Each strict target requires its exact positive Catch case, a 60-second inner
watchdog, a 75-second CTest bound and the PostgreSQL schema resource lock.
The existing member-name targets remain independently required. Both
Clang 20/21 ASan/UBSan jobs and the Clang 21 TSan job discover and run these
two tagged-value targets explicitly with the existing fatal environments.
Registration and discovery alone are insufficient execution evidence.

Parent #90 remains open for its other entries, conversion branches and
applicable ownership schedules. JSON/JSONB structural-versus-text policy,
FLOAT4 rounding, direct transaction-helper combinations and database type
fallbacks retain their separate decisions and coverage requirements.
