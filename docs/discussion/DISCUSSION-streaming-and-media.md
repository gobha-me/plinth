# Streaming, Long-Running Capabilities, and Media — Design Space Summary

**Status:** Discussion capture — not a design doc, not authoritative
**Source:** Post-methodology-update session, 2026-04-17; updated
2026-04-17 after ai-bridge drafting reframed §2.c
**Participants:** the maintainer (Architect) + Claude (Architecture Session)
**Feeds into:** Client SDK spec (0.6.3), sidecar contract (0.8.x),
future DESIGN-streaming-v0Xx.md if one is needed
**References:**
- `architecture/02-capabilities.md §3` (Capability call flow)
- `architecture/03-data.md §2.3` (Storage HTTP surface)
- `architecture/03-data.md §3` (Realtime pub/sub)
- `architecture/04-services-ha.md §5` (Sidecar contract)
- `DESIGN-shell-v06x.md §4` (Panel SDK — the stream wrapper lives here)
- `DISCUSSION-ai-bridge.md` §4 (provider abstraction — sidecars are
  one routing destination among several)
- `DISCUSSION-cross-cutting-composition.md` §3b (System-level
  services — live audio/video belong in that arc, not this one)

---

## 0. Scope Note

This discussion is about how extensions can stream data to users
without each one inventing its own transport, not about kernel work
for the milestones currently in flight. Plinth-the-kernel's job
here is narrow: ship the realtime bus (0.5.x), expose the storage
HTTP surface with `Range` support (0.10.x), keep `plinth.call()`'s
return value opaque at the bridge (0.3.x), and allow streaming
response shapes in the sidecar contract (0.8.x). The streaming
wrapper lives in the client SDK (0.6.3); the streaming use cases
all live in extensions. This document exists so those kernel
milestones don't close off streaming patterns the applications on
Plinth will want.

## 1. The Problem

The `plinth.call(cap, args) → result` capability model is one-shot.
When an extension's handler returns, the caller gets the whole result.
This is correct for most capabilities (creating a note, fetching a
row, computing a value) and wrong for three classes of operation:

- **Streamed remote responses.** LLM API calls from providers like
  Venice, OpenRouter, Anthropic — upstream responses arrive as SSE
  chunks. The extension wants to forward chunks to the frontend as
  they arrive, not wait for completion.
- **Long-running jobs with progress.** Bulk imports, big exports,
  anything where the user benefits from progress events during
  execution.
- **Byte streams.** File download with `Range` support, stored audio
  playback, resumable uploads. These are bytes, not JSON, and
  architecturally should not traverse the JS runtime.

Nothing in the current architecture blocks streaming. But without
a deliberate decision about which mechanism is the primary one for
each class, extension authors will each invent their own conventions
and the frontend will end up with three incompatible streaming APIs.

## 2. The Four Mechanisms — and Which Class Each Serves

### 2.a. Realtime Bus (Out-of-Band)

How it works: the handler returns immediately with a stream ID. The
frontend subscribes to `ext_X:stream.chunk.{stream_id}` on the
existing realtime bus. The handler pushes chunks via `events.emit()`
in a loop. The frontend receives them through the pub/sub path.

**This is the primary mechanism for streamed remote responses and
long-running jobs with progress.** Concretely: wrapping Venice,
OpenRouter, Ollama-over-HTTP, upload progress, long computations —
all of these are one extension handler containing a loop that pulls
chunks from upstream (or produces chunks as work progresses) and
emits them.

Properties:
- Works today, in the architecture as written, with no new primitives.
- The realtime bus (0.5.x) already has the fan-out, ordering, and
  coalescing needed.
- The handler is an ordinary QuickJS function. No special API.
- Back-pressure, reconnect, and delta-sync are all inherited from
  the realtime infrastructure.

What needs to land in the client SDK (0.6.3):

```js
for await (const chunk of plinth.stream(cap, args)) {
  // render incrementally
}
```

