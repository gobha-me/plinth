# Extension HTTP Surface — proposal

**Status:** Discussion document. Not a commitment.
**Status (2026-04-29):** Ratified — see [`architecture/05-extensions.md §6`](../architecture/05-extensions.md) for the normative contract. This document remains the design-history origin record.
**Source:** Architecture conversation, 2026-04-27 (origin); builds on
the 2026-04-16 site-platform / extension-routing conversation that
concluded extensions did not own HTTP surface under the architecture
as it then stood.
**Participants:** the maintainer (Architect) — proposal author.
**Scheduled for ratification:** `0.6.0.N Architecture session:
extension HTTP surface` (paper slot, ROADMAP §0.6).
**Relates to:** kernel architecture (HTTP surface, route registration,
trust boundary); extension package model; sidecar execution mode;
upcoming Files arc and any future protocol-compatibility extension.
**References:**
- `docs/architecture/` (kernel-owned route inventory, capability
  dispatch model, RBAC; this proposal narrows the kernel-owned set
  and defines the primitive that lets extensions own everything else)
- `src/kernel/rbac/enforcement.cpp` rule-requirement registry —
  thread-safety pattern that this primitive's runtime route table
  reuses
- `DISCUSSION-post-shell-application-order.md` and
  `DISCUSSION-cross-cutting-composition.md` — earlier
  site-platform / extension-routing thinking that this proposal
  resolves

**Scope:** captures a design-space conversation about how Plinth
extensions can own HTTP surface area without putting application-
or protocol-specific knowledge into the kernel. It exists so the
thinking isn't re-invented when an architecture session opens this
question. Architecture cites this only as "see this document for the
thinking." The architecture document does not depend on this. No code
session should read it as a constraint.

---

## The principle

> **Kernel owns primitives. Extensions own application surfaces.**

The test, applied to any piece of HTTP-handling code: *can you
describe what it does without naming an application or a protocol?*

If yes, it's a primitive and belongs in the kernel. The kernel
currently passes this test cleanly:

- `/api/cap/*` — capability dispatch. Describable without naming any
  consumer.
- `/api/storage/{prefix}/{key}` — storage bytes from PVC. Describable
  without naming Files.
- `/api/webhook/{ext}/...` — inbound webhook delivery. Describable
  without naming any specific webhook source.
- `/healthz`, `/metrics`, kernel auth endpoints — kernel internals.

If no, it's an application surface and belongs in an extension. The
case that prompted this conversation: `/remote.php/dav/files/...`,
`/ocs/v1.php/cloud/...`, `/status.php`. You cannot describe these
without saying "Nextcloud." That is the signal that they don't
belong in the kernel.

The reasoning matters because the temptation runs the other way.
LLM-assisted architecture sessions surface "the kernel could just
handle this" as the path of least resistance — it's centralizing,
it's typed, it's easy to write. But every application-specific
endpoint added to the kernel is a future churn surface that ties
kernel releases to application protocol changes. The kernel becomes
a list of every protocol Plinth ever needed to speak, instead of a
small set of primitives that any application surface can be built
on.

The principle is the discipline that prevents that drift.

---

## The problem this surfaces

The principle implies extensions need a way to own HTTP routes.
The current architecture does not provide one.

The conversation that produced this document walked through several
options before settling on a shape:

**JS router.** Catch-all kernel route forwards into a QuickJS
dispatcher; extensions register routes with the dispatcher.
Rejected because it relocates rather than solves the trust and
perf problems — auth still has to live somewhere, RBAC still has
to run, and per-request C++→JS→C++ on high-throughput surfaces
(sync clients, asset serving) is wasteful. Also confused
implementation technique with the architectural question.

**Reserved-prefix list.** Kernel statically registers the prefix
set each protocol-compat extension needs (`/dav/*`, `/ocs/*`,
etc.) at startup. Rejected because it puts protocol knowledge in
the kernel — every new protocol is a kernel rebuild and a kernel
release. Violates the principle directly; the approach scales by
adding kernel exceptions.

**dlopen C++ plugins.** Native plugins loaded into kernel address
space for full perf. Rejected on three counts: C++ ABI is unstable
(every kernel build potentially breaks plugins, or you commit to a
C ABI that makes interesting interfaces painful), trust model
collapses (plugin runs in kernel space with no sandbox), crash
isolation gone (plugin segfault takes down the kernel). Solves a
problem (release coupling) that has cleaner answers.

**Status quo (extensions have no HTTP surface).** Rejected because
the use cases are real: protocol-compatibility extensions
(Nextcloud-client ecosystem inheritance is the immediate one,
others plausible), public extension pages, eventually whatever
shape the share primitive `/s/{token}` settles into.

The shape that emerged from rejecting these:

---

## Shape under discussion

**One catch-all kernel route, runtime route table, manifest-
declared prefixes, install-time conflict check, kernel-side auth,
execution-mode-agnostic dispatch.**

Mechanically:

1. Kernel registers `/{rest:.*}` as a catch-all in Drogon at
   startup, sitting after the fixed primitive routes (which match
   first by virtue of being more specific).
