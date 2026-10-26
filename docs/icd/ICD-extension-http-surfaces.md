# Extension HTTP surfaces — implementation contract

**Status:** maintainer-selected implementation contract for [#47](https://github.com/gobha-me/plinth/issues/47), adopted on 2026-10-26 after independent protocol review. Delivery requires independent review and green candidate/exact-merge CI. The general extension HTTP primitive is **not implemented**. The accepted specification revision gates implementation in [#48](https://github.com/gobha-me/plinth/issues/48); specification merge alone does not complete the measured-contract outcome.

**Source baseline:** [`24c9a993efa660e5d9703d7620a1b78c99adfd7f`](https://github.com/gobha-me/plinth/commit/24c9a993efa660e5d9703d7620a1b78c99adfd7f), tree `e88bb7c34a3346956dd0529208acd6ad1d335cc2`. Baseline references describe current behavior, not execution evidence for this future primitive.

**Authority:** [Extension architecture §6](../architecture/05-extensions.md#6-extension-http-surface) owns the ratified primitive; the [contract authority rules](../contracts/README.md#authority-and-conflicts) govern reconciliation. The bounded binary buffered, cookie-free, PAT-only new-route profile and its adapter/audit/timeout protocols below are selected delivery requirements for #48. This is not whole-application WebDAV/streaming/cookie compatibility or a change to existing APIs, grants, bundles, global transport limits or SDKs.

## 1. Authority, outcome and delivery sequence

**R** denotes a constraint of the already ratified primitive. **C** denotes current source behavior. **P** denotes an accepted planned requirement for #48, not current support. Section 17 records the maintainer's scoped decisions and the remaining measured gate; implementation classes and storage DDL belong to #48.

R: one kernel catch-all, manifest-declared exclusive HTTP ownership outside kernel/active-frontend routes, install conflict validation, kernel PAT validation before dispatch, captured handler references for THREE modes, safe lifecycle drain, authoritative per-dispatch audit and measured overhead. No arbitrary runtime registration, method/host split, per-prefix mode or fallback provider.

C: general HTTP claims are not implemented. The manifest parser retains unknown fields. Existing package/auth/CSRF/lifecycle/audit APIs remain authoritative and unchanged. Shell alone is a trusted bundled companion; Admin remains an ordinary API-installed package. Current void audit has no caller persistence acknowledgement.

The #47 authoring sequence uses completed post-0.6.3 architecture re-evaluation #29 and CSRF/origin #33. It does not call #29 a completed post-Dogfood review or satisfy, close or weaken #38/#39. An accepted independently reviewed revision gates #48. A specification may merge while #47 stays OPEN; #48 must implement, test and supply actual mode-qualified performance measurements and the recorded final threshold decision before the measured criterion and remaining #47 acceptance can close. Historical release aliases do not change this ordering. #46/#49 are not silently inserted as prerequisites.

## 2. Exact declaration and compatibility rules

P top-level fields:

```text
http_prefixes: array<string>, optional, maximum64
unauthenticated_prefixes: array<string>, optional, maximum64
handler_mode: "quickjs" | "sidecar" | "bundled_native"
```

Absent/empty claims mean no HTTP surface; absent/empty public subset means all claims protected. Each public entry must exactly equal a declared claim. Duplicates/overlap reject, including within one package. Nonempty claims require one handler_mode. For NEW explicit install/upgrade, a mode without nonempty claims rejects `http-mode-without-claims`; unknown mode or per-prefix mode objects reject. No permissive legacy fallback after malformed explicit declarations/raw duplicate keys.

Compatibility is provenance-based, not a rescan: already-installed unknown HTTP-looking fields remain inert on enable/restart/recovery until an explicit successful install/upgrade under this shared validator creates kernel-owned validated authority. No backfill/self-certification from JSON. Existing packages lacking claims retain existing behavior. CLI validation/install/upgrade use the same parser/findings; this does not import broad #84 work.

P logical ClaimReceipt1, persisted by the kernel, contains package/generation/artifact identity, canonical ordered claims/public subset, mode, adapter-binding identity and validated schema1 provenance. ACTIVE publication requires a matching committed receipt, physical handler readiness and captured generation. Recovery uses the committed receipt and package transition state, never reallocates authority from an arbitrary directory or unvalidated manifest. Storage layout and classes belong to #48; the externally relevant authority/recovery invariant is fixed here.

## 3. Grammar, matching, reservations and conflicts

P declarations are case-sensitive ASCII, ≤256bytes each:

```text
claim = literal | literal "/*"
literal = "/" segment *("/" segment)
segment = 1*(ALPHA | DIGIT | "." | "_" | "~" | "-")
```

Whole segments `.`/`..`, root `/`, empty segments, bare trailing slash, whitespace/control, percent escapes, query/fragment, backslash, authority/scheme and interior/multiple wildcards reject. Do not trim/fold/decode a declaration into validity.

Exact `/status.php` matches only that path. `/dav/*` matches `/dav`, `/dav/` and all segment descendants, not `/davx`. Query never selects ownership. All recognized HTTP methods share one owner; the route may return a protocol error but cannot hand an unsupported method to a second owner. Claims are single-tenant/all-host; no per-host split.

R/C reservations follow current architecture§2: kernel mount roots `/api`, `/ext`, `/s`, `/healthz`, `/metrics`, `/ws` retain segment-subtree protection; exact root redirect is not the entire URL tree. Respect configured/installed frontend ownership, including default `/app`. This reconciles the earlier planned `/docs/*` example with current §2: no new `/docs` reservation; `/api/docs` is already under `/api`. A new reservation requires its own architectural decision.

Conflicts are intersections of the above ownership sets, not unbounded prefix guesses. Reserve disabled/draining/recoverable installed generations until completed removal. Serialize conflict check/reservation/publication with package transitions; two racing installers cannot both commit. Same-name upgrade replaces authority atomically, failed upgrade preserves prior/fenced recovery state, no ownership gap or dual generation. Sort findings by canonical claim bytes; do not leak invisible owner metadata. Example `/dav/*` versus `/dav/files/*` rejects, versus `/davx/*` coexists.

## 4. Supported path/query representation

C pinned Drogon exposes selected UNDECODED getOriginalPath(), decoded getPath() and separately stored raw query()/getQuery(); its parser splits query at first literal`?`, can strip absolute-form scheme/authority and maps an empty absolute path to`/`. It does not preserve a delimiter bit distinguishing `/x` from `/x?`.

P common data therefore contains `original_path`, validated `path` and raw `query`, NOT full original_target/authority/form spelling. `/x` and `/x?` intentionally produce identical data. Do not reconstruct missing bytes or demand a framework patch solely to preserve an unused delimiter. Use public query accessor, not lazily decoded parameter maps.

Validate represented query independently: empty or visible ASCII0x21..0x7e excluding literal`#`, with every percent sign followed by two hex digits; no raw space/control/DEL/non-ASCII. Do not percent-decode query or validate percent-decoded query as UTF-8. `%FF`, `%00`, `%23`, `+`, later literal`?`, order/duplicate keys retain their represented spelling. Application interpretation belongs to the handler, never a second routing pass.

Validate original-path/decoded-path ownership consistently: reject malformed escapes, encoded separators/backslash/NUL, controls, invalid path UTF-8, repeated separators/dot segments and encoded aliases BEFORE lookup. Valid encoded filename data is data. A decoded`?` in path cannot become a query delimiter. Literal`#` in the exposed path/query rejects; do not claim access to stripped raw target portions. The final adapter must prove these chosen codec/router semantics through supported APIs, not invent an accessor.

## 5. Binary buffered profile and FEATURE budgets

P selects a BINARY buffered profile, not JSON-only and not a claim of full streaming/WebDAV/S3 application compatibility. Both request and response may contain arbitrary bytes up to 8 MiB decoded length. Body semantics are independent of content type; no implicit UTF-8/JSON conversion. Larger uploads/streaming applications remain explicitly unsupported by this profile, with their product implications reviewed in §17. Do not claim the full intended primitive is complete merely because a small easy handler passes.

C current default GLOBAL transport body ceiling is51MiB (package50MiB+multipart1MiB); JSON package-size configuration changes that computed ceiling. Current memory-body/spill default is64KiB; request streaming is not enabled by Plinth. Preserve all those current API/package settings; never globally lower to8MiB. Ordinary handlers may receive an already memory/file-buffered body BEFORE feature admission. Neither the spill threshold nor feature quotas prove a new aggregate prehandler networking bound.

P ordinary-handler feature admission reserves all of the following credits before auth/additional copies. Existing transport objects remain separately owned; the feature charges its retained references rather than pretending those bytes first arrived now.

| Credit/limit | Selected future value and meaning |
|---|---|
| Active feature tasks | maximum 4 per package,8 across this feature; includes admitted preauth/schema/persistence/response work, not only handler execution. |
| Borrowed transport retention | maximum 256 MiB per package and 512 MiB feature-wide. Charge actual retained body/metadata/file byte length from trusted transport metadata before retaining it; no copy solely to discover length. If safe length accounting is unavailable, reject bounded admission rather than fabricate a counter. |
| Extra feature memory/copies | reserve96MiB per lease,384MiB per package,768MiB feature-wide; actual buffers and overlapping marshal/encoded/decoded/output copies must fit that credit. Reserve before allocating; no uncharged extra copies. Runtime's own lower limits still apply, never raised implicitly. |
| Feature-created temporary files | ZERO; no adapter-created spool/cache. Framework request-body spill files may preexist and remain charged as borrowed storage while retained. |
| Body limits |8MiB request and8MiB response decoded bytes; enforce before additional full-body copies/output serialization. |
| Headers | maximum 64 pairs,32KiB aggregate UTF-8 name/value bytes, token name1..128ASCII bytes, individual value≤8KiB; no CR/LF/NUL. |
| Paths/query | original_path, validated path and represented query each≤8KiB. |
| Queues/retries | no unbounded preauth/postauth queue, no handler retry. Resource rejection uses the existing transport owner to respond without creating an extension task. |

These are FEATURE upper bounds from ordinary-handler entry onwards, not a wholesale global connection retrofit. Header/body feature errors come after feature resource admission/auth as in§13; absolute framing/global transport rejection may happen earlier. Use metadata/bounded reads to reject a received8..51MiB body without first copying it into new feature buffers. On rejected admission the existing request/transport owner remains responsible until its normal release.

P chooses ordinary buffered handlers for the initial interface. Supported optional request-stream APIs are an explicitly separate future body profile, not silently enabled globally or necessary merely to meet this chosen buffered contract. Any later stream profile must preserve every mode's bounded representation, existing API compatibility and actual ownership; no pause/cancel/backpressure/task-join API is inferred. Header-ready streaming may still expose previously retained body bytes. Current task/byte credits require concrete #48 integration/proof, but no unmeasured promise that the entire prehandler server has these extra quotas is made.

## 6. Authentication, public origin and install authority

R protected routes require PAT validation in kernel before dispatch. C existing APIs select a nonempty `plinth_session` cookie before Bearer. An absent or empty `plinth_session`, or unrelated Cookie headers, does not suppress Bearer selection. P preserves that selector, then admits only a selected Bearer PAT for NEW protected claims: a nonempty `plinth_session` alone or together with a PAT, or a selected Bearer session token, fails PAT-required with no fallback to another credential. Missing/invalid/revoked/expired/disabled identity401; auth backend unavailable503. All Cookie headers are still removed before extension invocation. No runtime kernel.admin rule is invented for every application request; existing application capability/RBAC checks still use the authentic caller.

P public claims always supply anonymous context regardless of ambient Cookie/Authorization; do not elevate those credentials. Current PublicOriginFilter is METHOD-INDEPENDENT, whereas CsrfFilter has the safe-method exemption. P intentionally places the existing origin predicate only on UNSAFE NEW anonymous extension requests, leaving existing API filters unchanged:

| Method/origin/marker combination | NEW public-route outcome after earlier admission |
|---|---|
| GET/HEAD/OPTIONS; any/missing/foreign/malformed Origin; markers present or absent | No origin-profile rejection; anonymous; other syntax/schema/protocol rules still apply. |
| Unsafe, same trusted Origin, either marker state | Anonymous origin pass. |
| Unsafe, foreign/malformed Origin, either marker state |403 csrf_failed; no handler. |
| Unsafe, missing Origin, existing browser_shaped_request predicate true |403 csrf_failed. |
| Unsafe, missing Origin, predicate false |Anonymous native-shaped origin pass. |

Safe anonymous navigation grants no CORS read/credentials/preflight promise. Kernel does not mint session/CSRF values for it. No global auth/CSRF change.

P install rules retain packages.install and require future kernel-owned packages.install.with_http_prefixes; public subset additionally requires packages.install.with_unauthenticated_prefixes plus explicit confirmation bound to artifact SHA/normalized declaration/actor. Mere dry-run confirmation cannot authorize a changed upload. No new grants are performed by install/upgrade or this specification. Existing administrative universal kernel.admin authority remains its current rule; reconcile historical plural admins wording to singular admin without auto-granting ordinary users. Bundle exception is only actual kernel-selected trusted provenance, never a manifest flag/API installer assertion. Grammar/conflict/readiness still apply. No new bundled production package or public registration API.

## 7. Literal common data, headers and response contract

P definitions in §§7–10 are the complete external version1 schemas. All listed fields are REQUIRED unless a record explicitly says otherwise; there are no omitted nullable fields or extension bags. Extra/inherited fields do not satisfy required data. Wire JSON duplicate/unknown member names reject before DOM overwrite. JS returned records must have exactly the named own enumerable data properties, no own symbols/accessors or proxy/coercion behavior; no inherited defaults or JSON-source duplicate detection is falsely claimed for an already-constructed JS object. Host-generated cancellation controls/methods below are explicit exceptions to plain-data descriptors. Arrays preserve order; objects are semantic key sets. Canonical senders emit fields in definition order, with no whitespace; receivers accept any object-member order but never duplicate keys. Headers remain ordered tuples, never a map that erases repeated fields.

### 7.1 Scalar types and complete value enums

```text
UUID      = lowercase ASCII 8-4-4-4-12 hexadecimal UUID string,36bytes
Hex256    = lowercase ASCII hexadecimal string,64bytes
Ns        = decimal ASCII string for uint64 nanoseconds: "0" or [1-9][0-9]{0,19},
            numeric value <=18446744073709551615; no sign/exponent/float
Name      = captured validated package name, ASCII [a-z][a-z0-9-]{1,63}
Version   = captured validated SemVer2 string,1..256ASCII bytes; never normalized here
Text      = valid Unicode scalar string, measured in UTF-8 bytes; no lone surrogate
Claim     = exact selected canonical declaration from validated ClaimReceipt1,
            ASCII <=256bytes and satisfying §3 grammar
PathText  = Text <=8192bytes satisfying §4 path rules
QueryText = ASCII <=8192bytes satisfying §4 opaque-query rules
Method    = actual uppercase method token from the kernel HTTP parser,1..32ASCII bytes,
            matching [A-Z][A-Z0-9!#$%&'*+.^_`|~-]{0,31}; no adapter-created method,
            no additional method allowlist/method-scoped ownership
HeaderName  = lowercase ASCII token matching [!#$%&'*+.^_`|~0-9a-z-]{1,128}
HeaderValue = Text <=8192UTF-8 bytes, no CR/LF/NUL, otherwise subject to HTTP validation
HeaderPair  = [HeaderName,HeaderValue], exactly2elements
Headers     = ordered array <=64 HeaderPair; sum name+value UTF-8 bytes <=32768
Status      = integer200..599 (never boolean/string/fraction/101)
Binary8     = owned byte sequence length0..8388608; no implicit text/JSON conversion
Body64      = canonical padded RFC4648 base64 ASCII without whitespace/JSON escapes,
              decoded length <=8388608; re-encode(decoded)==original
CancelReason     = "deadline" | "client_gone" | "shutdown" | "generation_retired"
AdapterErrorCode = "handler_failed" | "response_invalid" | "adapter_unavailable" |
                   "resource_exhausted" | "execution_cancelled"
SettlementReason = "completed" | "cancelled" | "failed"
PeerEntryState   = "not_entered" | "entered"
```

Method is deliberately a bounded token type, not a fictitious enum of methods a particular build might reject before this surface. GET/HEAD/OPTIONS are the exact safe-method subset; every other parser-recognized Method is unsafe. This preserves all-method ownership, including recognized protocol-extension verbs, without claiming support for a method rejected by the current HTTP parser. No UTF-8 interpretation is applied to decoded opaque query octets or Binary8.

### 7.2 Closed record/union definitions

```text
Principal1 = {kind:"pat", user_id:UUID, pat_id:UUID} | {kind:"anonymous"}
Extension1 = {package_id:UUID, name:Name, version:Version,
              artifact_sha256:Hex256, generation_receipt_id:UUID}
ContextData1 = {
  schema:1, dispatch_id:UUID, owner_epoch:UUID, adapter_epoch:UUID,
  clock_domain_id:UUID, admitted_at_ns:Ns, execution_cutoff_ns:Ns,
  matched_prefix:Claim, principal:Principal1, extension:Extension1
}
RequestData1 = {
  schema:1, request_id:UUID, method:Method, path:PathText,
  original_path:PathText, query:QueryText, headers:Headers
}
JsRequest1   = RequestData1 plus REQUIRED body:(Uint8Array|ArrayBuffer)
WireRequest1 = RequestData1 plus REQUIRED body_b64:Body64
NativeRequest1 = RequestData1 plus REQUIRED body:Binary8
JsResponse1   = {schema:1,status:Status,headers:Headers,body:(Uint8Array|ArrayBuffer)}
WireResponse1 = {schema:1,status:Status,headers:Headers,body_b64:Body64}
NativeResponse1 = {schema:1,status:Status,headers:Headers,body:Binary8}
AdapterError1 = {kind:"error",code:AdapterErrorCode}
WireOutcome1 = {kind:"response",response:WireResponse1} | AdapterError1
NativeOutcome1 = {kind:"response",response:NativeResponse1} | AdapterError1
Settlement1 = {reason:SettlementReason,entry_state:PeerEntryState,
               entered_at_ns:(Ns|null)}
```

`plus` means exactly the base fields followed by the one additional field, not arbitrary members. Every schema field is the NUMBER1. Empty body/headers/query are required explicit empty values. Anonymous has ONLY kind; PAT has exactly its three fields. IDs/context come from kernel captures, never request headers/body. For one request, request_id==context.dispatch_id==UDS dispatch_id. Context epochs/clock/extension match the captured AdapterBinding1; matched_prefix is the actual selected canonical declaration captured from the validated ClaimReceipt1, never caller input or a second lookup. Generation/claim identity never changes during auth, dispatch, result or recovery. Clock domain must be the internally verified same-host monotonic domain, not a peer assertion.

ContextData1 carries the kernel-captured matched_prefix in every mode; Version permits up to 256 bytes to match existing captured-generation limits. Neither grants new authority or implies that general HTTP context already exists.

`execution_cutoff_ns` is the EXECUTION/ENTRY cutoff only (§11); it is NOT wire expiry. `admitted_at_ns <= execution_cutoff_ns`. For Settlement1, not_entered requires entered_at_ns:null; entered requires nonnull timestamp equal to its prior controlled entered observation, admitted_at_ns<=entered_at_ns<execution_cutoff_ns. This is the peer's protocol assertion, not independent proof an arbitrary process has joined; the kernel still retains actual adapter/process owners. Kernel durable recovery may record entry UNKNOWN separately; it cannot invent a peer entered timestamp.

P response/framing/header policy: status200..599, no101/raw upgrade/CONNECT tunnel/trailer/socket transfer. Validate shape/bytes before applying HEAD/bodyless HTTP serialization. Unknown return members/type/coercion or excessive response gives fixed handler failure, not another owner. Request forwarding removes Cookie/Authorization/Proxy-Authorization/X-Plinth-CSRF/Host/Forwarded/X-Forwarded-* and hop-by-hop/Connection-nominated fields. Response forbids Set-Cookie/Cookie/auth/proxy-auth/hop-by-hop/framing/Upgrade/Access-Control-* fields; kernel owns framing/security policy. Other validated application headers remain bounded. No new application cookies/CORS or credential forwarding.

Normative owned-policy cross-reference: apply the existing `auth::harden_auth_response(response, origin_sensitive)` contract (`src/kernel/auth/middleware.hpp:45–48`, middleware.cpp:73–78) for authentication/error responses: its no-store and Cookie/Authorization Vary behavior, adding Origin through its origin_sensitive path where this request's origin policy applies. Retain the owned `csrf_failure`/CsrfFilter/PublicOriginFilter policies and proposed placement distinction in §6 (`src/kernel/auth/csrf.cpp:61–68,141–187`). Do not handwrite a competing cache/origin policy or change existing API filters. Kernel security/framing headers remain authoritative.

## 8. Literal QuickJS interface, cancellation and entry

P fixed captured `server/http.js` default export:

```text
handle(request:JsRequest1, context:JsContext1)
  -> JsResponse1 | engine-native Promise<JsResponse1>
JsContext1 = EXACT ContextData1 fields plus:
  cancel:JsCancellation1,
  remaining_execution_ms: host function () -> integer0..4500
JsCancellation1 = nonextensible host control with exactly:
  aborted: read-only live boolean getter,
  reason: read-only live (CancelReason|null) getter,
  onAbort: host function (callback:(CancelReason)->any) -> Unsubscribe1
Unsubscribe1 = host function () -> undefined
```

No wire-expiry getter or ambiguous remaining/deadline method is exported. remaining_execution_ms returns `max(0,floor((execution_cutoff_ns-now)/1000000))`, also0 for an already tighter cancelled owner. It is a convenience snapshot, never authority to start work from a stale value. JsContext1 metadata is deeply frozen; cancellation getters reflect host state, not user-writable identity. Request metadata/arrays are frozen; body is a handler-local mutable byte copy (typed-array contents are not falsely advertised as Object.freeze-immutable). Return body is snapshotted under credit before releasing user buffers; only Uint8Array/ArrayBuffer, no implicit strings/other typed arrays. Arbitrary thenables are invalid return shape, not a new implicit then-observation API.

onAbort requires exactly one argument; unsubscribe and remaining_execution_ms require zero arguments. Wrong arity throws TypeError before side effects. onAbort details are fixed: non-function throws TypeError; maximum 64 active registrations per request, exceeding it throws RangeError before adding. First abort freezes one CancelReason; later abort calls cannot replace it. Each active registration receives that reason at most once, on an OWNED runtime queue turn, never synchronously/reentrantly inside onAbort or the local arbiter. Registration AFTER abort schedules the same reason on that owned queue. Returned unsubscribe is idempotent/returns undefined: before callback starts it cancels that registration (including a queued post-abort callback); after start it cannot undo it and does not release its active owner before settlement. Completed/unsubscribed registration leaves the active set. Callback return is ignored (no awaiting/thenable observation), throw is contained, never another response/handler attempt. Destroyed runtime cannot create a new callback realm; retained callback owners must settle before release.

QuickJS queued entry must, under the captured request owner, check cancellation/epoch/generation and current monotonic `now >= execution_cutoff_ns` IMMEDIATELY before invoking the export. Expired/retired queued work does NOT enter user code, even if wire expiry is later or a timer callback was delayed. It returns a cancelled entry observation/settles owners. No fresh realm/mode/provider retry. Engine jobs, host-native work and callbacks retain the captured generation through actual settlement. Real binary/promise/cancel/lifetime controls remain required.

## 9. Literal bounded UDS frames and sequencing

P uses a kernel-owned internal AdapterBinding1 (no public registration API/arbitrary endpoint) and real eligible OS-peer/socket/generation ownership. Only this binding identifies the verified owner_epoch, adapter_epoch, clock_domain_id and Extension1. Peer hello echo is NOT the proof of OS identity or clock domain.

Frame:4octets UNSIGNED BIG-ENDIAN lengthN followed by EXACTLY N strict UTF-8 JSON bytes;1<=N<=12582912 (12MiB). Prefix excludes its own4octets. No compression/ancillary descriptors. Read length/check/reserve before payload allocation. Reject duplicate/unknown keys, invalid UTF-8, depth>8 or nodes>1024. Nodes count each object/array/scalar VALUE once (not member-name strings); root depth1 and each nested value adds1. Canonical sender JSON has no whitespace; definition field order; quoted keys/strings minimally escaped; no slash escaping, no base64 escapes, non-ASCII metadata emitted as UTF-8. Receiver accepts other member order but not duplicate keys; total encoded payload bound always applies.

Closed frame union (each literal key required; hello variants have NO dispatch_id):

```text
Hello1 = {schema:1,type:"hello",owner_epoch:UUID,adapter_epoch:UUID,
          clock_domain_id:UUID,extension:Extension1}
HelloAck1 = {schema:1,type:"hello_ack",owner_epoch:UUID,adapter_epoch:UUID,
             clock_domain_id:UUID,extension:Extension1}
Call1 = {schema:1,type:"call",owner_epoch:UUID,adapter_epoch:UUID,
         dispatch_id:UUID,generation_receipt_id:UUID,
         request:WireRequest1,context:ContextData1}
Accepted1 = {schema:1,type:"accepted",owner_epoch:UUID,adapter_epoch:UUID,
             dispatch_id:UUID,generation_receipt_id:UUID}
Entered1 = {schema:1,type:"entered",owner_epoch:UUID,adapter_epoch:UUID,
            dispatch_id:UUID,generation_receipt_id:UUID,entered_at_ns:Ns}
Result1 = {schema:1,type:"result",owner_epoch:UUID,adapter_epoch:UUID,
           dispatch_id:UUID,generation_receipt_id:UUID,outcome:WireOutcome1}
Cancel1 = {schema:1,type:"cancel",owner_epoch:UUID,adapter_epoch:UUID,
           dispatch_id:UUID,generation_receipt_id:UUID,reason:CancelReason}
Settled1 = {schema:1,type:"settled",owner_epoch:UUID,adapter_epoch:UUID,
            dispatch_id:UUID,generation_receipt_id:UUID,settlement:Settlement1}
Frame1 = Hello1|HelloAck1|Call1|Accepted1|Entered1|Result1|Cancel1|Settled1
```

Call1 equality checks are exact: request.request_id==context.dispatch_id==dispatch_id; context.owner_epoch/adapter_epoch match envelope/binding; context.clock_domain_id==binding clock; context.extension equals handshake Extension1; envelope generation_receipt_id==context.extension.generation_receipt_id; context.matched_prefix equals the actual captured selected claim from ClaimReceipt1 and matches request.path under §3/§4. Every per-call reply/cancel uses that identical identity tuple. It is never sufficient that a dispatch UUID alone matches.

Metadata fitting rules (including matched_prefix≤256 and Version≤256): sum decoded UTF-8 bytes of all metadata string VALUES excluding body_b64 is≤65536; count actual required fields/headers/paths/context. All non-body JSON tokens INCLUDING key spelling, quotes, punctuation and metadata escapes total≤524288 encoded UTF-8 bytes. This leaves maximal canonical Body64(8388608bytes)=11184812bytes plus metadata≤524288 =>11709100bytes, BELOW12582912; no escaped body alphabet doubles this bound. The largest closed Call1 with 64 header pairs has 227 JSON value nodes/depth5;1024/depth8 are additional hard ceilings, not an allowance for unknown nesting. Count length in emitted UTF-8 BYTES, not characters or UTF-16 code units; headers include their32KiB aggregate bound and shared path/context scalar caps. A serializer cannot use arbitrary whitespace/escapes to exceed12MiB and still claim maximal body support. Base64/body/frame copies remain within the existing96MiB credit, not a measured heap-use guarantee.

Legal state/ordering, per binding and dispatch_id:

1. Kernel sends exactly one Hello1, peer must reply matching HelloAck1 before calls. Any mismatch/other frame before handshake fences binding; no requests/retry are released on it.
2. Kernel sends Call1 ONCE only after durable obligation confirmation and the local serialized cutoff gate; no uncertain retransmission or application replay. Concurrent calls interleave only within existing4-per-package/8-feature/byte credits.
3. Peer sends exactly one Accepted1 before any Entered1/Result1/Settled1 for that call. If current receiver `now >= context.execution_cutoff_ns`, cancelled or retired before entry, it MUST NOT enter: send Result1 error execution_cancelled and then Settled1 cancelled/not_entered/null.
4. Otherwise peer rechecks the EXECUTION cutoff/cancel/epoch IMMEDIATELY at physical queued entry, emits Entered1 exactly once with admitted_at_ns<=entered_at_ns<execution_cutoff_ns, and then invokes once.5000ms wire expiry is NOT an entry deadline and is not transmitted as one.
5. Exactly one Result1 follows Accepted1 (and Entered1 if entered). It is a candidate only; after cutoff/choice freeze the kernel can ignore it for status while retaining owners. AdapterErrorCode maps handler_failed/response_invalid/resource_exhausted to handler-failure500; adapter_unavailable to HTTP503 with error `http_surface_unavailable`; execution_cancelled is valid only for cancellation or now>=execution cutoff and is resolved through the kernel's existing frozen cancellation/deadline choice, never a new200. An unsolicited execution_cancelled without that condition is response_invalid.
6. Settled1 follows Result1 exactly once ONLY after actual peer request children/callbacks/buffers settle; not_entered/null if never entered, or entered/timestamp equal to Entered1. Reason completed/cancelled/failed reports physical outcome, not a competing HTTP choice. Result, cancel acknowledgement or EOF alone is not settlement.
7. Kernel cancellation is idempotent: first CancelReason freezes and emits at most one Cancel1; later identical Cancel1 for the same live call is a no-op at peer. Cancel may race Accepted1/Entered1/Result1/Settled1; a cancel for an already-settled/unknown call at the valid binding epoch is a no-op with no new allocation/execution. A mismatched epoch/generation is not ignored as a valid cancel. Cancellation never produces a second Result1. If cancel arrived before entry, no Entered1 is permitted; if already entered, actual work must stop/settle through its owner.
8. Other duplicate, unexpected/future ID, wrong epoch, malformed/oversize or out-of-order frame fences the binding and prevents fallback/replay. A late result for a still-tracked cancelled/finalized call cannot alter its frozen decision but must be consumed/accounted to actual settlement; frames cannot recreate released task authority. Closed connection preserves UNKNOWN/owned failure where facts are missing; reconnect is a NEW binding epoch, never replay of old calls.

Receiver and all queues use the verified same-host monotonic execution_cutoff_ns. Peer timestamps/settled frames alone do not prove an arbitrary process physically joined; kernel adapter/process ownership must supply real conformance/settlement evidence. Noncooperation retains the owned failure fence/resources and bounded lifecycle failure, not fictitious join or automatic kill. Positive REAL framed/binary/epoch/cancel/settlement tests remain mandatory for full mode acceptance.

## 10. Literal native callable semantics

P compiled NativeHttpBinding1 remains internal and binds schema1, eligible trusted companion/artifact/generation and owner epoch to the fixed callable. No arbitrary symbol/library path, ordinary Admin self-certification, new production bundle or public registration API. Internal C++ ABI/class representation is #48 engineering; the language-neutral callable is fixed:

```text
invoke(request:NativeRequest1, context:NativeContext1,
       candidate:CandidateSink1, physical:SettlementSink1) -> CancellationHandle1
NativeContext1 = EXACT ContextData1 plus:
  cancel:CancellationView1,
  remaining_execution_ms: function()->integer0..4500
CancellationView1 = same aborted/reason/onAbort/unsubscribe semantics as§8,
                    callbacks on the owned adapter executor (not detached)
CandidateSink1.submit(outcome:NativeOutcome1) -> "accepted"|"duplicate"|"retired"
SettlementSink1.settled(settlement:Settlement1) -> undefined
CancellationHandle1.request(reason:CancelReason) -> undefined
```

All handles/sinks are owned, not raw caller references. Candidate accepts at most one response/error; duplicate returns duplicate, retired owner returns retired, neither selects another status or releases physical owners. Error codes/cancel conditions map identically to §9. Settlement fires exactly once after actual native children/callbacks/bytes settle; duplicate settlement is contained/fails invariant without another decrement. Cancellation request is idempotent/first reason wins, with subscribe/unsubscribe/post-abort behavior fixed above; no automatic settlement from requesting stop or invoke returning. Exceptions produce one handler_failed candidate and preserve actual cleanup ownership.

The native scheduler must recheck owner epoch/generation/cancel and `now >= execution_cutoff_ns` IMMEDIATELY before actual callable entry, not merely when enqueueing. Expired queued invocation emits no user callable entry; records not_entered/cancelled and settles through owned queues. QuickJS and sidecar have the SAME cutoff rule. Wire expiry remains kernel-only and cannot extend admission. Real eligible callable/exception/duplicate/cancel/lifetime fixtures remain required; mode-unavailable alone cannot close this scope.

## 11. Logical storage, deadline and owner state

P request states: TRANSPORT_OWNED → TARGET_VALID → SELECTED → FEATURE_ADMITTED → AUTH/ANONYMOUS_VALID → FEATURE_SCHEMA_VALID → DURABLE_OBLIGATION_CONFIRMED → DISPATCHED → CHOICE_FROZEN → CHOICE_DURABLE → WIRE_TERMINAL; physical owners settle separately before release. FEATURE_ADMITTED is a bounded resource owner, not extension dispatch. Current transport may already hold full body bytes. No new global prehandler guarantee is implied by this diagram.

Selected EXTRA timing has TWO named boundaries. At admission t0: `wire_expiry_ns=min(t0+5000000000, already-known enclosing deadline)`; `execution_cutoff_ns=min(t0+4500000000, wire_expiry_ns)`. Both use the verified monotonic clock and can only be shortened, never reset. Ordinary earlier body receipt remains transport-owned/outside this feature clock. The kernel-only request owner retains wire_expiry_ns; only execution_cutoff_ns/admitted_at_ns appear in ContextData1/UDS. All mode remaining_execution_ms queries refer to4500ms execution cutoff, NOT5000ms wire expiry. No callable may use wire expiry to authorize queued entry.

QuickJS runtime jobs, native queued invoke and UDS receiving/queued dispatch each check captured owner/cancel/generation and EXACT `now >= execution_cutoff_ns` immediately before physical handler entry; an enqueue at4400ms followed by entry attempt4600ms is cancelled/not_entered even if wire is open. A deadline callback queued late cannot extend entry. Already-entered physical work is cancelled/settled under its actual owners, not magically joined at cutoff. At wire_expiry_ns, the kernel closes/UNKNOWN if durable choice/response outcome is not safely available, never replaces a frozen200. Known tighter retirement/coordinator cancellation prevents entry independently of the immutable captured cutoff; receiving cancel cannot extend it. This does not enlarge existing package/coordinator budgets or promise a504 under storage uncertainty.

At the serialized admission/choice gate, `now >= cutoff` means expired, even if timer callback was delayed. All auth/schema/prepare callbacks carry captured owner epoch/generation/version. Expiry closes ADMITTED→DISPATCHED; late completions settle actual owners only. Handoff is bounded/single-use, with per-mode queued-start deadline/cancellation gates; no uncharged postauth queue or handler retry. No native/promise/socket task is automatically joined by response, disconnect or reader destruction.

Claim/AdapterBinding/DispatchObligation semantic records are kernel-owned and survive committed recovery; ordinary request fields cannot forge them. Old unvalidated JSON cannot be backfilled into claims. Missing/mismatched receipts give unavailable, never fallback. Recovery preserves immutable decisions and generation attribution; it never reruns an uncertain handler. Concrete C++ classes/storage tables/indexes belong to #48, but these externally observable invariants are fixed, not optional implementation guesses.

## 12. HTTP-specific durable per-dispatch audit and single arbiter

P uses one local response arbiter. Existing void audit APIs and all old callers remain unchanged. HTTP requires a separate acknowledged durable-obligation contract; invoking void audit is NOT its implementation. This document does not implement storage or alter the current writer.

Before actual dispatch, durably accept DispatchObligation1 with unique dispatch_id, immutable principal/prefix/method/package/generation/adapter, owner epoch and version. Unavailable/unknown prepare bars dispatch; no application side effect may be started to “try it anyway.” Actual dispatch means the one irrevocable kernel execution-authorization handoff; physical handler entry is recorded separately KNOWN_ENTERED/KNOWN_NOT_ENTERED/UNKNOWN. Crash between authorization and entry is not silently classified as physical execution. Proven pre-dispatch abort is not emitted as an executed dispatch.

One authoritative logical `extension.http.dispatched` event exists per dispatched ID. State/journal/projection records for that ID are refinements, not additional logical events; distinct IDs may not be coalesced under a60s action/prefix dedup. Rate-limited diagnostic chatter stays separate. Event identity/detail is kernel-authored; no extension opt-out/caller override.

The local owner serializes handler result, cutoff/cancel and wire callbacks and freezes ONE choice: `{kind:response,status,reason,choice_id}` or `{kind:no_response,status:null,reason}`. A timely handler may win200; expiry first wins504; disconnect before a choice may select no_response, not invented499. A delayed handler after cutoff cannot win200 merely because its timer was queued late. Once frozen, timeout cannot replace200 with504.

Persist ONLY that immutable choice idempotently under dispatch_id/epoch/version. Durable commit ACK or authoritative bounded readback of the SAME payload is required BEFORE any wire status. No independent200/504 database contenders exist in the live owner. A conflicting/stale stored choice is fail-closed; never overwrite/reply twice. If finalization outcome is unknown at the wire bound, close/UNKNOWN; late ACK settles ownership but cannot reopen/respond/retry.

Separate `response_decision_status` from `wire_status_observation` and delivery: durable200 is NOT proof200 was attempted/delivered. Wire state is NOT_ATTEMPTED, ATTEMPT_STARTED, WRITE_COMPLETED, WRITE_FAILED or UNKNOWN; server write completion is not client receipt. Record only actual observations. A crash between write and observation remains UNKNOWN. A locally frozen choice lost before durable commit cannot be reconstructed as a retrospective504; recovery records terminal-choice uncertainty. A committed-but-ACK-lost choice is preserved after readback.

The durable event/refinement carries only bounded IDs, validated method/status/prefix/package/generation, user/PAT or anonymous marker, existing appropriate user/session/IP context and audit schema1 decision/wire/entry states. No token/hash/Cookie/Authorization/query/body/private full resource path or raw exception text. Current AuditCtx has no top-level PAT column; do not invent one as current fact. Audited results cannot falsely imply rollback when handler side effects preceded a lost response.

Recovery acquires a new fenced epoch; stale writes/observations cannot supersede it. FINAL choices are immutable, pending obligations remain recoverable until explicitly terminal/retained, no TTL discards them. Recovery never reconnects/replays a user's request. An epoch fence does not itself kill an old socket/native task; actual owners still settle under their lifecycle. Bounded pending storage/backpressure denies NEW dispatch before overflow. Eventual finalization assumes durable-store recovery; permanent store loss is not given a fabricated unconditional liveness guarantee. No best-effort downgrade is selected.

Section 17 records the scoped decision/status/physical-entry reconciliation of the older audit prose. Internal prepare/finalize/observe persistence and projection design is #48 work; the required durable acknowledgement, idempotence/fencing and failure outcomes above are not left as undefined external behavior.

## 13. Drain, deterministic errors and mixed faults

R/C safe drain remains admission-closed/actual-owner settlement; P adds EXACT HTTP-specific future configuration `extension_http.drain_timeout_ms`, optional integer (not bool/fraction/string), default5000, configured range1000..5000 inclusive. Invalid configured values reject configuration; this is NOT a rewrite/clamp of existing `packages.upgrade_drain_timeout_ms` or current APIs.

For one transition at monotonic t: `http_drain_deadline=min(t+configured_http_ms, enclosing_package_deadline, enclosing_coordinator_deadline)` for the enclosing deadlines that exist. The1000ms lower bound is for the CONFIGURED value only; remaining enclosing time may be700ms,100ms or0 and wins WITHOUT rounding/clamping up or starting a new1000ms wait. A package deadline longer than5000ms still cannot extend HTTP beyond its configured maximum. Already-expired/zero remaining deadline means immediate bounded drain failure when owners remain; no extra wait. Examples: HTTP5000/package remaining700 gives700; HTTP1000/package remaining6000 gives1000; HTTP1000/coordinator remaining100 gives100. The SAME absolute remaining deadline is shared across relevant owners—no serial5s HTTP/native/audit waits and no reset on callback/progress.

Close new admission503 before transition; wait actual physical owners, not just logical status. On settled zero, remove/replace atomically. On pre-destructive expiry, fail transition; restore valid original ACTIVE admission under its lock or preserve established invalid/recovery fence. Retain unsettled code/data/tasks; never fake counters or destroy them on a timer. Current package setting and overall coordinator limits remain unchanged. Shutdown composes existing gate/dependency order without atexit/detached cleanup.

P feature errors use bounded `{error,message}` without credentials/uncontrolled exception/path data. Ordered conditions after existing absolute transport/fixed-route handling:

1. Invalid exposed path/query/target bound400 http_target_invalid.
2. No committed owner404 http_surface_not_found.
3. Draining/unavailable owner503 http_surface_unavailable.
4. FEATURE task/byte retention capacity exhausted503 http_surface_busy BEFORE auth/origin/feature schema; no unbounded queue.
5. Missing/invalid protected PAT401; backend unavailable503 service_unavailable; public unsafe origin failure403 csrf_failed.
6. Invalid feature request/header syntax400 http_request_invalid; header/count/aggregate/body feature-size excess413 http_request_too_large. No unbounded copy before checking.
7. Durable-obligation prepare unavailable before dispatch 503 http_audit_unavailable if wire can safely respond, else close; no handler.
8. Captured adapter loss503 `http_surface_unavailable`; handler error/invalid response500 http_handler_failed; logical cutoff choice504 http_handler_timeout. All go through one arbiter/ACK gate once dispatched; no secondary owner/mode.

A2xx/non2xx cannot by itself prove application side effects or durable wire delivery. Existing package install APIs keep canonical report envelopes. Invalid declarations use fixed findings `http-prefix-invalid/duplicate/overlap/reserved`, `http-public-prefix-not-declared`, `http-mode-without-claims`, `http-handler-mode-unavailable`; competing otherwise-valid ownership uses the existing conflict-report integration, not an unrelated endpoint redesign.

E01 fixed mixed-fault outcomes: existing invalid framing/global transport violation wins before feature; malformed represented query escape plus full capacity/bad PAT gives400; no owner plus full capacity gives404; draining owner plus busy/bad PAT gives503 unavailable; READY owner busy plus invalid PAT gives503 busy/no auth; available feature lease plus bad PAT and8..51MiB body gives401 without new full-body copy (transport may already hold it); public unsafe foreign Origin plus bad feature body gives403; public safe GET/HEAD/OPTIONS with foreign/missing Origin/markers remains anonymous and then yields valid dispatch or its400/413 schema error, not origin403. `/x` versus `/x?` is NEVER an error distinction.

## 14. Complete three-mode implementation acceptance

Mode-unavailable/readiness refusals must be tested, but are NOT a shortcut to #48 completion. Full acceptance requires an actual QuickJS binary request/promise/cancel owner, actual UDS framed peer with epochs/cancel/real settlement, and actual eligible compiled-native callable with lifetime proof. A named partial implementation child may merge with an explicit incomplete mode ledger; whole #48 cannot close by silently redefining scope to QuickJS or mock callbacks. No new production bundled package/public registration API is needed merely to create truthful test-only positive fixtures.

Proposed fixed ledger remains 59 named requirement IDs across 13 families (not 59 executed cases or a claim of current coverage):

| IDs | Literal positive/negative obligations |
|---|---|
| M01–M06 | Valid declarations/binary mode binding; exact grammar; duplicate raw keys;64/65+256/257 edges; public subset; missing/unknown/mode-without-claims. |
| C01–C06 | Exact/subtree/component conflicts; kernel/frontend boundaries; retained disabled claims; concurrent installers; atomic upgrade; recovery/provenance with no unknown-field activation. |
| P01–P04 | Actual original/decoded path+raw query API mapping; separator/dot/UTF-8 path controls; valid encoded filenames; opaque `%FF/%00/%23`, later`?`/`+`, malformed query and intentional empty-delimiter normalization. |
| A01–A06 | PAT positive; revoke/expiry/disabled; authDB unavailable; cookie/Bearer-session/mixed credentials; anonymous no ambient identity; full safe/unsafe Origin/marker truth table with unchanged API filters. |
| I01–I04 | Base installer denial; extra public rule+artifact confirmation; trusted provenance only; ordinary Admin cannot self-certify, no automatic grants. |
| Q01–Q04 | Real binary request/response including nontext8MiB edges; native Promise/reject/invalid thenable; quota/runtime limits; captured module/cancel/actual settlement. |
| S01–S04 | Literal Frame1 real UDS binary/handshake/identity/UTF-8 length/node bounds; wrong/oversize/duplicate/out-of-order unions;4500 execution versus5000 kernel-wire/cancel/disconnect/no replay; endpoint provenance+real peer/owner settlement, negative noncooperation retains failure owners. |
| N01–N04 | Real eligible callable; exception/duplicate sink; captured bytes/context; cancel and actual physical settlement with no new production bundle. |
| B01–B04 |8MiB+header/bodyless/protocol edges; credential/cookie/CORS/framing header rejection; feature retention/copy/temp-file credits; actual existing transport buffering plus no global51MiB change/false aggregate guarantee. |
| L01–L06 | Admission fence and immediate queued entry checks at4500ms (not5000); held physical work timeout; HTTP config1000/5000 edges and remaining700/100/0 cap without clamp-up; failed transition proper fence; actual joined ownership/recovery. |
| E01–E03 | Above mixed-fault precedence; no fallback/retry; lost/partial wire response is not rollback/idempotent replay. |
| U01–U05 | Exact kernel audit privacy/identity; unique ID versus distinct-ID retention; actual durable prepare/finalize; delayed ACK+local cutoff+wire UNKNOWN races; crash/readback/epoch/physical-entry uncertainty and joined recovery. |
| F01–F03 | Equivalent fixed/catch-all experiment; complete predeclared mode/trial/sample data; actual measured accepted per-request target with uncertainty, no percentile substitution. |

## 15. Full measurement obligation and issue47 status

R: 100 µs starting bound is PER REQUEST for catch-all+lookup+auth added overhead versus equivalent fixed route. This contract does not substitute p95, mean, small handler, missing adapter or submission-only audit for it. Same binary, hardware/limits, actual auth, handler/binary payload, connection and DURABLE audit policy on both sides; instrument comparable boundaries and also end-to-end latency. Keep every sample/failure and uncertainty, report distributions/max as description, not retroactive criteria.

P experiment skeleton: three paired trials per actually implemented mode, alternating order,1000warmups+10000timed requests each route/trial at concurrency1; one predeclared bounded concurrent profile within chosen4/8+byte credits. Missing/error/skipped/unavailable-mode data invalidates that claimed scope. No benchmark is executed here. Actual #48 data and explicit reviewed environment/measurement interpretation are REQUIRED to ground the per-request criterion. Any changed threshold/statistic needs recorded scope/architecture reconciliation, not selection after seeing which number passes.

The accepted sequencing keeps #47 OPEN after any specification merge until real #48 measurement and its remaining criteria are met. A source-complete document is not measured acceptance. General browser/load/security/publication readiness is not inferred.

## 16. Review and implementation boundary

This text fixes the external delivery contract. #48 chooses internal C++ classes/DDL/journal projection/buffer plumbing; it must implement the stated closed schemas, credit accounting, fences/ACK semantics and real adapter ownership, not treat them as unspecified later policy. Supported framework APIs and ordinary buffering are available; exact raw empty-delimiter preservation/global-network retrofit is NOT a hidden prerequisite. A technical integration failure remains explicit proof work under the selected contract; it does not authorize skipped mode/audit/measurement credit.

The spec itself registers no route/permission/grant/adapter/process and changes no existing global body/filter/API. No new production bundled package, public registration/bootstrap API, storage service, unrelated infrastructure work or #42/#84 scope expansion follows.

## 17. Maintainer decisions and remaining measured gate

The maintainer selected the following bounded delivery decisions on 2026-10-26 after independent protocol review. Delivery requires review and all configured candidate/exact-merge checks. The #47 sequencing retains its original four criteria and actual #48 measurement requirement:

1. **Audit selected:** irreversible kernel authorization with explicit physical-entry uncertainty; durable logical event/refinements; one immutable response decision distinct from observed wire status; UNKNOWN/close for storage/ACK uncertainty. Architecture §6.7 records this scoped reconciliation. Dedup applies only to the same dispatch ID, never distinct requests; the existing void writer is not a substitute for the acknowledged HTTP obligation.
2. **Profile selected:** 8 MiB binary bodies, 4/8 feature tasks, 96 MiB per-lease extra memory, 256/512 MiB retained transport credits, no new feature temp files, 4500 ms execution cutoff/5000 ms wire expiry and HTTP drain default5000/range1000..5000 capped by remaining transition. The selected profile excludes larger uploads and streaming; it does not supply whole WebDAV/S3 application compatibility. All three real adapters remain required. A larger-body/stream profile needs an explicit reviewed extension, not a false claim that it already exists.
3. **Credential/header profile selected:** cookie-free/no-new-CORS application interface and selected-PAT-only protected routes with the precise `plinth_session` precedence in §6. Application cookies, cross-origin credential forwarding or arbitrary remote endpoint authority require a separate trust/profile amendment. Existing API credentials/filters are unchanged and no new grants are made.
4. **Measured gate OPEN:** actual mode-qualified measurements must ground the per-request100µs target and declared environment/uncertainty interpretation before #47 closes. No percentile shortcut or unsupported-mode substitute is approved. All three real adapters and failure/restart owners must be proved before whole #48 completion.

Acceptance of this specification does not implement the primitive, prove tests or measurements, or authorize a release. Structural trust/lifetime changes still require the authority named by the contract rules. This document creates no route, permission/default grant, package, public registration API or global networking retrofit.

## 18. Source and contract references

Current-source statements are bounded to the stated baseline. Future schemas/protocols are delivery requirements, not shipped APIs.

- [Extension architecture](../architecture/05-extensions.md), §§1.4,2,6: ordinary/bundled provenance, reservations and the ratified future primitive.
- [Deferred HTTP questions](../DEFERRED.md): host/per-prefix dispositions, drain proposal and unresolved actual measurement.
- [Contract authority](../contracts/README.md) and [shutdown ownership](../architecture/shutdown.md): scoped reconciliation, owned callbacks/tasks and bounded dependency teardown.
- [Manifest parser](../../src/kernel/packages/manifest.cpp) and [typed manifest](../../src/kernel/packages/manifest.hpp): existing unknown-field retention, no HTTP activation.
- [Cross-file validator](../../src/kernel/packages/cross_file_validator.cpp): current reserved frontend-mount roots/component boundaries; [package routes](../../src/kernel/packages/handlers.cpp): existing packages.install authority.
- [Authentication](../../src/kernel/auth/middleware.cpp) and [context/hardening declarations](../../src/kernel/auth/middleware.hpp): credential precedence/PAT validation and owned response hardening.
- [CSRF filters](../../src/kernel/auth/csrf.cpp) and [declarations](../../src/kernel/auth/csrf.hpp): safe-method/credential policy versus method-independent PublicOriginFilter; the new-route placement is specified in §6.
- [Configuration](../../src/kernel/config.hpp), [configuration loading](../../src/kernel/config.cpp), [main setup](../../src/kernel/main.cpp) and [lifecycle](../../src/kernel/packages/install_lifecycle.cpp): current body settings/default package5s drain and failure before destructive transition.
- [Audit declarations](../../src/kernel/logging.hpp): current void writer/context versus the future acknowledged HTTP obligation; this contract does not change old callers.
- Pinned Drogon `89aca8c7993c8194f2c109c1d06a3b45bf363d5d`, selected by [CMake](../../CMakeLists.txt): supported public original-path/query/body interfaces. Exact absent empty-query delimiters and automatic task join are not promised by them.
