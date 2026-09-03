# Plinth — Architecture (Index)

**Status:** Living document. Decomposed from `ARCHITECTURE-plinth-v3.md` on 2026-04-16.
**Methodology:** LLM-Assisted Development (`METHODOLOGY-llm-assisted-development.md`)
**Depends on:** `DESIGN-rbac-philosophy.md`

This document is the entry point to the Plinth architecture. It contains the
manifesto, stack, deployment model, source-tree layout, cross-doc conventions,
and the map of which sub-document owns which question. Substantive content
lives in the files under `architecture/`.

---

## 1. Purpose and Philosophy

A self-hosted, extensible platform built on a kernel-first philosophy:
**the kernel ships empty; extensions are the product.**

The kernel provides identity, auth, groups, capability registry, database,
file storage, realtime pub/sub, audit logging, notifications, scheduled
tasks, metrics, and HA coordination. Everything else is an extension.

This is a from-scratch rewrite. The architecture is informed by lessons
learned from three prior iterations (ArcadeCtl, Chat Switchboard, Armature)
and codifies decisions that worked while discarding what didn't.

### What This Is Not

- Not a team collaboration platform (no Teams concept).
- Not a workflow engine.
- Not a SaaS product (self-hosted, personal/small-group use).

---

## 2. Core Stack

| Layer | Technology | Rationale |
|-------|-----------|-----------|
| Language | C++20 (targeting C++23 where supported) | Architect proficiency (3.8/5), direct system access, catches LLM mistakes earlier |
| HTTP/WS | Drogon | Async, PG built-in, WebSocket, filter chains, actively maintained, MIT |
| Database | PostgreSQL (committed, no abstraction) | HA coordination, LISTEN/NOTIFY, schemas, advisory locks, JSONB, partitioning. See `architecture/03-data.md`. |
| JSON | nlohmann/json | Header-only, proven, architect has used in 3-4 projects |
| Scripting | QuickJS (ES2023) | Real language, sandboxed, memory/time limits, everyone knows JS |
| Frontend | Preact/htm | Lightweight, no build step, same language as extension scripts |
| Logging | spdlog | Header-only, async, multi-sink. Replaces Drogon's built-in logger. See `DESIGN-logging-subsystem.md`. |
| CLI | argparse (p-ranav) | Header-only, C++17+, subcommand support, MIT |
| Testing | Catch2 | C++ standard, header-only option, well-documented |
| Build | CMake | Industry standard for C++ |
| Allocator | jemalloc (production) | Avoids musl/Alpine malloc performance issues under QuickJS allocation patterns. Benchmark musl vs glibc vs jemalloc during metrics work. |
| Deployment | Docker/Helm/K8s | Same homelab infrastructure. Debian slim base image (not Alpine — allocator concerns). |
| CI | Gitea CI (DinD runners) | Existing infrastructure |

---

## 3. Document Map

The architecture is split into the following documents. Each answers one
kind of question. When a design doc or ICD traces upward, it should cite
the specific sub-document and section, not this index.

| Document | Owns |
|----------|------|
| `architecture/01-identity.md` | Identity and authentication (§3.1), groups and RBAC (§3.2), anonymous identity (§3.2.1), user deletion cleanup contract. |
| `architecture/02-capabilities.md` | Capability registry (§3.3), kernel standard library / QuickJS APIs (§3.8), capability call flow. |
| `architecture/03-data.md` | Database (§3.4), storage (§3.5), file upload/download HTTP surface, realtime pub/sub (§3.6), PG schema layout. |
| `architecture/04-services-ha.md` | Audit logging (§3.7), scheduled tasks (§3.10), notifications (§3.11), metrics (§3.12), sidecar contract (§4.3), high availability (§5), security model (§6). |
| `architecture/05-extensions.md` | Package structure (§4.1), QuickJS runtime (§4.2), reserved URL prefixes (§3.13), cross-cutting composition framework (traits/slots/augments), deferred public HTTP options. |
| `architecture/06-frontend.md` | Frontend architecture (§7), shell-as-extension, `frontend.mount`, extension asset serving, design token serving, BYO frontend stance, panel-system summary. |

Section numbers in the right column refer to the pre-decomposition monolith
(`ARCHITECTURE-plinth-v3.md`) for migration-period orientation. After the
decomposition lands, cite new sections by document-name + local section
number: e.g. `architecture/01-identity.md §2.1` rather than `§3.1`.

Subsystems with their own design documents (`DESIGN-rbac-philosophy.md`,
`DESIGN-capability-registry.md`, `DESIGN-quickjs-bridge.md`,
`DESIGN-logging-subsystem.md`, `DESIGN-packages-v04x.md`,
`DESIGN-shell-v06x.md`, `DESIGN-admin-v06x.md`, `DESIGN-sharing-v011x.md`)
continue to own their own authoritative content; the architecture files
define the contract, the design docs define the mechanism.

