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
| Audit (#133) | `plinth_tests_audit_corpus` | Audit admission, owned snapshots and simulated host settlement |
| Pubsub (#148) | `plinth_tests_pubsub_corpus` | Publish/subscribe admission, callback replacement/invocation, returned unsubscribe enqueue and runtime ownership |

These are bounded slices, not comprehensive coverage claims. DB query/exec/
batch and internal transaction callbacks, capability call/batch, extension
module import, distinct conversion implementations and broader async dispatch
remain separate requested targets. The separate JS/JSON implementations in
`eval.cpp`, `runtime_pool.cpp`, `bridge_context.cpp` and the extension runtime
registry do not inherit exhaustive coverage from the shared conversion slice.
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
requires exactly five known logger fixtures and this one exact pubsub case,
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
