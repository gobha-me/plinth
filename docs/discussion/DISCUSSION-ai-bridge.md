# AI Bridge Extension — Design Space Summary

**Status:** Discussion capture — not a design doc, not authoritative
**Source:** Post-methodology-update session, 2026-04-17
**Participants:** the maintainer (Architect) + Claude (Architecture Session)
**Feeds into:** Future DESIGN-ai-bridge-v0Xx.md (likely ~0.12–0.13,
after notes ships and before any AI-dependent extension begins)
**References:**
- `architecture/02-capabilities.md §1` (Capability registry — the
  dispatch mechanism the bridge relies on)
- `architecture/02-capabilities.md §3` (Capability call flow)
- `architecture/03-data.md §3` (Realtime pub/sub — how streamed
  responses reach the frontend)
- `architecture/04-services-ha.md §5` (Sidecar contract — for
  self-hosted provider paths)
- `architecture/01-identity.md §2` (RBAC — gates who can call
  expensive capabilities and who can configure providers)
- `DISCUSSION-streaming-and-media.md` (the streaming substrate this
  extension uses)
- `DISCUSSION-cross-cutting-composition.md` (how ai-bridge composes
  with future augmenters like grammar, AI writing assist)

---

## 0. Scope Note

This discussion is about an **extension** — ai-bridge — that would
be built on top of Plinth, not about kernel work. Plinth-the-kernel
doesn't do LLMs, embeddings, TTS, image generation, or any AI work
itself; it provides capability dispatch, realtime pub/sub, storage,
RBAC, and sidecar coordination. The ai-bridge extension uses those
primitives to offer a normalized AI capability surface to other
extensions. This document exists so:

1. The kernel milestones currently in flight don't accidentally
   foreclose the ai-bridge pattern (none appear to — the
   don't-foreclose checks in §7 are all already satisfied by the
   current architecture).
2. When someone eventually writes ai-bridge, the design space has
   been explored and they don't start from zero.
3. Other extensions that depend on AI capabilities can be designed
   with ai-bridge's contract in mind, even before ai-bridge itself
   ships.

## 1. The Problem

A long list of planned extensions depend on AI capabilities:

- LLM chat (human ↔ model conversation)
- Multi-user chat with LLM participation
- LLM personas with memory management
- Notes with AI augmentation (summarize, rewrite, suggest)
- Knowledge base with semantic search
- SWORD bible study with LLM commentary
- LLM-based games
- TTS for any content panel (reading notes, narrating notifications)
- STT for input methods
- Image generation triggered from any text panel
- Embeddings for any content-indexing extension

Each of these needs one or more of: text completion (streaming),
embeddings (batched), image generation, TTS, STT, occasionally video
or music. Each of those has multiple possible providers: Venice,
OpenRouter, Anthropic direct, OpenAI, Gemini, local Ollama, local
Whisper, local Kokoro, local embedding servers.

The naïve implementation: every extension that wants an LLM wires up
its own HTTP client to its own configured provider. The chat
extension talks to Venice; the notes extension talks to OpenRouter;
the KB extension talks to a local embedding sidecar. Result:

- Credentials scattered across extension configs.
- No way to switch a provider without editing every extension.
- No cost tracking; you discover the bill at end-of-month.
- No rate-limit coordination; two extensions simultaneously hammer
  the same provider and hit the rate wall together.
- Every extension author writes the same retry/backoff/streaming
  glue. Three almost-correct versions. The duplicate-implementation
  failure mode the methodology exists to prevent.

The fix is a single extension — **ai-bridge** — that provides a
normalized set of AI capabilities and internally routes to whichever
provider is configured for each capability. Other extensions call
`ai:1:chat(messages, opts)` and stay provider-agnostic.

## 2. The Design Principle

**Capabilities are stable; providers are routed.** The capability
surface the ai-bridge registers is a product decision and changes
slowly. The provider backing each capability is a configuration
decision and can change per-capability, per-user, or per-call.

Every consumer of an AI capability calls `ai:1:chat` (or whichever
capability fits) without knowing whether the call hits Venice,
OpenRouter, a local Ollama sidecar, or a fallback chain across all
three. This is the capability-registry pattern already in Plinth,
applied at the "provider abstraction" layer.

