# ICD-0.6.1-shell-schema-user-preferences

**Traces to:** ICD-0.6.0 §15 *Bundled-package first-boot install
lifecycle* (lines 1010–1020 — "**Closes: 0.6.1** per
`DESIGN-shell-v06x.md §9.1`. The 0.6.0 static-handler approach (§8)
is the simplest conforming form per METHODOLOGY *Caller-Triggered
Implementation*; the 0.6.1 lifecycle replaces the handler bytes-source
from 'embedded resource' to 'package-system extraction' while keeping
the URL contract identical." — this ICD authors the package-system
extraction path); ICD-0.6.0 §15 *`frontend.mount` manifest contract*
(lines 1022–1030 — "**Closes: 0.6.1** when the shell's manifest lands
through the install lifecycle. 0.6.0 hardcodes the equivalent of
`mount='/app'` at the kernel layer; this works because there is
exactly one frontend in 0.6.0 and the active-frontend singleton from
`architecture/06-frontend.md §2.2` reduces to a no-op." — this ICD
authors the manifest-driven mount declaration); ICD-0.6.0 §15
*`ext_shell` PG schema and user preferences* (lines 1032–1040 —
"**Closes: 0.6.1** per the §9.1 exit criterion ('preferences round-
trip'). 0.6.0 ships zero persisted user state — every reload returns
to the login form; theme is system-default, scale is 100%, no per-
user customizations exist yet." — this ICD authors the schema, the
table, and the round-trip surface); ICD-0.6.0 §8.3 *Assets storage*
(line 624 — "Per **OQ1**, the recommended storage form is embedded
resources baked into the kernel binary…" — architect overrode at code-
session ship per `project_next_session_post_060.md` to **on-disk
installed shell**; this ICD normalizes that override into the install
contract); ICD-0.6.0 §8.1 *Drogon registration* (lines 581–593 — the
`/app/{path:.*}` SPA-fallback handler that this ICD's manifest-driven
dispatch replaces while preserving the URL contract verbatim); ICD-0.6.0
§7 *Top-level error boundary* (preserved unchanged — the boundary's
componentDidCatch payload, fallback render, and `?force-throw=1` test
seam all carry forward into the installed shell);
DESIGN-shell-v06x.md §0.6.1 (lines 710–719 — the canonical milestone
definition: shell installs through the standard package lifecycle on
first boot; preferences round-trip; entry = 0.4.4; exit = "preferences
round-trip (write, reload, read back)" — this ICD pins the contracts
that satisfy that exit criterion);
DESIGN-shell-v06x.md §3.7 *Shell as an Extension: Own Data*
(lines 359–384 — the canonical model: `ext_shell.user_preferences`
keyed on `(user_id, key)` with JSONB value; `ext_shell.default_apps`
keyed on `(scope, user_id, content_type)` deferred to 0.6.6; user-
deletion cleanup as `DELETE WHERE user_id = $1`); architecture/06-frontend.md
§1 *The Shell is an Extension* (the ICD-0.6.0 traces-to footnote that
"first boot bundled-package install lifecycle is *deferred* to 0.6.1"
— this ICD discharges that footnote); architecture/06-frontend.md §2
*`frontend.mount` URL ownership* (the ICD-0.6.0 §15 deferral
specifically — manifest-driven mount declaration belongs here);
architecture/06-frontend.md §2.2 *Active-frontend singleton* (the
single-frontend invariant the kernel resolves on every shell-asset
request — already a no-op in 0.6.0 because exactly one frontend
exists; this ICD pins the resolver shape so multi-frontend extension
becomes a 0.6.4 follow-up rather than a re-litigation);
architecture/05-extensions.md §2 *Reserved URL Prefixes* (the
`/app/*` row preserved verbatim — this ICD's manifest-driven dispatch
claims the prefix through the active-frontend `frontend.mount` value
rather than ICD-0.6.0's hardcoded literal; the `/api/*`, `/ext/*`,
and `/ws` rows are unchanged); ICD-0.4.3-extension-schema-creation-and-migration
§Schema + GRANT Contract (lines 61–91 — `CREATE SCHEMA ext_{name}` +
`ext_{name}_role` + `GRANT USAGE, CREATE ON SCHEMA` template; this
ICD splices `name='shell'` into that template verbatim with one
addition: a `plinth.users` GRANT extension for the FK), ICD-0.4.3
§Migration Execution Contract (the per-file transactional apply +
`plinth.migrations` checksum tracking — this ICD lists the shell's
own migration file, `migrations/001_init.sql`, that ships in the
shell.zip and runs exactly once at MIGRATING stage on first install);
ICD-0.4.4-package-install-lifecycle §State Machine (the
UPLOADING → VALIDATING → MIGRATING → REGISTERING → EXTRACTING →
ACTIVATING → ACTIVE state machine the bundled shell traverses
identically to user-uploaded packages — this ICD's first-boot path
constructs an `InstallerContext` with `provenance='bundled'` and
hands off to the existing orchestrator at `install_lifecycle.cpp`);
ICD-0.4.4 §Audit Events (the install-lifecycle audit family the
shell's first-boot install rides into — no new audits for the
install path itself; this ICD only adds first-boot-detection and
preference-roundtrip audits);
ICD-0.4.6-rbac-rule-registration (the capabilities.json shape +
RBAC rule registration the shell uses for `shell.preferences.read`
and `shell.preferences.write` — same as any user-uploaded extension;
this ICD's only addition is two specific rule names);
ICD-0.5.0.3-extension-dispatch §Tier 2 dispatch (the extension-JS
execution path the shell's `shell.preferences.get` /
`shell.preferences.set` capabilities flow through — same as any
extension-provided capability);
DESIGN-packages-v04x.md §0.4.4 *First-boot bundled-package install*
(the design-side foundation for first-boot detection and bundled
install — this ICD splices that section into a normative kernel
contract);
`docs/sketches/shell-design-2026-04-27/` (the canonical
visual reference for the 0.6.x arc; the shell's own data
`(user_id, key, value JSONB)` shape is consumed by every visible
preference UI in the design bundle — theme toggle in the avatar
popdown, scale slider, app-switcher tab order, tray collection
position).

**Depends on:** ICD-0.6.0 (the prior milestone whose static-handler
this ICD's manifest-driven dispatch replaces; the four §15 deferral
pointers this ICD discharges; the `Config::Shell { enabled,
root_redirect }` block this ICD extends with `bundle_path`);
ICD-0.4.3 (schema + role + GRANT contract reused verbatim); ICD-0.4.4
(install lifecycle the bundled shell rides into — first-boot
detection layers on top of the existing orchestrator without
forking the state machine); ICD-0.4.6 (RBAC rule registration —
`shell.preferences.read` / `shell.preferences.write` registered
through the same `capabilities.json` rule path as any extension);
ICD-0.4.1-manifest-parsing (the manifest schema this ICD extends
with the `frontend.mount` + `frontend.entry` field validation);
ICD-0.5.0.3-extension-dispatch (the Tier 2 dispatch path
`shell.preferences.get` and `shell.preferences.set` flow through —
no new dispatch surface needed); ICD-0.1.5-rbac-enforcement (the
filter chain the get/set capabilities pass through — `plinth.user.id`
binding from the session cookie is the same as any other
RBAC-gated capability call); ICD-0.1.7-audit (the audit writer
infrastructure the new rate-limited `shell.preferences.*` and
`shell.firstboot.*` audits ride).

**Milestone:** 0.6.1 — Shell Schema and User Preferences. Second
0.6.x code milestone (after v0.6.0 + 0.6.0.1 atexit-shutdown fix).
Authored as paper-only follow-up `0.6.0.N ICD-0.6.1 authoring` per
METHODOLOGY-llm-assisted-development.md §3.1 forward-ICD-presence
rule and `feedback_icd_horizon.md` (ICDs one milestone ahead). The
piece that closes ICD-0.6.0 §15's four explicit deferral pointers
queued at 2026-04-27 ship.

**Status:** Paper. Authored 2026-04-29 on
`feat/0.6.0.N-icd-0.6.1-authoring`. Code session pins OQ1–OQ7 then
implements; expected 4–5 phase commit arc.

**Methodology:** LLM-Assisted Development (METHODOLOGY-llm-assisted-development.md)

**Related:**
- `docs/architecture/06-frontend.md` (the contract owner; §1 first-
  boot install + §2 mount declaration + §2.2 active-frontend
  singleton are the substrate this ICD's 0.6.1 slice carves out
  from. Architecture promotions land in the eventual 0.6.1 *code*
  session, not in this paper-only PR).
- `docs/design/DESIGN-shell-v06x.md` (the 0.6.x shell arc; §0.6.1
  is the milestone definition; §3.7 the canonical data-ownership
  model; §6.1 / §6.2 the consumer surfaces persisted to
  `user_preferences`).
- `docs/design/DESIGN-packages-v04x.md §0.4.4` (first-boot bundled-
  package install — the design-side foundation this ICD splices
  into a normative install contract).
- `docs/icd/ICD-0.6.0-frontend-shell-bootstrap.md` (the prior
  milestone whose §8 static-handler this ICD decommissions; §15
  deferrals lines 1010–1040 are the discharge anchors).
- `docs/icd/ICD-0.4.3-extension-schema-creation-and-migration.md`
  §Schema + GRANT Contract + §Migration Execution Contract (reused
  verbatim for `ext_shell`).
- `docs/icd/ICD-0.4.4-package-install-lifecycle.md` (the install
  state machine the bundled shell rides into; first-boot detection
  is a thin pre-flight that hands off to the existing
  `InstallerContext` flow).
- `docs/icd/ICD-0.4.6-rbac-rule-registration.md` (the rule
  registration path `shell.preferences.read` /
  `shell.preferences.write` use).
- `docs/icd/ICD-0.5.0.3-extension-dispatch.md` (Tier 2 dispatch
  path the get/set capabilities flow through; no kernel-side
  capability registration needed — the shell's `capabilities.json`
  carries the declarations same as any extension).
- `docs/icd/ICD-0.5.5-sequence-numbers-client-debounce.md` (the
  most-recent paper-authored ICD; format reference for §-numbering,
  Glossary, test-case prefix convention).
- `docs/sketches/shell-design-2026-04-27/` (the canonical
  visual reference; preference-driven surfaces the get/set
  capabilities serve).
- [src/kernel/shell/static_handler.cpp](../../src/kernel/shell/static_handler.cpp)
  (what gets decommissioned in the 0.6.1 code session — this ICD's
  manifest-driven dispatch replaces the hardcoded `/app/*` handler
  with `register_active_frontend_routes(cfg, db, data_dir)`).
- [client/shell/manifest.json](../../client/shell/manifest.json)
  (the current `frontend.mount: "/app"` field — this ICD makes the
  field load-bearing and the kernel reads it at boot).
- [src/kernel/packages/install_lifecycle.cpp](../../src/kernel/packages/install_lifecycle.cpp)
  (the install orchestrator the bundled-shell first-boot path hands
  off to — no state-machine change, only a thin pre-flight caller).
- [src/kernel/config.hpp](../../src/kernel/config.hpp)
  (the `Config::Shell` block this ICD extends with `bundle_path`).

---

## Overview

ICD-0.6.1 closes the four explicit `Closes: 0.6.1` deferral pointers
queued in ICD-0.6.0 §15. The 0.6.0 milestone (shipped 2026-04-27)
landed the in-browser frame as a kernel-baked static handler;
0.6.1 makes the shell self-deliver as a regular installed extension,
unlocks per-user state, and pins the manifest-driven mount contract
that future bundled or user-installed frontends will use.

Three things land:

1. **Bundled-shell first-boot install lifecycle.** On every kernel
   boot the orchestrator queries `plinth.packages` for an active
   `provenance='bundled'` row at `name='shell'`; absent, the kernel
   reads `<bundle_path>/shell.zip` from disk, hands the bytes to
   the existing 0.4.4 `InstallerContext` flow, and exits the boot
   handshake only after the install reaches `state='ACTIVE'`. The
   ICD-0.6.0 §8 static-handler stays in the binary as a defensive
   fallback for the empty-`plinth.packages` window during the very
   first boot, then is decommissioned in favour of the manifest-
   driven dispatch wired in by §4.

2. **`frontend.mount` manifest-driven mount declaration.** The
   manifest field that ICD-0.6.0 §Appendix B sketched as forward-
   compat becomes load-bearing: extensions whose `frontend.mount`
   resolves to a reserved-prefix-conforming URL get a kernel-
   registered SPA-fallback route at that prefix; the active-
   frontend resolver picks one mount per kernel instance (the
   `architecture/06-frontend.md §2.2` singleton — multi-frontend
   simultaneity deferred to 0.6.4); the asset-resolution path
   reads bytes from `<data_dir>/extensions/<name>/<version>/client/`
   exactly as the bundled shell installs them. The wire contract
   `/app/(.*)` is preserved verbatim — the same URL serves the
   same files, only the byte source moved from kernel `.text` to
   on-disk extension files.

3. **`ext_shell` PG schema + `ext_shell.user_preferences` table +
   get/set capability pattern.** The shell's own PG schema lands
   through the standard ICD-0.4.3 path on its first
   `run_migrations()` call. The schema's first table,
   `ext_shell.user_preferences (user_id, key, value, updated_at)`,
   becomes the persistence target for theme, scale, tab order,
   and any other per-user state the shell manages from 0.6.2
   onward. Two new capabilities — `shell.preferences.get(key)` and
   `shell.preferences.set(key, value)` — provide the round-trip;
   they ride the existing Tier 2 extension-JS dispatch path
   (ICD-0.5.0.3) and gate on two new RBAC rules
   (`shell.preferences.read` / `shell.preferences.write`)
   registered through the standard ICD-0.4.6 rule-registration
   path. `ext_shell.default_apps` is forward-stubbed in the
   schema-reservation block but its DDL belongs to 0.6.6.

The boundary stays narrow on purpose. 0.6.1 does not touch design
tokens (0.6.2), the panel SDK (0.6.3), tabs/launcher (0.6.4), or
floats (0.6.5). The shell's `index.html` + `shell.js` content stays
byte-identical to ICD-0.6.0's 0.6.0 ship; only the byte source
changes from kernel-baked to on-disk-installed.

**Out of scope (deferred):**

- **Design tokens, theme, UI scaling.** Out of scope; 0.6.2 per
  `DESIGN-shell-v06x.md §9.2`. ICD-0.6.2 will land the `:root`
  custom-property contract and hook theme toggles into the
  `user_preferences` round-trip this ICD pins.
- **Panel SDK + client SDK.** Out of scope; 0.6.3 per
  `DESIGN-shell-v06x.md §9.3`. ICD-0.6.3 will wrap
  `shell.preferences.get` / `set` with a `plinth.preferences.get`
  / `plinth.preferences.set` client wrapper for downstream
  extensions; the kernel-side capabilities this ICD pins are the
  substrate.
- **`ext_shell.default_apps` table.** Out of scope; 0.6.6 per
  `DESIGN-shell-v06x.md §9.6`. The `(scope, user_id, content_type)`
  PK shape is documented in §5.4 as a reservation block so future
  extensions plant `default_apps` rows in a stable location, but
  the DDL itself ships with the tray/content-type-resolution
  milestone.
- **Tabs / app-switcher / Home launcher.** Out of scope; 0.6.4 per
  `DESIGN-shell-v06x.md §9.4`. Tab ordering will persist to
  `user_preferences` via the round-trip this ICD pins; storage
  shape is reserved (key prefix `topbar.tab_order`) but no
  shell-side reads/writes happen in 0.6.1.
- **Floats / tray system / responsive transforms.** Out of scope;
  0.6.5 / 0.6.6.
- **Multi-frontend simultaneity.** ICD-0.6.1 pins the active-
  frontend singleton (one frontend mount per kernel instance);
  multi-frontend simultaneity (e.g. an admin extension alongside
  the user shell) is `DESIGN-shell-v06x.md §11` OQ-deferred to
  0.6.4 when the panels query API lands. The `frontend.mount`
  contract here is forward-compat for that case — multiple
  installable frontends can carry the field; only one is "active"
  at a time per `architecture/06-frontend.md §2.2`.
- **Dynamic active-frontend swap.** ICD-0.6.1 resolves the active
  frontend at boot only. Hot-swapping the mount during a running
  process is **not** in scope — it would require route un-
  registration which Drogon does not support. Admin-driven swap
  (uninstall current frontend, install new, restart kernel) is
  the supported workflow.
- **Bundled-shell upgrade path.** ICD-0.6.1 pins the *first-boot*
  install. Upgrading the bundled shell to a new version is the
  same as any extension upgrade per ICD-0.4.5 §Upgrade Contract —
  drop in a new shell.zip on disk, kernel runs the upgrade
  lifecycle on next boot. The first-boot detection skips because
  the active-row predicate already matches.
- **Headless-browser harness for `M.*` mount routing cases.** Out
  of scope; deferred to 0.6.0.N test-fixture buildout's
  browser-harness slot per ICD-0.6.0 OQ2 — same posture as the
  9 deferred 0.6.0 cases. The HTTP-fixture (`HttpTestFixture` from
  0.6.0.N session 2) is sufficient for all `M.*` cases this ICD
  enumerates; browser cases are absorbed under 0.6.3 panel-SDK
  testing or earlier if the harness lands.
- **Capability batch / silent / search_path semantics.** Inherited
  unchanged from ICD-0.5.3 — `shell.preferences.set` interacts with
  `db.batch` and `silent` exactly like any other extension write;
  this ICD adds no new semantics there.

---

## Glossary

- **Bundled extension.** A package whose `provenance='bundled'`
  row in `plinth.packages` is created by the kernel itself at
  first boot from on-disk shell.zip bytes, not by an admin upload
  through `POST /api/packages`. Bundled extensions traverse the
  identical 0.4.4 install state machine as user-uploaded packages
  — the `provenance` column distinguishes the *origin*, not the
  install path.
- **Active frontend.** The one row in `plinth.packages` for which
  `provenance='bundled' OR provenance='admin'`,
  `frontend_mount IS NOT NULL`, `state IN ('ACTIVE','ACTIVE_FLAGGED')`
  is true at kernel boot. Resolution is single-row by design per
  `architecture/06-frontend.md §2.2`; multi-row is a kernel boot
  error (`ERR_MULTIPLE_ACTIVE_FRONTENDS`) until 0.6.4 lifts the
  singleton.
- **First-boot.** The kernel-boot iteration on which the
  active-frontend query returns zero rows. After the bundled
  shell install completes, the row exists and subsequent boots
  short-circuit. First-boot is not the same as a fresh PG
  database — a kernel-binary upgrade against a populated PG is
  *not* first-boot if a shell row already exists.
- **`ext_shell`.** The PG schema reserved for the bundled shell
  extension, identical in shape to any other `ext_<name>` schema
  per ICD-0.4.3 §Schema + GRANT Contract. The kernel reserves the
  literal `name='shell'` for the bundled extension; user-uploaded
  packages with that name are rejected at validation time
  (§5.5).
- **`shell.zip`.** The on-disk bundled-shell artifact at
  `<bundle_path>/shell.zip`. Bytes match exactly what the 0.4.4
  install lifecycle expects from a `POST /api/packages` upload —
  the same `manifest.json`, `capabilities.json`, `migrations/`,
  `client/`, `server/` tree, validated through the same 0.4.0
  structural pre-flight.
- **`bundle_path`.** New `Config::Shell` field added in 0.6.1.
  Default `share/plinth/bundled` relative to the kernel binary's
  install root; resolved to absolute at config load. Per OQ5,
  may be made configurable; default is normative.
- **Preference round-trip.** The `DESIGN-shell-v06x.md §0.6.1`
  exit criterion: a client (the shell's own `shell.js` or any
  Tier 2 extension) calls `shell.preferences.set(key, value)`,
  reloads the kernel process, calls
  `shell.preferences.get(key)`, and receives the byte-identical
  JSONB value back. The two capabilities are atomic and ride the
  same RBAC + audit + dispatch path as any extension call.
- **Active-frontend singleton invariant.** Exactly one row in
  `plinth.packages` matches the active-frontend predicate at
  any time. Enforced at install time via a partial unique index
  on `frontend_mount` filtered by the active-state predicate;
  enforced at boot via a `LIMIT 2` query that errors on row 2.
  Deferred relaxation to 0.6.4 when multi-frontend mounts land.
- **`provenance`.** The existing `plinth.packages.provenance`
  column from ICD-0.4.4 §Data Model. Values: `'admin'` (uploaded
  through `POST /api/packages`), `'bundled'` (kernel-installed at
  first boot — this ICD adds the writer), reserved
  `'sidecar'` (0.8.x). The shell row carries `'bundled'` for the
  lifetime of the install (no migration to `'admin'` on upgrade).
- **`frontend.mount` / `frontend.entry`.** Two new manifest fields
  this ICD pins. `frontend.mount` is a URL-prefix string (e.g.
  `/app/`); `frontend.entry` is a file path under `client/`
  (e.g. `index.html`) used as the SPA fallback. Validation
  contract in §4.2.
- **Mount-prefix conflict.** Install-time rejection of any package
  whose `frontend.mount` overlaps a reserved kernel prefix
  (`/api/*`, `/ext/*`, `/ws`) or another active frontend's mount.
  Yields `ERR_MOUNT_CONFLICT` from the install lifecycle's
  VALIDATING stage; fix is to choose a different mount.
- **Preference key.** A non-empty UTF-8 string under the per-user
  scope. Maximum length 255 bytes. Format-free at the kernel layer
  — extensions own their own conventions (e.g. shell uses
  `theme.mode`, `topbar.tab_order`, `tray.icon_position[<id>]`).
- **Preference value.** A `jsonb` document: any JSON-serialisable
  value, including `null` (which is *distinct* from an absent
  key — see §6.4). Maximum 64 KiB serialised; depth-limited per
  `architecture/03-data.md §1.4` JSONB validation contract
  (already enforced at the kernel layer for any JSONB column).

---

## §3 — First-boot bundled-shell install lifecycle

### 3.1 The boot pre-flight

ICD-0.6.0 §8 wired a kernel-baked static handler at `/app/*` that
served `index.html` + `shell.js` + `shell.css` from a
`constexpr std::string_view` symbol embedded into the kernel binary.
That handler was the simplest conforming form of "shell is an
extension that delivers in-browser frame X" — the URL contract was
right, only the byte source was wrong.

ICD-0.6.1 keeps the URL contract verbatim and changes the byte
source to **the active frontend's installed file tree**, exactly
where the standard 0.4.4 install lifecycle places them:
`<data_dir>/extensions/<name>/<version>/client/`. The shell becomes
a regular installed extension whose `provenance='bundled'` row in
`plinth.packages` is *created by the kernel itself* on first boot
from on-disk `<bundle_path>/shell.zip` bytes.

Pre-flight pseudocode (executes once per kernel boot, after PG
bootstrap completes, before HTTP listener starts):

```
ensure_bundled_shell_installed(cfg: Config, db: PGconn*):
  // 1. Detection. Has any ACTIVE bundled frontend already installed?
  rows = db.query(
    "SELECT id, name, version, frontend_mount "
    "FROM plinth.packages "
    "WHERE provenance = 'bundled' "
    "  AND frontend_mount IS NOT NULL "
    "  AND state IN ('ACTIVE','ACTIVE_FLAGGED') "
    "LIMIT 2"  -- LIMIT 2 to detect singleton-violation
  )
  if rows.size() == 1:
    log.info("bundled shell already installed: name={} version={}",
             rows[0].name, rows[0].version)
    return Ok(rows[0])
  if rows.size() == 2:
    return Err(ERR_MULTIPLE_ACTIVE_FRONTENDS)  // §3.5

  // 2. First-boot path. No active bundled frontend; install one.
  bundle_path = cfg.shell.bundle_path / "shell.zip"  // §9
  if !filesystem.exists(bundle_path):
    return Err(ERR_BUNDLE_MISSING)                   // §3.5

  audit("shell.firstboot.bundled_install_started", {
    "bundle_path": bundle_path,
    "bundle_size_bytes": filesystem.size(bundle_path),
  })

  // 3. Hand off to standard 0.4.4 install lifecycle.
  ctx = InstallerContext{
    .source_path = bundle_path,
    .provenance  = Provenance::Bundled,   // new value; §3.3
    .actor       = SystemActor{ "kernel-firstboot" },
    .upgrade_drain_timeout_ms = cfg.upgrade_drain_timeout_ms,
  }
  result = install_lifecycle::install_package(ctx, db)

  if result.is_ok():
    audit("shell.firstboot.bundled_install_completed", {
      "package_id":   result.value().id,
      "name":         result.value().name,
      "version":      result.value().version,
      "elapsed_ms":   result.value().elapsed_ms,
    })
    return Ok(result.value())
  else:
    audit("shell.firstboot.bundled_install_failed", {
      "failure_kind": result.error().kind_string(),
      "failed_stage": result.error().failed_stage,
      "message":      result.error().message,
    })
    return Err(ERR_BUNDLE_INSTALL_FAILED)            // §3.5
```

The call slots into `src/kernel/main.cpp` between PG bootstrap (the
existing `bootstrap_kernel_schema` call) and the HTTP listener
start. Failure aborts boot — the kernel exits non-zero with the
audit row already persisted (audit writes happen synchronously
before the error return per ICD-0.1.7 §Audit Writer's
`durability_at_emit` posture).

### 3.2 Bundle byte source

Per architect override of ICD-0.6.0 OQ1 (recorded in
`project_next_session_post_060.md`: "OQ1 pin: bundle byte source =
on-disk installed shell"), the shell.zip lives on disk at
`<bundle_path>/shell.zip`. `bundle_path` is a new
`Config::Shell::bundle_path` field defaulting to
`share/plinth/bundled` resolved relative to the kernel binary's
install root (CMake-driven; matches existing layouts for
schema.sql, etc.).

The kernel binary therefore ships *with* the bundle file alongside
it (CMake `install(FILES client/shell.zip DESTINATION
${CMAKE_INSTALL_DATADIR}/plinth/bundled)`); the binary itself does
not contain shell bytes anymore. ICD-0.6.0's embedded-resource
header is removed in the 0.6.1 code session.

**Why on-disk over embedded:**

1. **Single source of truth.** The shell.zip on disk is the same
   bytes the install lifecycle inspects, hashes (`plinth.migrations`
   checksum), and extracts. Bypassing disk for first-boot would
   force a parallel byte-source surface — embedded vs. on-disk —
   and the two could drift.
2. **Honours "shell is an extension like any other."** Admins can
   replace `<bundle_path>/shell.zip` in-place to ship an alternate
   bundled shell (e.g. a custom-branded variant); on next boot the
   kernel installs the replacement through the same lifecycle.
   Embedded would require a re-link to swap.
3. **0.4.4 install lifecycle re-uses.** The lifecycle's UPLOADING
   stage already accepts a path-or-bytes source; first-boot just
   passes a path. The same `validate_zip_structure`,
   `extract_to_data_dir`, `run_migrations`, `register_capabilities`,
   `register_rbac_rules`, `activate_routes` calls run unchanged.
4. **CSP / cache headers identical.** The static-asset surface
   served by §4's manifest-driven dispatch reads from
   `<data_dir>/extensions/shell/<version>/client/` and applies the
   same strict CSP + immutable-cache-for-named-assets / no-cache-
   for-`index.html` posture ICD-0.6.0 §8.2 already pinned. Byte
   identity preserved.

### 3.3 New `Provenance::Bundled` value

`plinth.packages.provenance` is an existing column from ICD-0.4.4
§Data Model with values `'admin'` (initial value) and reserved
`'sidecar'` (0.8.x). 0.6.1 adds `'bundled'`:

- **PG schema change (kernel-side, dev-mode):** `migrations/schema.sql`
  CHECK-constraint widening:
  `provenance IN ('admin', 'bundled')`. Reserved `'sidecar'`
  remains documented but uncommitted.
- **Kernel-side enum:** `enum class Provenance : std::uint8_t {
  Admin = 0, Bundled = 1, Sidecar = 2 };` with
  `to_string` / `from_string` round-trip per the existing 0.4.4
  pattern.
- **Default value at admin upload:** unchanged — `POST /api/packages`
  inserts `'admin'` as before.
- **First-boot value:** `'bundled'` set by the new
  `ensure_bundled_shell_installed` pre-flight call's
  `InstallerContext::provenance` field.

### 3.4 Idempotency

The detection query at §3.1 step 1 is *the* idempotency gate.
Every boot runs the pre-flight; every boot after the first
short-circuits at the single-row branch. No additional state is
stored — the detection lives entirely in `plinth.packages`.

Two scenarios where the first-boot path runs more than once,
both legal:

1. **Admin uninstalls the bundled shell.** The active row's state
   transitions to `'UNINSTALLED'` (or row deleted, depending on
   ICD-0.4.5 retention policy). Next boot: detection returns zero
   rows; first-boot install runs again. Idempotent.
2. **Admin replaces shell.zip on disk between boots.** The
   on-disk bytes change; previous install row stays as
   `'ACTIVE'`. Detection returns one row; pre-flight short-
   circuits. The replacement bytes are *not* installed
   automatically — admin must explicitly upgrade through
   ICD-0.4.5 §Upgrade Contract (e.g.
   `POST /api/packages` with the new bundle, or future admin CLI
   `plinth bundled-shell upgrade`). The kernel does not auto-
   upgrade on bundle-bytes change because that would mask
   accidental corruption.

### 3.5 Failure modes

The pre-flight surfaces five hard-fail conditions, all aborting boot:

| Code                              | Trigger                                                  | Audit `failure_kind`         |
|-----------------------------------|----------------------------------------------------------|------------------------------|
| `ERR_BUNDLE_MISSING`              | `<bundle_path>/shell.zip` does not exist or unreadable   | `bundle-missing`             |
| `ERR_BUNDLE_INSTALL_FAILED`       | `install_lifecycle::install_package` returned an `Err`   | `install-lifecycle-failed`   |
| `ERR_MULTIPLE_ACTIVE_FRONTENDS`   | Detection query returns ≥2 rows                          | `singleton-violation`        |
| `ERR_BUNDLE_DETECTION_FAILED`     | `db.query` returned an error                             | `detection-failed`           |
| `ERR_BUNDLE_SCHEMA_RESERVED`      | A user-uploaded package with `name='shell'` already in `plinth.packages` (created prior to 0.6.1 schema reservation) | `schema-name-conflict`       |

Each failure emits a `shell.firstboot.bundled_install_failed` audit
row before exit. The kernel's process-exit code distinguishes:
- `1` for `ERR_BUNDLE_MISSING` (operator-fixable: drop the file).
- `2` for `ERR_BUNDLE_INSTALL_FAILED` (operator/admin-fixable:
  inspect audit, repair).
- `3` for `ERR_MULTIPLE_ACTIVE_FRONTENDS` /
  `ERR_BUNDLE_SCHEMA_RESERVED` (database-state-fixable: admin
  intervention).
- `4` for `ERR_BUNDLE_DETECTION_FAILED` (PG-fixable).

The five-code split mirrors ICD-0.4.4's `failure_to_status` layering
— admin-surfaceable distinction by category, not by individual
case. No retry loop in the kernel — boot-time failures are
deterministic; an operator must intervene before the next start.

### 3.6 Boot ordering

The pre-flight registration site in `src/kernel/main.cpp`:

```cpp
int main(int argc, char** argv) {
  // ... arg parsing, config load ...

  bootstrap_kernel_schema(cfg.db);            // existing, ICD-0.1.1
  install_lifecycle::recover_in_flight(cfg);  // existing, ICD-0.4.4

  // NEW in 0.6.1: ensure bundled shell is installed before HTTP
  // listener accepts traffic. Post-MIGRATING the schema is ready;
  // post-ACTIVATING the routes the active-frontend resolver will
  // pick up are registered.
  auto firstboot = ensure_bundled_shell_installed(cfg, db);
  if (!firstboot) {
    log.error("bundled shell first-boot failed: {}",
              firstboot.error().to_string());
    return firstboot.error().exit_code();
  }

  init_resolver(cfg);                         // existing, ICD-0.5.0.3
  init_registry(cfg);                         // existing, ICD-0.5.0.3
  reload_tier2_cache(db);                     // existing, ICD-0.5.0.3

  // Replaces ICD-0.6.0's register_shell_routes:
  register_active_frontend_routes(cfg, db, cfg.data_dir);  // §4

  asset_server::restore_routes(db, cfg.data_dir);  // existing
  register_package_routes();                  // existing, ICD-0.4.4

  // ... drogon::app().run() ...
}
```

The pre-flight runs *before* `init_resolver` / `init_registry`
because the install lifecycle's REGISTERING stage writes capability
rows to `plinth.capabilities`; the resolver/registry init reads
those rows at startup. Running them in the wrong order would mean
the resolver's first cache fill misses the shell's
`shell.preferences.*` rows.

Tier 2 capability dispatch is unaffected — extensions installed
by admin upload after kernel start still hit
`reload_tier2_cache(db)` from their REGISTERING stage and the
resolver picks them up via the existing 0.5.0.3 invalidation
flow.

---

## §4 — `frontend.mount` manifest contract

### 4.1 Manifest schema additions

`PackageManifest` (ICD-0.4.1 §Manifest Schema) gains an optional
`frontend` block:

```jsonc
{
  "name": "shell",
  "version": "0.6.1",
  // ...existing fields...

  "frontend": {                  // OPTIONAL; absence = headless extension
    "mount": "/app/",            // REQUIRED if "frontend" present
    "entry": "index.html"        // REQUIRED if "frontend" present
  }
}
```

The forward-compat sketch from ICD-0.6.0 Appendix B becomes
load-bearing. `frontend.mount` and `frontend.entry` are validated
by `parse_manifest` (ICD-0.4.1) and surfaced through
`PackageManifest::frontend` as a new optional struct:

```cpp
struct PackageManifest {
  // ...existing fields...

  struct Frontend {
    std::string mount;   // validated regex; trailing slash required
    std::string entry;   // relative path under client/
  };
  std::optional<Frontend> frontend;
};
```

The optional-ness preserves the existing extension surface — any
existing `manifest.json` without a `frontend` block continues to
parse as before; existing extensions are headless.

### 4.2 Mount + entry validation

**`frontend.mount` regex:** `^/[a-z][a-z0-9_-]*/$`

Rules (all enforced at `parse_manifest` time, before VALIDATING
stage):

- Leading slash, single path component, trailing slash. Single
  segment for 0.6.1; multi-segment (`/app/v2/`) deferred per
  §15.
- First character is `[a-z]` to disambiguate from numeric paths
  and to keep mounts pronounceable.
- Subsequent characters in `[a-z0-9_-]`. Same alphabet as
  package names per ICD-0.4.1 §`name` regex; future-proofs
  package-name = mount-name conventions.
- Length 3–48 bytes inclusive (counting both slashes). Matches
  reserved-prefix table density.

**Reserved-prefix conflicts** (rejected at install VALIDATING):

| Mount value        | Outcome                                  |
|--------------------|------------------------------------------|
| `/api/`            | `ERR_MOUNT_CONFLICT { reserved: "api" }` |
| `/api/v1/`         | `ERR_MOUNT_CONFLICT { reserved: "api" }` (prefix-match) |
| `/ext/`            | `ERR_MOUNT_CONFLICT { reserved: "ext" }` |
| `/ws/`             | `ERR_MOUNT_CONFLICT { reserved: "ws" }`  |
| `/`                | `ERR_MOUNT_INVALID` (must be sub-prefix) |
| `/static/`         | OK                                       |
| `/app/` (bundled)  | OK if no other active mount holds it     |

Reserved-prefix list comes from `architecture/05-extensions.md §2`
and is normalized into a kernel-side constant (`kReservedMounts`)
checked during VALIDATING. Adding a reserved prefix in the future
(e.g. `/auth/` if 0.1.x splits the auth surface) is a one-line
kernel change; no ICD-0.6.1 amendment needed.

**`frontend.entry` validation:**

- Non-empty string, max 255 bytes.
- No path traversal: `..` / `.` / leading `/` / trailing `/`
  rejected.
- Resolves under `client/` after extraction; if the resolved file
  does not exist post-EXTRACTING, the install fails at
  ACTIVATING with `ERR_FRONTEND_ENTRY_MISSING`.
- Default-suggestion (informative; not enforced):
  `index.html`. The shell's manifest carries this value
  verbatim.

### 4.3 New `plinth.packages` columns

`plinth.packages` (ICD-0.4.4 §Data Model) gains two columns:

```sql
ALTER TABLE plinth.packages
    ADD COLUMN frontend_mount TEXT,
    ADD COLUMN frontend_entry TEXT;
-- Both NULL for headless extensions; non-NULL pair for frontends.
-- CHECK: (frontend_mount IS NULL) = (frontend_entry IS NULL)
ALTER TABLE plinth.packages
    ADD CONSTRAINT chk_frontend_pair
        CHECK ((frontend_mount IS NULL) = (frontend_entry IS NULL));
```

**Active-frontend partial unique index** (the singleton-invariant
enforcer):

```sql
CREATE UNIQUE INDEX idx_active_frontend_singleton
    ON plinth.packages (frontend_mount)
    WHERE frontend_mount IS NOT NULL
      AND state IN ('ACTIVE', 'ACTIVE_FLAGGED');
```

Index commits at install ACTIVATING-stage transition; the unique
constraint surfaces `pg_error: 23505` on conflict, which the
install lifecycle converts to `ERR_MOUNT_CONFLICT { reason:
"another-active-frontend" }`. Two simultaneous installs racing the
same mount are serialized by ICD-0.4.5's per-name advisory lock
plus this singleton index — both layers needed because the
advisory lock keys on package name (`pg_try_advisory_lock(
hashtextextended('plinth.packages.<name>', 0))`) but two
*different* package names can compete for the same mount.

### 4.4 Active-frontend resolution

Replaces ICD-0.6.0 §8.1 `register_shell_routes` with a manifest-
driven equivalent. New entry:

```cpp
// src/kernel/shell/active_frontend.{hpp,cpp} — replaces 0.6.0's static_handler.
namespace plinth::shell {

struct ActiveFrontend {
    std::string id;                // plinth.packages.id
    std::string name;              // e.g. "shell"
    std::string version;           // e.g. "0.6.1"
    std::string mount;             // e.g. "/app/"
    std::string entry;             // e.g. "index.html"
    std::filesystem::path client_dir;  // <data_dir>/extensions/<name>/<version>/client/
};

// Resolve at boot (after first-boot ensure). Returns nullopt if no active
// frontend (e.g. admin uninstalled the shell and is running headless).
auto resolve_active_frontend(
    const Config::Shell&         cfg_shell,
    PGconn&                      db,
    const std::filesystem::path& data_dir
) -> std::optional<ActiveFrontend>;

// Register / 302 redirect + mount/(.*)  SPA fallback.  Pre-conditions:
// resolve_active_frontend returned a value.  Replaces ICD-0.6.0's
// register_shell_routes verbatim except the byte source comes from
// active.client_dir, not embedded constants.
auto register_active_frontend_routes(
    const Config::Shell&  cfg_shell,
    const ActiveFrontend& active
) -> void;

}  // namespace plinth::shell
```

**Resolver query:**

```sql
SELECT id, name, version, frontend_mount, frontend_entry
FROM plinth.packages
WHERE provenance IN ('bundled', 'admin')
  AND frontend_mount IS NOT NULL
  AND state IN ('ACTIVE', 'ACTIVE_FLAGGED')
LIMIT 2;  -- LIMIT 2 to detect singleton-violation explicitly
```

Predicate matches the active-frontend definition from §Glossary.
`LIMIT 2` is intentional — index guarantees ≤1 result, but the
resolver uses count to surface a hard error if the index ever
disagrees with the data (PG corruption / manual UPDATE bypassing
constraints).

### 4.5 Route registration

Identical to ICD-0.6.0 §8.1 except the prefix comes from
`active.mount`:

```
GET cfg.root_redirect (default "/")  →  302 active.mount
GET active.mount + "(.*)"            →  SPA-fallback handler
```

The SPA-fallback handler logic (ICD-0.6.0 §8.2) is reused
verbatim except the file-resolution step:

```
handler(request, callback):
  // strip mount prefix; "/app/" → ""; "/app/login" → "login"
  path = request.path.substr(active.mount.length())

  // SPA fallback: index.html for empty, no-extension, or unmatched.
  if path.empty() or has_no_extension(path) or !is_named_asset(path):
    callback(serve(active.client_dir / active.entry,
                   "text/html",
                   cache: "no-cache",
                   csp: STRICT_CSP))
    return

  // Resolve named asset. Path traversal closed by component-level
  // reject of "..", ".", empty, NUL plus weakly_canonical prefix
  // check (same pattern as ICD-0.6.0 §8.2).
  resolved = resolve_safe(active.client_dir, path)
  if !resolved:
    callback(404)
    return

  callback(serve(resolved,
                 mime_for(resolved),
                 cache: "public, max-age=31536000, immutable",
                 csp: STRICT_CSP))
```

Strict CSP (`script-src 'self'; style-src 'self' 'unsafe-inline';
connect-src 'self'`) is unchanged from ICD-0.6.0 §8.2. Cache
headers unchanged. The `is_named_asset` predicate is satisfied if
the file exists under `client_dir`; the named-set whitelist from
ICD-0.6.0 §8.2 is dropped because the on-disk tree is the
authoritative set (the bundle's structural validation already
ensures only intended assets ship; path-traversal hardening
remains via `resolve_safe`).

### 4.6 Route ordering

Per ICD-0.6.0 §8.1, the SPA-fallback glob `mount + "(.*)"` MUST
register *after* every `/api/*` and `/ws` handler so Drogon's
first-match dispatch picks the API/WS handlers first. The wiring
in §3.6 (`register_active_frontend_routes` placed at the
post-`asset_server::restore_routes`-and-pre-`drogon::app().run()`
slot) preserves that order. The CI smoke test from ICD-0.6.0
§8.1 (`GET /api/auth/session` returns 401 not 200-with-index.html)
is reused verbatim and gains a sibling case asserting `/ext/...`
similarly does not get shadowed.

### 4.7 What happens with no active frontend

A fully-headless deployment (e.g. admin uninstalled the shell
without installing a replacement frontend) is legal but uncommon.
`resolve_active_frontend` returns `std::nullopt`;
`register_active_frontend_routes` is a no-op; `GET /` returns
404 (no redirect handler registered); `/app/*` paths return 404.
The kernel's API surface continues to function — the shell is
optional infrastructure.

This case can only be reached *after* first-boot succeeded once
(otherwise the pre-flight at §3.1 hard-fails). The path exists
to support headless API-only deployments where an admin
deliberately removes the shell post-install.

### 4.8 Manifest field flow into `plinth.packages`

`install_lifecycle::install_package` gains one extra step in
REGISTERING:

```
register_stage(ctx, manifest, db):
  // ...existing capability + RBAC rule registration...

  if manifest.frontend.has_value():
    db.query(
      "UPDATE plinth.packages "
      "SET frontend_mount = $1, frontend_entry = $2 "
      "WHERE id = $3",
      manifest.frontend->mount,
      manifest.frontend->entry,
      ctx.package_id
    )
  // No update if frontend is absent — columns stay NULL.
```

The UPDATE is in the same transaction as capability/RBAC rule
INSERTs, so a REGISTERING failure rolls back atomically. The
ACTIVATING stage's state-transition INSERT is what satisfies the
partial unique index's `state IN ('ACTIVE','ACTIVE_FLAGGED')`
predicate; conflicts at that point surface through the existing
ICD-0.4.5 §Atomic Swap T3 retry path.

---

## §5 — `ext_shell` PG schema

### 5.1 Schema-creation path

The `ext_shell` schema is created through the standard 0.4.3 path
verbatim. When `install_lifecycle::install_package` reaches the
MIGRATING stage with the bundled-shell `InstallerContext`, it
calls:

```cpp
plinth::packages::run_migrations(
    /*extension_name=*/ "shell",
    /*package_root=*/   ctx.extracted_path,
    /*admin_conn=*/     admin_conn);
```

`run_migrations` (ICD-0.4.3 §Library Surface) splices `name='shell'`
into the schema-creation transaction:

```sql
BEGIN;
  CREATE SCHEMA ext_shell;

  DO $$ BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'ext_shell_role') THEN
      CREATE ROLE ext_shell_role NOLOGIN;
    END IF;
  END $$;

  GRANT USAGE, CREATE ON SCHEMA ext_shell TO ext_shell_role;
  GRANT USAGE   ON SCHEMA plinth         TO ext_shell_role;
  GRANT SELECT  ON plinth.users          TO ext_shell_role;  -- ICD-0.4.3 default
COMMIT;
```

ICD-0.4.3 already grants `SELECT ON plinth.users` to every
`ext_<name>_role` because the `user_preferences.user_id FK ON
plinth.users` reference at §6.1 is the canonical use case the
default GRANT was provisioned for. No additional GRANT extension
is required for the shell — the kernel-stdlib growth path is
ICD-0.4.3 §Schema + GRANT Contract design note 2 ("Additional
reads… are added one-at-a-time as the kernel stdlib grows in
0.5.x — each addition ships as an extension-role GRANT migration
on the kernel side, not an ALTER of this 0.4.3 template").

### 5.2 Migrations folder layout in `shell.zip`

The bundled shell ships with one initial migration:

```
shell.zip/
├── manifest.json
├── capabilities.json
├── server/
│   ├── main.js
│   └── handlers/
│       ├── preferences_get.js
│       └── preferences_set.js
├── client/
│   ├── index.html
│   ├── shell.js
│   └── vendor/
│       ├── preact.module.js
│       └── htm.module.js
└── migrations/
    └── 001_init.sql
```

`migrations/001_init.sql` (full DDL in **Appendix A**) creates the
sole 0.6.1 table — `ext_shell.user_preferences` — and the
`ext_shell.default_apps` reservation block (table not yet
created; see §5.4). The migration runs through ICD-0.4.3
§Migration Execution Contract verbatim: per-file transaction,
checksum recorded in `plinth.migrations`, advisory lock prevents
concurrent runs.

### 5.3 GRANT idempotency on bundled-shell upgrade

When the bundled shell upgrades (ICD-0.4.5 §Upgrade Contract;
either admin-driven or future bundle-replacement workflow per
§3.4), the schema-creation transaction runs again on the new
version's MIGRATING stage. Per ICD-0.4.3 §Schema + GRANT Contract
*Idempotence*, the schema-create block is skipped (schema already
exists) but the GRANT block re-runs — GRANTs are already
idempotent in PG.

Future migrations (e.g. `002_add_default_apps.sql` in the 0.6.6
bundle update) ship in the same `migrations/` folder and run
through the existing 0.4.3 path. The 0.6.1 ICD does not pre-author
0.6.6's migration — it only reserves the `ext_shell.default_apps`
name (§5.4).

### 5.4 `ext_shell.default_apps` reservation

Per `DESIGN-shell-v06x.md §3.7` line 371–373, the second
`ext_shell` table the shell needs is `ext_shell.default_apps`,
keyed on `(scope, user_id, content_type)`. ICD-0.6.1 explicitly
**reserves the name** (no other extension may create
`ext_shell.default_apps`) but **does not author the DDL** — that
ships with ICD-0.6.6 *Tray system + content-type resolution*
when the consumer surface lands.

The reservation manifests as a comment in `001_init.sql`:

```sql
-- 0.6.6 will add ext_shell.default_apps via a 002_*.sql migration.
-- Reserved here to document the schema's intended shape; do not
-- create this table in 0.6.1 — it has no consumer yet.
-- See ICD-0.6.6-tray-content-type-navigation.md §default_apps.
```

The reservation discipline is stricter than ordinary squatter
prevention because `ext_shell` is a kernel-reserved schema (§5.5):
only the bundled shell's own `migrations/` folder may add tables
to it.

### 5.5 Schema-name reservation

The literal package name `'shell'` is reserved for the bundled
shell extension. User-uploaded packages with `manifest.json`
`name='shell'` are rejected at `parse_manifest` time
(ICD-0.4.1 §Manifest Schema validation extension):

```cpp
if (manifest.name == "shell" && ctx.provenance != Provenance::Bundled) {
    return Err(ManifestParseError::RESERVED_NAME);
}
```

The `'plinth'` schema is similarly reserved (kernel-owned, never
created through 0.4.3); `'shell'` joins it as a reserved package
name. Both reservations are kernel-side constants; future
reservations (e.g. `'admin'` for the bundled admin extension at
0.6a) ship as one-line additions to the constant.

`ERR_BUNDLE_SCHEMA_RESERVED` (§3.5) covers the migration window
where a 0.4.x-installed user package with `name='shell'` exists
in `plinth.packages` at first 0.6.1 boot. The migration path
for that operator is documented in §11.4.

### 5.6 What `ext_shell` does NOT contain

By design, the following are **not** in `ext_shell`:

- **Session state.** Lives in `plinth.sessions` (kernel-owned).
- **RBAC rules.** Live in `plinth.rbac_rules` (kernel-owned per
  ICD-0.1.5).
- **Capability registrations.** Live in `plinth.capabilities`
  (kernel-owned per ICD-0.2.0).
- **Audit rows.** Live in `plinth.audit` (kernel-owned per
  ICD-0.1.7). Shell preference get/set audits land in `plinth.audit`
  with the `category='shell.preferences.*'` discriminator, not in
  `ext_shell`.
- **Other extensions' preferences.** Each extension owns its own
  `ext_<name>` schema per `DESIGN-shell-v06x.md §3.7`'s "own your
  data in your own schema" principle. Extensions that need
  per-user preferences create their own per-user table; the shell
  is the dogfood case, not the universal store.

---

## §6 — `ext_shell.user_preferences` table

### 6.1 DDL

Authoritative DDL (full file in **Appendix A**):

```sql
CREATE TABLE ext_shell.user_preferences (
    user_id     UUID         NOT NULL,
    key         TEXT         NOT NULL,
    value       JSONB        NOT NULL,
    updated_at  TIMESTAMPTZ  NOT NULL DEFAULT now(),

    PRIMARY KEY (user_id, key),

    CONSTRAINT fk_user_preferences_user_id
        FOREIGN KEY (user_id)
        REFERENCES plinth.users (id)
        ON DELETE CASCADE,

    CONSTRAINT chk_key_nonempty
        CHECK (length(key) BETWEEN 1 AND 255),

    CONSTRAINT chk_value_size
        CHECK (octet_length(value::text) <= 65536)  -- 64 KiB
);

CREATE INDEX idx_user_preferences_updated_at
    ON ext_shell.user_preferences (updated_at);
-- Index supports future "tell me what changed since" queries from
-- the panel SDK (0.6.3+); not consumed in 0.6.1.

GRANT SELECT, INSERT, UPDATE, DELETE
    ON ext_shell.user_preferences
    TO ext_shell_role;
```

### 6.2 Column semantics

| Column        | Type           | Notes                                                                  |
|---------------|----------------|------------------------------------------------------------------------|
| `user_id`     | `UUID NOT NULL`| Always the calling user; never another user's id (RBAC §7.4 enforces) |
| `key`         | `TEXT NOT NULL`| Extension-defined; format-free at kernel layer; 1–255 byte length     |
| `value`       | `JSONB NOT NULL`| Any JSON-serialisable value including `null`; 64 KiB max serialised  |
| `updated_at`  | `TIMESTAMPTZ`  | Server-side stamp at INSERT/UPDATE; never set by client                |

**`value` semantic of `null`:** A row with `value = 'null'::jsonb`
is *present* and distinguishable from a missing row. Setting a
key to JSON `null` is a legal "explicit clear without delete"
operation. `shell.preferences.get` returns `null` for that case;
`shell.preferences.get` for a missing key returns `undefined`
(absence-of-key). See §7.3 for the get-side disambiguation.

**`updated_at` is purely informative** in 0.6.1 — no consumer.
The column exists because (a) future 0.6.3 panel SDK may surface
"last modified" indicators per the design bundle's avatar popdown
prototype, and (b) cleanup tasks (e.g. "purge preferences for
deleted users older than N days" — out of scope per `ON DELETE
CASCADE` covering it transitively) become trivial with the
column in place.

### 6.3 Row sizing and limits

- **`key` length:** 1–255 bytes UTF-8. The 255 cap matches typical
  identifier limits (DNS labels, max btree key prefix at PG's
  default 8 KiB pages); shorter is preferred by convention.
- **`value` payload:** 64 KiB serialised JSON max. This is the
  per-row enforcement; the round-trip transport (`shell.preferences.set`
  capability call) re-checks this limit at the resolver path
  (§7.5) so over-large values fail before reaching PG (cleaner
  error surfacing).
- **Per-user row count:** No hard cap at the schema layer.
  Convention: extensions should avoid using `user_preferences`
  as a generic key/value store for high-cardinality data
  (audit-log-like cases; per-item state for thousands of items).
  The capability layer (§7) emits an audit row when a user's row
  count crosses 1000 to surface anomalies; no enforcement, just
  visibility.

### 6.4 Get/set semantics summary

(Detailed contracts at §7.) Quick reference:

| Operation         | Effect                                                      | Returns       |
|-------------------|-------------------------------------------------------------|---------------|
| `set(key, value)` | UPSERT row at `(user_id, key)`; `updated_at = now()`        | `{ ok: true }` |
| `set(key, null)`  | UPSERT row with `value = 'null'::jsonb`                     | `{ ok: true }` |
| `delete(key)`     | DELETE row at `(user_id, key)`. Implemented via `set(key, undefined)` per OQ3 architect-recommendation; no separate capability | `{ ok: true, deleted: bool }` |
| `get(key)`        | SELECT one row; `null` if value is JSON `null`; `undefined` if no row | `{ value: T \| null \| undefined }` |
| `get_all()`       | SELECT all rows for the user                                | `{ entries: [{key, value}, ...] }` |

`get_all` is added per OQ2 architect-recommendation (eager bulk
fetch on shell init); see §7.3.

### 6.5 Concurrency

Standard PG row-level locking. Two simultaneous `set` calls for
the same `(user_id, key)` serialize on the row's PK; last writer
wins by `updated_at` order. No advisory locking needed —
preferences are user-scoped and the same user concurrently
calling set on the same key is rare; the consequence (one of two
values persists) is benign for any preference-class state.

### 6.6 User deletion cascade

`ON DELETE CASCADE` from `plinth.users(id)` covers user-deletion
cleanup automatically. ICD-0.6.1 adds no explicit cleanup hook to
the shell-side capability surface — the cascade fires inside the
existing `plinth.users` DELETE transaction (kernel-owned, ICD-0.1.2
§User Deletion). The shell's consumer surfaces will see `get(key)`
return `undefined` for any key after a user's row is deleted,
which is the correct (defensive) behaviour for the no-row case.

`DESIGN-shell-v06x.md §3.7` line 380–384 ("User deletion: …
Shell-side cleanup is trivial: delete rows where `user_id =
deleted_id`. Preference data has no intrinsic value after the
user is gone.") is satisfied entirely by the cascade — no shell-
side delete logic needed.

---

## §7 — Get/set capability pattern

### 7.1 Capability registrations

The shell's `capabilities.json` declares two atomic capabilities
(per OQ1 architect-recommendation; alternatives in §14). The
declaration shape mirrors any other extension's `capabilities.json`
per ICD-0.4.6 §Capability Manifest:

```jsonc
// shell.zip/capabilities.json
{
  "provides": [
    {
      "namespace": "shell",
      "version":   1,
      "function":  "preferences.get",
      "params": [
        { "name": "key", "type": "string" }
      ],
      "returns":   "object",
      "scope":     "instance",
      "rbac_rule": "shell.preferences.read"
    },
    {
      "namespace": "shell",
      "version":   1,
      "function":  "preferences.set",
      "params": [
        { "name": "key",   "type": "string" },
        { "name": "value", "type": "any"    }
      ],
      "returns":   "object",
      "scope":     "instance",
      "rbac_rule": "shell.preferences.write"
    },
    {
      "namespace": "shell",
      "version":   1,
      "function":  "preferences.get_all",
      "params":    [],
      "returns":   "object",
      "scope":     "instance",
      "rbac_rule": "shell.preferences.read"
    }
  ]
}
```

The `1` in `version` is the capability's wire-version (ICD-0.5.0.3
§Versioning); bumping is reserved for future breaking-change
events (none currently planned for 0.6.x). `params.type='any'` for
the value parameter accepts any JSONB-serialisable input;
validation runs server-side (§7.5).

### 7.2 RBAC rule registrations

Two new RBAC rules, registered through ICD-0.4.6 §Rule Registration
verbatim — both shipping via the bundled shell's `rbac.json`:

```jsonc
// shell.zip/rbac.json
{
  "rules": [
    {
      "name": "shell.preferences.read",
      "description": "Read this user's shell preferences (own scope only)."
    },
    {
      "name": "shell.preferences.write",
      "description": "Write this user's shell preferences (own scope only)."
    }
  ]
}
```

**Default group grants:** Both rules SHALL be granted to the
kernel's `users` group at install time. The grant is added to the
shell's `rbac.json` `default_grants` block (ICD-0.4.6
§Default Grants):

```jsonc
{
  "default_grants": [
    { "group": "users", "rule": "shell.preferences.read"  },
    { "group": "users", "rule": "shell.preferences.write" }
  ]
}
```

Rationale: every authenticated user can read and write *their
own* preferences. The rule does not authorise reading or writing
*another user's* preferences — that scoping is enforced at §7.4
in the capability handler, not at the RBAC layer. (RBAC checks
scope-of-action; the per-user-id scoping is an authorization
post-condition, not a permission grant.)

Admins may revoke these grants per existing ICD-0.4.6 §Group-
Rule Manipulation; doing so disables the shell's preference
round-trip for the affected users. The shell's own UI (post-0.6.2)
gracefully degrades to default theme/scale when get/set returns
RBAC-denied — out of scope here, but the capability surface
returns `403 rbac.denied` exactly the same as any other
RBAC-gated capability call.

### 7.3 Handler logic — `preferences.get` and `preferences.get_all`

The handlers run as Tier 2 extension JS per ICD-0.5.0.3 §Tier 2
Dispatch. The shell's `server/handlers/preferences_get.js`:

```javascript
// shell.zip/server/handlers/preferences_get.js
async function preferences_get(args, ctx) {
  const { key } = args;
  if (typeof key !== "string" || key.length === 0 || key.length > 255) {
    throw new CapabilityError("invalid_argument", "key must be 1..255 byte string");
  }

  // ctx.user.id is bound by the kernel from the session cookie.
  // Per §7.4 we never accept a user_id from args — own-scope only.
  const rows = await db.query(
    "SELECT value FROM ext_shell.user_preferences " +
    "WHERE user_id = $1 AND key = $2",
    [ctx.user.id, key]
  );

  if (rows.length === 0) {
    return { value: undefined };  // §6.4: distinguishable from JSON null
  }
  return { value: rows[0].value };
}
```

`preferences.get_all`:

```javascript
async function preferences_get_all(args, ctx) {
  const rows = await db.query(
    "SELECT key, value FROM ext_shell.user_preferences " +
    "WHERE user_id = $1 " +
    "ORDER BY key ASC",
    [ctx.user.id]
  );
  return { entries: rows };  // [{key, value}, ...]
}
```

`get_all` returns every key/value pair for the calling user.
Capped implicitly by the per-user row-count audit threshold at
§6.3 (no explicit cap; 64 KiB × 1000 row anomaly = 64 MiB
worst-case which is well under JSON-response sanity limits).

### 7.4 Handler logic — `preferences.set`

```javascript
// shell.zip/server/handlers/preferences_set.js
async function preferences_set(args, ctx) {
  const { key, value } = args;
  if (typeof key !== "string" || key.length === 0 || key.length > 255) {
    throw new CapabilityError("invalid_argument", "key must be 1..255 byte string");
  }

  const serialised = JSON.stringify(value);
  if (serialised === undefined) {
    // Pure undefined value: per OQ3, this is the deletion gesture.
    const { rowCount } = await db.query(
      "DELETE FROM ext_shell.user_preferences " +
      "WHERE user_id = $1 AND key = $2",
      [ctx.user.id, key]
    );
    return { ok: true, deleted: rowCount > 0 };
  }
  if (serialised.length > 65536) {
    throw new CapabilityError("payload_too_large",
                              "value exceeds 64 KiB serialised limit");
  }

  await db.query(
    "INSERT INTO ext_shell.user_preferences (user_id, key, value) " +
    "VALUES ($1, $2, $3::jsonb) " +
    "ON CONFLICT (user_id, key) DO UPDATE " +
    "SET value = EXCLUDED.value, updated_at = now()",
    [ctx.user.id, key, serialised]
  );

  return { ok: true };
}
```

**`undefined`-as-delete semantics (OQ3 architect-recommendation).**
JavaScript distinguishes `undefined` from `null` natively; the
QuickJS bridge serialisation passes `undefined` through to the
handler argv as the JSValue tag `JS_UNDEFINED`. Setting
`value=undefined` on the wire deletes the row;
`value=null` UPSERTs a row with `value = 'null'::jsonb` (the
explicit-clear-without-delete case from §6.4). This avoids
adding a third capability (`shell.preferences.delete`) and the
distinction maps naturally to JS callers' mental model.

For non-JS callers (a hypothetical HTTP-direct preference editor),
the wire-protocol omission of `value` from the JSON request body
serialises to `undefined` after the QuickJS bridge's
`JS_ParseJSON`, achieving the same semantic. JSON `null` in the
body serialises to `JS_NULL`. The bridge's existing argument
unpacking (ICD-0.3.4 §Argument Marshalling) handles both verbatim.

### 7.5 Resolver-side validation

ICD-0.5.0.3 §Resolver Path runs before the Tier 2 handler. For
preference set/get the resolver pre-checks:

1. **RBAC rule is granted to the user.** Standard ICD-0.4.6
   filter; rejected as `403 rbac.denied { rule:
   "shell.preferences.read|write" }` if not.
2. **Argument count + type at the manifest level.** Drogon-side
   parser; rejected as `400 invalid_arguments` for wrong-arity
   calls.
3. **`shell.preferences.set` value-size pre-check.** If the
   JSONB-serialised value exceeds 64 KiB, the resolver rejects
   before Tier 2 dispatch (`413 payload_too_large` rather than
   the shell's `CapabilityError("payload_too_large")`). The
   double-check at the handler layer is a defense-in-depth in
   case a future SDK bypasses the resolver pre-flight.

The validation post-conditions match the §7.3/§7.4 handler
preconditions; both layers exist because ICD-0.5.0.3 dispatch
permits *any* capability handler to enforce its own additional
checks beyond what the resolver pre-validates.

### 7.6 Audit emission

Three audit categories ride the capability surface (full taxonomy
at §10):

- **`shell.preferences.set`** — emitted on every successful set,
  *deduped per `(user_id, key)` with 60 s TTL* (per OQ7
  architect-recommendation). Bursty UI sliders setting
  `theme.scale_factor` 50 times per second do not flood the
  audit log; the dedup window collapses them to one row per
  minute.
- **`shell.preferences.read_failed`** — emitted on get-side
  PG errors. Rate-limited per `(user_id, "read")` with 60 s TTL.
  Distinguishes "PG down" from "key absent" (the latter returns
  `value: undefined` and emits no audit).
- **`shell.preferences.row_count_anomaly`** — emitted once per
  user when their row count crosses 1000 (§6.3 informational
  threshold). One-shot per user; no dedup window needed.

### 7.7 Write-after-write concurrency

§6.5 covers PG-level concurrency. At the *capability* layer,
two concurrent `set(theme.mode, "dark")` calls from the same
user serialise via the row PK. The audit dedup window is
keyed at the rolling-window level: both calls land in the same
60 s window so only one audit row appears, but both PG
transactions commit successfully — the user perceives both
clicks as "succeeded" and the UI state matches the second-write
value.

### 7.8 Read-after-write coherence

Drogon's connection pool plus PG's MVCC default isolation
(`READ COMMITTED`) guarantee that a `set` followed by a `get` on
the same connection sees the new value. The eager-bulk-fetch
posture from OQ2 means the shell client typically calls
`get_all` once at init then operates on a local cache; the only
read-after-write concern is the resolver-level case where two
back-to-back capability calls hit different connections from the
pool. Even there, MVCC `READ COMMITTED` on the second call's
SNAPSHOT advances past the first's COMMIT; the second `get` sees
the freshly-set value.

The shell client is responsible for cache invalidation when it
issues a `set` — the design bundle's avatar popdown JSX shows
the pattern (set then patch-local-state), the kernel-side
contract is just the round-trip.

---

## §8 — Hand-off from ICD-0.6.0

### 8.1 What ICD-0.6.0 pinned that this ICD inherits unchanged

- **Top-level error boundary** (ICD-0.6.0 §7) — the
  `componentDidCatch` payload, fallback render with Reload
  button, and `?force-throw=1` URL seam carry forward verbatim.
  The boundary lives in `client/shell.js` which ships in
  `shell.zip` unchanged from the 0.6.0 baked artifact.
- **Login flow + 429 retry-after UX** (ICD-0.6.0 §5) — pinned
  at OQ3, unchanged.
- **Four-zone topbar layout** (ICD-0.6.0 §6) — empty zones
  preserved; tab-strip / app-switcher / tray content remain
  out-of-scope until 0.6.4 / 0.6.6.
- **CSP + cache headers** (ICD-0.6.0 §8.2 + §11) — strict CSP
  applied verbatim by the new manifest-driven dispatch (§4.5);
  `index.html` cached `no-cache`, named assets cached
  `public, max-age=31536000, immutable`.
- **Vendored Preact + htm versions** (ICD-0.6.0 §4.3 +
  Appendix B) — `preact.module.js@10.22.0` and
  `htm.module.js@3.1.1` ship inside `shell.zip` at the same
  paths under `client/vendor/`. The vendor-time grep contract
  (no `eval(`, no `new Function(`) is preserved by the bundle's
  packaging — a CI smoke step in the 0.6.1 code session re-runs
  the grep against the post-extraction tree.

### 8.2 What ICD-0.6.0 pinned that this ICD replaces

- **§8 Static-asset handler.** ICD-0.6.0's
  `register_shell_routes(cfg, db, data_dir)` resolved the
  active bundled frontend at boot from a kernel-baked byte
  source. ICD-0.6.1 replaces this with `register_active_frontend_routes`
  (§4.4) which reads bytes from
  `<data_dir>/extensions/<name>/<version>/client/`. URL
  contract is byte-identical; route registration order is
  byte-identical; CSP + cache headers are byte-identical;
  byte source moves from `.text` to filesystem.
- **OQ1 (bundle byte source).** ICD-0.6.0 OQ1 recommended
  embedded resources; architect overrode to on-disk at code-
  session ship per `project_next_session_post_060.md`.
  ICD-0.6.1 §3.2 normalizes that override into the install
  contract — the question is settled, not re-litigated.
- **Embedded shell-asset symbols.** The `constexpr
  std::string_view shell_index_html` etc. baked into the
  kernel binary by the 0.6.0 code session are removed in the
  0.6.1 code session; the kernel binary becomes ~80 KiB
  smaller.

### 8.3 What this ICD pre-stages for 0.6.2+

- **`user_preferences` round-trip surface.** The pin that 0.6.2
  consumes for theme + scale; the pin that 0.6.4 consumes for
  tab ordering; the pin that 0.6.5 consumes for float position
  persistence. Every milestone that lands a UI surface with
  user-scoped state hits this round-trip — 0.6.1 is the
  one-time investment.
- **Capability-as-shell-stdlib pattern.** `shell.preferences.get`
  / `set` are the first kernel-stdlib-shaped capabilities
  outside the 0.5.x realtime + db cluster. ICD-0.6.3 panel SDK
  will wrap them as `plinth.preferences.get` /
  `plinth.preferences.set` client wrappers; the kernel-side
  capability is the substrate.
- **`ext_shell.default_apps` reservation.** §5.4. 0.6.6 picks
  this up.
- **Manifest-driven mount routing pattern.** §4 generalises to
  multi-frontend at 0.6.4; the singleton invariant in 0.6.1
  is the simplest conforming form.

### 8.4 Phase ordering for the 0.6.1 code session

Recommended phase commit arc (architect-confirmable in code-
session plan):

1. **Schema + bundled-shell artifact.** Author
   `client/shell/migrations/001_init.sql`, update
   `client/shell/manifest.json` to bump `version` and add
   `frontend.entry`, generate `client/shell.zip` artifact via
   CMake (`add_custom_command(OUTPUT shell.zip COMMAND zip ...)`),
   wire to `install(FILES …)` for `<bundle_path>` deployment.
2. **`Provenance::Bundled` + manifest-fields.** New enum value
   + `provenance` CHECK widening + `plinth.packages.frontend_*`
   columns + partial unique index + `parse_manifest` field
   parsing + reserved-prefix conflict checker.
3. **First-boot pre-flight.** New
   `src/kernel/shell/firstboot.{hpp,cpp}` with
   `ensure_bundled_shell_installed`; wire into `main.cpp` per
   §3.6; B.\* test coverage.
4. **Manifest-driven dispatch.** New
   `src/kernel/shell/active_frontend.{hpp,cpp}` replacing
   `static_handler.{hpp,cpp}`; wire `register_active_frontend_routes`
   in `main.cpp`; M.\* test coverage. Decommission embedded
   shell-asset symbols.
5. **Capabilities + RBAC rules.** Author `shell.zip`
   `capabilities.json` + `rbac.json` + `server/handlers/preferences_*.js`;
   register through 0.4.6 path; P.\* test coverage; integration
   test at I.\*.

Each phase has a one-bisect-cycle-to-revert-or-amend property
per the existing METHODOLOGY phase-commit pattern.

---

## §9 — Configuration surface

### 9.1 New `Config::Shell::bundle_path` field

Extends the existing `Config::Shell` block from ICD-0.6.0 §9.1:

```cpp
struct Config {
  // ...existing blocks...
  struct Shell {
    bool        enabled        = true;       // ICD-0.6.0
    std::string root_redirect  = "/app/";    // ICD-0.6.0; default unchanged
    std::string bundle_path    = "";         // NEW in 0.6.1; resolved at load
  } shell;
  // ...
};
```

### 9.2 JSON loader

`Config::Shell::bundle_path` parses from `shell.bundle_path` in
the kernel config JSON. Loader behaviour:

- **Absent or empty:** resolved to the default
  `share/plinth/bundled` relative to the kernel binary's install
  root (CMake `${CMAKE_INSTALL_DATADIR}/plinth/bundled` resolved
  at runtime via the same mechanism that `migrations/schema.sql`
  uses). The default is normative — a deployment with no config
  override gets the bundled shell.
- **Non-empty absolute path:** used verbatim.
- **Non-empty relative path:** resolved against the kernel
  binary's install root (consistent with default).

The resolved absolute path is logged once at boot per the
existing `Config::log_summary` pattern.

### 9.3 Validation

`Config::Shell::validate` extends:

```cpp
auto Config::Shell::validate() const -> std::optional<ConfigError> {
  // ...existing root_redirect regex check...

  if (resolved_bundle_path().empty()) {
    return ConfigError{ "shell.bundle_path resolves to empty string" };
  }
  // Existence check is deferred to ensure_bundled_shell_installed (§3.5
  // ERR_BUNDLE_MISSING) — config validation does not assert filesystem
  // state because the bundle may legitimately be added between config
  // load and first boot in some deployments.
  return std::nullopt;
}
```

### 9.4 No new realtime / extension config

ICD-0.6.1 introduces no new `Config::Realtime` or `Config::Extensions`
fields. Capability dispatch reuses the existing 0.5.0.3 surface;
no new pool-size / timeout / rate-limit knobs at the shell layer.

### 9.5 Migration from ICD-0.6.0 config

Existing 0.6.0 config files require no change. The new
`shell.bundle_path` field defaults at load. Operators who need
to override (e.g. installing the bundled shell from
`/opt/plinth-bundles/shell.zip`) add the JSON line and reload —
no breaking change.

---

## §10 — Audit events

### 10.1 First-boot install lifecycle

Three single-shot audits ride the first-boot pre-flight (§3.1).
None are deduped — each boot's first-boot pass should produce at
most one of each. Category prefix `shell.firstboot.*`:

| Audit                                       | Trigger                                  | Payload                                                |
|---------------------------------------------|------------------------------------------|--------------------------------------------------------|
| `shell.firstboot.bundled_install_started`   | Pre-flight enters first-boot path        | `{ bundle_path, bundle_size_bytes }`                   |
| `shell.firstboot.bundled_install_completed` | `install_lifecycle::install_package` Ok  | `{ package_id, name, version, elapsed_ms }`            |
| `shell.firstboot.bundled_install_failed`    | Pre-flight returns `Err`                 | `{ failure_kind, failed_stage, message }` (§3.5 codes) |

The actor on these rows is `system: kernel-firstboot`; no `user_id`.
This matches existing kernel-internal audits per ICD-0.1.7
§Actor Field.

### 10.2 Preference round-trip

Three rate-limited audit categories under `shell.preferences.*`:

| Audit                                  | Dedup key                  | TTL  | Trigger                                       |
|----------------------------------------|----------------------------|------|-----------------------------------------------|
| `shell.preferences.set`                | `(user_id, key)`           | 60 s | Successful UPSERT or DELETE                   |
| `shell.preferences.read_failed`        | `(user_id, "read")`        | 60 s | PG error inside get/get_all handler           |
| `shell.preferences.row_count_anomaly`  | `(user_id)`                | once | User's row count crosses 1000 (one-shot)      |

Dedup uses the existing 0.5.0.3 audit-rate-limit infrastructure
(rolling window with `(category, dedup_key)` slots cleared on TTL
expiry). The 60 s TTL on `set` is OQ7's architect-recommendation
tuned for design-bundle UI patterns where slider drags emit dense
sets per user input.

`shell.preferences.set` payload:

```jsonc
{
  "category":     "shell.preferences.set",
  "actor":        { "kind": "user", "user_id": "..." },
  "key":          "<the preference key>",
  "value_class":  "object" | "array" | "string" | "number" | "boolean" | "null" | "deleted",
  "value_size":   <bytes>,
  "deduped":      <bool>     // true if this is the dedup-collapse merge row
}
```

The `value_class` column is *not* the value itself — preferences
are PII-adjacent and storing values in audit would create a
parallel privacy surface. Class + size is enough for "user X
changed their theme N times this hour"-style observability without
PII risk.

### 10.3 What does NOT generate an audit

- **`shell.preferences.get` happy path.** Reads are not audited
  by category — they're high-rate and PII-adjacent. The kernel's
  general access logging (request-level) is sufficient.
- **`shell.preferences.set` rejected pre-handler** (RBAC
  denial / size limit / arg validation). These hit the standard
  ICD-0.5.0.3 audit family (`capability.rbac_denied` /
  `capability.invalid_argument` / `capability.payload_too_large`).
  No shell-specific audit needed.

---

## §11 — Security constraints

### SC1 — `user_id` from session, never from arguments

Both `shell.preferences.set` and `shell.preferences.get` use
`ctx.user.id` bound from the session cookie — never an `args.user_id`
field. This is enforced structurally: the capability `params`
arrays (§7.1) do not include `user_id`. A shell-side mistake
(adding `user_id` to args) would surface at parse time
(`capability.invalid_argument` because the receiver doesn't
declare it).

The check is ICD-0.6.1's primary cross-tenant defence: User A
cannot read User B's preferences because the underlying SQL
binds `WHERE user_id = $1` to `ctx.user.id`, which is bound to
A's session. There is no surface for A to set User B's id at
the capability boundary.

### SC2 — RBAC rules grant scope-of-action, not scope-of-target

`shell.preferences.read` and `shell.preferences.write` permit
their bearer to read / write preferences. The *target* (always
the calling user's own preferences) is enforced at SC1 above.
This split mirrors `notes.read` / `notes.write` from
fixture extensions where the target is also bound by handler
logic, not RBAC scope.

Admins reading another user's preferences is **not** supported in
0.6.1 — there is no `shell.preferences.read_other_user` rule, no
`scope='admin'` capability variant, no admin-bypass path. If
0.6a's admin extension needs preference inspection, that's its
own ICD slot with its own capability.

### SC3 — Migration SQL not subject to GlassWorm Layer 2

Per ICD-0.4.1 §Out-of-scope, `.sql` files reach libpq, not
`JS_Eval`; Layer 2 scanning is library-level static analysis
on JS source. The shell's `migrations/001_init.sql` ships in
`shell.zip` and runs through ICD-0.4.3 verbatim — no GlassWorm
augmentation, no exception, no special-casing. Bundled status
does not weaken any other 0.4.x security check; bundled
extensions traverse the identical install pipeline as
admin-uploaded.

### SC4 — Preference value 64 KiB cap

§6.1's `chk_value_size` (PG-side) + §7.5's resolver pre-check
(kernel-side) pin a 64 KiB serialised JSONB ceiling. The cap
exists primarily to prevent `user_preferences` from being misused
as a generic key/value blob store; 64 KiB easily covers any
reasonable preference (theme settings, tab ordering, position
maps for hundreds of items) while making "store the user's CSV
import results here" obviously unworkable. Larger payloads belong
in the 0.10.0 storage subsystem.

### SC5 — Bundled-shell schema reservation

§5.5's reservation of `name='shell'` for the bundled extension
prevents a user-uploaded package from squatting on the schema
the bundled shell needs. Migration window: any 0.4.x deployment
that already has a user package with `name='shell'` in
`plinth.packages` at first 0.6.1 boot hits
`ERR_BUNDLE_SCHEMA_RESERVED`. Operator fix: rename the
user-installed package (`plinth admin rename-package <id>
<new_name>`, future admin CLI; 0.6.1 the workaround is to
`UPDATE plinth.packages SET name = '<new_name>' WHERE id =
'<id>'` directly with admin-DB access), or uninstall it and
reinstall under a different name.

The migration window is empty for fresh installs — no operator
workflow today recommends `name='shell'` because the design
bundle and DESIGN-shell-v06x.md §3.7 reserve it explicitly. The
guard is defensive against an unlikely-but-possible state.

### SC6 — Mount-conflict denial of frontend hijack

§4.2's reserved-prefix conflict + §4.3's partial unique index
together prevent: (a) a user-uploaded extension declaring
`frontend.mount: "/app/"` while the bundled shell holds it
(unique-index conflict at ACTIVATING; ICD-0.4.5 §Atomic Swap T3
surfaces 409 to the admin); (b) a user-uploaded extension
declaring `frontend.mount: "/api/"` (`ERR_MOUNT_CONFLICT` at
VALIDATING). The kernel API surface cannot be shadowed by an
extension's frontend by construction.

### SC7 — Audit dedup TTL not user-controllable

The 60 s TTL on `shell.preferences.set` audit dedup (§10.2) is a
kernel constant, not a user-tunable. A malicious or buggy
extension with `shell.preferences.write` cannot drown the audit
log by emitting set-storms — the dedup window collapses to one
row per minute regardless. The constant lives in the same audit-
rate-limit table as ICD-0.5.5's broker dedups; tuning is a kernel
ICD amendment.

---

## §12 — Test cases

35 cases organized by prefix. Tests land in the eventual 0.6.1
**code** session — paper-only authoring lists them and assigns
group prefixes; the 0.6.1 code session writes the test files and
asserts against the contracts here.

### 12.1 `B.*` — first-boot install (6 cases, library-level)

| Case  | Scenario                                                                                           |
|-------|----------------------------------------------------------------------------------------------------|
| B.01  | Fresh PG; `<bundle_path>/shell.zip` present → first-boot pre-flight installs; row at `state='ACTIVE'`, `provenance='bundled'`, `frontend_mount='/app/'` |
| B.02  | Existing ACTIVE bundled shell row → pre-flight short-circuits in <10 ms; no audit row; no install |
| B.03  | `<bundle_path>/shell.zip` missing → boot aborts with exit code 1; `shell.firstboot.bundled_install_failed { failure_kind: "bundle-missing" }` audit landed |
| B.04  | Bundle exists but `extract_to_data_dir` fails (corrupt zip) → boot aborts exit 2; audit `failure_kind: "install-lifecycle-failed"` with stage `EXTRACTING` |
| B.05  | Two ACTIVE bundled-frontend rows manufactured pre-boot → pre-flight aborts exit 3; `failure_kind: "singleton-violation"` |
| B.06  | Pre-existing user package `name='shell'` with `provenance='admin'` → pre-flight aborts exit 3; `failure_kind: "schema-name-conflict"` |

### 12.2 `M.*` — manifest-driven mount routing (8 cases, integration-level via HttpTestFixture)

| Case  | Scenario                                                                                           |
|-------|----------------------------------------------------------------------------------------------------|
| M.01  | Fresh install; `GET /` → 302 `/app/`; `GET /app/` → 200 `index.html` with strict CSP + no-cache    |
| M.02  | `GET /app/shell.js` → 200 with `application/javascript` and `public, max-age=31536000, immutable` |
| M.03  | `GET /app/index.html` (named index) → 200 with no-cache + html mime                                |
| M.04  | `GET /app/login` (no extension, SPA fallback) → 200 `index.html`                                  |
| M.05  | `GET /app/missing.css` → 404 (not SPA-fallback because explicit extension)                         |
| M.06  | `GET /api/auth/session` (no shell session) → 401, NOT 200-with-html (route-order regression)      |
| M.07  | `GET /ext/some/0.0.1/main.js` (not the shell prefix) → 404 (no shadowing of `/ext/*`)              |
| M.08  | After admin uninstalls bundled shell → `resolve_active_frontend` returns nullopt; routes deregister; `GET /` → 404 (no redirect handler) |

### 12.3 `S.*` — schema migration (4 cases, library-level)

| Case  | Scenario                                                                                           |
|-------|----------------------------------------------------------------------------------------------------|
| S.01  | First MIGRATING stage on bundled shell → `ext_shell` schema exists; `ext_shell_role` exists; GRANTs applied; `ext_shell.user_preferences` table exists with PK + FK + CHECKs |
| S.02  | Re-running MIGRATING (idempotent re-install) → `001_init.sql` checksum-matches existing row in `plinth.migrations`; no duplicate-table error |
| S.03  | Modified `001_init.sql` between boots (operator hand-edit on disk) → `MigrationError::CHECKSUM_MISMATCH` at MIGRATING |
| S.04  | User-installed package `name='shell'` rejected at `parse_manifest` (`RESERVED_NAME`) — never reaches MIGRATING |

### 12.4 `P.*` — preference get/set (14 cases, integration-level via HttpTestFixture)

| Case  | Scenario                                                                                           |
|-------|----------------------------------------------------------------------------------------------------|
| P.01  | `set("theme.mode", "dark")` → row inserted; `get("theme.mode")` → `{value: "dark"}`                |
| P.02  | `set("theme.mode", "dark")` then `set("theme.mode", "light")` → second value overwrites; one row  |
| P.03  | `get` for absent key → `{value: undefined}` (NOT `{value: null}`)                                  |
| P.04  | `set("scratch", null)` → row exists with `value='null'::jsonb`; `get("scratch")` → `{value: null}` (NOT `undefined`) |
| P.05  | `set("scratch", undefined)` → row deleted; `get("scratch")` → `{value: undefined}`                 |
| P.06  | `set("topbar.tab_order", ["notes","files","kb"])` → JSONB array round-trips byte-identical         |
| P.07  | `set("complex", {a: [1,2,{b: "c"}]})` → nested object round-trips                                  |
| P.08  | `set` with `key=""` → 400 `invalid_argument`                                                        |
| P.09  | `set` with `key.length > 255` → 400 `invalid_argument`                                              |
| P.10  | `set` with 65 KiB serialised value → 413 `payload_too_large` at resolver pre-check                 |
| P.11  | User A's `set("k", "A-val")`, User B's `get("k")` → `{value: undefined}` (cross-tenant isolation)  |
| P.12  | User without `shell.preferences.read` rule → 403 `rbac.denied { rule: "shell.preferences.read" }`  |
| P.13  | `get_all()` returns all keys for the user, sorted ascending; empty array for new user              |
| P.14  | User deletion (DELETE plinth.users WHERE id=X) → cascade fires; `get_all` for X returns empty (irrelevant — user has no session — but row count drops)  |

### 12.5 `I.*` — integration (3 cases, full-stack)

| Case  | Scenario                                                                                           |
|-------|----------------------------------------------------------------------------------------------------|
| I.01  | Fresh DB + bundled shell.zip on disk → `./build/plinth serve` boots clean; HTTP `GET /app/` returns the shell HTML; manual login works; `cap.call("shell.preferences.set", "theme.mode", "dark")` from devtools → success; restart kernel → reload page → `cap.call("shell.preferences.get", "theme.mode")` returns `{value: "dark"}` |
| I.02  | Two simultaneous `set("k", $val)` calls from same user via two HTTP connections → both succeed; one of the two values persists; PG row count stays 1 (no duplicate) |
| I.03  | User-installed package whose `frontend.mount='/app/'` → install rejected at ACTIVATING with `ERR_MOUNT_CONFLICT`; bundled shell continues to serve from `/app/`  |

### 12.6 Test counts

35 cases: 6 B.\* + 8 M.\* + 4 S.\* + 14 P.\* + 3 I.\*. The
prefixes match ICD-0.6.0 §12's convention. The bulk (P.\* +
M.\*) lands at integration level via the `HttpTestFixture`
already shipped in 0.6.0.N session 2; B.\* and S.\* are
library-level Catch2 cases against the install-lifecycle and
migration surfaces. The 0.6.1 code session phase plan (§8.4)
spreads these across 5 phase commits.

**Status update 2026-09-03:** P.01-P.14 are automated in
`tests/kernel/shell/preferences_dispatch_test.cpp` through the installed
bundled QuickJS handler. I.01-I.03 retain their full-stack/browser or
concurrency-specific ownership and remain listed in `docs/DEFERRED.md`.

---

## §13 — Entry / Exit Criteria

**Entry:** Package system shipped through 0.4.4 (install lifecycle
operational); 0.6.0 frontend-shell-bootstrap shipped (in-browser
frame contracts established); 0.6.0.1 atexit-shutdown ordering fix
shipped (kernel teardown clean).

**Exit:** All four `Closes: 0.6.1` deferral pointers from ICD-0.6.0
§15 are discharged:

1. Bundled-shell first-boot install lifecycle: `./build/plinth serve`
   on a fresh database with `<bundle_path>/shell.zip` present
   produces an `ACTIVE` `provenance='bundled'` `name='shell'` row
   in `plinth.packages` before the HTTP listener accepts traffic;
   B.01 + B.02 + B.03 land in `plinth_tests_pg`.
2. `frontend.mount` manifest contract: the kernel's route
   registration reads `frontend_mount` from `plinth.packages`;
   M.01–M.08 land in `plinth_tests_ws` (HTTP fixture).
3. `ext_shell` PG schema: `ext_shell` schema exists; `ext_shell_role`
   exists; `ext_shell.user_preferences` table exists with the §6.1
   DDL; S.01–S.04 land in `plinth_tests_pg`.
4. `ext_shell.user_preferences` table + get/set: P.01–P.14 round-
   trip green; manual smoke walkthrough (`./build/plinth serve` →
   browser login → devtools `cap.call("shell.preferences.set", …)`
   → kernel restart → `cap.call("shell.preferences.get", …)` returns
   the same value) per `feedback_fe_visualize.md`.

The `DESIGN-shell-v06x.md §0.6.1` exit criterion ("Shell installs
through the standard package lifecycle on first boot; preferences
round-trip (write, reload, read back)") is satisfied by I.01.

---

## §14 — Open Questions

Each OQ carries an architect-recommendation; code-session pin
sequence per ICD-0.5.5 §17 / ICD-0.6.0 §17 OQ Resolutions
precedent (a §17 amendment block lands in the code-session ship
PR, not in this paper).

**OQ1 — Capability shape: two atomic vs. one dispatched.** §7.1
declares two atomic capabilities (`shell.preferences.get` /
`shell.preferences.set`) plus `get_all`. An alternative is one
dispatched capability (`shell.preferences.access(op, key, value?)`)
with op = `'get' | 'set' | 'delete'`. **Recommendation:** two
atomic + `get_all`. Rationale: matches existing fixture-
extension `capabilities.json` patterns (one capability per
function); RBAC rule mapping is direct (read on get/get_all, write
on set); easier 0.6.3 SDK wrapping (`plinth.preferences.get` and
`set` are first-class methods rather than dispatched by string).
Architect: confirm or redirect.

**OQ2 — First-load hydration: eager bulk vs. lazy per-key.** The
shell's startup path can either fetch all preferences once
(`get_all` at init) and cache them client-side, or fetch lazily
per-key as consumers (theme provider, scale provider, etc.) need
values. **Recommendation:** eager bulk fetch on shell init.
Rationale: single round-trip; preferences set is rare; cache-on-
client cheap (under 64 KiB worst-case per the §6 cap × 1000-row
informational threshold = sub-MiB); matches the design bundle's
single-hydration-on-load posture in `project/Plinth Shell.html`.
Lazy hydration adds latency to first-paint of every preference-
gated UI element. Architect: confirm or redirect.

**OQ3 — Reset semantics: `set(key, undefined)` deletes vs. separate
`shell.preferences.delete` capability.** §7.4 implements deletion
as `set(key, undefined)` (exploiting JS's
`undefined`-vs-`null`-vs-missing trichotomy). The alternative is a
third capability `shell.preferences.delete(key) -> { deleted:
bool }`. **Recommendation:** `set(undefined)` deletes; no separate
delete capability. Rationale: avoids capability proliferation; the
JS `undefined` semantic is intuitive (analogous to `delete obj[key]`
versus `obj[key] = null`); JSON wire-protocol omitting the value
field maps cleanly to `JS_UNDEFINED`. Architect: confirm or
redirect.

**OQ4 — Schema-name reservation behaviour: kernel rejects user-
extension `name='shell'` at parse vs. install-time.** §5.5
implements the reservation at `parse_manifest` time
(`ManifestParseError::RESERVED_NAME`). An alternative is to defer
to install-time check (`install_lifecycle::install_package` rejects
at VALIDATING). **Recommendation:** parse-time. Rationale: the
reservation is a *manifest contract*, not a runtime state — the
manifest is invalid by construction, surfacing earlier is better;
operators get the rejection during local `plinth validate` (0.10.5
CLI) rather than at deploy-time. Architect: confirm or redirect.

**OQ5 — `bundle_path` default: `share/plinth/bundled` literal vs.
`Config::Shell::bundle_path` default-required.** §9.2 makes the
default literal `share/plinth/bundled` resolved relative to the
binary install root. An alternative is to *require* operators to
specify `shell.bundle_path` explicitly — no default; absent =
config-validation error. **Recommendation:** literal default.
Rationale: the bundled shell is a normative kernel artifact;
operators should not need to know its on-disk location for a
working deployment; the CMake `install` target places it
deterministically. Operators who need to override (custom-branded
bundle, bundle on shared network mount) provide the override
explicitly. Architect: confirm or redirect.

**OQ6 — Mount conflict resolution: kernel rejects at install vs.
last-writer-wins.** §4.3's partial unique index makes mount
conflicts hard at ACTIVATING. An alternative is to allow the
install but de-activate the prior frontend (last-writer-wins).
**Recommendation:** kernel rejects at install (`ERR_MOUNT_CONFLICT`).
Rationale: matches the existing reserved-prefix posture for
`/api/*` (kernel-level rejection); silent de-activation of the
prior frontend would be surprising; admins who want to swap the
active frontend should do so explicitly (uninstall current →
install new). Architect: confirm or redirect.

**OQ7 — Audit dedup TTL: 60 s for `shell.preferences.set`.** §10.2
sets the dedup window at 60 s per `(user_id, key)`. **Recommendation:**
60 s. Rationale: matches the existing 0.5.5 broker dedup
infrastructure; covers bursty UI patterns (slider drags, scroll
position auto-saves) without losing observability into intentional
preference changes (a user changing `theme.mode` deliberately
twice in a minute will appear once + a deduped-merge row).
Tunability is a kernel ICD amendment, not user-facing config.
Architect: confirm or redirect.

---

## §15 — What Must Not Be Decided Yet

These items are explicitly out of scope for ICD-0.6.1. Each names
the milestone (or trigger) that closes the deferral. Per
`feedback_icd_horizon.md`, ICDs author one milestone ahead and
pre-deciding 0.6.2+ contracts based on 0.6.1 state would violate
that discipline.

### Design tokens and theme persistence wiring

`DESIGN-shell-v06x.md §6` specifies `:root` custom properties +
light/dark/system toggle + 80–175% scaling. **Closes: 0.6.2** per
`DESIGN-shell-v06x.md §9.2`. ICD-0.6.2 will hook the theme +
scale toggles into `shell.preferences.set("theme.mode", …)` /
`shell.preferences.set("ui.scale", …)` round-trips this ICD pins.
0.6.1 ships zero theme-toggle UI; the round-trip is the substrate
only.

_Discharged by ICD-0.6.2 (paper authored 2026-04-29 on
`feat/0.6.1.N-icd-0.6.2-authoring`). ICD-0.6.2 §4.1 / §5.2 pins the
two well-known preference keys as `shell.theme` and `shell.scale_pct`
(not `theme.mode` / `ui.scale` — naming finalised at authoring
time). Ships in v0.6.2 (date pending)._

### Panel SDK client wrappers

`DESIGN-shell-v06x.md §4` specifies `plinth.preferences.get` /
`plinth.preferences.set` client wrappers. **Closes: 0.6.3** per
`DESIGN-shell-v06x.md §9.3`. ICD-0.6.3 will wrap the
`shell.preferences.*` capabilities in a `plinth.preferences.*`
client SDK surface; the kernel-side capability declaration in
ICD-0.6.1 §7 is the substrate. No wrapper code in 0.6.1.

### `ext_shell.default_apps` table DDL

§5.4 reserves the name. **Closes: 0.6.6** per
`DESIGN-shell-v06x.md §9.6`. The `(scope, user_id, content_type)`
PK shape is documented in §5.4 but the DDL ships in the 0.6.6
bundle update's `002_*.sql` migration alongside the tray /
content-type-resolution consumer surface.

### Tab ordering / float position / tray icon ordering keys

Future 0.6.4 / 0.6.5 / 0.6.6 milestones will write specific
preference keys (`topbar.tab_order`, `floats.position[<id>]`,
`tray.icon_position[<id>]`, etc.). 0.6.1 reserves no key prefixes
beyond what `DESIGN-shell-v06x.md §3.7` lists informally — the
key namespace is shell-extension-owned and convention-driven, not
kernel-validated. Each consumer milestone documents its keys.

### Multi-frontend simultaneity

§4.4's active-frontend resolver is a single-row predicate. A
deployment that wants two simultaneously-mounted frontends (e.g.
end-user shell at `/app/` + admin shell at `/admin/`) must wait
for 0.6.4 when the resolver lifts the singleton. **Closes: 0.6.4**.
The `frontend.mount` contract here is forward-compat — installing
multiple frontends is legal *today* (each gets its own
`plinth.packages` row + columns), but only one is `'ACTIVE'` at a
time. The 0.6a admin extension milestone will exercise this
relaxation.

### Hot-swap / runtime active-frontend change

§4.7 documents that active-frontend resolution happens at boot
only. Drogon does not support route un-registration; hot-swap
would need a route-registry rebuild + listener restart that
0.6.1 explicitly does not pursue. **Trigger:** a future
infrastructure ICD if multi-tenant hosting requires it; not on
the current 0.6.x roadmap.

### Bundled-shell upgrade workflow

§3.4 documents that admin-driven upgrade through ICD-0.4.5 is the
path. A *kernel-driven* auto-upgrade on bundle-bytes change would
require checksum tracking + change detection on disk; this masks
operator mistakes (corrupt bundle replaces working install). **Not
planned.** If operator workflow demands automation, a future
admin CLI command (`plinth bundled-shell upgrade`) is the surface,
not kernel-internal autopilot.

### Migration of existing user-installed `name='shell'` packages

§3.5's `ERR_BUNDLE_SCHEMA_RESERVED` aborts boot when a pre-existing
user package squats the name. A *migration* path that auto-renames
the squatter is **not** in scope — 0.6.1 ships the guard, not the
fix. Operator runs ad-hoc SQL or waits for a future admin CLI.

---

## Appendix A — Authoritative `001_init.sql`

```sql
-- shell.zip/migrations/001_init.sql
-- Initial migration for the bundled shell extension. Creates the
-- ext_shell.user_preferences table and reserves ext_shell.default_apps
-- for the 0.6.6 tray / content-type resolution milestone.
--
-- Per ICD-0.6.1 §6, ICD-0.4.3 §Schema + GRANT Contract.

CREATE TABLE ext_shell.user_preferences (
    user_id     UUID         NOT NULL,
    key         TEXT         NOT NULL,
    value       JSONB        NOT NULL,
    updated_at  TIMESTAMPTZ  NOT NULL DEFAULT now(),

    PRIMARY KEY (user_id, key),

    CONSTRAINT fk_user_preferences_user_id
        FOREIGN KEY (user_id)
        REFERENCES plinth.users (id)
        ON DELETE CASCADE,

    CONSTRAINT chk_key_nonempty
        CHECK (length(key) BETWEEN 1 AND 255),

    CONSTRAINT chk_value_size
        CHECK (octet_length(value::text) <= 65536)
);

CREATE INDEX idx_user_preferences_updated_at
    ON ext_shell.user_preferences (updated_at);

GRANT SELECT, INSERT, UPDATE, DELETE
    ON ext_shell.user_preferences
    TO ext_shell_role;

-- Reservation: 0.6.6 will add ext_shell.default_apps via a
-- 002_add_default_apps.sql migration.  (scope, user_id, content_type)
-- PK; FK on plinth.users(id) ON DELETE CASCADE for user-scope rows;
-- admin-scope rows have user_id = NULL.  Do NOT create this table in
-- 0.6.1 — it has no consumer yet.
-- See ICD-0.6.6-tray-content-type-navigation.md §default_apps.
```

---

## Appendix B — `shell.zip` contents

```
shell.zip/
├── manifest.json
│   { "name": "shell", "version": "0.6.1",
│     "entry_point": "server/main.js",
│     "frontend": { "mount": "/app/", "entry": "index.html" } }
├── capabilities.json
│   {
│     "provides": [
│       { "namespace": "shell", "version": 1,
│         "function": "preferences.get",
│         "params": [{"name":"key","type":"string"}],
│         "returns": "object", "scope": "instance",
│         "rbac_rule": "shell.preferences.read" },
│       { "namespace": "shell", "version": 1,
│         "function": "preferences.set",
│         "params": [{"name":"key","type":"string"},
│                    {"name":"value","type":"any"}],
│         "returns": "object", "scope": "instance",
│         "rbac_rule": "shell.preferences.write" },
│       { "namespace": "shell", "version": 1,
│         "function": "preferences.get_all",
│         "params": [],
│         "returns": "object", "scope": "instance",
│         "rbac_rule": "shell.preferences.read" }
│     ]
│   }
├── rbac.json
│   {
│     "rules": [
│       { "name": "shell.preferences.read",  "description": "..." },
│       { "name": "shell.preferences.write", "description": "..." }
│     ],
│     "default_grants": [
│       { "group": "users", "rule": "shell.preferences.read"  },
│       { "group": "users", "rule": "shell.preferences.write" }
│     ]
│   }
├── server/
│   ├── main.js             — empty default export (no on-load logic)
│   └── handlers/
│       ├── preferences_get.js
│       ├── preferences_set.js
│       └── preferences_get_all.js
├── client/
│   ├── index.html          — byte-identical to 0.6.0 ship
│   ├── shell.js            — byte-identical to 0.6.0 ship
│   └── vendor/
│       ├── preact.module.js@10.22.0
│       └── htm.module.js@3.1.1
└── migrations/
    └── 001_init.sql        — Appendix A
```

Total uncompressed size ~96 KiB; compressed shell.zip ~24 KiB.
The `client/` tree is byte-identical to ICD-0.6.0's baked
artifacts; 0.6.1 adds `capabilities.json` + `rbac.json` +
`server/` + `migrations/` only.

---

## Appendix C — First-boot sequence (informative)

```
kernel main()
  → parse argv, load config
  → bootstrap_kernel_schema(cfg.db)              [ICD-0.1.1]
  → install_lifecycle::recover_in_flight(cfg)    [ICD-0.4.4]
  → ensure_bundled_shell_installed(cfg, db)      [ICD-0.6.1 §3.1]
       │
       ├─ db.query "SELECT id FROM plinth.packages WHERE provenance='bundled' …"
       │   ├─ row count == 1: log + return Ok                  ──── steady state
       │   ├─ row count == 2: return Err MULTIPLE_ACTIVE_FRONTENDS
       │   └─ row count == 0: continue first-boot path
       │
       ├─ shell.zip exists? else return Err BUNDLE_MISSING
       │
       ├─ audit shell.firstboot.bundled_install_started
       │
       └─ install_lifecycle::install_package(ctx, db)          [ICD-0.4.4]
              │
              ├─ UPLOADING:    extract zip to staging dir
              ├─ VALIDATING:   parse_manifest (with frontend.mount); parse_capabilities; parse_rbac
              ├─ MIGRATING:    run_migrations("shell", staging, admin_conn)  [ICD-0.4.3]
              │                  → CREATE SCHEMA ext_shell
              │                  → CREATE ROLE ext_shell_role
              │                  → GRANT USAGE+CREATE on schema, USAGE on plinth, SELECT on plinth.users
              │                  → run 001_init.sql in transaction; record checksum
              ├─ REGISTERING:  insert capability rows; insert RBAC rule rows; default-group grants
              │                UPDATE plinth.packages SET frontend_mount, frontend_entry
              ├─ EXTRACTING:   move staging/* to <data_dir>/extensions/shell/0.6.1/
              └─ ACTIVATING:   UPDATE plinth.packages SET state='ACTIVE'
                               (partial unique index commits at this transaction)
       
       ├─ audit shell.firstboot.bundled_install_completed
       └─ return Ok
  
  → init_resolver(cfg)                         [ICD-0.5.0.3]
  → init_registry(cfg)                         [ICD-0.5.0.3]
  → reload_tier2_cache(db)                     [picks up shell.preferences.* rows]
  → register_active_frontend_routes(cfg, db)   [ICD-0.6.1 §4.4]
  → asset_server::restore_routes(...)
  → register_package_routes()
  → drogon::app().run()                        [HTTP listener accepts traffic]
```

Subsequent boots short-circuit at the first detection branch
(rows == 1) in microseconds; no audit emission for the steady-
state case.

---

## §17 — OQ Resolutions (post-ship amendment, 2026-04-29 v0.6.1 ship)

The seven §14 OQs are pinned per the architect-recommendation
defaults; six implementation deviations are recorded per
METHODOLOGY §Phase 2 Constraint #4. This section is the
authoritative source — `feedback_real_code_paths.md` requires
deviations to be visible in the ICD itself, not only in the
ship CHANGELOG.

### OQ pin table

| # | OQ | Resolution |
|---|----|------------|
| OQ1 | Capability shape: two atomic vs. one dispatched | **Two atomic + `get_all`** (recommendation) |
| OQ2 | First-load hydration: eager bulk vs. lazy per-key | **Eager bulk fetch on shell init** (recommendation; deferred to 0.6.2 wiring) |
| OQ3 | Reset semantics: `set(undefined)` deletes vs. separate `delete` capability | **`set(undefined)` deletes** (recommendation) |
| OQ4 | Schema-name reservation: parse-time vs. install-time | **Parse-time** (recommendation; threaded via `bool is_bundled` parameter on `PackageManifest::parse`) |
| OQ5 | `bundle_path` default | **Auto-resolve from `/proc/self/exe`**: try `<bin>/share/plinth/bundled` (dev layout, where binary lives at `<build>/plinth`) then fall back to `<bin>/../share/plinth/bundled` (FHS install). Deviation from §9.2's literal `share/plinth/bundled` — the resolver tries both layouts so CMake-built dev binaries work without an explicit config override |
| OQ6 | Mount-conflict resolution | **Kernel rejects at install** (recommendation; enforced by the existing `uniq_packages_mount_active` partial unique index — no new pre-check needed) |
| OQ7 | Audit dedup TTL: 60 s for `shell.preferences.set` | **60 s** (recommendation) |

### Implementation deviations from §3–§7 pseudocode

1. **rbac.json group reference: `everyone` not `users`.** §7.2's
   `default_grants` block uses group name `users`; the kernel's
   `plinth::groups::bootstrap_groups`
   ([`src/kernel/groups/handlers.cpp`](../src/kernel/groups/handlers.cpp))
   seeds `admin` + `everyone` (no `users`). Default grants
   reference `everyone` (the existing semantic match for "all
   authenticated users"). Future ICD amendment may rename if a
   user-vs-everyone distinction emerges.

2. **rbac.json field name: `rule` not `name`.** §7.2's pseudocode
   shows `{"name": "shell.preferences.read"}`; the
   [`rbac_manifest.cpp`](../src/kernel/rbac/rbac_manifest.cpp)
   parser expects the existing `rule` field per ICD-0.4.6
   §RbacManifest. Bundled rbac.json uses the parser's contract.

3. **Handler file naming: dotted, not underscored.** §7.3 / §7.4's
   pseudocode block-comments
   `// shell.zip/server/handlers/preferences_get.js`; the
   `cf4-handler-missing` rule in
   [`cross_file_validator.cpp`](../src/kernel/packages/cross_file_validator.cpp)
   matches `<function>.js` verbatim. Files ship as
   `preferences.get.js` / `preferences.set.js` /
   `preferences.get_all.js`.

4. **`"any"` accepted as a manifest param type.** §7.1 declares
   `"type": "any"` for `shell.preferences.set`'s `value` param;
   the [`capabilities_manifest.cpp`](../src/kernel/packages/capabilities_manifest.cpp)
   `PARAM_TYPE_LITERALS` array originally only accepted six
   types. `"any"` added as a wildcard (general feature; benefits
   future extensions that need pass-through any-shape value
   parameters). Handler is responsible for runtime shape checks;
   the manifest declares "validate server-side."

5. **Full P.\* / I.\* JS-dispatch test suite deferred to 0.6.1.N.**
   §12.4 / §12.5 enumerates 14 P.\* + 3 I.\* cases that exercise
   the end-to-end JS-handler dispatch via cap.call. v0.6.1 ships 9
   schema-level / grant-level / handler-deployment tests
   (`P.s.01`–`P.s.06`, `P.r.01`, `P.r.02`, `P.h.01`) covering the
   contracts the JS-dispatch suite assumes hold. Full integration
   suite carved into a 0.6.1.N follow-up — needs the
   `async_bridge_fixture` + `ws_test_fixture` scaffold (the same
   path `project_test_fixture_inflight.md` session 9 noted needs
   the `init_registry` teardown work resolved before real-bridge
   capability dispatch through the WS fixture is stable).

   **Update 2026-04-30 (0.6.3.N):** Both blockers closed. The
   ctx-injection fix (Bug 1 in `runtime_registry.cpp:298-307` +
   `__handler_ctx` injection) makes `shell.preferences.get/set/get_all`
   actually work end-to-end (pre-fix they always rejected with
   `cap.handler_threw: TypeError`). The `init_registry` teardown
   (Bug 2 in `ws_test_fixture.cpp::start_test_server` atexit chain)
   makes the WS test fixture safe for real-bridge dispatch. New
   library-level test `P.dispatch.01` at
   `tests/kernel/shell/preferences_test.cpp` proves the
   `extensions::dispatch` round-trip (GET-missing → SET → GET-back)
   end-to-end. Full P.\*/I.\* WS-fixture suite still scheduled to
   the `0.6.3.N JS-dispatch test suite backfill` milestone (per
   ROADMAP §0.6).

   **Update 2026-09-03:** The server/kernel portion is complete.
   `tests/kernel/shell/preferences_dispatch_test.cpp` implements P.01-P.14
   through the real resolver, RBAC gate, bundled QuickJS handler, PostgreSQL,
   and HTTP status seam. It also locks absence apart from stored JSON null,
   deletion affected-row reporting, caller isolation, cascade behavior,
   payload limits, and privacy-preserving mutation audit deduplication. The
   three I.* browser/full-stack cases remain deferred; they are not part of
   the server fixture and are tracked in `docs/DEFERRED.md`.

6. **`upgrade_package` parses with `Provenance::USER`.** §15
   defers bundled-shell upgrade workflow; the existing user-
   driven upgrade path always parses with `Provenance::USER`. A
   user upload with `name='shell'` is rejected at
   `install_package`'s parse stage (RESERVED_NAME) before
   reaching `upgrade_package`. Future kernel-driven bundled
   upgrade must thread the existing row's provenance to
   `read_minimal_manifest`.

### Default-grants infrastructure (general)

The default-grants pass that §7.2 specifies for the bundled shell
is implemented as a general feature in
[`rbac_manifest.{hpp,cpp}`](../src/kernel/rbac/rbac_manifest.hpp)
+ [`install_lifecycle.cpp::register_extension_rbac_rules`](../src/kernel/packages/install_lifecycle.cpp).
Any extension declaring `default_grants[]` in its `rbac.json`
gets the same idempotent INSERT...SELECT into
`plinth.group_rules` after rules are upserted. Re-installs and
upgrades re-apply without duplication. Unknown group names log a
warning and skip (install does not abort — operator
remediation).
