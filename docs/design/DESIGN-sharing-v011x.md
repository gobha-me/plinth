# DESIGN: Public Sharing (0.11.x) — Outline

**Status:** Outline only — **not a commitment to build.** Produced to lock down the `shareable[]` manifest slot before 0.4 freezes, and to preserve the shape of the share primitive so that a future architect picking this up starts from a sketch rather than from zero.
**Scale:** Would be 2 (multi-version arc) if elevated to a full design doc.
**Traces to:**
- architecture/01-identity.md §3 (Anonymous Identity), architecture/05-extensions.md §2 (Reserved URL Prefixes), architecture/05-extensions.md §5.1 (Share Primitive)

- DESIGN-packages-v04x.md §7.2 (shareable[] deferred slot), Appendix A
- DESIGN-rbac-philosophy.md (everyone group, least privilege)
**Depends on (if elevated):**
- DESIGN-capability-registry.md (0.2.x capability dispatch)
- DESIGN-quickjs-bridge.md (new handler entry point)
- DESIGN-packages-v04x.md (shareable[] parsing, `render_share` file resolution)
- 0.10.0–0.10.1 storage (Files is the first consumer)

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)
**Outline date:** 2026-04-16

---

## 0. Status and Scope Boundary

This document is an **outline**, not a design. Its only binding claims are the `shareable[]` manifest schema in §3 and the `/s/*` URL reservation (which already exists in `architecture/05-extensions.md §2`). Everything else is a sketch that a future architecture session may revise freely.

The distinction matters:

- **Binding (must be preserved):** `shareable[]` manifest field shape, `/s/*` URL prefix, `UserContext::anonymous()` as the caller identity for public share rendering. These are load-bearing because 0.4 depends on them or reserves them.
- **Sketched (may evolve):** table schemas, handler signatures, request flow, HTML composition, rate limits, caching strategy. These are written to show the shape is plausible; they are not contracts.

If and when the public sharing arc is picked up, an architecture session produces the full `DESIGN-sharing-v011x.md` with Scale-2 rigor. This outline is input to that session, not output of it.

---

## 1. Decision (Conditional)

**If** public sharing is built, it takes the form of a kernel-mediated primitive at the reserved `/s/{token}` URL prefix, supporting multiple extensions (Files, Notes, LLM Chat, and any future content-producing extension) through a constrained handler contract.

The primitive is **not** a generic public HTTP surface. It is specifically for exposing individual user-owned resources at stable opaque-token URLs. Human-readable public URLs (slugs, forum threads, package registry) are a different concern and are sketched in `architecture/05-extensions.md §5.2` (site-host extension), not here.

**If** public sharing is never built, the `shareable[]` manifest slot stays empty forever. This is the explicit accepted outcome.

---

## 2. Why This Is Deferred and Uncommitted

Recorded so a future architect understands the decision trail:

- The 0.1.5-follow-on architecture session (session notes: `SESSION-site-host-decision.md` equivalent) concluded that Plinth is an app platform, not a site+app platform. Public HTTP surface is not a product requirement.
- Public sharing of individual resources (share a note, share a chat transcript) was identified as a pervasive pattern that almost every content extension will eventually want. Unlike a site-host extension, it is naturally kernel-mediated and does not fragment the platform.
- The `shareable[]` slot is reserved in 0.4 specifically so that if sharing is built, no manifest schema migration is needed. The cost of reserving a slot is zero. The cost of *not* reserving it and adding it later is a breaking manifest change, which §7.2 of the 0.4 design doc rules out.
- Whether sharing is ever built depends on whether the three named use cases (Files, Notes, LLM Chat) become real enough to justify the arc. None is committed.

---

## 3. The `shareable[]` Manifest Field (Binding Contract)

This is the single committed contract from this document. The 0.4.1 parser validates it structurally; this document defines its eventual semantics.

### 3.1 Schema

```json
{
  "shareable": [
    {
      "resource_type": "note",
      "handler": "server/handlers/share_note.js",
      "description": "Public share render for notes"
    },
    {
      "resource_type": "file",
      "handler": "server/handlers/share_file.js",
      "description": "Public share render for files"
    }
  ]
}
```

