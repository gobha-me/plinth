# LLM-Assisted Development: A Practitioner's Methodology

**Audience:** Developers using LLMs (Claude, GPT, Copilot, etc.) as
implementation partners, not just autocomplete.

---

## The Problem This Solves

LLMs write code fast. They also:

- Invent new interfaces instead of using existing ones
- Drift from design intent over long sessions
- Produce plausible-looking systems that quietly accumulate structural
  debt
- Treat every prompt as a fresh problem, even when the answer should
  come from an existing decision

Speed without coherence is refactoring debt at compile time.

The methodology exists to exploit the speed while constraining the
drift. It separates the work into phases where the human provides what
LLMs are worst at (architectural judgment, constraint definition, taste)
and the LLM provides what humans are slowest at (implementation,
boilerplate, test coverage, mechanical consistency).

---

## The Three Roles

**Architect** — the human. Defines system shape, trust boundaries,
extension points, sequencing, and trade-offs. Makes decisions that
require understanding the full system context, business constraints,
and deployment reality. Never writes implementation code in this role.

**Implementer** — the LLM code session. Writes source code, tests,
migrations, CI config. Works within the constraints defined by the
architect. Never makes structural decisions — when it encounters a
structural question, it asks rather than invents.

**Reviewer** — either role. Evaluates the delta between what was
planned and what exists. Produces gap analyses. Feeds findings back
into the architecture.

These roles can be the same human and the same LLM, but they are
never the same *session*. An architecture conversation and a code
session have different contexts, different tools, and different
failure modes. Mixing them is the primary source of problems.

---

## Phase 0: Architecture and Roadmap

**Participants:** Human + LLM (architecture session)
**Output:** Two documents minimum

### Architecture Document

The source of truth. Everything else derives from it.

Contents:
- System design (components, boundaries, data flow)
- Trust model (who can do what, authentication, authorization)
- Extension/plugin points (how the system grows without forking)
- Data model (entities, relationships, storage strategy)
- Security constraints (what must never happen, not just what should)
- Non-functional requirements (performance, deployment, scaling)

The architecture document is not a spec. It's a constraint system.
It tells future sessions what the walls are so they don't need to
guess.

### The Commitment Gradient

Architecture content carries different weights depending on how close
its implementation is. A section defining what milestone K+1 builds
deserves precise, testable wording — code sessions comply with it
letter-for-letter. A section sketching what K+10 might do deserves
directional wording — code sessions should know where the platform
is heading, but should not treat it as a constraint on today's
implementation. Without explicit labels, readers infer the weight
from tone, and that inference fails in two directions: sessions
over-comply with fuzzy sections (freezing premature decisions into
code), or under-comply with precise sections (because they read as
the same prose as everything else).

Architecture sections and roadmap milestones both carry band labels
from the same three-band vocabulary. N and M below are
project-dependent; for a small-team, high-cadence project, N=3 and
M=7 are reasonable defaults.

**Strong.** Load-bearing for work in the next N milestones.
Precisely worded, testable, citable in ICDs. Code sessions comply
as written; deviation requires an architecture session and revision
of the document. Example: an auth/session/PAT model once the
milestones implementing it are in flight.

**Medium.** Directional for work in the N+1 through M milestone
window. Covers where the platform is headed with enough precision
to keep near-term work from foreclosing it, but specific wording is
expected to tighten as implementation approaches. Design docs citing
a medium-band section are saying "we're building toward this," not
"we must comply with this." Example: a sidecar contract while
milestone 0.4 is in flight — its shape is committed, its endpoint
details tighten when 0.8 approaches.

**Fuzzy.** Sketch for work beyond M milestones or with unknown
preconditions. Its job is narrative coherence — "we know we're
going in this direction" — not authority. Code and design sessions
do not cite fuzzy-band content as a constraint. If a question
about fuzzy-band content comes up during implementation, the answer
is to leave it open, not to promote it. Example: a cross-cutting
composition framework before real extensions exist to pressure-test
it.

The gradient terminates at the planned roadmap horizon. Content
beyond that horizon is not fuzzy architecture, it is product vision
— if it belongs anywhere in the docs, it belongs in a separate
vision document, not in the architecture tree.

### Architecture Section Labels

Every section that isn't obviously strong-band carries its band in
the header — `## 4. Composition Framework [fuzzy]` or a trailing
annotation `[medium: 0.5–0.8]`. Strong-band is the default and
doesn't need a label. Doing this as a bulk pass once, and for new
sections as they're written, costs nothing and makes the document's
commitment structure legible.

Unlabeled is strong. This is a deliberate default: the most common
case is the least annotation. But it places a burden on the
architect writing new content — an unlabeled section is an implicit
strong-band commitment, and that commitment should feel heavy. If
the section isn't load-bearing for the next N milestones, label it.
If in doubt, label it medium; you can tighten to strong during the
next re-evaluation.

### Roadmap Milestone Labels

Every pending milestone in the roadmap carries a band annotation
matching the weakest-band architecture content it depends on.

- **`[strong]`** — all architectural dependencies are strong-band.
  This milestone is a code session: read the arch, read the ICDs,
  plan, implement, test, merge.
- **`[medium]`** — at least one architectural dependency is
  medium-band. This milestone is a tightening session followed by a
  code session. The tightening session promotes medium-band
  dependencies to strong, producing ICDs and precise section
  wording; the code session then proceeds normally.
- **`[fuzzy]`** — at least one architectural dependency is
  fuzzy-band. This milestone is an architecture session, then
  tightening, then coding. Running a code session against a fuzzy
  dependency is the "code session invents architecture on the fly"
  failure mode.

