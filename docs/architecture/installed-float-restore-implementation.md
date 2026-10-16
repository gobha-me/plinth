# Installed float/restore implementation plan

Status: implementation in progress for #44 and part of #46. This is not a
shipped-behavior contract. The approved specifications remain
[shell floats](../icd/ICD-shell-floats.md) and
[typed resolution](../icd/ICD-shell-trays-resolution-navigation.md).

## Delivery boundary

One functional delivery must demonstrate ordinary authenticated API installation
of a caller and provider, unchanged SDK `openFloat`, personal/administrator/
earliest-installed resolution, native panel import, an approved minimal stable
descriptor saved through guarded shell storage, and a clean reload/re-login
with user separation. Mechanism tests or an entirely live-only implementation
do not substitute for that journey. Close #44 only after its full acceptance;
#46 remains open for trays, navigation and the default-manager UI.

## Dependencies and ownership

1. Capture the entire exact installed version tree into bounded immutable
   owned buffers. Store a write-once generation/profile receipt; cache bytes
   evictably. Cache pressure is unavailable, not permission loss or removal.
2. Bind the authoritative bundled shell's fixed storage code, exact pool,
   captured artifact, migration checksum and contract marker coherently. This
   readiness is distinct from typed provider readiness: shell has no panels.
3. Render one captured document SDK/Preact/hooks/htm graph before modules run,
   with the exact trusted import-map text hashed into the existing strict CSP.
   Preserve legacy redirect, custom frontend and primary launcher behavior.
4. Admit typed native assets with fresh session, provider, panel AND capability
   authorization. Serve captured client members only. Shared response/transport
   callback owners retain the artifact, counted drain guard and process lease
   until actual source disposal, including abort. Cleanup notification alone
   is not unique completion; TLS copies are not source-ownership proof.
5. Load optional protected policy once before ingress, then freshly verify
   reviewer eligibility and full artifact/schema/meaning/projection binding
   before persistence or restore. Every required reopen field must survive
   approved/requested projection, otherwise the panel is live-only.
6. Use only kernel-selected, captured fixed shell storage code for layout and
   defaults. Preserve original caller/depth, shell DB role, registered existing
   read/write/admin gates, and acknowledged settlement. Generic `shell.floats`
   writes AND deletes must be refused at common dispatch and handler defense.
7. Wire bounded browser coordination to fresh typed admission, synchronous
   manager evidence, physical preparation/cleanup and preference intent/ACK
   fences. No duplicate SDK, saved generation/URL, automatic mutation retry,
   stale-user ACK, or database rollback/CAS claim.

All worker/capture/send/runtime owners must participate in production shutdown;
no detached work, timed-out-as-joined result or raw asynchronous owner pointer.
Use nonblocking package-name advisory ownership before a counted dispatch guard
to avoid waiting on a fence whose lifecycle operation is draining that guard.

## Selected internal transport

The public SDK is unchanged. `GET /api/frontend/float-layout` returns exactly
`{schema_version:1,present:boolean,layout?,policy_revision:null|string,
restore_targets:[{index,status,target?}]}`. Omit `layout` iff absent; stored null
is present malformed data. Bound stored SQL text before materialization at
32 KiB and the entire response at 128 KiB. An exact version-1/floats-array
envelope (at most five) yields complete original indexes, including unavailable
malformed/duplicate entries. Malformed root envelopes yield no restore targets.
Never shift indexes after normalization. Fresh ordinary pinned admission still
precedes import; these targets are ephemeral, not new saved fields.

`PUT` accepts exactly `{schema_version:1,layout,bindings}` (64 KiB), where each
of at most five entries has one unique binding `{index,generation,
artifact_digest,schema_digest,projection_contract:{id,version},policy_revision}`.
Freshly validate every binding before constructing the private native commit
token. Empty layout plus empty bindings resets through the same guard. Return
exactly `{schema_version:1,saved:true}` only after actual acknowledgement.
Unknown authority/storage is 503, never fabricated absence. An already admitted
write may finish for its original owner after later abort/revocation; fence stale
browser acknowledgements rather than promising retroactive rollback.

Use response-only `X-Plinth-Platform-Profile` for document compatibility; it is
not identity or permission. Typed provider URLs are
`/_plinth/typed/{generation_uuid}/{artifact_digest}/{panel_id}/client/{path}`;
the shared platform graph is
`/_plinth/platform/{generation_uuid}/{artifact_digest}/client/{path}`.
Components are canonical percent-encoded path segments; no mutable asset
fallback. Persistence metadata is null or exactly `{projection_contract:
{id,version},artifact_digest,schema_digest,approved_keys,policy_revision}`.

## Protected policy deployment profile

Configuration selects `packages.restore_policy_file` or
`PLINTH_RESTORE_POLICY_FILE`; the default is empty (no approval). This initial
Linux profile requires root-owned, non-symlink ancestors and one root-owned
regular file, no group/other writes or set-id bits, and one file hardlink.
The service must have non-root real/effective/saved/filesystem UID, no permitted,
effective or inheritable capabilities, and `no_new_privs=1`. ACL write masks
therefore cannot grant service/group writes. Deployment operators must actually
preserve that boundary; a service-owned chmod 0444 file is not trusted.

