# Cross-Cutting Extension Composition — Design Space Summary

**Status:** Discussion capture — not a design doc, not authoritative
**Source:** Shell design review session, 2026-04-16; updated
2026-04-17 after ai-bridge drafting narrowed Mode 3's scope
**Participants:** the maintainer (Architect) + Claude (Architecture Session)
**Feeds into:** Future DESIGN-composition-v09x.md (Scale 2,
~0.9.x-ish)
**References:**
- `architecture/05-extensions.md §4` (reserved manifest fields —
  the strong-band footprint of this thinking)
- `architecture/02-capabilities.md §1.12` (capability registry
  don't-foreclose for augmenter-query extension)
- `DESIGN-shell-v06x.md §3.1` (reserved panel fields), §4
  constraint 10 (don't-foreclose composition in panel SDK)
- `DISCUSSION-ai-bridge.md` §4 (how ai-bridge interacts with
  Mode 3 system services)

---

## 0. Scope Note

This discussion is about how Plinth's extensions compose with one
another, not about kernel work. Plinth-the-kernel's job here is
narrow: parse and store manifest fields without interpreting them,
and don't foreclose future composition in the capability registry
or the panel SDK. The mechanism itself is extension-layer work,
designed when real extensions exist to pressure-test it. This
document exists so the kernel work done in 0.2–0.8 doesn't
accidentally make that later design impossible.

## 1. The Problem

Some extensions don't stand alone — they augment other extensions.
A grammar checker operates on any text editor. An AI assistant
augments any content panel. STT converts speech to text input for
any surface that accepts it. TTS vocalizes content from any surface
that provides it. Code completion works in any code editor. A
translation service transforms content in any text surface.

The naive implementation: the grammar checker lists the extensions
it modifies (Notes, Chat, Email, Code Workspace, ...). This couples
the augmenter to every host. Install a new text editor and the
grammar checker doesn't know about it. Update the grammar checker
and every host needs testing. The coupling list grows without bound.

This was a lesson learned from Armature. The coupling was painful
and became the primary barrier to adding new extensions.

## 2. The Design Principle

**Extensions declare what surfaces they expose, not who may augment
them. Augmenters declare what surface traits they can operate on,
not which extensions they target.**

A grammar checker says "I augment any surface with the `text-editor`
trait." Notes says "my editor panel has the `text-editor` trait."
Neither knows the other exists. The system matches them.

This is the capability registry pattern extended from "who provides
X?" and "who handles content-type Y?" to "who augments surfaces
with trait Z?"

## 3. Two Distinct Mechanisms

The session identified two fundamentally different augmentation
patterns. Trying to unify them into one mechanism would produce
something bad at both.

### 3a. In-Surface Augmentation

Operates within an extension's UI. Needs injection points. Runs
when the host surface is active.

**Examples:** Grammar checking, code completion, AI writing
assistance, spell check, formatting tools, annotation overlays,
collaborative cursors, lint warnings.

**Primary mechanism: Slot injection (~60%+ of cases).**

Host panels declare named slots — `toolbar`, `context-menu`,
`status-bar`, `overlay`, `gutter`, `inline` — as part of their
panel declaration. Augmenter extensions declare which traits they
augment and which slots they inject into. The shell mediates the
injection based on trait matching and RBAC.

```
Host panel (Notes editor):
  surface_traits: ["text-editor", "rich-text", "markdown"]
  slots: { "toolbar": {}, "status-bar": {}, "context-menu": {} }

Augmenter (GrammarBot):
  augments_traits: ["text-editor"]
  injects_into: ["toolbar", "status-bar"]
```

The shell sees that Notes' editor has the `text-editor` trait,
GrammarBot augments `text-editor` surfaces, and GrammarBot wants
the `toolbar` and `status-bar` slots. If RBAC permits, the shell
injects GrammarBot's components into those slots when the editor
panel is active.

Notes doesn't know GrammarBot exists. GrammarBot doesn't know
Notes exists. Install a new markdown editor with `text-editor`
trait and GrammarBot augments it automatically.

This is the VS Code "contributes" model — extensions contribute
to `editor/context`, `editor/title`, etc. without naming specific
editors.

**Secondary mechanism: Event stream / pipeline (do not block).**

For augmentation that isn't UI injection — content analysis,
background processing, transformation pipelines.

Host panels emit surface events for their traits:
- `text-editor:content-changed`
- `text-editor:selection-changed`
- `text-editor:cursor-moved`

Augmenters subscribe to events on matching traits, process them,
and push results back through a standardized augmentation-results
channel. The host reads results generically — it knows how to
render "diagnostics" (underlines, markers, suggestions) but
doesn't know who produces them.

This is the Language Server Protocol model. The editor knows how
to display diagnostics; it doesn't know which language server
produced them.

**Pipeline variant:** Content passes through a chain of processors.
Raw text → grammar check → AI suggestions → formatted output. The
shell (or a kernel-level orchestrator) manages the pipeline based
on trait matching. Useful for LLM integration — "run this content
through every augmenter that operates on `text-editor` surfaces
and collect their annotations."

### 3b. System-Level Services

Operates at the boundary between the user and any surface. Does
not inject into panels. Runs at the shell/panel-SDK level.

**Examples:** "Read this selection aloud" (TTS applied to panel
content), "Transcribe this audio clip I just recorded" (STT into a
panel's text input), real-time translation overlay, accessibility
services, system-wide search indexing, clipboard enhancement.

**Why this is distinct from in-surface augmentation:**

System services don't augment "the Notes editor" — they operate on
*the content the active panel provides or accepts*. TTS doesn't
know it's reading a note; it reads text the shell extracted from
the active panel via a standard "provide text content" interaction.
STT doesn't know it's dictating into chat; it feeds text into the
active panel's "accept text input" surface.

**Narrower than previously framed:** System services are **not a
distinct provider mechanism**. The TTS or STT or translation
*capability* is provided by some ordinary extension — typically
ai-bridge (see `DISCUSSION-ai-bridge.md`). The system-service layer
is the **routing of panel content to those capabilities**, done by
the shell / panel SDK.

Concretely, the three-layer picture:

1. **Capability layer**: `ai-bridge` extension registers `ai:1:tts`,
   `ai:1:stt`, `ai:1:translate`. Admin has configured each to a
   provider (Kokoro sidecar, Whisper sidecar, Venice API, etc.).
   This is existing Plinth architecture — no new mechanism.
2. **System-service layer (this mode)**: the shell provides "read
   aloud" as an action available to any panel exposing a
   "provides-text-content" trait. When the user invokes it, the
   shell extracts the text (via a standard surface interaction) and
   calls `ai:1:tts` through the normal capability dispatch.
3. **Panel SDK layer**: panels declare which content-producing and
   content-consuming traits they expose (a "text-editor" trait, an
   "accepts-text-input" trait, a "provides-audio-selection" trait).
   Panels don't know about specific system services; they know
   about traits.

What the composition arc decides for Mode 3:

- Which traits system services rely on (`provides-text-content`,
  `accepts-text-input`, `provides-audio-selection`, etc.).
- The panel-SDK interactions for content extraction / injection.
- Which system services ship in the shell's built-in action palette.
- Per-user preferences UX ("my preferred TTS action" routes through
  the user's configured ai-bridge TTS provider).

What the composition arc does **not** decide for Mode 3:

- The capabilities themselves (owned by ai-bridge).
- The providers backing the capabilities (owned by ai-bridge).
- The actual TTS / STT / translation engines (owned by providers).

This cleanup reduces Mode 3 from "a system to design" to "a thin
panel-SDK layer over capabilities ai-bridge already exposes." The
heavy lifting happens in ai-bridge.

**Dependencies for Mode 3:**
- ai-bridge shipped with TTS/STT/translation capabilities
  configured.
- Composition arc's trait vocabulary (§4) established.
- Panel SDK support for content extraction / injection via traits
  (new SDK work).

Sequencing: Mode 3 should not begin before ai-bridge provides the
underlying capabilities. A shell that exposes "read aloud" but
routes to an unconfigured capability is worse than no "read aloud"
button.

## 4. The Slot Vocabulary Problem

The slot vocabulary (`toolbar`, `status-bar`, `context-menu`,
`overlay`, `gutter`, `inline`, ...) becomes a contract. It needs
to be:

- **Standardized enough** that augmenters can target slots by name
  without knowing which extension provides them. If Notes calls it
  `toolbar` and Chat calls it `action-bar`, GrammarBot needs to
  know both names or one of them needs to change.
- **Extensible enough** that extensions can define custom slots for
  domain-specific augmentation points. A video editor might have a
  `timeline-overlay` slot that no text editor needs.
- **Sparse enough** that extensions aren't required to implement
  every standard slot. A minimal text editor might only declare
  `toolbar` and `status-bar`. A rich IDE might declare 12 slots.

Proposed approach (to be refined in the composition arc):

- A small set of **standard slot names** defined in the architecture
  doc. Extensions that declare the `text-editor` trait are expected
  to support at least `toolbar` and one of `status-bar` or `inline`.
  This is a convention, not enforced at install time.
- Extensions can declare **additional custom slots** beyond the
  standard set. Augmenters that target custom slots are coupled to
  extensions that provide them — this is acceptable for domain-
  specific augmentation.
- The standard slot list grows slowly through architecture sessions.
  Adding a new standard slot name is a decision, not a PR.

## 5. RBAC for Augmentation

Open question, flagged for the composition arc:

**Does the user control which augmenters attach to which surfaces?**

Probable answer: yes. Same "default applications" UX pattern used
for content-type resolution. User settings include an "augmenters"
section:

- "Grammar checking for text editors: GrammarBot [change]"
- "Code completion for code editors: CopilotExt [change]"
- "AI suggestions for text editors: none [enable]"

Admin can set defaults. Users override per-trait. This reuses the
three-tier resolution pattern (user pref → admin default →
first-installed) already established for content-type handlers.

The RBAC question also covers: can an augmenter read the host
panel's content? A grammar checker reading your text is expected.
A "helpful toolbar button" extension silently reading your text is
a privacy concern. The capability model should require augmenters
to declare what they read, and RBAC should gate it.

## 6. LLM Integration Implications

LLM integration (via the ai-bridge extension — see
`DISCUSSION-ai-bridge.md`) is a specific and important case of
cross-cutting composition. An LLM augmenter:

- Subscribes to `text-editor:content-changed` events (event stream)
- Processes content through an `ai:1:chat` or `ai:1:chat_structured`
  capability (pipeline)
- Pushes suggestions back as augmentation results (inline
  suggestions, status-bar indicators)
- May inject UI into `toolbar` slots (a "suggest" button) and
  `overlay` slots (suggestion popover)

This exercises all three sub-mechanisms: slot injection for UI,
event stream for content awareness, pipeline for processing. LLM
integration is the stress test for the composition architecture.
If the composition arc can support a grammar checker, a code
completer, and an LLM writing assistant all augmenting the same
text editor simultaneously without conflict, the architecture is
right.

Note that the LLM augmenter itself doesn't own provider routing —
it calls `ai:1:chat` and ai-bridge handles which provider responds.
This keeps the augmenter focused on augmentation semantics
(when to suggest, what to show) rather than provider management.

## 7. What Is Decided Now

Decisions made in this session, landed in the shell doc and the
architecture decomposition:

1. **`surface_traits` is a reserved field in `panels.json`.** Array
   of strings. Validated, stored in `plinth.panels.declaration`,
   ignored until the composition arc. Early extensions should
   declare traits even before augmenters exist.

2. **`slots` is a reserved field in `panels.json`.** Object mapping
   slot names to slot definitions. Same treatment as
   `surface_traits` — reserved, stored, not interpreted.

3. **`augments_traits` is a reserved manifest-level field.** Exact
   location (`capabilities.json` vs. a new `augmenters.json`)
   deferred to the composition arc; 0.4 parsers accept either.

4. **The panel SDK must not foreclose composition.** Constraint 10
   in the shell doc: the SDK's event model must be extensible so
   surface events and slot injection can be added without breaking
   existing API. Panel containers must not prevent future component
   injection.

5. **In-surface augmentation and system-level services are two
   mechanisms.** They share the trait vocabulary but differ in
   injection model, RBAC model, and lifecycle.

6. **Slot injection is the primary (~60%+) mechanism.** Event
   stream and pipeline are secondary — the architecture must not
   block them, but slot injection is designed first.

7. **The composition arc depends on real extensions.** Files, Notes,
   ai-bridge, and at least one augmenter must be stable before the
   composition arc can produce good abstractions. Designing in the
   abstract produces wrong abstractions.

8. **Extension docs project (~0.8.0).** A separate Gitea project for
   extension design docs, isolated from kernel code. The composition
   arc needs a place for trait vocabulary documentation and extension
   authoring guidance.

## 8. What Is Deferred

To the composition arc (DESIGN-composition-v09x.md, Scale 2):

- Full slot injection mechanism design
- Event stream / pipeline mechanism design
- Standard trait vocabulary (which traits exist, what slots each
  implies)
- Standard slot vocabulary (names, semantics, rendering rules)
- `augments_traits` manifest field semantics
- Augmentation RBAC model
- Augmenter lifecycle (what happens when augmenter is uninstalled
  while a host panel has its components injected?)
- Conflict resolution (two grammar checkers both want the toolbar
  slot — who wins?)
- Performance implications (10 augmenters all subscribing to
  content-changed events on a large document)
- LLM integration as the stress-test case
- Mode 3 panel-SDK additions: trait-based content extraction
  (`provides-text-content`, `provides-audio-selection`, etc.),
  content injection (`accepts-text-input`), and action-palette
  integration. The capability side of Mode 3 is owned by
  `DISCUSSION-ai-bridge.md`, not this arc.

## 9. What This Document Is Not

This is a conversation capture, not a design doc. It records the
design space as explored in one session (plus the Mode 3 narrowing
from drafting ai-bridge). The composition arc will produce the
actual design doc with precise schemas, SDK contracts, and
implementation milestones. This document is input to that arc —
it says "here's where we were thinking" so the composition session
doesn't start from zero.

Do not treat claims in this document as commitments. The slot
injection model might turn out to be wrong when real extensions
test it. The trait vocabulary might need a different shape. The
two-mechanism split might collapse into one or expand into three.
The composition arc decides; this document sketches.