Completed milestones are unlabeled (the retrospective band isn't
useful; shipping resolved the question). New milestones added to
the roadmap start at whatever band their weakest dependency is at
right now, and slide toward `[strong]` as re-evaluation sessions
tighten their dependencies. Single-line items whose architectural
dependency is trivial or already strong are labeled `[strong]`
without further analysis.

A milestone's label and the architecture section band it reads from
must stay consistent. If `04-services-ha.md §5` is medium-band, any
milestone that depends on it is at best medium. Re-evaluation
sessions surface and repair inconsistencies.

In addition to band-labeled code milestones, the roadmap contains
**re-evaluation items** scheduled at the project's chosen cadence
(see Phase 3 §3.3). These items are distinct from code milestones;
they do not carry band annotations and they do not produce code.
They exist on the roadmap so the sliding-window mechanism is
visible to any session reading the roadmap. A roadmap containing
only code milestones is incomplete.

### Pre-Architecture Content: `discussion/`

Some content isn't ready to be architecture in any band. LLM
architecture sessions surface questions faster than the architect
can answer them authoritatively; documenting the thinking prevents
re-invention, but writing it into the architecture tree — even at
fuzzy band — implies a commitment that hasn't been made. These
artifacts live in `discussion/` alongside the architecture tree.

A discussion doc is a conversation capture with an explicit "not a
commitment" framing — structured as "here is the design space we
explored," with an explicit list of what is decided now (usually
limited to: reserved schema fields and don't-foreclose constraints)
and what is deferred. The architecture cites a discussion doc only
as "see `discussion/X.md` for the thinking," not as a source of
truth.

The movement paths between locations form the machinery of Phase 3
re-evaluation:

- `discussion/` → fuzzy: the thinking has firmed up enough to
  sketch a direction, and preconditions are visible on the roadmap.
- fuzzy → medium: implementation is entering the M-milestone window.
  The section tightens from "we're going in this direction" to
  "this is the shape we're committing to."
- medium → strong: implementation is entering the N-milestone
  window. The section tightens further into precise, testable,
  ICD-citable wording.
- Any band → `deferred/`: the implementation milestone has slipped
  past the roadmap horizon. Content is preserved, relocated, and
  clearly marked inactive.
- strong → medium (rare): intervening milestones invalidated the
  precise wording, but the directional commitment survives. Demotion
  is deliberate, not a silent correction.

Each movement on an architecture section propagates to roadmap
milestones that depend on it. Tightening a section from medium to
strong unlocks any `[medium]` milestones that depended on it — their
labels update to `[strong]` in the same re-evaluation session.

### Roadmap

A sequenced plan where each milestone is:
- Small enough for one code session to complete
- Testable in isolation (CI can verify it)
- Traceable to the architecture document

The roadmap is a plan, not a commitment. It gets re-evaluated as the
system evolves. But it exists so that code sessions don't have to
decide what to build next — that decision is architectural.

### Why Phase 0 Matters

Without it, each code session invents architecture on the fly. Session
1 decides auth works one way. Session 2 decides it works a slightly
different way. Session 3 builds a third approach because it didn't see
the first two. You end up with three auth patterns in one codebase,
all of them almost correct.

The architecture document prevents this by making structural decisions
explicit and persistent. Code sessions read the document. They don't
reinvent the decisions.

---

## Phase 1: Interface Contracts

**Participants:** Human + LLM (architecture session)
**Output:** Interface Control Documents (ICDs)

This is the single most impactful step for preventing the most common
LLM failure mode: duplicate implementation.

### What an ICD Contains

For each boundary between components:

- API endpoints: method, path, request shape, response shape, error
  codes, authentication requirements
- Data types: field names, types, constraints, nullability
- SDK/client contract: method names, parameter types, return types
- Event contracts: event names, payload shapes, ordering guarantees

### Why This Prevents Duplication

When a code session has an ICD that says:

```
POST /api/items
Request:  { name: string, folder_id: string }
Response: { id: string, name: string, created_at: string }
```

It doesn't invent a second shape in the handler, a third in the store
interface, and a fourth in the frontend. Every layer traces back to one
document.

Without the ICD, the LLM reinvents the interface from context and
memory. Context windows are finite. Memory is lossy. The result is
three slightly different versions of the same struct, each one
*almost* right, each one requiring a slightly different fix later.

### Cost/Benefit

The ICD takes 30 minutes to write. The duplicate-implementation debug
cycle takes 2–4 hours per occurrence, and it recurs every time a code
session touches a boundary that wasn't documented. The ICD is the
cheapest artifact in the pipeline.

---

## Scales of Work: When to Design, When to Just Build

Not all work needs the same documentation. The methodology recognizes
three scales, each with different documentation requirements:

### Scale 1: Single-Line Items

**Example:** "Fix the CSS cursor on the attach button."
**Documentation:** The roadmap line IS the spec.
**Code session freedom:** Full. Pick an approach, implement, test, done.

A design doc for this is overhead. The code session reads the one-line
description, finds the file, fixes it, writes a test. The approval gate
is the PR review, not a document review.

**The test:** Can this be fully described in one sentence? Can a code
session complete it without knowing what comes next on the roadmap?
If yes to both → single-line item.

### Scale 2: Multi-Version Arcs

**Example:** "Notes extension" — 11 versions from v0.11.0 to v0.11.10.
**Documentation:** Dedicated design document covering the full arc.
**Code session freedom:** Constrained to the current version's scope,
guided by awareness of the full arc.

This is where most projects go wrong with LLMs. A code session working
on v0.11.3 (Live Preview + Rich Editing) needs to know:

- What v0.11.0–v0.11.2 established (data model, navigation patterns,
  component structure) so it builds on them rather than reinventing.
