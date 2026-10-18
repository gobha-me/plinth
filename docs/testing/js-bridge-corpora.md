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
| Audit (#133) | `plinth_tests_audit_corpus` | Audit admission, owned snapshots and simulated host settlement |
| Pubsub (#148) | `plinth_tests_pubsub_corpus` | Publish/subscribe admission, callback replacement/invocation, returned unsubscribe enqueue and runtime ownership |
| Capability (#150) | `plinth_tests_cap_corpus` | Call/batch admission, owned snapshots, real Promise.all composition with simulated host settlement and runtime ownership |
| Ordinary database (#151) | `plinth_tests_db_admission_corpus` | Query/exec admission, parameter and client-manager snapshots, simulated host settlement and runtime ownership; no SQL execution |
| Public database batch (#174, parent #90) | `plinth_tests_db_batch_corpus` | Public callback admission and installed batch orchestration with simulated host completion/reset; no SQL execution |

These are bounded slices, not comprehensive coverage claims. Broader direct
internal transaction callback denial/coercion policy, real DB dispatch/driver/result conversion,
capability resolver/dispatch, extension
module import, remaining conversion implementations and broader async dispatch
remain separate requested targets. The separate JS/JSON implementations in
`eval.cpp`, `runtime_pool.cpp`, `bridge_context.cpp` and the extension runtime
registry do not inherit exhaustive coverage from the shared conversion slice.
The synchronous slice below covers the first two entry points with finite
inputs; reverse conversion and the other implementations remain outstanding.
Reserved, unimplemented HTTP/storage operation variants are not exposed fuzz
targets. Parent #90 stays open while requested coverage remains incomplete.

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
the production cancellation cascade. Direct internal-helper coercion and
denial policy, exotic accessors/global replacements and remaining parent #90
targets stay outstanding; no production policy or limit changes.

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

Parent #90 stays open. Extension import, reverse conversion, remaining runtime
conversion paths, SQL/driver execution, capability dispatch, production
cancellation and SDK/HTTP ambiguity obligations remain separate. This work
does not authorize a version bump, release or publication.
