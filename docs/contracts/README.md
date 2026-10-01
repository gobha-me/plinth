# Living subsystem contracts

Status: bounded adoption approved by the maintainer on 2026-10-01 in
[#41](https://github.com/gobha-me/plinth/issues/41). This is the decision and
navigation index, not a complete subsystem contract or a platform/API freeze.
The [historical proposal](../discussion/DISCUSSION-living-subsystem-contracts.md)
records the motivation and alternatives; its candidate counts are not current
readiness assessments.

## Authority and conflicts

| Document role | Authority |
|---|---|
| [Architecture](../ARCHITECTURE.md) and its owning sections | Why, system shape, trust boundaries, commitment bands, and explicit shipped/planned classification. |
| Ratified living subsystem contract | Current protocol behavior within its reviewed scope and baseline, consistent with the architecture. Drafts and this index do not have that authority. |
| Active approved ICD | The bounded delivery delta being implemented, including planned additions and recorded deviations. It does not silently override a ratified current contract. |
| Shipped ICD and design history | Delivery rationale, original scope, deviations, and traceability. Supersession must be explicit for the affected surface; unrelated unsuperseded content is retained. |
| Source and tests | Evidence of implementation and verification, not permission to promote accidental behavior into policy. |

If these disagree, identify the owning scope, the reviewed baseline, and the
specific discrepancy. Do not choose a winner just because a file or test is
newer. Trace an explicit reconciliation: repair implementation, correct an
erroneous contract, or obtain architect approval for a structural change.
Record the decision and any narrowly scoped supersession in the owning docs.
Until resolved, do not claim that the disputed behavior is ratified or covered.
Medium/fuzzy architecture and unsupported/planned behavior remain distinct
from supported current protocol.

## Existing current-authority entry points

These documents retain their present paths and authority. They are linked,
not copied, moved, or declared newly complete/ratified subsystem fills by #41.

| Reader journey | Current owner |
|---|---|
| Production/test startup, cancellation, bounded teardown and dependency ownership | [Shutdown ownership](../architecture/shutdown.md) |
| Restricted extension database identity, provisioning, migrations and runtime-client ownership | [Extension database authority](../architecture/extension-database-isolation.md) |
| WebSocket credential/RBAC lease, renewal, fail-closed delivery and shutdown ownership | [WebSocket authority lifetime](../architecture/websocket-authority.md) |

For other surfaces, start at the architecture's
[document map](../ARCHITECTURE.md#3-document-map), then read the relevant
active ICDs and their implementation/deviation history. Absence from this
index does not remove existing obligations or prove that a subsystem lacks
implementation. Inclusion is not completeness certification.

## Naming, placement and admission

New subsystem documents use descriptive, unnumbered kebab-case names under
`docs/contracts/`. Group tightly coupled reader journeys rather than creating
one file per historical milestone, C++ header, or small function. Keep the
existing authority documents above where they are; linking them avoids two
competing copies of the same contract.

Each initial fill requires a separate bounded plan, source/test reconciliation,
independent review and maintainer/architect ratification. A candidate or draft
must say so explicitly. A ratified document identifies its scope, reviewed
source baseline, review/ratification reference, current limitations and
verification evidence. Ratification is not a release, semver commitment, or
automatic approval of future changes.

The protocol body must stand on its own, together with its stated architecture
dependencies: interfaces and data shapes, authority, state transitions,
ownership/lifetime, ordering, cancellation, errors and limits as applicable.
Distinguish supported, planned and unsupported behavior; do not fill missing
guarantees by guessing from implementation. Link independent verification and
historical delivery/deviation records, rather than substituting a test count
or a header pointer for the protocol. Engine-specific implementation notes are
clearly subordinate and cannot redefine that body. A reader should be able to
design a compatible implementation in another stack without reverse-engineering
the source to discover the protocol. Unreconciled gaps keep the document a draft.

## Maintenance

Update the owning ratified contract in the same PR that changes its behavior,
alongside relevant tests and the active ICD's delivery/deviation record. A
behavior-changing PR must not leave its current contract update to a later
re-evaluation. Structural/trust changes still require architect approval.

Re-evaluation checks merged changes for missed contract additions and
cross-ICD drift, reconciles them against source/tests, and rolls the approved
current behavior forward. Preserve shipped ICDs/designs as historical records;
use explicit supersession references rather than rewriting their original
delivery claims as if they had always described today's implementation.
See [methodology](../METHODOLOGY-llm-assisted-development.md#312-living-contract-roll-forward).

## Initial candidate order, not completed fills

1. Auth, sessions and PATs as one reader journey; proposed name
   `auth-sessions-pats.md`.
2. Capability registry and dispatch contract; proposed name
   `capability-registry.md`, after the first fill's reconciliation and review.

Neither file is created or ratified by this decision. Each needs its own plan
and assessment of current readiness before authoring. Further candidates need
the same admission process; the historical six/seven-subsystem count does not
admit them automatically. This change does not modify production behavior,
APIs, database schema, compatibility/version policy, releases, or settings.
