# Shell Trays, Handler Resolution and Navigation Specification

**Specification owner:** [#45](https://github.com/gobha-me/plinth/issues/45).
**Implementation owner:** [#46](https://github.com/gobha-me/plinth/issues/46).
**Source baseline:** `f677f8d16bc35db2821e11859aa5dce13d37f53c`.
**Status:** Reviewed delivery specification, not shipped behavior, API
ratification or a release promise. Concrete engineering proposals below require
#46's architecture/source review before implementation. Approved product choices
are distinguished explicitly; no production code changes accompany this paper.

## 1. Authority, current behavior and sequencing

This specification owns prospective typed tray/float declarations, tray chrome,
content-type/default resolution and explicit navigation/focus adapters. It
supersedes those scoped historical sketches in
[the shell design](../design/DESIGN-shell-v06x.md), sections 3.1, 3.4, 3.5, 3.7,
4, 5, the tray/navigation milestone in 9 and the tray declarations in 12.
Unrelated delivery history remains history. Follow the
[documentation authority rules](../contracts/README.md#authority-and-conflicts).

Current behavior remains separate:

- [Primary discovery and launcher](ICD-application-discovery-launcher.md)
  owns shipped Home/app/tab navigation, retained primaries and generation
  replacement. Registration uses `PanelType::PRIMARY`; discovery explicitly
  selects primary rows. That catalog does not authorize floats or trays.
- `panels_manifest.cpp` recognizes `id`, `client_path`, `title`, `icon`,
  `rbac_rule`, `order` and the `panels` root. Unknown metadata is inert; database
  enum support for float/tray is not typed validation or admission.
- `panel_api.js` returns the original initial context reference (default `{}`).
  `navigate`, `openFloat`, `requestFocus`, `setTrayState` and `setTrayBadge`
  remain unsupported stubs. `onNavigationIntent` registers an ordered chain,
  but no production navigation adapter dispatches it. Its current internal loop
  ignores returns and stops at the first synchronous throw.
- The authenticated frame owns the current avatar, theme, scale and Sign Out.
  `client/shell/panels.json` is empty. Shell preference handlers derive the owner
  from `ctx.user.id`; neither caller-selected user scope nor administrative
  defaults are provided. `ext_shell.default_apps` is a migration comment only.
- `manifest_checksum` hashes manifest bytes, not the complete package artifact.
  Current extension dispatch leases a runtime pool and preserves caller/depth,
  but subsequently reads handler code from a mutable active package path. It
  is not the atomic generation/code/storage binding proposed in section 5.

Source anchors are `src/kernel/packages/{panels_manifest,install_lifecycle}.cpp`,
`src/kernel/frontend/applications.cpp`, `src/kernel/auth/{middleware.hpp,csrf.cpp}`,
`src/kernel/rbac/enforcement.cpp`, `src/kernel/capabilities/resolution.cpp`,
`src/kernel/extensions/runtime_registry.cpp` and the shell files named above.
Authentication supplies identity, not effective-rule data: each new route must
load fresh effective rules itself. Handler contexts do not contain
`effective_rules`; this paper does not invent them.

[The approved float specification](ICD-shell-floats.md) retains authority over
hard-five document-lifetime reservations, chrome, responsive geometry, dirty
close and descriptor-only persistence. Do not substitute primary cache policy,
reset that budget on logout or claim a sixth slot before cleanup settles.

The delivery sequence is: #43 specification delivered; #45 specification;
#44 mechanism plus separately reviewed typed admission seam; #46's existing
`openFloat` resolver adapter; real API-installed caller/provider/browser proof;
then remaining #46 tray/navigation/default delivery. A mechanism PR may merge
while #44 remains OPEN. Only its real-browser acceptance, including an approved
persistent descriptor through the unchanged SDK, closes #44. A fixture accepting
an internally resolved target is not working extension `openFloat`. #46 remains
open until its whole acceptance matrix passes. Notification data is #81; a
health/performance tray is #55; broader user-deletion protocols remain #97.

Non-goals: implementation here; new SDK methods, grants or settings panels;
primary catalog/cache changes; arbitrary component/dirty-memory persistence;
notification APIs; new sequence/replay guarantees; releases, bundling changes,
repository settings or approval services.

## 2. Typed declarations and context proposals

The prospective root is `{format_version: 2, panels: [...]}`. Absence preserves
the legacy object/array parser and primary behavior. Unsupported explicit
versions fail a new install/upgrade, never downgrade silently. Stored unknown
version/type metadata cannot activate on reads or restart: explicit install or
upgrade under the typed validator is required. #46 must review marker collisions
with legacy packages; this specification does not certify all old metadata safe
for automatic reinterpretation.

Keep the six current panel fields. Added fields are exactly `type`, `capability`,
`content_types`, `context_schema`, `accepts_navigation_intent`, `tray_states`,
`tray_initial_state`, `preferred_width`, `chrome_essential`. Types are `primary`,
`float`, `tray`; `settings` is unsupported. No `label`/`component` aliases or
executable validators. Unrelated unknown fields round-trip inertly; reserved
traits/slots do not activate augmentation.

| Type | Required future declaration and restrictions |
| --- | --- |
| Primary | Navigation opt-in defaults false. True requires `context_schema`; existing non-opt-in initial-primary behavior is unchanged. |
| Float | Package-owned registered capability, literal `content_types` and `context_schema`; both declaration and capability permissions are checked server-side. |
| Tray | Plain `tray_states` name-to-icon-token map, declared `tray_initial_state`, `preferred_width`; `chrome_essential` defaults false. Omit float capability/content types; navigation opt-in cannot be true in this delivery. |

`context_schema` has exactly `{version:1, fields:{...}, persist_keys:[...]}`.
Each field has exactly `{required:boolean, max_bytes:integer, meaning:string}`.
There are at most 16 fields; `max_bytes` is 0..512; meaning is 1..256 UTF-8 bytes.
Keys use #43's descriptor grammar; `persist_keys` is a unique subset of declared
fields. Unknown schema/field keys refuse validation. Meaning is reviewed
semantics, not code capable of certifying secrecy.

The public context remains `unknown`; adapters explicitly validate and copy it.
No arrays, nesting, getters, `toJSON`, coercion or executable mapping. Undefined
means empty only if the schema permits. Inherit #43 exactly: at most 16 own
string keys, keys 1..64 UTF-8 bytes, values 0..512 bytes, depth one, compact
canonical JSON at most 4,096 bytes, and context key at most 512 bytes. Reject
NUL, lone surrogates and prototype-related invalid keys. Proposed context key
is lowercase hex SHA-256 of canonical bytes for indexing only: independently
compare the entire canonical descriptor, so collisions never merge different
contexts. Neither hash nor descriptor confers permission.

`persist_keys` requests a projection, never approves one. Without independently
reviewed safe stable field meanings, ordinary declarations are live-only even
if their text claims non-secret data. No results, credentials, private URLs,
edited text, capability handles or ephemeral component state may be persisted.

## 3. Protected local restore-policy proposal

**Approved governance:** a site administrator with existing `kernel.admin` may
review a minimal stable reopen projection through protected local deployment
policy. A central Plinth PR may supply reviewed evidence but is not the exclusive
route. Extension self-declaration, ordinary preferences and administrator handler
defaults cannot approve persistence.

**Prospective engineering facility, not existing configuration:** one explicitly
server-configured file supplies a bounded immutable kernel-owned snapshot at
startup/deployment restart. No runtime approval endpoint, browser/extension
write, automatic fetch, signing service, new grant or implicit live watcher.
A privileged operator deploys the administrator's attestation out-of-band; the
reviewer need not have operating-system write privileges.

The trusted deployment account owns a protected parent directory and regular
file, writable by neither extension/service identities nor untrusted groups;
the running service reads it only. Verify modes/ownership, no symlink/path
substitution, bounded bytes and stable file identity before activation. A
service-writable package/data location is forbidden. Extensions share the
service's OS identity; SQL roles are not OS isolation. Docker's `USER 10001`
and Helm's non-root/read-only-root settings are substrate, not proof of a
protected policy mount. If the deployment cannot preserve this boundary, leave
persistence unavailable and return the exact architecture question for review.

The proposed exact policy shape is:

```text
{format_version:1, policy_revision:"positive-uint64-decimal",
 review:{admin_user_id, reviewed_at, review_ref, evidence_sha256},
 entries:[{application_id, panel_id, capability,
           projection_contract:{id,version}, artifact_digest, schema_digest,
           fields:[{key,meaning}]}]}
```

Trust comes from protected deployment attestation, not a UUID or a check that
someone is an administrator. Before use the kernel also checks that reviewer
identity's current effective rules include `kernel.admin`, without impersonating
that user. Missing identity, lost eligibility, unavailable authority, malformed,
conflicting, stale or unprotected policy means unavailable/live-only. Reviewer
UUID stays server-side; browser data cannot expose it. Policy/evidence contains
no usernames, tokens, cookies, credentials, saved descriptors, raw contexts or
private URLs.

Proposed policy bounds: 1 MiB UTF-8, 256 entries, 16 KiB per entry, JSON depth
at most six and at most 16,384 nodes. Each entry has at most 16 unique approved
fields; key/meaning limits are as above. Reject unknown keys, raw duplicate keys,
invalid UTF-8, excess bounds and unsupported versions; do not truncate or retain
malformed subsets. Digests are 64 lowercase SHA-256 hex characters. Contract ID
uses the existing at-most-64-byte ID grammar; version is a positive safe integer.
Reviewer UUID is canonical, `reviewed_at` RFC3339 at most 32 ASCII bytes,
`review_ref` at most 256 UTF-8 bytes; revision is canonical positive uint64
decimal, at most 20 digits, without leading zeroes. Validation of already
decoded JSON cannot reconstruct duplicate raw keys erased upstream.

Recompute a full artifact digest over a canonical sorted immutable file inventory
(path, mode, exact bytes; excluding ZIP timestamps and mutable user data), not
`manifest_checksum`. Bind artifact, schema, field meanings, projection contract,
policy revision and current generation freshly before persistence or restore.
Package inventory/IO deadlines and immutable hash ownership must be source-
designed under existing package byte limits in #46; no unbounded traversal,
detached hashing or invented current inventory ceiling.

A fresh nullable browser `persistence_contract` projects only contract ID/version,
schema/artifact digests, approved keys and active policy revision. Intersect
approved keys with requested `persist_keys`. Coherent source binding, current
session, reviewer eligibility, RBAC and readiness are mandatory. Their changes
fence pending snapshots. Saved #43 envelopes remain unchanged: no URL,
generation or policy token is stored. Missing approval says “Not restored after
reload”; this is not backup of unsaved work. Policy changes become active through
explicit restart; no instantaneous file watching is promised. Future persistence
commit fencing, including internal guarded transport, needs source review:
generic key/value writes alone do not prove it; public SDK stays unchanged.

## 4. Typed authority transport proposals

All routes below are NEW proposals for #46 source review, not available APIs.
Preserve the schema-1 primary applications resource unchanged. SessionFilter's
existing session/PAT modes establish the original caller; fresh effective RBAC,
declaration, registered capability, package readiness and immutable asset-path
validation establish typed authority. No denied rows/counts/placeholders or
client-side RBAC. Responses use `no-store`, `Vary: Cookie, Authorization` and
production-redacted diagnostics, never raw contexts/exceptions.

| Proposed route | Contract |
| --- | --- |
| `GET /api/frontend/surfaces` | `kind=primary|float|tray`; optional canonical `content_type` for float; `limit=1..64`, opaque cursor. Primary means typed navigation receiver, not reused launcher rows. `include_admin_default=1` is allowed only with float plus content type; absent/0 performs NO administrator-storage lookup. |
| `POST /api/frontend/surfaces/admission` | Exact root `{schema_version:1,targets:[...]}`; at most 38 targets, 32 KiB request, 1 MiB response. Cookie mutation uses existing `X-Plinth-CSRF` and same-origin checks; preserve existing explicit bearer/PAT non-cookie policy. |

Admission has exactly two tuple variants:

```text
fresh:  {application_id,panel_id,kind,expected_generation:null,
         content_type /* float only */}
pinned: {application_id,panel_id,kind,expected_generation:"nonempty",
         capability /* float only */}
```

Fresh float lookup derives capability/generation from authoritative declaration;
caller supplies no capability or permission. Pinned revalidation checks exact
generation and registered capability, and omits lookup content type. Primary and
tray omit both float-only fields. Unknown/mixed variants and duplicate logical
app/panel/kind tuples refuse the whole request. Results are complete, exactly one
per input index, with `available`, `unavailable` or `stale`; only available carries
target metadata. Missing/duplicate indexes or unknown keys invalidate the response.
The exact proposed response is
`{schema_version:1,results:[{index,status,target?}]}`. Index is the input's
zero-based integer index. `target` is present exactly for `available`, never
null or present for `unavailable`/`stale`. Unknown envelope/result/target keys
refuse the response. This is stateless checking, not reservation, grant or
authorization lease.

Canonical target keys are `application_id`, `panel_id`, `type`, `generation`,
`version`, `title`, `icon`, `module_url`, plus the type-specific keys below.
Request `kind` is not a second target key: target `type` must equal request
`kind` and the logical input identity; a pinned available target must match the
expected generation/capability. The catalog uses the same target shape.

| Target type | Exact additional keys |
| --- | --- |
| Float | `capability`, `content_types`, `context_schema`, `logical_install_order`, `persistence_contract` (explicit null when no approved projection). |
| Primary navigation receiver | `accepts_navigation_intent` (true), `context_schema`. Non-opt-in primaries remain in the unchanged launcher, not this receiver catalog. |
| Tray | `tray_states`, `tray_initial_state`, `preferred_width`, `chrome_essential` (boolean derived from server-validated authoritative active-shell identity, never copied from a foreign claim). |

Omit other types' keys; declaration/schema fields retain section 2 meanings and
section 7 bounds. All base keys and the listed type-specific keys are required.
Module URL comes only from validated immutable ready assets, not a caller or
stored preference. Revalidate before import and through section 7's owner watch.

Catalog root is `{schema_version:1,kind,targets,next_cursor:null|string}`; pages
contain at most 64 targets/1 MiB, each target at most 16 KiB. Stop at either bound
with honest continuation, never truncated entries. Cursor binds user/filter/
sort position, expires after 60 seconds and fails on forgery/staleness/filter
changes. Its exact encoding is a future source choice. Pages are independently
fresh, not a global lease. Browser cache is at most 128 records/2 MiB; discard
earlier pages, deduplicate page IDs and never treat partial results as absence
of a known owner.

For float/content-type automatic resolution, the first fresh page is globally
ordered by eligible logical install rank, so its first target proves earliest
without scanning unbounded pages. Successful authoritative zero means missing;
failed/partial query means unavailable. Only the requested administrator flag
adds `admin_default_candidate:null|{application_id,panel_id,generation}` after
fresh authorization filtering. Null requires a successful storage read, not a
503. An authorized candidate outside page one is fetched by the fresh lookup
variant (null generation plus content type); never fabricate its capability or
use an incomplete pinned tuple. Subsequent checks use the returned pinned tuple.

## 5. Defaults storage and privileged dispatch proposal

Own defaults use existing own-user shell preferences with NEW per-key validation
for proposed `shell.handler_defaults`:

```text
{version:1,choices:[{content_type,application_id,panel_id}]}
```

At most 128 choices/32 KiB; reject duplicate canonical types, unknown keys,
versions and oversize. Invalid reads mean unavailable with a non-destructive
warning, not silently emptied or overwritten. The caller cannot supply user ID.

Prospective `/api/frontend/handler-defaults/admin` GET/PUT/DELETE requires fresh
`kernel.admin` for every management read/mutation. Cookie mutations use existing
CSRF. PUT is exactly `{schema_version:1,content_type,handler:{application_id,panel_id}}`;
DELETE omits handler. No caller-selected user scope. One mutation is at most
2 KiB; GET selects one content type or a cursor page of at most 64/128 KiB.
These are preferences, not permission grants or restore approvals.

Administrator data/migrations/SQL remain shell-owned under its existing extension
DB role. A fixed platform-only version-1 storage contract is proposed, NOT an
ordinary preference capability, generic privileged entry, caller-chosen module,
table, SQL or version. The kernel route owns identity, fresh management gate,
validation and audit, then invokes the fixed active-shell module under an owned
lease preserving the ORIGINAL caller, session, extension identity and bounded
incremented depth/MAX_CALL_DEPTH. No admin impersonation, reset depth, injected
effective rules or direct kernel SQL fallback.

Capture admission, module contract/storage version, artifact/current generation,
immutable handler code and runtime lease atomically under lifecycle admission.
An old pool cannot later read a new mutable active-path handler. Current dispatch
does not provide this complete binding. Owned args/code/pool live through
settlement; quiesce/drain cancels and joins before migration/disable/uninstall;
old frame acknowledgements are fenced. Missing/disabled/incompatible storage,
migration/generation races, unavailable pool or dispatch failure returns honest
503/409. Do not acknowledge uncertain mutation success or automatically retry it.
If this cannot be implemented within current authority/isolation, return the
specific architecture question rather than bypassing it.

A DISTINCT kernel-resolver-only read, `resolve_candidate(content_type)`, carries
the ordinary authenticated ORIGINAL caller/depth, reads at most one logical
candidate and lets the kernel freshly filter authority before browser projection.
It is not management GET, table exposure, an ordinary capability or a bypass of
management `kernel.admin`. Without it the approved three-tier algorithm is
blocked, not quietly changed to personal/earliest only. The same extension's
server/schema is trusted; this internal contract is not a sandbox against a
malicious module within that extension.

HTTP outcomes: 400 invalid; 401 terminal identity; 403 CSRF/management denial;
409 stale generation; 503 authority/storage/bounded dispatch unavailable;
200 only honest complete success. Do not expose denied handler metadata.

## 6. Resolution and explicit alternatives

**Approved automatic priority:** usable personal default, then usable
administrator default, then earliest-installed authorized handler. No automatic
ambiguity chooser. All choices concern the canonical literal content type and
logical application/panel identity; a default never grants authority.

1. Read own preference and freshly admit its logical float target. A usable
   personal choice returns WITHOUT catalog or administrator-storage reads.
   Affirmative absence/unusable choice advances; unknown preference or identity/
   authority failure stops unavailable.
2. Query the fresh authorized float catalog with administrator flag. A usable
   administrator candidate wins. Successful absence/unusable candidate advances;
   unavailable administrator storage stops automatic resolution, never silently
   chooses earliest.
3. Select the first globally ordered eligible target. No eligible target after
   a successful query means missing-handler; partial/failing authority means
   unavailable. Selected context/admission/import/readiness failure retains that
   handler's honest fallback, not a loop through other providers.

Keep stale preferences; skip affirmatively denied/unavailable choices with
generic status rather than silently rewriting them. A manual Open with or
change-default is a separate explicit intent with its independently authorized
catalog omitting the administrator flag, so alternatives remain available despite
administrator-storage failure. Never expose an automatic chooser as fallback.

Prospective durable `logical_install_order` is a canonical positive uint64
decimal string, not a lossy JavaScript number. Allocate at the first committed
logical handler install; retain across upgrade, disable/enable, restart and
verified rollback, independent of generation/registration timestamp. A new
handler gets a new ordinal; uninstall/reinstall gets a new logical order.
Tie-break by bytewise application then panel ID. Allocation, concurrency and
retention migrations need #46 source review; current timestamps do not certify
these semantics.

`openFloat(contentType, context)` validates type, resolves and builds the bounded
descriptor, freshly admits the selected generation, then calls the #44 mechanism's
admission/dedup/limit path. Resolve `{floatId}` only for a current ready owner;
missing, denied, unavailable, invalid, limit and load failure reject honestly.
The exact implementation error mapping needs source review, not silent no-op.
Same canonical context shares an existing owner; hash collision alone does not.
Minimized duplicates restore under #43 rules, and primary navigation never
destroys unrelated floats. `requestFocus()` remains void: for a current authorized
float it raises/restores via #43 focus rules; no primary switch or tray open is
implied, and stale/denied owners cannot steal focus.

## 7. Finite ownership and authority controllers

Numbers not labelled approved or inherited are engineering proposals, not
measurements or existing enforcement. Bytes mean compact encoded UTF-8 JSON;
refuse invalid input, never truncate strings.

| Resource | Bound and honest overflow/failure |
| --- | --- |
| Typed manifest | 256 KiB, 256 entries, 16 KiB per declaration; new v2 install/upgrade refuses excess without retrospectively replacing legacy rules. |
| Metadata | Existing ID grammar at most 64 ASCII bytes; generation/version at most 256 UTF-8 bytes, title 1,024 bytes, icon token 64 ASCII bytes, module URL 2,048 bytes; validated path/fresh authority still required. |
| Types/capability | At most 32 literal types per float, each at most 128 ASCII bytes; canonical case-folded ASCII type/subtype, no wildcards, parameters, whitespace guessing. Capability at most 256 UTF-8 bytes plus registry grammar/ownership. |
| Float owners/context | Inherit #43's five document-lifetime owners and all descriptor/persistence limits without modification. |
| Tray owners | **Approved hard 32 document-lifetime owners**, including bell/avatar, closed/hidden/loading/failed/retiring and old-frame pending preparation. No automatic eviction; extras stay OFF until explicit deactivation AND cleanup releases capacity. |
| Tray states/popover | **Approved at most 16 declared states and one open popover.** State/icon tokens at most 64 ASCII bytes; declared initial state. |
| Tray readiness | Proposed 15,000 ms from admission to import/factory/render readiness and, if open, first presentation activation. Closed ready owners activate only on later explicit presentation, not by reusing an expired loading token. |
| Authority controllers | One catalog controller and one known-owner batch controller; each one in-flight request plus one latest refresh, proposed ten-second deadline. Known-owner batch at most 38 = 32 trays + five floats + one current primary. |
| Interactive float admission | One request active plus at most five bounded distinct pending intents; share duplicates, refuse excess as request-busy (not falsely five-open). A newer request does not cancel an unrelated valid request. |
| Navigation | One active intent plus one replaceable latest intent per frame; one dirty confirmation globally. |
| Preference writes | One in-flight plus one latest pending snapshot per controller, 250-ms coalescing, revision/epoch fencing, no automatic I/O retry. |

The known-owner watch includes closed trays and minimized/loading/failed floats,
not opaque retired-frame cleanup tokens. Existing primary-cache reconciliation
remains separate. Refresh at frame readiness, before import/Retry/restore/
navigation/focus/tray presentation, explicit Refresh, existing
`applications.changed` hints, realtime-ready/reconnect and visibility return.
One owned poll timer is proposed: 30 seconds visible, 60 hidden; dispose when
empty/retired. Coalesce hints and polls; no overlapping/per-owner timers or
immediate failure cascades. Abort/fence requests on retirement.

Only a complete fresh indexed response is authoritative: unavailable retires an
affected owner without dirty veto; stale/new generation retires the old owner
before fresh admission; available updates evidence. Timeout, malformed, partial
or 503 means authority unavailable: deny new admission/presentation/navigation,
retain bounded ownership with generic stale/unavailable status, not invented
removal/resurrection. Independent capability calls remain kernel-gated.
Logout/terminal session loss immediately fences and cleans the frame. RBAC loss
without an exact event is detected on the next successful watch; no wall-clock
revocation guarantee exists offline, throttled or with failed requests. This
does not create #42 rights-stream or replay semantics.

## 8. Tray journey, persistence and recovery

A logical tray target reserves one document-lifetime slot BEFORE import. Its
owner has frame/generation/record/incarnation tokens and at most one preparation,
component/API/effects tree, fallback and cleanup. Readiness is loading/ready/
failed; popover open/closed is independent presentation. Closing a popover
retains its component, effects, callbacks, dirty state and icon updates. Open/
close fires presentation activation/deactivation, not destruction; opening
another requests dirty-aware close of the current popover first. Cancel keeps
the existing popover/owner/focus. All dirty current owners, including closed
trays, contribute to one page-level unload guard.

Explicit deactivation performs dirty Cancel/Discard, then fences, deactivates if
needed, unmounts, unbinds shortcuts/callbacks, removes DOM/references and releases
capacity only after owned preparation/cleanup settles. Forced logout, denial,
disable/uninstall or generation retirement has no dirty veto. Generation
replacement cleans old ownership before creating its replacement. Failures and
timeouts immediately fence the incarnation and retain a closeable bounded
fallback. Capture monotonic admission time and check elapsed time plus owner
tokens before import settlement can construct/mount, and after synchronous
factory/render/initial activation return can commit success. A delayed timer
cannot turn a post-15,000-ms result into readiness. This is event-loop deadline
checking, not native import cancellation or wall-clock JavaScript preemption.
Retry is explicit, fresh-authorized and once at a time; while old
non-abortable import remains pending it is disabled, not a second import behind
one reservation. Late settlement cannot construct or activate. Cleanup hooks
are invoked once without awaiting arbitrary extension promises.

Admit the two essential trays first in a fresh document, subject to outstanding
reservations. Logout/login cannot reset the hard 32 or conceal old pending
owners to load another pair. Retired tokens expose no previous user's target,
context or chrome. Browser module cache/arbitrary JS memory is not certified
by this record cap; synchronous hostile JavaScript is not timer-preemptible.
Cleanup failure or permanently stalled work needs a bounded error and explicit
real-document reload with unsaved-work warning, never an automatic manager/frame
reset pretending the realm was destroyed.

An authorized paginated tray manager exposes excess declarations as not running.
Explicitly deactivate one and finish cleanup before activating another; overflow
reorganizes existing owners, never hidden extra imports or silent truncation.
The exact prospective own preference key is `shell.trays`, with envelope
`{version:1,trays:[{application_id,panel_id}]}`. The array gives desired active
ordinary-tray order: at most 32 unique logical identities, existing ID grammar,
at most 256 UTF-8 bytes per item and 8,192 bytes aggregate. Exact known keys
only; reject duplicate raw keys wherever observable, without claiming decoded
JSON can recover erased duplicates. No module URL, generation, runtime token,
context, component/dirty memory or credentials. This is future per-key validation
under existing own-scope storage, not a currently validated key or API grant.

Stored selection/order is intent, never authorization or capacity. Fresh
essentials-first admission and outstanding document reservations still win;
extras do not auto-import behind the cap. Proposed deterministic initial
selection admits authorized essentials first, then the desired ordinary order
when a valid preference exists; otherwise normal authorized ordinary targets
in the typed catalog's bytewise application/panel order, stopping at available
capacity. Non-selected/overflow extras stay OFF in the manager; do not interpret
an invalid preference as permission to activate its entries or as a rule that
forbids every ordinary initial tray. An absent key is normal first use and uses
that legitimate bounded initial behavior without a warning. Malformed,
unknown-version or oversized values produce a generic non-destructive warning
and that bounded fallback, without automatic overwrite. A read outage, timeout
or 503 remains unavailable, not absence or valid empty selection: retain
legitimately admitted essentials/current owners, offer explicit Retry, do not
restore ordinary selection from the failed read or persist a replacement value.
Exact initial/default and
collection-position presentation remain prospective #46 source-review details;
essential pinned position is not changed by this ordinary array.

Restore is fresh-authorized; missing/denied targets do not execute. Read/restore
captures the current user-intent revision: accepted explicit deactivation or
other collection change fences remaining pending restore, so late reads/imports
cannot resurrect a closed owner. One ten-second-bounded preference attempt and
one explicit Retry at a time; writes use the section 7 250-ms, one-active/
one-latest controller with no automatic I/O retries. Failed writes retain UI
with “Layout not saved”. Old-user responses cannot update a new frame; admitted
server writes may still finish for their original owner. Independent tabs are
last successful server write, not invented compare-and-swap.

Essential status is server-validated ONLY for authoritative active-shell bell/
avatar identities; a foreign `chrome_essential:true` confers nothing. Exact
active-shell admission needs source review. Preserve an independent existing
authenticated-frame Sign Out even when tray import, preferences or RBAC fails;
it is not an extra imported tray owner or generic data/capability bypass.
Bell uses real tray infrastructure but says notification data unavailable until
#81; no fake zero unread count, read/dismiss action or substitute notification
bus. Ordinary API-installed test trays prove real event-driven state/badge
updates. Avatar preserves identity/theme light/dark/system and scale 80–175%;
settings navigation is unavailable unless a real authorized target exists.

`setTrayState` accepts only declared names, no markup/arbitrary SVG mutation.
`setTrayBadge` accepts null, `"dot"`, or non-negative safe integer; zero clears,
visual count saturates at `999+` with bounded accessible count description.
NaN, infinity, fractions and coercion refuse without changing prior state.
Retired/hidden/failed owners cannot gain focus or content shortcuts from updates.

## 9. Navigation, context timing and Jump

Public signatures remain exactly:

```ts
openFloat(contentType: string, context?: unknown): Promise<{ floatId: string }>;
navigate(target: string, context?: unknown): void;
onNavigationIntent(callback: (target: string, context: unknown) => void): void;
requestFocus(): void;
setTrayState(stateName: string): void;
setTrayBadge(value: number | "dot" | null): void;
getContext(): unknown;
```

No `openTray`, acknowledgement method or caller promise for navigation.
Navigation target is exactly `application_id:panel_id`, one colon, at most 129
ASCII bytes: application full-string `[a-z][a-z0-9-]{1,63}`, panel full-string
`[a-z][a-z0-9_-]{0,63}`. Reject all controls/space including trailing newline;
do not trim/coerce/URL-decode/percent-decode or reinterpret as MIME/resolver input.
The receiver gets ONLY local panel ID. Content-type resolution is `openFloat`,
not `navigate`. Explicitly typed opted-in primary admission is required; no
registered navigation handler means unavailable.

Prospective context-cell timing applies ONLY to explicitly v2 opted-in receivers
on this new navigation path. Existing initial launcher-primary raw references
and default `{}` are not retroactively cloned/frozen. Validate/copy a bounded
frozen owned snapshot before selection; dirty Cancel, failed admission or
stale-before-commit leaves old selection/context unchanged. For an existing ready
instance commit the new context cell immediately BEFORE its ordered handler
chain: callback argument and `getContext()` return the SAME new snapshot during
synchronous delivery; previously returned references are not mutated. A newly
navigation-created instance gets that snapshot at factory entry, then its
registered chain runs once after factory/render readiness and fresh fences.

First synchronous throw aborts remaining callbacks and reports failure; committed
selection/context and earlier side effects are NOT rolled back. IGNORE ALL
callback returns, including native Promises and thenables: do not await, inspect
`then`, coerce, attach reactions/rejection sinks/listeners or treat returns as
acknowledgement. Only ordered synchronous chain return without throw/current-
token loss confirms INTERNAL shell handoff; asynchronous domain work/later
rejection is outside that acknowledgement. Reentrancy or callback retirement
invalidates the token; serialized next intents cannot resurrect an old frame.
Asynchronous shell admission/import/readiness errors remain honest shell status,
not a new async receiver protocol or synchronous-void success claim.

Jump validates the corresponding authorized opted-in primary, commits readiness/
context and confirms the internal synchronous chain before requesting #43's
ordinary close. Missing, denied, stale, failed or cancelled navigation retains
the source float. Dirty-close Cancel after successful handoff retains the source
and states honestly that primary navigation succeeded. Unsupported Jump remains
disabled with explanation; no guessed primary, no dispatch-and-destroy.

## 10. Responsive and accessible presentation

Tray preferred width is proposed 280..480 effective CSS pixels, clamped to the
actual visual work area; height at most 70% with reachable scrolling controls.
Mobile uses a bottom sheet, not mandatory drag-only interaction. Resize, keyboard,
orientation, zoom and 80–175% shell scaling re-clamp presentation while retaining
the same owner/component, not refiring construction.

Native named buttons/list controls provide keyboard-only icon, overflow, manager,
ordering, position, default choice and activation/deactivation paths; reordering
cannot require drag. Essential items stay hard-pinned right of the separator,
not user-movable. Fit existing ordinary icons through accessible overflow rather
than losing active owners. First open focuses an eligible control; user close
returns to its connected authorized icon, otherwise an eligible manager/Home
control. Background events/readiness never steal focus. Use appropriate labelled
region/dialog semantics; arbitrary form content is not a blanket ARIA menu.

Exactly one active modal trap across tray, responsive float and dirty prompt.
The confirmation suspends underlying modal declarations/traps and panel shortcuts;
Cancel validates return focus, forced retirement recomputes current-frame fallback.
Escape first addresses geometry/editor/prompt, then dirty-aware popover close;
outside click cannot dismiss through a prompt or discard dirty state. Hidden,
minimized, failed, loading and retired owners receive no content shortcuts.
Verify 320-CSS-pixel width, text zoom, high contrast, visible focus, reduced
motion and at least 44-pixel targets where viewport permits. No outcome depends
on animation completion or inaccessible offscreen controls.

## 11. Required acceptance and handoff

This matrix specifies FUTURE evidence, not tests executed by paper delivery.
Independent #45 review must cover every cell, current/history reconciliation,
proposed wire shapes/authority, complete five-document envelope and links.
#44/#46 require actual backend and real-browser evidence with ordinary
API-installed caller/provider packages, task-owned kernel/database, real auth,
RBAC, lifecycle/versioned assets, durable realtime and full shell module graph.
Mocks supplement, never replace this route. Preserve applicable format/lint/
sanitizer/full PostgreSQL/WebSocket/CI gates; candidate and terminal exact-merge
CI remain required for each delivery.

| Family | Required independently observable cases / owner |
| --- | --- |
| Typed compatibility | Legacy absent marker remains primary; unknown metadata inert on read/restart; explicit v2 install/upgrade; malformed/unknown/duplicate raw keys and marker collisions; type/field/schema/MIME bounds; float/tray-only readiness on install/enable/recovery/restart; no primary-catalog authorization (#44 seam/#46). |
| Permission/lifetime | Session/PAT, implicit everyone/admin/ordinary allow/deny, fresh rule changes, unknown versus empty, no denied metadata, CSRF, batch exact variants/indexes/limits, current generation/module path, quiesce before migration, disable/uninstall/revoke, failed upgrade/recovery and late import/intents/preferences (#44/#46). |
| Policy/persistence | Protected file/parent identity, symlink/modes/shared OS identity; reviewer attestation versus eligibility, lost reviewer/DB unavailable, raw duplicates/unknown versions/bounds, full artifact versus manifest checksum, schema/meaning/key/projection/revision binding changes before write/restore, live-only unapproved data, fresh persisted descriptor, user intent fencing and separate users/tabs (#44/#46). |
| Default algorithm | Zero/one/many, personal wins without catalog/admin read, unusable/denied/stale defaults, administrator outside first page fresh lookup, administrator storage outage stops automatic but manual alternative works, earliest stable ordinal/tie/upgrade/rollback/reinstall, no chooser or silent write, selected-load failure not provider loop (#46). |
| Admin dispatch | Fresh management kernel.admin/CSRF and own-user isolation; fixed storage contract versus generic capability; ORIGINAL identity/depth/max; atomic code/storage/generation/pool capture, drain/migration/disable races, resolver-only one-candidate filtered read, incompatible/unavailable/uncertain outcome without SQL fallback or retry (#46). |
| Trays/chrome-essential | 32 boundary including both essential/closed/failed/loading/retiring/old-frame imports; extras OFF manager, explicit cleanup before replacement; 16 states/badge invalid/boundary; one popover and dirty Cancel, retained real event updates, repeated login cannot reset budget; foreign essential refused, independent Sign Out, unavailable bell data, state/order/position restore, failure/Retry/cleanup/reload (#46; notification data #81). |
| Navigation/context | Exact app:panel/local receiver, controls/trailing newline/MIME refused; opt-in/missing-handler; raw initial legacy unchanged; same-instance snapshot before chain/getContext identity; new factory readiness/once; dirty Cancel and stale-before-commit unchanged; first throw abort/no rollback; every return ignored including hostile thenable/native Promise without new observation; reentrancy/latest-intent fencing; internal handoff not async business acknowledgement (#46). |
| Float/focus/Jump | All #43 hard-five/mechanism criteria, dedup collision, close-cleanup boundary, minimized owner focus; real SDK resolution caller/provider plus approved persistent descriptor closes #44; unsupported/missing/denied/stale/failed Jump retains source, confirmed handoff precedes close, subsequent dirty Cancel retains source (#44/#46). |
| Authority watch/recovery | Exact known-owner indexed results versus partial pages, 38 bound, one timer/request/latest, hints/reconnect/visibility/poll, no online revocation guarantee on failed watch; unavailable denies new presentation without invented removal, no immediate retry cascade, forced retirement without dirty veto, owned stalled-work accounting/no automatic realm reset (#46). |
| Accessibility/responsive | Keyboard icon/manager/overflow/default/order/position, dirty modal precedence/one trap/return focus, Escape/outside click, no update focus theft or hidden shortcuts; bottom sheet/width-height clamps, 320px/text zoom/80–175%/high contrast/reduced motion, same tree across transforms (#46). |

If implementation needs a new grant, SDK method, authority bypass or incompatible
primary change, stop for the exact source-backed review question. Neither this
specification nor a mechanism fixture licenses that expansion or narrows away
the original end-to-end acceptance.