Use a root-owned protected bind mount/directory and non-root service identity.
Docker requires `--security-opt=no-new-privileges` and dropped capabilities;
Kubernetes requires the existing non-root/read-only/no-escalation settings AND
a protected compatible mount. Symlink-based ConfigMap projections do not meet
this no-symlink profile: materialize reviewed bytes into a protected mount
out-of-band. No runtime approval endpoint, same-owner fallback or file watcher.

The loader opens components descriptor-relatively with `NOFOLLOW`, checks
bounded exact bytes, EOF and stable descriptor/path identity, and owns the
parsed snapshot. This is not reviewer authorization. Empty configuration is
affirmative absence and keeps persistence live-only. A configured invalid,
unavailable or unprotected policy fails startup, never silently becomes absence.
Fresh reviewer/database uncertainty remains unavailable. Regular-file syscalls
can block despite `NONBLOCK`;
byte/depth bounds are not hard I/O latency guarantees.

Operational allocation, length or hashing failure is not affirmative policy
absence or semantic context rejection. Projection boundaries propagate it as a
whole-request unavailable error; restore keeps that distinction through saved
descriptor normalization. Request depth/node limits remain malformed input,
distinct from resource failure. Neither case authorizes an automatic rewrite.

## Required evidence before activation/delivery

Focused tests first, then full disposable PostgreSQL/WebSocket/lifecycle and
native-browser installed-app journeys, new-owner ASan/UBSan and feasible TSan,
Clang 21 formatting/fatal lint, independent review/fixes, candidate CI, normal
guarded merge and fresh exact-merge CI. The protected-policy fixture must use
actual root-owned files and a distinct unprivileged service, not synthetic trust
metadata. No old test receipt proves new capture/stream/storage lifetimes. No
release, advisory publication, grant or repository-setting change is included.

## Current local integration boundary

The local candidate now wires the registry, protected-policy snapshot, native
`FloatLayoutService`, physical asset streams and HTTP facade from `main`.
This is source integration, not completed native-browser or delivery evidence.
The service uses one persistent metadata thread and a fixed FIFO bounded at
64 preparing/queued/executing jobs. Separate service credits remain occupied
through late native descendants and stream callback disposal; queue wait counts
against the original ten-second deadline. Budget pressure returns unavailable.

Under the existing nonwaiting package-name session lock it refreshes credentials,
both panel/capability authority, exact pool stamp, full artifact/registration
binding and durable receipt before counted admission. Shell compatibility guards
retain the original profile through synchronous response-header construction.
Public prelogin platform files are restricted to the generated captured bundled
client graph; typed provider URLs require fresh session/RBAC checks on every
request. Existing legacy `/ext/*` static visibility is unchanged; no client
source-code secrecy or new permission grant is claimed.
No borrowed PG connection or raw service pointer crosses the loop handoff.

Guarded writes acquire all unique names in stable order before provider guards,
then validate actual policy/reviewer, generation/schema/projection and minimal
context. Only this service constructs the opaque commit token. The same owned
request persists through native database settlement, and success requires its
actual ACK. Reads preserve original saved values and indexes while independently
deriving restore eligibility. Neither path falls back to kernel preference SQL.

Captured sends retain a separate physical owner with no request/response
backedge. Stop closes their weak connections outside locks; drain waits for the
last physical callback copy. Production closes and joins these owners through
the existing coordinator before process async/runtime/database/loop teardown.

Logical JavaScript error delivery is not physical database completion. Focused
real-PostgreSQL tests hold a marker-table lock, observe the actual blocked SQL,
allow the driver's logical timeout, and require both name/service owners to
remain counted until lock release and actual settlement. Another case fences
the name while the admitted request waits behind an owned main-loop barrier;
the original request must complete and drain. A stopped-before-loop variant
keeps the SQL marker lock held while releasing the loop barrier and requires
physical drain without starting late SQL. Mixed full-artifact captures at
the same UUID/version and a missing durable receipt are refusal cases. These
read tests do not establish guarded writes, COMMIT/lost-ACK outcomes, native
browser acceptance or the separate JavaScript wall-clock abandonment route.

The metadata worker's absolute PostgreSQL deadline does not renew per query.
Cancellation alone has a private two-second control-connection budget to send
and confirm cancellation after the main deadline has expired. This does not
admit another ordinary query, renew the expired parent or imply rollback.

`InstalledGenerationRegistry` is the in-progress process-owned composition of
capture, full registered-declaration binding, write-once receipts and evictable
bytes. It schedules no background work and has one nonwaiting physical capture
lane. Its staging operation uses the caller's transaction; it never commits or
publishes that transaction. Publication and current reads require an idle,
same-name session, fresh ACTIVE state and matching durable receipt. Lifecycle
activation/recovery stage receipts in the first ACTIVE transaction and publish
only after acknowledged COMMIT; disable/uninstall/GC retire exact UUID cache
membership without revoking existing byte leases. Startup bootstraps only the
selected bundled shell; a first existing-v2 request has an explicit same-name
counted transaction, not an implicit receipt mutation in registry reads.
Lost COMMIT ACK remains fenced and is not established by post-ACK failure tests.

The conditional browser path is likewise not native deployment evidence.
Focused controls now include terminal session/profile responses received after
a logical timeout, retired-frame isolation, and missing-policy refusal without
automatically saving an empty layout. The server must independently enforce
those policy/write conditions. A controlled DOM/transport test cannot replace
the ordinary installed-app, real-browser, guarded save/reload journey above.
