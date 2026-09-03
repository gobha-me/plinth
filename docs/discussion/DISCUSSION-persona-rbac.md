# Persona RBAC — Design Space Summary

**Status:** Discussion capture — not a design doc, not authoritative
**Source:** 2026-04-17 session, building on `DESIGN-memory.md` and
`DISCUSSION-post-shell-application-order.md §13`
**Participants:** the maintainer (Architect) + Claude (Architecture Session)
**Feeds into:** Future DESIGN-persona-rbac.md (likely ~0.13+, after
ai-bridge is fully shipped)
**References:**
- `architecture/01-identity.md §2` (Groups and RBAC — the substrate)
- `architecture/02-capabilities.md §1` (capability registry — where
  delegated rules have to land)
- `DESIGN-memory.md` (memory scopes: user, persona, workspace,
  organization)
- `DISCUSSION-ai-bridge.md` (the home for `ai:1:chat` — personas are
  a consumer of ai-bridge, not a component of it)
- `DISCUSSION-post-shell-application-order.md §9` (step-up auth
  primitive — a dependency for the persona delegation model below)

---

## 0. Scope Note

This discussion is about a specific extension pattern (personas)
and the kernel-level RBAC primitive that pattern needs (delegated
rules). The kernel work is small and specific; the extension work
is a future design doc. This document exists so the kernel work
does not accidentally foreclose the persona pattern, and so the
persona design starts from a shared model rather than re-deriving
the question space.

The kernel commitment from this document is a single concept:
**delegated rules** as a distinct class of rule grant, alongside
group-derived rules. Everything else is extension-layer design
deferred to the persona arc.

---

## 1. Terminology

Three terms that have been used loosely and need tightening:

**Model.** A raw LLM endpoint. Has no rules of its own. When a user
calls an LLM directly, the call runs under the user's identity with
the user's rules. The model is a compute resource, not a principal.

**Persona.** A baked combination of:
- A system prompt.
- An allowed tool list (capabilities the persona may call).
- A rule set delegated to the persona by its creator.
- Optionally, persona-scoped memory (per `DESIGN-memory.md`).

A persona IS a principal in the RBAC sense. It has a stable
identity, an auditable set of rules, and its own memory scope.
Calls made *through* a persona run under the persona's identity,
not the calling user's.

**Persona invocation.** A user talking to a persona. The user is
the caller; the persona is the executor. RBAC checks happen at the
persona boundary, not at the user boundary.

This is the Unix `sudo` analogy the architect reached for: the
persona is a `sudoers` entry — it defines what actions are allowed
under a different principal, invokable by specific users.

## 2. The Three Questions

Personas surface three distinct RBAC questions, which the
discussion earlier conflated. Separating them makes the design
space tractable.

**Q1: Creator → Persona.** How does a persona inherit rules from
the user who created it?

**Q2: Persona → Execution.** When a persona acts (calls tools,
reads memory, writes state), under whose identity and rules does
that action run?

**Q3: Persona → Sharing.** When a persona is shared with another
user, what rights and visibilities transfer?

Each has a distinct answer. The answers compose; they do not
override each other.

## 3. Q1 — Creator → Persona: Delegation, Not Inheritance

A persona's rules are **explicitly delegated** by its creator, not
automatically inherited from the creator's rule set. The creator
chooses, at persona creation time, which rules to delegate from
their own rule set into the persona.

The kernel invariant: `persona.rules ⊆ creator.rules_at_creation_time`.

This looks like inheritance with an extra explicit-grant step. It
is different in three ways that matter:

- **Explicit delegation is auditable.** Every rule the persona holds
  has a specific provenance ("creator granted `jellyfin.restart` to
  this persona on 2026-04-17 at 14:32") rather than being implied
  by group membership at invocation time.
- **Delegation does not track creator changes automatically.** If
  the creator is later removed from a group that held a delegated
  rule, the persona retains that rule. The creator explicitly
  revokes if they want the persona to lose it. This is intentional
  — silent permission loss is worse than stale delegations for
  both audit and reliability.
- **Delegation can be narrower than inheritance.** A creator who
  has `kernel.admin` (which implicitly grants everything) can
  delegate a persona just `jellyfin.restart` and `jellyfin.logs`.
  The persona is not an admin; it is a narrowly-scoped agent.

**Kernel concept needed:** the RBAC model gains a third class of
rule grant alongside direct and group-derived: **delegated**.
Delegated rules live on the persona principal, point back to the
creator who granted them, and survive creator group changes. Full
audit trail on delegation, revocation, and use.

This is the single concrete kernel requirement from this document.

## 4. Q2 — Persona → Execution: The Sudo Model

When a user talks to a persona and the persona invokes a tool, the
tool runs under **the persona's identity, not the user's**.

This is Option D from the earlier framing — "sudo for LLMs." The
creator of the persona is saying "these specific capabilities can
be invoked by anyone allowed to talk to this persona, under the
persona's authority, without the invoking user needing the
underlying rules."

Concretely, when a user asks a shared persona "restart Jellyfin":

1. The user's message reaches the persona. The user's rules do
   not need to include `jellyfin.restart`.
2. The persona's LLM decides to call the `k8s:1:restart_pod`
   capability.
3. The capability dispatch runs under the persona's `UserContext`.
   The persona has `k8s.pod.restart` in its delegated rules.
4. The capability executes. Audit records: "user X invoked persona
   Y, which executed `k8s:1:restart_pod` under delegated rule
   `k8s.pod.restart` originally granted by creator Z on date D."
5. The persona responds to the user.

The invoking user does not gain the persona's rules. They gain
the ability to *ask the persona to do things*, mediated by the
persona's system prompt and allowed tool list. The system prompt
is the soft constraint ("you are a homelab helper focused on
media services"); the allowed tool list is the hard constraint
(the persona cannot call capabilities outside its allowed list
even if its system prompt is jailbroken).

**Step-up auth applies here.** Capabilities the persona can invoke
that are flagged step-up-required (per
`DISCUSSION-post-shell-application-order.md §9`) prompt the
invoking user for confirmation before execution. A persona that
can restart a Jellyfin pod without confirmation is convenient; a
persona that can power-cycle the whole cluster without
confirmation is dangerous. The step-up flag lives on the
capability, not on the persona, so it applies uniformly.

**Audit is the critical backstop.** Every persona invocation is
audited with three identities: invoking user, persona, creator.
This is what makes the sudo model safe. If a persona misbehaves
or is misused, the audit log shows exactly who asked it to do
what and which delegated rule authorized the action.

## 5. Q3 — Sharing: Personas Have a Visibility Scope

A persona has a visibility scope, set by its creator:

- **`private`**: only the creator can invoke. Default.
- **`shared`**: specific users can invoke. Creator lists the users
  (or groups) allowed to invoke.
- **`public`**: any authenticated user can invoke.

Sharing a persona means granting invocation rights to additional
users. It does NOT change the persona's delegated rules, its
memory scope, or its execution identity. All of those remain as
the creator configured them.

What sharing changes:

**Execution rules do not change.** A shared persona runs under its
own delegated rules, regardless of who invokes it. Your wife
invoking the Jellyfin-helper persona causes `k8s:1:restart_pod` to
execute under the persona's identity, using the persona's
delegated `k8s.pod.restart` rule. Your wife does not gain that
rule; the persona uses it on her behalf.

**Memory access follows the scope rules from `DESIGN-memory.md`.**
This is the privacy question, and the answer is a hard separation:

- **User-scoped memory is never visible to shared personas.** A
  persona invoked by user X cannot read user X's private memory,
  period. The persona has no business reading the user's personal
  notes, preferences outside persona-relevant ones, or prior
  conversations the user had with other personas or models.
- **Persona-scoped memory is always visible to the persona.** This
  is the persona's own memory — things the persona has learned or
  been told in the course of its existence. All invoking users
  share this memory view because they are talking to the same
  persona.
- **Workspace/organization memory follows its own RBAC.** Per the
  memory doc, workspace and organization memory have their own
  access rules; the persona respects those rules using the
  invoking user's identity for memory access, not the persona's.
  (A persona shared with a user who cannot see a workspace memory
  does not expose that memory through persona conversation.)

**The memory rule, compressed:** personas read the invoking user's
memory through the invoking user's permissions; personas write
only to persona-scoped memory; personas never read user-scoped
memory of anyone other than the creator.

The last clause ("other than the creator") is the subtle one. A
creator's user-scoped memory is not visible to shared personas
either, because the persona is a separate principal and
user-scope means user-only. If the creator wants some of their
knowledge available to personas they create, that knowledge goes
into persona-scoped memory at persona creation time. The creator
curates what the persona knows; the persona does not see
everything the creator knows.

This is the "parrot in public spaces" discipline the architect
named. The persona's public behavior is exactly and only what
the creator packed into it — system prompt, tool list, persona
memory. Nothing leaks from the creator's private world by
accident.

**Chat history is a specific case of this.** When a persona
accesses a "chat search" tool or similar, it searches within the
invoking user's chat history, not the creator's. This is the
architect's specific example: "only on chat history should the
user's own be visible." Generalized: tools that access per-user
state use the invoking user's identity for that access, even when
the persona itself is running under its own identity for the tool
call. This is a dual-identity model — persona identity for
authorization of the tool call, invoking user identity for the
data the tool operates on.

## 6. Worked Example — The Jellyfin Persona

This is the scenario the architect raised. Walking it through the
full model:

**Setup.**
- Creator (the maintainer) has `kernel.admin` and thus implicitly holds
  `k8s.pod.restart`, `jellyfin.admin`, etc.
- Creator creates a "Jellyfin Helper" persona:
  - System prompt: "You are a homelab helper focused on Jellyfin
    media server. You can check logs, restart pods, and answer
    questions about why Jellyfin might be misbehaving. You cannot
    touch anything else."
  - Allowed tool list: `k8s:1:describe_pod`, `k8s:1:logs`,
    `k8s:1:restart_pod` (scoped to jellyfin-namespace),
    `ai:1:chat` (for reasoning).
  - Delegated rules: `k8s.pod.describe.jellyfin`,
    `k8s.pod.logs.jellyfin`, `k8s.pod.restart.jellyfin`.
  - Persona memory: notes about Jellyfin's typical failure modes,
    known config quirks, contacts for upstream issues.
  - Visibility: `shared` with Wife (specific user, not a group).

**Invocation.**
1. Wife opens the chat extension and talks to the Jellyfin Helper
   persona.
2. Wife: "Jellyfin stopped working again, can you fix it?"
3. The persona's LLM (running under the persona's identity, via
   ai-bridge) decides to check the logs first.
4. The persona calls `k8s:1:logs(jellyfin-pod)`. The capability
   dispatch runs under the persona's identity. RBAC checks the
   persona's delegated rules and finds `k8s.pod.logs.jellyfin`.
   Allowed. Logs are returned.
5. The persona's LLM analyzes the logs, identifies a memory
   pressure issue.
6. The persona decides to restart the pod. Calls
   `k8s:1:restart_pod(jellyfin-pod)`.
7. `k8s:1:restart_pod` is flagged as step-up-required.
   Plinth prompts Wife: "The Jellyfin Helper is about to restart
   the Jellyfin pod. Confirm? [Yes] [No]"
8. Wife clicks Yes. The step-up confirmation token is attached
   to the call. Execution proceeds under the persona's identity
   using `k8s.pod.restart.jellyfin`.
9. The persona reports back: "Jellyfin is restarting. Give it a
   minute, it looked like it ran out of memory — I've left a
   note for the maintainer to check if we need to bump the limit."
10. The "note for the maintainer" is written into persona-scoped memory,
    visible the next time any invoker talks to the persona.

**Audit trail for this single interaction:**
```
14:23:01  user_invocation     user=wife persona=jellyfin-helper
14:23:04  capability_called   persona=jellyfin-helper cap=k8s:1:logs
                              rule=k8s.pod.logs.jellyfin (delegated by jeff)
14:23:07  capability_called   persona=jellyfin-helper cap=k8s:1:restart_pod
                              rule=k8s.pod.restart.jellyfin (delegated by jeff)
                              step_up=confirmed_by=wife
14:23:09  memory_write        scope=persona persona=jellyfin-helper
                              key=jellyfin_memory_issue_note
```

Notice what Wife gained: the ability to fix Jellyfin when it
breaks. Notice what Wife did not gain: any k8s rule at the user
level, any ability to touch non-Jellyfin pods, any visibility
into the maintainer's personal memory or chat history.

Notice what the creator paid for this capability: the deliberate
work of packaging a persona with exactly the right scope. The
persona is harder to create than just "give Wife admin access"
— and that effort is the whole point. The creator is taking the
time to shape a narrow, safe agent rather than broadening an
identity.

## 7. What Is Decided Now

Three reservations. Two are kernel commitments; one is a memory-
doc alignment.

1. **Kernel RBAC gains a "delegated" rule class.** Alongside
   direct-granted rules and group-derived rules, the kernel
   supports rules granted to a principal via delegation from
   another principal. Delegated rules carry:
   - The grantor (who delegated).
   - The grantee (the principal receiving the rule — typically a
     persona).
   - The grant timestamp.
   - The grant context (which persona, for persona rules; other
     contexts reserved).
   - The audit trail on every use.
   The concept is reserved now; the implementation lives alongside
   the persona arc, probably in the 0.13+ window.

2. **Personas are principals, not objects.** In the RBAC model, a
   persona has a `UserContext`-analog (probably
   `PersonaContext : UserContext`) that is distinct from any
   user's `UserContext`. Persona-executed capabilities run under
   persona identity, not invoking-user identity. This is
   reservation-level — no code changes needed yet, but the
   `UserContext` type and its serialization should not assume
   "user" as the only principal kind.

3. **Memory scope enforcement is strict.** The memory doc's scope
   categories (`user`, `persona`, `workspace`, `organization`)
   are enforced by the kernel at the memory API level, not by
   extension-level convention. In particular:
   - User-scoped memory is never returned to a principal other
     than the owning user, even through persona invocation.
   - Persona-scoped memory is returned to any invoker of the
     persona.
   - Dual-identity tool calls (persona executes, invoking user's
     data accessed) use the invoking user's identity for memory
     lookups of user scope.
   This aligns with `DESIGN-memory.md §Scoping & Access` — the
   RBAC-orthogonal access rules the memory doc flags are where
   this enforcement lives.

## 8. What Is Deferred

To DESIGN-persona-rbac.md (Scale 2 or 3, targets post-ai-bridge):

- Persona creation UX (which rules are delegable? how does the
  UI present the delegation surface?).
- Revocation UX (how does a creator revoke a persona's rule?
  what happens to in-flight invocations at revocation time?).
- Visibility changes (can a persona be made `public` after being
  `shared`? is there a quarantine period?).
- Delegation chains (can a persona be granted a rule by another
  persona's creator? probably no; flag as rejected below).
- Persona memory export / portability (does "share a persona
  template" exist as a distinct concept from "give someone
  invocation rights"?).
- Quota and cost semantics (a shared persona hitting ai-bridge
  consumes the creator's API quota, or the invoker's? or split?).
- The relationship between persona step-up requirements and the
  invoking user's step-up state (if wife just confirmed step-up
  for one Jellyfin restart, does a follow-up restart require
  another confirmation?).
- UI for "what can this persona do?" — making the delegated rule
  set visible to invokers so they know what they're authorizing.
- Persona impersonation detection and fraud — if an audit shows a
  persona did something the creator thinks it shouldn't have,
  what's the forensic process?

## 9. What Is Rejected

- **Transitive delegation.** A persona cannot delegate its rules
  to another persona. Only users delegate, and they delegate
  only rules they themselves hold. This prevents privilege-
  laundering chains where creator A creates persona P with rule
  R, persona P creates persona P' with rule R, and now P' holds
  R without any direct link to A's grant. Flat delegation only.
- **Persona-granted sharing.** A persona cannot share itself with
  new users. Sharing is a creator action. A persona is an agent,
  not a principal that can extend its own reach.
- **Implicit creator identity on shared persona calls.** Tools
  invoked by a shared persona run under the persona's identity,
  not the creator's identity. "The persona did it on behalf of
  the creator" is a conceptual frame, not an RBAC frame. The
  persona is its own principal; audit is how the creator-link
  is preserved, not runtime-identity.
- **Rule elevation through persona.** A persona cannot acquire
  rules its creator never had. The kernel invariant
  `persona.rules ⊆ creator.rules_at_creation_time` is strict. A
  creator who is later granted new rules does not retroactively
  extend their existing personas; they must explicitly re-
  delegate into each persona. This is intentional friction.
- **Reading user-scoped memory across principals.** Under no
  circumstances does a persona read user-scope memory belonging
  to a user other than the invoker. Even for the creator's own
  memory, the persona does not get access — the creator curates
  the persona's knowledge into persona-scope memory at creation
  time. User-scope is user-only.

## 10. What This Document Is Not

Conversation capture, not a design doc. The persona arc produces
the real design when it is picked up (post-ai-bridge, probably
~0.13+). The kernel reservations in §7 are the only binding
content — the delegation concept must exist and memory scope must
enforce strictly. Everything else is deferred to the design arc
and the questions in §8.

The scenario in §6 is illustrative, not specification. When the
persona design doc is written, "the Jellyfin persona" is the
canonical example to trace through — not because Jellyfin is
special, but because the household-admin-delegation case is the
hardest one to get right, and getting it right makes everything
else easy.
