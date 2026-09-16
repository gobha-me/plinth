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

The implemented kernel provides identity, auth, groups, capability registry,
extension-scoped database access, realtime pub/sub, audit logging, package
lifecycle, and a sandboxed extension runtime. File storage, notifications,
general scheduled tasks, metrics, sidecars, and HA coordination are planned
contracts owned by linked GitHub issues, not current product claims. Everything
else is an extension.

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
| Language | C++23 | Required by CMake and the supported toolchain matrix |
| HTTP/WS | Drogon | Async, PG built-in, WebSocket, filter chains, actively maintained, MIT |
| Database | PostgreSQL (committed, no abstraction) | HA coordination, LISTEN/NOTIFY, schemas, advisory locks, JSONB, partitioning. See `architecture/03-data.md`. |
| JSON | nlohmann/json | Header-only, proven, architect has used in 3-4 projects |
| Scripting | QuickJS (ES2023) | Real language, sandboxed, memory/time limits, everyone knows JS |
| Frontend | Preact/htm | Lightweight, no build step, same language as extension scripts |
| Logging | spdlog | Header-only, async, multi-sink. Replaces Drogon's built-in logger. See `DESIGN-logging-subsystem.md`. |
| CLI | argparse (p-ranav) | Header-only, C++17+, subcommand support, MIT |
| Testing | Catch2 | C++ standard, header-only option, well-documented |
| Build | CMake | Industry standard for C++ |
| Allocator | Development container preloads jemalloc; production undecided | The kernel target does not link jemalloc directly. #35 owns allocator evidence and the production image decision. |
| Deployment | Development Docker Compose today; production OCI/Kubernetes planned | [#35](https://github.com/gobha-me/plinth/issues/35) owns the production image and [#36](https://github.com/gobha-me/plinth/issues/36) owns Kubernetes/Traefik support. |
| CI | GitHub Actions | GCC, Clang 20/21, ASan/UBSan, formatting/static analysis, CodeQL, and full image validation |

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
| `architecture/shutdown.md` | Production/test lifecycle ownership, dependency order, bounds, partial-startup unwind, and database/event-loop shutdown. |
| `architecture/extension-database-isolation.md` | Restricted runtime identities, provisioning, grants, migration guards, notification authority, and extension database ownership. |
| `architecture/websocket-authority.md` | WebSocket credential/RBAC lease, renewal, fail-closed delivery, and shutdown ownership. |

Parenthetical legacy section numbers in the right column are orientation only;
the pre-decomposition monolith is not retained in this repository. Cite current
sections by document name and local number, for example
`architecture/01-identity.md §2.1`.

The current architecture files and dedicated authority documents in the table
own present intent and shipped/planned classification. Design documents and
ICDs preserve the mechanism proposed for their delivery arc; they are
historical where a current architecture note, source owner, or regression test
records a superseding implementation. A still-active design detail must not
contradict its upward architecture contract.

---

## 4. Deployment

### 4.1 Single Binary

The kernel compiles to a single binary (or binary + assets directory).
Direct native dependencies include libc/libstdc++, Drogon, libpq, OpenSSL,
Argon2, and libzip; the supported production-image work in #35 owns the final
runtime closure, allocator decision, and provenance. QuickJS is linked into the
binary. The shell ZIP is a separately staged and installed companion asset.

### 4.2 Docker

The repository ships a development Dockerfile, a CI image, and a disposable
Docker Compose PostgreSQL service. It does not yet publish a supported
production image. [Issue #35](https://github.com/gobha-me/plinth/issues/35)
owns that deliverable, including the final base-image and allocator evidence.

### 4.3 Kubernetes

No supported Helm chart or Kubernetes deployment exists yet.
[Issue #36](https://github.com/gobha-me/plinth/issues/36) owns the supported
Kubernetes and Traefik contract after the production image and browser-origin
security boundary are ready. The HA topology in
`architecture/04-services-ha.md §6` is a planned contract, not a claim that
multi-node operation is currently supported.

---

## 5. Source Tree Layout

The current layout is descriptive. Names for unimplemented subsystems are not
frozen before their outcome issue defines them.

### 5.1 Current

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
    cap/
    capabilities/
    db/
    extensions/
    frontend/
    groups/
    js/
    lifecycle/
    packages/
    rbac/
    realtime/
    scheduled_tasks/
    security/
    shell/
    ws/

tests/kernel/   (mirrors src/kernel/ subdirectories)
tests/browser/  (production browser and retained-upgrade journeys)
tests/integration/
client/shell/
load-harness/
migrations/
cmake/
docker/
docs/
```

### 5.2 Planned additions

| Candidate area | Outcome owner |
|----------------|---------------|
| File/blob storage | [#78](https://github.com/gobha-me/plinth/issues/78), [#79](https://github.com/gobha-me/plinth/issues/79) |
| General scheduler and maintenance tasks | [#58](https://github.com/gobha-me/plinth/issues/58), [#59](https://github.com/gobha-me/plinth/issues/59) |
| Retained metrics and extension metrics | [#57](https://github.com/gobha-me/plinth/issues/57), [#61](https://github.com/gobha-me/plinth/issues/61) |
| Notification bus | [#81](https://github.com/gobha-me/plinth/issues/81) |
| Sidecar registration and remote dispatch | [#63](https://github.com/gobha-me/plinth/issues/63) through [#70](https://github.com/gobha-me/plinth/issues/70) |
| Multi-node HA | [#71](https://github.com/gobha-me/plinth/issues/71) through [#77](https://github.com/gobha-me/plinth/issues/77) |

`ws/` and `realtime/` are intentionally separate siblings. `ws/` owns the
transport, connection authority, and subscriptions; `realtime/` owns durable
outbox consumption, coalescing, replay, and broker behavior. The former naming
question is resolved.

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
- Metrics (concept retained, mechanism reopened; fuzzy #57/#61 now own
  PostgreSQL retention, extension recording, and export decisions)
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

## 8. Issue-owned follow-ups and observations

GitHub Issues are the executable backlog. This section is a navigation aid,
not a second task list:

- Production hostname, TLS, proxy, and deployment choices belong to
  [#36](https://github.com/gobha-me/plinth/issues/36).
- Realtime retention adequacy is measured by reconnect-under-storm work in
  [#88](https://github.com/gobha-me/plinth/issues/88).
- Capability type compatibility and shell SDK versioning are release-contract
  questions for [#93](https://github.com/gobha-me/plinth/issues/93).
- Kernel rule presentation belongs to the RBAC administration surface in
  [#52](https://github.com/gobha-me/plinth/issues/52); compatibility freezes in
  #93.
- Extension documentation placement and publication belong to
  [#87](https://github.com/gobha-me/plinth/issues/87).

A central package registry remains an observation, not a committed deliverable.
Git-based distribution is sufficient until real ecosystem demand produces a
bounded issue. It must not be inferred as scheduled from this document.

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
- ~~Alpine vs Debian~~ → the current development container is Debian-based and
  preloads jemalloc; #35 owns the production base and allocator decision.
- ~~PG abstraction layer~~ → Killed. Commit to PG.
- ~~Extension isolation~~ → PG schema per extension.
- ~~Example packages~~ → No. Documentation instead. (§6 above.)
- ~~RBAC test execution~~ → Two-phase. (`architecture/01-identity.md §2`.)
- ~~Realtime per-row events~~ → Debounced change summaries.
  (`architecture/03-data.md §3`.)
- ~~Capability call overhead~~ → Three-tier resolution with caching.
  (`architecture/02-capabilities.md §1`.)
- ~~Metrics storage~~ → the former no-PG/in-memory-only decision is
  superseded. Fuzzy #57/#61 own retained storage, extension recording, and
  export behavior. (`architecture/04-services-ha.md §3`.)
- ~~URL space ownership~~ → Kernel owns a fixed list of prefixes.
  (`architecture/05-extensions.md §2`.)
- ~~Shell privilege model~~ → Shell is a built-in extension. No
  kernel-privileged shell code path. (`architecture/06-frontend.md §1`.)
- ~~Anonymous identity~~ → `UserContext::anonymous()` is first-class;
  member of `everyone` only. (`architecture/01-identity.md §3`.)
- ~~QuickJS default limits~~ → 16 MiB memory, 100 ms CPU, 30 s wall
  time, 256 stack frames, and call depth 8 in `default_runtime_limits()`.
- ~~Admin rule mechanism~~ → the built-in admin group receives the explicit
  kernel-owned bootstrap rules; manifests can opt extension rules into its
  defaults, and `kernel.admin` remains the capability universal match.
- ~~`ws/` vs `realtime/` naming~~ → retain the two sibling trees with the
  ownership split described in §5.

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

The current files in `docs/architecture/` own the architecture contract.
Deviations require a reviewed revision of the relevant current document.
Design docs and ICDs trace upward through that tree and do not override a
newer current-state correction.

---

## 10. Cross-Document Trust Boundary

The kernel's contract ends at the kernel API (the interfaces described
in `architecture/02-capabilities.md`, `architecture/03-data.md`, and the
HTTP surfaces in `architecture/05-extensions.md §2`). Everything above
that line — the shell, the admin extension, application extensions — is
extension territory, governed by the package system and the panel SDK,
not by kernel-privileged code paths. The single exception is the kernel's own
bootstrap of the bundled shell package on first boot. The install uses the
package lifecycle, but the canonical name, bundled provenance, and explicit
kernel-owned upgrade path remain protected from ordinary HTTP/admin callers. A
bundled admin package is planned design, not current behavior.
See `architecture/06-frontend.md §1` and
`architecture/05-extensions.md §1.4`.

---

**This architecture tree is the current source of truth.** Design documents
and ICDs trace upward to it and remain valuable historical delivery evidence.
When shipped source/tests differ from a current contract, the mismatch must be
resolved explicitly rather than silently treating either layer as current.