- What v0.11.4+ expects (sharing model, graph view, transclusion) so
  it doesn't make decisions that block future work.

Without the design doc, each sub-version session operates in isolation.
Session 6 invents a data model that contradicts the one session 2
established. Session 3 hardcodes a navigation pattern that prevents
session 8's transclusion feature from working.

**The test:** Does this span more than 2 versions? Does a later version
depend on decisions made in an earlier one? Would a code session need
to know what comes after it, not just what came before? If yes to any
→ design doc required.

### Scale 3: Architecture Arcs

**Example:** "Sidecar tier" — fundamental new capability touching
kernel, extensions, auth, deployment.
**Documentation:** Design document + architecture addendum + ICD.
**Code session freedom:** Minimal. The design doc dictates structure,
interfaces, and sequencing.

These are rare. They change how the system works, not just what it does.
They typically require updating the original architecture document
because they introduce new primitives, trust boundaries, or extension
points.

### The Design Doc Decision

```
Can this be described in one sentence?
     │
     ├── Yes → Single-line item. Roadmap line + code session freedom.
     │
     └── No
          │
          Does it span multiple versions?
               │
               ├── No → Might still need an ICD if it crosses boundaries,
               │         but not a design doc. Phase 1 contracts suffice.
               │
               └── Yes
                    │
                    Does each version build on the previous?
                         │
                         ├── No → Independent features sharing a theme.
                         │         Roadmap grouping is enough. Each gets
                         │         its own ICD if it crosses boundaries.
                         │
                         └── Yes → DESIGN DOC REQUIRED.
                                   │
                                   The doc must contain:
                                   ├── Full arc overview (where this ends)
                                   ├── Per-version scope (what each builds)
                                   ├── Data model (established early, used throughout)
                                   ├── Interface contracts (what each version exposes)
                                   ├── Dependencies between versions
                                   └── What must NOT be decided until later versions
```

### What a Multi-Version Design Doc Contains

The design doc is not a spec for each version. It's a narrative that
tells each version's code session three things:

1. **What has already been decided.** Data model, component structure,
   naming conventions, patterns established by earlier versions. The
   session builds on these, it doesn't revisit them.

2. **What this version is responsible for.** Specific scope, entry
   criteria (what must exist from prior versions), exit criteria (what
   must work when this version ships).

3. **What must be left open.** Decisions that belong to later versions.
   The session must not foreclose these. If a choice now would block a
   future version, the design doc says so explicitly.

This third point is the most important and the most commonly omitted.
An LLM optimizes for the task in front of it. Without explicit "do not
decide this yet" constraints, it will make convenient choices that
happen to block future work. The design doc is the mechanism that
prevents local optimization from undermining global coherence.

### Examples from Practice

**Notes extension (DESIGN-notes-v011x.md):** 11 versions. The design
doc establishes the folder tree, note model, and editor component in
v0.11.0. Every subsequent version builds on these primitives. v0.11.8
(Transclusion + Embeds) would be impossible if v0.11.3 (Live Preview)
had made different editor architecture choices. The design doc
constrains v0.11.3 to make choices compatible with v0.11.8.

**Chat extension (DESIGN-chat-v012x.md):** 9 versions. The design doc
separates human-to-human messaging (v0.12.0–v0.12.5) from AI
integration (deferred to llm-bridge). Without the doc, a code session
building chat would naturally add LLM features because "chat with AI"
is the obvious association. The design doc says "do not build this here,
it belongs to a different extension" — a constraint a code session
would never infer on its own.

**Sidecar tier (DESIGN-sidecar-v014x.md):** 6 versions. Architecture
arc. The design doc defines the 4-endpoint HTTP contract, the two
scopes (instance + user), the RBAC model, and the tool meta-tool
integration. Without it, each version would reinvent the contract
boundary. The doc makes the boundary non-negotiable across all 6
versions.

---

## Phase 2: Implementation Loop

**Participants:** Human + LLM (code session)
**Tool:** LLM with repo access, CI pipeline, version control

Implementation is a loop, not a waterfall:

```
Review docs + plan
     │
     ▼
Plan the work → get human approval
     │
     ▼
Create branch → implement + write tests
     │
     ▼
CI passes? ─── no → fix → (retry)
     │
    yes
     │
     ▼
Does the change affect the docs/ICD?
     │              │
    no             yes → update docs
     │              │
     ▼              ▼
   Merge ◄──────────┘
     │
     ▼
Next roadmap item → (loop)
```

### Three Non-Negotiable Constraints

**1. Nothing commits without a test.**

This is not quality assurance. This is a forcing function.

The LLM has to understand what it built well enough to prove it works.
Tests catch the cases where the implementation looks right but doesn't
match the ICD. Tests also let future sessions modify code they didn't
write without breaking it silently.

Without tests, every session starts with "I'm afraid to touch this
because I don't know what it does."

**2. Justify the change.**

When code evolves or requirements change: why? Does the architecture
doc need updating? Does the ICD need updating? If a change can't be
justified against the existing design, either the change is wrong or
the design needs to evolve — and the design evolves through the
architecture session, not through a code session making ad-hoc
structural decisions.

**3. Human approval before implementation.**

The LLM presents a plan (what files, what changes, why). The human
approves, rejects with feedback, or redirects. This prevents scope
creep and catches misunderstandings before they become code.

**4. Record deviations in the owning ICD.**

When a code session takes a deviation from the ICD it implements —
a simpler body, a scoped-down error path, a stand-in signature, a
postponed sub-feature — the owning ICD gets an "Implementation
deviation" subsection **in the same PR** that takes the deviation.
Not in CHANGELOG only. Not in a code header comment only. Not in a
follow-up PR "when there's time."