2. Each extension's manifest declares the prefix(es) it claims —
   e.g. `["/dav/*", "/ocs/*", "/remote.php/*", "/status.php",
   "/index.php/login/v2"]`.
3. At install time, the kernel validates that the claimed prefixes
   don't overlap any kernel-owned prefix and don't overlap any
   already-installed extension's prefixes. Conflicts fail the
   install with a clear error.
4. The kernel maintains a runtime route table (prefix → extension +
   handler reference). Same thread-safety pattern as the
   `enforcement.cpp` rule-requirement registry: `shared_mutex` or
   COW atomic, dominated by reads, writes only on install/uninstall.
5. The catch-all handler does prefix lookup, validates auth (PAT
   in the kernel, before dispatch), and forwards to the extension's
   declared handler.
6. The handler reference is execution-mode-agnostic: it can resolve
   to a QuickJS handler in the extension, a sidecar process over
   UDS, or a bundled native handler (Shell, etc.). The kernel
   doesn't care which. The extension's manifest declares the mode.

Auth is unconditionally kernel-side and pre-dispatch. RBAC for
capability-mediated work happens at the capability layer as it does
everywhere else; for direct-handler work, the extension is
responsible for its own authorization story within the auth context
the kernel provides.

---

## Why this works

**The principle is preserved.** The kernel knows nothing about
Nextcloud, WebDAV, OCS, or any future protocol. It knows about
prefixes, handler references, and dispatch. The protocol code lives
where it belongs — in the extension. New protocol-compat extensions
ship without touching the kernel.

**Data structure cost is trivial.** The route table is a prefix
trie or radix tree on path components. Even at 1000 extensions
× 5 prefixes each (5000 entries — implausibly high), it's a few
hundred KB and microsecond-scale lookups. The "giant map in memory"
concern doesn't materialize at any realistic Plinth scale.

**Thread-safety is the same pattern already needed elsewhere.** The
`shared_mutex` fix on the rule-requirement registry is independently
required for 0.4. The route table uses the same shape. No new
concurrency primitive is being introduced.

**Trust isolation is intact.** Kernel-side PAT validation ensures
no extension handler runs without the kernel having already
identified the caller. RBAC over capability calls is unchanged.
Sidecars run as separate processes with their own credentials and
can be confined further by the host.

**Execution-mode flexibility falls out for free.** The handler
reference indirection means the same primitive serves QuickJS
handlers, sidecars, and bundled native handlers. Sidecars give
high-throughput surfaces (WebDAV under sync clients) native perf
without putting native code in the kernel. QuickJS gives
low-throughput surfaces (most public pages) the simplest possible
extension authoring story. The kernel doesn't pick.

