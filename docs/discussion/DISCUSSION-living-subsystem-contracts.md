# Living subsystem contract docs — proposal

**Status:** Discussion capture — proposal for the next architecture session
**Source:** four-ICD audit pass (0.1.2, 0.3.3, 0.4.4, 0.5.2), 2026-04-24
**Participants:** the maintainer (Architect) — proposal author
**Scheduled for ratification:** `0.6.0.N Architecture session: contract-docs proposal` (paper slot, ROADMAP §0.6)
**Feeds into:** new `docs/contracts/` tier; METHODOLOGY §Phase 3 update
**References:**
- `docs/METHODOLOGY-llm-assisted-development.md` §Phase 3 (Re-evaluation;
  the proposed roll-forward step lands here)
- `docs/architecture/` (decisions at commitment bands; contracts
  reference architecture, not the other way around)
- `docs/icd/` (per-milestone authoring contracts; retained as history
  under this proposal)
- `docs/design/` (per-arc narrative; complementary, unchanged)
- ICD-AUDIT-0.3.3-findings.md F1–F4 (the cross-ICD-extension drift
  evidence cited below)

**Scope:** tail cleanup mid-development; not a methodology redesign.

---

## Problem

Current organization: one ICD per roadmap milestone. This works for
**authoring** — each code session gets a single document as the
handoff contract. It fails for **reading** after ship:

> "What's the current contract for the async bridge?"

To answer today, a reader consults:
- ICD-0.3.3 (original surface + 0.3.3.1 + 0.3.3.3 amendments)
- ICD-0.3.4 (CAP_CALL additions to AsyncOp)
- ICD-0.5.0 (PUBSUB_PUBLISH additions)
- ICD-0.5.1 (bc_extension_name field)
- ICD-0.5.2 (PUBSUB_SUBSCRIBE/UNSUBSCRIBE)
- `src/kernel/js/async_op.hpp` (for the actual current shape)
- `src/kernel/js/bridge_context.hpp` (4 parallel-dispatch fields)

Six docs to reconstruct a single struct's contract. In practice,
readers grep the code, which means the ICDs have silently ceased to
be authoritative for any contract that spans milestones.

**The audit evidence** (see `ICD-AUDIT-0.3.3-findings.md` F1–F4):
`AsyncOp::Type` enum, `AsyncOp` struct fields, and
`ns_for_cancellation` value set have all grown beyond what ICD-0.3.3
(the owner) describes. The extending milestones did their part —
each wrote the addition into code with ICD attribution in the
comment. ICD-0.3.3 itself stayed a snapshot. No discipline failure;
an organizational gap.

**The same gap, other subsystems:**

- `plinth.packages` schema: declared in ICD-0.4.4, extended by
  0.4.5 (`retired_at`, `supersedes_id`, `SUPERSEDED` state), 0.4.7
  (`last_rbac_test_run_at`, `last_rbac_test_result`). Current
  shape lives in `migrations/schema.sql` only.
- Session middleware filter chain: declared in ICD-0.1.2,
  extended by ICD-0.1.3 (PAT tokens), 0.1.4/0.1.5 (RBAC integration
  sits adjacent). No single doc describes what the current chain
  accepts.
- Capability registry shape: ICD-0.2.0 / 0.2.2 / 0.2.4 / 0.2.6 across
  four milestones.

---

## The re-implement bar

The right test for whether a subsystem's contract is complete: **a
competent engineer reading the contract doc (plus the architecture it
depends on) could rebuild the subsystem from scratch, potentially in
a different implementation stack, as a documentation exercise
followed by roadmap-driven coding.**

Concrete framing: Rust + v8 instead of C++20 + QuickJS. Or Go + goja
(with the caveat that goja is ECMA 5.1, so either extensions narrow
to 5.1 or a transpile step gets added — a portability constraint the
contract should name explicitly). If the architecture docs + subsystem
contracts get the reader most of the way there and the remaining work
is legitimately "build a roadmap and execute," the contracts are
doing their job. If the reader has to read C++ source to understand
the protocol, they aren't.

This bar has two useful properties:

1. **It forces engine-agnostic writing.** The bulk of what makes
   Plinth work — the capability dispatch protocol, the Four Realtime
   Layers, the RBAC model, the package lifecycle, the HTTP surface,
   the DB schema, the audit taxonomy — is implementation-independent.
   Engine-specific details (QuickJS's `JS_*` API, the interrupt
   handler mechanism, libpq coroutine integration) are a minority
   surface and should be segregated into clearly-marked
   implementation-notes sections.

2. **It maps onto real portability scenarios.** Under the current
   architecture, the shell is a near-drop-in to any re-implementation
   — it consumes the kernel through HTTP + WebSocket + `/ext/*`
   static serving, all engine-agnostic. The "shell-as-extension,
   kernel-owns-all-HTTP" decision isn't just architectural cleanliness;
   it's a portability property. Contract docs should make this
   property explicit and defend it.

A contract doc that fails the re-implement bar isn't done; it's a
draft. That's the completeness criterion that replaces the earlier
count-based heuristic.

---

## Proposal

Add a **third tier** to the current doc hierarchy:

```
README (quickstart, install, TLDR)
  │
Architecture docs            ← decisions at commitment bands;
  │                             what will be + why
  │
  ├── Design docs (per arc)  ← narrative across milestones
  │
  ├── Subsystem contracts    ← NEW TIER: current shipped reality,
  │     in `docs/contracts/`   complete enough to re-implement
  │                             against
  │
  └── ICDs (per milestone)   ← shipping handoff; retained as history
```

Hierarchy property: **contracts do not reference headers**. A contract
doc is the next logical level above the header — the interface
described in its own terms, not a pointer to implementation. If the
contract and the header disagree, the contract settles the argument;
either the contract has a bug and needs a deliberate edit, or the
header drifted and the code needs fixing. One source of truth per
subsystem.

This overrides the "headers-as-normative" suggestion floated earlier
in the audit discussion. That suggestion was wrong under the
re-implement bar. A contract that reads "see `broker.hpp`" fails the
bar by construction — a Rust re-implementer can't read a C++ header
and expect to produce a correct port.

**Subsystem contract docs (`docs/contracts/<n>.md`):**

- One per subsystem whose contract is complete (see §Completeness
  criterion below).
