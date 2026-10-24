# JavaScript bridge coverage ledger

[Issue #90](https://github.com/gobha-me/plinth/issues/90) requests bounded
coverage of malformed types, sizes, nesting, lifetime, cancellation and error
conversion across exposed JavaScript bridges. This ledger maps the
[21 registered native entries](js-bridge-corpora.md#registered-entry-inventory)
to selected named test assertions. The [corpus guide](js-bridge-corpora.md)
describes each harness and its wider exclusions.

This is **source coverage accounting, not an execution report**. Source links
are pinned to commit `0802bb8ad8e67ced54b2861079319729c5bc5749`. A target's
existence or a passing aggregate run does not show that every input or schedule
was exercised. Exact-source execution, strict discovery, sanitizer results,
skips and unavailable checks need their own evidence. Parent #90 remains open.

## Reading the matrix

- **D — direct:** a registered callback/function is invoked and the stated
  observation is asserted. Downstream work may still be simulated.
- **N — native:** selected assertions also establish actual downstream native
  work, such as PostgreSQL, extension dispatch or owned callback execution.
  This describes the test source, not a fresh execution result.
- **P — projected/simulated:** fixture projection or simulated host settlement;
  it does not establish native execution of that part.
- **U — unestablished:** not established by the selected source assertions.
  It does not assert that no other relevant test exists.
- **I — indirect only:** documented valid installed orchestration; dedicated
  direct malformed-helper coverage remains incomplete.

Finite lengths/depths and harness budgets are not new production quotas.
Rejecting an object at a primitive-only entry is type evidence, not recursive
depth coverage. Reset/shutdown evidence is not automatically per-call
cancellation evidence. A cell can carry both direct admission and projected
settlement evidence. The source keys link to the named cases and targets below.

## Native entries and requested dimensions

| # / native entry | Malformed types | Sizes | Nesting | Lifetime | Cancellation | Error conversion |
| --- | --- | --- | --- | --- | --- | --- |
| 1 `log.debug` | D missing/wrong message/context [L] | D empty, NUL, Unicode, 256-codepoint sample [L] | D depth 1–3, cycle, throwing `toJSON` [L] | D release/destroy/rebuild/shutdown [L] | U per-call | D caught class plus no rejected sink record [L] |
| 2 `log.info` | D as above [L] | D as above [L] | D as above [L] | D as above [L] | U per-call | D as above [L] |
| 3 `log.warn` | D as above [L] | D as above [L] | D as above [L] | D as above [L] | U per-call | D as above [L] |
| 4 `log.error` | D as above [L] | D as above [L] | D as above [L] | D as above [L] | U per-call | D as above [L] |
| 5 `config.get` | D missing and eight wrong argument kinds [C] | D finite unknown-key strings; key budget 96, not a tested maximum policy [C] | U depth; `.nested` is a dotted string, not a nested payload [C] | D held lease/shutdown refusal, reset and retirement [C] | U per-call | D TypeError; P values from independent fake Config [C] |
| 6 `crypto.hash` | D wrong scalar/object inputs [K] | D empty, NUL/Unicode, 1024-character/byte samples and offset views [K] | U depth; object rejection only [K] | D real pool release/reset/shutdown [K] | U per-call | D independently calculated digest; literal error-class oracle [K] |
| 7 `crypto.randomBytes` | D strict-number matrix including coercible/nonfinite/fractional values [K] | D successful 1 / 4096 and rejected 0 / -0/outside/wrapping/huge values [K] | U depth; object rejection only [K] | D pool controls [K] | U per-call | D exact TypeError/RangeError and range message [K] |
| 8 `crypto.timingSafeEqual` | D wrong argument kinds [K] | D empty, unequal length, offset views and equal/different bytes [K] | U depth; object rejection only [K] | D pool controls [K] | U per-call | D boolean/class oracle [K] |
| 9 `db.query` | D SQL/parameter/type/numeric admission [Q] | D finite SQL 128 / value 64 / parameter 4 budgets; N selected native BYTEA cases [Q], [S] | U recursive depth; unsupported object parameter rejection is narrower [Q] | D leases/reset; N selected native queries and SQLSTATE cleanup [Q], [S] | D pre-cancel; N qualified wall-clock fan-out case, not deterministic physical-cancel proof [Q], [S] | D admission; P simulated settlement; N SQLSTATE projection/recovery [Q], [S] |
| 10 `db.exec` | D SQL/parameter/type/numeric admission [Q] | D same finite budgets; N native stored BYTEA [Q], [S] | U recursive depth [Q] | D leases/reset; N selected native exec and SQLSTATE cleanup [Q], [S] | D pre-cancel; U live exec-specific cascade in selected sources | D admission + P settlement; N SQLSTATE projection/recovery [Q], [S] |
| 11 `db.batch` | D public callback/options/flow admission [B] | D bounded operation/source/flow cases; N finite native binary transaction [B], [S] | D nested public batch rejection; U general recursive payload depth [B] | D actual public orchestrator/pool; P host completion; N selected rollback/cleanup [B], [S] | D pre-cancel; P timeout/reset/terminal completion simulation; no native timer/cascade credit from [B] | D public admission + P settlement; N two real SQLSTATE batch scenarios with rollback/recovery [B], [S] |
| 12 `__db_batch_begin__` | I direct malformed entry incomplete | I incomplete | I incomplete | I valid installed orchestration only [B], [S] | I direct helper combinations incomplete | I valid orchestration only; direct malformed precedence incomplete |
| 13 `__db_batch_commit__` | I direct malformed entry incomplete | I incomplete | I incomplete | I valid installed orchestration only [B], [S] | I direct helper combinations incomplete | I valid orchestration only; direct malformed precedence incomplete |
| 14 `__db_batch_rollback__` | I direct malformed entry incomplete | I incomplete | I incomplete | I valid installed orchestration only [B], [S] | I direct helper combinations incomplete | I valid orchestration only; direct malformed precedence incomplete |
| 15 `audit.log` | D event/payload/reserved-field/authority admission [A] | D event 256 / string 64 finite samples [A] | D depth 3 / node 15 trees, reserved-field near misses [A] | D owned snapshots/pool; N row ownership, drain and blocked-persistence witness [A] | D pre-cancel/precedence; P host settlement; U physical persistence cancellation | D admission + P settlement; N actual acknowledgement/persistence separation, not rejection-family completeness [A] |
| 16 `cap.call` | D admission; N literal native value families [H] | D finite input bounds; N exact byte/depth edges [H] | D finite trees/cycle rejection; N selected native conversion-depth edges [H] | D snapshots/pool; N caller/callee generation ownership and drain [H] | D pre-cancel; N actual owned callee cancellation propagation [H] | D admission + P host settlement; N eight literal native error families [H] |
| 17 `cap.batch` | D tuple/batch admission; N native data [H] | D batch 4 / tuple 3 finite corpus; N exact conversion edges [H] | D tree/cycle plus real Promise.all composition; N selected native edges [H] | D snapshots/pool; N causal fan-out, generation ownership/drain [H] | D pre-cancel; N actual caller-to-callee cancellation for this route [H] | D admission + P settlement; N literal native error families [H] |
| 18 `pubsub.publish` | D direct admission; N selected actual outbox payload kinds [P] | D finite channel/payload/quota cases; N finite outbox values [P] | D depth 3 / node 15 trees; N selected nested outbox payload [P] | D captured-copy/reset controls; N publish owner shutdown/outbox receipt [P] | D pre-cancel; U full native chain cancellation | D admission + P settlement; N acknowledgement/outbox match is not full native error-family coverage [P] |
| 19 `pubsub.subscribe` | D channel/handler admission [P] | D finite channel/quota cases [P] | U recursive input depth: channel/function entry; nested delivered payload is separate [P] | D replacement/callback/reset; N owner-loop and queued-retirement schedules [P] | D pre-cancel; N enclosing subscribed execution cancels after delivery, plus queued-checkout retirement; U active callback/per-subscribe cancellation; not whole chain [P] | D admission + P registration settlement; U comprehensive real broker rejection conversion [P] |
| 20 returned unsubscribe callback | U malformed arguments (selected corpus calls zero args) [P] | U argument size | U argument nesting | D actual closure enqueue; P manual settlement/deregistration seam; N separate registered unsubscribe cleanup case [P] | U per-unsubscribe cancellation | P return/settlement in admission harness; U native rejection family [P] |
| 21 `import_from_src` | D/N missing arity and coercion traces/throws in source-gate regression [M] | D/N finite source 4096 / id 128 budgets and byte/string cases, not production maxima [M] | D/N bounded nested import; U arbitrary recursive import depth [M] | N fresh registry owners, marker replacement/reset, inflight-zero shutdown [M] | U production cancellation/unresolved async [M] | D/N actual coercion/compile/evaluate/throw observations and literal classes; full parser prose not pinned [M] |

## Named source assertions and target traceability

### Log

Targets: `plinth_tests_log_corpus`.

[`log_corpus_test.cpp:147`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/log_corpus_test.cpp#L147) constructs independent message/context cases; lines 225–267 specify expected classes and invoke the chosen log level. Lines 292–315 compare actual sink record count, exact payload and level. Named case [`QuickJS log callbacks have a bounded deterministic corpus` at 395](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/log_corpus_test.cpp#L395) runs all four levels with replay/rebuild/teardown controls. Context depth 1–3 and the 256-codepoint sample are finite; cancellation is not supplied by those lifecycle controls.

### Config

Targets: `plinth_tests_config_corpus`.

[`config_corpus_test.cpp:309`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/config_corpus_test.cpp#L309) invokes the real registered getter against missing/wrong arguments and extra ignored arguments. Its expected allowed values come from an independent **fake Config**, not native configuration loading. Named case [`QuickJS config.get projection has a bounded deterministic corpus` at 424](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/config_corpus_test.cpp#L424) includes excluded keys and held-owner shutdown controls. `Kind::nested` at 251–279 makes a dotted key string; it must not become a recursive-data claim.

### Crypto

Targets: `plinth_tests_crypto_corpus`; the linked strict numeric regression is separate source evidence.

[`crypto_corpus_test.cpp:82`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/crypto_corpus_test.cpp#L82) builds bounded strings/views and requested sizes. Lines 286–340 compare independent digest/boolean/typed-array/error expectations with actual evaluation. Named case [`QuickJS crypto host callbacks have a bounded deterministic corpus` at 437](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/crypto_corpus_test.cpp#L437) supplies pool ownership. The separate [`crypto.randomBytes enforces strict integer and range boundaries` at 83](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/crypto_numeric_regression_test.cpp#L83), with literal matrix 31–59 and matcher 65–78, supersedes any inference of missing numeric tests from the older corpus generator's exclusion comment.

### Ordinary database admission

Targets: `plinth_tests_db_admission_corpus`.

[`db_admission_corpus_test.cpp:679`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/db_admission_corpus_test.cpp#L679) calls actual registered query/exec functions, captures owned operations and uses **simulated settlement**, without opening a DB client. [`numeric_controls` at 1418](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/db_admission_corpus_test.cpp#L1418) checks 27 numbers × 2 entries × normal/cancelled/unavailable × 2 owners (324 calls); these are literal admission/tag/sign observations, not driver acceptance. Named case [`QuickJS ordinary database calls have a bounded deterministic admission and ownership corpus` at 1482](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/db_admission_corpus_test.cpp#L1482) checks 192 cases and ownership controls.

### Public database batch

Targets: `plinth_tests_db_batch_corpus`.

[`db_batch_corpus_test.cpp:735`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/db_batch_corpus_test.cpp#L735) invokes public `db.batch` and the installed JS orchestrator. Host completions, rollback-owner and timeout terminal transitions at 543–590 are simulated. Named case [`QuickJS db.batch has a bounded deterministic admission and orchestration corpus` at 1177](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/db_batch_corpus_test.cpp#L1177) asserts 138 cases with finite source grammar/depth and pool controls. Public nested-batch rejection is distinct from arbitrary recursive parameter conversion. The direct malformed-helper corpus remains incomplete.

### Selected native database supplements

Targets: `plinth_tests_native_bytea_query`, `plinth_tests_native_bytea_exec`, `plinth_tests_native_bytea_batch`, `plinth_tests_sqlstate_projection`. The fan-out case is additional source evidence in the linked native test file.

[`async_bridge_test.cpp:715`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/async_bridge_test.cpp#L715) names separate query/exec/batch BYTEA cases. The [`wall-clock cancel during fan-out` case at 862](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/async_bridge_test.cpp#L862) runs eight real `db.query` sleeps and requires wall-clock failure/bounded completion; queries may finish before cancellation is detected, so it does not establish deterministic physical query cancellation. PG-unavailable skips remain possible at source level. [`Registered database bindings preserve authoritative SQLSTATE` at 306](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/db_sqlstate_projection_test.cpp#L306) runs query/exec projections plus two batch scenarios per epoch. The actual generated program at 213–255 uses `db.batch`, insert, failing query, catch and recovery; lines 351–376 additionally require rollback and owned teardown before assertions. This is real SQLSTATE projection despite the filename's word “projection”. Selected PG scalar/text/benign-column corpora are separately recorded by the guide; they do not establish legacy OID/JSON/disabled-flag policies.

### Audit

Targets: `plinth_tests_audit_corpus`, `plinth_tests_native_audit_corpus`, `plinth_tests_native_audit_acknowledgement`.

[`audit_corpus_test.cpp:233`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/audit_corpus_test.cpp#L233) generates event/payload/authority/precancel cases; line 475 invokes actual `audit.log` with captured fake caller and simulated settlement. Named case [`QuickJS audit.log has a bounded deterministic contract corpus` at 855](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/audit_corpus_test.cpp#L855) checks replay and ownership. Separate [`Native audit writes preserve the owned corpus` at 727](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_audit_corpus_test.cpp#L727) verifies actual stored rows/provenance and closes owners before semantic assertions. [`Native audit acknowledgement precedes blocked persistence` at 942](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_audit_corpus_test.cpp#L942) witnesses an actual blocked INSERT and early acknowledgement before unlock/drain. Its synthetic caller profile is not authenticated HTTP evidence; acknowledgement is not persistence completion.

### Capabilities

Targets: `plinth_tests_cap_corpus`, `plinth_tests_native_capability_data`, `plinth_tests_native_capability_edges`, `plinth_tests_native_capability_errors`, `plinth_tests_native_capability_ordering`, `plinth_tests_native_capability_ownership`, `plinth_tests_native_capability_nested`.

[`cap_corpus_test.cpp:654`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/cap_corpus_test.cpp#L654) directly calls either registered entry; line 699 explicitly describes simulated host completion. Named case [`QuickJS capability calls have a bounded deterministic admission and batch corpus` at 1255](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/cap_corpus_test.cpp#L1255) checks admission/owned snapshots/Promise.all and lifecycle. Native extensions are separate: [`bounded data corpus` at 429](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_capability_data_test.cpp#L429) requires actual values and cleanup; [`byte/depth boundaries` at 682 and literal error families at 744](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_capability_edges_test.cpp#L682) run both routes and independent corruptions. [`causal ordering/fanout` at 697 and generation ownership at 702](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_capability_ownership_test.cpp#L697) distinguish logical cancellation from physical drain. [`Native capability cancellation reaches the owned callee generation` at 463](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_capability_corpus_test.cpp#L463) asserts propagation for both call and batch, actual rejection, released SQL blocker and all owners/drains complete. These assertions do not establish HTTP authorization or whole-network behavior.

### Pubsub

Targets: `plinth_tests_pubsub_corpus`, `plinth_tests_native_pubsub_publish`, `plinth_tests_native_pubsub_delivery`, `plinth_tests_native_pubsub_owner`, `plinth_tests_native_pubsub_retirement`.

[`pubsub_corpus_test.cpp:603`](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/pubsub_corpus_test.cpp#L603) calls registered publish/subscribe; lines 705–805 explicitly simulate settlement, invoke captured callbacks and invoke the returned unsubscribe with **zero arguments**, then manually settle/free the seam. Named case [`QuickJS pubsub has a bounded deterministic admission and callback corpus` at 1122](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/pubsub_corpus_test.cpp#L1122) includes pool/callback ownership controls. [`Native pubsub publish acknowledgements preserve the owned corpus` at 477](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_pubsub_publish_corpus_test.cpp#L477) checks actual PostgreSQL outbox rows after owner finish; it does not prove delivery. [`Native pubsub registered delivery preserves owned cleanup` at 542](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/native_pubsub_delivery_corpus_test.cpp#L542), with observe 449–482, checks registered unsubscribe/owner destroy/broker drain against an explicitly **synthetic producer**. [`registered owner loop` at 566](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/pubsub_dispatch_owner_test.cpp#L566) and [`queued delivery rejects retired checkout ownership` at 1221](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/js/pubsub_dispatch_owner_test.cpp#L1221) provide real bounded owner-loop/retirement controls. These separate cases do not establish the full listener → writer → broker chain.

### Module import

Targets: `plinth_tests_module_import_corpus`; the source-gate regression is
separate linked source evidence.

[`Extension module loading enforces the original source gate` at 2168](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/capabilities/dispatch_extension_test.cpp#L2168)
checks actual arity/coercion traces and throws, with five literal scenarios and
entry counters at 2387–2418.
[`Extension module imports have a bounded deterministic corpus` at 2501](https://github.com/gobha-me/plinth/blob/0802bb8ad8e67ced54b2861079319729c5bc5749/tests/kernel/capabilities/dispatch_extension_test.cpp#L2501)
stages handlers and checks actual compile/evaluate/default/nested-entry
observations, fresh-owner markers and shutdown before copied corruptions.
The [module import guide](js-bridge-corpora.md#extension-module-import)
limits this to 124 dispatches and excludes unresolved async/production
cancellation. Source 4096 / id 128 budgets do not establish production maxima.

## Incomplete scopes and separate ledgers

- Dedicated direct malformed transaction/helper combinations remain incomplete.
  Valid installed `db.batch` orchestration does not discharge the three internal
  helper rows or establish their malformed-input/error-precedence contracts.
- The full native pubsub listener → writer → broker chain remains incomplete.
  Actual outbox persistence, synthetic producer delivery and real owner-loop
  retirement schedules retain their separate narrower credit.
- Legacy PostgreSQL result coverage remains incomplete. Selected supported
  scalar/text/BYTEA/benign-column cases do not establish omitted OID,
  JSON/JSONB, disabled-flag or key-policy branches. This ledger does not add
  tests or select those policies.

These incomplete scopes and U cells record the limits of the selected evidence.
They do not select new implementation work or change existing contracts.
Missing, unbuildable, skipped or incomplete targets must remain explicit.

The [distinct conversion ledger](js-bridge-corpora.md#distinct-conversion-and-ownership-coverage)
keeps shared JS-to-JSON, one-shot synchronous, pooled synchronous and reverse
JSON-to-JS converters separate. Their bounded corpora do not automatically
cover every registered callback. Reverse tagged and safe unsigned registry/
promise slices are separate conversion evidence, not a 22nd native entry.
Selected PostgreSQL result, text and benign-column corpora likewise retain
their narrower guide sections.

`__db_batch_orchestrator__` is an additional directly callable **JavaScript**
function, not a native callback. Test-only observers and `test_host_sleep`,
and reserved/unimplemented HTTP/storage enum variants, do not enlarge the
21-entry exposed native inventory. Parent #90 remains open while its requested
coverage is incomplete.

[L]: #log
[C]: #config
[K]: #crypto
[Q]: #ordinary-database-admission
[B]: #public-database-batch
[S]: #selected-native-database-supplements
[A]: #audit
[H]: #capabilities
[P]: #pubsub
[M]: #module-import