### 3.2 Field Semantics

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `resource_type` | string | Yes | Extension-local identifier for the shareable resource type. Matches `^[a-z][a-z0-9_-]{1,63}$`. Used as the `resource_type` column in `plinth.shares`. An extension may declare multiple shareable types (e.g., a Files extension might expose `file` and `folder`). |
| `handler` | string | Yes | Path relative to the package root, must resolve to a file under `server/handlers/`. The file must exist (0.4.2 cross-file validation). |
| `description` | string | Yes | Human-readable. Shown in admin UI when managing shareable resource types. |

### 3.3 0.4-Era Behavior (Before This Arc Is Picked Up)

Per DESIGN-packages-v04x.md §7.2 and Appendix A, the 0.4.1 manifest parser:

- Accepts `shareable` as an optional field.
- Validates it is an array (type check).
- **In 0.4.x only:** validates the array is empty. Non-empty values produce an install-time warning `"shareable is reserved for a future version"` and the field is otherwise ignored.
- Does NOT validate individual entry shapes.
- Does NOT verify handler files exist (only the array-emptiness check).

When (if) this arc is elevated, the 0.4.1 parser gains full per-entry validation. The addition is strictly additive — any package that had an empty `shareable[]` still validates; any package that had a warning-producing non-empty `shareable[]` now gets proper validation instead of a warning.

### 3.4 Why These Three Fields And Not Others

Fields deliberately *not* included in the schema, with reasoning so a future architect doesn't accidentally add them:

- **No `capability` field.** Share rendering is not a capability call in the §3.3 sense — it has a different caller identity (anonymous), a different dispatch path (HTTP, not `cap.call`), and a different RBAC model (rate-limited token lookup, not rule checking). Coupling the two would force compromises on both.
- **No `max_age` or cache fields.** Caching is per-share (at creation time), not per-resource-type. The manifest describes *which types are shareable*; the share itself carries its cache policy.
- **No `og_defaults` field.** OG metadata comes from the `render_share` handler's return value, per-resource. Manifest-level defaults are not worth the ceremony.
- **No `requires_auth` flag.** If a resource type needs authenticated access, it's not using the share primitive — it's a normal RBAC-gated capability. The share primitive is exclusively for public sharing.

---

## 4. Sketch: Kernel Surface Area

Everything in §4 onward is a sketch. It may be wrong in detail; it exists to show the overall shape is plausible.

### 4.1 `plinth.shares` Table (Sketch)

Columns an implementation would likely need:

- `token` — the public-facing identifier. Opaque, URL-safe. Sketch: 128-bit random, base62-encoded (~22 chars). Primary key.
- `owning_extension` — which extension's `render_share` handler gets invoked. FK to `plinth.packages(name)` conceptually; actual FK semantics depend on lifecycle (see §5).
- `resource_type` — one of the `resource_type` values from the owning extension's `shareable[]`.
- `resource_id` — extension-interpreted identifier (e.g., a note UUID, a file path). Opaque to the kernel.
- `created_by` — `plinth.users(id)`, the user who created the share.
- `created_at` — timestamp.
- `expires_at` — optional, nullable.
- `revoked_at` — nullable.
- `access_count` — for rate limiting and analytics.
- `options_json` — JSONB, extension-interpreted share options (include-tool-results on a chat share, download-vs-preview on a file share, etc.).

**What's unresolved:**
- Whether `resource_id` gets a length limit or is arbitrary TEXT.
- Whether to add a `revoked_by` column for audit (probably yes).
- Whether token generation uses a kernel crypto primitive or a shared utility.

### 4.2 `/s/{token}` Request Flow (Sketch)

Rough dispatch sequence. Ordering matters for security; a real design session locks this down:

