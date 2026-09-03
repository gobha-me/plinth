# Service-principal pattern

**Status:** Discussion. Pre-commitment thinking.
**Cited by:** `discussion/llm-augmented-game.md` (and future autonomous-extension docs).
**Scope:** Kernel.

## Problem

Plinth's principal model is user-shaped: every authenticated action runs as a user. This is correct for interactive UIs but breaks for code that runs without a user pulling the trigger.

Three cases pressure-test the gap:

1. **Scheduled tasks.** An extension's cron-style hook fires at 3 AM. No user is logged in.
2. **Webhook handlers.** An external service posts to a Plinth endpoint. No user initiated it.
3. **Autonomous agents.** An LLM-backed NPC (e.g., AI Houses in an EotFS-style game) acts on its own initiative during a turn-resolve phase.

Implementing each as "the install user" or "always the host" produces three failures:

- **Quota attribution misattributes spend.** AI calls fired by the game charge whichever user the call ran as. Audit logs read "alice paid for House Decados" — misleading because alice didn't ask.
- **Audit log integrity breaks.** "alice did X" when alice was offline is wrong.
- **Authorization scope leaks.** A call running as alice can do anything alice can do — even when the *extension* should be more constrained.

## Decided now

**Extension-as-principal becomes a first-class type in the kernel identity model.** The principal union grows from `User` to `User | ExtensionPrincipal`. Every kernel subsystem that consumes principals — audit log, capability check, RBAC, quota enforcement — recognizes both.

Consumers (ai-bridge, the storage layer, the audit logger) become transparent: they ask the kernel for the current principal and apply rules; they don't know or care which type it is. This is the kernel-vs-ai-bridge boundary — ai-bridge does not invent its own principal model, it consumes the kernel's.

## Reserved schema fields

To avoid foreclosing the design while it's in discussion:

- `audit_log.principal_type` — enum, includes `user` and `extension` (room for more).
- `audit_log.principal_id` — opaque ID, resolves against either users or extension installs depending on type.
- `quota.principal_ref` — same shape, allows quota records keyed on extension principals.

These exist as nullable columns or reserved type tags until the mechanism lands.

## Open questions (deferred)

- **Hierarchy.** Is `ExtensionPrincipal` one type, or does it split into "extension acting on its own" vs. "extension acting on behalf of user X"? The latter is needed for cases like "alice configured the homecare extension to notify her contacts" — alice-driven intent, extension-driven execution. Not urgent until a use case forces the question.
- **Quota attribution policy.** Does an instance get a global "extension quota pool"? Per-extension-install quotas? Inherited from the install-time user? Configurable per extension? This is policy, not mechanism — answer when a real tenant complains.
- **RBAC roles for extensions.** Do extensions have role definitions analogous to users, or are capabilities the only authorization primitive for extension principals? Lean capabilities-only for simplicity; defer commitment.
- **Existing scheduled-task and webhook contexts.** Code-aware re-eval question. If the kernel already has timer-context or webhook-context paths with ad hoc principal handling, those need consolidation under this proposal. The audit is downstream of the unified type landing.
- **Extension principal lifecycle.** Created on extension install? Destroyed on uninstall? What happens to audit log entries referencing a destroyed principal? Tombstone or orphan tolerance needed; defer.

## Consumers

Subsystems and extensions that benefit once this lands:

- ai-bridge (service-principal mode for extension-driven LLM calls).
- The eventual scheduler primitive (cron-style hooks).
- The webhook dispatch route (kernel-owned per WebSocket-primary architecture).
- Any LLM-augmented extension (games, summarizers, agents).
- The homecare extension's medication-check agent.

## Pressure for promotion

Discussion → fuzzy: when the first autonomous extension is on the roadmap. Likely candidates are chore reminders or the scheduler primitive itself, both of which precede the game by several milestones.

Fuzzy → medium: when ai-bridge's design begins, since ai-bridge has to consume this. ai-bridge cannot be designed without committing to a principal model.

Medium → strong: at most one milestone before the first autonomous-extension milestone enters the next-N window.

## Methodology note

This doc has multiple downstream consumers across both the kernel arc and several extension arcs. The current methodology (per the LLM-assisted development methodology doc) handles discussion-doc dependencies via citation, not formalism. That works here, but if the autonomous-extension category produces three or more discussion docs all citing this one, "which doc promotes first" becomes a real coordination question. Worth flagging at the next methodology re-eval.

## Claude Design prompt

> Generate a structural diagram of the kernel principal model. Two source nodes at the top: "User principal" (c-blue) and "Extension principal" (c-purple). Both feed into a unified "Principal context" box (c-gray) in the middle. From the principal context, arrows fan downward to four consumers: audit log, capability check, RBAC, quota enforcement. Below those, ai-bridge sits as a meta-consumer that uses all four. Label the user→context path "today's path" and the extension→context path "proposed". The diagram's job is to show that consumers don't fork on principal type — they consume a unified context.
