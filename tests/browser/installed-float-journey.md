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

The failed migration expects the actual HTTP 400 `upgrade-migration-failed`
contract and old ACTIVE-but-unready fenced generation, not rollback or automatic
recovery. Only the next deliberate ordinary upgrade can replace it. The new
generation must use its new captured native namespace and remain live-only:
the startup policy still names the original artifact and is never rewritten.
An explicit Restore refresh followed by an explicit Save verifies exclusion
without granting approval to the replacement. These are twelve named native
groups, retaining the original seven assertions in their original relative order.
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

The `CI` workflow's existing GCC native browser step runs the eleven harness
construction controls and then this actual journey against `build/plinth` and
`build/share/plinth/bundled`, after its other production browser journeys.
Pull-request and main-push runs must each supply fresh evidence for their actual
checkout; candidate success is not exact-merge success. The twelve named journey
groups are separate from the construction controls, client units and CTest
inventory. This placement provides GCC/Linux native coverage only, not OCI,
ARM, ASan/UBSan or TSan journey coverage; those existing checks remain unchanged.

This is a native-only harness. Existing container staging rewrites copied modes
and lacks this protected read-only policy mount/privilege launch seam. Container
support therefore needs a separately reviewed exact-mode-preserving adapter; this
does not claim an OCI artifact journey. New twelve-group assertions are unexecuted
until the owner runs this changed harness; earlier seven-group receipts do not
transfer. Syntax/construction checks do not establish backend,
RBAC, browser, full float mechanism, recovery or operational restore acceptance.
Remaining wider matrix (e.g. five live/minimized limit, dirty close interactions,
in-flight install/retirement/policy races, process crashes/lost acknowledgement and
real image deployment) stays with the integrated feature gates; these twelve journey
groups alone do not close either issue.
