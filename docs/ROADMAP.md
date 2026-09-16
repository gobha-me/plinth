# Plinth Roadmap

GitHub Issues are the authoritative executable backlog. This document records
strategy, ordering, and historical numbering; it is not a second task tracker.
Every remaining deliverable must have an issue with an outcome, acceptance
criteria, dependencies, verification, milestone, priority, size, and commitment
label.

- [Outcome milestones](https://github.com/gobha-me/plinth/milestones)
- [Open issues](https://github.com/gobha-me/plinth/issues)
- [Deferred design ledger](DEFERRED.md)
- [Completed release history](CHANGELOG.md)

The old `0.x.y`, `0.6a`, `0.6b`, `LH-*`, and `RE-EVAL` identifiers are
historical aliases, not release promises. Each issue's **Traceability** section
preserves the relevant alias. GitHub milestones are outcome-based, and issue
dependencies determine execution order within them.

The next feature release is `v0.6.6`. Earlier roadmap text called the launcher
work `0.6.4`, but maintenance and security releases already used `v0.6.4` and
`v0.6.5`; those published versions will not be renumbered.

## Execution policy

Work follows `plan -> approval -> branch -> implement -> validate/review/fix ->
PR -> green candidate CI -> merge -> exact-merge-SHA CI`. Release publication
requires separate approval and exact release-SHA evidence.

The immediate finish line is **Dogfood Complete**: a real downstream application
must run through the production launcher on a monitored, data-minimal Plinth
deployment behind Traefik. The public canary must not collect private content or
secrets. OIDC remains a stretch goal; safe local registration is required first.

## Dogfood Complete

This is the current critical path.

The dependency-ready execution waves are:

1. **Wave 0:** #29 (this architecture reconciliation).
2. **Wave 1, parallel:** #30, #33, #34, and #35.
3. **Wave 2, parallel:** #31 after #30; #36 after #33 and #35; #37 after
   #29 and #33.
4. **Wave 3, parallel:** #32 after #31; #38 after #34, #35, and #36.
5. **Wave 4, parallel:** #39 after #31, #32, #38, and its downstream
   application is ready; #40 after #33, #36, and #37.

GitHub issue dependencies remain authoritative if this summary and live issue
state diverge.

- [#28 Track every remaining Plinth deliverable in GitHub Issues](https://github.com/gobha-me/plinth/issues/28)
- [#29 Re-evaluate the post-0.6.3 architecture against current main](https://github.com/gobha-me/plinth/issues/29)
- [#30 Specify application discovery, tabs, and the home launcher](https://github.com/gobha-me/plinth/issues/30)
- [#31 Implement application discovery, tabs, and the home launcher](https://github.com/gobha-me/plinth/issues/31)
- [#32 Complete deferred browser and client-runtime contract coverage](https://github.com/gobha-me/plinth/issues/32)
- [#33 Protect mutating browser API routes against CSRF](https://github.com/gobha-me/plinth/issues/33)
- [#34 Complete crash-injection coverage for upgrade and realtime recovery](https://github.com/gobha-me/plinth/issues/34)
- [#35 Publish a production Plinth OCI image](https://github.com/gobha-me/plinth/issues/35)
- [#36 Provide a supported Kubernetes and Traefik deployment](https://github.com/gobha-me/plinth/issues/36)
- [#37 Define and enforce safe non-OIDC account registration](https://github.com/gobha-me/plinth/issues/37)
- [#38 Prove deployment backup, restore, upgrade, rollback, and bounded shutdown](https://github.com/gobha-me/plinth/issues/38)
- [#39 Prove a real downstream application journey through Plinth](https://github.com/gobha-me/plinth/issues/39)
- [#40 Run DAST against the monitored ingress surface](https://github.com/gobha-me/plinth/issues/40)

## Shell and Administration

- [#41 Decide whether to adopt living subsystem contracts](https://github.com/gobha-me/plinth/issues/41)
- [#42 Resolve realtime source-sequence tracking and superseded sequences](https://github.com/gobha-me/plinth/issues/42)
- [#43 Specify the shell float system](https://github.com/gobha-me/plinth/issues/43)
- [#44 Implement the shell float system](https://github.com/gobha-me/plinth/issues/44)
- [#45 Specify trays, content-type resolution, and navigation intents](https://github.com/gobha-me/plinth/issues/45)
- [#46 Implement trays, content-type resolution, and navigation intents](https://github.com/gobha-me/plinth/issues/46)
- [#47 Specify manifest-declared extension HTTP surfaces](https://github.com/gobha-me/plinth/issues/47)
- [#48 Implement manifest-declared extension HTTP surfaces](https://github.com/gobha-me/plinth/issues/48)
- [#49 Re-evaluate the completed shell and extension-HTTP arc](https://github.com/gobha-me/plinth/issues/49)
- [#50 Build the package-management administration panel](https://github.com/gobha-me/plinth/issues/50)
- [#51 Build the groups administration panel](https://github.com/gobha-me/plinth/issues/51)
- [#52 Build the RBAC administration and policy-explanation panel](https://github.com/gobha-me/plinth/issues/52)
- [#53 Build the audit-log administration panel](https://github.com/gobha-me/plinth/issues/53)
- [#54 Build the schedules and retention administration panel](https://github.com/gobha-me/plinth/issues/54)
- [#55 Build the performance and health tray extension](https://github.com/gobha-me/plinth/issues/55)

## Operational Core

- [#56 Freeze the database schema behind immutable migrations](https://github.com/gobha-me/plinth/issues/56)
- [#57 Add partitioned kernel metrics storage](https://github.com/gobha-me/plinth/issues/57)
- [#58 Implement the distributed scheduler](https://github.com/gobha-me/plinth/issues/58)
- [#59 Implement default maintenance tasks](https://github.com/gobha-me/plinth/issues/59)
- [#60 Re-evaluate the operational-core arc](https://github.com/gobha-me/plinth/issues/60)
- [#61 Expose extension metrics registration and recording APIs](https://github.com/gobha-me/plinth/issues/61)
- [#62 Add hard and crushing load tiers with metrics cross-validation](https://github.com/gobha-me/plinth/issues/62)

## Sidecar Platform

- [#63 Implement sidecar registration with single-use bootstrap authority](https://github.com/gobha-me/plinth/issues/63)
- [#64 Register sidecar capabilities in the resolver](https://github.com/gobha-me/plinth/issues/64)
- [#65 Re-evaluate sidecar registration and capability discovery](https://github.com/gobha-me/plinth/issues/65)
- [#66 Implement Tier 3 remote capability dispatch](https://github.com/gobha-me/plinth/issues/66)
- [#67 Contain sidecar failures with timeouts and circuit breakers](https://github.com/gobha-me/plinth/issues/67)
- [#68 Poll sidecar health and collect bounded metrics](https://github.com/gobha-me/plinth/issues/68)
- [#69 Build the sidecar administration panel](https://github.com/gobha-me/plinth/issues/69)
- [#70 Re-evaluate the completed sidecar platform](https://github.com/gobha-me/plinth/issues/70)

## High Availability

- [#71 Add durable node membership and heartbeats](https://github.com/gobha-me/plinth/issues/71)
- [#72 Detect stale nodes and fail closed through self-eviction](https://github.com/gobha-me/plinth/issues/72)
- [#73 Route extension capabilities across Plinth nodes](https://github.com/gobha-me/plinth/issues/73)
- [#74 Route sidecar capabilities across Plinth nodes](https://github.com/gobha-me/plinth/issues/74)
- [#75 Re-evaluate cross-node routing before HA completion](https://github.com/gobha-me/plinth/issues/75)
- [#76 Aggregate metrics across Plinth nodes](https://github.com/gobha-me/plinth/issues/76)
- [#77 Reconnect WebSocket clients across nodes with delta synchronization](https://github.com/gobha-me/plinth/issues/77)

## Storage, Notifications and Polish

- [#78 Provide extension-scoped file storage](https://github.com/gobha-me/plinth/issues/78)
- [#79 Expose file storage through the QuickJS capability layer](https://github.com/gobha-me/plinth/issues/79)
- [#80 Re-evaluate the storage API before platform polish](https://github.com/gobha-me/plinth/issues/80)
- [#81 Implement an in-app notification bus](https://github.com/gobha-me/plinth/issues/81)
- [#82 Provide allowlisted outbound HTTP capabilities](https://github.com/gobha-me/plinth/issues/82)
- [#83 Implement safe extension hot reload](https://github.com/gobha-me/plinth/issues/83)
- [#84 Harden the plinth validate command](https://github.com/gobha-me/plinth/issues/84)
- [#85 Re-evaluate the polished pre-1.0 platform](https://github.com/gobha-me/plinth/issues/85)
- [#86 Complete the pre-1.0 security assessment and remediation gate](https://github.com/gobha-me/plinth/issues/86)
- [#87 Finalize the extension author guide](https://github.com/gobha-me/plinth/issues/87)
- [#97 Implement extension user-deletion cleanup contract](https://github.com/gobha-me/plinth/issues/97)

## Continuous Quality

These issues may run alongside the outcome milestones once their dependencies
are ready.

- [#88 Stress reconnect and delta sync during an event storm](https://github.com/gobha-me/plinth/issues/88)
- [#89 Add property-based tests for RBAC permission logic](https://github.com/gobha-me/plinth/issues/89)
- [#90 Fuzz every JavaScript-to-C++ bridge boundary](https://github.com/gobha-me/plinth/issues/90)
- [#91 Add supported ThreadSanitizer CI for realtime and lifecycle concurrency](https://github.com/gobha-me/plinth/issues/91)
- [#92 Preserve PostgreSQL SQLSTATE typing through Drogon batch aborts](https://github.com/gobha-me/plinth/issues/92)
- [#95 Eliminate the recurring Drogon join-self teardown abort](https://github.com/gobha-me/plinth/issues/95)

## 1.0 Stable

- [#93 Stabilize the Plinth 1.0 API and operational contract](https://github.com/gobha-me/plinth/issues/93)

## Reconciliation notes

- LH-1 was already completed; its stale unchecked line was removed.
- Only ICD-0.4.5 X.12 and ICD-0.5.5 S.07 remain from the old crash-injection
  fixture umbrella; they are tracked together in #34.
- The duplicate pre-1.0 security-audit line now resolves to #86.
- Browser/client backfill is #32; DAST is #40; reconnect stress is #88.
- The source-code CSRF deferral is #33, and the previously unscheduled Drogon
  SQLSTATE limitation is #92.
- PR #94 candidate CI reproduced the Drogon join-self teardown abort; #95 and
  PR #96 subsequently resolved that reliability defect.
- The historical `users.deleted` / `users.list` contract had no implementation
  or issue; #97 now owns that work.
- Metrics persistence follows #57's partitioned PostgreSQL outcome. The older
  in-memory-only architecture sketch is historical, and load issue #62 is
  fuzzy until #57 and #61 establish the surface it must cross-validate.