## 3. Capability Surface — First Cut

This is a sketch of what `ai-bridge` registers. Final names and
signatures belong in the ai-bridge design doc. Included here so
later discussion can reference them.

| Capability | Shape | Streaming | Notes |
|------------|-------|-----------|-------|
| `ai:1:chat` | `(messages, opts) → text` | Yes (realtime bus) | Primary text completion |
| `ai:1:chat_structured` | `(messages, schema, opts) → json` | No | JSON-mode / tool-use where supported |
| `ai:1:embed` | `(text \| text[], opts) → vector[]` | No | Batched input/output |
| `ai:1:tts` | `(text, voice, opts) → audio_stream_id` | Yes (realtime bus) | Returns stream ID, chunks emitted |
| `ai:1:stt` | `(audio_ref, opts) → text` | Yes (realtime bus, partial transcripts) | audio_ref from storage |
| `ai:1:image_generate` | `(prompt, opts) → image_stream_id` | Yes if provider supports progressive | Stored to extension storage |
| `ai:1:image_edit` | `(image_ref, prompt, opts) → image_stream_id` | Yes if provider supports | — |
| `ai:1:image_upscale` | `(image_ref, opts) → image_stream_id` | Yes if provider supports | — |
| `ai:1:video_generate` | `(prompt, opts) → video_job_id` | No (polled) | Long-running; poll for status |
| `ai:1:music_generate` | `(prompt, opts) → audio_stream_id` | Depending on provider | — |
| `ai:1:sfx_generate` | `(description, opts) → audio_stream_id` | — | — |

**Notes on the shape:**

- Streaming capabilities return a stream ID (per
  `DISCUSSION-streaming-and-media.md §2.a`). The caller uses
  `plinth.stream()` on the frontend to iterate chunks.
- Media-producing capabilities (image, video, music) write results
  to storage (per `architecture/03-data.md §2.3`) and return refs,
  not inline bytes. This keeps the byte path out of the JS runtime
  and reuses the storage HTTP surface for delivery.
- `opts` is always an object for forward compatibility. Providers
  may support fields others don't; unsupported fields are ignored
  with a log entry, not an error.
- Version tier is `1`. If the capability surface needs a breaking
  change, `ai:2:*` lives alongside `ai:1:*` during a transition.

## 4. Provider Abstraction — How Routing Works

Each capability has a **provider** configured. A provider is a named
backend with its own driver module inside the ai-bridge extension.

### 4.a. Routing Tiers

The routing decision is made in the following order. First hit wins.

1. **Per-call override** (rare): the caller passed
   `opts.provider = "venice"`. Respected if the caller has the
   `ai.provider.override` rule.
2. **Per-user preference**: the calling user has a preferred provider
   for this capability, set in their preferences. Respected if the
   user has the `ai.provider.choose` rule.
3. **Admin-configured default**: the admin has set a default provider
   for this capability (`ai.defaults` table). This is the normal
   path.
4. **Built-in fallback**: ai-bridge ships with a compile-time default
   per capability so a fresh install works after basic credential
   setup.

### 4.b. Fallback Chains

A capability's provider is really a *chain*: `[venice, openrouter,
local_ollama]` for chat, for example. The chain is tried in order;
advancement happens on:

- Rate limit (HTTP 429).
- Upstream timeout.
- Upstream error (5xx).
- Capability refusal (content policy — configurable whether this
  advances or fails).

Per-call advancement is capped (e.g., one fallback per call), and
per-minute advancement across users is capped (circuit breaker per
provider). If the whole chain fails, the capability returns a
structured error that the caller can present to the user.

### 4.c. Driver Modules

Each provider has a driver: a QuickJS module inside ai-bridge that
exports one function per supported capability, normalizing the
provider's API to the ai-bridge capability signature.

```
providers/
  venice/
    chat.js        — Venice chat completion, SSE chunking
    embed.js       — Venice embeddings
    image.js       — Venice image gen/edit/scale
    tts.js         — Venice TTS
    sfx.js         — Venice sound effects
  openrouter/
    chat.js        — OpenRouter chat, SSE chunking
    embed.js       — OpenRouter embed
  anthropic/
    chat.js
  openai/
    chat.js
    embed.js
    image.js
  ollama/
    chat.js        — calls local Ollama sidecar's /api/generate
    embed.js       — calls local Ollama sidecar's /api/embed
  kokoro/
    tts.js         — calls local Kokoro TTS sidecar
  whisper/
    stt.js         — calls local Whisper sidecar
  embed-local/
    embed.js       — calls local embedding sidecar (sentence-transformers et al.)