---

## 4. Deployment

### 4.1 Single Binary

The kernel compiles to a single binary (or binary + assets directory).
Runtime dependencies: libc, libpq, jemalloc.

### 4.2 Docker

Official Docker image. **Debian slim** base (avoids musl allocator
performance issues). jemalloc compiled in.

### 4.3 Kubernetes

Helm chart. Horizontal scaling via replicas (HA model — see
`architecture/04-services-ha.md §5`). PG can be in-cluster or external.

---

## 5. Source Tree Layout

The layout below has two passes: (a) *current* — directories that
actually exist as of the most recently shipped milestone — and (b)
*forward-looking* — directories expected by later milestones. Code
sessions write into (a); new milestones expand (a) by pulling
directories from (b) as they land. The forward-looking layout is a
commitment to naming, not a commitment to populate on any schedule.

### 5.1 Current (through 0.2.5)

```
src/
  kernel/
    main.cpp
    config.hpp/.cpp
    logging.hpp/.cpp
    auth/
      crypto.hpp/.cpp
      handlers.hpp/.cpp
      middleware.hpp/.cpp
      pat_handlers.cpp
      rate_limiter.hpp/.cpp
    audit/
      handlers.hpp/.cpp
    capabilities/
      batch.hpp/.cpp
      bootstrap.hpp/.cpp
      listener.hpp/.cpp
      parser.hpp/.cpp
      registration.hpp/.cpp
      resolution.hpp/.cpp
      types.hpp
      validation.hpp/.cpp
    db/
      bootstrap.hpp/.cpp
    groups/
      handlers.hpp/.cpp
    rbac/
      enforcement.hpp/.cpp
    ws/
      auth_flow.hpp/.cpp
      close_codes.hpp
      conn_state.hpp
      connection_registry.hpp/.cpp
      events_controller.hpp/.cpp
      heartbeat.hpp/.cpp
      messages.hpp
      publish.hpp/.cpp
      registration.hpp/.cpp
      subscriptions.hpp/.cpp

tests/kernel/   (mirrors src/kernel/ subdirectories)
migrations/
cmake/
docker/
helm/
docs/
```

### 5.2 Forward-looking (milestone additions)

As each milestone lands, new directories appear under `src/kernel/`.
Expected names and the milestone that introduces each:

| Directory | Contents (summary) | Introduced by |
|-----------|--------------------|---------------|
| `scripting/` | QuickJS runtime pool, C++↔JS bridge, kernel standard library injection | 0.3.0–0.3.2 |
| `packages/` | Package validation, manifest parsing, install/enable/disable lifecycle | 0.4.x |
| `realtime/` (see open question below) | Debounced coalescer, delta-sync, `plinth.events` writer | 0.5.x |
| `storage/` | Filesystem backend + quotas + the HTTP surface in `architecture/03-data.md §2.3` | 0.10.0–0.10.1 |
| `scheduler/` | Cron parser, PG advisory-lock scheduler, default tasks | 0.7.x |
| `metrics/` | In-memory counters + histograms + `/metrics` Prometheus endpoint | 0.7.x |
| `notifications/` | In-app notification bus over WebSocket | 0.10.2 |
| `ha/` | `plinth.node_registry`, heartbeat writer, stale-node sweep | 0.9.x |
| `api/` | Top-level HTTP route registration aggregator (if the `register_*_routes()` fan-out outgrows `main.cpp`) | when needed |

A forthcoming `dispatch/` directory — the 0.8 remote-proxy path for
Tier 3 resolution plus the sidecar HTTP client — is also anticipated.
Whether remote-proxy code lives there, under `capabilities/`, or
under `sidecar/` is an implementation decision for 0.8.

### 5.3 Open question — `ws/` vs `realtime/` naming