**Conflict semantics are clear.** Two extensions cannot both claim
`/dav/*`. The install-time check makes this a deterministic,
debuggable failure with a clear error and clear resolution
(uninstall one, or rename one's prefix). No ambiguity about which
handler "wins" a route.

---

## What the kernel continues to own

The principle requires this list to be short and stable. Current
contents:

- `/api/cap/*` — capability dispatch
- `/api/storage/{prefix}/{key}` — storage bytes
- `/api/webhook/{ext}/...` — inbound webhook delivery
- `/api/auth/*` — kernel auth endpoints (PAT mint, session, etc.)
- `/healthz`, `/metrics` — kernel observability

These are the routes the kernel registers explicitly at startup,
matched before the catch-all. No application or protocol can be
named in describing any of them. New entries to this list should
be rare and require the same justification (genuine primitive,
not application-shaped).

---

## Trust and auth

**PAT validation is kernel-side, pre-dispatch.** The catch-all
handler runs auth in the kernel before resolving the extension
handler. An extension never sees an unauthenticated request unless
the extension explicitly declares an unauthenticated prefix in its
manifest (e.g. `/status.php` for protocol-compat extensions whose
clients probe it before authenticating, or public-page prefixes).

**The unauthenticated-prefix declaration is a separate manifest
field.** Extensions cannot accidentally drop auth — they have to
declare the prefix as unauthenticated, and that declaration is
visible in the manifest, in audit logs, and in the install
confirmation. RBAC rules can constrain which extensions are
permitted to declare unauthenticated prefixes at all.

**RBAC for capability-mediated work is unchanged.** Extensions
that do their work via `cap.call()` get the existing RBAC
enforcement at the capability layer. RBAC for direct-handler work
(handler runs without going through a capability) is the
extension's responsibility within the auth context the kernel
provides — but the kernel-issued audit log entry exists either way.

**Audit logging is a kernel concern.** The catch-all handler emits
an audit entry for every dispatched request: timestamp, PAT
identity (or unauthenticated marker), prefix matched, extension,
HTTP status returned. Extensions can emit additional entries via
the audit capability; the kernel-issued one is non-skippable.

---

## Generalizes to

This primitive enables, without further architectural work:

- Protocol-compatibility extensions in general (Files-Nextcloud is
  the immediate case; CalDAV, CardDAV, S3-compat object storage,
  ActivityPub federation, and so on are plausible later cases that
  would have re-litigated the same tension under reserved-prefix).
- Public extension pages at clean URLs (the deferred app-vs-site
  question becomes "extensions can claim URL prefixes for public
  pages, the trust model is per-prefix authentication declarations"
  — answerable rather than open).
- The share primitive `/s/{token}`, if it ends up wanting to be
  owned by a specific sharing extension rather than the kernel.
- Embedded/iframe surfaces for OAuth flows, payment widgets, and
  other surfaces that need real URLs at predictable paths.

The point is not to commit to any of these. The point is that the
primitive doesn't foreclose any of them, and each one becomes an
extension-design question rather than a kernel question.

---

## Risks and unknowns

- **Prefix-claim semantics.** Wildcard depth (`/dav/*` vs
  `/dav/files/*` — can two extensions claim non-overlapping
  sub-trees?), method scoping (can two extensions split GET vs
  POST on the same prefix?), and host scoping (does any of this
  vary per-host in a multi-tenant deployment?) all need decisions.
  Likely answer: simplest-rule first — full-prefix exclusive
  ownership, no method scoping, no host scoping — and revisit
  if a real use case justifies more.
- **Uninstall while requests in flight.** A request matched to an
  extension whose handler is being unloaded mid-flight needs a
  defined behavior. Probably: mark the extension's table entry as
  draining, let in-flight requests complete, reject new requests
  with 503 until the entry is removed.
- **Manifest schema additions.** New fields: `http_prefixes`,
  `unauthenticated_prefixes`, `handler_mode` (`quickjs` | `sidecar`
  | `bundled_native`), and per-prefix dispatch metadata. Need an
  ICD before this lands in code.
- **Permission model for HTTP-surface declarations.** Should every
  extension be allowed to claim arbitrary HTTP prefixes, or is that
  a privileged capability? Likely: privileged. The Files extension
  declaring `/dav/*` is appropriate; an arbitrary third-party
  extension claiming `/admin/*` is not. This is RBAC for the
  install operation, not for runtime.
- **Catch-all interaction with Drogon's existing routing.** Need to
  verify the catch-all does not interfere with the more-specific
  fixed routes' precedence, and that Drogon's path-matching cost
  doesn't degrade with a single catch-all in the table.
- **Performance under realistic load.** Not expected to be a
  problem at Plinth's scale, but the catch-all + lookup + auth
  path should be benchmarked once before declaring it adequate
  for high-throughput surfaces.

---

## Decided now

(Reserved-fields and don't-foreclose constraints only — no actual
architecture decisions are taken in this document.)

- The principle ("kernel owns primitives, extensions own
  application surfaces") is the design stance under which extension
  HTTP surface will be considered. Future architecture sessions
  start from it; if a session wants to depart from it, that's the
  thing being argued.
- No new application- or protocol-specific routes are added to the
  kernel pending resolution of this question. The Files arc does
  not motivate kernel-side `/dav/*` reservations.
- The kernel-owned primitive route set is the set listed above and
  no more. Additions require the principle's test ("describable
  without naming any application").

## Deferred

Everything substantive belongs in an architecture session:

- The shape itself — adoption of the catch-all + manifest-prefix +
  runtime-table model, or some variant of it.
- Manifest schema additions and the ICD describing them.
- Prefix-claim semantics (wildcard depth, method scoping, host
  scoping).
- Uninstall-while-in-flight behavior.
- Permission/privilege model for prefix declarations.
- Audit log entry shape for catch-all dispatched requests.
- Performance benchmark plan and acceptance threshold.
- Whether this primitive is implemented as a 0.5.x sub-milestone,
  a discrete arc, or interleaved with other 0.5.x work.

## Roadmap implications

- This primitive is a precondition for any extension that wants
  HTTP surface, including Files for Nextcloud-compat. It must land
  before the Files arc opens.
- An architecture session is required to ratify (or modify) the
  shape proposed here. That session also produces the manifest
  schema additions and the ICD.
- Implementation likely fits in 0.5.x — same general window as the
  rule-registry thread-safety fix, with which it shares
  thread-safety patterns and an architectural neighborhood.
- The Shell arc (0.6.x) does not depend on this and is not
  blocked by it. Shell uses bundled-with-kernel registration for
  its own panels and tray, which is separate machinery.
- The Files extension's design doc, when written, depends on this
  primitive having landed. The Files-protocol-compat discussion
  will be reframed against this primitive once the primitive is
  ratified.

---

## References

- Conversation, 2026-04-27 — origin of this discussion. Path:
  JS router → reserved-prefix list → catch-all + manifest-declared
  prefixes + runtime table. Each rejection clarified what the
  primitive needed to be.
- Conversation, 2026-04-16 — earlier site-platform / extension-
  routing discussion. Concluded that extensions did not own HTTP
  surface under the architecture as it then stood. This document
  is the proposal for how that concludes.
- ARCHITECTURE — kernel-owned route inventory, capability
  dispatch model, RBAC.
- `enforcement.cpp` rule-requirement registry — thread-safety
  pattern that this primitive's route table reuses.
