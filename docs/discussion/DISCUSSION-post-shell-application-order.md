# Post-Shell Application Build Order

**Status:** Discussion capture — not a design doc, not authoritative
**Source:** 2026-04-17 session
**Participants:** the maintainer (Architect) + Claude (Architecture Session)
**Feeds into:** Per-extension design docs as each milestone arrives;
ROADMAP.md additions post-0.6 once the shell is shipped.

---

## 0. Scope Note

This document orders the extensions Plinth will grow after the shell
and admin ship in 0.6.x. Each entry is a future Scale-2 or Scale-3
design effort in its own right; this document only orders them and
notes the dependencies that drive the order. The sequencing here is
not a commitment — it is a considered ordering based on current
understanding. Re-evaluation sessions refine it as milestones close.

Plinth-the-kernel's job regarding this list is narrow: the kernel
work in 0.1.x through 0.6.x provides the substrate. Every extension
below consumes that substrate without asking the kernel for new
primitives, with two exceptions called out explicitly (step-up auth
in §9, multi-identity persona RBAC in the companion document
`DISCUSSION-persona-rbac.md`).

---

## 1. Build Order

### 1. Files

First because it is the pattern proof. Files exercises content-type
handler resolution, float panels, the storage HTTP surface, and the
shell's panel SDK. Every later extension benefits from Files having
pressure-tested these paths first. Also the extension with the
clearest mental model — a folder tree and file operations — so if
the platform fights Files, the platform is wrong.

**Depends on:** shell 0.6.x complete, storage HTTP surface (0.10.0–
0.10.1).
**Ships:** a minimal viable file manager panel. Not a replacement
for Nautilus or Finder; a storage browser that makes Plinth's
storage model usable.

### 2. Notes

First personal-use test. Obsidian-shaped data model, no AI
dependency, markdown storage via the file system Files exposes.
This is the validation milestone for whether any Plinth extension
sustains non-architect use. If Notes becomes at least a sometimes-
driver for the architect, the platform has earned the right to
grow. If Notes does not, that is strong signal about whether any
extension will.

**Depends on:** Files (for storage UI primitives, content-type
registration), shell SDK.
**Ships:** Obsidian-equivalent personal notes. Folder tree, markdown
editor, wiki-links, basic search.

### 3. Chat

Human-to-human messaging. No AI in v1. The discipline of shipping
chat without AI — one version that establishes the chat data model,
message rendering, thread structure, and multi-user mechanics — is
itself valuable. It forces the chat extension to stand on its own
merits rather than being "LLM chat disguised as multi-user."

**Depends on:** realtime pub/sub (0.5.x), identity/RBAC.
**Ships:** multi-user chat, threaded conversations, message
history, realtime presence.
**Notes:** v1 will feel slightly thin compared to existing
messaging products the household uses. This is acceptable and
expected. AI features arrive through ai-bridge in a later
sub-version, not by merging the chat extension with an LLM
provider.

### 4. ai-bridge (skeleton)

Minimal viable ai-bridge. Single-provider (Venice), chat-only
(`ai:1:chat`), no fallback chains, no quota tracking, no
cost tracking. Enough to unblock downstream extensions; the
fuller design doc in DISCUSSION-ai-bridge.md lists the features
that arrive in sub-versions.

**Depends on:** capability registry (0.2.x), QuickJS runtime (0.3.x),
realtime bus for streaming responses.
**Ships:** `ai:1:chat` capability that routes to Venice, with
streaming over the realtime bus. Sufficient for SWORD and the
first game to call AI.
**Notes:** build the skeleton first rather than the full ai-bridge
because the full version has several tricky edges (credential
handling at rest, provider fallback semantics, cost-model
updates) that could stall for weeks. The skeleton unblocks 5–6
and the full version grows underneath them.

### 5. SWORD

AI-assisted bible study. The LLM-commentary piece is the
differentiator — a SWORD reader without commentary is a bible
reader, and those exist in polished form elsewhere. The
commentary makes it a study tool.

