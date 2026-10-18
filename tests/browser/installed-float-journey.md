# Installed float journey — scope and execution gates

`run-installed-floats.py --binary /exact/candidate/plinth` owns a disposable
database, two native kernel starts, browser descendants, fake accounts, API
installed packages, and one unique root-owned policy directory under `/run`.
`--bundle-dir` may select the exact candidate bundled shell directory. Never use
an older bundled shell. The kernel runs as a nonroot real/effective/saved/fs UID
with zero process capabilities and no-new-privileges. Root filesystem operations
use individual exact argv targets; nonroot runners require reviewed passwordless
sudo for those filesystem operations. The kernel/browser are never sudoed.

The first phase bootstraps/registers/logs in using the actual auth API/UI, installs
both ordinary ZIPs using the authenticated package API, checks provenance and no
default grants, and assigns exact declared fixture rules through owned groups to
two nonadministrator consumers. A third consumer is denied. No package/capture/
receipt/authority rows are fabricated. Stop and restart is intentional: reviewed
policy is startup-only, not a new runtime approval API. The supervisor independently
hashes the actual installed tree and schema and requires the kernel receipt to
match before installing the root-owned regular policy file.
The independent walker streams directory entries with a bounded lookahead,
checks path/count/metadata/cumulative stat sizes before reads or appends, and
caps each file read at its checked stat size plus one before detecting changes.

Verification requires actual captured document/response profile agreement, strict
authorized/denied surfaces, ordinary caller SDK `openFloat`, returned ready ID,
native static/dynamic imports from the captured provider graph and one shared
SDK/Preact/hooks module namespace. It exercises guarded acknowledged minimal
descriptor SQL storage, reload and re-login with a new component instance, no
unsaved-text reconstruction, separate-user isolation, required live-only field
exclusion, generic set/delete refusal, missing-CSRF refusal, denied pinned
admission/direct native-module access, and normal authenticated package uninstall.
Five additional groups exercise an installed component render failure and explicit
fresh-admitted Retry, revocation of only one consumer's existing capability grant,
ordinary authenticated disable/enable, a real failed-migration upgrade and a
successful later replacement. Per-instance activation/unmount witnesses require
cleanup exactly once; the caller primary and another float stay usable after the
component failure. Real SDK Request fixture focus or shell Focus actions trigger
fresh native authority checks; a prior genuine retirement is followed by a new
ordinary SDK refusal, not a fabricated realtime hint. Revocation adds no grant.

Rule revocation, disable and enable each pre-arm an observation of the current
acknowledged native WebSocket. Recovery requires its actual `auth_failed`, a
different socket created afterward, its application-channel subscription ACK,
and a new catalog request created after that ACK with HTTP 200, a captured body
and physical completion. The current Home must contain the authorized Caller
tile with an empty primary host before an explicit fresh Caller intent. This
preserves documented fail-closed primary destruction and never automatically
resurrects Caller. These observations share the original assertion deadline;
they do not retry a mutation or manufacture a subscription or realtime event.

The failed migration expects the actual HTTP 400 `upgrade-migration-failed`
contract and old ACTIVE-but-unready fenced generation, not rollback or automatic
recovery. Only the next deliberate ordinary upgrade can replace it. The new
generation must use its new captured native namespace and remain live-only:
the startup policy still names the original artifact and is never rewritten.
An explicit Restore refresh followed by an explicit Save verifies exclusion
without granting approval to the replacement. Three additional transport groups
exercise ordinary installed opted-in primary/tray catalog paging and cross-kind
cursor refusal, indexed mixed38 admission, real PAT issue/use/revocation, cookie
CSRF, denied module access, relative/dynamic captured imports, foreign essential
refusal and fresh declaration-rule revocation. Non-opted-in primary is excluded
from this typed receiver path and its typed module is refused. The existing real
disable and failed-migration actions also require primary/tray catalog/admission/
module denial; successful enable restores fresh metadata, and replacement refuses
the actual original captured URLs and generation. No extra lifecycle mutation,
deadline extension or retry is added. They do not execute navigation callbacks or tray
owners/chrome. These are fifteen named native groups, retaining all original
twelve assertions in their original relative order.
Only after kernel/browser/protected-file/database cleanup does the supervisor emit
its single success receipt. Failures are aggregated; timeout does not imply
rollback or permit a mutation retry. Fake credentials remain in owned mode600
state; only redacted private failure logs may be retained. One shared redactor
removes bootstrap/PG/fake account passwords from browser and kernel logs, including
early install failures before the phase state file exists.