The 0.1.6 WebSocket work landed in `src/kernel/ws/`. The pre-existing
§5 sketch named the same space `realtime/`. 0.5.x lands the
debounced coalescer, sequence numbers, and `plinth.events` writer —
the work that makes "realtime" the more accurate name. At 0.5.0,
the architect will either rename `ws/` → `realtime/` (and collapse
0.1.6's ten files into a coalescer-sibling layout) or keep `ws/`
and update this section plus §8. Decided at 0.5.0; tracked in §8.

---

## 6. No Example Packages

The kernel ships with **zero** example packages. This is deliberate.

Example packages in prior iterations caused scope creep. We ship an
`EXTENSION-GUIDE.md` that walks through building a minimal extension
from scratch. A different LLM session, given only the architecture docs,
the ICDs, and the extension guide, should be able to create a working
extension from scratch. If it can't, the docs are insufficient.

---

## 7. What Transfers from Armature

### Transfers directly

- PG HA model (LISTEN/NOTIFY, heartbeat, leaderless)
- Sidecar 4-endpoint contract
- Bootstrap token security model
- Panel system concept
- Audit logging pattern
- LLM-assisted development discipline

### Transfers with modification

- Package manifest (split files, versioned capability-based deps)
- RBAC (simplified: groups only, extension-registered rules, two-phase testing)
- Metrics (in-memory + Prometheus, not PG-stored)
- Frontend (same Preact/htm, `plinth.*` SDK namespace)
- Realtime (first-class, debounced change streams, delta sync)
- Database isolation (PG schemas instead of table prefix checking)
- **Shell-as-extension packaging.** New in Plinth — the reference frontend
  is installed through the standard package lifecycle on first boot.
  See `architecture/06-frontend.md`.

### Does not transfer

- Starlark (replaced by QuickJS)
- Teams
- Workflow engine
- Roles
- `depends`/`requires` package dependency model
- Go-specific patterns
- SQLite
- Database abstraction layer
- Example packages

---

## 8. Open Questions

Tracked here as a backlog. When resolved, the resolution lives in the
relevant sub-document and the line is deleted from here (or moved to §8.1
with a one-line pointer). Rejected proposals are recorded where they would
have belonged, as short "rejected" notes, per the conventions in §9.

1. **Domain.** `plinth.dev`? `plinth.run`? Check availability and register.
2. **QuickJS memory/time defaults.** Current hypothesis: 64MB memory,
   5000ms CPU, 1000 stack depth. Needs benchmarking.
   (`architecture/05-extensions.md §2`.)
3. **Realtime delta sync retention.** Default 1h for `plinth.events`.
   Is that sufficient? (`architecture/03-data.md §3`.)
4. **Package registry.** `plinth.dev` (or wherever) as a registry? Or
   just git-based distribution for now? (`architecture/05-extensions.md §1`.)
5. **Capability type system depth.** Current: type strings. Sufficient
   for v1. Shake out in 0.2.x testing.
   (`architecture/02-capabilities.md §1`.)
6. **Exact kernel rule set.** Granularity and naming of kernel-level
   rules (`kernel.admin`, `system.backup.run`, `packages.manage`, etc.).
   To be refined during 0.1.7–0.7 while respecting
   `DESIGN-rbac-philosophy.md`.
7. **Admin rule mechanism.** Whether `admin` receives a single powerful
   rule or is granted a curated set of high-privilege rules.
   (`architecture/01-identity.md §2`.)
8. **Shell SDK versioning mechanism.** How an extension declares "built
   against shell SDK version X." Additive manifest field, probably 0.7+.
   (`architecture/06-frontend.md`.)
9. **Extension-design-docs repository.** By ~0.8.0, extension design
   docs should live in a separate Gitea project (docs-only, isolated
   from kernel code) so the cross-cutting composition arc has a home
   for extension authors to declare surface traits. Project-infrastructure
   decision, not architecture. (`architecture/05-extensions.md §3`.)
10. **`ws/` vs `realtime/` kernel directory name.** 0.1.6 landed the
    WebSocket code in `src/kernel/ws/`; §5's original sketch named
    the same space `realtime/`. 0.5.x lands the coalescer + sequence
    numbers + `plinth.events` writer that make "realtime" the more
    accurate umbrella. Decide at 0.5.0 whether to rename `ws/` →
    `realtime/` (and collapse existing files into a coalescer-sibling
    layout) or keep `ws/` and update §5 accordingly. Surfaced by
    RE-EVAL following 0.2.x; deferred until 0.5.0 has real design
    pressure. (§5.3.)

### 8.1 Resolved (pointer)

- ~~SQLite dual-store~~ → Dropped. PG-only. (`architecture/03-data.md §1`.)
- ~~Capability versioning~~ → `namespace:version:function`. Exact match.
  (`architecture/02-capabilities.md §1`.)
- ~~Data classification~~ → Deferred to post-v1.
- ~~Sidecar HA routing~~ → Proxy to correct node. Circuit breaker.
  (`architecture/04-services-ha.md §4`, `architecture/02-capabilities.md §1`.)
- ~~Scheduled task distribution~~ → PG advisory locks, first-grab.
  (`architecture/04-services-ha.md §2`.)
- ~~RBAC version awareness~~ → Rules are NOT version-aware.
  (`architecture/01-identity.md §2`.)
- ~~Database vs storage~~ → Separate. DB = PG. Storage = file/blob.
  (`architecture/03-data.md §1`, `architecture/03-data.md §2`.)
- ~~Alpine vs Debian~~ → Debian slim. jemalloc. (§2 above.)
- ~~PG abstraction layer~~ → Killed. Commit to PG.
- ~~Extension isolation~~ → PG schema per extension.
- ~~Example packages~~ → No. Documentation instead. (§6 above.)
- ~~RBAC test execution~~ → Two-phase. (`architecture/01-identity.md §2`.)
- ~~Realtime per-row events~~ → Debounced change summaries.
  (`architecture/03-data.md §3`.)
- ~~Capability call overhead~~ → Three-tier resolution with caching.
  (`architecture/02-capabilities.md §1`.)
- ~~Metrics storage~~ → No PG. In-memory + Prometheus endpoint.
  (`architecture/04-services-ha.md §3`.)
- ~~URL space ownership~~ → Kernel owns a fixed list of prefixes.
  (`architecture/05-extensions.md §2`.)
- ~~Shell privilege model~~ → Shell is a built-in extension. No
  kernel-privileged shell code path. (`architecture/06-frontend.md §1`.)
- ~~Anonymous identity~~ → `UserContext::anonymous()` is first-class;
  member of `everyone` only. (`architecture/01-identity.md §3`.)

---

## 9. Conventions for Future Architectural Changes

These conventions govern how new decisions and revisions land in this
document set. They are themselves editable — proposed changes go through
an architecture session, not a code session.

### 9.1 In-place edits are the default

Single-document changes — new subsections, clarifications, new reserved
fields, new table rows — are integrated directly into the affected
`architecture/NN-*.md` file. `CHANGELOG.md` (at `docs/` root) gets one
line per change:

```
2026-04-16 | ARCHITECTURE decomposition | the maintainer | 01,02,03,04,05,06
  Split ARCHITECTURE-plinth-v3.md into six sub-documents. Integrated
  URL-layout patch. Added file upload surface, user-deletion contract,
  design-token serving, cross-cutting composition framework.
```

No patch artifacts for single-document changes.

### 9.2 Patch artifacts are only for atomic multi-document changes

When a decision spans three or more documents and must be reviewed as a
coherent unit before being split — the URL-layout patch is the canonical
example — a temporary `ARCHITECTURE-patch-{topic}.md` may be produced
for review. It is deleted after integration. The source of truth is
always the architecture docs themselves, never the patch.

A patch artifact is not a permanent document. If one exists in the repo
for more than one architect-review cycle, something has gone wrong with
the integration discipline.

### 9.3 Resolved decisions live at the point of effect

Once a decision is made, the decision lives in the section of the
architecture doc where it takes effect, not in a "decisions log." §8.1
above is a pointer index, not a substantive record. The substantive
record is the section that claims the decision.

### 9.4 Deferred decisions live in a "Deferred" section in the owning doc

Not in an appendix to a separate artifact. Example: the deferred public
HTTP options (share primitive, site-host extension) live in
`architecture/05-extensions.md §Deferred`, not in a standalone
"deferred options" file.

### 9.5 Rejected decisions are recorded with enough context to not re-open them

Rejected proposals get a short "rejected" note in the section where they
would have belonged had they been accepted, with a one-sentence
rationale. This prevents a later session from independently re-deriving
the same rejected proposal and wasting review time. Example:
"Extensions registering arbitrary HTTP routes: rejected. The capability
registry replaces this pattern; re-introducing arbitrary routing defeats
the capability-model guarantees."

### 9.6 Section numbers are local

Each sub-document uses local section numbers starting from §1. Cross-doc
references cite by document name plus local number:
`architecture/01-identity.md §2.1`. This is what makes the decomposition
renumber-stable: inserting a new §2.2 in the identity doc does not
renumber §3.1 in the capabilities doc.

### 9.7 Authority stands

All architecture docs inherit the authority of the original monolith.
Deviations from any of them require a new architecture session and a
revision of the relevant document. Design docs and ICDs trace upward
through the document tree, not horizontally.

---

## 10. Cross-Document Trust Boundary

The kernel's contract ends at the kernel API (the interfaces described
in `architecture/02-capabilities.md`, `architecture/03-data.md`, and the
HTTP surfaces in `architecture/05-extensions.md §2`). Everything above
that line — the shell, the admin extension, application extensions — is
extension territory, governed by the package system and the panel SDK,
not by kernel-privileged code paths. The single exception is the
kernel's own bootstrap of the bundled shell and admin packages on first
boot; that bootstrap runs the packages through the **standard** install
lifecycle and is indistinguishable afterward from any other install.
See `architecture/06-frontend.md §1` and
`architecture/05-extensions.md §1.4`.

---

**This document tree is the permanent source of truth.** All ICDs,
design documents, and implementation sessions must conform to the RBAC
philosophy defined in `DESIGN-rbac-philosophy.md` and the contracts
established in the ICDs. Any deviation requires a new architecture
session and revision of the relevant architecture document.
