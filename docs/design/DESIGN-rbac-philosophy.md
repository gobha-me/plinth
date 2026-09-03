# DESIGN-rbac-philosophy

**Status:** Approved v1  
**Scale:** 2 — Foundational philosophy for multi-version RBAC arc  
**Traces to:** architecture/01-identity.md §2 (Groups and RBAC)  
**Milestone:** 0.1.4–0.1.5 (Groups & RBAC enforcement) and all future permission-related work  
**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)  
**Decision Date:** 2026-04-15  
**Author:** the maintainer (Architect) + Grok 4.20 Multi-Agent (Architecture Session)

---

## Decision

The Plinth RBAC system follows a **least-privilege, additive group model** inspired by Linux `sudo`/`wheel` rather than a single omnipotent `root` account.

- Permissions are granted exclusively through **rules** registered by extensions (or kernel primitives).
- A user’s effective permissions are the **union** of all rules granted to any group they belong to.
- Users may belong to multiple groups. A user in both `backup-operators` and `package-managers` receives the union of rules from both.
- The built-in `admin` group is a convenience group that, by default, is granted a comprehensive `kernel.admin` rule (or set of rules) granting full system control. It is **not** an absolute bypass that skips the RBAC system entirely.
- Granular rules are encouraged for all significant operations (`system.backup.run`, `packages.install`, `users.manage`, `rbac.rules.grant`, `terminal.shell.execute`, etc.).
- The `everyone` group starts with zero rules.

This philosophy prioritizes auditable, delegable, least-privilege administration while still allowing a small number of true administrators to control the entire system.

---

## Core Principles

1. **Additive Union Model**  
   A user’s permissions are the union of rules from every group they belong to. There is no “primary group” or negative rules. This matches the architecture document and is non-negotiable.

2. **Least Privilege by Default**  
   No user should receive more permission than their role requires. Fine-grained rules (`system.backup.run`, `packages.manage`, `rbac.groups.manage`) should be created and granted to specific groups rather than relying on `admin`.

3. **Admin as “Wheel”, Not “Root”**  
   The `admin` group is granted a powerful rule (or set of rules) that effectively grants full control. However, membership in `admin` still flows through the normal rule-checking path. This allows auditing of admin actions and makes it possible to create “almost-admin” groups that lack only a few sensitive rules.

4. **Explicit Rule Declaration**  
   Every protected operation must declare the exact rule(s) it requires. No inference from paths, methods, or resource ownership is allowed in the base system.

5. **Separation of Concerns**  
   - Groups & rule storage: ICD-0.1.4
   - Enforcement middleware: ICD-0.1.5
   - Rule registration & validation: 0.4.x package system
   - Capability-to-rule mapping: 0.2.x capability registry

---

## Default Rules and Groups

On first bootstrap the kernel creates:

- `admin` group (built-in)
- `everyone` group (built-in)
- `kernel.admin` rule (granted to `admin` by default)

Extensions register their own rules via `rbac.json`. The kernel may register additional kernel-level rules (`users.manage`, `groups.manage`, `rbac.rules.grant`, `system.backup.run`, etc.) during 0.1.7–0.7.

---

## What Must Not Be Decided Yet

- Specific list of kernel-level rules (beyond `kernel.admin`). These will be refined during implementation of 0.1.7 (audit), 0.4 (packages), 0.7 (metrics/scheduler), and 0.8 (sidecars).
- Whether `admin` receives a single `kernel.admin` rule or is automatically granted every registered rule. The exact mechanism must be decided in an architecture session once more rules exist.
- Rule wildcards (`*.manage`, `terminal.*`), group hierarchy/inheritance, dynamic rule evaluation, or attribute-based access control (ownership checks).
- Session-level or per-user permission caching strategy.
- Any deviation from the additive union model.
- Any change that would make `admin` an absolute bypass outside the normal rule-checking path.

All future permission-related design documents and ICDs **must** conform to the least-privilege, additive, explicit-rule philosophy defined here.

---

## Open Questions

1. Should there be a small set of true “kernel.root” rules that can only be granted to `admin` and are never granted to other groups?
2. Should we provide convenience “role-like” groups (e.g. `operators`, `auditors`) as built-in groups with sensible default rules? (Currently discouraged but open for re-evaluation after 1.0.)
3. Exact naming convention for kernel-level rules (`kernel.*`, `system.*`, or `plinth.*`).

---

**This document is the permanent philosophical authority on all RBAC and permission decisions in Plinth.** Any code session, design document, or ICD touching groups, rules, capabilities, or authorization **must** read this document first. Deviations require a new architecture session and explicit revision of this design document.