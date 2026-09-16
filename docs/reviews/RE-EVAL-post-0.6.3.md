# Post-v0.6.3 architecture re-evaluation

**Issue:** [#29](https://github.com/gobha-me/plinth/issues/29)  
**Baseline:** `v0.6.3` through `431eb511fb388a7cb96ff717bc2b17577bd28487`  
**Reconciled:** 2026-09-16  
**Scope:** source, tests, architecture, deferred ledger, and executable GitHub
backlog; no runtime behavior change

## Outcome

The post-v0.6.3 repair and security work changed what Plinth can honestly claim.
The kernel has a substantially stronger identity, extension-database,
realtime-authority, asset-upgrade, and shutdown baseline than the old
architecture set described. At the same time, the product surface remains a
single-node development system: its launcher journey, production image and
deployment, operational recovery proof, metrics platform, sidecars, HA,
storage, and notification APIs are not shipped.

This review separates those states, maps every actionable gap to a GitHub
issue, and fixes the Dogfood dependency order. Historical ICDs remain as
delivery records; targeted current-state notes point readers to the newer
authority where their lifecycle or security mechanisms have been superseded.

## Review method

The comparison used:

- production entry points and owners under `src/kernel/`;
- package, browser, authentication, WebSocket, PostgreSQL, realtime, and
  lifecycle tests under `tests/`;
- every document under `docs/architecture/`, plus the ICDs whose old
  mechanisms conflicted with current code;
- commits and release notes from `v0.6.3` through the baseline SHA; and
- all open GitHub issues, their outcome milestones, status, dependencies,
  priority, size, and commitment labels.

This is a source-and-test reconciliation. It does not claim a fresh production
deployment run, sanitizer run, or load result; those remain implementation and
operational issue verification obligations.

Representative evidence inspected:

| Surface | Production owner | Representative proof |
|---|---|---|
| Identity bootstrap and disabled-user rejection | `src/kernel/auth/handlers.cpp`, `src/kernel/auth/middleware.cpp` | `tests/integration/auth_http.py`, `tests/kernel/auth/auth_integration_test.cpp`, `tests/kernel/auth/pat_integration_test.cpp` |
| Browser/native WebSocket authentication and authority renewal | `src/kernel/ws/auth_flow.cpp`, `src/kernel/ws/authority.cpp` | `tests/kernel/ws/auth_test.cpp`, `tests/integration/ws_authority.py`, `tests/browser/realtime-smoke.mjs` |
| Restricted extension database and guarded migrations | `src/kernel/js/extension_database.cpp`, `migrations/extension_database.sql` | `tests/kernel/js/db_search_path_test.cpp`, `tests/kernel/packages/migrations_test.cpp` |
| Durable realtime ingress and ordered wakeups | `src/kernel/realtime/emit.cpp`, `src/kernel/realtime/listener.cpp` | `tests/kernel/realtime/listener_test.cpp`, `tests/kernel/realtime/coalescer_test.cpp` |
| Extension dispatch leases and bounded process teardown | `src/kernel/extensions/runtime_registry.cpp`, `src/kernel/lifecycle/shutdown_coordinator.cpp` | `tests/kernel/capabilities/dispatch_extension_test.cpp`, `tests/kernel/lifecycle/shutdown_coordinator_test.cpp`, `tests/kernel/lifecycle/process_shutdown_test.cpp` |
| Active frontend and browser primitives | `src/kernel/shell/active_frontend.cpp`, `client/shell/client/panels/loader.js` | `tests/kernel/shell/active_frontend_test.cpp`, `tests/browser/shell-smoke.mjs`, `tests/browser/shell-upgrade.mjs` |

## Current versus planned

| Surface | Current evidence | Classification and owner |
|---|---|---|
| Identity and local authorization | Cookie/PAT auth, groups/RBAC, atomic first-admin creation, disabled-user rejection | Shipped; safe non-OIDC registration remains strong Dogfood work in #37 |
| WebSocket authority | Browser cookie plus exact Origin/Host upgrade, native token frame, session/permission revalidation, bounded DB lease | Shipped; cross-node reconnect remains fuzzy #77 and storm evidence is medium #88 |
| Extension database isolation | Restricted per-extension logins, transient execution guard, migration ownership transfer | Shipped; SQLSTATE preservation remains medium #92 |
| Realtime durability | Durable outbox, protected publication authority, and server replay protocol | Shipped server behavior; browser resume/smart-query integration remains #32 and source-sequence policy remains fuzzy #42 |
| QuickJS extension dispatch | Tier 2 runtime registry with shared dispatch leases and bounded shutdown | Shipped; Tier 3 remote dispatch is fuzzy #66 |
| Frontend primitives | Active mount, immutable versioned assets, token and SDK redirects, panel loader and fixtures | Shipped primitives only; discovery/launcher/tabs are strong #30/#31 and browser completion is #32 |
| Production packaging/deployment | Developer Docker path only; no supported published image or Kubernetes/Traefik contract | Strong Dogfood work #35/#36 |
| Recovery and downstream proof | Focused crash/lifecycle tests exist; no supported deployment restore/rollback or external app proof | Strong Dogfood work #34/#38/#39 |
| Metrics and scheduling | No current product metrics API/storage or distributed scheduler | Fuzzy #57-#62; #62 depends on #57 and #61 |
| Sidecars and high availability | No shipped registration, remote dispatch, membership, routing, or failover platform | Fuzzy #63-#77 |
| File storage, notifications, outbound HTTP | No shipped extension API | Fuzzy #78/#79/#81/#82 |
| Extension user-deletion cleanup | Architectural text existed without implementation or an issue | Newly owned by fuzzy [#97](https://github.com/gobha-me/plinth/issues/97) |

## Corrections made

### Architecture set

- `ARCHITECTURE.md` now describes C++23, GitHub Actions, the actual source
  tree, the developer-only container path, and the supported current authority
  documents. Old prose-only backlog bullets were either mapped to issues or
  made explicitly conditional.
- `01-identity.md` now distinguishes `SessionFilter` browser/PAT handling from
  public routes, records atomic first-admin and disabled-user fail-closed
  behavior, describes WebSocket authority, and moves the unimplemented user
  cleanup contract to #97.
- `02-capabilities.md` now distinguishes shipped kernel and Tier 2 extension
  capabilities from unavailable Tier 3 dispatch. It removes claims that
  notification, storage, metrics, HTTP, HMAC, or `users.list` are already
  kernel-provided and maps planned surfaces to their issues.
- `03-data.md` now treats restricted database roles as the security boundary,
  keeps `search_path` as name resolution, points to the durable outbox, uses
  the explicit shutdown coordinator, and distinguishes shipped server replay
  from unimplemented browser smart-query/resume behavior.
- `04-services-ha.md` now marks metrics, scheduling, notifications, sidecars,
  and HA as planned. Partitioned PostgreSQL metrics issue #57 supersedes the
  old in-memory-only sketch.
- `05-extensions.md` now separates shipped package/runtime behavior from
  planned extension HTTP, administration, hot reload, supervision,
  documentation, runtime-override, and conditional composition work.
- `06-frontend.md` now describes shipped asset/SDK/loader primitives without
  claiming an operational launcher, tab strip, or production panel journey.

### Historical contracts

Targeted notes in ICD-0.1.6, ICD-0.3.3, ICD-0.5.0.3, and ICD-0.5.3 preserve
their historical designs while directing readers to the shipped WebSocket,
database-isolation, transaction, lease, and shutdown contracts. In particular:

- browser WebSockets do not expose session tokens to JavaScript;
- `search_path` is not the database authorization boundary;
- Drogon transaction completion is callback-observed rather than the explicit
  SQL `COMMIT` sequence in the old ICD; and
- normal runtime/database teardown is coordinator-owned, not an `atexit`
  chain or copied fixture sequence.

The capability-registry and QuickJS-bridge design documents are now explicitly
historical where later architecture/source/test authority supersedes their
Tier 3, batching, standard-library, realtime, database, or teardown mechanism.

### Backlog and deferred ledger

- Created #97 for the previously unowned `users.deleted` and `users.list`
  contract.
- Demoted #62 from medium to fuzzy because its acceptance depends on the fuzzy
  metrics storage and extension API outcomes #57/#61.
- Clarified #38 dependencies as #34/#35/#36 and #39 dependencies as
  #31/#32/#38 plus the downstream application.
- Expanded #32 to own reconciliation and executable coverage for browser
  smart-query and replay/resume claims, coordinated with #42 where sequence
  policy is a prerequisite.
- Added #97 to the roadmap and active deferred index.
- Assigned the living-subsystem-contract proposal to #41 rather than an
  expired `0.6.0.N` paper slot.
- Retained historical deferred entries as evidence. The active index, not the
  legacy narrative, is the complete actionable set.

## Commitment classification

**Strong:** Dogfood #29-#40, extension HTTP #47/#48, and supported TSan #91.
These are near-term load-bearing product, security, delivery, or reliability
gates.

**Medium:** the bounded shell contract decision and float/navigation arc
#41/#43-#46/#49/#50, continuous reconnect/RBAC/fuzz/SQLSTATE evidence
#88-#90/#92. These have plausible value and owners but do not gate the first
Dogfood journey. Their live dependencies still control readiness.

**Fuzzy:** sequence-policy #42, later administration panels #51-#55, and the
operational-core, sidecar, HA, storage/polish, and 1.0 arcs #56-#87/#93/#97.
Their desired outcomes are recorded, but details and commitment can change at
their preceding re-evaluation gates.

## Dogfood dependency order

The bounded critical path is:

1. **Wave 0:** finish this re-evaluation (#29).
2. **Wave 1, parallel:** specify the launcher (#30), add CSRF protection (#33),
   complete crash injection (#34), and publish the production image (#35).
3. **Wave 2, parallel:** implement launcher/discovery (#31 after #30), provide
   Kubernetes/Traefik deployment (#36 after #33/#35), and define safe local
   registration (#37 after #29/#33).
4. **Wave 3, parallel:** complete browser/runtime coverage (#32 after #31) and
   prove recovery/upgrade/rollback/shutdown (#38 after #34/#35/#36).
5. **Wave 4, parallel:** prove the downstream application (#39 after
   #31/#32/#38 and downstream readiness) and run ingress DAST (#40 after
   #33/#36/#37).

Issue status must follow live dependencies. After #29 merges, #30 becomes
ready. #37 remains blocked on #33.

## Resolved and conditional questions

- QuickJS remains the embedded engine with the shipped bounded defaults; this
  review does not reopen that decision.
- Administration is not a general route bypass: HTTP surfaces declare their
  required rules. Within capability resolution, `kernel.admin` is explicitly
  the universal match, alongside the separately bootstrapped package rules.
- `src/kernel/ws/` and `src/kernel/realtime/` are sibling concerns: connection
  protocol versus durable event pipeline. No directory merge is planned.
- A package registry is conditional and has no scheduled owner. It must not be
  inferred as backlog from architecture prose.
- Font/icon redirect endpoints and a broad shared component library are
  conditional until a consumer and owning issue exist.
- Public readiness remains a separate security, license, documentation, and
  operational decision; this review does not make a release or visibility
  commitment.

## Zero-gap checks

The dedicated current-state documents for shutdown,
extension-database isolation, and WebSocket authority match the inspected
owners and tests. No corrective edit was needed in those documents. The
remaining active SQLSTATE and lifecycle/concurrency work is explicitly owned
by #92 and #91 rather than hidden in those contracts.

## Verification contract

For this documentation-only change:

- check formatting and whitespace with the repository formatter and
  `git diff --check`;
- run the public-readiness/link validation;
- search the current architecture set for stale compiler, forge, lifecycle,
  and shipped-versus-planned claims;
- verify every active deferred item and roadmap entry resolves to an open
  issue; and
- require all repository candidate checks and exact-merge-SHA post-merge
  checks before treating #29 as complete.

No C++ source, schema, migration, runtime configuration, or test expectation is
changed by this re-evaluation.