Concrete dependencies before execution:

- Exact candidate binary and freshly generated captured bundled shell assets;
  ordinary package installation and immutable artifact receipt/profile1 capture.
- Main's coherent platform publication, protected-policy loading/admin-reviewer
  checks, authenticated surfaces/admission/native asset endpoints and guarded
  layout GET/PUT, lifecycle/drain hooks and generic-storage guard all integrated.
- Healthy task-owned PostgreSQL with test database creation and scoped cleanup;
  Linux setpriv/procfs, protected `/run` filesystem authority, Node/Playwright and
  actual browser dependencies. No default permissions are granted to satisfy this.
- Root-owned policy path ancestry and nonroot service identity must genuinely
  pass; unavailable sudo/capability/platform seams are failures, not skipped pass.

The `CI` workflow's existing GCC native browser step runs the nineteen harness
construction controls, the offline fixed-browser helper controls, and then this
actual journey against `build/plinth` and
`build/share/plinth/bundled`, after its other production browser journeys.
Pull-request and main-push runs must each supply fresh evidence for their actual
checkout; candidate success is not exact-merge success. The fifteen named journey
groups are separate from the construction controls, client units and CTest
inventory. This placement provides GCC/Linux native coverage only, not OCI,
ARM, ASan/UBSan or TSan journey coverage; those existing checks remain unchanged.

Only this installed-float journey uses the exact test-only Chrome for Testing
**157.0.8103.6 beta** selected by
`.github/scripts/run-installed-floats-fixed-browser.py`. Its official Linux64
archive is pinned to SHA256
`7802b25411aa5b8aa387cc733a45df6eed1aea8cc046fd4e2dec64d5918b1853`
(202676171 bytes). This checksum was measured from the downloaded official
artifact, not supplied as a vendor signature. The helper checks integrity before
bounded extraction and version execution, owns its temporary directory until
the native supervisor exits, and fails rather than selecting another browser.
Other browser journeys retain Playwright's bundled Chromium; this browser is
not shipped with Plinth and establishes no broader browser-support promise.

The pin contains Chromium's completed-stream cancellation fix
[`62473ad0f747e245e514c93e741bb8e168a2a207`](https://github.com/chromium/chromium/commit/62473ad0f747e245e514c93e741bb8e168a2a207),
associated with [Playwright issue #42742](https://github.com/microsoft/playwright/issues/42742).
The older browser can deliver a complete streamed `no-store` body to the page
while reporting `ERR_ABORTED` and denying the native body observer its data.
The pin repairs that test-observation boundary; it does not change application
caching, bounded streaming, payload limits, error assertions or deadlines. Local
native success is not candidate CI or exact-merge CI success; each still requires
a fresh complete run. Revisit the pin when a reviewed stable build includes the
fix, preserving those same gates.

This is a native-only harness. The container adapter preserves copied modes,
but lacks this protected read-only policy mount/privilege launch seam. Container
support therefore needs separately reviewed protected-policy/privilege-launch integration; this
does not claim an OCI artifact journey. Earlier seven-group receipts do not
establish the fifteen-group gate. Syntax/construction checks do not establish backend,
RBAC, browser, full float mechanism, recovery or operational restore acceptance.
Remaining wider matrix (e.g. five live/minimized limit, dirty close interactions,
in-flight install/retirement/policy races, process crashes/lost acknowledgement and
real image deployment) stays with the integrated feature gates; these fifteen journey
groups alone do not close either issue.