**Depends on:** Files (for SWORD module storage), Notes (for
study note integration), ai-bridge skeleton (for commentary
calls).
**Ships:** SWORD module reading, passage navigation, LLM
commentary on selected passages, study note capture that lives
alongside notes in the Notes extension.
**Motivation:** personal daily-use driver for the architect.
This is the use case that tests whether Plinth earns daily use
beyond pure-hobbyist interaction.

### 6. Confabulation (first game)

The first game from `DISCUSSION-llm-games.md §3.1`. Smallest
state complexity, highest conceptual clarity, fastest round loop.
Ships the Plinth-games thesis with minimal risk. Household plays
it or they do not; either outcome informs whether to build more
games.

**Depends on:** ai-bridge skeleton, chat (for lobby/round-control
messaging), identity (players), realtime (round state).
**Ships:** multiplayer Confabulation with 2–8 player rounds,
session history, basic scoring.
**Why here, not slot 9:** games are Plinth's household-facing
differentiator. Shipping a game between SWORD and ai-bridge
fleshed-out gives the architect (and household) something
Plinth-native to actually play. Waiting until slot 9 risks the
game never shipping because it is always six months away.

### 7. ai-bridge (full)

Extend the skeleton: multi-provider support (OpenRouter,
Anthropic, OpenAI, optional local sidecars), fallback chains,
per-user and per-capability provider selection, cost tracking,
rate limiting, the full credential vault pattern. Full details
live in `DISCUSSION-ai-bridge.md`.

**Depends on:** ai-bridge skeleton shipped and proven by SWORD
and Confabulation usage, so the full version has real callers
to validate against.
**Ships:** production-viable ai-bridge with provider routing,
cost tracking, admin configuration UI.

### 8. cluster-manager v1 (read-only)

LLM-assisted homelab control plane. Pulls metrics and state from
Ceph, k8s, and the Minisforum nodes into a single Plinth panel,
with ai-bridge-backed "explain this to me" assistance. Read-only
in v1 — no destructive operations. Useful immediately and does
not require the step-up auth primitive (§9) that destructive
operations need.

**Depends on:** ai-bridge full, sidecar tier (0.8.x) for the
actual scrapers/collectors.
**Ships:** a panel per managed system (Ceph, k8s, nodes),
metrics visualization, LLM-assisted "what is wrong with this"
query interface, audit trail of every query.
**Why this is interesting beyond the architect:** this extension
is the most likely of the list to appeal to other self-hosters.
"Self-hosted LLM-assisted homelab control plane" is a specific
thing the r/homelab audience would try, and it does not exist
off-the-shelf.

### 9. Step-up auth primitive (kernel work)

Kernel pattern for "this capability call requires re-confirmation
or re-authentication before execution." Needed by cluster-manager
v2's destructive operations, persona impersonation (see
`DISCUSSION-persona-rbac.md`), admin operations like uninstall,
and any future capability where "user is authenticated and
authorized" is not a strong enough gate.

**Depends on:** capability registry, RBAC.
**Ships:** kernel primitive exposed to extensions as a capability
wrapper. A capability declared as step-up-required triggers a
confirmation flow before execution; the result is cached for a
short window per-capability or per-session.
**Why this is kernel work, not extension work:** step-up auth is
a pattern multiple extensions will need, and each extension
reinventing it will produce subtly-different confirmation
flows. A single kernel primitive with one audited implementation
is safer than five extension-local versions.

### 10. cluster-manager v2 (destructive operations)

Extends v1 with the ability to execute destructive operations
(restart pod, rebalance Ceph pool, power-cycle a node, apply a
k8s manifest). Every destructive capability is step-up-gated per
§9. Every execution is audited with the query that triggered it,
the LLM response chain, and the user confirmation.

**Depends on:** cluster-manager v1 shipped, step-up auth primitive.
**Ships:** the full homelab control plane. This is the extension
that most plausibly earns daily use from the architect and
potentially attracts external users.

### 11. Coder-workspace sidecar

