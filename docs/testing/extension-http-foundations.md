# Extension HTTP foundations and remaining implementation

This is a partial implementation of [#48](https://github.com/gobha-me/plinth/issues/48).
The [extension HTTP contract](../icd/ICD-extension-http-surfaces.md) still requires
three real adapters, committed installation authority, owned dispatch and drain,
and actual mode-qualified measurements. There is no general extension HTTP
route or supported handler mode yet. Neither #48 nor #47 is complete.

## Current behavior

Explicit package submissions parse the original manifest bytes before ordinary
validation can discard duplicate keys. HTTP declarations have shared grammar,
public-subset and mode validation. Install authorization binds the actor,
operation, original artifact and declarations; registering the two additional
installer rules grants them to nobody. Public claims require the additional
rule and artifact-bound confirmation. Only the actual kernel-selected bundle
operation can provide bundled provenance.

Until the actual selected-mode receipt and readiness path is connected,
nonempty HTTP declarations fail install and upgrade, including their dry runs,
with `http-handler-mode-unavailable` after authorization. Successful declaration
parsing is data validation, not installation or route readiness. Ordinary valid
packages without HTTP claims retain their existing lifecycle. Already-installed
HTTP-looking unknown fields remain inert; restart and enable do not create
validated HTTP authority from those fields.

Bootstrap creates empty, checked storage for claim receipts, reservations,
adapter bindings and pending dispatch obligations. It refuses incompatible
retained definitions and does not backfill legacy authority. These tables and
their component transaction APIs do not publish an HTTP owner. Restrictive
references retain live generation receipts; there is no timer-based release.

The internal building blocks comprise:

- A captured `server/http.js` module index with bounded relative imports and
  no subsequent source-directory reads. An opt-in QuickJS hook supplies complete
  import-name lengths before normalization or cache lookup. Existing callers
  do not acquire this new import policy automatically.
- Closed binary request, response and context codecs; charged Frame1 payloads
  and canonical framing; synchronous native QuickJS value conversion without
  getter, proxy or arbitrary thenable observation. These codecs do not start
  a socket, handler or driver. The plain DTO codec requires caller-owned
  pre-copy credits; charged frame and native-response wrappers retain their
  own credits through destruction.
- Feature admission and a single logical response arbiter. A response decision
  does not establish durable acknowledgement, actual transport write or
  physical handler settlement.
- A separately owned PostgreSQL dispatch store with acknowledged prepare and
  ARM operations, bounded pending capacity and actual database cleanup owners.
  Handoff, final choice, observation, terminal release and recovery remain
  unavailable until their real common owner supplies the required facts.
- An HTTP authentication wrapper using the existing kernel filters, preserving
  selected-cookie precedence while requiring a selected PAT for protected
  claims. It is exercised at the filter seam, not through a new HTTP route.

The configuration parser accepts `extension_http.drain_timeout_ms` as an integer
from 1000 through 5000, defaulting to 5000. The deadline helper caps it by the
remaining enclosing deadline. This is not evidence of an integrated HTTP drain.
Global transport body/spill settings, existing API filters and permission grants
are unchanged.

## Requirement ledger

The 59 IDs below are requirements from the implementation contract, not a test
count. Component tests provide the listed evidence; every family still needs
its real integrated acceptance. Synthetic identities, stored fixture rows,
trusted request attributes and test module maps establish only their labeled
component inputs, not installed provenance or full authentication journeys.

| Contract IDs | Component evidence | Remaining integrated proof |
|---|---|---|
| M01–M06 | Original-byte declarations, grammar, duplicates, scalar limits and public subset | Actual selected-mode install/readiness |
| C01–C06 | Caller-owned transactions, serialization, reservations, frontend conflicts and fresh receipt reads | Atomic lifecycle publication, replacement, removal and restart recovery |
| P01–P04 | Original/decoded-path agreement and opaque raw-query validation | Real parser and catch-all transport mapping |
| A01–A06 | Authentication decision controls and actual PostgreSQL filter cases | New route, backend failure and full mixed-fault journeys |
| I01–I04 | Bound authorization and public confirmation; rule definitions without grants | Complete authenticated install/upgrade-to-live journey |
| Q01–Q04 | Captured module index, actual import hook and native value/binary/Promise codec | Real owned QuickJS driver, cancellation and child settlement |
| S01–S04 | Exact closed Frame1 variants, aggregate bounds and charged buffers | Real UDS peer, verified identity/clock, sequence and physical settlement |
| N01–N04 | Shared native DTO validation | Eligible compiled callable, executor, cancellation and actual children |
| B01–B04 | Binary/header bounds, body ownership and copy-credit controls | Real transport buffering, ordered fields, bodyless and protocol behavior |
| L01–L06 | Feature credits, cutoff/arbiter controls and capped drain arithmetic | Actual queued entry, lifecycle fencing and joined owners in all modes |
| E01–E03 | Target/schema validation and single-choice controls | Full route precedence, adapter loss and wire uncertainty |
| U01–U05 | Privacy-bounded store binding, real prepare/ARM commits and restrictive pending pins | Irrevocable handoff, durable event/choice, wire facts, terminal and crash recovery |
| F01–F03 | No measurements claimed | Equivalent fixed/catch-all trials for all modes and reviewed per-request result |

In particular, unavailable-mode refusal is not positive mode coverage. A native
Promise tag does not own its pending jobs. Result, cancellation, EOF, a database
callback, a wire choice or an epoch fence does not join physical work. An empty
schema or stored receipt does not prove that any adapter is ready.

## Validation entry points

The repository build includes the codecs and their tests. Run focused checks
before the full disposable-PostgreSQL/WebSocket suite, using the contributor
[validation instructions](../../CONTRIBUTING.md). `ctest -L extension_http`
selects the individually verified HTTP component cases; each strict gate
requires exactly its named case, positive assertions and zero failures or
skips. Pure aggregate success does not replace those gates.

Run formatting, fatal lint, ASan/UBSan and feasible TSan lifecycle coverage as
required by the contributor guidance. Candidate and exact-merge CI remain
separate delivery gates. A passing component or regression suite cannot close
the remaining adapter, lifecycle, audit or measurement requirements above.