- Always authoritative for **current** shape.
- Engine-agnostic protocol in the body; engine-specific implementation
  notes in a clearly-marked subordinate section (e.g., "QuickJS
  binding details").
- Append-only history section at bottom (which milestone landed which
  extension), but the main body reads as "here is what the subsystem
  is today."
- Re-eval sessions are the update mechanism. Paper half of re-eval
  rolls the milestone's ICD additions into the relevant subsystem
  contract doc. The milestone ICD itself remains unchanged as history.

**Milestone ICDs continue unchanged.** They are the authoring contract
between the architecture session and the code session. Their
deviation footers continue to function. The re-eval process gains one
new step: "roll milestone's shipped contract additions into the
subsystem contract doc(s) they extend."

---

## Completeness criterion

A subsystem's contract gets lifted to a doc when:

- **Its arc has closed** — the milestones that define the subsystem
  have shipped, and no planned milestone in the next N-M window
  extends its core shape. Future amendments are expected to be small
  (adding a capability, extending a taxonomy) rather than structural
  (changing the state machine, reshaping the struct).
- **It passes the re-implement bar** — an architect reading the
  existing ICDs + code could write the contract doc, and a fresh
  engineer reading the contract doc + architecture could
  (conceptually) rebuild the subsystem.
- **The architect judges it ready.** Not a count; an assessment.
  Same kind of call as promoting an architecture section from medium
  to strong band.

A subsystem mid-arc is NOT a candidate. Freezing a contract during
its active evolution is a premature-commitment failure mode.

**Lifting a subsystem to contract status is a formalization event.**
Post-contract, changes to that subsystem require touching the
contract doc, which makes extensions more deliberate — same posture
as a semver minor-version freeze. Breaking changes aren't prevented,
they're made visible and considered.

---

## Candidate subsystems under the completeness criterion

**Ready now** — arcs closed, shipped, stable:

| Subsystem | Arc | Size est. |
|---|---|---|
| Auth + sessions + PATs | 0.1.2, 0.1.3 (core closed; 0.1.4–0.1.7 are adjacent consumers, not extenders) | ~400 lines |
| RBAC (rules + enforcement + registration + test execution) | 0.1.4, 0.1.5, 0.4.6, 0.4.7 (persona-RBAC explicitly deferred; current contract stable) | ~500 lines |
| Capability registry | 0.2.0, 0.2.2, 0.2.4, 0.2.6 (arc closed) | ~400 lines |
| Async bridge + QuickJS runtime | 0.3.0–0.3.5 (arc closed; 0.5.x extensions are additive) | ~600 lines |
| Package install + lifecycle | 0.4.0–0.4.7 (arc closed) | ~700 lines |
| Audit | 0.1.7 (machinery stable; taxonomy grows per milestone but shape doesn't) | ~250 lines |

**Wait for arc close:**

- Realtime pipeline — 0.5.0/0.5.0.3/0.5.1/0.5.2 shipped; 0.5.3
  inflight; 0.5.4/0.5.5 pending. Initial contract fill lands after
  0.5.5.

**Too early:**

- Shell, admin extension, sidecar, HA, file storage, notifications,
  outbound HTTP. All unshipped or mid-evolution. Contract docs for
  these would be premature freeze.

**Six ready, one waiting, several deferred.** Each ready candidate
represents a subsystem whose contract is currently scattered across
2–8 ICDs plus code — consolidation is pure reading-improvement with
no authoring-path impact.

---

## What this fixes

- **Cross-ICD extension drift** across every subsystem that has one.
  ICD-0.3.3's F1–F4 disappear by construction; ICD-0.4.4's 12-value
  enum drift disappears; ICD-0.5.2's §Public API staleness
  disappears (the contract doc becomes the normative surface; the
  ICD §Public API section retires).
- **"Which ICD do I read?" confusion for the reading case.** One doc
  per subsystem; milestone ICDs remain as history and authoring
  record.
- **The Rust/v8 or Go + goja port scenario becomes a doc-first
  exercise rather than a reverse-engineering slog.** Whether any such
  port is ever actually built is beside the point — the contract
  doc's ability to support the port is the test of its completeness.

## What this doesn't fix

- **Own-milestone-surface drift** (the ICD-0.5.2 §Public API
  category). Contract docs don't prevent a milestone from
  under-specifying its own surface. Constraint #4 on milestone ICDs
  remains the right tool.
- **Own-milestone deviation-footer discipline** (the ICD-0.4.4
  category). Same — constraint #4 is still the remedy.

---

## Filesystem layout note

Contract docs subsume the interface-discovery role that a separate
`include/` tree would play in traditional C++ project layouts. A
reader asking "what's the interface to the async bridge?" goes to
`docs/contracts/async-bridge.md`, not to a headers directory. So
Plinth's current side-by-side header/implementation layout is the
right call — headers are implementation partners for their `.cpp`,
not a public interface surface. For LLM context specifically,
side-by-side is a small plus: reading a `.cpp` resolves the
co-located `.hpp` without cross-tree navigation, one less convention
to encode in the context window.

The only case where a separate header tree would earn its keep is
public-vs-internal kernel partitioning. Plinth doesn't have that
distinction today — every header is internal — so the split would be
ceremonial.

---

## Proposed sequence

1. **One architecture session to decide this.** Not a code session.
   Resolves: adopt or not; naming convention; placement in repo;
   re-eval-step wording; contract-fill as a roadmap-visible paper
   milestone tag.

2. **If adopted, one initial-fill session per ready subsystem.**
   Probably 1–2 hours each; content already exists scattered across
   existing ICDs + code. Can be interleaved with normal milestone
   work — each re-eval slot can pick one subsystem's initial fill.
   Roadmap entries get a `[contract-fill]` tag analogous to
   `[rewrite session]`.

3. **Re-eval methodology updated** with a new step: "roll milestone
   contract additions into the subsystem contract doc." Add to
   `METHODOLOGY-llm-assisted-development.md` §Phase 3.

4. **Audit findings from this pass get absorbed** into the
   subsystem contract docs during initial fill. F1–F4 on 0.3.3, all
   the cross-ICD drift on 0.4.4, the §Public API staleness on 0.5.2
   for the broker — all resolved by construction.

**Estimated total cost:** one architecture session + 6 fill sessions
(~12 hours of paper work across several weeks of re-evals) + an
incremental ~30 min per future milestone for the roll-forward step.

**Benefit:** reading a current contract becomes a single-document
lookup. The "I have to grep the code to know what's true" posture —
which the audit pass has been operating under throughout — reverses.

---

## Alternatives considered

**A. Do nothing.** Accept that shipped-contract reconstruction
requires reading N ICDs + code. Argument: discipline is working;
drift is bounded; contract docs double the maintenance surface.
Counter: audit evidence shows the ICDs have already silently ceased
to be the read-authority for cross-milestone contracts. Readers grep
code today; making the contract explicit concentrates existing
informal work rather than adding new work.

**B. Retire ICDs; move to subsystem-first docs only.** Argument: one
tier instead of two. Counter: ICDs do real work as the arch-to-code
handoff for a specific milestone. A subsystem doc under active
development has no clean "this is what the next milestone ships"
boundary; the ICD is that boundary. Not recommended.

**C. Per-interface ICDs.** Considered earlier in the audit
conversation. Doesn't solve the cross-milestone reading problem —
more ICDs per milestone, same reconstruction burden. Fragments the
integration view milestone ICDs provide. Not recommended.

**D. Let headers be the normative surface.** Considered earlier.
Fails the re-implement bar: a Rust re-implementer can't read C++
headers as protocol. Also doesn't cover HTTP surfaces, DB schemas,
audit events, RBAC taxonomies. Under this proposal, headers are
demoted to implementation reality; the contract doc is normative.

---

## Open questions for the architecture session

1. **Naming.** `docs/contracts/` preferred. Distinguishes from
   `docs/architecture/` (band-labeled decisions) and `docs/design/`
   (per-arc narrative).

2. **Granularity.** Realtime pipeline likely wants to be one contract
   doc (listener + coalescer + broker are tightly coupled and
   separately read would fragment the flow). Capability registry
   likely one doc (even though 4 ICDs). Package install +
   lifecycle one doc (the install and transition arms share the state
   machine). Auth + sessions + PATs one doc. Default heuristic:
   follow code module boundaries; combine when separately-read is
   confusing.

3. **Milestone ICD retention.** Keep indefinitely as history. Cost
   is zero; historical value is real.

4. **Who writes the initial fill?** LLM architecture session with
   architect ratification — same pattern as any re-eval.

5. **Interaction with `architecture/` tree.** Architecture carries
   decisions (WHY + at what commitment band). Contracts carry current
   shape (WHAT is shipped). Short cross-reference in each direction
   handles overlap. Architecture references contract; contract does
   not reference header.

6. **Test-case tables.** Stay in the milestone ICD. Contract doc
   links to them and may carry a subsystem-level test-count summary,
   but the per-case specs stay with the milestone that authored them.

7. **Engine-specific content.** Segregated into a clearly-marked
   subordinate section within each contract doc. The body is
   engine-agnostic protocol; the subordinate section carries the
   QuickJS-specific, Drogon-specific, libpq-specific details. A
   re-implementation stack (v8, tokio, sqlx) would replace the
   subordinate section; the body survives.

---

## Scheduling note (post-2026-04-27 cleanup)

Slotted on ROADMAP as `0.6.0.N Architecture session: contract-docs
proposal` `[strong]`, paper. Runs alongside the existing 0.6.0.N
follow-ups (`Test-fixture buildout`, `ICD-0.6.1 authoring`); not
critical-path for 0.6.0 code. Decision precedes any contract-fill
sessions.

**Updated candidate list (post-0.5.5 ship):** the realtime pipeline
("Wait for arc close" entry above) is now ready — 0.5.0/0.5.1/0.5.2/
0.5.3/0.5.4/0.5.5 all shipped. Initial fill set is **seven**
subsystems, not six.