The rationale is the same one that drives the ICDs themselves:
fresh sessions consult the ICD as the specification. A CHANGELOG
entry is a release narrative; a code header is local implementation
context; neither is read as the contract. If the ICD says X and the
code does Y, the next session has three options — implement against
X (and break), implement against Y (and call the ICD wrong), or
stop and reconcile (the mode the methodology should make cheap).
Writing the deviation back to the ICD in the same PR eliminates
options one and two.

The subsection should state: what shipped instead, why (with the
constraint that justified the deviation), and the named trigger
that closes the gap (a specific milestone, a specific caller, or
both). `ICD-0.1.6`'s *Implementation Notes (0.1.6)* footer is the
working template.

This constraint is not a style preference. The code-aware
re-evaluation loop (§Phase 3.1.1) exists specifically to find the
cases where it was skipped; each such find is a cycle of drift
that accumulated because the discipline wasn't applied at the PR
boundary. Applying it at the boundary is strictly cheaper.

---

### Caller-Triggered Implementation

A named pattern that recurs when the architecture doc and ICDs are
written ahead of the code. The full interface is committed to in
the ICD; the body ships stubbed, hardcoded, or in the simplest
conforming form; the filled-out body is gated on a named future
trigger.

**Shape.**

- The interface shape is decidable now (the ICD can be written,
  the struct fields can be listed, the error codes can be
  enumerated).
- The body's correctness depends on a consumer that doesn't exist
  yet — a caller whose shape will pressure-test the handler, a
  threading context that doesn't exist, a downstream subsystem
  that hasn't landed.
- The trigger that closes the gap is named explicitly — a specific
  future milestone, a specific future caller, or both. "When we
  need it" is not a trigger.

**Not YAGNI.** YAGNI omits the interface until a caller exists.
Caller-triggered implementation commits to the interface now
because future callers need to code against it, and builds the
body later because implementing it without a caller is either
guesswork or wasted work.

**Not speculative abstraction.** Speculative abstraction builds
machinery on the hypothesis that a caller will appear. Caller-
triggered implementation writes the *contract* on that hypothesis
but defers the *body* — and the contract is the part that future
callers will reference.

**Legitimate uses.**