`plinth.stream()` is a thin wrapper: call the capability, get back a
`{stream_id, final_event}` descriptor, subscribe to the chunk channel,
yield chunks until the final event arrives, unsubscribe. Twenty lines
of SDK code. If option 2.b ever lands, the same API works unchanged.

### 2.b. Async Generators at the Bridge (Optional)

How it would work: the QuickJS→C++ bridge recognizes `AsyncIterator`
return types and pipes them through the WS frame protocol as chunked
responses keyed to the original call ID.

Properties:
- Nicer author ergonomics (`async function* handler()`).
- Same capability as 2.a — only the transport changes.
- Requires new WS frame type and bridge work.
- Not required for any streaming use case; 2.a covers everything.

**Optional polish.** Skip unless authors start asking for it. If you
do adopt it, the client-facing `plinth.stream()` API from 2.a does
not change — the change is internal to the transport.

### 2.c. Sidecar Streaming for Heavy Compute

Sidecars are one routing destination for AI and compute-heavy
capabilities, not a distinct streaming mechanism. The decision to
use a sidecar — versus a cloud API provider — is a **deployment
configuration** made by the admin through an extension's provider
config, not an architectural decision made by the extension author.

See `DISCUSSION-ai-bridge.md` for the routing pattern: an extension
registers a normalized capability (e.g., `ai:1:embed`), and the
admin configures which provider backs it (Venice, OpenRouter, a
local Ollama sidecar, a local embedding sidecar, etc.). The caller
of the capability doesn't know or care. The sidecar is just another
provider from the caller's perspective.

Sidecars make sense as the chosen provider when:
- A model loaded in RAM should stay loaded (self-hosted Ollama, an
  embedding server, a transcription service with a model in memory).
- GPU pinning matters.
- The work batches usefully across concurrent requests (embeddings).
- The work is CPU-heavy enough that doing it in-kernel would starve
  other capabilities.
- Deployment policy requires offline / no-external-API operation.

They don't make sense as the chosen provider for lightweight
transport work. Wrapping a remote streaming API (Venice, OpenRouter,
Anthropic) is HTTP client work that QuickJS handles fine — no
sidecar needed even though the upstream happens to stream tokens.

**Don't-foreclose constraint on 0.8.x.** The sidecar HTTP contract
must allow streaming responses (chunked transfer / SSE), not just
request/response JSON. A sidecar-backed AI provider needs to stream
tokens back the same way a cloud provider does. The client sees a
capability call that emits chunks on the realtime bus regardless of
whether the chunks originated in a sidecar or a remote API —
same 2.a wrapper on the frontend.

What the sidecar arc decides:
- Exact streaming semantics over the sidecar HTTP contract.
- Chunk framing, end-of-stream signaling, error mid-stream.
- How sidecar-emitted chunks reach the realtime bus (direct emit
  from sidecar-side SDK, or kernel proxy that bridges
  sidecar-response-chunks to bus-events).

### 2.d. Byte Streams via Storage HTTP Surface

The storage HTTP surface in `architecture/03-data.md §2.3` already
solves this for stored byte streams:

- `GET /api/storage/{ext}/{path}` with `Range` header → streamed
  download, bytes go through Drogon directly, not through QuickJS.
- `POST /api/storage/{ext}/{path}` for resumable upload.

Stored media playback (audio, video) reads from the storage surface
with `Range`. File download is the same call. Upload progress uses
2.a (the handler emits progress chunks on the realtime bus while the
upload proceeds).

**Live media is a different problem.** Microphone input for STT,
realtime TTS output, WebRTC — these bypass capability dispatch
entirely. They live in the system-services arc documented in
`DISCUSSION-cross-cutting-composition.md §3b` (STT/TTS/translation
as Mode 3). Not this doc's scope. The kernel holds signaling; a
sidecar or the client library handles the media pipeline itself.

## 3. The Split, Restated