1. Parse token from URL. If malformed (wrong length, invalid charset), 404 without touching the database.
2. Rate-limit check for caller IP × token (not just IP — a single popular share shouldn't DoS rate limits for other shares from the same IP). Exceeded → 429.
3. `SELECT` from `plinth.shares` by token. Miss / `revoked_at IS NOT NULL` / `expires_at < NOW()` → 404 (not distinguishable to the caller — don't leak revocation state).
4. Look up owning extension. If the package is uninstalled, 404. If disabled, the design session must decide: 404 (share is treated as effectively revoked) or 503 (share is temporarily unavailable). Both are defensible; 404 is simpler.
5. Dispatch to `{owning_extension}.render_share(token, resource_type, resource_id, options_json)` via the QuickJS bridge, with `UserContext::anonymous()`. Extension returns structured data (see §5).
6. Kernel composes HTML response. Headers set per share options and kernel defaults.
7. Increment `access_count`. Audit-log the access (extension, resource_type, success/failure).

**Caller identity:** `UserContext::anonymous()` per `architecture/01-identity.md §3`. The share handler runs as an anonymous user with `everyone`-only group membership. Any `cap.call()` the handler makes goes through `RbacFilter` and is denied unless `everyone` has been granted a rule — which, by the RBAC philosophy, it never is by default.

**This is intentional.** The share handler must read its own data directly (via `db.query` on its own schema, via `storage.get` on its own prefix). It must not call cross-extension capabilities, because those would leak authenticated data into public responses. A design session may add a narrow exception mechanism if needed, but the default is "no outbound capability calls from a share handler."

### 4.3 HTML Composition (Sketch)

The kernel owns HTML generation. The extension returns structured data. Concretely:

```
Kernel template:
  <!DOCTYPE html>
  <html>
    <head>
      <meta charset="utf-8">
      <title>{og.title}</title>
      <meta property="og:title" content="{og.title}">
      <meta property="og:description" content="{og.description}">
      <meta property="og:type" content="{og.type}">
      <meta property="og:image" content="{og.image}">  <!-- if present -->
      <meta name="robots" content="{robots_directive}">  <!-- per share options -->
      <link rel="stylesheet" href="/s/_style.css">
    </head>
    <body>
      <main id="share-content">
        {body}  <!-- HTML-escaped by kernel unless handler opts into safe_html -->
      </main>
      <script type="application/json" id="share-hydration">
        {hydration_data}  <!-- JSON-serialized -->
      </script>
      <script src="/s/_hydrate.js"></script>
    </body>
  </html>
```

**XSS surface:** All `{...}` substitutions are HTML-escaped by the kernel. The `body` field is escaped by default. An extension that needs to emit actual HTML (e.g., Notes rendering markdown → HTML) returns `{body: ..., body_is_safe_html: true}` — and the design session decides whether the kernel runs a sanitizer pass or trusts the extension.

**Shared hydration script (`/s/_hydrate.js`):** A kernel-owned small script that reads the `<script id="share-hydration">` JSON and invokes extension client code to render interactive features. Whether it's a separate endpoint or inlined per-share is a design-session decision. If it exists, it's kernel-owned code, not per-extension — extensions hand off hydration *data*, not hydration *logic*.

### 4.4 Response Headers (Sketch)

Defaults the kernel would set:

- `Content-Type: text/html; charset=utf-8`
- `Cache-Control` — derived from share options, clamped. Default for most shares: `public, max-age=300, s-maxage=3600` (5min browser, 1hr CDN). Clamped to `private, no-store` if the share is marked sensitive.
- `ETag` — derived from share token + resource state. Precise derivation is a design-session decision.
- `Content-Security-Policy` — strict, kernel-defined. No inline scripts (hydration script is external). No external resources without explicit allowlisting.
- `X-Robots-Tag` — `noindex` by default. Shares marked public-and-indexable opt in to `index, follow`.
- `Referrer-Policy: strict-origin-when-cross-origin`
- `X-Content-Type-Options: nosniff`

---

## 5. Sketch: Extension Handler Contract

### 5.1 `render_share` Signature (Sketch)

Invoked by the kernel for `GET /s/{token}` after token lookup succeeds:

```javascript
// Called by the kernel. Caller context: UserContext::anonymous().
async function render_share(token, resource_type, resource_id, options) {
  // 1. Look up the resource via db.query on the extension's own schema.
  // 2. Produce structured output.
  return {
    og: {
      title: "My shared note",
      description: "A note about architecture decisions",
      type: "article",              // or "website", "image", etc.
      image: null                   // optional URL
    },
    body: "<p>The note content rendered as HTML</p>",
    body_is_safe_html: true,        // handler asserts it sanitized the output
    hydration_data: {
      note_id: resource_id,
      can_edit: false               // anonymous viewer can't edit
    },
    cache_max_age: 300,             // seconds; kernel may clamp lower
    robots_indexable: false         // default: noindex
  };
}
```

**Constraints:**

- The handler runs in the standard QuickJS runtime for the extension, with the same limits (memory, CPU time) defined in the manifest's `runtime` section. Design session decides if share rendering gets a stricter budget — it probably should, since anonymous users can trigger it.
- The handler MUST NOT call cross-extension capabilities (§4.2 above). The kernel enforces this by having anonymous `UserContext` fail every capability RBAC check by default.
- The handler MUST NOT emit HTML via string concatenation without asserting `body_is_safe_html: true`. If it does, the kernel escapes the output, and the resulting page will display literal `<p>` tags instead of rendered content — which is the safe failure mode.
- The handler MAY return `null` or throw to indicate "this resource is no longer shareable" — the kernel responds 404 to the caller (not 500, to avoid distinguishing "gone" from "never existed").

### 5.2 How the Handler File Is Resolved

From the `shareable[]` entry:

```json
{ "resource_type": "note", "handler": "server/handlers/share_note.js" }
```

At package install (0.4.4 REGISTERING stage, if and when this arc is picked up):

- The path is resolved relative to the package root.
- Must exist under `server/handlers/` (same directory convention as capability handlers).
- File is loaded into the extension's QuickJS runtime on first invocation, cached thereafter (standard runtime-pool behavior from DESIGN-quickjs-bridge.md).
- Multiple `shareable[]` entries may reference different files, same directory, same extension's runtime.

The file's expected exports are a design-session decision — top-level function, named export, default export are all plausible.

---

## 6. Sketch: Share Creation and Revocation

This is the authenticated side of the primitive — how a share comes into existence.

### 6.1 Creation (Sketch)

Most likely: a new kernel capability invoked by extensions.

```javascript
// Inside an authenticated extension handler (e.g., the Notes extension's
// "share this note" capability):
const share = await cap.call("kernel:1:share.create", {
  resource_type: "note",
  resource_id: noteId,
  options: { include_comments: false },
  expires_at: null
});
// Returns: { token, url: "/s/abc123...", created_at }
```

The share creator must have an authenticated session. The extension calling `share.create` must be the same extension that will render the share (inferred from caller context — an extension can't create shares on another extension's behalf).

**Rule required:** `kernel.share.create` — RBAC-gated so admins can disable sharing globally or per-group if needed. Default: granted to `everyone` on first install, matching typical expectations. Admins who want to lock down sharing revoke it from `everyone` and grant to specific groups.

### 6.2 Revocation (Sketch)

The creator (or an admin) revokes via `kernel:1:share.revoke(token)`. Sets `revoked_at`. The `/s/{token}` request flow (§4.2) returns 404 thereafter.

Deletion vs. soft-revoke: soft-revoke (keep the row, set `revoked_at`) is preferable for audit. A scheduled task GCs rows older than a retention window.

### 6.3 Listing and Management

Share creators can list their own shares. Admins can list all shares. This is admin-UI territory and has no architectural weight — a design session lays out the API when the arc is picked up.

---

## 7. Sketch: Rate Limiting and Abuse Surface

The `/s/*` prefix is the most-abused surface in the system by construction — it's publicly reachable, it involves a database lookup, and it invokes extension code. Design session must cover:

- **Per-IP rate limits** on `/s/*` as a whole (independent of token).
- **Per-token rate limits** to prevent a single popular share from exhausting limits.
- **Per-IP × token rate limits** to prevent scrapers enumerating tokens.
- **Token enumeration defense** — tokens are 128-bit random, so brute-forcing is computationally infeasible, but log and alert on elevated 404 rates from a single IP.
- **Rendering cost budget** — QuickJS execution for share rendering should have a tighter timeout than normal capability calls. Default sketch: 500ms wall-clock.
- **Response size cap** — shares should not be unbounded. Default sketch: 10MB. Larger responses (e.g., a shared 100MB file) should use a redirect or streaming path, not direct render.

None of these values are committed. All are design-session decisions.

---

## 8. What This Outline Does Not Cover

Explicit about scope to prevent drift when the full design is written:

- **Human-readable public URLs (slugs).** Not here. See `architecture/05-extensions.md §5.2` (site-host extension).
- **Public writes.** No anonymous POST, no anonymous capability calls. Shares are read-only by design. Public writes are a separate, much harder problem (captcha, abuse moderation) and are not part of this arc.
- **Authenticated sharing.** "Share this with user Y" is a different pattern — it's RBAC grants, not public URLs. That belongs to the Files extension (and other content extensions) and uses the existing RBAC model, not this primitive.
- **Embeddable widgets.** "Embed this share in my blog" via `<iframe src=/s/...>` — the CSP story makes this work accidentally if the share allows it, but official embed support with `oEmbed` endpoints or postMessage APIs is out of scope.
- **Federation.** Sharing between different Plinth instances. Deferred indefinitely; if relevant, it's a separate arc.

---

## 9. Possible Per-Version Breakdown (Speculative)

If and when this arc is elevated to a full design doc, the sub-versions might look like this. This is illustrative — the full design session decides.

- **0.11.0 — Share table and token generation.** `plinth.shares` table, token generator, `kernel:1:share.create` / `.revoke` / `.list` capabilities. No `/s/*` route yet — share creation works, rendering does not.
- **0.11.1 — `render_share` handler dispatch.** QuickJS bridge extension to invoke `render_share` handlers. Manifest parser upgrade (0.4.1 additive) to fully validate `shareable[]`.
- **0.11.2 — `/s/{token}` HTTP flow.** Route registration, kernel HTML template, response header defaults, integration with `UserContext::anonymous()`.
- **0.11.3 — Rate limiting and abuse surface.** Per-IP, per-token, and per-IP×token limits. 429 responses. Audit logging.
- **0.11.4 — Management surface.** Share-list UI (in the shell), admin override for revocation, retention policy.
- **0.11.5 — First consumer: Files.** Files extension adds `shareable[]` entry, implements `render_share` for file and folder types. This is the end-to-end test case.

Arc dependencies: 0.2.x, 0.3.x, 0.4.x all operational. 0.10.0–0.10.1 (storage) operational before 0.11.5.

---

## 10. Open Questions (for the Full Design Session When It Runs)

Recorded so the eventual architect doesn't have to re-derive them:

1. Does the share handler get a stricter runtime budget than normal capability handlers? If so, what's the default and is it per-extension configurable?
2. What happens when an extension is disabled — share returns 404 or 503? (§4.2)
3. Does the kernel run an HTML sanitizer on `body_is_safe_html: true` content, or trust the extension? Design trade-off: defense in depth vs. simplicity.
4. Is there a shared hydration script (`/s/_hydrate.js`) or is hydration inlined per share? Trade-off: caching vs. complexity.
5. Does `share.create` require an explicit RBAC grant beyond being authenticated, or is it default-on-for-everyone? The 6.1 sketch defaults to on; admins can revoke. The alternative is default-off, admins grant. Either is defensible.
6. Token format: 128-bit random / base62 (~22 chars) vs. longer-but-readable vs. something else. No strong reason to pre-commit.
7. Is there a maximum number of active shares per user? Per extension? Per resource?
8. Cross-instance federation — ruled out or deferred? (§8 ruled out; design session may revisit.)

---

## 11. Trigger Conditions for Elevation

This outline becomes a full `DESIGN-sharing-v011x.md` when one of the following happens:

- A concrete use case commits to needing this. E.g., "we're building a Notes extension that markets public note sharing as a feature."
- A potential design decision in another arc (0.5 realtime, 0.10 storage) is blocked by uncertainty about whether shares will exist.
- The project decides to bootstrap an ecosystem and needs shareable-by-default to be part of the 1.0 story.

Until one of those happens, this outline is the full extent of the design thinking. 0.4 freezes the manifest slot; everything else remains sketched.

---

## 12. Relationship to 0.4 (What Must Freeze Now)

Concretely, what 0.4 commits to on behalf of this outline:

- The `shareable[]` manifest field exists, is optional, is an array. **Frozen.**
- Per-entry shape (`resource_type`, `handler`, `description`). **Frozen** — but not validated per-entry in 0.4.x, only structurally.
- `shareable[]` entries producing a warning when non-empty in 0.4.x. **Frozen.**
- When this arc is elevated, 0.4.1's parser gains strict per-entry validation. **Additive, not breaking.**

0.4 commits to nothing else from this outline. In particular, 0.4 makes no commitment to:
- Ever building `/s/*` serving
- Ever implementing `plinth.shares`
- Ever adding `share.create` / `.revoke` capabilities
- Any specific extension ever implementing `render_share`

The slot is reserved. The feature is not.

---

**This document is an outline.** Any architecture session that picks up the sharing arc replaces this document with a full `DESIGN-sharing-v011x.md`. Until then, the `shareable[]` manifest contract in §3 is the only part of this document that any other design doc or code session may rely on.