- A dispatch function whose signature is declared but whose
  coroutine return type is stubbed as synchronous because no
  coroutine callers exist in-repo yet (canonical example:
  Plinth 0.2.6's deferred `drogon::Task<>` wrapper).
- Tier-1 handlers that return `{"not_implemented": signature}`
  until the first caller demands a real body; the registration
  machinery is real, the handler bodies are stubs.
- A pub/sub `publish()` entry point wired for subscribers today;
  the `LISTEN/NOTIFY` reader that will feed it ships in a later
  milestone.
- An ICD field documented and stored but not yet exposed through
  the API — the deferral is declared ("user-scope not supported
  in 0.2.x"), the column exists, the later milestone fills it in.

**Obligations.**

1. The interface must be real enough for future callers to code
   against. A stub that leaks implementation details (wrong error
   codes, wrong return shape) defeats the purpose — you've
   committed to a contract the filled-out body can't honour.
2. The trigger must be named. The roadmap or the ICD (or both)
   say exactly what unblocks filling in the body. This is what
   separates the pattern from YAGNI-with-steps.
3. Constraint #4 above applies in full — if the shipped body
   deviates from the committed interface, the owning ICD records
   it in the same PR.

Re-evaluation (§Phase 3.1.1) finds caller-triggered implementations
whose trigger has arrived but whose body is still stubbed, and
schedules the fill-in as a roadmap item if the original milestone
closed without it.

---

## Phase 3: Re-evaluation

**Participants:** Human + LLM (architecture session)
**Trigger:** Milestone boundary (the primary cadence), audit finding,
or new requirement that invalidates an existing commitment.

Re-evaluation is the sliding-window mechanism that keeps the
commitment gradient aligned to the roadmap *and* keeps the
architecture aligned to the code that actually exists. It has two
complementary halves:

- **Structural re-evaluation** (the paper half). Read the
  architecture and roadmap. Promote medium-band content whose
  milestone is now in range; tighten it into strong-band wording.
  Promote fuzzy content whose preconditions materialized. Demote
  or relocate content whose milestone slipped. Update roadmap
  milestone labels to reflect any dependency promotions.
- **Code-aware re-evaluation** (the gap-analysis half). Read the
  code as it actually exists — the kernel source, the ICDs as
  implemented, the extension handlers, the test suite coverage.
  Check whether strong-band architecture sections match the code
  that claims to implement them. Find architectural decisions
  embedded in code comments that should be promoted to the
  architecture doc. Find new interfaces that exist in code but
  aren't documented. Find tests missing for claims made in the
  architecture. Produce a gap analysis that feeds back into the
  architecture and the roadmap.

Milestones close; the near horizon moves forward; content promotes
and demotes; bands get recalibrated; and the architecture is
checked against reality. Both halves happen in the same
re-evaluation session, in either order — typically code-aware
first, because the gap analysis often surfaces content that should
have been promoted during the paper pass anyway.

### 3.1 Cadence

Re-evaluation happens at milestone boundaries, not on a calendar.
When milestone K ships, a re-eval session precedes milestone K+1
kickoff. The session asks:

- Did what we built match what the strong-band sections said? What
  did implementation teach us that needs to be written back into
  the document?
- Is content that was medium-band, now in the next-N-milestones
  window, ready for strong-band tightening? If not, why not?
- Is there fuzzy-band content whose preconditions are now in view?
  Does it promote to medium?
- Is there strong or medium content whose milestone has slipped?
  If so, does it demote, or does the roadmap need adjustment?
- For every pending roadmap milestone in the next N+M window: does
  its label still match its weakest-band dependency? Did any
  dependency promote? Did any demote? Update labels accordingly.
- **Forward ICD presence check.** For every pending `[strong]`
  milestone inside the next-N window: does its ICD exist? Strong-
  band says "code-ready," which presupposes a tight contract — if
  the ICD is missing, the milestone is not actually strong. Either
  demote it (acknowledging the contract gap) or schedule an ICD-
  authoring slot ahead of it (X.Y.Z.N sub-task, see §3.3). Fresh
  code sessions that pick a strong milestone without an ICD hit an
  immediate blocker; catching this here avoids that round trip.
- Is there `discussion/` content that's been sitting long enough
  that it should either promote into fuzzy-band or fold into
  `deferred/`? A discussion doc that's been parked for more than
  a few milestones without movement is either ready to be written
  up or ready to be archived.

A re-eval session that doesn't touch the architecture document or
the roadmap labels is rare. "Nothing needs to change" usually means
"we didn't look hard enough" — implementation always teaches
something.

### 3.1.1 Code-Aware Inputs

The code-aware half of re-evaluation reads, at minimum:

- **The kernel source tree** (or whatever the project's core code
  layer is). Structure, module boundaries, actual C++ / Rust / Go /
  TS / whatever-the-language-is organization.
- **The ICDs and the code that claims to implement them.** Do the
  handlers match the request/response shapes declared in the ICD?
  Are error codes consistent? Do authentication requirements match
  what's enforced in the filter chain? Is the audit logging where
  the architecture says it is?
- **Extension handlers against their `capabilities.json`
  declarations.** Are declared capabilities actually implemented?
  Are undeclared capabilities leaking into the code? (Both are
  architectural issues, not bugs.)
- **The test suite coverage.** Strong-band architecture sections
  that describe behavior should have tests enforcing that behavior.
  Sections without test coverage are candidates for either
  (a) writing the missing tests or (b) admitting the section isn't
  actually load-bearing and demoting it.
- **Recently-merged PRs' diff against docs.** Changes to code that
  should have produced documentation updates and didn't.

The session doesn't need to read every line. Strategic skimming,
filtered by the sections currently being re-evaluated, is
sufficient. The goal is detection, not exhaustive audit — the CI
pipeline and test suite handle exhaustive verification on every
commit; re-evaluation handles the questions CI can't ask, like "is
the code actually implementing the architecture, or just something
that compiles and passes tests?"

**What to produce when gaps are found:**

- **Code-diverged-from-arch.** The code is wrong. File an issue or
  open a PR to fix the code. Do not change the architecture to
  match the divergent code unless the divergence is an improvement
  the architect ratifies — in which case, update the architecture
  and note the change in the session's output.
- **Arch-silent-on-code.** The code is right and the architecture
  doesn't describe it. Update the architecture to document what
  exists. If the new content is load-bearing for further work,
  label it strong; otherwise band it appropriately.
- **Decision-embedded-in-comments.** A code comment captures a
  decision that should be in the architecture or a discussion doc.
  Extract it; cite the original code location; label the
  extraction appropriately.
- **Missing-test-for-arch-claim.** A strong-band section describes
  behavior that has no test. Either schedule the test as a roadmap
  item (probably `[strong]` because it's near-term catch-up) or
  demote the section to a band that matches the level of commitment
  actually backed by tests.
- **Interface-drift.** An ICD no longer matches the handler that
  implements it. Fix one to match the other; prefer updating the
  ICD if the code change was deliberate, prefer updating the code
  if the divergence was accidental.

### 3.2 Scope of a Re-evaluation Session

Re-evaluation is a rewrite session, not a code session. It may
produce: band changes on architecture sections, tightened wording,
demotions, new discussion docs, updated roadmap labels, roadmap
adjustments, and in rare cases new architecture decisions that close
open questions. It does not produce code. If it surfaces an
architectural question too large to answer in the session, that
question becomes a discussion doc and waits for its own architecture
session.

A code-aware re-evaluation session may additionally produce:

- Issue or PR filings against the code to repair divergences.
- Test additions scheduled as strong-band roadmap items.
- Promotion of embedded-in-code decisions into the architecture
  tree or discussion directory.
- ICD updates where interface drift is detected.
- New discussion docs where the code surfaces an architectural
  question that wasn't previously visible.

The session still does not produce new code beyond what's needed
to amend docs and tests. If the gap analysis finds a substantial
code issue, that becomes a roadmap item to be picked up by a
subsequent code session, not work done in the re-evaluation
session itself.

### 3.3 Scheduling

Re-evaluation sessions appear on the roadmap as explicit items,
interleaved with code milestones. A roadmap that lists only code
milestones will be worked only on code; re-evaluation will drift
indefinitely. Making re-evaluation a scheduled item keeps it
visible to any session reading the roadmap, including sessions
with no conversational context.

**Cadence.** Re-evaluation runs at whatever cadence matches the
project's pace. In LLM-driven development where one session
typically closes one milestone, every 3–5 milestones is a
reasonable default — frequent enough to catch drift, infrequent
enough that each session has something meaningful to do. In
human-team development, re-evaluation fits naturally at sprint
or release boundaries. The exact cadence is a project decision;
what matters is that it is declared and followed.

**Roadmap placement.** A re-evaluation item is marked as such on
the roadmap, distinguished from code milestones. Whatever
annotation the project uses for band labels (strong / medium /
fuzzy) does not apply to re-evaluation items — these are
architecture-session work, not code work. The annotation convention
can be as simple as a prefix:

```
- [ ] 0.5.3 db.batch() and silent mode          [strong]
- [ ] 0.5.4 plinth.events table, delta sync     [strong]
- [ ] RE-EVAL following 0.5.x                   [rewrite session]
- [ ] 0.6.0 Shell bootstrap and frame           [strong]
```

The tag is conventional, not prescriptive. What matters is that
the re-evaluation item exists on the roadmap and a fresh session
picking "the next thing" can distinguish a re-eval session from a
code session.

**What a re-evaluation item's entry criteria look like.** All
preceding milestones in its cadence window have shipped. If a
re-evaluation is scheduled after 0.5.x, it is picked up once
0.5.5 merges; no further code milestone begins until it completes.

**What a re-evaluation item's exit criteria look like.** The
architecture document and roadmap have been updated per §3.1's
questions. Band promotions and demotions have been applied.
Roadmap milestone labels have been updated. New discussion docs
have been written if the session surfaced architectural questions
too large to answer. The session's output is committed to the
documentation tree. Code is untouched.

**Adding re-evaluation items retroactively.** On first adoption,
add re-evaluation items at whatever cadence the project chooses,
starting from the next eligible boundary. Do not retroactively
schedule re-evaluations for completed milestones — those windows
have closed. Going forward, every new roadmap addition includes
the relevant re-evaluation items at the chosen cadence.

**Structural-only re-evaluations.** In practice, not every
scheduled re-evaluation needs the full code-aware pass. Some
cadence points legitimately only need the paper half — when
nothing has been merged that would affect the architecture's
accuracy, for example, or when the cadence point falls between
two code milestones that only touched strong-band content with
trivial implementations. Let the session decide during its
opening assessment whether the code-aware pass is warranted. The
default assumption should be "yes, do the code-aware pass" unless
there's a clear reason to skip it.

**Project-lifetime implications.** Early in a project, code-aware
re-evaluation is cheap because there isn't much code. Late in a
project, it becomes more expensive but also more valuable —
there's more code to drift and the drift compounds. The session
can skim strategically: focus on the sections relevant to work
since the last re-evaluation, not the entire codebase. The goal
is proportional investigation, not full-codebase audit.

---

## LLM Failure Modes and Mitigations

| Failure Mode | What Happens | Mitigation |
|-------------|-------------|------------|
| Duplicate implementation | Invents new interfaces instead of reusing existing ones. Three handlers parse the same data three different ways. | Phase 1 ICD. All layers trace to one contract document. |
| Context drift | Forgets design decisions from earlier in a long session or from previous sessions entirely. | Architecture docs persist across sessions. Code sessions read docs before working. |
| Plausible-but-wrong | Generates code that compiles and passes a glance review but violates an unstated assumption. | Tests required. Assumptions stated in architecture doc, not left implicit. |
| Scope creep | Adds features that weren't requested because they seem related or because the training data associates them. | Human approval gate. Plan must be approved before implementation begins. |
| Structural drift | Makes architectural decisions during implementation because the question came up and the answer seemed obvious. | Code sessions implement. Architecture sessions decide. The boundary is absolute. |
| Confident incorrectness | States something as fact that is wrong, particularly about library APIs, configuration options, or runtime behavior. | CI/test pipeline catches behavioral errors. Architecture session verifies claims about system design against the actual codebase. |
| Sycophantic agreement | Agrees with the human's suggestion even when it contradicts the existing design, rather than flagging the conflict. | The architecture document is the authority, not the most recent message. Code sessions check the doc, not just the prompt. |
| Local optimization | Makes the best decision for this version that happens to block a future version. Picks a data model that's perfect for v3 but incompatible with v5's requirements. | Multi-version design doc with explicit "do not decide yet" constraints. The doc tells the session what to leave open. |
| Architecture inflation | Decisions accumulate in strong-band wording without corresponding implementation. LLM sessions surface design questions faster than code sessions can answer them, and the architect's correct preference for documented decisions over handwaving feeds the inflow. Over time, strong-band wording covers content that is actually medium or fuzzy. Code sessions either over-comply (freezing premature decisions into code) or start treating the document as aspirational (losing its authority). Without roadmap labels, even correctly-banded architecture is consumed indiscriminately — a fresh session cannot tell which milestones are code-ready from the roadmap alone. | Explicit commitment gradient (strong/medium/fuzzy) with band labels on both architecture sections and roadmap milestones. Re-evaluation as sliding window — content that was strong but hasn't been exercised in N milestones demotes or relocates. Pre-architectural thinking goes to `discussion/`, not into the architecture tree. |
| Re-evaluation drift | Phase 3 is defined in the methodology but not scheduled on the roadmap. Fresh sessions reading the roadmap see only code milestones and pick the next code milestone. The sliding window never slides. Over several milestones, bands age out of sync with implementation: medium content that should have tightened stays medium, fuzzy content whose preconditions arrived stays fuzzy, roadmap labels diverge from their architectural dependencies. The methodology's drift-prevention mechanism, itself drifts. | Schedule re-evaluation as explicit roadmap items at the project's chosen cadence. Re-eval items are distinct from code milestones, have their own entry/exit criteria, and are picked up by whichever session reaches them in the roadmap order. The sliding window is visible because its invocation is a first-class roadmap entry. |
| Silent architectural divergence | Code is written against an architecture section, compiles, passes tests, and merges. The tests validate behavior against the code itself, not against the architecture. If the code drifted from what the architecture said — wrong semantics, different authentication model, capability-dispatch shortcuts, cross-layer shortcuts — the tests enforce the drifted version. No mechanism in the per-milestone loop catches this. Over time, the architecture describes what was intended and the code implements something subtly different; the two diverge, and future code sessions build on whichever one they happened to read first. | Code-aware re-evaluation. A session whose explicit job is reading the code against the architecture and reporting divergences catches drift that tests can't, because tests validate the code against itself. This is a methodology-level check; it doesn't replace CI, it complements it. |
| Undocumented deviation | A code session takes a justified shortcut against the ICD — a sync form instead of the declared async one, a cache-miss-is-not-found instead of the declared DB-fallback, an extra result field for audit enrichment — and records it in the code header comment and the CHANGELOG entry. The owning ICD is left untouched. Fresh sessions consulting the ICD as the specification read a contract that diverges from shipped reality. They either re-implement against the outdated contract (breaking the working code) or flag the working code as broken (wasting a cycle discovering the deviation was deliberate). Code-aware re-evaluation eventually surfaces these, but every cycle between the deviation and its re-eval is drift that compounded while it was invisible at the PR level. | Constraint #4 (Phase 2). When a code session takes a deviation, the owning ICD gets an "Implementation deviation" subsection in the same PR. Not in CHANGELOG only, not in a code header only, not in a follow-up. The ICD is the specification; the deviation belongs where future sessions read the specification. |
| Missing ICD on the near horizon | A pending milestone is labeled `[strong]` — meaning the architecture commits to it being code-ready — but no ICD has been authored. The gap is not visible during code-aware re-eval (which looks backward at shipped milestones) and not visible to a fresh session reading the roadmap (which sees the label, not the contract's existence). A code session picks the milestone, reads the roadmap entry, begins planning, and only then discovers there's no ICD. The session either blocks (cheap, but a wasted round trip) or writes the ICD ad hoc as part of implementation (violating the Phase 1 / Phase 2 split and producing a contract that was pressure-tested by a single implementation attempt rather than by the architect). | §3.1 *Forward ICD presence check*. Every re-eval verifies that each `[strong]` milestone in the next-N window has an ICD. Missing ICDs trigger either a demotion (if the contract isn't actually ready to be tightened) or a scheduled X.Y.Z.N ICD-authoring slot ahead of the milestone. The slot lands on the roadmap as a first-class item so the next fresh session picks it up naturally. |

---

## What This Is Not

**This is not waterfall.** The loop runs continuously. A milestone might
take a day. Re-evaluation might happen mid-milestone if the code session
discovers something structural.

**This is not micromanagement.** The human doesn't dictate
implementation details. The human defines constraints and approves
plans. The LLM has full autonomy within the approved plan.

**This is not LLM-specific.** This methodology works with a human
team too. The difference is that LLMs fail in specific, predictable
ways (duplication, drift, scope creep) that humans fail in less
predictable ways. The methodology is optimized for the predictable
failure modes.

**This is not a guarantee.** The methodology reduces the frequency
and severity of problems. It doesn't eliminate them. The CI pipeline,
the test suite, and the re-evaluation loop exist because problems
will still happen. The goal is to catch them before they compound.

---

## Anti-Patterns

**"Just build it, we'll design later."**
Produces working code with no structural coherence. Each feature works
in isolation; nothing composes. Refactoring becomes the project.

**"The code session can figure out the architecture."**
The code session will figure out *an* architecture. A different one each
time. You'll find three auth patterns, two data access layers, and a
handler that re-implements the store because it didn't know the store
existed.

**"Tests slow us down."**
Tests are the only thing that lets future sessions modify code they
didn't write without breaking it. Without tests, every session starts
with "I'm afraid to touch this because I don't know what it does."

**"The ICD is overhead."**
The ICD takes 30 minutes. The duplicate-implementation debug cycle
takes 2–4 hours per occurrence, and it recurs. The ICD is the cheapest
artifact in the pipeline.

**"We don't need to re-evaluate, the roadmap is fine."**
The roadmap was fine when it was written. Eight versions later, reality
has diverged. Skipping re-evaluation means building on assumptions that
no longer hold.

**"It's only 5 versions, we don't need a design doc."**
The code session working on version 3 doesn't know what version 5
needs. It makes a convenient choice that blocks version 5 entirely.
The design doc costs an hour. The re-architecture costs a week and
a version bump that wasn't on the roadmap.

**"Each version is independent, they just share a theme."**
If they share a data model, a component library, a navigation pattern,
or any structural decision, they are not independent. The first
session that establishes a pattern becomes the de facto design doc,
except nobody wrote it down, so the fourth session reinvents it
differently.

**"We decided it, so it belongs in strong-band architecture."**
Deciding is correct when the alternative is handwaving — you pay
for handwaving every time a code session reinvents the same question
in a different way. But the location of the decision matters.
Strong-band architecture is for commitments load-bearing in the
next N milestones; anything further out belongs in medium, fuzzy,
or `discussion/` depending on how far out and how formed the
thinking is. A decision that lives in strong-band wording without
the implementation to pressure-test it acquires an authority it
hasn't earned, and the first code session that runs into it either
freezes a premature choice or quietly routes around it. Both
outcomes are worse than the decision having been parked in a lower
band where its provisional status was explicit.

**"The whole architecture document is authoritative."**
True for the strong band; increasingly less true as you move through
medium and fuzzy. A code session that cites a fuzzy-band section as
a binding constraint has misread the document's structure. Labels
exist so this can't happen — if a section isn't labeled, it is
strong; if it is labeled medium or fuzzy, the label means what it
says. An architect writing an unlabeled section implicitly commits
to strong-band wording. That commitment should feel heavy.

**"The roadmap is just a to-do list, no need to label it."**
In a human-team setting, the team's accumulated context compensates
for an unannotated roadmap — the architect can say "don't start 0.6
yet, we need a design session first." In LLM-driven development,
that compensation isn't available. The roadmap is a contract between
the architect and every future code session: "here are the tasks,
in order, and here is what each one is ready for." An unannotated
roadmap invites a fresh session to pick the wrong next thing and
waste a conversation discovering why. Label every pending milestone.

**"We'll re-evaluate when we need to."**
True in small doses and only when "we" has accumulated context about
what needs re-evaluating. In LLM-driven development, "we" is
whichever session happens to be running, and that session has no
accumulated context beyond what's in the docs. A session reading an
unannotated roadmap has no way to know that re-evaluation is overdue;
it picks the next code milestone because that's what the roadmap
says to do. The result is the same as if re-evaluation had never
been part of the methodology. Schedule re-evaluation as a roadmap
item so every session sees it as the next thing to do when its turn
arrives. "We'll do it when we need to" is the same failure mode as
"we'll label the bands when we need to" — it offloads discipline to
memory, and the LLM-driven setup provides no memory.

**"The deviation is documented in the code, that's enough."**
A code header comment that explains a deviation is excellent context
for the session that wrote it and for a reviewer reading the diff.
It is not enough for the next session. Future sessions read the
ICD as the specification — that is the ICD's entire purpose. A
CHANGELOG entry is a release narrative, not a spec; a code header
is local implementation context, not a spec. If the shipped code
diverges from the ICD, the divergence belongs in the ICD, in the
same PR that takes the deviation, so every future session reading
the spec reads the shipped reality. The code-aware re-evaluation
loop catches this eventually, but every cycle of drift before it
does is drift that was preventable at the PR boundary. The
discipline is cheap — a few lines per PR — and it closes a class
of interface-drift by construction instead of by discovery.

**"Tests passing means the architecture is followed."**
Tests validate that code matches its own interfaces and produces
its own expected outputs. They do not validate that code matches
the architecture document. A handler implementing the wrong
capability semantics with a test that exercises those wrong
semantics will pass CI indefinitely. The architecture document is
the specification; the tests are a behavioral contract with
whatever the code currently does. These can diverge without any
signal in the CI pipeline. Code-aware re-evaluation is the
mechanism that catches this divergence — a session whose job is
reading the architecture and reading the code and reporting
mismatches. The alternative is learning about the divergence when
a new extension author reads the architecture, implements against
it, and finds their extension doesn't work because the kernel
drifted.

---

## Document Hierarchy

Every artifact traces upward. If the code contradicts the ICD, the
code is wrong. If the ICD contradicts the architecture, the ICD needs
updating through an architecture session.

```
Architecture Document (source of truth)
│
├── Roadmap (sequenced plan, derived from arch)
│
├── Design Documents (multi-version arcs)
│   ├── Per-version scope, entry/exit criteria
│   ├── Shared data model and patterns
│   ├── Interface contracts between versions
│   └── Explicit "do not decide yet" constraints
│
├── ICDs / API Specs (single-version contracts)
│   └── Implementation (code, derived from ICD or design doc)
│       └── Tests (verification, derived from contracts)
│
└── Audit / Review (feedback into architecture)
    └── Gap Analysis (delta between plan and reality)
        └── Roadmap Update (re-sequencing based on findings)
```

Design documents sit between the architecture and the ICD. They
decompose a large architectural concept into versioned milestones,
each with its own contracts. A code session working on version N
reads the design doc's section for version N, not the full
architecture document — the design doc has already translated the
architectural intent into version-scoped constraints.

---

## Minimum Viable Adoption

If you adopt nothing else:

1. **Write the architecture doc before the first code session.** Even
   a rough one. Even just "here are the components, here are the
   boundaries, here is what each one does." The LLM will be 3x more
   consistent with it than without it.

2. **Write the ICD for your most-touched boundary.** The one where
   the frontend talks to the backend. The one where two services
   communicate. Document the shape once. Reference it in every session
   that touches it.

3. **Require tests.** Tell the code session "nothing merges without
   a test." It will write the test. The test will catch the next
   session's mistakes. This is the compound interest of LLM-assisted
   development.

4. **Write a design doc for anything spanning 3+ versions.** If you're
   about to put a feature on the roadmap that needs multiple releases
   to complete, stop and write the arc first. 30 minutes now saves
   the re-architecture session later when version 4 discovers that
   version 2 made an incompatible choice.

5. **Label the bands everywhere.** For every section in your
   architecture document, ask: is this load-bearing for work in the
   next N milestones? If yes, no label needed (strong is default).
   If N+1 through M, label medium. If further out, label fuzzy.
   For every pending milestone in your roadmap, annotate with the
   weakest band its architectural dependencies fall into — `[strong]`
   means code-ready, `[medium]` means tighten first, `[fuzzy]` means
   architecture session first. Do this as one bulk pass on the
   existing tree; do it for new content as it's written. This costs
   nothing and makes the document's commitment structure legible to
   both code sessions and to your future self reading the docs six
   months later. Without labels, every section reads as strong and
   every milestone reads as ready, and inflation is invisible. Once
   labels exist, re-evaluation has a concrete job at every milestone
   boundary: slide the bands forward, update the roadmap, tighten
   what's in range.

6. **Schedule re-evaluation on the roadmap, including code-aware
   passes.** Every 3–5 code milestones, insert a re-evaluation
   item. Each re-evaluation has two halves: the paper half (slide
   bands, update labels, relocate aged content) and the code-aware
   half (read the code against the architecture, produce a gap
   analysis). The code-aware half is the one that catches
   architectural drift CI can't see. Default to doing both halves
   at every re-evaluation; skip the code-aware half only when
   nothing meaningful has merged since the last pass. A roadmap
   containing only code milestones and paper re-evaluations is
   still incomplete, because code that silently diverges from the
   architecture will pass tests forever.