```

Adding a new provider is: write the driver file, register it in the
provider index, add its capabilities to the routing config. No other
extension needs to change.

### 4.d. Self-Hosted Providers Are Just Providers

This is the key unification. A "sidecar-backed provider" is a driver
whose driver module calls a sidecar capability (Tier 3 dispatch)
instead of an external HTTP endpoint. From the rest of the system's
point of view, there's no distinction. An admin who wants
self-hosted embeddings swaps the `embed` provider from `venice` to
`embed-local`; the sidecar has to exist (that's the operational
precondition), but the capability surface doesn't change.

Sidecars become a **deployment choice**, not an architectural
choice. `DISCUSSION-streaming-and-media.md §2.c` framed sidecars as
"heavy compute belongs here" — true, but the ai-bridge pattern shows
that the *choice* of heavy vs. remote is made by the admin at
configuration time, not by the extension author at design time.

## 5. Credential and Secret Handling

Providers need credentials. This is the piece ai-bridge cannot
hand-wave:

- Credentials live in a dedicated `ext_ai_bridge.credentials` table.
- The credentials table is **write-only from the admin UI**. Values
  are never rendered back — only "configured" / "not configured"
  indicators. Editing replaces, never displays.
- Credentials are **at-rest encrypted** using a key derived from a
  kernel-level secret (configured at deployment time, not stored in
  PG).
- Extension code reads credentials through a narrow
  `credentials.fetch(provider, field)` interface, gated by a rule
  only ai-bridge itself holds (`ai_bridge.credentials.read`).
- Audit log every credential read, with the capability call that
  caused it.

**This is a Plinth-level concern wider than ai-bridge.** Any
extension wrapping a paid API needs credential management. The
pattern ai-bridge establishes should generalize — possibly into a
kernel-provided credentials vault in a later milestone. For now,
ai-bridge implements it internally; the design doc calls out the
generalization as a future extraction.

## 6. Cost Tracking and Quotas

Cloud AI is metered. Users and admins want visibility into spend.
Ai-bridge records every call:

- `ext_ai_bridge.calls` table: capability, provider, user, timestamp,
  input_size, output_size, declared_cost (if the provider returns
  one in headers), estimated_cost (ai-bridge's model of per-token
  pricing), success/fail, latency.
- Per-user quota: `ai.quota.user` rule gates calls when
  monthly-spend estimate exceeds the user's limit. Admins configure
  limits; "unlimited" is a value.
- Per-provider rate limits: ai-bridge applies local rate limiting
  *before* the upstream rate limit hits, to avoid chain advancement
  due to avoidable 429s.

Admin dashboard surfaces: calls/day, estimated spend/provider,
quota utilization per user, failure rates per provider-capability
pair. This is normal BI UI work.

## 7. What Is Decided Now

Three small moves before ai-bridge is picked up for implementation,
none of which block current milestones:

1. **The `ai:*` namespace is reserved.** No other extension should
   register capabilities in the `ai:*` namespace. If someone wants
   to build a "gemini-direct" extension, it registers under
   `gemini:*` and is either consumed directly by its callers or
   wrapped by an ai-bridge driver. This prevents namespace collision
   and makes the ai-bridge contract discoverable.

2. **Streaming AI capabilities use the stream-ID pattern from
   `DISCUSSION-streaming-and-media.md §2.a`.** `plinth.stream()` on
   the frontend is the canonical way to consume streaming AI
   responses. Client SDK (0.6.3) is where this wrapper lives.

3. **Credential-management design is scoped to ai-bridge initially
   but generalizable.** The ai-bridge design doc flags the credential
   vault as a candidate for extraction into a kernel primitive after
   the second extension (whichever it is — probably an email/SMS
   provider wrapper) needs the same shape.

## 8. What Is Deferred

To DESIGN-ai-bridge-v0Xx.md (Scale 2, targets ~0.12–0.13):

- Final capability signatures and option objects.
- Provider chain configuration UI (admin panel).
- Per-user provider-preference UI.
- Exact cost-model shape and how it stays updated (providers change
  pricing; is there a pull mechanism, or manual updates?).
- Streaming-specific error semantics (what happens when the stream
  fails mid-emission after fallback has been consumed).
- Tool-use / function-calling normalization across providers whose
  tool-calling syntaxes differ substantially.
- Multimodal inputs (image-in-chat, audio-in-chat) normalization.
- Whether to expose a lower-level "bring your own prompt template"
  path or only high-level capabilities.
- Caching (should identical embedding requests for the same text
  hit a cache? If so, where does the cache live?).
- Interaction with composition framework — an "AI writing assist"
  augmenter calls ai-bridge capabilities; the augmenter is defined
  in the composition arc, the capabilities are defined here.

## 9. What Is Rejected

- **Letting each extension wrap its own provider directly.** This
  is the problem ai-bridge solves. An extension that wants Venice-
  specific behavior ai-bridge doesn't expose adds a driver to
  ai-bridge, or requests a new capability. It does not implement
  its own Venice client.
- **Provider-specific capabilities exposed in ai-bridge.** No
  `ai:1:venice_specific_feature`. If a feature is provider-specific
  and worth exposing, it goes under a provider-named namespace
  (`venice:1:foo`), and the caller acknowledges they're
  provider-locked. ai-bridge stays provider-agnostic.
- **Compile-time provider selection.** All routing is runtime
  configuration. Admins and users can change providers without
  restarting Plinth or redeploying ai-bridge.

## 10. Things Noticed While Drafting

Three observations that surfaced writing this doc:

### 10.a. The Sidecar Framing Needed Updating

`DISCUSSION-streaming-and-media.md §2.c` originally framed sidecars
as "heavy compute belongs here." That's true but incomplete.
Sidecars are **one provider implementation** in the ai-bridge
pattern — a deployment choice equivalent to "use Venice" or
"use OpenRouter." The architectural distinction is
provider-vs-caller, not sidecar-vs-API. The streaming doc has been
updated to reflect this.

### 10.b. System-Services Composition Is Cleaner Than Sketched

`DISCUSSION-cross-cutting-composition.md §3b` originally described
system-level services (STT, TTS) as a deferred-further composition
mode. Drafting ai-bridge made the shape clearer: TTS and STT are
**ordinary ai-bridge capabilities** (`ai:1:tts`, `ai:1:stt`). The
system-level composition concern is the *routing of content between
panels and these capabilities* — "read this selection aloud,"
"transcribe this audio clip I just recorded."

That routing is a shell / panel-SDK concern, not an ai-bridge
concern. The shell provides a "send to TTS" action; ai-bridge
provides the TTS capability; the user's configured provider
(Kokoro sidecar, Venice API, whatever) provides the actual audio.
Clean three-layer separation. The composition doc has been updated
to reflect this narrower scope for Mode 3.

### 10.c. Ai-Bridge Unblocks the Earliest AI Demos

The first extension that wants any AI capability needs ai-bridge to
exist first — otherwise it invents provider handling and sets a bad
precedent that other extensions copy. This suggests ai-bridge should
ship *before* any AI-consuming extension, not alongside them.

Rough sequencing:

- **0.11** — notes extension ships (no AI yet; just the core
  data model and editor).
- **0.11.x–0.12** — ai-bridge ships. First version is
  single-provider-per-capability, static config, Venice as the
  default for anything Venice supports. Chain/fallback/cost-tracking
  come in later sub-versions.
- **0.12.x+** — chat, personas, KB, SWORD, etc. all build against
  ai-bridge from day one.

The alternative — "let each extension handle its own AI calls until
we have enough experience to design ai-bridge" — is exactly the
Armature failure mode. Don't do that.

## 11. What This Document Is Not

Conversation capture, not a design doc. The ai-bridge design doc
produces the real design when the milestone arrives. Do not cite
claims in this document as constraints on implementation — except
the three decided-now items in §7, which are reservations that
early work must respect.