A sidecar holding the development environment — storage, build
tools, compilers, git, language servers — exposed to Plinth
through a panel UI. The sidecar is where Plinth extensions
themselves get developed, and "publish" writes a built extension
package into Plinth's package system.

**Depends on:** ai-bridge full (for LLM-assisted coding), sidecar
tier (0.8.x), package system (0.4.x).
**Ships:** web-accessible dev environment, LLM-assisted coding
against the architect's own codebases, direct publish path into
Plinth's package system.
**Why here, not earlier:** the extension is complex and benefits
from every preceding primitive being solid. Also: building
extensions in the coder-workspace requires Plinth to be running
somewhere you would want to expose a sidecar — if Plinth is not
yet a daily-driver environment, the coder-workspace has no
reason to exist.

### 12. Metrics dashboard

A shell-integrated dashboard for Plinth's own metrics plus
extension-provided metrics. Moved later in the ordering because
the value proposition is "I already have to open Grafana, let
me just not," which requires enough other extensions to have
shipped that the dashboard has something substantive to show.

**Depends on:** metrics primitive (0.7.x), enough extensions to
justify the dashboard.
**Ships:** Plinth-native metrics UI, extension metric
registration, alerting integrations.

### 13. Remaining ai-* extensions

Bucket for:
- LLM personas with memory management (see
  `DISCUSSION-persona-rbac.md` for the unresolved RBAC questions).
- Knowledge base with semantic search (requires embeddings — maybe
  a Kokoro or local-embedding sidecar at this point).
- Image generation integrations (Venice for API, optional Stable
  Diffusion sidecar for self-host).
- Audio transcription (Whisper sidecar option).
- TTS for panel content (Kokoro sidecar option, surfaced through
  the composition Mode 3 system-services pattern when that
  lands).

Each is its own design doc. Build order within this bucket is
driven by architect interest and household need.

### 14. More games

The remaining entries from `DISCUSSION-llm-games.md`. Build order
within this bucket is driven by the success of Confabulation and
which designs still feel compelling once one game has been
pressure-tested. The Redactor is the natural second (cleanest
single-player entry). The Interrogation is the natural second
multiplayer (exercises more of the platform).

---

## 2. Open Questions Flagged by This Ordering

### 2.1 Persona RBAC

Placed in §13. The question of how a persona — a model identity
created by a user, possibly shared with other users — inherits,
carries, and confers permissions is non-trivial. Captured in
`DISCUSSION-persona-rbac.md` (companion to this document).

### 2.2 Step-up auth scope

§9 describes step-up auth as a kernel primitive. The exact scope —
per-capability, per-session, with or without hardware-backed keys,
TOTP vs. password re-entry — is a design-session question when
§9 is picked up. For now, the commitment is that it exists and
cluster-manager v2 depends on it.

### 2.3 Cluster-manager sidecar architecture

Cluster-manager needs scrapers for Ceph, k8s, Minisforum hardware.
Each is a sidecar, and the aggregation into a single panel UI is
the Plinth-side extension. The exact boundary between "scraper
sidecar" and "aggregator extension" is a design-session question
when §8 is picked up. For now, the commitment is that cluster-
manager v1 exists and is scoped to read-only.

### 2.4 Ai-bridge skeleton feature floor

§4 describes the skeleton as "Venice chat-only." But SWORD may
need `ai:1:chat_structured` for citation formatting; Confabulation
may need structured output for consistent round data. Designing
the skeleton against the two first callers (SWORD and
Confabulation) produces a slightly richer skeleton than "chat-
only" implies. The skeleton design session should read both
extensions' needs before settling its surface.

---

## 3. What This Document Is Not

An ordering sketch, not a commitment. Each entry is a design
effort in its own right; the order here is the current best
understanding of dependencies and priorities. Re-evaluation
sessions adjust this ordering as milestones close and priorities
shift. The architect's household needs, shell usability findings,
and platform-friction discoveries will all move entries around.

This is not the ROADMAP.md itself. When any of these entries is
ready to be scheduled concretely, it enters ROADMAP.md with a
band label and a slot in the milestone sequence; this document
remains as the higher-level narrative.