| Use case | Mechanism | Status |
|----------|-----------|--------|
| Remote LLM API (Venice, OpenRouter, Anthropic) | 2.a — realtime bus wrapper | Works today |
| Upload / export / long job progress | 2.a — realtime bus wrapper | Works today |
| Local inference (Ollama pinned, embedding server) | 2.c — sidecar with streaming response | Deferred to 0.8.x |
| File download / stored media playback | 2.d — storage HTTP with Range | Solved by 0.10.0–0.10.1 |
| Resumable upload | 2.d — storage HTTP | Solved by 0.10.0–0.10.1 |
| Live mic / camera / WebRTC | Not this doc — system services arc | Deferred post-composition |

## 4. What Is Decided Now

Three small moves, none of which block 0.3.x work currently in
flight:

1. **Client SDK spec (0.6.3) includes `plinth.stream(cap, args)`.**
   Documented as the canonical streaming API. Implemented as a thin
   wrapper over the realtime bus (mechanism 2.a). Even if the bridge
   later gains async-iterator support (2.b), this API is forward-
   compatible — only the transport changes.

2. **Sidecar contract (0.8.x) allows streaming responses.** One line
   in the contract: chunked / SSE responses are a supported response
   shape. No design work now; just don't foreclose. The exact
   framing, end-of-stream signaling, and kernel↔sidecar↔bus bridging
   are 0.8.x design-doc work.

3. **`plinth.call()` return value is opaque to the bridge.** The
   current 0.3.x bridge work must not assume anything about the
   handler's return value shape. Whatever JSON the handler returns
   gets returned to the caller. Streams are JSON containing a stream
   ID; that's the bridge's view. This is probably already the
   intended behavior — flagging it so it doesn't get accidentally
   narrowed.

## 5. What Is Deferred

- Exact bus event shape for stream chunks (naming, payload
  envelope, end-of-stream marker).
- Error propagation mid-stream.
- Cancellation: client disconnects mid-stream — does the handler
  stop producing? Decided in 0.5.x or 0.6.3.
- Back-pressure from slow clients.
- Multiple concurrent streams from the same capability call.
- Whether to adopt mechanism 2.b (async iterators in the bridge).

## 6. What Is Rejected

- **Sidecars for transport reasons.** Sidecars are for heavy compute
  that doesn't belong in QuickJS. Wrapping a remote API — even a
  streaming one — is not sidecar work.
- **Sidecars as a distinct streaming mechanism.** Sidecars are
  provider implementations of capabilities that happen to stream,
  not a separate dispatch path. The streaming substrate is the
  realtime bus (§2.a); the sidecar contract must be compatible with
  that substrate. See `DISCUSSION-ai-bridge.md §4.d` for the
  provider abstraction.
- **A dedicated streaming capability dispatch path.** Adding a
  second dispatch mechanism alongside `plinth.call()` doubles the
  surface area and authoring complexity. The realtime bus is the
  mechanism.
- **Streaming through QuickJS for byte/media content.** Bytes go
  through Drogon via the storage HTTP surface. Audio and video
  pipelines do not traverse the JS runtime.

## 7. What This Document Is Not

Conversation capture, not a design doc. The client SDK spec and the
sidecar contract each do their own design-doc-level work when those
milestones arrive. This document records the framing and the three
don't-foreclose decisions; everything else is deferred.

## 8. Appendix: Plinth vs. Armature Streaming

Armature could not stream because Starlark is a config language
without real async and the bridge was request/response. Plinth's
improvement here is not in the scripting language alone — async JS
helps, but the missing piece in Armature was a realtime bus that
extensions could emit on. Plinth has that (0.5.x) as an independent
kernel primitive. Together, real async + realtime bus = streaming
that doesn't need a new transport mechanism.

That matters because it means Plinth's capability increase over
Armature includes streaming *without* a dedicated streaming
primitive. The primitives (capability dispatch + realtime bus +
storage HTTP) compose into streaming. A new blessed streaming API
would be a fifth primitive solving what four already solve.
